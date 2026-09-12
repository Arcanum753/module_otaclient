#ifndef _MODULE_OTACLIENT_TYPES_h
#define _MODULE_OTACLIENT_TYPES_h

// ============================================================
// module_otaclient_types.h — типы, структуры и define'ы OTA-клиента.
// Реализация: module_otaclient.cpp (шаблон) и
// module_otaclient_engine.cpp (манифест, HTTP и загрузка).
// ============================================================

#include <Arduino.h>
#include <stdint.h>

#define CONFIG_FILE_OTACLIENT     "/config_otaclient.json"
#define HTML_FILE_OTACLIENT       "/otaclient.html"

#define OTACLIENT_TIME_DFLT       5
#define OTACLIENT_POWERON         true
#define OTACLIENT_SERVER_ADDR     "192.168.88.171"
#define OTACLIENT_SERVER_PORT     8080
#define OTACLIENT_MANIFEST_PATH   "/manifest.json"

// Max entries in manifest
#define OTACLIENT_MAX_MANIFEST_ENTRIES 20

// Max retries for update download
#define OTACLIENT_MAX_RETRIES     3

// Chunk size for streaming download
#if defined(ESP8266)
#define OTACLIENT_CHUNK_SIZE      512
#endif
#if !defined(ESP8266)
#define OTACLIENT_CHUNK_SIZE      1024
#endif

// Test result codes
#define OTACLIENT_TEST_IDLE       0
#define OTACLIENT_TEST_PENDING    1
#define OTACLIENT_TEST_SERVER_UNAVAIL 2
#define OTACLIENT_TEST_NO_UPDATES     3
#define OTACLIENT_TEST_UPDATE_AVAIL   4
#define OTACLIENT_TEST_UPDATING       5

typedef struct {
    uint16_t timeOut;
    bool  powerOn;
    String serverAddress;
    uint16_t serverPort;
    String manifestPath;
} strOtaClientConfig;

// Structure for a single manifest entry
struct ManifestEntry {
    String name;
    String type;     // "firmware" or "filesystem"
    size_t size;
    String md5;
};

#endif // _MODULE_OTACLIENT_TYPES_h
