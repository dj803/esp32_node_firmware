# Next big session — pick one

Drafted 2026-04-28 morning, after the 12 h autonomous window
(2026-04-27 21:30 → 2026-04-28 ~07:25 SAST) closed cleanly. Fleet on
v0.4.20 release (5 devices) + v0.4.20.0 canary (Charlie, sticky via
OTA_DISABLE, 9+ h continuous soak with heap pinned at 131036).

## Recommended path

### A — Cut v0.4.21 release (this session)

Pure diagnostic + tooling enhancement. Already validated end-to-end
on Bravo via M1+M2+M3:
- #76 sub-G — `restart_cause` field in boot announcement
- #71 minimal-variant infrastructure (RFID_DISABLED gate)
- OTA_DISABLE compile gate for canary builds

Risk profile: low. Production behaviour unchanged; only adds a JSON
field on boot announcements. The 5 release devices on v0.4.20 will
auto-OTA up to v0.4.21 within an hour. Charlie's canary stays on
v0.4.20.0 because of OTA_DISABLE.

After v0.4.21 lands, all production devices have:
- The diagnostic field needed to distinguish OTA-restart vs
  cred_rotate vs cmd/restart vs mqtt_unrecoverable on next boot.
- The infrastructure to opt a device into a feature-subset variant
  later without touching every consumer.

### B — Investigate Alpha's `loopTask` panic (next session)

Tonight's surprise: Alpha (production v0.4.20) panicked once at
~23:30 SAST with a NEW signature:

```
exc_task: loopTask
exc_pc:   0x4008ec14
exc_cause: IllegalInstruction
backtrace: 14 frames (0x4008ec14 0x4008ebd9 0x400954ad 0x401ae04b ...)
```

Cannot decode locally — Alpha's CI v0.4.20 binary has `app_sha_prefix`
`a5bb3114`; local builds produce `dd877030`. To proceed:

1. Download the CI v0.4.20 ELF from the release artefacts page
   (https://github.com/dj803/esp32_node_firmware/releases/tag/v0.4.20),
   if the retention window covers it.
2. OR rebuild from the v0.4.20 tag locally:
   `git fetch --tags && git checkout v0.4.20 && pio run -e esp32dev`
   (deterministic-ish — same source, same toolchain, should produce
   matching addresses for IRAM/IROM frames).
3. Run `xtensa-esp32-elf-addr2line -e firmware.elf -fipC <each PC>`
   on the captured backtrace.
4. Compare site to the existing `raw_netif_ip_addr_changed` family —
   if it converges to a similar lwIP/AsyncTCP path, single-bug story.
   If it's elsewhere (heap allocator, JSON serialiser, RFID tick),
   document as a separate item.

### C — Charlie canary soak — long-tail wait

Currently sticky at 9+ h, heap pinned, no panic. Continue until either:
- Canary trips with a stack-overflow halt (root cause for hypothesis #5
  from #51 confirmed; bump the offending task's stack), OR
- 7 days clean (hypothesis #5 retired, return Charlie to release).

Existing soak watcher catches abnormal boots; the canary halt itself
is silent on MQTT but visible on serial (open COM5 carefully —
DTR-reset wipes the halt state).

## After A + B + C

The big unblocked feature work is **v0.5.0 relay + Hall hardware**
([PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md)). With
coredump-to-flash + restart_cause + canary detector all in place,
introducing hardware-divergent nodes to the fleet is now well-supported
diagnostically — any new failure mode auto-captures its backtrace.

## Followups not on the critical path

- #76 sub-B (NVS ring buffer of last-N restart contexts) is the natural
  next builder on top of sub-G. ~50 lines.
- #76 sub-F (`CONFIG_ESP_TASK_WDT_TIMEOUT_S` 5 s → 12 s) is a 1-line
  change in sdkconfig.defaults — bundle with sub-B.
- v0.4.22 batch can ship sub-B/C/D/F/H/I together once they're written.
- The mosquitto.log file rotation issue (log frozen at 13:59 since
  the M3 chaos work) needs investigation — service is running but
  not writing to the canonical log path. Likely a side-effect of the
  blip-watcher's restart cycles. Out of scope for v0.4.21 but worth
  filing as #83.

## Deferred decisions (already in the backlog)

- **#57** host gcc — RESOLVED. MinGW64 works via `PATH=/c/mingw64/bin:$PATH`.
- **#56** MQTT_HEALTHY deferred-flag — RESOLVED in v0.4.13.
- **#82** split FAILURE_MODES + memory_budget docs — parked (neither
  fits the index/archive convention's prerequisites).
- **#6/#7/#13/#18** → moved to WONT_DO with documented rationale.
