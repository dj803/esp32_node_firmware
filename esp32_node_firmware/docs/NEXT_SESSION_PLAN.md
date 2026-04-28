# Next session plan

Drafted 2026-04-28 morning, after v0.4.21 release shipped + the 12 h
autonomous window's discoveries (Alpha loopTask panic, #83 mosquitto
log freeze, Charlie canary 9.75 h sticky soak with no trip).

## State at last sweep (2026-04-28 ~07:55 SAST)

| | |
|---|---|
| Fleet | 5/6 on **v0.4.21 release** (Alpha, Bravo, Delta, Echo, Foxtrot); Charlie on **v0.4.20.0 canary** (sticky via OTA_DISABLE) |
| Charlie soak | 9.75 h continuous, heap pinned at 131036, no panic, no canary trip |
| Backlog | OPEN 48 (down from 62), RESOLVED 30 (up from 10), WONT_DO 5 |
| Open coredump from tonight | Alpha v0.4.20 `loopTask` IllegalInstruction PC 0x4008ec14 — un-decoded (CI-binary SHA mismatch) |
| Other open finding | #83 mosquitto.log frozen at 13:59 since blip-watcher cycles |

## Recommended next session — STABILITY DEEP-DIVE

Focused single-track session, ~3-4 hours. Goal: convert tonight's
fresh diagnostic data into either a fix or a confirmed-deferred
hypothesis.

### Phase 1 — Decode Alpha's loopTask panic (~1 h)

```bash
cd esp32_node_firmware
# Use a worktree so the bench checkout doesn't collide with HEAD
git worktree add /tmp/v0.4.20-decode v0.4.20
cd /tmp/v0.4.20-decode/esp32_node_firmware
PYTHONIOENCODING=utf-8 pio run -e esp32dev
ELF=.pio/build/esp32dev/firmware.elf
A2L=$HOME/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line.exe

# Alpha's captured backtrace (from /diag/coredump 2026-04-27 ~23:30):
$A2L -e $ELF -fipC \
  0x4008ec14 0x4008ebd9 0x400954ad 0x401ae04b 0x401ae080 \
  0x401ae15f 0x401ae1f2 0x400e4b0d 0x400e2c81 0x400ee19e \
  0x400f8bc2 0x40100023 0x40107ce4 0x4008ff31
```

Outcome: a function-name backtrace. If it converges in `loopTask`
inside our code (ESP-NOW responder, MQTT publish, RFID tick), file as
a new specific entry and try a fix. If it lands in framework code
(esp_event, mbedtls, json), file as #84 and watch for repro on v0.4.21.

Document the decode in archive entry #46 + commit.

If `git worktree add` fails because v0.4.20 was already the source
tree at HEAD when CI built, the ELF should match exactly. If it
doesn't, the alternative is downloading the CI artefact from
<https://github.com/dj803/esp32_node_firmware/actions> within the
retention window.

### Phase 2 — Fix #83 mosquitto.log freeze (~45 min)

Diagnostic step 1 in CLAUDE.md is rotted: mosquitto's log file stops
growing after the first blip-watcher service restart cycle. Fix
options (try in order):

1. Inspect `C:\Program Files\mosquitto\mosquitto.conf` vs the
   templated `mosquitto.conf.new` to confirm `log_dest file ...` and
   `log_type all` are persisted (not just applied via
   `apply-logging-config.ps1` once-off).
2. If config is missing the persistent block: bake it in, restart
   mosquitto once elevated, verify file grows after a synthetic blip.
3. If the block IS persistent but log still freezes: investigate
   service-restart file-handle behaviour; consider wrapping mosquitto
   in NSSM or adding a post-start hook.

Acceptance: after a manual blip cycle, `mosquitto.log` last-write
timestamp is current.

Mark #83 RESOLVED in the index/archive.

### Phase 3 — Pick ONE #76 sub-item for v0.4.22 (~1.5 h)

The shipped sub-G primitive enables sub-B cheaply, and sub-F is a
1-line change. Pick whichever feels more useful:

**Sub-B — NVS ring buffer of last-N restart contexts.** Extends sub-G
to track the last 8 restart causes per device. New
`include/restart_history.h` (uses Preferences with a small JSON ring
buffer). Boot announcement adds `last_restart_reasons:[ ... ]`. Lets
the dashboard see "this device restarted 3 times in the last hour all
with reason=mqtt_unrecoverable" — a pattern that warrants escalation.

**Sub-F — `CONFIG_ESP_TASK_WDT_TIMEOUT_S` 5 s → 12 s.** One-line
change in `sdkconfig.defaults`. Combines with the broker-probe + heap
guard to give legitimate slow reconnects more headroom without
losing the safety net. Validate via M3 chaos.

Either way, build + USB-flash to Bravo + M1+M2+M3 chaos before
claiming done. Then bundle as v0.4.22 release if the rest of #76's
long tail follows soon, or ship standalone if not.

## Alternative — JUMP TO v0.5.0 RELAY+HALL

If Alpha's panic decodes to "framework, not our code" (Phase 1
outcome), the diagnostic safety-net is mature enough to start the
v0.5.0 hardware feature work. Plan exists at
[PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md). This is a
multi-day initiative (relay driver, Hall ADC, Node-RED tile, OTA
of new variant), not a one-session task — but Phase 1 of the plan
(bench wiring + relay.h skeleton + relayInit/relaySet) fits in a
single session.

## Long-tail observations to keep in mind

- **Charlie canary** continues. If it trips with a stack-overflow halt
  (silent on MQTT, visible only on serial), that's the diagnostic
  win for #54 hypothesis #5 from #51. Don't open serial unless
  investigating — DTR-reset wipes the halt state.
- **#46 sweep** — every panic on v0.4.21 will now auto-publish a
  coredump. Watch `+/diag/coredump` retained payloads daily.
- **#48 UUID drift** — Bravo's UUID rotation tonight (NVS wipe via
  erase-flash) is consistent with the Delta/Echo finding from
  2026-04-25. The CLAUDE.md table is now correct as of this morning;
  daily-health resolves UUIDs from live MQTT.

## Followups not on the critical path (≤1 h each)

- **#27** Library-API regression test in CI — we have `lib_api_assert.h`
  but it's compile-only, not a CI gate. Promote.
- **#29** WDT-heartbeat audit for all blocking I/O — manageable code
  read; satisfied incrementally during cascade-fix but never a
  formal sweep.
- **#36** Heartbeat / boot-reason monitoring as a Node-RED dashboard
  tile (the firmware-side data has been there since v0.4.11; this is
  pure UI work).
- **#55** AsyncMqttClient malformed-packet counter — small heartbeat
  payload addition.
- **#69** Wakeup vs persistent-monitor preemption — apply A+C+E from
  the archive entry (event-filtering + composite OTA progress topic +
  single orchestrator). Saves agent autonomous-session productivity.

## Won't do this session

- v0.5.0 hardware bring-up beyond Phase 1 — too much for one session.
- Major #78 fix attempt — needs more diagnostic data first (Charlie
  canary outcome OR Alpha-style decoded panic).
- #61 / #68 (mosquitto auth, Node-RED adminAuth) — explicitly deferred
  until end of dev phase.
