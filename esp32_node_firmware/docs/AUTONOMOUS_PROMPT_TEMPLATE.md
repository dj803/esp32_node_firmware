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
  • Edit any file in the repo — source code, headers, platformio.ini,
    sdkconfig.defaults, partitions.csv, docs, tools, tests. Memory files
    under ~/.claude/projects/.../memory/ and user-level helpers in
    ~/.claude/commands/ are also in scope. Off-limits items are listed
    explicitly under DO NOT below.
  • Build, flash (COM4/COM5), USB-flash, OTA stagger
  • Bump version + tag + push (release pipeline end-to-end)
  • Trigger broker blips: `echo N > C:\ProgramData\mosquitto\blip-trigger.txt`
    (writes an N-second blip — file-trigger watcher in operator's elevated
    PS picks it up within 500 ms and runs net stop/sleep/start mosquitto.
    Watcher must already be armed — see tools/blip-watcher.ps1 +
    "Start Blip Watcher.bat". Don't try Stop-Service / net stop directly
    — non-elevated sessions get access-denied.)
  • Push Node-RED flows via /flows API
  • Disable/enable ESP-NOW + BLE per device
  • Run /daily-health on demand
  • Re-run chaos tests (M1-M4, EN1, O2, I1) per docs/CHAOS_TESTING.md

DO NOT (without asking)
  • Push to gh-pages directly
  • Force-push to master
  • Roll back the OTA manifest
  • Reflash a device whose CURRENT state would be lost without a note —
    bench rigs actively producing data, devices mid-soak with unique
    crash history. Reflashing IS fine when (a) it's useful (e.g. canary
    soak, fix validation) AND (b) you record what was on the device
    first (firmware version, uptime, last boot reason) in a memory file
    or commit message.
  • Make user-visible Node-RED dashboard changes
  • Modify CI workflows
  • Disable security features (auth, TLS) even if mocked

CONSTRAINTS
  • Keep responses ≤3 sentences unless I ask for detail
  • Prefer event-driven via watchers over /loop wakeups
  • Stop a watcher before flashing the COM port it holds
  • Confirm blip-watcher is armed before scheduling chaos work — probe
    `Test-Path C:\ProgramData\mosquitto\blip-trigger.txt` then write
    a tiny 1-second blip and check `blip.log` for the entry. If silent,
    ask operator to start it via "Start Blip Watcher.bat" rather than
    falling back to "ask operator to run blips manually" each time.
  • `C:\ProgramData\mosquitto\mosquitto.log` is owned by SYSTEM (mosquitto
    service runs as SYSTEM) — non-elevated read returns "Permission
    denied". Same root cause as the blip-watcher elevation gap. Either
    ask operator for `Get-Content ... -Tail 60`, OR loosen the ACL once
    via `icacls C:\ProgramData\mosquitto\mosquitto.log /grant
    "$env:USERNAME:(R)"` from elevated PS — read-only, durable, no risk.
  • Use tools/dev/ota-rollout.sh for staggers (ack-driven, not fixed-interval)
  • Commit and push after every meaningful change so progress is visible
  • Verify-within-N-minutes after every state-changing action (#84). Flash:
    confirm boot announce within 60 s. OTA: poll for firmware_version=<target>
    within 3 min/device or use tools/dev/ota-monitor.sh. Blip: monitor
    +/status for event=online from all fleet members within 90 s. THEN post
    a status line — even if all-clean. Quiet success and quiet failure
    must look different to the operator.
  • In /loop dynamic, match wakeup cadence to expected next-event window.
    During ACTIVE work (in-flight OTA, post-flash boot, soak transition)
    use 60-300 s wakeups. Drop to 1200-1800 s only when (a) the operator
    explicitly says AFK, OR (b) the agent has confirmed the fleet is in
    steady state with nothing actively changing.

COMMUNICATION STYLE
  • One-word commands ("next", "yes", "continue") = pace/confirm. Execute, don't ask back.
  • Shortcut questions ("what next?", "what are we waiting for?") = give the next
    1-3 concrete actions, not a full status report.
  • Hardware observations from me ("green LEDs on alpha", "Charlie on COM5")
    = treat as ground truth and override your own stale notes.
  • "I'll be back in a few hours" usually means 5-30 min. Plan work in short
    chunks with natural checkpoints, not multi-hour arcs.

QUESTIONS — file-first, never block
  • Don't pause autonomous work for an answer. Save questions to
    `docs/SESSIONS/SESSION_QUESTIONS_YYYY_MM_DD.md` (one file per session)
    and keep working.
  • URGENT observations (fleet down, regression in flight, anything that
    needs operator action right when they re-engage) go in a top-level
    URGENT section so they aren't buried under routine Q&A.
  • Each question is self-contained — the operator hasn't seen the
    conversation, so include file paths, what was tried, what's blocking,
    what the recommended default is. Mark Q1, Q2, … so it's grep-able.
  • When the operator asks "what questions do you have?" / "any questions?"
    give a tight bullet summary of just the questions, not the full file
    body — they'll open the file if they want detail.
  • As answers arrive, mark each heading inline with
    `**[ANSWERED YYYY-MM-DD: <one-line gist>]**` rather than deleting
    the question. The closed Q&A stays in the file as a session artefact.
  • Items deliberately skipped this session get a short "why I skipped"
    note at the bottom — so the operator sees the option was considered
    rather than missed.

DECISION DEFAULTS (when the prompt is silent on a choice)
  • Diagnostic-first: on any panic/crash, capture the backtrace before any
    "fix" attempt. Backtraces are the diagnostic gold.
  • Document-then-move-on: a problem outside the GOAL gets logged with a
    # number in docs/SUGGESTED_IMPROVEMENTS.md — don't pause to discuss
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
  • Log finding in docs/SUGGESTED_IMPROVEMENTS.md with a # number
  • Continue with next item, don't block on a single failed branch

DONE WHEN
  • <explicit completion criterion>
  •  e.g. "All 6 fleet devices on v0.4.16 release AND fleet-wide M3 passes"
  •  or   "v0.4.17 #65 core-dump partition shipped + tested on Charlie"
  •  or   "Tier-2 chaos suite (M3, M4, EN1, O2) all green on v0.4.16 fleet"

  Three implicit requirements that always apply (regardless of explicit criterion):
  1. Goal results VALIDATED — not just "code compiled and tagged" but
     "behavior verified end-to-end on real hardware via the appropriate
     chaos test or visual confirmation".
  2. ALL DEVICES on the correct version — if a release was tagged, the
     fleet (or the devices in scope) must actually be running it. -dev
     binaries on test devices must be upgraded to release before stopping.
  3. Watchers + monitors back to a stable steady state — no devices
     in panic loops or mid-OTA at the time of stop.
  4. End-of-session doc-sweep run — see "End-of-session checklist"
     below. Operator should never have to ask "did you update X" or
     "is the index still accurate". If the agent stops before the
     checklist runs, it's not done.
```

## End-of-session checklist

Before ending an autonomous-mode session (whether done-naturally or
on operator command), the agent runs through this list. Each item
takes ≤5 minutes; the whole sweep is under 30. **No new code, no
release, no scope expansion** during the sweep — just bring the docs
into sync with what already shipped.

1. **Bump version in CLAUDE.md** if a release was cut. The
   `## Device Fleet (firmware vX.Y.Z)` heading is the canonical
   "fleet on" version; update it to the release that just shipped
   (or "vX.Y.Z — except <device> on <variant>" if a device is
   intentionally off-release).

2. **Update ROADMAP.md** with the just-shipped release(s):
   - Add a `### vX.Y.Z — DONE (shipped <date>)` entry under
     "Now (just shipped or in flight)" with a 3-5 bullet summary.
   - Bump the "Last updated" line at the top.

3. **Refresh NEXT_SESSION_PLAN.md** to reflect what's done + what's
   the new recommended next session. If the previous A/B/C menu's
   A is now done, promote B and C, add a new C if appropriate.

4. **Audit SUGGESTED_IMPROVEMENTS.md OPEN list** for stale entries.
   For each entry, ask: "is this still actionable, or did the work
   silently land elsewhere?" Common cases:
   - REJECTED design alternatives still in OPEN → move to WONT_DO
     with rationale + add STATUS line in archive.
   - Audits / hardware findings that produced a doc → move to
     RESOLVED with a pointer to the doc.
   - Sub-items of a tracking entry that have all shipped → move
     parent to RESOLVED or update parent's "(sub-A/B/C shipped,
     D/E open)" annotation.
   Verify total counts match the actual line counts. Bump the
   "Last index sweep" date.

5. **Update docs/README.md index** if any new doc was added or
   moved during the session. Date-stamped audits / incident reports
   live in `SESSIONS/`; convention/policy docs stay at top level
   (per TRACKING_DOC_CONVENTION.md). Includes the per-session
   `SESSION_QUESTIONS_YYYY_MM_DD.md` file (see "QUESTIONS" in the
   prompt body) — operator should not have to remember filenames to
   find unanswered questions.

6. **Update memory MEMORY.md + the session-summary memory file**
   under `~/.claude/projects/<project>/memory/`. The MEMORY.md
   index is the cross-session bridge; the per-session file is the
   detailed pickup point.

7. **Final commit** of all the above as `docs: end-of-session sweep
   post-vX.Y.Z`. Push.

8. **Final fleet snapshot** (mosquitto_sub for ~60 s) confirming all
   devices on intended versions, heaps healthy, no panic loops.
   Post a one-paragraph summary to the operator.

This list is itself a tracking-doc — updates to it should reference
the post-mortem in archive entry #85 (which captures the gap that
prompted formalising this).

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
