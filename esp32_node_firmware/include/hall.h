#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "logging.h"

// =============================================================================
// hall.h  —  BMT 49E Hall sensor (Honeywell SS49E + LM393) periodic telemetry
//
// Periodic ADC read → mGauss conversion → MQTT telemetry (#71/v0.5.0).
// Edge-detection on a configurable threshold publishes a separate event
// so Node-RED can react without polling.
//
// WIRING (per docs/PLAN_RELAY_HALL_v0.5.0.md):
//   VIN/VCC  →  ESP32 5V (SS49E spec is 4.5–10.5 V; 3.3 V is OUT OF SPEC)
//   GND      →  common ground with ESP32
//   AO       →  voltage divider (2 × 10 kΩ) → HALL_AO_PIN (default GPIO32, ADC1_CH4)
//   D0       →  HALL_DO_PIN (default GPIO33) — optional digital threshold
//
// CONVERSION:
//   At zero field the SS49E sits at VCC/2 = 2.5 V. After the 2× divider
//   the ADC sees 1.25 V. Sensitivity 1.4 mV/Gauss at the sensor. We
//   subtract a per-unit offset (set via cmd/hall/zero — captures the
//   current reading as the new zero-field point) and convert to Gauss.
//
//   gauss = (v_at_sensor - 2.5 V - offset_mv/1000) / 0.0014
//   v_at_sensor = v_at_adc * HALL_DIVIDER_RATIO
//
// MQTT topics (handled in mqtt_client.h):
//   Subscribe:  .../cmd/hall/config   retained JSON
//                                     {"interval_ms":1000,"threshold_gauss":50}
//               .../cmd/hall/zero     one-shot {} — sets offset_mv to current
//   Publish:    .../telemetry/hall    {"voltage_v":1.25,"gauss":-12,
//                                      "above_threshold":false}
//               .../telemetry/hall/edge on threshold cross
//                                     {"edge":"rising","gauss":62,
//                                      "threshold_gauss":50}
//   Heartbeat:  hall_enabled:true
//
// NVS namespace: HALL_NVS_NAMESPACE (default "esp32hall"), keys
//   offset_mv (int16), interval_ms (uint32), thresh_g (int16).
// =============================================================================

#ifdef HALL_ENABLED

namespace HallDriver {

    static int16_t  _offsetMv          = 0;
    static uint32_t _intervalMs        = HALL_INTERVAL_MS;
    static int16_t  _thresholdGauss    = HALL_THRESHOLD_GAUSS;
    static bool     _lastAboveThresh   = false;
    static uint32_t _nextSampleMs      = 0;

    // Forward-declare publishers (defined in mqtt_client.h).
    void hallPublishTelemetry(float voltage_v, int16_t gauss, bool aboveThresh);
    void hallPublishEdge(const char* edge, int16_t gauss, int16_t threshold_g);

    inline void _loadConfig() {
        Preferences p;
        if (p.begin(HALL_NVS_NAMESPACE, /*readOnly=*/true)) {
            _offsetMv       = (int16_t)p.getShort("offset_mv",  0);
            _intervalMs     = p.getUInt("interval_ms", HALL_INTERVAL_MS);
            _thresholdGauss = (int16_t)p.getShort("thresh_g",   HALL_THRESHOLD_GAUSS);
            p.end();
        }
        if (_intervalMs < 100) _intervalMs = 100;   // sanity floor
    }

    inline void _saveConfig() {
        Preferences p;
        if (p.begin(HALL_NVS_NAMESPACE, /*readOnly=*/false)) {
            p.putShort("offset_mv",  _offsetMv);
            p.putUInt ("interval_ms", _intervalMs);
            p.putShort("thresh_g",    _thresholdGauss);
            p.end();
        }
    }

    inline void init() {
        // ADC1 single-shot, 11 dB attenuation = ~0–3.3 V full scale.
        analogReadResolution(12);
        analogSetPinAttenuation(HALL_AO_PIN, ADC_11db);
        pinMode(HALL_AO_PIN, INPUT);
#ifdef HALL_DO_PIN
        pinMode(HALL_DO_PIN, INPUT);
#endif
        _loadConfig();
        _nextSampleMs = millis() + _intervalMs;
        LOG_I("Hall", "init pin=%u interval=%ums threshold=%dG offset=%dmV",
              (unsigned)HALL_AO_PIN, (unsigned)_intervalMs,
              (int)_thresholdGauss, (int)_offsetMv);
    }

    // Sample once. Returns Gauss after offset correction; sets out_voltage_v.
    inline int16_t _sample(float& out_voltage_v) {
        // Average 8 ADC reads to smooth out noise.
        uint32_t acc = 0;
        for (uint8_t i = 0; i < 8; i++) acc += analogRead(HALL_AO_PIN);
        uint32_t adc = acc / 8;
        // 11 dB attenuation calibrated to ~3.3 V full-scale on 12-bit ADC.
        float v_adc = (float)adc * 3.3f / 4095.0f;
        float v_sensor = v_adc * HALL_DIVIDER_RATIO;
        float v_zero  = (HALL_SENSOR_VCC_V / 2.0f) + ((float)_offsetMv / 1000.0f);
        float gauss_f = (v_sensor - v_zero) / (HALL_SENSITIVITY_MV_PER_GAUSS / 1000.0f);
        out_voltage_v = v_sensor;
        if (gauss_f >  32767.0f) gauss_f =  32767.0f;
        if (gauss_f < -32768.0f) gauss_f = -32768.0f;
        return (int16_t)gauss_f;
    }

    // Capture current reading as the new zero-field offset.
    inline void zeroNow() {
        float v;
        // First read to learn the current sensor voltage relative to VCC/2.
        (void)_sample(v);
        // offset_mv = (v_sensor - 2.5) * 1000  → so a future _sample()
        // computes (v_sensor - 2.5 - offset) ≈ 0.
        float drift = v - (HALL_SENSOR_VCC_V / 2.0f);
        _offsetMv = (int16_t)(drift * 1000.0f);
        _saveConfig();
        LOG_I("Hall", "zeroed: offset_mv=%d (v_sensor=%.3f)", (int)_offsetMv, v);
    }

    inline void setConfig(uint32_t interval_ms, int16_t thresh_g) {
        bool changed = false;
        if (interval_ms >= 100 && interval_ms != _intervalMs) {
            _intervalMs = interval_ms;
            changed = true;
        }
        if (thresh_g != _thresholdGauss) {
            _thresholdGauss = thresh_g;
            changed = true;
        }
        if (changed) {
            _saveConfig();
            LOG_I("Hall", "config: interval=%ums threshold=%dG",
                  (unsigned)_intervalMs, (int)_thresholdGauss);
        }
    }

    // Periodic-task entry point. Called from main loop. Returns immediately
    // unless the interval has elapsed.
    inline void loop() {
        uint32_t now = millis();
        if ((int32_t)(now - _nextSampleMs) < 0) return;
        _nextSampleMs = now + _intervalMs;

        float v;
        int16_t g = _sample(v);
        bool above = (g >= _thresholdGauss);
        hallPublishTelemetry(v, g, above);

        // Edge detection — publish a one-shot event on transition.
        if (above != _lastAboveThresh) {
            hallPublishEdge(above ? "rising" : "falling", g, _thresholdGauss);
            _lastAboveThresh = above;
        }
    }

}  // namespace HallDriver

inline void hallInit() { HallDriver::init(); }
inline void hallLoop() { HallDriver::loop(); }
inline void hallZeroNow() { HallDriver::zeroNow(); }
inline void hallSetConfig(uint32_t interval_ms, int16_t thresh_g) {
    HallDriver::setConfig(interval_ms, thresh_g);
}

#endif  // HALL_ENABLED
