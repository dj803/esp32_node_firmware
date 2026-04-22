# LED Control Reference

Single source of truth for the ESP32 node LEDs as exposed over MQTT.
Firmware (`include/ws2812.h`, `include/led.h`, `include/mqtt_client.h`)
and the Node-RED **LED Control** dashboard tab (`/led`, added in
v0.3.18) both follow this document — update here first when the wire
contract changes.

---

## Two independent LEDs

| Hardware | GPIO | Purpose | Control |
|---|---|---|---|
| Status LED (on-board) | `STATUS_LED_PIN` (2) | Connection + error state | Automatic, driven by firmware state machine (`led.h`). Overlay-only from outside: `cmd/locate`. |
| WS2812B strip | `LED_STRIP_PIN` (27) | User-facing indicator | Fully MQTT-controllable via `cmd/led`. |

---

## WS2812B strip — `cmd/led`

All topics live under the device's ISA-95 path:
`Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/…`

### Subscribe (Node-RED → Device)

Topic: `cmd/led` — QoS 1, retain=**false**.
Payload: JSON. The `cmd` field selects the action; other fields are
action-specific.

```jsonc
// Solid colour — RGB, 0-255 each.
{"cmd":"color", "r":255, "g":0, "b":128}

// Brightness 1-255 (persists to NVS).
{"cmd":"brightness", "value":100}

// Named animation: "solid" | "breathing" | "rainbow".
// Unknown names fall back to "solid".
{"cmd":"animation", "name":"breathing"}

// Active LED count 1-LED_MAX_NUM_LEDS (64) (persists to NVS).
{"cmd":"count", "value":16}

// All LEDs black (persists until RESET or another command).
{"cmd":"off"}

// Back to IDLE (slow blue breathing).
{"cmd":"reset"}
```

Missing / invalid fields use constrained defaults; unknown `cmd`
values are logged as a warning and ignored.

> Retain = **false** because commands are one-shot. A retained
> command would re-arm every node that reconnects.

### Publish (Device → Node-RED)

Topic: `status/led` — QoS 1, **retain=true**.
Payload:

```json
{
  "state":      "mqtt_color",
  "r":          255, "g": 0, "b": 128,
  "brightness": 100,
  "count":      16,
  "uptime_s":   43210
}
```

`state` ∈ `idle` | `mqtt_color` | `mqtt_off` | `boot_state` |
`ota` | `rfid_ok` | `rfid_fail`.

Retained so Node-RED populates the state table instantly on
subscribe — no need to wait for the next change.

---

## Status LED — `cmd/locate`

Topic: `cmd/locate` — QoS 1, retain=**false**.
Payload: **ignored**. Any publish (empty string works) arms the
flash.

Effect: the status LED runs **10 × 200 ms ON / 200 ms OFF**
(4 seconds total), then auto-reverts to whatever pattern was
running before (`MQTT_CONNECTED` heartbeat in normal operation).

Implementation detail — the overlay is realised as a new
`LedPattern::LOCATE` value in `include/led.h`, sharing the
save-and-restore machinery already used by `ESPNOW_FLASH`. The
flash is **non-blocking**: `ledSetPattern(LedPattern::LOCATE)`
returns immediately; the 10 ms timer callback handles the
sequence. Safe to call from the MQTT message handler.

The AP portal "Locate This Device" button (`ap_portal.h`) uses the
same path via `ledFlashLocate()`, which is now a one-liner wrapper
around `ledSetPattern(LedPattern::LOCATE)`.

---

## MQTT topic summary

| Topic | Direction | Payload | Retain |
|---|---|---|---|
| `cmd/led` | Node-RED → Device | JSON — see above | false |
| `cmd/locate` | Node-RED → Device | empty / ignored | false |
| `status/led` | Device → Node-RED | JSON — see above | **true** |

---

## Related

- **Status LED patterns** (internally driven, not MQTT-controllable):
  `include/led.h` — `OFF`, `BOOT`, `WIFI_CONNECTING`,
  `WIFI_CONNECTED`, `AP_MODE`, `MQTT_CONNECTED`, `OTA_UPDATE`,
  `ERROR`, `ESPNOW_FLASH`, `LOCATE`.
- **WS2812 animations**: `include/ws2812.h` — driven from a
  dedicated FreeRTOS task on Core 1 (30 ms refresh).
- **RFID overlays**: `rfid.h` posts `RFID_OK` (solid green, 2 s) or
  `RFID_FAIL` (solid red, 2 s) events to the strip that auto-revert
  to the previous state.
