#pragma once

#include <stdio.h>
#include <stdint.h>

// =============================================================================
// mac_utils.h  —  MAC address formatting helpers
//
// Pure utility header — no Arduino, no MQTT, no FreeRTOS.
// Safe to include from host-side unit tests.
//
// Previously duplicated at:
//   espnow_ranging.h:64   (snprintf inline, uppercase)
//   device_id.h:110       (sprintf inline, uppercase)
// =============================================================================

// Format a 6-byte MAC address into a "XX:XX:XX:XX:XX:XX\0" string.
// `out` must point to a buffer of at least 18 bytes.
// Always null-terminates. Produces uppercase hex.
inline void macToString(const uint8_t mac6[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac6[0], mac6[1], mac6[2], mac6[3], mac6[4], mac6[5]);
}

// Parse a "XX:XX:XX:XX:XX:XX" string into a 6-byte MAC array.
// Accepts upper or lower case hex. Returns true on success, false on any
// parse error (wrong length, non-hex char, etc).
// Used by app_config.h's per-peer calibration table (v0.4.09 / #41.7).
inline bool macStringToBytes(const char* s, uint8_t out[6]) {
    if (!s) return false;
    unsigned int b[6];
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xFF) return false;
        out[i] = (uint8_t)b[i];
    }
    return true;
}
