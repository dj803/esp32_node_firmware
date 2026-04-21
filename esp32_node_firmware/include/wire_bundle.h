#pragma once

// =============================================================================
// wire_bundle.h  —  Compact on-wire credential payload (176 bytes)
//
// Extracted from espnow_bootstrap.h in v0.3.14 so the struct layout can be
// unit-tested on the host (test_native) without pulling in Arduino, ESP-IDF,
// or esp_now.h. Only <stdint.h> is required here — no runtime dependencies.
//
// The bundleToWire()/wireToBundle() helpers that bridge CredentialBundle
// remain in espnow_bootstrap.h because they touch CredentialBundle, which
// is defined in credentials.h (which pulls Arduino.h).
//
// CONTRACT (do not change without bumping ESPNOW_WIRE_VERSION):
//   - Exactly 176 bytes, verified by static_assert below.
//   - First byte is `wire_version` so receivers can fast-reject mismatched
//     senders before parsing further.
//   - `#pragma pack(push, 1)` is mandatory — without it LX6 inserts 7 bytes
//     of padding before the uint64 `timestamp`, blowing the size contract
//     and desynchronising every field offset between sender and receiver.
//   - Little-endian only. All ESP32 parts (Xtensa LX6/LX7) are LE, so
//     `timestamp` is serialised LE-first with no byte-swap. Porting to a
//     big-endian target requires explicit conversion at the boundary.
// =============================================================================

#include <stdint.h>

// Increment whenever the WireBundle layout or field meaning changes.
// Receivers reject frames whose wire_version != ESPNOW_WIRE_VERSION.
// Version 0 is reserved to identify pre-v0.3.08 senders (those sent 175
// bytes, not 176, so the length check rejects them anyway).
#ifndef ESPNOW_WIRE_VERSION
#  define ESPNOW_WIRE_VERSION  1
#endif

#pragma pack(push, 1)
struct WireBundle {
    uint8_t  wire_version;       // Must equal ESPNOW_WIRE_VERSION — first byte for fast reject
    char     wifi_ssid[33];      // 32 chars + null
    char     wifi_password[33];  // 32 chars + null
    char     mqtt_broker_url[50];// e.g. mqtt://192.168.1.100:1883 (26 chars typical)
    char     mqtt_username[17];  // 16 chars + null
    char     mqtt_password[17];  // 16 chars + null
    uint8_t  rotation_key[16];   // raw 16-byte AES key
    uint64_t timestamp;
    uint8_t  source;             // 0=sibling 1=admin
};
#pragma pack(pop)

static_assert(sizeof(WireBundle) == 176, "WireBundle size changed — check ESP-NOW fit");
