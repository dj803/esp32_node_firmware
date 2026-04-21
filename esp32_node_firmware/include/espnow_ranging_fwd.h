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
