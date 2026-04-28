# LED commands — operator reference

> Single-source reference for every `cmd/led` payload supported by the
> firmware. Companion to [ws2812.h](../include/ws2812.h) (state machine)
> and [led_schedule.h](../include/led_schedule.h) (#22 schedule).
> Captured 2026-04-28 against v0.4.26.

## Topic and routing

```
publish:    Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/led
broadcast:  Enigma/JHBDev/broadcast/led        ← (#21) all-devices fan-out
status:     Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status/led
```

The broadcast topic uses identical schema; every device that receives a
publish applies the command locally. No deduplication — last-write-wins
if both broadcast and per-device publishes target the same device.

## Status payloads

`status/led` (retained, QoS 1):

```jsonc
{
  "state":      "idle|mqtt_healthy|mqtt_override|mqtt_pixels|rfid_ok|rfid_fail|ota|off",
  "r":          0,
  "g":          255,
  "b":          0,
  "brightness": 80,    // 0-255
  "count":      8      // active LED count
}
```

`status/led_scenes` (retained, QoS 1, published only after `scene_list`):

```json
{"scenes":"alarm,morning,party"}
```

`status/led_schedule` (retained, QoS 1, published only after `sched_list`):

```json
{"slots":[{"id":"morning","hour":7,"minute":0,"action":{...}}]}
```

## Commands

### `color` — set solid color (untimed)

```json
{"cmd":"color","r":255,"g":150,"b":0}
```

State → `mqtt_override` with anim "solid". No auto-revert. Use `reset`
or another command to leave.

### `brightness` — set strip brightness

```json
{"cmd":"brightness","value":80}
```

Persists to NVS (`led_strip` namespace). Range 1-255; values outside
clamp to LED_MAX_BRIGHTNESS.

### `count` — set active LED count

```json
{"cmd":"count","value":8}
```

Persists to NVS. Range 1-LED_MAX_NUM_LEDS. Pixels beyond the active
count always render black.

### `animation` — named animation (untimed)

```json
{"cmd":"animation","name":"breathing"}
```

Built-in names:
- `solid`     — fill with `_ledMqttColor`
- `breathing` — 4 s sine-wave breathing of `_ledMqttColor`
- `rainbow`   — fill_rainbow over the active count, 100 ms hue step
- `alarm`     — fast red flash (200 ms ON / 200 ms OFF) — added v0.4.26 (#23)
- `warn`      — slow amber breathing (2 s) — added v0.4.26 (#23)

Unrecognised names fall back to `solid`.

### `override` — timed override (#23, v0.4.26)

```json
{"cmd":"override","r":255,"g":0,"b":0,"anim":"alarm","duration_ms":5000}
```

State → `mqtt_override` with the supplied color + anim, auto-reverting
to the previous state after `duration_ms` ms. Use case: app-level
events (door left open, sensor fault, OTA-in-progress overlay) where
the LED should flash a warning then resume normal operation.

- `duration_ms` 0 = untimed (equivalent to `color`+`animation`).
- `duration_ms` cap = 3,600,000 ms (1 hour).

### `pixel` — single-pixel write (#19, v0.4.26)

```json
{"cmd":"pixel","i":3,"r":0,"g":255,"b":0}
```

Writes one pixel and auto-commits to `mqtt_pixels` state. Multiple
back-to-back single-pixel writes work as expected — each commit is a
no-op once already in `mqtt_pixels`.

`i` is bounds-checked against LED_MAX_NUM_LEDS; out-of-range indices
are silently dropped.

### `pixels` — bulk pixel write (#19, v0.4.26)

```json
{"cmd":"pixels","data":[[255,0,0],[255,128,0],[255,255,0],[0,255,0]]}
```

Each inner array is one pixel `[r, g, b]`. Pixels beyond the array
length keep their existing buffer value — send `{"cmd":"off"}` first
if you want a known-black starting state.

State → `mqtt_pixels` after the implicit commit.

### `off` — all LEDs black

```json
{"cmd":"off"}
```

State → `off`. RFID overlays still take precedence temporarily.

### `reset` — return to operational default

```json
{"cmd":"reset"}
```

State → `idle` (slow blue breathing). Subsequent MQTT health check
will transition to `mqtt_healthy` (slow green breathing).

### `scene_save` — persist current state as a named scene (#20, v0.4.26)

```json
{"cmd":"scene_save","name":"alarm"}
```

Captures `_leds[]` + brightness as a named scene in NVS. Names are
alphanumeric+underscore, max 12 characters. Up to LED_MAX_SCENES (8)
named slots; further saves are refused once the quota is full
(operator must `scene_delete` first).

Tip: send `pixels` first to set up the buffer, then `scene_save`.
Saving from an animated state (`rainbow`, `alarm`) captures the single
frame in the buffer — animations don't snapshot well.

### `scene_load` — apply a saved scene

```json
{"cmd":"scene_load","name":"alarm"}
```

Loads pixels + brightness from NVS, transitions to `mqtt_pixels`. If
the named scene doesn't exist the command is a logged no-op.

### `scene_delete` — remove a saved scene

```json
{"cmd":"scene_delete","name":"alarm"}
```

### `scene_list` — publish saved scene names

```json
{"cmd":"scene_list"}
```

Causes the device to publish to `status/led_scenes` (retained, QoS 1).
Operator UI subscribes to that topic to render a dropdown.

### `sched_add` — add or replace a time-of-day schedule (#22, v0.4.26)

```json
{
  "cmd":"sched_add",
  "id":"morning",
  "hour":7, "minute":0,
  "action":{
    "cmd":"override",
    "r":255,"g":150,"b":0,
    "anim":"breathing",
    "duration_ms":3600000
  }
}
```

The `action` object is serialised back to JSON and stored in NVS; at
the scheduled time the stored payload is re-fed through this same
handler — so any cmd/led schema is reusable as a schedule action.

- `id` alphanumeric+underscore, max 12 characters
- `hour` 0-23, `minute` 0-59 (local time per LED_SCHEDULE_TZ_OFFSET_S,
  default UTC+2 = SAST)
- Up to LED_SCHEDULE_MAX_SLOTS (8) total schedules per device
- Replaces existing slot with same `id`; otherwise occupies first free

### `sched_remove` — remove a schedule slot

```json
{"cmd":"sched_remove","id":"morning"}
```

### `sched_clear` — remove all schedule slots

```json
{"cmd":"sched_clear"}
```

### `sched_list` — publish saved schedules

```json
{"cmd":"sched_list"}
```

Causes the device to publish to `status/led_schedule` (retained, QoS 1)
with the slot snapshot including each slot's stored action.

## State machine

```
              ┌─────────┐ MQTT_HEALTHY     ┌──────────────┐
              │  IDLE   │─────────────────►│ MQTT_HEALTHY │
              │ (blue)  │                  │   (green)    │
              └─────────┘                  └──────────────┘
                  ▲ ▲                          ▲ ▲ ▲
                  │ │                          │ │ │
   reset ─────────┘ │   ┌────────────────────┐ │ │ │
                    │   │   MQTT_OVERRIDE    │◄┘ │ │
   color/anim ──────┴──►│  (untimed or       │   │ │
                        │   timed via #23)   │───┘ │
                        └────────────────────┘     │
                                ▲ │  auto-revert   │
                                │ │  (timed only)  │
   override (timed) ────────────┘ │                │
                                  ▼                │
                        ┌────────────────────┐     │
                        │   MQTT_PIXELS      │─────┘
   pixel/pixels ───────►│   (#19 per-pixel)  │
                        └────────────────────┘

   RFID overlays (RFID_OK / RFID_FAIL / LOCATE)  — auto-revert to previous
   OTA overlay   (orange chase)                  — auto-revert on OTA_DONE
```

## Limits and notes

- **Heartbeat cadence**: status/led publishes happen on every state-changing
  command + on `mqtt_connect` reconnect. Pre-existing minor race: the
  publish runs BEFORE the ws2812 task drains its event queue, so the
  retained payload can lag the actual state by one cmd. Not specific
  to v0.4.26.
- **NVS budget**: led_scenes (8 × ~30 B) + led_sched (8 × ~290 B)
  ≈ 2.5 KB. Well within the typical NVS partition size.
- **Time source**: NTP via `pool.ntp.org` configured at first
  OPERATIONAL tick. If NTP hasn't synced, `ledScheduleTick` skips
  silently — schedules don't fire until the clock is set.
- **Broadcast vs per-device**: a broadcast `cmd/led pixels` will write
  the SAME pixel array to every device. Useful for synchronized strip
  effects across a room; less useful when devices have different
  active-pixel counts.

## Operator quick reference

```bash
# Solid red:
mosquitto_pub -h <broker> -t '<root>/<uuid>/cmd/led' -m '{"cmd":"color","r":255,"g":0,"b":0}'

# 5-second alarm overlay:
mosquitto_pub -h <broker> -t '<root>/<uuid>/cmd/led' \
  -m '{"cmd":"override","r":255,"g":0,"b":0,"anim":"alarm","duration_ms":5000}'

# Site-wide alarm broadcast:
mosquitto_pub -h <broker> -t 'Enigma/JHBDev/broadcast/led' \
  -m '{"cmd":"override","r":255,"g":0,"b":0,"anim":"alarm","duration_ms":3000}'

# Save current strip state as "shift_change":
mosquitto_pub -h <broker> -t '<root>/<uuid>/cmd/led' \
  -m '{"cmd":"scene_save","name":"shift_change"}'

# Schedule "warm white" every weekday morning at 07:00 for 1 hour:
mosquitto_pub -h <broker> -t '<root>/<uuid>/cmd/led' -m '{
  "cmd":"sched_add","id":"morning",
  "hour":7,"minute":0,
  "action":{"cmd":"override","r":255,"g":200,"b":150,
            "anim":"solid","duration_ms":3600000}
}'

# Reset to operational default (green breathing):
mosquitto_pub -h <broker> -t '<root>/<uuid>/cmd/led' -m '{"cmd":"reset"}'
```
