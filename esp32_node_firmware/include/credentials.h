#pragma once

#include <Arduino.h>
#include <Preferences.h>   // Arduino ESP32 wrapper around ESP-IDF NVS (Non-Volatile Storage)
#include "config.h"
#include "nvs_utils.h"     // NvsPutIfChanged — compare-before-write wrappers

// =============================================================================
// credentials.h  —  Credential bundle definition and NVS storage helpers
//
// The CredentialBundle is the in-memory representation of everything a node
// needs to connect to Wi-Fi and the MQTT broker. It is NEVER transmitted
// directly — only the compact WireBundle (defined in espnow_bootstrap.h) is
// sent over ESP-NOW, and it is always encrypted first.
//
// CredentialStore wraps the Arduino Preferences library to persist and load
// the bundle from NVS flash — data survives power cycles and OTA updates.
// =============================================================================


// Which source provided the stored credentials.
// Used to decide whether to skip ESP-NOW bootstrap on next boot.
enum class CredSource : uint8_t {
    SIBLING = 0,   // Credentials were received via ESP-NOW from a peer node
    ADMIN   = 1    // Credentials were entered by an administrator via the AP portal
};


// In-memory credential bundle.
// Field sizes are generous to accommodate long passwords and URLs.
// IMPORTANT: Never log, print, or transmit this struct in plaintext.
struct CredentialBundle {
    char     wifi_ssid[33]        = {0};   // Wi-Fi network name (max 32 chars + null)
    char     wifi_password[65]    = {0};   // Wi-Fi password (max 64 chars + null)
    char     mqtt_broker_url[128] = {0};   // Full broker URL e.g. mqtt://192.168.1.10:1883
    char     mqtt_username[65]    = {0};   // MQTT username (may be empty)
    char     mqtt_password[65]    = {0};   // MQTT password (may be empty)
    uint8_t  rotation_key[16]     = {0};   // 16-byte AES-128 key used to decrypt
                                           // credential rotation messages from MQTT.
                                           // Set via the AP portal (hex input field).
    uint64_t timestamp            = 0;     // Unix epoch (seconds) when these credentials
                                           // were last set by an admin. Nodes pick the
                                           // bundle with the highest timestamp.
    CredSource source             = CredSource::SIBLING;  // Who provided these credentials
};


// Static helper class that reads/writes CredentialBundle to/from NVS flash.
// All methods open and close the NVS namespace on each call — this is
// intentionally simple; credentials are only read/written at boot and during
// rotation, not in a tight loop.
class CredentialStore {
public:

    // Load the stored credential bundle from NVS into `out`.
    // Returns true only if a bundle exists AND has a non-zero timestamp
    // (a zero timestamp means the NVS was empty or was never written).
    static bool load(CredentialBundle& out) {
        Preferences prefs;
        // Open NVS namespace in read-only mode (true = read-only)
        if (!prefs.begin(NVS_NAMESPACE, true)) return false;

        out.timestamp = prefs.getULong64("ts", 0);            // 0 = not stored
        out.source    = (CredSource)prefs.getUChar("src", 0); // default: SIBLING

        // Validate that mandatory fields are present and not too long
        size_t n;
        n = prefs.getBytesLength("ssid");
        if (n == 0 || n >= sizeof(out.wifi_ssid)) { prefs.end(); return false; }
        prefs.getBytes("ssid",  out.wifi_ssid,        sizeof(out.wifi_ssid));

        n = prefs.getBytesLength("wpass");
        if (n >= sizeof(out.wifi_password))           { prefs.end(); return false; }
        prefs.getBytes("wpass", out.wifi_password,    sizeof(out.wifi_password));

        n = prefs.getBytesLength("murl");
        if (n == 0 || n >= sizeof(out.mqtt_broker_url)) { prefs.end(); return false; }
        prefs.getBytes("murl",  out.mqtt_broker_url,  sizeof(out.mqtt_broker_url));

        // Optional fields — missing keys return empty arrays, which is fine
        prefs.getBytes("musr",   out.mqtt_username,   sizeof(out.mqtt_username));
        prefs.getBytes("mpwd",   out.mqtt_password,   sizeof(out.mqtt_password));
        prefs.getBytes("rotkey", out.rotation_key,    16);  // raw 16 bytes, no null term

        prefs.end();
        return out.timestamp > 0;   // Only valid if timestamp was actually written
    }


    // Save a credential bundle to NVS flash.
    // Returns true only if every field was written successfully.
    // Strings are stored with their null terminator (+1) for easy re-read.
    static bool save(const CredentialBundle& b) {
        Preferences prefs;
        // Open NVS namespace in read-write mode (false = read-write)
        if (!prefs.begin(NVS_NAMESPACE, false)) return false;

        bool ok = true;
        // NvsPutIfChanged compares existing value and skips the write if
        // unchanged — reduces NVS flash wear on repeated rotations of the
        // same bundle. Return semantics match Preferences::put*() so the
        // "> 0" / "== 1" success checks below keep working.
        ok &= NvsPutIfChanged(prefs, "ts",     (uint64_t)b.timestamp)              > 0;
        ok &= NvsPutIfChanged(prefs, "src",    (uint8_t)b.source)                  == 1;
        ok &= NvsPutIfChanged(prefs, "ssid",   b.wifi_ssid,       strlen(b.wifi_ssid)       + 1) > 0;
        ok &= NvsPutIfChanged(prefs, "wpass",  b.wifi_password,   strlen(b.wifi_password)   + 1) > 0;
        ok &= NvsPutIfChanged(prefs, "murl",   b.mqtt_broker_url, strlen(b.mqtt_broker_url) + 1) > 0;
        ok &= NvsPutIfChanged(prefs, "musr",   b.mqtt_username,   strlen(b.mqtt_username)   + 1) > 0;
        ok &= NvsPutIfChanged(prefs, "mpwd",   b.mqtt_password,   strlen(b.mqtt_password)   + 1) > 0;
        ok &= NvsPutIfChanged(prefs, "rotkey", b.rotation_key,    16)                        > 0;

        prefs.end();
        return ok;
    }


    // Returns true if the stored credentials were set by an admin (via AP portal).
    // Used at boot to decide whether to skip ESP-NOW bootstrap — if an admin
    // already configured this device there is no need to ask siblings for credentials.
    static bool hasPrimary() {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, true)) return false;
        bool primary = prefs.getUChar("src", 0) == (uint8_t)CredSource::ADMIN;
        prefs.end();
        return primary;
    }


    // Reset the device restart counter to zero.
    // Called after a successful Wi-Fi connection to clear the failure count.
    // Uses NvsPutIfChanged — on a clean boot the counter is already 0, so
    // the write is skipped, which matters because this is called on every
    // successful WiFi connect (potentially many times a day under flapping
    // network conditions).
    static void clearRestartCount() {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, false)) return;
        NvsPutIfChanged(prefs, "restarts", (uint8_t)0);
        prefs.end();
    }


    // Add 1 to the stored restart counter and return the new value.
    // Called when a Wi-Fi connection fails after all attempts are exhausted,
    // just before calling ESP.restart(). When this counter reaches
    // DEVICE_RESTART_MAX the firmware gives up restarting and enters AP mode.
    static uint8_t incrementRestartCount() {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, false)) return 0;
        uint8_t n = prefs.getUChar("restarts", 0) + 1;  // read, increment, write back
        prefs.putUChar("restarts", n);
        prefs.end();
        return n;
    }


    // Read the current restart counter without changing it.
    // Used at the start of WIFI_CONNECT state to check if we are already
    // close to the restart limit before attempting to connect.
    static uint8_t getRestartCount() {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, true)) return 0;
        uint8_t n = prefs.getUChar("restarts", 0);
        prefs.end();
        return n;
    }


    // Mark credentials as potentially stale.
    // Called by loop() before a restart caused by sustained WiFi or MQTT failure,
    // so that the next boot forces a sibling credential re-verify even if the stored
    // credentials came from an admin (hasPrimary() == true).
    //
    // Uses a read-before-write guard to avoid unnecessary NVS flash wear in
    // flapping network scenarios where loop() would otherwise write on every restart.
    static void setCredStale(bool stale) {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, false)) return;
        uint8_t current = prefs.getUChar(NVS_KEY_CRED_STALE, 0);
        uint8_t desired = stale ? 1 : 0;
        if (current != desired) prefs.putUChar(NVS_KEY_CRED_STALE, desired);
        prefs.end();
    }


    // Returns true if credentials were flagged stale by a previous boot cycle.
    // Called once at the start of BOOT state before the hasPrimary() check.
    // A true result forces BOOTSTRAP_REQUEST even for admin-provisioned devices.
    static bool isCredStale() {
        Preferences prefs;
        if (!prefs.begin(NVS_NAMESPACE, true)) return false;
        bool stale = prefs.getUChar(NVS_KEY_CRED_STALE, 0) != 0;
        prefs.end();
        return stale;
    }
};
