SUGGESTED IMPROVEMENTS — OPEN INDEX
====================================
Short index of every numbered entry. Open items are listed first, with a
one-line title and (where relevant) an ETA / link to the relevant version.
RESOLVED items are listed at the bottom for context but the full text is
in SUGGESTED_IMPROVEMENTS_ARCHIVE.md.

Last index sweep: 2026-04-28 evening (post-v0.4.26 — regrouped 29 OPEN items into 7 thematic clusters; verified each is genuinely open).

Phase 2 R1 verification attempt 2026-04-28 evening surfaced 3 new entries (#86 / #87 / #88) all blocking the original B-group items — see "Phase 2 verification blockers" note below the OPEN list.

To add a new entry:
  - Append the full entry to SUGGESTED_IMPROVEMENTS_ARCHIVE.md with a new
    number (continue the sequence).
  - Add a one-line summary here in the appropriate section.

To resolve an entry:
  - Append a "STATUS: RESOLVED YYYY-MM-DD in vX.Y.Z" line to the entry in
    the archive file.
  - Move its summary line in this index from OPEN to RESOLVED.

────────────────────────────────────────────────────────────

## OPEN

Grouped by theme (numeric IDs preserved). Group letters are stable handles for
session-planning; reorder within a group freely.

### A. RFID / NFC capability (6) — hardware-blocked on Foxtrot bench
  #11   NTAG21x write support (NTAG213 / NTAG215 / NTAG216)
  #12   NTAG424 DNA secure messaging (AES)
  #14   Per-sector key rotation from Node-RED (cmd/rfid/set_key)
  #15   NDEF encoding / decoding helpers
  #16   Preset "program this kind of tag" forms
  #17   LF 125 kHz support (HID, EM4100, T5577)

### B. ESP-NOW ranging + bootstrap (9) — #87/#88/#89 SHIPPED in v0.4.29; Phase 2 R1 verified PIPELINE on Delta↔Foxtrot 2026-04-28 evening; numerical acceptance fails due to room multipath (#37 #5/#6 territory)
  #37   ESP-NOW ranging — A↔B asymmetry causes and mitigations          (asymmetry baseline + systematic Echo 4-rotation test 2026-04-28 evening — root cause ranking REVISED: #2 (antenna pattern) is DOMINANT at 9-14 dB amplitude (was thought Medium); #5 (multipath) confirmed via Delta↔Foxtrot non-monotonic 1m=-75/4m=-78/7.5m=-91 curve; #6 (router 50 cm from Alpha) confirmed; #1 (per-device default mismatch) DOWNGRADED — original "Echo's TX 3-6 dB weak" finding was unlucky-rotation, not hardware. Through-wall pairs naturally average over antenna lobes, so #2 is mostly an LOS-pair problem)
  #38   ESP-NOW ranging — runtime-tunable beacon / publish intervals
  #39   ESP-NOW calibration — multi-point + arbitrary-distance variants  (firmware shipped v0.4.07; pipeline VERIFIED end-to-end 2026-04-28 evening — Delta↔Foxtrot linreg over 3 points produced tx=-73 / n=1.61 / R²=0.718 / RMSE=3.69, all numerically valid but outside #47 acceptance bounds due to multipath)
  #42   ESP-NOW ranging — temporary "active" / "calibrating" / "setup" mode
  #47   Hardware verification of #39 multi-point + #41.7 per-peer calibration  (PIPELINE VERIFIED 2026-04-28 evening on Delta↔Foxtrot; cal_entries went 0→1, calibrated:true visible in /espnow; numerical acceptance criteria (R²≥0.95 / n∈[2,4] / tx∈[-65,-45] / RMSE<2.0) ALL FAILED due to indoor multipath. Open question: tighten criteria to site-specific bounds OR test in clean RF environment)
  #49   Bootstrap protocol does not propagate OTA URL to new siblings
  #86   ESP-NOW calibration sample collection silent failure              (BRAVO-SPECIFIC confirmed 2026-04-28; DEMOTED from HIGH to LOW 2026-04-28 evening autonomous follow-on — Bravo's calibration WORKED after power-cycle + 45 min steady-state uptime, ruling out a code-level branch. Most likely a heap-corruption residue from a prior #78 panic; fixing #78 fixes #86. Workaround: "power-cycle the affected device" — document in operator install guide #40)
  #90   Device PCB mounting orientation has large RF impact — QUANTIFIED 2026-04-28 evening   (pins-down→antennas-up flip: +3 dB mean (~+7-8 dB same-distance equivalent). Echo 4-rotation systematic test shows 9-14 dB rotation swing per peer (LOS), 0 dB through-wall (multipath averages out). Echo HW exonerated — original "weak TX" was unlucky-rotation. Quantifies #37 root cause #2 at 9-14 dB amplitude. #40 install guide should add "if asymmetry > 6 dB, rotate one device 90° before calling install good")
  #91   Investigate ESP32-WROOM-32U with external antenna (U.FL/IPEX)   (procurement candidate — ~$15-30 in parts; head-to-head bench test against current WROOM-32 fleet. Targets #37 root cause #2 + #41 RFID coupling + #90 orientation sensitivity all at once. Open questions: does a single external-antenna node in mixed fleet still benefit asymmetry, do calibration R² values improve, can directional antennas extend usable range > 10m? Regulatory check (FCC/CE/ICASA) needed before production)

### C. Boot / OTA safety net (0)
  (parked 2026-04-28 — #25 needs pioarduino log_printf-wrap fix; #26 needs
   8 MB flash module. Both moved to WONT_DO with revisit triggers.)

### D. Open stability investigations (3) — #78 + #96 SHIPPED in v0.4.28; #97 SHIPPED in v0.4.29; #98 partial in v0.4.30
  #46   Recent Abnormal Reboots — fleet-wide WDT / panic investigation     (Alpha v0.4.20 "IllegalInstruction" decoded 2026-04-28 — was actually the same bad_alloc cascade as #51, fixed by v0.4.22; remaining scope: ≥24 h fleet-soak on v0.4.22+ to confirm closure. SOAK RESET 2026-04-28 evening — Phase 2 R1 cascade event added fresh coredumps. NEW DATA 2026-04-29 morning — int_wdt failure mode observed for first time (Delta + Echo) during the overnight power-failure recovery storm. Adds int_wdt to the panic/wdt vocabulary for #46. Soak inconclusive — interrupted by power event)
  #98   Heartbeat / device-status reconnect needs to be faster after router power failure   (PARTIAL FIX SHIPPED in v0.4.30 — compressed WIFI_BACKOFF_STEPS_MS from saturating at 600s to saturating at 60s. Original schedule fix-option-(c). Followup option-(a) — periodic SSID probe with short-circuit on availability — remains tracked for next stability release. Real-world data point: 4/6 fleet stuck silent for 16+ min post-router-recovery before fix)
  #92   Power-restoration reconnect storm reproduces #78 — 2026-04-29 morning event   (second independent cascade in 24h. Reproduction recipe: bring fleet up steady-state, kill the AP for 30+ s, restore — fleet-wide cascade follows within ~30 s of AP recovery. REFINED 2026-04-29 morning: cold-swapping 3 devices from battery to mains DID NOT cascade — confirms trigger is specifically synchronized fleet-wide WiFi loss, NOT individual device power events. Bug lives in shared-state / contention paths (lwIP PCB allocator, AsyncTCP event-queue dispatch, WiFi driver TX cleanup) — anywhere multiple connections converge on shared mutable state. Solo reconnects thread the needle without hitting it. Bench-debuggable: attach serial to Bravo or Delta before the next AP cycle. Battery does NOT help. Calibration NVS persistence confirmed across cascade)

### E. CI / security gates (0)
  (all closed — #27 RESOLVED, #63 RESOLVED, #68 WONT_DO)

### F. Hardware bench / variants (1)
  #72   Bench-supply voltage stress testing rig                      (was #59 cascade-session)

### G. Docs / process / long-tail closure (4) — #95/#97 SHIPPED in v0.4.29
  #33   Versioned MQTT topic prefixes                                  (design doc shipped 2026-04-28 as docs/TOPIC_VERSIONING_DESIGN.md; implementation deferred to v1.0 / fleet > 10 / first breaking schema change)
  #40   Operator install guide — ESP32-WROOM antenna orientation       (doc shipped 2026-04-28 as docs/OPERATOR_INSTALL_GUIDE.md; updated 2026-04-29 with cascade-recovery / antenna-orientation / #86 power-cycle notes; entry kept open until field-validated)
  #76   Recovery + reporting hardening — restart policy redesign     (was #65 cascade-session; all sub-items A/B/C/D/E/F/G/H/I now code-shipped — full closure pending v0.4.24+ fleet-validation soak)
  #85   End-of-session doc-sweep tooling                              (partial fix 2026-04-28: A + C shipped via CLAUDE.md + AUTONOMOUS_PROMPT_TEMPLATE; B sub-tool prototype shipped 2026-04-28 PM as tools/dev/end-of-session-sweep.sh — closure pending 2-3 sessions of validation that the 4 checks catch real gaps without false positives)
  #93   Production firmware is SERIAL-SILENT — decide whether to instrument   (2026-04-29 morning serial captures of Charlie + Alpha both produced 0 bytes during steady-state. Decision required: status quo (A), periodic heartbeat-to-serial (B, recommended), canary-only watermark prints (C), or on-demand cmd/diag/serial_dump (D, recommended bundled with B). Theme alignment with v0.4.29 ranging UX bundle — "make firmware self-document its state". Cost-benefit cheap; impact is on diagnostic ergonomics not stability)
  #94   ESP-NOW reinit on WiFi reconnect + LED state-machine MQTT_LOST event   (PATCHED + SHIPPED in v0.4.27 — fleet flashed 2026-04-29 morning. Targets 2 distinct bugs surfaced by bench-debug AP-cycle session: (a) silent ESP-NOW driver breakage post-WiFi-reconnect, (b) LED state-machine stuck on MQTT_HEALTHY when MQTT drops while WiFi up. Open until 24h+ field-soak validation surfaces no regression)

### H. LED / visual diagnostics (1) — new group filed 2026-04-29 PM
  #99   Status-LED blink patterns are not diagnostic — make them more useful   (filed 2026-04-29 PM; same incident as #98 — all 6 devices had heartbeat blink even when 4 were MQTT-stuck. Proposed: distinct patterns for boot/pre-WiFi (10Hz), WiFi-up-MQTT-down (1Hz), MQTT_HEALTHY (slow breathing), AP_MODE (double-blink). led.h already has the state slots — just need distinct waveforms. MEDIUM priority; pairs with #98 "self-document state" theme)

### I. Tooling speed / ergonomics (1) — new group filed 2026-04-29 PM
  #100  ota-rollout.sh — speed up beyond adaptive timeout                    (PARTIAL FIX SHIPPED 2026-04-29 PM — adaptive timeout (#2), skip already-up-to-date devices (#3), pre-validate broker+manifest (#6), skip safety gap on last device (#5 partial). Test on v0.4.30 fleet went from 12.5 min → 14 s for a no-op rollout. REMAINING: phased parallel rollout (#1, biggest theoretical win — 1→2→3 wave pattern from operator suggestion) + persistent heartbeat subscription (#4). Bundle for v0.4.31 tooling release. MEDIUM priority)

  Total open: 27  (A6 + B9 + C0 + D3 + E0 + F1 + G6 + H1 + I1) — net delta 2026-04-29 PM: -5 from v0.4.29 (#87/#88/#89/#95/#97 RESOLVED), +3 from incidents (#98 D, #99 H, #100 I), #98 + #100 partial-fixed in v0.4.30 + tooling. Detailed entries in SUGGESTED_IMPROVEMENTS_ARCHIVE.md

  Phase 2 R1 verification 2026-04-28 evening — outcome:
    Goal was no-flash verification of #47 / #39 against the operator's
    bench rig (1 m triangle + Foxtrot at 6.5/6.5/7.5 m). Pre-flight
    surfaced #88 (ranging off on bench devices); manual republish of
    cmd/espnow/ranging "1" got /espnow flowing. Asymmetry baseline
    captured cleanly and feeds #37 (#1/#2/#5/#6 root causes confirmed).

    Bravo as anchor → #86 surfaced (silent zero-sample collection).
    Switch to Delta as anchor → calibration pipeline WORKED. After a
    mid-sweep cascade event (5 fresh coredumps across the fleet —
    Alpha panic, Bravo/Delta/Echo/Foxtrot async_tcp/tiT/loopTask in
    various combinations; recovery via operator power-cycle), the
    cautious retry on Delta succeeded:
      Buffer:   (4.0m, -78) → (1.0m, -75) → (7.5m, -91)
      Linreg:   tx_power_dbm=-73, path_loss_n=1.61
                R²=0.718, RMSE=3.69 dB
      Per-peer: cal_entries 0→1, calibrated:true on Foxtrot in /espnow

    Pipeline VERIFIED end-to-end (#39 firmware + #47 procedure ✓).
    Numerical acceptance bounds from #47 ALL violated due to indoor
    multipath (non-monotonic curve) — environmental finding, not a
    pipeline issue. #47 needs disposition decision: accept this rig
    as documented site-specific failure, OR re-run in cleaner RF
    environment, OR loosen the acceptance criteria to site-aware
    bounds. Pairs naturally with #37(c) Node-RED asymmetry badge —
    a per-pair quality indicator would surface this kind of
    environmental issue without requiring acceptance criteria.

────────────────────────────────────────────────────────────

## WONT_DO (intentional non-actions, full text in docs/WONT_DO.md)

  #6    OTA HTTPS certificate pinning                       (parked 2026-04-27 — internal-IoT threat model)
  #7    MQTT-over-TLS to the broker                         (parked 2026-04-27 — private LAN)
  #13   UID-clone / "Chinese backdoor" MIFARE support       (parked 2026-04-27 — no use case)
  #18   NFC phone / card emulation                          (parked 2026-04-27 — ESP32 cannot emulate ISO 14443A)
  #8    Sibling ESP-NOW "come back online" broadcast        (parked 2026-04-28 — REJECTED in v0.3.15 plan; siblings deinit ESP-NOW in AP mode)
  #9    Time-limited AP mode → reboot                       (parked 2026-04-28 — REJECTED in v0.3.15 plan; background STA scan supersedes)
  #10   "Has ever successfully connected" NVS flag          (parked 2026-04-28 — REJECTED in v0.3.15 plan; subsumed by indefinite OPERATIONAL backoff)
  #82   Audit tracking docs for split pattern               (parked 2026-04-27 — neither candidate fits the convention)
  #68   Node-RED: enable adminAuth in settings.js           (parked 2026-04-28 — same private-LAN threat-model logic as httpNodeAuth WONT_DO #2)
  #25   Bootloader rollback safety net                      (parked 2026-04-28 — pioarduino log_printf-wrap blocker; revisit when upstream fix lands or fleet pushes builds without serial access; see WONT_DO #8)
  #26   Recovery partition app                              (parked 2026-04-28 — doesn't fit on 4 MB flash; revisit when fleet migrates to 8 MB modules or a production device is bricked without serial recovery; see WONT_DO #9)

────────────────────────────────────────────────────────────

## RESOLVED (full text in archive)

  #1    Upgrade GitHub Actions to Node.js 24                          (addressed 2026-04-22)
  #2    JTAG debugging setup                                          (was audit item 20)
  #3    Reproducible builds                                           (was audit item 21)
  #4    Dependency SBOM + supply-chain scan                           (was audit item 22)
  #5    Local PlatformIO validation                                   (addressed 2026-04-22)
  #24   OTA transient freeze — watchdog-safe progress timeout         (addressed 2026-04-22 in v0.3.33-era; OTA_PROGRESS_TIMEOUT_MS + trigger-time heap snapshot; index audit cleanup 2026-04-28)
  #28   NVS / static-string lifetime audit + naming convention        (resolved 2026-04-23 in v0.4.02 — docs/STRING_LIFETIME.md + lib_api_assert.h shipped; re-audit 2026-04-28 confirmed no new dangerous .c_str() callsites)
  #29   WDT-heartbeat audit for all blocking I/O                      (resolved 2026-04-28 — see docs/SESSIONS/WDT_AUDIT_2026_04_28.md)
  #32   Heap-headroom gate at boot for each subsystem                 (resolved 2026-04-28 — heapGateOk() helper + per-subsystem thresholds gating MQTT init, BLE init, TLS keygen)
  #19   Per-LED addressing                                            (resolved 2026-04-28 — cmd/led pixel + pixels handlers + LedState::MQTT_PIXELS where renderer skips fill_solid; _leds[] is the source of truth. Auto-commit on cmd/led pixel for one-shot UX.)
  #20   Scene / preset save-to-device                                  (resolved 2026-04-28 — NVS namespace led_scenes with up to 8 named slots; cmd/led scene_save/load/delete/list. Captures _leds[]+brightness, restores on demand.)
  #21   Group / broadcast LED commands                                 (resolved 2026-04-28 — Enterprise/Site/broadcast/led topic mirrors cmd/led semantics. Subscribed in onMqttConnect, routed through handleLedCommand.)
  #22   Scheduled / time-of-day LED automation                         (resolved 2026-04-28 — led_schedule.h with NTP-synced minute-poll, 8 NVS-persisted slots, action-as-cmd/led-JSON re-fed at fire time. cmd/led sched_add/remove/list/clear.)
  #23   OTA / heartbeat LED override from Node-RED                    (resolved 2026-04-28 — cmd/led override with auto-revert via _ledOverrideEndMs. New "alarm" + "warn" anim names. duration_ms 0 = untimed.)
  #31   Pin LED + RFID tasks to Core 1                                (resolved 2026-04-28 — verified arduino-esp32 v3.x puts loopTask + RFID on Core 1 via CONFIG_ARDUINO_RUNNING_CORE=1, ws2812Task explicitly pinned Core 1; WiFi/AsyncTCP stay Core 0. Original concern was a v2.x default that no longer applies.)
  #34   Captive portal DNS responder                                  (resolved 2026-04-28 — Phase 1 DNS hijack + Phase 2 port-80 redirector both shipped; second httpd instance on :80 with wildcard 302 → https://192.168.4.1/)
  #30   AsyncTCP fork swap (marvinroger → mathieucarbou)              (resolved 2026-04-27 in v0.4.14)
  #41   Hardware finding — breakout + RFID-RC522 antenna distortion   (resolved as documented finding 2026-04-25; informs #37/#40)
  #43   Local build leaves firmware_version field EMPTY               (addressed v0.4.10)
  #44   Addressable LED status colors (green/yellow/red) not lighting
  #45   Fleet-control buttons (OTA/Restart/Firmware) need stagger    (addressed v0.4.10 — Node-RED hb_cmd_fn/hb_restart_fn patched)
  #48   Device UUID drift — Delta and Echo had unexpected UUIDs       (root cause 2026-04-28 — RNG-pre-WiFi pseudo-random; see docs/SESSIONS/UUID_DRIFT_AUDIT_2026_04_28.md; fix bundles with v0.5.0)
  #50   esptool v5.2 erase-flash does NOT wipe NVS                    (resolved 2026-04-25 — was DTR-induced second-boot, reconfirmed 2026-04-27)
  #51   v0.4.10 stability regression — LED MQTT_HEALTHY hooks suspect (resolved 2026-04-27 in v0.4.16; full root cause closed v0.4.22)
  #52   Node-RED file logging configured                              (resolved Tier-1 T1.1 2026-04-27)
  #53   Per-heartbeat LOG_HEAP for fleet-wide leak surveillance       (firmware part shipped v0.4.11; dashboard tile downstream)
  #55   AsyncMqttClient malformed-packet counter                      (resolved 2026-04-28 in v0.4.23 — mqtt_disconnects + mqtt_last_disconnect heartbeat fields)
  #56   Re-implement MQTT_HEALTHY safely via deferred-flag pattern    (resolved 2026-04-27 in v0.4.13)
  #57   Install host gcc/g++ to enable native unit tests              (resolved 2026-04-27)
  #58   Fix daily_health_config.json local_clone path                 (resolved 2026-04-27)
  #59   Root .gitignore                                               (resolved 2026-04-27)
  #60   Root .gitattributes (CRLF normalisation)                      (resolved 2026-04-27)
  #61   Mosquitto: add auth (passwd + ACL)                            (2026-04-26 audit; intentionally deferred)
  #62   LICENSE file                                                  (addressed 2026-04-26)
  #64   Root README.md                                                (resolved 2026-04-27)
  #65   esp32_node_firmware/include/build_config.h stub               (resolved 2026-04-27)
  #66   .claude/commands/ — operational shortcuts                     (resolved 2026-04-27)
  #67   Node-RED project package.json — declare custom node deps      (2026-04-26 audit)
  #69   Wakeup vs persistent-monitor preemption                       (resolved 2026-04-28 — sub-E shipped via tools/dev/ota-rollout.sh + ota-monitor.sh; cadence rule via #84)
  #77   Adaptive OTA stagger interval                                 (resolved 2026-04-28 — A/B/C all shipped under #79 via tools/dev/ota-rollout.sh; fixed-gap baseline obsolete)
  #73   Silent-failure watcher (tools/silent_watcher.sh)              (was #60 cascade-session; shipped)
  #74   IPv6Address.h support — moot                                  (mathieucarbou/AsyncTCP v3.3.2 chosen, no shim needed)
  #75   Chaos-testing framework — promote tools/chaos/                (resolved 2026-04-28 — release-smoke.sh wrapper shipped + GH-Actions infeasibility documented)
  #27   Library-API regression test in CI                             (resolved 2026-04-23 in v0.4.02 — recorded retroactively 2026-04-28; lib_api_assert.h static_asserts run on every esp32dev CI build)
  #35   Operational practice: canary OTA pattern                      (resolved 2026-04-28 — codified in docs/CANARY_OTA.md; Charlie has been the dedicated canary since 2026-04-27)
  #36   Operational practice: heartbeat / boot-reason monitoring      (resolved 2026-04-28 — codified in docs/MONITORING_PRACTICE.md)
  #63   Add trufflehog secrets-scan job to build.yml                  (resolved 2026-04-28 — secrets-scan job added to build.yml; first run 25052020093 GREEN)
  #71   Per-device feature-subset firmware variants                   (resolved 2026-04-28 — first cut + CI matrix; build-variants job greens esp32dev_minimal + esp32dev_relay_hall on every push)
  #79   Version-update watcher + ack-driven OTA                       (was #68 cascade-session; shipped tools/dev/{ota-rollout,version-watch}.sh)
  #80   -dev suffix breaks OTA upgrade path                           (was #70 cascade-session; resolved v0.4.18+v0.4.20)
  #81   Renumbering pass on archive (resolve #58–#70 collisions)      (resolved 2026-04-27)
  #83   Mosquitto log file frozen after blip-watcher service restarts (resolved 2026-04-28 — size-cap rotation in rotate-log.ps1)
  #84   Agent post-action verification gap                            (resolved 2026-04-28 — discipline rule + ota-monitor.sh + cadence rule)

  #54   Stack-canary build (CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2)         (resolved 2026-04-29 with positive evidence — Charlie canary 35 h+ soak across multiple #78 cascade events without firing; full text in archive)
  #78   AsyncTCP _error path race                                          (resolved 2026-04-29 in v0.4.28 — root-cause symbolic decode + cascade-window publish guard; full text in archive)
  #96   Long-outage AP-mode loop — phantom restart-loop signature          (resolved 2026-04-29 in v0.4.28 — sub-A ap_portal pushes "ap_recovered" + sub-B mqttScheduleRestart idempotent; full text in archive)

  #87   Calibration UX: silent during sample-collection waiting period    (resolved 2026-04-29 in v0.4.29 — 1 Hz "calib":"waiting" heartbeat in espnowRangingLoop while non-IDLE; full text in archive)
  #88   ESP-NOW ranging defaults to OFF / persistence relies on retained MQTT (resolved 2026-04-29 in v0.4.29 — AppConfig.espnow_ranging_enabled NVS field + boot-time apply; full text in archive)
  #89   ESP-NOW calibration multi-point buffer is RAM-only — lost on reboot (resolved 2026-04-29 in v0.4.29 — visibility-only fix: cal_points_buffered + ranging_enabled in /espnow JSON; full text in archive)
  #95   PIO upload hangs with UnicodeEncodeError on Windows cp1252 console (resolved 2026-04-29 in v0.4.29 — tools/dev/pio-utf8.sh wrapper + CLAUDE.md note; full text in archive)
  #97   Auto-OTA-during-cascade-recovery gate                              (resolved 2026-04-29 in v0.4.29 — otaCheckNow gates on mqttGetLastDisconnectMs with OTA_CASCADE_QUIET_MS = 300_000 ms; full text in archive)

  Total resolved: 61
