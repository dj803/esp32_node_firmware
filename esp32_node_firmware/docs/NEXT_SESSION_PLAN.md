# Next session plan

Drafted 2026-04-28 evening — wrapping a long autonomous block that shipped
**three releases same day** (v0.4.24 / v0.4.25 / v0.4.26):

- **v0.4.24** — #76 sub-C/D/I (time-based MQTT unrecoverable + restart-
  loop AP fallback + /daily-health WDT/SW categorisation) + #75 chaos
  framework + #34 Phase 1 captive DNS + #40 operator install guide +
  #24/#28/#77 audit-stale RESOLVED.
- **v0.4.25** — #32 heap-headroom gates at MQTT/BLE/TLS init + #34 Phase 2
  (port-80 redirector) + ota-rollout EXCLUDE_UUIDS + #33 versioned-topic
  design doc + ota-rollout TIMEOUT_PER_DEVICE_S 180 → 300.
- **v0.4.26** — LED feature bundle: #19 per-LED addressing + #20 scene
  save-to-device + #21 broadcast LED commands + #22 time-of-day schedule
  + #23 timed override + #31 task pin verified. Plus `docs/LED_COMMANDS.md`
  reference + `cmd/led "test"` self-test on master pending next cut.

Open: 39 → 29. Resolved: 32 → 47. WONT_DO unchanged at 8.

## State at end of session (2026-04-28 evening)

| | |
|---|---|
| Master HEAD | `feat(LED): cmd/led "test" — installation self-test pattern` (b5cc195) on master pending the next cut |
| Fleet | 5/5 production on **v0.4.26** (Alpha, Bravo, Delta, Echo, Foxtrot); **Charlie** sticky on v0.4.20.0 canary per Q1 decision |
| Master vs fleet | Self-test feature is one commit ahead of v0.4.26 — fleet doesn't have it yet. Cut v0.4.27 if/when more bundle-able items accumulate, or stay on v0.4.26 indefinitely. |
| Charlie soak | Up 15+ h since the AP-outage recovery. Heap healthy. Retained `/diag/coredump` from #78 InstructionFetchError preserved. |
| Backlog | OPEN 29, RESOLVED 47, WONT_DO 8 |

## Bench state (2026-04-28 late afternoon)

- **Alpha on COM4** — operator swapped 2026-04-28 afternoon to put the
  WS2812-equipped device on a serial-accessible bench port. Unblocks
  visual + serial validation of LED-feature work.
- **Charlie on COM5** — sticky canary on v0.4.20.0, do not disturb
  (preserves the retained `/diag/coredump` for #78 forensics).
- **Bravo off-bench** — operator will re-attach for the v0.5.0 relay +
  Hall hardware bring-up session.

## Recommended next session — A/B/C menu

### A. v0.5.0 hardware bring-up (relay + Hall on Bravo) — ~2 h

The natural next big milestone. Plan unchanged from earlier sessions
(see [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md)). Pre-session
operator action: re-attach Bravo to the bench, wire relay (GPIO 25/26)
+ Hall (GPIO 32/33) per the GPIO inventory.

Steps once Bravo is wired:
1. Capture Bravo's pre-flash state (firmware, uptime, boot_reason).
2. USB-flash `esp32dev_relay_hall` to Bravo on COM4 (will reassign
   COM4 ↔ Alpha — operator should swap cables back).
3. Validate `cmd/relay` (click + retained state + NVS restore).
4. Validate `cmd/hall/zero` + `cmd/hall/config` + `telemetry/hall` +
   `telemetry/hall/edge`.
5. M1 + M2 chaos to confirm no new failure surface.
6. Tag **v0.5.0** if validation lands clean. The accumulated `cmd/led
   "test"` from this session rides along in v0.5.0.

### B. LED self-test validation + dashboard tile — ~1 h (operator-led)

The cmd/led `test` self-test is on master but not yet OTA'd. Cutting
v0.4.27 just for it is excessive; bundle into v0.5.0 or whatever
ships next. Once it's on the fleet, operator can fire `mosquitto_pub
-t '<root>/<uuid>/cmd/led' -m '{"cmd":"test"}'` against Alpha and
visually confirm the RGBW + per-pixel walk.

If the dashboard work is desired (#36 — DO NOT for autonomous):
operator builds a Vue tile in Node-RED that exposes the new schema:
brightness slider + color picker + scene dropdown + schedule editor.
Reference: [LED_COMMANDS.md](LED_COMMANDS.md) is the single-source
schema doc.

### C. #78 AsyncTCP race deep-dive — ~3 h diagnostic + design

Charlie's earlier canary trip (12.4 h before the AP outage) ruled out
stack overflow for the async_tcp `InstructionFetchError` family. The
remaining hypotheses are use-after-free in `_error` path or task-
priority race between async_tcp service task and lwIP TCP timer.
Defer until more diagnostic data accumulates — premature without
another panic capture on v0.4.26 to compare.

## Items still in OPEN that DON'T match A/B/C

Hardware-blocked (need bench + sensors):
- #11/#12/#14-17 RFID feature work (Foxtrot bench)
- #37/#38/#39/#42 ESP-NOW ranging
- #47 hardware verification

Pioarduino-blocked: #25 bootloader rollback (upstream issue).

DO-NOT-for-autonomous (operator session):
- #27 lib-API CI gate (workflow change)
- #36 dashboard tile (Node-RED operator-visible)
- #63 trufflehog CI (workflow change)
- #68 Node-RED adminAuth
- #71 full variant infra (substantial; needs operator scoping)
- #75 chaos CI hook (workflow change — runner+scripts already shipped)

Investigation/defer:
- #46 abnormal reboots — Alpha int_wdt'd in this morning's AP outage,
  no follow-up action without another data point.
- #49 OTA URL bootstrap propagation — deferred to v0.5.0 bundle.
- #54 Charlie canary soak — ongoing.
- #72 bench-supply rig — needs operator hardware.
- #78 AsyncTCP race — see C above.
- #85 doc-sweep tooling B sub-tool — partial fix already shipped.

## Won't do at session start

- New autonomous releases without operator scoping. Three releases in
  one day is a lot; let the v0.4.26 fleet soak before the next cut.
- Anything DO-NOT.
