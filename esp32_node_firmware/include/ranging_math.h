#pragma once

#include <math.h>
#include <stdint.h>

// =============================================================================
// ranging_math.h  —  Shared RSSI-to-distance conversion + multi-point fit
//
// This is a pure utility header — no Arduino, no MQTT, no FreeRTOS.
// Safe to include from host-side unit tests.
//
// MODEL:  log-distance path loss
//   d = 10 ^ ((txPower - rssi) / (10 × n))
//
// Parameters:
//   rssi      — received signal strength in dBm (negative)
//   txPower   — measured power at 1 m in dBm (typically -59)
//   pathLossN — path-loss exponent
//               2.0f = free space (BLE open area)
//               2.5f = semi-open indoor (ESP-NOW in office/warehouse)
//               3.0f = heavy indoor obstruction
//
// Both BLE (ble.h) and ESP-NOW ranging (espnow_ranging.h) use the same
// formula; this file eliminates that duplication and makes the function
// trivially testable on a host without hardware.
// =============================================================================

// Returns the estimated distance in metres.
// Clamps negative distances (physically impossible) to 0.01 m so callers
// never receive NaN or a negative value.
inline float rssiToDistance(int8_t rssi, int8_t txPower, float pathLossN) {
    float d = powf(10.0f, (float)(txPower - rssi) / (10.0f * pathLossN));
    return (d < 0.01f) ? 0.01f : d;
}


// =============================================================================
// Multi-point calibration least-squares fit (v0.4.07 / SUGGESTED_IMPROVEMENTS #39)
//
// Given N pairs of (distance_m, rssi_median) measurements, fits the
// log-distance model:
//     rssi = tx_power_dbm  -  10 · n · log10(d)
//
// In linear-regression terms: y = a + b·x   where y=rssi, x=log10(d),
//                                           a=tx_power_dbm, b = -10·n.
//
// Outputs the fitted constants plus R² (coefficient of determination) and
// RMSE (root-mean-square residual in dB) so the operator can judge fit
// quality before committing the result to NVS.
//
// VALIDITY:
//   - point_count must be ≥ 2.
//   - At least two distances must differ (otherwise slope is undefined —
//     all measurements at 1 m can't tell us n).
//   - distances must be > 0 (log10 undefined for ≤ 0).
//   Returns valid=false if any of the above are violated.
//
// COMPLEXITY:
//   O(N) — single pass for sums, no matrix operations. Stable for the
//   small N (≤ 6) the calibration buffer holds.
// =============================================================================
struct CalibFitResult {
    float    tx_power_dbm;   // intercept at log10(d)=0 → predicted rssi at 1 m
    float    path_loss_n;    // -slope / 10
    float    r_squared;      // [0..1]; 1.0 = perfect fit, NaN if SS_tot=0
    float    rmse_db;        // root-mean-square residual (dB)
    uint8_t  point_count;    // number of points used (echo of input n)
    bool     valid;          // false if degenerate inputs
};

// Predicted rssi for a given distance under the fitted model.
inline float calibPredictedRssi(float distance_m, float tx_power_dbm, float path_loss_n) {
    if (distance_m <= 0.0f) return tx_power_dbm;
    return tx_power_dbm - 10.0f * path_loss_n * log10f(distance_m);
}

// Residual = (actual_rssi - predicted_rssi) for one point.
inline float calibResidual(float distance_m, float rssi, float tx_power_dbm, float path_loss_n) {
    return rssi - calibPredictedRssi(distance_m, tx_power_dbm, path_loss_n);
}

// Least-squares fit over `n` calibration points.
// distances[] must be > 0 m. rssis[] in dBm (negative numbers OK).
inline CalibFitResult calibLinreg(const float* distances, const float* rssis, uint8_t n) {
    CalibFitResult r = { 0.0f, 0.0f, 0.0f, 0.0f, n, false };
    if (n < 2) return r;

    // Compute means of x = log10(d) and y = rssi
    float sum_x = 0.0f, sum_y = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        if (distances[i] <= 0.0f) return r;   // invalid input
        sum_x += log10f(distances[i]);
        sum_y += rssis[i];
    }
    float mean_x = sum_x / (float)n;
    float mean_y = sum_y / (float)n;

    // Compute slope and intercept
    float ss_xx = 0.0f;   // Σ(x - mean_x)²
    float ss_xy = 0.0f;   // Σ(x - mean_x)(y - mean_y)
    for (uint8_t i = 0; i < n; i++) {
        float dx = log10f(distances[i]) - mean_x;
        float dy = rssis[i]              - mean_y;
        ss_xx += dx * dx;
        ss_xy += dx * dy;
    }
    if (ss_xx <= 0.0f) return r;   // all distances equal — slope undefined

    float slope     = ss_xy / ss_xx;
    float intercept = mean_y - slope * mean_x;

    r.tx_power_dbm = intercept;
    r.path_loss_n  = -slope / 10.0f;

    // Compute R² and RMSE
    float ss_res = 0.0f;   // Σ(residual²)
    float ss_tot = 0.0f;   // Σ((y - mean_y)²)
    for (uint8_t i = 0; i < n; i++) {
        float pred = intercept + slope * log10f(distances[i]);
        float res  = rssis[i] - pred;
        ss_res += res * res;
        float dy = rssis[i] - mean_y;
        ss_tot += dy * dy;
    }
    r.r_squared = (ss_tot > 0.0f) ? (1.0f - ss_res / ss_tot) : NAN;
    r.rmse_db   = sqrtf(ss_res / (float)n);
    r.valid     = true;
    return r;
}
