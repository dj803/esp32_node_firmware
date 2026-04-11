#pragma once

// =============================================================================
// rfid.h  —  MFRC522v2 RFID tag reader module
//
// Detects ISO 14443A RFID cards via hardware IRQ and publishes the scanned
// UID to the MQTT telemetry topic as JSON.
//
// INCLUDE ORDER: Must come after mqtt_client.h in esp32_firmware.ino.
// mqttPublish() and mqttIsConnected() are static in mqtt_client.h and are
// visible here because Arduino compiles all headers as a single translation unit.
//
// WIRING (default ESP32 VSPI):
//
//  RC522 Pin │ ESP32 GPIO │ Signal  │ Function
//  ──────────┼────────────┼─────────┼──────────────────────────────────────────
//  SDA       │ GPIO  5    │ SS      │ SPI slave-select (active LOW, chip enable)
//  SCK       │ GPIO 18    │ SCK     │ SPI clock
//  MOSI      │ GPIO 23    │ MOSI    │ SPI data ESP32 → RC522
//  MISO      │ GPIO 19    │ MISO    │ SPI data RC522 → ESP32
//  IRQ       │ GPIO  4    │ IRQ     │ Interrupt output (active LOW, open-drain)
//  GND       │ GND        │ GND     │ Ground
//  RST       │ GPIO 22    │ NRSTPD  │ Reset / power-down (LOW = off, HIGH = run)
//  3.3 V     │ 3.3 V      │ VCC     │ Supply — do NOT connect to 5 V
//
//  SS, RST, and IRQ GPIOs are set via RFID_SS_PIN / RFID_RST_PIN / RFID_IRQ_PIN
//  in config.h and can be changed without editing this file.
//
// RST:
//   Driven LOW then HIGH in rfidInit() for a clean hardware reset before
//   PCD_Init(). Uses MFRC522DriverPinSimple (same class as SS).
//
// IRQ:
//   MFRC522 is armed with a REQA transceive command. When a card enters the
//   field and responds with ATQA, the MFRC522 asserts IRQ (active LOW).
//   An ISR sets _rfidIrqFired; rfidLoop() reacts and completes the read.
//   rfidArm() is called periodically (RFID_REARM_MS) because REQA times out
//   after ~25 ms if no card is present. Re-arming is cheap (6 register writes).
// =============================================================================

#ifdef RFID_ENABLED

#include <MFRC522v2.h>
#include <MFRC522DriverSPI.h>
#include <MFRC522DriverPinSimple.h>
#include <MFRC522Debug.h>
#include "config.h"
#include "led.h"

static MFRC522DriverPinSimple _rfidSsPin(RFID_SS_PIN);
static MFRC522DriverPinSimple _rfidRstPin(RFID_RST_PIN);
static MFRC522DriverSPI       _rfidDriver{_rfidSsPin};
static MFRC522                _rfid{_rfidDriver};
static bool                   _rfidReady        = false; // False if reader not detected at init
static String                 _rfidLastUid      = "";    // UID of last published card
static uint32_t               _rfidLastReadMs   = 0;     // millis() of last publish
static volatile bool          _rfidIrqFired     = false; // Set by ISR, cleared in rfidLoop()
static uint32_t               _rfidLastArmMs    = 0;     // millis() of last rfidArm() call
static uint32_t               _rfidQuietUntilMs = 0;     // Suppress re-arm until this time
                                                          // (post-read quiet period)


// ── rfidIrqHandler ────────────────────────────────────────────────────────────
// ISR — called on the FALLING edge of RFID_IRQ_PIN.
// The MFRC522 asserts IRQ (active LOW) when a card sends an ATQA in response
// to the REQA command issued by rfidArm(). Just sets the flag; all real work
// happens in rfidLoop() on the main task.
// IRAM_ATTR keeps the ISR in IRAM so it can run even when flash is busy.
void IRAM_ATTR rfidIrqHandler() {
    _rfidIrqFired = true;
}


// ── rfidArm ───────────────────────────────────────────────────────────────────
// Arms the MFRC522 to detect the next card by issuing a REQA transceive.
// When a card enters the field and sends an ATQA, the RxIRq bit fires and
// the IRQ pin goes LOW, triggering rfidIrqHandler().
// Must be called again after each read (or timeout) to resume detection.
static void rfidArm() {
    using R = MFRC522Constants::PCD_Register;
    using C = MFRC522Constants::PCD_Command;
    _rfidDriver.PCD_WriteRegister(R::CommandReg,    (byte)C::PCD_Idle);   // cancel any current command
    _rfidDriver.PCD_WriteRegister(R::ComIrqReg,     0x7F);                // clear all pending IRQ flags
    _rfidDriver.PCD_WriteRegister(R::FIFOLevelReg,  0x80);                // flush FIFO buffer
    _rfidDriver.PCD_WriteRegister(R::FIFODataReg,   0x26);                // REQA command byte (ISO 14443-3)
    _rfidDriver.PCD_WriteRegister(R::BitFramingReg, 0x07);                // 7-bit short frame
    _rfidDriver.PCD_WriteRegister(R::CommandReg,    (byte)C::PCD_Transceive); // transmit REQA, listen for ATQA
    _rfidDriver.PCD_WriteRegister(R::BitFramingReg, 0x87);                // StartSend=1 — actually sends
}


// ── rfidInit ──────────────────────────────────────────────────────────────────
// Initialises the MFRC522 over SPI with hardware reset and IRQ interrupt.
// Sets _rfidReady = false if the version register returns 0x00 or 0xFF,
// which indicates the reader is absent or the SPI bus is not connected.
// Call once from the OPERATIONAL block in setup(), after mqttBegin().
void rfidInit() {
    // Hardware reset: pulse RST LOW → HIGH to ensure a clean chip state.
    // This is more reliable than relying on power-on reset alone, especially
    // after a warm restart (OTA update, credential rotation, etc.).
    Serial.printf("[RFID] Hardware reset — RST=GPIO%d LOW...\n", RFID_RST_PIN);
    _rfidRstPin.init();
    _rfidRstPin.low();
    delay(10);
    _rfidRstPin.high();
    delay(50);   // Wait for oscillator startup after RST de-asserted
    Serial.println("[RFID] RST HIGH — chip starting");

    _rfid.PCD_Init();

    // PCD_GetVersion() reads the VersionReg and returns a PCD_Version enum value.
    // Version_Unknown (0xFF) or 0x00 means the SPI bus returned nothing — reader
    // absent or miswired. Guard rfidLoop() with _rfidReady so it does not churn
    // on SPI noise when no reader is attached.
    MFRC522::PCD_Version ver = _rfid.PCD_GetVersion();
    if ((byte)ver == 0x00 || ver == MFRC522::PCD_Version::Version_Unknown) {
        Serial.println("[RFID] WARNING: MFRC522 not detected — check wiring");
        Serial.printf( "[RFID]   SS=GPIO%d  RST=GPIO%d  IRQ=GPIO%d\n",
                       RFID_SS_PIN, RFID_RST_PIN, RFID_IRQ_PIN);
        Serial.printf( "[RFID]   SCK=GPIO18  MISO=GPIO19  MOSI=GPIO23\n");
        Serial.printf( "[RFID]   PCD_GetVersion() returned 0x%02X\n", (byte)ver);
        _rfidReady = false;
        return;
    }

    _rfidReady = true;
    Serial.printf("[RFID] MFRC522 v%02X ready  SS=GPIO%d  RST=GPIO%d  IRQ=GPIO%d\n",
                  (byte)ver, RFID_SS_PIN, RFID_RST_PIN, RFID_IRQ_PIN);

    // IRQ pin is open-drain active LOW from the MFRC522 — use internal pull-up.
    pinMode(RFID_IRQ_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RFID_IRQ_PIN), rfidIrqHandler, FALLING);
    Serial.printf("[RFID] Interrupt attached on GPIO%d (FALLING)\n", RFID_IRQ_PIN);

    // Route RxIRq to the IRQ pin:
    //   Bit 7 (IRqInv=1): invert IRQ pin → active LOW (matches open-drain wiring)
    //   Bit 5 (RxIEn=1):  fire when FIFO has received enough bytes (ATQA arrived)
    _rfidDriver.PCD_WriteRegister(MFRC522Constants::PCD_Register::ComIEnReg, 0xA0);
    Serial.println("[RFID] ComIEnReg: IRqInv=1, RxIEn=1");

    // Arm the first detection cycle
    rfidArm();
    _rfidLastArmMs = millis();
    Serial.printf("[RFID] Armed — re-arming every %lu ms, debounce %lu ms per card\n",
                  (unsigned long)RFID_REARM_MS, (unsigned long)RFID_DEBOUNCE_MS);
    Serial.println("[RFID] Waiting for tag...");
}


// ── rfidLoop ──────────────────────────────────────────────────────────────────
// Called every loop() iteration. Non-blocking — returns immediately if idle.
//
// Behaviour:
//   - Periodically re-arms the MFRC522 (REQA times out in ~25 ms, so we
//     re-issue it every RFID_REARM_MS to keep detection active).
//   - When _rfidIrqFired is set (card sent ATQA), completes the read via
//     PICC_ReadCardSerial() and publishes the UID to MQTT telemetry.
//   - Per-card debounce prevents the same UID being re-published within
//     RFID_DEBOUNCE_MS; different cards are always processed immediately.
void rfidLoop() {
    if (!_rfidReady) return;   // Reader not detected at init — skip all SPI work

    uint32_t now = millis();

    // Post-read quiet period: discard any spurious IRQ triggered by a card still in
    // the field after PICC_HaltA() and hold off re-arming until the window expires.
    // Eliminates the "serial read failed — card removed too quickly?" noise log.
    if (now < _rfidQuietUntilMs) {
        _rfidIrqFired = false;
        return;
    }

    // Re-arm periodically: REQA times out if no card responds within ~25 ms.
    // Re-arming is 6 fast register writes — negligible CPU cost.
    if (now - _rfidLastArmMs >= RFID_REARM_MS) {
        _rfidLastArmMs  = now;
        _rfidIrqFired   = false;
        rfidArm();
        return;
    }

    if (!_rfidIrqFired) return;   // No IRQ since last arm — nothing to do
    _rfidIrqFired = false;

    // IRQ fired — card sent ATQA. Complete anti-collision + SELECT.
    Serial.println("[RFID] IRQ fired — card in field, reading serial...");
    if (!_rfid.PICC_ReadCardSerial()) {
        Serial.println("[RFID] Serial read failed — card removed too quickly?");
        _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;
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

    // Per-card debounce: suppress re-reads of the same card within the window.
    // Two different cards can always be scanned back-to-back without delay.
    if (uid == _rfidLastUid && (now - _rfidLastReadMs) < RFID_DEBOUNCE_MS) {
        Serial.printf("[RFID] Debounce — same card (%s) within %lu ms window, skipping\n",
                      uid.c_str(), (unsigned long)RFID_DEBOUNCE_MS);
        _rfid.PICC_HaltA();
        _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;
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
        // MQTT not connected — log the dropped publish so it's visible in the serial monitor
        Serial.printf("[RFID] MQTT not connected — publish dropped (uid=%s)\n", uid.c_str());
    } else {
        mqttPublish("telemetry", payload);
    }

    _rfid.PICC_HaltA();
    _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;   // suppress spurious post-halt IRQ
    Serial.println("[RFID] Waiting for next tag...");
}

#endif // RFID_ENABLED
