#pragma once

#include <Arduino.h>
#include <AsyncMqttClient.h>   // Non-blocking MQTT client (marvinroger/async-mqtt-client)
#include <ArduinoJson.h>       // JSON parsing for cmd/led and cmd/rfid/whitelist handlers
#include <esp_wifi.h>          // esp_wifi_get_mac / WIFI_IF_STA
#include <esp_system.h>        // esp_reset_reason() — boot-reason telemetry (v0.3.33)
#include "config.h"
#include "logging.h"
#include "fwevent.h"
#include "credentials.h"
#include "crypto.h"
#include "device_id.h"   // Persistent UUID-based device identity
#include "app_config.h"   // gAppConfig: runtime GitHub + MQTT topic segments (NVS-backed)
#include "broker_discovery.h"  // BrokerResult from mDNS / port scan / stored URL
#include "led.h"

// Forward declarations collected into dedicated _fwd.h headers so include
// order in esp32_firmware.ino no longer needs to be fragile.
// Each _fwd.h declares only the symbols this file needs to call; the
// actual definitions remain in their respective implementation headers.
#include "ota_fwd.h"     // otaCheckNow(), otaTrigger()
#include "ws2812_fwd.h"  // ws2812PostEvent(), ws2812PublishState()

// (v0.3.34) Forward-declared from ota_validation.h. Cannot #include directly
// because ota_validation.h depends on mqttPublishStatus, defined in this file.
void otaValidationOnMqttConnect();
void otaValidationConfirmHealth();

// rfid.h whitelist API — rfid.h is included AFTER mqtt_client.h.
// These cannot move to rfid_fwd.h without pulling in RFID_UID_STR_LEN,
// so they remain here where config.h (which defines it) is already included.
bool    rfidWhitelistAdd(const char* uid);
bool    rfidWhitelistRemove(const char* uid);
void    rfidWhitelistClear();
void    rfidWhitelistList(char out[][RFID_UID_STR_LEN], uint8_t& count);

// RFID playground API (v0.3.17). The struct types are hardware-independent and
// live in rfid_types.h so the MQTT handlers below can populate them without
// pulling in MFRC522v2. rfid.h defines the implementations.
#ifdef RFID_ENABLED
#include "rfid_types.h"
#include "ndef.h"             // ndefBuildUri — used by handleRfidNdefWrite
void rfidArmProgram(const RfidProgramRequest& req);
void rfidArmRead(const RfidReadRequest& req);
void rfidCancelPending();
#endif

// BLE API — ble.h is included AFTER mqtt_client.h.
#ifdef BLE_ENABLED
void bleTriggerScan();
void bleSetTrackedMacs(const char** macs, uint8_t count);
void bleClearTracked();
void blePublishList();
#endif

// =============================================================================
// mqtt_client.h  —  MQTT connectivity, topic helpers, credential rotation,
//                   and OTA trigger via MQTT (spec Sections 9, 10, 12)
//
// TOPIC PATTERN (ISA-95 / Unified Namespace):
//   [Enterprise]/[Site]/[Area]/[Line]/[Cell]/[DeviceType]/[DeviceId]/[Prefix]
//
// SUBSCRIPTIONS (all set up in onMqttConnect):
//   .../cmd                General application commands (extend in main sketch)
//   .../cmd/cred_rotate    Encrypted credential rotation bundle (AES-128-GCM)
//   .../cmd/ota_check      Trigger an immediate GitHub OTA version check
//   .../cmd/config_mode    Start the settings HTTP portal on the device LAN IP
//   broadcast/cred_rotate  Site-wide rotation broadcast (all nodes receive this)
//
// PUBLICATIONS:
//   .../status           Boot announcement, heartbeat, OTA events (retained, QoS 1)
//   .../response         Command acknowledgements
//   .../telemetry        Application data (not used by this file — extend in main sketch)
// =============================================================================

// ── Module-level state ────────────────────────────────────────────────────────
// CONCURRENCY AUDIT (mqtt_client.h):
//   onMqttDisconnect / onMqttConnect run in the async_tcp task (Core 0 at high priority).
//   mqttNeedsRediscovery() / mqttIsHung() / mqttHeartbeat() are called from loop() (Core 0,
//   normal priority). Because both contexts run on Core 0, there is no true dual-core race,
//   but the compiler may still cache values across the task-switch boundary.
//   _mqttNeedsRediscovery is marked volatile so the compiler re-reads it on every check in
//   loop() rather than hoisting it into a register that the disconnect callback cannot update.

static AsyncMqttClient  _mqttClient;                         // The MQTT client instance
static TimerHandle_t    _mqttReconnectTimer = nullptr;       // FreeRTOS timer for reconnect delay
static uint32_t         _mqttReconnectDelay = 1000;          // Current reconnect delay (ms); grows on failure
static int              _mqttReconnectCount = 0;             // Consecutive failure count; reset on connect
static volatile bool    _mqttNeedsRediscovery = false;       // Set in disconnect callback; cleared by loop()
static uint32_t         _mqttConnectStartMs = 0;             // millis() when connect() was last called; 0 = idle
// (v0.3.36) volatile — written from MQTT callback context (async_tcp task),
// read from loop(). Without volatile the compiler may cache the read across
// the task-switch boundary and the deferred restart never fires (or fires
// twice). Same rationale as _mqttNeedsRediscovery above.
static volatile uint32_t _mqttRestartAtMs   = 0;             // Non-zero: restart device when millis() >= this value
static volatile bool    _mqttOtaActive     = false;          // OTA in progress — block reconnect attempts
// (#44) Set in onMqttConnect() (async_tcp task); consumed by mqttHeartbeat() (loop task).
// Posting the WS2812 MQTT_HEALTHY event directly from the async callback hit the TWDT
// (the ws2812 task handshake stalls Core 0 long enough to trip the watchdog). The
// deferred-flag pattern moves the post to loop() where it is safe.
static volatile uint32_t _mqttLedHealthyAtMs = 0;

// ── Sleep dispatch state (v0.3.20) ───────────────────────────────────────────
// `cmd/sleep` and `cmd/deep_sleep` take the radio offline, so dispatch is
// deferred SLEEP_DEFER_MS after the status publish — same shape as restart.
// `cmd/modem_sleep` is different: the radio stays associated with the AP and
// MQTT stays connected, so we only need an expiry timer to revert.
enum class SleepKind : uint8_t { NONE, LIGHT, DEEP };
// (v0.3.36) volatile — same task-switch caching rationale as _mqttRestartAtMs.
// Set in onMqttMessage callback for cmd/sleep / cmd/deep_sleep / cmd/modem_sleep,
// read in mqttHeartbeat() loop. Without volatile a stale 0 may persist in a
// register and the deferred sleep never fires.
static volatile uint32_t _mqttSleepAtMs       = 0;            // Non-zero: enter sleep when millis() >= this value
static volatile uint32_t _mqttSleepDurationS  = 0;            // Seconds to sleep for (timer wake-up)
static volatile SleepKind _mqttSleepKind      = SleepKind::NONE;
static volatile uint32_t _mqttModemSleepUntilMs = 0;          // Non-zero: exit modem sleep when millis() >= this value

static String           _deviceId;                        // UUID from DeviceId::get(), set in mqttBegin()
// LIFETIME: the next four globals are passed by .c_str() / raw-pointer to
// AsyncMqttClient setters that STORE the pointer without copying. They MUST
// remain alive (module-static) for the lifetime of _mqttClient. See
// docs/STRING_LIFETIME.md for the codebase-wide convention. Do not move
// these to function-local scope or pass a temporary in their place — that
// is the v0.1.7 / v0.3.30 / v0.3.31 use-after-free shape recurring.
static String           _mqttClientId;                    // LIFETIME: setClientId()
static String           _mqttHost;                        // LIFETIME: setServer()
static String           _mqttWillTopic;                   // LIFETIME: setWill()
static CredentialBundle _mqttBundle;                      // LIFETIME: setCredentials() (mqtt_username/password)


// ── Topic builders ─────────────────────────────────────────────────────────────
// These functions assemble full topic strings from gAppConfig (loaded from NVS at
// boot by AppConfigStore::load()). Using gAppConfig instead of config.h constants
// means the topic hierarchy can be changed via the settings portal without reflashing.
// _deviceId is the device UUID, set once in mqttBegin() and never changes.

// Sanitise one MQTT topic segment read from NVS.
// MQTT wildcards ('/', '+', '#') in a segment would silently create an invalid or
// wildcard topic path — e.g. a stored site value of "JHB/Dev" would split the topic
// mid-hierarchy. Null bytes and control chars are also replaced.
// Replace each offending character with '_' in-place.
static String mqttSanitizeSegment(const char* seg) {
    String s(seg);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '/' || c == '+' || c == '#' || c == '\0' || (uint8_t)c < 0x20) {
            s[i] = '_';
        }
    }
    return s;
}

// Build a device-specific topic:  Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/prefix
static String mqttTopic(const char* prefix) {
    // All six hierarchy segments come from gAppConfig (NVS-backed, portal-editable).
    // Each segment is sanitised to strip MQTT wildcards before concatenation.
    // The DeviceId segment is the persistent UUID — never changes for this device.
    return mqttSanitizeSegment(gAppConfig.mqtt_enterprise) + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_site)       + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_area)       + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_line)       + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_cell)       + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_device_type) + "/" +
           _deviceId                                        + "/" + prefix;
}

// Build the site-wide broadcast rotation topic: Enterprise/Site/broadcast/cred_rotate
// All nodes subscribe to this so a single publish can rotate credentials on every device.
static String mqttBroadcastRotateTopic() {
    return mqttSanitizeSegment(gAppConfig.mqtt_enterprise) + "/" +
           mqttSanitizeSegment(gAppConfig.mqtt_site) + "/broadcast/cred_rotate";
}


// ── Publish helpers ────────────────────────────────────────────────────────────

// Low-level publish — silently drops messages if the client is not connected
// OR if the heap is too fragmented to safely allocate the publish buffer.
//
// (#51 root-cause fix, 2026-04-27)  AsyncMqttClient::publish() internally
// `new`s a contiguous buffer for the framed MQTT message. Under network
// reconnect storms the heap fragments — `free` may stay healthy but the
// largest contiguous block drops below what the publish needs, and the
// allocation throws std::bad_alloc which arduino-esp32 escalates to abort()
// (see backtrace in #51 panic dump 2026-04-27 02:44 SAST). Pre-check the
// largest free block; if too small, drop the publish with a WARN. The
// next call (after broker stabilises and queue drains) will succeed.
//
// Threshold: 4 KB — empirically large enough to fit any typical publish
// payload (status JSON ~250 B, espnow ~370 B, response ~600 B) plus
// AsyncMqttClient framing overhead and async_tcp internal buffers.
#define MQTT_PUBLISH_HEAP_MIN  4096

static void mqttPublish(const char* prefix, const String& payload,
                        uint8_t qos = 0, bool retain = false) {
    if (!_mqttClient.connected()) return;   // Do nothing if not connected
    if (ESP.getMaxAllocHeap() < MQTT_PUBLISH_HEAP_MIN) {
        // Heap fragmented — skip rather than risk bad_alloc panic.
        // Rate-limit the warning to avoid log flooding (one per second max).
        static uint32_t _lastSkipWarnMs = 0;
        uint32_t now = millis();
        if (now - _lastSkipWarnMs > 1000) {
            LOG_W("MQTT", "publish skipped (heap_largest=%u < %u) — "
                          "fragmentation under network stress; #51",
                  (unsigned)ESP.getMaxAllocHeap(), (unsigned)MQTT_PUBLISH_HEAP_MIN);
            _lastSkipWarnMs = now;
        }
        return;
    }
    String topic = mqttTopic(prefix);
    LOG_D("MQTT", "pub %s  %s", topic.c_str(), payload.c_str());
    _mqttClient.publish(topic.c_str(), qos, retain, payload.c_str());
}

// Map esp_reset_reason() → short string for telemetry (v0.3.33).
// Captured ONCE at startup (before the watchdog or any other reset overwrites
// it during this boot's lifetime) and emitted in the retained boot announcement
// so the fleet manager can correlate field reboots with their cause.
static esp_reset_reason_t _firstBootReason  = ESP_RST_UNKNOWN;
static bool               _firstMqttConnect = true;   // distinguishes true boot from reconnect (#61)
static const char* bootReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "other_wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

// Build and publish a standard status JSON message to the .../status topic.
// All status messages share the same JSON structure so Node-RED flows can
// parse them with a single JSON node.
//
// `event` identifies what happened: "boot", "heartbeat", "ota_checking", etc.
// `extraJson` is an optional string of additional key:value pairs to append,
//   e.g. "\"target_version\":\"1.2.0\""  — must NOT include surrounding braces.
static void mqttPublishStatus(const char* event, const char* extraJson = nullptr) {
    // Build the JSON payload onto a stack buffer with snprintf() — avoids the
    // heap-fragmentation churn that the previous String+String chain produced
    // on every heartbeat (30 s cadence). 512 B is sized to fit the longest
    // observed `event` + `extraJson` tail by a comfortable margin; on overflow
    // snprintf truncates rather than corrupts, and we log a warning.
    // Capability flags advertised on every status message so Node-RED can
    // auto-discover which nodes expose optional subsystems without needing a
    // separate handshake. Read from the retained boot announcement on
    // subscribe, then refreshed on every heartbeat.
#ifdef RFID_ENABLED
    const char* rfidEnabledStr = "true";
#else
    const char* rfidEnabledStr = "false";
#endif

    // Friendly node name from NVS — empty string until the operator sets one.
    // Always emitted so Node-RED's roster flow can label by name when present
    // and fall back to device_id when "".
    const char* nodeName = gAppConfig.node_name;

    // Wi-Fi channel — emitted so Node-RED can warn if anchors are on different
    // channels (ESP-NOW silently fails across channels on a shared radio).
    uint8_t wifiCh = (uint8_t)WiFi.channel();

    // Anchor snippet (F5) — only non-empty when this node is configured as a
    // fixed anchor. Node-RED reads the retained .../status boot announcement
    // to build the anchor registry for the 2-D map.
    // Boot-reason snippet (v0.3.33) — only emitted on the boot event so the
    // fleet manager can correlate the reset cause without bloating heartbeats.
    char anchorSnip[160] = "";
    int anchorOff = 0;
    if (gAppConfig.anchor_role == 1) {
        anchorOff = snprintf(anchorSnip, sizeof(anchorSnip),
            ",\"anchor\":{\"role\":\"anchor\",\"x_m\":%.3f,\"y_m\":%.3f,\"z_m\":%.3f}",
            gAppConfig.anchor_x_mm / 1000.0f,
            gAppConfig.anchor_y_mm / 1000.0f,
            gAppConfig.anchor_z_mm / 1000.0f);
        if (anchorOff < 0 || anchorOff >= (int)sizeof(anchorSnip)) anchorOff = 0;
    }
    if (strcmp(event, "boot") == 0 && anchorOff < (int)sizeof(anchorSnip) - 32) {
        snprintf(anchorSnip + anchorOff, sizeof(anchorSnip) - anchorOff,
            ",\"boot_reason\":\"%s\"", bootReasonStr(_firstBootReason));
    }

    // (#53) Heap snapshot — sampled at publish time so heartbeat consumers
    // can plot heap-free trajectory per device and surface slow leaks
    // BEFORE they manifest as panic. Cheap to call (~µs each).
    uint32_t heapFree    = ESP.getFreeHeap();
    uint32_t heapLargest = ESP.getMaxAllocHeap();

    char buf[640];
    int n;
    if (extraJson && *extraJson) {
        n = snprintf(buf, sizeof(buf),
            "{\"device_id\":\"%s\","
            "\"node_name\":\"%s\","
            "\"mac\":\"%s\","
            "\"firmware_version\":\"" FIRMWARE_VERSION "\","
            "\"firmware_ts\":%u,"
            "\"uptime_s\":%u,"
            "\"rfid_enabled\":%s,"
            "\"wifi_channel\":%u,"
            "\"heap_free\":%u,"
            "\"heap_largest\":%u,"
            "\"event\":\"%s\","
            "%s%s}",
            DeviceId::get().c_str(),
            nodeName,
            DeviceId::getMac().c_str(),
            (unsigned)(uint32_t)FIRMWARE_BUILD_TIMESTAMP,
            (unsigned)(millis() / 1000),
            rfidEnabledStr,
            (unsigned)wifiCh,
            (unsigned)heapFree,
            (unsigned)heapLargest,
            event, extraJson, anchorSnip);
    } else {
        n = snprintf(buf, sizeof(buf),
            "{\"device_id\":\"%s\","
            "\"node_name\":\"%s\","
            "\"mac\":\"%s\","
            "\"firmware_version\":\"" FIRMWARE_VERSION "\","
            "\"firmware_ts\":%u,"
            "\"uptime_s\":%u,"
            "\"rfid_enabled\":%s,"
            "\"wifi_channel\":%u,"
            "\"heap_free\":%u,"
            "\"heap_largest\":%u,"
            "\"event\":\"%s\"%s}",
            DeviceId::get().c_str(),
            nodeName,
            DeviceId::getMac().c_str(),
            (unsigned)(uint32_t)FIRMWARE_BUILD_TIMESTAMP,
            (unsigned)(millis() / 1000),
            rfidEnabledStr,
            (unsigned)wifiCh,
            (unsigned)heapFree,
            (unsigned)heapLargest,
            event, anchorSnip);
    }
    if (n < 0 || n >= (int)sizeof(buf)) {
        LOG_W("MQTT", "status payload truncated (event=%s, wanted=%d)", event, n);
    }

    // The boot announcement is published as retained QoS 1 so that any broker
    // subscriber (e.g. a Node-RED flow) that comes online after the device boots
    // still sees the last known status. All other events are non-retained.
    bool retain = (strcmp(event, "boot") == 0);
    mqttPublish("status", String(buf), 1, retain);
}

// Typed overload — converts a FwEvent enum value to its string representation
// and delegates to the existing const char* version.
// Prefer this overload at all new call sites to prevent typos.
static void mqttPublishStatus(FwEvent ev, const char* extraJson = nullptr) {
    mqttPublishStatus(fwEventName(ev), extraJson);
}


// Serialise a JsonDocument and publish it to <prefix>.
// Eliminates the repeated JsonDocument → String → mqttPublish() boilerplate
// that exists at 10+ call sites across ble.h, espnow_ranging.h, and rfid.h.
static void mqttPublishJson(const char* prefix, JsonDocument& doc,
                            uint8_t qos = 0, bool retain = false) {
    if (!_mqttClient.connected()) return;
    String payload;
    serializeJson(doc, payload);
    mqttPublish(prefix, payload, qos, retain);
}


// ── LED state publish helper ───────────────────────────────────────────────────
// Called by ws2812PublishState() (defined in ws2812.h) to publish current strip
// state to .../status/led as a retained QoS 1 message.
// ws2812PublishState() is the outward-facing function; this helper is the MQTT
// transport layer so ws2812.h stays decoupled from MQTT internals.
static void mqttPublishLedState(const char* state,
                                uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness, uint8_t count) {
    // snprintf on a stack buffer — same rationale as mqttPublishStatus().
    // 192 B covers the maximum-length field values with headroom.
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\","
        "\"r\":%u,\"g\":%u,\"b\":%u,"
        "\"brightness\":%u,\"count\":%u,"
        "\"uptime_s\":%u}",
        state,
        (unsigned)r, (unsigned)g, (unsigned)b,
        (unsigned)brightness, (unsigned)count,
        (unsigned)(millis() / 1000));
    if (n < 0 || n >= (int)sizeof(buf)) {
        LOG_W("MQTT", "led state payload truncated");
    }
    mqttPublish("status/led", String(buf), 1, true);   // QoS 1, retained
}


// ── LED command handler ────────────────────────────────────────────────────────
// Called from onMqttMessage when a message arrives on .../cmd/led.
// Parses the JSON command and posts the appropriate event to the WS2812 task.
static void handleLedCommand(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        LOG_W("MQTT", "cmd/led: JSON parse error");
        return;
    }
    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "color") == 0) {
        LedEvent e{};
        e.type = LedEventType::MQTT_COLOR;
        e.r    = (uint8_t)constrain(doc["r"] | 0, 0, 255);
        e.g    = (uint8_t)constrain(doc["g"] | 0, 0, 255);
        e.b    = (uint8_t)constrain(doc["b"] | 0, 0, 255);
        ws2812PostEvent(e);
        ws2812PublishState();

    } else if (strcmp(cmd, "brightness") == 0) {
        LedEvent e{};
        e.type       = LedEventType::MQTT_BRIGHTNESS;
        e.brightness = (uint8_t)constrain(doc["value"] | LED_MAX_BRIGHTNESS, 1, 255);
        ws2812PostEvent(e);
        ws2812PublishState();

    } else if (strcmp(cmd, "animation") == 0) {
        LedEvent e{};
        e.type = LedEventType::MQTT_ANIMATION;
        strlcpy(e.animName, doc["name"] | "solid", sizeof(e.animName));
        ws2812PostEvent(e);

    } else if (strcmp(cmd, "count") == 0) {
        LedEvent e{};
        e.type  = LedEventType::MQTT_COUNT;
        e.count = (uint8_t)constrain(doc["value"] | LED_DEFAULT_COUNT,
                                     1, LED_MAX_NUM_LEDS);
        ws2812PostEvent(e);

    } else if (strcmp(cmd, "off") == 0) {
        LedEvent e{};
        e.type = LedEventType::MQTT_OFF;
        ws2812PostEvent(e);
        ws2812PublishState();

    } else if (strcmp(cmd, "reset") == 0) {
        LedEvent e{};
        e.type = LedEventType::RESET;
        ws2812PostEvent(e);
        ws2812PublishState();

    } else {
        LOG_W("MQTT", "cmd/led: unknown cmd '%s'", cmd);
    }
}


// Normalise an RFID UID: strip ':' and '-' separators, uppercase the rest.
// `out` is always null-terminated. Used for both `add` and `remove` so the
// same input format is accepted for either operation — previously the two
// call sites had identical inline blocks that had to be kept in sync.
static void rfidNormalizeUid(const char* in, char* out, size_t outLen) {
    if (outLen == 0) return;
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < outLen; i++) {
        char c = in[i];
        if (c == ':' || c == '-') continue;
        out[j++] = (char)toupper((unsigned char)c);
    }
    out[j] = '\0';
}


// ── RFID whitelist handler ────────────────────────────────────────────────────
// Called from onMqttMessage when a message arrives on .../cmd/rfid/whitelist.
// All UIDs are normalised (uppercase, no separators) before processing.
static void handleRfidWhitelist(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        LOG_W("MQTT", "cmd/rfid/whitelist: JSON parse error");
        return;
    }
    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "add") == 0) {
        const char* raw = doc["uid"] | "";
        char uid[RFID_UID_STR_LEN] = {};
        rfidNormalizeUid(raw, uid, sizeof(uid));
        bool ok = rfidWhitelistAdd(uid);
        LOG_I("MQTT", "rfid/whitelist add %s: %s", uid, ok ? "ok" : "full");
        String resp = ok
            ? ("{\"event\":\"rfid_whitelist\",\"cmd\":\"add\",\"uid\":\"" + String(uid) + "\",\"result\":\"ok\"}")
            : ("{\"event\":\"rfid_whitelist\",\"cmd\":\"add\",\"uid\":\"" + String(uid) + "\",\"result\":\"error\",\"reason\":\"list full\"}");
        mqttPublish("response", resp);

    } else if (strcmp(cmd, "remove") == 0) {
        const char* raw = doc["uid"] | "";
        char uid[RFID_UID_STR_LEN] = {};
        rfidNormalizeUid(raw, uid, sizeof(uid));
        bool ok = rfidWhitelistRemove(uid);
        LOG_I("MQTT", "rfid/whitelist remove %s: %s", uid, ok ? "ok" : "not found");
        String resp = ok
            ? ("{\"event\":\"rfid_whitelist\",\"cmd\":\"remove\",\"uid\":\"" + String(uid) + "\",\"result\":\"ok\"}")
            : ("{\"event\":\"rfid_whitelist\",\"cmd\":\"remove\",\"uid\":\"" + String(uid) + "\",\"result\":\"error\",\"reason\":\"not found\"}");
        mqttPublish("response", resp);

    } else if (strcmp(cmd, "clear") == 0) {
        rfidWhitelistClear();
        LOG_I("MQTT", "rfid/whitelist cleared");
        mqttPublish("response",
            String("{\"event\":\"rfid_whitelist\",\"cmd\":\"clear\",\"result\":\"ok\"}"));

    } else if (strcmp(cmd, "list") == 0) {
        char buf[RFID_MAX_WHITELIST][RFID_UID_STR_LEN] = {};
        uint8_t count = 0;
        rfidWhitelistList(buf, count);
        String resp = "{\"event\":\"rfid_whitelist\",\"cmd\":\"list\","
                      "\"count\":" + String(count) + ",\"uids\":[";
        for (uint8_t i = 0; i < count; i++) {
            if (i > 0) resp += ",";
            resp += "\"";
            resp += buf[i];
            resp += "\"";
        }
        resp += "]}";
        mqttPublish("response", resp);

    } else {
        LOG_W("MQTT", "cmd/rfid/whitelist: unknown cmd '%s'", cmd);
    }
}


#ifdef RFID_ENABLED
// ── RFID playground handlers (v0.3.17) ────────────────────────────────────────
// cmd/rfid/program     arms a multi-block write on the NEXT card scanned
// cmd/rfid/read_block  arms a one-shot block read on the NEXT card scanned
// cmd/rfid/cancel      clears any armed request and returns the reader to idle
//
// All three publish their outcome on the .../response topic. The schema
// (status values, request_id pairing) is documented in docs/rfid_tag_profiles.md
// and enforced by the response publishers in rfid.h.
static void handleRfidProgram(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        LOG_W("MQTT", "cmd/rfid/program: JSON parse error");
        return;
    }

    RfidProgramRequest req = {};
    strlcpy(req.profile, doc["profile"] | RFID_PROFILE_MIFARE_CLASSIC_1K,
            sizeof(req.profile));
    strlcpy(req.request_id, doc["request_id"] | "", sizeof(req.request_id));
    req.timeout_ms = doc["timeout_ms"] | 0u;

    JsonArray writes = doc["writes"].as<JsonArray>();
    uint8_t n = 0;
    if (writes) {
        for (JsonVariant v : writes) {
            if (n >= RFID_MAX_WRITE_BLOCKS) break;
            RfidWriteBlock& w = req.writes[n];
            w.block = (uint16_t)(v["block"] | 0);

            const char* dataHex = v["data_hex"] | "";
            uint8_t blockSize = rfidProfileBlockSize(req.profile);
            size_t decoded = rfidHexDecode(dataHex, w.data, sizeof(w.data));
            if (decoded != blockSize) {
                LOG_W("MQTT", "cmd/rfid/program: writes[%u] data_hex length %u != "
                              "profile block size %u — rejecting batch",
                      (unsigned)n, (unsigned)decoded, (unsigned)blockSize);
                return;
            }

            const char* keyHex = v["key_a_hex"] | "";
            if (keyHex && *keyHex) {
                if (rfidHexDecode(keyHex, w.keyA, sizeof(w.keyA)) != RFID_KEY_SIZE) {
                    LOG_W("MQTT", "cmd/rfid/program: writes[%u] key_a_hex must be 12 hex chars",
                          (unsigned)n);
                    return;
                }
                w.has_key_a = true;
            } else {
                w.has_key_a = false;
            }
            n++;
        }
    }
    if (n == 0) {
        LOG_W("MQTT", "cmd/rfid/program: no writes[] entries — rejecting");
        return;
    }
    req.write_count = n;

    LOG_I("MQTT", "cmd/rfid/program: arming %u block(s) on %s (req=%s)",
          (unsigned)n, req.profile, req.request_id);
    rfidArmProgram(req);
}

static void handleRfidReadBlock(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        LOG_W("MQTT", "cmd/rfid/read_block: JSON parse error");
        return;
    }

    RfidReadRequest req = {};
    strlcpy(req.profile, doc["profile"] | RFID_PROFILE_MIFARE_CLASSIC_1K,
            sizeof(req.profile));
    strlcpy(req.request_id, doc["request_id"] | "", sizeof(req.request_id));
    req.timeout_ms = doc["timeout_ms"] | 0u;
    req.block      = (uint16_t)(doc["block"] | 0);

    const char* keyHex = doc["key_a_hex"] | "";
    if (keyHex && *keyHex) {
        if (rfidHexDecode(keyHex, req.keyA, sizeof(req.keyA)) != RFID_KEY_SIZE) {
            LOG_W("MQTT", "cmd/rfid/read_block: key_a_hex must be 12 hex chars");
            return;
        }
        req.has_key_a = true;
    }

    LOG_I("MQTT", "cmd/rfid/read_block: arming block %u on %s (req=%s)",
          (unsigned)req.block, req.profile, req.request_id);
    rfidArmRead(req);
}

static void handleRfidCancel(const char* /*payload*/, size_t /*len*/) {
    LOG_I("MQTT", "cmd/rfid/cancel: clearing any armed request");
    rfidCancelPending();
}

// cmd/rfid/ndef_write — encode a URI as an NDEF Type-2 message and arm a
// multi-page Ultralight write starting at page 4 (the user-data region).
// Reuses the existing rfidArmProgram path so the response schema, timeout
// handling, and trailer guard are identical.
//
// Payload: {"uri":"https://example.com","request_id":"...","timeout_ms":15000}
//
// Length budget: 32 pages × 4 B = 128 B, of which 4 B are TLV/terminator
// overhead and 5 B record header — leaves ~119 B for the URI body. With the
// "https://" prefix abbreviation that covers URLs up to ~119 chars, which
// fits virtually every real-world deep-link.
static void handleRfidNdefWrite(const char* payload, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        LOG_W("MQTT", "cmd/rfid/ndef_write: JSON parse error");
        return;
    }
    const char* uri = doc["uri"] | "";
    if (!*uri) {
        LOG_W("MQTT", "cmd/rfid/ndef_write: missing 'uri' field");
        return;
    }

    uint8_t buf[RFID_MAX_WRITE_BLOCKS * 4 + 4] = {0};   // +4 padding headroom
    size_t n = ndefBuildUri(uri, buf, RFID_MAX_WRITE_BLOCKS * 4);
    if (n == 0) {
        LOG_W("MQTT", "cmd/rfid/ndef_write: encoded NDEF too large for tag (max %u B)",
              (unsigned)(RFID_MAX_WRITE_BLOCKS * 4));
        return;
    }

    // Pad to 4-byte page boundary; pages must be written whole.
    size_t pageBytes = (n + 3) & ~3u;
    uint8_t pageCount = (uint8_t)(pageBytes / 4);

    RfidProgramRequest req = {};
    strlcpy(req.profile, RFID_PROFILE_NTAG21X, sizeof(req.profile));
    strlcpy(req.request_id, doc["request_id"] | "", sizeof(req.request_id));
    req.timeout_ms = doc["timeout_ms"] | 0u;
    req.write_count = pageCount;
    for (uint8_t p = 0; p < pageCount; p++) {
        req.writes[p].block = (uint16_t)(4 + p);     // NTAG21x user data starts at page 4
        memcpy(req.writes[p].data, buf + p * 4, 4);
        req.writes[p].has_key_a = false;
    }

    LOG_I("MQTT", "cmd/rfid/ndef_write: %u-page NDEF (%u B) arming for '%s' (req=%s)",
          (unsigned)pageCount, (unsigned)n, uri, req.request_id);
    rfidArmProgram(req);
}
#endif // RFID_ENABLED


// ── Credential rotation handler ────────────────────────────────────────────────
// Called when a message arrives on .../cmd/cred_rotate or the broadcast topic.
// The payload is a raw binary AES-128-GCM encrypted CredentialBundle:
//   [nonce 12B][ciphertext][tag 16B]
// It is decrypted using the rotation_key stored in the active credential bundle.
//
// Security checks:
//   1. GCM authentication tag must match (rejects tampered or wrongly-keyed payloads)
//   2. Timestamp in the new bundle must be strictly newer than the current one
//      (prevents replay attacks — replaying an old rotation has no effect)
static void handleCredRotation(const char* payload, size_t len) {
    LOG_I("MQTT", "Credential rotation message received");

    // Minimum valid size: nonce (12) + at least 1 byte of ciphertext + tag (16)
    if (len <= GCM_NONCE_LEN + GCM_TAG_LEN) {
        LOG_W("MQTT", "Rotation payload too short - ignoring");
        return;
    }

    // Copy the rotation key from the active bundle — we need it to decrypt
    uint8_t aesKey[AES_KEY_LEN];
    memcpy(aesKey, _mqttBundle.rotation_key, AES_KEY_LEN);

    // Attempt to decrypt. cryptoDecrypt returns false if the GCM tag is wrong,
    // meaning either the wrong key was used or the payload was tampered with.
    uint8_t plaintext[BUNDLE_PLAINTEXT_MAX];
    size_t  plaintextLen = 0;
    if (!cryptoDecrypt(aesKey, (const uint8_t*)payload, len, plaintext, plaintextLen)) {
        LOG_W("MQTT", "Rotation decryption/auth failed - ignoring");
        memset(aesKey, 0, AES_KEY_LEN);                         // Wipe key from RAM even on failure
        mqttPublishStatus(FwEvent::CRED_ROTATE_REJECTED);        // Let Node-RED know it was rejected
        return;
    }
    memset(aesKey, 0, AES_KEY_LEN);   // Wipe key from RAM immediately after use

    // Verify the decrypted data is exactly the size of a CredentialBundle
    if (plaintextLen != sizeof(CredentialBundle)) {
        LOG_W("MQTT", "Rotation bundle size mismatch - ignoring");
        return;
    }

    // Copy decrypted bytes into a CredentialBundle struct
    CredentialBundle newBundle;
    memcpy(&newBundle, plaintext, sizeof(CredentialBundle));
    memset(plaintext, 0, sizeof(plaintext));   // Wipe plaintext from RAM

    // Reject the rotation if the new bundle's timestamp is not newer.
    // This prevents an attacker who captured a previous rotation message from
    // replaying it to roll credentials back to an older version.
    if (newBundle.timestamp <= _mqttBundle.timestamp) {
        LOG_W("MQTT", "Rotation ignored - new ts %llu <= current ts %llu",
              newBundle.timestamp, _mqttBundle.timestamp);
        return;
    }

    // Save the new bundle to NVS flash so it persists across restarts
    if (!CredentialStore::save(newBundle)) {
        LOG_E("MQTT", "Failed to write rotated credentials to NVS");
        return;
    }

    // Announce success. Schedule a deferred restart so the MQTT publish has
    // time to be sent without blocking inside the message callback.
    // delay() here would stall AsyncMqttClient's keepalive loop and may cause
    // it to drop the connection before the PUBACK is received.
    LOG_I("MQTT", "Credentials rotated successfully - restarting in 600 ms");
    mqttPublishStatus(FwEvent::CRED_ROTATED);
    _mqttRestartAtMs = millis() + 600;   // mqttHeartbeat() drives the deferred restart
}


// Forward declarations — defined in espnow_ranging.h, included AFTER mqtt_client.h.
void espnowRangingSetEnabled(bool en);
void espnowCalibrateCmd(const char* payload, size_t len);
void espnowSetFilter(const char* payload, size_t len);
void espnowSetTrackedMacs(const char** macs, uint8_t n);


// ── Sleep helpers (v0.3.20) ──────────────────────────────────────────────────
// Three distinct paths:
//
//   1. mqttEnterSleep(sec, LIGHT) — esp_light_sleep_start(). CPU halts, radio
//      is torn down, RAM is preserved. Post-wake control returns to the caller
//      inside mqttHeartbeat(); we re-init Wi-Fi and the existing loop() state
//      machine handles MQTT reconnect.
//
//   2. mqttEnterSleep(sec, DEEP)  — esp_deep_sleep_start(). Cold boot on wake.
//      Never returns.
//
//   3. mqttEnterModemSleep(sec)   — WIFI_PS_MAX_MODEM + CPU freq drop. CPU
//      keeps running, MQTT session stays alive. mqttExitModemSleep() reverts
//      when the timer expires (checked every tick in mqttHeartbeat()).
//
// CRITICAL: AsyncMqttClient's TCP socket cannot survive a multi-second light
// sleep — lwIP silently drops the connection. For LIGHT and DEEP we explicitly
// disconnect(true) before putting the radio to sleep so the LWT doesn't fire
// mid-sleep and the broker doesn't see a half-open session.
static void mqttEnterSleep(uint32_t seconds, SleepKind kind) {
    char extra[48];
    snprintf(extra, sizeof(extra), "\"duration_s\":%u", (unsigned)seconds);
    FwEvent ev = (kind == SleepKind::DEEP) ? FwEvent::DEEP_SLEEPING
                                           : FwEvent::SLEEPING;
    mqttPublishStatus(ev, extra);

    // Drain the QoS-1 publish (AsyncMqttClient is non-blocking).
    // 200 ms is empirically enough on a LAN broker.
    uint32_t drainUntil = millis() + 200;
    while ((int32_t)(millis() - drainUntil) < 0) { delay(10); }

    // Clean teardown — prevents LWT from firing during sleep and avoids a
    // half-open session server-side.
    _mqttClient.disconnect(true);
    delay(50);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

    if (kind == SleepKind::DEEP) {
        LOG_I("MQTT", "Entering deep sleep for %u s (cold boot on wake)",
              (unsigned)seconds);
        esp_deep_sleep_start();   // Never returns — wake is a full reboot
    } else {
        LOG_I("MQTT", "Entering light sleep for %u s", (unsigned)seconds);
        esp_light_sleep_start();  // Blocks until timer fires
        LOG_I("MQTT", "Light sleep wake — re-initialising Wi-Fi");

        // Post-wake: re-init Wi-Fi using the cached credential bundle. The
        // main.cpp loop() state machine will detect the re-association and
        // the existing AsyncMqttClient reconnect timer will re-establish MQTT.
        WiFi.mode(WIFI_STA);
        if (_mqttBundle.wifi_ssid[0] != '\0') {
            WiFi.begin(_mqttBundle.wifi_ssid, _mqttBundle.wifi_password);
        }
    }
}

static void mqttEnterModemSleep(uint32_t seconds) {
    char extra[48];
    snprintf(extra, sizeof(extra), "\"duration_s\":%u", (unsigned)seconds);
    mqttPublishStatus(FwEvent::MODEM_SLEEPING, extra);

    // WIFI_PS_MAX_MODEM — the radio sleeps between DTIM beacons (~300 ms
    // intervals). Wi-Fi stays associated with the AP; MQTT stays connected.
    // The broker never notices. Power saving is ~20-40 mA from the radio
    // alone; dynamic CPU freq scaling could double that but would pull in
    // the esp_pm driver (~1 KB IRAM) which pushes the firmware over the
    // IRAM budget. If power savings become critical, revisit with a custom
    // sdkconfig bump to iram0_0_seg.
    WiFi.setSleep(WIFI_PS_MAX_MODEM);

    _mqttModemSleepUntilMs = millis() + seconds * 1000;
    LOG_I("MQTT", "Modem sleep armed for %u s (Wi-Fi PS_MAX)",
          (unsigned)seconds);
}

static void mqttExitModemSleep() {
    WiFi.setSleep(WIFI_PS_NONE);
    _mqttModemSleepUntilMs = 0;
    // Fresh heartbeat so Node-RED flips the card back to Connected.
    mqttPublishStatus(FwEvent::HEARTBEAT);
    LOG_I("MQTT", "Modem sleep expired — restored full power");
}

// ── Message receive callback ───────────────────────────────────────────────────
// Called by AsyncMqttClient whenever a subscribed topic receives a message.
// Routes the message to the appropriate handler based on the topic string.
static void onMqttMessage(char* topic, char* payload,
                          AsyncMqttClientMessageProperties props,
                          size_t len,   // Payload length (may not be null-terminated)
                          size_t index, // Byte offset for fragmented messages
                          size_t total) // Total expected bytes (for fragmented messages)
{
    String t(topic);   // Convert C-string to Arduino String for easy comparison

    // Pre-build the topics we care about for this device
    String rotTopic   = mqttTopic("cmd/cred_rotate");   // Device-specific rotation
    String bcastTopic = mqttBroadcastRotateTopic();      // Site-wide broadcast rotation
    String otaTopic   = mqttTopic("cmd/ota_check");      // OTA trigger

    // Ignore partial fragments — only process once the final fragment arrives.
    // AsyncMqttClient may split large payloads (e.g. encrypted rotation bundles)
    // across multiple callbacks; acting on a fragment would pass a truncated buffer
    // to handleCredRotation and fail the GCM auth tag check.
    if (index + len < total) return;

    if (t == rotTopic || t == bcastTopic) {
        // Credential rotation request — decrypt and apply
        handleCredRotation(payload, len);
    } else if (t == mqttTopic("cmd")) {
        // General application command — extend this block to handle
        // device-specific commands (e.g. LED control, sensor reads)
        LOG_D("MQTT", "cmd received: %.*s", (int)len, payload);
    } else if (t == otaTopic) {
        // OTA check triggered remotely from Node-RED or another MQTT client
        LOG_I("MQTT", "OTA check triggered via MQTT");
        otaCheckNow();   // Defined in ota.h (included after this file)
    } else if (t == mqttTopic("cmd/config_mode")) {
        // Start the settings HTTP portal on the device LAN IP.
        // Admin can then browse to http://<device-ip>/settings to update
        // GitHub and MQTT settings without entering AP mode.
        LOG_I("MQTT", "Config mode triggered - starting settings portal");
        mqttPublishStatus(FwEvent::CONFIG_MODE_ACTIVE,
            (String("\"settings_url\":\"https://") + WiFi.localIP().toString() + "/settings\"").c_str());
        settingsServerStart();
    } else if (t == mqttTopic("cmd/led")) {
        // WS2812B LED strip control
        handleLedCommand(payload, len);
    } else if (t == mqttTopic("cmd/locate")) {
        // Status LED locate flash — 4 s non-blocking overlay, auto-reverts.
        // Payload is ignored; any publish arms the flash.
        LOG_I("MQTT", "cmd/locate received - flashing status LED");
        ledSetPattern(LedPattern::LOCATE);
        mqttPublishStatus(FwEvent::LOCATING);
    } else if (t == mqttTopic("cmd/rfid/whitelist")) {
        // RFID UID whitelist management
        handleRfidWhitelist(payload, len);
#ifdef RFID_ENABLED
    } else if (t == mqttTopic("cmd/rfid/program")) {
        // v0.3.17 playground: arm a multi-block write on the next scanned card
        handleRfidProgram(payload, len);
    } else if (t == mqttTopic("cmd/rfid/read_block")) {
        // v0.3.17 playground: arm a one-shot block read on the next scanned card
        handleRfidReadBlock(payload, len);
    } else if (t == mqttTopic("cmd/rfid/cancel")) {
        // v0.3.17 playground: clear any pending program/read arm
        handleRfidCancel(payload, len);
    } else if (t == mqttTopic("cmd/rfid/ndef_write")) {
        // v0.4.11: encode URI as NDEF + arm multi-page write on next NTAG21x
        handleRfidNdefWrite(payload, len);
#endif
#ifdef BLE_ENABLED
    } else if (t == mqttTopic("cmd/ble/scan")) {
        bleTriggerScan();
    } else if (t == mqttTopic("cmd/ble/clear")) {
        bleClearTracked();
    } else if (t == mqttTopic("cmd/ble/list")) {
        blePublishList();
    } else if (t == mqttTopic("cmd/ble/track")) {
        JsonDocument doc;
        DeserializationError jsonErr = deserializeJson(doc, payload, len);
        if (jsonErr != DeserializationError::Ok) {
            LOG_W("MQTT", "cmd/ble/track: JSON parse error - %s", jsonErr.c_str());
        } else {
            const char* macPtrs[BLE_MAX_TRACKED];
            uint8_t count = 0;
            JsonArray macsArr = doc["macs"].as<JsonArray>();
            if (macsArr) {
                // {"macs": ["AA:BB:...", "CC:DD:..."]}
                for (JsonVariant v : macsArr) {
                    const char* m = v.as<const char*>();
                    if (m && strlen(m) == 17 && count < BLE_MAX_TRACKED)
                        macPtrs[count++] = m;
                }
            } else {
                // Backward compat: {"mac": "AA:BB:..."}
                const char* m = doc["mac"];
                if (m && strlen(m) == 17)
                    macPtrs[count++] = m;
            }
            if (count > 0) bleSetTrackedMacs(macPtrs, count);
        }
#endif
    } else if (t == mqttTopic("cmd/espnow/ranging")) {
        // Enable or disable ESP-NOW ranging from Node-RED.
        // Payload "1" = enable, "0" (or anything else) = disable.
        // Node-RED publishes with retain=true so the device re-receives the
        // state on reconnect and doesn't silently revert to disabled after reboot.
        bool enable = (len > 0 && payload[0] == '1');
        espnowRangingSetEnabled(enable);
        LOG_I("MQTT", "ESP-NOW ranging %s via MQTT", enable ? "enabled" : "disabled");
    } else if (t == mqttTopic("cmd/espnow/name")) {
        // Set the friendly per-device name. Payload: {"name":"Alpha"}.
        // Validates against [A-Za-z0-9_-]{1,15}; silently rejects invalid input
        // so a malformed retained message can't brick the name field.
        // Persisted via AppConfigStore::save() — Node-RED publishes retained so
        // a reboot or reconnect re-applies the same name without operator action.
        char buf[64];
        size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';

        JsonDocument doc;
        if (deserializeJson(doc, buf)) {
            LOG_W("MQTT", "cmd/espnow/name: bad JSON");
            return;
        }
        const char* name = doc["name"] | "";
        size_t nameLen = strlen(name);
        if (nameLen == 0 || nameLen >= APP_CFG_NODE_NAME_LEN) {
            LOG_W("MQTT", "cmd/espnow/name: length %u out of [1..%u]",
                  (unsigned)nameLen, (unsigned)(APP_CFG_NODE_NAME_LEN - 1));
            return;
        }
        for (size_t i = 0; i < nameLen; i++) {
            char c = name[i];
            bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!valid) {
                LOG_W("MQTT", "cmd/espnow/name: invalid char '%c'", c);
                return;
            }
        }

        // No-op if unchanged so we don't churn NVS on every retained replay.
        if (strcmp(gAppConfig.node_name, name) == 0) return;

        AppConfig copy = gAppConfig;
        strncpy(copy.node_name, name, APP_CFG_NODE_NAME_LEN - 1);
        copy.node_name[APP_CFG_NODE_NAME_LEN - 1] = '\0';
        if (AppConfigStore::save(copy)) {
            LOG_I("MQTT", "Node name set to '%s'", gAppConfig.node_name);
            mqttPublishStatus("name_changed");
        } else {
            LOG_E("MQTT", "cmd/espnow/name: NVS save failed");
        }
    } else if (t == mqttTopic("cmd/espnow/calibrate")) {
        // Two-point calibration wizard. Drives the state machine in espnow_ranging.h.
        // Not retained — each cmd/espnow/calibrate message is a one-shot action.
        espnowCalibrateCmd(payload, len);
    } else if (t == mqttTopic("cmd/espnow/filter")) {
        // Update EMA + outlier settings. Retain=true on broker side; the device
        // re-applies the filter on reconnect without operator re-entry.
        espnowSetFilter(payload, len);
    } else if (t == mqttTopic("cmd/espnow/track")) {
        // MAC publish filter — only peers in the list appear in .../espnow publishes.
        // Empty "macs":[] clears the filter and reverts to publish-all.
        // Observations continue for all peers regardless of the filter.
        char buf[256];
        size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';
        JsonDocument doc;
        if (deserializeJson(doc, buf) == DeserializationError::Ok) {
            const char* macPtrs[ESPNOW_MAX_TRACKED];
            uint8_t count = 0;
            JsonArray macsArr = doc["macs"].as<JsonArray>();
            if (macsArr) {
                for (JsonVariant v : macsArr) {
                    const char* m = v.as<const char*>();
                    if (m && strlen(m) == 17 && count < ESPNOW_MAX_TRACKED)
                        macPtrs[count++] = m;
                }
            }
            espnowSetTrackedMacs(macPtrs, count);
        }
    } else if (t == mqttTopic("cmd/espnow/anchor")) {
        // Set this node's role and fixed-anchor coordinates.
        // {"role":"anchor","x_m":0.0,"y_m":0.0,"z_m":0.0} — fixed position
        // {"role":"mobile"} — clear anchor role and coordinates
        char buf[192];
        size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';
        JsonDocument doc;
        if (deserializeJson(doc, buf) != DeserializationError::Ok) {
            LOG_W("MQTT", "cmd/espnow/anchor: bad JSON"); return;
        }
        const char* role = doc["role"] | "mobile";
        AppConfig copy = gAppConfig;
        copy.anchor_role = (strcmp(role, "anchor") == 0) ? 1 : 0;
        if (copy.anchor_role == 1) {
            copy.anchor_x_mm = (int32_t)(((float)(doc["x_m"] | 0.0f)) * 1000.0f);
            copy.anchor_y_mm = (int32_t)(((float)(doc["y_m"] | 0.0f)) * 1000.0f);
            copy.anchor_z_mm = (int32_t)(((float)(doc["z_m"] | 0.0f)) * 1000.0f);
        } else {
            copy.anchor_x_mm = 0; copy.anchor_y_mm = 0; copy.anchor_z_mm = 0;
        }
        if (AppConfigStore::save(copy)) {
            LOG_I("MQTT", "Anchor role=%u pos=(%.2f, %.2f, %.2f) m",
                  copy.anchor_role,
                  copy.anchor_x_mm / 1000.0f,
                  copy.anchor_y_mm / 1000.0f,
                  copy.anchor_z_mm / 1000.0f);
            mqttPublishStatus("anchor_updated");
        } else {
            LOG_E("MQTT", "cmd/espnow/anchor: NVS save failed");
        }
    } else if (t == mqttTopic("cmd/restart")) {
        // Deferred restart — publish status then let mqttHeartbeat() fire
        // ESP.restart() after 300 ms so the MQTT publish drains without
        // blocking inside this callback.
        LOG_I("MQTT", "Restart command received - restarting in 300 ms");
        mqttPublishStatus(FwEvent::RESTARTING);
        _mqttRestartAtMs = millis() + 300;
    } else if (t == mqttTopic("cmd/sleep") || t == mqttTopic("cmd/deep_sleep")) {
        // Deferred light / deep sleep. Payload: {"seconds":N} with
        // N in [MIN_SLEEP_SECONDS, MAX_SLEEP_SECONDS].
        // Dispatch is deferred SLEEP_DEFER_MS via mqttHeartbeat() so the
        // status publish drains before the radio goes off.
        bool isDeep = (t == mqttTopic("cmd/deep_sleep"));
        const char* label = isDeep ? "deep_sleep" : "sleep";

        // Copy payload into null-terminated buffer for ArduinoJson
        char buf[96];
        size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, buf);
        if (err) {
            LOG_W("MQTT", "cmd/%s: bad JSON (%s)", label, err.c_str());
            return;
        }
        int32_t secs = doc["seconds"] | -1;
        if (secs < (int32_t)MIN_SLEEP_SECONDS || secs > (int32_t)MAX_SLEEP_SECONDS) {
            LOG_W("MQTT", "cmd/%s: seconds=%d out of range [%u..%u]",
                  label, (int)secs,
                  (unsigned)MIN_SLEEP_SECONDS, (unsigned)MAX_SLEEP_SECONDS);
            return;
        }

        _mqttSleepDurationS = (uint32_t)secs;
        _mqttSleepKind      = isDeep ? SleepKind::DEEP : SleepKind::LIGHT;
        _mqttSleepAtMs      = millis() + SLEEP_DEFER_MS;
        LOG_I("MQTT", "cmd/%s accepted: %d s, dispatching in %u ms",
              label, (int)secs, (unsigned)SLEEP_DEFER_MS);
    } else if (t == mqttTopic("cmd/modem_sleep")) {
        // Modem sleep — CPU keeps running, radio stays associated.
        // Applied immediately (no deferred dispatch needed) because MQTT
        // remains connected throughout.
        char buf[96];
        size_t copyLen = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, buf);
        if (err) {
            LOG_W("MQTT", "cmd/modem_sleep: bad JSON (%s)", err.c_str());
            return;
        }
        int32_t secs = doc["seconds"] | -1;
        if (secs < (int32_t)MIN_SLEEP_SECONDS || secs > (int32_t)MAX_SLEEP_SECONDS) {
            LOG_W("MQTT", "cmd/modem_sleep: seconds=%d out of range [%u..%u]",
                  (int)secs, (unsigned)MIN_SLEEP_SECONDS, (unsigned)MAX_SLEEP_SECONDS);
            return;
        }

        // Stacking: if already modem-sleeping, re-arm the timer with the new
        // duration and publish a fresh status so Node-RED updates the countdown.
        mqttEnterModemSleep((uint32_t)secs);
    }
}


// ── onMqttConnect ─────────────────────────────────────────────────────────────
// Called by AsyncMqttClient after a successful broker connection (or reconnect).
// Sets up all subscriptions and sends the boot announcement.
static void onMqttConnect(bool sessionPresent) {
    LOG_I("MQTT", "Connected to broker");
    _mqttReconnectDelay     = 1000;   // Reset back-off delay after a successful connect
    _mqttReconnectCount     = 0;      // Clear failure counter
    _mqttNeedsRediscovery   = false;  // Clear rediscovery flag if loop() hadn't seen it yet
    _mqttConnectStartMs     = 0;      // Clear watchdog — we are no longer in a connecting state

    // Update sibling health advertisement — MQTT is now connected.
    // responderSetHealthFlag is defined in espnow_responder.h, which is included
    // before mqtt_client.h in esp32_firmware.ino.
    responderSetHealthFlag(1, true);
    ledSetPattern(LedPattern::MQTT_CONNECTED);   // 50ms pulse / 2s off — normal operational

    // (#44) Arm the MQTT_HEALTHY WS2812 event via the deferred-flag pattern.
    // Direct post from this callback (async_tcp task, Core 0) stalls the
    // ws2812 queue handshake long enough to trip the TWDT — the root cause
    // of the v0.4.10 fleet crash (#51). mqttHeartbeat() (loop task) consumes
    // _mqttLedHealthyAtMs and posts safely from a non-TWDT-subscribed context.
    _mqttLedHealthyAtMs = millis();

    // Subscribe to all command topics for this device.
    // QoS 1 = "at least once" delivery (safe for commands, may duplicate but won't lose)
    // QoS 2 = "exactly once" delivery (used for credential rotation to prevent double-apply)
    _mqttClient.subscribe(mqttTopic("cmd").c_str(),               1);   // General commands
    _mqttClient.subscribe(mqttTopic("cmd/cred_rotate").c_str(),   2);   // Device-specific rotation
    // QoS 0 chosen deliberately (since v0.2.15). On QoS 1 the broker
    // would re-deliver the message at the next reconnect because the
    // device reboots mid-OTA before sending PUBACK — re-delivery would
    // trigger another OTA check on the new firmware, potentially looping
    // through versions if the manifest still advertises an upgrade. QoS 0
    // is fire-and-forget: a missed publish just means we wait for the
    // next periodic OTA check (default 1 hour).
    _mqttClient.subscribe(mqttTopic("cmd/ota_check").c_str(),     0);   // QoS 0 — OTA reboots before PUBACK; re-delivery would trigger redundant check
    _mqttClient.subscribe(mqttTopic("cmd/config_mode").c_str(),   1);   // Start HTTP settings portal on LAN IP
    _mqttClient.subscribe(mqttBroadcastRotateTopic().c_str(),     2);   // Site-wide rotation
    _mqttClient.subscribe(mqttTopic("cmd/led").c_str(),           1);   // WS2812B LED strip control
    _mqttClient.subscribe(mqttTopic("cmd/locate").c_str(),        1);   // Status LED locate flash
    _mqttClient.subscribe(mqttTopic("cmd/rfid/whitelist").c_str(), 1);  // RFID whitelist management
#ifdef RFID_ENABLED
    _mqttClient.subscribe(mqttTopic("cmd/rfid/program").c_str(),    1); // RFID playground: arm write
    _mqttClient.subscribe(mqttTopic("cmd/rfid/read_block").c_str(), 1); // RFID playground: arm read
    _mqttClient.subscribe(mqttTopic("cmd/rfid/cancel").c_str(),     1); // RFID playground: cancel arm
    _mqttClient.subscribe(mqttTopic("cmd/rfid/ndef_write").c_str(), 1); // NDEF URI write (v0.4.11)
#endif
#ifdef BLE_ENABLED
    _mqttClient.subscribe(mqttTopic("cmd/ble/scan").c_str(),      1);   // BLE: trigger scan
    _mqttClient.subscribe(mqttTopic("cmd/ble/track").c_str(),     1);   // BLE: set tracked beacon
    _mqttClient.subscribe(mqttTopic("cmd/ble/clear").c_str(),     1);   // BLE: clear tracked beacon
    _mqttClient.subscribe(mqttTopic("cmd/ble/list").c_str(),      1);   // BLE: re-publish last results
#endif
    _mqttClient.subscribe(mqttTopic("cmd/espnow/ranging").c_str(),    1);  // ranging on/off — retain
    _mqttClient.subscribe(mqttTopic("cmd/espnow/name").c_str(),      1);  // friendly name — retain
    _mqttClient.subscribe(mqttTopic("cmd/espnow/calibrate").c_str(), 1);  // calibration wizard — QoS 1 (not retained; operator-initiated)
    _mqttClient.subscribe(mqttTopic("cmd/espnow/filter").c_str(),    1);  // EMA/outlier filter settings — QoS 1 + retain
    _mqttClient.subscribe(mqttTopic("cmd/espnow/track").c_str(),     1);  // MAC publish filter — retain
    _mqttClient.subscribe(mqttTopic("cmd/espnow/anchor").c_str(),    1);  // anchor role + coords — retain
    _mqttClient.subscribe(mqttTopic("cmd/restart").c_str(),          0);  // Remote restart — QoS 0 prevents re-delivery loop on reconnect
    _mqttClient.subscribe(mqttTopic("cmd/sleep").c_str(),          0);  // Light sleep — QoS 0; radio-off prevents PUBACK anyway
    _mqttClient.subscribe(mqttTopic("cmd/deep_sleep").c_str(),     0);  // Deep sleep — QoS 0; cold boot discards any session state
    _mqttClient.subscribe(mqttTopic("cmd/modem_sleep").c_str(),    1);  // Modem sleep — QoS 1 safe, session stays connected

    // Publish boot/online announcement (retained QoS 1).
    // First connect after power-on → BOOT (includes boot_reason field).
    // Subsequent reconnects → ONLINE (no boot_reason; avoids misleading #61).
    if (_firstMqttConnect) {
        _firstMqttConnect = false;
        mqttPublishStatus(FwEvent::BOOT);
        LOG_I("MQTT", "Boot announcement published (v" FIRMWARE_VERSION ")");
    } else {
        mqttPublishStatus(FwEvent::ONLINE);
        LOG_I("MQTT", "Online announcement published (reconnect)");
    }

    // (v0.3.34) If this is a post-OTA boot, announce OTA_VALIDATING and call
    // mark_app_valid_cancel_rollback() now — MQTT is healthy enough to publish
    // the boot announcement, which is the strongest signal we have that the
    // network stack works. ota_validation.h gates this so it's a no-op outside
    // the post-OTA window.
    otaValidationOnMqttConnect();
    otaValidationConfirmHealth();

    // Re-publish current LED strip state (retained) so Node-RED re-syncs after
    // a broker restart or reconnect without requiring a reboot.
    ws2812PublishState();
}


// ── onMqttDisconnect ──────────────────────────────────────────────────────────
// Called when the broker connection is lost (network drop, broker restart, etc.)
// Starts the reconnect timer instead of reconnecting immediately, to avoid
// hammering the broker during outages.
static const char* mqttDisconnectReasonStr(AsyncMqttClientDisconnectReason reason) {
    switch (reason) {
        case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED:             return "TCP_DISCONNECTED";
        case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION: return "UNACCEPTABLE_PROTOCOL_VERSION";
        case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED:    return "IDENTIFIER_REJECTED";
        case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE:     return "SERVER_UNAVAILABLE";
        case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS:  return "MALFORMED_CREDENTIALS";
        case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED:         return "NOT_AUTHORIZED";
        case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE:    return "NOT_ENOUGH_SPACE";
        case AsyncMqttClientDisconnectReason::TLS_BAD_FINGERPRINT:         return "TLS_BAD_FINGERPRINT";
        default:                                                            return "UNKNOWN";
    }
}

static void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    _mqttConnectStartMs = 0;   // Callback fired — client is not hung, just disconnected
    _mqttReconnectCount++;

    // Update sibling health advertisement — MQTT is no longer connected.
    responderSetHealthFlag(1, false);
    ledSetPattern(LedPattern::WIFI_CONNECTED);   // slow 2s/2s blink — Wi-Fi up, MQTT reconnecting

    // (#44 / #51 v0.4.10.1) BOOT_STATE "wifi" ws2812 post DISABLED for the same
    // reason documented in onMqttConnect above. Posting from the async_tcp
    // disconnect callback is the most likely watchdog-trigger path.
    if (_mqttReconnectCount == MQTT_REDISCOVERY_THRESHOLD) {
        _mqttNeedsRediscovery = true;   // Signals loop() to re-run broker discovery
    }

    LOG_W("MQTT", "Disconnected (%s) - retrying in %lu ms",
          mqttDisconnectReasonStr(reason), _mqttReconnectDelay);

    // Only start the timer if it isn't already running and OTA isn't active.
    // During OTA, reconnect attempts race with the OTA teardown and corrupt
    // AsyncMqttClient's internal state, crashing the FreeRTOS context switch.
    if (!_mqttOtaActive && xTimerIsTimerActive(_mqttReconnectTimer) == pdFALSE) {
        xTimerChangePeriod(_mqttReconnectTimer,
                           pdMS_TO_TICKS(_mqttReconnectDelay), 0);
        xTimerStart(_mqttReconnectTimer, 0);
    }

    // Double the delay for next time (exponential back-off), capped at MQTT_RECONNECT_MAX_MS.
    // This means: 1s → 2s → 4s → 8s → 16s → 32s → 60s → 60s → ...
    _mqttReconnectDelay = min(_mqttReconnectDelay * 2, (uint32_t)MQTT_RECONNECT_MAX_MS);
}


// ── mqttReconnectTimerCb ──────────────────────────────────────────────────────
// FreeRTOS timer callback — fires when the reconnect delay has elapsed.
// Only attempts to reconnect if Wi-Fi is still available; if Wi-Fi also dropped,
// the main loop's Wi-Fi recovery code will restart the device.
static void mqttReconnectTimerCb(TimerHandle_t) {
    // Guard: OTA is in progress — do not attempt to reconnect.  A reconnect
    // from the timer task while the loop task is tearing down the MQTT
    // connection for OTA causes a race in AsyncMqttClient that corrupts the
    // FreeRTOS task context and panics with IllegalInstruction.
    if (_mqttOtaActive) return;

    if (WiFi.isConnected()) {
        LOG_I("MQTT", "Attempting reconnect...");
        _mqttConnectStartMs = millis();   // Start watchdog — loop() will restart if no callback arrives
        _mqttClient.connect();            // Triggers onMqttConnect on success, onMqttDisconnect on failure
    } else {
        // Wi-Fi not yet up (e.g. post-light-sleep association delay).  The
        // one-shot timer stops permanently if we do nothing here, meaning MQTT
        // never reconnects after light sleep.  Re-arm for 2 s so we retry once
        // the radio finishes re-associating with the AP.
        if (xTimerIsTimerActive(_mqttReconnectTimer) == pdFALSE) {
            xTimerChangePeriod(_mqttReconnectTimer, pdMS_TO_TICKS(2000), 0);
            xTimerStart(_mqttReconnectTimer, 0);
        }
    }
}


// ── mqttBegin ─────────────────────────────────────────────────────────────────
// Initialises the MQTT client and starts the first connection attempt.
// Called once from the main sketch when entering OPERATIONAL state.
// `bundle` provides the broker URL and credentials, and is kept in _mqttBundle
// so the rotation handler can access the rotation key later.
void mqttBegin(const CredentialBundle& bundle, const BrokerResult& broker) {
    memcpy(&_mqttBundle, &bundle, sizeof(CredentialBundle));   // Keep a local copy

    // Capture the reset reason once per boot — emitted in the retained boot
    // announcement (v0.3.33). Calling esp_reset_reason() later in the boot
    // can return ESP_RST_UNKNOWN if a software reset has been requested
    // mid-boot, so we snapshot it here before any possible restart path.
    if (_firstBootReason == ESP_RST_UNKNOWN) {
        _firstBootReason = esp_reset_reason();
    }

    // Use the persistent UUID as the device identity in all MQTT topics.
    // DeviceId::init() is called at the start of setup() before mqttBegin().
    _deviceId = DeviceId::get();   // e.g. "a3f2c1d4-5e6f-4789-8abc-def012345678"

    // Create the FreeRTOS one-shot timer used for reconnect back-off.
    // pdFALSE = one-shot (not repeating), nullptr = no timer ID needed
    _mqttReconnectTimer = xTimerCreate("mqttReconnect",
                                       pdMS_TO_TICKS(1000),   // Initial period (overwritten before start)
                                       pdFALSE,               // One-shot, not auto-reload
                                       nullptr,
                                       mqttReconnectTimerCb);

    // Use the pre-resolved broker address from discoverBroker().
    // Discovery already handled URL parsing and the three-step fallback
    // (mDNS -> port scan -> stored URL), so we just read the result.
    _mqttHost = String(broker.host);        // module-level — outlives this function
    uint16_t port = broker.port;           // TCP port (typically 1883)

    // ── Broker address validation ─────────────────────────────────────────────
    // Guard against an empty hostname (e.g. stored URL was never set or failed to
    // parse). Connecting with an empty host silently hangs the async TCP layer.
    if (_mqttHost.isEmpty() || port == 0) {
        LOG_E("MQTT", "mqttBegin aborted - invalid broker address (host='%s' port=%d)",
              _mqttHost.c_str(), port);
        return;
    }

    // Log which step found the broker for serial monitor / Node-RED diagnostics
    const char* methodStr =
        broker.method == DiscoveryMethod::MDNS     ? "mDNS"      :
        broker.method == DiscoveryMethod::PORTSCAN ? "port scan" :
                                                     "stored URL";
    LOG_I("MQTT", "Broker: %s:%d (via %s)", _mqttHost.c_str(), port, methodStr);

    // Register event callbacks before calling connect()
    _mqttClient.onConnect(onMqttConnect);       // Fired after successful connect
    _mqttClient.onDisconnect(onMqttDisconnect); // Fired when connection is lost
    _mqttClient.onMessage(onMqttMessage);       // Fired when a subscribed message arrives

    // AsyncMqttClient hardcodes MQTT 3.1.1 in its CONNECT packet — no version setter needed.
    _mqttClient.setServer(_mqttHost.c_str(), port);

    // Persistent session: broker queues QoS 1/2 messages (including cred_rotate) while
    // the device is offline and delivers them on reconnect. Requires a stable client ID,
    // which we guarantee via the persistent UUID above.
    _mqttClient.setCleanSession(false);

    // Only set credentials if a username was provided (some brokers allow anonymous access)
    if (strlen(bundle.mqtt_username) > 0) {
        _mqttClient.setCredentials(bundle.mqtt_username, bundle.mqtt_password);
    }

    // Client ID must be unique on the broker. Use the first 13 chars of the UUID
    // prefixed with "ESP32-" for a 19-char ID that is readable in broker logs.
    // e.g. "ESP32-a3f2c1d4-5e6" — unique per device, survives OTA updates.
    _mqttClientId = "ESP32-" + _deviceId.substring(0, 13);
    _mqttClient.setClientId(_mqttClientId.c_str());

    // Last-Will and Testament — broker publishes this retained message if the
    // TCP connection drops without a clean MQTT DISCONNECT. Node-RED watches
    // .../status for "online":false to grey out anchors on the map and clear
    // stale peer entries in the roster. The will is set once here and
    // re-sent automatically on every reconnect by AsyncMqttClient.
    _mqttWillTopic = mqttTopic("status");
    _mqttClient.setWill(_mqttWillTopic.c_str(), 1, true,
        "{\"online\":false,\"event\":\"offline\"}");

    if (WiFi.isConnected()) {
        LOG_I("MQTT", "Connecting to %s:%d as %s",
              _mqttHost.c_str(), port, _mqttClientId.c_str());
        _mqttConnectStartMs = millis();   // Start watchdog for the initial connect attempt
        _mqttClient.connect();            // Non-blocking — result arrives via callbacks
    } else {
        // WiFi IP was just assigned but the stack is not yet ready — defer via the
        // reconnect timer (which also guards with WiFi.isConnected()) rather than
        // calling connect() blindly.
        LOG_W("MQTT", "WiFi not ready at mqttBegin - deferring first connect");
        xTimerChangePeriod(_mqttReconnectTimer, pdMS_TO_TICKS(500), 0);
        xTimerStart(_mqttReconnectTimer, 0);
    }
}


// ── mqttNeedsRediscovery / mqttClearRediscoveryFlag / mqttFailCount ───────────
// Self-heal API — called from loop() to detect and recover from a stuck
// reconnect loop without requiring a manual reset.

bool mqttNeedsRediscovery()      { return _mqttNeedsRediscovery; }
void mqttClearRediscoveryFlag()  { _mqttNeedsRediscovery = false; }
int  mqttFailCount()             { return _mqttReconnectCount; }

// Returns true if connect() was called but no callback has arrived within
// MQTT_HUNG_TIMEOUT_MS — the AsyncMqttClient's TCP layer has silently stalled.
bool mqttIsHung() {
    return _mqttConnectStartMs > 0 &&
           !_mqttClient.connected() &&
           (millis() - _mqttConnectStartMs >= MQTT_HUNG_TIMEOUT_MS);
}

bool mqttIsConnected() { return _mqttClient.connected(); }


// ── mqttForceDisconnect ───────────────────────────────────────────────────────
// (v0.4.15) Releases a stuck async-client state without rebooting. Called from
// loop() when mqttIsHung() returns true: the connect() call has been pending
// without success/failure callback for MQTT_HUNG_TIMEOUT_MS. Force-disconnect
// the underlying TCP socket so async_tcp + lwIP free their PCB cleanly, clear
// our watchdog state, and let the existing reconnect timer schedule the next
// attempt. Avoids the ESP.restart() race with AsyncTCP's pending error handler
// (the v0.4.13 / v0.4.14 cascade signature).
void mqttForceDisconnect() {
    _mqttClient.disconnect(true);   // force-close TCP; onDisconnect callback will fire normally
    _mqttConnectStartMs = 0;        // clear hung watchdog
    // Don't change _mqttReconnectDelay — we want the existing back-off to keep
    // climbing if the broker is genuinely down for an extended period.
}



// ── mqttReinit ────────────────────────────────────────────────────────────────
// Re-points the MQTT client at a new broker address and kicks off a fresh
// connection attempt. Called by loop() after Tier 1 broker rediscovery.
// Does NOT reset _mqttReconnectCount — it keeps climbing toward
// MQTT_RESTART_THRESHOLD so Tier 2 (hard restart) still fires if needed.
void mqttReinit(const BrokerResult& broker) {
    _mqttHost = String(broker.host);    // module-level — outlives this function
    LOG_I("MQTT", "Reinit - broker %s:%d", broker.host, broker.port);
    _mqttClient.disconnect(true);   // Force-close any lingering TCP connection
    _mqttClient.setServer(_mqttHost.c_str(), broker.port);
    _mqttReconnectDelay = 1000;     // Reset backoff for the fresh address
    if (xTimerIsTimerActive(_mqttReconnectTimer) == pdTRUE) {
        xTimerStop(_mqttReconnectTimer, 0);
    }
    // Use the reconnect timer for the brief post-disconnect gap instead of
    // delay(100). delay() in a loop() context blocks the ESP32 main loop
    // needlessly; the timer fires asynchronously in the FreeRTOS scheduler.
    // 150 ms is enough for async_tcp to fully close the previous connection
    // before a new connect() is issued. Both WiFi-ready and WiFi-not-ready
    // paths now flow through the same mqttReconnectTimerCb guard, which
    // already checks WiFi.isConnected() before calling connect().
    xTimerChangePeriod(_mqttReconnectTimer, pdMS_TO_TICKS(150), 0);
    xTimerStart(_mqttReconnectTimer, 0);
}


// ── mqttHeartbeat ─────────────────────────────────────────────────────────────
// Publishes a heartbeat to .../status every HEARTBEAT_INTERVAL_MS.
// Must be called from loop() to fire — it does nothing if the interval
// hasn't elapsed yet, so it is cheap to call every loop iteration.
static uint32_t _lastHeartbeat = 0;
void mqttHeartbeat() {
    // ── Deferred restart ──────────────────────────────────────────────────────
    // Credential rotation and cmd/restart set _mqttRestartAtMs instead of
    // calling delay()+ESP.restart() inline, so AsyncMqttClient's keepalive
    // loop is not blocked and the outgoing MQTT publish has time to drain.
    if (_mqttRestartAtMs > 0 && millis() >= _mqttRestartAtMs) {
        _mqttRestartAtMs = 0;
        LOG_I("MQTT", "Deferred restart firing");
        ESP.restart();
    }

    // ── Deferred sleep (LIGHT / DEEP) ─────────────────────────────────────────
    // Same pattern as restart — the cmd/sleep and cmd/deep_sleep handlers set
    // _mqttSleepAtMs so that mqttEnterSleep() is called here rather than inside
    // the async_tcp callback (where a long-running call or teardown of the
    // underlying TCP socket would deadlock AsyncMqttClient).
    if (_mqttSleepAtMs > 0 && millis() >= _mqttSleepAtMs) {
        uint32_t   secs = _mqttSleepDurationS;
        SleepKind  kind = _mqttSleepKind;
        _mqttSleepAtMs      = 0;
        _mqttSleepDurationS = 0;
        _mqttSleepKind      = SleepKind::NONE;
        mqttEnterSleep(secs, kind);
        // For LIGHT sleep, mqttEnterSleep() returns after wake; for DEEP it
        // never returns. Either way, fall through — the next tick is either
        // post-wake or a fresh boot.
    }

    // ── Modem-sleep expiry ────────────────────────────────────────────────────
    // Modem sleep doesn't take the radio offline, so no deferred dispatch is
    // needed; we only need to revert CPU freq + Wi-Fi PS when the timer fires.
    if (_mqttModemSleepUntilMs > 0 && millis() >= _mqttModemSleepUntilMs) {
        mqttExitModemSleep();
    }

    // ── Deferred MQTT_HEALTHY LED (#44) ──────────────────────────────────────────
    // Consume the flag set by onMqttConnect() and post the WS2812 event here,
    // in loop() context, where it is safe to call ws2812PostEvent().
    if (_mqttLedHealthyAtMs > 0) {
        _mqttLedHealthyAtMs = 0;
        LedEvent e{};
        e.type = LedEventType::MQTT_HEALTHY;
        ws2812PostEvent(e);
    }

    if (millis() - _lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        mqttPublishStatus(FwEvent::HEARTBEAT);   // Sends {"mac":..., "event":"heartbeat", ...}
        _lastHeartbeat += HEARTBEAT_INTERVAL_MS; // Advance by fixed interval to prevent drift
    }
}
