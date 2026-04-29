# Operator manuals

Field-and-bench-facing reference docs for the ESP32 fleet. Engineer-facing
docs (failure modes, internal architecture, root-cause histories) live in
the parent `docs/` folder; these are the day-to-day "what do I do when..."
references.

## When to look here

- Setting up a new device → [INSTALL_GUIDE.md](INSTALL_GUIDE.md) + [AP_MODE_SETUP.md](AP_MODE_SETUP.md)
- Adding hardware to an existing device (NeoPixel matrix, relay, Hall) → [HARDWARE_WIRING.md](HARDWARE_WIRING.md)
- Something looks broken → [TROUBLESHOOTING.md](TROUBLESHOOTING.md)
- A device just rebooted unexpectedly → [ABNORMAL_REBOOTS.md](ABNORMAL_REBOOTS.md)
- Need to publish to / understand an MQTT topic → [MQTT_COMMAND_REFERENCE.md](MQTT_COMMAND_REFERENCE.md) + [DIAG_TOPICS.md](DIAG_TOPICS.md)
- LEDs are showing a pattern, what does it mean? → [LED_REFERENCE.md](LED_REFERENCE.md)
- Driving the WS2812 strip from Node-RED → [LED_COMMANDS.md](LED_COMMANDS.md)
- OTA-rollout, blip, or any daily-driver fleet command → [FLEET_OPS.md](FLEET_OPS.md)
- Cutting a release with a canary → [CANARY_OTA.md](CANARY_OTA.md)
- Setting up monitoring → [MONITORING_PRACTICE.md](MONITORING_PRACTICE.md)

## Index

### Field / bench install
- [INSTALL_GUIDE.md](INSTALL_GUIDE.md) — antenna orientation, RC522 distance, USB cable routing, calibration discipline
- [HARDWARE_WIRING.md](HARDWARE_WIRING.md) — expansion modules (NeoPixel matrix, 2-ch relay, future Hall) wiring + firmware enable
- [AP_MODE_SETUP.md](AP_MODE_SETUP.md) — first-boot bootstrap walkthrough via the captive portal

### Daily ops
- [FLEET_OPS.md](FLEET_OPS.md) — daily-driver one-liners (fleet snapshot, OTA-rollout, blip, broker log)
- [MONITORING_PRACTICE.md](MONITORING_PRACTICE.md) — heartbeat / boot-reason watching, dashboard layers
- [CANARY_OTA.md](CANARY_OTA.md) — canary-OTA pattern for safe fleet upgrades

### When things break
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — symptom → action playbook
- [ABNORMAL_REBOOTS.md](ABNORMAL_REBOOTS.md) — boot-reason triage table

### Reference
- [MQTT_COMMAND_REFERENCE.md](MQTT_COMMAND_REFERENCE.md) — every `cmd/*` topic with payload examples
- [DIAG_TOPICS.md](DIAG_TOPICS.md) — what `/status`, `/diag/coredump`, `/espnow`, `/response` payloads contain
- [LED_REFERENCE.md](LED_REFERENCE.md) — firmware-driven onboard LED + WS2812 patterns (what each pattern means)
- [LED_COMMANDS.md](LED_COMMANDS.md) — operator-driven WS2812 schema (`cmd/led` MQTT API)

## Stub status

Most of these docs are first-pass writes (2026-04-29). They're meant
to exist NOW so future operators have a starting point; expand each
one with field experience as it accumulates. The engineer-facing
parent-folder docs (e.g. `ESP32_FAILURE_MODES.md`,
`COREDUMP_DECODE.md`, `TWDT_POLICY.md`) stay where they are — they're
the deeper reference these operator docs link out to.

## Convention

- Use markdown, not plain text. Headings, tables, and code-blocks
  matter for readability when these are pulled up on the bench.
- Cross-link freely between docs. If a question crosses two doc
  scopes, the right answer is usually a one-line link in the simpler
  one pointing at the deeper one.
- Lead with the symptom or task, not the implementation. The operator
  arrives with a problem; the doc should match the problem statement
  before it explains the firmware-side mechanism.
- Date the bottom of each doc when it gets a meaningful update, so
  the operator can tell at a glance whether the content is fresh.
