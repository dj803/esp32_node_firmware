# Diagnostic topic reference

What every retained `<root>/<uuid>/...` topic contains and how to read it.

## `<root>/<uuid>/status`

Live + retained per device. Three event types:

- **`event="boot"`** — published once per boot. Carries:
  - `firmware_version`, `firmware_ts` — what's running
  - `boot_reason` — see [ABNORMAL_REBOOTS.md](ABNORMAL_REBOOTS.md)
  - `restart_cause` — only present after a `software` boot, identifies the trigger (`cmd_restart`, `cred_rotate`, `mqtt_unrecoverable`, `ota_reboot`)
  - `last_restart_reasons` — array of the last 8 restart causes, ring-buffered. A streak of `mqtt_unrecoverable` means the device cycled through the cascade-recovery path
  - `mac`, `node_name`, `wifi_channel`, `mqtt_disconnects`, heap fields
- **`event="heartbeat"`** — published every 60 s. Carries `uptime_s`, `heap_free`, `heap_largest`, `mqtt_disconnects`, `wifi_channel`, capability flags (`rfid_enabled`, `relay_enabled`, `hall_enabled`).
- **`event="offline"`** — LWT, retained. Set by broker when the device's MQTT session drops without a clean disconnect. Cleared by the next live publish.

## `<root>/<uuid>/diag/coredump`

Retained, fired ONCE on the first boot after a panic / WDT bite (since
v0.4.17 / #65). Cleared from device-side NVS after publish, but stays
retained on the broker until overwritten.

```json
{
  "event": "coredump",
  "exc_task": "loopTask",
  "exc_pc": "0x4008a9f2",
  "exc_cause": "LoadProhibited",
  "core_dump_version": 258,
  "app_sha_prefix": "0ae8bff8",
  "backtrace": ["0x4008a9f2", "0x400e46b5", ...]
}
```

- `exc_task` — name of the FreeRTOS task that crashed. Common: `loopTask`, `async_tcp`, `IDLE1`.
- `exc_pc` — program counter at the fault.
- `exc_cause` — ESP-IDF exception name (`InstFetchProhibited`, `LoadProhibited`, `IllegalInstruction`, `StoreProhibited`).
- `app_sha_prefix` — first 8 chars of the firmware build SHA. Use this to find the matching ELF for symbolication.
- `backtrace` — array of PC values, deepest first. Decode via `xtensa-esp32-elf-addr2line` against the matching ELF — see [../COREDUMP_DECODE.md](../COREDUMP_DECODE.md).

## `<root>/<uuid>/espnow`

Live + retained. Fires every 2 s when ESP-NOW ranging is enabled.
Carries the per-peer table:

```json
{
  "node_name": "ESP32-Alpha",
  "peer_count": 3,
  "cal_entries": 1,                    // (#41.7) per-peer cal coverage
  "cal_points_buffered": 0,            // (#89, v0.4.29) in-flight buffer
  "ranging_enabled": true,             // (#88, v0.4.29) helps diagnose silent-off
  "peers": [
    {"mac": "AA:BB:..", "rssi": -56, "rssi_ema": -55, "dist_m": "0.8", "calibrated": true, "rejects": 0},
    ...
  ]
}
```

## `<root>/<uuid>/response`

Non-retained. Used for command-response pairs:
- `cmd/ota_check` → response contains stage progression
- `cmd/espnow/calibrate` → progress + commit/done events including the new
  `"calib":"waiting"` 1 Hz heartbeat (v0.4.29 / #87)
- `cmd/locate` → ack
- `cmd/rfid/program` → write result

## `<root>/<uuid>/telemetry/rfid`

Non-retained. Fires when an RFID card is read (in normal scanning mode,
not during `cmd/rfid/program` arm).

## `<root>/health/daily`

Retained. Published by `tools/daily_health_check.py` after each daily
run. Aggregate fleet health summary.

## How to drain stale retained payloads

```bash
mosquitto_pub -h 192.168.10.30 -r -t '<full_topic>' -m ''
```

(empty payload + retain flag clears the topic on the broker.) Useful
when a stale `event=offline` LWT lingers after a recovered device has
gone back online but Node-RED doesn't pick up the change.

## Stub status

Add new diagnostic topics here as they ship. Last sweep: 2026-04-29 PM
after v0.4.31.
