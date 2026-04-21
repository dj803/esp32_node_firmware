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
