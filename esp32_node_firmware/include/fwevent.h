#pragma once

// =============================================================================
// fwevent.h  —  Typed firmware event identifiers for MQTT status messages
//
// Replaces the free-form string literals previously scattered across
// mqtt_client.h, ota.h, and any future callers of mqttPublishStatus().
//
// Motivation: typos in event name strings create silent new states in Node-RED.
// A compile-time enum prevents misspellings and makes exhaustiveness-checking
// possible. The fwEventName() lookup table keeps the outgoing string values
// identical to the previous literals so no Node-RED flows need updating.
//
// IMPORTANT: do NOT rename the string values returned by fwEventName() without
// also updating the matching Node-RED function nodes. They are part of the
// published MQTT contract.
//
// This header is dependency-free (no Arduino.h, no FreeRTOS) so it can be
// included by both on-device firmware modules and host-side Unity test builds.
// =============================================================================

#include <stdint.h>

// ── Event enum ────────────────────────────────────────────────────────────────
enum class FwEvent : uint8_t {
    BOOT                 = 0,
    HEARTBEAT            = 1,
    CRED_ROTATE_REJECTED = 2,
    CRED_ROTATED         = 3,
    CONFIG_MODE_ACTIVE   = 4,
    RESTARTING           = 5,
    OTA_CHECKING         = 6,
    OTA_DOWNLOADING      = 7,
    OTA_FAILED           = 8,
    OTA_SUCCESS          = 9,
    SLEEPING             = 10,   // cmd/sleep — light sleep for duration_s seconds
    DEEP_SLEEPING        = 11,   // cmd/deep_sleep — deep sleep for duration_s seconds
    MODEM_SLEEPING       = 12,   // cmd/modem_sleep — Wi-Fi modem sleep for duration_s seconds
    LOCATING             = 13,   // cmd/locate — status LED locate flash in progress (4 s)
};


// ── String lookup ─────────────────────────────────────────────────────────────
// Returns the Node-RED event string for a given FwEvent value.
// Declared inline so it can be included in multiple translation units without
// violating the One Definition Rule.
inline const char* fwEventName(FwEvent ev) {
    switch (ev) {
        case FwEvent::BOOT:                  return "boot";
        case FwEvent::HEARTBEAT:             return "heartbeat";
        case FwEvent::CRED_ROTATE_REJECTED:  return "cred_rotate_rejected";
        case FwEvent::CRED_ROTATED:          return "cred_rotated";
        case FwEvent::CONFIG_MODE_ACTIVE:    return "config_mode_active";
        case FwEvent::RESTARTING:            return "restarting";
        case FwEvent::OTA_CHECKING:          return "ota_checking";
        case FwEvent::OTA_DOWNLOADING:       return "ota_downloading";
        case FwEvent::OTA_FAILED:            return "ota_failed";
        case FwEvent::OTA_SUCCESS:           return "ota_success";
        case FwEvent::SLEEPING:              return "sleeping";
        case FwEvent::DEEP_SLEEPING:         return "deep_sleeping";
        case FwEvent::MODEM_SLEEPING:        return "modem_sleeping";
        case FwEvent::LOCATING:             return "locating";
        default:                             return "unknown";
    }
}
