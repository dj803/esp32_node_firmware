# tools/chaos/ — failure-injection scripts

Promoted from the manual scenarios in [docs/CHAOS_TESTING.md](../../docs/CHAOS_TESTING.md)
into something a release pipeline (or a smoke-test cron) can call.
Tracking entry: SUGGESTED_IMPROVEMENTS #75.

## Layout

| File | Role |
|---|---|
| `runner.sh`        | Orchestrator. Snapshots pre-state, fires a scenario, observes events, decides PASS/FAIL, writes a JSON report. |
| `blip_short.ps1`   | M1 — 5 s broker outage. |
| `blip_long.ps1`    | M2 (30 s default) / M3 (180 s) — long broker outage. |
| `blip_burst.ps1`   | M4 — 3 × short blips with brief recovery between. Stresses half-open-cleanup race. |
| `wifi_cycle.ps1`   | W2 — STUB until an AP-control hook is wired. |

The PowerShell scripts must be run **elevated** (they use `net stop/start`).
The runner itself does not need elevation, but invokes the scripts in a
nested PowerShell which inherits the current shell's privileges. Run the
runner from an elevated shell to skip the UAC prompt mid-trigger.

## Quick run

```bash
# from an elevated bash (Git Bash / WSL with broker reach):
tools/chaos/runner.sh M1                    # short blip + 60 s observation
tools/chaos/runner.sh M2 --window 90        # 30 s blip + 60 s observation
tools/chaos/runner.sh M3 --window 300       # 180 s blip + 120 s observation
tools/chaos/runner.sh M4 --window 90
```

Reports land in `~/daily-health/chaos-<scenario>-<ts>.json`. Schema:

```json
{
  "scenario": "M2",
  "description": "30 s broker blip",
  "timestamp": "20260428-153045",
  "window_s": 90,
  "trigger_rc": 0,
  "pre_devices": 5,
  "post_seen": 5,
  "abnormal_boots": 0,
  "coredumps": 0,
  "missing_devices": [],
  "boot_reasons": {},
  "result": "PASS"
}
```

`result` is the headline. CI / cron can parse just that line.

## What "PASS" means

A scenario passes when ALL of:

- Every device that was online before the trigger published at least one
  post-trigger event (any of `boot` / `online` / `heartbeat` will do —
  the device proved its MQTT path is alive).
- Zero `boot_reason` ∈ {`panic`, `task_wdt`, `int_wdt`, `brownout`}.
- Zero new `/diag/coredump` retained payloads (a coredump means a panic
  was captured to flash and just now drained — same severity as a fresh
  WDT-class boot).
- The trigger script's exit code is 0 (mosquitto stop+start succeeded).

A FAIL prints the missing devices and any abnormal boot reasons to
stderr. The JSON report has the full breakdown.

## Adding a scenario

1. Drop a new trigger script under this directory (`<name>.ps1` or
   `<name>.sh`).
2. Register it in `runner.sh`'s case statement near the top — give it a
   short uppercase tag (e.g. `EN1`, `O2`) and a description string.
3. If the scenario needs operator-side hardware (smart plug, PDU), gate
   it the way `wifi_cycle.ps1` is — exit 2 with a header note until the
   hook is wired.

## What the runner can NOT do

- Cannot reproduce the exact 14:04 v0.4.13 panic on its own — the
  no-DTR/RTS-toggle serial monitor on Charlie's COM port is not part
  of this framework. Add that out-of-band when chasing a panic that
  the boot announcement won't capture.
- Cannot detect deadlocks that don't reboot the device (the silent
  failures from #73). Pair with `tools/silent_watcher.sh` for that.
- Cannot test scenarios that need physical access (RFID hot-plug,
  PSU brownout) — that's the bench-rig domain (#72).

## Pre-release smoke

For a v0.x.y release, the recommended sequence is:

```bash
tools/chaos/runner.sh M1 --window 30
tools/chaos/runner.sh M2 --window 90
tools/chaos/runner.sh M4 --window 90
```

M3 (180 s) is the cascade-class stress test — run it for releases that
touch MQTT path or library versions, not for every patch.
