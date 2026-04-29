# Next session plan

Refreshed 2026-04-29 evening at session close. Four releases shipped
today (v0.4.27 morning + v0.4.28/v0.4.29/v0.4.30/v0.4.31 PM). Operator-
facing docs folder (`docs/Operator/`) live with 13 docs. Soak deferred
to 20:00 SAST tonight on v0.4.31 (operator will signal when ready).

## State at session close (2026-04-29 ~17:00 SAST)

| | |
|---|---|
| Master HEAD | `f34fe3b` + #101 commit pending below |
| Latest tag | v0.4.31 |
| Fleet | **6/6 on v0.4.31**, all healthy, heap_largest steady 81908 |
| Backlog | OPEN **25**, RESOLVED **64**, WONT_DO **11** |
| Released today | v0.4.27, v0.4.28, v0.4.29, v0.4.30, v0.4.31 |
| Soak | Deferred to **20:00 SAST tonight** on v0.4.31 (length TBD by /evening-soak) |

## Immediate next action — soak

Operator will signal when ready (~19:55 SAST). Then:

1. Run `/evening-soak` slash command (new this session, lives at
   `~/.claude/commands/evening-soak.md`). It does pre-flight gate +
   baseline capture + watcher arm + closure-criteria reminder.
2. silent_watcher.sh runs as a persistent Monitor through the night.
3. Operator returns in the morning → run `/morning-close` (PROPOSED
   but not yet written — write tomorrow before running).

## What `/morning-close` should do

(stub for tomorrow — write before running):

- Read the most-recent baseline from `~/soak-baselines/`
- Compare current fleet snapshot against baseline:
   - heap_largest decline > 5%/hour on any device → flag
   - mqtt_disconnects climbing during the window → flag
   - Any new `/diag/coredump` payload with fresh `app_sha_prefix` → flag
   - Any abnormal `boot_reason` in retained boot announcements → flag
- TaskStop the silent_watcher.sh Monitor armed by /evening-soak
- Output GREEN/YELLOW/RED + recommendation:
   - GREEN → close #46 (long-tail abnormal-reboot investigation)
   - YELLOW → leave #46 open, note the new evidence
   - RED → file new SUGGESTED_IMPROVEMENTS entry, decide rollback

## After the soak

If clean (most likely):

### A. Close #46
v0.4.22 + v0.4.28 + v0.4.31 cumulative bundle covers the long-tail
abnormal-reboot work. Move to RESOLVED in archive.

### B. Investigate `restart_cause=<unknown>` on OTA-reboot path
Daily-health flagged YELLOW on each device because the most recent
boot was `software` with `restart_cause=<unknown>`. The OTA-reboot
flow apparently doesn't stamp `restart_cause=ota_reboot` in NVS
reliably. File as #102 if it persists post-soak.

### C. v0.5.0 hardware bring-up
Operator may wire the 4x4 NeoPixel matrix + 2-channel relay onto
Alpha (per `docs/Operator/HARDWARE_WIRING.md`). Hall sensor remains
on the v0.5.0 plan but not in scope for this immediate session.

After wiring:
- Bench-flash with `[env:esp32dev_relay_hall]` variant (or
  uncomment `RELAY_ENABLED` in config.h)
- Bump LED count to 16 (replace) or 24 (cascade) via cmd/led
- Bench-test relay clicks via `cmd/relay`
- Watch for brownouts under combined load (Alpha v0.4.26 history)

### D. #101 log-rotation audit
Filed this evening. Document mosquitto/Node-RED/daily-health
rotation strategies in MONITORING_PRACTICE.md + add a daily-health
check that verifies rotation tasks are still armed.

### E. Write `/morning-close` slash command
Pair with `/evening-soak`. ~30 lines; mirrors the daily-health
delta logic but specific to soak baselines.

## Won't-do at next-session start

- Skip the soak prep — gate exists for a reason.
- Edit `daily_health_config.json` to set `expected_firmware` —
  the auto-resolver handles this now (set 2026-04-29 PM via the
  resolve_expected_firmware function).
- Trigger chaos blips during the soak window — contaminates the
  closure criteria.
- v0.5.0 hardware code work without operator wiring confirmed.

## Open questions for operator

- When will the soak be armed? Operator said "I'll let you know"
  after wrap — expecting ~19:55-20:00 SAST.
- Soak length on v0.4.31? Per CLAUDE.md "Soak windows": 8-12 h
  default for the cumulative cascade-fix bundle, evening start.
  24 h+ if you want to catch daily-cycle effects.
- 4x4 matrix + relay wiring tonight, tomorrow, or later?
- `/morning-close` — write tonight (so it's ready for wake-up), or
  tomorrow morning when running it?

## Reference for tomorrow

- Session memory: `~/.claude/projects/<project>/memory/session_2026_04_29_pm.md`
- Operator docs: `docs/Operator/README.md`
- v0.5.0 plan: `docs/PLAN_RELAY_HALL_v0.5.0.md`
- Hardware wiring guide: `docs/Operator/HARDWARE_WIRING.md`
- Closure criteria: `CLAUDE.md` "Soak windows" section
