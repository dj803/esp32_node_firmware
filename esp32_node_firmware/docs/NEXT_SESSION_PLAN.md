# Next big session — pick one

Drafted 2026-04-27 evening, after the v0.4.13 → v0.4.20 cascade-fix
marathon closed and the 2026-04-26 audit hygiene batch (#58-#65)
landed. Fleet is on v0.4.20-release; OTA manifest matches.

## Recommendation revised 2026-04-27 evening

The original A (coredump-to-flash) is **already shipped**. Charlie's
canary first-boot tonight published a real coredump on
`/diag/coredump`: AsyncTCP `InstructionFetchError`, exc_task=async_tcp,
PC=0x3f409271. That confirms include/coredump_publish.h (added in
v0.4.17, commit 43c71a6) is wired through, and the partition table /
sdkconfig.defaults are correct on real fleet hardware. Backlog #65
sub-item E moves to "shipped + validated".

Sub-item A (pre-restart diag publish) shipped in v0.4.19 (commit
9ed19a4). Remaining #65 sub-items B/C/D/F/G/H/I are all nice-to-have
but not blockers.

**New recommended next session: B (v0.5.0 relay + Hall hardware).**

The coredump win + canary build + v0.4.20 fleet stability mean the
diagnostic + safety-net layer is now strong enough to support feature
work. v0.5.0 is the deliberate moment to introduce hardware-divergent
nodes; if any new failure mode emerges, the coredump path will catch
it without requiring a serial monitor.

## Originally-listed: B — Start v0.5.0 relay + Hall hardware

Plan already drafted at
[esp32_node_firmware/docs/PLAN_RELAY_HALL_v0.5.0.md](esp32_node_firmware/docs/PLAN_RELAY_HALL_v0.5.0.md).
GPIO assignments, NVS schemas, MQTT topics, wiring all worked out.
Hardware on hand. Cleanly scoped: `relay.h` + `hall.h` modules
mirror existing `ws2812.h` / `rfid.h` patterns.

Pros: pure feature work, no diagnostic chase risk, unblocks the
"add a sensor" use case the deployment exists to support.

Cons: doesn't pay down the panic-investigation tax — next cascade
(if it happens) costs the same 2-3 hours. Better to let v0.4.20
soak undisturbed first; this introduces NEW failure modes (relay
inrush brownout, ADC contention) right after a stability win.

## Alternative: C — 7-day stack-canary soak (#54)

[`platformio.ini`](esp32_node_firmware/platformio.ini) already has
[env:esp32dev_canary]; sdkconfig.defaults already sets
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y. Just flash it to one
non-bench device and let it run for a week.

Pros: catches the lingering hypothesis #5 from #51 (WS2812 task
stack overflow at 4 KB) without any code changes.

Cons: not really a "session" — flash + leave it. Doesn't move other
backlog items forward. Best done as a silent background check
parallel to A or B, not as the headline session.

## Recommendation

B then (if budget) the long-tail #65 sub-items (C/D/F/G/H/I —
threshold tuning, cool-off counter, WDT bump, distinct boot_reason).
Run C (canary soak) in parallel on Charlie — already armed
2026-04-27 evening, see memory/canary_soak_charlie_2026_04_27.md.

The Charlie coredump from tonight (AsyncTCP InstructionFetchError)
suggests #67 (AsyncTCP _error race) is still latent in v0.4.20 —
the v0.4.16 broker-probe reduces the trigger surface but doesn't
fully eliminate the underlying library bug. Investigate as a
parallel thread, not the main session, since the coredump path is
now the diagnostic safety-net.

## Deferred decisions

- #57 (host gcc install): MinGW64 already at `C:\mingw64\bin`
  per platformio.ini comment line 152 — close to working. Move to
  RESOLVED after one successful `pio test -e native -v` invocation.
- #56 (MQTT_HEALTHY deferred-flag): per memory note, the pattern was
  shipped in v0.4.13. Confirm in code (look for `_mqttLedHealthyAtMs`
  consumed in `mqttHeartbeat()`) and move to RESOLVED.
- #81 renumbering pass: still cosmetic; don't bundle.
- #82 split FAILURE_MODES + memory_budget: low priority; tackle next
  time one of those docs grows past 600 lines.
