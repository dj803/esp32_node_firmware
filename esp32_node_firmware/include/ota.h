#pragma once

#include <Arduino.h>
#include <ESP32OTAPull.h>
#include "esp_task_wdt.h"  // esp_task_wdt_delete — unsubscribe async_tcp before download
#include "config.h"
#include "logging.h"
#include "fwevent.h"
#include "semver.h"   // semverIsNewer() — extracted to allow host-side unit testing
#include "app_config.h"   // gAppConfig.ota_json_url set via portal
#include "led.h"
#ifdef BLE_ENABLED
#include <NimBLEDevice.h>   // NimBLEDevice::deinit() — free BLE heap before OTA flash write
#endif

// =============================================================================
// ota.h  —  Automatic OTA firmware updates via ESP32-OTA-Pull (spec §13)
//
// HOW IT WORKS:
//   1. Every OTA_CHECK_INTERVAL_MS (default 1 hour), or immediately when
//      triggered by an MQTT message on .../cmd/ota_check, the device calls
//      otaCheckNow().
//
//   2. otaCheckNow() fetches the JSON filter file at gAppConfig.ota_json_url
//      (a stable GitHub Pages URL) using the ESP32-OTA-Pull library.
//      The JSON contains the latest firmware version and its download URL:
//
//        {
//          "Configurations": [
//            {
//              "Version": "1.3.0",
//              "URL": "https://github.com/<owner>/<repo>/releases/download/v1.3.0/firmware.bin"
//            }
//          ]
//        }
//
//   3. If the JSON version is strictly newer than FIRMWARE_VERSION, the library
//      downloads firmware.bin and writes it directly to the inactive OTA flash
//      partition. The ESP32's bootloader then boots from the new partition.
//
//   4. NVS (credentials, device UUID, broker cache) is on a separate partition
//      and survives the OTA update unchanged.
//
// SECURITY NOTE:
//   ESP32-OTA-Pull uses HTTPClient without explicit CA certificate pinning.
//   HTTPS traffic is encrypted in transit but the server certificate is not
//   verified against a pinned root CA. For an internal IoT deployment this is
//   acceptable. See config.h for more detail.
//
// NOTE: mqttPublishStatus() is defined in mqtt_client.h, which is #included
// BEFORE this file in the main sketch, so no forward declaration is needed here.
// =============================================================================


// ── Module state ──────────────────────────────────────────────────────────────
static uint32_t _lastOtaCheck = 0;     // millis() timestamp of last check
static bool     _otaForced    = false; // True when MQTT has requested an immediate check



// Re-subscribe the async_tcp task to the task watchdog. Paired with the
// esp_task_wdt_delete() call we do before the blocking OTA download. On any
// non-OK exit path below we MUST re-subscribe or the device keeps running with
// a silently-downgraded watchdog until the next reboot.
static void _otaReaddAsyncTcpToWdt() {
    TaskHandle_t t = xTaskGetHandle("async_tcp");
    if (t) {
        esp_err_t err = esp_task_wdt_add(t);
        if (err != ESP_OK) {
            // ESP_ERR_INVALID_ARG means "already subscribed" — benign.
            if (err != ESP_ERR_INVALID_ARG) {
                LOG_W("OTA", "task_wdt_add(async_tcp) failed: %d", err);
            }
        }
    }
}


// ── otaCheckNow ───────────────────────────────────────────────────────────────
// The main OTA entry point. Fetches the JSON filter file, compares the version,
// and flashes the new firmware if a newer version is available.
// Called periodically from otaLoop() and on demand from the MQTT handler.
//
// `isSiblingRetry` is set to true for the single self-recursive call made after
// a GitHub fetch failure. It prevents infinite recursion if the sibling-
// provided URL also fails — we give up rather than asking the same siblings
// again. Default false keeps the public API unchanged for existing callers.
void otaCheckNow(bool isSiblingRetry) {
    // ── OTA URL validation ────────────────────────────────────────────────────
    // Reject obviously broken URLs before handing them to ESP32-OTA-Pull.
    // A blank URL or one that doesn't start with http(s):// will produce a
    // cryptic HTTP error code with no indication of root cause.
    const char* otaUrl = gAppConfig.ota_json_url;
    bool validScheme = (strncmp(otaUrl, "https://", 8) == 0 ||
                        strncmp(otaUrl, "http://",  7) == 0);
    if (!validScheme || strlen(otaUrl) < 10) {
        LOG_E("OTA", "Skipping check - invalid OTA JSON URL: '%s'", otaUrl);
        mqttPublishStatus(FwEvent::OTA_FAILED,
            "\"error\":\"invalid ota_json_url (missing scheme or empty)\","
            "\"current_version\":\"" FIRMWARE_VERSION "\"");
        return;
    }

    LOG_I("OTA", "Checking for new firmware... (running: " FIRMWARE_VERSION ")");
    mqttPublishStatus(FwEvent::OTA_CHECKING,
                      "\"current_version\":\"" FIRMWARE_VERSION "\"");

    ESP32OTAPull ota;

    // Progress callback — logs each 10% increment and resets the task watchdog
    // on every invocation so the loopTask's TWDT token stays alive throughout
    // the blocking download.  Without this, loop() is blocked for the full
    // download duration and the TWDT fires (~3 s on this SDK config).
    ota.SetCallback([](int offset, int total) {
        esp_task_wdt_reset();   // Keep loopTask WDT token alive every chunk
        static int lastPct = -1;
        int pct = (total > 0) ? (offset * 100 / total) : 0;
        if (pct / 10 != lastPct / 10) {
            LOG_I("OTA", "Progress: %d%%", pct);
            lastPct = pct;
        }
    });

    // ── Pass 1: check only — do not flash yet ────────────────────────────────
    // We pass "0.0.0" as the installed version so the library always fetches
    // the JSON and returns UPDATE_AVAILABLE (its lexicographic comparator breaks
    // on double-digit patch numbers, e.g. "0.2.7" > "0.2.15"). We then apply
    // our own numeric semver comparison via semverIsNewer().
    esp_task_wdt_reset();   // JSON fetch blocks loop() for ~0.8 s — pre-reset the loopTask WDT
    int ret = ota.CheckForOTAUpdate(gAppConfig.ota_json_url,
                                    "0.0.0",
                                    ESP32OTAPull::DONT_DO_UPDATE);

    if (ret == ESP32OTAPull::UPDATE_AVAILABLE) {
        // JSON fetched — now apply our own semver check
        String candidateVer = ota.GetVersion();
        if (!semverIsNewer(FIRMWARE_VERSION, candidateVer.c_str())) {
            responderSetHealthFlag(2, true);
            LOG_I("OTA", "Firmware up to date (running: " FIRMWARE_VERSION ", fetched: %s)",
                  candidateVer.c_str());
            return;
        }
        // Fall through — update is genuinely available
    }

    if (ret == ESP32OTAPull::NO_UPDATE_AVAILABLE) {
        // JSON fetched successfully — GitHub is reachable (no profile matched)
        responderSetHealthFlag(2, true);
        LOG_I("OTA", "Firmware up to date (running: " FIRMWARE_VERSION ", fetched: %s)",
              ota.GetVersion().c_str());
        return;
    }

    if (ret == ESP32OTAPull::NO_UPDATE_PROFILE_FOUND) {
        // JSON fetched successfully — GitHub is reachable (no matching profile is not a network error)
        responderSetHealthFlag(2, true);
        LOG_I("OTA", "No matching profile in OTA JSON");
        return;
    }

    if (ret != ESP32OTAPull::UPDATE_AVAILABLE) {
        // Positive ret values are raw HTTP error codes; negative are library codes.
        // JSON fetch failed — GitHub is not reachable from this node right now.
        responderSetHealthFlag(2, false);
        LOG_W("OTA", "JSON fetch failed (code %d)", ret);
        mqttPublishStatus(FwEvent::OTA_FAILED,
            "\"error\":\"OTA JSON unreachable\","
            "\"current_version\":\"" FIRMWARE_VERSION "\"");

        // Ask siblings for a working OTA URL and retry once with it.
        // espnowRequestOtaUrl() is defined in espnow_responder.h (included before
        // this file). `isSiblingRetry` prevents infinite recursion — the retry
        // call passes true so we won't attempt another round of sibling queries
        // if the sibling-provided URL also fails.
        if (!isSiblingRetry) {
            LOG_I("OTA", "Asking siblings for a working OTA URL...");
            if (espnowRequestOtaUrl()) {
                LOG_I("OTA", "Retrying with sibling-provided URL");
                otaCheckNow(/*isSiblingRetry=*/true);
            }
        }
        return;
    }

    // UPDATE_AVAILABLE — JSON was fetched successfully
    responderSetHealthFlag(2, true);

    // ── Update available ─────────────────────────────────────────────────────
    String targetVersion = ota.GetVersion();
    LOG_I("OTA", "Update available: %s -> %s", FIRMWARE_VERSION, targetVersion.c_str());

    String extra = "\"target_version\":\"" + targetVersion + "\"";
    mqttPublishStatus(FwEvent::OTA_DOWNLOADING, extra.c_str());
    delay(200);   // Give the MQTT publish time to be sent before the download blocks

    // Shut down MQTT and suppress the task watchdog on async_tcp before the
    // blocking download. Three things must happen in order:
    //
    //   1. disconnect(true) — fires onMqttDisconnect asynchronously, which
    //      re-arms the reconnect timer because it sees no active timer.
    //   2. delay(200) — gives the callback time to run and re-arm the timer.
    //   3. xTimerStop — kills the re-armed timer so no reconnect fires during
    //      the download (a mid-download connect attempt blocks async_tcp and
    //      triggers the task watchdog).
    //   4. esp_task_wdt_delete(async_tcp) — even with no reconnect attempt,
    //      async_tcp holds a watchdog subscription and cannot reset its token
    //      while the TCP stack is saturated by the download. Unsubscribing it
    //      prevents the watchdog from firing at ~40-50% progress.
    //      Safe because the device reboots immediately on success.
    // Block reconnect timer from racing with OTA teardown.  The TCP connection
    // may already have dropped (broker-initiated disconnect during the JSON
    // fetch), which means onMqttDisconnect already started the 1000 ms
    // reconnect timer.  Without this guard that timer fires from the timer
    // task and calls _mqttClient.connect() simultaneously with our disconnect,
    // corrupting AsyncMqttClient state and crashing _xt_context_restore.
    _mqttOtaActive = true;
    if (_mqttReconnectTimer) xTimerStop(_mqttReconnectTimer, 0);  // kill pre-existing timer

    _mqttClient.disconnect(true);
    delay(200);   // Let onMqttDisconnect fire (it will NOT re-arm the timer)
    if (_mqttReconnectTimer) xTimerStop(_mqttReconnectTimer, 0);  // defensive: kill if re-armed anyway
    TaskHandle_t asyncTcpTask = xTaskGetHandle("async_tcp");
    if (asyncTcpTask) esp_task_wdt_delete(asyncTcpTask);

    // Free NimBLE heap before the blocking download so Update.begin() can
    // allocate its 4 KB write buffer. mbed TLS keeps two 16 KB SSL record
    // buffers live during HTTPS; NimBLE's host allocations (~20-40 KB) are the
    // only large pool we can safely release without touching the active WiFi/TLS
    // stack. The device reboots on success (BLE inits fresh at next boot). On
    // failure we also restart (see below), so the deinit is always safe.
#ifdef BLE_ENABLED
    LOG_I("OTA", "Heap before BLE deinit: free=%u largest=%u",
          heap_caps_get_free_size(MALLOC_CAP_8BIT),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    NimBLEDevice::deinit(true);
    LOG_I("OTA", "Heap after BLE deinit:  free=%u largest=%u",
          heap_caps_get_free_size(MALLOC_CAP_8BIT),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif

    // In arduino-esp32 3.x, loopTask is not subscribed to the TWDT by default.
    // If we call esp_task_wdt_reset() in the progress callback without first
    // subscribing, ESP-IDF logs an E-level "task not found" message every chunk
    // (~hundreds of formatted prints deep inside the HTTPS call stack).  That
    // accumulated stack pressure causes an IllegalInstruction crash at ~60%.
    // Subscribe loopTask now; ESP_ERR_INVALID_ARG = already subscribed (benign).
    {
        esp_err_t wdtE = esp_task_wdt_add(NULL);
        if (wdtE != ESP_OK && wdtE != ESP_ERR_INVALID_ARG) {
            LOG_W("OTA", "esp_task_wdt_add(loopTask) failed: %d", wdtE);
        }
    }

    // ── Pass 2: download and flash, but do not reboot yet ────────────────────
    // UPDATE_BUT_NO_BOOT lets us publish ota_success before the connection drops.
    ledSetPattern(LedPattern::OTA_UPDATE);   // solid ON — flash write in progress
    { LedEvent e{}; e.type = LedEventType::OTA_START; ws2812PostEvent(e); }
    ret = ota.CheckForOTAUpdate(gAppConfig.ota_json_url,
                                "0.0.0",
                                ESP32OTAPull::UPDATE_BUT_NO_BOOT);

    if (ret == ESP32OTAPull::UPDATE_OK) {
        LOG_I("OTA", "Flash succeeded - restarting");
        mqttPublishStatus(FwEvent::OTA_SUCCESS,
            ("\"new_version\":\"" + targetVersion + "\"").c_str());
        delay(500);     // Give the MQTT publish time to be sent before disconnect
        ESP.restart();  // Boot into the new firmware — watchdog state doesn't matter
    } else {
        LOG_E("OTA", "Flash failed (code %d) — restarting to recover", ret);
        { LedEvent e{}; e.type = LedEventType::OTA_DONE; ws2812PostEvent(e); }
        // MQTT was deliberately disconnected before the download so
        // mqttPublishStatus would silently drop. BLE was also deinited.
        // The device is in a degraded state (MQTT + BLE both torn down) —
        // restart cleanly rather than re-adding async_tcp to the WDT, which
        // would resume processing buffered data through a torn-down
        // AsyncMqttClient and crash with an InstrFetchProhibited null-vtable panic.
        delay(200);   // Let the LED OTA_DONE event post to the WS2812 task
        ESP.restart();
    }
}


// ── otaLoop ───────────────────────────────────────────────────────────────────
// Periodic OTA check driver — must be called from loop().
// Fires otaCheckNow() when either:
//   a) OTA_CHECK_INTERVAL_MS has elapsed since the last check, OR
//   b) otaTrigger() was called (e.g. from the MQTT handler)
void otaLoop() {
    if (_otaForced || (millis() - _lastOtaCheck >= OTA_CHECK_INTERVAL_MS)) {
        _lastOtaCheck = millis();   // Reset the interval timer
        _otaForced    = false;      // Clear the force flag
        otaCheckNow();
    }
}


// ── otaTrigger ────────────────────────────────────────────────────────────────
// Schedules an immediate OTA check on the next loop() iteration.
// Called by the MQTT message handler when a message arrives on .../cmd/ota_check.
// Setting a flag (rather than calling otaCheckNow() directly) keeps the MQTT
// callback short and avoids blocking the AsyncMqttClient's internal task.
void otaTrigger() {
    _otaForced = true;   // otaLoop() will call otaCheckNow() on the next loop tick
}
