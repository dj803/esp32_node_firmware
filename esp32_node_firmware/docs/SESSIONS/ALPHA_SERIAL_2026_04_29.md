# Alpha serial capture — 2026-04-29

Captured 2026-04-29 ~09:12 SAST as a companion to Charlie's canary
serial capture (see `CHARLIE_CANARY_SERIAL_2026_04_29.md`). Operator-
authorised optional capture — "you can also check Alpha if it's useful,
else leave Alpha alone".

## Setup

- **Device:** ESP32-Alpha, UUID `32925666-155a-4a67-bf50-27c1ffa22b11`,
  MAC `84:1F:E8:1A:CC:98`
- **Firmware:** v0.4.26 (production, NOT canary)
- **Hardware:** WS2812 LED rig (8× pixels), bench-attached on COM4 since
  2026-04-28 afternoon swap
- **Pre-capture state:** uptime_s 2220 (37 min since most-recent boot
  after this morning's #92 cascade reboot with `boot_reason: panic`)
- **Capture method:** pyserial 3.5 with no-reset open (DTR=False,
  RTS=False pre-set). Verified non-disruptive: Alpha's MQTT uptime
  climbed 2220 → 2280 across the capture window (60 s elapsed wall-
  clock matches sentinel delta, no reset).
- **Capture window:** 45 s, port COM4, baud 115200

## Result

**0 bytes captured.** Alpha is also silent on serial during steady-state
operation, same as Charlie. Production v0.4.26 firmware does not emit
periodic debug prints; it only prints under abnormal conditions
(panics, fatal errors, dedicated debug-mode builds).

## What this tells us

The "pull serial backlog" workflow yields no diagnostic data UNLESS:
- We catch the device in the act of an abnormal event (panic print,
  watchdog warning, etc.)
- The build is a debug variant with periodic LOG_D output
- The capture is timed to coincide with a known-noisy event (boot,
  WiFi reconnect, MQTT disconnect)

For the next #78 bench-debug session, this means: serial capture
strategy must be **continuous logging running BEFORE the cascade
event fires**. Open the port, start writing to a file, THEN trigger
the AP cycle. Capturing post-event will miss the panic dump.

Aligns with #87 (calibration UX silence) thematically: production
firmware is intentionally quiet, which is good for steady-state but
hostile to forensics. Future canary builds would benefit from a
periodic-print task that logs `uxTaskGetStackHighWaterMark()` per
task + heap free + heap-largest every 60 s — useful surveillance
data without flooding the channel.

## What this does NOT tell us

- The pre-cascade context for Alpha's morning `boot_reason: panic`
  — that data was in the serial buffer pre-reboot but is gone
- Whether the LED rig has been current-spiking (would be visible as
  brownout boot_reason in MQTT, not serial; we already track this)
- Anything about Alpha's calibration state (no `peer_cal_table`
  entry; cal_entries=0 in /espnow)

## Operational note

Alpha can be left alone. Its serial silence is normal. If a future
investigation needs Alpha's serial output, the strategy is to start
the capture BEFORE inducing the event (e.g. before kicking the AP
for the #78 reproduction recipe).
