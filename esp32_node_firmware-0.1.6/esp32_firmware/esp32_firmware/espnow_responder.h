#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "credentials.h"
#include "crypto.h"
#include "app_config.h"   // gAppConfig (ota_json_url) + AppConfigStore::save()
#include "led.h"

// =============================================================================
// espnow_responder.h  —  Serve credential bundles to bootstrapping siblings
//
// Listens for CREDENTIAL_REQUESTs in OPERATIONAL state.
// Encrypts a WireBundle (175 bytes) per request — fits in 250-byte ESP-NOW limit.
// The requester scans channels 1–13 to find us — no fixed channel constant needed.
// =============================================================================

static CredentialBundle _localBundle;
static bool             _responderActive = false;

// ── Active broker address (set after discovery, served to siblings) ────────────
static char     _activeBrokerHost[64] = {0};
static uint16_t _activeBrokerPort     = 0;

// Call this from setup() once discoverBroker() returns a result.
// Enables BROKER_REQ responses — siblings can ask us for the broker address.
inline void espnowResponderSetBroker(const char* host, uint16_t port) {
    strncpy(_activeBrokerHost, host, sizeof(_activeBrokerHost) - 1);
    _activeBrokerHost[sizeof(_activeBrokerHost) - 1] = '\0';
    _activeBrokerPort = port;
}

// ── Sibling broker response state (requester side) ────────────────────────────
static volatile bool _brokerRespReceived = false;
static char          _siblingBrokerHost[64] = {0};
static uint16_t      _siblingBrokerPort     = 0;


// ── Health flags (used by optional primary selection) ─────────────────────────
// Tracks this node's own connectivity health for advertisement to siblings.
// Updated by mqtt_client.h and ota.h whenever state changes.
//
// Bit assignments (match SiblingHealth.health_flags in espnow_bootstrap.h):
//   bit 0 — WiFi connected
//   bit 1 — MQTT connected
//   bit 2 — GitHub reachable (last OTA JSON fetch succeeded)
static volatile uint8_t _responderHealthFlags = 0x00;

// Set or clear a single health flag bit. Safe to call from any context.
// bit: 0=WiFi, 1=MQTT, 2=GitHub
inline void responderSetHealthFlag(uint8_t bit, bool set) {
    if (set) _responderHealthFlags |=  (uint8_t)(1u << bit);
    else      _responderHealthFlags &= (uint8_t)~(1u << bit);
}


// ── OTA URL sharing state ──────────────────────────────────────────────────────
// Written by onEspNowOtaUrlResponse() when a sibling's URL arrives.
// Read by espnowRequestOtaUrl() which blocks on _otaUrlRespReceived.
static volatile bool _otaUrlRespReceived = false;
static char          _receivedOtaUrl[201] = {0};   // max 200 chars + null


// ── Per-MAC request rate limiting ─────────────────────────────────────────────
// Prevents a rebooting or misbehaving node from flooding siblings with requests.
// Each slot stores the MAC and the millis() timestamp of the last response sent.
// On overflow, the oldest slot is evicted (LRU via a round-robin write pointer).
#define RESPONDER_COOLDOWN_MS  30000   // Ignore repeat requests within 30 s
#define RESPONDER_MAC_SLOTS    8       // Track up to 8 distinct requesters

static struct {
    uint8_t  mac[6];
    uint32_t last_ms;
} _respCooldown[RESPONDER_MAC_SLOTS];
static uint8_t _respCooldownNext = 0;   // Next slot to evict (round-robin)

// Returns true if the given MAC is within its cooldown window.
// If not, records the MAC and current time, then returns false (caller may serve).
static bool responderIsRateLimited(const uint8_t* mac) {
    uint32_t now = millis();
    for (int i = 0; i < RESPONDER_MAC_SLOTS; i++) {
        if (memcmp(_respCooldown[i].mac, mac, 6) == 0) {
            if (now - _respCooldown[i].last_ms < RESPONDER_COOLDOWN_MS) {
                return true;   // Still in cooldown
            }
            // Cooldown expired — update timestamp and allow
            _respCooldown[i].last_ms = now;
            return false;
        }
    }
    // MAC not seen before — record in the next round-robin slot
    memcpy(_respCooldown[_respCooldownNext].mac, mac, 6);
    _respCooldown[_respCooldownNext].last_ms = now;
    _respCooldownNext = (_respCooldownNext + 1) % RESPONDER_MAC_SLOTS;
    return false;
}

void espnowResponderSetBundle(const CredentialBundle& b) {
    memcpy(&_localBundle, &b, sizeof(CredentialBundle));
    _responderActive = true;
}

// Called when a REQUEST arrives
void onEspNowRequest(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
    const uint8_t* requesterMac = recvInfo->src_addr;

    Serial.printf("[ESP-NOW Responder] REQ from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
                  requesterMac[0], requesterMac[1], requesterMac[2],
                  requesterMac[3], requesterMac[4], requesterMac[5], len);

    if (!_responderActive) {
        Serial.println("[ESP-NOW Responder] Not active yet");
        return;
    }
    if (responderIsRateLimited(requesterMac)) {
        Serial.printf("[ESP-NOW Responder] Rate-limited %02X:%02X:%02X:%02X:%02X:%02X\n",
                      requesterMac[0], requesterMac[1], requesterMac[2],
                      requesterMac[3], requesterMac[4], requesterMac[5]);
        return;
    }
    if (len != (int)REQ_LEN) {
        Serial.printf("[ESP-NOW Responder] Bad length %d (expected %d)\n", len, REQ_LEN);
        return;
    }
    if (data[0] != ESPNOW_MSG_CREDENTIAL_REQ) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) {
        Serial.printf("[ESP-NOW Responder] Version mismatch %d\n", data[1]);
        return;
    }

    const uint8_t* requesterPubKey = data + 8;  // 32 bytes

    // Generate ephemeral responder keypair
    EcdhContext respCtx;
    if (!cryptoGenKeypair(respCtx)) {
        Serial.println("[ESP-NOW Responder] Keygen failed");
        return;
    }

    // Derive shared AES key — use a copy so respCtx.publicKey is still valid for the response
    EcdhContext keyCtx;
    memcpy(&keyCtx, &respCtx, sizeof(keyCtx));
    uint8_t aesKey[AES_KEY_LEN];
    if (!cryptoDeriveKey(keyCtx, requesterPubKey, aesKey)) {
        Serial.println("[ESP-NOW Responder] Key derivation failed");
        return;
    }

    // Serialise to WireBundle
    WireBundle wire;
    bundleToWire(_localBundle, wire);

    // Encrypt
    // Encrypted payload layout: [nonce 12B][ciphertext 175B][tag 16B] = 203 bytes
    uint8_t encBuf[sizeof(WireBundle) + GCM_NONCE_LEN + GCM_TAG_LEN + 4];
    size_t  encLen = 0;
    if (!cryptoEncrypt(aesKey, (const uint8_t*)&wire, sizeof(WireBundle), encBuf, encLen)) {
        Serial.println("[ESP-NOW Responder] Encryption failed");
        memset(aesKey, 0, AES_KEY_LEN);
        memset(&wire, 0, sizeof(wire));
        return;
    }
    memset(aesKey, 0, AES_KEY_LEN);
    memset(&wire, 0, sizeof(wire));

    // Build response packet
    // [type 1][ver 1][mac 6][responder_pubkey 32][payload_len 2][nonce+ct+tag 203] = 245 bytes
    size_t totalLen = 1 + 1 + 6 + CURVE25519_KEY_LEN + 2 + encLen;
    Serial.printf("[ESP-NOW Responder] Response size: %d bytes (limit 250)\n", (int)totalLen);
    if (totalLen > 250) {
        Serial.println("[ESP-NOW Responder] Response too large — this should not happen");
        return;
    }

    uint8_t resp[250] = {0};
    size_t  idx = 0;
    resp[idx++] = ESPNOW_MSG_CREDENTIAL_RESP;
    resp[idx++] = ESPNOW_PROTOCOL_VERSION;

    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    memcpy(&resp[idx], myMac, 6);                              idx += 6;
    memcpy(&resp[idx], respCtx.publicKey, CURVE25519_KEY_LEN); idx += CURVE25519_KEY_LEN;

    uint16_t plen = (uint16_t)encLen;
    memcpy(&resp[idx], &plen, 2);                              idx += 2;
    memcpy(&resp[idx], encBuf, encLen);                        idx += encLen;

    // Register requester as peer on the fixed channel and send
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, requesterMac, 6);
    peer.channel = 0;   // 0 = current Wi-Fi channel (matches router in OPERATIONAL mode)
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(requesterMac)) {
        esp_now_add_peer(&peer);
    }

    esp_err_t err = esp_now_send(requesterMac, resp, idx);
    if (err == ESP_OK) {
        Serial.println("[ESP-NOW Responder] Bundle sent to sibling");
        ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    } else {
        Serial.printf("[ESP-NOW Responder] Send failed: %d\n", err);
    }
}

// ── OTA URL request handler (responder side) ──────────────────────────────────
// Answers an OTA_URL_REQ from any sibling by sending our own OTA JSON URL.
// Only responds if health flag bit 2 is set — confirming our URL actually works.
static void onEspNowOtaUrlRequest(const esp_now_recv_info_t* recvInfo,
                                   const uint8_t* data, int len) {
    if (!_responderActive) return;
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
    if (!(_responderHealthFlags & (1u << 2))) {
        Serial.println("[ESP-NOW Responder] OTA URL req ignored — own URL not verified");
        return;
    }

    const uint8_t* requesterMac = recvInfo->src_addr;
    size_t urlLen = strnlen(gAppConfig.ota_json_url, 200);
    if (urlLen == 0) {
        Serial.println("[ESP-NOW Responder] OTA URL req ignored — no URL configured");
        return;
    }

    // Wire: [OTA_URL_RESP 1B][version 1B][sender_mac 6B][url_len 1B][url up to 200B]
    uint8_t buf[2 + 6 + 1 + 200];
    buf[0] = ESPNOW_MSG_OTA_URL_RESP;
    buf[1] = ESPNOW_PROTOCOL_VERSION;
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    memcpy(buf + 2, myMac, 6);
    buf[8] = (uint8_t)urlLen;
    memcpy(buf + 9, gAppConfig.ota_json_url, urlLen);
    size_t totalLen = 9 + urlLen;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, requesterMac, 6);
    peer.channel = 0;   // 0 = current Wi-Fi channel
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(requesterMac)) esp_now_add_peer(&peer);
    esp_now_send(requesterMac, buf, totalLen);
    ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    Serial.println("[ESP-NOW Responder] OTA URL sent to requester");
}


// ── OTA URL response handler (requester side, OPERATIONAL mode) ───────────────
// Populates _receivedOtaUrl when a sibling's OTA_URL_RESP arrives.
// Called from espnowReceiveDispatch while this node is OPERATIONAL.
static void onEspNowOtaUrlResponse(const esp_now_recv_info_t* recvInfo,
                                    const uint8_t* data, int len) {
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
    if (len < 10) return;              // 2 header + 6 mac + 1 len_byte + ≥1 char
    if (_otaUrlRespReceived) return;   // Already accepted one — first response wins
    uint8_t urlLen = data[8];
    if (urlLen == 0 || urlLen > 200) return;
    if (len < (int)(9 + urlLen)) return;
    memcpy(_receivedOtaUrl, data + 9, urlLen);
    _receivedOtaUrl[urlLen] = '\0';
    _otaUrlRespReceived = true;
    Serial.printf("[ESP-NOW] OTA URL received from sibling: %s\n", _receivedOtaUrl);
}


// ── espnowRequestOtaUrl ───────────────────────────────────────────────────────
// Broadcasts an OTA_URL_REQ on the existing OPERATIONAL ESP-NOW session and
// waits up to OTA_URL_REQUEST_TIMEOUT_MS for a sibling response.
// If a response arrives, adopts the URL into gAppConfig and persists it to NVS.
// Returns true if the URL was updated, false if no sibling responded.
//
// Called by ota.h when a GitHub fetch fails. ESP-NOW MUST already be initialized
// (espnowResponderStart() was called during OPERATIONAL setup).
bool espnowRequestOtaUrl() {
    _otaUrlRespReceived = false;
    memset(_receivedOtaUrl, 0, sizeof(_receivedOtaUrl));

    // Broadcast peer may already exist from responder setup; add it if not
    esp_now_peer_info_t bcastPeer = {};
    memcpy(bcastPeer.peer_addr, ESPNOW_BROADCAST, 6);
    bcastPeer.channel = 0;   // 0 = current Wi-Fi channel (router's channel in OPERATIONAL mode)
    bcastPeer.encrypt = false;
    if (!esp_now_is_peer_exist(ESPNOW_BROADCAST)) esp_now_add_peer(&bcastPeer);

    uint8_t req[2] = { ESPNOW_MSG_OTA_URL_REQ, ESPNOW_PROTOCOL_VERSION };
    esp_err_t err = esp_now_send(ESPNOW_BROADCAST, req, sizeof(req));
    if (err != ESP_OK) {
        Serial.printf("[ESP-NOW] OTA URL req send failed: %d\n", err);
        return false;
    }
    ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    Serial.println("[ESP-NOW] OTA URL request broadcast — waiting for sibling...");

    uint32_t deadline = millis() + OTA_URL_REQUEST_TIMEOUT_MS;
    while (!_otaUrlRespReceived && millis() < deadline) delay(10);

    if (!_otaUrlRespReceived) {
        Serial.println("[ESP-NOW] No OTA URL response from siblings");
        return false;
    }

    // Adopt the received URL — update live gAppConfig and persist to NVS
    AppConfig cfg;
    memcpy(&cfg, &gAppConfig, sizeof(AppConfig));
    strncpy(cfg.ota_json_url, _receivedOtaUrl, sizeof(cfg.ota_json_url) - 1);
    cfg.ota_json_url[sizeof(cfg.ota_json_url) - 1] = '\0';
    AppConfigStore::save(cfg);
    Serial.printf("[ESP-NOW] OTA URL updated to: %s\n", gAppConfig.ota_json_url);
    return true;
}


#ifdef SIBLING_PRIMARY_SELECTION
// ── HEALTH_QUERY handler (primary selection) ──────────────────────────────────
// Responds to a sibling's health query with this node's firmware version and
// connectivity health flags. The querying node uses this to pick the best
// sibling before sending a unicast credential request.
//
// Wire format sent: [msg_type 1B][protocol_version 1B][SiblingHealth 17B] = 19 bytes
// SiblingHealth is defined in espnow_bootstrap.h (included before this file in .ino).
static void onEspNowHealthQuery(const esp_now_recv_info_t* recvInfo,
                                const uint8_t* data, int len) {
    if (!_responderActive) return;
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;   // Version gate

    const uint8_t* requesterMac = recvInfo->src_addr;

    SiblingHealth h;
    memset(&h, 0, sizeof(h));
    esp_wifi_get_mac(WIFI_IF_STA, h.mac);
    h.fw_version_uint32 = fwVersionToUint32(FIRMWARE_VERSION);
    h.health_flags      = _responderHealthFlags;
    h.protocol_version  = ESPNOW_PROTOCOL_VERSION;

    // Wire: [HEALTH_RESP][version][SiblingHealth]
    uint8_t buf[2 + sizeof(SiblingHealth)];
    buf[0] = ESPNOW_MSG_HEALTH_RESP;
    buf[1] = ESPNOW_PROTOCOL_VERSION;
    memcpy(buf + 2, &h, sizeof(SiblingHealth));

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, requesterMac, 6);
    peer.channel = 0;   // 0 = current Wi-Fi channel
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(requesterMac)) esp_now_add_peer(&peer);

    esp_now_send(requesterMac, buf, sizeof(buf));
    Serial.printf("[ESP-NOW Responder] HEALTH_RESP → %02X:%02X:%02X:%02X:%02X:%02X"
                  "  fw=0x%06X flags=0x%02X\n",
                  requesterMac[0], requesterMac[1], requesterMac[2],
                  requesterMac[3], requesterMac[4], requesterMac[5],
                  h.fw_version_uint32, h.health_flags);
}
#endif // SIBLING_PRIMARY_SELECTION


// ── BROKER_REQ handler (responder side) ──────────────────────────────────────
// Answers a sibling's broker query with our currently connected broker address.
// Only responds if a broker address has been set via espnowResponderSetBroker().
//
// Wire format sent: [BROKER_RESP 1B][version 1B][host_len 1B][host up to 63B][port 2B]
static void onEspNowBrokerRequest(const esp_now_recv_info_t* recvInfo,
                                  const uint8_t* data, int len) {
    if (!_responderActive) return;
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
    if (_activeBrokerHost[0] == '\0' || _activeBrokerPort == 0) {
        Serial.println("[ESP-NOW Responder] BROKER_REQ ignored — no broker set yet");
        return;
    }

    const uint8_t* requesterMac = recvInfo->src_addr;
    uint8_t hostLen = (uint8_t)strnlen(_activeBrokerHost, 63);

    // Wire: [BROKER_RESP][version][host_len][host...][port 2B]
    uint8_t buf[2 + 1 + 63 + 2];
    buf[0] = ESPNOW_MSG_BROKER_RESP;
    buf[1] = ESPNOW_PROTOCOL_VERSION;
    buf[2] = hostLen;
    memcpy(buf + 3, _activeBrokerHost, hostLen);
    memcpy(buf + 3 + hostLen, &_activeBrokerPort, 2);
    size_t totalLen = 3 + hostLen + 2;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, requesterMac, 6);
    peer.channel = 0;   // current Wi-Fi channel
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(requesterMac)) esp_now_add_peer(&peer);

    esp_now_send(requesterMac, buf, totalLen);
    Serial.printf("[ESP-NOW Responder] BROKER_RESP → sibling: %s:%d\n",
                  _activeBrokerHost, _activeBrokerPort);
}


// ── BROKER_RESP handler (requester side) ─────────────────────────────────────
// Stores the broker address received from a sibling. Called from the dispatcher
// when this node has broadcast a BROKER_REQ and a sibling replies.
static void onEspNowBrokerResponse(const esp_now_recv_info_t* recvInfo,
                                   const uint8_t* data, int len) {
    if (_brokerRespReceived) return;   // first response wins
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
    if (len < 5) return;               // need at least 1 byte host + 2 byte port
    uint8_t hostLen = data[2];
    if (hostLen == 0 || hostLen > 63) return;
    if (len < (int)(3 + hostLen + 2)) return;
    memcpy(_siblingBrokerHost, data + 3, hostLen);
    _siblingBrokerHost[hostLen] = '\0';
    memcpy(&_siblingBrokerPort, data + 3 + hostLen, 2);
    _brokerRespReceived = true;
    Serial.printf("[ESP-NOW] BROKER_RESP from sibling: %s:%d\n",
                  _siblingBrokerHost, _siblingBrokerPort);
}


// ── espnowGetSiblingBroker ────────────────────────────────────────────────────
// Broadcasts a BROKER_REQ on the current Wi-Fi channel and waits up to
// BROKER_ESPNOW_TIMEOUT_MS for any sibling to reply with their broker address.
// Returns true and populates host/port if a sibling responded.
// ESP-NOW MUST already be initialized (espnowResponderStart() called).
bool espnowGetSiblingBroker(char* hostOut, size_t hostOutLen, uint16_t* portOut) {
    _brokerRespReceived = false;
    memset(_siblingBrokerHost, 0, sizeof(_siblingBrokerHost));
    _siblingBrokerPort = 0;

    // Ensure the broadcast peer exists on the current channel
    esp_now_peer_info_t bcastPeer = {};
    memcpy(bcastPeer.peer_addr, ESPNOW_BROADCAST, 6);
    bcastPeer.channel = 0;   // current Wi-Fi channel
    bcastPeer.encrypt = false;
    if (!esp_now_is_peer_exist(ESPNOW_BROADCAST)) esp_now_add_peer(&bcastPeer);

    uint8_t req[2] = { ESPNOW_MSG_BROKER_REQ, ESPNOW_PROTOCOL_VERSION };
    esp_err_t err = esp_now_send(ESPNOW_BROADCAST, req, sizeof(req));
    if (err != ESP_OK) {
        Serial.printf("[ESP-NOW] BROKER_REQ send failed: %d\n", err);
        return false;
    }
    Serial.println("[ESP-NOW] BROKER_REQ broadcast — waiting for sibling...");

    uint32_t deadline = millis() + BROKER_ESPNOW_TIMEOUT_MS;
    while (!_brokerRespReceived && millis() < deadline) delay(10);

    if (!_brokerRespReceived) {
        Serial.println("[ESP-NOW] No broker response from siblings");
        return false;
    }

    strncpy(hostOut, _siblingBrokerHost, hostOutLen - 1);
    hostOut[hostOutLen - 1] = '\0';
    *portOut = _siblingBrokerPort;
    return true;
}


// Combined receive dispatcher (used in OPERATIONAL mode)
static void espnowReceiveDispatch(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
    if (len < 2) return;
    switch (data[0]) {
        case ESPNOW_MSG_CREDENTIAL_REQ:
            onEspNowRequest(recvInfo, data, len);
            break;
        case ESPNOW_MSG_CREDENTIAL_RESP:
            // Ignore — only relevant during bootstrap phase
            break;
        case ESPNOW_MSG_OTA_URL_REQ:
            onEspNowOtaUrlRequest(recvInfo, data, len);
            break;
        case ESPNOW_MSG_OTA_URL_RESP:
            onEspNowOtaUrlResponse(recvInfo, data, len);
            break;
        case ESPNOW_MSG_BROKER_REQ:
            onEspNowBrokerRequest(recvInfo, data, len);
            break;
        case ESPNOW_MSG_BROKER_RESP:
            onEspNowBrokerResponse(recvInfo, data, len);
            break;
#ifdef SIBLING_PRIMARY_SELECTION
        case ESPNOW_MSG_HEALTH_QUERY:
            onEspNowHealthQuery(recvInfo, data, len);
            break;
#endif
        default:
            break;
    }
}

void espnowResponderStart() {
    // Do NOT call esp_wifi_set_channel() here. In OPERATIONAL mode Wi-Fi is
    // already connected and the radio is locked to the router's channel.
    // Forcing a different channel would drop the Wi-Fi connection.
    // ESP-NOW automatically uses the current Wi-Fi channel; peers are
    // registered with channel=0 so their responses go out on the same channel.
    esp_now_init();
    esp_now_register_recv_cb(espnowReceiveDispatch);
    uint8_t ch = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &second);
    Serial.printf("[ESP-NOW Responder] Listening on Wi-Fi channel %d\n", ch);
}
