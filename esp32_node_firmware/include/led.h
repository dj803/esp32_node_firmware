#pragma once

#include <Arduino.h>
#include "esp_timer.h"
#include "config.h"

// =============================================================================
// led.h — Non-blocking status LED blink patterns
//
// Driven by a 10 ms esp_timer callback (fires from a FreeRTOS task — no ISR
// constraints). Call ledInit() once from setup(), then ledSetPattern() whenever
// the device state changes. No polling or delay() needed.
//
// PATTERNS (most → least alarming):
//   ERROR          — 3 × 100 ms flash, 700 ms pause — unrecoverable failure
//   OTA_UPDATE     — solid ON — firmware being written to flash
//   AP_MODE        — double-blink (50/50/50/850 ms) — config portal active
//   WIFI_CONNECTING — 200 ms / 200 ms rapid blink — alarm: can't reach WiFi
//   WIFI_CONNECTED  — 500 ms / 500 ms moderate blink — WiFi up, no MQTT yet
//   MQTT_CONNECTED  — mostly-on with 100 ms heartbeat-blip — operational steady
//   BOOT            — solid ON — early initialisation
//   ESPNOW_FLASH    — 40 ms overlay then revert — ESP-NOW packet sent/received
//   LOCATE          — 10 × 200 ms ON/OFF (4 s) then revert — physical locate flash
//   OFF             — always off
//
// (#99, v0.4.31) Patterns retuned for visual distinctiveness from across the
// bench. Original timings were time-correct but too similar visually — the
// 50 ms MQTT_CONNECTED pulse looked like the 500 ms WIFI_CONNECTING blink
// from a few metres away. The 2026-04-29 PM router-power-failure incident
// found 4/6 devices stuck silent for 16+ min (#98) but the operator
// couldn't tell them apart from the 2 healthy ones because all six LEDs
// "looked like blinking." New mapping:
//   - WIFI_CONNECTING 5Hz alarm vs MQTT_CONNECTED mostly-on heartbeat —
//     unmistakably different: "blinking fast" vs "steady glow"
//   - AP_MODE double-blink-pause: distinct from any other pattern
//   - WIFI_CONNECTED 1Hz: intermediate "WiFi up but waiting on MQTT"
// =============================================================================

enum class LedPattern : uint8_t {
    OFF,              // LED always off
    BOOT,             // Solid ON — device is initialising
    WIFI_CONNECTING,  // 500 ms ON / 500 ms OFF — waiting for Wi-Fi or MQTT reconnect
    WIFI_CONNECTED,   // 2000 ms ON / 2000 ms OFF — Wi-Fi up, no MQTT broker yet
    AP_MODE,          // 100 ms ON / 100 ms OFF — hosting config portal
    MQTT_CONNECTED,   // 50 ms ON / 2000 ms OFF — normal operational heartbeat
    OTA_UPDATE,       // Solid ON — firmware flash in progress (device reboots after)
    ERROR,            // 3 × 100 ms flash, 700 ms pause — unrecoverable / restarting
    ESPNOW_FLASH,     // 40 ms overlay flash then revert to previous pattern
    LOCATE,           // 10 × 200 ms ON/OFF (4 s) then auto-revert — physical-locate flash
};

static volatile LedPattern _ledPat     = LedPattern::OFF;
static volatile LedPattern _ledPrevPat = LedPattern::OFF;  // restored after ESPNOW_FLASH
static volatile uint32_t   _ledTick    = 0;                // increments every 10 ms
static volatile uint32_t   _ledFlashAt = 0;                // tick when ESPNOW_FLASH started

static esp_timer_handle_t  _ledTimer   = nullptr;

// ── Timer callback (fires every 10 ms from FreeRTOS timer task) ───────────────
static void _ledTimerCb(void*) {
    uint32_t tick = ++_ledTick;
    LedPattern pat = _ledPat;
    bool on = false;

    switch (pat) {
        case LedPattern::BOOT:
        case LedPattern::OTA_UPDATE:
            on = true;
            break;

        case LedPattern::WIFI_CONNECTING:
            // (#99, v0.4.31) 200 ms ON / 200 ms OFF — 2.5 Hz rapid alarm.
            // Visually: "fast blinking, something's wrong"
            on = (tick % 40) < 20;
            break;

        case LedPattern::WIFI_CONNECTED:
            // (#99, v0.4.31) 500 ms ON / 500 ms OFF — 1 Hz moderate blink.
            // Intermediate state: WiFi up, waiting on MQTT.
            on = (tick % 100) < 50;
            break;

        case LedPattern::AP_MODE:
            // (#99, v0.4.31) double-blink-pause: ON 50 ms / OFF 50 ms /
            // ON 50 ms / OFF 850 ms. 1000 ms cycle. Visually unmistakable
            // — two quick pulses then a long dark gap. Says "operator,
            // come configure me" without being confused for any other state.
            {
                uint32_t t = tick % 100;   // 1000 ms cycle
                on = (t < 5) || (t >= 10 && t < 15);
            }
            break;

        case LedPattern::MQTT_CONNECTED:
            // (#99, v0.4.31) 1900 ms ON / 100 ms OFF — mostly-on with a
            // brief heartbeat-blip every 2 seconds. Visually: "steady
            // glow with a pulse" — clearly distinct from any blink
            // pattern. Says "I'm healthy and processing".
            on = (tick % 200) >= 10;     // off for first 100ms of each 2s cycle
            break;

        case LedPattern::ERROR:
            {   // 3 × 100 ms ON, 700 ms OFF = 1200 ms total (120-tick cycle)
                uint32_t t = tick % 120;
                on = (t < 10) || (t >= 20 && t < 30) || (t >= 40 && t < 50);
            }
            break;

        case LedPattern::ESPNOW_FLASH:
            if (tick - _ledFlashAt < 4) {   // 40 ms overlay
                on = true;
            } else {
                _ledPat = _ledPrevPat;   // revert to whatever was showing before the flash
                on = false;
            }
            break;

        case LedPattern::LOCATE:
            {   // 10 × 200 ms ON / 200 ms OFF = 4000 ms total (400 ticks),
                // then auto-revert to whatever pattern was running before.
                uint32_t dt = tick - _ledFlashAt;
                if (dt < 400) {
                    on = (dt % 40) < 20;     // 200 ms ON / 200 ms OFF
                } else {
                    _ledPat = _ledPrevPat;
                    on = false;
                }
            }
            break;

        case LedPattern::OFF:
        default:
            on = false;
            break;
    }

    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// ── ledInit ───────────────────────────────────────────────────────────────────
// Initialise the LED pin and start the 10 ms timer. Call once from setup().
inline void ledInit() {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    esp_timer_create_args_t args;
    memset(&args, 0, sizeof(args));
    args.callback        = _ledTimerCb;
    args.dispatch_method = ESP_TIMER_TASK;   // fires from a FreeRTOS task — safe to call digitalWrite
    args.name            = "led";

    esp_timer_create(&args, &_ledTimer);
    esp_timer_start_periodic(_ledTimer, 10000);   // 10 ms = 10,000 µs
}

// ── ledSetPattern ─────────────────────────────────────────────────────────────
// Switch to a new blink pattern. Safe to call from any FreeRTOS task.
//
// ESPNOW_FLASH is a one-shot 40 ms overlay — the LED flashes briefly, then the
// previous pattern resumes automatically. Do not call it if the current pattern
// is already ESPNOW_FLASH (the second flash is queued implicitly by the timer).
inline void ledSetPattern(LedPattern p) {
    // Auto-revert patterns (ESPNOW_FLASH, LOCATE) save the current pattern so
    // the timer callback can restore it when the overlay expires.
    if ((p == LedPattern::ESPNOW_FLASH || p == LedPattern::LOCATE) && _ledPat != p) {
        _ledPrevPat = _ledPat;    // save current pattern for restore
        _ledFlashAt = _ledTick;   // record when the overlay started
    }
    _ledPat = p;
}
