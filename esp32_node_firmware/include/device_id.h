#pragma once

#include <Arduino.h>
#include <Preferences.h>   // Arduino NVS wrapper — UUID persists across reboots and OTA updates
#include <esp_wifi.h>      // esp_wifi_get_mac — used as entropy seed for UUID generation
#include "config.h"

// =============================================================================
// device_id.h  —  Persistent UUID-based device identity
//
// DESIGN (Option 2 from spec):
//   On first boot, a UUID is generated and written to a dedicated NVS namespace
//   ("esp32id") that is separate from credentials. On every subsequent boot the
//   stored UUID is read back. This gives the device a stable, unique identity
//   that survives firmware OTA updates and credential rotations.
//
//   The UUID is only lost if the NVS partition is explicitly erased (e.g. via
//   esptool.py --erase-flash). In that case a new UUID is auto-generated on
//   the next boot — the device gets a new identity but continues operating.
//
// UUID FORMAT:
//   Standard RFC 4122 version 4 (random), lowercase with hyphens:
//   e.g.  "a3f2c1d4-5e6f-4789-8abc-def012345678"
//   Length: 36 chars + null = 37 bytes.
//
// ENTROPY SOURCE:
//   The ESP32 hardware RNG (esp_random()) is seeded at boot from a combination
//   of thermal noise, radio jitter, and boot-time state. It produces good
//   quality random numbers without requiring Wi-Fi or Bluetooth to be active.
//   The MAC address is XOR'd in as an extra uniqueness guarantee so that two
//   devices flashed simultaneously from the same binary cannot collide.
//
// NVS NAMESPACE:
//   "esp32id" — separate from "esp32cred" so that a credential wipe (e.g.
//   factory-resetting credentials via NVS clear) does not also destroy the
//   device identity.
//
// MQTT USAGE:
//   The UUID replaces the MAC-derived 6-char ID in all MQTT topic paths.
//   Full topic example:
//     Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/
//       a3f2c1d4-5e6f-4789-8abc-def012345678/status
// =============================================================================

#define DEVICE_ID_NVS_NAMESPACE  "esp32id"   // Separate namespace from credentials
#define DEVICE_ID_NVS_KEY        "uuid"      // Key within the namespace
#define DEVICE_ID_LEN            36          // UUID string length without null terminator


// ── DeviceId class ─────────────────────────────────────────────────────────────
// All methods are static — call DeviceId::get() anywhere after DeviceId::init().
class DeviceId {
public:

    // ── init ──────────────────────────────────────────────────────────────────
    // Must be called once at boot (before mqttBegin or any MQTT topic building).
    // Loads the stored UUID from NVS, or generates and saves a new one if none
    // exists. After this call, DeviceId::get() returns the stable UUID string.
    static void init() {
        Preferences prefs;

        // Try to load an existing UUID from NVS
        if (prefs.begin(DEVICE_ID_NVS_NAMESPACE, true)) {   // true = read-only
            String stored = prefs.getString(DEVICE_ID_NVS_KEY, "");
            prefs.end();

            if (stored.length() == DEVICE_ID_LEN) {
                // Valid UUID found — use it
                _uuid = stored;
                Serial.printf("[DeviceId] Loaded UUID: %s\n", _uuid.c_str());
                return;
            }
        }

        // No valid UUID in NVS — generate a new one
        _uuid = _generate();
        // (#48) WARN-level so this stands out in fleet logs. A "Generated new"
        // message after first-boot provisioning is the smoking gun for UUID
        // drift on subsequent boots — see SUGGESTED_IMPROVEMENTS.txt #48.
        Serial.printf("[W][DeviceId] Generated new UUID: %s "
                      "(if not first boot, NVS lost / regenerate-bug — see #48)\n",
                      _uuid.c_str());

        // Persist it so the same UUID is used on all future boots
        if (prefs.begin(DEVICE_ID_NVS_NAMESPACE, false)) {   // false = read-write
            prefs.putString(DEVICE_ID_NVS_KEY, _uuid);
            prefs.end();
            Serial.println("[DeviceId] UUID saved to NVS");
        } else {
            // NVS write failed — UUID will still work this session but won't
            // survive a reboot. Log the failure so the developer can investigate.
            Serial.println("[DeviceId] WARNING: failed to save UUID to NVS");
        }
    }


    // ── get ───────────────────────────────────────────────────────────────────
    // Returns the device UUID as an Arduino String (e.g. "a3f2c1d4-5e6f-...").
    // Always call DeviceId::init() before the first call to get().
    // If init() was never called, returns "uninitialized" as a safe fallback.
    static const String& get() {
        return _uuid;
    }


    // ── getMac ────────────────────────────────────────────────────────────────
    // Returns the full MAC address as a colon-separated string for logging and
    // status payloads. The MAC is still useful as a hardware reference even
    // though it is no longer used as the primary device identity.
    // Example: "AA:BB:CC:A3:F2:C1"
    static String getMac() {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(buf);
    }


private:

    static String _uuid;   // Holds the UUID after init() — never empty after init


    // ── _generate ─────────────────────────────────────────────────────────────
    // Generates a random RFC 4122 version 4 UUID.
    //
    // RFC 4122 v4 format:  xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    //   - All x positions are random hex digits
    //   - The '4' in position 13 is the version number (always 4)
    //   - The y in position 17 is one of: 8, 9, a, b (sets the variant bits)
    //
    // Entropy: esp_random() returns hardware random 32-bit integers.
    // XOR with MAC bytes adds per-device uniqueness on top of the hardware RNG.
    static String _generate() {
        // Read 16 random bytes from the ESP32 hardware RNG
        uint8_t rnd[16];
        for (int i = 0; i < 4; i++) {
            uint32_t r = esp_random();   // Hardware RNG — 32 bits per call
            rnd[i*4 + 0] = (r >>  0) & 0xFF;
            rnd[i*4 + 1] = (r >>  8) & 0xFF;
            rnd[i*4 + 2] = (r >> 16) & 0xFF;
            rnd[i*4 + 3] = (r >> 24) & 0xFF;
        }

        // XOR the MAC address into the first 6 bytes for extra per-device entropy.
        // Two devices flashed at the same millisecond from the same binary will
        // still get different UUIDs because their MACs differ.
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        for (int i = 0; i < 6; i++) {
            rnd[i] ^= mac[i];
        }

        // Apply RFC 4122 version 4 bit fields:
        //   Byte 6, top nibble = 0100 (version 4)
        //   Byte 8, top 2 bits = 10   (variant 1)
        rnd[6] = (rnd[6] & 0x0F) | 0x40;   // version 4
        rnd[8] = (rnd[8] & 0x3F) | 0x80;   // variant bits 10xxxxxx

        // Format as lowercase hex with hyphens: 8-4-4-4-12
        char buf[37];
        snprintf(buf, sizeof(buf),
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            rnd[0],  rnd[1],  rnd[2],  rnd[3],   // 8 hex chars
            rnd[4],  rnd[5],                       // 4 hex chars
            rnd[6],  rnd[7],                       // 4 hex chars (contains version)
            rnd[8],  rnd[9],                       // 4 hex chars (contains variant)
            rnd[10], rnd[11], rnd[12],
            rnd[13], rnd[14], rnd[15]);     //  12 hex chars
        return String(buf);
    }
};

// Static member definition — must be outside the class body in C++
String DeviceId::_uuid = "uninitialized";
