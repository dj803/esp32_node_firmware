#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "config.h"
#include "led.h"         // LedPattern::LOCATE — used by ledFlashLocate()
#include "nvs_utils.h"   // NvsPutIfChanged — compare-before-write wrappers
#include "prefs_quiet.h" // (v0.4.03) prefsTryBegin — silent on missing namespace
#include "mac_utils.h"   // (v0.4.09) macStringToBytes for per-peer cal lookup
#include "peer_cal.h"    // (v0.4.09) PeerCalTable + lookup/upsert/forget

// =============================================================================
// app_config.h  —  Runtime-configurable application settings (NVS namespace
//                  "esp32cfg")
//
// PURPOSE:
//   Stores settings that are deployment-specific but NOT sensitive enough to
//   be credentials, and NOT device identity. Specifically:
//     - OTA JSON URL (stable GitHub Pages URL used by ota.h to check for updates)
//     - MQTT ISA-95 topic hierarchy segments (Enterprise, Site, Area, Line,
//       Cell, DeviceType) used by mqtt_client.h to build all topic paths
//
//   These values override the compile-time defaults in config.h without
//   requiring a reflash. They are set via:
//     a) The AP mode full setup form (GET / → POST /save) on first provisioning
//     b) The settings portal (GET /settings → POST /settings) when the device
//        is already connected to Wi-Fi, triggered by MQTT cmd/config_mode
//
// WHY SEPARATE FROM CREDENTIALS ("esp32cred"):
//   These settings are not secret. Keeping them in their own NVS namespace
//   means an admin can wipe or re-enter credentials without losing GitHub/MQTT
//   topology settings, and vice versa.
//
// WHY SEPARATE FROM DEVICE IDENTITY ("esp32id"):
//   The UUID must never be overwritten. Using a third namespace ensures no
//   AppConfigStore write can ever touch the device identity partition.
//
// FALLBACK:
//   Any field not yet saved in NVS falls back to its compile-time default from
//   config.h. This means a freshly flashed device works immediately with the
//   defaults baked in at compile time, and only fields that differ from those
//   defaults need to be entered in the portal.
//
// GLOBAL INSTANCE:
//   gAppConfig is populated by AppConfigStore::load() at boot and is then
//   read directly by ota.h (for ota_json_url) and mqtt_client.h
//   (for all mqtt_* fields). It is also updated in-place by AppConfigStore::save()
//   so changes made via the settings portal take effect immediately without restart.
// =============================================================================

// NVS namespace for this module — different from "esp32cred" and "esp32id"
#define APP_CONFIG_NVS_NAMESPACE  "esp32cfg"


// ── ledFlashLocate ────────────────────────────────────────────────────────────
// Flash STATUS_LED_PIN 10 times (200 ms on / 200 ms off) so the device can be
// physically located in a rack, cabinet, or on a shelf.
// Called by the "Locate This Device" button in the web portals (ap_portal.h)
// and from the MQTT `cmd/locate` handler (mqtt_client.h).
//
// Non-blocking: posts LedPattern::LOCATE to the 10 ms timer callback in led.h,
// which runs the 4-second flash sequence and auto-reverts to the previous
// pattern. Returning immediately is critical — callers include the MQTT task,
// where a blocking 4 s delay would stall heartbeats and backlog the broker.
static void ledFlashLocate() {
    ledSetPattern(LedPattern::LOCATE);
}

// Per-field buffer sizes — generous enough for real-world values, conservative
// enough to fit comfortably in NVS (max NVS string value is 4000 bytes)
#define APP_CFG_OTA_JSON_URL_LEN   256  // Full HTTPS URL of the OTA JSON filter file
#define APP_CFG_MQTT_SEG_LEN       48   // One ISA-95 topic segment: Enterprise/Site/etc.
#define APP_CFG_NODE_NAME_LEN      16   // Friendly node name ("Alpha", "Bravo", …)


// =============================================================================
// Per-peer calibration table (v0.4.09 / SUGGESTED_IMPROVEMENTS #41.7)
//
// PURPOSE:
//   Stores tx_power_dbm + path_loss_n per (this device, peer) pair, so the
//   six pairwise paths in a 3-node fleet can each have their own constants.
//
// SCHEMA + ALGORITHM live in peer_cal.h (host-testable, no Arduino deps).
// This file owns the single AppConfig instance and the NVS persistence;
// see PeerCalTable<N> + peerCalLookupT/UpsertT/ForgetT in peer_cal.h.
// =============================================================================
#define APP_CFG_PEER_CAL_MAX_PEERS  8


// ── AppConfig struct ──────────────────────────────────────────────────────────
// In-memory representation of all runtime-configurable settings.
// Loaded once at boot by AppConfigStore::load(), then treated as read-only
// until the settings portal saves a new copy via AppConfigStore::save().
struct AppConfig {
    // OTA JSON URL — stable URL of the JSON filter file on GitHub Pages.
    // Set via the AP portal (first setup) or settings portal (already connected).
    char ota_json_url[APP_CFG_OTA_JSON_URL_LEN]   = {0};   // e.g. "https://myorg.github.io/esp32-firmware/ota.json"

    // MQTT topic hierarchy (ISA-95 / Unified Namespace).
    // Full path: Enterprise/Site/Area/Line/Cell/DeviceType/<UUID>/prefix
    // These six segments map to the first six levels of the ISA-95 hierarchy.
    char mqtt_enterprise[APP_CFG_MQTT_SEG_LEN]    = {0};   // e.g. "Enigma"
    char mqtt_site[APP_CFG_MQTT_SEG_LEN]          = {0};   // e.g. "JHBDev"
    char mqtt_area[APP_CFG_MQTT_SEG_LEN]          = {0};   // e.g. "Office"
    char mqtt_line[APP_CFG_MQTT_SEG_LEN]          = {0};   // e.g. "Line"
    char mqtt_cell[APP_CFG_MQTT_SEG_LEN]          = {0};   // e.g. "Cell"
    char mqtt_device_type[APP_CFG_MQTT_SEG_LEN]   = {0};   // e.g. "ESP32NodeBox"

    // Friendly per-device name. Editable at runtime via MQTT cmd/espnow/name
    // (retained) and at provisioning time via the AP / settings portal.
    // Empty string means "no friendly name set" — Node-RED will fall back to
    // the device UUID for display.
    char node_name[APP_CFG_NODE_NAME_LEN]         = {0};   // e.g. "Alpha"

    // ESP-NOW ranging calibration + drift-filter parameters.
    // Editable at runtime via cmd/espnow/calibrate (commit) and cmd/espnow/filter.
    // Compile-time defaults are used until the operator calibrates.
    int8_t  espnow_tx_power_dbm    = ESPNOW_TX_POWER_DBM;               // RSSI @ 1 m (dBm)
    uint8_t espnow_path_loss_n_x10 = (uint8_t)(ESPNOW_PATH_LOSS_N * 10); // n × 10 (e.g. 25 = 2.5)
    uint8_t espnow_ema_alpha_x100  = ESPNOW_EMA_ALPHA_X100;              // α × 100 (e.g. 30 = 0.30)
    uint8_t espnow_outlier_db      = ESPNOW_OUTLIER_DB;                  // outlier gate (dB)

    // Anchor role + 3-D position (F5).
    // 0 = mobile node, 1 = fixed anchor. Coordinates in mm (integer) to avoid
    // floating-point NVS serialisation; divide by 1000 for metres.
    // Set via cmd/espnow/anchor {"role":"anchor","x_m":0.0,"y_m":0.0,"z_m":0.0}.
    uint8_t anchor_role   = 0;   // 0 = mobile, 1 = anchor
    int32_t anchor_x_mm   = 0;
    int32_t anchor_y_mm   = 0;
    int32_t anchor_z_mm   = 0;

    // (v0.4.09 / #41.7) Per-peer calibration table. Slot 0 = most-recently
    // calibrated; LRU eviction drops the tail when full.
    PeerCalTable<APP_CFG_PEER_CAL_MAX_PEERS> peer_cal_table;
};


// ── Global instance ───────────────────────────────────────────────────────────
// Declared here, populated at boot. All other files read this directly.
// Only AppConfigStore::save() may write to it after boot.
AppConfig gAppConfig;


// =============================================================================
// Per-peer calibration wrappers (v0.4.09 / #41.7)
//
// Thin wrappers that operate on gAppConfig.peer_cal_table. The actual
// algorithm lives in peer_cal.h (host-testable). Mutations require an
// explicit AppConfigStore::save(gAppConfig) afterwards to persist to NVS.
// =============================================================================

inline bool peerCalLookup(const char* macStr, int8_t* out_tx_power, uint8_t* out_n_x10) {
    return peerCalLookupT(gAppConfig.peer_cal_table, macStr, out_tx_power, out_n_x10);
}

inline bool peerCalUpsert(const char* macStr, int8_t tx_power_dbm, uint8_t n_x10) {
    return peerCalUpsertT(gAppConfig.peer_cal_table, macStr, tx_power_dbm, n_x10);
}

inline bool peerCalForget(const char* macStr) {
    return peerCalForgetT(gAppConfig.peer_cal_table, macStr);
}


// ── AppConfigStore ────────────────────────────────────────────────────────────
class AppConfigStore {
public:

    // ── load ──────────────────────────────────────────────────────────────────
    // Read all settings from NVS into gAppConfig.
    // For any key not yet present in NVS, the corresponding config.h compile-time
    // default is used instead. This means:
    //   - First boot with no portal configuration → uses config.h defaults
    //   - After portal save → uses the saved values
    //   - After OTA update → NVS values are preserved (NVS survives OTA)
    // Call once near the top of setup(), after DeviceId::init().
    static void load() {
        Preferences prefs;
        // true = read-only mode; safe to call even if NVS is uninitialised
        bool opened = prefsTryBegin(prefs, APP_CONFIG_NVS_NAMESPACE, true);   // (v0.4.03) silent if missing

        // Lambda helper: read one NVS string key into `out`.
        // If the key is missing or empty, copies the compile-time `def` instead.
        // Always null-terminates within maxLen bytes.
        //
        // (v0.3.37) prefs.isKey() pre-check suppresses the
        // "[E][Preferences.cpp:506] getString(): nvs_get_str len fail: ...
        //  NOT_FOUND" log spam every boot for keys not yet set. The error
        // is harmless (key just doesn't exist), but it logs at E (error)
        // level which drowns real errors during diagnostics.
        auto readStr = [&](const char* key, char* out, size_t maxLen, const char* def) {
            if (opened && prefs.isKey(key)) {
                String val = prefs.getString(key, "");
                if (val.length() > 0) {
                    strncpy(out, val.c_str(), maxLen - 1);
                    out[maxLen - 1] = '\0';
                    return;   // NVS value found — use it
                }
            }
            // Key not set yet — fall back to compile-time default from config.h
            strncpy(out, def, maxLen - 1);
            out[maxLen - 1] = '\0';
        };

        // NVS key names are short (≤15 chars) to stay within the NVS key length limit
        readStr("ota_json_url", gAppConfig.ota_json_url,     APP_CFG_OTA_JSON_URL_LEN, OTA_JSON_URL);
        readStr("mq_ent",       gAppConfig.mqtt_enterprise,  APP_CFG_MQTT_SEG_LEN,     MQTT_ENTERPRISE);
        readStr("mq_site",    gAppConfig.mqtt_site,        APP_CFG_MQTT_SEG_LEN,     MQTT_SITE);
        readStr("mq_area",    gAppConfig.mqtt_area,        APP_CFG_MQTT_SEG_LEN,     MQTT_AREA);
        readStr("mq_line",    gAppConfig.mqtt_line,        APP_CFG_MQTT_SEG_LEN,     MQTT_LINE);
        readStr("mq_cell",    gAppConfig.mqtt_cell,        APP_CFG_MQTT_SEG_LEN,     MQTT_CELL);
        readStr("mq_devtype", gAppConfig.mqtt_device_type, APP_CFG_MQTT_SEG_LEN,     MQTT_DEVICE_TYPE);
        readStr("node_name",  gAppConfig.node_name,        APP_CFG_NODE_NAME_LEN,    NODE_NAME);

        // Integer fields: read directly via Preferences (no readStr wrapper needed).
        // Compile-time defaults from config.h are used when the key doesn't exist yet.
        if (opened) {
            gAppConfig.espnow_tx_power_dbm    = prefs.getChar("en_txpow",   ESPNOW_TX_POWER_DBM);
            gAppConfig.espnow_path_loss_n_x10 = prefs.getUChar("en_pathN",  (uint8_t)(ESPNOW_PATH_LOSS_N * 10));
            gAppConfig.espnow_ema_alpha_x100  = prefs.getUChar("en_alpha",  ESPNOW_EMA_ALPHA_X100);
            gAppConfig.espnow_outlier_db      = prefs.getUChar("en_outlier", ESPNOW_OUTLIER_DB);
            gAppConfig.anchor_role            = prefs.getUChar("anc_role",  0);
            gAppConfig.anchor_x_mm            = prefs.getInt("anc_x_mm",   0);
            gAppConfig.anchor_y_mm            = prefs.getInt("anc_y_mm",   0);
            gAppConfig.anchor_z_mm            = prefs.getInt("anc_z_mm",   0);

            // (v0.4.09) Per-peer calibration table.
            // NVS schema: count (uint8) + entries blob (8 entries × 8 bytes).
            gAppConfig.peer_cal_table.count = prefs.getUChar("pc_count", 0);
            if (gAppConfig.peer_cal_table.count > APP_CFG_PEER_CAL_MAX_PEERS) {
                gAppConfig.peer_cal_table.count = 0;   // corrupted — treat as empty
            }
            if (gAppConfig.peer_cal_table.count > 0) {
                size_t blobLen = sizeof(gAppConfig.peer_cal_table.entries);
                size_t got = prefs.getBytes("pc_blob", gAppConfig.peer_cal_table.entries, blobLen);
                if (got != blobLen) {
                    memset(gAppConfig.peer_cal_table.entries, 0, blobLen);
                    gAppConfig.peer_cal_table.count = 0;
                }
            }
        }

        if (opened) prefs.end();

        // (#49) WARN if the loaded OTA URL is still the config.h placeholder.
        // The placeholder ("myorg.github.io/...") is never a valid deployed
        // URL — if a device boots with this it has either never been provisioned
        // OR its NVS was wiped without re-bootstrap. The OTA_FALLBACK_URLS list
        // saves us today, but changing the manifest URL would silently brick
        // OTA for any such device.
        if (strcmp(gAppConfig.ota_json_url, OTA_JSON_URL) == 0) {
            Serial.printf("[W][AppConfig] OTA URL is the config.h placeholder — "
                          "device may have been re-bootstrapped without OTA URL "
                          "in the credential bundle (see #49)\n");
        }

        // Log the active values so the serial monitor confirms what is in use
        Serial.printf("[AppConfig] OTA JSON URL: %s\n", gAppConfig.ota_json_url);
        Serial.printf("[AppConfig] Topic:  %s/%s/%s/%s/%s/%s/<uuid>/...\n",
                      gAppConfig.mqtt_enterprise, gAppConfig.mqtt_site,
                      gAppConfig.mqtt_area,       gAppConfig.mqtt_line,
                      gAppConfig.mqtt_cell,        gAppConfig.mqtt_device_type);
        Serial.printf("[AppConfig] Node name: %s\n",
                      gAppConfig.node_name[0] ? gAppConfig.node_name : "(unset)");
    }


    // ── save ──────────────────────────────────────────────────────────────────
    // Write all fields from `cfg` to NVS and update gAppConfig in-memory.
    // Returns true only if every NVS write succeeded.
    //
    // After a successful save, ota.h and mqtt_client.h will use the new values
    // immediately (they read gAppConfig directly). The MQTT topic hierarchy
    // change takes effect on the next subscribe/publish cycle. GitHub settings
    // take effect on the next OTA check.
    static bool save(const AppConfig& cfg) {
        Preferences prefs;
        // false = read-write mode
        if (!prefs.begin(APP_CONFIG_NVS_NAMESPACE, false)) return false;

        bool ok = true;
        // NvsPutIfChanged reads the existing NVS value and only writes when
        // it differs. The settings portal typically saves all 7 fields at
        // once even though only 1–2 may have changed, so per-field guarding
        // is a meaningful flash-wear saving in real-world use.
        ok &= NvsPutIfChanged(prefs, "ota_json_url", cfg.ota_json_url)     > 0;
        ok &= NvsPutIfChanged(prefs, "mq_ent",       cfg.mqtt_enterprise)  > 0;
        ok &= NvsPutIfChanged(prefs, "mq_site",      cfg.mqtt_site)        > 0;
        ok &= NvsPutIfChanged(prefs, "mq_area",      cfg.mqtt_area)        > 0;
        ok &= NvsPutIfChanged(prefs, "mq_line",      cfg.mqtt_line)        > 0;
        ok &= NvsPutIfChanged(prefs, "mq_cell",      cfg.mqtt_cell)        > 0;
        ok &= NvsPutIfChanged(prefs, "mq_devtype",   cfg.mqtt_device_type)        > 0;
        ok &= NvsPutIfChanged(prefs, "node_name",    cfg.node_name)               > 0;
        ok &= NvsPutIfChanged(prefs, "en_txpow",     cfg.espnow_tx_power_dbm)     > 0;
        ok &= NvsPutIfChanged(prefs, "en_pathN",     cfg.espnow_path_loss_n_x10)  > 0;
        ok &= NvsPutIfChanged(prefs, "en_alpha",     cfg.espnow_ema_alpha_x100)   > 0;
        ok &= NvsPutIfChanged(prefs, "en_outlier",   cfg.espnow_outlier_db)       > 0;
        ok &= NvsPutIfChanged(prefs, "anc_role",     cfg.anchor_role)             > 0;
        ok &= NvsPutIfChanged(prefs, "anc_x_mm",     cfg.anchor_x_mm)            >= 4;
        ok &= NvsPutIfChanged(prefs, "anc_y_mm",     cfg.anchor_y_mm)            >= 4;
        ok &= NvsPutIfChanged(prefs, "anc_z_mm",     cfg.anchor_z_mm)            >= 4;

        // (v0.4.09) Per-peer calibration table — write count + blob.
        ok &= NvsPutIfChanged(prefs, "pc_count",     cfg.peer_cal_table.count)    > 0;
        size_t pcWritten = prefs.putBytes("pc_blob", cfg.peer_cal_table.entries,
                                          sizeof(cfg.peer_cal_table.entries));
        ok &= (pcWritten == sizeof(cfg.peer_cal_table.entries));

        prefs.end();

        if (ok) {
            // Mirror the saved values into the live gAppConfig so callers see
            // the new settings immediately without needing to call load() again
            memcpy(&gAppConfig, &cfg, sizeof(AppConfig));
        }
        return ok;
    }


    // ── hasCustomOtaUrl ───────────────────────────────────────────────────────
    // Returns true if an OTA JSON URL has been explicitly saved to NVS,
    // meaning the admin has completed at least the OTA section of setup.
    // Used in the AP portal to warn if the firmware would fall back to the
    // compile-time placeholder URL in config.h.
    static bool hasCustomOtaUrl() {
        Preferences prefs;
        if (!prefsTryBegin(prefs, APP_CONFIG_NVS_NAMESPACE, true)) return false;   // (v0.4.03) silent
        String url = prefs.getString("ota_json_url", "");
        prefs.end();
        return url.length() > 0;   // Empty string means never saved via portal
    }
};
