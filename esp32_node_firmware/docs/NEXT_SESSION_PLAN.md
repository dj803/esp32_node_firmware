# Next big session — pick one

Drafted 2026-04-27 evening, after the v0.4.13 → v0.4.20 cascade-fix
marathon closed and the 2026-04-26 audit hygiene batch (#58-#65)
landed. Fleet is on v0.4.20-release; OTA manifest matches.

## Recommended: **A — Coredump-to-flash (#65 sub-item E)**

Single most leverage-positive piece of remaining cascade-fix work.
Every panic captured this week (v0.4.13 cascade, 14:04 backtrace,
M3 chaos runs) cost manual serial-monitor scaffolding + addr2line
gymnastics. With ESP-IDF coredump-to-flash, the firmware writes the
panic backtrace to a dedicated partition on crash, then publishes it
to MQTT on next boot — no serial monitor required, no chip needs to
be on the bench at the moment of failure.

Concrete deliverables:
- Add `coredump,data,coredump,,64K` to
  [esp32_node_firmware/partitions.csv](esp32_node_firmware/partitions.csv)
  (verify free space with current `min_spiffs.csv` budget).
- Set `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` and
  `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y` in
  [esp32_node_firmware/sdkconfig.defaults](esp32_node_firmware/sdkconfig.defaults).
  (File is currently untracked — commit it as part of this session.)
- New include/coredump.h: on boot, `esp_core_dump_image_check()`; if
  present, base64-encode and publish to
  `Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/diag/coredump`
  (one-shot retained), then `esp_core_dump_image_erase()`.
- Node-RED flow: subscribe to `+/diag/coredump`, decode, post to a
  dashboard tile or write to disk for `addr2line` post-mortem.
- Validate via M2-style synthetic blip — confirm a forced panic on
  one device produces a coredump on the broker before the device
  reboots into normal operation.

Effort: ~half a day firmware + ~1 hour Node-RED + 1 hour partition
table validation. Risk: partition resize requires erase-flash on
existing devices (NVS wiped — then re-bootstrap). Run on Charlie
(bench) first; OTA-fleet only after a 24 h soak.

## Alternative: B — Start v0.5.0 relay + Hall hardware

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

A then B. Coredump-to-flash unblocks every future panic
investigation, locking in the diagnostic gain from this week's
cascade chase. Then v0.5.0 feature work can proceed against a
firmware base where any new panic auto-captures its own backtrace.

Run C in parallel on Charlie throughout — it costs nothing.

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
