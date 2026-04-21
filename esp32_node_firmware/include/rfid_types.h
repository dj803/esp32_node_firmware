#pragma once

// =============================================================================
// rfid_types.h  —  Shared RFID playground types (no hardware dependency)
//
// Lives between mqtt_client.h (which parses JSON into these structs) and rfid.h
// (which executes the arm / write / read against the MFRC522). Keeping it
// dependency-free lets the host test environment (test/test_native) verify the
// profile mapper and sector-trailer guard without linking any Arduino / MFRC522
// code.
//
// Added in v0.3.17 for the "generic RFID playground" feature (see the RFID
// Playground plan + docs/rfid_tag_profiles.md).
// =============================================================================

#include <stdint.h>
#include <string.h>

#include "config.h"   // RFID_MAX_WRITE_BLOCKS, RFID_PROGRAM_TIMEOUT_MS

// ── Profile names (string tokens exchanged on the wire) ──────────────────────
// Matched against the `profile` field in cmd/rfid/program, cmd/rfid/read_block
// payloads, and republished on telemetry/rfid. Keep in lock-step with
// docs/rfid_tag_profiles.md.
#define RFID_PROFILE_MIFARE_CLASSIC_1K "mifare_classic_1k"
#define RFID_PROFILE_MIFARE_CLASSIC_4K "mifare_classic_4k"
#define RFID_PROFILE_MIFARE_UL         "mifare_ul"        // NTAG21x is a superset
#define RFID_PROFILE_NTAG21X           "ntag21x"
#define RFID_PROFILE_UNKNOWN           "unknown"

// Maximum request-id length stored for response pairing.
// Node-RED typically sends values like "rfid-prog-1729876543210" — 48 B covers
// that plus any UUID-style request ids with headroom.
#define RFID_REQUEST_ID_LEN            48

// MIFARE Classic block size (16 B). Also the write buffer width passed to
// MFRC522::MIFARE_Write(). NTAG21x uses 4 B pages but we still allocate the
// larger classic size for the request buffer — the firmware truncates for
// page-based profiles.
#define RFID_BLOCK_SIZE                16
#define RFID_KEY_SIZE                   6


// ── RfidWriteBlock ────────────────────────────────────────────────────────────
// One row of a multi-block write. The raw `data` buffer is 16 B regardless of
// profile; for 4-byte-page profiles (NTAG21x) only the first page_size bytes
// are consumed by the firmware.
struct RfidWriteBlock {
    uint16_t block;                    // Block (MIFARE Classic) or page (NTAG) index
    uint8_t  data[RFID_BLOCK_SIZE];    // Raw bytes to write
    uint8_t  keyA[RFID_KEY_SIZE];      // MIFARE Classic Key A (ignored for NTAG)
    bool     has_key_a;                // false → use profile default key
};


// ── RfidProgramRequest ────────────────────────────────────────────────────────
// Parsed form of a cmd/rfid/program payload. Copied onto the rfid.h module
// state when armed; remains valid until completion / cancellation / timeout.
struct RfidProgramRequest {
    char            profile[24];                          // e.g. "mifare_classic_1k"
    RfidWriteBlock  writes[RFID_MAX_WRITE_BLOCKS];
    uint8_t         write_count;                          // 0..RFID_MAX_WRITE_BLOCKS
    uint32_t        timeout_ms;                           // 0 → use RFID_PROGRAM_TIMEOUT_MS
    char            request_id[RFID_REQUEST_ID_LEN];      // Echoed back in response JSON
};


// ── RfidReadRequest ───────────────────────────────────────────────────────────
// Parsed form of a cmd/rfid/read_block payload.
struct RfidReadRequest {
    char     profile[24];
    uint16_t block;
    uint8_t  keyA[RFID_KEY_SIZE];
    bool     has_key_a;
    uint32_t timeout_ms;
    char     request_id[RFID_REQUEST_ID_LEN];
};


// ── Operational mode of the RFID module ──────────────────────────────────────
// Exposed so Node-RED (via telemetry) and unit tests can reason about state
// transitions. IDLE is the pre-v0.3.17 behaviour; PROGRAMMING / READING_BLOCK
// are transient states entered by rfidArmProgram() / rfidArmRead() and exited
// when the next card is handled, the request times out, or rfidCancelPending()
// is called.
enum class RfidMode : uint8_t {
    IDLE = 0,
    PROGRAMMING,
    READING_BLOCK,
};


// ── Sector-trailer guard ──────────────────────────────────────────────────────
// MIFARE Classic sector trailers (blocks 3, 7, 11, 15, … 63 for 1K; up to 255
// for 4K) hold Key A, access bits, Key B. Writing the wrong bytes there can
// permanently brick a tag. The guard is a hard property of the firmware — not
// a feature flag — so even a malformed Node-RED request cannot punch through.
//
// For MIFARE Classic: trailer  ==  ((block + 1) % 4 == 0).
// For other profiles: no trailer concept, always returns false.
inline bool rfidIsSectorTrailer(const char* profile, uint16_t block) {
    if (!profile) return false;
    const bool isMifareClassic =
        strcmp(profile, RFID_PROFILE_MIFARE_CLASSIC_1K) == 0 ||
        strcmp(profile, RFID_PROFILE_MIFARE_CLASSIC_4K) == 0;
    if (!isMifareClassic) return false;
    // Sector trailers sit at the last block of every 4-block sector.
    return (block % 4) == 3;
}


// ── Profile block/page size helper ────────────────────────────────────────────
// Returns the on-tag write granularity in bytes. NTAG21x / Ultralight use 4-B
// pages; MIFARE Classic uses 16-B blocks. Used by mqtt_client.h to sanity-check
// data_hex length before arming the reader.
inline uint8_t rfidProfileBlockSize(const char* profile) {
    if (!profile) return RFID_BLOCK_SIZE;
    if (strcmp(profile, RFID_PROFILE_NTAG21X) == 0 ||
        strcmp(profile, RFID_PROFILE_MIFARE_UL) == 0) {
        return 4;
    }
    return RFID_BLOCK_SIZE;
}


// ── Hex helpers ───────────────────────────────────────────────────────────────
// Small utility functions kept with the shared types so host tests can exercise
// them without dragging in Arduino String. Decode returns the number of bytes
// written, or 0 on parse error / buffer overflow.

inline int rfidHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

inline size_t rfidHexDecode(const char* hex, uint8_t* out, size_t outCap) {
    if (!hex || !out) return 0;
    size_t hexLen = strlen(hex);
    if (hexLen & 1) return 0;           // Odd length — malformed
    size_t bytes = hexLen / 2;
    if (bytes > outCap) return 0;
    for (size_t i = 0; i < bytes; i++) {
        int hi = rfidHexNibble(hex[2 * i]);
        int lo = rfidHexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return bytes;
}

inline void rfidHexEncode(const uint8_t* bytes, size_t len, char* out /* 2*len+1 */) {
    static const char lut[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = lut[(bytes[i] >> 4) & 0x0F];
        out[2 * i + 1] = lut[bytes[i] & 0x0F];
    }
    out[2 * len] = '\0';
}
