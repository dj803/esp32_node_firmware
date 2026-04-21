#pragma once

#include <math.h>

// =============================================================================
// ranging_math.h  —  Shared RSSI-to-distance conversion
//
// This is a pure utility header — no Arduino, no MQTT, no FreeRTOS.
// Safe to include from host-side unit tests.
//
// MODEL:  log-distance path loss
//   d = 10 ^ ((txPower - rssi) / (10 × n))
//
// Parameters:
//   rssi      — received signal strength in dBm (negative)
//   txPower   — measured power at 1 m in dBm (typically -59)
//   pathLossN — path-loss exponent
//               2.0f = free space (BLE open area)
//               2.5f = semi-open indoor (ESP-NOW in office/warehouse)
//               3.0f = heavy indoor obstruction
//
// Both BLE (ble.h) and ESP-NOW ranging (espnow_ranging.h) use the same
// formula; this file eliminates that duplication and makes the function
// trivially testable on a host without hardware.
// =============================================================================

// Returns the estimated distance in metres.
// Clamps negative distances (physically impossible) to 0.01 m so callers
// never receive NaN or a negative value.
inline float rssiToDistance(int8_t rssi, int8_t txPower, float pathLossN) {
    float d = powf(10.0f, (float)(txPower - rssi) / (10.0f * pathLossN));
    return (d < 0.01f) ? 0.01f : d;
}
