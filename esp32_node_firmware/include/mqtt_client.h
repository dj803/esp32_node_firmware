#pragma once

#include <Arduino.h>
#include <AsyncMqttClient.h>   // Non-blocking MQTT client (marvinroger/async-mqtt-client)
#include <ArduinoJson.h>       // JSON parsing for cmd/led and cmd/rfid/whitelist handlers
#include <esp_wifi.h>          // esp_wifi_get_mac / WIFI_IF_STA
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

// rfid.h whitelist API — rfid.h is included AFTER mqtt_client.h.
// These cannot move to rfid_fwd.h without pulling in RFID_UID_STR_LEN,
// so they remain here where config.h (which defines it) is already included.
bool    rfidWhitelistAdd(const char* uid);
bool    rfidWhitelistRemove(const char* uid);
void    rfidWhitelistClear();
void    rfidWhitelistList(char out[][RFID_UID_STR_LEN], uint8_t& count);

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
static uint32_t         _mqttRestartAtMs    = 0;             // Non-zero: restart device when millis() >= this value
static String           _deviceId;                        // UUID from DeviceId::get(), set in mqttBegin()
static String           _mqttClientId;                    // kept alive so setClientId()'s raw ptr stays valid
static String           _mqttHost;                        // kept alive so setServer()'s raw ptr stays valid
static CredentialBundle _mqttBundle;                      // Copy of credentials, kept for rotation key access


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

// Low-level publish — silently drops messages if the client is not connected.
// `prefix` becomes the last segment of the topic path.
static void mqttPublish(const char* prefix, const String& payload,
                        uint8_t qos = 0, bool retain = false) {
    if (!_mqttClient.connected()) return;   // Do nothing if not connected
    String topic = mqttTopic(prefix);
    LOG_D("MQTT", "pub %s  %s", topic.c_str(), payload.c_str());
    _mqttClient.publish(topic.c_str(), qos, retain, payload.c_str());
}

// Build and publish a standard status JSON message to the .../status topic.
// All status messages share the same JSON structure so Node-RED flows can
// parse them with a single JSON node.
//
// `event` identifies what happened: "boot", "heartbeat", "ota_checking", etc.
// `extraJson` is an optional string of additional key:value pairs to append,
//   e.g. "\"target_version\":\"1.2.0\""  — must NOT include surrounding braces.
static void mqttPublishStatus(const char* event, const char* extraJson = nullptr) {
    // Include UUID (primary identity) and MAC (hardware reference) in every status message.
    // device_id is the stable UUID that persists across OTA updates.
    // mac is the hardware MAC address, useful for physical device tracing.
    String macStr = DeviceId::getMac();   // e.g. "AA:BB:CC:A3:F2:C1"

    // Build the JSON payload hand-constructed to avoid heap fragmentation.
    String payload = "{\"device_id\":\"" + DeviceId::get() + "\""
                   + ",\"mac\":\""          + macStr + "\""
                     ",\"firmware_version\":\"" FIRMWARE_VERSION "\""
                   + ",\"firmware_ts\":"     + String((uint32_t)FIRMWARE_BUILD_TIMESTAMP)
                   + ",\"uptime_s\":"        + String(millis() / 1000)
                   + ",\"event\":\""         + event + "\"";
    if (extraJson) {
        payload += ",";          // Append extra fields if provided
        payload += extraJson;
    }
    payload += "}";

    // The boot announcement is published as retained QoS 1 so that any broker
    // subscriber (e.g. a Node-RED flow) that comes online after the device boots
    // still sees the last known status. All other events are non-retained.
    bool retain = (strcmp(event, "boot") == 0);
    mqttPublish("status", payload, 1, retain);
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
    String payload = "{\"state\":\""    + String(state)      + "\""
                   + ",\"r\":"          + String(r)
                   + ",\"g\":"          + String(g)
                   + ",\"b\":"          + String(b)
                   + ",\"brightness\":" + String(brightness)
                   + ",\"count\":"      + String(count)
                   + ",\"uptime_s\":"   + String(millis() / 1000) + "}";
    mqttPublish("status/led", payload, 1, true);   // QoS 1, retained
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
        strlcpy(uid, raw, RFID_UID_STR_LEN);
        { char buf[RFID_UID_STR_LEN]={}; int j=0;
          for(int i=0;uid[i]&&j<RFID_UID_STR_LEN-1;i++)
            if(uid[i]!=':'&&uid[i]!='-') buf[j++]=(char)toupper((unsigned char)uid[i]);
          memcpy(uid,buf,RFID_UID_STR_LEN); }
        bool ok = rfidWhitelistAdd(uid);
        LOG_I("MQTT", "rfid/whitelist add %s: %s", uid, ok ? "ok" : "full");
        String resp = ok
            ? ("{\"event\":\"rfid_whitelist\",\"cmd\":\"add\",\"uid\":\"" + String(uid) + "\",\"result\":\"ok\"}")
            : ("{\"event\":\"rfid_whitelist\",\"cmd\":\"add\",\"uid\":\"" + String(uid) + "\",\"result\":\"error\",\"reason\":\"list full\"}");
        mqttPublish("response", resp);

    } else if (strcmp(cmd, "remove") == 0) {
        const char* raw = doc["uid"] | "";
        char uid[RFID_UID_STR_LEN] = {};
        strlcpy(uid, raw, RFID_UID_STR_LEN);
        { char buf[RFID_UID_STR_LEN]={}; int j=0;
          for(int i=0;uid[i]&&j<RFID_UID_STR_LEN-1;i++)
            if(uid[i]!=':'&&uid[i]!='-') buf[j++]=(char)toupper((unsigned char)uid[i]);
          memcpy(uid,buf,RFID_UID_STR_LEN); }
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
            (String("\"settings_url\":\"http://") + WiFi.localIP().toString() + "/settings\"").c_str());
        settingsServerStart();
    } else if (t == mqttTopic("cmd/led")) {
        // WS2812B LED strip control
        handleLedCommand(payload, len);
    } else if (t == mqttTopic("cmd/rfid/whitelist")) {
        // RFID UID whitelist management
        handleRfidWhitelist(payload, len);
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
    } else if (t == mqttTopic("cmd/restart")) {
        // Deferred restart — publish status then let mqttHeartbeat() fire
        // ESP.restart() after 300 ms so the MQTT publish drains without
        // blocking inside this callback.
        LOG_I("MQTT", "Restart command received - restarting in 300 ms");
        mqttPublishStatus(FwEvent::RESTARTING);
        _mqttRestartAtMs = millis() + 300;
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

    // Subscribe to all command topics for this device.
    // QoS 1 = "at least once" delivery (safe for commands, may duplicate but won't lose)
    // QoS 2 = "exactly once" delivery (used for credential rotation to prevent double-apply)
    _mqttClient.subscribe(mqttTopic("cmd").c_str(),               1);   // General commands
    _mqttClient.subscribe(mqttTopic("cmd/cred_rotate").c_str(),   2);   // Device-specific rotation
    _mqttClient.subscribe(mqttTopic("cmd/ota_check").c_str(),     0);   // QoS 0 — OTA reboots before PUBACK; re-delivery would trigger redundant check
    _mqttClient.subscribe(mqttTopic("cmd/config_mode").c_str(),   1);   // Start HTTP settings portal on LAN IP
    _mqttClient.subscribe(mqttBroadcastRotateTopic().c_str(),     2);   // Site-wide rotation
    _mqttClient.subscribe(mqttTopic("cmd/led").c_str(),           1);   // WS2812B LED strip control
    _mqttClient.subscribe(mqttTopic("cmd/rfid/whitelist").c_str(), 1);  // RFID whitelist management
#ifdef BLE_ENABLED
    _mqttClient.subscribe(mqttTopic("cmd/ble/scan").c_str(),      1);   // BLE: trigger scan
    _mqttClient.subscribe(mqttTopic("cmd/ble/track").c_str(),     1);   // BLE: set tracked beacon
    _mqttClient.subscribe(mqttTopic("cmd/ble/clear").c_str(),     1);   // BLE: clear tracked beacon
    _mqttClient.subscribe(mqttTopic("cmd/ble/list").c_str(),      1);   // BLE: re-publish last results
#endif
    _mqttClient.subscribe(mqttTopic("cmd/restart").c_str(),      0);   // Remote restart — QoS 0 prevents re-delivery loop on reconnect

    // Publish boot announcement. This is retained (QoS 1) so Node-RED flows
    // that subscribe after boot still see this device's last known state.
    mqttPublishStatus(FwEvent::BOOT);
    LOG_I("MQTT", "Boot announcement published (v" FIRMWARE_VERSION ")");

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
    if (_mqttReconnectCount == MQTT_REDISCOVERY_THRESHOLD) {
        _mqttNeedsRediscovery = true;   // Signals loop() to re-run broker discovery
    }

    LOG_W("MQTT", "Disconnected (%s) - retrying in %lu ms",
          mqttDisconnectReasonStr(reason), _mqttReconnectDelay);

    // Only start the timer if it isn't already running (prevents timer pile-up)
    if (xTimerIsTimerActive(_mqttReconnectTimer) == pdFALSE) {
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
    if (WiFi.isConnected()) {
        LOG_I("MQTT", "Attempting reconnect...");
        _mqttConnectStartMs = millis();   // Start watchdog — loop() will restart if no callback arrives
        _mqttClient.connect();            // Triggers onMqttConnect on success, onMqttDisconnect on failure
    }
    // If Wi-Fi is not connected, do nothing — the main loop handles Wi-Fi recovery
}


// ── mqttBegin ─────────────────────────────────────────────────────────────────
// Initialises the MQTT client and starts the first connection attempt.
// Called once from the main sketch when entering OPERATIONAL state.
// `bundle` provides the broker URL and credentials, and is kept in _mqttBundle
// so the rotation handler can access the rotation key later.
void mqttBegin(const CredentialBundle& bundle, const BrokerResult& broker) {
    memcpy(&_mqttBundle, &bundle, sizeof(CredentialBundle));   // Keep a local copy

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

    if (millis() - _lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        mqttPublishStatus(FwEvent::HEARTBEAT);   // Sends {"mac":..., "event":"heartbeat", ...}
        _lastHeartbeat += HEARTBEAT_INTERVAL_MS; // Advance by fixed interval to prevent drift
    }
}
