#pragma once

#include <string.h>
#include <stdint.h>

// =============================================================================
// peer_tracker.h  —  LRU MAC-keyed RSSI peer table
//
// Replaces the four parallel arrays (_enrMac / _enrRssi / _enrDistM /
// _enrSeenMs) that existed in espnow_ranging.h. The same pattern is also
// present in ble.h's tracked-beacon arrays; PeerTracker is the shared
// abstraction for both.
//
// TEMPLATE PARAMETER:
//   N — maximum number of simultaneously tracked peers.
//       Set to ESPNOW_MAX_TRACKED for the ESP-NOW ranging module.
//
// USAGE:
//   PeerTracker<8> tracker;
//
//   // On every received frame:
//   tracker.observe("AA:BB:CC:DD:EE:FF", -65, 2.3f);
//
//   // Once per loop — evicts silent peers:
//   tracker.expire(millis(), ESPNOW_STALE_MS);
//
//   // Publish:
//   tracker.forEach([](const PeerEntry& p) {
//       Serial.printf("%s  rssi=%d  dist=%.1f\n", p.mac, p.rssi, p.distM);
//   });
//
// CONCURRENCY:
//   Not thread-safe. Call only from a single task/context (main loop).
//   If written from an ISR or callback, wrap calls in portENTER_CRITICAL.
// =============================================================================

struct PeerEntry {
    char     mac[18]   = {};   // "XX:XX:XX:XX:XX:XX\0"
    int8_t   rssi      = 0;
    float    distM     = 0.0f;
    uint32_t seenMs    = 0;    // millis() of last observation
};

template <uint8_t N>
class PeerTracker {
public:

    // Record an observation for `mac`.
    // If `mac` is already in the table, updates its entry.
    // If the table is full, evicts the least-recently-seen slot (LRU).
    void observe(const char* mac, int8_t rssi, float distM) {
        int  slot   = -1;
        int  oldest = 0;
        for (int i = 0; i < N; i++) {
            if (strcmp(_slots[i].mac, mac) == 0) { slot = i; break; }   // existing
            if (_slots[i].mac[0] == '\0')         { slot = i; break; }   // empty
            if (_slots[i].seenMs < _slots[oldest].seenMs) oldest = i;   // track LRU
        }
        if (slot < 0) slot = oldest;   // table full — evict LRU

        strncpy(_slots[slot].mac, mac, 17);
        _slots[slot].mac[17]  = '\0';
        _slots[slot].rssi     = rssi;
        _slots[slot].distM    = distM;
        _slots[slot].seenMs   = _nowMs;  // set by loop() via setNow()
    }

    // Provide the current millis() value before calling observe() or expire().
    // Separates the timing source from the tracker so tests can inject a fixed clock.
    void setNow(uint32_t nowMs) { _nowMs = nowMs; }

    // Remove all entries that have not been seen for longer than staleMs.
    void expire(uint32_t staleMs) {
        for (int i = 0; i < N; i++) {
            if (_slots[i].mac[0] && (_nowMs - _slots[i].seenMs > staleMs)) {
                memset(&_slots[i], 0, sizeof(PeerEntry));
            }
        }
    }

    // Call `fn` once for every active (non-empty) entry.
    // fn signature: void fn(const PeerEntry& entry)
    template <typename Fn>
    void forEach(Fn fn) const {
        for (int i = 0; i < N; i++) {
            if (_slots[i].mac[0]) fn(_slots[i]);
        }
    }

    // Returns the number of active (non-empty) slots.
    uint8_t count() const {
        uint8_t n = 0;
        for (int i = 0; i < N; i++) if (_slots[i].mac[0]) n++;
        return n;
    }

    // Erase all entries.
    void clear() { memset(_slots, 0, sizeof(_slots)); }

private:
    PeerEntry _slots[N] = {};
    uint32_t  _nowMs    = 0;
};
