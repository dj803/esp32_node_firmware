#pragma once

// rate_limit.h — host-testable token-bucket math for espnow_responder.h
//
// Extracted so the refill math can be exercised on the host (test_native)
// without dragging the ESP-NOW / Arduino dependencies of espnow_responder.h.
// The live rate-limiter in espnow_responder.h uses this helper; the boundary
// conditions (cur at cap, massive elapsed, zero elapsed) are covered in
// test/test_native/test_main.cpp.
//
// Design notes — why the clamp lives BEFORE the add:
//   `elapsed / RATE_REFILL_MS` can be arbitrarily large (device uptime, or
//   a MAC that was evicted then re-seen). Doing `cur + raw` first and then
//   clamping is only safe because RATE_BUCKET_CAP is small (3) and uint8_t
//   has 253 headroom, but the reader has to do that arithmetic to convince
//   themselves it's safe. Clamping the addend against the remaining headroom
//   makes the intent unambiguous and leaves no latent overflow for a future
//   edit to stumble into (e.g. raising CAP to 200).

#include <stdint.h>

#ifndef RATE_BUCKET_CAP
#define RATE_BUCKET_CAP   3       // Max tokens per MAC
#endif
#ifndef RATE_REFILL_MS
#define RATE_REFILL_MS    60000   // One token refills per 60 s
#endif

// Clamp the refill math so `cur + toAdd` cannot overflow a uint8_t regardless
// of how long the bucket sat idle (elapsedMs can be hours). Invariant:
// returned value is in [cur, RATE_BUCKET_CAP]; side-effect-free.
static inline uint8_t rateClampRefill(uint8_t cur, uint32_t elapsedMs) {
    if (cur >= RATE_BUCKET_CAP) return RATE_BUCKET_CAP;
    uint32_t raw      = elapsedMs / RATE_REFILL_MS;
    uint32_t headroom = (uint32_t)(RATE_BUCKET_CAP - cur);
    uint32_t toAdd    = raw < headroom ? raw : headroom;
    return (uint8_t)(cur + toAdd);
}
