#pragma once

#include <stdint.h>
#include <string.h>

// =============================================================================
// ndef.h  —  Minimal NDEF (NFC Data Exchange Format) URI encoder
//
// Builds a complete NDEF Type 2 (NTAG21x / Ultralight) memory image from a
// single URI record. Output bytes start at page 4 of the tag (the user-data
// region) and end with the 0xFE terminator. Caller passes the returned length
// to MIFARE_Ultralight_Write in 4-byte page chunks.
//
// Format:
//   TLV header:    0x03 LEN              (or 0x03 0xFF HH LL when LEN > 254)
//   Record header: 0xD1 0x01 PL 0x55     (MB=ME=1, SR=1, well-known type 'U')
//   Payload:       PREFIX_CODE + URI body (1 + N bytes)
//   Terminator:    0xFE
//
// PREFIX_CODE values (NDEF URI Record Type Definition, NFCForum-TS-RTD_URI_1.0):
//   0x00  no prefix       — full URI follows verbatim
//   0x01  http://www.
//   0x02  https://www.
//   0x03  http://
//   0x04  https://        — most common
//   0x05  tel:
//   0x06  mailto:
//   ... (full table in spec; we cover the four most useful)
//
// Caller is expected to pad the buffer to a 4-byte page boundary before
// writing — pages must be written whole even if the NDEF terminator lands
// mid-page.
//
// USAGE:
//   uint8_t buf[128];
//   size_t  n = ndefBuildUri("https://example.com", buf, sizeof(buf));
//   // pad to 4-byte boundary, then write pages 4, 5, 6, ... via Ultralight_Write
// =============================================================================

// Match the prefix table above. Returns the abbreviation code + how many bytes
// of the input URI were consumed by the abbreviation.
inline uint8_t ndefUriPrefixCode(const char* uri, size_t* prefixLen) {
    struct P { const char* s; uint8_t code; };
    static const P table[] = {
        { "https://www.", 0x02 },
        { "http://www.",  0x01 },
        { "https://",     0x04 },
        { "http://",      0x03 },
        { "tel:",         0x05 },
        { "mailto:",      0x06 },
    };
    for (const P& p : table) {
        size_t plen = strlen(p.s);
        if (strncmp(uri, p.s, plen) == 0) {
            *prefixLen = plen;
            return p.code;
        }
    }
    *prefixLen = 0;
    return 0x00;   // no abbreviation — full URI follows
}

// Build NDEF URI record into `out`. Returns total byte count (TLV + record +
// terminator), or 0 on overflow / invalid input.
inline size_t ndefBuildUri(const char* uri, uint8_t* out, size_t outCap) {
    if (!uri || !out) return 0;
    size_t prefixLen = 0;
    uint8_t prefixCode = ndefUriPrefixCode(uri, &prefixLen);
    size_t bodyLen = strlen(uri) - prefixLen;        // URI bytes after prefix
    size_t payloadLen = 1 + bodyLen;                 // prefix code + body
    if (payloadLen > 0xFF) return 0;                 // SR=1 caps at 255 — long URLs need MB/ME chunking (out of scope)

    // TLV LEN field is the NDEF MESSAGE length, i.e. record header + payload.
    size_t recordLen = 4 + payloadLen;               // 0xD1 0x01 PL 0x55 + payload
    size_t tlvLenFieldLen = (recordLen <= 254) ? 1 : 3;
    size_t total = 1 + tlvLenFieldLen + recordLen + 1;   // 0x03 + LEN + record + 0xFE
    if (total > outCap) return 0;

    size_t i = 0;
    out[i++] = 0x03;                                 // NDEF Message TLV tag
    if (tlvLenFieldLen == 1) {
        out[i++] = (uint8_t)recordLen;
    } else {
        out[i++] = 0xFF;                             // 3-byte length sentinel
        out[i++] = (uint8_t)((recordLen >> 8) & 0xFF);
        out[i++] = (uint8_t)(recordLen & 0xFF);
    }

    // NDEF record (single, MB=ME=1, SR=1, TNF=well-known)
    out[i++] = 0xD1;
    out[i++] = 0x01;                                 // type length: 1 ("U")
    out[i++] = (uint8_t)payloadLen;
    out[i++] = 'U';                                  // type field
    out[i++] = prefixCode;
    memcpy(out + i, uri + prefixLen, bodyLen);
    i += bodyLen;

    out[i++] = 0xFE;                                 // NDEF terminator TLV
    return i;
}
