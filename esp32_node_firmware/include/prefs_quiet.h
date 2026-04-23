#pragma once

#include <Preferences.h>
#include <nvs.h>

// =============================================================================
// prefs_quiet.h  —  silent wrapper around Preferences::begin()
//
// PROBLEM: arduino-esp32's Preferences class calls log_e() inside begin()
// when the namespace doesn't yet exist. log_e() expands to log_printf()
// which goes straight to Arduino's HAL printer (NOT esp_log_write), so
// esp_log_level_set() cannot silence it (we discovered the hard way in
// v0.4.02 — the no-op tombstone is in src/main.cpp).
//
// Every boot of any device that has never written to a particular NVS
// namespace (post-OTA validation namespace, RFID whitelist before any
// cmd/rfid/whitelist add, BLE tracked-MAC namespace before any
// cmd/ble/track) prints:
//   [E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND
//
// FIX: pre-check via the underlying ESP-IDF nvs_open() (which does NOT
// log on NOT_FOUND in IDF 5.x) and short-circuit BEFORE calling
// Preferences::begin(). The Arduino log line is therefore never reached
// when the namespace is genuinely missing. begin() is only called for
// existing namespaces, where it succeeds silently.
//
// USAGE: replace `prefs.begin(NS, true)` with `prefsTryBegin(prefs, NS, true)`
// at all read-only call sites. The signature is identical; the helper
// returns false (silently) when the namespace is missing, just as the
// caller already expects from a failed begin(). Read-write callers can
// continue to use prefs.begin() directly — RW always creates the
// namespace, never logs.
// =============================================================================

// Pre-check whether an NVS namespace exists. Uses nvs_open(NVS_READONLY)
// which returns ESP_ERR_NVS_NOT_FOUND silently for missing namespaces in
// ESP-IDF 5.x (verified empirically: arduino-esp32 3.1.1 / IDF 5.x base).
//
// Returns true if the namespace exists and a read-only open would succeed.
// Returns false otherwise (missing namespace, partition error, etc.).
inline bool nvsNamespaceExists(const char* ns) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        nvs_close(h);
        return true;
    }
    return false;
}

// Drop-in replacement for prefs.begin(NS, readOnly).
// For read-only opens: pre-checks namespace existence to avoid the
// Arduino log_e spam. For read-write opens: passes through (RW creates
// the namespace, doesn't log).
//
// Behaviour mirrors Preferences::begin():
//   - Returns true on success
//   - Returns false if namespace doesn't exist (read-only) or any other
//     init failure
//   - Caller must call prefs.end() on success, same as before
inline bool prefsTryBegin(Preferences& prefs, const char* ns, bool readOnly = false) {
    if (readOnly && !nvsNamespaceExists(ns)) {
        return false;   // silent — caller treats this same as begin()==false
    }
    return prefs.begin(ns, readOnly);
}
