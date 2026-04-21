#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "nvs_utils.h"   // NvsPutIfChanged — compare-before-write wrappers

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
// Called by the "Locate This Device" button in both web portals (ap_portal.h)
// and can also be called directly from the main sketch or MQTT handlers.
static void ledFlashLocate() {
    pinMode(STATUS_LED_PIN, OUTPUT);
    for (int i = 0; i < 10; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(200);
        digitalWrite(STATUS_LED_PIN, LOW);
        delay(200);
    }
}

// Per-field buffer sizes — generous enough for real-world values, conservative
// enough to fit comfortably in NVS (max NVS string value is 4000 bytes)
#define APP_CFG_OTA_JSON_URL_LEN   256  // Full HTTPS URL of the OTA JSON filter file
#define APP_CFG_MQTT_SEG_LEN       48   // One ISA-95 topic segment: Enterprise/Site/etc.


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
};


// ── Global instance ───────────────────────────────────────────────────────────
// Declared here, populated at boot. All other files read this directly.
// Only AppConfigStore::save() may write to it after boot.
AppConfig gAppConfig;


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
        bool opened = prefs.begin(APP_CONFIG_NVS_NAMESPACE, true);

        // Lambda helper: read one NVS string key into `out`.
        // If the key is missing or empty, copies the compile-time `def` instead.
        // Always null-terminates within maxLen bytes.
        auto readStr = [&](const char* key, char* out, size_t maxLen, const char* def) {
            if (opened) {
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

        if (opened) prefs.end();

        // Log the active values so the serial monitor confirms what is in use
        Serial.printf("[AppConfig] OTA JSON URL: %s\n", gAppConfig.ota_json_url);
        Serial.printf("[AppConfig] Topic:  %s/%s/%s/%s/%s/%s/<uuid>/...\n",
                      gAppConfig.mqtt_enterprise, gAppConfig.mqtt_site,
                      gAppConfig.mqtt_area,       gAppConfig.mqtt_line,
                      gAppConfig.mqtt_cell,        gAppConfig.mqtt_device_type);
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
        ok &= NvsPutIfChanged(prefs, "mq_devtype",   cfg.mqtt_device_type) > 0;
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
        if (!prefs.begin(APP_CONFIG_NVS_NAMESPACE, true)) return false;
        String url = prefs.getString("ota_json_url", "");
        prefs.end();
        return url.length() > 0;   // Empty string means never saved via portal
    }
};
