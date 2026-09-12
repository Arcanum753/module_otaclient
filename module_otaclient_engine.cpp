#include <Arduino.h>
#include "version.h"
#include "main.h"

#include <WiFiClient.h>
#if defined(ESP32)
#include <Update.h>
#endif
#if defined(ESP8266)
#include <ArduinoOTA.h>
#endif
#include <core_ntp/NtpClientLib.h>
#include "core_json/core_json.h"
#include "core_sys/core_sys.h"
#include "module_otaclient/module_otaclient.h"
#include "core_sys/eertos.h"

// Static manifest entries buffer (avoid stack allocation)
ManifestEntry CLASS_MODULE_OTACLIENT::_manifestEntries[OTACLIENT_MAX_MANIFEST_ENTRIES];

// ============================================================
// Свободные функции (EERTOS задачи)
// ============================================================
// ========== TIMER (for calling from other files) ==========
void otaclientTimer() {
    uint16_t timeout = module_otaclient.getTimeOut();
    if (module_otaclient.isStart() == false){   return; }
    
    // timeOut == 0: timer runs at 1min interval, no update checks
    if (timeout == 0) {
        SetTimerTask(otaclientTimer, SEC * MINUTES * 1);
        return;
    }
    
    if (timeout > 60){ timeout = 60;}
    
    // Don't start a new check if update is already in progress
    if (module_otaclient._updateInProgress) {
        DEBUGOTACLIENT("OtaClient update in progress, rescheduling timer\n");
        SetTimerTask(otaclientTimer, SEC * MINUTES * timeout);
        return;
    }
    
    DEBUGOTACLIENT("OtaClient timeout %d min. ", timeout);
    SetTimerTask(otaclientTimer, SEC * MINUTES * timeout);
    
    // Check for updates on each timer tick
    module_otaclient.checkForUpdates();
}

// ========== LOOP TASK (called via eertos timer every 50ms) ==========
void otaclientLoopTask() {
    module_otaclient.loop();
    SetTimerTask(otaclientLoopTask, 50);
}

// ============================================================
// Публичное API (геттеры)
// ============================================================
uint16_t CLASS_MODULE_OTACLIENT::getTimeOut()          { return _config.timeOut; }
bool CLASS_MODULE_OTACLIENT::powerOnGet()              { return _config.powerOn; }
String CLASS_MODULE_OTACLIENT::serverAddressGet()      { return _config.serverAddress; }
uint16_t CLASS_MODULE_OTACLIENT::serverPortGet()       { return _config.serverPort; }
String CLASS_MODULE_OTACLIENT::manifestPathGet()       { return _config.manifestPath; }
uint8_t CLASS_MODULE_OTACLIENT::isStart()              { return _isStarted; }

// ========== JSON GET ==========
String CLASS_MODULE_OTACLIENT::jsonGet() {
    String ret = "";
    ret += "{\n";
    ret += "  \"deviceName\": \"" + core_sys.getDeviceName() + "\",\n";
    ret += "  \"deviceSerial\": \"" + core_sys.getDeviceSerial() + "\",\n";
    ret += "  \"ip\": \"" + WiFi.localIP().toString() + "\",\n";
    ret += "  \"mac\": \"" + WiFi.macAddress() + "\",\n";
    ret += "  \"timeOut\": " + String(_config.timeOut) + ",\n";
    ret += "  \"serverPort\": " + String(_config.serverPort) + ",\n";
    ret += "  \"target\": \"" + String(BUILD_ENV) + "\",\n";
    ret += "  \"buildtime\": \"" + String(BUILD_TIME) + "\",\n";
    ret += "  \"gitbranch\": \"" + String(GIT_BRANCH) + "\",\n";
    ret += "  \"gitcommit\": \"" + String(GIT_COMMIT) + "\",\n";
    ret += "  \"uptime\": \"" + String(NTP.getUptimeString()) + "\",\n";
    ret += "  \"rstreason\": \"" + ESPHTTPServer.getResetReason() + "\",\n";
    ret += "  \"espVer\": \"" + String(FIRMWARE_VERSION) + "\"\n";
    ret += "}\n";
    return ret;
}

// ========== ON WiFi CONNECT ==========
void CLASS_MODULE_OTACLIENT::onWiFiConnect() {
    DEBUGOTACLIENT("%s: powerOn=%d\r\n", __FUNCTION__, _config.powerOn);
    if (_config.powerOn) {
        // Отложить проверку обновлений на 5 секунд после WiFi connect.
        // ESP32: onWiFiConnect вызывается из WiFi-события в задаче arduino_events
        // с маленьким стеком — выполнение OTA-загрузки прямо здесь переполняет стек
        // (Stack canary watchpoint) и вызывает перезагрузку устройства.
        // ESP8266: дать стеку WiFi стабилизироваться и избежать WDT reset.
        SetTimerTask(otaclientTimer, SEC * 5);
    }
}

// ========== LOOP (called from main loop) ==========
void CLASS_MODULE_OTACLIENT::loop() {
    // Don't process any loop actions while update is in progress
    if (_updateInProgress) {
        return;
    }
    
    // Handle pending test
    if (_testStatusCode == OTACLIENT_TEST_PENDING) {
        _testStatusCode = OTACLIENT_TEST_IDLE;
        
        // Check WiFi
        if (WiFi.status() != WL_CONNECTED) {
            _testStatusCode = OTACLIENT_TEST_SERVER_UNAVAIL;
            _testStatusMessage = "WiFi not connected";
            DEBUGOTACLIENT("checkForUpdates from button: WiFi not connected\n");
            return;
        }
        
        // Fetch manifest (using static buffer to avoid stack allocation)
        int entryCount = 0;
        
        if (!fetchManifest(_manifestEntries, entryCount)) {
            _testStatusCode = OTACLIENT_TEST_SERVER_UNAVAIL;
            _testStatusMessage = "Server unavailable";
            DEBUGOTACLIENT("checkForUpdates from button: server unavailable\n");
            return;
        }
        
        DEBUGOTACLIENT("checkForUpdates from button: manifest has %d entries\n", entryCount);
        
        fileCompareResult fwResult, fsResult;
        bool fwValid, fsValid;
        _testCompareResult = 0;
        _testFwCompareResult = 0;
        _testFsCompareResult = 0;
        
        ManifestEntry* firmwareEntry = NULL;
        ManifestEntry* fsEntry = NULL;
        checkManifestEntries(_manifestEntries, entryCount, fwResult, fsResult, fwValid, fsValid,
                             _testFwCompareResult, _testFsCompareResult,
                             firmwareEntry, fsEntry);
        
        // Combine compare results for display:
        // - If any is NEWER (1) -> combined = 1
        // - Else if any is SAME (0) -> combined = 0
        // - Else if any is OLDER (-1) -> combined = -1
        // - Else combined = -2 (missing)
        if (_testFwCompareResult == 1 || _testFsCompareResult == 1) {
            _testCompareResult = 1;
        } else if (_testFwCompareResult == 0 || _testFsCompareResult == 0) {
            _testCompareResult = 0;
        } else if (_testFwCompareResult == -1 || _testFsCompareResult == -1) {
            _testCompareResult = -1;
        } else {
            _testCompareResult = -2;
        }
        
        // Build result message
        if (fwValid || fsValid) {
            _testStatusCode = OTACLIENT_TEST_UPDATE_AVAIL;
            _testStatusMessage = "Update available: ";
            
            if (fsValid && fsEntry) {
                _testStatusMessage += "FS: " + fsEntry->name;
                _testUpdateName = fsEntry->name;
                _testUpdateUrl = "http://" + _config.serverAddress + ":" + String(_config.serverPort) + "/firmware/" + fsEntry->name;
                _testUpdateSize = fsEntry->size;
                _testUpdateMd5 = fsEntry->md5;
                _testUpdateFileType = FILE_TYPE_FILESYSTEM;
            }
            if (fwValid && firmwareEntry) {
                if (_testUpdateName.length() > 0) _testStatusMessage += ", ";
                _testStatusMessage += "FW: " + firmwareEntry->name;
                // Prefer firmware over FS for the update button
                _testUpdateName = firmwareEntry->name;
                _testUpdateUrl = "http://" + _config.serverAddress + ":" + String(_config.serverPort) + "/firmware/" + firmwareEntry->name;
                _testUpdateSize = firmwareEntry->size;
                _testUpdateMd5 = firmwareEntry->md5;
                _testUpdateFileType = FILE_TYPE_FIRMWARE;
            }
        } else {
            _testStatusCode = OTACLIENT_TEST_NO_UPDATES;
            // Build detailed status message for both FW and FS
            if (firmwareEntry || fsEntry) {
                _testStatusMessage = "";
                if (firmwareEntry) {
                    _testStatusMessage += "FW: ";
                    if (_testFwCompareResult == 1) {
                        _testStatusMessage += "newer";
                    } else if (_testFwCompareResult == 0) {
                        _testStatusMessage += "current (same)";
                    } else if (_testFwCompareResult == -1) {
                        _testStatusMessage += "server older";
                    } else if (_testFwCompareResult == -2) {
                        _testStatusMessage += "no version info";
                    } else {
                        _testStatusMessage += "unknown";
                    }
                }
                if (fsEntry) {
                    if (_testStatusMessage.length() > 0) _testStatusMessage += ", ";
                    _testStatusMessage += "FS: ";
                    if (_testFsCompareResult == 1) {
                        _testStatusMessage += "newer";
                    } else if (_testFsCompareResult == 0) {
                        _testStatusMessage += "current (same)";
                    } else if (_testFsCompareResult == -1) {
                        _testStatusMessage += "server older";
                    } else if (_testFsCompareResult == -2) {
                        _testStatusMessage += "no version info";
                    } else {
                        _testStatusMessage += "unknown";
                    }
                }
                if (_testStatusMessage.length() == 0) {
                    _testStatusMessage = "No updates available";
                }
            } else {
                _testStatusMessage = "No updates available (no matching files)";
            }
        }
        
        DEBUGOTACLIENT("Test result: code=%d, fwCompare=%d, fsCompare=%d, combined=%d, msg=%s\n",
                       _testStatusCode, _testFwCompareResult, _testFsCompareResult, _testCompareResult, _testStatusMessage.c_str());
    }
    
    // Handle pending update (triggered by "Update Now" button)
    if (_testStatusCode == OTACLIENT_TEST_UPDATING) {
        _testStatusCode = OTACLIENT_TEST_IDLE;
        
        if (_testUpdateName.length() > 0 && _testUpdateUrl.length() > 0) {
            DEBUGOTACLIENT("Loop: starting update for %s\n", _testUpdateName.c_str());
            bool ok = downloadAndUpdate(_testUpdateUrl, _testUpdateSize, _testUpdateMd5, _testUpdateFileType);
            if (ok) {
                DEBUGOTACLIENT("Update successful, restarting...\n");
                delay(500);
                ESPHTTPServer.restart_esp();
            } else {
                _testStatusCode = OTACLIENT_TEST_SERVER_UNAVAIL;
                _testStatusMessage = "Update failed";
                DEBUGOTACLIENT("Update failed\n");
            }
        }
    }
}

// ============================================================
// Мини-HTTP клиент поверх WiFiClient (без TLS/HTTPClient).
// Сервер обновлений локальный (http://), поэтому HTTPS не требуется:
// HTTPClient тянет WiFiClientSecure + mbedTLS (~сотни КБ) без пользы.
// ============================================================

// Поток тела HTTP-ответа: поддержка Content-Length и chunked.
class HttpBodyStream : public Stream {
public:
    HttpBodyStream(WiFiClient& client, bool chunked)
        : _client(client), _chunked(chunked), _chunkLeft(0), _done(false) {}

    int available() {
        if (_done) { return 0; }
        if (_chunked) {
            if (_chunkLeft == 0 && !nextChunk()) { return 0; }
            int a = _client.available();
            return (a > (int)_chunkLeft) ? (int)_chunkLeft : a;
        }
        return _client.available();
    }

    int read() {
        if (_done) { return -1; }
        if (_chunked) {
            if (_chunkLeft == 0 && !nextChunk()) { return -1; }
            int c = _client.read();
            if (c < 0) { return -1; }
            _chunkLeft--;
            if (_chunkLeft == 0) { consumeCrlf(); }
            return c;
        }
        return _client.read();
    }

    int peek() {
        if (_chunked) { return -1; }
        return _client.peek();
    }

    size_t write(uint8_t) { return 0; }
    void flush() {}

private:
    bool nextChunk() {
        String line = _client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) { _done = true; return false; }
        long sz = strtol(line.c_str(), NULL, 16);
        if (sz <= 0) { _done = true; return false; }
        _chunkLeft = (size_t)sz;
        return true;
    }

    void consumeCrlf() {
        uint32_t t = millis() + 100;
        while (millis() < t) {
            int c = _client.peek();
            if (c == '\r' || c == '\n') { _client.read(); }
            else { break; }
        }
    }

    WiFiClient& _client;
    bool        _chunked;
    size_t      _chunkLeft;
    bool        _done;
};

// Отправляет GET и разбирает статус/заголовки. Тело остаётся в client.
static bool httpGetOpen(WiFiClient& client, const String& host, uint16_t port,
                        const String& path, int& status, bool& chunked, long& contentLen) {
    status = 0;
    chunked = false;
    contentLen = -1;

    client.setTimeout(5000);
    if (!client.connect(host.c_str(), port)) { return false; }

    client.print(String("GET ") + path + " HTTP/1.1\r\n" +
                 "Host: " + host + ":" + String(port) + "\r\n" +
                 "Connection: close\r\n\r\n");

    uint32_t timeout = millis() + 8000;
    while (!client.available() && client.connected() && millis() < timeout) { delay(1); }
    if (!client.available()) { return false; }

    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    int sp1 = statusLine.indexOf(' ');
    if (sp1 > 0) { status = statusLine.substring(sp1 + 1).toInt(); }

    while (client.connected()) {
        if (!client.available()) {
            if (millis() > timeout) { break; }
            delay(1);
            continue;
        }
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) { break; }
        String low = line;
        low.toLowerCase();
        if (low.startsWith("transfer-encoding:") && low.indexOf("chunked") >= 0) { chunked = true; }
        if (low.startsWith("content-length:")) { contentLen = line.substring(15).toInt(); }
    }
    return (status > 0);
}

// ============================================================
// FETCH MANIFEST from OTA server
// ============================================================
bool CLASS_MODULE_OTACLIENT::fetchManifest(ManifestEntry* entries, int& count) {
    count = 0;
    
    if (_config.serverAddress.length() == 0) {
        DEBUGOTACLIENT("fetchManifest: serverAddress is empty\n");
        return false;
    }
    
#if defined(ESP8266)
    if (ESP.getFreeHeap() < 10000) {
        DEBUGOTACLIENT("fetchManifest: low memory (%d bytes), skipping\n", ESP.getFreeHeap());
        return false;
    }
#endif

    WiFiClient client;
    int status = 0;
    bool chunked = false;
    long contentLen = -1;
    String path = _config.manifestPath + "?target=" + BUILD_ENV + "&ver=" + FIRMWARE_VERSION;
    DEBUGOTACLIENT("fetchManifest: http://%s:%d%s\n",
                   _config.serverAddress.c_str(), (int)_config.serverPort, path.c_str());

    if (!httpGetOpen(client, _config.serverAddress, (uint16_t)_config.serverPort,
                     path, status, chunked, contentLen)) {
        DEBUGOTACLIENT("fetchManifest: connect/open failed\n");
        return false;
    }
    if (status != 200) {
        DEBUGOTACLIENT("fetchManifest: HTTP error %d\n", status);
        client.stop();
        return false;
    }

    // Читаем тело (Content-Length или chunked) с WDT feed.
    String payload = "";
    HttpBodyStream body(client, chunked);
    uint32_t timeout = millis() + 8000;
    while (millis() < timeout) {
        if (body.available() > 0) {
            int c = body.read();
            if (c < 0) { break; }
            payload += (char)c;
            if (payload.length() > 4096) {  // Жёсткий лимит: 4096 байт
                DEBUGOTACLIENT("fetchManifest: payload too large\n");
                client.stop();
                return false;
            }
        } else if (!client.connected()) {
            break;
        } else {
            delay(1);
#if defined(ESP8266)
            ESP.wdtFeed();
#endif
        }
    }
    client.stop();

    if (payload.length() == 0) {
        DEBUGOTACLIENT("fetchManifest: timeout or empty response\n");
        return false;
    }
    DEBUGOTACLIENT("fetchManifest: received %d bytes\n", payload.length());
    
    // Parse JSON
    bool hasFiles = false;
    if (!core_json.jsonParseNestedBool(payload, "has_files", hasFiles) || !hasFiles) {
        DEBUGOTACLIENT("fetchManifest: no files for target %s on server\n", BUILD_ENV);
        return false;
    }
    
    int numFiles = core_json.jsonGetArraySize(payload, "files");
    if (numFiles <= 0) {
        DEBUGOTACLIENT("fetchManifest: no 'files' array in manifest\n");
        return false;
    }
    
    int idx = 0;
    for (int i = 0; i < numFiles && idx < OTACLIENT_MAX_MANIFEST_ENTRIES; i++) {
        String name, type, md5;
        core_json.jsonGetArrayStr(payload, "files", i, "name", name);
        core_json.jsonGetArrayStr(payload, "files", i, "type", type);
        
        if (name.length() == 0 || type.length() == 0) {
            DEBUGOTACLIENT("  [%d] SKIPPED (missing name or type)\n", i);
            continue;
        }
        
        entries[idx].name = name;
        entries[idx].type = type;
        
        int32_t sizeVal = 0;
        if (core_json.jsonGetArrayInt(payload, "files", i, "size", sizeVal)) entries[idx].size = (size_t)sizeVal;
        core_json.jsonGetArrayStr(payload, "files", i, "md5", entries[idx].md5);
        
        DEBUGOTACLIENT("  [%d] %s (%s) %u bytes MD5:%s\n",
            idx, entries[idx].name.c_str(), entries[idx].type.c_str(),
            entries[idx].size, entries[idx].md5.c_str());
        
        idx++;
    }
    
    count = idx;
    return count > 0;
}

// ============================================================
// UNIFIED MANIFEST ENTRY CHECKING
// ============================================================
void CLASS_MODULE_OTACLIENT::checkManifestEntries(ManifestEntry* entries, int count,
                                                    fileCompareResult& fwResult, fileCompareResult& fsResult,
                                                    bool& fwValid, bool& fsValid,
                                                    int8_t& fwCompareResult, int8_t& fsCompareResult,
                                                    ManifestEntry*& fwEntryOut, ManifestEntry*& fsEntryOut) {
    ManifestEntry* firmwareEntry = NULL;
    ManifestEntry* fsEntry = NULL;
    int bestFwBuild = -1, bestFsBuild = -1;
    
    // Разделители имени файла для текущего устройства (строгое совпадение префикса)
    String fwSeparator = String(BUILD_ENV) + OTA_STR_SEPARATOR_FIRMWARE;
    String fsSeparator = String(BUILD_ENV) + OTA_STR_SEPARATOR_FILESYSTEM;
    
    for (int i = 0; i < count; i++) {
        int build = -1;
        int lastDot = entries[i].name.lastIndexOf('.');
        int prevDot = (lastDot > 0) ? entries[i].name.lastIndexOf('.', lastDot - 1) : -1;
        if (prevDot > 0) {
            String buildStr = entries[i].name.substring(prevDot + 1, lastDot);
            build = buildStr.toInt();
        }
        
        if (entries[i].type == "filesystem") {
            // Пропускаем файлы других устройств/вариантов (например, esp32_clock-mech_ring-...)
            if (!entries[i].name.startsWith(fsSeparator)) {
                DEBUGOTACLIENT("  Skip foreign target file: %s\n", entries[i].name.c_str());
                continue;
            }
            if (build > bestFsBuild) {
                bestFsBuild = build;
                fsEntry = &entries[i];
            }
            DEBUGOTACLIENT("  Found FS file: %s (build=%d)\n", entries[i].name.c_str(), build);
        } else if (entries[i].type == "firmware") {
            if (!entries[i].name.startsWith(fwSeparator)) {
                DEBUGOTACLIENT("  Skip foreign target file: %s\n", entries[i].name.c_str());
                continue;
            }
            if (build > bestFwBuild) {
                bestFwBuild = build;
                firmwareEntry = &entries[i];
            }
            DEBUGOTACLIENT("  Found firmware file: %s (build=%d)\n", entries[i].name.c_str(), build);
        }
    }
    
    fwValid = false;
    fsValid = false;
    fwCompareResult = 0;
    fsCompareResult = 0;
    
    if (firmwareEntry) {
        int8_t ret = fileNameCheck(firmwareEntry->name, &fwResult);
        int8_t fwCompare = compareWithCurrentFsVersion(&fwResult, firmwareEntry->name);
        fwValid = (ret == 1 && fwCompare == 1);
        fwCompareResult = fwCompare;
        DEBUGOTACLIENT("Firmware version check: %s (nameMatch=%d, fsCompare=%d, valid=%d)\n",
                       fwResult.nameMatch == 1 ? "MATCH" : "NO MATCH", fwResult.nameMatch, fwCompare, fwValid);
    }
    
    if (fsEntry) {
        int8_t ret = fileNameCheck(fsEntry->name, &fsResult);
        int8_t fsCompare = compareWithCurrentFsVersion(&fsResult, fsEntry->name);
        fsValid = (ret == 1 && fsCompare == 1);
        fsCompareResult = fsCompare;
        
        DEBUGOTACLIENT("FS version check: %s (nameMatch=%d, fsCompare=%d, valid=%d)\n",
                       fsResult.nameMatch == 1 ? "MATCH" : "NO MATCH", fsResult.nameMatch, fsCompare, fsValid);
    }
    
    fwEntryOut = firmwareEntry;
    fsEntryOut = fsEntry;
}

// ============================================================
// PERFORM UPDATE FROM STREAM
// ============================================================
bool CLASS_MODULE_OTACLIENT::performUpdateFromStream(Stream& stream, size_t size, const String& expectedMd5, int fileType) {
    DEBUGOTACLIENT("performUpdateFromStream: size=%u, type=%d, md5=%s\n", size, fileType, expectedMd5.c_str());
    
    // Set MD5 for verification
    if (expectedMd5.length() > 0) {
        Update.setMD5(expectedMd5.c_str());
    }
    
    // Determine partition
    int updatePartition = U_FLASH;
#if defined(ESP32)
    if (fileType == FILE_TYPE_FILESYSTEM) { updatePartition = U_SPIFFS; }
#endif
#if defined(ESP8266)
    if (fileType == FILE_TYPE_FILESYSTEM) { updatePartition = U_FS; }
#endif
    
    // End filesystem before update
    fsEnd();
    _fsEnded = true;
    
#if defined(ESP8266)
    Update.runAsync(true);
#endif
    
    if (!Update.begin(size, updatePartition)) {
        DEBUGOTACLIENT("Update.begin failed!\n");
#ifdef DEBUG_OTA
        Update.printError(Serial);
#endif
        ledMacrosUpdateError();
        // Remount filesystem
        fsRemount();
        _fsEnded = false;
        return false;
    }

    // Моргание обновления: прошивка или файловая система
    if (fileType == FILE_TYPE_FIRMWARE) { ledMacrosUpdateFirmware(); }
    if (fileType == FILE_TYPE_FILESYSTEM) { ledMacrosUpdateFilesystem(); }
    
    // Stream data in chunks
    uint8_t buf[OTACLIENT_CHUNK_SIZE];
    size_t totalWritten = 0;
    size_t remaining = size;
    
    while (remaining > 0) {
        size_t toRead = (remaining < OTACLIENT_CHUNK_SIZE) ? remaining : OTACLIENT_CHUNK_SIZE;
        int bytesRead = stream.readBytes(buf, toRead);
        
#if defined(ESP8266)
        ESP.wdtFeed();
#endif
        
        if (bytesRead <= 0) {
            DEBUGOTACLIENT("Stream read error at %u/%u\n", totalWritten, size);
#if defined(ESP32)
            Update.abort();
#endif
#if defined(ESP8266)
            Update.end();
#endif
            ledMacrosUpdateError();
            fsRemount();
            _fsEnded = false;
            return false;
        }
        
        size_t written = Update.write(buf, bytesRead);
        if (written != (size_t)bytesRead) {
            DEBUGOTACLIENT("Update.write error: wrote %u of %u\n", written, bytesRead);
#if defined(ESP32)
            Update.abort();
#endif
#if defined(ESP8266)
            Update.end();
#endif
            ledMacrosUpdateError();
            fsRemount();
            _fsEnded = false;
            return false;
        }
        
        totalWritten += written;
        remaining -= bytesRead;
        
        // Progress (every 5%, no duplicates)
        uint16_t percent = (totalWritten * 100) / size;
        if ((percent % 5) == 0 && (percent != _lastReportedPercent)) {
            _lastReportedPercent = percent;
            DEBUGOTACLIENT("Update progress: %u%%\n", percent);
        }
    }
    
    // Finalize update
    if (!Update.end(true)) {
        DEBUGOTACLIENT("Update.end failed!\n");
#ifdef DEBUG_OTA
        Update.printError(Serial);
#endif
        ledMacrosUpdateError();
        fsRemount();
        _fsEnded = false;
        return false;
    }
    
    DEBUGOTACLIENT("Update successful! MD5: %s\n", Update.md5String().c_str());
    
    // Update was successful, FS will be remounted after restart
    _fsEnded = true;
    
    return true;
}

// ============================================================
// DOWNLOAD AND UPDATE a single file
// ============================================================
bool CLASS_MODULE_OTACLIENT::downloadAndUpdate(const String& url, size_t size, const String& md5, int fileType) {
    DEBUGOTACLIENT("downloadAndUpdate: %s (%u bytes)\n", url.c_str(), size);

    // url вида http://host:port/path — разбираем вручную (локальный сервер, только HTTP).
    String rest = url;
    if (rest.startsWith("http://")) { rest = rest.substring(7); }
    int slash = rest.indexOf('/');
    String hostPort = (slash >= 0) ? rest.substring(0, slash) : rest;
    String path = (slash >= 0) ? rest.substring(slash) : String("/");

    String host = hostPort;
    uint16_t port = 80;
    int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        port = (uint16_t)hostPort.substring(colon + 1).toInt();
    }

    WiFiClient client;
    int status = 0;
    bool chunked = false;
    long contentLen = -1;
    if (!httpGetOpen(client, host, port, path, status, chunked, contentLen)) {
        DEBUGOTACLIENT("downloadAndUpdate: connect/open failed\n");
        return false;
    }
    if (status != 200) {
        DEBUGOTACLIENT("downloadAndUpdate: HTTP error %d\n", status);
        client.stop();
        return false;
    }

    // Verify content length matches expected size (при chunked длина неизвестна)
    if (!chunked && contentLen > 0 && (size_t)contentLen != size) {
        DEBUGOTACLIENT("downloadAndUpdate: size mismatch! expected %u, got %ld\n", size, contentLen);
        client.stop();
        return false;
    }

    client.setTimeout(30000); // 30 second timeout for download
    HttpBodyStream body(client, chunked);
    bool success = performUpdateFromStream(body, size, md5, fileType);
    client.stop();
    return success;
}

// ============================================================
// CHECK FOR UPDATES (main logic)
// ============================================================
void CLASS_MODULE_OTACLIENT::checkForUpdates() {
    DEBUGOTACLIENT("checkForUpdates from timer\n\r");
    
    // Don't start if already updating
    if (_updateInProgress) {
        DEBUGOTACLIENT("Update already in progress, skipping\n");
        return;
    }
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
        DEBUGOTACLIENT("WiFi not connected, skipping\n");
        return;
    }
    
    // Fetch manifest (using static buffer to avoid stack allocation)
    int entryCount = 0;
    
    if (!fetchManifest(_manifestEntries, entryCount)) {
        DEBUGOTACLIENT("Failed to fetch manifest\n");
        return;
    }
    
    DEBUGOTACLIENT("Manifest has %d entries\n", entryCount);
    
    fileCompareResult fwResult, fsResult;
    bool fwValid, fsValid;
    int8_t fwCompareResult, fsCompareResult;
    ManifestEntry* firmwareEntry = NULL;
    ManifestEntry* fsEntry = NULL;
    
    checkManifestEntries(_manifestEntries, entryCount, fwResult, fsResult, fwValid, fsValid,
                         fwCompareResult, fsCompareResult,
                         firmwareEntry, fsEntry);
    
    // Decision logic:
    // If both are valid and have the same version -> update FS first, then firmware
    // If only one is valid -> update that one
    // If both valid but different versions -> update each independently (FS first)
    
    _updateInProgress = true;
    
    // Stop the loop task timer to prevent re-entrancy during update
    DelTimerTask(otaclientLoopTask);
    
    // First, update filesystem if valid
    if (fsValid && fsEntry) {
        String url = "http://" + _config.serverAddress + ":" + String(_config.serverPort) + "/firmware/" + fsEntry->name;
        DEBUGOTACLIENT("Updating FS from %s\n", url.c_str());
        
        bool fsOk = downloadAndUpdate(url, fsEntry->size, fsEntry->md5, FILE_TYPE_FILESYSTEM);
        if (fsOk) {
            DEBUGOTACLIENT("FS update successful, restarting...\n");
            delay(500);
            ESPHTTPServer.restart_esp();
            return;
        } else {
            DEBUGOTACLIENT("FS update failed\n");
        }
    }
    
    // Then, update firmware if valid
    if (fwValid && firmwareEntry) {
        String url = "http://" + _config.serverAddress + ":" + String(_config.serverPort) + "/firmware/" + firmwareEntry->name;
        DEBUGOTACLIENT("Updating firmware from %s\n", url.c_str());
        
        bool fwOk = downloadAndUpdate(url, firmwareEntry->size, firmwareEntry->md5, FILE_TYPE_FIRMWARE);
        if (fwOk) {
            DEBUGOTACLIENT("Firmware update successful, restarting...\n");
            delay(500);
            ESPHTTPServer.restart_esp();
            // Never reaches here after restart
            return;
        } else {
            DEBUGOTACLIENT("Firmware update failed\n");
        }
    }
    
    _updateInProgress = false;
    
    // Re-enable loop task timer after update completes (or fails)
    SetTimerTask(otaclientLoopTask, 50);
    
    // If FS was ended during update attempt, try to remount it
    if (_fsEnded) {
        fsRemount();
        _fsEnded = false;
    }
    
    if (!fwValid && !fsValid) {
        DEBUGOTACLIENT("No valid updates found (server version not newer than current)\n");
    }
}
