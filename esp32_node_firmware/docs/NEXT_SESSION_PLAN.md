# Next session plan

Drafted 2026-04-28 mid-morning, after the STABILITY DEEP-DIVE session
(2026-04-28 ~07:55 → 09:30 SAST) AND the v0.5.0 Phase 1 code-only ship
(2026-04-28 ~09:50 → 10:20). v0.4.22 cut + fleet OTA'd; #46/#51 root
cause completion + #83 mosquitto.log fix + #84 agent verification
discipline all SHIPPED. relay.h + hall.h drivers scaffolded behind
default-disabled gates; both esp32dev and esp32dev_relay_hall compile
clean. NO devices flashed yet — hardware-presence verification needed.

## State at last sweep (2026-04-28 ~09:30 SAST)

| | |
|---|---|
| Fleet | 5/6 on **v0.4.22 release** (Alpha, Bravo, Delta, Echo, Foxtrot); Charlie still on **v0.4.20.0 canary** sticky via OTA_DISABLE |
| Charlie soak | 10.7+ h continuous, heap pinned at 130776, no panic, no canary trip |
| Backlog | OPEN 47, RESOLVED 32, WONT_DO 5 |
| Open coredump | None unresolved — Alpha's loopTask v0.4.20 panic fully decoded as the same bad_alloc shape from #51, fixed in v0.4.22 |

## Recommended next session — v0.5.0 Phase 2: HARDWARE BRING-UP

Phase 1 (code-only scaffolding) shipped this session in commit cc0a074:
- `include/relay.h` driver with active-LOW logic + boot-click guard +
  NVS state persistence at namespace "esp32relay".
- `include/hall.h` driver with ADC1 11 dB read + 8-sample average +
  Gauss conversion via the 2× divider + per-unit offset calibration
  via cmd/hall/zero.
- `cmd/relay`, `cmd/hall/config`, `cmd/hall/zero` MQTT handlers in
  `mqtt_client.h`, each `#ifdef`-gated.
- `relay_enabled` + `hall_enabled` heartbeat fields.
- `[env:esp32dev_relay_hall]` PIO env enabling both flags via
  `-DRELAY_ENABLED -DHALL_ENABLED`.
- Both `esp32dev` (default paths) and `esp32dev_relay_hall` (full
  paths) compile clean.

Phase 2 needs HARDWARE on the bench — that's the operator action that
gates this session.

### Pre-session prep (operator hands-on, ~30 min)

Wire the BDD 2CH relay + BMT 49E Hall sensor to Bravo per the table
in PLAN_RELAY_HALL_v0.5.0.md "Hardware summary" + "GPIO inventory":

| Module | ESP32 GPIO | ESP32 power | Notes |
|---|---|---|---|
| Relay VCC | — | 5V | signal-side opto supply |
| Relay GND | — | GND | common |
| Relay IN1 | **GPIO 25** | — | active-LOW |
| Relay IN2 | **GPIO 26** | — | active-LOW |
| Relay JD-VCC | — | **separate 5V supply** | ~150 mA inrush; ESP32 5V will brownout |
| Hall VIN/VCC | — | 5V | SS49E spec 4.5–10.5 V; 3.3 V is OUT OF SPEC |
| Hall GND | — | GND | common |
| Hall AO | **GPIO 32** | — | via 2×10 kΩ divider (5 V → 2.5 V; ADC max 3.3 V) |
| Hall DO | **GPIO 33** *(optional)* | — | LM393 threshold output |

Verify no boot-brownout with both modules attached.

### Session scope (~2 h)

1. **Capture Bravo's pre-flash state** (firmware version, uptime,
   last boot reason) per the verify-after-action / pre-reflash
   discipline (#84). Stop any watcher holding COM4.

2. **USB-flash `esp32dev_relay_hall` to Bravo:**
   ```
   pio run -e esp32dev_relay_hall -t upload --upload-port COM4
   ```
   Bravo will report `firmware_version="0.4.22.0"` (local-build dev
   variant of v0.4.22 with the gates flipped on). Heartbeat will
   advertise `relay_enabled:true,hall_enabled:true`.

3. **Validate relay end-to-end:**
   ```
   mosquitto_pub -t '<bravo>/cmd/relay' -m '{"ch":1,"state":true}'
   ```
   Expect: relay 1 clicks closed, MQTT `status/relay` retained
   `{"ch1":true,"ch2":false}`, log line `Relay ch1 → ON (pin 25)`.

   Then `cmd/restart` Bravo. After reboot, relay state should
   self-restore from NVS without a boot-click.

4. **Validate Hall end-to-end:**
   - `mosquitto_sub -t '<bravo>/telemetry/hall'` — confirm periodic
     `{"voltage_v":..,"gauss":..,"above_threshold":..}` at the
     default 1 s cadence.
   - `cmd/hall/zero` with no magnet near — captures the current
     reading as offset, persists to NVS.
   - Sweep a magnet past the sensor — `gauss` value moves; on
     threshold cross, `telemetry/hall/edge` fires with
     `{"edge":"rising"|"falling",...}`.
   - `cmd/hall/config '{"interval_ms":500,"threshold_gauss":30}'` —
     confirm cadence + threshold change.

5. **M1 + M2 chaos** to confirm the relay/hall paths don't add new
   failure surface (per Test-after-change discipline). Skip M3
   unless the M2 outcome warrants tightening.

6. **Document** what shipped + what's deferred (Node-RED dashboard
   tile is operator-visible per DO NOT; defer to a manual operator
   session).

7. **Cut v0.5.0 release** if the bench validation lands clean. Bravo
   is on the variant binary; the release manifest stays at v0.4.22
   for the rest of the fleet OR we update PLAN_RELAY_HALL with a
   per-device variant-aware OTA story (the per-variant manifest from
   #71).

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
