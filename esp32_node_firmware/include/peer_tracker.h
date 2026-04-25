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
    char     mac[18]        = {};    // "XX:XX:XX:XX:XX:XX\0"
    int8_t   rssi           = 0;     // raw RSSI of the most recent frame
    float    distM          = 0.0f;  // raw distance from most recent RSSI
    uint32_t seenMs         = 0;     // millis() of last observation
    int16_t  rssi_ema_x10   = 0;     // EMA-smoothed RSSI × 10; 0 = uninitialised
    uint16_t rejects        = 0;     // frames dropped by the outlier gate
    int8_t   outlier_streak = 0;     // signed count of consecutive same-direction
                                     //   outliers since last accepted sample.
                                     //   +N = N consecutive samples ABOVE EMA.
                                     //   −N = N consecutive samples BELOW EMA.
                                     //   When |streak| reaches OUTLIER_RESEED_N the
                                     //   EMA is forcibly reseeded at the latest sample
                                     //   (recovers from real environmental step-changes
                                     //    that would otherwise leave EMA stuck forever).
};

// Number of consecutive same-direction outliers required to forcibly reseed the EMA.
// Lower = faster recovery from step-changes, but more vulnerable to brief glitches
// hijacking the EMA. Higher = more glitch-tolerant but longer stuck-EMA recovery.
// 3 = ~9 s at the default 3 s beacon interval — well below the 15 s peer stale
// timeout, so a peer that crosses outlier_db once-only doesn't trigger reseed
// (single-frame interference burst), but a sustained move triggers reseed before
// the peer would be evicted entirely.
#ifndef PEER_TRACKER_OUTLIER_RESEED_N
#define PEER_TRACKER_OUTLIER_RESEED_N 3
#endif

template <uint8_t N>
class PeerTracker {
public:

    // Record an observation for `mac`.
    // If `mac` is already in the table, updates its entry.
    // If the table is full, evicts the least-recently-seen slot (LRU).
    //
    // EMA smoothing (F4):
    //   alpha_x100 = 0   — filtering disabled; rssi_ema_x10 tracks raw RSSI × 10
    //   alpha_x100 > 0   — EMA: new = α×rssi + (1−α)×prev, with optional outlier gate
    //   outlier_db  > 0  — drop frames where |rssi − ema| > outlier_db (rejects++)
    void observe(const char* mac, int8_t rssi, float distM,
                 uint8_t alpha_x100 = 0, uint8_t outlier_db = 0) {
        int  slot   = -1;
        int  oldest = 0;
        for (int i = 0; i < N; i++) {
            if (strcmp(_slots[i].mac, mac) == 0) { slot = i; break; }   // existing
            if (_slots[i].mac[0] == '\0')         { slot = i; break; }   // empty
            if (_slots[i].seenMs < _slots[oldest].seenMs) oldest = i;   // track LRU
        }
        if (slot < 0) slot = oldest;   // table full — evict LRU

        // True when this is a continuing observation for the same peer.
        // False for brand-new empty slots or LRU-evicted slots (different peer).
        bool samePeer = (_slots[slot].mac[0] != '\0' && strcmp(_slots[slot].mac, mac) == 0);

        strncpy(_slots[slot].mac, mac, 17);
        _slots[slot].mac[17] = '\0';
        _slots[slot].rssi    = rssi;
        _slots[slot].distM   = distM;
        _slots[slot].seenMs  = _nowMs;
        if (!samePeer) {
            _slots[slot].rssi_ema_x10   = 0;   // reset EMA when peer changes
            _slots[slot].rejects        = 0;
            _slots[slot].outlier_streak = 0;
        }

        // EMA + outlier update
        if (alpha_x100 == 0 || _slots[slot].rssi_ema_x10 == 0) {
            // Initialise or run without filtering: track raw RSSI
            _slots[slot].rssi_ema_x10   = (int16_t)(rssi * 10);
            _slots[slot].outlier_streak = 0;
        } else {
            int16_t prevEma = _slots[slot].rssi_ema_x10;
            // Outlier gate: reject if deviation exceeds threshold
            if (outlier_db > 0) {
                int16_t dev    = (int16_t)(rssi * 10) - prevEma;     // signed deviation
                int16_t devAbs = (dev < 0) ? -dev : dev;             // |dev|
                if (devAbs > (int16_t)(outlier_db * 10)) {
                    // Outlier. Track consecutive same-direction streak.
                    int8_t dir = (dev > 0) ? +1 : -1;
                    if ((_slots[slot].outlier_streak > 0 && dir > 0) ||
                        (_slots[slot].outlier_streak < 0 && dir < 0)) {
                        // Same direction as previous outlier — extend streak (saturating).
                        if (_slots[slot].outlier_streak < INT8_MAX && dir > 0)
                            _slots[slot].outlier_streak++;
                        else if (_slots[slot].outlier_streak > INT8_MIN && dir < 0)
                            _slots[slot].outlier_streak--;
                    } else {
                        // First outlier or direction flipped — start fresh.
                        _slots[slot].outlier_streak = dir;
                    }

                    // Reseed if the streak has reached the threshold.
                    int8_t absStreak = _slots[slot].outlier_streak;
                    if (absStreak < 0) absStreak = -absStreak;
                    if (absStreak >= PEER_TRACKER_OUTLIER_RESEED_N) {
                        _slots[slot].rssi_ema_x10   = (int16_t)(rssi * 10);
                        _slots[slot].outlier_streak = 0;
                        return;   // EMA reseeded; this frame is the new baseline
                    }

                    _slots[slot].rejects++;
                    return;   // not yet at reseed threshold — skip EMA update
                }
            }
            // Sample passed the gate (or gate disabled): clear outlier streak.
            _slots[slot].outlier_streak = 0;
            // α×rssi×10 + (1−α)×prevEma, all × 100 for integer math
            int32_t updated = (int32_t)alpha_x100 * (rssi * 10)
                            + (int32_t)(100 - alpha_x100) * prevEma;
            _slots[slot].rssi_ema_x10 = (int16_t)(updated / 100);
        }
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
