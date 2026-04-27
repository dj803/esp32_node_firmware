# Roadmap

Forward plan synthesized from [SUGGESTED_IMPROVEMENTS.txt](SUGGESTED_IMPROVEMENTS.txt), [ESP32_FAILURE_MODES.md](ESP32_FAILURE_MODES.md), [memory_budget.md](memory_budget.md), [TOOLING_INTEGRATION_PLAN.md](TOOLING_INTEGRATION_PLAN.md), [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md), and the per-version plans in `~/.claude/plans/`.

Last updated: 2026-04-27 (v0.4.12 fleet OTA confirmed complete; Tier 1 tooling session).

---

## Now (just shipped or in flight)

### v0.4.11 — DONE
Heap-guard fix in `mqttPublish()` (#51 root cause). Plus bundled: BLE off, NDEF feature, #48/#49 visibility logs, heap heartbeat, tools/ directory, CLAUDE.md, .claude/settings.json.

### v0.4.12 — DONE
Cosmetic re-tag of v0.4.11 for OTA-path validation. Same code, version string bumped so `-dev` devices accept the OTA. Fleet-wide stagger complete.

---

## Next (week of 2026-04-28)

### v0.4.13 — BLE coexistence + #61 reconnect-boot cleanup
Plan at [`~/.claude/plans/v0.4.13-plan.md`](C:\Users\drowa\.claude\plans\v0.4.13-plan.md). Two scopes:

**A) BLE silent deadlock real fix.** With-BLE devices hang ~70 min after a network reconnect. RTC WDT doesn't bite; chip stays powered, FreeRTOS scheduler hangs. Today's workaround = BLE off in config.h. Real fix needs NimBLE 2.x + WiFi + ESP-NOW coexistence audit. Investigation steps:
- Synthetic broker-blip every 30 min on a single bench device with BLE on + LOG_LEVEL_DEBUG.
- Audit IDF coexist scheduler config, NimBLE deinit completeness, scan duty cycle.
- Possible mitigations: pin nimble_host to Core 1, lower BLE scan duty, BLE-watchdog loop in main.

**B) #61 misleading `event=boot` on every MQTT reconnect.** Add `FwEvent::ONLINE`, track first-boot vs reconnect via static bool. Update boot_history flow context filter.

If (A) inconclusive after a day, ship (B) standalone, defer (A) to v0.4.15.

### Tier 1 tooling (cross-tool integration plan) — executing 2026-04-27
- **T1.1** Node-RED file logging in `settings.js` — eliminates the "log frozen at 20:30" failure mode we hit 2026-04-26.
- **T1.2** Mosquitto log rotation — log is 87 MB and growing ~30 MB/day.
- **T1.3** `.dummy/` cleanup → `tools/node_red/` — partially done (tools/ created v0.4.11); completing this session.
- **T1.4** Run `/less-permission-prompts` periodically as the session transcript grows.
- **T1.5** `tools/flash_dev.sh` + `tasks.json` shipped in v0.4.11; verify step done this session.
- **T2.7** `tools/fleet_status.sh` already exists; promote + document this session.
- **T2.1** GitHub Pages probe in `/daily-health` — add `gh api .../pages` check this session.

### Bench-isolate Charlie (#46)
Charlie's chronic flake (136 mosquitto disconnects in 2 days, brownout 11:50, persistent across firmware versions) wasn't fixed by anything in this release. 12 h bench soak alone (separate desk, separate USB, no ESP-NOW peers) to establish whether it's hardware or environmental.

---

## Month (v0.4.14, v0.4.15)

### v0.4.14 — re-introduce MQTT_HEALTHY green LED (#56) + Bravo back online
Plan at [`~/.claude/plans/v0.4.14-plan.md`](C:\Users\drowa\.claude\plans\v0.4.14-plan.md). Use the deferred-flag pattern: callback sets `_mqttLedHealthyAtMs = millis()`, `loop()` consumes and posts the ws2812 event from a non-TWDT-subscribed context. Document the pattern in TWDT_POLICY.md as canonical "callback wants long work".

**Bravo reintroduction.** Bravo has been powered off since 2026-04-26 ~10:50 as the cascade-trigger suspect. Now that #51 root cause is identified as bad_alloc (not Bravo's transmit behavior), Bravo can rejoin the fleet. v0.4.14 is the natural moment — power Bravo on, OTA-update or USB-flash to v0.4.14. Watch for whether the chronic-flake pattern (#46, 136 disconnects in 2 days) recurs; if it does, Bravo is hardware-side and a candidate for the bench-supply rig (#59).

### v0.4.15+ — observability accretion
Bundle in any minor:
- Per-heartbeat `LOG_HEAP` (#53) — already partially done, surface as Node-RED dashboard tile.
- Silent-failure dashboard tile (#60) — already have `tools/silent_watcher.sh`; promote to Node-RED Vue tile for at-a-glance.
- Stack canary build (#54) — `[env:esp32dev_canary]` in platformio.ini for one-device soak.
- AsyncMqttClient malformed-packet counter (#55) — surface the rare 2-events-in-2-days signal.
- Reproducible builds (#3) — replace `FIRMWARE_BUILD_TIMESTAMP` with `SOURCE_DATE_EPOCH`.

### Phase 3 RFID/NFC bench-tests (Foxtrot only)
- #11 NTAG215 write — already shipped via existing `cmd/rfid/program`; just bench-verify (5 min).
- #15 NDEF read decode — currently we WRITE NDEF; READ-decode would let smartphone-written tags surface as parsed records to MQTT. ~150 lines of inline decoder.

---

## v0.5.0 — relay + Hall hardware

[PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md) carries the design. Key dependencies before it ships:
- **#48 UUID drift fix.** A fleet that loses identities on reboot can't gain more devices without doubling identity-management complexity.
- **#58 per-device firmware variants.** Relay/Hall devices have a different role from sensor-only devices. Variants enable smaller binaries per role, lower attack surface, easier diagnosis (today's BLE-off A/B test would be CI-built rather than hand-flagged).

### #58 per-device variants — design summary
Add `[env:esp32dev_full / rfid / ranging / minimal]` to platformio.ini; CI builds N binaries; `ota.json` schema gains `Variant` keyed entry; firmware bakes `BUILD_VARIANT` define and selects matching manifest entry. Detailed spec in SUGGESTED_IMPROVEMENTS #58.

### AsyncMqttClient replacement
v0.4.11's heap-guard is a workaround. The deeper fix is to replace AsyncMqttClient with a static-buffer-based MQTT client (e.g. PubSubClient with custom backpressure) that doesn't `new` per publish. Larger refactor; appropriate alongside v0.5.0 hardware introduction.

---

## v0.6.0+ — security + capability accretion

| Item | Source | Notes |
|---|---|---|
| NTAG424 DNA secure messaging | #12 | Hardware on hand; needs flash slim or 8 MB module |
| Bootloader rollback | #25 | Blocked on pioarduino rebuild upstream |
| Recovery partition | #26 | Needs ~150 KB binary slim or 8 MB flash |
| MQTT-over-TLS | #7 | Trusted LAN today; revisit if deployment moves |
| OTA CA pinning | #6 | Internal-IoT threat model accepts; revisit if origin moves |
| SBOM + Trivy + Dependabot | #4 | Low operational risk currently |
| JTAG debugging | #2 | When a hardware fault is hard to repro via serial |

---

## Open #51 follow-ups (not blockers, tracked)

| Follow-up | Where | When |
|---|---|---|
| BLE silent-deadlock real fix | v0.4.13 (A) | Next |
| Re-introduce MQTT_HEALTHY safely | v0.4.14 | Next-next |
| AsyncMqttClient replacement | v0.5.0 (deeper fix) | Bigger lift |
| Mass-flash Charlie/Echo/Foxtrot to v0.4.12 | OTA complete | DONE |
| Bench-isolate Charlie | Operational | Anytime |

---

## Tooling cross-cutting (TOOLING_INTEGRATION_PLAN.md)

Tier-1 (≤ 1h each — executing 2026-04-27):
- Node-RED file logging ← doing this session
- Mosquitto log rotation ← doing this session
- `.dummy/` cleanup → `tools/node_red/` ← doing this session
- `/less-permission-prompts` ← doing this session
- Verify `flash_dev.sh` / `tasks.json` ← doing this session

Tier-2 quick wins (pulling forward to this session):
- `tools/fleet_status.sh` promote + document
- GitHub Pages probe in `/daily-health`

Tier-2 (≤ 1d each, this month — deferred):
- Heap-trajectory dashboard tile (depends on heap heartbeat heartbeat; do after v0.4.13)
- Branch protection
- Mosquitto WebSocket listener
- Auto-tag on version commit

Tier-3 (only if a need surfaces):
- Mosquitto auth + TLS
- Node-RED HTTPS admin + auth
- Reproducible builds
- Hardware lab discipline (cable inventory, bench-isolation rig)

---

## How to extend this roadmap

When a new release is cut, append a section under "Now" with what shipped and move open items downward. Keep the v0.5.0 / v0.6.0+ sections as the "what's beyond" buffer. SUGGESTED_IMPROVEMENTS.txt remains the long-tail backlog; this doc is the synthesized "what we're actually doing soon" view.
