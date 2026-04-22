#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"
#include "app_config.h"       // gAppConfig: calibration + filter params
#include "mqtt_client.h"      // mqttPublishJson(), mqttIsConnected()
#include "ranging_math.h"     // rssiToDistance()
#include "mac_utils.h"        // macToString()
#include "peer_tracker.h"     // PeerTracker<N>

// =============================================================================
// espnow_ranging.h  —  Passive RSSI-based distance estimation for ESP-NOW peers
//
// HOW IT WORKS:
//   Every ESPNOW_BEACON_INTERVAL_MS ± 20 % jitter this node broadcasts a tiny
//   ranging beacon (ESPNOW_MSG_RANGING_BEACON = 0x09).  Every sibling does the
//   same.
//
//   When ANY ESP-NOW frame arrives, espnow_responder.h calls
//   espnowRangingObserve(mac, rssi). The observation feeds two things:
//     1. PeerTracker EMA + outlier filter (F4) — smooths raw RSSI into a stable
//        distance estimate published to MQTT.
//     2. Calibration sample collector (F3) — when a calibration step is in
//        progress, raw RSSI is recorded into a circular buffer; the median is
//        published via mqttPublish("response", …) for operator review.
//
//   Calibration (F3):
//     cmd/espnow/calibrate → espnowCalibrateCmd()
//       {"cmd":"measure_1m","peer_mac":"AA:BB:..","samples":30}
//         Collect 30 raw RSSI samples from that peer; publish median as tx_power.
//       {"cmd":"measure_d","peer_mac":"AA:BB:..","distance_m":4.0,"samples":30}
//         Collect 30 samples; compute n = (tx_power - rssi_d)/(10·log10(dist));
//         publish for review.
//       {"cmd":"commit","tx_power_dbm":-47,"path_loss_n":2.7}
//         Operator-confirmed values → NVS via AppConfigStore::save().
//       {"cmd":"reset"}
//         Restore compile-time defaults.
//
//   Drift filter (F4):
//     cmd/espnow/filter → espnowSetFilter()
//       {"alpha_x100":30,"outlier_db":15}
//         Update gAppConfig + NVS.
// =============================================================================


// ── Peer table ────────────────────────────────────────────────────────────────
static PeerTracker<ESPNOW_MAX_TRACKED> _enrPeers;

// ── Enable / disable flag ─────────────────────────────────────────────────────
static bool _rangingEnabled = false;

void espnowRangingSetEnabled(bool en) {
    if (en == _rangingEnabled) return;
    _rangingEnabled = en;
    Serial.printf("[ESP-NOW Ranging] ranging %s\n", en ? "ENABLED" : "DISABLED");
}


// ── MAC publish filter (F2) ───────────────────────────────────────────────────
// When non-empty, only peers whose MAC is in _filterMacs are included in the
// MQTT publish. Observations still go into PeerTracker for all peers so that
// switching the filter is instant (no warm-up delay needed).
static char    _filterMacs[ESPNOW_MAX_TRACKED][18] = {};
static uint8_t _filterCount = 0;

static bool _enrIsFiltered(const char* mac) {
    if (_filterCount == 0) return true;   // no filter → publish all
    for (uint8_t i = 0; i < _filterCount; i++) {
        if (strcmp(_filterMacs[i], mac) == 0) return true;
    }
    return false;
}

void espnowSetTrackedMacs(const char** macs, uint8_t n) {
    _filterCount = 0;
    for (uint8_t i = 0; i < n && i < ESPNOW_MAX_TRACKED; i++) {
        if (!macs[i] || strlen(macs[i]) != 17) continue;
        strncpy(_filterMacs[_filterCount], macs[i], 17);
        _filterMacs[_filterCount][17] = '\0';
        _filterCount++;
    }
    Serial.printf("[ESP-NOW Ranging] track filter: %u MACs\n", (unsigned)_filterCount);
}


// ── Calibration state machine (F3) ───────────────────────────────────────────

enum class EnrCalibState : uint8_t { IDLE, MEASURING_1M, MEASURING_D };

static EnrCalibState _calibState      = EnrCalibState::IDLE;
static char          _calibPeerMac[18] = {};
static int8_t        _calibBuf[ESPNOW_CALIBRATION_SAMPLES];
static uint8_t       _calibCount      = 0;
static uint8_t       _calibTarget     = ESPNOW_CALIBRATION_SAMPLES;
static float         _calibDistanceM  = 0.0f;  // set by measure_d command
static int8_t        _calibTxPower    = 0;     // median from measure_1m step
static uint32_t      _calibStartMs    = 0;

// Simple insertion-sort median on a copy of buf[0..n-1].
static int8_t _enrMedian(const int8_t* src, uint8_t n) {
    int8_t tmp[ESPNOW_CALIBRATION_SAMPLES];
    memcpy(tmp, src, n);
    for (uint8_t i = 1; i < n; i++) {
        int8_t key = tmp[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[n / 2];
}

// Called from espnowRangingObserve when a calibration step is in progress.
// Returns immediately if not calibrating, or peer MAC doesn't match.
static void _enrCalibrateCollect(const char* macStr, int8_t rssi) {
    if (_calibState == EnrCalibState::IDLE) return;
    if (strcmp(macStr, _calibPeerMac) != 0) return;

    // Timeout guard
    if (millis() - _calibStartMs > ESPNOW_CALIBRATION_TIMEOUT_MS) {
        _calibState = EnrCalibState::IDLE;
        if (mqttIsConnected()) {
            mqttPublish("response",
                String("{\"calib\":\"error\",\"msg\":\"timeout waiting for peer\"}"),
                1, false);
        }
        return;
    }

    _calibBuf[_calibCount++] = rssi;

    // Publish progress every 5 samples
    if (_calibCount % 5 == 0 || _calibCount == _calibTarget) {
        if (mqttIsConnected()) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"progress\",\"collected\":%u,\"total\":%u}",
                (unsigned)_calibCount, (unsigned)_calibTarget);
            mqttPublish("response", String(buf), 1, false);
        }
    }

    if (_calibCount < _calibTarget) return;

    // ── Step complete ─────────────────────────────────────────────────────────
    int8_t median = _enrMedian(_calibBuf, _calibCount);

    if (_calibState == EnrCalibState::MEASURING_1M) {
        _calibTxPower = median;
        _calibState   = EnrCalibState::IDLE;
        if (mqttIsConnected()) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"measure_1m_done\",\"rssi_median\":%d,"
                "\"tx_power_dbm\":%d,\"samples\":%u}",
                (int)median, (int)median, (unsigned)_calibTarget);
            mqttPublish("response", String(buf), 1, false);
        }
        LOG_I("ESP-NOW Calib", "1 m step done: rssi_median=%d", (int)median);

    } else {  // MEASURING_D
        if (_calibDistanceM <= 0.0f || _calibTxPower == 0) {
            _calibState = EnrCalibState::IDLE;
            if (mqttIsConnected())
                mqttPublish("response",
                    String("{\"calib\":\"error\",\"msg\":\"run measure_1m first\"}"),
                    1, false);
            return;
        }
        float logDist = log10f(_calibDistanceM);
        float n = (logDist > 0.0f)
                  ? (float)(_calibTxPower - median) / (10.0f * logDist)
                  : 0.0f;
        _calibState = EnrCalibState::IDLE;
        if (mqttIsConnected()) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"measure_d_done\",\"rssi_median\":%d,"
                "\"distance_m\":%.2f,\"path_loss_n\":%.2f,"
                "\"tx_power_dbm\":%d,\"samples\":%u}",
                (int)median, _calibDistanceM, n,
                (int)_calibTxPower, (unsigned)_calibTarget);
            mqttPublish("response", String(buf), 1, false);
        }
        LOG_I("ESP-NOW Calib", "distance step done: rssi_median=%d n=%.2f", (int)median, n);
    }
}

// ── espnowCalibrateCmd ────────────────────────────────────────────────────────
// Called from mqtt_client.h's onMqttMessage for cmd/espnow/calibrate.
void espnowCalibrateCmd(const char* payload, size_t len) {
    char buf[256];
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        LOG_W("ESP-NOW Calib", "bad JSON in cmd/espnow/calibrate");
        return;
    }

    const char* cmd = doc["cmd"] | "";

    if (strcmp(cmd, "measure_1m") == 0 || strcmp(cmd, "measure_d") == 0) {
        const char* peerMac = doc["peer_mac"] | "";
        if (strlen(peerMac) != 17) {
            LOG_W("ESP-NOW Calib", "missing/invalid peer_mac");
            return;
        }
        uint8_t samples = (uint8_t)(doc["samples"] | ESPNOW_CALIBRATION_SAMPLES);
        if (samples == 0 || samples > ESPNOW_CALIBRATION_SAMPLES)
            samples = ESPNOW_CALIBRATION_SAMPLES;

        strncpy(_calibPeerMac, peerMac, 17);
        _calibPeerMac[17] = '\0';
        _calibCount  = 0;
        _calibTarget = samples;
        _calibStartMs = millis();

        if (strcmp(cmd, "measure_d") == 0) {
            float d = doc["distance_m"] | 0.0f;
            if (d <= 0.0f) {
                LOG_W("ESP-NOW Calib", "measure_d: distance_m must be > 0");
                return;
            }
            _calibDistanceM = d;
            _calibState = EnrCalibState::MEASURING_D;
            LOG_I("ESP-NOW Calib", "measuring at %.2f m from %s (%u samples)",
                  d, peerMac, (unsigned)samples);
        } else {
            _calibState = EnrCalibState::MEASURING_1M;
            LOG_I("ESP-NOW Calib", "measuring at 1 m from %s (%u samples)",
                  peerMac, (unsigned)samples);
        }
        if (mqttIsConnected()) {
            char pb[128];
            snprintf(pb, sizeof(pb),
                "{\"calib\":\"started\",\"cmd\":\"%s\",\"peer_mac\":\"%s\","
                "\"samples\":%u}", cmd, peerMac, (unsigned)samples);
            mqttPublish("response", String(pb), 1, false);
        }

    } else if (strcmp(cmd, "commit") == 0) {
        int   txp = doc["tx_power_dbm"] | (int)ESPNOW_TX_POWER_DBM;
        float pln = doc["path_loss_n"]  | ESPNOW_PATH_LOSS_N;
        if (txp > 0 || txp < -120 || pln < 1.0f || pln > 6.0f) {
            LOG_W("ESP-NOW Calib", "commit: values out of range (txp=%d n=%.2f)", txp, pln);
            return;
        }
        AppConfig copy = gAppConfig;
        copy.espnow_tx_power_dbm    = (int8_t)txp;
        copy.espnow_path_loss_n_x10 = (uint8_t)(pln * 10.0f + 0.5f);
        if (AppConfigStore::save(copy)) {
            LOG_I("ESP-NOW Calib", "committed: tx_power=%d n=%.2f", txp, pln);
            if (mqttIsConnected()) {
                char pb[128];
                snprintf(pb, sizeof(pb),
                    "{\"calib\":\"committed\",\"tx_power_dbm\":%d,\"path_loss_n\":%.2f}",
                    txp, pln);
                mqttPublish("response", String(pb), 1, false);
            }
        } else {
            LOG_E("ESP-NOW Calib", "commit: NVS save failed");
        }

    } else if (strcmp(cmd, "reset") == 0) {
        AppConfig copy = gAppConfig;
        copy.espnow_tx_power_dbm    = ESPNOW_TX_POWER_DBM;
        copy.espnow_path_loss_n_x10 = (uint8_t)(ESPNOW_PATH_LOSS_N * 10);
        copy.espnow_ema_alpha_x100  = ESPNOW_EMA_ALPHA_X100;
        copy.espnow_outlier_db      = ESPNOW_OUTLIER_DB;
        if (AppConfigStore::save(copy)) {
            LOG_I("ESP-NOW Calib", "reset to compile-time defaults");
            if (mqttIsConnected())
                mqttPublish("response",
                    String("{\"calib\":\"reset\"}"), 1, false);
        }
        _calibState = EnrCalibState::IDLE;

    } else {
        LOG_W("ESP-NOW Calib", "unknown cmd '%s'", cmd);
    }
}

// ── espnowSetFilter ───────────────────────────────────────────────────────────
// Called from mqtt_client.h's onMqttMessage for cmd/espnow/filter.
// Payload: {"alpha_x100":30,"outlier_db":15}
void espnowSetFilter(const char* payload, size_t len) {
    char buf[96];
    size_t n = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        LOG_W("ESP-NOW Filter", "bad JSON in cmd/espnow/filter");
        return;
    }

    int alpha   = doc["alpha_x100"] | -1;
    int outlier = doc["outlier_db"] | -1;
    if (alpha < 0 || alpha > 99 || outlier < 0 || outlier > 30) {
        LOG_W("ESP-NOW Filter", "values out of range (alpha_x100=%d outlier_db=%d)",
              alpha, outlier);
        return;
    }

    AppConfig copy = gAppConfig;
    copy.espnow_ema_alpha_x100 = (uint8_t)alpha;
    copy.espnow_outlier_db     = (uint8_t)outlier;
    if (AppConfigStore::save(copy)) {
        LOG_I("ESP-NOW Filter", "updated alpha_x100=%d outlier_db=%d", alpha, outlier);
    } else {
        LOG_E("ESP-NOW Filter", "NVS save failed");
    }
}


// ── espnowRangingObserve ──────────────────────────────────────────────────────
// Called by espnow_responder.h's receive dispatcher for EVERY incoming frame.
void espnowRangingObserve(const uint8_t* mac6, int8_t rssi) {
    if (!_rangingEnabled) return;
    char macStr[18];
    macToString(mac6, macStr);

    // Calibration sample collection — uses raw RSSI, not EMA
    _enrCalibrateCollect(macStr, rssi);

    float dist = rssiToDistance(rssi,
                                gAppConfig.espnow_tx_power_dbm,
                                gAppConfig.espnow_path_loss_n_x10 / 10.0f);
    _enrPeers.setNow(millis());
    _enrPeers.observe(macStr, rssi, dist,
                      gAppConfig.espnow_ema_alpha_x100,
                      gAppConfig.espnow_outlier_db);
}


// ── Ranging beacon broadcast ──────────────────────────────────────────────────
static uint32_t _enrLastBeacon  = 0;
static uint32_t _enrNextBeaconInterval = ESPNOW_BEACON_INTERVAL_MS;

static void _enrSendBeacon() {
    uint8_t buf[2] = { ESPNOW_MSG_RANGING_BEACON, 0x01 };
    uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcast, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(broadcast)) esp_now_add_peer(&peer);

    esp_err_t err = esp_now_send(broadcast, buf, sizeof(buf));
    if (err != ESP_OK) {
        Serial.printf("[ESP-NOW Ranging] Beacon send failed: %s\n", esp_err_to_name(err));
    }

    // Jitter next interval by ±20 % so >30 nodes don't synchronise collisions
    uint32_t jitter = (uint32_t)(esp_random() % (ESPNOW_BEACON_INTERVAL_MS / 5));
    bool subtract = (bool)(esp_random() & 1);
    _enrNextBeaconInterval = subtract
        ? ESPNOW_BEACON_INTERVAL_MS - jitter
        : ESPNOW_BEACON_INTERVAL_MS + jitter;
}


// ── espnowRangingLoop ─────────────────────────────────────────────────────────
// Call from loop(). Handles three periodic tasks:
//   1. Broadcast a ranging beacon every ~ESPNOW_BEACON_INTERVAL_MS (jittered)
//   2. Evict peers silent for ESPNOW_STALE_MS
//   3. Publish peer table to MQTT every ESPNOW_MQTT_PUBLISH_MS
static uint32_t _enrLastPublish = 0;

void espnowRangingLoop() {
    if (!_rangingEnabled) return;

    uint32_t now = millis();
    _enrPeers.setNow(now);

    // ── 1. Beacon broadcast ───────────────────────────────────────────────────
    if (now - _enrLastBeacon >= _enrNextBeaconInterval) {
        _enrLastBeacon = now;
        _enrSendBeacon();
    }

    // ── 2. Expire stale peers ─────────────────────────────────────────────────
    _enrPeers.expire(ESPNOW_STALE_MS);

    // ── 3. MQTT publish ───────────────────────────────────────────────────────
    if (!mqttIsConnected()) return;
    if (now - _enrLastPublish < ESPNOW_MQTT_PUBLISH_MS) return;
    _enrLastPublish = now;

    float txPow = (float)gAppConfig.espnow_tx_power_dbm;
    float pathN = gAppConfig.espnow_path_loss_n_x10 / 10.0f;

    JsonDocument doc;
    doc["node_name"]  = gAppConfig.node_name;
    doc["peer_count"] = _enrPeers.count();
    JsonArray arr = doc["peers"].to<JsonArray>();

    _enrPeers.forEach([&arr, txPow, pathN](const PeerEntry& p) {
        if (!_enrIsFiltered(p.mac)) return;   // F2: skip if not in track filter
        JsonObject o = arr.add<JsonObject>();
        o["mac"]  = p.mac;
        o["rssi"] = p.rssi;

        // Prefer EMA-smoothed RSSI for distance; fall back to raw on first frame.
        if (p.rssi_ema_x10 != 0) {
            float emaRssi = p.rssi_ema_x10 / 10.0f;
            o["rssi_ema"] = (int)roundf(emaRssi);
            o["dist_m"]   = String(rssiToDistance((int8_t)emaRssi, (int8_t)txPow, pathN), 1);
        } else {
            o["rssi_ema"] = p.rssi;
            o["dist_m"]   = String(p.distM, 1);
        }
        o["rejects"] = p.rejects;
        LOG_D("ESP-NOW Ranging", "%s  rssi=%d  ema=%d  dist=%s  rej=%u",
              p.mac, p.rssi, p.rssi_ema_x10 / 10,
              o["dist_m"].as<const char*>(), (unsigned)p.rejects);
    });

    mqttPublishJson("espnow", doc);
}
