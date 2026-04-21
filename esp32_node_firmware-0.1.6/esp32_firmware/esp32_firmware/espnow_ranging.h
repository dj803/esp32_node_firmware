#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include "config.h"
#include "mqtt_client.h"      // mqttPublishJson(), mqttIsConnected()
#include "ranging_math.h"     // rssiToDistance()
#include "mac_utils.h"        // macToString()
#include "peer_tracker.h"     // PeerTracker<N>

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
//   via rssiToDistance() from ranging_math.h — the formula is no longer
//   duplicated between this module and ble.h.
//
//   Peer state is kept in a PeerTracker<ESPNOW_MAX_TRACKED> (see peer_tracker.h).
//   Stale entries (no frame seen for ESPNOW_STALE_MS) are evicted on every
//   loop tick. The LRU eviction policy means the most-recently-seen peers are
//   always retained when the table is full.
//
//   Every ESPNOW_MQTT_PUBLISH_MS, espnowRangingLoop() publishes to MQTT:
//     topic:  <node_prefix>/espnow
//     payload: { "peer_count": N,
//                "peers": [ { "mac": "XX:XX:XX:XX:XX:XX",
//                             "rssi": -65,
//                             "dist_m": "2.3" }, … ] }
//
// INCLUDE ORDER:
//   espnow_ranging.h MUST come after mqtt_client.h in the .ino include list.
//   espnow_responder.h forward-declares espnowRangingObserve() so the receive
//   dispatcher can call it before this file is fully included.
// =============================================================================


// ── Peer table ────────────────────────────────────────────────────────────────
// Replaces the four parallel arrays (_enrMac/_enrRssi/_enrDistM/_enrSeenMs)
// that existed in the previous version. PeerTracker<N> encapsulates LRU
// eviction, observe(), expire(), forEach(), and count() in one place.
static PeerTracker<ESPNOW_MAX_TRACKED> _enrPeers;


// ── espnowRangingObserve ──────────────────────────────────────────────────────
// Called by espnow_responder.h's receive dispatcher for EVERY incoming frame.
// Updates (or creates) the slot for this sender MAC with the current RSSI.
void espnowRangingObserve(const uint8_t* mac6, int8_t rssi) {
    char macStr[18];
    macToString(mac6, macStr);
    float dist = rssiToDistance(rssi, ESPNOW_TX_POWER_DBM, ESPNOW_PATH_LOSS_N);
    _enrPeers.setNow(millis());
    _enrPeers.observe(macStr, rssi, dist);
}


// ── Ranging beacon broadcast ──────────────────────────────────────────────────
static uint32_t _enrLastBeacon = 0;

static void _enrSendBeacon() {
    // Minimal 2-byte frame: [msg_type][placeholder]
    // Receivers extract the RSSI from recvInfo->rx_ctrl->rssi — no payload needed.
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
    _enrPeers.setNow(now);

    // ── 1. Beacon broadcast ───────────────────────────────────────────────────
    if (now - _enrLastBeacon >= ESPNOW_BEACON_INTERVAL_MS) {
        _enrLastBeacon = now;
        _enrSendBeacon();
    }

    // ── 2. Expire stale peers ─────────────────────────────────────────────────
    _enrPeers.expire(ESPNOW_STALE_MS);

    // ── 3. MQTT publish ───────────────────────────────────────────────────────
    if (!mqttIsConnected()) return;
    if (now - _enrLastPublish < ESPNOW_MQTT_PUBLISH_MS) return;
    _enrLastPublish = now;

    JsonDocument doc;
    doc["peer_count"] = _enrPeers.count();
    JsonArray arr = doc["peers"].to<JsonArray>();

    _enrPeers.forEach([&arr](const PeerEntry& p) {
        JsonObject o = arr.add<JsonObject>();
        o["mac"]    = p.mac;
        o["rssi"]   = p.rssi;
        o["dist_m"] = String(p.distM, 1);
        Serial.printf("[ESP-NOW Ranging] %s  rssi=%d  dist=%.1fm\n",
                      p.mac, p.rssi, p.distM);
    });

    mqttPublishJson("espnow", doc);
}
