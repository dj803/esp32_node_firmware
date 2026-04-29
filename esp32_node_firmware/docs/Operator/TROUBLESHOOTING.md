# Troubleshooting — symptom → action

Quick lookup for the most common operator-facing problems. Cross-references
deeper docs where the underlying cause is documented.

## Device shows offline in Node-RED but heartbeat LED is blinking

Means: firmware is running (loop tick alive) but MQTT isn't connected.
Most likely WiFi recovery is mid-backoff.

1. Check the **onboard LED pattern** — see [LED_REFERENCE.md](LED_REFERENCE.md).
   - Fast blink (5 Hz) → WiFi-stuck. Probably backoff or SSID mismatch.
   - Medium blink (1 Hz) → WiFi up, MQTT down. Broker reachability or
     credentials issue.
   - Double-blink-pause → AP-mode portal active. Operator action needed.
2. Check broker log: `Get-Content C:\ProgramData\mosquitto\mosquitto.log -Tail 60` (elevated PS, or after
   running `tools/Grant Mosquitto Log Read.bat` once).
3. If 16+ min after a router blip on v0.4.30+, run `/fleet-status` and
   power-cycle any devices still silent. v0.4.31+ adds the SSID-probe
   short-circuit so this should be rare.

## Device rebooted with `boot_reason=panic`

See [ABNORMAL_REBOOTS.md](ABNORMAL_REBOOTS.md) for the full triage table.
Quick path:

1. Subscribe to `<root>/<uuid>/diag/coredump` — retained payload has
   `exc_task`, `exc_pc`, `exc_cause`, backtrace.
2. If you can match the firmware version to its CI ELF, decode via
   [../COREDUMP_DECODE.md](../COREDUMP_DECODE.md).
3. If repeating: file a SUGGESTED_IMPROVEMENTS entry with the coredump
   payload + `app_sha_prefix`.

## Fleet OTA-rollout fails partway

`tools/dev/ota-rollout.sh` exits 2 on first abnormal/timeout. To resume
on the remaining devices:

```bash
FLEET_UUIDS="<remaining_uuid_1> <remaining_uuid_2> ..." \
  tools/dev/ota-rollout.sh <target_version>
```

If the failure was a timeout (`✗ TIMEOUT after Ns`):
- Verify the device is healthy before retrying — `/fleet-status` should
  show a recent heartbeat.
- The adaptive timeout may have squeezed the per-device deadline too
  tight. Override: `TIMEOUT_INITIAL_S=600 ADAPTIVE_FACTOR=3 tools/dev/ota-rollout.sh ...`

If the failure was abnormal-boot (`✗ ABNORMAL panic v...`):
- Capture the coredump first, then decide whether to retry or halt.
  Don't blindly resume without root-causing.

## OTA fires but device gets stuck "downloading"

Symptoms: `event=ota_downloading` published but no follow-up
`ota_success` / `ota_failed` for >2 min.

1. `OTA_PROGRESS_TIMEOUT_MS` (30 s default) should restart the device
   on stall. If it didn't fire, check `_otaProgressWatchdog` is armed
   (it should be, but worth a sanity check in serial).
2. Heap may have been too low at trigger time — pre-flight gate
   (v0.3.33+) usually catches this and aborts cleanly with stage:`preflight`.
3. v0.4.29+ has the cascade-recovery gate (#97) — if the device just
   reconnected, it skips OTA for 5 min. Look for `stage:"cascade_quiet"`
   in OTA_FAILED publishes.

## RFID stops reading cards

1. Check `rfid_enabled:true` in the heartbeat — if false, RFID was
   compiled out (a `_minimal` variant).
2. Check the RC522 antenna distance to the WROOM antenna — see
   [INSTALL_GUIDE.md](INSTALL_GUIDE.md). <5 cm causes detuning per #41.
3. If recently power-cycled, RFID may not have re-armed. Try a
   `cmd/restart` to force a clean init.
4. Persistent silence with `rfid_enabled:true` and no RFID errors in
   the log → SPI bus issue or hardware fault. Cross-check with another
   reader.

## Calibration sample collection silent for 30+ s

v0.4.29+ publishes a 1 Hz `"calib":"waiting"` heartbeat to `/response`
during sample collection. If THAT is silent too:
- `ranging_enabled` may be false. Check `/espnow` JSON for
  `ranging_enabled` field. Republish `cmd/espnow/ranging "1"` if needed.
- Peer device may be unreachable. Confirm peer is in `/espnow` peer list
  with a recent rssi.
- The `peer_mac` in your `cmd/espnow/calibrate` payload may not match
  any visible peer. Cross-check against `/espnow` peer list.

If `/response` IS publishing `"calib":"waiting"` but `collected` stays at 0:
- Power-cycle the affected device — see #86 (heap-corruption residue
  workaround).

## Whole fleet panics simultaneously

Cascade event (#78 family). Capture state before resetting:

1. Subscribe to `+/diag/coredump` — every device should publish one.
2. Subscribe to `+/status` — `last_restart_reasons` shows the streak.
3. If on v0.4.27 or older: USB-flash up to v0.4.28+ to get the
   cascade-window publish guard.
4. If on v0.4.28+ and a fresh cascade fires anyway: file as a regression
   in SUGGESTED_IMPROVEMENTS, attach all 6 coredump payloads, escalate.

## Node-RED dashboard shows stale data

1. Confirm broker is up: `Test-Path \\.\pipe\mosquitto` or just connect
   with `mosquitto_sub -h 192.168.10.30 -t '#' -W 5`.
2. Confirm the device is publishing live (not just retained):
   `mosquitto_sub -h 192.168.10.30 -t '<full_topic>/status' -R -W 90`
   should show a fresh `event=heartbeat` within 60-90 s.
3. If broker + device are fine, the issue is Node-RED side — restart
   the dashboard flow or re-deploy.

## Stub status

This document covers the v0.4.31 firmware. Add new symptoms here as
they're encountered. Keep entries short — link to deeper docs for
the full investigation history.
