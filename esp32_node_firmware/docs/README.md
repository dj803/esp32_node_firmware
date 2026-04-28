# `docs/` — index

Layout grouped by lifecycle (not by topic). See
[TRACKING_DOC_CONVENTION.md](TRACKING_DOC_CONVENTION.md) for the
underlying convention.

Last sweep: 2026-04-28.

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

## Operational guides

- [AUTONOMOUS_PROMPT_TEMPLATE.md](AUTONOMOUS_PROMPT_TEMPLATE.md) — how
  to write a multi-hour AFK prompt for the agent.
- [CHAOS_TESTING.md](CHAOS_TESTING.md) — M1-M4, EN1, O2, I1 chaos test
  procedures + framework proposal.
- [TOOLING_INTEGRATION_PLAN.md](TOOLING_INTEGRATION_PLAN.md) — Tier 1
  / Tier 2 tooling roadmap.
- [TRACKING_DOC_CONVENTION.md](TRACKING_DOC_CONVENTION.md) — meta-doc
  on managing tracking docs.

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
