SUGGESTED IMPROVEMENTS — OPEN INDEX
====================================
Short index of every numbered entry. Open items are listed first, with a
one-line title and (where relevant) an ETA / link to the relevant version.
RESOLVED items are listed at the bottom for context but the full text is
in SUGGESTED_IMPROVEMENTS_ARCHIVE.md.

Last index sweep: 2026-04-28 afternoon (autonomous followups session — #76 sub-C/D/I + #75 chaos framework code-shipped on master, awaits v0.4.24 cut + fleet recovery before validation; #24 + #28 audit-stale entries moved to RESOLVED).

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

  #11   NTAG21x write support (NTAG213 / NTAG215 / NTAG216)
  #12   NTAG424 DNA secure messaging (AES)
  #14   Per-sector key rotation from Node-RED (cmd/rfid/set_key)
  #15   NDEF encoding / decoding helpers
  #16   Preset "program this kind of tag" forms
  #17   LF 125 kHz support (HID, EM4100, T5577)
  #19   Per-LED addressing
  #20   Scene / preset save-to-device
  #21   Group / broadcast LED commands
  #22   Scheduled / time-of-day LED automation
  #23   OTA / heartbeat LED override from Node-RED
  #25   Bootloader rollback safety net (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)
  #26   Recovery partition app
  #27   Library-API regression test in CI
  #31   Pin LED + RFID tasks to Core 1
  #32   Heap-headroom gate at boot for each subsystem
  #33   Versioned MQTT topic prefixes
  #34   Captive portal DNS responder                                  (Phase 1 DNS hijack code-shipped 2026-04-28; Phase 2 port-80 redirector still open)
  #35   Operational practice: canary OTA pattern
  #36   Operational practice: heartbeat / boot-reason monitoring
  #37   ESP-NOW ranging — A↔B asymmetry causes and mitigations
  #38   ESP-NOW ranging — runtime-tunable beacon / publish intervals
  #39   ESP-NOW calibration — multi-point + arbitrary-distance variants
  #40   Operator install guide — ESP32-WROOM antenna orientation
  #42   ESP-NOW ranging — temporary "active" / "calibrating" / "setup" mode
  #46   Recent Abnormal Reboots — fleet-wide WDT / panic investigation
  #47   Hardware verification of #39 multi-point + #41.7 per-peer calibration
  #49   Bootstrap protocol does not propagate OTA URL to new siblings
  #54   Stack-canary build (CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2)
  #63   Add trufflehog secrets-scan job to build.yml                 (2026-04-26 audit)
  #68   Node-RED: enable adminAuth in settings.js                    (2026-04-26 audit)
  #71   Per-device feature-subset firmware variants                  (was #58 cascade-session)
  #72   Bench-supply voltage stress testing rig                      (was #59 cascade-session)
  #75   Chaos-testing framework — promote tools/chaos/               (was #64 cascade-session; scripts + runner shipped 2026-04-28, CI hook still open)
  #76   Recovery + reporting hardening — restart policy redesign     (was #65 cascade-session; all sub-items A/B/C/D/E/F/G/H/I now code-shipped — full closure pending v0.4.24 cut + fleet-validation)
  #77   Adaptive OTA stagger interval                                (was #66 cascade-session)
  #78   AsyncTCP _error path race — replace stack or patch library   (was #67 cascade-session; v0.4.16 mitigates, latent bug confirmed 2026-04-27)
  #85   End-of-session doc-sweep tooling                              (partial fix 2026-04-28 in CLAUDE.md + AUTONOMOUS_PROMPT_TEMPLATE; B sub-tool deferred)

  Total open: 38

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
  #73   Silent-failure watcher (tools/silent_watcher.sh)              (was #60 cascade-session; shipped)
  #74   IPv6Address.h support — moot                                  (mathieucarbou/AsyncTCP v3.3.2 chosen, no shim needed)
  #79   Version-update watcher + ack-driven OTA                       (was #68 cascade-session; shipped tools/dev/{ota-rollout,version-watch}.sh)
  #80   -dev suffix breaks OTA upgrade path                           (was #70 cascade-session; resolved v0.4.18+v0.4.20)
  #81   Renumbering pass on archive (resolve #58–#70 collisions)      (resolved 2026-04-27)
  #83   Mosquitto log file frozen after blip-watcher service restarts (resolved 2026-04-28 — size-cap rotation in rotate-log.ps1)
  #84   Agent post-action verification gap                            (resolved 2026-04-28 — discipline rule + ota-monitor.sh + cadence rule)

  Total resolved: 38
