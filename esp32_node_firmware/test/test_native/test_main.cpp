// =============================================================================
// test_main.cpp  —  Host-side unit tests for pure firmware utility headers
//                   (PlatformIO Unity harness)
//
// Previously test_all.cpp under a custom MSVC/cl.exe build path. Ported to
// Unity in v0.3.03 as part of the PlatformIO migration.
//
// Tests the three dependency-free headers introduced in Phase 5:
//   ranging_math.h   rssiToDistance()
//   mac_utils.h      macToString()
//   peer_tracker.h   PeerTracker<N>
//
// Also tests the MQTT topic sanitiser extracted in Phase 1.
//
// BUILD & RUN:
//   pio test -e native
// =============================================================================

#include <unity.h>
#include <string.h>
#include <stdio.h>

// Pull in the headers under test.
// They include only <math.h>, <stdio.h>, <string.h>, <stdint.h> — safe on host.
#include "ranging_math.h"
#include "mac_utils.h"
#include "peer_tracker.h"
#include "fwevent.h"   // FwEvent enum + fwEventName() — dependency-free, compiles on host

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
void test_ranging_math_one_metre() {
    // At 1 m with txPower == rssi, exponent is 0 → distance should be 1.0 m
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rssiToDistance(-59, -59, 2.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, rssiToDistance(-59, -59, 2.5f));
}

void test_ranging_math_ten_metres_freespace() {
    // At 10 m (free space, n=2.0): RSSI = txPower - 10*n*log10(10) = -59 - 20 = -79
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 10.0f, rssiToDistance(-79, -59, 2.0f));
}

void test_ranging_math_two_metres_freespace() {
    // At 2 m (n=2.0): RSSI = -59 - 20*log10(2) ≈ -59 - 6.02 = -65.02
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 1.995f, rssiToDistance(-65, -59, 2.0f));
}

void test_ranging_math_clamp_below_one_metre() {
    // rssi > txPower gives exponent < 0 → would be < 1 m; clamp floor is 0.01 m
    float clamped = rssiToDistance(-40, -59, 2.0f);
    TEST_ASSERT_TRUE(clamped >= 0.01f);
}

void test_ranging_math_espnow_indoor_three_metres() {
    // n=2.5 (ESP-NOW indoor), 3 m: RSSI ≈ -59 - 25*log10(3) ≈ -71
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 3.16f, rssiToDistance(-71, -59, 2.5f));
}


// =============================================================================
// mac_utils.h tests
// =============================================================================
void test_mac_utils_all_zero() {
    char out[18];
    uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    macToString(zero, out);
    TEST_ASSERT_EQUAL_STRING("00:00:00:00:00:00", out);
}

void test_mac_utils_broadcast() {
    char out[18];
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    macToString(bcast, out);
    TEST_ASSERT_EQUAL_STRING("FF:FF:FF:FF:FF:FF", out);
}

void test_mac_utils_typical_uppercase() {
    char out[18];
    uint8_t dev[6] = {0xAA, 0xBB, 0x0C, 0x1D, 0x2E, 0x3F};
    macToString(dev, out);
    TEST_ASSERT_EQUAL_STRING("AA:BB:0C:1D:2E:3F", out);
}

void test_mac_utils_null_terminated_17_char() {
    char out[18];
    uint8_t dev[6] = {0xAA, 0xBB, 0x0C, 0x1D, 0x2E, 0x3F};
    macToString(dev, out);
    TEST_ASSERT_EQUAL_CHAR('\0', out[17]);
    TEST_ASSERT_EQUAL_size_t(17u, strlen(out));
}


// =============================================================================
// peer_tracker.h tests
// =============================================================================
void test_peer_tracker_empty_starts_at_zero() {
    PeerTracker<4> tracker;
    tracker.setNow(1000);
    TEST_ASSERT_EQUAL_UINT8(0, tracker.count());
}

void test_peer_tracker_observe_updates_existing() {
    PeerTracker<4> tracker;
    tracker.setNow(1000);
    tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    TEST_ASSERT_EQUAL_UINT8(1, tracker.count());
    tracker.observe("AA:BB:CC:DD:EE:01", -62, 2.2f);
    TEST_ASSERT_EQUAL_UINT8(1, tracker.count());
}

void test_peer_tracker_lru_eviction_on_full() {
    PeerTracker<4> tracker;
    tracker.setNow(1000); tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    tracker.setNow(2000); tracker.observe("AA:BB:CC:DD:EE:02", -70, 3.5f);
    tracker.setNow(3000); tracker.observe("AA:BB:CC:DD:EE:03", -75, 5.0f);
    tracker.setNow(4000); tracker.observe("AA:BB:CC:DD:EE:04", -80, 7.0f);
    TEST_ASSERT_EQUAL_UINT8(4, tracker.count());

    // 5th MAC → evicts LRU (:01)
    tracker.setNow(5000); tracker.observe("AA:BB:CC:DD:EE:05", -55, 1.5f);
    TEST_ASSERT_EQUAL_UINT8(4, tracker.count());

    bool found01 = false, found05 = false;
    tracker.forEach([&](const PeerEntry& p) {
        if (strcmp(p.mac, "AA:BB:CC:DD:EE:01") == 0) found01 = true;
        if (strcmp(p.mac, "AA:BB:CC:DD:EE:05") == 0) found05 = true;
    });
    TEST_ASSERT_FALSE(found01);
    TEST_ASSERT_TRUE(found05);
}

void test_peer_tracker_expire_stale_entries() {
    PeerTracker<4> tracker;
    tracker.setNow(1000); tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    tracker.setNow(2000); tracker.observe("AA:BB:CC:DD:EE:02", -70, 3.5f);

    // Advance clock well past stale window
    tracker.setNow(10000);
    tracker.expire(2500);
    TEST_ASSERT_EQUAL_UINT8(0, tracker.count());
}

void test_peer_tracker_clear_resets() {
    PeerTracker<4> tracker;
    tracker.setNow(0);
    tracker.observe("AA:BB:CC:DD:EE:FF", -50, 1.0f);
    TEST_ASSERT_EQUAL_UINT8(1, tracker.count());
    tracker.clear();
    TEST_ASSERT_EQUAL_UINT8(0, tracker.count());
}

void test_peer_tracker_foreach_empty_noop() {
    PeerTracker<4> tracker;
    int calls = 0;
    tracker.forEach([&](const PeerEntry&) { calls++; });
    TEST_ASSERT_EQUAL_INT(0, calls);
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

void test_topic_sanitizer_leaves_plain_alnum_unchanged() {
    char seg[64];
    strcpy(seg, "JHBDev");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("JHBDev", seg);
}

void test_topic_sanitizer_replaces_forward_slash() {
    char seg[64];
    strcpy(seg, "JHB/Dev");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("JHB_Dev", seg);
}

void test_topic_sanitizer_replaces_mqtt_wildcards() {
    char seg[64];
    strcpy(seg, "site+area");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("site_area", seg);
    strcpy(seg, "all#nodes");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("all_nodes", seg);
}

void test_topic_sanitizer_replaces_control_chars() {
    char ctrl[] = "abc\x01xyz";
    sanitizeInPlace(ctrl, sizeof(ctrl));
    TEST_ASSERT_EQUAL_CHAR('_', ctrl[3]);
}

void test_topic_sanitizer_empty_string() {
    char seg[64];
    strcpy(seg, "");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("", seg);
}


// =============================================================================
// nvs_utils.h compare-logic tests
//
// We can't link against Arduino's Preferences class on the host runner, so
// this tests the raw compare-then-skip logic by re-implementing the
// decision surface of NvsPutIfChanged() against a fake in-memory store.
// The shape must mirror nvs_utils.h exactly — if either drifts, update both.
// =============================================================================

struct FakePrefs {
    // Track write count so the test can assert "no write happened"
    int writes = 0;
    // Stored values (just the types the real call-sites use)
    bool has_u8 = false;  uint8_t  u8  = 0;
    bool has_u16 = false; uint16_t u16 = 0;
    bool has_u64 = false; uint64_t u64 = 0;
    bool has_str = false; char str[128] = {0};
    bool has_bytes = false; uint8_t bytes[128] = {0}; size_t bytes_len = 0;

    void put_u8(uint8_t v) {
        if (has_u8 && u8 == v) return;
        u8 = v; has_u8 = true; writes++;
    }
    void put_u16(uint16_t v) {
        if (has_u16 && u16 == v) return;
        u16 = v; has_u16 = true; writes++;
    }
    void put_u64(uint64_t v) {
        if (has_u64 && u64 == v) return;
        u64 = v; has_u64 = true; writes++;
    }
    void put_str(const char* v) {
        if (has_str && strcmp(str, v) == 0) return;
        strncpy(str, v, sizeof(str) - 1);
        str[sizeof(str) - 1] = '\0';
        has_str = true; writes++;
    }
    void put_bytes(const void* data, size_t len) {
        if (has_bytes && bytes_len == len && memcmp(bytes, data, len) == 0) return;
        memcpy(bytes, data, len);
        bytes_len = len; has_bytes = true; writes++;
    }
};

void test_nvs_u8_skip_on_identical() {
    FakePrefs p;
    p.put_u8(42); p.put_u8(42); p.put_u8(42);
    TEST_ASSERT_EQUAL_INT(1, p.writes);   // only the first write counts
}

void test_nvs_u8_writes_on_change() {
    FakePrefs p;
    p.put_u8(1); p.put_u8(2); p.put_u8(3);
    TEST_ASSERT_EQUAL_INT(3, p.writes);
}

void test_nvs_u16_skip_on_identical() {
    FakePrefs p;
    p.put_u16(1883); p.put_u16(1883);
    TEST_ASSERT_EQUAL_INT(1, p.writes);
}

void test_nvs_u64_skip_on_identical() {
    FakePrefs p;
    p.put_u64(1745229000ULL); p.put_u64(1745229000ULL);
    TEST_ASSERT_EQUAL_INT(1, p.writes);
}

void test_nvs_str_skip_on_identical() {
    FakePrefs p;
    p.put_str("Enigma"); p.put_str("Enigma"); p.put_str("Enigma");
    TEST_ASSERT_EQUAL_INT(1, p.writes);
}

void test_nvs_str_writes_on_change() {
    FakePrefs p;
    p.put_str("JHBDev"); p.put_str("JHBProd");
    TEST_ASSERT_EQUAL_INT(2, p.writes);
}

void test_nvs_bytes_skip_on_identical() {
    FakePrefs p;
    uint8_t mac_a[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac_b[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};   // identical content
    p.put_bytes(mac_a, 6);
    p.put_bytes(mac_b, 6);
    TEST_ASSERT_EQUAL_INT(1, p.writes);
}

void test_nvs_bytes_writes_on_length_change() {
    FakePrefs p;
    uint8_t a[6] = {1,2,3,4,5,6};
    uint8_t b[8] = {1,2,3,4,5,6,7,8};
    p.put_bytes(a, 6);
    p.put_bytes(b, 8);   // same prefix, different length → must write
    TEST_ASSERT_EQUAL_INT(2, p.writes);
}


// =============================================================================
// fwevent.h tests
//
// Verifies that fwEventName() returns the exact string literals that Node-RED
// flows depend on. If either the enum or the string table drifts, these tests
// fail at compile/test time — before a firmware release can ship a rename that
// silently breaks downstream flows.
// =============================================================================

void test_fwevent_boot_is_retained_string() {
    // "boot" is the one event published as a retained MQTT message.
    // The exact string is checked by onMqttConnect's strcmp() retain logic —
    // any rename here would stop the boot announcement from being retained.
    TEST_ASSERT_EQUAL_STRING("boot", fwEventName(FwEvent::BOOT));
}

void test_fwevent_all_known_values() {
    // Every enum value must map to the string the Node-RED flows expect.
    // Update both the enum and this test together whenever a new event is added.
    TEST_ASSERT_EQUAL_STRING("boot",                  fwEventName(FwEvent::BOOT));
    TEST_ASSERT_EQUAL_STRING("heartbeat",             fwEventName(FwEvent::HEARTBEAT));
    TEST_ASSERT_EQUAL_STRING("cred_rotate_rejected",  fwEventName(FwEvent::CRED_ROTATE_REJECTED));
    TEST_ASSERT_EQUAL_STRING("cred_rotated",          fwEventName(FwEvent::CRED_ROTATED));
    TEST_ASSERT_EQUAL_STRING("config_mode_active",    fwEventName(FwEvent::CONFIG_MODE_ACTIVE));
    TEST_ASSERT_EQUAL_STRING("restarting",            fwEventName(FwEvent::RESTARTING));
    TEST_ASSERT_EQUAL_STRING("ota_checking",          fwEventName(FwEvent::OTA_CHECKING));
    TEST_ASSERT_EQUAL_STRING("ota_downloading",       fwEventName(FwEvent::OTA_DOWNLOADING));
    TEST_ASSERT_EQUAL_STRING("ota_failed",            fwEventName(FwEvent::OTA_FAILED));
    TEST_ASSERT_EQUAL_STRING("ota_success",           fwEventName(FwEvent::OTA_SUCCESS));
}

void test_fwevent_unknown_value_returns_string() {
    // The default branch must never return nullptr — a nullptr passed to
    // mqttPublishStatus() would crash the JSON builder.
    FwEvent unknown = static_cast<FwEvent>(0xFF);
    const char* name = fwEventName(unknown);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("unknown", name);
}


// =============================================================================
// Unity entry point
// =============================================================================
void setUp(void) {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // ranging_math
    RUN_TEST(test_ranging_math_one_metre);
    RUN_TEST(test_ranging_math_ten_metres_freespace);
    RUN_TEST(test_ranging_math_two_metres_freespace);
    RUN_TEST(test_ranging_math_clamp_below_one_metre);
    RUN_TEST(test_ranging_math_espnow_indoor_three_metres);

    // mac_utils
    RUN_TEST(test_mac_utils_all_zero);
    RUN_TEST(test_mac_utils_broadcast);
    RUN_TEST(test_mac_utils_typical_uppercase);
    RUN_TEST(test_mac_utils_null_terminated_17_char);

    // peer_tracker
    RUN_TEST(test_peer_tracker_empty_starts_at_zero);
    RUN_TEST(test_peer_tracker_observe_updates_existing);
    RUN_TEST(test_peer_tracker_lru_eviction_on_full);
    RUN_TEST(test_peer_tracker_expire_stale_entries);
    RUN_TEST(test_peer_tracker_clear_resets);
    RUN_TEST(test_peer_tracker_foreach_empty_noop);

    // topic_sanitizer
    RUN_TEST(test_topic_sanitizer_leaves_plain_alnum_unchanged);
    RUN_TEST(test_topic_sanitizer_replaces_forward_slash);
    RUN_TEST(test_topic_sanitizer_replaces_mqtt_wildcards);
    RUN_TEST(test_topic_sanitizer_replaces_control_chars);
    RUN_TEST(test_topic_sanitizer_empty_string);

    // nvs_utils compare-logic
    RUN_TEST(test_nvs_u8_skip_on_identical);
    RUN_TEST(test_nvs_u8_writes_on_change);
    RUN_TEST(test_nvs_u16_skip_on_identical);
    RUN_TEST(test_nvs_u64_skip_on_identical);
    RUN_TEST(test_nvs_str_skip_on_identical);
    RUN_TEST(test_nvs_str_writes_on_change);
    RUN_TEST(test_nvs_bytes_skip_on_identical);
    RUN_TEST(test_nvs_bytes_writes_on_length_change);

    // fwevent — string table consistency
    RUN_TEST(test_fwevent_boot_is_retained_string);
    RUN_TEST(test_fwevent_all_known_values);
    RUN_TEST(test_fwevent_unknown_value_returns_string);

    return UNITY_END();
}
