#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "credentials.h"
#include "crypto.h"

// =============================================================================
// espnow_responder.h  —  Serve credential bundles to bootstrapping siblings
//
// Listens for CREDENTIAL_REQUESTs in OPERATIONAL state.
// Encrypts a WireBundle (175 bytes) per request — fits in 250-byte ESP-NOW limit.
// Uses the same ESPNOW_CHANNEL as espnow_bootstrap.h.
// =============================================================================

static CredentialBundle _localBundle;
static bool             _responderActive = false;

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
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(requesterMac)) {
        esp_now_add_peer(&peer);
    }

    esp_err_t err = esp_now_send(requesterMac, resp, idx);
    if (err == ESP_OK) {
        Serial.println("[ESP-NOW Responder] Bundle sent to sibling");
    } else {
        Serial.printf("[ESP-NOW Responder] Send failed: %d\n", err);
    }
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
        default:
            break;
    }
}

void espnowResponderStart() {
    // Ensure we're on the fixed channel
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_now_init();
    esp_now_register_recv_cb(espnowReceiveDispatch);
    Serial.printf("[ESP-NOW Responder] Listening on channel %d\n", ESPNOW_CHANNEL);
}
