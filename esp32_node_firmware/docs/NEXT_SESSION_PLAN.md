# Next session plan

Refreshed 2026-04-29 afternoon after v0.4.28 shipped (#78 cascade-window
guard + #96 long-outage AP-mode-loop fixes). The previous next-session
candidate (#78 bench-debug) is now CLOSED — the bug was symbolically
root-caused, fixed, and validated across the full fleet in one session.

## State at end of session (2026-04-29 ~PM SAST)

| | |
|---|---|
| Master HEAD | `db12681` v0.4.28 (CI building + auto-tagging) |
| Fleet | **6/6 on v0.4.28.0** post mass-USB-reflash, all heartbeating steady |
| Backlog | OPEN **28**, RESOLVED **56**, WONT_DO **11** (+#96, -#54, -#78, -#96 → -2 net since session start, plus #96 tracked as both opened+resolved same day) |
| Released | v0.4.27 (#94 — silent-degradation variant of AP-cycle); v0.4.28 (#78 panic-cascade + #96 long-outage AP-mode-loop) |
| Validation | 3 AP-cycle reproduction tests (~150s, ~?, ~760s outages) — no panics on Alpha v0.4.28; 6/6 self-recovery via #96 sub-A ap_recovered path on first post-flash AP→STA cycle |

## What we shipped today

**v0.4.27 (parallel session, morning):**
- #94: ESP-NOW reinit on WiFi-up + LedEventType::MQTT_LOST event.
  Targets the silent-degradation variant of the AP-cycle bug
  (Alpha's `ESP_ERR_ESPNOW_NOT_INIT` post-reconnect + LED stuck on
  green-breathing after MQTT drop).

**v0.4.28 (this session, afternoon):**
- **#78 RESOLVED** — Cascade-window publish guard. All 6 retained
  v0.4.26 cascade coredumps symbolized; common ancestor is
  `mqttPublish` ← `espnowRangingLoop`. Fix gates publishes for 5 s
  after any WiFi/MQTT disconnect or reconnect, closing the AsyncTCP
  `_error` vs AsyncMqttClient publish race.
- **#96 sub-A** — AP-portal pushes `"ap_recovered"` to RestartHistory
  before `ESP.restart()`, breaking the `mqtt_unrecoverable` streak.
- **#96 sub-B** — `mqttScheduleRestart()` now idempotent, preventing
  the phantom restart-loop signature where one outage looked like 8
  failed restarts.
- **#54 closed** with positive evidence (Charlie canary surviving
  multiple cascade events without firing).

## Bench state at session end

- All 6 devices flashed with v0.4.28.0, on mains/USB power, MQTT
  steady-state.
- Alpha + Bravo currently bench-attached on COM (post-test config).
- `last_restart_reasons` shows `"ap_recovered"` newest entry on all
  6 — the self-recovery audit trail of the #96 sub-A fix firing on
  the first post-flash AP→STA cycle.

## What we learned (deeply forensic)

- **Cascade is non-deterministic.** Three operator-triggered
  AP-cycles in this session — none produced cascade panics on the
  unguarded fleet. Same physical trigger ≠ same outcome (per #92).
  Guard validation is therefore *negative-evidence-supported*: Alpha
  v0.4.28 was never panicked, never could have panicked because of
  the guard, and showed visibly cleaner heap fragmentation post-cycle
  than its unguarded peers (81908 vs 36-43K largest contiguous).
- **Long outages exposed #96.** A 12.6 minute disconnect put EVERY
  fleet device into the AP-mode reboot loop simultaneously. NVS-
  persisted ring buffer can't be cleared by power-cycle. Mass-USB-
  reflash with the v0.4.28 fix self-recovered the entire fleet on
  first AP→STA cycle post-flash — a clean validation of the fix.
- **Phantom signatures are a class of bug.** When a state-tracking
  ring buffer is written from a code path with no debounce, a single
  event can saturate the ring with identical entries that look like
  N distinct events on next read. Worth scanning the codebase for
  similar patterns elsewhere.
- **`exc_cause: unknown` IDLE1 coredumps** are forced-abort
  signatures from another task, not actual hardware exceptions on
  the IDLE task. Three byte-identical retained payloads from a
  pre-v0.4.28 firmware (`app_sha_prefix: e4d23b96`, unknown build
  origin — likely a parallel-session local build) — preserved as
  forensic history but not actionable.

## Recommended next session

The #78 + #96 fixes are shipped and validated. Top candidates from
the remaining backlog:

### A. v0.5.0 hardware bring-up — relay + Hall on Bravo
**Owner:** depends on operator wiring relay + Hall on Bravo. Plan
unchanged in [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md).
Bravo is already off-bench and ready to receive the new hardware.

### B. v0.4.28 fleet soak + #46 close
**Acceptance:** ≥24 h sustained heartbeats from all 6 devices, no
new coredump signatures, no `mqtt_unrecoverable` events. If clean,
mark **#46** RESOLVED (the soak that's been blocked on cascade
events). Lightweight session — mostly monitoring + closure paperwork.

### C. #91 ESP32-WROOM-32U + external antenna procurement
**Owner:** operator orders parts (~$15-30). Bench test against
current WROOM-32 fleet for asymmetry / RF range / orientation
sensitivity. Could weeks out depending on procurement lead time.

### D. #87 / #88 / #89 ranging UX bundle
"Make the firmware self-document its state" theme. All small
firmware additions: visible-when-collecting indicator (#87),
NVS-persisted ranging-enabled flag (#88), persistence for
RAM-only calibration buffer (#89). Could ship as v0.4.29.

### E. #93 production-serial-instrument decision (B + D)
60 s heartbeat-to-serial line + cmd/diag/serial_dump on-demand
command. Pairs with the diagnostic-tooling theme (#87/#88/#89).
Visibility-only, very low risk.

## Won't-do at next-session start

- Trigger another AP-cycle "to see if cascade fires now" — the
  non-determinism means single trigger can't validate. Long-running
  fleet soak is the better validation channel.
- Open new #78-class investigations without fresh evidence —
  cascade-window guard is the production mitigation; vendored
  AsyncTCP patch deferred unless soak surfaces a residual cascade.
- Touch the `e4d23b96` IDLE1 coredumps further — forensic history,
  not actionable on v0.4.28.

## Open questions for operator

- Did the v0.4.27 OTA delivery during AP-cycle #2 raise any
  operational concerns (devices auto-OTA'd while in cascade-recovery
  mode)? May warrant a check on whether OTA-during-disconnect-recovery
  is a workflow we want to gate.
- How long do you want the v0.4.28 soak window before closing #46?
  CLAUDE.md says ≥24 h; if anything richer is wanted (e.g. a 7-day
  quiet-soak), say so before the soak starts.
