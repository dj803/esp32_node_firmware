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
#include "ota_validation.h"   // (v0.4.08) otaValidationIsPending() — gate beacons during validation window

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
//
// v0.4.07 (#39): adds MEASURING_AT for arbitrary-distance points and a
// shared multi-point buffer that all three measure_* commands populate.
// 'commit' performs least-squares linreg over the buffer when ≥2 points
// are present; falls back to operator-supplied tx_power/n otherwise
// (backwards-compatible with the single-point wizard flow).

enum class EnrCalibState : uint8_t { IDLE, MEASURING_1M, MEASURING_D, MEASURING_AT };

static EnrCalibState _calibState      = EnrCalibState::IDLE;
static char          _calibPeerMac[18] = {};
static int8_t        _calibBuf[ESPNOW_CALIBRATION_SAMPLES];
static uint8_t       _calibCount      = 0;
static uint8_t       _calibTarget     = ESPNOW_CALIBRATION_SAMPLES;
static float         _calibDistanceM  = 0.0f;  // set by measure_d / measure_at command
static int8_t        _calibTxPower    = 0;     // median from measure_1m step (legacy single-point flow)
static uint32_t      _calibStartMs    = 0;

// Multi-point calibration buffer. Every completed measure_1m / measure_d /
// measure_at step appends one entry; commit performs linreg over them all.
struct EnrCalibPoint {
    float   distance_m;
    int8_t  rssi_median;
    uint8_t samples;
};
static EnrCalibPoint _calibPoints[ESPNOW_CALIB_MAX_POINTS];
static uint8_t       _calibPointCount = 0;

// Append a point to the multi-point buffer. If full, evict the OLDEST
// entry (FIFO — newer measurements are typically more trustworthy).
static void _enrCalibPushPoint(float dist_m, int8_t rssi_median, uint8_t samples) {
    if (_calibPointCount < ESPNOW_CALIB_MAX_POINTS) {
        _calibPoints[_calibPointCount++] = { dist_m, rssi_median, samples };
        return;
    }
    // Buffer full — shift left, append at end.
    for (uint8_t i = 1; i < ESPNOW_CALIB_MAX_POINTS; i++) {
        _calibPoints[i - 1] = _calibPoints[i];
    }
    _calibPoints[ESPNOW_CALIB_MAX_POINTS - 1] = { dist_m, rssi_median, samples };
}

static void _enrCalibClearPoints() {
    _calibPointCount = 0;
    memset(_calibPoints, 0, sizeof(_calibPoints));
}

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
        // v0.4.07: also append to multi-point buffer at d=1.0 m so that
        // commit's linreg can use this measurement.
        _enrCalibPushPoint(1.0f, median, _calibTarget);
        if (mqttIsConnected()) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"measure_1m_done\",\"rssi_median\":%d,"
                "\"tx_power_dbm\":%d,\"samples\":%u,\"points\":%u}",
                (int)median, (int)median, (unsigned)_calibTarget,
                (unsigned)_calibPointCount);
            mqttPublish("response", String(buf), 1, false);
        }
        LOG_I("ESP-NOW Calib", "1 m step done: rssi_median=%d points=%u",
              (int)median, (unsigned)_calibPointCount);

    } else if (_calibState == EnrCalibState::MEASURING_D) {
        // Legacy two-step flow: measure_1m -> measure_d -> commit
        // Reports n derived from the (1m, d) pair; also appends the d-point
        // to the multi-point buffer so commit's linreg can use it.
        _enrCalibPushPoint(_calibDistanceM, median, _calibTarget);
        float logDist = log10f(_calibDistanceM);
        float n = (logDist != 0.0f && _calibTxPower != 0)
                  ? (float)(_calibTxPower - median) / (10.0f * logDist)
                  : 0.0f;
        _calibState = EnrCalibState::IDLE;
        if (mqttIsConnected()) {
            char buf[224];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"measure_d_done\",\"rssi_median\":%d,"
                "\"distance_m\":%.2f,\"path_loss_n\":%.2f,"
                "\"tx_power_dbm\":%d,\"samples\":%u,\"points\":%u}",
                (int)median, _calibDistanceM, n,
                (int)_calibTxPower, (unsigned)_calibTarget,
                (unsigned)_calibPointCount);
            mqttPublish("response", String(buf), 1, false);
        }
        LOG_I("ESP-NOW Calib", "distance step done: rssi_median=%d n=%.2f points=%u",
              (int)median, n, (unsigned)_calibPointCount);

    } else {  // MEASURING_AT (v0.4.07 — multi-point flow)
        _enrCalibPushPoint(_calibDistanceM, median, _calibTarget);
        _calibState = EnrCalibState::IDLE;
        if (mqttIsConnected()) {
            char buf[192];
            snprintf(buf, sizeof(buf),
                "{\"calib\":\"measure_at_done\",\"rssi_median\":%d,"
                "\"distance_m\":%.2f,\"samples\":%u,\"points\":%u}",
                (int)median, _calibDistanceM,
                (unsigned)_calibTarget, (unsigned)_calibPointCount);
            mqttPublish("response", String(buf), 1, false);
        }
        LOG_I("ESP-NOW Calib", "measure_at done: %.2f m  rssi_median=%d  points=%u",
              _calibDistanceM, (int)median, (unsigned)_calibPointCount);
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

    if (strcmp(cmd, "measure_1m") == 0 ||
        strcmp(cmd, "measure_d")  == 0 ||
        strcmp(cmd, "measure_at") == 0) {
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

        if (strcmp(cmd, "measure_d") == 0 || strcmp(cmd, "measure_at") == 0) {
            float d = doc["distance_m"] | 0.0f;
            if (d <= 0.0f) {
                LOG_W("ESP-NOW Calib", "%s: distance_m must be > 0", cmd);
                return;
            }
            _calibDistanceM = d;
            _calibState = (strcmp(cmd, "measure_d") == 0)
                          ? EnrCalibState::MEASURING_D
                          : EnrCalibState::MEASURING_AT;
            LOG_I("ESP-NOW Calib", "%s at %.2f m from %s (%u samples)",
                  cmd, d, peerMac, (unsigned)samples);
        } else {
            _calibDistanceM = 1.0f;
            _calibState = EnrCalibState::MEASURING_1M;
            LOG_I("ESP-NOW Calib", "measuring at 1 m from %s (%u samples)",
                  peerMac, (unsigned)samples);
        }
        if (mqttIsConnected()) {
            char pb[160];
            snprintf(pb, sizeof(pb),
                "{\"calib\":\"started\",\"cmd\":\"%s\",\"peer_mac\":\"%s\","
                "\"samples\":%u,\"points\":%u}",
                cmd, peerMac, (unsigned)samples, (unsigned)_calibPointCount);
            mqttPublish("response", String(pb), 1, false);
        }

    } else if (strcmp(cmd, "clear") == 0) {
        // v0.4.07: explicit buffer wipe. Useful when the operator wants to
        // start a fresh multi-point calibration without rebooting.
        _enrCalibClearPoints();
        _calibState   = EnrCalibState::IDLE;
        _calibCount   = 0;
        _calibTxPower = 0;
        LOG_I("ESP-NOW Calib", "multi-point buffer cleared");
        if (mqttIsConnected())
            mqttPublish("response",
                String("{\"calib\":\"cleared\",\"points\":0}"), 1, false);

    } else if (strcmp(cmd, "commit") == 0) {
        // v0.4.07 (#39): if the multi-point buffer has ≥2 entries, compute
        // tx_power and path_loss_n via least-squares linreg over them.
        // Otherwise fall back to operator-supplied JSON values (legacy flow).
        int   txp = 0;
        float pln = 0.0f;
        float r2  = NAN;
        float rmse = NAN;
        bool  used_linreg = false;

        if (_calibPointCount >= 2) {
            float dists[ESPNOW_CALIB_MAX_POINTS];
            float rssis[ESPNOW_CALIB_MAX_POINTS];
            for (uint8_t i = 0; i < _calibPointCount; i++) {
                dists[i] = _calibPoints[i].distance_m;
                rssis[i] = (float)_calibPoints[i].rssi_median;
            }
            CalibFitResult fit = calibLinreg(dists, rssis, _calibPointCount);
            if (!fit.valid) {
                LOG_W("ESP-NOW Calib", "commit: linreg degenerate (n=%u)",
                      (unsigned)_calibPointCount);
                if (mqttIsConnected())
                    mqttPublish("response",
                        String("{\"calib\":\"error\",\"msg\":\"linreg degenerate — distances all equal?\"}"),
                        1, false);
                return;
            }
            txp = (int)lroundf(fit.tx_power_dbm);
            pln = fit.path_loss_n;
            r2  = fit.r_squared;
            rmse = fit.rmse_db;
            used_linreg = true;
            LOG_I("ESP-NOW Calib", "linreg over %u points: txp=%d n=%.2f R²=%.3f RMSE=%.2f dB",
                  (unsigned)_calibPointCount, txp, pln, r2, rmse);
        } else {
            txp = doc["tx_power_dbm"] | (int)ESPNOW_TX_POWER_DBM;
            pln = doc["path_loss_n"]  | ESPNOW_PATH_LOSS_N;
        }

        if (txp > 0 || txp < -120 || pln < 1.0f || pln > 6.0f) {
            LOG_W("ESP-NOW Calib", "commit: values out of range (txp=%d n=%.2f)", txp, pln);
            if (mqttIsConnected()) {
                char pb[160];
                snprintf(pb, sizeof(pb),
                    "{\"calib\":\"error\",\"msg\":\"out of range\","
                    "\"tx_power_dbm\":%d,\"path_loss_n\":%.2f}", txp, pln);
                mqttPublish("response", String(pb), 1, false);
            }
            return;
        }
        // (v0.4.09 / #41.7) Per-peer commit when linreg used.
        // The multi-point flow associates the points with a specific peer
        // MAC (_calibPeerMac, set by the most recent measure_*). For that
        // case, write the result into the per-peer table instead of
        // overwriting the device-global tx_power/n. The legacy single-pair
        // operator-supplied 'commit' path (no points buffered) still writes
        // the global so existing workflows keep working.
        AppConfig copy = gAppConfig;
        bool perPeerCommit = false;
        if (used_linreg && strlen(_calibPeerMac) == 17) {
            // Mutate copy.peer_cal_table.entries[] via the helper (it operates on
            // gAppConfig but we want to stage in a copy so save() is atomic).
            // Quickest path: apply to gAppConfig directly, snapshot into copy.
            peerCalUpsert(_calibPeerMac, (int8_t)txp, (uint8_t)(pln * 10.0f + 0.5f));
            // Re-snapshot copy so AppConfigStore::save() persists the new entry.
            memcpy(copy.peer_cal_table.entries,
                   gAppConfig.peer_cal_table.entries,
                   sizeof(copy.peer_cal_table.entries));
            copy.peer_cal_table.count = gAppConfig.peer_cal_table.count;
            perPeerCommit = true;
        } else {
            // Legacy: write the device-global tx_power / n.
            copy.espnow_tx_power_dbm    = (int8_t)txp;
            copy.espnow_path_loss_n_x10 = (uint8_t)(pln * 10.0f + 0.5f);
        }
        if (AppConfigStore::save(copy)) {
            LOG_I("ESP-NOW Calib", "committed: tx_power=%d n=%.2f scope=%s",
                  txp, pln, perPeerCommit ? "per-peer" : "global");
            if (mqttIsConnected()) {
                char pb[320];
                if (perPeerCommit) {
                    snprintf(pb, sizeof(pb),
                        "{\"calib\":\"committed\",\"tx_power_dbm\":%d,"
                        "\"path_loss_n\":%.2f,\"points\":%u,"
                        "\"r_squared\":%.3f,\"rmse_db\":%.2f,"
                        "\"method\":\"linreg\",\"scope\":\"per_peer\","
                        "\"peer_mac\":\"%s\",\"cal_entries\":%u}",
                        txp, pln, (unsigned)_calibPointCount, r2, rmse,
                        _calibPeerMac, (unsigned)gAppConfig.peer_cal_table.count);
                } else if (used_linreg) {
                    snprintf(pb, sizeof(pb),
                        "{\"calib\":\"committed\",\"tx_power_dbm\":%d,"
                        "\"path_loss_n\":%.2f,\"points\":%u,"
                        "\"r_squared\":%.3f,\"rmse_db\":%.2f,"
                        "\"method\":\"linreg\",\"scope\":\"global\"}",
                        txp, pln, (unsigned)_calibPointCount, r2, rmse);
                } else {
                    snprintf(pb, sizeof(pb),
                        "{\"calib\":\"committed\",\"tx_power_dbm\":%d,"
                        "\"path_loss_n\":%.2f,\"method\":\"manual\","
                        "\"scope\":\"global\"}",
                        txp, pln);
                }
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
        // (v0.4.09 / #41.7) Reset clears the per-peer cal table too.
        memset(copy.peer_cal_table.entries, 0, sizeof(copy.peer_cal_table.entries));
        copy.peer_cal_table.count = 0;
        if (AppConfigStore::save(copy)) {
            LOG_I("ESP-NOW Calib", "reset to compile-time defaults (per-peer table cleared)");
            if (mqttIsConnected())
                mqttPublish("response",
                    String("{\"calib\":\"reset\",\"cal_entries\":0}"), 1, false);
        }
        _calibState = EnrCalibState::IDLE;
        _enrCalibClearPoints();
        _calibTxPower = 0;

    } else if (strcmp(cmd, "forget_peer") == 0) {
        // (v0.4.09 / #41.7) Drop the calibration entry for one peer.
        const char* peerMac = doc["peer_mac"] | "";
        if (strlen(peerMac) != 17) {
            LOG_W("ESP-NOW Calib", "forget_peer: missing/invalid peer_mac");
            return;
        }
        bool removed = peerCalForget(peerMac);
        if (removed) {
            AppConfigStore::save(gAppConfig);   // persist the deletion
            LOG_I("ESP-NOW Calib", "forgot peer %s — %u entries left",
                  peerMac, (unsigned)gAppConfig.peer_cal_table.count);
        } else {
            LOG_I("ESP-NOW Calib", "forget_peer: %s not in table", peerMac);
        }
        if (mqttIsConnected()) {
            char pb[160];
            snprintf(pb, sizeof(pb),
                "{\"calib\":\"forget_peer\",\"peer_mac\":\"%s\","
                "\"removed\":%s,\"cal_entries\":%u}",
                peerMac, removed ? "true" : "false",
                (unsigned)gAppConfig.peer_cal_table.count);
            mqttPublish("response", String(pb), 1, false);
        }

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

    // (v0.4.08) Post-OTA validation window suppression.
    //
    // For the first ESPNOW_POST_OTA_QUIET_MS milliseconds of a boot that's
    // still in the OTA validation window, we DO NOT broadcast beacons or
    // publish to MQTT. This frees radio time and CPU budget for WiFi
    // association + MQTT setup + heartbeat — the steps that
    // otaValidationConfirmHealth() depends on to mark the new image valid.
    //
    // Why: triangle-position devices on power-bank power take 5–8 s to
    // associate with the AP after a cold OTA boot. While that's happening,
    // ESP-NOW beacon TX (every 3 s) eats radio time + the receive callback
    // path enqueues work onto the loopTask, contributing to task watchdog
    // pressure. Charlie's v0.4.07 OTA died exactly here: task_wdt fired
    // before MQTT validation completed, partition rolled back to v0.4.06.
    //
    // Skipping the beacon doesn't affect ranging quality on peers — they
    // continue beaconing on their own; this device just doesn't HEAR the
    // first ~30 s of distance estimates after an OTA. Tradeoff: 30 s of
    // missing data vs. a forced rollback.
    if (otaValidationIsPending() && now < ESPNOW_POST_OTA_QUIET_MS) {
        return;
    }

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

    float globalTxPow = (float)gAppConfig.espnow_tx_power_dbm;
    float globalPathN = gAppConfig.espnow_path_loss_n_x10 / 10.0f;

    JsonDocument doc;
    doc["node_name"]    = gAppConfig.node_name;
    doc["peer_count"]   = _enrPeers.count();
    doc["cal_entries"]  = gAppConfig.peer_cal_table.count;   // (v0.4.09 / #41.7) per-peer cal coverage
    JsonArray arr = doc["peers"].to<JsonArray>();

    _enrPeers.forEach([&arr, globalTxPow, globalPathN](const PeerEntry& p) {
        if (!_enrIsFiltered(p.mac)) return;   // F2: skip if not in track filter
        JsonObject o = arr.add<JsonObject>();
        o["mac"]  = p.mac;
        o["rssi"] = p.rssi;

        // (v0.4.09 / #41.7) Per-peer calibration lookup. If this device has
        // its own calibrated tx_power / path_loss_n for this peer, use those;
        // otherwise fall back to the device-global constants.
        int8_t  txp_int = (int8_t)globalTxPow;
        uint8_t n_x10   = (uint8_t)(globalPathN * 10.0f + 0.5f);
        bool perPeer = peerCalLookup(p.mac, &txp_int, &n_x10);
        float pathN  = n_x10 / 10.0f;
        o["calibrated"] = perPeer;

        // Prefer EMA-smoothed RSSI for distance; fall back to raw on first frame.
        if (p.rssi_ema_x10 != 0) {
            float emaRssi = p.rssi_ema_x10 / 10.0f;
            o["rssi_ema"] = (int)roundf(emaRssi);
            o["dist_m"]   = String(rssiToDistance((int8_t)emaRssi, txp_int, pathN), 1);
        } else {
            // Cold start — recompute distance with the per-peer constants
            // (the cached p.distM was computed with pre-lookup globals).
            o["rssi_ema"] = p.rssi;
            o["dist_m"]   = String(rssiToDistance(p.rssi, txp_int, pathN), 1);
        }
        o["rejects"] = p.rejects;
        LOG_D("ESP-NOW Ranging", "%s  rssi=%d  ema=%d  dist=%s  rej=%u  cal=%s",
              p.mac, p.rssi, p.rssi_ema_x10 / 10,
              o["dist_m"].as<const char*>(), (unsigned)p.rejects,
              perPeer ? "peer" : "global");
    });

    mqttPublishJson("espnow", doc);
}
