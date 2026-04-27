# Chaos testing — failure injection for fleet resilience

> Created 2026-04-27 after the v0.4.13 panic cascade was reproduced and root-caused
> via a synthetic broker blip. The blip exposed a use-after-free race in
> me-no-dev/AsyncTCP. v0.4.14 switches to esphome/AsyncTCP. This doc captures
> the testing approach so the bug class can't reach production silently again.

## Why chaos testing is non-optional for this fleet

The 6 ESP32s talk to each other via ESP-NOW, to the broker via MQTT-over-Wi-Fi,
and (sometimes) to BLE beacons or RFID tags. Any one of those subsystems can
fail under field conditions: AP reboots, mosquitto restarts for log rotation,
Wi-Fi channel hops, BLE coexistence stalls, RFID reader brown-outs.

Stable bench behaviour is necessary but not sufficient. The 10:42 cascade was
**only** triggered by network instability — every device-side metric was green
right up to the moment they all panicked. Synthetic failure injection is the
only way to catch this class before deployment.

## Active watchers (run during every chaos test)

These should be persistent across the test session so events stream in real time.

1. **`tools/silent_watcher.sh`** — alerts on LWT-offline + abnormal `boot_reason`
   (panic, *_wdt, brownout). Single source of truth for "is the fleet alive?"
2. **Charlie serial monitor** (no DTR/RTS toggle, panic/boot lines only):
   ```python
   import serial
   s = serial.Serial(); s.port='COM5'; s.baudrate=115200
   s.dtr=False; s.rts=False; s.open()
   # filter for: 'Guru', 'panic', 'PC', 'EXCVADDR', 'Backtrace', 'LoadProhibited',
   #             'assert', 'abort', 'rst:', 'ets '
   ```
   Captures the panic header + register dump + backtrace which RTC WDT reset
   would otherwise wipe. Without this you get `boot_reason=panic` and nothing else.
3. **Heap CSV logger** for any device under suspicion — append `heap_free,heap_largest`
   from each heartbeat to a file. Shape of the curve before failure is the key signal.

## Mosquitto chaos

### M1 — short blip (5 s)
```powershell
net stop mosquitto; Start-Sleep 5; net start mosquitto
```
**Expected:** all devices LWT, reconnect cleanly within ~10 s, publish `event=online` (per #61).
**Observed v0.4.13 (2026-04-27):** clean reconnect, no panics. Pass.

### M2 — long blip (30 s)
```powershell
net stop mosquitto; Start-Sleep 30; net start mosquitto
```
**Expected:** devices hit reconnect backoff (1→2→4→8→16 s). On reconnect, all should publish ONLINE without panic.
**Observed v0.4.13 (2026-04-27 14:04 SAST):** **5/6 panicked, 1 hit `int_wdt`**. Backtrace decoded to `tcp_arg` in lwIP via `AsyncClient::_error` in AsyncTCP. Fixed in v0.4.14 by switching to `esphome/AsyncTCP`.

### M3 — long blip (180 s)
Same shape, 3-minute outage. Drives the reconnect timer to its 60 s cap and
exercises Tier 1 broker rediscovery (`mqttNeedsRediscovery()` after
`MQTT_REDISCOVERY_THRESHOLD` failures).

### M4 — rapid succession (3 blips, 5 s apart)
```powershell
1..3 | ForEach-Object { net stop mosquitto; Start-Sleep 2; net start mosquitto; Start-Sleep 5 }
```
Stresses the half-open connection cleanup path. Race window where AsyncTCP is
mid-error-handler when a new disconnect arrives.

### M5 — broker config change without restart
```powershell
# Edit mosquitto.conf, then SIGHUP-equivalent on Windows:
Restart-Service mosquitto
```
Tests that retained-message republish on subscribe still works after broker config drift.

### M6 — mosquitto.log rotation under load
Force-fire the daily rotation script during heartbeat traffic:
```powershell
schtasks /run /tn "MosquittoLogRotate"
```
Tests the existing `rotate-log.ps1` doesn't drop messages or corrupt LWT state.

## Wi-Fi chaos

### W1 — host Wi-Fi adapter cycle
```powershell
Get-NetAdapter -Name "Wi-Fi*" | Disable-NetAdapter -Confirm:$false; Start-Sleep 10; Get-NetAdapter -Name "Wi-Fi*" | Enable-NetAdapter -Confirm:$false
```
Note: this drops the **host's** connection, not the devices'. Devices stay on the AP. Use to test Node-RED + daily-health resilience.

### W2 — AP reboot (manual)
Power-cycle the Wi-Fi AP. Drops all devices simultaneously. Most realistic
production failure — the 10:42 cascade pattern.
**Expected:** devices retry Wi-Fi association, then MQTT, then publish ONLINE.
**Watch for:** `boot_reason=panic` (firmware bug) vs `boot_reason=software` (a
controlled `MQTT unrecoverable → ESP.restart()` after `MQTT_RESTART_THRESHOLD`).

### W3 — channel hop
If the AP supports it, force a 5 GHz channel reassignment. Devices on 2.4 GHz
keep their channel; this tests cross-channel ESP-NOW silently failing.

### W4 — RSSI degradation (hard to script)
Move the AP further away. Document RSSI crossover where heartbeats start dropping.

### W5 — DHCP exhaustion / lease expiry
Set DHCP lease to a very short interval (e.g. 60 s) on the AP. Device renews
the lease mid-MQTT-traffic. Catches any `WiFi.localIP()` cached-pointer bugs.

## ESP-NOW chaos

### EN1 — disable + re-enable mid-flight
```bash
mosquitto_pub -h 192.168.10.30 -r -t '.../UUID/cmd/espnow/ranging' -m '0'
# wait 30 s
mosquitto_pub -h 192.168.10.30 -r -t '.../UUID/cmd/espnow/ranging' -m '1'
```
Tests `espnowRangingSetEnabled(false)` cleanly tears down the responder + tx tasks.

### EN2 — flood test
Use `espnow_inject` (if it exists) or a host-side ESP32 to flood ranging
broadcasts. Verifies the receiver doesn't OOM or stack-overflow the handler task.

### EN3 — peer-table churn
Programmatically cycle the tracked-MAC list every few seconds via
`cmd/espnow/track`. Tests `espnowSetTrackedMacs` is allocation-safe.

### EN4 — channel mismatch
Move one device to a different Wi-Fi channel. ESP-NOW is silent across channels.
Verify the silent-failure dashboard tile (#60) flags it.

## BLE chaos

### B1 — coexistence soak (Path C Phase 1)
BLE_ENABLED + BLE_BENCH_RIG (10 % scan duty), Wi-Fi + MQTT live, no peers.
Run for 24 h. Verify scheduler doesn't deadlock at the documented 70 min mark.
**Status (2026-04-27):** Bravo at 3 h 27 min, no hang. In progress.

### B2 — beacon flood
Place many BLE advertisers in range. NimBLE scan callback fires fast — verify
the `_bleMutex` (timeout 0) drops on contention rather than blocking.

### B3 — toggle BLE while MQTT is reconnecting
Send `cmd/ble/scan` during a synthetic broker blip. Verifies the BLE post path
doesn't cross-corrupt MQTT reconnect state.

## RFID chaos

### R1 — rapid reader hot-plug
Disconnect/reconnect MFRC522 SPI bus mid-scan (only on bench, hardware-side).
Verifies the `PCD_GetVersion() returned 0xFF` "no reader" path doesn't crash.

### R2 — concurrent NDEF-write + scan
Send `cmd/rfid/ndef_write` and present a different tag mid-arming.

## OTA chaos

### O1 — OTA mid-flight cancel
Trigger OTA, then `taskkill /F` the upload server. Device should hit progress
watchdog (`OTA_PROGRESS_TIMEOUT_MS`) and roll back via `_otaValidationTick`.

### O2 — OTA to a bad URL
Edit OTA manifest to point at a 404. Verify `OTA_FAILED` event publishes and
device stays on current version.

### O3 — OTA to a corrupted bin
Edit OTA manifest to point at a truncated bin. Verify checksum fails and
rollback fires.

## Heap chaos

### H1 — sustained publish storm
Have Node-RED publish many `cmd/...` messages per second to one device.
Verify `mqttPublish()`'s heap-guard skips correctly when `getMaxAllocHeap`
drops below `MQTT_PUBLISH_HEAP_MIN`. Heap should recover; no panic.

### H2 — large payload publish
Publish a 6 KB JSON to `cmd/led`. Tests the `JsonDocument` parser bounds.

## Cross-tool / integration

### I1 — Node-RED restart during heartbeat
```bash
sudo systemctl restart nodered  # or kill + bat re-run on Windows
```
Devices keep publishing; Node-RED catches up via retained on subscribe.

### I2 — host clock skew
Set host clock back 5 minutes. Verify TLS still works for OTA pull (pioarduino
TLS does NOT enforce time normally) and credential rotation timestamp comparison
isn't broken (#7).

### I3 — full power blackout
Power off everything for 5 min. Power back. All devices should boot, find broker
via cached → mDNS → port-scan, reconnect. Tests `tryCachedBrokers` priority.

## Deployment-time tests (when devices ship to a new site)

Bundle into a single `tools/site_acceptance.sh` script that the operator runs
before declaring the site "live":

| Step | Action | Pass condition |
|---|---|---|
| 1 | All devices online in `/daily-health` | 6 G, 0 Y, 0 R |
| 2 | `firmware_version` matches expected | exact match |
| 3 | M1 short blip | 6 / 6 RECOVERED within 10 s |
| 4 | M2 long blip (30 s) | 6 / 6 RECOVERED, 0 panics |
| 5 | M4 rapid succession (3 × 5 s blip) | 6 / 6 stable, 0 panics |
| 6 | EN1 ESP-NOW toggle | ranging publishes resume within 5 s |
| 7 | O2 OTA bad URL | `OTA_FAILED`, device stays on current ver |
| 8 | I3 full blackout (manual) | all 6 reconnect within 60 s of power restore |

Steps 3-7 are the chaos suite. Step 8 is the realistic worst case. Failing any
step blocks site acceptance.

## Test framework integration

This is a manual / semi-manual test plan today. To make it a real framework:

- **`tools/chaos/`** directory with one script per scenario above.
- **`tools/chaos/runner.sh`** orchestrates: arm watchers, run scenario, collect
  events for N seconds, decide pass/fail, write JSON report.
- **CI hook** — pre-release, runs M1-M4 + EN1 against a smoke fleet (one device).
- **Field hook** — `tools/site_acceptance.sh` from the table above; mandatory
  before flipping a deployment to "live".

`broker_blip.ps1` (already shipped) is the seed of this — generalise it to one
script per failure mode.

## What we still don't have a test for

- **AP firmware update mid-flight** — would need network admin coordination.
- **Power-supply brownout below ESP32 brown-out threshold** — needs bench rig
  with programmable PSU. Tracked under #59 (hardware lab discipline).
- **Cosmic ray / single-event upset** — unmitigated by software. ECC RAM would
  help but ESP32 doesn't have it.
- **Cabal of malicious devices** — if an attacker has Wi-Fi creds, they can
  publish anything to the broker. Mitigation = MQTT auth + TLS (#7), tracked.

## Findings from this session (2026-04-27)

- **M1 short blip (5 s, v0.4.13):** PASS — clean reconnect.
- **M2 long blip (30 s, v0.4.13):** **FAIL — fleet-wide panic cascade**. Root cause:
  `me-no-dev/AsyncTCP` use-after-free in `tcp_arg`. Captured backtrace 14:04 SAST
  via Charlie serial (no-DTR/RTS-toggle mode):
  ```
  Guru Meditation Error: Core 1 panic'ed (LoadStoreAlignment)
  PC: 0x4012210e (tcp_arg @ lwip/tcp.c:2039)
  ← AsyncClient::_error @ AsyncTCP.cpp:1024
  ← _async_service_task @ AsyncTCP.cpp:307
  ```
- The same shape almost certainly explains the v0.4.10 (#51) and 10:42 cascades.

### Diagnosis revision (after M1, M2 ×3, M4)

The "AsyncTCP race during disconnect" hypothesis was **wrong**. M1 (5 s) and
M4 (3 × 5 s rapid) both passed cleanly across the entire fleet on me-no-dev
and on mathieucarbou. Only M2 (30 s) reproduced the cascade. The trigger
isn't the disconnect path; it's the **firmware's own self-heal threshold**:

```c
// main.cpp:674
if (mqttIsHung()) {  // millis() - _mqttConnectStartMs >= 12000
    LOG_W("Loop", "MQTT hung (no callback) — restarting device");
    ESP.restart();
}
```

`MQTT_HUNG_TIMEOUT_MS = 12000` was tuned for the "TCP up, MQTT CONNACK never
arrived" hang. But the same path fires on the much more common "broker down,
lwIP SYN retrying" case — lwIP's TCP connect timeout is ~75 s by default. So
any broker outage longer than 12 s caused every device to ESP.restart()
simultaneously, producing the cascade. The AsyncTCP tcp_arg use-after-free
panic was a **secondary** effect of the restart-storm, not the trigger.

### v0.4.14 fix shipped: MQTT_HUNG_TIMEOUT_MS 12 s → 90 s

```c
#define MQTT_HUNG_TIMEOUT_MS  90000   // gives lwIP SYN room to fail naturally
```

Validation (M2 30 s blip at 15:06:44 SAST, 2026-04-27):

| Device | Build | Result |
|---|---|---|
| **Charlie** | **v0.4.14 (90 s timeout, mathieucarbou)** | **clean reconnect 34 s, no restart** ✅ |
| Foxtrot/Delta/Echo | v0.4.13 (12 s timeout, me-no-dev) | panic |
| Alpha/Bravo | v0.4.13 (12 s timeout, me-no-dev) | int_wdt |

The restart-storm is gone. `event=online` (#61) publishes confirm clean reconnect.

### v0.4.14 also bundles esphome → mathieucarbou AsyncTCP swap

mathieucarbou/AsyncTCP v3.3.2 doesn't fix the cascade (the cascade was the
hung-timeout). Its contribution: under int_wdt-class crashes (which still
*can* happen under sustained network instability) it gives a **recoverable
watchdog reset** instead of a **memory-corruption panic**. That's strictly
better for postmortem (`boot_reason=int_wdt` vs unrecoverable `panic` with
the heap state lost).

### Legacy (do not delete) — esphome/AsyncTCP attempt rolled back

- Pinned `esphome/AsyncTCP@dc64fedec0c953ba4f52b6bb8bc5ef1a3abdc22d` (v2.0.1, pre-IPv6).
  v2.1.4 was first choice but requires `IPv6Address.h` not shipped by arduino-esp32 3.3.8 (#63).
- Built clean. Flashed Charlie (COM5).
- **Boot-looped immediately:** `assert failed: tcp_alloc /IDF/components/lwip/lwip/src/core/tcp.c:1854 (Required to lock TCPIP core functionality!)`. The lwIP TCPIP-core-locking model in arduino-esp32 3.3.8 is stricter than what esphome/AsyncTCP v2.0.1 expects — it assumes lwIP calls happen from inside the TCPIP task or with the core lock held; v2.0.1 calls from another task without acquiring the lock.
- Reverted Charlie to me-no-dev/AsyncTCP (functional but race remains).
- v0.4.14 release **PARKED** until a compatible fork is identified.

### v0.4.15 fleet-wide validation (2026-04-27 17:29 SAST)

After OTA stagger to v0.4.15 across the fleet, M2 30 s blip results:

| Device | Build | Result | Uptime preserved? |
|---|---|---|---|
| Alpha | v0.4.15 release | online via #61 | yes (492 s → 492 s) |
| Delta | v0.4.15 release | online via #61 | yes (337 s → 337 s) |
| Echo | v0.4.15 release | online via #61 | yes (204 s → 204 s) |
| Foxtrot | v0.4.15 release | online via #61 | yes (157 s → 157 s) |
| Bravo | v0.4.15-dev | online via #61 | yes (1830 s → 1830 s) |
| Charlie | v0.4.15-dev | online via #61 | yes (1832 s → 1832 s) |

**0 panics, 0 abnormal boots, 0 ESP.restart() — clean reconnect across the entire fleet.**

EN1 (ESP-NOW ranging toggle on Alpha) — PASS, espnow stream resumed after `cmd/espnow/ranging '1'`.

O2 (OTA-to-bad-URL) — skipped (would require corrupting live Pages manifest).
I1 (Node-RED restart) — skipped (would interrupt watchers + dashboard).

### M3 (180 s blip) — KNOWN LIMITATION

Even after v0.4.15's `MQTT_HUNG_TIMEOUT_MS=300000` + `mqttForceDisconnect()` (no
ESP.restart()), M3 still cascades. Charlie + Bravo on v0.4.15-dev panicked
during the 180 s outage window at ~113 s — same `tcp_arg` PC as M2 panic on
v0.4.13, but multiple distinct exception shapes across reboots
(LoadStoreAlignment, StoreProhibited, InstructionFetchError) indicating
unrecoverable memory corruption in AsyncTCP.

The bug fires inside `AsyncClient::_error` when lwIP's natural TCP-timeout
error callback runs (~75-90 s into a connect attempt against a dead broker).
**No firmware-level change can prevent this** — the race is between AsyncTCP's
async-task and lwIP's TCP timer, both inside the libraries.

**Workarounds available to firmware:**
- Skip MQTT reconnect attempts when broker is known down (probe before connect).
- Replace AsyncMqttClient + AsyncTCP with a synchronous stack (PubSubClient).
- Patch AsyncTCP locally to lock around `tcp_arg`/error-callback paths.

For typical production (mosquitto log rotation = 5-10 s outage), v0.4.15 is
clean. The M3 class hits in long AP outages, broker host crashes, network
maintenance windows. Mitigation: avoid maintenance windows > 75 s, or accept
device reboot recovery.

### Next mitigation candidates

1. **mathieucarbou/AsyncTCP** — heavily maintained, used by ESP-Async-WebServer. Test next.
2. **Patch me-no-dev/AsyncTCP locally** — backport just the `_error` / `tcp_arg` race fix from esphome.
3. **Bump arduino-esp32 framework** to a version that ships `IPv6Address.h`, then use esphome/AsyncTCP v2.1.4.
4. **Replace AsyncMqttClient + AsyncTCP entirely** with PubSubClient (synchronous, no race surface).

In the meantime: the heap-guard in `mqttPublish()` (v0.4.11) reduces cascade frequency under fragmentation pressure, and chaos-tests are documented so the bug class is at least visible.

## Future improvements

- IPv6 support — would unblock newer esphome/AsyncTCP (v2.1.4+) which has further
  hardening. Currently pinned to v2.0.1 because arduino-esp32 3.3.8 doesn't ship
  `IPv6Address.h`. Tracked separately; low priority while LAN is IPv4-only.
- Remote chaos trigger — a `cmd/chaos` topic that takes a JSON `{"scenario":"M2"}`
  and runs it. Would let a site operator self-test without shell access.
- Chaos events as MQTT — every triggered scenario publishes
  `health/chaos/<scenario>` so the daily-health summary captures test history.
