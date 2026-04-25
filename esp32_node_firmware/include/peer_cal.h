#pragma once

#include <stdint.h>
#include <string.h>
#include "mac_utils.h"

// =============================================================================
// peer_cal.h  —  Per-peer calibration table operations (v0.4.09 / #41.7)
//
// Pure utility header — no Arduino, no MQTT, no NVS — safe to include from
// host-side unit tests. Storage and persistence are owned by app_config.h
// (which has the gAppConfig instance and the NVS code); this header only
// contains the in-memory algorithm.
//
// SCHEMA:
//   8 entries × 8 bytes = 64 bytes. mac == {0,0,0,0,0,0} means "empty slot".
//   Slot 0 is most recently calibrated; LRU (tail) is dropped when full.
//
// USAGE:
//   PeerCalTable<8> table;
//   peerCalUpsertT(table, "AA:BB:CC:DD:EE:FF", -55, 25);   // n×10 = 25 → n=2.5
//   int8_t  txp; uint8_t n10;
//   if (peerCalLookupT(table, "AA:BB:CC:DD:EE:FF", &txp, &n10)) { ... }
// =============================================================================

struct PeerCalEntryT {
    uint8_t mac[6];           // 0,0,0,0,0,0 → empty slot
    int8_t  tx_power_dbm;
    uint8_t path_loss_n_x10;
};
static_assert(sizeof(PeerCalEntryT) == 8,
              "PeerCalEntryT must be 8 bytes for NVS schema stability");

template <uint8_t N>
struct PeerCalTable {
    PeerCalEntryT entries[N] = {};
    uint8_t       count      = 0;
};

namespace _peerCalDetail {
    inline bool macEqual(const uint8_t a[6], const uint8_t b[6]) {
        return memcmp(a, b, 6) == 0;
    }
}

// Lookup. Returns true on hit and fills out pointers; false on miss.
template <uint8_t N>
inline bool peerCalLookupT(const PeerCalTable<N>& t, const char* macStr,
                           int8_t* out_tx_power, uint8_t* out_n_x10) {
    uint8_t mac[6];
    if (!macStringToBytes(macStr, mac)) return false;
    for (uint8_t i = 0; i < t.count && i < N; i++) {
        if (_peerCalDetail::macEqual(t.entries[i].mac, mac)) {
            if (out_tx_power) *out_tx_power = t.entries[i].tx_power_dbm;
            if (out_n_x10)    *out_n_x10    = t.entries[i].path_loss_n_x10;
            return true;
        }
    }
    return false;
}

// Insert or update. Move-to-front on existing key, prepend + LRU-evict on
// new key when full. Returns true on success (false only on bad MAC string).
template <uint8_t N>
inline bool peerCalUpsertT(PeerCalTable<N>& t, const char* macStr,
                           int8_t tx_power_dbm, uint8_t path_loss_n_x10) {
    uint8_t mac[6];
    if (!macStringToBytes(macStr, mac)) return false;

    int existing = -1;
    for (uint8_t i = 0; i < t.count && i < N; i++) {
        if (_peerCalDetail::macEqual(t.entries[i].mac, mac)) { existing = (int)i; break; }
    }

    PeerCalEntryT fresh;
    memcpy(fresh.mac, mac, 6);
    fresh.tx_power_dbm    = tx_power_dbm;
    fresh.path_loss_n_x10 = path_loss_n_x10;

    if (existing >= 0) {
        for (int i = existing; i > 0; i--) t.entries[i] = t.entries[i - 1];
        t.entries[0] = fresh;
        return true;
    }

    uint8_t newCount = (t.count < N) ? (uint8_t)(t.count + 1) : N;
    for (int i = (int)newCount - 1; i > 0; i--) t.entries[i] = t.entries[i - 1];
    t.entries[0] = fresh;
    t.count = newCount;
    return true;
}

// Remove. Returns true if found and removed.
template <uint8_t N>
inline bool peerCalForgetT(PeerCalTable<N>& t, const char* macStr) {
    uint8_t mac[6];
    if (!macStringToBytes(macStr, mac)) return false;

    for (uint8_t i = 0; i < t.count && i < N; i++) {
        if (_peerCalDetail::macEqual(t.entries[i].mac, mac)) {
            for (uint8_t j = i; j + 1 < t.count; j++) t.entries[j] = t.entries[j + 1];
            memset(&t.entries[t.count - 1], 0, sizeof(PeerCalEntryT));
            t.count--;
            return true;
        }
    }
    return false;
}
