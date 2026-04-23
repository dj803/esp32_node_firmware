#pragma once

// =============================================================================
// lib_api_assert.h  —  compile-time guards on third-party library APIs
//
// PURPOSE: Catch silent ABI / API drift in pinned libraries at build time
// rather than at runtime (or worse, at field deploy time). The v0.2.10
// NimBLE 1→2 migration was a runtime surprise that could have been a
// compile-time failure with a guard like the ones below; this header is the
// generalised pattern.
//
// Each block does ONE of:
//   1. Test a library version macro if the lib exposes one. Most direct.
//   2. static_assert on the type of a member-function pointer. Catches
//      argument-list / return-type drift even when no version macro exists.
//
// If a static_assert below fires, DO NOT silence it without thinking:
//   - Confirm the lib's git ref in platformio.ini (intentional bump?)
//   - Audit every callsite of the affected symbol for behaviour change
//   - Update the assertion to match the new signature
//   - Note the bump + audit in FIXES_LOG.txt
//
// Included from src/main.cpp exactly once. Pure compile-time check; no
// runtime cost, no symbol emission, no link-time impact.
// =============================================================================

#include <type_traits>
#include <AsyncMqttClient.hpp>
#include <ESP32OTAPull.h>
#ifdef BLE_ENABLED
#include <NimBLEDevice.h>
#endif
#ifdef RFID_ENABLED
#include <MFRC522v2.h>
#endif

// ── AsyncMqttClient (marvinroger@3d93fc7) ────────────────────────────────────
// The setClientId / setCredentials / setServer / setWill family take
// `const char*` and STORE THE POINTER without copying. Ensuring their
// signatures are stable is half the v0.1.7 / v0.3.30 / v0.3.31 string-
// lifetime safety story (the other half is in docs/STRING_LIFETIME.md).
static_assert(
    std::is_same<decltype(&AsyncMqttClient::setClientId),
                 AsyncMqttClient&(AsyncMqttClient::*)(const char*)>::value,
    "AsyncMqttClient::setClientId signature drifted — see docs/STRING_LIFETIME.md");

static_assert(
    std::is_same<decltype(&AsyncMqttClient::setKeepAlive),
                 AsyncMqttClient&(AsyncMqttClient::*)(uint16_t)>::value,
    "AsyncMqttClient::setKeepAlive signature drifted");

static_assert(
    std::is_same<decltype(&AsyncMqttClient::setCredentials),
                 AsyncMqttClient&(AsyncMqttClient::*)(const char*, const char*)>::value,
    "AsyncMqttClient::setCredentials signature drifted — see docs/STRING_LIFETIME.md");

static_assert(
    std::is_same<decltype(&AsyncMqttClient::setWill),
                 AsyncMqttClient&(AsyncMqttClient::*)(const char*, uint8_t, bool, const char*, size_t)>::value,
    "AsyncMqttClient::setWill signature drifted — see docs/STRING_LIFETIME.md");


// ── NimBLE-Arduino (h2zero, ^2.0.0) ──────────────────────────────────────────
// Already has a runtime version-macro guard at the top of include/ble.h.
// Repeated here so the check is centralised with the other libs.
#ifdef BLE_ENABLED
#  if !defined(NIMBLE_CPP_VERSION_MAJOR) || NIMBLE_CPP_VERSION_MAJOR < 2
#    error "NimBLE-Arduino 2.x required (see lib_deps in platformio.ini)"
#  endif
#endif


// ── ESP32-OTA-Pull (mikalhart@v1.0.2) ────────────────────────────────────────
// Pass 1 of the OTA path still calls into ESP32-OTA-Pull; Pass 2 has been
// replaced by esp_https_ota in v0.3.35 but the manifest fetch + version
// extraction is still the library's job.
static_assert(
    std::is_same<decltype(&ESP32OTAPull::CheckForOTAUpdate),
                 int(ESP32OTAPull::*)(const char*, const char*, ESP32OTAPull::ActionType)>::value,
    "ESP32OTAPull::CheckForOTAUpdate signature drifted — see ota.h");


// ── MFRC522v2 (OSSLibraries fork) ─────────────────────────────────────────────
// We use PCD_Init + PICC_IsNewCardPresent + PICC_ReadCardSerial. Drift in
// any of these would silently break RFID reads.
#ifdef RFID_ENABLED
static_assert(
    std::is_same<decltype(&MFRC522::PCD_Init),
                 bool(MFRC522::*)()>::value,
    "MFRC522::PCD_Init signature drifted");
#endif
