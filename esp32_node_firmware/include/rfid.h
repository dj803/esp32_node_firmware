#pragma once

// =============================================================================
// rfid.h  —  MFRC522v2 RFID tag reader module
//
// Detects ISO 14443A RFID cards via hardware IRQ. Each scan checks the UID
// against a NVS-persisted whitelist and publishes the result (including an
// "authorized" field) to .../telemetry/rfid. The WS2812B strip shows green
// for authorised cards and red for unknown ones.
//
// INCLUDE ORDER: Must come after mqtt_client.h AND ws2812.h in esp32_firmware.ino.
// mqttPublish(), mqttIsConnected(), ws2812PostEvent() are all static in their
// respective headers and visible here because Arduino compiles all headers as
// a single translation unit.
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
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"
#include "led.h"
#include "nvs_utils.h"   // NvsPutIfChanged — compare-before-write wrappers
#include "rfid_types.h"  // RfidMode, RfidProgramRequest, RfidReadRequest, guards

static MFRC522DriverPinSimple _rfidSsPin(RFID_SS_PIN);
static MFRC522DriverPinSimple _rfidRstPin(RFID_RST_PIN);
static MFRC522DriverSPI       _rfidDriver{_rfidSsPin};
static MFRC522                _rfid{_rfidDriver};
// _rfidReady is module-internal — never read from outside rfid.h.
// Use rfidIsReady() for any external query.
static bool                   _rfidReady        = false; // False if reader not detected at init
static String                 _rfidLastUid      = "";    // UID of last published card (colon fmt)
static uint32_t               _rfidLastReadMs   = 0;     // millis() of last publish
static volatile bool          _rfidIrqFired     = false; // Set by ISR, cleared in rfidLoop()
static uint32_t               _rfidLastArmMs    = 0;     // millis() of last rfidArm() call
static uint32_t               _rfidQuietUntilMs = 0;     // Suppress re-arm until this time
                                                          // (post-read quiet period)

// ── Whitelist storage ─────────────────────────────────────────────────────────
// UIDs stored in compact uppercase hex, no separators (e.g. "AABBCCDD").
// All external UIDs (colon-separated or mixed case) are normalised before use.
static char    _rfidWhitelist[RFID_MAX_WHITELIST][RFID_UID_STR_LEN] = {};
static uint8_t _rfidWhitelistCount = 0;

// (v0.3.36) Spinlock guarding all access to _rfidWhitelist[] +
// _rfidWhitelistCount. The mutators (rfidWhitelistAdd/Remove) are called
// from MQTT command handlers that run on the async_tcp task; the reader
// (rfidWhitelistContains) is called from rfidLoop() on the main task when
// a card is read. Without this, a card-present callback racing a
// cmd/rfid/whitelist add could read past _rfidWhitelistCount or read a
// half-overwritten UID slot.
//
// Critical sections are short (≤8 strncmp's of a 9-byte string) and
// contain no I/O. NVS writes from _rfidWhitelistSave() snapshot the
// whitelist UNDER the mux first, then write the snapshot from outside
// the critical section so we don't hold the mux during flash I/O.
static portMUX_TYPE _rfidWhitelistMux = portMUX_INITIALIZER_UNLOCKED;

// ── Playground state (v0.3.17) ───────────────────────────────────────────────
// The module spends virtually all its time in RfidMode::IDLE. A single
// cmd/rfid/program or cmd/rfid/read_block transitions it into a transient
// armed state; the next IRQ is routed through the write / read path instead
// of the normal telemetry publish, and the mode returns to IDLE on
// completion / timeout / cancellation.
static RfidMode            _rfidMode          = RfidMode::IDLE;
static RfidProgramRequest  _rfidProgramReq    = {};
static RfidReadRequest     _rfidReadReq       = {};
static uint32_t            _rfidModeDeadlineMs = 0;   // 0 when idle


// ── rfidNormaliseUid ──────────────────────────────────────────────────────────
// Strip colons and hyphens, convert to uppercase in-place.
// Applied to every UID before whitelist lookup, add, or remove.
static void rfidNormaliseUid(char* uid) {
    char buf[RFID_UID_STR_LEN] = {};
    int  j = 0;
    for (int i = 0; uid[i] && j < RFID_UID_STR_LEN - 1; i++) {
        if (uid[i] != ':' && uid[i] != '-')
            buf[j++] = (char)toupper((unsigned char)uid[i]);
    }
    memcpy(uid, buf, RFID_UID_STR_LEN);
}


// ── Whitelist NVS helpers ─────────────────────────────────────────────────────

static void _rfidWhitelistSave() {
    // (v0.3.36) Snapshot under the mux, then write the snapshot to NVS
    // from outside the critical section. Holding the mux across NVS flash
    // I/O would block card reads for tens of ms.
    char    snapshot[RFID_MAX_WHITELIST][RFID_UID_STR_LEN];
    uint8_t snapshotCount;
    portENTER_CRITICAL(&_rfidWhitelistMux);
    snapshotCount = _rfidWhitelistCount;
    memcpy(snapshot, _rfidWhitelist, sizeof(snapshot));
    portEXIT_CRITICAL(&_rfidWhitelistMux);

    Preferences p;
    p.begin(RFID_NVS_NAMESPACE, false);
    // NvsPutIfChanged skips writes when the slot already holds the same UID.
    // Whitelist persists across boots, so repeat calls with an unchanged list
    // (e.g. after a retained MQTT cmd/rfid/whitelist replay on reconnect)
    // should not re-write every slot.
    NvsPutIfChanged(p, "count", (uint8_t)snapshotCount);
    for (uint8_t i = 0; i < snapshotCount; i++) {
        char key[4];
        snprintf(key, sizeof(key), "u%d", i);
        NvsPutIfChanged(p, key, snapshot[i]);
    }
    p.end();
}

void rfidWhitelistLoad() {
    Preferences p;
    p.begin(RFID_NVS_NAMESPACE, true);
    _rfidWhitelistCount = p.getUChar("count", 0);
    if (_rfidWhitelistCount > RFID_MAX_WHITELIST)
        _rfidWhitelistCount = 0;
    for (uint8_t i = 0; i < _rfidWhitelistCount; i++) {
        char key[4];
        snprintf(key, sizeof(key), "u%d", i);
        String val = p.getString(key, "");
        strlcpy(_rfidWhitelist[i], val.c_str(), RFID_UID_STR_LEN);
    }
    p.end();
    Serial.printf("[RFID] Whitelist loaded: %d UID(s)\n", _rfidWhitelistCount);
}


// ── Whitelist public API ──────────────────────────────────────────────────────

// (v0.3.36) All three functions take _rfidWhitelistMux to prevent the
// reader (rfidLoop on main task, calling Contains via the published-card
// path) from racing the mutators (MQTT cmd/rfid/whitelist on async_tcp).
// Save is called OUTSIDE the critical section by Add/Remove because
// _rfidWhitelistSave snapshots the array under its own mux acquire and
// then performs flash I/O without holding the mux.

static bool rfidWhitelistContains(const char* uid) {
    bool found = false;
    portENTER_CRITICAL(&_rfidWhitelistMux);
    for (uint8_t i = 0; i < _rfidWhitelistCount; i++) {
        if (strncmp(_rfidWhitelist[i], uid, RFID_UID_STR_LEN) == 0) { found = true; break; }
    }
    portEXIT_CRITICAL(&_rfidWhitelistMux);
    return found;
}

bool rfidWhitelistAdd(const char* uid) {
    bool needSave = false;
    bool ok       = false;
    portENTER_CRITICAL(&_rfidWhitelistMux);
    // Inline duplicate-check to avoid recursive mux acquire from Contains.
    bool duplicate = false;
    for (uint8_t i = 0; i < _rfidWhitelistCount; i++) {
        if (strncmp(_rfidWhitelist[i], uid, RFID_UID_STR_LEN) == 0) { duplicate = true; break; }
    }
    if (duplicate) {
        ok = true;   // already present — idempotent
    } else if (_rfidWhitelistCount < RFID_MAX_WHITELIST) {
        strlcpy(_rfidWhitelist[_rfidWhitelistCount++], uid, RFID_UID_STR_LEN);
        ok       = true;
        needSave = true;
    }
    portEXIT_CRITICAL(&_rfidWhitelistMux);
    if (needSave) _rfidWhitelistSave();
    return ok;
}

bool rfidWhitelistRemove(const char* uid) {
    bool needSave = false;
    bool ok       = false;
    portENTER_CRITICAL(&_rfidWhitelistMux);
    for (uint8_t i = 0; i < _rfidWhitelistCount; i++) {
        if (strncmp(_rfidWhitelist[i], uid, RFID_UID_STR_LEN) == 0) {
            // Compact array: overwrite with last entry
            if (i < _rfidWhitelistCount - 1)
                memcpy(_rfidWhitelist[i], _rfidWhitelist[_rfidWhitelistCount - 1],
                       RFID_UID_STR_LEN);
            memset(_rfidWhitelist[--_rfidWhitelistCount], 0, RFID_UID_STR_LEN);
            ok       = true;
            needSave = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_rfidWhitelistMux);
    if (needSave) _rfidWhitelistSave();
    return ok;
}

void rfidWhitelistClear() {
    memset(_rfidWhitelist, 0, sizeof(_rfidWhitelist));
    _rfidWhitelistCount = 0;
    _rfidWhitelistSave();
}

void rfidWhitelistList(char out[][RFID_UID_STR_LEN], uint8_t& count) {
    count = _rfidWhitelistCount;
    for (uint8_t i = 0; i < _rfidWhitelistCount; i++)
        memcpy(out[i], _rfidWhitelist[i], RFID_UID_STR_LEN);
}


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

    // Load UID whitelist from NVS
    rfidWhitelistLoad();
    Serial.println("[RFID] Waiting for tag...");
}


// ── Profile mapper ────────────────────────────────────────────────────────────
// Translates the MFRC522v2 PICC_Type enum (decoded from SAK on SELECT) into the
// wire profile string used by the RFID playground. Published alongside the
// existing `card_type` field in telemetry/rfid and echoed in response JSON.
static const char* rfidPiccToProfile(MFRC522::PICC_Type t) {
    using PT = MFRC522Constants::PICC_Type;
    switch (t) {
        case PT::PICC_TYPE_MIFARE_MINI:
        case PT::PICC_TYPE_MIFARE_1K:    return RFID_PROFILE_MIFARE_CLASSIC_1K;
        case PT::PICC_TYPE_MIFARE_4K:    return RFID_PROFILE_MIFARE_CLASSIC_4K;
        case PT::PICC_TYPE_MIFARE_UL:    return RFID_PROFILE_NTAG21X;   // NTAG21x shares the Ultralight SAK
        default:                         return RFID_PROFILE_UNKNOWN;
    }
}


// ── Public arm / cancel API ───────────────────────────────────────────────────
// Callable from mqtt_client.h's cmd/rfid/* handlers. Overwrites any pending
// request — the playground is single-user, and stacking arms is intentionally
// unsupported (each operator press of Write/Read replaces the previous one).
void rfidArmProgram(const RfidProgramRequest& req) {
    _rfidProgramReq    = req;
    _rfidMode          = RfidMode::PROGRAMMING;
    uint32_t to = req.timeout_ms ? req.timeout_ms : RFID_PROGRAM_TIMEOUT_MS;
    _rfidModeDeadlineMs = millis() + to;
    Serial.printf("[RFID] ARMED PROGRAM  profile=%s  blocks=%u  timeout=%ums  req=%s\n",
                  req.profile, (unsigned)req.write_count, (unsigned)to, req.request_id);
}

void rfidArmRead(const RfidReadRequest& req) {
    _rfidReadReq       = req;
    _rfidMode          = RfidMode::READING_BLOCK;
    uint32_t to = req.timeout_ms ? req.timeout_ms : RFID_PROGRAM_TIMEOUT_MS;
    _rfidModeDeadlineMs = millis() + to;
    Serial.printf("[RFID] ARMED READ  profile=%s  block=%u  timeout=%ums  req=%s\n",
                  req.profile, (unsigned)req.block, (unsigned)to, req.request_id);
}

void rfidCancelPending() {
    if (_rfidMode == RfidMode::IDLE) return;
    Serial.println("[RFID] Pending request cancelled");
    _rfidMode           = RfidMode::IDLE;
    _rfidModeDeadlineMs = 0;
}

RfidMode rfidGetMode() { return _rfidMode; }


// ── Response publishers ───────────────────────────────────────────────────────
// Publish to .../response with the schema agreed with Node-RED — see
// docs/rfid_tag_profiles.md. `status` is one of:
//   ok | auth_failed | write_failed | trailer_guard | timeout | cancelled
static void _rfidPublishProgramResponse(const char* status,
                                        const char* uid,
                                        const uint8_t* blocksWritten, uint8_t n) {
    JsonDocument doc;
    doc["event"]      = "rfid_program";
    doc["request_id"] = _rfidProgramReq.request_id;
    doc["uid"]        = uid ? uid : "";
    doc["profile"]    = _rfidProgramReq.profile;
    doc["status"]     = status;
    JsonArray arr = doc["blocks_written"].to<JsonArray>();
    for (uint8_t i = 0; i < n; i++) arr.add(blocksWritten[i]);
    String payload; serializeJson(doc, payload);
    mqttPublish("response", payload, 1, false);
}

static void _rfidPublishReadResponse(const char* status,
                                     const char* uid,
                                     const uint8_t* data, uint8_t len) {
    JsonDocument doc;
    doc["event"]      = "rfid_read_block";
    doc["request_id"] = _rfidReadReq.request_id;
    doc["uid"]        = uid ? uid : "";
    doc["profile"]    = _rfidReadReq.profile;
    doc["block"]      = _rfidReadReq.block;
    doc["status"]     = status;
    if (data && len > 0) {
        char hex[2 * RFID_BLOCK_SIZE + 1];
        rfidHexEncode(data, len, hex);
        doc["data_hex"] = hex;
    }
    String payload; serializeJson(doc, payload);
    mqttPublish("response", payload, 1, false);
}


// ── Write path ────────────────────────────────────────────────────────────────
// Authenticate each block with its Key A (Key A only — no Key B path in v0.3.17)
// and write 16 B via MIFARE_Write. Sector-trailer guard is enforced here: any
// write targeting a trailer block aborts the whole batch with status
// "trailer_guard" regardless of how many blocks preceded it.
//
// NTAG21x / Ultralight path uses MIFARE_Ultralight_Write (4-byte pages) and
// skips authentication — page 0..3 are factory-read-only, the library returns
// STATUS_ERROR which we surface as write_failed.
static void _rfidExecuteProgram(const String& uidColon) {
    const RfidProgramRequest& req = _rfidProgramReq;

    // Sector-trailer guard — refuse up front before touching the reader.
    for (uint8_t i = 0; i < req.write_count; i++) {
        if (rfidIsSectorTrailer(req.profile, req.writes[i].block)) {
            Serial.printf("[RFID] trailer_guard: refusing block %u on %s\n",
                          (unsigned)req.writes[i].block, req.profile);
            _rfidPublishProgramResponse("trailer_guard", uidColon.c_str(), nullptr, 0);
            return;
        }
    }

    const bool isMifareClassic =
        strcmp(req.profile, RFID_PROFILE_MIFARE_CLASSIC_1K) == 0 ||
        strcmp(req.profile, RFID_PROFILE_MIFARE_CLASSIC_4K) == 0;
    const bool isUltralight =
        strcmp(req.profile, RFID_PROFILE_NTAG21X) == 0 ||
        strcmp(req.profile, RFID_PROFILE_MIFARE_UL) == 0;

    if (!isMifareClassic && !isUltralight) {
        Serial.printf("[RFID] unsupported profile for write: %s\n", req.profile);
        _rfidPublishProgramResponse("write_failed", uidColon.c_str(), nullptr, 0);
        return;
    }

    uint8_t written[RFID_MAX_WRITE_BLOCKS];
    uint8_t nWritten = 0;

    for (uint8_t i = 0; i < req.write_count; i++) {
        const RfidWriteBlock& w = req.writes[i];

        if (isMifareClassic) {
            // Authenticate the target sector with Key A (default 0xFFx6 unless
            // the request supplied one).
            MFRC522::MIFARE_Key key;
            if (w.has_key_a) memcpy(key.keyByte, w.keyA, 6);
            else             memset(key.keyByte, 0xFF, 6);

            auto st = _rfid.PCD_Authenticate(
                MFRC522Constants::PICC_Command::PICC_CMD_MF_AUTH_KEY_A,
                (byte)w.block, &key, &_rfid.uid);
            if (st != MFRC522Constants::StatusCode::STATUS_OK) {
                Serial.printf("[RFID] auth_failed at block %u (status=%d)\n",
                              (unsigned)w.block, (int)st);
                _rfidPublishProgramResponse("auth_failed", uidColon.c_str(),
                                            written, nWritten);
                _rfid.PCD_StopCrypto1();
                return;
            }

            st = _rfid.MIFARE_Write((byte)w.block, (byte*)w.data, RFID_BLOCK_SIZE);
            if (st != MFRC522Constants::StatusCode::STATUS_OK) {
                Serial.printf("[RFID] write_failed at block %u (status=%d)\n",
                              (unsigned)w.block, (int)st);
                _rfidPublishProgramResponse("write_failed", uidColon.c_str(),
                                            written, nWritten);
                _rfid.PCD_StopCrypto1();
                return;
            }

            written[nWritten++] = (uint8_t)w.block;
        } else {
            // Ultralight / NTAG21x — 4-byte page write, no auth in the
            // unprotected region. The library's page write expects a 4-byte
            // buffer; we pass the first 4 bytes of data[].
            auto st = _rfid.MIFARE_Ultralight_Write((byte)w.block,
                                                    (byte*)w.data, 4);
            if (st != MFRC522Constants::StatusCode::STATUS_OK) {
                Serial.printf("[RFID] ul write_failed at page %u (status=%d)\n",
                              (unsigned)w.block, (int)st);
                _rfidPublishProgramResponse("write_failed", uidColon.c_str(),
                                            written, nWritten);
                return;
            }
            written[nWritten++] = (uint8_t)w.block;
        }
    }

    if (isMifareClassic) _rfid.PCD_StopCrypto1();

    Serial.printf("[RFID] program ok — %u block(s) written to %s\n",
                  (unsigned)nWritten, uidColon.c_str());
    _rfidPublishProgramResponse("ok", uidColon.c_str(), written, nWritten);
}


// ── Read path ─────────────────────────────────────────────────────────────────
static void _rfidExecuteRead(const String& uidColon) {
    const RfidReadRequest& req = _rfidReadReq;

    const bool isMifareClassic =
        strcmp(req.profile, RFID_PROFILE_MIFARE_CLASSIC_1K) == 0 ||
        strcmp(req.profile, RFID_PROFILE_MIFARE_CLASSIC_4K) == 0;

    // MIFARE_Read returns 16 data bytes + 2 CRC bytes → library needs an
    // 18-byte buffer. We forward the first `page_size` bytes.
    uint8_t buf[18] = {};
    byte   size    = sizeof(buf);

    if (isMifareClassic) {
        MFRC522::MIFARE_Key key;
        if (req.has_key_a) memcpy(key.keyByte, req.keyA, 6);
        else               memset(key.keyByte, 0xFF, 6);

        auto st = _rfid.PCD_Authenticate(
            MFRC522Constants::PICC_Command::PICC_CMD_MF_AUTH_KEY_A,
            (byte)req.block, &key, &_rfid.uid);
        if (st != MFRC522Constants::StatusCode::STATUS_OK) {
            Serial.printf("[RFID] read auth_failed at block %u (status=%d)\n",
                          (unsigned)req.block, (int)st);
            _rfidPublishReadResponse("auth_failed", uidColon.c_str(), nullptr, 0);
            _rfid.PCD_StopCrypto1();
            return;
        }
    }

    auto st = _rfid.MIFARE_Read((byte)req.block, buf, &size);
    if (isMifareClassic) _rfid.PCD_StopCrypto1();
    if (st != MFRC522Constants::StatusCode::STATUS_OK) {
        Serial.printf("[RFID] read failed at block %u (status=%d)\n",
                      (unsigned)req.block, (int)st);
        _rfidPublishReadResponse("write_failed", uidColon.c_str(), nullptr, 0);
        return;
    }

    uint8_t outLen = rfidProfileBlockSize(req.profile);
    Serial.printf("[RFID] read ok — %u byte(s) from block %u of %s\n",
                  (unsigned)outLen, (unsigned)req.block, uidColon.c_str());
    _rfidPublishReadResponse("ok", uidColon.c_str(), buf, outLen);
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
// Returns true when the MFRC522 was detected and initialised successfully.
inline bool rfidIsReady() { return _rfidReady; }

void rfidLoop() {
    if (!_rfidReady) return;   // Reader not detected at init — skip all SPI work

    uint32_t now = millis();

    // Playground state machine — fire a timeout response if the operator armed
    // a program/read and then walked away. Runs before the quiet-period check so
    // timeouts still fire during the post-read suppression window.
    if (_rfidMode != RfidMode::IDLE && _rfidModeDeadlineMs &&
        (int32_t)(now - _rfidModeDeadlineMs) >= 0) {
        Serial.println("[RFID] armed request timed out");
        if (_rfidMode == RfidMode::PROGRAMMING) {
            _rfidPublishProgramResponse("timeout", "", nullptr, 0);
        } else {
            _rfidPublishReadResponse("timeout", "", nullptr, 0);
        }
        _rfidMode           = RfidMode::IDLE;
        _rfidModeDeadlineMs = 0;
    }

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
    const char* profile = rfidPiccToProfile(piccType);

    // ── Armed-request path ───────────────────────────────────────────────────
    // If Node-RED armed a program or read, this card is the target. Auto-
    // publish on telemetry/rfid is paused so the operator doesn't get a
    // spurious "unauthorised card" buzz during programming. The response on
    // .../response is the only observable outcome.
    if (_rfidMode == RfidMode::PROGRAMMING) {
        _rfidMode           = RfidMode::IDLE;
        _rfidModeDeadlineMs = 0;
        _rfidExecuteProgram(uid);
        _rfid.PICC_HaltA();
        _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;
        Serial.println("[RFID] Waiting for next tag...");
        return;
    }
    if (_rfidMode == RfidMode::READING_BLOCK) {
        _rfidMode           = RfidMode::IDLE;
        _rfidModeDeadlineMs = 0;
        _rfidExecuteRead(uid);
        _rfid.PICC_HaltA();
        _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;
        Serial.println("[RFID] Waiting for next tag...");
        return;
    }

    // Whitelist check — use compact form (strip colons from colon-separated uid)
    String uidCompact = uid;
    uidCompact.replace(":", "");
    char uidCompactBuf[RFID_UID_STR_LEN] = {};
    strlcpy(uidCompactBuf, uidCompact.c_str(), RFID_UID_STR_LEN);
    rfidNormaliseUid(uidCompactBuf);   // enforce uppercase, remove any stray separators
    bool authorized = rfidWhitelistContains(uidCompactBuf);

    Serial.printf("[RFID] Tag scanned: %s  type=%s  profile=%s  size=%d bytes  authorized=%s  uptime=%lus\n",
                  uid.c_str(), typeName.c_str(), profile, _rfid.uid.size,
                  authorized ? "YES" : "NO", millis() / 1000);

    // Post WS2812 strip event — green for authorized, red for unknown
    {
        LedEvent e{};
        e.type = authorized ? LedEventType::RFID_OK : LedEventType::RFID_FAIL;
        ws2812PostEvent(e);
    }

    // Include new `profile` field (v0.3.17) alongside the existing schema — pure
    // additive change; any Node-RED flow that ignores unknown fields is unaffected.
    String payload = "{\"uid\":\""         + uid
                   + "\",\"uid_size\":"    + String(_rfid.uid.size)
                   + ",\"card_type\":\""   + typeName
                   + "\",\"profile\":\""   + String(profile)
                   + "\",\"authorized\":"  + (authorized ? "true" : "false") + "}";

    if (!mqttIsConnected()) {
        Serial.printf("[RFID] MQTT not connected — publish dropped (uid=%s)\n", uid.c_str());
    } else {
        mqttPublish("telemetry/rfid", payload);
    }

    _rfid.PICC_HaltA();
    _rfidQuietUntilMs = millis() + RFID_POST_READ_QUIET_MS;   // suppress spurious post-halt IRQ
    Serial.println("[RFID] Waiting for next tag...");
}

#endif // RFID_ENABLED
