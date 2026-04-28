# Plan: BDD 2CH Relay + BMT 49E Hall Sensor integration (v0.5.0)

> **Status:** Deferred. Plan captured 2026-04-23; hardware on hand but
> integration not yet started. Pick up when ready.
>
> Products:
> - [BDD Relay Board 2CH 5V](https://www.communica.co.za/products/bdd-relay-board-2ch-5v)
> - [BMT 49E Hall Sensor Module](https://www.communica.co.za/products/bmt-49e-hall-sensor-module)

## Hardware summary (from research)

### BDD Relay Board 2CH 5V (JQC-3FF-S-Z clone)
- **Wiring:** `VCC → 5V`, `GND → ESP32 GND`, `IN1 / IN2 → ESP32 GPIOs`,
  `JD-VCC → separate 5V supply` (~150mA inrush for 2 coils will brown
  out the ESP32 otherwise).
- **Logic:** Active-LOW (GPIO LOW = relay energized). **Must write HIGH
  before `pinMode(OUTPUT)`** or both relays click at boot.
- **3.3V opto drive margin:** Works reliably per standard designs;
  flagged as borderline — power signal-side from 5V VCC, ESP32 GPIO
  just pulls LOW.
- **Contacts:** 10A @ 250VAC / 30VDC; onboard flyback diodes.
- **Settle time:** ~10 ms operate / 5 ms release.

### BMT 49E Hall (SS49E + LM393)
- **Supply:** SS49E is 4.5–10.5V per Honeywell datasheet — **3.3V is
  out of spec**. Power from ESP32 VIN/5V rail.
- **Output:** Analog swings 0–5V centred on VCC/2 = 2.5V at zero field;
  sensitivity 1.4 mV/Gauss; range ±1000G linear. **Need a 2×10kΩ
  divider** before ESP32 ADC (3.3V max).
- **D0:** LM393 comparator + onboard trimpot for threshold (optional
  digital out).
- **ADC:** ADC1 required (ADC2 conflicts with WiFi) — GPIO 32–39.

## GPIO inventory (from codebase survey 2026-04-23)

Currently used: 2, 4, 5, 18, 19, 22, 23, 27. Free + safe: 12, 13, 14,
16, 17, 21, 25, 26, 32, 33. Input-only ADC1: 34, 35, 36, 39.

**Proposed assignment** (editable in `config.h` per device if needed):

| Pin | New role | Rationale |
|---|---|---|
| **GPIO 25** | `RELAY_CH1_PIN` | Output-capable, no strapping conflict |
| **GPIO 26** | `RELAY_CH2_PIN` | Same |
| **GPIO 32** | `HALL_AO_PIN` | ADC1_CH4, safe with WiFi |
| **GPIO 33** | `HALL_DO_PIN` *(optional)* | If you want the threshold-digital line too; skip if unused |

## Module design (patterns match existing code)

### `include/relay.h` — mirrors `ws2812.h` (digital output + NVS-persisted state)
- **Guard:** `#ifdef RELAY_ENABLED` (like `RFID_ENABLED`, defined in
  `config.h`)
- **State:** `static bool _relayState[2]` + `portMUX` (MQTT handler runs
  on async_tcp; loop reads)
- **API:** `relayInit()`, `relaySet(ch, bool)`, `relayGet(ch)`,
  `relayLoop()` (optional — for scheduled-off later)
- **NVS:** namespace `esp32relay`, keys `ch1` / `ch2` (uint8_t).
  Restored on boot so state survives reboot
- **MQTT:**
  - `cmd/relay` retained JSON `{"ch":1,"state":true}` or
    `{"ch":"all","state":false}`
  - `status/relay` retained JSON `{"ch1":true,"ch2":false}` — republished
    on every state change
  - `relay_enabled: true` field added to heartbeat status
- **LED feedback:** single white flash on relay toggle via
  `ws2812PostEvent` (mirrors RFID pattern)
- **Safety detail:** `digitalWrite(pin, HIGH)` **before**
  `pinMode(OUTPUT)` in init — prevents boot-click

### `include/hall.h` — mirrors `ble.h` (periodic telemetry, NVS-persisted config)
- **Guard:** `#ifdef HALL_ENABLED`
- **State:** `static int16_t _hallOffsetMv`, `static uint32_t
  _hallIntervalMs`, `static int16_t _hallThresholdGauss`, `static bool
  _hallLastAboveThreshold`
- **API:** `hallInit()` (ADC config, NVS load), `hallLoop()` (periodic
  read + publish + threshold edge detect)
- **NVS:** namespace `esp32hall`, keys `offset_mv` (calibration — press
  "zero" to capture current reading), `interval_ms`, `thresh_g`
- **MQTT:**
  - `cmd/hall/config` retained JSON
    `{"interval_ms":1000,"threshold_gauss":50}`
  - `cmd/hall/zero` one-shot — sets `offset_mv` to current reading,
    persists
  - `telemetry/hall` periodic
    `{"voltage_v":1.25,"gauss":-12,"above_threshold":false}`
  - `telemetry/hall/edge` on threshold cross
    `{"edge":"rising","gauss":62,"threshold_gauss":50}` — so Node-RED
    can alert without polling
  - `hall_enabled: true` field in heartbeat
- **Conversion:** `gauss = (v_sensor - offset_mv/1000 - 2.5) / 0.0014` —
  calibrated per unit via `cmd/hall/zero`
- **Core pinning:** loop runs on main task at `_hallIntervalMs` cadence
  (no separate task)

### `config.h` additions
```cpp
// Relay
#define RELAY_ENABLED
#define RELAY_CH1_PIN         25
#define RELAY_CH2_PIN         26
#define RELAY_ACTIVE_LOW      1   // 1 = LOW energizes (JQC-3FF-S-Z); 0 = active-HIGH boards
#define RELAY_NVS_NAMESPACE   "esp32relay"

// Hall
#define HALL_ENABLED
#define HALL_AO_PIN           32   // ADC1_CH4
#define HALL_INTERVAL_MS      1000
#define HALL_THRESHOLD_GAUSS  50
#define HALL_DIVIDER_RATIO    2.0f // 2×10k divider from 0-5V to 0-2.5V
#define HALL_SENSOR_VCC_V     5.0f
#define HALL_SENSITIVITY_MV_PER_GAUSS  1.4f
#define HALL_NVS_NAMESPACE    "esp32hall"
```

## mqtt_client.h integration

- Subscribe `cmd/relay`, `cmd/hall/config`, `cmd/hall/zero` in
  `onMqttConnect` (QoS 1 retained for the two state-carrying ones,
  QoS 1 non-retained for the one-shot `zero`)
- Dispatch branches in `onMqttMessage` — follow the `cmd/rfid/*`
  pattern exactly
- Add `relay_enabled` + `hall_enabled` fields to `mqttPublishStatus`
  heartbeat / boot announcement
- Forward-declares in `mqtt_client.h` header (like existing RFID / BLE
  forward decls)

## main.cpp integration

- `#include "relay.h"` + `#include "hall.h"` (after `ws2812.h`, before
  `mqtt_client.h` so the forward-decls resolve)
- `relayInit()` + `hallInit()` in the one-time-OPERATIONAL setup block
  (~line 559–566, next to `rfidInit()` / `bleInit()`)
- `hallLoop()` in the main loop alongside `bleLoop()` (relay needs no
  loop — it's write-only)
- `LOG_HEAP("after-relay")` and `LOG_HEAP("after-hall")` for boot-phase
  visibility

## Node-RED UI

- **Relay tile** on the existing Device Controls group: per-device card
  with two toggle switches (CH1 / CH2). Publishes
  `cmd/relay {ch, state}`. Subscribes to `status/relay` to reflect
  actual device state.
- **Hall tile** on a new "Sensors" group OR on Device Status: live
  gauge (Gauss), threshold slider (`cmd/hall/config`), a Zero button
  (`cmd/hall/zero`). Edge events populate a small rolling log.
- Both tiles use the existing `hb_devices` registry to populate device
  dropdown — same pattern as the LED Control tab.
- Push via `push_map_flow.py`-style Admin API deploy.

## New docs (create alongside this plan when implementing)

- **`docs/HARDWARE_WIRING.md`** — diagram + pinout table for both
  modules, including the **separate-5V-supply** warning for the relay
  and the **voltage divider** warning for the Hall sensor. Mirrors the
  wiring block already in `rfid.h`'s top comment.

## Release plan

- **v0.5.0 — firmware + Node-RED in one release**
  - Two new modules + config.h + mqtt_client.h + main.cpp +
    docs/HARDWARE_WIRING.md + flows.json additions.
  - All 3 devices build & flash identically (compile-time guards
    enabled by default). Devices without physical hardware just have
    idle GPIOs + the ADC reading floating noise — no harm; you wire in
    the modules as/when you want each device to have them.
- **Flash Alpha via COM5** (user can attach the hardware first OR
  after — the code is safe either way).
- **OTA Bravo + Charlie** — they'll have the firmware but no hardware
  connected; MQTT cmd/relay to them is a no-op on the wire side.
- **Hardware attachment** is per-device and independent of the firmware
  flash order.

## Verification

1. `pio run -e esp32dev` — clean compile (static_asserts in
   `lib_api_assert.h` still pass).
2. Flash Alpha with v0.5.0 after wiring up at least one relay channel +
   the Hall sensor.
3. `mosquitto_pub .../cmd/relay '{"ch":1,"state":true}'` — should click
   relay 1 closed; verify LED flash.
4. Reboot Alpha (`cmd/restart`) — relay state should be restored from
   NVS (if retained MQTT cmd survives).
5. `mosquitto_sub .../telemetry/hall` — verify periodic
   `voltage_v`/`gauss` publications.
6. Wave a magnet near the Hall sensor — confirm `telemetry/hall/edge`
   fires on threshold cross.
7. Node-RED tiles render + respond to UI clicks on Alpha.
8. OTA Bravo + Charlie — confirm no panic (they have no hardware
   attached, should boot clean, `telemetry/hall` reports noise around
   ~0 G).

## Scope choices to reconfirm when picking this up

| Choice | Options | Default if you don't pick |
|---|---|---|
| Ship both modules together or split? | 1) v0.5.0 = both. 2) v0.5.0 = relay only, v0.5.1 = hall | **Both together** — they don't share code, risk is additive not compounding |
| Hall D0 pin? | 1) Use it (one more GPIO). 2) Skip — rely on firmware threshold logic | **Skip** — threshold in firmware is more flexible than onboard trimpot |
| Relay scheduled-off? | 1) Just set/get now. 2) Add `cmd/relay {ch,state:true,auto_off_s:30}` | **Just set/get** for v0.5.0; scheduled-off is a clean v0.5.1 addition if you want it |
| Compile-time guards default | 1) Enabled by default (all devices build with relay+hall). 2) Disabled, enable per-device in a local header | **Enabled by default** — compile-time cost is trivial, untaken GPIOs are harmless |

## Appendix: research sources

- Tongling JQC-3FF-S-Z datasheet (coil current ~72 mA, contact rating
  10A).
- Honeywell SS49E datasheet rev GLB-40, 2008 (supply 4.5–10.5V, 1.4
  mV/Gauss, VCC/2 quiescent).
- Espressif ESP32 ADC errata (ADC2 unavailable while WiFi active; use
  ADC1 GPIO 32–39).
- Existing peripheral integrations: `include/rfid.h`, `include/ble.h`,
  `include/ws2812.h` — templates to mirror.
