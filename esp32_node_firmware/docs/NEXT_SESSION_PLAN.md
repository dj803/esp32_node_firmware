# Next session plan

Drafted 2026-04-28 afternoon (autonomous followups while waiting for relay
hardware). Goal of session was to chip away at OPEN items; outcome:

- **#76 sub-C/D/I** code-shipped to master (firmware: time-based MQTT
  unrecoverable trigger + restart-loop AP fallback + /daily-health
  WDT/SW categorisation).
- **#75 chaos framework** code-shipped to `tools/chaos/` (scripts +
  runner.sh + README + JSON reports under `~/daily-health/`).
- **#24, #28 — audit-stale** entries moved to RESOLVED.
- **v0.4.24 NOT TAGGED** — fleet went LWT-offline mid-session; cannot
  validate new restart-policy paths until fleet returns.

Open: 47 → 38. Resolved: 36 → 38. WONT_DO unchanged at 8.

## State at end of session (2026-04-28 afternoon)

| | |
|---|---|
| Master HEAD | `firmware(#76): time-based MQTT escalation + restart-loop AP fallback` (8031d0b) → `tools(#75): promote chaos scenarios to tools/chaos/ + runner.sh` (e0b6cb3) plus end-of-session doc sweep |
| Fleet | **OFFLINE** — every device LWT-offline. Broker healthy. Operator-side issue (AP suspected). See SESSION_QUESTIONS_2026_04_28 |
| Charlie soak | Pre-session: ~12.4 h sticky on v0.4.20.0 canary. Tripped silently — `/diag/coredump` shows another #78 async_tcp InstructionFetchError. Cleanly **ruled out stack overflow** for this crash family — canary build would have halted at violation. |
| Backlog | OPEN 38, RESOLVED 38, WONT_DO 8 |
| Pending releases | **v0.4.24** code-only on master, awaits fleet + tag |

## Recommended next session — fleet recovery + v0.4.24 cut + relay hardware

Three menu items in priority order:

### A. Fleet recovery + v0.4.24 release (~30 min — critical path)

1. **Operator triage** the fleet outage. Most likely path:
   - Power-cycle the AP if uncertain.
   - Run `/daily-health` to confirm devices reassociated.
   - Check `mosquitto.log` for any unusual disconnect pattern around
     ~10:50 SAST 2026-04-28.

2. Once 5/6 fleet (or all 6 if Charlie's been swapped to release) shows
   healthy heartbeats, **cut v0.4.24** via the existing release pipeline:
   - bump version → tag → push → CI builds → OTA manifest update →
     fleet OTA stagger → validation.
   - Validation = M1 + M2 chaos via the new `tools/chaos/runner.sh`.
   - Specifically watch for `restart_cause=mqtt_unrecoverable` in any
     boot announcement during M2 — that confirms sub-C is firing on
     time rather than count.

3. If sub-D fires unexpectedly (a device enters AP_MODE on first boot),
   that's a regression — clear the RestartHistory ring via the dev
   path, document the trigger, and roll back v0.4.24.

### B. v0.5.0 Phase 2 hardware bring-up (relay + Hall on Bravo) — ~2 h

Plan unchanged from previous session (PLAN_RELAY_HALL_v0.5.0.md). Bravo
is already wired per the GPIO inventory. Steps:

1. Capture Bravo's pre-flash state (firmware, uptime, boot_reason).
2. USB-flash `esp32dev_relay_hall` to Bravo on COM4.
3. Validate `cmd/relay` end-to-end (click + retained state + NVS
   restore on restart).
4. Validate `cmd/hall/zero` + `cmd/hall/config` + `telemetry/hall` +
   `telemetry/hall/edge`.
5. M1 + M2 chaos to confirm no new failure surface.
6. Tag v0.5.0 if validation lands clean.

This depends on (A) being done — releasing v0.4.24's restart-policy
changes BEFORE introducing new hardware paths gives a cleaner blast
radius if the new hardware tickles a regression.

### C. #78 AsyncTCP race deep dive — ~3 h diagnostic + design

Charlie's canary trip ruled out stack overflow as the cause for the
async_tcp `InstructionFetchError` family. The remaining hypotheses
are use-after-free in `_error` path or task-priority race between
async_tcp service task and lwIP TCP timer. Next-step options:

1. **Patch AsyncTCP locally** — wrap `tcp_arg` / `tcp_recv` / `tcp_sent`
   / `tcp_err` in `tcpip_api_call` so all lwIP calls happen in TCPIP
   task context. Risk: maintenance burden against upstream.
2. **Replace AsyncMqttClient + AsyncTCP** with PubSubClient
   (synchronous). No async-task race surface. Larger refactor.
3. **Capture + decode another panic** with the v0.4.24 build's
   improved restart-policy diagnostics. May reveal whether it's a
   service-task or timer race.

Premature without more data. Recommend deferring until v0.4.24 +
v0.5.0 ship and we have another month of fleet uptime context.

## Other followups (≤1 h each)

- **#27** Library-API regression test in CI — promote `lib_api_assert.h`
  to a CI gate (DO-NOT for autonomous since it touches workflows).
- **#36** Heartbeat / boot-reason monitoring tile in Node-RED Dashboard
  2.0 — operator session, DO-NOT for autonomous.
- **#46** Recent abnormal reboots — Charlie canary trip + Foxtrot UUID
  drift (+ today's fleet outage if it produces new boot reasons) all
  feed this. Audit at the next quiet point.
- **#71 second cut** — full variant infrastructure beyond
  `esp32dev_minimal` and `esp32dev_relay_hall`. Combined with the
  ota.json `Variant` schema. v0.5.x territory.

## Won't do this session

- v0.4.24 tag without fleet recovery (validation discipline).
- v0.5.0 hardware before v0.4.24 is shipped (clean blast radius).
- #78 fix attempt without more diagnostic data.
- Anything DO-NOT (CI workflows, dashboard tiles, mosquitto auth, etc.).
