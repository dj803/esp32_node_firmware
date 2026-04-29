# Abnormal reboots — operator triage

A device can reboot for many reasons. The firmware reports each via the
retained `<root>/<uuid>/status` boot announcement's `boot_reason` field.
This is the operator-facing triage table; for full detail see
[../ESP32_FAILURE_MODES.md](../ESP32_FAILURE_MODES.md) (engineer-facing).

## Boot-reason taxonomy

| `boot_reason` | What it means | Action |
|---|---|---|
| `poweron` | Vcc went 0 → 3.3 V (USB plug, mains return). | Normal — expected after a power cycle. |
| `external` | EN button or RTS pin reset. | Normal — expected during USB-flash. |
| `software` | Firmware called `esp_restart()` deliberately. | Read `restart_cause` field for the trigger (`cmd_restart`, `cred_rotate`, `mqtt_unrecoverable`, `ota_reboot`). Each is benign. |
| `panic` | Unhandled exception (null pointer, stack corruption, divide-by-zero). | **Abnormal.** Check `<root>/<uuid>/diag/coredump` for the backtrace. See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) and [DIAG_TOPICS.md](DIAG_TOPICS.md). |
| `task_wdt` | A subscribed task didn't feed the watchdog within 12 s. | **Abnormal.** Usually a blocked I/O loop or runaway flash write. Pair with coredump if one fires. |
| `int_wdt` | An interrupt handler ran too long (>300 ms). | **Abnormal.** Usually third-party library bug or driver path stall. Rare. |
| `other_wdt` | RTC watchdog or brownout-related. | **Abnormal.** Check power supply (#1 cause) before suspecting firmware. |
| `brownout` | Vcc dipped below ~2.43 V. | **Abnormal.** Power-supply issue: USB cable, weak charger, big peak draw. Not firmware. |
| `deepsleep` | Wake from `cmd/deep_sleep`. | Normal — operator-initiated. |

## What to check first

1. **`/fleet-status`** — see all 6 devices' last `boot_reason` and uptime
   in one snapshot.
2. **`<root>/<uuid>/diag/coredump`** — retained payload. If present, a
   panic (or forced WDT) fired since last boot. See [DIAG_TOPICS.md](DIAG_TOPICS.md)
   for how to read it.
3. **`last_restart_reasons`** field in the boot announcement — last 8
   restart causes. A streak of `mqtt_unrecoverable` means the device is
   in the cascade-recovery path; v0.4.28+ self-clears via
   `ap_recovered`.

## Common patterns

- **One-off panic, no recurrence** — usually a one-bit cosmic-ray flip
  or an over-stretched WiFi reconnect cycle. Note it, leave the device
  running, watch for repeats.
- **Repeating panic with same `exc_pc` / `exc_task`** — actionable bug.
  Pair the coredump with the running firmware's ELF (`COREDUMP_DECODE.md`
  in `docs/`) to find the source line.
- **Brownout cluster on one device** — almost always power-supply.
  Check the USB cable, the charger, and any bulk capacitor on the LED
  strip rail. Alpha v0.4.26 brownouts were traced to LED inrush
  current.
- **Fleet-wide simultaneous panic** — cascade event. v0.4.28+ has the
  cascade-window publish guard; v0.4.31 has SSID probe + auto-OTA-
  during-recovery gate. If you see a fresh fleet-wide cascade
  post-v0.4.31, capture all coredumps and file as a regression.

## When to escalate

- Same panic `exc_pc` fires on the same device 3+ times within an hour.
- 2+ devices fail simultaneously with non-power-related boot reasons.
- A new `boot_reason` value you haven't seen before (means the firmware
  taxonomy may have grown — re-check this doc).

## Related docs

- [../ESP32_FAILURE_MODES.md](../ESP32_FAILURE_MODES.md) — engineer-facing catalogue with per-mode root-cause and fix history
- [../TWDT_POLICY.md](../TWDT_POLICY.md) — task-watchdog subscription model
- [../COREDUMP_DECODE.md](../COREDUMP_DECODE.md) — how to symbolicate a coredump
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — symptom → action playbook
- [DIAG_TOPICS.md](DIAG_TOPICS.md) — what `/diag/coredump`, `/status`, `last_restart_reasons` payloads contain
