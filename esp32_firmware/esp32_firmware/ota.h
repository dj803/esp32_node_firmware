#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>  // HTTPS client with certificate validation
#include <HTTPClient.h>        // HTTP GET requests (used for GitHub API)
#include <HTTPUpdate.h>        // Streams a .bin file directly to flash (OTA)
// NOTE: ArduinoJson has been removed. The GitHub Releases API response is
// parsed with a lightweight custom extractor (see jsonExtractField below).
// This saves ~25-35 KB of flash compared to the full ArduinoJson library.
#include "config.h"
#include "app_config.h"   // gAppConfig.github_owner / github_repo set via portal

// =============================================================================
// ota.h  —  Automatic OTA firmware updates from GitHub Releases (spec §13)
//
// HOW IT WORKS:
//   1. Every OTA_CHECK_INTERVAL_MS (default 1 hour), or immediately when
//      triggered by an MQTT message on .../cmd/ota_check, the device calls
//      otaCheckNow().
//
//   2. otaCheckNow() fetches the GitHub Releases "latest release" API:
//        GET https://api.github.com/repos/{owner}/{repo}/releases/latest
//      This returns a JSON object containing the release tag name (e.g. "v1.3.0")
//      and a list of file assets attached to the release.
//
//   3. The tag is compared to FIRMWARE_VERSION using semantic versioning
//      (major.minor.patch). If the release is strictly newer, the device
//      downloads the firmware.bin asset from that release.
//
//   4. HTTPUpdate streams the .bin file directly to the inactive OTA flash
//      partition — the entire binary never has to fit in RAM at once.
//      The ESP32's bootloader then switches to the new partition on restart.
//
//   5. NVS (credentials, restart counter) is on a separate partition and
//      survives the OTA update unchanged.
//
// SECURITY:
//   All HTTPS connections use the ISRG Root X1 certificate (defined in config.h)
//   to verify github.com and objects.githubusercontent.com. This prevents a
//   man-in-the-middle attack from substituting a malicious firmware binary.
//   Firmware signature verification is a planned v2 feature.
//
// NOTE: mqttPublishStatus() is defined in mqtt_client.h, which is #included
// BEFORE this file in the main sketch, so no forward declaration is needed here.
// =============================================================================


// ── Module state ──────────────────────────────────────────────────────────────
static uint32_t _lastOtaCheck = 0;     // millis() timestamp of last check
static bool     _otaForced    = false; // True when MQTT has requested an immediate check


// ── jsonExtractField ──────────────────────────────────────────────────────────
// Lightweight JSON string-value extractor. Replaces ArduinoJson entirely.
//
// Searches `json` for the pattern:  "key":"value"
// and copies the value into `outBuf` (null-terminated, max outBufSize-1 chars).
//
// Handles the two GitHub API fields we need:
//   "tag_name":"v1.3.0"
//   "browser_download_url":"https://..."
//
// Limitations (intentional — we only need simple string values):
//   - Only extracts string values (quoted). Does not handle numbers, arrays,
//     nested objects, or escaped quotes inside values.
//   - Finds the FIRST occurrence of the key — sufficient because tag_name
//     appears once and browser_download_url is read after confirming the
//     asset name matches "firmware.bin".
//   - Does not validate JSON structure — relies on GitHub's well-formed output.
//
// Returns true if the key was found and value fits in outBuf, false otherwise.
static bool jsonExtractField(const char* json, const char* key,
                              char* outBuf, size_t outBufSize) {
    // Build search pattern:  "key":"
    // Maximum key length we'll encounter is "browser_download_url" = 20 chars
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    // Find the pattern in the JSON string
    const char* found = strstr(json, pattern);
    if (!found) return false;

    // Move pointer past the pattern to the start of the value
    const char* valueStart = found + strlen(pattern);

    // Scan forward to find the closing quote, honouring \" escapes
    const char* p = valueStart;
    size_t len = 0;
    while (*p && *p != '"' && len < outBufSize - 1) {
        if (*p == '\\' && *(p + 1) == '"') {
            // Escaped quote inside value — include the literal quote char
            if (len < outBufSize - 1) outBuf[len++] = '"';
            p += 2;   // Skip both the backslash and the quote
        } else {
            outBuf[len++] = *p++;
        }
    }

    outBuf[len] = '\0';
    return len > 0;   // Return false if value was empty
}


// ── fetchLatestRelease ────────────────────────────────────────────────────────
// Calls the GitHub Releases API and extracts the release tag and the download
// URL of the firmware.bin asset.
//
// Strategy: read the entire HTTP response body into a single heap buffer, then
// run two lightweight string searches on it. The GitHub "latest release" response
// is typically 3–6 KB — well within the ESP32's heap capacity.
//
// Returns false if:
//   - The HTTPS request fails (no internet, wrong cert, rate-limited)
//   - The response body cannot be read into the buffer
//   - tag_name is not found in the response
//   - No asset named "firmware.bin" is attached to the latest release
static bool fetchLatestRelease(String& tagName, String& downloadUrl) {
    WiFiClientSecure client;
    client.setCACert(GITHUB_ROOT_CA);   // Verify github.com against the bundled root cert

    HTTPClient http;
    // Build the GitHub Releases API URL from gAppConfig (NVS-backed, set via portal).
    // Using gAppConfig rather than the config.h constants means the GitHub owner and
    // repo can be changed at runtime without reflashing the firmware.
    String url = String("https://api.github.com/repos/")
               + gAppConfig.github_owner + "/" + gAppConfig.github_repo + "/releases/latest";

    http.begin(client, url);
    http.addHeader("User-Agent", "ESP32-OTA/" FIRMWARE_VERSION); // GitHub requires a User-Agent
    http.addHeader("Accept", "application/vnd.github+json");     // Request GitHub's JSON format

    int code = http.GET();
    if (code != 200) {
        // Common non-200 codes: 404 (no releases yet), 403 (rate limited), -1 (network error)
        Serial.printf("[OTA] GitHub API returned HTTP %d\n", code);
        http.end();
        return false;
    }

    // Read the full response body into a heap buffer.
    // GitHub "latest release" responses are 3-6 KB. We cap at 8 KB to be safe —
    // if the response is larger, something unusual has happened.
    int contentLength = http.getSize();   // -1 if unknown (chunked transfer)
    size_t bufSize = (contentLength > 0 && contentLength < 8192)
                     ? (size_t)contentLength + 1
                     : 8192;

    char* body = (char*)malloc(bufSize);
    if (!body) {
        Serial.println("[OTA] malloc failed for response buffer");
        http.end();
        return false;
    }

    // Read bytes from the HTTP stream into the buffer.
    // getStream().readBytes() returns the number of bytes actually read.
    WiFiClient* stream = http.getStreamPtr();
    size_t bytesRead = 0;
    uint32_t timeout = millis() + 5000;   // 5-second read timeout
    while (bytesRead < bufSize - 1 && millis() < timeout) {
        if (stream->available()) {
            int c = stream->read();
            if (c == -1) break;            // End of stream
            body[bytesRead++] = (char)c;
        } else {
            delay(1);   // Yield briefly while waiting for more data
        }
    }
    body[bytesRead] = '\0';   // Null-terminate for strstr() use
    http.end();

    if (bytesRead == 0) {
        Serial.println("[OTA] Empty response body");
        free(body);
        return false;
    }

    // ── Extract tag_name ──────────────────────────────────────────────────────
    // Example JSON fragment we're searching for:
    //   "tag_name":"v1.3.0"
    char tagBuf[32];   // A semver tag will never exceed 32 chars
    if (!jsonExtractField(body, "tag_name", tagBuf, sizeof(tagBuf))) {
        Serial.println("[OTA] tag_name not found in response");
        free(body);
        return false;
    }
    tagName = String(tagBuf);

    // ── Find firmware.bin and extract its download URL ────────────────────────
    // The assets array looks like:
    //   "assets":[{"name":"firmware.bin","browser_download_url":"https://..."},...]
    //
    // Strategy: find the string "firmware.bin" in the body, then search forward
    // from that position for "browser_download_url" to get the URL that belongs
    // to that specific asset — not a URL from a different asset.
    const char* assetNamePos = strstr(body, "\"firmware.bin\"");
    if (!assetNamePos) {
        Serial.println("[OTA] No firmware.bin asset in release");
        free(body);
        return false;
    }

    // Search for browser_download_url starting from the "firmware.bin" position.
    // Limit the forward search to 512 bytes — the URL field appears within a few
    // hundred bytes of the asset name in GitHub's JSON layout.
    size_t searchLen = 512;
    size_t remaining = strlen(assetNamePos);
    if (remaining < searchLen) searchLen = remaining;

    // Temporarily null-terminate a window of the body for the field search
    char saved = assetNamePos[searchLen];
    ((char*)assetNamePos)[searchLen] = '\0';

    char urlBuf[256];   // GitHub download URLs are typically ~100-150 chars
    bool found = jsonExtractField(assetNamePos, "browser_download_url",
                                  urlBuf, sizeof(urlBuf));
    ((char*)assetNamePos)[searchLen] = saved;   // Restore the original character

    free(body);   // Release the response buffer — we have what we need

    if (!found) {
        Serial.println("[OTA] browser_download_url not found near firmware.bin");
        return false;
    }

    downloadUrl = String(urlBuf);
    return true;
}


// ── semverNewer ───────────────────────────────────────────────────────────────
// Returns true if `candidate` version is strictly greater than `current`.
// Accepts both "v1.2.3" (GitHub tag format) and "1.2.3" (config.h format).
// Comparison is numeric (e.g. "1.9.0" < "1.10.0"), not lexicographic.
static bool semverNewer(const char* current, const char* candidate) {
    // Strip a leading 'v' or 'V' from either string before parsing
    auto strip = [](const char* s) -> const char* {
        return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
    };

    // Parse "major.minor.patch" into three integers
    int cMaj=0, cMin=0, cPat=0;   // Current version components
    int nMaj=0, nMin=0, nPat=0;   // Candidate version components
    sscanf(strip(current),   "%d.%d.%d", &cMaj, &cMin, &cPat);
    sscanf(strip(candidate), "%d.%d.%d", &nMaj, &nMin, &nPat);

    // Compare major first, then minor, then patch — first difference wins
    if (nMaj != cMaj) return nMaj > cMaj;
    if (nMin != cMin) return nMin > cMin;
    return nPat > cPat;   // Patch is the tiebreaker
}


// ── doOtaUpdate ───────────────────────────────────────────────────────────────
// Downloads the firmware binary from `downloadUrl` and flashes it to the
// inactive OTA partition. Publishes MQTT status events throughout.
//
// HTTPUpdate handles:
//   - Streaming the download directly to flash (no RAM buffer needed)
//   - Partition selection (writes to whichever of ota_0/ota_1 is inactive)
//   - Setting the bootloader flag to boot from the new partition on restart
//
// On success: publishes "ota_success" to MQTT then calls ESP.restart().
// On failure: publishes "ota_failed" to MQTT and returns (device stays running).
static void doOtaUpdate(const String& downloadUrl, const String& targetVersion) {
    Serial.printf("[OTA] Starting download: %s\n", downloadUrl.c_str());

    // Notify Node-RED that download is starting so it can show progress in a dashboard
    String extra = "\"target_version\":\"" + targetVersion + "\"";
    mqttPublishStatus("ota_downloading", extra.c_str());

    WiFiClientSecure client;
    client.setCACert(GITHUB_ROOT_CA);   // Verify the download host (objects.githubusercontent.com)

    // Register progress callbacks so the serial log shows meaningful output
    httpUpdate.onStart([]() {
        Serial.println("[OTA] Flash write started");
    });
    httpUpdate.onEnd([]() {
        Serial.println("[OTA] Flash write complete");
    });
    httpUpdate.onProgress([](int cur, int total) {
        // Only print at each 10% increment to avoid flooding the serial log
        static int lastPct = -1;
        int pct = (total > 0) ? (cur * 100 / total) : 0;
        if (pct / 10 != lastPct / 10) {
            Serial.printf("[OTA] Progress: %d%%\n", pct);
            lastPct = pct;
        }
    });
    httpUpdate.onError([](int err) {
        Serial.printf("[OTA] Flash error code: %d\n", err);
    });

    // Disable automatic restart — we want to publish the success MQTT message
    // before the connection drops, so we control the restart ourselves.
    httpUpdate.rebootOnUpdate(false);

    // Perform the actual download and flash.
    // This call blocks until the download is complete or fails.
    t_httpUpdate_return result = httpUpdate.update(client, downloadUrl);

    switch (result) {
        case HTTP_UPDATE_OK:
            // Flash succeeded — the bootloader will switch partitions on next boot
            Serial.println("[OTA] Flash succeeded — restarting");
            mqttPublishStatus("ota_success",
                ("\"new_version\":\"" + targetVersion + "\"").c_str());
            delay(500);     // Give the MQTT publish time to be sent before disconnect
            ESP.restart();  // Boot into the new firmware
            break;

        case HTTP_UPDATE_NO_UPDATES:
            // The server says no update is available — should not happen since we
            // already compared versions, but handle it gracefully
            Serial.println("[OTA] Server reports no update available");
            break;

        case HTTP_UPDATE_FAILED:
        default:
            // Flash failed — device stays running on the current firmware
            Serial.printf("[OTA] Update failed: %s\n",
                          httpUpdate.getLastErrorString().c_str());
            mqttPublishStatus("ota_failed",
                ("\"error\":\"" + httpUpdate.getLastErrorString() +
                 "\",\"current_version\":\"" FIRMWARE_VERSION "\"").c_str());
            break;
    }
}


// ── otaCheckNow ───────────────────────────────────────────────────────────────
// The main OTA entry point. Fetches the latest release, compares version,
// and calls doOtaUpdate() if a newer version exists.
// Called periodically from otaLoop() and on demand from the MQTT handler.
void otaCheckNow() {
    Serial.println("[OTA] Checking GitHub for new firmware...");
    mqttPublishStatus("ota_checking",
                      "\"current_version\":\"" FIRMWARE_VERSION "\"");

    String tagName, downloadUrl;
    if (!fetchLatestRelease(tagName, downloadUrl)) {
        // Network error, missing field, or no firmware.bin — stay on current firmware
        Serial.println("[OTA] Could not fetch release info");
        mqttPublishStatus("ota_failed",
            "\"error\":\"GitHub API unreachable\","
            "\"current_version\":\"" FIRMWARE_VERSION "\"");
        return;
    }

    Serial.printf("[OTA] Latest release: %s   Running: %s\n",
                  tagName.c_str(), FIRMWARE_VERSION);

    // If the latest tag is not newer than what we're running, do nothing
    if (!semverNewer(FIRMWARE_VERSION, tagName.c_str())) {
        Serial.println("[OTA] Firmware is already up to date");
        return;
    }

    // A newer version exists — start the download and flash
    doOtaUpdate(downloadUrl, tagName);
    // Note: if doOtaUpdate succeeds it never returns (calls ESP.restart()).
    // If it fails it returns here and the device continues operating normally.
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
