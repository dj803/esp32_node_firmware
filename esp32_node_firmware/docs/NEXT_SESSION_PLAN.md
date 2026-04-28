# Next session plan

Drafted 2026-04-28 morning, after the STABILITY DEEP-DIVE session
(2026-04-28 ~07:55 → 09:30 SAST) closed cleanly. v0.4.22 cut + fleet
OTA'd; #46/#51 root cause completion + #83 mosquitto.log fix + #84
agent verification discipline all SHIPPED.

## State at last sweep (2026-04-28 ~09:30 SAST)

| | |
|---|---|
| Fleet | 5/6 on **v0.4.22 release** (Alpha, Bravo, Delta, Echo, Foxtrot); Charlie still on **v0.4.20.0 canary** sticky via OTA_DISABLE |
| Charlie soak | 10.7+ h continuous, heap pinned at 130776, no panic, no canary trip |
| Backlog | OPEN 47, RESOLVED 32, WONT_DO 5 |
| Open coredump | None unresolved — Alpha's loopTask v0.4.20 panic fully decoded as the same bad_alloc shape from #51, fixed in v0.4.22 |

## Recommended next session — v0.5.0 RELAY + HALL HARDWARE Phase 1

The diagnostic safety-net is now mature enough to support feature work:

- v0.4.17 coredump-to-flash captures any panic backtrace without serial
- v0.4.21 restart_cause distinguishes software-restart causes
- v0.4.22 dual-guard + try/catch around the bad_alloc class (#46/#51)
- Charlie canary continues to detect stack overflows silently
- ota-monitor.sh + verify-after-action discipline mean rollouts won't
  silently fail

Open the [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md) plan and
ship Phase 1:

### Scope (single session, ~3-4 h)

1. **Hardware bring-up on bench device.** Pick Bravo (no peripherals,
   already the active flashable bench rig). Wire BDD relay board
   (5V VCC + JD-VCC bridge, IN1=GPIO25, IN2=GPIO26) and BMT 49E Hall
   sensor (VIN + 2×10kΩ divider to ADC1 GPIO32, DO to GPIO33). Verify
   no boot brownout with both attached.

2. **`include/relay.h`** mirroring `ws2812.h` shape:
   - `#ifdef RELAY_ENABLED` gate.
   - `relayInit()`: pinMode + drive HIGH BEFORE OUTPUT (avoid boot
     click), restore from NVS namespace `esp32relay`.
   - `relaySet(ch, bool)`, `relayGet(ch)`, persisted via Preferences.
   - MQTT handler in `mqtt_client.h`: `cmd/relay` retained JSON
     `{"ch":1,"state":true}`.
   - Heartbeat advertises `relay_enabled:true` and `relay_state:[bool,bool]`.

3. **`include/hall.h`** for the Hall sensor:
   - `#ifdef HALL_ENABLED` gate.
   - ADC1 single-shot read with 11 dB attenuation, 12-bit resolution.
   - Reading published periodically via `+/sensor/hall` (separate from
     `+/status` heartbeat).
   - Optional digital threshold from D0 (LM393 comparator) for low-
     latency interrupt.

4. **Validate end-to-end** on Bravo:
   - cmd/relay ch=1 state=true → relay clicks, GPIO25 goes LOW (relay
     active-low), heartbeat reflects new state.
   - Magnet swept past Hall → ADC value moves; D0 threshold fires.
   - Reboot Bravo via cmd/restart → relay state restored from NVS,
     no boot-click on either channel.

5. **Document** in PLAN_RELAY_HALL_v0.5.0.md what shipped + what's
   deferred to Phase 2 (Node-RED dashboard tile, MQTT command shape,
   variant-build integration with #71).

### After Phase 1

- Variant build (#71) story extended with `[env:esp32dev_relay_hall]`.
- Node-RED dashboard tile (Phase 2 of v0.5.0) — operator-facing, NOT
  in autonomous scope per DO NOT.
- Cut v0.5.0 release once at least one device is wearing the hardware
  in production.

## Alternative single-session paths if v0.5.0 isn't ready

### #76 long-tail batch (sub-B/C/D/F/H/I) — ~3 h

Now that v0.4.22 has the heap-guard fix, the original sub-B (NVS ring
buffer of last-N restart contexts) is the natural next builder on top
of sub-G's `restart_cause`. Bundle with sub-F (WDT timeout 5s → 12s)
into a single v0.4.23 patch.

### #78 deep dive — needs more diagnostic data

The async_tcp _error race is now the only known latent stability bug.
Charlie's canary at 10+ h with no trip suggests it's NOT stack overflow.
Continue waiting for Charlie's canary to either trip or hit a 7-day
clean mark before deciding the next move.

## Long-tail observations to keep watching

- **Alpha v0.4.22**: tonight's panic was on v0.4.20. v0.4.22 has the
  hardened guards. If Alpha re-panics on v0.4.22 with the same
  bad_alloc backtrace, the guard threshold needs further raising or
  PubSubClient swap is on the table.
- **Charlie canary**: still sticky thanks to OTA_DISABLE. Any canary
  halt would be silent on MQTT (visible only on serial); LWT-only
  state is the only MQTT-side signal.
- **#46 Recent abnormal reboots**: with coredump-to-flash + restart_cause
  + heap-guard fix, the next /diag/coredump appearance has clean
  diagnostic context built in.
- **mosquitto.log**: now writing again. Confirm operator registers the
  daily MosquittoLogRotate scheduled task per #83 archive entry.

## Followups not on the critical path (≤1 h each)

- **#27** Library-API regression test in CI — promote `lib_api_assert.h`
  from compile-only to a CI gate.
- **#29** WDT-heartbeat audit — formal sweep across blocking I/O sites.
- **#36** Heartbeat / boot-reason monitoring tile in Node-RED Dashboard
  2.0 (firmware-side data exists since v0.4.11).
- **#48** UUID drift root cause — Bravo's NVS-wipe rotation tonight is
  benign + expected; Delta/Echo's earlier drift may also have been
  erase-flash.
- **#55** AsyncMqttClient malformed-packet counter — heartbeat payload
  addition.
- **#69** Wakeup vs persistent-monitor preemption — apply A+C+E from
  the archive entry.

## Won't do this session

- **v0.5.0 hardware Phase 2** (Node-RED dashboard) — DO NOT for autonomous
  per template.
- **#78 fix attempt** — premature without more diagnostic data.
- **#61 / #68** (mosquitto auth, Node-RED adminAuth) — explicitly
  deferred until end of dev phase.
