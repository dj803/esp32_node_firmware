#pragma once

// =============================================================================
// rfid.h  —  MFRC522v2 RFID tag reader module
//
// Polls for ISO 14443A RFID cards and publishes the scanned UID to the
// MQTT telemetry topic as JSON.
//
// INCLUDE ORDER: Must come after mqtt_client.h in esp32_firmware.ino.
// mqttPublish() and mqttIsConnected() are static in mqtt_client.h and are
// visible here because Arduino compiles all headers as a single translation unit.
//
// WIRING (default ESP32 VSPI):
//   SS   → GPIO RFID_SS_PIN  (default 5, set in config.h)
//   SCK  → GPIO 18
//   MISO → GPIO 19
//   MOSI → GPIO 23
//   3.3V and GND as normal — do NOT connect to 5V
// =============================================================================

#ifdef RFID_ENABLED

#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include "config.h"
#include "led.h"

static MFRC522DriverPinSimple _rfidSsPin(RFID_SS_PIN);
static MFRC522DriverSPI       _rfidDriver{_rfidSsPin};
static MFRC522                _rfid{_rfidDriver};
static bool                   _rfidReady      = false;   // False if reader not detected at init
static String                 _rfidLastUid    = "";      // UID of last published card
static uint32_t               _rfidLastReadMs = 0;       // millis() of last publish


// ── rfidInit ──────────────────────────────────────────────────────────────────
// Initialises the MFRC522 over SPI and verifies it is responding.
// Sets _rfidReady = false if the version register returns 0x00 or 0xFF,
// which indicates the reader is absent or the SPI bus is not connected.
// rfidLoop() checks _rfidReady and skips all polling when false.
// Call once from the OPERATIONAL block in setup(), after mqttBegin().
void rfidInit() {
    _rfid.PCD_Init();

    // PCD_GetVersion() reads the VersionReg and returns a PCD_Version enum value.
    // Version_Unknown (0xFF) or 0x00 means the SPI bus returned nothing — reader absent or miswired.
    MFRC522::PCD_Version ver = _rfid.PCD_GetVersion();
    if ((byte)ver == 0x00 || ver == MFRC522::PCD_Version::Version_Unknown) {
        Serial.println("[RFID] WARNING: MFRC522 not detected — check SPI wiring");
        Serial.printf( "[RFID]   SS=GPIO%d  SCK=GPIO18  MISO=GPIO19  MOSI=GPIO23\n",
                       RFID_SS_PIN);
        Serial.printf( "[RFID]   PCD_GetVersion() returned 0x%02X\n", (byte)ver);
        _rfidReady = false;
    } else {
        _rfidReady = true;
        Serial.printf("[RFID] MFRC522 v%02X ready (SS=GPIO%d)\n", (byte)ver, RFID_SS_PIN);
        Serial.printf("[RFID] Debounce window: %lu ms per card\n",
                      (unsigned long)RFID_DEBOUNCE_MS);
        Serial.println("[RFID] Waiting for tag...");
    }
}


// ── rfidLoop ──────────────────────────────────────────────────────────────────
// Polls for a new card and publishes its UID to the MQTT telemetry topic.
// Non-blocking — returns immediately if no card is present.
// Call every loop() iteration; it is cheap when idle.
void rfidLoop() {
    if (!_rfidReady) return;   // Reader not detected at init — skip SPI polling

    if (!_rfid.PICC_IsNewCardPresent()) return;

    Serial.println("[RFID] Card detected — reading serial...");
    if (!_rfid.PICC_ReadCardSerial()) {
        Serial.println("[RFID] Serial read failed — card removed too quickly?");
        return;
    }

    // Build colon-separated uppercase UID hex string (e.g. "AB:CD:EF:01")
    String uid = "";
    for (byte i = 0; i < _rfid.uid.size; i++) {
        if (i > 0) uid += ":";
        if (_rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(_rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    // Per-card debounce: suppress repeated reads of the same card within the window.
    // Two *different* cards can always be scanned back-to-back without delay.
    uint32_t now = millis();
    if (uid == _rfidLastUid && (now - _rfidLastReadMs) < RFID_DEBOUNCE_MS) {
        Serial.printf("[RFID] Debounce — same card (%s) within %lu ms window, skipping\n",
                      uid.c_str(), (unsigned long)RFID_DEBOUNCE_MS);
        _rfid.PICC_HaltA();
        return;
    }
    _rfidLastUid    = uid;
    _rfidLastReadMs = now;

    // Decode card family from SAK byte (e.g. "MIFARE 1KB", "NTAG21x", "ISO-14443-4")
    // PICC_GetType is on the MFRC522 class; PICC_GetTypeName is on MFRC522Debug and
    // returns __FlashStringHelper* — wrap in String() to use as a regular string.
    MFRC522::PICC_Type piccType = _rfid.PICC_GetType(_rfid.uid.sak);
    String typeName = String(MFRC522Debug::PICC_GetTypeName(piccType));

    Serial.printf("[RFID] Tag scanned: %s  type=%s  size=%d bytes  uptime=%lus\n",
                  uid.c_str(), typeName.c_str(), _rfid.uid.size, millis() / 1000);

    ledFlashLocate();   // Brief LED flash as visual scan confirmation

    String payload = "{\"uid\":\""       + uid                    + "\","
                      "\"uid_size\":"    + String(_rfid.uid.size) + ","
                      "\"card_type\":\"" + typeName               + "\"}";

    if (!mqttIsConnected()) {
        // Publish dropped — log it so the missing scan is visible in the serial monitor
        Serial.printf("[RFID] MQTT not connected — publish dropped (uid=%s)\n", uid.c_str());
    } else {
        Serial.printf("[RFID] Publishing to telemetry: %s\n", payload.c_str());
        mqttPublish("telemetry", payload);
    }

    _rfid.PICC_HaltA();   // Halt the card so it does not re-trigger on the next poll
    Serial.println("[RFID] Waiting for next tag...");
}

#endif // RFID_ENABLED
