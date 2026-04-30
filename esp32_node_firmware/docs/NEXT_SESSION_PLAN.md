# Next session plan

Refreshed 2026-04-30 mid-morning after v0.4.32 fleet rollout. The
v0.4.31 overnight soak verdict was RED (#103); the fix shipped + rolled
to fleet within the morning session. Next-session is dominated by
**re-soak** to confirm #103 closure and unblock #46.

## State at session close (2026-04-30 ~10:30 SAST)

| | |
|---|---|
| Master HEAD | `06c976e` (v0.4.32 fixes) on top of `bf6bcfd` (operator-* rename) |
| Latest tag | **v0.4.32** (shipped this morning) |
| Fleet | **6/6 on v0.4.32**, all heartbeating cleanly post-rollout (Charlie restored to fleet by operator) |
| Backlog | OPEN **25**, RESOLVED **66** (was 64 — +2 from #103/#102), WONT_DO **11** |
| Soak status | armed for 2026-04-30 evening — operator runs `/operator-evening-soak` at ~17:00–20:00 SAST |
| Known retained coredumps | 6 retained `/diag/coredump` payloads, all from pre-v0.4.32 firmware (Alpha+Delta v0.4.31 #103 panics + 4 older). No fresh v0.4.32-prefix coredumps post-rollout |

## Highest-priority next-session item — soak closure for #46

The v0.4.32 re-soak is the gate for #46 closure. Path:

### A. `/operator-evening-soak` at ~17:00–20:00 SAST tonight

Run on operator's bench. Expect a clean GO gate (6/6 fresh heartbeats,
no abnormal pre-existing boots, no in-flight OTA, no blip-watcher
activity). Baseline file lands in
`C:\Users\drowa\soak-baselines\YYYY-MM-DD_HHMMSS.md`.

Recommended length: **8–12 h** (overnight default) per CLAUDE.md
"Soak windows" — matches the ship-cadence of the recent fixes.

### B. `/operator-morning-close` next morning

Reads the baseline, captures current state, decides GREEN/YELLOW/RED
per the closure criteria.

**Verdict outcomes:**
- **GREEN** → close #46 (close cumulative v0.4.22→v0.4.32 stability
  bundle as field-validated).
- **YELLOW** → investigate the FLAG; #46 stays open.
- **RED** → look for a new failure shape; if it's the same
  `0x4008a9f2` strlen pattern, escalate to #103 fix option (a)
  stable-connectivity gate (3 min steady before un-silencing
  publishes), since (c) alone wasn't enough.

### C. If GREEN — close #46

Move the index entry to RESOLVED block. Append archive STATUS line.
The closure narrative: cumulative bundle of #51 (v0.4.16/v0.4.22),
#78 (v0.4.28), #97 (v0.4.29), #98 (v0.4.30+v0.4.31), #103 (v0.4.32)
field-validated by the v0.4.32 overnight soak.

## Other items deferred from today

### D. v0.5.0 hardware bring-up — DELAYED (relay module shipping delay)
Relay module delivery delayed (operator update 2026-04-30 mid-morning).
v0.5.0 wiring on Alpha deferred until parts arrive. Prep work already
shipped this session is on-tap for whenever:
- HARDWARE_WIRING.md schema fix (cmd/relay correct format) + 5-step
  smoke-test sequence.
- esp32dev_relay_hall variant build verified clean on v0.4.32 HEAD
  (4:48, no size delta).
- #104 RESOLVED — next tag-cut publishes
  `firmware-esp32dev_relay_hall.bin` to the GH release, eliminating
  the local-build dependency.

When parts arrive: pick up at "[Quick smoke test](Operator/HARDWARE_WIRING.md#quick-smoke-test)"
in HARDWARE_WIRING.md. No code work needed — just wire + flash + smoke.

### E. #91 ESP32-WROOM-32U + external antenna procurement
Operator orders parts (~$15–30). Bench-test against current
WROOM-32 fleet for asymmetry / RF range / orientation sensitivity
once parts arrive.

### F. Phase 2 ranging cluster
#37, #38, #39, #42, #47, #49, #86, #90, #91 still open. Bundle
option after #46 closes (re-soak GREEN unblocks).

### G. #93 Production firmware serial-silent decision
Operator decision required: status quo (A), periodic
heartbeat-to-serial (B), canary-only watermark prints (C), or
on-demand cmd/diag/serial_dump (D). B+D recommended in archive
entry.

### H. #101 Log-rotation process audit
MEDIUM priority. Documentation work in MONITORING_PRACTICE.md +
a daily-health check. Pairs naturally with a quiet session.

## Won't-do at next-session start

- Trigger another OTA on v0.4.32 — fleet just rolled out, let it
  soak.
- Touch #46 closure prematurely — wait for the morning soak verdict.
- Bench-flash anything during the soak window — the operator's call
  per "Self-corrections from 2026-04-30 review" (ask before
  bench-flash during soak).
- Re-roll v0.4.32 to canary — Charlie is back in production fleet
  per operator's call this morning.

## Open questions for operator

- If soak goes GREEN: do you want #46 + #103 + #102 closure
  bundled into the morning's session-close commit, or batched
  for a later docs sweep?
- #91 antenna procurement timing — anything we can do now to
  prepare the bench rig (regulatory check, mount design)?
- v0.5.0 wiring is deferred pending parts arrival (relay module
  shipping delay 2026-04-30). No action required; status note only.

## Reference

- v0.4.32 release: https://github.com/dj803/esp32_node_firmware/releases/tag/v0.4.32
- OTA manifest: https://dj803.github.io/esp32_node_firmware/ota.json (v0.4.32)
- Rollout log: `esp32_node_firmware/ota-rollout-20260430-*.jsonl` (most recent)
- v0.4.31 soak closure: `C:\Users\drowa\soak-closures\2026-04-30_075829.md`
- #103 archive entry: docs/SUGGESTED_IMPROVEMENTS_ARCHIVE.md (line 5760+ with STATUS)
- #102 archive entry: docs/SUGGESTED_IMPROVEMENTS_ARCHIVE.md (line 5682+ with STATUS)
- COREDUMP decode runbook: docs/COREDUMP_DECODE.md
- v0.5.0 hardware: docs/PLAN_RELAY_HALL_v0.5.0.md +
  docs/Operator/HARDWARE_WIRING.md
