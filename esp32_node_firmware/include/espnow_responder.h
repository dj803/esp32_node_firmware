#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "logging.h"    // LOG_I / LOG_W / LOG_E macros
#include "credentials.h"
#include "crypto.h"
#include "app_config.h"   // gAppConfig (ota_json_url) + AppConfigStore::save()
#include "led.h"

#include "espnow_ranging_fwd.h"   // espnowRangingObserve() — defined in espnow_ranging.h

// =============================================================================
// espnow_responder.h  —  Serve credential bundles to bootstrapping siblings
//
// Listens for CREDENTIAL_REQUESTs in OPERATIONAL state.
// Encrypts a WireBundle (175 bytes) per request — fits in 250-byte ESP-NOW limit.
// The requester scans channels 1–13 to find us — no fixed channel constant needed.
// =============================================================================

static CredentialBundle _localBundle;
// _responderActive is module-internal — never read from outside espnow_responder.h.
// Use espnowResponderIsActive() for any external query.
static bool             _responderActive = false;

// Returns true while the ESP-NOW credential responder is running.
inline bool espnowResponderIsActive() { return _responderActive; }

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


// ── Per-MAC request rate limiting (token bucket) ──────────────────────────────
// Prevents a misbehaving sibling from flooding credential requests and forcing
// expensive ECDH + AES cycles on every arrival.
//
// Each tracked MAC gets a token bucket:
//   Capacity:  RATE_BUCKET_CAP tokens
//   Refill:    1 token every RATE_REFILL_MS (60 s)
//   Cost:      1 token per credential request
// A request is dropped (rate-limited) when the bucket is empty.
// When the table is full, the oldest entry (round-robin) is evicted (LRU).
//
// This allows a well-behaved node that reboots 3 times in quick succession to
// still bootstrap successfully (3 tokens), while blocking a sustained flood.
// ─────────────────────────────────────────────────────────────────────────────
#define RATE_BUCKET_CAP   3       // Max tokens per MAC
#define RATE_REFILL_MS    60000   // One token refills per 60 s
#define RATE_MAC_SLOTS    8       // Track up to 8 distinct requesters

#include "rate_limit.h"   // rateClampRefill() — host-testable refill math

static struct {
    uint8_t  mac[6];
    uint8_t  tokens;        // Current token count (0 … RATE_BUCKET_CAP)
    uint32_t lastRefillMs;  // millis() when last refill was applied
} _rateBuckets[RATE_MAC_SLOTS];
static uint8_t _rateBucketNext = 0;   // Round-robin eviction pointer

// Returns true (rate-limited) if the MAC has no tokens left.
// Otherwise consumes one token and returns false (caller may serve).
// Side-effects: refills tokens based on elapsed time before consuming.
static bool responderIsRateLimited(const uint8_t* mac) {
    uint32_t now = millis();

    // Search for an existing bucket for this MAC
    for (int i = 0; i < RATE_MAC_SLOTS; i++) {
        if (memcmp(_rateBuckets[i].mac, mac, 6) != 0) continue;

        // Found — top up tokens based on elapsed time
        uint32_t elapsed = now - _rateBuckets[i].lastRefillMs;
        if (elapsed >= RATE_REFILL_MS) {
            _rateBuckets[i].tokens       = rateClampRefill(_rateBuckets[i].tokens, elapsed);
            _rateBuckets[i].lastRefillMs = now;
        }

        if (_rateBuckets[i].tokens == 0) return true;   // bucket empty — drop
        _rateBuckets[i].tokens--;
        return false;   // token consumed — allow
    }

    // MAC not seen before — evict round-robin slot, init with (CAP-1) tokens
    memcpy(_rateBuckets[_rateBucketNext].mac, mac, 6);
    _rateBuckets[_rateBucketNext].tokens       = RATE_BUCKET_CAP - 1;  // use 1 now
    _rateBuckets[_rateBucketNext].lastRefillMs = now;
    _rateBucketNext = (_rateBucketNext + 1) % RATE_MAC_SLOTS;
    return false;   // allowed (first request from this MAC)
}

void espnowResponderSetBundle(const CredentialBundle& b) {
    memcpy(&_localBundle, &b, sizeof(CredentialBundle));
    _responderActive = true;
}

// Called when a REQUEST arrives
void onEspNowRequest(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len) {
    const uint8_t* requesterMac = recvInfo->src_addr;

    LOG_I("Responder", "REQ from %02X:%02X:%02X:%02X:%02X:%02X len=%d",
          requesterMac[0], requesterMac[1], requesterMac[2],
          requesterMac[3], requesterMac[4], requesterMac[5], len);

    if (!_responderActive) {
        LOG_W("Responder", "Not active yet — ignoring request");
        return;
    }
    if (responderIsRateLimited(requesterMac)) {
        LOG_W("Responder", "Rate-limited %02X:%02X:%02X:%02X:%02X:%02X — dropping",
              requesterMac[0], requesterMac[1], requesterMac[2],
              requesterMac[3], requesterMac[4], requesterMac[5]);
        return;
    }
    if (len != (int)REQ_LEN) {
        LOG_W("Responder", "Bad length %d (expected %d)", len, REQ_LEN);
        return;
    }
    if (data[0] != ESPNOW_MSG_CREDENTIAL_REQ) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) {
        LOG_W("Responder", "Protocol version mismatch %d", data[1]);
        return;
    }

    const uint8_t* requesterPubKey = data + 8;  // 32 bytes

    // Generate ephemeral responder keypair
    EcdhContext respCtx;
    if (!cryptoGenKeypair(respCtx)) {
        LOG_E("Responder", "ECDH keygen failed");
        return;
    }

    // Derive shared AES key — use a copy so respCtx.publicKey is still valid for the response
    EcdhContext keyCtx;
    memcpy(&keyCtx, &respCtx, sizeof(keyCtx));
    uint8_t aesKey[AES_KEY_LEN];
    if (!cryptoDeriveKey(keyCtx, requesterPubKey, aesKey)) {
        LOG_E("Responder", "AES key derivation failed");
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
        LOG_E("Responder", "Encryption failed");
        memset(aesKey, 0, AES_KEY_LEN);
        memset(&wire, 0, sizeof(wire));
        return;
    }
    memset(aesKey, 0, AES_KEY_LEN);
    memset(&wire, 0, sizeof(wire));

    // Build response packet
    // [type 1][ver 1][mac 6][responder_pubkey 32][payload_len 2][nonce+ct+tag 203] = 245 bytes
    size_t totalLen = 1 + 1 + 6 + CURVE25519_KEY_LEN + 2 + encLen;
    LOG_I("Responder", "Response size: %d bytes (limit 250)", (int)totalLen);
    if (totalLen > 250) {
        LOG_E("Responder", "Response too large — this should not happen");
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
        LOG_I("Responder", "Bundle sent to sibling");
        ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    } else {
        LOG_W("Responder", "Send failed: %d", err);
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
        LOG_I("Responder", "OTA URL req ignored — own URL not yet verified");
        return;
    }

    const uint8_t* requesterMac = recvInfo->src_addr;
    size_t urlLen = strnlen(gAppConfig.ota_json_url, 200);
    if (urlLen == 0) {
        LOG_I("Responder", "OTA URL req ignored — no URL configured");
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
    LOG_I("Responder", "OTA URL sent to requester");
}


// Returns true if `s` looks like a safe http(s) URL:
//   - begins with "http://" or "https://"
//   - length strictly within appcfg bounds (NUL terminator + at least one byte
//     after the scheme)
//   - contains no control characters (<0x20 or ==0x7F) — blocks newline/tab
//     injection that would later split HTTP headers or break out of the
//     value="..." attribute in the settings page (even with htmlEscape, defence
//     in depth is cheap)
//
// Applied to URLs arriving on the wire (OTA_URL_RESP) before they are written
// to NVS (and therefore before they can ever be rendered into HTML).
static bool isSafeOtaUrl(const char* s, size_t nBytes) {
    if (!s || nBytes == 0) return false;
    if (nBytes >= APP_CFG_OTA_JSON_URL_LEN) return false;    // no room for NUL
    bool okScheme = (nBytes >= 7 && memcmp(s, "http://",  7) == 0)
                 || (nBytes >= 8 && memcmp(s, "https://", 8) == 0);
    if (!okScheme) return false;
    for (size_t i = 0; i < nBytes; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}


// ── OTA URL response handler (requester side, OPERATIONAL mode) ───────────────
// Populates _receivedOtaUrl when a sibling's OTA_URL_RESP arrives.
// Called from espnowReceiveDispatch while this node is OPERATIONAL.
//
// Defence note: a malicious sibling can craft any payload here. The URL is
// validated (scheme, length, control chars) BEFORE being copied to the buffer
// that will later be written to NVS via espnowRequestOtaUrl(). Combined with
// htmlEscape() in ap_portal.h this closes the XSS chain that previously let a
// sibling poison the admin's /settings page.
static void onEspNowOtaUrlResponse(const esp_now_recv_info_t* recvInfo,
                                    const uint8_t* data, int len) {
    if (len < 2) return;
    if (data[1] != ESPNOW_PROTOCOL_VERSION) return;
    if (len < 10) return;              // 2 header + 6 mac + 1 len_byte + ≥1 char
    if (_otaUrlRespReceived) return;   // Already accepted one — first response wins
    uint8_t urlLen = data[8];
    if (urlLen == 0 || urlLen > 200) return;
    if (len < (int)(9 + urlLen)) return;
    if (!isSafeOtaUrl((const char*)(data + 9), urlLen)) {
        LOG_W("ESP-NOW", "OTA URL response rejected — malformed or non-http(s)");
        return;
    }
    memcpy(_receivedOtaUrl, data + 9, urlLen);
    _receivedOtaUrl[urlLen] = '\0';
    _otaUrlRespReceived = true;
    LOG_I("ESP-NOW", "OTA URL received from sibling: %s", _receivedOtaUrl);
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
        LOG_W("ESP-NOW", "OTA URL req send failed: %d", err);
        return false;
    }
    ledSetPattern(LedPattern::ESPNOW_FLASH);   // brief TX indicator
    LOG_I("ESP-NOW", "OTA URL request broadcast — waiting for sibling...");

    uint32_t deadline = millis() + OTA_URL_REQUEST_TIMEOUT_MS;
    while (!_otaUrlRespReceived && millis() < deadline) delay(10);

    if (!_otaUrlRespReceived) {
        LOG_I("ESP-NOW", "No OTA URL response from siblings");
        return false;
    }

    // Adopt the received URL — update live gAppConfig and persist to NVS
    AppConfig cfg;
    memcpy(&cfg, &gAppConfig, sizeof(AppConfig));
    strncpy(cfg.ota_json_url, _receivedOtaUrl, sizeof(cfg.ota_json_url) - 1);
    cfg.ota_json_url[sizeof(cfg.ota_json_url) - 1] = '\0';
    AppConfigStore::save(cfg);
    LOG_I("ESP-NOW", "OTA URL updated to: %s", gAppConfig.ota_json_url);
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
    LOG_I("Responder", "HEALTH_RESP → %02X:%02X:%02X:%02X:%02X:%02X fw=0x%06X flags=0x%02X",
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
        LOG_W("Responder", "BROKER_REQ ignored — no broker set yet");
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
    LOG_I("Responder", "BROKER_RESP → sibling: %s:%d", _activeBrokerHost, _activeBrokerPort);
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
    LOG_I("Responder", "BROKER_RESP from sibling: %s:%d", _siblingBrokerHost, _siblingBrokerPort);
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
        LOG_E("Responder", "BROKER_REQ send failed: %d", err);
        return false;
    }
    LOG_I("Responder", "BROKER_REQ broadcast — waiting for sibling...");

    uint32_t deadline = millis() + BROKER_ESPNOW_TIMEOUT_MS;
    while (!_brokerRespReceived && millis() < deadline) delay(10);

    if (!_brokerRespReceived) {
        LOG_W("Responder", "No broker response from siblings");
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

    // ── Passive RSSI observation for all frames ───────────────────────────────
    // espnowRangingObserve() is defined in espnow_ranging.h (included after
    // mqtt_client.h in the .ino). The forward declaration at the top of this
    // file lets us call it here without a circular include dependency.
    // rx_ctrl->rssi is the per-packet RSSI in dBm, available in all ESP-IDF versions.
    // Cast to int8_t — the field is signed but typed as int in some SDK headers.
    espnowRangingObserve(recvInfo->src_addr, (int8_t)recvInfo->rx_ctrl->rssi);

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
        case ESPNOW_MSG_RANGING_BEACON:
            // Ranging beacon — RSSI already recorded by espnowRangingObserve() above.
            // No additional action needed; the 2-byte payload carries no data.
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
    LOG_I("Responder", "Listening on Wi-Fi channel %d", ch);
}
