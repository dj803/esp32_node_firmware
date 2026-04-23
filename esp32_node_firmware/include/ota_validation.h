#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include "config.h"
#include "logging.h"
#include "fwevent.h"
#include "prefs_quiet.h"   // (v0.4.03) prefsTryBegin — silent on missing namespace

// =============================================================================
// ota_validation.h  —  Post-OTA app self-validation (Phase 2 / v0.3.34)
//
// PURPOSE: Close the highest-severity OTA gap identified in the v0.3.32
// bulletproofing investigation: a new firmware that boots, publishes
// OTA_SUCCESS, then crashes 30 s later (or fails to reach MQTT) currently
// just sits there forever. This module makes that survivable.
//
// DETECTION (two paths, both supported):
//   1. NVS flag (universal — works on any bootloader). The OLD firmware
//      writes an `ota_pending` flag to NVS in otaValidationArmRollback()
//      JUST BEFORE calling ESP.restart() after a successful flash. The NEW
//      firmware reads the flag in otaValidationCheckBoot() and arms the
//      validation deadline. This is the path that protects the existing
//      fleet starting from the FIRST OTA done by v0.3.34+ firmware.
//   2. PENDING_VERIFY partition state (bootloader-assisted — currently
//      DISABLED, see platformio.ini). When CONFIG_BOOTLOADER_APP_ROLLBACK
//      _ENABLE=y is on the bootloader, esp_ota_get_state_partition() returns
//      PENDING_VERIFY for a freshly-OTA'd slot. We honor either signal —
//      they're complementary. Phase 4 brings this online by serial-flashing
//      the new bootloader.
//
// VALIDATION FLOW:
//   1. Boot → otaValidationCheckBoot() reads NVS flag + partition state.
//      If either says "post-OTA", set _otaValidationPending = true and
//      _otaValidationDeadlineMs = millis() + OTA_VALIDATION_DEADLINE_MS.
//   2. MQTT connects → onMqttConnect calls otaValidationOnMqttConnect()
//      (publishes OTA_VALIDATING) and otaValidationConfirmHealth() (calls
//      esp_ota_mark_app_valid_cancel_rollback(), clears NVS flag, publishes
//      OTA_VALIDATED). Boot announcement is the proof MQTT works — that's
//      the strongest signal we have.
//   3. loop() ticks otaValidationTick(). If millis() crosses the deadline
//      without confirmation, calls esp_ota_mark_app_invalid_rollback_and
//      _reboot(). This works WITHOUT bootloader ROLLBACK_ENABLE.
//
// ARMING (called from ota.h before ESP.restart() on flash success):
//   otaValidationArmRollback() writes the NVS flag. The NEW firmware will
//   read it on boot — that's how it knows to validate. No-op if flash
//   failed; we don't arm rollback for a slot we never wrote to.
// =============================================================================

// NVS namespace + key for the cross-boot pending-validation flag.
// Stored as a string (FIRMWARE_VERSION at the moment we armed) so that a
// stale flag from a prior boot can be detected & discarded if FIRMWARE_VERSION
// no longer matches.
#define OTA_VALIDATION_NVS_NS    "ota_v"
#define OTA_VALIDATION_NVS_KEY   "pending_ver"


// ── Module state ──────────────────────────────────────────────────────────────
static bool      _otaValidationPending    = false;  // post-OTA boot detected
static bool      _otaValidationCompleted  = false;  // mark_app_valid called this boot
static uint32_t  _otaValidationDeadlineMs = 0;      // millis() deadline
static bool      _otaValidationAnnounced  = false;  // published OTA_VALIDATING already


// ── otaValidationArmRollback ──────────────────────────────────────────────────
// Call from ota.h immediately after a successful flash, BEFORE ESP.restart().
// Writes the FIRMWARE_VERSION the new firmware will boot into to NVS. The
// new firmware reads this in otaValidationCheckBoot() — if the value matches
// its own FIRMWARE_VERSION, it knows the previous boot armed it for validation.
//
// `targetVersion` is the version string we're booting INTO (i.e. what the
// manifest reported). Passing it explicitly lets the new firmware verify that
// the boot transition actually happened (vs. a stale flag from a failed OTA).
inline void otaValidationArmRollback(const char* targetVersion) {
    Preferences prefs;
    if (!prefs.begin(OTA_VALIDATION_NVS_NS, false)) {
        LOG_W("OTAValid", "Failed to open NVS for arm");
        return;
    }
    prefs.putString(OTA_VALIDATION_NVS_KEY, targetVersion);
    prefs.end();
    LOG_I("OTAValid", "Armed rollback for incoming version '%s'", targetVersion);
}


// ── otaValidationCheckBoot ────────────────────────────────────────────────────
// Call from setup() AFTER Serial is initialised but BEFORE mqttBegin().
// Two detection paths (either is sufficient):
//   1. NVS pending-version flag matches our FIRMWARE_VERSION → previous
//      boot armed us. (Works on every bootloader version.)
//   2. esp_ota_get_state_partition() returns PENDING_VERIFY → bootloader
//      with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y armed us.
inline void otaValidationCheckBoot() {
    bool armed = false;

    // Path 1: NVS flag (universal)
    {
        Preferences prefs;
        if (prefsTryBegin(prefs, OTA_VALIDATION_NVS_NS, true)) {   // (v0.4.03) silent if missing
            String pendingVer = prefs.getString(OTA_VALIDATION_NVS_KEY, "");
            prefs.end();
            if (pendingVer.length() > 0) {
                if (pendingVer == FIRMWARE_VERSION) {
                    LOG_W("OTAValid", "NVS flag matches running version — post-OTA boot detected");
                    armed = true;
                } else {
                    // Stale flag from a previous OTA that didn't take. Clear it.
                    LOG_W("OTAValid", "Stale NVS flag '%s' (running '%s') — clearing",
                          pendingVer.c_str(), FIRMWARE_VERSION);
                    Preferences clearPrefs;
                    if (clearPrefs.begin(OTA_VALIDATION_NVS_NS, false)) {
                        clearPrefs.remove(OTA_VALIDATION_NVS_KEY);
                        clearPrefs.end();
                    }
                }
            }
        }
    }

    // Path 2: partition state (bootloader-assisted)
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running != nullptr) {
            esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
            esp_err_t r = esp_ota_get_state_partition(running, &state);
            if (r == ESP_OK) {
                LOG_I("OTAValid", "Boot partition '%s' state: %d", running->label, (int)state);
                if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                    armed = true;
                }
            } else {
                LOG_I("OTAValid", "Running on non-OTA partition (err %d)", r);
            }
        }
    }

    if (armed) {
        _otaValidationPending    = true;
        _otaValidationDeadlineMs = millis() + OTA_VALIDATION_DEADLINE_MS;
        LOG_W("OTAValid",
              "Post-OTA boot — must validate within %d ms or rollback fires",
              OTA_VALIDATION_DEADLINE_MS);
    }
}


// ── otaValidationOnMqttConnect ────────────────────────────────────────────────
// Call from the MQTT onConnect handler (or the boot announcement publish).
// Publishes OTA_VALIDATING exactly once per boot so Node-RED can show the
// device as "Validating" until the heartbeat that flips it to "Validated".
inline void otaValidationOnMqttConnect() {
    if (_otaValidationPending && !_otaValidationCompleted && !_otaValidationAnnounced) {
        _otaValidationAnnounced = true;
        char extra[128];
        uint32_t remaining = (_otaValidationDeadlineMs > millis())
                             ? (_otaValidationDeadlineMs - millis()) : 0;
        snprintf(extra, sizeof(extra),
            "\"deadline_ms\":%u,\"current_version\":\"" FIRMWARE_VERSION "\"",
            (unsigned)remaining);
        // mqttPublishStatus is declared in mqtt_client.h; this header is included
        // AFTER mqtt_client.h in the main sketch so the symbol is in scope.
        mqttPublishStatus(FwEvent::OTA_VALIDATING, extra);
    }
}


// ── otaValidationConfirmHealth ────────────────────────────────────────────────
// Call from the heartbeat publisher AFTER WiFi + MQTT broker are both
// confirmed reachable (i.e. heartbeat publish would have succeeded). Calls
// esp_ota_mark_app_valid_cancel_rollback() — bootloader will boot this slot
// from now on without entering PENDING_VERIFY.
//
// Idempotent: only acts on the first call per boot.
inline void otaValidationConfirmHealth() {
    if (!_otaValidationPending || _otaValidationCompleted) return;

    esp_err_t r = esp_ota_mark_app_valid_cancel_rollback();
    _otaValidationCompleted = true;
    _otaValidationPending   = false;

    // Clear the NVS arm flag (regardless of partition-state path success)
    // so subsequent boots don't re-trigger validation.
    Preferences prefs;
    if (prefs.begin(OTA_VALIDATION_NVS_NS, false)) {
        prefs.remove(OTA_VALIDATION_NVS_KEY);
        prefs.end();
    }

    if (r == ESP_OK) {
        LOG_I("OTAValid", "App validated — mark_app_valid_cancel_rollback OK");
        mqttPublishStatus(FwEvent::OTA_VALIDATED,
            "\"current_version\":\"" FIRMWARE_VERSION "\"");
    } else {
        // Without bootloader ROLLBACK_ENABLE, partition state was already VALID;
        // mark_app_valid is then a no-op returning a non-OK code. Still treat
        // as validated (NVS flag cleared above) — the validation gate's job
        // was to prove MQTT works, and it just did.
        LOG_I("OTAValid", "mark_app_valid_cancel_rollback returned %d (NVS cleared)", r);
        mqttPublishStatus(FwEvent::OTA_VALIDATED,
            "\"current_version\":\"" FIRMWARE_VERSION "\","
            "\"note\":\"nvs_only\"");
    }
}


// ── otaValidationTick ─────────────────────────────────────────────────────────
// Call from loop(). If the validation deadline has passed without
// otaValidationConfirmHealth() being called, force a rollback. The
// bootloader will boot the previous (known-good) OTA slot on next reset.
inline void otaValidationTick() {
    if (!_otaValidationPending || _otaValidationCompleted) return;
    if ((int32_t)(millis() - _otaValidationDeadlineMs) <= 0) return;

    LOG_E("OTAValid",
          "Validation deadline (%d ms) exceeded — rolling back",
          OTA_VALIDATION_DEADLINE_MS);

    if (esp_ota_check_rollback_is_possible()) {
        // This call sets the running partition to ABORTED, picks the previous
        // valid partition as the boot target, and reboots. Does not return.
        esp_ota_mark_app_invalid_rollback_and_reboot();
        // If we get here, rollback failed — fall through to plain restart.
        LOG_E("OTAValid", "mark_app_invalid_rollback_and_reboot returned — restarting plainly");
    } else {
        LOG_E("OTAValid", "No previous valid partition — cannot roll back; restarting plainly");
    }

    delay(200);
    ESP.restart();
}


// ── otaValidationIsPending ────────────────────────────────────────────────────
// Read-only accessor for callers (e.g. heartbeat code) that want to know
// whether the current boot is still in the validation window.
inline bool otaValidationIsPending() {
    return _otaValidationPending && !_otaValidationCompleted;
}
