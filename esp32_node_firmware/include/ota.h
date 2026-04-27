#pragma once

#include <Arduino.h>
#include <ESP32OTAPull.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_https_ota.h>     // (v0.3.35) Phase 3 — replace ESP32-OTA-Pull's writer
#include <esp_http_client.h>
#include <esp_crt_bundle.h>    // built-in CA bundle for esp-tls
#include "esp_task_wdt.h"  // esp_task_wdt_delete — unsubscribe async_tcp before download
#include <esp_now.h>       // esp_now_deinit() before flash write — see v0.4.08 fix below
#include "config.h"
#include "logging.h"
#include "fwevent.h"
#include "semver.h"   // semverIsNewer() — extracted to allow host-side unit testing
#include "app_config.h"   // gAppConfig.ota_json_url set via portal
#include "led.h"

// (v0.3.34) Forward-declared from ota_validation.h. Cannot #include directly
// because the validation module needs to be ordered after mqtt_client.h
// (uses mqttPublishStatus) but before ota.h (provides otaValidationArmRollback
// for the ESP.restart path below). Decoupling via forward decl avoids the
// circular include.
void otaValidationArmRollback(const char* targetVersion);
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
static uint32_t     _lastOtaCheck         = 0;     // millis() timestamp of last check
static bool         _otaForced            = false; // True when MQTT has requested an immediate check
static TimerHandle_t _otaProgressWatchdog = NULL;  // One-shot stall detector during download
static TimerHandle_t _otaWdtFeederTimer   = NULL;  // (v0.4.09) periodic task_wdt feed during download

// One-shot FreeRTOS timer fired if no progress callback arrives within
// OTA_PROGRESS_TIMEOUT_MS. Recovers from CDN stalls / TCP black-holes that
// don't trip the TWDT (e.g. v0.3.28 Charlie freeze).
static void _otaProgressTimeout(TimerHandle_t /*xTimer*/) {
    LOG_E("OTA", "Progress watchdog fired (no callback for %d ms) - restarting",
          OTA_PROGRESS_TIMEOUT_MS);
    ESP.restart();
}

// (v0.4.09) Periodic task_wdt feeder.
//
// The pre-existing per-chunk progress callback resets the loopTask WDT on
// every HTTP chunk. On marginal AP signal (triangle position observed during
// v0.4.07 → v0.4.08 OTA on Charlie), a chunk can take >5 s to arrive,
// during which no reset fires and the default 5 s task_wdt triggers a
// reboot mid-download. This timer fires every 1 s from the FreeRTOS timer
// task and resets the loopTask token — the WDT can never starve regardless
// of chunk pacing. Started just before esp_https_ota_perform's loop,
// stopped after the loop exits (success or failure).
static void _otaWdtFeederCb(TimerHandle_t /*xTimer*/) {
    esp_task_wdt_reset();
}



// ── otaExtractBinaryUrl ───────────────────────────────────────────────────────
// (v0.3.35 / Phase 3) Re-fetches the manifest at `manifestUrl` and pulls
// out the .bin URL of the first profile that matches our board (or has no
// Board filter set). Mirrors ESP32-OTA-Pull's selection logic so existing
// manifests work unchanged.
//
// Why a second fetch? ESP32-OTA-Pull does NOT expose the URL it found
// during Pass 1 — it only stores Version. For Pass 2 we now use
// esp_https_ota (resume-capable, actively maintained) which needs the
// .bin URL directly. The duplicate fetch is ~200 bytes of JSON over the
// already-warm TLS connection — cheap.
//
// Returns: empty String on failure; .bin URL on success. Caller must
// validate that the returned String is non-empty before passing it to
// esp_https_ota_begin().
static String otaExtractBinaryUrl(const char* manifestUrl) {
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(manifestUrl)) {
        LOG_W("OTA", "otaExtractBinaryUrl: http.begin failed");
        return String();
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        LOG_W("OTA", "otaExtractBinaryUrl: GET %s -> %d", manifestUrl, code);
        http.end();
        return String();
    }
    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        LOG_W("OTA", "otaExtractBinaryUrl: JSON parse error %s", err.c_str());
        return String();
    }

    JsonArray configs = doc["Configurations"].as<JsonArray>();
    for (JsonObject cfg : configs) {
        const char* board = cfg["Board"]   | "";
        const char* url   = cfg["URL"]     | "";
        // Match if Board is empty/missing or equals our board.
        if (strlen(board) > 0 && strcmp(board, ARDUINO_BOARD) != 0) continue;
        if (strlen(url) == 0) continue;
        return String(url);
    }
    return String();
}


// ── otaPerformWithEspIdf ──────────────────────────────────────────────────────
// (v0.3.35 / Phase 3) The replacement for ESP32-OTA-Pull's Pass 2.
// Uses Espressif's esp_https_ota with partial_http_download=true so a stalled
// download can resume (where the server supports HTTP Range requests).
// Returns 0 on success, negative on any failure.
//
// Caller is responsible for:
//   - MQTT teardown + async_tcp WDT unsubscribe (handled in otaCheckNow)
//   - BLE deinit (handled in otaCheckNow)
//   - Pre-flight heap gate (handled in otaCheckNow)
//   - LoopTask TWDT subscribe (handled in otaCheckNow)
//   - Progress watchdog arm (handled in otaCheckNow)
//
// This function only owns the perform-loop and progress logging.
static int otaPerformWithEspIdf(const char* binaryUrl) {
    LOG_I("OTA", "esp_https_ota: starting download from %s", binaryUrl);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url                  = binaryUrl;
    http_cfg.crt_bundle_attach    = esp_crt_bundle_attach;   // use framework's CA bundle
    http_cfg.timeout_ms           = 30000;
    http_cfg.keep_alive_enable    = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config           = &http_cfg;
    ota_cfg.partial_http_download = true;   // enable HTTP Range support
    ota_cfg.max_http_request_size = 16 * 1024;  // chunk size cap

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        LOG_E("OTA", "esp_https_ota_begin failed: %d (%s)", err, esp_err_to_name(err));
        return -1;
    }

    int totalSize = esp_https_ota_get_image_size(handle);
    LOG_I("OTA", "esp_https_ota: image size = %d bytes", totalSize);

    int lastPct = -1;
    while (true) {
        // Reset watchdogs on every iteration. esp_https_ota_perform processes
        // one HTTP chunk per call so this is the analogue of ESP32-OTA-Pull's
        // SetCallback chunk-by-chunk hook.
        esp_task_wdt_reset();
        if (_otaProgressWatchdog) xTimerReset(_otaProgressWatchdog, 0);

        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;

        int writtenBytes = esp_https_ota_get_image_len_read(handle);
        int pct = (totalSize > 0) ? (writtenBytes * 100 / totalSize) : 0;
        if (pct / 10 != lastPct / 10) {
            LOG_I("OTA", "Progress: %d%%", pct);
            lastPct = pct;
        }
    }

    if (err != ESP_OK) {
        LOG_E("OTA", "esp_https_ota_perform failed: %d (%s)", err, esp_err_to_name(err));
        esp_https_ota_abort(handle);
        return -2;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        LOG_E("OTA", "esp_https_ota: incomplete image received");
        esp_https_ota_abort(handle);
        return -3;
    }

    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        LOG_E("OTA", "esp_https_ota_finish failed: %d (%s)", err, esp_err_to_name(err));
        return -4;
    }

    LOG_I("OTA", "esp_https_ota: download + verify complete");
    return 0;
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
#ifdef OTA_DISABLE
    // Variant builds (e.g. esp32dev_canary, #54 stack-overflow soak) can set
    // -DOTA_DISABLE in build_flags so the device cannot self-OTA away from
    // the variant binary. Without this gate, a -dev/.0 version like 0.4.20.0
    // is treated as older than the matching release per #80's 4-component
    // semver and gets pulled back to release on first OTA check, killing
    // any in-progress soak.
    LOG_I("OTA", "OTA disabled at compile time (OTA_DISABLE) — skipping check");
    return;
#endif
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
            "\"stage\":\"preflight\","
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
        if (_otaProgressWatchdog) xTimerReset(_otaProgressWatchdog, 0);
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
    //
    // URL fallback chain (v0.3.33): primary (NVS-stored gAppConfig.ota_json_url)
    // is tried first; on any HTTP/library failure we walk OTA_FALLBACK_URLS in
    // config.h. If the entire chain returns network-level errors we then ask
    // siblings (sibling-URL fallback unchanged from v0.3.32). This means a dead
    // CDN / Pages outage / bad portal entry no longer strands the device on
    // the first failure.
    int ret = -1;
    const char* successfulUrl = nullptr;
    {
        // Build the candidate list: primary URL first, then any fallbacks
        // that don't equal the primary (de-dup so a portal that points at
        // the GitHub Pages URL doesn't try the same URL twice in a row).
        const char* candidates[1 + OTA_FALLBACK_URL_COUNT];
        uint8_t candCount = 0;
        candidates[candCount++] = gAppConfig.ota_json_url;
        for (uint8_t i = 0; i < OTA_FALLBACK_URL_COUNT; i++) {
            if (strcmp(OTA_FALLBACK_URLS[i], gAppConfig.ota_json_url) != 0) {
                candidates[candCount++] = OTA_FALLBACK_URLS[i];
            }
        }

        for (uint8_t i = 0; i < candCount; i++) {
            esp_task_wdt_reset();   // JSON fetch blocks loop() for ~0.8 s — pre-reset the loopTask WDT
            LOG_I("OTA", "Manifest fetch %u/%u: %s", i + 1, candCount, candidates[i]);
            ret = ota.CheckForOTAUpdate(candidates[i], "0.0.0", ESP32OTAPull::DONT_DO_UPDATE);
            if (ret == ESP32OTAPull::UPDATE_AVAILABLE ||
                ret == ESP32OTAPull::NO_UPDATE_AVAILABLE ||
                ret == ESP32OTAPull::NO_UPDATE_PROFILE_FOUND) {
                successfulUrl = candidates[i];
                if (i > 0) {
                    LOG_I("OTA", "Manifest fetched from fallback URL #%u", i);
                }
                break;
            }
            LOG_W("OTA", "Manifest fetch %u/%u failed (code %d)", i + 1, candCount, ret);
        }
    }

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
        // JSON fetch failed on EVERY URL in the candidate chain — GitHub /
        // configured CDN are not reachable from this node right now.
        responderSetHealthFlag(2, false);
        LOG_W("OTA", "All %d manifest URLs unreachable (last code %d)",
              1 + OTA_FALLBACK_URL_COUNT, ret);
        char extra[160];
        snprintf(extra, sizeof(extra),
            "\"stage\":\"manifest\","
            "\"error\":\"all manifest URLs unreachable\","
            "\"last_code\":%d,"
            "\"current_version\":\"" FIRMWARE_VERSION "\"",
            ret);
        mqttPublishStatus(FwEvent::OTA_FAILED, extra);

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

    // Snapshot heap state at trigger time (before any teardown). If a future
    // OTA freezes mid-download, correlating this with serial logs lets us
    // distinguish heap-fragmentation stalls from network stalls.
    LOG_I("OTA", "Heap at trigger: free=%u largest=%u",
          heap_caps_get_free_size(MALLOC_CAP_8BIT),
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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

    // (v0.4.08) Stop ESP-NOW BEFORE the blocking flash write.
    //
    // ESP-NOW receive callbacks are NOT marked IRAM_ATTR. esp_partition_write
    // disables instruction caches while it programs flash. If a frame arrives
    // during that cache-disabled window, the callback dispatch path tries to
    // execute flash-resident code (the registered recv callback or anything it
    // pulls in) and the CPU faults with EXCCAUSE 0 (IllegalInstruction in
    // _xt_coproc_restorecs / spi_flash_enable_interrupts_caches_and_other_cpu).
    //
    // Empirical case: every v0.4.07 OTA attempt on Bravo/Alpha repeatably
    // panicked at 60–70 % flash progress with this exact stack. The triangle
    // ranging fleet broadcasts ESP-NOW beacons every ~3 s, so a frame is in
    // flight on every chunk write — the panic is statistical, not random.
    //
    // The device reboots on every code path below (success → ESP.restart, all
    // failure paths → ESP.restart), so we never need to bring ESP-NOW back up
    // here. setup() re-inits it on the next boot.
    esp_now_unregister_recv_cb();
    esp_err_t enErr = esp_now_deinit();
    if (enErr == ESP_OK) {
        LOG_I("OTA", "ESP-NOW de-initialised before flash write");
    } else {
        // Non-fatal — proceed; the deinit failing usually means it wasn't
        // initialised yet (e.g. AP-mode boot that never reached operational).
        LOG_I("OTA", "ESP-NOW deinit returned %d (likely never initialised)", enErr);
    }

    // ── Pre-flight heap gate (v0.3.33) ───────────────────────────────────────
    // After all teardown / deinit has completed but BEFORE we kick off the
    // blocking download, verify we have enough headroom for mbedTLS record
    // buffers (~16 KB IN + 16 KB OUT) PLUS Update.begin's 4 KB write buffer.
    // If we don't, abort cleanly. The next OTA_CHECK_INTERVAL_MS retry will
    // try again from a freshly-rebooted heap state.
    {
        size_t freeNow    = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (freeNow < OTA_PREFLIGHT_HEAP_FREE_MIN ||
            largestNow < OTA_PREFLIGHT_HEAP_BLOCK_MIN) {
            LOG_E("OTA", "Pre-flight heap gate FAILED: free=%u (min %u) largest=%u (min %u)",
                  (unsigned)freeNow, (unsigned)OTA_PREFLIGHT_HEAP_FREE_MIN,
                  (unsigned)largestNow, (unsigned)OTA_PREFLIGHT_HEAP_BLOCK_MIN);
            // Re-init what we tore down so the device stays useful until next retry.
            // BLE will re-init on next boot anyway when ESP.restart() runs;
            // we restart here too to guarantee a known-good state.
            char extra[192];
            snprintf(extra, sizeof(extra),
                "\"stage\":\"preflight\","
                "\"error\":\"heap_low\","
                "\"free\":%u,\"largest\":%u,"
                "\"free_min\":%u,\"largest_min\":%u,"
                "\"current_version\":\"" FIRMWARE_VERSION "\"",
                (unsigned)freeNow, (unsigned)largestNow,
                (unsigned)OTA_PREFLIGHT_HEAP_FREE_MIN,
                (unsigned)OTA_PREFLIGHT_HEAP_BLOCK_MIN);
            mqttPublishStatus(FwEvent::OTA_PREFLIGHT, extra);
            delay(200);     // let the publish drain
            ESP.restart();  // fresh heap on next boot — natural retry
        }
    }

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

    // Arm the per-chunk progress watchdog: created lazily so multiple OTA
    // attempts in one boot reuse a single timer object. Each progress callback
    // resets it; if no callback fires for OTA_PROGRESS_TIMEOUT_MS, the timer
    // task calls ESP.restart() to break the stall.
    if (!_otaProgressWatchdog) {
        _otaProgressWatchdog = xTimerCreate("ota_wd",
                                            pdMS_TO_TICKS(OTA_PROGRESS_TIMEOUT_MS),
                                            pdFALSE,   // one-shot
                                            NULL,
                                            _otaProgressTimeout);
    }
    if (_otaProgressWatchdog) xTimerStart(_otaProgressWatchdog, 0);

    // (v0.4.09) Independent task_wdt feeder. 1 s period, auto-reload, runs
    // throughout the download. Stopped on exit below.
    if (!_otaWdtFeederTimer) {
        _otaWdtFeederTimer = xTimerCreate("ota_wdtf",
                                          pdMS_TO_TICKS(1000),
                                          pdTRUE,   // auto-reload
                                          NULL,
                                          _otaWdtFeederCb);
    }
    if (_otaWdtFeederTimer) xTimerStart(_otaWdtFeederTimer, 0);

    // (v0.3.35 / Phase 3) Pass 2 now uses esp_https_ota with HTTP resume
    // support, replacing ESP32-OTA-Pull's blocking writer. Steps:
    //   1. Re-fetch the manifest to extract the .bin URL (ESP32-OTA-Pull
    //      does not expose this after Pass 1).
    //   2. Hand the .bin URL to esp_https_ota with partial_http_download=true.
    //   3. esp_https_ota_perform() loop with progress + watchdog hooks.
    const char* manifestUrlForBinary = (successfulUrl != nullptr)
                                       ? successfulUrl : gAppConfig.ota_json_url;
    String binaryUrl = otaExtractBinaryUrl(manifestUrlForBinary);
    if (binaryUrl.length() == 0) {
        LOG_E("OTA", "Could not extract binary URL from manifest %s", manifestUrlForBinary);
        if (_otaProgressWatchdog) xTimerStop(_otaProgressWatchdog, 0);
        char extra[192];
        snprintf(extra, sizeof(extra),
            "\"stage\":\"manifest_url\","
            "\"error\":\"binary URL extraction failed\","
            "\"current_version\":\"" FIRMWARE_VERSION "\"");
        mqttPublishStatus(FwEvent::OTA_FAILED, extra);
        delay(200);
        ESP.restart();
    }

    int otaResult = otaPerformWithEspIdf(binaryUrl.c_str());

    // Download returned (success or failure) — disarm the stall watchdog
    // and the periodic WDT feeder.
    if (_otaProgressWatchdog) xTimerStop(_otaProgressWatchdog, 0);
    if (_otaWdtFeederTimer)   xTimerStop(_otaWdtFeederTimer,   0);

    if (otaResult == 0) {
        LOG_I("OTA", "Flash succeeded - restarting");
        mqttPublishStatus(FwEvent::OTA_SUCCESS,
            ("\"new_version\":\"" + targetVersion + "\"").c_str());
        // (v0.3.34) Arm the post-OTA validation gate. The NEW firmware reads
        // this NVS flag in otaValidationCheckBoot() — if it sees its own
        // FIRMWARE_VERSION as the pending version, it sets the validation
        // deadline and rolls back if MQTT doesn't come back up in time.
        otaValidationArmRollback(targetVersion.c_str());
        delay(500);     // Give the MQTT publish time to be sent before disconnect
        ESP.restart();  // Boot into the new firmware — watchdog state doesn't matter
    } else {
        LOG_E("OTA", "Flash failed (esp_https_ota result %d) — restarting to recover", otaResult);
        { LedEvent e{}; e.type = LedEventType::OTA_DONE; ws2812PostEvent(e); }
        char extra[160];
        snprintf(extra, sizeof(extra),
            "\"stage\":\"flash\","
            "\"error\":\"esp_https_ota failed\","
            "\"code\":%d,"
            "\"current_version\":\"" FIRMWARE_VERSION "\"",
            otaResult);
        // MQTT was deliberately disconnected before the download — publish
        // is best-effort (will be retained on the device-level LWT path).
        // BLE was also deinited. The device is in a degraded state —
        // restart cleanly rather than re-adding async_tcp to the WDT, which
        // would resume processing buffered data through a torn-down
        // AsyncMqttClient and crash with an InstrFetchProhibited null-vtable panic.
        mqttPublishStatus(FwEvent::OTA_FAILED, extra);
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
