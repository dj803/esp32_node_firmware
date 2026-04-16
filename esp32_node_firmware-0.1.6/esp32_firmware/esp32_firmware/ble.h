#pragma once
#ifdef BLE_ENABLED

// =============================================================================
// ble.h  —  Passive BLE beacon scanner
//
// ROLE: BLE central only (scanner). No advertising, no GATT server.
//       The ESP32 never pairs or connects to beacons — it only reads their
//       broadcast advertisements.
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
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "config.h"


// ── Beacon record ─────────────────────────────────────────────────────────────
struct BleBeacon {
    char   mac[18];    // "AA:BB:CC:DD:EE:FF\0"
    char   name[32];   // advertised local name — may be empty
    int8_t rssi;       // dBm at time of scan
    int8_t txPower;    // measured power at 1 m (from iBeacon) or BLE_DEFAULT_TX_POWER
    float  distM;      // estimated distance in metres
};


// ── Module state ──────────────────────────────────────────────────────────────
static BleBeacon         _bleBeacons[BLE_MAX_BEACONS];
static uint8_t           _bleBeaconCount     = 0;

// Tracked beacon — persisted in NVS
static char              _bleTrackedMac[18]  = {0};  // empty = none
static int8_t            _bleTrackedRssi     = 0;
static float             _bleTrackedDistM    = 0.0f;
static char              _bleTrackedName[32] = {0};
static uint32_t          _bleTrackedSeenMs   = 0;    // millis() of last sighting; 0 = never

// Scan control flags — written from BLE stack task, read from loop task
static volatile bool     _bleScanInProgress  = false;
static volatile bool     _bleScanDone        = false;

// Command flags — written from MQTT lwIP task, consumed in bleLoop() (loop task)
static volatile bool     _bleScanRequested   = false;
static volatile bool     _bleClearPending    = false;
static volatile bool     _bleListPending     = false;
static volatile bool     _bleTrackPending    = false;
static char              _blePendingMac[18]  = {0};

// Timers
static uint32_t          _bleMqttPublishMs   = 0;
static uint32_t          _bleSerialPrintMs   = 0;
static uint32_t          _bleLastTrackScanMs = 0;

// FreeRTOS mutex — protects _bleBeacons[] / _bleBeaconCount
static SemaphoreHandle_t _bleMutex           = nullptr;


// ── Distance formula ──────────────────────────────────────────────────────────
static float _bleCalcDist(int8_t rssi, int8_t txPower) {
    return powf(10.0f, (float)(txPower - rssi) / (10.0f * BLE_PATH_LOSS_N));
}


// ── NVS helpers ───────────────────────────────────────────────────────────────
static void _bleNvsLoad() {
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/true);
    String mac = p.getString(BLE_NVS_KEY_TRACKED, "");
    p.end();
    if (mac.length() == 17)
        mac.toCharArray(_bleTrackedMac, sizeof(_bleTrackedMac));
}

static void _bleNvsSave(const char* mac) {
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/false);
    p.putString(BLE_NVS_KEY_TRACKED, mac);
    p.end();
}

static void _bleNvsClear() {
    Preferences p;
    p.begin(BLE_NVS_NAMESPACE, /*readOnly=*/false);
    p.remove(BLE_NVS_KEY_TRACKED);
    p.end();
}


// ── Scan result callback ──────────────────────────────────────────────────────
// Runs in the BLE stack task (Core 0). Must be fast and non-blocking.
// Drops the result silently if the mutex is already held (best-effort).
class _BleScanCb : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        if (xSemaphoreTake(_bleMutex, 0) != pdTRUE) return;

        if (_bleBeaconCount < BLE_MAX_BEACONS) {
            BleBeacon& b = _bleBeacons[_bleBeaconCount++];

            // MAC address
            strncpy(b.mac, dev.getAddress().toString().c_str(), 17);
            b.mac[17] = '\0';

            // Advertised local name
            if (dev.haveName()) {
                strncpy(b.name, dev.getName().c_str(), sizeof(b.name) - 1);
                b.name[sizeof(b.name) - 1] = '\0';
            } else {
                b.name[0] = '\0';
            }

            // RSSI
            b.rssi = (int8_t)dev.getRSSI();

            // TX power — prefer iBeacon measured power at byte [24]
            b.txPower = BLE_DEFAULT_TX_POWER;
            if (dev.haveManufacturerData()) {
                String mfr = dev.getManufacturerData();
                // iBeacon: company id 0x004C (little-endian), type 0x02, length 0x15
                if (mfr.length() >= 25
                    && (uint8_t)mfr[0] == 0x4C && (uint8_t)mfr[1] == 0x00
                    && (uint8_t)mfr[2] == 0x02 && (uint8_t)mfr[3] == 0x15) {
                    b.txPower = (int8_t)mfr[24];
                }
            }
            if (b.txPower == BLE_DEFAULT_TX_POWER && dev.haveTXPower())
                b.txPower = (int8_t)dev.getTXPower();

            b.distM = _bleCalcDist(b.rssi, b.txPower);

            // Update tracked beacon cache if this is the one we care about
            if (_bleTrackedMac[0] != '\0'
                && strcasecmp(b.mac, _bleTrackedMac) == 0) {
                _bleTrackedRssi  = b.rssi;
                _bleTrackedDistM = b.distM;
                strncpy(_bleTrackedName, b.name, sizeof(_bleTrackedName) - 1);
                _bleTrackedName[sizeof(_bleTrackedName) - 1] = '\0';
                _bleTrackedSeenMs = millis();
            }
        }

        xSemaphoreGive(_bleMutex);
    }
};


// ── Scan completion callback ──────────────────────────────────────────────────
// Runs in the BLE stack task — only sets flags, never blocks.
static void _bleScanDoneCb(BLEScanResults /*results*/) {
    _bleScanInProgress = false;
    _bleScanDone       = true;
}


// ── Scan launcher ─────────────────────────────────────────────────────────────
// durationS: BLE_SCAN_DURATION_S (full scan, clears list)
//         or BLE_TRACK_SCAN_DURATION_S (tracking scan, accumulates into list)
static void _bleRunScan(uint8_t durationS) {
    _bleScanInProgress = true;
    _bleScanDone       = false;

    // Full scans reset the beacon list; tracking scans accumulate
    if (durationS == BLE_SCAN_DURATION_S) {
        if (xSemaphoreTake(_bleMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            _bleBeaconCount = 0;
            memset(_bleBeacons, 0, sizeof(_bleBeacons));
            xSemaphoreGive(_bleMutex);
        }
    }

    BLEScan* pScan = BLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new _BleScanCb(), /*wantDuplicates=*/false);
    pScan->setActiveScan(false);  // passive — do not send scan requests
    pScan->setInterval(100);      // ms
    pScan->setWindow(99);         // ms — just under interval for maximum coverage
    pScan->start(durationS, _bleScanDoneCb, /*async=*/true);
    // Returns immediately; completion fires _bleScanDoneCb from BLE task
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
            o["mac"]    = _bleBeacons[i].mac;
            o["name"]   = _bleBeacons[i].name;
            o["rssi"]   = _bleBeacons[i].rssi;
            o["dist_m"] = serialized(String(_bleBeacons[i].distM, 1));
        }
        xSemaphoreGive(_bleMutex);
    }

    String payload;
    serializeJson(doc, payload);
    mqttPublish("ble/scan_results", payload);
}

static void _blePublishTracked() {
    // Publish tracked beacon to telemetry/ble — skip if never seen or stale (> 10 s)
    if (_bleTrackedMac[0] == '\0' || _bleTrackedSeenMs == 0) return;
    if (millis() - _bleTrackedSeenMs > 10000UL) return;

    JsonDocument doc;
    doc["mac"]    = _bleTrackedMac;
    doc["rssi"]   = _bleTrackedRssi;
    doc["dist_m"] = serialized(String(_bleTrackedDistM, 1));
    doc["name"]   = _bleTrackedName;

    String payload;
    serializeJson(doc, payload);
    mqttPublish("ble", payload);
}


// ── Public API — called from mqtt_client.h handlers ──────────────────────────
// These are called from the MQTT lwIP task. They ONLY set volatile flags or
// copy into a staging buffer — never touch NVS or BLE directly.

inline void bleTriggerScan() {
    _bleScanRequested = true;
}

inline void bleSetTrackedMac(const char* mac) {
    strncpy(_blePendingMac, mac, 17);
    _blePendingMac[17] = '\0';
    _bleTrackPending = true;
}

inline void bleClearTracked() {
    _bleClearPending = true;
}

inline void blePublishList() {
    _bleListPending = true;
}


// ── bleInit ───────────────────────────────────────────────────────────────────
inline void bleInit() {
    _bleMutex = xSemaphoreCreateMutex();
    _bleNvsLoad();   // restore tracked MAC from NVS

    BLEDevice::init("");  // empty device name — we are not advertising
    BLEDevice::getScan(); // initialise scan object

    if (_bleTrackedMac[0] != '\0')
        Serial.printf("[BLE] scanner ready — tracking %s\n", _bleTrackedMac);
    else
        Serial.println("[BLE] scanner ready — no beacon tracked");
}


// ── bleLoop ───────────────────────────────────────────────────────────────────
// Called from Arduino loop(). Lightweight on every tick — only runs work when
// flags are set or timers fire.
inline void bleLoop() {
    uint32_t now = millis();

    // ── Consume deferred MQTT commands (written by lwIP task) ─────────────────
    if (_bleTrackPending) {
        _bleTrackPending = false;
        strncpy(_bleTrackedMac, _blePendingMac, 17);
        _bleTrackedMac[17] = '\0';
        _bleTrackedSeenMs  = 0;   // reset stale guard for new target
        _bleNvsSave(_bleTrackedMac);
        Serial.printf("[BLE] now tracking %s\n", _bleTrackedMac);
    }

    if (_bleClearPending) {
        _bleClearPending       = false;
        _bleTrackedMac[0]      = '\0';
        _bleTrackedSeenMs      = 0;
        _bleLastTrackScanMs    = 0;
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
    bool trackingActive = (_bleTrackedMac[0] != '\0');
    if (trackingActive && !_bleScanInProgress
        && (now - _bleLastTrackScanMs >= BLE_MQTT_PUBLISH_MS)) {
        _bleLastTrackScanMs = now;
        _bleRunScan(BLE_TRACK_SCAN_DURATION_S);
    }

    // ── Handle scan completion ────────────────────────────────────────────────
    if (_bleScanDone) {
        _bleScanDone = false;
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
        if (_bleTrackedSeenMs > 0 && (now - _bleTrackedSeenMs) < 10000UL) {
            Serial.printf("[BLE] %s  RSSI=%d dBm  dist=%.1f m  name=%s\n",
                          _bleTrackedMac, _bleTrackedRssi,
                          _bleTrackedDistM, _bleTrackedName);
        } else {
            Serial.printf("[BLE] %s  — not seen in last 10 s\n", _bleTrackedMac);
        }
    }
}

#endif // BLE_ENABLED
