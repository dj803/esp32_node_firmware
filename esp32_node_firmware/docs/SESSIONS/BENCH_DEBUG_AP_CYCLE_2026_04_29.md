# Bench-debug AP-cycle session — 2026-04-29 morning

First time we captured a #78-territory cascade event with **continuous
serial logging on two devices** through the cascade. Highest-yield #78
diagnostic data of the whole project so far.

## Setup

- **Charlie on COM5** — canary firmware v0.4.20.0, sticky 35 h+ uptime
  pre-event, `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`, OTA_DISABLE
- **Alpha on COM4** — production firmware v0.4.26, ~50 min uptime
  pre-event (had rebooted from this morning's overnight cascade)
- **Bravo, Delta, Echo, Foxtrot** — production v0.4.26, all on mains
  (recently moved from battery), no serial attached
- **Continuous logging:** Python no-reset serial loggers on COM4 + COM5
  via `tools/dev/serial_logger.py`, plus an MQTT-side mosquitto_sub
  capturing all relevant fleet topics
- **Trigger:** operator powered down the WiFi/internet router for ~120s,
  then restored. Captured 09:21:24 to 09:23:24 outage on Alpha's serial
  (120,178 ms outage stamp).

## Logs captured

- [`ALPHA_BENCH_DEBUG_2026_04_29.log`](ALPHA_BENCH_DEBUG_2026_04_29.log) — 23 KB
- [`CHARLIE_BENCH_DEBUG_2026_04_29.log`](CHARLIE_BENCH_DEBUG_2026_04_29.log) — 1.4 KB
- [`MQTT_BENCH_DEBUG_2026_04_29.log`](MQTT_BENCH_DEBUG_2026_04_29.log) — 70 KB

## Result — completely different cascade pattern

**No panics. No reboots. Both serial-attached devices stayed up
throughout the event.** Same physical trigger (router power-cycle)
that produced 4 panic'd devices this morning produced ZERO panics
this time.

This is significant: **the cascade is non-deterministic**. Same
trigger doesn't always produce the same result. Heap state at the
moment of the event matters more than the trigger itself.

## What both devices did

**Charlie (canary v0.4.20.0):**
```
09:21:23.907  [W][MQTT] Disconnected (TCP_DISCONNECTED) - retrying in 1000ms
09:21:23.907  [I][WiFi] Disconnected (reason=200)        # BEACON_TIMEOUT
09:21:23.907  [W][Loop] Wi-Fi lost — entering backoff-retry
09:21:23.907  [I][Loop] WiFi reconnect attempt (next backoff 15000 ms)
              ... 4 attempts with exponential backoff (15s/30s/60s/120s)
09:23:08.944  [W][Loop] WiFi.reconnect() failed 3 times — switching to
              disconnect+begin (BEACON_TIMEOUT workaround)
09:23:26.685  [I][WiFi] Connected - IP: 192.168.10.203
09:23:26.685  [I][Loop] Wi-Fi reconnected after 122786 ms outage
              (silence after this — production-quiet)
```
**Charlie's behaviour is exemplary.** Clean WiFi loss detection, graceful
backoff, BEACON_TIMEOUT workaround engaged on schedule, reconnect
worked. NO ESP-NOW errors logged. NO stack-canary fires.

**Alpha (production v0.4.26):**
```
09:21:24.021  Same disconnect sequence (BEACON_TIMEOUT, reason=200)
09:21:24-09:23:09  Same backoff-retry pattern
09:23:24.201  [I][WiFi] Connected - IP: 192.168.10.69
09:23:24.201  [I][Loop] Wi-Fi reconnected after 120178 ms outage
09:23:24.201  E (...) ESPNOW: esp now not init!         # ← NEW BUG
09:23:24.201  [ESP-NOW Ranging] Beacon send failed: ESP_ERR_ESPNOW_NOT_INIT
              ... repeats every ~3.5s for the entire post-reconnect window
```
**Alpha survived but its ESP-NOW driver broke.** WiFi+MQTT recovered
(no panic), but every subsequent ranging beacon attempt fails because
the ESP-NOW subsystem is uninitialized.

## NEW failure mode found: ESP_ERR_ESPNOW_NOT_INIT post-WiFi-reconnect

**Root cause hypothesis (strong):** Charlie (v0.4.20.0) survived the
WiFi cycle with ESP-NOW intact. Alpha (v0.4.26) didn't. **Something in
the v0.4.20→v0.4.26 firmware changes regressed ESP-NOW reinit on
WiFi recovery.**

The ESP-NOW driver is bound to the WiFi driver's lifecycle. On
ESP-IDF, calling `esp_now_init()` requires WiFi to be started first.
When WiFi disconnects and reconnects, ESP-NOW is implicitly torn down
and needs to be re-initialized. If the firmware doesn't re-init
on WiFi-up, ranging silently breaks.

Charlie has different WiFi/MQTT recovery code than v0.4.26 (some
v0.4.21+ changes — v0.4.22 heap-guard, v0.4.23 mqtt_disconnects, v0.4.26
LED bundle — modified the recovery path). Bisect candidates:
- v0.4.21 — 4-component semver fix
- v0.4.22 — heap-guard hardening (dual-guard + 8 KB + try/catch)
- v0.4.23 — #55 mqtt_disconnects + #76 sub-B/F/H + #29 WDT audit
- v0.4.24 — #76 sub-C/D/I + #75 chaos + #34 Phase 1
- v0.4.25 — #32 heap gates + #34 Phase 2
- v0.4.26 — LED bundle (#19/#20/#21/#22/#23/#31)

The most likely culprit is **#76 recovery+reporting hardening** which
modified the WiFi/MQTT recovery state machine, OR **#32 heap gates** at
boot which may have changed init ordering. A targeted git bisect with
the AP-cycle reproduction recipe would pinpoint the regression.

## Off-serial fleet behaviour (from MQTT log)

- **Foxtrot** — uptime kept climbing (no reboot). Briefly published an
  `event:online` at 09:23:27 with `mqtt_disconnects:1`, then dropped to
  LWT offline at 09:23:50. Confirms WiFi recovered on Foxtrot too, but
  MQTT also dropped post-recovery.
- **Bravo, Delta, Echo** — no boot announcements (didn't reboot), no
  online events captured. Stuck somewhere in the recovery cycle.
- **No new `/diag/coredump` payloads.** The 6 retained coredumps at
  09:24:02 are the same ones from this morning (same exc_pcs, same
  app_sha_prefix). My MQTT subscriber dropped during the outage and
  reconnected at 09:24:02, replaying retained payloads = false-positive
  "new coredumps" if you only look at timestamps.

## Operator's physical observations (post-event, ~09:27)

- Alpha: green LED pulsing = LED state-machine reports MQTT_HEALTHY
  **but MQTT is actually down** (offline LWT retained, no telemetry,
  ESP-NOW broken). **NEW LED-state-bug** — LED transition only happens
  on MQTT_CONNECTED, not on MQTT_DISCONNECTED while WiFi stays up.
- Bravo, Delta, Echo, Foxtrot, Charlie: red power + slow-blue =
  WIFI_CONNECTING. Indicates devices haven't gotten WiFi back, OR
  briefly reconnected then dropped again (which Foxtrot's log confirms).

## Big takeaways

1. **Cascade is non-deterministic.** Same physical trigger (router
   cycle), totally different outcomes from session to session. Heap
   state and timing are what matters.
2. **A new failure mode emerged: silent ESP-NOW driver breakage post-
   WiFi-reconnect.** Distinct from the panic cascade but in the same
   #78-related area. Likely a v0.4.20→v0.4.26 regression in ESP-NOW
   reinit logic.
3. **Charlie (v0.4.20.0 canary) is the "control" build that doesn't
   regress.** It survived this AND every previous cascade event. Its
   firmware version pre-dates whatever introduced the ESP-NOW reinit
   bug.
4. **NEW LED-state bug:** MQTT_HEALTHY transition is one-way. When MQTT
   drops while WiFi remains up, LED stays green-pulsing. Misleading.
5. **Continuous-logging-before-event strategy was VALIDATED.** Without
   serial running pre-event, we'd have caught nothing useful (production
   firmware is silent in steady-state). This becomes the standard
   #78-debug protocol.

## Next steps queued for follow-up sessions

### Immediate (next session)
1. **Bisect the v0.4.20.0 → v0.4.26 ESP-NOW reinit regression.** Build
   each intermediate version, test against the AP-cycle reproduction
   recipe, identify which commit introduced the bug.
2. **Add ESP-NOW reinit on WiFi-up event.** Likely a 5-10 line patch
   in the WiFi state-change handler that calls `esp_now_init()` +
   re-registers the recv callback + re-adds peers.
3. **Fix the LED state-machine bug** for MQTT_DISCONNECTED while
   WiFi-up. Add a transition in `mqttHeartbeat()` or
   `onMqttDisconnect()` that drives the LED off MQTT_HEALTHY when MQTT
   drops. Pairs with #44 RESOLVED — same state machine, missing
   transition.

### Eventually
4. **Decode the historical exc_pcs** (Bravo wifi 0x401d2c66, Alpha
   async_tcp 0x00000019) against v0.4.26 ELF to confirm whether the
   earlier panic cascades are also rooted in the same WiFi/ESP-NOW
   tear-down race that this session surfaced as silent breakage.
5. **Re-run AP-cycle test post-fix** to verify both ESP-NOW reinit
   AND the LED transition work as expected.

## Bench state at session end (2026-04-29 09:30)

- All 5 in-room devices alive but in degraded MQTT state (offline LWT
  retained on broker, blue-blinking LEDs except Alpha's misleading
  green-pulsing)
- Charlie alive on canary, post-WiFi-reconnect, MQTT state unknown
- Recovery may take time (devices' MQTT backoff is at higher steps now)
  OR may require operator power-cycle to fully restore
- 6 retained `/diag/coredump` payloads on broker — same set as this
  morning, NO new ones from this session
