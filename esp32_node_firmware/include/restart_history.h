#pragma once

// ── Restart-history ring buffer (#76 sub-B) ──────────────────────────────────
// Extends the sub-G `restart_cause` primitive with a circular buffer of the
// last N restart reasons. Lets the dashboard / daily-health spot patterns
// like "this device restarted 3 times with mqtt_unrecoverable in the last
// hour" — pattern detection that single-shot restart_cause can't surface.
//
// Storage: NVS namespace "rstdiag" (same as restart_cause), keys
//   h0, h1, ..., h<N-1>  — each a String reason (max ~32 chars)
//   hHead                — uint8 next-write index (0 .. N-1)
//
// Push semantics: oldest entry is overwritten when the ring fills.
// Read semantics: oldest-first array — slot[head] is the oldest (about to be
// overwritten next push), slot[head - 1 mod N] is the newest.
//
// Wear: writes only happen on intentional restarts (cmd/restart, cred_rotate,
// mqtt_unrecoverable, OTA reboot). Bounded by operator activity. NVS is good
// for ~100k erase cycles per page; at 1 restart/day this lasts > 250 years.
//
// Wired into mqttScheduleRestart() in mqtt_client.h alongside RestartCause::set
// and into the boot announcement JSON via anchorSnip (boot events only).

#include <Preferences.h>
#include <Arduino.h>

namespace RestartHistory {

    static constexpr const char* NS         = "rstdiag";
    static constexpr const char* HEAD_KEY   = "hHead";
    static constexpr uint8_t     RING_SIZE  = 8;

    inline void _slotKey(char* out, size_t out_sz, uint8_t i) {
        snprintf(out, out_sz, "h%u", (unsigned)i);
    }

    // Append a new restart reason to the ring. Wraps at RING_SIZE.
    inline bool push(const char* cause) {
        if (!cause || !*cause) return false;
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/false)) return false;
        uint8_t head = p.getUChar(HEAD_KEY, 0);
        if (head >= RING_SIZE) head = 0;   // sanity-clamp on corruption
        char key[8];
        _slotKey(key, sizeof(key), head);
        p.putString(key, cause);
        p.putUChar(HEAD_KEY, (head + 1) % RING_SIZE);
        p.end();
        return true;
    }

    // Build a JSON array fragment of all non-empty entries, oldest-first.
    // Returns "[]" when ring is empty / never pushed.
    inline String readAsJsonArray() {
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/true)) return String("[]");
        uint8_t head = p.getUChar(HEAD_KEY, 0);
        if (head >= RING_SIZE) head = 0;
        // Walk N slots starting from head (oldest) → wrapping → ending at
        // (head - 1) mod N (newest). Skip empty slots.
        String out = "[";
        bool first = true;
        for (uint8_t i = 0; i < RING_SIZE; i++) {
            uint8_t idx = (head + i) % RING_SIZE;
            char key[8];
            _slotKey(key, sizeof(key), idx);
            String v = p.getString(key, "");
            if (v.length() == 0) continue;
            if (!first) out += ",";
            out += "\"";
            out += v;
            out += "\"";
            first = false;
        }
        out += "]";
        p.end();
        return out;
    }

    // (v0.4.24, #76 sub-D) Count consecutive newest entries that match `cause`.
    // Walks backwards from the most-recently-pushed slot. Stops at the first
    // mismatch or empty slot. Used by setup() to detect a restart-loop
    // ("3 consecutive mqtt_unrecoverable") and route to AP_MODE instead of
    // repeating the doomed cycle.
    inline uint8_t countTrailingCause(const char* cause) {
        if (!cause || !*cause) return 0;
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/true)) return 0;
        uint8_t head = p.getUChar(HEAD_KEY, 0);
        if (head >= RING_SIZE) head = 0;
        uint8_t count = 0;
        for (uint8_t i = 0; i < RING_SIZE; i++) {
            // Walk backwards from (head - 1) — the newest entry — towards the
            // oldest. Stop on first mismatch or empty slot.
            uint8_t idx = (head + RING_SIZE - 1 - i) % RING_SIZE;
            char key[8];
            _slotKey(key, sizeof(key), idx);
            String v = p.getString(key, "");
            if (v.length() == 0) break;
            if (v != cause) break;
            count++;
        }
        p.end();
        return count;
    }

    // Diagnostic only — clears the ring. Not currently exposed as a command;
    // call manually if a forensic reset is needed.
    inline void clearAll() {
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/false)) return;
        for (uint8_t i = 0; i < RING_SIZE; i++) {
            char key[8];
            _slotKey(key, sizeof(key), i);
            p.remove(key);
        }
        p.remove(HEAD_KEY);
        p.end();
    }

}  // namespace RestartHistory
