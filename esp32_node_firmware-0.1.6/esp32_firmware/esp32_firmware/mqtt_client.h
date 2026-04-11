#pragma once

#include <Arduino.h>
#include <AsyncMqttClient.h>   // Non-blocking MQTT client (marvinroger/async-mqtt-client)
#include <esp_wifi.h>          // esp_wifi_get_mac / WIFI_IF_STA
#include "config.h"
#include "credentials.h"
#include "crypto.h"
#include "device_id.h"   // Persistent UUID-based device identity
#include "app_config.h"   // gAppConfig: runtime GitHub + MQTT topic segments (NVS-backed)
#include "broker_discovery.h"  // BrokerResult from mDNS / port scan / stored URL
#include "led.h"

// ota.h is included AFTER this file in the main sketch.
// Forward-declare otaCheckNow here so onMqttMessage can call it when
// an OTA trigger arrives via MQTT, even though ota.h isn't included yet.
void otaCheckNow();

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
static AsyncMqttClient  _mqttClient;                      // The MQTT client instance
static TimerHandle_t    _mqttReconnectTimer = nullptr;    // FreeRTOS timer for reconnect delay
static uint32_t         _mqttReconnectDelay = 1000;       // Current reconnect delay (ms); grows on failure
static int              _mqttReconnectCount = 0;          // Consecutive failure count; reset on connect
static bool             _mqttNeedsRediscovery = false;    // Set at MQTT_REDISCOVERY_THRESHOLD; cleared by loop()
static uint32_t         _mqttConnectStartMs = 0;          // millis() when connect() was last called; 0 = idle
static String           _deviceId;                        // UUID from DeviceId::get(), set in mqttBegin()
static String           _mqttClientId;                    // kept alive so setClientId()'s raw ptr stays valid
static String           _mqttHost;                        // kept alive so setServer()'s raw ptr stays valid
static CredentialBundle _mqttBundle;                      // Copy of credentials, kept for rotation key access


// ── Topic builders ─────────────────────────────────────────────────────────────
// These functions assemble full topic strings from gAppConfig (loaded from NVS at
// boot by AppConfigStore::load()). Using gAppConfig instead of config.h constants
// means the topic hierarchy can be changed via the settings portal without reflashing.
// _deviceId is the device UUID, set once in mqttBegin() and never changes.

// Build a device-specific topic:  Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/prefix
static String mqttTopic(const char* prefix) {
    // All six hierarchy segments come from gAppConfig (NVS-backed, portal-editable).
    // The DeviceId segment is the persistent UUID — never changes for this device.
    return String(gAppConfig.mqtt_enterprise) + "/" + gAppConfig.mqtt_site    + "/" +
           gAppConfig.mqtt_area               + "/" + gAppConfig.mqtt_line    + "/" +
           gAppConfig.mqtt_cell               + "/" + gAppConfig.mqtt_device_type + "/" +
           _deviceId                          + "/" + prefix;
}

// Build the site-wide broadcast rotation topic: Enterprise/Site/broadcast/cred_rotate
// All nodes subscribe to this so a single publish can rotate credentials on every device.
static String mqttBroadcastRotateTopic() {
    return String(gAppConfig.mqtt_enterprise) + "/" + gAppConfig.mqtt_site + "/broadcast/cred_rotate";
}


// ── Publish helpers ────────────────────────────────────────────────────────────

// Low-level publish — silently drops messages if the client is not connected.
// `prefix` becomes the last segment of the topic path.
static void mqttPublish(const char* prefix, const String& payload,
                        uint8_t qos = 0, bool retain = false) {
    if (!_mqttClient.connected()) return;   // Do nothing if not connected
    String topic = mqttTopic(prefix);
    Serial.printf("[TELEMETRY] %s  %s\n", topic.c_str(), payload.c_str());
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
    Serial.println("[MQTT] Credential rotation message received");

    // Minimum valid size: nonce (12) + at least 1 byte of ciphertext + tag (16)
    if (len <= GCM_NONCE_LEN + GCM_TAG_LEN) {
        Serial.println("[MQTT] Rotation payload too short — ignoring");
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
        Serial.println("[MQTT] Rotation decryption/auth failed — ignoring");
        memset(aesKey, 0, AES_KEY_LEN);            // Wipe key from RAM even on failure
        mqttPublishStatus("cred_rotate_rejected");  // Let Node-RED know it was rejected
        return;
    }
    memset(aesKey, 0, AES_KEY_LEN);   // Wipe key from RAM immediately after use

    // Verify the decrypted data is exactly the size of a CredentialBundle
    if (plaintextLen != sizeof(CredentialBundle)) {
        Serial.println("[MQTT] Rotation bundle size mismatch — ignoring");
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
        Serial.printf("[MQTT] Rotation ignored — new ts %llu <= current ts %llu\n",
                      newBundle.timestamp, _mqttBundle.timestamp);
        return;
    }

    // Save the new bundle to NVS flash so it persists across restarts
    if (!CredentialStore::save(newBundle)) {
        Serial.println("[MQTT] Failed to write rotated credentials to NVS");
        return;
    }

    // Announce success before restarting so Node-RED can confirm the rotation
    Serial.println("[MQTT] Credentials rotated successfully — restarting");
    mqttPublishStatus("cred_rotated");
    delay(500);         // Allow the MQTT publish to be sent before the connection drops
    ESP.restart();      // Restart to connect with the new credentials
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
        Serial.printf("[MQTT] cmd received: %.*s\n", (int)len, payload);
    } else if (t == otaTopic) {
        // OTA check triggered remotely from Node-RED or another MQTT client
        Serial.println("[MQTT] OTA check triggered via MQTT");
        otaCheckNow();   // Defined in ota.h (included after this file)
    } else if (t == mqttTopic("cmd/config_mode")) {
        // Start the settings HTTP portal on the device LAN IP.
        // Admin can then browse to http://<device-ip>/settings to update
        // GitHub and MQTT settings without entering AP mode.
        Serial.println("[MQTT] Config mode triggered — starting settings portal");
        mqttPublishStatus("config_mode_active",
            (String("\"settings_url\":\"http://") + WiFi.localIP().toString() + "/settings\"").c_str());
        settingsServerStart();
    }
}


// ── onMqttConnect ─────────────────────────────────────────────────────────────
// Called by AsyncMqttClient after a successful broker connection (or reconnect).
// Sets up all subscriptions and sends the boot announcement.
static void onMqttConnect(bool sessionPresent) {
    Serial.println("[MQTT] Connected to broker");
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
    _mqttClient.subscribe(mqttTopic("cmd").c_str(),             1);   // General commands
    _mqttClient.subscribe(mqttTopic("cmd/cred_rotate").c_str(), 2);   // Device-specific rotation
    _mqttClient.subscribe(mqttTopic("cmd/ota_check").c_str(),   1);
    _mqttClient.subscribe(mqttTopic("cmd/config_mode").c_str(), 1);   // Start HTTP settings portal on LAN IP
    _mqttClient.subscribe(mqttBroadcastRotateTopic().c_str(),   2);   // Site-wide rotation

    // Publish boot announcement. This is retained (QoS 1) so Node-RED flows
    // that subscribe after boot still see this device's last known state.
    mqttPublishStatus("boot");
    Serial.println("[MQTT] Boot announcement published (v" FIRMWARE_VERSION ")");
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

    Serial.printf("[MQTT] Disconnected (%s) — retrying in %lu ms\n",
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
        Serial.println("[MQTT] Attempting reconnect...");
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

    // Log which step found the broker for serial monitor / Node-RED diagnostics
    const char* methodStr =
        broker.method == DiscoveryMethod::MDNS     ? "mDNS"      :
        broker.method == DiscoveryMethod::PORTSCAN ? "port scan" :
                                                     "stored URL";
    Serial.printf("[MQTT] Broker: %s:%d (via %s)\n",
                  _mqttHost.c_str(), port, methodStr);

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
        Serial.printf("[MQTT] Connecting to %s:%d as %s\n", _mqttHost.c_str(), port, _mqttClientId.c_str());
        _mqttConnectStartMs = millis();   // Start watchdog for the initial connect attempt
        _mqttClient.connect();            // Non-blocking — result arrives via callbacks
    } else {
        // WiFi IP was just assigned but the stack is not yet ready — defer via the
        // reconnect timer (which also guards with WiFi.isConnected()) rather than
        // calling connect() blindly.
        Serial.println("[MQTT] WiFi not ready at mqttBegin — deferring first connect");
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
    Serial.printf("[MQTT] Reinit — broker %s:%d\n", broker.host, broker.port);
    _mqttClient.disconnect(true);   // Force-close any lingering TCP connection
    _mqttClient.setServer(_mqttHost.c_str(), broker.port);
    _mqttReconnectDelay = 1000;     // Reset backoff for the fresh address
    if (xTimerIsTimerActive(_mqttReconnectTimer) == pdTRUE) {
        xTimerStop(_mqttReconnectTimer, 0);
    }
    delay(100);
    if (WiFi.isConnected()) {
        _mqttConnectStartMs = millis();   // Start watchdog for the reinit connect attempt
        _mqttClient.connect();
    } else {
        Serial.println("[MQTT] WiFi not ready at mqttReinit — deferring via reconnect timer");
        xTimerChangePeriod(_mqttReconnectTimer, pdMS_TO_TICKS(500), 0);
        xTimerStart(_mqttReconnectTimer, 0);
    }
}


// ── mqttHeartbeat ─────────────────────────────────────────────────────────────
// Publishes a heartbeat to .../status every HEARTBEAT_INTERVAL_MS.
// Must be called from loop() to fire — it does nothing if the interval
// hasn't elapsed yet, so it is cheap to call every loop iteration.
static uint32_t _lastHeartbeat = 0;
void mqttHeartbeat() {
    if (millis() - _lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        mqttPublishStatus("heartbeat");          // Sends {"mac":..., "event":"heartbeat", ...}
        _lastHeartbeat += HEARTBEAT_INTERVAL_MS; // Advance by fixed interval to prevent drift
    }
}
