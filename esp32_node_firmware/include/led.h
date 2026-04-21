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
//   AP_MODE        — 100 ms / 100 ms rapid blink — config portal active
//   WIFI_CONNECTING — 500 ms / 500 ms slow blink — waiting for Wi-Fi / MQTT
//   WIFI_CONNECTED  — 2000 ms / 2000 ms very slow blink — Wi-Fi up, no MQTT yet
//   MQTT_CONNECTED  — 50 ms pulse / 2000 ms off — normal operational heartbeat
//   BOOT            — solid ON — early initialisation
//   ESPNOW_FLASH    — 40 ms overlay then revert — ESP-NOW packet sent/received
//   OFF             — always off
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
            on = (tick % 100) < 50;     // 500 ms ON / 500 ms OFF  (100-tick cycle)
            break;

        case LedPattern::WIFI_CONNECTED:
            on = (tick % 400) < 200;    // 2000 ms ON / 2000 ms OFF (400-tick cycle)
            break;

        case LedPattern::AP_MODE:
            on = (tick % 20) < 10;      // 100 ms ON / 100 ms OFF  (20-tick cycle)
            break;

        case LedPattern::MQTT_CONNECTED:
            on = (tick % 205) < 5;      // 50 ms ON / 2000 ms OFF  (205-tick cycle)
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
    if (p == LedPattern::ESPNOW_FLASH && _ledPat != LedPattern::ESPNOW_FLASH) {
        _ledPrevPat = _ledPat;    // save current pattern for restore
        _ledFlashAt = _ledTick;   // record when the flash started
    }
    _ledPat = p;
}
