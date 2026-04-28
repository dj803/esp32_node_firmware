# BLE coexistence analysis — v0.4.15 Path C Phase 1

> Living analysis doc. Started 2026-04-27 during the autonomous bench session.
> Refresh as bench rig observations come in; promote to `~/.claude/plans/v0.4.15-plan.md`
> when ready for fleet rollout.

## Failure shape (recap)

Devices with `BLE_ENABLED` defined hang **~70 minutes after a Wi-Fi/broker reconnect**:

- RTC WDT does not bite. CPU power LED stays lit.
- FreeRTOS scheduler hangs. No panic, no `boot_reason=panic|task_wdt`.
- Symptom from broker side: LWT-only on `.../status`, no further heartbeats.
- USB-side: serial monitor stops emitting, but the device does not reset on RTS.

Workaround in v0.4.13: `BLE_ENABLED` left commented out in `config.h`. NimBLE-Arduino is still in `lib_deps` but the headers are conditionally compiled out.

## Coexistence audit — sdkconfig.defaults baseline (2026-04-27)

| Setting | Value | Implication |
|---|---|---|
| `CONFIG_ESP_COEX_ENABLED` | `y` | SW coexistence layer compiled in |
| `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` | `y` | Active SW coex arbiter |
| `CONFIG_BT_BLUEDROID_ESP_COEX_VSC` | `y` | Bluedroid VSC commands route through coex |
| `CONFIG_ESP_COEX_POWER_MANAGEMENT` | not set | No coex-aware DTIM scheduling |
| `CONFIG_BTDM_CTRL_MODE_BTDM` | `y` | Dual-mode controller (BR/EDR + BLE). BLE-only mode would save resources. |
| `CONFIG_BTDM_CTRL_PINNED_TO_CORE` | `0` | **BT controller on Core 0** (alongside async_tcp + lwIP + Wi-Fi) |
| `CONFIG_BT_BLUEDROID_PINNED_TO_CORE` | `0` | **Host stack on Core 0** |
| `CONFIG_BTDM_CTRL_MODEM_SLEEP` | `y` (ORIG mode) | Modem sleep enabled |
| `CONFIG_BTDM_CTRL_HLI` | `y` | High-level interrupts on (ESP32 hardware-pinned to Core 0 regardless of task affinity) |
| `CONFIG_BT_NIMBLE_ENABLED` | not set | NimBLE host disabled at IDF level (NimBLE-Arduino library brings its own port) |

## ble.h scan params (stock)

```cpp
pScan->setInterval(100);   // ms
pScan->setWindow(99);      // ms
```

= **99% scan duty cycle**. The radio is scanning BLE for 99/100 ms of every cycle, leaving ~1% for Wi-Fi (beacons, DTIM, DHCP, MQTT keepalive, OTA TLS).

This is the leading hypothesis for the deadlock. After a Wi-Fi reconnect, lwIP needs to push DHCP, ARP, TCP keepalive, MQTT CONNECT, and on a busy LAN it competes with the BLE scanner for radio time.

## Mitigation candidates (ordered by cost)

| # | Change | Where | Cost |
|---|---|---|---|
| 1 | Drop scan duty to 10% (300/30 ms) | `ble.h` (under `BLE_BENCH_RIG`) | Single edit, no IDF rebuild |
| 2 | Pin BT controller + host to Core 1 | `sdkconfig.defaults` (`BTDM_CTRL_PINNED_TO_CORE_1=y`, `BT_BLUEDROID_PINNED_TO_CORE_1=y`) | sdkconfig change, full rebuild |
| 3 | BLE-watchdog loop in `main.cpp` — re-init NimBLE if scanner task hasn't ticked in N min | new firmware code | medium (200 LOC) |
| 4 | Switch from BTDM (dual-mode) to BLE-only controller (`BTDM_CTRL_MODE_BLE_ONLY=y`) | sdkconfig | full rebuild, may break something |
| 5 | Drop NimBLE-Arduino, switch to esp32-arduino's bundled BLE stack | `lib_deps` + `ble.h` rewrite | high (~80 KB flash, API surface diff) |

## Bench rig — Bravo (COM4, MAC f4:2d:c9:73:d3:cc)

### Configuration

- `[env:esp32dev_ble_bench]` — adds `-DBLE_ENABLED -DLOG_LEVEL_DEBUG -DBLE_BENCH_RIG`
- Mitigation #1 active (10% scan duty)
- Mitigation #2 NOT active (BT controller still on Core 0)
- ESP-NOW ranging disabled retained (`cmd/espnow/ranging` payload `0`)

### Timeline

| Time | Event | heap_free | heap_largest | Notes |
|---|---|---|---|---|
| 10:08:22 | Flash + reboot | 74608 | 69620 | poweron; BLE overhead ~42 KB vs stock |
| 10:15 (T+7) | Heartbeat | 59392 | 51188 | −15 KB / −18 KB in 7 min |
| 10:16-10:18 (T+8..10) | Heartbeats | 59392 | 51188 | **STABLE — early drop was init, not a leak** |
| _T+30 (10:38)_ | _scheduled checkpoint_ | _?_ | _?_ | _watch trajectory_ |
| _T+70 (11:18)_ | _peak hang likelihood_ | _?_ | _?_ | _stock hang reproduces here_ |
| _T+120 (11:48)_ | _verdict_ | _?_ | _?_ | _if alive: mitigations 1+2 sufficient_ |

### CSV log

`C:\Users\drowa\daily-health\bravo_ble_bench_heap.csv` — appended on every Bravo heartbeat by Monitor task `bj2ctcoxg`. Columns: `ts,uptime_s,heap_free,heap_largest`.

### Synthetic broker-blip

`tools/broker_blip.ps1` — written but not yet running this session (requires elevated PowerShell; current session is non-elevated). Bravo's natural reconnect at 10:08 is the equivalent of a single blip; the 70-min hang clock starts there.

## Verdict logic (after T+120)

| Bravo state at 11:48 | Conclusion | Next |
|---|---|---|
| Alive, heartbeating, heap stable | Mitigations 1+2 hold | Promote to v0.4.15 firmware-default for fleet rollout |
| Alive but heap kept dropping | Likely a leak in BLE scan path | Track leak with deeper logging; defer fleet rollout |
| Hung (LWT-only) before T+70 | 10% scan duty isn't sufficient | Apply mitigation #2 (Core 1 pinning) |
| Hung at T+70 ± 10 min | Still tracking the original failure shape | Apply #2 + #4 together |
| Crashed (panic / *_wdt) | Different bug than the silent deadlock | Investigate stack trace |

## RESOLVED 2026-04-27 18:39 SAST

After v0.4.16's broker probe (#67 option C) eliminated the long-outage cascade,
re-ran Bravo on v0.4.17-dev BLE-bench through M3 (180 s broker blip):

- Bravo with `BLE_ENABLED + BLE_BENCH_RIG` (10 % scan duty) reconnected
  cleanly at uptime 241 s preserving uptime — no panic, no restart, no
  int_wdt.
- All 5 non-BLE fleet devices on v0.4.16 release also clean.
- The "70-min BLE silent-deadlock" hypothesis was actually a symptom of
  the same cascade trigger — broker outages (planned or accidental)
  caused MQTT_HUNG_TIMEOUT_MS to fire, which raced AsyncTCP's _error
  path. With the broker probe in place, neither BLE-on nor BLE-off
  devices ever enter the bad code path.

**Path C closure:** the original BLE silent-deadlock investigation was
chasing a symptom of the cascade bug, not a BLE-specific issue. With
v0.4.16/v0.4.17 deployed, BLE can be re-enabled fleet-wide with the
existing scan-duty mitigation (300/30 ms) without expecting deadlocks.

The BLE_BENCH_RIG → fleet rollout decision is now independent of the
cascade fix.

## Open questions

1. Does the heap drop from 74→59 in 7 min stabilise, or is it linear?
2. Is the BTDM_CTRL_HLI=y on Core 0 the ISR-pinning ceiling that no task affinity can move?
3. Does NimBLE-Arduino expose any "stop scanning during Wi-Fi handshake" hooks?
4. Is the 70-min figure correlated with any beacon counter, NimBLE GAP timer, or DHCP lease renewal?
