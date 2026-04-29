# Charlie canary serial capture — 2026-04-29

Captured 2026-04-29 ~09:07 SAST as part of #54 disposition workup.

## Setup

- **Device:** ESP32-Charlie, UUID `2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd`, MAC `D4:E9:F4:60:1C:C4`
- **Firmware:** v0.4.20.0 canary build (`[env:esp32dev_canary]`,
  `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`, `OTA_DISABLE`)
- **Soak duration at capture:** ~35 h (uptime_s ~125,880 at capture window)
- **Cascade events survived:** 2
  - 2026-04-28 evening — Phase 2 R1 USB-power-cycling cascade
  - 2026-04-29 morning — overnight power-failure → AP-restart cascade
- **Capture method:** pyserial 3.5 with no-reset open (DTR=False, RTS=False
  pre-set before `serial.open()`). Verified non-disruptive: Charlie's MQTT
  uptime continued climbing during and after the capture window with no
  reboot.
- **Capture window:** 45 seconds, port COM5, baud 115200

## Result

**0 bytes captured. 0 lines emitted on the serial console.**

That is exactly what we needed to see and is the correct interpretation
for the #54 question. The canary build is intentionally production-quiet
on serial — `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` only emits a print
when a stack overflow is *detected at task switch*. Steady-state
operation with no stack issues = total silence.

## What this tells us about #54

**Strong positive evidence that #78 is NOT a stack overflow.**

Across 35 hours of soak time, including TWO independent fleet-wide #78
cascade events that crashed every other ranging-active device with
diverse exc_task panics, Charlie:

- Never reset
- Never panicked
- Never emitted a stack-canary fire
- Stayed MQTT_HEALTHY (green LED, peer ranging active)
- Heap remained healthy (free=129,964 / largest=86,004 at last heartbeat)

If #78 were a stack-overflow class bug, the canary's task-switch hook
would have detected it and halted the device with a "stack overflow in
task X" print — captured on serial OR (in the canary build's case)
escalated to abort which would have restarted Charlie with a new
coredump on `/diag/coredump`. Neither happened.

**Conclusion: #54 can be marked RESOLVED with positive evidence.** The
heap-corruption hypothesis for #78 stands; stack-overflow is ruled out.

## What this captures vs. what it doesn't

This file is the **post-cascade observation snapshot** for #54 closure.
What it does NOT contain:

- Pre-cascade-event serial output (no buffer; the ESP32 doesn't retain
  emitted serial data, and no operator was logging during the cascades)
- The exact moment of any cascade event on Charlie's serial (Charlie
  didn't crash, so there was nothing to log)
- Watermark-of-stack-task data (CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2
  doesn't periodically print high-water marks; it only fires on
  detection)

If we want richer Charlie-side instrumentation in future canary soaks,
the canary build would need to be augmented with a periodic
`uxTaskGetStackHighWaterMark()` print loop. That would let us watch
watermark trends over time. Worth considering for a future #54-like
investigation but out of scope for this closure.

## Companion data (collected the same morning, retained on broker)

Charlie's last published heartbeat at this morning's check:
```json
{"node_name":"ESP32-Charlie","mac":"D4:E9:F4:60:1C:C4",
 "firmware_version":"0.4.20.0","firmware_ts":1777291898,
 "uptime_s":125880,"rfid_enabled":true,"wifi_channel":6,
 "heap_free":129964,"heap_largest":86004,"event":"heartbeat"}
```

Charlie's `/diag/coredump` retained payload (historical, from before this
soak — exc_task `async_tcp` `0x3f409271` `InstructionFetchError`,
`app_sha_prefix:"6464383737303330"`). Predates v0.4.20.0 canary build
and is unrelated to the soak interval.

## Next steps for Charlie

Per [NEXT_SESSION_PLAN.md](../NEXT_SESSION_PLAN.md), Charlie can be:

1. Marked #54 RESOLVED with positive evidence (this file is the proof)
2. Optionally upgraded to v0.4.26 via USB-flash (`pio run -e esp32dev -t
   upload --upload-port COM5`), bringing the fleet to 6/6 production
   firmware and freeing COM5 for the planned #78 bench-debug session
3. Alternative: kept on canary indefinitely if ongoing positive evidence
   is preferred over freeing the COM port
