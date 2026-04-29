# Next session plan

Refreshed 2026-04-29 PM after v0.4.29 shipped (ranging UX +
auto-OTA-recovery + PIO wrapper bundle). Previous next-session
candidate (v0.4.28 #46 soak) is folded into this plan because
v0.4.29 is the better soak target — same well-understood cascade
fixes plus new visibility code paths.

## State at end of session (2026-04-29 ~PM SAST)

| | |
|---|---|
| Master HEAD | v0.4.29 tag (CI building + gh-pages auto-update) |
| Fleet | 6/6 on v0.4.29 (target, post-OTA-rollout) — soak begins ~20:00 SAST |
| Backlog | OPEN **24**, RESOLVED **61**, WONT_DO **11** (-5 net this session: #87/#88/#89/#95/#97 RESOLVED) |
| Released today | v0.4.27 (morning #94), v0.4.28 (#78 + #96), v0.4.29 (#87/#88/#89/#95/#97) — three releases in one day |
| Validation | Native 105/105; Alpha USB-flash + M2 30s blip PASS via blip-watcher (no reboot, mqtt_disconnects 0→1, heap_largest unchanged 81908) |

## What we shipped today (v0.4.29)

- **#87** — Calibration UX: 1 Hz "calib":"waiting" heartbeat in
  `espnowRangingLoop` while `_calibState != IDLE`. Surfaces
  #86-class silence in 1 s instead of 120 s.
- **#88** — `AppConfig.espnow_ranging_enabled` (NVS key `en_rng`,
  default 0). Boot-time apply via `espnowRangingSetEnabled()` right
  after `espnowResponderStart()`. Devices that miss their retained
  `cmd/espnow/ranging` MQTT message come up in the same on/off
  state as before reboot.
- **#89** — `/espnow` JSON adds `cal_points_buffered` and
  `ranging_enabled` so dashboards can warn on uncommitted points
  before reboot. Visibility-only fix; full NVS persistence of the
  multi-point buffer deliberately deferred.
- **#95** — `tools/dev/pio-utf8.sh` wrapper exporting
  `PYTHONIOENCODING=utf-8 PYTHONUTF8=1` before exec'ing pio.
  CLAUDE.md "Build & Test" updated.
- **#97** — `otaCheckNow()` early-returns
  `stage:"cascade_quiet"` OTA_FAILED if `mqttGetLastDisconnectMs()`
  is within `OTA_CASCADE_QUIET_MS` (300_000 ms = 5 min default).
  Mirrors v0.4.28 publish-guard pattern.

Build: 1647024 bytes flash (+280 vs v0.4.28), RAM 22.3 % unchanged.

## Recommended next session

Soak begins this evening at ~20:00 SAST and runs overnight. Morning
session = closure paperwork + decision on next direction.

### A. Close #46 if soak is clean
Per CLAUDE.md "Soak windows" guidance: 8-12 h overnight on v0.4.29
covers the v0.4.28 cascade-window guard validation that #46 has
been blocked on since 2026-04-28 evening's Phase 2 R1 cascade
event reset the soak. Closure criteria (per CLAUDE.md template):
- No new `/diag/coredump` payloads with fresh `app_sha_prefix`.
- No `silent_watcher.sh` LWT-or-panic alerts.
- `heap_largest` stable (no monotonic decline > 5 % / hour).
- `mqtt_disconnects` not climbing during the window.
- Boot announcement at end shows expected `restart_cause`
  (empty for clean uptime).

If clean → mark **#46 RESOLVED in v0.4.29** (the v0.4.22 + v0.4.28 +
v0.4.29 bundle finally closes the long-tail abnormal-reboot
investigation). #92 stays open as the cascade-trigger entry for
future investigation.

### B. v0.5.0 hardware bring-up — relay + Hall on Bravo
Plan unchanged at [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md).
GPIO assignments worked out; hardware on hand. Bravo currently
bench-attached on COM5 (per 2026-04-29 PM PnP probe — operator
re-attached during today's chaos prep). Blocker: #48 UUID drift fix
must ship first. With #46 closure planned for tomorrow morning,
this becomes the next logical big session.

### C. #93 production-serial-instrumentation
Decide between status quo (A), 60 s heartbeat-to-serial (B,
recommended), canary-only watermark prints (C), or on-demand
cmd/diag/serial_dump (D, recommended bundled with B). Visibility-only,
pairs with v0.4.29's "make firmware self-document its state" theme.
Low-risk shippable as v0.4.30.

### D. #91 ESP32-WROOM-32U + external antenna procurement
Operator orders parts (~$15-30). Bench-test against current WROOM-32
fleet for asymmetry / RF range / orientation sensitivity. Targets
#37 root cause #2 + #41 RFID coupling + #90 orientation. Regulatory
check (FCC/CE/ICASA) needed before production.

### E. Phase 2 ranging cluster — multipath / criteria adjustment
#37, #38, #39, #42, #47, #49, #86, #90, #91 are all open in this
group. Most need either bench time, procurement, or environmental
re-test (clean RF). v0.4.29's #87/#88/#89 visibility wins make any
future on-bench session smoother. Bundle option: take #38 (runtime-
tunable beacon intervals) + #42 (active/calibrating/setup mode) as
a small follow-on after #46 closes.

## Bench state at session end

- Alpha (COM4) on v0.4.29.0 — primary test target, M2 PASS verified
  pre-rollout.
- Bravo (COM5) re-attached — was off-bench until v0.5.0; now
  bench-accessible.
- Charlie/Delta/Echo/Foxtrot — fleet positions unchanged from v0.4.28
  morning state. All eligible for v0.4.29 OTA rollout.
- Charlie canary — closed 2026-04-29 morning with #54 RESOLVED;
  Charlie is back on release firmware.

## Won't-do at next-session start

- Trigger another cascade-class M3 blip without symbolic-decode
  apparatus ready (per the v0.4.28 lesson — non-determinism means
  single trigger can't validate).
- Open new #78-class investigations without fresh evidence —
  cascade-window guard + auto-OTA-recovery gate are the production
  mitigations; vendored AsyncTCP patch deferred unless soak surfaces
  a residual cascade.
- Mass-flash without checking COM-port assignments (per CLAUDE.md
  rule). PnP probe before every flash.

## Open questions for operator

- v0.4.29 is in flight as the soak target. Acceptable to skip a
  separate v0.4.28 soak window since v0.4.29 is additive over
  v0.4.28 and validates strictly more code surface?
- v0.5.0 hardware (Bravo relay + Hall wiring) — ready for
  tomorrow's session, or operator wiring still pending?
- The blip-watcher mechanism worked end-to-end this session
  (memory file `mosquitto_blip_watcher.md` saved). Any preference
  on auto-running blips during off-hours (with pre-armed silent_watcher
  for any abnormal boot signature)?
