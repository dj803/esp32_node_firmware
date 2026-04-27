#pragma once

// ── Restart cause NVS persistence (#76 sub-G) ────────────────────────────────
// Persists a short string identifying the reason for a software restart so the
// next boot announcement can include it. Today's `boot_reason` field reports
// the chip-level reason (poweron / panic / task_wdt / software). When the
// firmware itself initiates a restart, "software" is uninformative — was it
// a cred_rotate, an OTA reboot, a cmd/restart, an mqtt_unrecoverable, etc.?
// This header lets call sites tag their intent before ESP.restart(), and the
// next boot reads + clears the tag so it appears once.
//
// Usage:
//   RestartCause::set("cmd_restart");
//   delay(50);            // give Preferences::end() time to commit
//   ESP.restart();
//
//   // Early in setup(), exactly once:
//   String cause = RestartCause::consume();   // returns "" if none stored
//   // include in next boot announcement
//
// NVS layout: namespace "rstdiag", key "cause" (string, max 32 chars).
// Wear: writes only happen on intentional restarts — bounded by operator
// activity. Not a per-second hotspot.

#include <Preferences.h>
#include <Arduino.h>

namespace RestartCause {

    static constexpr const char* NS  = "rstdiag";
    static constexpr const char* KEY = "cause";

    // Persist `cause` to NVS. Truncated silently to ~32 chars by Preferences.
    // Safe to call from any context that can take ~5 ms for the NVS commit.
    // Returns true on success.
    inline bool set(const char* cause) {
        if (!cause || !*cause) return false;
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/false)) return false;
        size_t n = p.putString(KEY, cause);
        p.end();
        return n > 0;
    }

    // Read the saved cause and clear it so it surfaces only once. Returns
    // an empty String if none is stored (the normal case for poweron /
    // panic / wdt boots that didn't pass through set()).
    inline String consume() {
        String out;
        Preferences p;
        if (!p.begin(NS, /*readOnly=*/false)) return out;
        out = p.getString(KEY, "");
        if (out.length() > 0) {
            p.remove(KEY);
        }
        p.end();
        return out;
    }

}  // namespace RestartCause
