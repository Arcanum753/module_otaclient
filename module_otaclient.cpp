#include <Arduino.h>
#include "version.h"
#include "main.h"

#include "core_ntp/NtpClientLib.h"
#include "core_json/core_json.h"
#include "core_sys/core_sys.h"
#include "module_otaclient/module_otaclient.h"
#include "core_sys/eertos.h"
#include "common/common.h"
#include "module_otaclient_version.h"

// Single global object - the class itself
CLASS_MODULE_OTACLIENT module_otaclient; 

CLASS_MODULE_OTACLIENT::CLASS_MODULE_OTACLIENT() {
    _isStarted = false;
    _updateInProgress = false;
    _updateRetries = 0;
    _lastReportedPercent = 0;
    _testStatusCode = OTACLIENT_TEST_IDLE;
    _testCompareResult = 0;
    _testFwCompareResult = 0;
    _testFsCompareResult = 0;
    _fsEnded = false;
}

// ============================================================
// begin()
// ============================================================
void CLASS_MODULE_OTACLIENT::begin(String _hostname, String _password) {
    CLASS_CORE_OTA::begin(_hostname, _password);
    defaultConfig();
    if ( load_config() == false) {save_config();}
    DEBUGOTACLIENT("%s\r\n", __FUNCTION__);
    if (_isStarted){    return;    }
    
    _isStarted = true;
    SetTimerTask(otaclientTimer, SEC * MINUTES * module_otaclient.getTimeOut());
    SetTimerTask(otaclientLoopTask, 50);
}

void CLASS_MODULE_OTACLIENT::begin(ModContext& ctx) {
    _fs = ctx.fs;
    begin(ctx.hostname, ctx.password);
}

// ============================================================
// web_Init()
// ============================================================
void CLASS_MODULE_OTACLIENT::web_Init(void) {
    registerCommonRoutes();
    registerCustomRoutes();
}

void CLASS_MODULE_OTACLIENT::registerCustomRoutes() {
    ESPHTTPServer.on(HTML_FILE_OTACLIENT, HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) {return request->requestAuthentication(); }
        get_configuration_html(request);
    });
    
    ESPHTTPServer.on("/otaclient/info", HTTP_GET, [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) {return request->requestAuthentication(); }
        send_configuration_values_html(request);
    });
    
    ESPHTTPServer.on("/otaclient/test", [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) {return request->requestAuthentication(); }
        test(request);
    });
    
    // Test status endpoint (polled by JS)
    ESPHTTPServer.on("/otaclient/teststatus", [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) {return request->requestAuthentication(); }
        String json = "{";
        json += "\"code\":" + String(_testStatusCode) + ",";
        json += "\"message\":\"" + _testStatusMessage + "\",";
        json += "\"name\":\"" + _testUpdateName + "\",";
        json += "\"url\":\"" + _testUpdateUrl + "\",";
        json += "\"size\":" + String(_testUpdateSize) + ",";
        json += "\"md5\":\"" + _testUpdateMd5 + "\",";
        json += "\"compare\":" + String(_testCompareResult);
        json += "}";
        request->send(200, "application/json", json);
    });
    
    // Update trigger endpoint
    ESPHTTPServer.on("/otaclient/update", [this](AsyncWebServerRequest *request) {
        if (!ESPHTTPServer.checkAuth(request)) {return request->requestAuthentication(); }
        if (_testStatusCode == OTACLIENT_TEST_UPDATE_AVAIL && _testUpdateName.length() > 0) {
            _testStatusCode = OTACLIENT_TEST_UPDATING;
            _testStatusMessage = "Updating...";
            request->send(200, "text/plain", "Update started");
            // Run update asynchronously via loop()
        } else {
            request->send(400, "text/plain", "No update available");
        }
    });

    ESPHTTPServer.on("/otaclient/ver", [this](AsyncWebServerRequest *request) {
        html_ver_get(request);
    });
}

// ============================================================
// Веб-обработчики
// ============================================================

// ========== TEST (async - just sets flag) ==========
void CLASS_MODULE_OTACLIENT::test(AsyncWebServerRequest *request) {
    DEBUGOTACLIENT("%s\n\r", __FUNCTION__);
    
    // Защита от множественных тестов: если тест уже выполняется, отклоняем
    if (_testStatusCode == OTACLIENT_TEST_PENDING) {
        DEBUGOTACLIENT("Test already in progress, rejecting\n");
        request->send(429, "text/plain", "Test already in progress");
        return;
    }
    
    _testStatusCode = OTACLIENT_TEST_PENDING;
    _testStatusMessage = "Checking...";
    _testUpdateName = "";
    _testUpdateUrl = "";
    _testUpdateSize = 0;
    _testUpdateMd5 = "";
    _testUpdateFileType = -1;
    _testCompareResult = 0;
    request->send(200, "text/plain", "OK");
}

// ========== SEND CONFIG HTML ==========
void CLASS_MODULE_OTACLIENT::send_configuration_values_html(AsyncWebServerRequest *request) {
    DEBUGOTACLIENT("%s\n\r", __FUNCTION__);
    String values = "";
    values += "otaclienttime|"     + String(_config.timeOut) + "|input\n";
    values += "otaclientpoweron|"  + String(_config.powerOn ? "checked" : "") + "|chk\n";
    values += "otaclientaddr|"  + _config.serverAddress + "|input\n";
    values += "otaclientport|"  + String(_config.serverPort) + "|input\n";
    values += "otaclientmanifest|" + _config.manifestPath + "|input\n";
    request->send(200, "text/plain", values);
}

// ========== GET CONFIG HTML ==========
void CLASS_MODULE_OTACLIENT::get_configuration_html(AsyncWebServerRequest *request) {
    DEBUGOTACLIENT("%s\n\r", __PRETTY_FUNCTION__);
    _config.powerOn  = false; 
    if (request->args() > 0) {
        for (uint8_t i = 0; i < request->args(); i++) {
            DEBUGOTACLIENT("Arg %d: %s %s\r\n", i, 
                     request->argName(i).c_str(), 
                     request->arg(i).c_str());
            
            if (request->argName(i) == "otaclienttime")     { _config.timeOut = request->arg(i).toInt(); }
            if (request->argName(i) == "otaclientpoweron")  { _config.powerOn = true;  }
            if (request->argName(i) == "otaclientaddr")     { _config.serverAddress = urldecode(request->arg(i)); }
            if (request->argName(i) == "otaclientport")     { _config.serverPort = request->arg(i).toInt(); }
            if (request->argName(i) == "otaclientmanifest") { _config.manifestPath = urldecode(request->arg(i)); }
        }
        
        save_config();
        request->send(200, "application/json", "{\"success\":true}");
    }
    else {
        ESPHTTPServer.handleFileRead(request->url(), request);
    }
}

// ============================================================
// Конфиг
// ============================================================

// ========== DEFAULT CONFIG ==========
void CLASS_MODULE_OTACLIENT::defaultConfig() {
    _config.timeOut = OTACLIENT_TIME_DFLT;
    _config.powerOn = OTACLIENT_POWERON;
    _config.serverAddress = OTACLIENT_SERVER_ADDR;
    _config.serverPort = OTACLIENT_SERVER_PORT;
    _config.manifestPath = OTACLIENT_MANIFEST_PATH;
}

// ========== SAVE CONFIG ==========
bool CLASS_MODULE_OTACLIENT::save_config() {
    DEBUGOTACLIENT("%s\n\r", __PRETTY_FUNCTION__);
    JsonDocument doc;
    core_json.jsonFileLoadDoc(CONFIG_FILE_OTACLIENT, doc);
    doc["timeOut"] = _config.timeOut;
    doc["powerOn"] = _config.powerOn;
    doc["serverAddress"] = _config.serverAddress;
    doc["serverPort"] = _config.serverPort;
    doc["manifestPath"] = _config.manifestPath;
    return core_json.jsonFileSaveDoc(CONFIG_FILE_OTACLIENT, doc);
}

// ========== LOAD CONFIG ==========
bool CLASS_MODULE_OTACLIENT::load_config() {
    DEBUGOTACLIENT("%s\n\r", __PRETTY_FUNCTION__);
    JsonDocument doc;
    if (!core_json.jsonFileLoadDoc(CONFIG_FILE_OTACLIENT, doc)) return false;
    _config.timeOut = doc["timeOut"].as<uint16_t>();
    _config.powerOn = doc["powerOn"].as<bool>();
    _config.serverAddress = doc["serverAddress"].as<String>();
    _config.serverPort = doc["serverPort"].as<uint16_t>();
    _config.manifestPath = doc["manifestPath"].as<String>();
    
    DEBUGOTACLIENT("timeOut: %d\n\r", _config.timeOut);
    DEBUGOTACLIENT("powerOn: %d\n\r", _config.powerOn);
    DEBUGOTACLIENT("serverAddress: %s\n\r", _config.serverAddress.c_str());
    DEBUGOTACLIENT("serverPort: %d\n\r", _config.serverPort);
    DEBUGOTACLIENT("manifestPath: %s\n\r", _config.manifestPath.c_str());
    
    return true;
}

// ============================================================
// Версионные методы
// ============================================================

String CLASS_MODULE_OTACLIENT::getVersionStr(){
    return String(MODULE_OTACLIENT_VERSION);
}

String CLASS_MODULE_OTACLIENT::getGeneratedTime(){
    return String(MODULE_OTACLIENT_GENERATED_TIME);
}

String CLASS_MODULE_OTACLIENT::getCommitDateStr(){
    return String(MODULE_OTACLIENT_COMMIT_DATE_STR);
}

void CLASS_MODULE_OTACLIENT::html_ver_get(AsyncWebServerRequest *request) {
    DEBUGOTACLIENT("%s\n\r", __FUNCTION__);
    String values = "";
    values += "otaclientversion|"     + getVersionStr()    + "|div\n";
    values += "otaclientgentime|"     + getGeneratedTime() + "|div\n";
    values += "otaclientgendate|"     + getCommitDateStr() + "|div\n";
    request->send(200, "text/plain", values);
}
