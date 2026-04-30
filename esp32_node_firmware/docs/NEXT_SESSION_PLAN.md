# Next session plan

Refreshed 2026-04-30 morning after `/morning-close`. The overnight v0.4.31
soak ran into the recurring panic shape that #46 has been waiting on a
clean soak to close. Soak verdict: **RED**. #103 filed for the refined
root-cause; #46 stays OPEN until the #103 fix ships and re-soaks clean.

## State at session close (2026-04-30 ~08:00 SAST)

| | |
|---|---|
| Master HEAD | `dcd7c65` + #103 commit pending below |
| Latest tag | v0.4.31 |
| Fleet | **5/5 soaking devices on v0.4.31**, all heartbeating; Alpha + Delta rebooted from panic during soak; Charlie off-fleet per operator |
| Backlog | OPEN **26** (was 25; +1 from #103), RESOLVED **64**, WONT_DO **11** |
| Soak closure | `C:\Users\drowa\soak-closures\2026-04-30_075829.md` |
| Soak baseline | `C:\Users\drowa\soak-baselines\2026-04-29_185000.md` |

## Highest-priority next-session item — #103 fix

The overnight soak surfaced the missing piece for #46 closure. Path:

### A. Symbolic-decode `0x4008a9f2` against the v0.4.31 release ELF

First time we have a decodable instance of this panic shape on a
v0.4.28+ build. ELF SHA prefix `0ee173b8`. Procedure per
[../COREDUMP_DECODE.md](../COREDUMP_DECODE.md):

1. Find the v0.4.31 ELF — either download from the GitHub release
   artefacts (if retention window is open) or rebuild from the v0.4.31
   tag locally and confirm its `app_sha_prefix` matches `0ee173b8`.
2. `xtensa-esp32-elf-addr2line -e firmware.elf 0x4008a9f2` →
   function:line.
3. Decode the rest of the backtrace frames:
   `0x400e4659 0x400e2881 0x400ef3ce 0x400fbb7a 0x40103ebb 0x4010bfd0 0x4008ff51`.
4. Identify the call path from `loopTask` down to the LoadProhibited
   instruction. Likely sits inside `mqttPublish()` →
   AsyncMqttClient::publish() → AsyncTCP send.

### B. Apply the proposed v0.4.32 fix (per #103 archive entry)

Three options, recommended order:

1. **(c) Re-stamp `_lastNetworkDisconnectMs` on every WiFi-state-change
   event.** Smallest change. In main.cpp's WiFi event handler, on any
   transition (CONNECTED, DISCONNECTED, IP_LOST, etc.), call
   `mqttMarkNetworkDisconnect()` so the brief AP-return at the start
   of a flaky-recovery window doesn't open the publish window.
2. **(a) Stable-connectivity gate.** Only un-silence publishes after
   N consecutive heartbeat-cadence-passes (e.g. 60 s × 3) of stable
   WiFi + MQTT. Larger surface; more conservative. Apply if (c) alone
   doesn't fully eliminate.
3. **(b) TCP-probe-before-publish.** Cheap defence-in-depth at the
   publish call site itself. Apply if (c) + (a) still don't fully
   eliminate (less likely).

After fix: bench-flash to one device, re-run a soak with a synthetic
flaky-AP scenario (start the blip-watcher with multiple short blips
spaced ~30 s apart to reproduce the partial-recovery), confirm no
panic. Then full overnight soak. Then close #46.

### C. Apply #102 fix (independent, can ship in parallel)

`RestartCause::set("ota_reboot")` (and 4 other context-specific tags)
before each `ESP.restart()` in ota.h. Confirmed gap from yesterday's
investigation. ~5 lines per call site. Lives at lines 80, 541, 606,
626, 645 of `include/ota.h` (per the #102 archive entry).

### D. Tag v0.4.32 with both fixes

Commit + tag + push. CI builds. OTA-rollout via the new phased-parallel
script (`tools/dev/ota-rollout.sh 0.4.32`). Should complete in ~3 min
end-to-end given the v0.4.31 rollout took 5:46.

### E. Re-arm soak

Run `/evening-soak` on v0.4.32 after the rollout completes. Restore
Charlie to the fleet first (operator decision — not in scope for the
slash command).

## Other items deferred from yesterday

### F. v0.5.0 hardware bring-up
Bravo wiring + relay/Hall code. Operator may wire the 4x4 NeoPixel
matrix + 2-ch relay onto Alpha per
[Operator/HARDWARE_WIRING.md](Operator/HARDWARE_WIRING.md). Recommend
**defer until #103 fix ships** — adding hardware before stabilising
the recurring panic complicates diagnosis.

### G. #91 ESP32-WROOM-32U + external antenna procurement
Operator orders parts (~$15–30). Bench-test against current WROOM-32
fleet for asymmetry / RF range / orientation sensitivity.

### H. Phase 2 ranging cluster
#37, #38, #39, #42, #47, #49, #86, #90, #91 still open. Bundle option
after #46 + #103 close.

### I. #99 LED patterns soak validation
The retuned LED patterns shipped in v0.4.31 are visible on Alpha but
haven't been operator-validated across the rest of the fleet. Confirm
each device's LED visually matches the v0.4.31 reference table in
[Operator/LED_REFERENCE.md](Operator/LED_REFERENCE.md).

## Won't-do at next-session start

- Close #46 — soak failed the closure criteria, fix-shaped work is needed first.
- Roll the soak forward without a #103 fix — same scenario will reproduce.
- Touch v0.5.0 hardware code until #103 ships.
- Auto-rollback from v0.4.31 — partial wins (3/5 survival, heap stable,
  SSID probe) are real and worth keeping.

## Open questions for operator

- When to re-add Charlie to the fleet? It was excluded for tonight per
  your earlier intent.
- v0.4.32 timing — ship this morning after symbolic decode, or wait
  for clearer root-cause data first?
- v0.5.0 hardware wiring — proceed in parallel (matrix + relay are
  additive, won't affect #103) or defer?

## Reference

- Soak closure: `C:\Users\drowa\soak-closures\2026-04-30_075829.md`
- Soak baseline: `C:\Users\drowa\soak-baselines\2026-04-29_185000.md`
- #103 archive entry: docs/SUGGESTED_IMPROVEMENTS_ARCHIVE.md
- #102 archive entry: docs/SUGGESTED_IMPROVEMENTS_ARCHIVE.md
- COREDUMP decode runbook: docs/COREDUMP_DECODE.md
- v0.5.0 hardware: docs/PLAN_RELAY_HALL_v0.5.0.md +
  docs/Operator/HARDWARE_WIRING.md
