# ESP32 — Why Devices Stop Working

Reference catalogue of failure modes seen on classic ESP32 (WROOM-32, 4 MB flash) deployments, with our specific fleet's history flagged. Use as a triage checklist when a device goes silent or reboots unexpectedly.

Boot reasons reported by the firmware (via `esp_reset_reason()` mapped in our serial log + the retained `boot_reason` field in `/status`):

- `poweron` — fresh power-up (Vcc went 0 → 3.3 V)
- `external` — RTS pin / EN button reset
- `software` — clean `esp_restart()` from firmware (e.g. our cmd/restart, post-OTA reboot)
- `panic` — exception caught by ESP-IDF panic handler — pointer/stack/divide-by-zero
- `int_wdt` — interrupt watchdog (ISR exceeded budget; default 300 ms)
- `task_wdt` — task watchdog (a subscribed task didn't feed within the configured window — default 5 s, or per [TWDT_POLICY.md](TWDT_POLICY.md))
- `other_wdt` — RTC watchdog or brownout-related WDT (chip-level safety net)
- `brownout` — brownout detector tripped (Vcc dipped below ~2.43 V)
- `deepsleep` — wake from deep sleep (not a failure)

---

## 1. Power problems

| Mode | Symptom | Boot reason | Fleet history |
|---|---|---|---|
| Brownout (transient) | random reboots when WiFi TX peaks | `brownout`, `other_wdt` | Suspected on Alpha 2026-04-25/26 (#51) |
| Insufficient bulk cap | peak draws (~600 mA WiFi TX, ~60 mA per WS2812) sag rail | `other_wdt`, `panic` mid-burst | Possible on Alpha (LED strip + WiFi) |
| Battery exhausted | clean cutoff, never returns | `poweron` on next charge | Charlie 2026-04-25 23:02 |
| USB cable resistance | thin/long cable drops Vcc under load | `brownout`, `other_wdt` | Suspect when devices crash on USB but not bench supply |
| USB hub current limit | 100 mA "USB 2.0 default" hub starves device | `brownout` on TX bursts | Avoid passive hubs |
| Regulator thermal cutoff | AMS1117 on dev boards overheats, drops out | random `other_wdt` | Mostly when driving high LED counts |
| Mains outage (operator) | all mains-powered devices off simultaneously | `poweron` when restored | 2026-04-25 overnight (partial) |

---

## 2. Watchdog crashes

Three independent watchdogs on the ESP32:

- **Task WDT** (`task_wdt`) — checks subscribed FreeRTOS tasks. Subscribers in our firmware: see [TWDT_POLICY.md](TWDT_POLICY.md). Triggered by long blocking calls (TLS handshake, OTA download), priority inversion, or genuine task hangs.
- **Interrupt WDT** (`int_wdt`) — fires if interrupts stay disabled longer than the budget. Triggered by long ISRs, `portENTER_CRITICAL` held too long, or busy-loops in ISR context.
- **RTC WDT** (`other_wdt`) — chip-level catch-all. Often fires during brownouts, or when both cores are unresponsive.

Common firmware-side causes:

- ESP-NOW receive callback doing too much work (must defer to a task)
- `delay()` inside MQTT callback (callbacks run in async_tcp task — blocking starves it)
- `Preferences::putString()` with a long key list during a critical-section
- Crypto ops on Core 0 while WiFi has BSS-update IRQs queued
- Logging at LOG_LEVEL_DEBUG over 115200 baud — Serial.print() blocks ~1 ms per 10 chars

Our recent fleet hits: Alpha + Delta paired `int_wdt` / `other_wdt` overnight 2026-04-25/26 — see #51.

---

## 3. Memory issues

| Mode | Detection |
|---|---|
| Heap exhaustion | LOG_HEAP shows `free` dropping to <8 KB, `malloc` returns NULL → `panic` next allocation |
| Heap fragmentation | `largest` block << `free` total. Visible during OTA preflight (we gate on this — see config.h `OTA_PREFLIGHT_HEAP_BLOCK_MIN`) |
| Stack overflow | Random `panic` with corrupted PC. ESP-IDF's `configCHECK_FOR_STACK_OVERFLOW` would catch but is OFF in arduino-esp32 by default |
| Buffer overrun | Adjacent variable corruption — eventual `panic` or wrong behavior |
| Use-after-free | Pointer to freed AsyncTCP/AsyncMqttClient object — common when a connection drops mid-callback |
| Cache fault | Code-on-flash fetched while SPI bus is in non-cached operation → `LoadProhibited` panic |

Our defenses: the LOG_HEAP markers in main.cpp (`after-serial`, `after-wifi`, `after-mqtt`, `after-ble`), OTA preflight heap gate, NimBLE-deinit-before-OTA path.

---

## 4. Concurrency / race conditions

ESP32 is dual-core (LX6 Core 0 + Core 1). Our fleet tasks:

- **loopTask** — Arduino loop(), Core 1, default priority 1
- **async_tcp** — AsyncTCP/AsyncMqttClient, Core 0, priority 5+
- **wifi / btc / nimble_host** — IDF-managed, Core 0
- **ws2812 task** — our LED renderer, Core 1, priority 1
- **rfid IRQ-driven** — IRQ on Core 1 → defers to loop()

Common races:

- Volatile-required state read across cores without `portMUX_TYPE` (we audited this in v0.3.36 for `_mqttNeedsRediscovery`, others)
- Posting to a FreeRTOS queue from an unexpected context (xQueueSend is safe from anywhere, but calling Preferences/Serial in MQTT callback is not — they can block)
- ISR using floating-point (FPU regs not saved by IDF default ISR entry)
- Mutex held across `vTaskDelay()` — fine but easy to extend accidentally

Currently under suspicion: posting `LedEvent` from `onMqttDisconnect` (async_tcp ctx) → consumed by ws2812 task. The post itself is non-blocking (`xQueueSend(_, &e, 0)`) but suspect for #51.

---

## 5. WiFi / network crashes

- **WiFi stack panic on bad packet** — fixed mostly upstream; rare on current arduino-esp32
- **DHCP renewal** — short outage if lease is short and not handled cleanly
- **AP changes channel** — devices have to re-scan; can cascade if many do it at once
- **mbedTLS heap pressure** — TLS handshake needs ~30 KB; near-OOM scenario crashes
- **AsyncMqttClient hang** — connection stuck in CONNECTING; we mitigate via `MQTT_HUNG_TIMEOUT_MS` (12 s watchdog)
- **DNS unreachable** — getaddrinfo blocks; mitigated by stored broker URL/IP
- **GitHub Pages 404** — OTA manifest fetch fails. We have OTA_FALLBACK_URLS and the firmware retries gracefully. Witnessed 2026-04-26 morning when Pages was disabled on the repo (separate event from #51)

---

## 6. Flash / OTA

- **Corrupted partition table** — bootloader can't find app, hangs. Recovery: full erase + flash.
- **OTA write fail mid-flash** — bootloader rolls back to previous slot (when rollback is enabled — currently DEFERRED in our build per #25)
- **NVS corruption** — rare with proper API use, but force-power-loss during commit can do it. Recovery: erase NVS region.
- **SPI flash wear** — sector endurance ~10K writes. Our hot writes: NVS (creds, calibration), boot count. Estimated lifetime in years.
- **OTA URL drift** — gAppConfig.ota_json_url may not match the deployed manifest. Mitigated by OTA_FALLBACK_URLS hardcoded list.

---

## 7. Hardware faults

- **Antenna detuning** — adjacent metal, breakout boards, RFID coil shift TX power +/- 20 dB. See [RF_CONFIG_TEST_2026_04_25.md](RF_CONFIG_TEST_2026_04_25.md).
- **GPIO overcurrent** — max 40 mA per pin. Driving a relay coil directly = burnout.
- **SPI bus contention** — multiple devices sharing SS without proper isolation
- **Crystal failure** — clock drift causes WiFi association to fail
- **ESD damage** — handling without grounding strap
- **Solder joint cracks** — vibration / thermal cycling, especially on USB connector

---

## 8. Environment

- **Temperature** — WROOM-32 rated 0-65°C industrial. Outdoor/sun deployments can exceed 80°C and trigger throttle then crash.
- **EMI** — nearby motors, relays, switchmode supplies inject noise. Mitigated by ferrite beads, decoupling.
- **Humidity / corrosion** — coastal deployments. Conformal coating is the answer.
- **WiFi router changes** — firmware update on the router can change channel auth or roaming behavior

---

## 9. Firmware bugs (project-specific)

- **Library version mismatch** — NimBLE 1.x → 2.x API changes (caught Aug 2025). MFRC522 v1 → v2 (caught Mar 2026). Always cross-check `library_deps` in platformio.ini against current API.
- **Pointer lifetime in C-API setters** — see [STRING_LIFETIME.md](STRING_LIFETIME.md). Several v0.2.x crashes traced to passing temporary `String::c_str()` into setters that store the pointer.
- **State-flag races** — addressed in v0.3.36 (volatile audit on cross-task flags).
- **NVS namespace registry** — must stay in sync between #define and `nvsNsName()` enum (config.h).
- **Compile-time config drift** — OTA URL placeholder (`myorg.github.io...`) vs. deployed URL led to silent bootstrap failures (see #49).

---

## 10. External-service failures (looks like a device problem, isn't)

- **MQTT broker down** — devices reconnect-loop, may not crash but go dark
- **DNS unreachable** — no broker discovery
- **WiFi router rebooted / channel changed** — temporary mass-disconnect across fleet
- **GitHub Pages disabled** (witnessed 2026-04-26) — OTA polling 404s; firmware degrades gracefully via fallback URLs
- **NTP unreachable** — timestamps drift; affects credential rotation acceptance windows

---

## 11. Project-specific known issues (cross-reference)

| ID | Title | Status |
|---|---|---|
| #46 | Recent abnormal reboots — fleet-wide WDT/panic baseline | Open |
| #48 | DeviceId UUID drift on restart | Open, live-confirmed |
| #49 | Bootstrap protocol OTA URL propagation | Open |
| #50 | Fresh-boot serial capture pitfall (DTR-reset) | Resolved (workflow doc) |
| #51 | v0.4.10 stability regression (LED MQTT_HEALTHY hooks) | Under investigation 2026-04-26 |

---

## Triage flowchart

When a device goes silent:

1. **Mosquitto log** — when did it last speak? `tail -200 C:/ProgramData/mosquitto/mosquitto.log | grep <UUID>`
2. **boot_history flow context** — what was the last boot reason? `curl http://127.0.0.1:1880/context/flow/0cc8e394107eb034/boot_history`
3. **Power** — confirm device is physically powered. If on battery, check voltage. If on USB, swap cable.
4. **WiFi** — can other devices on the same SSID reach the broker? If only this device fails, it's device-side. If many fail, it's network/broker-side.
5. **Serial** — if you can plug it in, capture the next boot. ESP-IDF prints reset reason at the very top of boot log (right after the `rst:0xN` line).
6. **Compare against #51** — is it a known v0.4.x regression? Roll back if confirmed.

Don't conclude "device is dead" until you've ruled out external services + power + WiFi state.

---

## Historical evidence (cross-session review 2026-04-26)

Reviewing mosquitto.log, boot_history, gh CI runs, past plans, and prior commits before formalising the verification plan.

### Mosquitto disconnect baseline (2 days of log, 04-24 → 04-26)

| Reason | Count | Note |
|---|---|---|
| `session taken over` | 248 | Normal reconnect — broker dropped old session when device CONNACK'd a new one. Not a failure. |
| `exceeded timeout` | 158 | Real outage — broker stopped getting MQTT keepalives. Either device crashed, WiFi dropped, or TCP stuck. |
| `connection closed by client` | 27 | Clean disconnect — our `cmd/restart` path or PySerial DTR-reset during dev. Not a failure. |
| `malformed packet` | **2** | Rare — Charlie 2026-04-25 16:02:48; Delta 18:07:02. Indicates the broker received MQTT bytes that didn't parse. Possible causes: memory corruption mid-publish; bug in AsyncMqttClient framing; bit-flip on the wire (unlikely on a 100-Mbit LAN). Worth keeping a counter on. |

Per-device disconnect frequency over 2 days:

| Device | Disconnects | Note |
|---|---|---|
| Charlie | **136** | 2× more than any other device. Pre-dates v0.4.10. Already documented as chronic flake in #46 (`int_wdt`/`task_wdt`/`other_wdt` history). Hardware suspect. |
| Alpha | 78 | High-volume but explained by fleet leadership role + this session's OTA chaos. |
| Bravo | 72 | Similar to Alpha — battery test cycles bumped count. |
| Echo | 71 | Similar to Alpha. |
| Delta | 67 | Similar to Alpha. |
| Foxtrot | 9 | New to fleet (only since 2026-04-25). |

Day-by-day disconnect spikes:

| Date | Timeouts | Note |
|---|---|---|
| 2026-04-24 | 7 | Quiet baseline — pre-session. |
| 2026-04-25 | **148** | Chaotic session day — fleet OTA, USB-flash cycles, isolation tests. Most of this is operator activity, not bugs. |
| 2026-04-26 | 9 | Returning to baseline. |

**Implication:** the "fleet-wide stability problem" is partly an artefact of session activity. The morning daily-health red was real, but the disconnect-volume baseline is closer to the 04-24 number.

### Boot-history pattern (post-v0.4.06)

`boot_history` flow context shows recurring abnormal reboots well before v0.4.10. From the 20-entry rolling window:

- Multiple Echo `panic`/`software` bouncing on v0.4.08 (2026-04-25 17:21–18:11)
- Charlie `int_wdt` / `other_wdt` on v0.4.08 (2026-04-25 18:49–18:56)
- Delta `other_wdt` on v0.4.08 (2026-04-25 19:22)
- Alpha `panic` on v0.4.10 + Delta `int_wdt` on v0.4.10-dev paired at 23:42–23:43

So **abnormal reboots are NOT new with v0.4.10** — they exist on v0.4.08 too. v0.4.10 may have introduced a NEW failure path on top of an existing chronic-instability baseline.

### Past commits — recurring WDT/OTA hardening

The fleet has been fighting watchdog issues for months. Relevant commits:

| Version | Theme |
|---|---|
| v0.3.26 | Free BLE heap before flash write; restart on flash failure |
| v0.3.27 | Subscribe loopTask to TWDT before download (WDT spam fix) |
| v0.3.28 | Remove dead helper |
| v0.3.32 | Per-chunk OTA progress watchdog + trigger-time heap log |
| v0.3.33 | OTA bulletproofing Phase 1: preflight heap gate |
| v0.3.34 | OTA bulletproofing Phase 2: post-OTA self-validation + rollback |
| v0.3.35 | OTA bulletproofing Phase 3: esp_https_ota writer + HTTP resume |
| v0.3.36 | Concurrency hardening: volatile audit across subsystems |
| v0.4.01 | Phase B observability + cross-cutting hardening |
| v0.4.02 | Lib API guards + string lifetime convention + log silence |
| v0.4.03 | Preferences spam fix + TWDT policy doc + heap phase logging |
| v0.4.08 | OTA reliability: eliminate three failure modes (#35) |
| v0.4.09 | Per-peer calibration + OTA WDT hardening |

Pattern: every minor release adds another defence against WDT/heap/OTA failure modes. The fact that v0.4.10's only material change (LED hooks) immediately produces a new crash class fits "yet another way to trip WDT we hadn't accounted for".

### TWDT subscribers — relevant to LED hook

From `docs/TWDT_POLICY.md`:

- `IDLE0` / `IDLE1` — always subscribed by IDF
- `loopTask` — subscribed only during OTA + TLS keygen
- **`async_tcp` — always subscribed by AsyncTCP library**
- `nimble_host` — subscribed by NimBLE

The new `ws2812PostEvent(MQTT_HEALTHY)` call in `onMqttConnect` runs on the **async_tcp task**, which IS subscribed to TWDT. Posting to a queue is non-blocking, but if the `xQueueSend` itself contends or the strlcpy/struct copy somehow blocks, it could starve async_tcp. Also: on disconnect we do `strlcpy(e.animName, "wifi", sizeof(e.animName))` then post — fine in isolation, but if MQTT disconnect/reconnect cycles rapidly, the queue could fill and `xQueueSend(_, _, 0)` returns failure (ignored), and the time spent attempting may compound. Worth measuring.

### Past Claude session continuity

The 2026-04-25 morning plan ([espnow-tracking-firmware-improvements-2026-04-25.md](C:\Users\drowa\.claude\plans\espnow-tracking-firmware-improvements-2026-04-25.md)) had **Phase 0.5 — staggered-OTA dashboard tooling** scheduled. We delivered the firmware-side stagger via Node-RED `hb_cmd_fn` patch yesterday but the visual progress bar UI from that plan is still pending. Tracking-related improvements (#39, #41.7) shipped in v0.4.07 + v0.4.09. Nothing in that plan suggests v0.4.10's LED hooks would crash.

---

## #51 Verification plan (v0.4.10 stability regression)

### Ranked hypotheses for Alpha's crash pattern (revised after historical review)

Ordering reflects (a) fit to observed evidence + historical baseline, (b) Alpha-specificity (Alpha is the only device with WS2812 strip wired), and (c) cheapness to test.

**Pre-flight context** — abnormal reboots EXIST on v0.4.08 (Echo, Charlie pre-v0.4.10). v0.4.10 may have added a new failure path on top of pre-existing chronic instability. Charlie has 2× the disconnect rate of any other device (136 in 2 days), pre-dating v0.4.10. So "fleet-wide v0.4.10 regression" is partly a misdiagnosis — much of yesterday's noise was operator activity. Alpha's specific 3× `other_wdt` in 9 h is the cleanest v0.4.10-attributable signal.

| Rank | Hypothesis | Evidence for | Evidence against | Test cost |
|---|---|---|---|---|
| 1 | **WDT trip on async_tcp task via ws2812 post** | `onMqttConnect`/`onMqttDisconnect` run on async_tcp task — confirmed TWDT-subscribed in [TWDT_POLICY.md](TWDT_POLICY.md). Only new code path in v0.4.10. Alpha + Delta paired `other_wdt` at 00:04 fits a "shared trigger event". | Delta has no strip; would only matter if the queue post itself blocks. `xQueueSend(_, _, 0)` is documented non-blocking. | LOW — Phase A v0.4.10.1 revert in progress. |
| 2 | **Panic in LED state machine (rare path)** | Alpha `panic` at 23:42:56 confirmed. Could be unhandled state transition, null deref in the new MQTT_HEALTHY case, or render-frame edge case. | One-shot rather than recurring; subsequent crashes were `other_wdt` not `panic`. | LOW — capture serial trace of next panic for full backtrace. v0.4.10.1 ALSO suppresses this since the post never fires. |
| 3 | **Brownout from current spike on Alpha-only strip** | Alpha is the ONLY device with strip wired. Only Alpha has chronic crashes (3 in 9 h on v0.4.10) AT IDLE. MQTT_HEALTHY → green breathing pulls more peak current than IDLE blue. | `other_wdt` is RTC watchdog, not strictly `brownout`. Strip current draw is bounded by `setMaxPowerInVoltsAndMilliamps(5, 500)`. | MEDIUM — Phase B disconnect-strip A/B test. |
| 4 | **Charlie-class chronic flakiness leaking into v0.4.10** | Charlie: 136 disconnects (2× Alpha). `int_wdt`/`other_wdt` history in #46 shows Charlie was unstable on v0.4.08 too. Possible bad solder joint / antenna issue / per-module flake. | Doesn't explain Alpha's specific crashes — Alpha was stable before v0.4.10. | LOW — observe Charlie on v0.4.10.1; compare to other devices. |
| 5 | **Heap fragmentation over hours** | Crashes spaced 4–9 h apart. Long-running fragmentation profile fits. | Existing OTA preflight heap gate + LOG_HEAP markers haven't shown drift. | MEDIUM — Phase C: per-heartbeat LOG_HEAP for 24 h. |
| 6 | **Stack overflow in ws2812 task** | Random `panic` could be deepest-call corruption. WS2812 task stack is 4 KB; new state added in v0.4.10. | Code path doesn't recurse. arduino-esp32 default has no detection. | MEDIUM — Phase C: rebuild with `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`. |
| 7 | **Parasitic powering / RF detuning by strip** | Alpha-only correlation. Strip near antenna detunes (per [RF_CONFIG_TEST_2026_04_25.md](RF_CONFIG_TEST_2026_04_25.md)). | Detuning usually = WiFi reconnect loops, not `panic`. | LOW — Phase D: move strip 10 cm from antenna. |
| 8 | **Malformed packet → AsyncMqttClient crash** | Mosquitto logged 2 "malformed packet" events in 2 days (Charlie, Delta). Could indicate transient memory corruption in the publish path. | Only 2 events in 270 connections. Doesn't explain Alpha-specific. | LOW — add a counter for AsyncMqttClient errors; ship telemetry. |
| 9 | **Concurrency / volatile audit gap** | Cross-core writes to `_ledStateR/G/B` could tear under specific timing. v0.3.36 audit didn't cover the new v0.4.10 fields. | Single-byte writes are atomic on Xtensa; tearing observable but not crash-causing. | HIGH — formal code review of LED state machine; potentially add portMUX. |

### Phased test plan

**Phase A — In progress (2026-04-26 09:00 SAST):**
- Alpha + Charlie running v0.4.10.1 (LED hooks reverted).
- Other 4 devices on buggy v0.4.10/-dev / v0.4.11-dev.
- **Pass criterion:** Alpha + Charlie up >4 h with no abnormal `boot_reason`.
- **Fail criterion:** Alpha logs another `other_wdt` / `panic` within 4 h → revert hypothesis #1 falsified, escalate to Phase B.

**Phase B (only if A fails):**
- Capture full serial trace of Alpha's next crash. The ESP-IDF panic handler dumps registers + backtrace to UART. Save raw trace as `docs/panic_<timestamp>.txt`.
- Run `xtensa-esp32-elf-addr2line -e .pio/build/esp32dev/firmware.elf <PC>` on the program counter from the panic to identify the offending source line.
- Disconnect Alpha's WS2812 strip. Re-flash v0.4.10. Run for 4 h. If now stable → hypothesis #3 (current spike / brownout / parasitic) wins over #1.

**Phase C (longer-term, regardless of A/B outcome):**
- Add a per-heartbeat `LOG_HEAP` line to mqtt_client.h's heartbeat publisher. Run fleet for 7 days, plot heap trajectory per device. Surfaces hypothesis #4 even if #1 was the proximate cause.
- Rebuild with `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` (canary method). Catches hypothesis #5 going forward at cost of ~+8 ms per task switch.
- Schedule a code review of every `volatile` field in cross-core paths — there's a v0.3.36 audit but new code in v0.4.10 wasn't covered.

**Phase D (if Phase B confirms hardware-side):**
- Increase bulk capacitor near Alpha (1000 µF + 0.1 µF on VIN rail).
- Confirm WS2812 data line has the recommended 300 Ω series resistor (check breadboard wiring).
- Re-route LED strip so its data line is >5 cm from the antenna.

### What "good" looks like at each phase

| Phase | Watch for | Action if seen |
|---|---|---|
| A | Alpha boot_reason stays `poweron` only after this morning's flash, no new boot_history entries for 4 h | Roll v0.4.10.1 to rest of fleet. Cut a tag. Update #51 to RESOLVED. |
| A | New `other_wdt` / `panic` on Alpha within 4 h | Stop. Move to Phase B. |
| B | Disconnected-strip Alpha is stable for 4 h | Hardware-side — Phase D. Re-architect LED feature with safety margins. |
| B | Disconnected-strip Alpha STILL crashes | Software-side, deeper bug. Capture multiple panics, compare backtraces. |
| C | Heap free trends downward over 24 h | Memory leak. Run `vTaskList()` periodically to spot the growing task. |
| C | Stack canary tripped | Identify which task; raise its stack. |

### Documentation hooks

- Update [SUGGESTED_IMPROVEMENTS.md](SUGGESTED_IMPROVEMENTS.md) #51 with phase outcomes as they land.
- If the regression is closed, add a "Resolved" note + tag the v0.4.10.1 → v0.4.11 release whose first commit reverts the hooks.
- If the LED feature can be re-implemented safely, document the new pattern (e.g. deferred-flag set in callback, consumed by loop()) in [TWDT_POLICY.md](TWDT_POLICY.md).

---

## 2026-04-26 codebase audit findings

Full audit of `src/main.cpp` + every header in `include/` against the failure-mode catalogue. Findings ranked by likelihood of causing v0.4.10 field issues.

### Confirmed the v0.4.10.1 revert is the right immediate fix

**Finding 1.1 — async_tcp callback posting to WS2812 queue.** [mqtt_client.h:1104–1110](../include/mqtt_client.h) (connect) and [mqtt_client.h:1198–1200](../include/mqtt_client.h) (disconnect). Both callbacks run on the async_tcp task which is TWDT-subscribed (per [TWDT_POLICY.md](TWDT_POLICY.md) line 29). v0.4.10.1 disables the posts; comments at the call sites flag the suspected WDT path. Severity HIGH, confidence HIGH.

**Long-term fix:** re-implement MQTT_HEALTHY as a deferred-flag pattern — `_mqttLedHealthyAtMs = millis()` in the callback (single atomic store, no queue), consumed by `loop()` which posts the WS2812 event from a non-TWDT-subscribed context.

### Mitigations verified in place (no new work needed)

| Code path | Audit | Status |
|---|---|---|
| AsyncMqttClient `.c_str()` lifetime | [mqtt_client.h:115–124](../include/mqtt_client.h) — module-static Strings, lib_api_assert.h guards setter signatures | Protected since v0.4.02 |
| RFID whitelist cross-task | [rfid.h:78–90, 126–130, 178–228](../include/rfid.h) — `portMUX` guards add/remove/check | Protected since v0.3.36 |
| ESP-NOW responder health flags | [espnow_responder.h:76–81](../include/espnow_responder.h) — `portMUX` on read-modify-write | Protected since v0.3.36 |
| OTA WDT | [ota.h:77–95](../include/ota.h) — per-chunk feed + 1-s periodic timer feed | Double-defended since v0.4.09 |
| MQTT hung-client | [mqtt_client.h:91, 1236](../include/mqtt_client.h) — `MQTT_HUNG_TIMEOUT_MS` 12 s watchdog | Mitigated |
| TLS keygen heap/stack | [ap_portal.h:159–200](../include/ap_portal.h) — ~6.5 KB used of 8 KB stack; WDT fed between mbedTLS steps | Within budget |
| OTA URL fallback | [ota.h](../include/ota.h) — `OTA_FALLBACK_URLS` list survives manifest 404 | Witnessed working 2026-04-26 morning when GH Pages was disabled |
| NVS namespace registry | [config.h](../include/config.h) — declared centrally, used consistently | Aligned |
| Strapping pins | GPIO 5 (RFID SS) is technically a strapping pin but only asserted during SPI transactions, never held LOW at boot | Safe by design |
| WS2812 `_ledStateR/G/B` cross-core read | [ws2812.h:100–130](../include/ws2812.h) — single-byte volatile reads atomic on Xtensa LX6 | Documented tradeoff (one-frame-old worst case) |

### Low-priority items surfaced for future cleanup

- **Polling `delay(10)` loops** in setup/bootstrap paths ([mqtt_client.h:748](../include/mqtt_client.h) light-sleep drain; [espnow_responder.h:414, 573](../include/espnow_responder.h); [espnow_bootstrap.h:278, 294, 385, 535, 614, 689](../include/espnow_bootstrap.h)). All are setup-only or rare-path; loopTask is not TWDT-subscribed at those points so no immediate WDT risk. Worth converting to event-driven over time but not urgent.
- **Per-peer tracker + ranging math** are owned by another session; brief audit found no critical issues in the persistence + RSSI-to-distance code.

### Audit-driven recommendations (in priority order)

1. **Confirm Phase A passes** (Alpha + Charlie >4 h on v0.4.10.1-dev with no abnormal boot_reason). Currently in progress.
2. **Cut v0.4.11** = v0.4.10.1 LED revert + NDEF feature ([ndef.h](../include/ndef.h), already on disk) + #48/#49 visibility logs ([device_id.h](../include/device_id.h), [app_config.h](../include/app_config.h), already on disk). Single-tag, full release.
3. **Plan v0.4.12** to re-introduce MQTT_HEALTHY via the deferred-flag pattern + add a per-heartbeat `LOG_HEAP` line for fleet-wide leak surveillance.
4. **Bench-isolate Charlie** for 12 h alone — its 136 mosquitto disconnects in 2 days vs ~70 for the rest is the strongest "this device specifically" signal in the data, and it pre-dates v0.4.10. Hardware suspect (#46).
5. **Optional v0.4.13:** stack canary build (`CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`) flashed to one device for 7-day soak. Catches future stack overflows automatically at cost of ~+8 ms per task switch.

---

## 2026-04-26 mid-day update — Phase A INCOMPLETE, new failure modes observed

Phase A timeline + findings since the morning update:

### Timeline of fresh events

| Time  | Event |
|---|---|
| 10:14 | **Foxtrot** `int_wdt` on v0.4.11-dev (had LED hooks). Watcher fire #1. |
| 10:18 | **Alpha hung silently** on v0.4.10.1-dev. WS2812 strip frozen at solid blue, GPIO 2 LED off, no boot_reason — RTC WDT did not bite. CPU+scheduler deadlock that the chip-level safety nets failed to recover from. **New failure mode: silent FreeRTOS deadlock.** Operator power-cycled. |
| 10:25 | Alpha re-flashed to v0.4.11-dev (revert + NDEF + heap heartbeat + visibility) and rejoined fleet. |
| 10:29 | **Cascade** — Delta + Bravo + Charlie all `int_wdt` within 22 s. **Charlie has the LED revert** — the cascade refutes "LED hooks alone are the bug". |
| 10:34 | All three reconnect with `int_wdt` boot reason. |
| ~10:50 | Bravo powered off as #46 chronic-flake suspect (136 mosquitto disconnects in 2 days vs ~70 for the rest, predates v0.4.10). Soak in progress. |

### Updated hypothesis ranking

Phase A's revert-the-hooks experiment FAILED the cleanest reading: Charlie crashed on hooks-reverted firmware. So:

- **Hypothesis #1 (callback path WDT) — partial.** May still cause some `other_wdt` events, but is NOT the only cause. Foxtrot's 10:14 still fits #1 (had hooks); 10:29 cascade does NOT (Charlie reverted).
- **Hypothesis #3 (current spike on Alpha-only strip) — REJECTED again.** Cascade affects multiple devices, only one has a strip wired.
- **NEW Hypothesis #1b — peer-broadcast storm.** ESP-NOW receive callbacks run in interrupt context. A misbehaving peer broadcasting malformed/oversized/too-frequent packets can blow the int_wdt budget on every receiver simultaneously. The 22-s spread of the 10:29 cascade fits a wave of broadcasts hitting peers in radio range as the source's clock drifts. Top candidate transmitter: **Bravo** (chronic flake, predates v0.4.10).
- **NEW failure mode — silent FreeRTOS deadlock (Alpha 10:18).** The chip stayed powered, LED tasks frozen mid-frame, but no reset fired. Worst-case shape: BOTH cores blocked + RTC WDT path silenced. Possible causes: priority inversion holding a mutex IDLE0/1 wants; xQueueSend contention from async_tcp ctx (the very path we suspected for #51); or interrupt cascade exhausting stack mid-handler. This deserves its own diagnostic track separate from #51.

### Phase A status: INCOMPLETE

Cannot declare v0.4.10.1 as the fix. Two distinct failure modes remain:

- (a) `int_wdt` cascade across the fleet under specific conditions (Bravo isolation test in progress).
- (b) Silent deadlock on Alpha after ~70 min uptime (single observation; needs reproduction).

### Revised plan

1. **Wait out the Bravo-off soak** (~1 h). If silent → Bravo was the cascade trigger; bench-isolate it for repair / replace. If cascade recurs → look elsewhere (PC network activity? broadcast storm from a different device? router blip?).
2. **Do NOT cut v0.4.11 yet.** Phase A is inconclusive and we'd be tagging an "improvement" that doesn't fix everything. Wait for clearer signal.
3. **Keep the v0.4.11 working tree** (revert + NDEF + heap + visibility) on disk; Alpha + Foxtrot are running it as live observers. If they crash too, that's the strongest signal that the revert was insufficient.
4. **Plan a panic-trace capture session for Phase B** — leave one device on COM5 with serial monitor running 24 h. Catches the next panic backtrace for `xtensa-esp32-elf-addr2line` decoding.

### 2026-04-26 — BLE disabled, IRAM relief observed

Per #51 diagnostic, BLE was disabled (commenting `#define BLE_ENABLED` in [config.h](../include/config.h)). Build deltas measured against pre-disable v0.4.11-dev:

| Resource | With BLE | No BLE | Delta |
|---|---|---|---|
| Flash (`.flash.text` + `.flash.rodata`) | 93.3 % | **82.1 %** | ~220 KB freed |
| IRAM (`.iram0.text`) | ~98 % | **~73 %** (95 KB / 130 KB) | ~30 KB freed |
| DRAM | 24.9 % | **21.5 %** | ~11 KB freed |

Charlie + Foxtrot now running the no-BLE v0.4.11-dev build as the cascade A/B test bed.

### Historical IRAM context (audit 2026-04-26)

IRAM overflow has been a recurring pressure point. Two significant past events:

| Event | Version | Cause | Fix | In place today |
|---|---|---|---|---|
| Bluedroid → NimBLE migration | v0.0.15 / commit `a23bb05` | Bluedroid BLE stack pushed binary >1.97 MB OTA partition limit | Switched to NimBLE-Arduino, saved ~630 KB **flash** (separate concern from IRAM) | Yes |
| Sleep driver `IRAM_ATTR` | v0.3.20 | `esp_sleep.c` IRAM_ATTR functions wouldn't fit in stock 128 KB | Extended `iram0_0_seg` by 2 KB (0x800) into SRAM1 via `ld/memory.ld` + `modify_link_path.py` linker override | Yes |

The ESP-IDF + arduino-esp32 ship with tight IRAM assumptions; each major feature risks overflow. Pattern: bump `iram0_0_seg len` in 2–4 KB increments, accept proportional heap reduction. **Max safe extension ~32 KB** before runtime heap becomes critical with all subsystems active.

With BLE disabled the firmware no longer needs the 2 KB SRAM1 extension — but we keep it in place for now to avoid linker-script churn during diagnosis. Could be removed in v0.5.0 cleanup if BLE remains off long-term.

### Heap fragmentation & OTA (v0.3.26 — separate from IRAM)

Worth noting because it's often confused with IRAM pressure: v0.3.26 (commit `8e0a6f1`) addressed heap fragmentation during OTA flash writes. `Update.begin()` needs a contiguous 4 KB allocation; mbedTLS (2× 16 KB SSL record buffers) + NimBLE (20–40 KB) fragmented the heap. Fix: `NimBLEDevice::deinit(true)` after MQTT teardown and before flash write, freeing ~30 KB. **Heap pressure, not IRAM.** The device reboots on OTA success so the deinit is safe.

With BLE disabled we no longer need the deinit step in OTA — one fewer code path to maintain. Same #51 reasoning: keep the code in place under `#ifdef BLE_ENABLED` until BLE removal is settled.



---

## #51 RESOLUTION 2026-04-27 — bad_alloc in AsyncMqttClient::publish

The 24-hour diagnostic chase ended in a **captured serial panic backtrace** from Charlie at 02:44 SAST during a network-reconnect cascade (no operator activity at the time).

```
abort() called at PC 0x401ad3f7 on core 1

Backtrace (decoded):
  panic_abort                                  panic.c:477
  abort()                                      abort.c:38
  std::terminate()                             eh_terminate.cc:58
    __cxa_throw                                eh_throw.cc:98          <- bad_alloc
      operator new(unsigned int)               new_op.cc:54            <- FAILED
        AsyncMqttClient::publish               AsyncMqttClient.cpp:742
          mqttPublish                          mqtt_client.h:180
            espnowRangingLoop                  mqtt_client.h:339
              loop                             main.cpp:703
                loopTask                       arduino-esp32 main.cpp:82
```

Last log line before panic: `[I][Responder] BROKER_RESP -> sibling: 192.168.10.30:1883`.

### Interpretation

`AsyncMqttClient::publish()` internally `new`s a contiguous buffer for the framed MQTT message. Under network-reconnect storms the heap **fragments** -- `free` heap stays healthy (~117k observed) but the LARGEST contiguous block drops below what the publish needs. `operator new` throws `std::bad_alloc`. arduino-esp32 doesn't have full C++ exception support -> the throw escalates to `std::terminate()` -> `abort()` -> `boot_reason=panic`.

Trigger: the per-3 s ESP-NOW ranging publish hits the fragmented heap immediately after a network blip.

### Explains both observed cascades

- **2026-04-26 10:34** (Bravo + Delta + Charlie `int_wdt` within 22 s). Charlie was on hooks-reverted v0.4.10.1 yet still crashed -> not LED hooks. Now confirmed bad_alloc under stress.
- **2026-04-27 02:43-44** (Delta + Charlie + Alpha `panic` within 2 min). Pure repro after a broker blip with no operator activity.

### Fix shipped (v0.4.11-dev 2026-04-27)

In [mqtt_client.h::mqttPublish()](../include/mqtt_client.h):

```cpp
#define MQTT_PUBLISH_HEAP_MIN  4096

static void mqttPublish(const char* prefix, const String& payload, ...) {
    if (!_mqttClient.connected()) return;
    if (ESP.getMaxAllocHeap() < MQTT_PUBLISH_HEAP_MIN) {
        // Heap fragmented -- skip rather than risk bad_alloc panic.
        // Rate-limited WARN to avoid flooding.
        return;
    }
    ...
    _mqttClient.publish(...);
}
```

Drop is acceptable because all publishes are either retained (Node-RED replays on subscribe) or cosmetic ranging telemetry where one missed message is harmless.

### Updated failure-mode landscape

| Failure | Status | Notes |
|---|---|---|
| (a) Network-stress `bad_alloc` panic | **FIXED** in v0.4.11-dev | Heap-guard. Affects all devices regardless of BLE state. |
| (b) BLE silent deadlock (~70 min after reconnect) | **WORKAROUND** in v0.4.11-dev | BLE disabled. Real fix needs NimBLE/ESP-NOW/WiFi coexistence audit. |
| (c) Charlie chronic flake (brownout 11:50) | Hardware-side | Cable / port / regulator. Swap mitigates. |
| (d) Misleading `event=boot` on every MQTT reconnect | Logged as #61 | Cosmetic. Defer. |

### Future deeper fix

AsyncMqttClient could be replaced with PubSubClient or a static-buffer-based MQTT client that doesn't `new` per publish. Larger refactor; defer until v0.5.x.
