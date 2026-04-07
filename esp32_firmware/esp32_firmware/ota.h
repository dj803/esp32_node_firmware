#pragma once

#include <Arduino.h>
#include <ESP32OTAPull.h>
#include "esp_task_wdt.h"  // esp_task_wdt_delete — unsubscribe async_tcp before download
#include "config.h"
#include "app_config.h"   // gAppConfig.ota_json_url set via portal

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


// ── otaCheckNow ───────────────────────────────────────────────────────────────
// The main OTA entry point. Fetches the JSON filter file, compares the version,
// and flashes the new firmware if a newer version is available.
// Called periodically from otaLoop() and on demand from the MQTT handler.
void otaCheckNow() {
    Serial.println("[OTA] Checking for new firmware... (running: " FIRMWARE_VERSION ")");
    mqttPublishStatus("ota_checking",
                      "\"current_version\":\"" FIRMWARE_VERSION "\"");

    ESP32OTAPull ota;

    // Progress callback — logs each 10% increment and publishes nothing
    // (the ota_downloading MQTT event was already published before this runs).
    ota.SetCallback([](int offset, int total) {
        static int lastPct = -1;
        int pct = (total > 0) ? (offset * 100 / total) : 0;
        if (pct / 10 != lastPct / 10) {
            Serial.printf("[OTA] Progress: %d%%\n", pct);
            lastPct = pct;
        }
    });

    // ── Pass 1: check only — do not flash yet ────────────────────────────────
    // DONT_DO_UPDATE lets us learn the candidate version string via GetVersion()
    // so we can publish the ota_downloading MQTT event before the flash write
    // blocks the main task for ~30–60 seconds.
    int ret = ota.CheckForOTAUpdate(gAppConfig.ota_json_url,
                                    FIRMWARE_VERSION,
                                    ESP32OTAPull::DONT_DO_UPDATE);

    if (ret == ESP32OTAPull::NO_UPDATE_AVAILABLE) {
        Serial.printf("[OTA] Firmware is already up to date (running: " FIRMWARE_VERSION ", fetched: %s)\n",
                      ota.GetVersion().c_str());
        return;
    }

    if (ret == ESP32OTAPull::NO_UPDATE_PROFILE_FOUND) {
        Serial.println("[OTA] No matching profile in OTA JSON");
        return;
    }

    if (ret != ESP32OTAPull::UPDATE_AVAILABLE) {
        // Positive ret values are raw HTTP error codes; negative are library codes.
        Serial.printf("[OTA] JSON fetch failed (code %d)\n", ret);
        mqttPublishStatus("ota_failed",
            "\"error\":\"OTA JSON unreachable\","
            "\"current_version\":\"" FIRMWARE_VERSION "\"");
        return;
    }

    // ── Update available ─────────────────────────────────────────────────────
    String targetVersion = ota.GetVersion();
    Serial.printf("[OTA] Update available: %s → %s\n",
                  FIRMWARE_VERSION, targetVersion.c_str());

    String extra = "\"target_version\":\"" + targetVersion + "\"";
    mqttPublishStatus("ota_downloading", extra.c_str());
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
    _mqttClient.disconnect(true);
    delay(200);   // Let onMqttDisconnect fire and re-arm the reconnect timer
    if (_mqttReconnectTimer) xTimerStop(_mqttReconnectTimer, 0);
    TaskHandle_t asyncTcpTask = xTaskGetHandle("async_tcp");
    if (asyncTcpTask) esp_task_wdt_delete(asyncTcpTask);

    // ── Pass 2: download and flash, but do not reboot yet ────────────────────
    // UPDATE_BUT_NO_BOOT lets us publish ota_success before the connection drops.
    ret = ota.CheckForOTAUpdate(gAppConfig.ota_json_url,
                                FIRMWARE_VERSION,
                                ESP32OTAPull::UPDATE_BUT_NO_BOOT);

    if (ret == ESP32OTAPull::UPDATE_OK) {
        Serial.println("[OTA] Flash succeeded — restarting");
        mqttPublishStatus("ota_success",
            ("\"new_version\":\"" + targetVersion + "\"").c_str());
        delay(500);     // Give the MQTT publish time to be sent before disconnect
        ESP.restart();  // Boot into the new firmware
    } else {
        Serial.printf("[OTA] Flash failed (code %d)\n", ret);
        mqttPublishStatus("ota_failed",
            ("\"error\":\"flash failed code " + String(ret) +
             "\",\"current_version\":\"" FIRMWARE_VERSION "\"").c_str());
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
