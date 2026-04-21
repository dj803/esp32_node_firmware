#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include "config.h"
#include "mqtt_client.h"   // mqttPublish(), mqttConnected() — included BEFORE this file in .ino

// =============================================================================
// espnow_ranging.h  —  Passive RSSI-based distance estimation for ESP-NOW peers
//
// HOW IT WORKS:
//   Every ESPNOW_BEACON_INTERVAL_MS this node broadcasts a tiny ranging beacon
//   (ESPNOW_MSG_RANGING_BEACON = 0x09).  Every sibling does the same.
//
//   When ANY ESP-NOW frame arrives (not just ranging beacons), the dispatcher
//   in espnow_responder.h calls espnowRangingObserve(mac, rssi) so that even
//   application traffic contributes to the ranging estimate — ranging beacons
//   just guarantee a minimum update rate when no other traffic is flowing.
//
//   Distance is estimated with the same log-distance path-loss model as ble.h:
//     d = 10 ^ ((txPower - rssi) / (10 × n))
//   where txPower = ESPNOW_TX_POWER_DBM (-59 dBm) and n = ESPNOW_PATH_LOSS_N.
//
//   Peer state is kept in a flat array of ESPNOW_MAX_TRACKED slots (same
//   pattern as _bleTrackedMac / _bleTrackedRssi in ble.h).  Stale entries
//   (no frame seen for ESPNOW_STALE_MS) are evicted on every loop tick.
//
//   Every ESPNOW_MQTT_PUBLISH_MS, espnowRangingLoop() publishes to MQTT:
//     topic:  <node_prefix>/espnow
//     payload: { "peer_count": N,
//                "peers": [ { "mac": "XX:XX:XX:XX:XX:XX",
//                             "rssi": -65,
//                             "dist_m": "2.3" }, … ] }
//
// INCLUDE ORDER:
//   espnow_ranging.h MUST come after mqtt_client.h in the .ino include list
//   (uses mqttPublish / mqttConnected).
//   espnow_responder.h forward-declares espnowRangingObserve() so the receive
//   dispatcher can call it before this file is fully included.
// =============================================================================


// ── Per-peer tracking state ───────────────────────────────────────────────────
static char     _enrMac   [ESPNOW_MAX_TRACKED][18] = {};   // "XX:XX:XX:XX:XX:XX\0"
static int8_t   _enrRssi  [ESPNOW_MAX_TRACKED]      = {};
static float    _enrDistM [ESPNOW_MAX_TRACKED]       = {};
static uint32_t _enrSeenMs[ESPNOW_MAX_TRACKED]       = {};


// ── Distance formula (mirrors _bleCalcDist in ble.h) ─────────────────────────
static float _enrCalcDist(int8_t rssi) {
    return powf(10.0f, (float)(ESPNOW_TX_POWER_DBM - rssi) / (10.0f * ESPNOW_PATH_LOSS_N));
}


// ── espnowRangingObserve ──────────────────────────────────────────────────────
// Called by espnow_responder.h's receive dispatcher for EVERY incoming frame.
// Updates (or creates) the slot for this sender MAC with the current RSSI.
// When all slots are occupied and the MAC is new, the least-recently-seen
// slot is evicted (LRU — same approach as the responder's rate-limit table).
void espnowRangingObserve(const uint8_t* mac6, int8_t rssi) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac6[0], mac6[1], mac6[2], mac6[3], mac6[4], mac6[5]);

    // Search for an existing slot for this MAC; note the oldest seen while scanning
    int  slot   = -1;
    int  oldest = 0;
    for (int i = 0; i < ESPNOW_MAX_TRACKED; i++) {
        if (strcmp(_enrMac[i], macStr) == 0) { slot = i; break; }   // found
        if (_enrMac[i][0] == '\0')           { slot = i; break; }   // empty slot
        if (_enrSeenMs[i] < _enrSeenMs[oldest]) oldest = i;
    }
    if (slot < 0) slot = oldest;   // evict LRU when table is full

    strncpy(_enrMac[slot], macStr, 17);
    _enrMac[slot][17] = '\0';
    _enrRssi[slot]    = rssi;
    _enrDistM[slot]   = _enrCalcDist(rssi);
    _enrSeenMs[slot]  = millis();
}


// ── Ranging beacon broadcast ──────────────────────────────────────────────────
static uint32_t _enrLastBeacon = 0;

static void _enrSendBeacon() {
    // Minimal 2-byte frame: [msg_type][placeholder]
    // Receivers extract the RSSI from recvInfo->rx_power — no payload needed.
    uint8_t buf[2] = { ESPNOW_MSG_RANGING_BEACON, 0x01 };
    uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    // Ensure the broadcast peer is registered (may not exist if no prior broadcast sent)
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcast, 6);
    peer.channel = 0;     // 0 = current Wi-Fi channel
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(broadcast)) esp_now_add_peer(&peer);

    esp_err_t err = esp_now_send(broadcast, buf, sizeof(buf));
    if (err != ESP_OK) {
        // Common causes: peer not added yet (ESP_ERR_ESPNOW_NOT_FOUND),
        // radio busy (ESP_ERR_ESPNOW_NO_MEM), or Wi-Fi not yet started.
        Serial.printf("[ESP-NOW Ranging] Beacon send failed: %s\n", esp_err_to_name(err));
    }
}


// ── espnowRangingLoop ─────────────────────────────────────────────────────────
// Call from loop().  Handles three periodic tasks:
//   1. Broadcast a ranging beacon every ESPNOW_BEACON_INTERVAL_MS
//   2. Evict peers that have been silent for ESPNOW_STALE_MS
//   3. Publish the peer table to MQTT every ESPNOW_MQTT_PUBLISH_MS
static uint32_t _enrLastPublish = 0;

void espnowRangingLoop() {
    uint32_t now = millis();

    // ── 1. Beacon broadcast ───────────────────────────────────────────────────
    if (now - _enrLastBeacon >= ESPNOW_BEACON_INTERVAL_MS) {
        _enrLastBeacon = now;
        _enrSendBeacon();
    }

    // ── 2. Expire stale peers ─────────────────────────────────────────────────
    for (int i = 0; i < ESPNOW_MAX_TRACKED; i++) {
        if (_enrMac[i][0] && (now - _enrSeenMs[i] > ESPNOW_STALE_MS)) {
            Serial.printf("[ESP-NOW Ranging] Peer %s stale — removing\n", _enrMac[i]);
            memset(_enrMac[i], 0, sizeof(_enrMac[i]));
        }
    }

    // ── 3. MQTT publish ───────────────────────────────────────────────────────
    if (!mqttIsConnected()) return;
    if (now - _enrLastPublish < ESPNOW_MQTT_PUBLISH_MS) return;
    _enrLastPublish = now;

    int count = 0;
    for (int i = 0; i < ESPNOW_MAX_TRACKED; i++) if (_enrMac[i][0]) count++;

    JsonDocument doc;
    doc["peer_count"] = count;
    JsonArray arr = doc["peers"].to<JsonArray>();
    for (int i = 0; i < ESPNOW_MAX_TRACKED; i++) {
        if (!_enrMac[i][0]) continue;
        JsonObject o = arr.add<JsonObject>();
        o["mac"]    = _enrMac[i];
        o["rssi"]   = _enrRssi[i];
        o["dist_m"] = String(_enrDistM[i], 1);
        Serial.printf("[ESP-NOW Ranging] %s  rssi=%d  dist=%.1fm\n",
                      _enrMac[i], _enrRssi[i], _enrDistM[i]);
    }

    String payload;
    serializeJson(doc, payload);
    mqttPublish("espnow", payload.c_str());
}
