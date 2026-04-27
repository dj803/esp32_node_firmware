# Autonomous-mode prompt template

Use this when handing the agent a multi-hour autonomous stretch. Replaces the
"wall of as-needed actions" pattern that's hard to scan and drops items easily.

Three principles:
1. **GOAL up top** — single line, the thing success is judged on.
2. **YOU MAY / DO NOT lists** — sharp action boundaries instead of an ambiguous "as needed".
3. **DONE WHEN explicit** — without it the agent keeps finding more work indefinitely.

---

## Template (copy and edit per session)

```
Mode: AUTONOMOUS — I'll be back in <time>.

GOAL
  • <single primary objective>

YOU MAY (without asking)
  • Build, flash (COM4/COM5), USB-flash, OTA stagger
  • Bump version + tag + push (release pipeline end-to-end)
  • Trigger broker blips via tools/blip-watcher.ps1
  • Push Node-RED flows via /flows API
  • Disable/enable ESP-NOW + BLE per device
  • Run /daily-health on demand
  • Edit docs/* and commit
  • Re-run chaos tests (M1-M4, EN1, O2, I1) per docs/CHAOS_TESTING.md

DO NOT (without asking)
  • Push to gh-pages directly
  • Force-push to master
  • Roll back the OTA manifest
  • Reflash devices currently mid-bench-experiment without confirming
  • Make user-visible Node-RED dashboard changes
  • Modify CI workflows
  • Disable security features (auth, TLS) even if mocked

CONSTRAINTS
  • Keep responses ≤3 sentences unless I ask for detail
  • Prefer event-driven via watchers over /loop wakeups
  • Stop a watcher before flashing the COM port it holds
  • Use tools/dev/ota-rollout.sh for staggers (ack-driven, not fixed-interval)
  • Commit and push after every meaningful change so progress is visible

COMMUNICATION STYLE
  • One-word commands ("next", "yes", "continue") = pace/confirm. Execute, don't ask back.
  • Shortcut questions ("what next?", "what are we waiting for?") = give the next
    1-3 concrete actions, not a full status report.
  • Hardware observations from me ("green LEDs on alpha", "Charlie on COM5")
    = treat as ground truth and override your own stale notes.
  • "I'll be back in a few hours" usually means 5-30 min. Plan work in short
    chunks with natural checkpoints, not multi-hour arcs.

DECISION DEFAULTS (when the prompt is silent on a choice)
  • Diagnostic-first: on any panic/crash, capture the backtrace before any
    "fix" attempt. Backtraces are the diagnostic gold.
  • Document-then-move-on: a problem outside the GOAL gets logged with a
    # number in docs/SUGGESTED_IMPROVEMENTS.txt — don't pause to discuss
    unless it blocks GOAL.
  • Test-after-change: every firmware change runs at minimum M1 (5 s blip)
    + M2 (30 s blip) before claiming done. M3 (180 s) gates a release.
  • Commit small + often: every meaningful change gets a commit so I can
    pick up state from `git log` when I re-engage.

STATE YOU MAY HAVE STALE (verify before acting)
  • COM-port → device mapping (cables move, Windows renumbers).
    Verify with `esptool chip-id` against the fleet MAC table.
  • Which device has hardware peripherals (LED strip, RFID reader, BLE bench).
  • Whether a bench experiment is active on a specific device.
  • Live MQTT > CLAUDE.md > memory — when they disagree, trust live MQTT.

WHEN STUCK
  • Capture diagnostic state: serial backtrace, daily-health, MQTT snapshot
  • Log finding in docs/SUGGESTED_IMPROVEMENTS.txt with a # number
  • Continue with next item, don't block on a single failed branch

DONE WHEN
  • <explicit completion criterion>
  •  e.g. "All 6 fleet devices on v0.4.16 release AND fleet-wide M3 passes"
  •  or   "v0.4.17 #65 core-dump partition shipped + tested on Charlie"
  •  or   "Tier-2 chaos suite (M3, M4, EN1, O2) all green on v0.4.16 fleet"
```

---

## Why each section matters

**GOAL** — the agent has a long horizon and many open threads. Without a
single goal, it spreads effort across all of them and finishes none. Even
if you list 5 things in YOU MAY, exactly one is the goal.

**YOU MAY** — autonomous mode means no per-action confirmation. List the
classes of action you've already authorized so the agent doesn't pause for
permission and you don't get UAC-prompt-spam style commits.

**DO NOT** — far more important than YOU MAY in autonomous mode. Things
the agent might do "for completeness" that have side-effects you don't
want. Force-push and direct gh-pages writes are the most common ones for
this repo. Add per-session as needed (e.g. "DO NOT touch Foxtrot — it's
mid RFID test").

**CONSTRAINTS** — pacing and tool-choice cues. Three sentences vs three
paragraphs is a 5× context-window difference. Watcher discipline ("stop
before flash") avoids COM-busy errors.

**WHEN STUCK** — gives the agent permission to log a problem and move on
rather than spending hours on one root cause. With #67-style issues this
turns "blocked" into "documented + bypassed".

**DONE WHEN** — without this, the agent keeps finding more chaos tests
to run, more docs to update, more future-improvements to log. Concrete
terminal condition prevents drift.

---

## Example: today's session 2026-04-27

A retroactive version of what would have made the cascade work clearer:

```
Mode: AUTONOMOUS — back in ~3 h.

GOAL
  • Resolve the 10:42 fleet-panic cascade. Identify root cause, ship a fix,
    validate fleet-wide.

YOU MAY
  • Trigger broker blips (need elevated PS — use file-trigger watcher).
  • Build, USB-flash COM4/COM5, OTA stagger.
  • Bump + tag + push v0.4.X end-to-end.
  • Edit ANY docs file and commit.
  • Run chaos tests M1-M4 in any order.

DO NOT
  • Roll back OTA manifest without my approval (cost is ~15 min recovery).
  • Disable BLE permanently (Path C may want it back).
  • Modify .github/workflows/.

CONSTRAINTS
  • Capture serial backtrace if a panic reproduces — that's the diagnostic gold.
  • Watchers stay armed during chaos tests so events are visible.
  • Keep responses tight; no per-monitor-event commentary.

WHEN STUCK
  • Log AsyncTCP / library issues to SUGGESTED_IMPROVEMENTS with # number.
  • Don't go down library-rewrite rabbit holes; document and move on.

DONE WHEN
  • All 6 fleet devices recover cleanly through M2 (30 s blip).
  • Cascade root cause identified and fixed in firmware.
  • CHAOS_TESTING.md updated with results.
```

That would have ended at v0.4.14 (M2-clean) with #67 logged, instead of
running through v0.4.15 + v0.4.16 attempts. Both outcomes were valuable;
the difference is which one finishes when you're back vs which keeps
running until you re-engage.

---

## Common DO-NOT additions to consider per session

- "DO NOT change Wi-Fi credentials" (would brick fleet remotely)
- "DO NOT modify mosquitto.conf without restoring my running settings"
- "DO NOT push without me reviewing if commit count > 5"
- "DO NOT spawn Cron tasks" (some agent modes can do this; usually unwanted)

---

## When to skip the template

Single-hour sessions where you're checking in frequently — the structure
adds overhead. Use it for >2 h afk windows or when handing the agent
multi-step rollouts that span releases.
