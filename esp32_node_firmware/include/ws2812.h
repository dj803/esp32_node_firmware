#pragma once

#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include "config.h"
#include "nvs_utils.h"   // NvsPutIfChanged — compare-before-write wrappers
#include "prefs_quiet.h" // (v0.4.03) prefsTryBegin — silent on missing namespace

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
    MQTT_HEALTHY,    // WiFi + MQTT connected → slow green breathing (operational heartbeat)
    MQTT_OVERRIDE_TIMED, // (#23) Timed override: color+anim for duration_ms then auto-revert
    MQTT_PIXEL_SET,  // (#19) Set single pixel {index, r, g, b}
    MQTT_PIXEL_COMMIT, // (#19) Switch to MQTT_PIXELS state and freeze _leds[] for direct render
    SCENE_SAVE,      // (#20) Persist current _leds[] + brightness to NVS under animName
    SCENE_LOAD,      // (#20) Load scene by name from NVS and apply as MQTT_PIXELS
    SCENE_DELETE,    // (#20) Remove a saved scene from NVS
};

struct LedEvent {
    LedEventType type;
    uint8_t      r, g, b;        // for MQTT_COLOR
    uint8_t      brightness;     // for MQTT_BRIGHTNESS (0–255)
    uint8_t      count;          // for MQTT_COUNT / pixel index
    char         animName[16];   // for MQTT_ANIMATION / BOOT_STATE / MQTT_OVERRIDE_TIMED
    uint32_t     duration_ms;    // (#23) for MQTT_OVERRIDE_TIMED: ms before auto-revert
};


// ── State definitions ─────────────────────────────────────────────────────────

enum class LedState : uint8_t {
    BOOT_INDICATOR,
    IDLE,
    MQTT_HEALTHY,    // Slow green breathing — WiFi + MQTT both connected
    RFID_OK,
    RFID_FAIL,
    MQTT_OVERRIDE,
    MQTT_PIXELS,     // (#19) per-pixel mode: renderer skips fill_solid; _leds[] is the source of truth
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

// (#23, v0.4.26) MQTT-commanded timed override end-time. Set by
// MQTT_OVERRIDE_TIMED handler; checked by renderer in MQTT_OVERRIDE state.
// 0 = no expiry (untimed override from MQTT_COLOR / MQTT_ANIMATION).
static uint32_t      _ledOverrideEndMs = 0;

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


// ── Scene persistence (#20, v0.4.26) ──────────────────────────────────────────
// Saved scenes live in their own NVS namespace `led_scenes`. Each scene is
// stored under key `s_<name>` (where name is a 1-12 char alphanumeric tag —
// chosen short so the NVS key length stays < 15 chars). Payload format:
//
//   bytes 0:    brightness (uint8)
//   bytes 1..3*N: N pixel triples (r, g, b)
//   bytes 1+3*N: pixel count (uint8)  -- last byte for forward compat
//
// Bounded to LED_MAX_NUM_LEDS pixels = ~25 bytes per scene. Conservative
// limit of 8 scene slots (so the NVS partition isn't dominated by LED data).
// The list of saved scene names lives under key `_names` as a
// comma-separated string for simple enumeration via cmd/led scene_list.

#define LED_SCENES_NS         "led_scenes"
#define LED_SCENE_NAMES_KEY   "_names"
#define LED_MAX_SCENES        8
#define LED_SCENE_NAME_MAX    12

// Sanitise a scene name to alphanumeric + underscore, max 12 chars.
// Returns true if the name is non-empty after sanitising.
static bool _ledSceneNameOk(const char* name) {
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len > LED_SCENE_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}

// Serialise current _leds[] + brightness into a byte buffer.
// Caller-owned buffer must be at least 1 + 3 * LED_MAX_NUM_LEDS + 1 bytes.
// Returns the actual byte count written.
static size_t _ledSceneSerialise(uint8_t* out) {
    out[0] = (uint8_t)_ledActiveBrightness;
    uint8_t n = _ledActiveCount;
    if (n > LED_MAX_NUM_LEDS) n = LED_MAX_NUM_LEDS;
    for (uint8_t i = 0; i < n; i++) {
        out[1 + i*3 + 0] = _leds[i].r;
        out[1 + i*3 + 1] = _leds[i].g;
        out[1 + i*3 + 2] = _leds[i].b;
    }
    out[1 + n*3] = n;   // pixel count footer
    return 1 + n*3 + 1;
}

// Deserialise into _leds[] + adjust brightness. Returns true on a
// well-formed payload (size + count match).
static bool _ledSceneDeserialise(const uint8_t* in, size_t len) {
    if (len < 2) return false;
    uint8_t br = in[0];
    uint8_t n  = in[len - 1];
    if (n > LED_MAX_NUM_LEDS) return false;
    if (len != (size_t)(1 + n*3 + 1)) return false;
    if (br == 0 || br > LED_MAX_BRIGHTNESS) br = LED_MAX_BRIGHTNESS;
    _ledActiveBrightness = br;
    FastLED.setBrightness(br);
    fill_solid(_leds, LED_MAX_NUM_LEDS, CRGB::Black);
    for (uint8_t i = 0; i < n; i++) {
        _leds[i] = CRGB(in[1 + i*3 + 0], in[1 + i*3 + 1], in[1 + i*3 + 2]);
    }
    return true;
}

// Read+update the comma-separated names index. `op` is +1 to add, -1 to
// remove. Idempotent — adding an existing name is a no-op; removing a
// missing name is a no-op. Returns true on a successful NVS write.
static bool _ledSceneNamesUpdate(const char* name, int op) {
    Preferences p;
    if (!p.begin(LED_SCENES_NS, false)) return false;
    String names = p.getString(LED_SCENE_NAMES_KEY, "");
    String tok   = String(name);
    String wrapped = String(",") + names + ",";
    bool present = (wrapped.indexOf("," + tok + ",") >= 0);
    bool changed = false;
    if (op > 0 && !present) {
        if (names.length() > 0) names += ",";
        names += tok;
        changed = true;
    } else if (op < 0 && present) {
        wrapped.replace("," + tok + ",", ",");
        if (wrapped.length() >= 2) names = wrapped.substring(1, wrapped.length() - 1);
        else names = "";
        changed = true;
    }
    if (changed) p.putString(LED_SCENE_NAMES_KEY, names);
    p.end();
    return changed;
}

// Save current _leds[] + brightness as a named scene. Enforces the
// LED_MAX_SCENES quota by refusing the save when the names index is full
// (operator must scene_delete first).
static bool _ledSceneSave(const char* name) {
    if (!_ledSceneNameOk(name)) return false;
    Preferences p;
    if (!p.begin(LED_SCENES_NS, true)) {
        // Namespace empty — first scene. Re-open for write below.
    } else {
        String names = p.getString(LED_SCENE_NAMES_KEY, "");
        // Count comma-separated entries.
        int slots = names.length() == 0 ? 0 : 1;
        for (int i = 0; i < (int)names.length(); i++) if (names[i] == ',') slots++;
        // If saving a NEW name and we're already at the cap, refuse.
        String wrapped = "," + names + ",";
        bool exists = wrapped.indexOf("," + String(name) + ",") >= 0;
        p.end();
        if (!exists && slots >= LED_MAX_SCENES) {
            LOG_W("ws2812", "scene_save '%s' refused — quota %d full", name, LED_MAX_SCENES);
            return false;
        }
    }
    uint8_t buf[1 + 3 * LED_MAX_NUM_LEDS + 1];
    size_t n = _ledSceneSerialise(buf);
    if (!p.begin(LED_SCENES_NS, false)) return false;
    char key[20]; snprintf(key, sizeof(key), "s_%s", name);  // 20 = 2 prefix + LED_SCENE_NAME_MAX (12) + room
    p.putBytes(key, buf, n);
    p.end();
    _ledSceneNamesUpdate(name, +1);
    return true;
}

// Load a saved scene into _leds[] + brightness. Caller transitions the
// state machine to MQTT_PIXELS afterward.
static bool _ledSceneLoad(const char* name) {
    if (!_ledSceneNameOk(name)) return false;
    Preferences p;
    if (!p.begin(LED_SCENES_NS, true)) return false;
    char key[20]; snprintf(key, sizeof(key), "s_%s", name);  // 20 = 2 prefix + LED_SCENE_NAME_MAX (12) + room
    size_t len = p.getBytesLength(key);
    if (len == 0 || len > 1 + 3 * LED_MAX_NUM_LEDS + 1) {
        p.end();
        LOG_W("ws2812", "scene_load '%s' missing or malformed", name);
        return false;
    }
    uint8_t buf[1 + 3 * LED_MAX_NUM_LEDS + 1];
    p.getBytes(key, buf, len);
    p.end();
    return _ledSceneDeserialise(buf, len);
}

// Remove a saved scene from NVS.
static bool _ledSceneDelete(const char* name) {
    if (!_ledSceneNameOk(name)) return false;
    Preferences p;
    if (!p.begin(LED_SCENES_NS, false)) return false;
    char key[20]; snprintf(key, sizeof(key), "s_%s", name);  // 20 = 2 prefix + LED_SCENE_NAME_MAX (12) + room
    bool ok = p.remove(key);
    p.end();
    if (ok) _ledSceneNamesUpdate(name, -1);
    return ok;
}

// Read the comma-separated scene-names index. Public so cmd/led scene_list
// can publish the result. Caller-owned buffer.
static String ledSceneList() {
    Preferences p;
    if (!p.begin(LED_SCENES_NS, true)) return String("");
    String names = p.getString(LED_SCENE_NAMES_KEY, "");
    p.end();
    return names;
}


// ── NVS helpers ───────────────────────────────────────────────────────────────

static void _ledNvsSave() {
    Preferences p;
    p.begin(LED_STRIP_NVS_NAMESPACE, false);
    // NvsPutIfChanged skips the write when the stored value already matches.
    // Matters because every MQTT cmd/led brightness / count / animation call
    // triggers a save, even when the payload repeats (e.g. Node-RED dashboard
    // slider at rest firing periodic same-value updates).
    NvsPutIfChanged(p, "brightness", (uint8_t)_ledActiveBrightness);
    NvsPutIfChanged(p, "count",      (uint8_t)_ledActiveCount);
    p.end();
}

static void _ledNvsLoad() {
    Preferences p;
    // (v0.4.03) prefsTryBegin: silent on missing namespace (fresh device).
    // If begin fails, the getUChar defaults still apply.
    if (!prefsTryBegin(p, LED_STRIP_NVS_NAMESPACE, true)) {
        _ledActiveBrightness = LED_MAX_BRIGHTNESS;
        _ledActiveCount      = LED_DEFAULT_COUNT;
        return;
    }
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
            // Slow 4-second blue breathing — WiFi up, MQTT not yet connected
            fill_solid(_leds, _ledActiveCount, _breathe(4000, CRGB(0, 0, 255)));
            break;

        case LedState::MQTT_HEALTHY:
            // Slow 4-second green breathing — WiFi + MQTT both connected (operational)
            fill_solid(_leds, _ledActiveCount, _breathe(4000, CRGB(0, 255, 0)));
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
            // (#23, v0.4.26) Timed-override auto-revert. _ledOverrideEndMs is
            // 0 for untimed overrides (MQTT_COLOR / MQTT_ANIMATION) and
            // millis()+duration for MQTT_OVERRIDE_TIMED. When it expires,
            // restore the previous state — same shape as RFID_OK / LOCATE.
            if (_ledOverrideEndMs && (int32_t)(millis() - _ledOverrideEndMs) >= 0) {
                _ledState         = _ledPreviousState;
                _ledOverrideEndMs = 0;
                break;
            }
            if (strcmp(_ledMqttAnimName, "breathing") == 0) {
                fill_solid(_leds, _ledActiveCount,
                           _breathe(4000, _ledMqttColor));
            } else if (strcmp(_ledMqttAnimName, "rainbow") == 0) {
                uint8_t hue = (millis() / 10) & 0xFF;
                fill_rainbow(_leds, _ledActiveCount, hue, 255 / _ledActiveCount);
            } else if (strcmp(_ledMqttAnimName, "alarm") == 0) {
                // (#23) Fast red/black flash for alarm-class app events.
                bool on = (millis() % 400) < 200;
                fill_solid(_leds, _ledActiveCount, on ? CRGB::Red : CRGB::Black);
            } else if (strcmp(_ledMqttAnimName, "warn") == 0) {
                // (#23) Slow amber breathing for warn-class app events.
                fill_solid(_leds, _ledActiveCount,
                           _breathe(2000, CRGB(255, 180, 0)));
            } else {
                // Default: "solid" or any unrecognised name
                fill_solid(_leds, _ledActiveCount, _ledMqttColor);
            }
            break;
        }

        case LedState::MQTT_PIXELS:
            // (#19, v0.4.26) Per-pixel mode: _leds[] holds the operator-supplied
            // pixel buffer (set via MQTT_PIXEL_SET events). Renderer just
            // forwards what's there to FastLED. We still zero out beyond
            // _ledActiveCount above (top of function), so a strip narrower
            // than LED_MAX_NUM_LEDS won't accidentally light unused pixels.
            break;

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
            // Don't clobber _ledPreviousState if an override is already active
            // (operator might be tweaking color mid-override; revert target
            // should still be the pre-override state).
            if (_ledState != LedState::MQTT_OVERRIDE && _ledState != LedState::MQTT_PIXELS) {
                _ledPreviousState = _ledState;
            }
            _ledMqttColor     = CRGB(evt.r, evt.g, evt.b);
            strlcpy(_ledMqttAnimName, "solid", sizeof(_ledMqttAnimName));
            _ledState         = LedState::MQTT_OVERRIDE;
            _ledOverrideEndMs = 0;   // (#23) untimed — no auto-revert
            _ledStateR = evt.r; _ledStateG = evt.g; _ledStateB = evt.b;
            break;

        case LedEventType::MQTT_BRIGHTNESS:
            _ledActiveBrightness = constrain(evt.brightness, 1, LED_MAX_BRIGHTNESS);
            FastLED.setBrightness(_ledActiveBrightness);
            _ledNvsSave();
            break;

        case LedEventType::MQTT_ANIMATION:
            if (_ledState != LedState::MQTT_OVERRIDE && _ledState != LedState::MQTT_PIXELS) {
                _ledPreviousState = _ledState;
            }
            strlcpy(_ledMqttAnimName, evt.animName, sizeof(_ledMqttAnimName));
            _ledState         = LedState::MQTT_OVERRIDE;
            _ledOverrideEndMs = 0;   // (#23) untimed — no auto-revert
            break;

        case LedEventType::MQTT_OVERRIDE_TIMED: {
            // (#23, v0.4.26) Apply color + animation for duration_ms then
            // auto-revert to whatever was showing before. Mirrors the
            // RFID_OK / LOCATE auto-revert pattern but operator-controllable
            // via MQTT for app-level events (door left open, sensor fault,
            // OTA-in-progress overlay, etc.).
            if (_ledState != LedState::MQTT_OVERRIDE && _ledState != LedState::MQTT_PIXELS) {
                _ledPreviousState = _ledState;
            }
            _ledMqttColor = CRGB(evt.r, evt.g, evt.b);
            strlcpy(_ledMqttAnimName, evt.animName, sizeof(_ledMqttAnimName));
            _ledState         = LedState::MQTT_OVERRIDE;
            _ledOverrideEndMs = evt.duration_ms ? millis() + evt.duration_ms : 0;
            _ledStateR = evt.r; _ledStateG = evt.g; _ledStateB = evt.b;
            break;
        }

        case LedEventType::MQTT_PIXEL_SET:
            // (#19, v0.4.26) Single-pixel write into the FastLED buffer. Index
            // bounds-checked against _ledActiveCount. Multiple of these are
            // typically queued back-to-back from the cmd/led "pixels" handler;
            // the trailing MQTT_PIXEL_COMMIT switches state to MQTT_PIXELS so
            // the renderer stops overwriting the buffer.
            if (evt.count < _ledActiveCount && evt.count < LED_MAX_NUM_LEDS) {
                _leds[evt.count] = CRGB(evt.r, evt.g, evt.b);
            }
            break;

        case LedEventType::MQTT_PIXEL_COMMIT:
            // (#19) Switch to per-pixel mode. _leds[] is now the source of
            // truth — renderer skips fill_solid in MQTT_PIXELS state. Operator
            // returns to a normal state via cmd/led {"cmd":"reset"} or any
            // other state-changing event.
            if (_ledState != LedState::MQTT_PIXELS && _ledState != LedState::MQTT_OVERRIDE) {
                _ledPreviousState = _ledState;
            }
            _ledState         = LedState::MQTT_PIXELS;
            _ledOverrideEndMs = 0;
            // No _ledStateR/G/B update — the buffer is heterogeneous.
            break;

        case LedEventType::SCENE_SAVE:
            // (#20, v0.4.26) Persist current _leds[] + brightness to NVS.
            // Capture-as-snapshot; doesn't change visible state.
            if (_ledSceneSave(evt.animName)) {
                LOG_I("ws2812", "scene saved '%s'", evt.animName);
            }
            break;

        case LedEventType::SCENE_LOAD:
            // (#20) Load saved pixel buffer + brightness, switch to MQTT_PIXELS.
            if (_ledSceneLoad(evt.animName)) {
                if (_ledState != LedState::MQTT_PIXELS && _ledState != LedState::MQTT_OVERRIDE) {
                    _ledPreviousState = _ledState;
                }
                _ledState         = LedState::MQTT_PIXELS;
                _ledOverrideEndMs = 0;
                LOG_I("ws2812", "scene loaded '%s'", evt.animName);
            }
            break;

        case LedEventType::SCENE_DELETE:
            // (#20) Remove a saved scene. No state change.
            if (_ledSceneDelete(evt.animName)) {
                LOG_I("ws2812", "scene deleted '%s'", evt.animName);
            }
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

        case LedEventType::MQTT_HEALTHY:
            // Set as both current and previous so RFID/OTA overlays revert here.
            _ledState         = LedState::MQTT_HEALTHY;
            _ledPreviousState = LedState::MQTT_HEALTHY;
            _ledStateR = 0; _ledStateG = 255; _ledStateB = 0;
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
        case LedState::MQTT_HEALTHY:   stateStr = "mqtt_healthy";  break;
        case LedState::RFID_OK:        stateStr = "rfid_ok";       break;
        case LedState::RFID_FAIL:      stateStr = "rfid_fail";     break;
        case LedState::MQTT_OVERRIDE:  stateStr = "mqtt_override"; break;
        case LedState::MQTT_PIXELS:    stateStr = "mqtt_pixels";   break;
        case LedState::OTA:            stateStr = "ota";           break;
        case LedState::OFF:            stateStr = "off";           break;
        default:                       stateStr = "idle";          break;
    }
    mqttPublishLedState(stateStr,
                        _ledStateR, _ledStateG, _ledStateB,
                        _ledActiveBrightness, _ledActiveCount);
}

