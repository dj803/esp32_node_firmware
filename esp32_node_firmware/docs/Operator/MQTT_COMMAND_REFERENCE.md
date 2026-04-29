# MQTT command reference

Every `cmd/*` topic the firmware listens on, with payload examples and
behaviour. Topic prefix is the full ISA-95 path —
`Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/`.

Most commands accept JSON; a few accept a plain-string single value
(noted as **payload type: string**). Retain semantics noted per topic.

## Operational commands

| Topic | Retain | Payload | Behaviour |
|---|---|---|---|
| `cmd/restart` | no | `{}` (or empty) | Defer 300 ms, then `esp_restart()`. `restart_cause="cmd_restart"` published in next boot announcement. |
| `cmd/cred_rotate` | yes | `{"key": "...", "new_password": "..."}` (encrypted) | Rotate WiFi credentials. Decrypt → save NVS → restart. |
| `cmd/ota_check` | no | `{}` | Trigger immediate OTA poll against the manifest. Skips if inside `OTA_CASCADE_QUIET_MS` (v0.4.29+). |
| `cmd/config_mode` | no | `{}` | Reboot into AP-mode portal for re-configuration. |
| `cmd/locate` | no | `{}` | Flash STATUS_LED_PIN 10 × 200 ms (4 s) for physical identification. |
| `cmd/modem_sleep` | yes | `{"seconds": N}` | WiFi modem power save for N seconds. CPU keeps running. |
| `cmd/sleep` | no | `{"seconds": N}` | Light sleep for N seconds. Radio off, RAM preserved. |
| `cmd/deep_sleep` | no | `{"seconds": N}` | Deep sleep for N seconds. Cold boot on wake; RAM wiped. |

## ESP-NOW ranging commands

| Topic | Retain | Payload | Behaviour |
|---|---|---|---|
| `cmd/espnow/ranging` | yes | string `"1"` or `"0"` | Enable/disable ranging. v0.4.29+ persists to NVS so retained-MQTT loss doesn't lose state. |
| `cmd/espnow/name` | yes | `{"name": "Alpha"}` | Set friendly node name. Validates `[A-Za-z0-9_-]{1,15}`. |
| `cmd/espnow/calibrate` | no | see below | Multi-step calibration wizard. |
| `cmd/espnow/filter` | yes | `{"alpha_x100": 30, "outlier_db": 15}` | Update RSSI EMA + outlier-rejection settings. |
| `cmd/espnow/track` | yes | `{"macs": ["aa:bb:..", ...]}` | Limit `/espnow` publishes to listed peers. Empty array clears filter. |
| `cmd/espnow/anchor` | yes | `{"role": "anchor", "x_m": 0.0, "y_m": 0.0, "z_m": 0.0}` | Mark device as fixed anchor with 3-D position. |

`cmd/espnow/calibrate` payloads:
```json
{"cmd": "measure_1m", "peer_mac": "aa:bb:..", "samples": 30}
{"cmd": "measure_d",  "peer_mac": "aa:bb:..", "distance_m": 4.0, "samples": 30}
{"cmd": "measure_at", "peer_mac": "aa:bb:..", "distance_m": 4.0, "samples": 30}
{"cmd": "commit", "tx_power_dbm": -47, "path_loss_n": 2.7}   ← legacy single-pair flow
{"cmd": "commit"}                                            ← linreg over multi-point buffer
{"cmd": "clear"}        ← wipe in-memory multi-point buffer
{"cmd": "reset"}        ← restore compile-time defaults
{"cmd": "forget_peer", "peer_mac": "aa:bb:.."}
```

## RFID commands (RFID_ENABLED variants only)

| Topic | Retain | Payload |
|---|---|---|
| `cmd/rfid/whitelist` | yes | `{"add": ["AABBCCDD"]}` or `{"remove": [...]}` or `{"replace": [...]}` |
| `cmd/rfid/program` | no | `{"writes": [{"block": 4, "data": "hex...", "key_a": "FFFFFFFFFFFF"}], "timeout_ms": 15000}` |
| `cmd/rfid/read_block` | no | `{"block": 4, "key_a": "FFFFFFFFFFFF"}` |
| `cmd/rfid/cancel` | no | `{}` |
| `cmd/rfid/ndef_write` | no | `{"uri": "https://..."}` (NTAG21x only) |

## LED commands (WS2812 strip)

See [LED_COMMANDS.md](LED_COMMANDS.md) for the full schema. Topics:
`cmd/led`, `Enterprise/Site/broadcast/led` (fleet broadcast, v0.4.26+).

## Hall + relay commands (v0.5.0 / variant builds only)

| Topic | Retain | Payload |
|---|---|---|
| `cmd/relay` | yes | `{"ch1": true, "ch2": false}` |
| `cmd/hall/config` | yes | `{"interval_ms": 1000, "threshold_gauss": 50}` |
| `cmd/hall/zero` | no | `{}` (recalibrate to current ambient) |

## Diagnostic commands (planned)

| Topic | Status | Notes |
|---|---|---|
| `cmd/diag/serial_dump` | proposed (#93) | On-demand serial output capture |

## Notes

- **Retain semantics matter.** Retained topics replay on reconnect, so
  the device re-applies the value automatically. Non-retained topics
  fire once and are dropped if the device is offline.
- Payloads exceeding ~256 bytes are silently rejected on most topics
  (the firmware's stack buffer for command payloads is sized for
  small JSON).
- Per-device topics are gated by UUID — a malformed UUID in the path
  will not match any subscriber. Use `/fleet-status` to confirm
  current UUIDs before scripting.

## Stub status

This document covers the v0.4.31 firmware. As new commands ship,
update this table — `mqtt_client.h::onMqttMessage` is the
authoritative source-of-truth. Last sweep: 2026-04-29 PM after
v0.4.31.
