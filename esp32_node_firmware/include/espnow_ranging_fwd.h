#pragma once

#include <stdint.h>

// =============================================================================
// espnow_ranging_fwd.h  —  Forward declarations for espnow_ranging.h public API
//
// Include this file in espnow_responder.h (and any other module that calls
// espnowRangingObserve before espnow_ranging.h is included) to break the
// include-order dependency.
//
// The actual definition lives in espnow_ranging.h, which is included after
// mqtt_client.h in esp32_firmware.ino.
// =============================================================================

// Called by espnow_responder.h's receive dispatcher for every incoming frame.
// Updates the peer tracking table with the sender's MAC and RSSI.
void espnowRangingObserve(const uint8_t* mac6, int8_t rssi);

// Called from mqtt_client.h for cmd/espnow/calibrate. Drives the two-point
// calibration state machine (measure_1m / measure_d / commit / reset).
void espnowCalibrateCmd(const char* payload, size_t len);

// Called from mqtt_client.h for cmd/espnow/filter. Updates EMA alpha and
// outlier threshold in gAppConfig + NVS.
void espnowSetFilter(const char* payload, size_t len);

// Called from mqtt_client.h for cmd/espnow/track. Sets the MAC publish filter:
// only peers in `macs[0..n-1]` are included in the MQTT espnow publish.
// n == 0 clears the filter (publish all). Observations still accumulate for
// all peers so switching the filter takes effect immediately.
void espnowSetTrackedMacs(const char** macs, uint8_t n);
