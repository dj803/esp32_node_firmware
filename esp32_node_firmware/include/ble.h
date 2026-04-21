#pragma once
#ifdef BLE_ENABLED

// =============================================================================
// ble.h  —  Passive BLE beacon scanner  (NimBLE-Arduino 2.x backend)
//
// ROLE: BLE central only (scanner). No advertising, no GATT server.
//       The ESP32 never pairs or connects to beacons — it only reads their
//       broadcast advertisements.
//
// WHY NimBLE: Bluedroid (the default ESP32 BLE library) adds ~700 KB to the
//       binary. NimBLE-Arduino adds ~150 KB — a saving of ~550 KB that keeps
//       the firmware within the min_spiffs OTA partition limit (1.9 MB).
//
// NimBLE-Arduino 2.x API differences from 1.x:
//   - Callbacks class: NimBLEScanCallbacks (was NimBLEAdvertisedDeviceCallbacks)
//   - onResult: const NimBLEAdvertisedDevice* (const pointer, not value)
//   - onScanEnd: replaces the free-function callback passed to start()
//   - setScanCallbacks: replaces setAdvertisedDeviceCallbacks
//   - start(durationS, is_continue): no longer takes a callback argument
//
// MQTT INTERFACE (all topics relative to device base topic):
//   cmd/ble/scan              → trigger a 5-second full scan
//   cmd/ble/track  {"mac":"AA:BB:CC:DD:EE:FF"}  → set tracked beacon (persisted)
//   cmd/ble/clear             → clear tracked beacon
//   cmd/ble/list              → re-publish last scan results without new scan
//
//   telemetry/ble/scan_results → {"count":N,"beacons":[{mac,name,rssi,dist_m},...]}
//   telemetry/ble              → {"mac":"...","rssi":-65,"dist_m":2.1,"name":"..."}
//                                published every BLE_MQTT_PUBLISH_MS (2 s) when tracking
//
// DISTANCE MODEL:
//   d = 10 ^ ((txPower - RSSI) / (10 * n))
//   txPower: measured power at 1 m from iBeacon payload, or BLE_DEFAULT_TX_POWER
//   n: BLE_PATH_LOSS_N (2.0 = free space; 2.5-3.0 for indoor environments)
//
// THREAD SAFETY:
//   _bleBeacons[] / _bleBeaconCount — protected by _bleMutex
//   volatile bool flags             — single-writer pattern, safe on Xtensa
//   All NVS writes happen in bleLoop() on the main Arduino loop task
//   BLE callbacks never call mqttPublish, NVS, or any blocking function
//
// INCLUDE ORDER: ble.h MUST come after mqtt_client.h (uses mqttPublish)
//                and after rfid.h — include it last in esp32_firmware.ino
// =============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>

// ── NimBLE-Arduino 2.x API guard ──────────────────────────────────────────────
// The callback shape, start() signature, and setScanCallbacks() name above all
// depend on 2.x. A silent downgrade to 1.x (e.g. via platformio.ini edit) would
// still compile most of ble.h because 1.x symbols partially overlap — but
// `pScan->start(5, false)` has different argument semantics in 1.x and would
// produce subtle runtime breakage (scans never completing, callbacks firing on
// stack-allocated values). Fail the build instead.
#if !defined(NIMBLE_CPP_VERSION_MAJOR) || NIMBLE_CPP_VERSION_MAJOR < 2
#  error "ble.h requires NimBLE-Arduino 2.x. Bump lib_deps to >=2.0.0 or remove BLE_ENABLED."
#endif

#include "config.h"
#include "ranging_math.h"   // rssiToDistance() — shared with espnow_ranging.h


// ── Beacon record ─────────────────────────────────────────────────────────────
struct BleBeacon {
    char     mac[18];       // "AA:BB:CC:DD:EE:FF\0"
    char     name[32];      // advertised local name — may be empty
    int8_t   rssi;          // dBm at time of scan
    int8_t   txPower;       // measured power at 1 m (from iBeacon) or BLE_DEFAULT_TX_POWER
    float    distM;         // estimated distance in metres
    uint16_t companyId;     // manufacturer company ID (0 = not present), e.g. 0x07D0 = Tuya
    char     svcUuid[5];    // first 16-bit service UUID as uppercase hex, e.g. "FD50" (empty if none)
};


// ── Module state ──────────────────────────────────────────────────────────────
static BleBeacon         _bleBeacons[BLE_MAX_BEACONS];
static uint8_t           _bleBeaconCount     = 0;

// Tracked beacons — up to BLE_MAX_TRACKED, persisted in NVS as CSV
static uint8_t           _bleTrackedCount               = 0;
static char              _bleTrackedMac[BLE_MAX_TRACKED][18]  = {};
static int8_t            _bleTrackedRssi[BLE_MAX_TRACKED]     = {};
static float             _bleTrackedDistM[BLE_MAX_TRACKED]    = {};
static char              _bleTrackedName[BLE_MAX_TRACKED][32] = {};
static uint32_t          _bleTrackedSeenMs[BLE_MAX_TRACKED]   = {};  // 0 = never seen

// Scan control flags — written from BLE stack task, read from loop task
static volatile bool     _bleScanInProgress  = false;
static volatile bool     _bleScanDone        = false;

// Command flags — written from MQTT lwIP task, consumed in bleLoop() (loop task)
static volatile bool     _bleScanRequested   = false;
static volatile bool     _bleClearPending    = false;
static volatile bool     _bleListPending     = false;
static volatile bool     _bleTrackPending    = false;
static char              _blePendingMacs[BLE_MAX_TRACKED][18] = {};
static uint8_t           _blePendingMacCount = 0;

// Timers
static uint32_t          _bleMqttPublishMs   = 0;
static uint32_t          _bleSerialPrintMs   = 0;
static uint32_t          _bleLastTrackScanMs = 0;

// FreeRTOS mutex — protects _bleBeacons[] / _bleBeaconCount
static SemaphoreHandle_t _bleMutex           = nullptr;


// _bleCalcDist — thin wrapper around the shared rssiToDistance() from ranging_math.h.
// BLE uses BLE_PATH_LOSS_N (2.0) while ESP-NOW uses ESPNOW_PATH_LOSS_N (2.5).
static float _bleCalcDist(int8_t rssi, int8_t txPower) {
    return rssiToDistance(rssi, txPower, BLE_PATH_LOSS_N);
}


// ── NVS helpers ───────────────────────────────────────────────────────────────
static void _bleNvsLoad() {
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/true);
    String csv = p.getString(BLE_NVS_KEY_TRACKED, "");
    p.end();
    _bleTrackedCount = 0;
    if (csv.length() == 0) return;
    // Parse comma-separated list of MACs
    int start = 0;
    while (start < (int)csv.length() && _bleTrackedCount < BLE_MAX_TRACKED) {
        int comma = csv.indexOf(',', start);
        String mac = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
        mac.trim();
        if (mac.length() == 17) {
            mac.toCharArray(_bleTrackedMac[_bleTrackedCount], 18);
            _bleTrackedCount++;
        }
        if (comma < 0) break;
        start = comma + 1;
    }
}

static void _bleNvsSave() {
    String csv = "";
    for (uint8_t i = 0; i < _bleTrackedCount; i++) {
        if (i > 0) csv += ",";
        csv += _bleTrackedMac[i];
    }
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/false);
    p.putString(BLE_NVS_KEY_TRACKED, csv);
    p.end();
}

static void _bleNvsClear() {
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/false);
    p.remove(BLE_NVS_KEY_TRACKED);
    p.end();
}


// ── Scan callbacks (NimBLE-Arduino 2.x) ──────────────────────────────────────
// NimBLE 2.x uses NimBLEScanCallbacks which handles both per-device results
// (onResult) and scan completion (onScanEnd) in a single class.
//
// onResult runs in the BLE stack task (Core 0) — must be fast and non-blocking.
// Drops the result silently if the mutex is already held (best-effort).
class _BleScanCb : public NimBLEScanCallbacks {

    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (xSemaphoreTake(_bleMutex, 0) != pdTRUE) return;

        // ── Extract fields we need for both tracked-update and beacon list ────
        std::string mac_str = dev->getAddress().toString();
        int8_t rssi = (int8_t)dev->getRSSI();

        char   name[32]  = {0};
        int8_t txPower   = BLE_DEFAULT_TX_POWER;
        uint16_t companyId = 0;
        char   svcUuid[5] = {0};

        if (dev->haveName()) {
            std::string s = dev->getName();
            strncpy(name, s.c_str(), sizeof(name) - 1);
        }
        if (dev->haveManufacturerData()) {
            std::string mfr = dev->getManufacturerData();
            if (mfr.size() >= 2)
                companyId = (uint16_t)((uint8_t)mfr[0])
                          | (uint16_t)((uint8_t)mfr[1] << 8);
            // iBeacon: Apple 0x004C, type 0x02, length 0x15 → txPower at byte [24]
            if (mfr.size() >= 25
                && (uint8_t)mfr[0] == 0x4C && (uint8_t)mfr[1] == 0x00
                && (uint8_t)mfr[2] == 0x02 && (uint8_t)mfr[3] == 0x15)
                txPower = (int8_t)mfr[24];
        }
        if (txPower == BLE_DEFAULT_TX_POWER && dev->haveTXPower())
            txPower = (int8_t)dev->getTXPower();

        if (dev->haveServiceUUID()) {
            NimBLEUUID uuid = dev->getServiceUUID(0);
            if (uuid.bitSize() == 16) {
                std::string s = uuid.toString();
                strncpy(svcUuid, s.c_str(), sizeof(svcUuid) - 1);
                for (char* p = svcUuid; *p; p++) *p = toupper((unsigned char)*p);
            }
        }

        float distM = _bleCalcDist(rssi, txPower);

        // ── Update all tracked beacon caches, even when the list is full ────
        // BUG FIX: previously this was inside the count-guard, so once 32 unique
        // MACs filled _bleBeacons[], the tracked beacon's seen-time never updated
        // and tracking silently stopped until reset.
        for (uint8_t ti = 0; ti < _bleTrackedCount; ti++) {
            if (_bleTrackedMac[ti][0] != '\0'
                && strcasecmp(mac_str.c_str(), _bleTrackedMac[ti]) == 0) {
                _bleTrackedRssi[ti]  = rssi;
                _bleTrackedDistM[ti] = distM;
                strncpy(_bleTrackedName[ti], name, sizeof(_bleTrackedName[ti]) - 1);
                _bleTrackedName[ti][sizeof(_bleTrackedName[ti]) - 1] = '\0';
                _bleTrackedSeenMs[ti] = millis();
            }
        }

        // ── Append to beacon list (best-effort, silently drops when full) ─────
        if (_bleBeaconCount < BLE_MAX_BEACONS) {
            BleBeacon& b = _bleBeacons[_bleBeaconCount++];
            strncpy(b.mac, mac_str.c_str(), 17);
            b.mac[17] = '\0';
            strncpy(b.name, name, sizeof(b.name) - 1);
            b.name[sizeof(b.name) - 1] = '\0';
            b.rssi      = rssi;
            b.txPower   = txPower;
            b.distM     = distM;
            b.companyId = companyId;
            strncpy(b.svcUuid, svcUuid, sizeof(b.svcUuid) - 1);
            b.svcUuid[sizeof(b.svcUuid) - 1] = '\0';
        }

        xSemaphoreGive(_bleMutex);
    }

    // Replaces the free-function scan-done callback that NimBLE 1.x passed to start()
    void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
        _bleScanInProgress = false;
        _bleScanDone       = true;
    }
};


// ── Scan launcher ─────────────────────────────────────────────────────────────
// durationS: BLE_SCAN_DURATION_S (full scan)
//         or BLE_TRACK_SCAN_DURATION_S (tracking scan)
//
// IMPORTANT: NimBLE-Arduino 2.x start() takes MILLISECONDS (1.x used seconds).
//   Callbacks are set once in bleInit() — do NOT call setScanCallbacks here.
//
// IMPORTANT: start() can return false under WiFi/BLE radio contention. When it
//   does, onScanEnd never fires, so _bleScanInProgress must be cleared here or
//   the loop deadlocks waiting for a completion that never comes.
static void _bleRunScan(uint8_t durationS) {
    _bleScanInProgress = true;
    _bleScanDone       = false;

    // Reset beacon list before every scan — both full and tracking scans.
    // Without this, tracking scans (which never used to reset) would fill all
    // BLE_MAX_BEACONS slots with unique MACs, causing onResult to silently drop
    // the tracked beacon and stopping all distance/RSSI updates.
    if (xSemaphoreTake(_bleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _bleBeaconCount = 0;
        memset(_bleBeacons, 0, sizeof(_bleBeacons));
        xSemaphoreGive(_bleMutex);
    }

    NimBLEScan* pScan = NimBLEDevice::getScan();
    // Tracking scans use active mode → requests scan responses → gets name + extra data.
    // Full discovery scans stay passive → less radio contention with WiFi.
    pScan->setActiveScan(durationS == BLE_TRACK_SCAN_DURATION_S);
    // NimBLE 2.x: duration is in MILLISECONDS — multiply seconds constant by 1000
    bool ok = pScan->start((uint32_t)durationS * 1000UL, /*is_continue=*/false);
    if (!ok) {
        // start() failed — clear the in-progress flag so bleLoop() retries next cycle
        _bleScanInProgress = false;
        Serial.println("[BLE] scan start failed (radio busy) — will retry");
    }
    // On success: returns immediately; completion fires _BleScanCb::onScanEnd
}


// ── Publish helpers ───────────────────────────────────────────────────────────
// mqttPublish() is defined in mqtt_client.h which is included before ble.h.

static void _blePublishResults() {
    // Publish full beacon list to telemetry/ble/scan_results
    JsonDocument doc;
    doc["count"] = 0;
    JsonArray arr = doc["beacons"].to<JsonArray>();

    if (xSemaphoreTake(_bleMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        doc["count"] = _bleBeaconCount;
        for (uint8_t i = 0; i < _bleBeaconCount; i++) {
            JsonObject o = arr.add<JsonObject>();
            o["mac"]        = _bleBeacons[i].mac;
            o["name"]       = _bleBeacons[i].name;
            o["rssi"]       = _bleBeacons[i].rssi;
            o["dist_m"]     = serialized(String(_bleBeacons[i].distM, 1));
            o["company_id"] = _bleBeacons[i].companyId;   // 76 = 0x004C Apple iBeacon; 2000 = 0x07D0 Tuya
            if (_bleBeacons[i].svcUuid[0] != '\0')
                o["svc_uuid"] = _bleBeacons[i].svcUuid;   // e.g. "FD50" for Tuya, "FDA5" for Holy-IOT
        }
        xSemaphoreGive(_bleMutex);
    }

    mqttPublishJson("ble/scan_results", doc);
}

static void _blePublishTracked() {
    // Publish one MQTT message per tracked beacon — skip stale (> 10 s) or unseen
    uint32_t now = millis();
    for (uint8_t i = 0; i < _bleTrackedCount; i++) {
        if (_bleTrackedMac[i][0] == '\0' || _bleTrackedSeenMs[i] == 0) continue;
        if (now - _bleTrackedSeenMs[i] > 10000UL) continue;

        JsonDocument doc;
        doc["mac"]    = _bleTrackedMac[i];
        doc["rssi"]   = _bleTrackedRssi[i];
        doc["dist_m"] = serialized(String(_bleTrackedDistM[i], 1));
        doc["name"]   = _bleTrackedName[i];

        mqttPublishJson("ble", doc);
    }
}


// ── Public API — called from mqtt_client.h handlers ──────────────────────────
// These are called from the MQTT lwIP task. They ONLY set volatile flags or
// copy into a staging buffer — never touch NVS or BLE directly.

inline void bleTriggerScan() {
    _bleScanRequested = true;
}

inline void bleSetTrackedMacs(const char** macs, uint8_t count) {
    uint8_t n = (count < BLE_MAX_TRACKED) ? count : BLE_MAX_TRACKED;
    for (uint8_t i = 0; i < n; i++) {
        strncpy(_blePendingMacs[i], macs[i], 17);
        _blePendingMacs[i][17] = '\0';
    }
    _blePendingMacCount = n;
    _bleTrackPending = true;
}

inline void bleClearTracked() {
    _bleClearPending = true;
}

inline void blePublishList() {
    _bleListPending = true;
}


// ── bleInit ───────────────────────────────────────────────────────────────────
// Static callback instance — allocated once, reused for every scan.
// Must outlive all scan calls; static storage duration satisfies this.
static _BleScanCb _bleScanCbInstance;

inline void bleInit() {
    _bleMutex = xSemaphoreCreateMutex();
    _bleNvsLoad();   // restore tracked MAC from NVS

    NimBLEDevice::init("");  // empty device name — we are not advertising

    // Configure scan object once — callbacks, mode, and timing persist across
    // start() calls. setScanCallbacks must NOT be called per-scan (memory leak).
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(&_bleScanCbInstance, /*wantDuplicates=*/false);
    // setActiveScan is NOT set here — it is set per-scan in _bleRunScan()
    // so discovery scans can stay passive while tracking scans use active mode
    pScan->setInterval(100);      // ms
    pScan->setWindow(99);         // ms

    if (_bleTrackedCount > 0) {
        Serial.printf("[BLE] scanner ready (NimBLE) — tracking %u beacon(s)\n", _bleTrackedCount);
        for (uint8_t i = 0; i < _bleTrackedCount; i++)
            Serial.printf("[BLE]   %s\n", _bleTrackedMac[i]);
    } else {
        Serial.println("[BLE] scanner ready (NimBLE) — no beacon tracked");
    }
}


// ── bleLoop ───────────────────────────────────────────────────────────────────
// Called from Arduino loop(). Lightweight on every tick — only runs work when
// flags are set or timers fire.
inline void bleLoop() {
    uint32_t now = millis();

    // ── Consume deferred MQTT commands (written by lwIP task) ─────────────────
    if (_bleTrackPending) {
        _bleTrackPending = false;
        _bleTrackedCount = _blePendingMacCount;
        for (uint8_t i = 0; i < _bleTrackedCount; i++) {
            strncpy(_bleTrackedMac[i], _blePendingMacs[i], 17);
            _bleTrackedMac[i][17] = '\0';
            _bleTrackedSeenMs[i]  = 0;
            Serial.printf("[BLE] now tracking %s\n", _bleTrackedMac[i]);
        }
        _bleNvsSave();
    }

    if (_bleClearPending) {
        _bleClearPending    = false;
        _bleTrackedCount    = 0;
        memset(_bleTrackedMac,     0, sizeof(_bleTrackedMac));
        memset(_bleTrackedSeenMs,  0, sizeof(_bleTrackedSeenMs));
        _bleLastTrackScanMs = 0;
        _bleNvsClear();
        Serial.println("[BLE] tracking cleared");
    }

    if (_bleListPending) {
        _bleListPending = false;
        _blePublishResults();
    }

    // ── On-demand full scan ───────────────────────────────────────────────────
    if (_bleScanRequested && !_bleScanInProgress) {
        _bleScanRequested = false;
        Serial.println("[BLE] starting full scan (5 s)");
        _bleRunScan(BLE_SCAN_DURATION_S);
    }

    // ── Repeated tracking scan — fires every BLE_MQTT_PUBLISH_MS when tracking ─
    bool trackingActive = (_bleTrackedCount > 0);
    if (trackingActive && !_bleScanInProgress
        && (now - _bleLastTrackScanMs >= BLE_MQTT_PUBLISH_MS)) {
        _bleLastTrackScanMs = now;
        _bleRunScan(BLE_TRACK_SCAN_DURATION_S);
    }

    // ── Handle scan completion ────────────────────────────────────────────────
    if (_bleScanDone) {
        _bleScanDone = false;
        _bleLastTrackScanMs = now;  // defer next tracking scan — prevents it firing
                                    // immediately after a full 5 s scan completes
        _blePublishResults();   // always update Node-RED beacon list
    }

    // ── MQTT publish every 2 s ────────────────────────────────────────────────
    if (trackingActive && (now - _bleMqttPublishMs >= BLE_MQTT_PUBLISH_MS)) {
        _bleMqttPublishMs = now;
        _blePublishTracked();
    }

    // ── Serial print every 10 s ───────────────────────────────────────────────
    if (trackingActive && (now - _bleSerialPrintMs >= BLE_SERIAL_PRINT_MS)) {
        _bleSerialPrintMs = now;
        for (uint8_t i = 0; i < _bleTrackedCount; i++) {
            if (_bleTrackedSeenMs[i] > 0 && (now - _bleTrackedSeenMs[i]) < 10000UL) {
                Serial.printf("[BLE] %s  RSSI=%d dBm  dist=%.1f m  name=%s\n",
                              _bleTrackedMac[i], _bleTrackedRssi[i],
                              _bleTrackedDistM[i], _bleTrackedName[i]);
            } else {
                Serial.printf("[BLE] %s  — not seen in last 10 s\n", _bleTrackedMac[i]);
            }
        }
    }
}

#endif // BLE_ENABLED
