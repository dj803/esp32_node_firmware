# Roadmap

Forward plan synthesized from [SUGGESTED_IMPROVEMENTS.txt](SUGGESTED_IMPROVEMENTS.txt), [ESP32_FAILURE_MODES.md](ESP32_FAILURE_MODES.md), [memory_budget.md](memory_budget.md), [TOOLING_INTEGRATION_PLAN.md](TOOLING_INTEGRATION_PLAN.md), [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md), and the per-version plans in `~/.claude/plans/`.

Last updated: 2026-04-27 (v0.4.13 fleet-wide OTA + USB validation complete; #44 deferred-flag verified on hardware).

---

## Now (just shipped or in flight)

### v0.4.11 — DONE
Heap-guard fix in `mqttPublish()` (#51 root cause). Plus bundled: BLE off, NDEF feature, #48/#49 visibility logs, heap heartbeat, tools/ directory, CLAUDE.md, .claude/settings.json.

### v0.4.12 — DONE
Cosmetic re-tag of v0.4.11 for OTA-path validation.

### v0.4.14 — DONE (shipped 2026-04-27 afternoon — CASCADE ROOT-CAUSE FIX)
- **`MQTT_HUNG_TIMEOUT_MS = 12000` was the cascade trigger.** Shorter than lwIP's TCP SYN timeout (~75 s), so any broker outage > 12 s caused every device to ESP.restart() simultaneously, producing the v0.4.10 #51, 10:42, and 14:04 cascades. The AsyncTCP `tcp_arg` panic captured at 14:04 was a SECONDARY effect of the restart-storm, not the trigger.
- **Bumped `MQTT_HUNG_TIMEOUT_MS` to 90000** (90 s). Gives lwIP SYN room to fail naturally before the firmware aborts.
- **Bonus: switched AsyncTCP fork from `me-no-dev` → `mathieucarbou/AsyncTCP v3.3.2`.** Doesn't fix the cascade (timeout did) but downgrades remaining int_wdt-class hangs from `panic` (memory corruption) to recoverable watchdog reset.
- **Validated 15:36:52 SAST:** definitive M2 30 s broker blip on the entire fleet (6/6 on v0.4.14) — all 6 devices reconnected within 1 second of broker return, **zero panics, zero abnormal boots**. The same blip on v0.4.13 fleet panicked 5/6 devices.
- Tooling shipped: `tools/blip-watcher.ps1` + `Start Blip Watcher.bat` (file-trigger watcher), `docs/CHAOS_TESTING.md` (full chaos plan + framework proposal).

### v0.4.13 — DONE (shipped 2026-04-27 morning)
- **#61 ONLINE event on reconnect** — `FwEvent::ONLINE` distinguishes true boot from broker reconnect. Eliminates misleading boot flood in Node-RED `boot_history`.
- **#44 MQTT_HEALTHY green LED via deferred-flag** — `_mqttLedHealthyAtMs` set in `onMqttConnect()` (async_tcp task), consumed in `mqttHeartbeat()` (loop task). Avoids the v0.4.10 TWDT crash shape. Pattern documented as canonical in [TWDT_POLICY.md](TWDT_POLICY.md).
- **Hardware verification:** 6/6 fleet on v0.4.13 (Charlie on -dev via USB, 5/6 via OTA). 14 min uptime on Alpha post-OTA, 13 min on Charlie under WS2812 + ESP-NOW load — zero crashes, no `boot_reason=panic|task_wdt|int_wdt`.
- **Visual confirmation (2026-04-27):** green MQTT_HEALTHY breathing observed on Alpha's WS2812 strip. Deferred-flag pattern works in production end-to-end.
- Bravo back online (had been powered off since 2026-04-26 as #51 cascade-trigger suspect; #51 root cause now confirmed elsewhere).

### Tier 1 tooling — DONE (2026-04-27)
T1.1 Node-RED file logging, T1.2 Mosquitto log rotation (PowerShell scheduled task), T1.3 `.dummy/` → `tools/node_red/`, T1.4 `/less-permission-prompts`, T1.5 verify `flash_dev.sh`+`tasks.json`. Plus pulled-forward T2.1 GitHub Pages probe in `/daily-health` and T2.7 `tools/fleet_status.sh` documented.

---

## Next (week of 2026-04-28)

### v0.4.14 — Tier-2 quick wins bundle (Path B from 2026-04-27 plan)
No firmware changes. Tooling + repo hygiene:
- **Heap-trajectory Node-RED Dashboard 2.0 tile** — Vue template plotting rolling 24h heap_free per device (data already in heartbeats since v0.4.11). Pairs with existing `boot_history` tile.
- **T2.3 GitHub branch protection on `master`** — require status checks (CI green) before merge; keep direct push allowed for solo-dev workflow.
- **T2.6 Auto-tag GitHub Action** — on push-to-master, parse `config.h::FIRMWARE_VERSION`; if non-`-dev` and tag absent, auto-create + push tag. Eliminates "I forgot to tag" risk seen with prior releases.
- **T2.4 Mosquitto WebSocket listener** — 2 lines in `mosquitto.conf` (`listener 9001 / protocol websockets`). Lets browser MQTT tools connect when Node-RED is mid-debug.

### Bench-isolate Charlie (#46)
Charlie's chronic flake. 12 h bench soak (separate desk, separate USB, no ESP-NOW peers) to establish hardware vs environmental. Independent of firmware versions.

### v0.4.13 stability soak
24h `silent_watcher.sh` + `boot_history` poller. Watch for any post-OTA crash in #51-style failure modes. Charlie on `-dev` is the primary watch target — re-flash to release via OTA after soak.

### v0.4.15 — BLE coexistence real fix (Path C)
The deeper fix for the BLE silent-deadlock workaround currently shipped as `BLE_ENABLED 0` in `config.h`. Plan at [`~/.claude/plans/v0.4.15-plan.md`](C:\Users\drowa\.claude\plans\v0.4.15-plan.md) (to be created when Phase 1 begins).

**Failure shape recap.** With BLE enabled, devices hang ~70 min after a Wi-Fi/broker reconnect cycle. RTC WDT does not bite; chip stays powered; FreeRTOS scheduler hangs. Symptom: LWT-only on broker, no heartbeats, no panic.

#### Phase 1 — bench investigation (can start immediately, no soak gate)
Touches a single off-fleet device on a separate desk. Fleet keeps running v0.4.13 unaffected.

1. **Bench rig.** Single bench device with `BLE_ENABLED 1`, `LOG_LEVEL_DEBUG`, no ESP-NOW peers. Candidate: **Bravo** (no LEDs currently — fine for headless bench; also the natural soak target since it was the former #51 cascade-trigger suspect) **or a spare ESP32**. **Not Alpha** — Alpha currently carries the WS2812 strip and is providing the visual MQTT_HEALTHY validation for v0.4.13.
2. **Synthetic broker-blip.** PowerShell loop on the host stops + restarts the `mosquitto` service every 30 min for 24 h. Forces the reconnect-then-deadlock window.
3. **Coexistence audit (parallel work).**
   - IDF `CONFIG_ESP_COEX_*` settings vs current sdkconfig — check `BTDM_CTRL_COEX_*` knobs.
   - NimBLE `nimble_port_deinit()` completeness on shutdown paths.
   - BLE scan duty cycle vs Wi-Fi DTIM beacon overlap.
4. **Mitigation candidates** (try one at a time, leave the bench rig running 24 h between each):
   - Pin `nimble_host` task to Core 1 (currently floating).
   - Lower BLE scan duty (`BLE_SCAN_INTERVAL_MS` ↑, `BLE_SCAN_WINDOW_MS` ↓).
   - Add a BLE-watchdog loop in main: if `nimble_host` task hasn't ticked in N minutes, deinit + reinit.
   - Drop NimBLE entirely → switch to esp32-arduino's built-in BLE stack (heavier RAM, simpler coexistence model).
5. **Phase 1 acceptance criterion.** 24 h bench soak with BLE on + 30-min broker blips + zero silent deadlocks.

#### Phase 2 — fleet rollout (gated on v0.4.13 24h soak + Phase 1 acceptance)
Soak gate matters here, not in Phase 1: if v0.4.13 has a latent post-OTA issue and we ship v0.4.15 with BLE on top, a fleet-wide failure is hard to attribute — was it BLE coexistence or a v0.4.13 regression? Soak isolates the baseline.

**Prereqs:**
- v0.4.13 24 h soak across the fleet — zero `boot_reason=panic|task_wdt|int_wdt|brownout`, no LWT-only stalls.
- Phase 1 acceptance criterion met (24 h bench soak with BLE on, zero silent deadlocks).

**Rollout:** Fleet OTA stagger same shape as v0.4.13 (2-min intervals). Watch heap_free trajectory closely — re-enabled BLE adds ~50 KB heap pressure.

**Risks.**
- Coexistence bugs are notoriously timing-dependent — fix on bench may not reproduce in fleet.
- Re-enabling BLE adds ~50 KB heap pressure; verify `mqttPublish()` heap-guard threshold (4096) is still adequate post-fix.
- If we end up swapping NimBLE for arduino-bt, that pulls in another ~80 KB flash and changes the BLE scanner API surface — significant refactor of `ble.h`.

**Defer if:** v0.4.14 surfaces any post-OTA instability; Charlie's chronic flake (#46) turns out to be hardware. Either consumes the cycle budget Path C needs.

---

## Month — observability accretion (v0.4.16+)

Note: v0.4.14 (Path B Tier-2 wins) and v0.4.15 (Path C BLE) are both in the "Next" section above. The v0.4.14 MQTT_HEALTHY scope and Bravo reintroduction shipped early in v0.4.13.

### v0.4.16+ — observability accretion
Bundle in any minor:
- Heap-trajectory dashboard tile — split out into v0.4.14 explicitly above.
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
| BLE silent-deadlock real fix | v0.4.15 (Path C) | After v0.4.14 |
| Re-introduce MQTT_HEALTHY safely | v0.4.13 (C) | DONE — deferred-flag pattern |
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
