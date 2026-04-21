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
#include "fwevent.h"       // FwEvent enum + fwEventName() — dependency-free, compiles on host
#include "semver.h"        // semverIsNewer() + semverParse() — extracted from ota.h
#include "rate_limit.h"    // rateClampRefill() — extracted from espnow_responder.h
#include "wire_bundle.h"   // WireBundle struct — extracted from espnow_bootstrap.h
#include "wifi_recovery.h" // backoff index advance, auth-fail classifier, scan gate (v0.3.15)
#include "rfid_types.h"    // RFID playground profile mapper, trailer guard, hex helpers (v0.3.17)

#include <math.h>
#include <stddef.h>  // offsetof
#include <initializer_list>  // ranged-for over {...} literals in the v0.3.17 RFID tests

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
// semver.h tests (v0.3.08)
//
// semverIsNewer() parses MAJOR.MINOR.PATCH numerically.  The critical case is
// patch numbers >= 10: "0.2.15" must be newer than "0.2.7" even though '1' < '7'
// lexicographically.  This caught a real bug in ESP32OTAPull's String::compareTo.
// =============================================================================

void test_semver_newer_major() {
    TEST_ASSERT_TRUE( semverIsNewer("0.2.15", "1.0.0"));
    TEST_ASSERT_FALSE(semverIsNewer("1.0.0",  "0.2.15"));
}

void test_semver_newer_minor() {
    TEST_ASSERT_TRUE( semverIsNewer("0.2.15", "0.3.0"));
    TEST_ASSERT_FALSE(semverIsNewer("0.3.0",  "0.2.15"));
}

void test_semver_newer_patch_numeric_not_lexicographic() {
    // "0.2.7" < "0.2.15" numerically, but "15" < "7" lexicographically.
    // Confirm we parse numerically.
    TEST_ASSERT_TRUE( semverIsNewer("0.2.7",  "0.2.15"));
    TEST_ASSERT_FALSE(semverIsNewer("0.2.15", "0.2.7"));
}

void test_semver_equal_versions_not_newer() {
    TEST_ASSERT_FALSE(semverIsNewer("0.3.07", "0.3.07"));
    TEST_ASSERT_FALSE(semverIsNewer("1.0.0",  "1.0.0"));
}

void test_semver_leading_zeros_in_patch() {
    // "0.3.00" and "0.3.0" are semantically identical — neither is newer.
    TEST_ASSERT_FALSE(semverIsNewer("0.3.00", "0.3.0"));
    TEST_ASSERT_FALSE(semverIsNewer("0.3.0",  "0.3.00"));
}

void test_semver_double_digit_minor() {
    TEST_ASSERT_TRUE( semverIsNewer("0.9.5", "0.10.0"));
    TEST_ASSERT_FALSE(semverIsNewer("0.10.0", "0.9.5"));
}


// =============================================================================
// topic_sanitizer — additional pathological segment tests (v0.3.08)
//
// These extend the existing five sanitizer tests with inputs that caused
// silent breakage in the pre-sanitiser codebase: embedded slashes in segment
// values split the topic mid-hierarchy; wildcards subscribe to unintended trees.
// =============================================================================

void test_topic_sanitizer_multiple_slashes() {
    // A segment stored as "JHB/Dev/Floor2" has two slashes — all must be replaced.
    char seg[64];
    strcpy(seg, "JHB/Dev/Floor2");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("JHB_Dev_Floor2", seg);
}

void test_topic_sanitizer_leading_slash() {
    char seg[64];
    strcpy(seg, "/Leading");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("_Leading", seg);
}

void test_topic_sanitizer_only_wildcards() {
    char seg[8];
    strcpy(seg, "#/+");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("___", seg);
}

void test_topic_sanitizer_mixed_wildcards_and_text() {
    char seg[64];
    strcpy(seg, "site+area#zone");
    sanitizeInPlace(seg, sizeof(seg));
    TEST_ASSERT_EQUAL_STRING("site_area_zone", seg);
}


// =============================================================================
// peer_tracker.h — additional coverage (v0.3.14)
//
// Extends the original six peer_tracker tests with the two behaviours the
// LRU logic is most likely to break on during a refactor: re-observing a
// known MAC after the table fills must update the existing slot (never
// evict), and expire() with a threshold narrower than the age gap must
// leave younger entries intact.
// =============================================================================

void test_peer_tracker_reobserve_full_table_updates_in_place() {
    PeerTracker<4> tracker;
    tracker.setNow(1000); tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    tracker.setNow(2000); tracker.observe("AA:BB:CC:DD:EE:02", -70, 3.5f);
    tracker.setNow(3000); tracker.observe("AA:BB:CC:DD:EE:03", -75, 5.0f);
    tracker.setNow(4000); tracker.observe("AA:BB:CC:DD:EE:04", -80, 7.0f);
    TEST_ASSERT_EQUAL_UINT8(4, tracker.count());

    // Re-observe slot :01 (currently the LRU). Must NOT evict anyone.
    tracker.setNow(5000); tracker.observe("AA:BB:CC:DD:EE:01", -55, 1.0f);
    TEST_ASSERT_EQUAL_UINT8(4, tracker.count());

    bool all_present = true;
    const char* expected[] = {"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02",
                              "AA:BB:CC:DD:EE:03", "AA:BB:CC:DD:EE:04"};
    for (const char* want : expected) {
        bool found = false;
        tracker.forEach([&](const PeerEntry& p) {
            if (strcmp(p.mac, want) == 0) found = true;
        });
        if (!found) { all_present = false; break; }
    }
    TEST_ASSERT_TRUE(all_present);
}

void test_peer_tracker_expire_keeps_young_drops_old() {
    PeerTracker<4> tracker;
    tracker.setNow(1000); tracker.observe("AA:BB:CC:DD:EE:01", -60, 2.0f);
    tracker.setNow(8000); tracker.observe("AA:BB:CC:DD:EE:02", -70, 3.5f);

    // now=10000, threshold=3000 → :01 aged 9000 (drop), :02 aged 2000 (keep)
    tracker.setNow(10000);
    tracker.expire(3000);
    TEST_ASSERT_EQUAL_UINT8(1, tracker.count());

    bool found02 = false;
    tracker.forEach([&](const PeerEntry& p) {
        if (strcmp(p.mac, "AA:BB:CC:DD:EE:02") == 0) found02 = true;
    });
    TEST_ASSERT_TRUE(found02);
}


// =============================================================================
// rate_limit.h — rateClampRefill() boundary tests (v0.3.14)
//
// Guards against a future edit re-introducing the overflow smell that lived
// in espnow_responder.h pre-v0.3.13: `tokens + (elapsed / RATE_REFILL_MS)`
// with no pre-add clamp. The helper's invariant is that the return value
// is always in [cur, RATE_BUCKET_CAP] regardless of elapsedMs.
// =============================================================================

void test_rate_clamp_at_cap_returns_cap() {
    // Already saturated — any elapsed time must keep it at the cap, not exceed.
    TEST_ASSERT_EQUAL_UINT8(RATE_BUCKET_CAP,
                            rateClampRefill(RATE_BUCKET_CAP, 0));
    TEST_ASSERT_EQUAL_UINT8(RATE_BUCKET_CAP,
                            rateClampRefill(RATE_BUCKET_CAP, 60ul * 60 * 1000));
}

void test_rate_clamp_below_cap_refills_proportionally() {
    // cur=1, elapsed=2 refill periods → +2, result=3
    TEST_ASSERT_EQUAL_UINT8(3,
                            rateClampRefill(1, 2 * RATE_REFILL_MS));
    // cur=0, elapsed=1 period → +1
    TEST_ASSERT_EQUAL_UINT8(1,
                            rateClampRefill(0, RATE_REFILL_MS));
}

void test_rate_clamp_huge_elapsed_saturates_at_cap() {
    // Simulate a MAC that has been silent for the whole uint32 range.
    // Pre-clamp code would compute (cur + raw) first; if a future change
    // grew cur or cap, that arithmetic could overflow uint8_t before clamp.
    TEST_ASSERT_EQUAL_UINT8(RATE_BUCKET_CAP,
                            rateClampRefill(0, 0xFFFFFFFFul));
    TEST_ASSERT_EQUAL_UINT8(RATE_BUCKET_CAP,
                            rateClampRefill(1, 0xFFFFFFFFul));
}

void test_rate_clamp_zero_elapsed_unchanged() {
    // No refill period elapsed → result equals input (when below cap).
    TEST_ASSERT_EQUAL_UINT8(0, rateClampRefill(0, 0));
    TEST_ASSERT_EQUAL_UINT8(1, rateClampRefill(1, 0));
    TEST_ASSERT_EQUAL_UINT8(2, rateClampRefill(2, RATE_REFILL_MS - 1));
}


// =============================================================================
// wire_bundle.h — ESP-NOW on-wire contract tests (v0.3.14)
//
// Two devices running different firmware revisions must agree on field
// offsets and the leading version byte — otherwise a credential rotation
// silently writes garbage to NVS. These tests freeze the contract so any
// accidental layout change (e.g. dropped `#pragma pack`) fails the build or
// test before it ships.
// =============================================================================

void test_wirebundle_size_is_176_bytes() {
    TEST_ASSERT_EQUAL_size_t(176u, sizeof(WireBundle));
}

void test_wirebundle_version_is_leading_byte() {
    TEST_ASSERT_EQUAL_size_t(0u, offsetof(WireBundle, wire_version));

    WireBundle w = {};
    w.wire_version = ESPNOW_WIRE_VERSION;
    uint8_t raw[sizeof(WireBundle)];
    memcpy(raw, &w, sizeof(WireBundle));
    TEST_ASSERT_EQUAL_UINT8(ESPNOW_WIRE_VERSION, raw[0]);
}

void test_wirebundle_roundtrip_preserves_fields() {
    WireBundle tx = {};
    tx.wire_version = ESPNOW_WIRE_VERSION;
    strncpy(tx.wifi_ssid,       "Enigma",             sizeof(tx.wifi_ssid)       - 1);
    strncpy(tx.wifi_password,   "correct horse",      sizeof(tx.wifi_password)   - 1);
    strncpy(tx.mqtt_broker_url, "mqtt://10.0.0.1:1883", sizeof(tx.mqtt_broker_url) - 1);
    strncpy(tx.mqtt_username,   "node",               sizeof(tx.mqtt_username)   - 1);
    strncpy(tx.mqtt_password,   "s3cret",             sizeof(tx.mqtt_password)   - 1);
    for (int i = 0; i < 16; i++) tx.rotation_key[i] = (uint8_t)(0xA0 + i);
    tx.timestamp = 0x1122334455667788ULL;   // exercises all 8 bytes
    tx.source    = 1;                        // ADMIN

    // Emulate wire serialisation: raw memcpy over the packed struct.
    uint8_t wire[sizeof(WireBundle)];
    memcpy(wire, &tx, sizeof(WireBundle));

    WireBundle rx = {};
    memcpy(&rx, wire, sizeof(WireBundle));

    TEST_ASSERT_EQUAL_UINT8(ESPNOW_WIRE_VERSION, rx.wire_version);
    TEST_ASSERT_EQUAL_STRING("Enigma",               rx.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("correct horse",        rx.wifi_password);
    TEST_ASSERT_EQUAL_STRING("mqtt://10.0.0.1:1883", rx.mqtt_broker_url);
    TEST_ASSERT_EQUAL_STRING("node",                 rx.mqtt_username);
    TEST_ASSERT_EQUAL_STRING("s3cret",               rx.mqtt_password);
    for (int i = 0; i < 16; i++)
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xA0 + i), rx.rotation_key[i]);
    TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ULL, rx.timestamp);
    TEST_ASSERT_EQUAL_UINT8(1, rx.source);
}


// =============================================================================
// ranging_math.h — extreme-input guards (v0.3.14)
//
// The clamp floor at 0.01 m means the function never returns < 0.01 or a
// NaN for physically valid inputs. These tests cover the two inputs that
// most commonly feed garbage in from radios: an RSSI at the int8 floor
// (effectively "no signal") and RSSI above txPower (nearer than 1 m, which
// would otherwise yield a sub-centimetre distance).
// =============================================================================

void test_ranging_math_extreme_weak_rssi_finite() {
    // int8_t floor = -128 — should produce a large but finite distance,
    // never NaN or inf.
    float d = rssiToDistance(-128, -59, 2.0f);
    TEST_ASSERT_TRUE(isfinite(d));
    TEST_ASSERT_TRUE(d > 100.0f);   // very far, but a real number
}

void test_ranging_math_rssi_above_tx_clamps_to_floor() {
    // RSSI > txPower is physically implausible (closer than the reference
    // distance). The formula yields < 1 m; clamp floor is 0.01 m.
    float d = rssiToDistance(0, -59, 2.0f);
    TEST_ASSERT_TRUE(isfinite(d));
    TEST_ASSERT_TRUE(d >= 0.01f);
}


// =============================================================================
// semver.h — semverParse() strict-mode tests (v0.3.14)
//
// semverIsNewer() is intentionally tolerant (malformed strings parse to
// 0.0.0) for the OTA retry path. semverParse() is the strict complement
// that security-sensitive callers use to reject partial or malformed input
// up-front.
// =============================================================================

void test_semverparse_accepts_well_formed() {
    int maj = 9, min = 9, pat = 9;
    TEST_ASSERT_TRUE(semverParse("0.3.14", maj, min, pat));
    TEST_ASSERT_EQUAL_INT(0, maj);
    TEST_ASSERT_EQUAL_INT(3, min);
    TEST_ASSERT_EQUAL_INT(14, pat);
}

void test_semverparse_rejects_trailing_garbage() {
    int maj = -1, min = -1, pat = -1;
    TEST_ASSERT_FALSE(semverParse("0.3.14-rc1", maj, min, pat));
    TEST_ASSERT_FALSE(semverParse("0.3.14 ",    maj, min, pat));
    // On rejection the output must not be written to.
    TEST_ASSERT_EQUAL_INT(-1, maj);
    TEST_ASSERT_EQUAL_INT(-1, min);
    TEST_ASSERT_EQUAL_INT(-1, pat);
}

void test_semverparse_rejects_missing_components() {
    int maj = 0, min = 0, pat = 0;
    TEST_ASSERT_FALSE(semverParse("0.3",    maj, min, pat));
    TEST_ASSERT_FALSE(semverParse("0",      maj, min, pat));
    TEST_ASSERT_FALSE(semverParse("",       maj, min, pat));
    TEST_ASSERT_FALSE(semverParse(nullptr,  maj, min, pat));
}


// =============================================================================
// wifi_recovery.h tests (v0.3.15 — AP-mode recovery after router outage)
// =============================================================================

// ── wifiBackoffAdvance — saturates at the last slot so the slowest step repeats
void test_wifi_backoff_advance_from_zero() {
    // Fresh outage — first advance moves from step 0 to step 1.
    TEST_ASSERT_EQUAL_UINT8(1, wifiBackoffAdvance(0, 6));
}

void test_wifi_backoff_advance_saturates_at_cap() {
    // Count = 6 → cap index = 5. Advancing from the cap stays at the cap.
    TEST_ASSERT_EQUAL_UINT8(5, wifiBackoffAdvance(5, 6));
    TEST_ASSERT_EQUAL_UINT8(5, wifiBackoffAdvance(10, 6)); // above cap also clamps
}

void test_wifi_backoff_advance_empty_schedule_safe() {
    // Pathological: stepsCount == 0 must not underflow (0 - 1 == UINT8_MAX).
    TEST_ASSERT_EQUAL_UINT8(0, wifiBackoffAdvance(3, 0));
}

void test_wifi_backoff_advance_walks_full_schedule() {
    // Walk 0 → 1 → 2 → 3 → 4 → 5 → 5 (saturate) over 6 steps.
    uint8_t idx = 0;
    for (int i = 0; i < 10; i++) idx = wifiBackoffAdvance(idx, 6);
    TEST_ASSERT_EQUAL_UINT8(5, idx);
}


// ── wifiReasonIsAuthFail — only the four auth-related reason codes trigger fast path
void test_wifi_reason_auth_fail_codes_flagged() {
    // 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 202 AUTH_FAIL, 204 HANDSHAKE_TIMEOUT
    TEST_ASSERT_TRUE(wifiReasonIsAuthFail(2));
    TEST_ASSERT_TRUE(wifiReasonIsAuthFail(15));
    TEST_ASSERT_TRUE(wifiReasonIsAuthFail(202));
    TEST_ASSERT_TRUE(wifiReasonIsAuthFail(204));
}

void test_wifi_reason_transient_codes_not_flagged() {
    // Router-gone codes must NOT fast-path to AP — they're the outage case.
    TEST_ASSERT_FALSE(wifiReasonIsAuthFail(200));   // BEACON_TIMEOUT
    TEST_ASSERT_FALSE(wifiReasonIsAuthFail(201));   // NO_AP_FOUND
    TEST_ASSERT_FALSE(wifiReasonIsAuthFail(0));     // no disconnect yet
    TEST_ASSERT_FALSE(wifiReasonIsAuthFail(8));     // ASSOC_LEAVE
}


// ── apStaScanShouldRun — both gates (admin idle + scan-interval due) must hold
void test_ap_sta_scan_runs_when_idle_and_due() {
    // now=70_000, admin touched at t=0 (70 s ago > 60 s idle)
    // lastScan at t=0 (70 s ago > 30 s interval) → scan runs.
    TEST_ASSERT_TRUE(apStaScanShouldRun(70000, 0, 0, 60000, 30000));
}

void test_ap_sta_scan_blocked_by_recent_admin() {
    // Admin touched 10 s ago — under 60 s idle window → suppress scan.
    TEST_ASSERT_FALSE(apStaScanShouldRun(70000, 60000, 0, 60000, 30000));
}

void test_ap_sta_scan_blocked_by_recent_scan() {
    // Admin idle for 70 s (OK), but last scan only 10 s ago → not yet due.
    TEST_ASSERT_FALSE(apStaScanShouldRun(70000, 0, 60000, 60000, 30000));
}

void test_ap_sta_scan_survives_millis_wrap() {
    // millis() wrapped — now just past 0, lastAdmin + lastScan near UINT32_MAX.
    // Unsigned subtraction gives the correct elapsed values.
    uint32_t wrappedNow      = 100000;         // 100 s after wrap
    uint32_t preWrapAdmin    = 0xFFFFFFFF - 30000; // admin touched 130 s ago across wrap
    uint32_t preWrapLastScan = 0xFFFFFFFF - 60000; // last scan 160 s ago across wrap
    TEST_ASSERT_TRUE(apStaScanShouldRun(wrappedNow, preWrapAdmin, preWrapLastScan,
                                        60000, 30000));
}


// =============================================================================
// rfid_types.h tests (v0.3.17 — RFID playground)
// Profile mapper is MFRC522-dependent so not covered here; the pieces exercised
// here are the ones a malicious Node-RED payload could drive (block numbers,
// hex decoding, profile-aware block size) so regression coverage matters most
// on these. Host-only — no Arduino / MFRC522 link required.
// =============================================================================
void test_rfid_trailer_guard_refuses_classic_trailers() {
    // MIFARE Classic 1K sector trailers: blocks 3, 7, 11, 15, 19, ... 63.
    // Pattern: (block % 4) == 3.
    for (uint16_t block : {3, 7, 11, 15, 19, 23, 27, 31, 35, 39, 43, 47, 51, 55, 59, 63}) {
        TEST_ASSERT_TRUE_MESSAGE(rfidIsSectorTrailer(RFID_PROFILE_MIFARE_CLASSIC_1K, block),
                                 "classic 1K trailer should be refused");
    }
    // MIFARE Classic 4K extends the pattern — also trailers at 67, 71, ..., 255.
    TEST_ASSERT_TRUE(rfidIsSectorTrailer(RFID_PROFILE_MIFARE_CLASSIC_4K, 67));
    TEST_ASSERT_TRUE(rfidIsSectorTrailer(RFID_PROFILE_MIFARE_CLASSIC_4K, 255));
}

void test_rfid_trailer_guard_allows_classic_data_blocks() {
    // Data blocks (0..2, 4..6, 8..10, ...) must pass through.
    for (uint16_t block : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14, 62}) {
        TEST_ASSERT_FALSE_MESSAGE(rfidIsSectorTrailer(RFID_PROFILE_MIFARE_CLASSIC_1K, block),
                                  "data block must NOT be flagged as trailer");
    }
}

void test_rfid_trailer_guard_ignores_non_classic_profiles() {
    // NTAG21x / Ultralight have no sector-trailer concept — guard always false.
    TEST_ASSERT_FALSE(rfidIsSectorTrailer(RFID_PROFILE_NTAG21X,   3));
    TEST_ASSERT_FALSE(rfidIsSectorTrailer(RFID_PROFILE_NTAG21X, 255));
    TEST_ASSERT_FALSE(rfidIsSectorTrailer(RFID_PROFILE_MIFARE_UL, 7));
    TEST_ASSERT_FALSE(rfidIsSectorTrailer("unknown",              7));
    TEST_ASSERT_FALSE(rfidIsSectorTrailer(nullptr,                7));
}

void test_rfid_profile_block_size_mifare_is_16() {
    TEST_ASSERT_EQUAL_UINT8(16, rfidProfileBlockSize(RFID_PROFILE_MIFARE_CLASSIC_1K));
    TEST_ASSERT_EQUAL_UINT8(16, rfidProfileBlockSize(RFID_PROFILE_MIFARE_CLASSIC_4K));
    // Defensive default for unknown / null — fall back to classic width.
    TEST_ASSERT_EQUAL_UINT8(16, rfidProfileBlockSize(nullptr));
}

void test_rfid_profile_block_size_ntag_is_4() {
    TEST_ASSERT_EQUAL_UINT8(4, rfidProfileBlockSize(RFID_PROFILE_NTAG21X));
    TEST_ASSERT_EQUAL_UINT8(4, rfidProfileBlockSize(RFID_PROFILE_MIFARE_UL));
}

void test_rfid_hex_roundtrip_16_bytes() {
    const uint8_t in[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                            0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    char hex[33];
    rfidHexEncode(in, 16, hex);
    TEST_ASSERT_EQUAL_STRING("00112233445566778899AABBCCDDEEFF", hex);

    uint8_t out[16] = {};
    size_t n = rfidHexDecode(hex, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(16, n);
    TEST_ASSERT_EQUAL_MEMORY(in, out, 16);
}

void test_rfid_hex_decode_rejects_odd_length() {
    uint8_t out[8] = {};
    TEST_ASSERT_EQUAL_size_t(0, rfidHexDecode("ABC", out, sizeof(out)));
}

void test_rfid_hex_decode_rejects_non_hex_chars() {
    uint8_t out[8] = {};
    TEST_ASSERT_EQUAL_size_t(0, rfidHexDecode("ZZ", out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, rfidHexDecode("1G", out, sizeof(out)));
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

    // semver — numeric version comparison
    RUN_TEST(test_semver_newer_major);
    RUN_TEST(test_semver_newer_minor);
    RUN_TEST(test_semver_newer_patch_numeric_not_lexicographic);
    RUN_TEST(test_semver_equal_versions_not_newer);
    RUN_TEST(test_semver_leading_zeros_in_patch);
    RUN_TEST(test_semver_double_digit_minor);

    // topic_sanitizer — pathological segment values
    RUN_TEST(test_topic_sanitizer_multiple_slashes);
    RUN_TEST(test_topic_sanitizer_leading_slash);
    RUN_TEST(test_topic_sanitizer_only_wildcards);
    RUN_TEST(test_topic_sanitizer_mixed_wildcards_and_text);

    // peer_tracker — additional coverage (v0.3.14)
    RUN_TEST(test_peer_tracker_reobserve_full_table_updates_in_place);
    RUN_TEST(test_peer_tracker_expire_keeps_young_drops_old);

    // rate_limit — token-bucket refill boundaries (v0.3.14)
    RUN_TEST(test_rate_clamp_at_cap_returns_cap);
    RUN_TEST(test_rate_clamp_below_cap_refills_proportionally);
    RUN_TEST(test_rate_clamp_huge_elapsed_saturates_at_cap);
    RUN_TEST(test_rate_clamp_zero_elapsed_unchanged);

    // wire_bundle — ESP-NOW on-wire contract (v0.3.14)
    RUN_TEST(test_wirebundle_size_is_176_bytes);
    RUN_TEST(test_wirebundle_version_is_leading_byte);
    RUN_TEST(test_wirebundle_roundtrip_preserves_fields);

    // ranging_math — extreme-input guards (v0.3.14)
    RUN_TEST(test_ranging_math_extreme_weak_rssi_finite);
    RUN_TEST(test_ranging_math_rssi_above_tx_clamps_to_floor);

    // semver — strict-mode parser (v0.3.14)
    RUN_TEST(test_semverparse_accepts_well_formed);
    RUN_TEST(test_semverparse_rejects_trailing_garbage);
    RUN_TEST(test_semverparse_rejects_missing_components);

    // wifi_recovery — backoff + auth-fail + AP scan gate (v0.3.15)
    RUN_TEST(test_wifi_backoff_advance_from_zero);
    RUN_TEST(test_wifi_backoff_advance_saturates_at_cap);
    RUN_TEST(test_wifi_backoff_advance_empty_schedule_safe);
    RUN_TEST(test_wifi_backoff_advance_walks_full_schedule);
    RUN_TEST(test_wifi_reason_auth_fail_codes_flagged);
    RUN_TEST(test_wifi_reason_transient_codes_not_flagged);
    RUN_TEST(test_ap_sta_scan_runs_when_idle_and_due);
    RUN_TEST(test_ap_sta_scan_blocked_by_recent_admin);
    RUN_TEST(test_ap_sta_scan_blocked_by_recent_scan);
    RUN_TEST(test_ap_sta_scan_survives_millis_wrap);

    // rfid_types — v0.3.17 playground: profile mapper, trailer guard, hex codec
    RUN_TEST(test_rfid_trailer_guard_refuses_classic_trailers);
    RUN_TEST(test_rfid_trailer_guard_allows_classic_data_blocks);
    RUN_TEST(test_rfid_trailer_guard_ignores_non_classic_profiles);
    RUN_TEST(test_rfid_profile_block_size_mifare_is_16);
    RUN_TEST(test_rfid_profile_block_size_ntag_is_4);
    RUN_TEST(test_rfid_hex_roundtrip_16_bytes);
    RUN_TEST(test_rfid_hex_decode_rejects_odd_length);
    RUN_TEST(test_rfid_hex_decode_rejects_non_hex_chars);

    return UNITY_END();
}
