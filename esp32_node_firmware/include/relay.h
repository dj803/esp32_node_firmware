#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "logging.h"
// Visual relay-toggle feedback is deferred — plan called for a single
// white flash via ws2812PostEvent, but the existing LedEventType enum
// has no white-flash event yet. Add when LedEventType gains a generic
// PULSE / WHITE_FLASH variant; for now log + MQTT status are the
// operator-visible signals.

// =============================================================================
// relay.h  —  BDD 2CH 5V relay (JQC-3FF-S-Z) driver (#71/v0.5.0)
//
// Active-LOW signal lines (GPIO LOW = relay energized). Two channels with
// state persisted in NVS so a reboot restores the last commanded position.
//
// WIRING (per docs/PLAN_RELAY_HALL_v0.5.0.md):
//   VCC      →  ESP32 5V (signal-side opto supply)
//   GND      →  common ground with ESP32
//   IN1      →  RELAY_CH1_PIN (default GPIO25)
//   IN2      →  RELAY_CH2_PIN (default GPIO26)
//   JD-VCC   →  SEPARATE 5V supply (~150 mA inrush; ESP32 5V will brown out)
//   COM/NO/NC contacts → 10A 250VAC / 30VDC, onboard flyback diodes.
//
// SAFETY DETAIL (boot-click prevention):
//   pinMode(pin, OUTPUT) defaults the line LOW, which would energize the
//   relay on every boot. relayInit() therefore digitalWrite(HIGH) BEFORE
//   pinMode(OUTPUT) so the line is already deasserted when the driver
//   transitions out of input mode.
//
// MQTT topics (handled in mqtt_client.h):
//   Subscribe:  .../cmd/relay      JSON {"ch":1,"state":true}
//                                       {"ch":"all","state":false}
//   Publish:    .../status/relay   retained JSON {"ch1":bool,"ch2":bool}
//   Heartbeat:  relay_enabled:true + relay_state:[bool,bool]
//
// NVS namespace: RELAY_NVS_NAMESPACE (default "esp32relay"), keys ch1/ch2 (uint8_t).
// =============================================================================

#ifdef RELAY_ENABLED

namespace RelayDriver {

    static constexpr uint8_t  CHANNELS = 2;
    static const uint8_t      PINS[CHANNELS] = { RELAY_CH1_PIN, RELAY_CH2_PIN };
    static const char* const  NVS_KEYS[CHANNELS] = { "ch1", "ch2" };

#if RELAY_ACTIVE_LOW
    static constexpr uint8_t LEVEL_ON  = LOW;
    static constexpr uint8_t LEVEL_OFF = HIGH;
#else
    static constexpr uint8_t LEVEL_ON  = HIGH;
    static constexpr uint8_t LEVEL_OFF = LOW;
#endif

    // Authoritative state, mutated by relaySet from any context (MQTT
    // async_tcp task or main loop). Reads are atomic on Xtensa for bool.
    static volatile bool _state[CHANNELS] = { false, false };

    // Forward declare the publisher (defined in mqtt_client.h after include).
    // Mirrors the ws2812PublishState shape.
    void relayPublishState();

    // Configure pins safe-side: drive deasserted level FIRST, then OUTPUT.
    // Prevents the boot-click that would otherwise fire on every reset.
    inline void _safeOutputInit(uint8_t pin, bool initialOn) {
        digitalWrite(pin, initialOn ? LEVEL_ON : LEVEL_OFF);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, initialOn ? LEVEL_ON : LEVEL_OFF);
    }

    // One-shot init at boot: load NVS state, drive pins, log.
    inline void init() {
        Preferences p;
        bool loaded[CHANNELS] = { false, false };
        if (p.begin(RELAY_NVS_NAMESPACE, /*readOnly=*/true)) {
            for (uint8_t i = 0; i < CHANNELS; i++) {
                loaded[i] = (p.getUChar(NVS_KEYS[i], 0) != 0);
            }
            p.end();
        }
        for (uint8_t i = 0; i < CHANNELS; i++) {
            _state[i] = loaded[i];
            _safeOutputInit(PINS[i], loaded[i]);
        }
        LOG_I("Relay", "init pins ch1=%u ch2=%u state=[%s,%s] active_low=%d",
              PINS[0], PINS[1],
              loaded[0] ? "ON" : "off", loaded[1] ? "ON" : "off",
              (int)RELAY_ACTIVE_LOW);
    }

    // Get current state.
    inline bool get(uint8_t ch) {
        if (ch < 1 || ch > CHANNELS) return false;
        return _state[ch - 1];
    }

    // Set channel state, persist, drive pin, single-flash LED. Returns true
    // if the call changed state (false if no-op or invalid channel).
    inline bool set(uint8_t ch, bool on) {
        if (ch < 1 || ch > CHANNELS) {
            LOG_W("Relay", "set: invalid channel %u (1..%u)", ch, CHANNELS);
            return false;
        }
        uint8_t idx = ch - 1;
        if (_state[idx] == on) return false;     // no-op
        _state[idx] = on;
        digitalWrite(PINS[idx], on ? LEVEL_ON : LEVEL_OFF);
        Preferences p;
        if (p.begin(RELAY_NVS_NAMESPACE, /*readOnly=*/false)) {
            p.putUChar(NVS_KEYS[idx], on ? 1 : 0);
            p.end();
        }
        LOG_I("Relay", "ch%u → %s (pin %u)", ch, on ? "ON" : "off", PINS[idx]);
        return true;
    }

    // Apply to all channels at once. Used by cmd/relay {"ch":"all"}.
    inline void setAll(bool on) {
        for (uint8_t ch = 1; ch <= CHANNELS; ch++) (void)set(ch, on);
    }

}  // namespace RelayDriver

// Public API — keep flat names so mqtt_client.h's handler block
// reads cleanly without "RelayDriver::" prefixes.
inline void relayInit() { RelayDriver::init(); }
inline bool relayGet(uint8_t ch) { return RelayDriver::get(ch); }
inline bool relaySet(uint8_t ch, bool on) { return RelayDriver::set(ch, on); }
inline void relaySetAll(bool on) { RelayDriver::setAll(on); }

#endif  // RELAY_ENABLED
