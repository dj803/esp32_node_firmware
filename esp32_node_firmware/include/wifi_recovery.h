#pragma once

// =============================================================================
// wifi_recovery.h  —  Pure-logic helpers for v0.3.15 WiFi recovery
//
// These functions are deliberately dependency-free (no Arduino, no FreeRTOS)
// so the host test harness in test/test_native/ can cover the decision
// boundaries directly. The actual WiFi.reconnect() / ESP.restart() calls
// live in main.cpp and ap_portal.h — they just query the helpers below.
//
// Used by:
//   src/main.cpp                OPERATIONAL WiFi backoff loop
//   include/ap_portal.h         AP-mode STA scan gate
//   test/test_native/test_main  unit tests for all three predicates
// =============================================================================

#include <stdint.h>
#include <stddef.h>

// Advance the backoff index by one step, saturating at the last slot so the
// final step (10 min by default) repeats forever. Pure function — callers
// pass in the constants so the test harness doesn't drag in config.h.
//
//   next = wifiBackoffAdvance(curr, WIFI_BACKOFF_STEPS_COUNT);
static inline uint8_t wifiBackoffAdvance(uint8_t curr, uint8_t stepsCount) {
    if (stepsCount == 0) return 0;
    uint8_t cap = (uint8_t)(stepsCount - 1);
    return (curr >= cap) ? cap : (uint8_t)(curr + 1);
}

// Returns true if the given STA_DISCONNECTED reason code indicates an
// authentication failure — i.e. the password is almost certainly wrong and
// no amount of backoff will help. Caller uses this to decide when to bail to
// AP mode rather than continuing to retry indefinitely.
//
// Reason-code values from esp_wifi_types.h (ESP-IDF):
//   2   WIFI_REASON_AUTH_EXPIRE
//   15  WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
//   202 WIFI_REASON_AUTH_FAIL
//   204 WIFI_REASON_HANDSHAKE_TIMEOUT
//
// Transient codes (NOT auth-fail) include 200 BEACON_TIMEOUT and 201
// NO_AP_FOUND — those mean "router is gone," which is exactly the scenario
// we're trying to survive with indefinite backoff.
static inline bool wifiReasonIsAuthFail(uint8_t reason) {
    return reason == 2 || reason == 15 || reason == 202 || reason == 204;
}

// Admin-idle gate for the AP-mode STA scan. Returns true if the background
// scan should run right now.
//   now          — current millis()
//   lastAdminMs  — millis() of the most recent HTTPS handler invocation
//   lastScanMs   — millis() of the previous scan (0 on first call)
//   adminIdleMs  — grace period after admin activity (AP_ADMIN_IDLE_MS)
//   scanIntervalMs — minimum gap between scans (AP_STA_SCAN_INTERVAL_MS)
//
// The caller separately checks that no GOT_IP exit is already scheduled;
// this predicate only answers "is it time AND is the admin idle?"
static inline bool apStaScanShouldRun(uint32_t now,
                                      uint32_t lastAdminMs,
                                      uint32_t lastScanMs,
                                      uint32_t adminIdleMs,
                                      uint32_t scanIntervalMs) {
    // Unsigned subtraction handles millis() wrap-around correctly.
    bool adminIdle = (now - lastAdminMs) > adminIdleMs;
    bool scanDue   = (now - lastScanMs)  > scanIntervalMs;
    return adminIdle && scanDue;
}
