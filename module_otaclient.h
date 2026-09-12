#ifndef _MODOTACLIENT_h
#define _MODOTACLIENT_h

#include "main.h"
#include "core_web/FSWebServerLib.h"
#include "core_ota/core_ota.h"

#ifdef DEBUG_OTACLIENT
#define DEBUGOTACLIENT(...) DBG_MOD("[M_OTACLIENT] ", __VA_ARGS__)
#else
#define DEBUGOTACLIENT(...)
#endif

#include "module_otaclient_types.h"

void otaclientTimer(void);
void otaclientLoopTask(void);

class CLASS_MODULE_OTACLIENT : public CLASS_CORE_OTA {
public:
    CLASS_MODULE_OTACLIENT();
    
    void web_Init() override;
    void begin(String _hostname, String _password);
    void begin(ModContext& ctx);
    void test(AsyncWebServerRequest *request);
    void loop();
    
    void send_configuration_values_html(AsyncWebServerRequest *request);
    void get_configuration_html(AsyncWebServerRequest *request);
    
    uint16_t getTimeOut();
    bool powerOnGet();
    String serverAddressGet();
    uint16_t serverPortGet();
    String manifestPathGet();
    String jsonGet();
    uint8_t isStart();
    
    // Called from WiFi connect hook
    void onWiFiConnect();
    
    // Main update check (called from timer)
    void checkForUpdates();
    
    strOtaClientConfig _config; // OTA Client configuration
   
private:
    bool load_config();
    bool save_config();
    void defaultConfig();
    
    // Manifest download and parsing
    bool fetchManifest(ManifestEntry* entries, int& count);
    
    // Download and flash a single file
    bool downloadAndUpdate(const String& url, size_t size, const String& md5, int fileType);
    
    // Perform actual update from stream
    bool performUpdateFromStream(Stream& stream, size_t size, const String& expectedMd5, int fileType);
    
    // Unified manifest entry checking (used by test() and checkForUpdates())
    void checkManifestEntries(ManifestEntry* entries, int count,
                              fileCompareResult& fwResult, fileCompareResult& fsResult,
                              bool& fwValid, bool& fsValid,
                              int8_t& fwCompareResult, int8_t& fsCompareResult,
                              ManifestEntry*& fwEntryOut, ManifestEntry*& fsEntryOut);
    
    String getVersionStr() override;
    String getGeneratedTime() override;
    String getCommitDateStr() override;
    void  html_ver_get(AsyncWebServerRequest *request) override;
    void registerCustomRoutes() override;
    
    uint8_t _isStarted;
    bool _updateInProgress;
    uint8_t _updateRetries;
    uint16_t _lastReportedPercent;  // For progress reporting (5% step)
    bool _fsEnded;                  // Track if FS was ended for update
    
    // Static buffer for manifest entries (avoid stack allocation)
    static ManifestEntry _manifestEntries[OTACLIENT_MAX_MANIFEST_ENTRIES];
    
    // Test/status fields
    uint8_t _testStatusCode;       // OTACLIENT_TEST_*
    String _testStatusMessage;     // human-readable message
    String _testUpdateName;        // filename of available update
    String _testUpdateUrl;         // full URL of available update
    size_t _testUpdateSize;        // size of available update
    String _testUpdateMd5;         // MD5 of available update
    int _testUpdateFileType;       // FILE_TYPE_FIRMWARE or FILE_TYPE_FILESYSTEM
    int8_t _testCompareResult;     // combined fsVersionCompare: 1=newer, 0=same, -1=older, -2=missing
    int8_t _testFwCompareResult;   // firmware-specific compare result
    int8_t _testFsCompareResult;   // filesystem-specific compare result
    
    friend void otaclientTimer();
};

extern CLASS_MODULE_OTACLIENT module_otaclient;

#endif // _MODOTACLIENT_h
