#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// =============================================================================
// nvs_utils.h  —  Compare-before-write wrappers around Preferences::put*()
//
// WHY:
//   The ESP32's NVS partition is flash-backed. Every put*() call produces a
//   write even when the value hasn't changed, steadily consuming the NVS
//   sector's erase-cycle budget. In a fleet where credentials rotate daily,
//   settings are touched on every boot, and the broker cache re-saves on
//   every discovery sweep, the cumulative wear is nontrivial.
//
//   These helpers read the existing value first, compare it byte-for-byte
//   against the incoming value, and skip the write if they match. Return
//   semantics mimic Arduino's Preferences::put*() so existing
//   `ok &= prefs.putX(...)` accumulator chains continue to work.
//
// USAGE:
//   Replace:
//       ok &= prefs.putBytes("ssid", b.wifi_ssid, strlen(...) + 1) > 0;
//   With:
//       ok &= NvsPutIfChanged(prefs, "ssid", b.wifi_ssid, strlen(...) + 1) > 0;
//
//   Replace:
//       ok &= prefs.putUChar("src", (uint8_t)b.source) == 1;
//   With:
//       ok &= NvsPutIfChanged(prefs, "src", (uint8_t)b.source) == 1;
//
// RETURN SEMANTICS:
//   - On genuine write: mirrors the Preferences::put*() return (bytes written,
//     or 0 on failure).
//   - On skip (value unchanged): returns a positive value (sizeof the stored
//     type, or the length of the bytes/string) so callers' "> 0" / "== 1" /
//     "== 8" checks stay TRUE without re-writing. This preserves the existing
//     success semantics of save() methods that chain many writes with
//     `ok &= ...`.
//
// CONCURRENCY:
//   Same rules as bare Preferences: not safe to call concurrently from
//   multiple tasks against the same namespace. All call-sites in this
//   codebase currently invoke save() from the main task only.
// =============================================================================


// ── int8_t (putChar) ─────────────────────────────────────────────────────────
inline size_t NvsPutIfChanged(Preferences& p, const char* key, int8_t val) {
    if (p.isKey(key)) {
        int8_t existing = p.getChar(key, (int8_t)(val ^ 0x7F));
        if (existing == val) return 1;
    }
    return p.putChar(key, val);
}


// ── uint8_t (putUChar) ────────────────────────────────────────────────────────
inline size_t NvsPutIfChanged(Preferences& p, const char* key, uint8_t val) {
    if (p.isKey(key)) {
        uint8_t existing = p.getUChar(key, val ^ 0xFF);   // dummy default that can't equal val
        if (existing == val) return 1;                    // match Preferences::putUChar success return
    }
    return p.putUChar(key, val);
}


// ── uint16_t (putUShort) ──────────────────────────────────────────────────────
inline size_t NvsPutIfChanged(Preferences& p, const char* key, uint16_t val) {
    if (p.isKey(key)) {
        uint16_t existing = p.getUShort(key, val ^ 0xFFFF);
        if (existing == val) return 2;
    }
    return p.putUShort(key, val);
}


// ── uint64_t (putULong64) ─────────────────────────────────────────────────────
inline size_t NvsPutIfChanged(Preferences& p, const char* key, uint64_t val) {
    if (p.isKey(key)) {
        uint64_t existing = p.getULong64(key, val ^ 0xFFFFFFFFFFFFFFFFULL);
        if (existing == val) return 8;
    }
    return p.putULong64(key, val);
}


// ── raw bytes (putBytes) ──────────────────────────────────────────────────────
// Pass `len` explicitly — matches Preferences::putBytes signature. Uses a
// small stack buffer for comparison up to 256 bytes (covers every call-site
// in this codebase: CredentialBundle fields are ≤128, rotation_key is 16).
// For buffers > 256 bytes, falls through to unconditional write to avoid
// oversizing the stack frame.
inline size_t NvsPutIfChanged(Preferences& p, const char* key,
                              const void* data, size_t len) {
    if (len > 0 && len <= 256 && p.isKey(key)) {
        if (p.getBytesLength(key) == len) {
            uint8_t existing[256];
            p.getBytes(key, existing, len);
            if (memcmp(existing, data, len) == 0) return len;
        }
    }
    return p.putBytes(key, data, len);
}


// ── null-terminated string (putString) ────────────────────────────────────────
// String compare via strcmp. Note: putString stores a C-string with its null
// terminator, and getString returns an Arduino String — cheaper to compare
// with strcmp than to round-trip through String.
inline size_t NvsPutIfChanged(Preferences& p, const char* key, const char* val) {
    if (p.isKey(key)) {
        String existing = p.getString(key, "");
        if (strcmp(existing.c_str(), val) == 0) return strlen(val) + 1;
    }
    return p.putString(key, val);
}
