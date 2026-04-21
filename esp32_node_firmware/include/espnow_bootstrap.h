#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"
#include "logging.h"
#include "credentials.h"
#include "crypto.h"
#include "app_config.h"   // gAppConfig + AppConfigStore::save() for OTA URL adoption
#include "led.h"

// =============================================================================
// espnow_bootstrap.h  —  ESP-NOW credential request / response
//
// Fixes vs original:
//   1. Channel scanning: scans channels 1–13 to find the sibling automatically.
//      Tries the last known working channel first (cached in NVS) for a fast
//      path on subsequent boots. Falls through the full scan on first boot or
//      if the router channel changed.
//   2. WireBundle replaces raw CredentialBundle on the wire.
//      CredentialBundle is ~382 bytes; encrypted that exceeds ESP-NOW's 250B
//      hard limit. WireBundle uses tight field limits (175 bytes plaintext,
//      245 bytes on wire including header + ECDH overhead).
//   3. Removed esp_now_deinit() before response is parsed.
//   4. Fixed RESP_HDR / payload_len parse offset.
//
// REQUEST wire format  (40 bytes):
//   [msg_type 1B][protocol_version 1B][sender_mac 6B][requester_pubkey 32B]
//
// RESPONSE wire format (245 bytes max):
//   [msg_type 1B][version 1B][sender_mac 6B][responder_pubkey 32B]
//   [payload_len 2B][nonce 12B][ciphertext varB][auth_tag 16B]
// =============================================================================

// Channel scanning: bootstrap scans channels 1–13 automatically.
// The last found channel is cached in NVS (key "espnow_ch") for a fast path
// on subsequent boots. No fixed channel constant is needed.

#define REQ_LEN  (1 + 1 + 6 + CURVE25519_KEY_LEN)   // 40 bytes

// Response header length (everything before the encrypted payload)
#define RESP_HDR_LEN  (1 + 1 + 6 + CURVE25519_KEY_LEN + 2)   // 42 bytes
//                     typ ver mac  pubkey               plen

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ── WireBundle: compact on-wire representation (176 bytes) ───────────────────
// Field limits chosen so that the encrypted response fits inside 250 bytes.
// Tighter than CredentialBundle but sufficient for real-world values.
//
// Wire format versioning (v0.3.08+):
//   wire_version must equal ESPNOW_WIRE_VERSION (1). Receivers reject frames
//   with any other value. Version 0 is reserved to identify pre-v0.3.08
//   senders (which will fail the size check — they send 175 bytes, not 176).
#define ESPNOW_WIRE_VERSION  1   // Increment whenever the WireBundle layout changes
#pragma pack(push, 1)
struct WireBundle {
    uint8_t  wire_version;       // Must equal ESPNOW_WIRE_VERSION — first byte for fast reject
    char     wifi_ssid[33];      // 32 chars + null
    char     wifi_password[33];  // 32 chars + null
    char     mqtt_broker_url[50];// e.g. mqtt://192.168.1.100:1883 (26 chars typical)
    char     mqtt_username[17];  // 16 chars + null
    char     mqtt_password[17];  // 16 chars + null
    uint8_t  rotation_key[16];   // raw 16-byte AES key
    uint64_t timestamp;
    uint8_t  source;             // 0=sibling 1=admin
};
#pragma pack(pop)

static_assert(sizeof(WireBundle) == 176, "WireBundle size changed — check ESP-NOW fit");

#ifdef SIBLING_PRIMARY_SELECTION
// ── SiblingHealth: health advertisement for primary selection ─────────────────
// Sent by OPERATIONAL nodes in response to ESPNOW_MSG_HEALTH_QUERY.
// 17 bytes — fits in a single ESP-NOW frame with ample margin.
//
// health_flags bit assignments (must match responder_health_flags in espnow_responder.h):
//   bit 0 — WiFi connected
//   bit 1 — MQTT connected
//   bit 2 — GitHub reachable (last OTA JSON fetch succeeded)
#pragma pack(push, 1)
struct SiblingHealth {
    uint8_t  mac[6];              // Responder's own STA MAC (for unicast targeting)
    uint32_t fw_version_uint32;   // Packed: (major << 16) | (minor << 8) | patch
    uint8_t  health_flags;        // Bitmask — see bit assignments above
    uint8_t  protocol_version;    // Must equal ESPNOW_PROTOCOL_VERSION
    uint8_t  _pad[3];             // Reserved / alignment padding, set to 0
};
#pragma pack(pop)
static_assert(sizeof(SiblingHealth) == 17, "SiblingHealth size changed — check ESP-NOW fit");


// Parse a "major.minor.patch" version string into a packed uint32:
//   (major << 16) | (minor << 8) | patch
// Allows numeric comparison: higher packed value = newer firmware.
static uint32_t fwVersionToUint32(const char* ver) {
    unsigned maj = 0, min = 0, pat = 0;
    sscanf(ver, "%u.%u.%u", &maj, &min, &pat);
    return ((uint32_t)maj << 16) | ((uint32_t)min << 8) | (uint32_t)pat;
}


// ── Phase 1 collection state ──────────────────────────────────────────────────
#define SIBLING_HEALTH_MAX_RESPONSES  8   // Track at most 8 siblings per scan

static SiblingHealth    _healthResponses[SIBLING_HEALTH_MAX_RESPONSES];
static uint8_t          _healthCount      = 0;
static volatile bool    _healthCollecting = false;
#endif // SIBLING_PRIMARY_SELECTION

// Convert CredentialBundle → WireBundle (truncate oversized fields)
static void bundleToWire(const CredentialBundle& b, WireBundle& w) {
    memset(&w, 0, sizeof(w));
    w.wire_version = ESPNOW_WIRE_VERSION;
    strncpy(w.wifi_ssid,       b.wifi_ssid,       sizeof(w.wifi_ssid)       - 1);
    strncpy(w.wifi_password,   b.wifi_password,   sizeof(w.wifi_password)   - 1);
    strncpy(w.mqtt_broker_url, b.mqtt_broker_url, sizeof(w.mqtt_broker_url) - 1);
    strncpy(w.mqtt_username,   b.mqtt_username,   sizeof(w.mqtt_username)   - 1);
    strncpy(w.mqtt_password,   b.mqtt_password,   sizeof(w.mqtt_password)   - 1);
    memcpy(w.rotation_key, b.rotation_key, 16);
    w.timestamp = b.timestamp;
    w.source    = (uint8_t)b.source;
}

// Convert WireBundle → CredentialBundle
static void wireToBundle(const WireBundle& w, CredentialBundle& b) {
    memset(&b, 0, sizeof(b));
    strncpy(b.wifi_ssid,       w.wifi_ssid,       sizeof(b.wifi_ssid)       - 1);
    strncpy(b.wifi_password,   w.wifi_password,   sizeof(b.wifi_password)   - 1);
    strncpy(b.mqtt_broker_url, w.mqtt_broker_url, sizeof(b.mqtt_broker_url) - 1);
    strncpy(b.mqtt_username,   w.mqtt_username,   sizeof(b.mqtt_username)   - 1);
    strncpy(b.mqtt_password,   w.mqtt_password,   sizeof(b.mqtt_password)   - 1);
    memcpy(b.rotation_key, w.rotation_key, 16);
    b.timestamp = w.timestamp;
    b.source    = (CredSource)w.source;
}

// ── Shared state ──────────────────────────────────────────────────────────────
// Bootstrap-time OTA URL reception — populated by onEspNowReceive when a
// sibling sends OTA_URL_RESP during the bootstrap phase.
static volatile bool _bootstrapOtaUrlReceived = false;
static char          _bootstrapOtaUrl[201]     = {0};

static volatile bool _espnowResponseReceived = false;
static uint8_t       _espnowRespBuf[250]     = {0};
static size_t        _espnowRespLen          = 0;
static EcdhContext   _requesterCtx;

// ── Receive callback (requester side — bootstrap phase only) ──────────────────
static void onEspNowReceive(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
    if (len < 2) return;
    Serial.printf("[ESP-NOW CB] Received %d bytes, msg_type=0x%02X\n", len, data[0]);

#ifdef SIBLING_PRIMARY_SELECTION
    // ── Phase 1: collect health responses ─────────────────────────────────────
    if (data[0] == ESPNOW_MSG_HEALTH_RESP && _healthCollecting) {
        // Wire: [HEALTH_RESP 1B][version 1B][SiblingHealth 17B]
        if (len < (int)(2 + sizeof(SiblingHealth))) return;
        if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
        if (_healthCount >= SIBLING_HEALTH_MAX_RESPONSES) return;
        memcpy(&_healthResponses[_healthCount], data + 2, sizeof(SiblingHealth));
        _healthCount++;
        Serial.printf("[ESP-NOW CB] HEALTH_RESP #%d collected\n", _healthCount);
        return;
    }
#endif

    // ── Bootstrap-time OTA URL response ──────────────────────────────────────
    if (data[0] == ESPNOW_MSG_OTA_URL_RESP && !_bootstrapOtaUrlReceived) {
        if (len < 10 || data[1] != ESPNOW_PROTOCOL_VERSION) return;
        uint8_t urlLen = data[8];
        if (urlLen == 0 || urlLen > 200 || len < (int)(9 + urlLen)) return;
        memcpy(_bootstrapOtaUrl, data + 9, urlLen);
        _bootstrapOtaUrl[urlLen] = '\0';
        _bootstrapOtaUrlReceived = true;
        Serial.printf("[ESP-NOW CB] OTA URL from sibling: %s\n", _bootstrapOtaUrl);
        return;
    }

    // ── Phase 2 / plain bootstrap: credential response ────────────────────────
    if (data[0] != ESPNOW_MSG_CREDENTIAL_RESP) {
        Serial.printf("[ESP-NOW CB] Ignoring msg_type=0x%02X\n", data[0]);
        return;
    }
    if (len < (int)(RESP_HDR_LEN + GCM_NONCE_LEN + GCM_TAG_LEN)) {
        Serial.printf("[ESP-NOW CB] CREDENTIAL_RESP too short (%d bytes)\n", len);
        return;
    }
    if (data[1] != ESPNOW_PROTOCOL_VERSION) {
        Serial.printf("[ESP-NOW CB] Version mismatch: got %d expected %d\n",
                      data[1], ESPNOW_PROTOCOL_VERSION);
        return;
    }
    if (_espnowResponseReceived) return;

    memcpy(_espnowRespBuf, data, len);
    _espnowRespLen = len;
    _espnowResponseReceived = true;
    ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief RX indicator
    Serial.println("[ESP-NOW CB] Credential response accepted");
}

// Forward declaration — implemented in espnow_responder.h
void onEspNowRequest(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len);

// ── Last-known channel cache (NVS) ───────────────────────────────────────────
// Saves the channel where a sibling was last found so the next bootstrap can
// try it first — mirrors the broker address cache in broker_discovery.h.
// Returns 0 if no value is stored (triggers a full scan from channel 1).
static uint8_t espnowLoadLastChannel() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);   // read-only
    uint8_t ch = prefs.getUChar("espnow_ch", 0);
    prefs.end();
    return ch;
}

static void espnowSaveLastChannel(uint8_t ch) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("espnow_ch", ch);
    prefs.end();
}

// ── Send broadcast request on a specific channel ─────────────────────────────
static bool espnowSendRequest(const EcdhContext& ctx, uint8_t channel) {
    uint8_t buf[REQ_LEN] = {0};
    buf[0] = ESPNOW_MSG_CREDENTIAL_REQ;
    buf[1] = ESPNOW_PROTOCOL_VERSION;

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(&buf[2], mac, 6);
    memcpy(&buf[8], ctx.publicKey, CURVE25519_KEY_LEN);

    Serial.printf("[ESP-NOW] My MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);

    // Update the broadcast peer's channel, or add it if not yet registered
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, ESPNOW_BROADCAST, 6);
    peer.channel = channel;
    peer.encrypt = false;
    if (esp_now_is_peer_exist(ESPNOW_BROADCAST)) {
        esp_now_mod_peer(&peer);
    } else {
        esp_err_t addErr = esp_now_add_peer(&peer);
        if (addErr != ESP_OK) {
            Serial.printf("[ESP-NOW] Add peer failed: %d\n", addErr);
            return false;
        }
    }

    esp_err_t err = esp_now_send(ESPNOW_BROADCAST, buf, REQ_LEN);
    if (err == ESP_OK) ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    Serial.printf("[ESP-NOW] esp_now_send returned: %d\n", err);
    return err == ESP_OK;
}

// ── Bootstrap entry point ─────────────────────────────────────────────────────
// Scans Wi-Fi channels 1–13 to find the sibling regardless of which channel
// the router uses. On each channel: forces the radio, broadcasts a credential
// request, and waits ESPNOW_CHANNEL_DWELL_MS for a response. Stops as soon as
// a valid response arrives and records which channel succeeded.
bool espnowBootstrap(CredentialBundle& out) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_err_t initErr = esp_now_init();
    if (initErr != ESP_OK) {
        Serial.printf("[ESP-NOW] Init failed: %d\n", initErr);
        return false;
    }

    esp_now_register_recv_cb(onEspNowReceive);
    Serial.println("[ESP-NOW] Init OK — scanning channels 1–13");

    if (!cryptoGenKeypair(_requesterCtx)) {
        Serial.println("[ESP-NOW] ECDH keygen failed");
        esp_now_deinit();
        return false;
    }

    _espnowResponseReceived = false;
    uint8_t foundChannel = 0;

    // Try the last known working channel first — fast path on repeat boots
    uint8_t cachedChannel = espnowLoadLastChannel();
    if (cachedChannel >= 1 && cachedChannel <= 13) {
        Serial.printf("[ESP-NOW] Trying cached channel %d first\n", cachedChannel);
        esp_wifi_set_channel(cachedChannel, WIFI_SECOND_CHAN_NONE);
        if (espnowSendRequest(_requesterCtx, cachedChannel)) {
            uint32_t deadline = millis() + ESPNOW_CHANNEL_DWELL_MS;
            while (!_espnowResponseReceived && millis() < deadline) delay(10);
            if (_espnowResponseReceived) foundChannel = cachedChannel;
        }
    }

    // Full scan — skips the cached channel (already tried above)
    for (uint8_t ch = 1; ch <= 13 && !_espnowResponseReceived; ch++) {
        if (ch == cachedChannel) continue;   // already tried
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[ESP-NOW] Trying channel %d\n", ch);

        if (!espnowSendRequest(_requesterCtx, ch)) continue;

        Serial.println("[ESP-NOW] Request sent, waiting for sibling response...");
        uint32_t deadline = millis() + ESPNOW_CHANNEL_DWELL_MS;
        while (!_espnowResponseReceived && millis() < deadline) {
            delay(10);
        }
        if (_espnowResponseReceived) foundChannel = ch;
    }

    if (!_espnowResponseReceived) {
        Serial.println("[ESP-NOW] Timeout — no sibling response");
        esp_now_deinit();
        return false;
    }

    Serial.printf("[ESP-NOW] Sibling found on channel %d\n", foundChannel);
    if (foundChannel != cachedChannel) {
        espnowSaveLastChannel(foundChannel);   // update cache for next boot
    }

    // ── Parse response ────────────────────────────────────────────────────────
    // Layout: [type 1][ver 1][mac 6][responder_pubkey 32][payload_len 2][nonce+ct+tag]
    const uint8_t* resp             = _espnowRespBuf;
    const uint8_t* responderPubKey  = resp + 8;              // offset 8, 32 bytes
    uint16_t       payloadLen       = 0;
    memcpy(&payloadLen, resp + 40, 2);                        // offset 40, 2 bytes
    const uint8_t* encPayload       = resp + RESP_HDR_LEN;   // offset 42

    size_t encLen = _espnowRespLen - RESP_HDR_LEN;
    Serial.printf("[ESP-NOW] Parsing response: total=%d encLen=%d payloadLen=%d\n",
                  (int)_espnowRespLen, (int)encLen, (int)payloadLen);

    // Derive shared AES key
    uint8_t aesKey[AES_KEY_LEN];
    if (!cryptoDeriveKey(_requesterCtx, responderPubKey, aesKey)) {
        Serial.println("[ESP-NOW] Key derivation failed");
        esp_now_deinit();
        return false;
    }

    // Decrypt
    uint8_t plaintext[sizeof(WireBundle) + 4];  // small buffer — WireBundle only
    size_t  plaintextLen = 0;
    if (!cryptoDecrypt(aesKey, encPayload, encLen, plaintext, plaintextLen)) {
        Serial.println("[ESP-NOW] Decryption/auth failed — discarding");
        memset(aesKey, 0, AES_KEY_LEN);
        esp_now_deinit();
        return false;
    }
    memset(aesKey, 0, AES_KEY_LEN);

    if (plaintextLen != sizeof(WireBundle)) {
        Serial.printf("[ESP-NOW] Bundle size mismatch: got %d expected %d\n",
                      (int)plaintextLen, (int)sizeof(WireBundle));
        esp_now_deinit();
        return false;
    }

    WireBundle wire;
    memcpy(&wire, plaintext, sizeof(WireBundle));
    memset(plaintext, 0, sizeof(plaintext));

    if (wire.wire_version != ESPNOW_WIRE_VERSION) {
        LOG_W("ESP-NOW", "Unknown WireBundle version %d (expected %d) — rejecting",
              wire.wire_version, ESPNOW_WIRE_VERSION);
        memset(&wire, 0, sizeof(wire));
        esp_now_deinit();
        return false;
    }

    wireToBundle(wire, out);
    memset(&wire, 0, sizeof(wire));

    Serial.println("[ESP-NOW] Bundle received and verified");

    // ── Also request OTA URL from this sibling while ESP-NOW is still open ────
    // The responder's MAC is in the response header at bytes 2–7.
    // This is a best-effort enrichment — if the sibling doesn't respond or their
    // GitHub health flag is not set, we keep our existing OTA URL and continue.
    {
        _bootstrapOtaUrlReceived = false;
        memset(_bootstrapOtaUrl, 0, sizeof(_bootstrapOtaUrl));

        const uint8_t* responderMac = _espnowRespBuf + 2;   // bytes 2–7 in response
        uint8_t req[2] = { ESPNOW_MSG_OTA_URL_REQ, ESPNOW_PROTOCOL_VERSION };

        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, responderMac, 6);
        peer.channel = foundChannel;   // same channel where credentials were exchanged
        peer.encrypt = false;
        if (!esp_now_is_peer_exist(responderMac)) esp_now_add_peer(&peer);

        esp_err_t urlErr = esp_now_send(responderMac, req, sizeof(req));
        if (urlErr == ESP_OK) {
            uint32_t urlDeadline = millis() + OTA_URL_REQUEST_TIMEOUT_MS;
            while (!_bootstrapOtaUrlReceived && millis() < urlDeadline) delay(10);
        }

        if (_bootstrapOtaUrlReceived) {
            AppConfig cfg;
            memcpy(&cfg, &gAppConfig, sizeof(AppConfig));
            strncpy(cfg.ota_json_url, _bootstrapOtaUrl, sizeof(cfg.ota_json_url) - 1);
            cfg.ota_json_url[sizeof(cfg.ota_json_url) - 1] = '\0';
            AppConfigStore::save(cfg);
            Serial.printf("[ESP-NOW] OTA URL adopted from sibling: %s\n", gAppConfig.ota_json_url);
        } else {
            Serial.println("[ESP-NOW] No OTA URL from sibling — keeping existing");
        }
    }

    esp_now_deinit();
    return true;
}


// ── Async task-based bootstrap API ───────────────────────────────────────────
// espnowBootstrapBegin() spawns a FreeRTOS task that runs the full
// BOOTSTRAP_MAX_ATTEMPTS retry sequence without blocking the Arduino main task.
// Poll espnowBootstrapIsDone() from loop(); call espnowBootstrapGetResult()
// once done to obtain the received bundle (if any).
//
// Safe to call from setup() or loop(). Only one task may run at a time;
// calling espnowBootstrapBegin() while a task is already running is a no-op.
//
// Stack: 8 KB on the system heap — sufficient for ECDH + AES stack frames
// inside espnowBootstrap(). The task self-deletes on completion.
// ─────────────────────────────────────────────────────────────────────────────

static TaskHandle_t     _bootstrapTaskHandle = nullptr;
static volatile bool    _bootstrapTaskDone   = false;
static volatile bool    _bootstrapTaskOk     = false;
static CredentialBundle _bootstrapTaskBundle;   // written by task, read after done

static void _espnowBootstrapTask(void* /*pvParams*/) {
    const uint64_t MAX_TS  = FIRMWARE_BUILD_TIMESTAMP + SIBLING_TS_MAX_FUTURE_S;
    CredentialBundle received;
    bool gotBundle = false;

    for (int attempt = 1; attempt <= BOOTSTRAP_MAX_ATTEMPTS && !gotBundle; attempt++) {
        LOG_I("ESP-NOW", "Async bootstrap attempt %d of %d", attempt, BOOTSTRAP_MAX_ATTEMPTS);
        CredentialBundle candidate;
        bool gotResp = espnowBootstrap(candidate);
        if (gotResp) {
            if (candidate.timestamp > MAX_TS) {
                LOG_W("ESP-NOW", "Bundle timestamp exceeds cap — discarding");
            } else {
                received  = candidate;
                gotBundle = true;
            }
        } else if (attempt < BOOTSTRAP_MAX_ATTEMPTS) {
            LOG_I("ESP-NOW", "No response — retrying in 2 s");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    if (gotBundle) {
        _bootstrapTaskBundle = received;
        _bootstrapTaskOk     = true;
    }
    _bootstrapTaskDone   = true;
    _bootstrapTaskHandle = nullptr;
    vTaskDelete(nullptr);   // task self-deletes
}

// Start the async bootstrap. No-op if a task is already running.
static void espnowBootstrapBegin() {
    if (_bootstrapTaskHandle != nullptr) return;   // already running
    _bootstrapTaskDone = false;
    _bootstrapTaskOk   = false;
    memset(&_bootstrapTaskBundle, 0, sizeof(_bootstrapTaskBundle));
    xTaskCreate(_espnowBootstrapTask, "esp_bootstrap", 8192, nullptr, 5,
                &_bootstrapTaskHandle);
}

// Returns true once the task has finished (success or all attempts exhausted).
static inline bool espnowBootstrapIsDone()  { return _bootstrapTaskDone; }

// Copies the received bundle into `out` and returns true if bootstrap succeeded.
// Must only be called after espnowBootstrapIsDone() returns true.
static bool espnowBootstrapGetResult(CredentialBundle& out) {
    if (_bootstrapTaskOk) { out = _bootstrapTaskBundle; return true; }
    return false;
}


#ifdef SIBLING_PRIMARY_SELECTION
// ── espnowBootstrapWithPrimarySelection ───────────────────────────────────────
// Two-phase bootstrap that prefers the healthiest sibling:
//
//   Phase 1 — Broadcast HEALTH_QUERY, collect HEALTH_RESP for SIBLING_HEALTH_WAIT_MS.
//             Each HEALTH_RESP carries the responder's MAC, firmware version, and
//             connectivity health flags (WiFi/MQTT/GitHub).
//
//   Phase 2 — Score each sibling as (fw_version_uint32 << 8) | health_flags.
//             Unicast CREDENTIAL_REQ to the highest-scoring sibling.
//             Wait for their encrypted CREDENTIAL_RESP.
//
// Fallback — If no HEALTH_RESPs arrive in phase 1, calls espnowBootstrap() to
//            fall back to the plain broadcast. This makes the function safe for
//            mixed v1/v2 fleets; v1 nodes simply don't answer the health query.
//
// WiFi MUST NOT be connected when calling this function (it sets the channel).
bool espnowBootstrapWithPrimarySelection(CredentialBundle& out) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    // Use the cached channel as the starting point for the health query.
    // If no siblings respond (wrong channel or no v2 nodes), the fallback to
    // espnowBootstrap() performs a full channel scan.
    uint8_t psChannel = espnowLoadLastChannel();
    if (psChannel < 1 || psChannel > 13) psChannel = 1;
    esp_wifi_set_channel(psChannel, WIFI_SECOND_CHAN_NONE);

    esp_err_t initErr = esp_now_init();
    if (initErr != ESP_OK) {
        Serial.printf("[ESP-NOW PS] Init failed: %d — falling back to broadcast\n", initErr);
        return espnowBootstrap(out);
    }
    esp_now_register_recv_cb(onEspNowReceive);

    // ── Phase 1: broadcast HEALTH_QUERY ──────────────────────────────────────
    _healthCount      = 0;
    _healthCollecting = true;
    memset(_healthResponses, 0, sizeof(_healthResponses));

    // Add broadcast peer and send HEALTH_QUERY
    esp_now_peer_info_t bcastPeer = {};
    memcpy(bcastPeer.peer_addr, ESPNOW_BROADCAST, 6);
    bcastPeer.channel = psChannel;
    bcastPeer.encrypt = false;
    if (!esp_now_is_peer_exist(ESPNOW_BROADCAST)) esp_now_add_peer(&bcastPeer);

    uint8_t queryBuf[2] = { ESPNOW_MSG_HEALTH_QUERY, ESPNOW_PROTOCOL_VERSION };
    esp_now_send(ESPNOW_BROADCAST, queryBuf, sizeof(queryBuf));
    Serial.printf("[ESP-NOW PS] HEALTH_QUERY sent — collecting responses for %d ms\n",
                  SIBLING_HEALTH_WAIT_MS);

    uint32_t deadline = millis() + SIBLING_HEALTH_WAIT_MS;
    while (millis() < deadline) delay(10);
    _healthCollecting = false;

    if (_healthCount == 0) {
        // No v2-capable siblings found — fall back to plain broadcast bootstrap
        Serial.println("[ESP-NOW PS] No health responses — falling back to broadcast bootstrap");
        esp_now_deinit();
        return espnowBootstrap(out);
    }
    Serial.printf("[ESP-NOW PS] Collected %d health response(s)\n", _healthCount);

    // ── Pick best sibling ─────────────────────────────────────────────────────
    // Score: higher firmware version wins; ties broken by most health flags set.
    // (fw_version_uint32 << 8) | health_flags gives a single comparable uint64.
    int      bestIdx   = 0;
    uint64_t bestScore = 0;
    for (int i = 0; i < _healthCount; i++) {
        uint64_t score = ((uint64_t)_healthResponses[i].fw_version_uint32 << 8)
                       | _healthResponses[i].health_flags;
        if (score > bestScore) { bestScore = score; bestIdx = i; }
    }
    const SiblingHealth& best = _healthResponses[bestIdx];
    Serial.printf("[ESP-NOW PS] Best sibling: %02X:%02X:%02X:%02X:%02X:%02X"
                  "  fw=0x%06X  flags=0x%02X\n",
                  best.mac[0], best.mac[1], best.mac[2],
                  best.mac[3], best.mac[4], best.mac[5],
                  best.fw_version_uint32, best.health_flags);

    // ── Phase 2: unicast CREDENTIAL_REQ to best sibling ──────────────────────
    // Deinit so we can reinit cleanly (ECDH keygen needs a fresh state).
    esp_now_deinit();
    delay(50);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    esp_wifi_set_channel(psChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW PS] Reinit failed — falling back to broadcast");
        return espnowBootstrap(out);
    }

    _espnowResponseReceived = false;
    esp_now_register_recv_cb(onEspNowReceive);

    // Generate ephemeral ECDH keypair
    if (!cryptoGenKeypair(_requesterCtx)) {
        Serial.println("[ESP-NOW PS] ECDH keygen failed");
        esp_now_deinit();
        return false;
    }

    // Build CREDENTIAL_REQ packet — identical format to espnowSendRequest()
    uint8_t reqBuf[REQ_LEN] = {0};
    reqBuf[0] = ESPNOW_MSG_CREDENTIAL_REQ;
    reqBuf[1] = ESPNOW_PROTOCOL_VERSION;
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    memcpy(&reqBuf[2], myMac, 6);
    memcpy(&reqBuf[8], _requesterCtx.publicKey, CURVE25519_KEY_LEN);

    // Register the chosen sibling as a unicast peer
    esp_now_peer_info_t uniPeer = {};
    memcpy(uniPeer.peer_addr, best.mac, 6);
    uniPeer.channel = psChannel;
    uniPeer.encrypt = false;
    if (!esp_now_is_peer_exist(best.mac)) esp_now_add_peer(&uniPeer);

    esp_err_t sendErr = esp_now_send(best.mac, reqBuf, REQ_LEN);
    if (sendErr != ESP_OK) {
        Serial.printf("[ESP-NOW PS] Unicast send failed: %d — falling back to broadcast\n", sendErr);
        esp_now_deinit();
        return espnowBootstrap(out);
    }
    Serial.println("[ESP-NOW PS] Unicast CREDENTIAL_REQ sent — waiting for response...");

    // Wait for CREDENTIAL_RESP (same mechanism as espnowBootstrap)
    uint32_t respDeadline = millis() + BOOTSTRAP_TIMEOUT_MS;
    while (!_espnowResponseReceived && millis() < respDeadline) delay(10);

    if (!_espnowResponseReceived) {
        Serial.println("[ESP-NOW PS] Best sibling did not respond — falling back to broadcast");
        esp_now_deinit();
        return espnowBootstrap(out);
    }

    // ── Parse CREDENTIAL_RESP ─────────────────────────────────────────────────
    // Layout identical to espnowBootstrap(): RESP_HDR then encrypted WireBundle.
    const uint8_t* resp            = _espnowRespBuf;
    const uint8_t* responderPubKey = resp + 8;              // offset 8, 32 bytes
    uint16_t       payloadLen      = 0;
    memcpy(&payloadLen, resp + 40, 2);                       // offset 40, 2 bytes
    const uint8_t* encPayload      = resp + RESP_HDR_LEN;   // offset 42

    size_t encLen = _espnowRespLen - RESP_HDR_LEN;
    Serial.printf("[ESP-NOW PS] Parsing response: total=%d encLen=%d payloadLen=%d\n",
                  (int)_espnowRespLen, (int)encLen, (int)payloadLen);

    uint8_t aesKey[AES_KEY_LEN];
    if (!cryptoDeriveKey(_requesterCtx, responderPubKey, aesKey)) {
        Serial.println("[ESP-NOW PS] Key derivation failed");
        esp_now_deinit();
        return false;
    }

    uint8_t plaintext[sizeof(WireBundle) + 4];
    size_t  plaintextLen = 0;
    if (!cryptoDecrypt(aesKey, encPayload, encLen, plaintext, plaintextLen)) {
        Serial.println("[ESP-NOW PS] Decryption/auth failed — discarding");
        memset(aesKey, 0, AES_KEY_LEN);
        esp_now_deinit();
        return false;
    }
    memset(aesKey, 0, AES_KEY_LEN);

    if (plaintextLen != sizeof(WireBundle)) {
        Serial.printf("[ESP-NOW PS] Bundle size mismatch: got %d expected %d\n",
                      (int)plaintextLen, (int)sizeof(WireBundle));
        esp_now_deinit();
        return false;
    }

    WireBundle wire;
    memcpy(&wire, plaintext, sizeof(WireBundle));
    memset(plaintext, 0, sizeof(plaintext));

    if (wire.wire_version != ESPNOW_WIRE_VERSION) {
        LOG_W("ESP-NOW", "Unknown WireBundle version %d (expected %d) — rejecting",
              wire.wire_version, ESPNOW_WIRE_VERSION);
        memset(&wire, 0, sizeof(wire));
        esp_now_deinit();
        return false;
    }

    wireToBundle(wire, out);
    memset(&wire, 0, sizeof(wire));

    Serial.println("[ESP-NOW PS] Bundle received and verified from primary sibling");
    if (psChannel != espnowLoadLastChannel()) {
        espnowSaveLastChannel(psChannel);   // update cache for next boot
    }

    // ── Also request OTA URL from this sibling while ESP-NOW is still open ────
    {
        _bootstrapOtaUrlReceived = false;
        memset(_bootstrapOtaUrl, 0, sizeof(_bootstrapOtaUrl));

        uint8_t req[2] = { ESPNOW_MSG_OTA_URL_REQ, ESPNOW_PROTOCOL_VERSION };

        // best.mac is already registered as a peer from the credential request above
        esp_err_t urlErr = esp_now_send(best.mac, req, sizeof(req));
        if (urlErr == ESP_OK) {
            uint32_t urlDeadline = millis() + OTA_URL_REQUEST_TIMEOUT_MS;
            while (!_bootstrapOtaUrlReceived && millis() < urlDeadline) delay(10);
        }

        if (_bootstrapOtaUrlReceived) {
            AppConfig cfg;
            memcpy(&cfg, &gAppConfig, sizeof(AppConfig));
            strncpy(cfg.ota_json_url, _bootstrapOtaUrl, sizeof(cfg.ota_json_url) - 1);
            cfg.ota_json_url[sizeof(cfg.ota_json_url) - 1] = '\0';
            AppConfigStore::save(cfg);
            Serial.printf("[ESP-NOW PS] OTA URL adopted from primary sibling: %s\n",
                          gAppConfig.ota_json_url);
        } else {
            Serial.println("[ESP-NOW PS] No OTA URL from primary sibling — keeping existing");
        }
    }

    esp_now_deinit();
    return true;
}
#endif // SIBLING_PRIMARY_SELECTION
