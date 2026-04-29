# Roadmap

Forward plan synthesized from [SUGGESTED_IMPROVEMENTS.md](SUGGESTED_IMPROVEMENTS.md), [ESP32_FAILURE_MODES.md](ESP32_FAILURE_MODES.md), [memory_budget.md](memory_budget.md), [TOOLING_INTEGRATION_PLAN.md](TOOLING_INTEGRATION_PLAN.md), [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md), and the per-version plans in `~/.claude/plans/`.

Last updated: 2026-04-29 afternoon (post-v0.4.28 #78 cascade-window guard + #96 AP-mode-loop bundle).

---

## Now (just shipped or in flight)

### v0.4.28 — DONE (shipped 2026-04-29 afternoon — #78 cascade-window guard + #96 long-outage AP-mode-loop fixes)

Three bugs closed end-to-end by symbolic decode + bench validation.

- **#78 RESOLVED** — AsyncTCP `_error` vs AsyncMqttClient publish race.
  Decoded all 5 v0.4.26 cascade panics against the v0.4.26 ELF
  (worktree at `/c/Users/drowa/v0426-decode/`); common-ancestor frame
  is `mqttPublish` ← `espnowRangingLoop`. Fix: cascade-window publish
  guard with `_lastNetworkDisconnectMs` stamped from
  `onMqttDisconnect` + WiFi-lost + WiFi-reconnect sites; `mqttPublish`
  drops silently for `CASCADE_QUIET_MS` (5 s default) after any
  stamp. See [docs/SESSIONS/COREDUMP_DECODE_2026_04_29.md](SESSIONS/COREDUMP_DECODE_2026_04_29.md).
- **#96 sub-A RESOLVED** — Long-outage AP-mode reboot loop:
  `ap_portal.h:1001` now pushes `"ap_recovered"` to RestartHistory
  before `ESP.restart()` in the STA-reconnect path, so post-reboot
  `countTrailingCause("mqtt_unrecoverable")` returns 0. Self-clears
  the streak on first AP-→-STA cycle.
- **#96 sub-B RESOLVED** — `mqttScheduleRestart()` is now idempotent
  (early-return when `_mqttRestartAtMs != 0`), preventing the
  hundreds-per-cascade-window phantom restart-loop signature.
- **#54 RESOLVED 2026-04-29 morning** — Charlie's 35h+ canary on
  `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` survived multiple cascade
  events without firing. Strong positive evidence that #78 is heap
  corruption, not stack overflow. Charlie reflashed off canary to
  free COM5 for the #78 bench-debug session.

Validation: USB-flashed all 6 fleet devices with v0.4.28. Each
recovered cleanly from the AP-mode loop on first AP-→-STA reconnect
(`last_restart_reasons` ends in `"ap_recovered"` on every device).
6/6 healthy on v0.4.28.0 with steady heartbeats post-flash.

Build: esp32dev clean (~1646 KB flash, RAM 22.3% / Flash 83.7%, no
size delta vs v0.4.27 — guards are essentially free). All 6 fleet
devices on v0.4.28 manually flashed; CI tag will canonicalize to
v0.4.28 (3-component) and the gh-pages OTA manifest will publish.

### v0.4.26 — DONE (shipped 2026-04-28 evening — LED feature bundle)

Six LED-related items closed end-to-end. Operator swapped Alpha
(WS2812-equipped) onto COM4 to make this serial-validatable.
Full schema lives in mqtt_client.h's handleLedCommand() —
Node-RED dashboards can drive against the v0.4.26 schema once OTA'd.

- **#19 Per-LED addressing** — `cmd/led "pixel"` (single, auto-commit)
  + `cmd/led "pixels"` (bulk JSON array). New `LedState::MQTT_PIXELS`
  freezes `_leds[]` for direct-render.
- **#20 Scene/preset save-to-device** — NVS namespace `led_scenes`,
  up to 8 named slots. `scene_save / scene_load / scene_delete /
  scene_list`. Captures pixel buffer + brightness; restores on
  demand.
- **#21 Group/broadcast LED commands** — `Enterprise/Site/broadcast/led`
  topic mirrors `cmd/led` semantics. Subscribed at QoS 1.
- **#22 Scheduled/time-of-day automation** — new `include/led_schedule.h`
  with NTP-synced minute-poll, 8 NVS slots, action-as-cmd/led-JSON
  re-fed at fire time. `sched_add / sched_remove / sched_list /
  sched_clear`. Timezone hard-coded SAST (UTC+2, no DST); override
  via `LED_SCHEDULE_TZ_OFFSET_S`.
- **#23 OTA/heartbeat LED override** — `cmd/led "override"`
  {r,g,b,anim,duration_ms} with auto-revert. New anim names
  "alarm" + "warn" for app-level event UX.
- **#31 Pin LED + RFID tasks to Core 1** — verified-already-done
  in arduino-esp32 v3.x. Original archive concern was a v2.x
  default that no longer applies.

Build: esp32dev clean (1645696 bytes flash, +13.4 KB vs v0.4.25;
70696 RAM unchanged). 105/105 native tests pass.

### v0.4.25 — DONE (shipped 2026-04-28 afternoon)

Stability + UX patch on top of v0.4.24. Three thematic items:

- **#32 heap-headroom gates at subsystem init** — `heapGateOk(freeMin, blockMin, tag)`
  helper wraps MQTT init + BLE init (when compiled) + TLS keygen with
  skip-on-low-heap paths. Per-subsystem thresholds in config.h. Mirror
  of v0.3.33's OTA preflight gate. Subsystems that skip stay disabled
  this boot — operator reads `[W][HeapGate]` log and power-cycles.
- **#34 Phase 2 — captive-portal port-80 redirector.** Second httpd
  instance bound to :80 with wildcard catch-all 302 → https://192.168.4.1/.
  Combined with Phase 1's DNS hijack, the captive UX is now end-to-end:
  phone connects → DNS resolves probe URLs to AP IP → :80 redirects →
  OS pops the captive sheet on the real portal.
- **`tools/dev/ota-rollout.sh` EXCLUDE_UUIDS env var.** Subtractive
  filter for canary builds / devices under investigation; mirrors
  FLEET_UUIDS semantic. Closes the Q6 follow-up from earlier today.

Plus: **#33 versioned MQTT topic prefixes design doc** at
`docs/TOPIC_VERSIONING_DESIGN.md` — forward-planning, no code yet.
Implementation gated on fleet > 10 / first breaking schema / v1.0.

Closes: #32, #34 (Phase 1 + Phase 2). Partial: #33 (design only).

Build: esp32dev clean (1632256 bytes flash, +1816 vs v0.4.24).
105/105 native tests pass.

### v0.4.24 — DONE (shipped 2026-04-28 afternoon)

Restart-policy hardening + operator-recovery UX bundle. Closes the
#76 sub-A through sub-I long-tail and promotes the chaos framework to
a CI-ready tool surface.

- **#76 sub-C — time-based MQTT_RESTART_THRESHOLD.** Primary Tier-2
  escalation is now `MQTT_UNRECOVERABLE_TIMEOUT_MS` (10 min default);
  count-based threshold becomes a backstop bumped 10 → 30. Survives
  backoff-cadence weirdness — a 10 min outage triggers regardless of
  how many reconnect attempts the backoff schedule slotted in.
- **#76 sub-D — restart-loop AP-mode fallback.** ≥3 consecutive
  `mqtt_unrecoverable` entries in `RestartHistory` route setup() to
  AP_MODE on next boot. Streak is broken automatically by
  `mqttMarkHealthyIfDue()` after 5 min of stable MQTT.
- **#76 sub-I — /daily-health WDT vs SW categorisation.** Fleet-rollup
  row + per-device tagging distinguishes WDT-class (RED, hardware
  fault) from SW-restart (operator/OTA = neutral; self-heal = YELLOW)
  from poweron (GREEN).
- **#34 Phase 1 — captive-portal DNS hijack.** AsyncUDP-backed
  DNSServer on UDP:53 with empty-domain catch-all. Auto-pops captive
  sheet when sub-D drops a device to AP_MODE. Phase 2 (port-80
  redirector) deferred to a future release.
- **#75 chaos framework promoted** to `tools/chaos/` with
  M1/M2/M3/M4 PowerShell triggers + bash `runner.sh` orchestrator
  + JSON pass/fail reports under `~/daily-health/`.
- **#40 operator install guide** shipped at
  `docs/OPERATOR_INSTALL_GUIDE.md` distilling the #41 RF sweep findings
  into actionable rules (RC522 ≥ 5 cm from WROOM, single power path
  fleet-wide, USB cable routing, calibration discipline).
- **Index hygiene** — #24 + #28 + #77 moved OPEN → RESOLVED
  (audit-stale; all three already shipped in earlier releases). Open
  37 → see SUGGESTED_IMPROVEMENTS for current count.

Validated: esp32dev clean build (1630440 bytes flash, 70696 RAM);
105/105 native tests pass; fleet OTA rollout 2026-04-28 afternoon
took 5/6 production devices to v0.4.24 (Charlie excluded — sticky on
v0.4.20.0 canary per operator Q1 decision). Boot announcements
include `restart_cause:"ota_reboot"` + `last_restart_reasons` ring
buffer.

Notable fleet-rollout finding: `tools/dev/ota-rollout.sh` has two
bugs (silent discovery failure + stale-retained-boot poisoning the
abnormal-boot guard). Logged in SESSION_QUESTIONS_2026_04_28 Q6
for next-session fix.

### v0.4.23 — DONE (shipped 2026-04-28 mid-morning)

Followups-while-waiting-for-hardware batch on top of v0.4.22:

- **#55 — `mqtt_disconnects` cumulative counter** in heartbeat JSON.
  Pairs with `mqtt_last_disconnect` (enum value, 0xFF until first
  drop). Operators can spot a device drifting from 0 to 1+ over
  uptime. Validated: Bravo cumulated 3 disconnects across M1+M2+M3.
- **#76 sub-F — `CONFIG_ESP_TASK_WDT_TIMEOUT_S` 5 → 12 s.** 20×
  margin against every blocking site per the new
  [SESSIONS/WDT_AUDIT_2026_04_28.md](SESSIONS/WDT_AUDIT_2026_04_28.md).
- **#76 sub-B — NVS ring buffer of last-N restart causes.** New
  `include/restart_history.h` + boot announcement field
  `last_restart_reasons:[...]`. Pattern detection across reboots.
- **#76 sub-H — Jittered exponential backoff** in onMqttDisconnect.
  ±20% jitter prevents lock-step fleet reconnect storms after a
  broker outage.
- **#29 — WDT-heartbeat audit RESOLVED.** Read-only sweep
  documented in [SESSIONS/WDT_AUDIT_2026_04_28.md](SESSIONS/WDT_AUDIT_2026_04_28.md).
- **#48 — UUID drift root cause IDENTIFIED.** Audit at
  [SESSIONS/UUID_DRIFT_AUDIT_2026_04_28.md](SESSIONS/UUID_DRIFT_AUDIT_2026_04_28.md)
  — RNG-pre-WiFi pseudo-random determinism. Fix
  (`bootloader_random_enable` bookend) deferred to v0.5.0 alongside
  Hall ADC integration.

Validated: M1+M2+M3 chaos pass on Bravo before tag, fleet OTA-up
clean post-tag. Open: 47 → 44; Resolved: 32 → 35.

### v0.4.22 — DONE (shipped 2026-04-28 morning — #46/#51 root cause completion)

mqttPublish heap-guard hardened. Alpha's loopTask v0.4.20 panic
captured tonight via /diag/coredump decoded to the SAME bad_alloc
shape from v0.4.10 #51. The v0.4.11 heap-guard at threshold 4096 had
been load-bearing through the cascade-fix marathon but was undersized
— the function builds `String topic = mqttTopic(prefix)` BETWEEN the
guard check and the publish call, fragmenting the heap further.
v0.4.22 fix: dual-guard (re-check after topic build) + threshold
4096 → 8192 + try/catch on the publish call as defense-in-depth.

Validated: M1+M2+M3 all PASS on Bravo before tag.

### v0.4.21 — DONE (shipped 2026-04-28 morning)

Diagnostic + tooling release. Pure additions over v0.4.20; no behaviour
change for production fleet. Key entries:

- **#76 sub-G — NVS-persisted `restart_cause` in boot announcement.**
  New `include/restart_cause.h` (Preferences wrapper). `mqttScheduleRestart()`
  now writes the reason to NVS just before arming the deferred-restart
  deadline; `mqttBegin()` reads + clears once on boot. `mqttPublishStatus("boot")`
  appends `"restart_cause":"<reason>"` to JSON when non-empty (alongside
  the existing `boot_reason`). Validated end-to-end on Bravo: cmd/restart
  → reboot → boot announce carries `restart_cause:"cmd_restart"`;
  subsequent heartbeats do not (one-shot). M1+M2+M3 chaos all pass.
- **#71 first cut — minimal-variant infrastructure.** New
  `[env:esp32dev_minimal]` extending `esp32dev` with `-DRFID_DISABLED`.
  `config.h` now gates `#define RFID_ENABLED` with `#ifndef RFID_DISABLED`.
  `mqtt_client.h::handleRfidWhitelist` and its dispatcher wrapped in
  `#ifdef RFID_ENABLED` so the link doesn't fail when RFID is compiled out.
  Bravo flashed, `rfid_enabled:false` in heartbeat, M1+M2+M3 pass on the
  variant. Production esp32dev binary unchanged (RFID stays on by default).
- **#54 + #80 — `OTA_DISABLE` compile gate for canary builds.** Canary's
  `0.4.20.0` version sorts BEFORE `0.4.20` release per #80's 4-component
  semver, which made the canary self-OTA up to release on first OTA check
  and killed the soak. Added `-DOTA_DISABLE` to `[env:esp32dev_canary]`
  build_flags; `otaCheckNow()` returns immediately when defined. Re-flashed
  Charlie, validated via `cmd/ota_check` (no OTA fired); 9+ h sticky soak
  ongoing.

**Closes:** #30 (mathieucarbou fork swap, retroactive), #43 (firmware_version
EMPTY, retroactive), #50 (esptool erase-flash NVS — confirmed working),
#51 (v0.4.10 stability regression, closed by v0.4.16 cascade fix), #56
(MQTT_HEALTHY deferred-flag, retroactive), #57 (host gcc), #58/#59/#60/#62/
#64/#65 (repo-hygiene audit batch), #66 (`.claude/commands/` shortcuts),
#73 (silent-failure watcher, retroactive), #79 (version-update + ack-driven
OTA tooling, retroactive), #80 (-dev OTA path, retroactive), #81
(renumbering pass), #82 (parked — convention doesn't apply).

**Parks:** #6 OTA cert pinning, #7 MQTT-over-TLS, #13 UID-clone MIFARE,
#18 NFC card emulation → moved to docs/WONT_DO.md with rationale.

### v0.4.20 — DONE (shipped 2026-04-27 evening)
- **#70 option B — 4-component dev versioning.** Local builds now report
  `MAJOR.MINOR.PATCH.DEV` (e.g. `0.4.20.0`) which sorts cleanly before
  the matching release per `semverIsNewer`. -dev devices auto-upgrade
  to release via the existing periodic OTA check.

### v0.4.11 — DONE
Heap-guard fix in `mqttPublish()` (#51 root cause). Plus bundled: BLE off, NDEF feature, #48/#49 visibility logs, heap heartbeat, tools/ directory, CLAUDE.md, .claude/settings.json.

### v0.4.12 — DONE
Cosmetic re-tag of v0.4.11 for OTA-path validation.

### v0.4.20 — Plan C: per-device firmware variants (#58)

Today every device runs the same binary even though Foxtrot uses RFID, Bravo uses LEDs+BLE, others use only ESP-NOW. Each carries dead code (~30-40% binary bloat). Per-variant builds let us:
- Smaller binaries (faster OTA download, less flash wear).
- Smaller attack surface per role (RFID-less devices don't expose RFID command handlers).
- Easier diagnosis (today's BLE-off A/B test would be CI-built, not hand-flagged via `BLE_ENABLED`).

**Steps:**
1. New PIO envs: `[env:esp32dev_full]` (= current), `[env:esp32dev_rfid]`, `[env:esp32dev_ranging_only]`, `[env:esp32dev_minimal]`. Each toggles `RFID_ENABLED` / `BLE_ENABLED` / `WS2812_ENABLED` via build flags.
2. CI builds N binaries; uploads them to the GitHub release with names `firmware-<variant>.bin`.
3. Pages OTA manifest schema gains a `Variant` keyed entry:
   ```json
   {"Configurations":[
     {"Variant":"rfid","Version":"0.4.20","URL":"https://.../firmware-rfid.bin"},
     {"Variant":"ranging_only","Version":"0.4.20","URL":"https://.../firmware-ranging_only.bin"},
     {"Variant":"minimal","Version":"0.4.20","URL":"https://.../firmware-minimal.bin"}]}
   ```
4. Firmware bakes a `BUILD_VARIANT` define from the env's build flag and selects the matching manifest entry in `otaCheckNow()`.
5. Per-device variant assignment via NVS field (`gAppConfig.build_variant`); default = `full` for backwards compat.
6. `cmd/config_mode` web portal lets operator pick variant via dropdown.

**Risks/scope:** new variants must compile cleanly with subsystems disabled — likely flushes out latent `#ifdef` issues. CI matrix grows N×. OTA manifest schema bump needs Node-RED dashboard adjustment. Expect 1-2 days end-to-end.

**Defer:** until v0.4.20+ — none of the current friction blocks normal operation; it's pure efficiency. Best done after v0.5.0 hardware (#11/#13/#14) settles.

### v0.4.19 — DONE (shipped 2026-04-27 evening)
- **#65 Phase 2 — pre-restart diagnostic publish.** Single helper `mqttScheduleRestart(reason, deferMs)` builds a JSON with `restart_cause`, `fail_count`, `last_disconnect_reason`, `reconnect_delay_ms`, `wifi_rssi`. Hooked into 3 deferred-restart sites (`cmd/restart`, `cred_rotated`, `mqtt_unrecoverable`). Dashboard now sees WHY a device just decided to self-restart.
- **#54 — `[env:esp32dev_canary]` build env.** Adds `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` for soak runs. Not enabled by default (per-context-switch cost).

### v0.4.18 — DONE (shipped 2026-04-27 evening)
- **#70 fix** — semverIsNewer treats `-dev` as older than the same numeric release. USB-flashed dev devices auto-upgrade to release via the existing periodic OTA check. 4 new test cases.

### v0.4.17 — DONE (shipped 2026-04-27 evening)
- **#65 Phase 1 — coredump publish.** On boot, `coredumpPublishIfAny()` reads any stored ESP-IDF core dump, extracts the summary (exc_task, exc_pc, exc_cause, backtrace), publishes to `.../diag/coredump` retained QoS 1, then erases. Validated end-to-end: drained two stored panics from earlier session (Charlie InstructionFetchError, Bravo StoreProhibited) without serial access.

### v0.4.16 — DONE (shipped 2026-04-27 evening — CASCADE FULLY CLOSED)
- **Pre-connect broker probe** (#67 option C). Before `_mqttClient.connect()`, do a quick 1500 ms TCP SYN probe via `WiFiClient`. Only invoke AsyncMqttClient if the probe succeeds. Eliminates the AsyncTCP `tcp_arg` use-after-free that fires when lwIP's natural ~75 s SYN timeout finally errors out a connect against a dead broker.
- **Validated 17:59:29 SAST:** fleet-wide M3 (180 s broker outage). All 6/6 devices reconnected via `event=online` preserving uptime — zero panics, zero abnormal boots, zero ESP.restart() invocations. The same M3 on v0.4.15 produced 4/4 release devices abnormally booting (1 panic + 3 int_wdt).
- **Cascade story closed.** Spans v0.4.10 #51 (initial fleet crash) → 10:42 cascade → 14:04 backtrace capture → v0.4.14 90 s timeout → v0.4.15 force-disconnect → v0.4.16 broker probe.

### v0.4.15 — DONE (shipped 2026-04-27 afternoon — partial fix)
- Removed `ESP.restart()` from `mqttIsHung` path; bumped `MQTT_HUNG_TIMEOUT_MS` 90 s → 300 s. Sufficient for outages ≤ 75 s (= mosquitto log rotation, AP blip). M3 (>75 s) still cascaded — completed in v0.4.16.

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

### v0.4.22 — coredump from Alpha-class panics (#46 follow-up)

Tonight's Alpha panic (`loopTask` `IllegalInstruction` PC 0x4008ec14,
14-frame backtrace) gave a NEW signature distinct from the async_tcp
family. Cannot decode locally — Alpha's CI v0.4.20 binary has app_sha
prefix `a5bb3114`; my local builds produce `dd877030`. To root-cause
it we need either:
- Download the CI v0.4.20 ELF from the GitHub release artefacts (if
  retention window is long enough) and addr2line against it, OR
- Rebuild from the v0.4.20 tag locally (`git checkout v0.4.20 && pio
  run -e esp32dev`) to reproduce the same SHA, OR
- Wait for another Alpha-class panic on v0.4.21+ which we DO have ELF
  access to, then decode that.

The shape (`loopTask` + IllegalInstruction + long-uptime trigger) fits
either a slow heap leak overwriting a vtable OR a subtle stack overflow
that the canary build would catch. Charlie's canary at 9+ h with no trip
weakens the stack-overflow hypothesis but doesn't eliminate it —
Alpha's stack pressure may differ.

### Charlie canary soak — long-tail wait

Sticky now (OTA_DISABLE). Continue until either (a) canary tripping with
a stack-overflow halt → root cause found, (b) 7 days clean → hypothesis
retired and Charlie returns to release. Existing soak watcher catches
abnormal boot reasons; canary halts are silent except via serial — open
serial monitor on COM5 only when investigating, mindful of DTR-reset.

### v0.5.0 — Relay + Hall hardware introduction

Plan drafted at [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md).
GPIO assignments worked out (RELAY_CH1=25, RELAY_CH2=26, HALL_AO=32,
HALL_DO=33). Hardware on hand. The diagnostic safety-net (coredump +
restart_cause + canary build) is now strong enough to support feature
work — any new failure mode introduced by relay/Hall integration will
auto-capture its own backtrace.

This is the right next big session once #46 Alpha-class is resolved or
acknowledged stable.

### v0.4.22 — #76 long-tail (sub-B/C/D/F/H/I)

Restart-policy hardening leftovers from the cascade-fix entry. Each is
a small firmware change; can be bundled into one release after Charlie
canary completes a full week soak:

- **B.** NVS ring buffer of last-N restart contexts; surface in boot
  announcement as `last_restart_reasons:[...]`. Builds on the
  shipped sub-G primitive.
- **C.** Time-based MQTT_RESTART_THRESHOLD instead of count-based.
  More intuitive; survives backoff-window weirdness.
- **D.** NVS cool-off counter for restart-loops: ≥3 restarts in 10 min
  with reason=mqtt_unrecoverable → fall back to AP/config portal mode
  instead of restarting again.
- **F.** Bump `CONFIG_ESP_TASK_WDT_TIMEOUT_S` 5 s → 12 s. Combines
  with the broker probe + pre-restart publish to give legitimate slow
  reconnects more headroom without losing the safety net.
- **H.** Better backoff math; replace `1→2→4→...→60 s` cap-at-60 with
  jittered exponential.
- **I.** Surface WDT-reset vs SW_CPU_RESET as separate metrics in
  /daily-health and the heap-trajectory tile.

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

When a new release is cut, append a section under "Now" with what shipped and move open items downward. Keep the v0.5.0 / v0.6.0+ sections as the "what's beyond" buffer. SUGGESTED_IMPROVEMENTS.md remains the long-tail backlog; this doc is the synthesized "what we're actually doing soon" view.
