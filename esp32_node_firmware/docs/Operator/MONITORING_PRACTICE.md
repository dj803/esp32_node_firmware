# Heartbeat & boot-reason monitoring practice

How the operator (and Claude in autonomous mode) detects fleet trouble
before it becomes an incident. Tracks SUGGESTED_IMPROVEMENTS #36 — the
practice has existed since v0.4.11 added per-heartbeat `LOG_HEAP`; this
doc codifies it so it's not lore.

## Signals the firmware emits

Each device publishes:

| Topic | Cadence | Purpose |
|---|---|---|
| `<root>/<uuid>/status` event=boot       | Once per boot | Boot announcement: `firmware_version`, `boot_reason`, `restart_cause`, `uptime_s=0`, `mac`, `node_name` |
| `<root>/<uuid>/status` event=heartbeat  | Every 60 s    | Live tick: `uptime_s`, `heap_free`, `heap_largest`, `mqtt_disconnects`, `wifi_channel`, `rfid_enabled`, `relay_enabled`, `hall_enabled` |
| `<root>/<uuid>/status` event=offline    | LWT (broker)  | Last-Will retained when the device's TCP/MQTT session drops without a clean disconnect |
| `<root>/<uuid>/diag/coredump`           | Once per panic | Retained payload: `exc_task`, `exc_pc`, `exc_cause`, base64 backtrace, `app_sha_prefix`. Cleared after publish |

`boot_reason` taxonomy:
- `poweron` — clean cold boot. Expected after USB power cycle.
- `software` — clean `esp_restart()`. Look at `restart_cause` for the
  trigger (`cred_rotate`, `cmd_restart`, `mqtt_unrecoverable`,
  `ota_reboot`). Empty `restart_cause` after a `software` boot means
  the cause was lost — investigate.
- `panic` — crash. Coredump on `/diag/coredump` should be present.
- `task_wdt` / `int_wdt` / `other_wdt` — watchdog tripped. Always
  abnormal in steady state. See [TWDT_POLICY.md](TWDT_POLICY.md).
- `brownout` — Vcc droop. Hardware / power-supply issue, not firmware.

## Three monitoring layers

Routine ops should run all three. Quiet success and quiet failure must
look different (#84 discipline).

### 1. Daily snapshot — `/daily-health`

Slash command (or `/loop 24h /daily-health`). Backed by
`C:\Users\drowa\tools\daily_health_check.py`. Reports land in
`C:\Users\drowa\daily-health\` with exit codes 0=green, 1=yellow,
2=red.

Checks:
- Every fleet UUID published a heartbeat in the last 90 s.
- No abnormal `boot_reason` in the latest retained boot announcement.
- `heap_free` not monotonically declining vs yesterday's snapshot.
- No new `/diag/coredump` retained payload.

**Cadence.** Run **once a day**. Skipping a day is fine; skipping a
week loses the granularity of regression detection (the script compares
"today's snapshot" against "the most recent prior snapshot" — if that's
8 days ago, gradual drift is harder to attribute). If automated via a
Windows scheduled task or `/loop 24h /daily-health`, no manual action
is needed.

**What happens if the report folder gets big?** Each report is ~3 KB
of Markdown; even a year of daily reports is ~1 MB. The folder is
**effectively never a disk-space issue.** No auto-pruning is needed.
The only secondary concern is that a Glob over thousands of `.md`
files becomes slightly slower, which doesn't affect daily-health
itself (it only reads the two newest reports). If you want to tidy
up by hand: `rm ~/daily-health/2025-*.md` (keep the current year, prune
older).

**Auto-resolver gotcha.** Pre-2026-04-29 the `expected_firmware`
field in `daily_health_config.json` was hardcoded — every release
shipped a stale config and 9 false-yellows. Resolved 2026-04-29 PM:
the script now reads the latest GitHub tag at runtime and uses that
as the expected version. Set `expected_firmware` explicitly in the
config only for canary scenarios. **No config edit should be needed
for routine releases.**

### 2. Live snapshot — `/fleet-status`

Slash command. Subscribes to `+/status` for ~5 s, prints retained boot
announcements + last-seen times per device. Use ad-hoc when:
- Investigating "did device X reboot recently?"
- Confirming an OTA reached every node.
- Checking for stale UUIDs after an NVS-wipe event.

### 3. Active alerting — `tools/silent_watcher.sh`

Monitor task. Alerts on LWT offline (silent deadlocks that don't
reboot — failure mode (b) of #51) AND abnormal boot reasons. Pair
with the boot_history poller in Node-RED for full coverage:

| Watcher | Catches |
|---|---|
| `silent_watcher.sh`     | LWT offline (deadlock without reboot), abnormal `boot_reason` on next-boot announcement |
| Node-RED boot_history   | Net-new abnormal entries in the persistent boot_history flow context (cross-session memory of who's panicked when) |
| `/diag/coredump` listener | Auto-published retained coredumps after the first boot post-panic |

Without the silent watcher, deadlock-class failures go unnoticed until
someone eyeballs the LEDs — see CLAUDE.md "Monitoring sessions".

## Routine flow (what to actually do every day)

1. Open the day with `/daily-health`. Green → move on.
2. If yellow/red, run `/fleet-status` to localise which device(s).
3. For abnormal `boot_reason`, decode the coredump (worktree-built ELF
   for the device's `firmware_version`, `addr2line -pfiaC` on the
   backtrace — recipe in #46 archive entry).
4. File the finding against the relevant `SUGGESTED_IMPROVEMENTS`
   entry. Don't lose it to chat scrollback.

## Verify-after-action (#84) cadence

Every state-changing action gets a verification poll within the
expected completion window AND a status line posted to the operator —
even when everything's clean. Quiet success and quiet failure must
look different.

| Action | Verification window | How |
|---|---|---|
| USB-flash | 60 s | `mosquitto_sub` on the device's `/status`, confirm `event=boot` + `uptime_s≤5` |
| OTA single device | 3 min | `mosquitto_sub` for `firmware_version=<target>`, small uptime |
| OTA fleet rollout | 5 + 3 min/device | `tools/dev/ota-monitor.sh <version>` (auto-polls all-match or timeout) |
| Synthetic blip | 90 s | `mosquitto_sub` on `+/status`, confirm `event=online` from every fleet member |
| `cmd/restart`, `cmd/cred_rotate` | 90 s | poll for `boot_reason=software` with `restart_cause=<expected>` |
| Node-RED flow push | 30 s | poll `/flows` or visual confirm |

This pattern was the root cause of #84 — silent waits after
fire-and-forget OTAs forced the operator to ask "what are we waiting
for?". Don't repeat.

## Capturing fleet snapshots — gotcha

When you `mosquitto_sub -t '+/status' -W <N>` for a fleet snapshot,
you may think you'll get one retained message per device immediately.
You will NOT. The broker only retains the **last** message per topic,
and `/status` is the same topic for every event a device emits — boot,
heartbeat, online, offline. If the most-recent retained message is the
**LWT offline payload** (because the device's TCP/MQTT session dropped
without a clean disconnect at some prior point), then the retained
payload you receive on subscribe is `{"online":false,"event":"offline"}`
— useless for snapshot purposes.

To get a current heartbeat from every device, your subscription
window must be **at least one full heartbeat cycle long** (60 s
default + a margin) so each device emits a live heartbeat that
updates its retained payload. **75 s is the safe minimum** for the
current 60 s heartbeat cadence.

```bash
# Reliable: 75 s window catches every device's live heartbeat
mosquitto_sub -h 192.168.10.30 -t 'Enigma/.../+/status' -W 75
```

```bash
# Unreliable: 8-10 s only catches devices whose retained payload
# happens to currently be a heartbeat — typically only 0-2 of 5
mosquitto_sub -h 192.168.10.30 -t 'Enigma/.../+/status' -W 10
```

This bit a session on 2026-04-28: an hourly-snapshot monitor with
an 8 s window captured only Bravo (whose retained `/status` happened
to be a fresh heartbeat). The other 4 production devices' retained
`/status` were stale offline-LWTs from earlier broker drops, so the
short window saw only `online:false` and filtered them out. Lengthening
to 75 s captured all 5 immediately.

`/fleet-status` and `/daily-health` both use ≥75 s windows for this
reason; ad-hoc `mosquitto_sub` invocations need the same.

## Anti-patterns

- **Trusting the dashboard alone** — Node-RED's `boot_history` view is
  derived from `/status` events; if the broker drops a retained
  message or Node-RED is restarted between events, the dashboard's
  history is incomplete. Always cross-check with `tail` of
  `C:/ProgramData/mosquitto/mosquitto.log` when something looks off.
- **Hardcoding device UUIDs** — UUIDs have drifted on Delta/Echo (#48)
  and rotated on Bravo (#50). Resolve from live MQTT first, fall back
  to a hardcoded map only as a last resort.
- **Counting "no message" as healthy** — silence means either
  everything's fine OR the silent-deadlock failure mode. Without
  `silent_watcher.sh` armed, you can't distinguish.
- **Skipping the verify-after-action poll because "the script printed
  OK"** — the script's exit code is necessary but not sufficient.
  The device has to publish the new state on MQTT before the action
  is verified done. See #84 for the incident that codified this.
