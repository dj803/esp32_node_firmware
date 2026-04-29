# `docs/` — index

Layout grouped by lifecycle (not by topic). See
[TRACKING_DOC_CONVENTION.md](TRACKING_DOC_CONVENTION.md) for the
underlying convention.

Last sweep: 2026-04-29 (post-v0.4.31; introduced `Operator/` folder for field-facing reference).

## Active reference (foundational, read top-to-bottom)

- [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md) — the canonical firmware spec.
- [ESP32_FAILURE_MODES.md](ESP32_FAILURE_MODES.md) — taxonomy of why
  classic-ESP32 deployments stop working, with this fleet's history.
- [TWDT_POLICY.md](TWDT_POLICY.md) — task-watchdog policy and
  deferred-flag pattern for callbacks that need to do work.
- [memory_budget.md](memory_budget.md) — flash / IRAM / DRAM
  breakdowns at known builds.
- [STRING_LIFETIME.md](STRING_LIFETIME.md) — codebase-wide convention
  for lifetimes of strings passed to async APIs.

## Subsystem feature docs

- [led_control.md](led_control.md) — WS2812 strip API and command
  reference.
- [rfid_tag_profiles.md](rfid_tag_profiles.md) — MIFARE / NTAG profile
  conventions, command formats, status codes.
- [sleep.md](sleep.md) — modem-sleep / deep-sleep wake-up flow.

## Operational guides (engineer-facing)

- [AUTONOMOUS_PROMPT_TEMPLATE.md](AUTONOMOUS_PROMPT_TEMPLATE.md) — how
  to write a multi-hour AFK prompt for the agent.
- [COREDUMP_DECODE.md](COREDUMP_DECODE.md) — runbook for decoding
  `/diag/coredump` backtraces with `xtensa-esp32-elf-addr2line`
  (worked examples: #46, #78).
- [CHAOS_TESTING.md](CHAOS_TESTING.md) — M1-M4, EN1, O2, I1 chaos test
  procedures + framework proposal.
- [TOOLING_INTEGRATION_PLAN.md](TOOLING_INTEGRATION_PLAN.md) — Tier 1
  / Tier 2 tooling roadmap.
- [TRACKING_DOC_CONVENTION.md](TRACKING_DOC_CONVENTION.md) — meta-doc
  on managing tracking docs.

## Operator-facing reference (`Operator/`)

Field- and bench-facing docs for "what do I do when…" questions live
in [`Operator/`](Operator/README.md). Engineer-facing docs (above)
stay in `docs/`; the operator docs link out to them when deeper
detail is needed. See [`Operator/README.md`](Operator/README.md) for
the full index. Quick pointers:

- [Operator/INSTALL_GUIDE.md](Operator/INSTALL_GUIDE.md) — antenna +
  power layout rules of thumb (was `OPERATOR_INSTALL_GUIDE.md`; tracks #40).
- [Operator/HARDWARE_WIRING.md](Operator/HARDWARE_WIRING.md) — adding
  expansion modules (4x4 NeoPixel matrix, 2-ch relay) to a breakout.
- [Operator/AP_MODE_SETUP.md](Operator/AP_MODE_SETUP.md) — first-boot
  bootstrap walkthrough.
- [Operator/FLEET_OPS.md](Operator/FLEET_OPS.md) — daily-driver
  one-liners (fleet snapshot, OTA-rollout, broker log, blip).
- [Operator/MONITORING_PRACTICE.md](Operator/MONITORING_PRACTICE.md) —
  heartbeat / boot-reason monitoring practice (was top-level; tracks #36).
- [Operator/CANARY_OTA.md](Operator/CANARY_OTA.md) — canary-then-fleet
  OTA pattern (was top-level; tracks #35).
- [Operator/TROUBLESHOOTING.md](Operator/TROUBLESHOOTING.md) — symptom
  → action playbook.
- [Operator/ABNORMAL_REBOOTS.md](Operator/ABNORMAL_REBOOTS.md) —
  boot-reason triage (operator version of `ESP32_FAILURE_MODES.md`).
- [Operator/MQTT_COMMAND_REFERENCE.md](Operator/MQTT_COMMAND_REFERENCE.md) —
  every `cmd/*` topic with payload examples.
- [Operator/DIAG_TOPICS.md](Operator/DIAG_TOPICS.md) — `/status`,
  `/diag/coredump`, `/espnow`, `/response` payload reference.
- [Operator/LED_REFERENCE.md](Operator/LED_REFERENCE.md) — firmware-
  driven onboard LED + WS2812 patterns.
- [Operator/LED_COMMANDS.md](Operator/LED_COMMANDS.md) — operator-
  driven WS2812 schema (was top-level; cmd/led API).

## Plans & roadmap

- [ROADMAP.md](ROADMAP.md) — synthesised forward plan: Now / Next /
  Month / v0.5.0+. Updated each release.
- [NEXT_SESSION_PLAN.md](NEXT_SESSION_PLAN.md) — short-term recommended
  next session at the time of writing.
- [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md) — concrete
  hardware-introduction plan for the BDD relay + BMT 49E Hall sensor.

## Backlog (item-lifecycle tracking)

- [SUGGESTED_IMPROVEMENTS.md](SUGGESTED_IMPROVEMENTS.md) — the OPEN
  index, plus WONT_DO and recent RESOLVED entries.
- [SUGGESTED_IMPROVEMENTS_ARCHIVE.md](SUGGESTED_IMPROVEMENTS_ARCHIVE.md)
  — every entry's full body, append-only.
- [WONT_DO.md](WONT_DO.md) — intentional non-actions with rationale.
- [TOPIC_VERSIONING_DESIGN.md](TOPIC_VERSIONING_DESIGN.md) — design
  proposal for versioned MQTT topic prefixes (#33). Not yet
  implemented; ship trigger is fleet > 10 devices, first breaking
  schema change, or v1.0 firmware release.

(Operator-facing reference docs that used to live here —
`OPERATOR_INSTALL_GUIDE.md`, `LED_COMMANDS.md` — now live under
[`Operator/`](Operator/README.md). See the section above.)

## `SESSIONS/` — point-in-time reports

Snapshots of a single investigation, audit, or incident. Useful for
retrospectives but not active reference.

- [SESSIONS/2026-04-27.md](SESSIONS/2026-04-27.md) — cascade-fix
  marathon (v0.4.13 → v0.4.20).
- [SESSIONS/BLE_COEXISTENCE_ANALYSIS.md](SESSIONS/BLE_COEXISTENCE_ANALYSIS.md)
  — analysis of the suspected BLE 70-min hang; conclusion was the
  symptom was a cascade artefact, not BLE.
- [SESSIONS/ESPNOW_REVIEW_2026_04_24.md](SESSIONS/ESPNOW_REVIEW_2026_04_24.md)
  — ESP-NOW protocol review against TECHNICAL_SPEC.
- [SESSIONS/ESPNOW_UX_REVIEW_2026_04_24.md](SESSIONS/ESPNOW_UX_REVIEW_2026_04_24.md)
  — ESP-NOW UX/onboarding review.
- [SESSIONS/NODERED_ESPNOW_TAB_REBUILD_NOTES.md](SESSIONS/NODERED_ESPNOW_TAB_REBUILD_NOTES.md)
  — Node-RED dashboard tab rebuild notes.
- [SESSIONS/RF_CONFIG_TEST_2026_04_25.md](SESSIONS/RF_CONFIG_TEST_2026_04_25.md)
  — RF config sweep and per-peer asymmetry findings.
- [SESSIONS/UUID_DRIFT_AUDIT_2026_04_28.md](SESSIONS/UUID_DRIFT_AUDIT_2026_04_28.md)
  — root-cause audit of #48 Delta+Echo UUID drift (RNG-pre-WiFi
  pseudo-random determinism).
- [SESSIONS/WDT_AUDIT_2026_04_28.md](SESSIONS/WDT_AUDIT_2026_04_28.md)
  — read-only sweep of every blocking I/O site against the TWDT
  model (#29). Companion to TWDT_POLICY.md (top-level).
- [SESSIONS/SESSION_QUESTIONS_2026_04_28.md](SESSIONS/SESSION_QUESTIONS_2026_04_28.md)
  — open questions left for the operator from the 2026-04-28
  afternoon autonomous session, plus the URGENT fleet-LWT-offline
  observation from ~10:50 SAST that gated the v0.4.24 tag.
- [SESSIONS/ALPHA_SERIAL_2026_04_29.md](SESSIONS/ALPHA_SERIAL_2026_04_29.md)
  — Alpha v0.4.26 production serial capture during the 2026-04-29
  morning #54 + #78 disposition session.
- [SESSIONS/CHARLIE_CANARY_SERIAL_2026_04_29.md](SESSIONS/CHARLIE_CANARY_SERIAL_2026_04_29.md)
  — Charlie's canary v0.4.20.0 serial capture documenting 35 h+
  cascade-survival without firing the stack canary; closes #54.
- [SESSIONS/BENCH_DEBUG_AP_CYCLE_2026_04_29.md](SESSIONS/BENCH_DEBUG_AP_CYCLE_2026_04_29.md)
  — first time a #78-class cascade event was captured with
  continuous serial logging through the trigger; surfaced #94's
  silent-degradation variant + reproduction-recipe protocol.
- [SESSIONS/COREDUMP_DECODE_2026_04_29.md](SESSIONS/COREDUMP_DECODE_2026_04_29.md)
  — symbolic decode of all 6 retained `/diag/coredump` payloads
  against the v0.4.26 ELF; common-ancestor analysis for #78;
  #96 root-cause + side-effects writeup. v0.4.28 closing evidence.
- [SESSIONS/AP_OUTAGE_2026_04_28.md](SESSIONS/AP_OUTAGE_2026_04_28.md)
  — observation log from the 2026-04-28 fleet AP outage. First
  fleet-wide outage observed with v0.4.23's #55 mqtt_disconnects
  counter live; Alpha int_wdt'd while Delta/Foxtrot survived.
  Timeline + per-device classification + how v0.4.24's pending
  changes would have affected each device. Feeds #46.

## `archive/` — superseded reference

Kept for grep / audit reasons; no active maintenance. Each is replaced
by an active doc above.

- [archive/DEFERRED_IMPROVEMENTS.txt](archive/DEFERRED_IMPROVEMENTS.txt)
  — pre-v0.3.04 backlog; items moved to SUGGESTED_IMPROVEMENTS or
  WONT_DO.
- [archive/IMPROVEMENT_PLAN.txt](archive/IMPROVEMENT_PLAN.txt) —
  v0.2.17-era phased plan; phases shipped or rolled into ROADMAP.
- [archive/FIXES_LOG.txt](archive/FIXES_LOG.txt) — historical fix log;
  largely superseded by `git log` + RESOLVED entries in
  SUGGESTED_IMPROVEMENTS_ARCHIVE.

## Other

- [BusinessDocs/](BusinessDocs/) — business-case writeups; separate
  concern from the firmware engineering docs.
- `map_parse.py` — Python utility for parsing PIO `.map` files (paired
  with `memory_budget.md` workflow).
