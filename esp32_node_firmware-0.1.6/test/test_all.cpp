// =============================================================================
// test_all.cpp  —  Host-side unit tests for pure firmware utility headers
//
// Tests the three dependency-free headers introduced in Phase 5:
//   ranging_math.h   rssiToDistance()
//   mac_utils.h      macToString()
//   peer_tracker.h   PeerTracker<N>
//
// Also tests the MQTT topic sanitiser extracted in Phase 1.
//
// Build & run (from repo root):
//   Windows (MinGW/g++):
//     g++ -std=c++11 -I esp32_node_firmware-0.1.6/esp32_firmware/esp32_firmware ^
//         -o test\run_tests.exe test\test_all.cpp -lm && test\run_tests.exe
//
//   Linux / macOS:
//     g++ -std=c++11 -I esp32_node_firmware-0.1.6/esp32_firmware/esp32_firmware \
//         -o test/run_tests test/test_all.cpp -lm && test/run_tests
// =============================================================================

#include "test_runner.h"

// Pull in the headers under test.
// They include only <math.h>, <stdio.h>, <string.h>, <stdint.h> — safe on host.
#include "ranging_math.h"
#include "mac_utils.h"
#include "peer_tracker.h"

// ── Stub out millis() — used inside PeerTracker via setNow() but the test
//    drives the clock manually, so the real implementation is not needed.
// (On Arduino, millis() is provided by the runtime.)
#ifndef ARDUINO
static uint32_t _fake_millis = 0;
inline uint32_t millis() { return _fake_millis; }
#endif


// =============================================================================
// ranging_math.h tests
// =============================================================================
static void test_ranging_math() {
    // At 1 m with txPower == rssi, exponent is 0 → distance should be 1.0 m
    TEST_ASSERT_NEAR(rssiToDistance(-59, -59, 2.0f), 1.0f, 0.001f);
    TEST_ASSERT_NEAR(rssiToDistance(-59, -59, 2.5f), 1.0f, 0.001f);

    // At 10 m (free space, n=2.0): RSSI = txPower - 10*n*log10(10) = -59 - 20 = -79
    TEST_ASSERT_NEAR(rssiToDistance(-79, -59, 2.0f), 10.0f, 0.05f);

    // At 2 m (n=2.0): RSSI = -59 - 20*log10(2) ≈ -59 - 6.02 = -65.02
    TEST_ASSERT_NEAR(rssiToDistance(-65, -59, 2.0f), 1.995f, 0.05f);

    // Clamp: rssi > txPower gives exponent < 0 → would be < 1 m; minimum is 0.01 m
    float clamped = rssiToDistance(-40, -59, 2.0f);   // rssi stronger than txPower
    TEST_ASSERT(clamped >= 0.01f);

    // n=2.5 (ESP-NOW indoor) — 3 m distance:
    // RSSI ≈ -59 - 25*log10(3) ≈ -59 - 11.94 ≈ -71
    TEST_ASSERT_NEAR(rssiToDistance(-71, -59, 2.5f), 3.16f, 0.15f);

    printf("  ranging_math: all assertions checked\n");
}


// =============================================================================
// mac_utils.h tests
// =============================================================================
static void test_mac_utils() {
    char out[18];

    // All-zero MAC
    uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    macToString(zero, out);
    TEST_ASSERT(strcmp(out, "00:00:00:00:00:00") == 0);

    // All-FF MAC (broadcast)
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    macToString(bcast, out);
    TEST_ASSERT(strcmp(out, "FF:FF:FF:FF:FF:FF") == 0);

    // Typical device MAC — uppercase
    uint8_t dev[6] = {0xAA, 0xBB, 0x0C, 0x1D, 0x2E, 0x3F};
    macToString(dev, out);
    TEST_ASSERT(strcmp(out, "AA:BB:0C:1D:2E:3F") == 0);

    // Always null-terminates at position 17
    TEST_ASSERT(out[17] == '\0');

    // Output length is always exactly 17 printable chars + null
    TEST_ASSERT(strlen(out) == 17);

    printf("  mac_utils: all assertions checked\n");
}


// =============================================================================
// peer_tracker.h tests
// =============================================================================
static void test_peer_tracker() {
    PeerTracker<4> tracker;
    _fake_millis = 1000;
    tracker.setNow(_fake_millis);

    // Empty tracker
    TEST_ASSERT(tracker.count() == 0);

    // Observe one peer
    tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    TEST_ASSERT(tracker.count() == 1);

    // Observe same MAC — updates, count stays 1
    tracker.observe("AA:BB:CC:DD:EE:01", -62, 2.2f);
    TEST_ASSERT(tracker.count() == 1);

    // Add more peers
    _fake_millis = 2000; tracker.setNow(_fake_millis);
    tracker.observe("AA:BB:CC:DD:EE:02", -70, 3.5f);
    _fake_millis = 3000; tracker.setNow(_fake_millis);
    tracker.observe("AA:BB:CC:DD:EE:03", -75, 5.0f);
    _fake_millis = 4000; tracker.setNow(_fake_millis);
    tracker.observe("AA:BB:CC:DD:EE:04", -80, 7.0f);
    TEST_ASSERT(tracker.count() == 4);

    // Table full (N=4). Adding a 5th MAC evicts the LRU slot (seenMs=1000 → :01)
    _fake_millis = 5000; tracker.setNow(_fake_millis);
    tracker.observe("AA:BB:CC:DD:EE:05", -55, 1.5f);
    TEST_ASSERT(tracker.count() == 4);

    // :01 should have been evicted; :05 should be present
    bool found01 = false, found05 = false;
    tracker.forEach([&](const PeerEntry& p) {
        if (strcmp(p.mac, "AA:BB:CC:DD:EE:01") == 0) found01 = true;
        if (strcmp(p.mac, "AA:BB:CC:DD:EE:05") == 0) found05 = true;
    });
    TEST_ASSERT(!found01);
    TEST_ASSERT(found05);

    // expire() — advance clock past stale window, then check
    // :02 was seen at t=2000; stale threshold = 2500 ms
    _fake_millis = 10000; tracker.setNow(_fake_millis);
    tracker.expire(2500);
    // All peers except :05 (seen at t=5000) should be evicted at t=10000
    // :05 seenMs=5000, age=5000 ms > 2500 → also evicted
    // Actually all are older than 2500 ms at t=10000, so count should be 0
    TEST_ASSERT(tracker.count() == 0);

    // clear() resets everything
    _fake_millis = 0; tracker.setNow(_fake_millis);
    tracker.observe("AA:BB:CC:DD:EE:FF", -50, 1.0f);
    TEST_ASSERT(tracker.count() == 1);
    tracker.clear();
    TEST_ASSERT(tracker.count() == 0);

    // forEach on empty tracker calls fn zero times
    int calls = 0;
    tracker.forEach([&](const PeerEntry&) { calls++; });
    TEST_ASSERT(calls == 0);

    printf("  peer_tracker: all assertions checked\n");
}


// =============================================================================
// MQTT topic sanitiser — extracted from mqtt_client.h for testability
// (reproduced here as a standalone function since mqtt_client.h pulls in
// Arduino/FreeRTOS headers that do not compile on the host)
// =============================================================================
static void sanitizeInPlace(char* buf, size_t len) {
    for (size_t i = 0; i < len && buf[i]; i++) {
        char c = buf[i];
        if (c == '/' || c == '+' || c == '#' || (uint8_t)c < 0x20)
            buf[i] = '_';
    }
}

static void test_topic_sanitizer() {
    char seg[64];

    // Normal segment — unchanged
    strcpy(seg, "JHBDev"); sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT(strcmp(seg, "JHBDev") == 0);

    // Forward slash
    strcpy(seg, "JHB/Dev"); sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT(strcmp(seg, "JHB_Dev") == 0);

    // MQTT wildcards
    strcpy(seg, "site+area"); sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT(strcmp(seg, "site_area") == 0);
    strcpy(seg, "all#nodes"); sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT(strcmp(seg, "all_nodes") == 0);

    // Control character (e.g. null in middle — represented as \x01)
    char ctrl[] = "abc\x01xyz";
    sanitizeInPlace(ctrl, sizeof(ctrl));
    TEST_ASSERT(ctrl[3] == '_');

    // Empty string — no crash
    strcpy(seg, ""); sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT(strcmp(seg, "") == 0);

    printf("  topic_sanitizer: all assertions checked\n");
}


// =============================================================================
// Entry point
// =============================================================================
int main() {
    RUN_SUITE("ranging_math",     test_ranging_math);
    RUN_SUITE("mac_utils",        test_mac_utils);
    RUN_SUITE("peer_tracker",     test_peer_tracker);
    RUN_SUITE("topic_sanitizer",  test_topic_sanitizer);
    return test_summary();
}
