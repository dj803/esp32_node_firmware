#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include "config.h"

// =============================================================================
// ws2812.h  —  WS2812B addressable LED strip control
//
// Drives a WS2812B strip via FastLED on a dedicated FreeRTOS task pinned to
// Core 1. The strip runs independently of RFID (Core 1 loop) and MQTT
// (AsyncMqttClient, Core 0) via a thread-safe event queue.
//
// INCLUDE ORDER: Must come before mqtt_client.h and rfid.h in esp32_firmware.ino.
//   mqtt_client.h forward-declares ws2812PublishState() and ws2812PostEvent().
//   rfid.h calls ws2812PostEvent() and ledFlashLocate() — both defined here.
//
// WIRING:
//   WS2812B DIN  →  GPIO LED_STRIP_PIN (default GPIO27) via 330Ω resistor
//   WS2812B 5V   →  external 5V supply (not ESP32 3.3V)
//   WS2812B GND  →  common ground with ESP32
//   Bulk capacitor 1000µF across 5V/GND at first LED
//
// STATE MACHINE:
//   BOOT_INDICATOR  Boot phase animations (bootstrap/wifi/ap_mode)
//   IDLE            Slow blue breathing — default operational state
//   RFID_OK         Solid green for LED_RFID_OVERRIDE_MS, then → _previousState
//   RFID_FAIL       Solid red  for LED_RFID_OVERRIDE_MS, then → _previousState
//   MQTT_OVERRIDE   MQTT-commanded color or named animation
//   OTA             Orange chasing during OTA download
//   OFF             All LEDs off (RFID still temporarily overrides)
//
// MQTT TOPICS:
//   Subscribe: .../cmd/led             (handled in mqtt_client.h)
//   Publish:   .../status/led          (retained, QoS 1)
//
// PUBLIC API (called from .ino, mqtt_client.h, rfid.h, ota.h):
//   ws2812Init()           — FastLED setup, queue create, deterministic off at boot
//   ws2812TaskStart()      — spawn Core 1 task
//   ws2812PostEvent(e)     — thread-safe queue post (non-blocking, drops if full)
//   ws2812PublishState()   — publish current state to .../status/led (retained)
//   ledFlashLocate()       — called by rfid.h; posts RFID_OK (legacy hook)
// =============================================================================


// ── Event types and struct ────────────────────────────────────────────────────

enum class LedEventType : uint8_t {
    RFID_OK,         // Authorised card → solid green for LED_RFID_OVERRIDE_MS
    RFID_FAIL,       // Unauthorised card → solid red for LED_RFID_OVERRIDE_MS
    MQTT_COLOR,      // Set solid color {r, g, b}
    MQTT_BRIGHTNESS, // Set brightness {brightness}
    MQTT_ANIMATION,  // Set named animation {animName}
    MQTT_COUNT,      // Set active LED count {count}
    MQTT_OFF,        // All LEDs off
    RESET,           // Return to IDLE (blue breathing)
    BOOT_STATE,      // Boot phase indicator; animName = "bootstrap"|"wifi"|"ap_mode"
    OTA_START,       // OTA download beginning → orange chasing
    OTA_DONE,        // OTA failed or no update → restore _previousState
};

struct LedEvent {
    LedEventType type;
    uint8_t      r, g, b;        // for MQTT_COLOR
    uint8_t      brightness;     // for MQTT_BRIGHTNESS (0–255)
    uint8_t      count;          // for MQTT_COUNT
    char         animName[16];   // for MQTT_ANIMATION / BOOT_STATE
};


// ── State definitions ─────────────────────────────────────────────────────────

enum class LedState : uint8_t {
    BOOT_INDICATOR,
    IDLE,
    RFID_OK,
    RFID_FAIL,
    MQTT_OVERRIDE,
    OTA,
    OFF,
};


// ── Module-level state (all static — single translation unit) ─────────────────
//
// CONCURRENCY AUDIT:
//   _ws2812Task (Core 1) writes:  _ledState, _ledPreviousState, _ledRfidEndMs,
//                                  _ledMqttColor, _ledMqttAnimName,
//                                  _ledActiveBrightness, _ledActiveCount,
//                                  _ledStateR/G/B
//   Main context (Core 0) reads:  _ledActiveBrightness, _ledActiveCount,
//                                  _ledStateR/G/B  via ws2812PublishState()
//
//   Access is mediated by the FreeRTOS LED event queue: events are enqueued
//   from Core 0 and dequeued + applied in Core 1's task. State variables that
//   are only read from Core 0 after a queue flush are safe.
//
//   _ledActiveBrightness, _ledActiveCount, _ledStateR/G/B: these are read
//   from Core 0 in ws2812PublishState(). Marking them volatile prevents the
//   compiler from caching stale Core 1 writes in a Core 0 register.
//   For single-byte values on Xtensa LX6 this is sufficient; for coordinated
//   multi-field reads (R+G+B as a colour snapshot) a portENTER_CRITICAL guard
//   around ws2812PublishState() would be stronger but is omitted here for
//   simplicity — the worst case is a one-tick-old colour in an MQTT publish.

static QueueHandle_t _ledEventQueue    = nullptr;
static CRGB          _leds[LED_MAX_NUM_LEDS];        // pre-allocated, never heap
static LedState      _ledState         = LedState::BOOT_INDICATOR;
static LedState      _ledPreviousState = LedState::IDLE;  // restored after RFID/OTA

// RFID timed-override
static uint32_t      _ledRfidEndMs     = 0;          // millis() when RFID anim ends

// Saved MQTT_OVERRIDE parameters (restored after RFID/OTA)
static CRGB          _ledMqttColor     = CRGB::Black;
static char          _ledMqttAnimName[16] = "solid";

// Runtime-adjustable strip parameters (persisted to NVS)
// volatile: written in _ws2812Task (Core 1), read in ws2812PublishState() (Core 0)
static volatile uint8_t _ledActiveBrightness = LED_MAX_BRIGHTNESS;
static volatile uint8_t _ledActiveCount      = LED_DEFAULT_COUNT;

// Current state info for ws2812PublishState()
// volatile: written in _ws2812Task (Core 1), read in ws2812PublishState() (Core 0)
static volatile uint8_t _ledStateR = 0, _ledStateG = 0, _ledStateB = 0;


// ── Forward declarations ──────────────────────────────────────────────────────
// mqttPublishLedState is defined in mqtt_client.h which is included after this
// file. Use a forward declaration so ws2812PublishState() can call it.
static void mqttPublishLedState(const char* state,
                                uint8_t r, uint8_t g, uint8_t b,
                                uint8_t brightness, uint8_t count);


// ── NVS helpers ───────────────────────────────────────────────────────────────

static void _ledNvsSave() {
    Preferences p;
    p.begin(LED_STRIP_NVS_NAMESPACE, false);
    p.putUChar("brightness", _ledActiveBrightness);
    p.putUChar("count",      _ledActiveCount);
    p.end();
}

static void _ledNvsLoad() {
    Preferences p;
    p.begin(LED_STRIP_NVS_NAMESPACE, true);
    _ledActiveBrightness = p.getUChar("brightness", LED_MAX_BRIGHTNESS);
    _ledActiveCount      = p.getUChar("count",      LED_DEFAULT_COUNT);
    p.end();
    // Clamp to valid ranges
    if (_ledActiveBrightness == 0 || _ledActiveBrightness > LED_MAX_BRIGHTNESS)
        _ledActiveBrightness = LED_MAX_BRIGHTNESS;
    if (_ledActiveCount == 0 || _ledActiveCount > LED_MAX_NUM_LEDS)
        _ledActiveCount = LED_DEFAULT_COUNT;
}


// ── Animation helpers ─────────────────────────────────────────────────────────

// Millis-based sine breathing — no blocking, safe to call every frame.
// period_ms: full breathe cycle length. color: peak color.
static CRGB _breathe(uint32_t period_ms, CRGB color) {
    uint32_t t   = millis() % period_ms;
    // Map 0…period_ms → 0…255 sine wave (0=dark, 255=full bright)
    uint8_t  val = (uint8_t)(128 + 127 * sinf((float)t / period_ms * 2.0f * PI));
    return CRGB(
        scale8(color.r, val),
        scale8(color.g, val),
        scale8(color.b, val)
    );
}

// Simple chasing animation: 3 lit LEDs advancing across the active count.
static void _chase(CRGB color) {
    uint8_t  pos = (millis() / 80) % _ledActiveCount;
    fill_solid(_leds, _ledActiveCount, CRGB::Black);
    for (uint8_t i = 0; i < 3; i++) {
        _leds[(pos + i) % _ledActiveCount] = color;
    }
}


// ── Frame renderer ────────────────────────────────────────────────────────────

static void _ws2812RenderFrame() {
    // Ensure LEDs beyond active count are always off
    if (_ledActiveCount < LED_MAX_NUM_LEDS)
        fill_solid(_leds + _ledActiveCount,
                   LED_MAX_NUM_LEDS - _ledActiveCount, CRGB::Black);

    switch (_ledState) {

        case LedState::BOOT_INDICATOR: {
            // Boot phase animations driven by animName in transition handler.
            // The animName is stored in _ledMqttAnimName when BOOT_STATE is received.
            if (strcmp(_ledMqttAnimName, "bootstrap") == 0) {
                // Slow yellow breathing — searching for sibling credentials
                fill_solid(_leds, _ledActiveCount, _breathe(3000, CRGB(255, 180, 0)));
            } else if (strcmp(_ledMqttAnimName, "wifi") == 0) {
                // Fast blue pulse — associating with router
                bool on = (millis() % 500) < 250;
                fill_solid(_leds, _ledActiveCount, on ? CRGB(0, 0, 255) : CRGB::Black);
            } else if (strcmp(_ledMqttAnimName, "ap_mode") == 0) {
                // Slow red blink — config portal active
                bool on = (millis() % 2000) < 1000;
                fill_solid(_leds, _ledActiveCount, on ? CRGB(255, 0, 0) : CRGB::Black);
            } else {
                // Generic boot — dim white
                fill_solid(_leds, _ledActiveCount, CRGB(30, 30, 30));
            }
            break;
        }

        case LedState::IDLE:
            // Slow 4-second blue breathing
            fill_solid(_leds, _ledActiveCount, _breathe(4000, CRGB(0, 0, 255)));
            break;

        case LedState::RFID_OK:
            fill_solid(_leds, _ledActiveCount, CRGB::Green);
            if (millis() >= _ledRfidEndMs) {
                _ledState = _ledPreviousState;
            }
            break;

        case LedState::RFID_FAIL:
            fill_solid(_leds, _ledActiveCount, CRGB::Red);
            if (millis() >= _ledRfidEndMs) {
                _ledState = _ledPreviousState;
            }
            break;

        case LedState::MQTT_OVERRIDE: {
            if (strcmp(_ledMqttAnimName, "breathing") == 0) {
                fill_solid(_leds, _ledActiveCount,
                           _breathe(4000, _ledMqttColor));
            } else if (strcmp(_ledMqttAnimName, "rainbow") == 0) {
                uint8_t hue = (millis() / 10) & 0xFF;
                fill_rainbow(_leds, _ledActiveCount, hue, 255 / _ledActiveCount);
            } else {
                // Default: "solid" or any unrecognised name
                fill_solid(_leds, _ledActiveCount, _ledMqttColor);
            }
            break;
        }

        case LedState::OTA:
            _chase(CRGB(255, 80, 0));   // Orange chasing
            break;

        case LedState::OFF:
        default:
            fill_solid(_leds, _ledActiveCount, CRGB::Black);
            break;
    }

    FastLED.show();
}


// ── Event handler ─────────────────────────────────────────────────────────────

static void _ws2812HandleEvent(const LedEvent& evt) {
    switch (evt.type) {

        case LedEventType::RFID_OK:
        case LedEventType::RFID_FAIL:
            // Save current state so we can restore it after the animation
            if (_ledState != LedState::RFID_OK && _ledState != LedState::RFID_FAIL)
                _ledPreviousState = _ledState;
            _ledState     = (evt.type == LedEventType::RFID_OK)
                            ? LedState::RFID_OK : LedState::RFID_FAIL;
            _ledRfidEndMs = millis() + LED_RFID_OVERRIDE_MS;
            break;

        case LedEventType::MQTT_COLOR:
            _ledPreviousState = _ledState;
            _ledMqttColor     = CRGB(evt.r, evt.g, evt.b);
            strlcpy(_ledMqttAnimName, "solid", sizeof(_ledMqttAnimName));
            _ledState         = LedState::MQTT_OVERRIDE;
            _ledStateR = evt.r; _ledStateG = evt.g; _ledStateB = evt.b;
            break;

        case LedEventType::MQTT_BRIGHTNESS:
            _ledActiveBrightness = constrain(evt.brightness, 1, LED_MAX_BRIGHTNESS);
            FastLED.setBrightness(_ledActiveBrightness);
            _ledNvsSave();
            break;

        case LedEventType::MQTT_ANIMATION:
            _ledPreviousState = _ledState;
            strlcpy(_ledMqttAnimName, evt.animName, sizeof(_ledMqttAnimName));
            _ledState = LedState::MQTT_OVERRIDE;
            break;

        case LedEventType::MQTT_COUNT:
            _ledActiveCount = constrain(evt.count, 1, (uint8_t)LED_MAX_NUM_LEDS);
            _ledNvsSave();
            break;

        case LedEventType::MQTT_OFF:
            _ledPreviousState = _ledState;
            _ledState = LedState::OFF;
            break;

        case LedEventType::RESET:
            _ledPreviousState = LedState::IDLE;
            _ledState         = LedState::IDLE;
            break;

        case LedEventType::BOOT_STATE:
            strlcpy(_ledMqttAnimName, evt.animName, sizeof(_ledMqttAnimName));
            _ledState = LedState::BOOT_INDICATOR;
            break;

        case LedEventType::OTA_START:
            if (_ledState != LedState::OTA)
                _ledPreviousState = _ledState;
            _ledState = LedState::OTA;
            break;

        case LedEventType::OTA_DONE:
            _ledState = _ledPreviousState;
            break;
    }
}


// ── FreeRTOS task ─────────────────────────────────────────────────────────────

static void _ws2812Task(void*) {
    while (true) {
        LedEvent evt;
        while (xQueueReceive(_ledEventQueue, &evt, 0) == pdTRUE)
            _ws2812HandleEvent(evt);
        _ws2812RenderFrame();
        vTaskDelay(pdMS_TO_TICKS(LED_REFRESH_MS));
    }
}


// ── Public API ────────────────────────────────────────────────────────────────

// ws2812Init — call once from setup() before ws2812TaskStart().
// Creates the event queue, initialises FastLED, clears strip to black.
inline void ws2812Init() {
    _ledNvsLoad();   // restore persisted brightness + count

    _ledEventQueue = xQueueCreate(LED_EVENT_QUEUE_DEPTH, sizeof(LedEvent));

    FastLED.addLeds<WS2812B, LED_STRIP_PIN, GRB>(_leds, LED_MAX_NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);   // soft current cap
    FastLED.setCorrection(TypicalLEDStrip);           // gamma correction
    FastLED.setBrightness(_ledActiveBrightness);

    fill_solid(_leds, LED_MAX_NUM_LEDS, CRGB::Black);
    FastLED.show();   // deterministic off before first task frame
}

// ws2812TaskStart — spawn the strip task on Core 1.
// Call after ws2812Init().
inline void ws2812TaskStart() {
    xTaskCreatePinnedToCore(
        _ws2812Task,
        "ws2812",
        LED_TASK_STACK,
        nullptr,
        LED_TASK_PRIORITY,
        nullptr,
        1   // Core 1
    );
}

// ws2812PostEvent — thread-safe event post from any context.
// Non-blocking: drops the event silently if the queue is full.
// This is intentional — LED failures must never block RFID or MQTT.
inline void ws2812PostEvent(const LedEvent& e) {
    if (_ledEventQueue)
        xQueueSend(_ledEventQueue, &e, 0);
}

// ws2812PublishState — publish current LED state to .../status/led (retained).
// Called from onMqttConnect (re-sync after reconnect) and LED command handlers.
// mqttPublishLedState is forward-declared here and defined in mqtt_client.h.
inline void ws2812PublishState() {
    const char* stateStr;
    switch (_ledState) {
        case LedState::BOOT_INDICATOR: stateStr = "boot";          break;
        case LedState::IDLE:           stateStr = "idle";          break;
        case LedState::RFID_OK:        stateStr = "rfid_ok";       break;
        case LedState::RFID_FAIL:      stateStr = "rfid_fail";     break;
        case LedState::MQTT_OVERRIDE:  stateStr = "mqtt_override"; break;
        case LedState::OTA:            stateStr = "ota";           break;
        case LedState::OFF:            stateStr = "off";           break;
        default:                       stateStr = "idle";          break;
    }
    mqttPublishLedState(stateStr,
                        _ledStateR, _ledStateG, _ledStateB,
                        _ledActiveBrightness, _ledActiveCount);
}

