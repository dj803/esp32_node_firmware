# ESP32 Node Firmware — Technical Specification

> **Status:** Living document. Written 2026-04-23 against v0.4.04. ESP-NOW ranging section updated 2026-04-24. v0.4.10 update 2026-04-25 — added MQTT_HEALTHY LED state, dev-build version suffix, Node-RED fleet stagger + canary OTA. Fleet expanded to 6 nodes (added Foxtrot with RFID). Logged provisioning/UUID/OTA-URL anomalies (#48–#50) for follow-up. v0.4.11 update 2026-04-27 — heap-guard in `mqttPublish()` (#51 root cause: bad_alloc), BLE disabled via `BLE_ENABLED 0` in config.h pending NimBLE/WiFi coexistence audit (#61), heap heartbeat added to status payload (#53), `#48/#49` UUID/boot-reason visibility logs, `tools/` directory, NDEF feature on Foxtrot.
> The firmware is a fleet-management IoT platform built around a
> mesh-capable ESP32 node with on-device sensing, local auto-recovery,
> and a Node-RED dashboard as the operator surface.

## 1. What it is

A fleet firmware for **classic ESP32 (WROOM-32, 4 MB flash)** nodes that:

1. **Self-provision** on first boot via encrypted ESP-NOW credential
   exchange with a sibling node (no per-device factory write of
   WiFi/MQTT credentials required).
2. **Connect to a LAN MQTT broker** via a three-tier discovery chain
   (mDNS → parallel TCP port scan → NVS-cached URL).
3. **Stream sensor telemetry + accept commands** over a versioned MQTT
   topic hierarchy (ISA-95 / Unified Namespace shape).
4. **Self-range** with siblings over ESP-NOW using RSSI-to-distance
   path-loss estimation, with per-node calibration + EMA smoothing.
5. **Update themselves over-the-air** via HTTPS from a GitHub Pages
   manifest, with a multi-stage rollback safety net.
6. **Report to a Node-RED dashboard** (~200 nodes of UI + flow logic)
   that covers device status, RFID, BLE, LED control, ESP-NOW map,
   and pair-distance chart tabs.

## 2. Hardware target

- **MCU:** ESP32 WROOM-32 (Xtensa LX6 dual-core, 240 MHz, 520 KB SRAM,
  4 MB flash).
- **Peripherals currently integrated:**
  - **MFRC522v2** RFID reader over VSPI (GPIO 5 SS, 18 SCK, 19 MISO,
    23 MOSI, 22 RST, 4 IRQ). 13.56 MHz ISO-14443A.
  - **WS2812B** addressable LED strip on GPIO 27 (FastLED, RMT-driven).
  - **NimBLE-Arduino 2.x** BLE scanner (shared radio with WiFi).
- **Reserved / in plan:** 2-channel opto-isolated relay, SS49E linear
  Hall sensor (see `docs/PLAN_RELAY_HALL_v0.5.0.md`).

## 3. Architecture

```
                  ┌──────────────────────────────────────────┐
                  │              Node-RED dashboard          │
                  │  (Device Status · ESP-NOW · RFID · LED · │
                  │   Map · Pair chart · boot_history)       │
                  └──────────────┬───────────────────────────┘
                                 │  MQTT (plaintext LAN)
                  ┌──────────────▼───────────────┐
                  │      Mosquitto broker        │
                  └──────────────┬───────────────┘
                                 │
     ┌───────────────────┬───────┴───────────┬───────────────────┐
     │                   │                   │                   │
┌────▼────┐         ┌────▼────┐         ┌────▼────┐         ┌────▼────┐
│ Alpha   │  ESP-NOW│ Bravo   │  ESP-NOW│ Charlie │  ESP-NOW│  …      │
│ ESP32   ◀─────────▶ ESP32   ◀─────────▶ ESP32   ◀─────────▶         │
│         │ (ranging│         │ (ranging│         │ (ranging│         │
│         │ beacons+│         │ beacons+│         │ beacons+│         │
│         │ bootstrap)        │ bootstrap)        │ bootstrap)        │
└─────────┘         └─────────┘         └─────────┘         └─────────┘
```

Each node runs a non-blocking state machine: `BOOT → BOOTSTRAP_REQUEST →
WIFI_CONNECT → OPERATIONAL ↔ AP_MODE`. Recovery loops are
millis()-deadline driven (no `delay()` in hot paths since v0.3.06).

## 4. Subsystem breakdown

| Subsystem | Header | Role |
|---|---|---|
| **Boot state machine** | `src/main.cpp` | Coordinates WiFi, AP portal, bootstrap, operational loop |
| **Credential provisioning** | `include/espnow_bootstrap.h`, `include/credentials.h` | Async ESP-NOW credential request + encrypted wire-format bundle (versioned `WireBundle`) |
| **WiFi recovery** | `include/wifi_recovery.h`, loop in `main.cpp` | Indefinite exponential backoff (15s → 10min cap) for router outages; auth-fail hysteresis fall-through to AP mode |
| **AP-mode HTTPS portal** | `include/ap_portal.h` | ESP-IDF httpd_ssl, self-signed cert generated on first boot, persisted to NVS; CSRF + XSS hardened; 5-min idle timeout |
| **MQTT client** | `include/mqtt_client.h` | AsyncMqttClient with LWT, deferred-action dispatch (restart/sleep), `FwEvent` enum for status events. Heap-guard in `mqttPublish()`: drops publish if free heap < `MQTT_PUBLISH_HEAP_GUARD_BYTES` (workaround for bad_alloc panic — #51). Long-term fix: replace AsyncMqttClient with static-buffer client (v0.5.0). |
| **Broker discovery** | `include/broker_discovery.h` | mDNS → AsyncTCP parallel port scan → cached URL fallback |
| **ESP-NOW responder** | `include/espnow_responder.h` | Credential server for siblings, OTA URL fallback, sibling-provided broker address, rate-limited (per-MAC token bucket) |
| **ESP-NOW ranging** | `include/espnow_ranging.h`, `include/peer_tracker.h`, `include/ranging_math.h` | Passive RSSI sampling of all received ESP-NOW frames, LRU peer table, calibrated path-loss distance, EMA drift smoothing |
| **RFID** | `include/rfid.h`, `include/rfid_types.h` | MFRC522v2 card detection via IRQ, NVS-persisted whitelist, read/program playground for MIFARE Classic |
| **BLE scanner** | `include/ble.h` | NimBLE periodic scan, per-device tracking of specified MACs, RSSI-to-distance estimate (separate path-loss constant from ESP-NOW). **Currently disabled** (`BLE_ENABLED 0` in config.h) — NimBLE/WiFi/ESP-NOW coexistence audit pending v0.4.13 (#61). |
| **WS2812 LED** | `include/ws2812.h`, `include/led.h` | FreeRTOS task driving the strip with event queue (bounded, drop-on-full); status-LED patterns |
| **OTA** | `include/ota.h`, `include/ota_validation.h` | Manifest poll + URL fallback chain + `esp_https_ota` writer + NVS-flag post-OTA validation with active rollback |
| **Logging + telemetry** | `include/logging.h`, `include/fwevent.h` | Compile-time filtered log levels, typed event strings (grep-friendly), boot-reason + heap-phase structured telemetry |
| **App config / NVS** | `include/app_config.h`, `include/nvs_utils.h`, `include/prefs_quiet.h` | Runtime-configurable per-node settings (topic segments, OTA URL, calibration, anchor coordinates) |

## 5. Operational features

### OTA update chain
- Hourly poll of a GitHub Pages JSON manifest (+ hardcoded fallback URLs).
- `esp_https_ota` flash writer with HTTP Range resume.
- **Three independent rollback layers:**
  1. Per-chunk progress watchdog (30 s silence → `ESP.restart()`).
  2. NVS-flag post-OTA validation (new firmware has 5 min to reach
     MQTT and call `esp_ota_mark_app_valid_cancel_rollback()`; otherwise
     calls `esp_ota_mark_app_invalid_rollback_and_reboot()` which
     boots the previous slot).
  3. Pre-flight heap gate (≥ 80 KB free / ≥ 32 KB largest block before
     flash write starts).
- **Failure telemetry:** every `ota_failed` event is stage-tagged
  (`preflight | manifest | flash`) with machine-readable error code.
- **Boot-reason field** on every retained boot announcement (poweron /
  software / task_wdt / panic / brownout / etc.) surfaces reboots
  on a Node-RED dashboard tile.

### Concurrency model
- Every cross-task / cross-core shared variable guarded by `volatile`
  (single-byte), `portMUX_TYPE` spinlock, or a FreeRTOS mutex as
  appropriate. Pattern documented in `docs/TWDT_POLICY.md`.
- All AsyncMqttClient setters that store pointers (setClientId /
  setCredentials / setServer / setWill) fed from module-static
  `String`s. Convention documented in `docs/STRING_LIFETIME.md`.
- No `delay()` on hot paths — all waits are `millis()`-deadline.

### Self-healing
- WiFi outages: indefinite exponential backoff, never restart for loss
  alone.
- MQTT: ≥ N consecutive failures triggers broker re-discovery; ≥ 2N
  triggers device restart.
- AP mode: 5-min admin-idle timeout (only intended for first-boot
  provisioning or dev work).
- Preferences / NVS: `prefsTryBegin` silently skips missing namespaces
  (no log spam).

### Security posture
- **HTTPS admin portal** (self-signed per-device cert, NVS-persisted).
- **CSRF token ring** (2-slot, portMUX-guarded).
- **XSS-safe HTML** (every attribute value `htmlEscape`d).
- **ESP-NOW credential bundle** AES-GCM encrypted with per-site key.
- **MQTT over plaintext LAN** — acceptable for trusted-wire segments,
  documented as a follow-up (`SUGGESTED_IMPROVEMENTS.md #7`).
- **OTA manifest over HTTPS** but without CA pinning — documented
  trade-off for internal-IoT threat model.

### Observability
- Structured `LOG_D/I/W/E` macros (compile-time level filter).
- Retained `.../status` with `boot_reason`, `firmware_version`,
  `wifi_channel`, `uptime_s`, capability flags, anchor metadata.
- Typed MQTT event strings (`FwEvent` enum → `fwEventName()`): `boot`,
  `heartbeat`, `ota_checking | downloading | preflight | failed |
  success | validating | validated | rolled_back`, `sleeping | locating |
  cred_rotated | ...`.
- Heap phase telemetry at boot (`after-serial`, `after-wifi`,
  `after-mqtt`, `after-ble`).
- Node-RED `boot_history` tile — colour-coded table of recent abnormal
  reboots across the fleet.

### Host-side testing
- Pure utility headers (`ranging_math.h`, `peer_tracker.h`, `semver.h`,
  `rate_limit.h`, `wifi_recovery.h`, `wire_bundle.h`, `mac_utils.h`)
  isolated from Arduino/ESP-IDF dependencies and covered by a Unity
  test suite on a `pio test -e native` host environment (~54 tests).

### ESP-NOW ranging subsystem

This section documents the full ranging implementation. Source files:
`include/espnow_ranging.h`, `include/peer_tracker.h`,
`include/ranging_math.h`.

#### How ranging works

Every node continuously broadcasts a 9-byte **ranging beacon** over ESP-NOW.
Every node that receives any ESP-NOW frame — beacon or otherwise — records
the raw RSSI of that frame against the sender's MAC. This passive
observation approach means ranging is free-riding on all ESP-NOW traffic
(beacons + bootstrap + credential responses), not just explicit ranging
packets.

Each observed RSSI sample is passed through two filters before a distance
is computed:

1. **Outlier gate** — if the sample deviates more than `outlier_db` dB
   from the current EMA, it is discarded (`rejects` counter increments).
2. **EMA smoothing** — exponential moving average with factor α.
   On the first frame from a peer the raw RSSI is used as-is.

Distance is then estimated from the smoothed RSSI using the standard
log-distance path-loss model:

```
distance_m = 10 ^ ( (txPower_dBm − rssi_ema_dBm) / (10 × n) )
```

#### Default calibration constants

Stored per device in NVS. Configurable without reflash via the
calibration wizard (`cmd/espnow/calibrate`).

| Parameter | Default | NVS key | Range |
|---|---|---|---|
| `txPower_dBm` — RSSI at 1 m | −59 dBm | `en_txpow` | −120 to 0 |
| `n` — path-loss exponent | 2.5 | `en_pathN` (×10, uint8) | 1.0 to 6.0 |
| EMA alpha | 0.30 | `en_alpha` (×100, uint8) | 0.01 to 0.99 |
| Outlier gate threshold | 15 dB | `en_outlier` | 0 to 30 |

Guidance on path-loss exponent `n`:
- Free space / open plan: 2.0
- Typical office (light obstruction): 2.5 *(default)*
- Dense office / light walls: 3.0
- Heavy masonry / warehouse racking: 3.5–4.0

#### Timing constants

| Constant | Value | Description |
|---|---|---|
| Beacon interval | 3 000 ms ± 20 % jitter | Random jitter prevents beacon collision synchronisation |
| MQTT publish interval | 2 000 ms | How often the peer table is sent to Node-RED |
| Stale peer timeout | 15 000 ms | Peer evicted if no frame received for this long |
| Max tracked peers | 8 | LRU eviction — oldest peer replaced when full |
| Calibration samples per step | 30 | Median of 30 raw RSSI samples used for each cal step |
| Calibration step timeout | 120 000 ms | Cal step aborted if target peer goes silent |

#### MQTT interface

**Published by every node:**

| Topic suffix | QoS | Retain | Cadence | Content |
|---|---|---|---|---|
| `espnow` | 0 | no | 2 s | Peer table JSON (schema below) |
| `response` | 1 | no | per step | Calibration progress + results |

**Subscribed by every node:**

| Topic suffix | QoS | Retained by broker | Payload |
|---|---|---|---|
| `cmd/espnow/ranging` | 1 | yes | `{"enabled": true}` — enable/disable ranging globally |
| `cmd/espnow/name` | 1 | yes | `{"name": "Alpha"}` — set friendly label for this node |
| `cmd/espnow/calibrate` | 1 | no | Calibration wizard commands (see below) |
| `cmd/espnow/filter` | 1 | yes | `{"alpha_x100": 30, "outlier_db": 15}` — EMA + outlier settings |
| `cmd/espnow/track` | 1 | yes | `{"macs": ["AA:BB:CC:DD:EE:FF", …]}` — restrict MQTT publish to listed MACs |
| `cmd/espnow/anchor` | 1 | yes | `{"is_anchor": true, "x": 0.0, "y": 0.0, "z": 0.0}` — assign anchor role |

#### `espnow` publish payload schema

```json
{
  "node_name":  "ESP32-Alpha",
  "peer_count": 2,
  "peers": [
    {
      "mac":      "F4:2D:C9:73:D3:CC",
      "rssi":     -52,
      "rssi_ema": -52,
      "dist_m":   0.5,
      "rejects":  0
    },
    {
      "mac":      "D4:E9:F4:60:1C:C4",
      "rssi":     -48,
      "rssi_ema": -47,
      "dist_m":   0.4,
      "rejects":  2
    }
  ]
}
```

Field reference:

| Field | Type | Description |
|---|---|---|
| `node_name` | string | Friendly label set via `cmd/espnow/name`; UUID if not yet named |
| `peer_count` | integer | Total number of peers currently in the tracker table |
| `peers` | array | Only peers passing the `cmd/espnow/track` MAC filter are included |
| `peers[].mac` | string | Peer MAC address, colon-separated uppercase hex |
| `peers[].rssi` | integer | Raw RSSI of the most recent frame from this peer (dBm) |
| `peers[].rssi_ema` | integer | EMA-smoothed RSSI (dBm); equals raw `rssi` on the first frame |
| `peers[].dist_m` | number | Estimated distance in metres, rounded to 1 decimal place |
| `peers[].rejects` | integer | Cumulative count of frames dropped by the outlier gate for this peer |

> **Note:** `peer_count` reflects the total tracker table size. If a MAC
> filter is active via `cmd/espnow/track`, the `peers` array may be
> shorter than `peer_count`. A future firmware revision will add a
> separate `tracked_count` field to resolve this ambiguity.

#### Calibration wizard command reference

All calibration commands are sent to `cmd/espnow/calibrate` (non-retained,
QoS 1). Progress and results are published to `response`.

**Step 1 — Measure RSSI at 1 m (`measure_1m`)**

Place the node exactly 1 m from the target peer. Send:
```json
{"cmd": "measure_1m", "peer_mac": "F4:2D:C9:73:D3:CC", "samples": 30}
```
The firmware collects 30 raw RSSI samples, takes the median, and publishes
the result as `tx_power_dbm` to `response`. This value is the reference
signal strength at 1 m for this physical installation.

**Step 2 — Measure at known distance (`measure_d`)**

Move the node to a known distance (e.g. 4 m). Send:
```json
{"cmd": "measure_d", "peer_mac": "F4:2D:C9:73:D3:CC", "distance_m": 4.0, "samples": 30}
```
The firmware collects another 30 samples, then computes the path-loss
exponent `n` using the formula:

```
n = (tx_power_dbm − rssi_median) / (10 × log10(distance_m))
```

The computed `path_loss_n` is published to `response` for operator review.

**Step 3 — Commit**

If both values look correct, save them to NVS:
```json
{"cmd": "commit", "tx_power_dbm": -59, "path_loss_n": 2.5}
```
Valid ranges: `tx_power_dbm` ∈ [−120, 0]; `path_loss_n` ∈ [1.0, 6.0].
Values are applied immediately in-memory and persisted across reboots.

**Reset to compile-time defaults:**
```json
{"cmd": "reset"}
```

Progress is published to `response` every 5 samples during collection.
If MQTT disconnects during a calibration run, progress is silently dropped
(a known limitation; the calibration continues on-device).

#### Anchor role

A node designated as an **anchor** has known physical coordinates and acts
as a fixed reference point for Node-RED's Gauss-Newton multilateration.

Set via:
```json
{"cmd": "anchor", "is_anchor": true, "x": 0.0, "y": 3.5, "z": 0.0}
```
Coordinates (metres) are persisted to NVS and included in every `status`
heartbeat so Node-RED can build the anchor map without a separate
configuration step. A node can be un-anchored by sending `"is_anchor": false`.

**Minimum for 2-D positioning:** 3 anchors at non-collinear positions.  
**Recommended for reliable 2-D:** 4 anchors forming a convex polygon
around the tracking area.

#### MAC tracking filter (`cmd/espnow/track`)

By default, every peer in the tracker table is included in the `espnow`
MQTT publish. The tracking filter limits the `peers` array to a named
subset:

```json
{"macs": ["84:1F:E8:1A:CC:98", "F4:2D:C9:73:D3:CC"]}
```

Observations are still collected for **all** peers regardless of the
filter — switching the filter takes effect on the next 2-second publish
without any data loss. Maximum 8 MACs in the filter list (same as the
tracker table limit).

Send an empty list to disable filtering (publish all):
```json
{"macs": []}
```

> **Important (Node-RED rebuild note):** `cmd/espnow/track` is a retained
> broker message. Node-RED's flow context (`flow.get('espnow_tracked_macs')`)
> is a separate in-memory mirror of the firmware's filter that must be
> re-hydrated after every Node-RED deploy. The `espnow_set_tracked_fn`
> function node writes the mirror when the operator clicks "Track Selected"
> in the Peer Table. On redeploy the mirror is lost and must be re-set by
> the operator. A future Node-RED fix will auto-reload this from the
> retained MQTT message on startup. See `docs/NODERED_ESPNOW_TAB_REBUILD_NOTES.md`.

## 6. Build + release

- **PlatformIO** + `pioarduino` platform (arduino-esp32 3.1.1).
- **Library deps SHA-pinned** in `platformio.ini`; drift caught at
  compile time by `include/lib_api_assert.h` static_asserts on
  AsyncMqttClient / NimBLE / MFRC522 / ESP32-OTA-Pull signatures.
- **GitHub Actions CI** (`.github/workflows/build.yml`): builds + runs
  host unit tests on every push; publishes signed firmware to GitHub
  Releases + GitHub Pages `ota.json` on every `vX.Y.Z` tag.
- **Reproducible-ish:** same CI environment, same toolchain,
  `FIRMWARE_VERSION` injected from the tag name — any local build
  with an unset env variable compiles a `-dev` suffix to distinguish.

## 7. Operator surface (Node-RED)

Dashboard 2.0 tabs (~200 nodes total, single `flows.json`):

- **Device Status** — per-device card grid with state LED, firmware,
  last-seen, locate button. Abnormal-reboot history tile beneath.
- **ESP-NOW Tracking** — peer table showing all discovered peers with
  live RSSI, EMA-RSSI, estimated distance, and reject count. Controls:
  ranging on/off toggle, per-peer checkbox selection ("Track Selected"),
  Reset Graph, Refresh Peers. Calibration wizard UI (step-through),
  EMA alpha + outlier-dB tuner, per-node name/rename form, anchor
  coordinate setup. Distance chart (time-series, one line per tracked
  peer, Kalman-filtered). Node-RED flow context holds the selected MACs;
  operator must re-click "Track Selected" after a Node-RED redeploy
  (retained MQTT auto-reload is a planned fix — see
  `docs/NODERED_ESPNOW_TAB_REBUILD_NOTES.md`).
- **Pair Chart** — select any two nodes; shows bidirectional joined
  distance (mean of A→B and B→A), ± margin error bars from the
  asymmetry between the two measurements.
- **Map** — 2-D anchor layout with live mobile-node positions
  (Gauss-Newton multilateration in a function node).
- **RFID** — read/program playground, scan feed, whitelist manager.
- **LED Control** — per-device strip color, brightness, count,
  animations.
- **Sleep Control** — light / deep / modem-sleep scheduling per device.

## 8. Limits + known trade-offs

- **4 MB flash** limits OTA slot to ~1.75 MB; recovery-partition
  pattern deferred until 8 MB hardware or major app slim
  (`SUGGESTED_IMPROVEMENTS #26`).
- **Bootloader rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`)
  not enabled — blocked on a pioarduino/custom_sdkconfig framework
  rebuild bug (`SUGGESTED_IMPROVEMENTS #25`). Runtime NVS-flag
  rollback covers ~95 % of the gap.
- **ESP-NOW channel pinning** — nodes on APs on different WiFi channels
  cannot reach each other via ESP-NOW. Node-RED warns on channel
  mismatch.
- **ESP-NOW `peer_count` / `peers[]` mismatch** — when a MAC filter
  (`cmd/espnow/track`) is active, `peer_count` reflects the full
  tracker table size but `peers[]` only contains the filtered subset.
  Consumers should use `peers[].length` rather than `peer_count` when
  iterating. A `tracked_count` field is planned for a future firmware
  revision.
- **ESP-NOW `dist_m` published as string** — current firmware serialises
  the distance value as a JSON string (`"0.5"`) rather than a JSON
  number (`0.5`). Node-RED flows handle this via `parseFloat()` but
  third-party consumers must account for the type. A firmware fix is
  pending.
- **Node-RED flow context not persistent** — ESP-NOW tracked-MAC
  selection and chart state stored in Node-RED `flow.context`
  (in-memory) is lost on every Node-RED redeploy. Operator must
  re-select peers and click "Track Selected" after each deploy.
  File-backed context storage (`contextStorage: localfilesystem` in
  settings.js) would fix this permanently.
- **MQTT plaintext LAN** — documented acceptance for trusted wire
  segments. TLS path is a known follow-up.

## 9. Future possibilities

Short-list of features the current architecture makes cheap to add.
Full backlog is in `docs/SUGGESTED_IMPROVEMENTS.md`; these are the
ones with the best effort-to-value ratio.

### A. Hardware expansion

1. **2-channel relay + Hall sensor** — plan already written
   (`docs/PLAN_RELAY_HALL_v0.5.0.md`). Adds actuator output and
   contactless magnetic sensing with existing module pattern. ~1 day.
2. **Environmental cluster** — BME280 temp/humidity/pressure and
   CCS811 / SGP30 air quality over the existing I2C bus (GPIOs are
   free). Mirrors the BLE-beacon periodic telemetry pattern; ~half day.
3. **I²S microphone** — sound-level meter (SPL) + simple anomaly
   detection (FFT peak) for machine-health monitoring. Reuses the
   FreeRTOS-task / bounded-queue pattern from WS2812.
4. **LF RFID** — add RDM6300 (125 kHz EM4100 tags) behind a
   `LF_RFID_ENABLED` flag, parallel to the existing MFRC522 module.
5. **NDEF read / write for NTAG21x** — makes tags smartphone-readable
   (URL / text / contact); incremental on top of the MIFARE Classic
   support already shipping.

### B. Algorithms + ML

6. **Gauss-Newton position filter with Kalman smoothing** — the map
   flow already multilaterates; add motion smoothing in Node-RED
   (no firmware change).
7. **Tag authorisation via Node-RED** — move RFID whitelist from NVS
   to a Node-RED flow that looks up a central database (allows
   shift-based access control without reflashing).
8. **BLE-beacon room-level localisation** — fixed BLE beacons in each
   room, every node reports which it hears strongest → Node-RED
   decides "which room is device X in". Uses existing BLE tracking.
9. **Anomaly detection on heap-phase logs** — Node-RED watches the
   `after-ble` free-heap trend per device; alerts on drift > threshold
   (fragmentation / leak canary).
10. **Fingerprint-based indoor positioning** — record RSSI snapshots
    at known locations, match at runtime. Drop-in replacement for
    the current path-loss distance model.

### C. Fleet operations

11. **Canary OTA rollout (Node-RED)** — instead of manual OTA of
    Alpha first, dashboard button for "canary update" — one device
    goes, soaks 1 h, then the rest. Uses existing OTA + boot-history
    telemetry.
12. **Versioned MQTT topic prefixes** — `v1/Enigma/...` so schema
    drift between firmware versions doesn't silently break Node-RED
    consumers. Node-RED bridge for backwards-compat.
13. **Core-dump partition + on-boot upload** — partition table change
    + small flash hook that publishes crash summary over MQTT on next
    boot. Closes the "invisible crash in the field" observability gap.
14. **Fleet-wide config rollout** — one Node-RED form push sets
    anchor coordinates / ranging calibration / heartbeat interval on
    every device via retained MQTT commands (machinery for this is
    already in place, just needs UI).

### D. Operator UX

15. **Mobile-responsive dashboard theme** — the existing
    dashboard-2.0 tabs already render on phones; pass over with
    per-tab width tweaks and the day/night toggle (already shipping)
    makes it genuinely field-usable.
16. **QR-code commissioning** — AP-portal page prints a QR of the
    self-signed cert's fingerprint so admins can accept the cert on
    a phone without manual comparison.
17. **"What's this device?" LED locator** — one-click in the
    dashboard, device flashes a distinctive pattern for 10 s; already
    exists as `cmd/locate` — just needs UX polish.

### E. Security hardening

18. **MQTT over TLS to the broker** — AsyncMqttClient supports it;
    requires broker config + CA pinning.
19. **OTA CA pinning** — embed the GitHub root CA in firmware,
    pass to HTTPClient before OTA — closes the remaining MitM window
    on the HTTPS manifest fetch.
20. **ESP-NOW rate-limit authenticated** — currently per-MAC token
    bucket. Could add a site-shared HMAC to reject spoofed MAC
    floods.

### F. Deferred architecturals (tracked as still-open)

- **Recovery partition** — `SUGGESTED_IMPROVEMENTS #26`. Needs 8 MB
  flash or main-app slim.
- **Bootloader rollback** — `SUGGESTED_IMPROVEMENTS #25`. Blocked on
  pioarduino bug.
- **AsyncTCP fork swap** — `SUGGESTED_IMPROVEMENTS #30`. Low urgency.

### G. Data persistence + offline resilience

21. **MQTT offline queue** — buffer outgoing telemetry to a flash
    ring during MQTT outages, replay on reconnect. Saves the data
    points the fleet currently drops during a router reboot. ~100
    KB ring in a new NVS partition, LRU eviction.
22. **Local SD card rotation log** — optional micro-SD daughterboard
    for high-cadence logging (sub-second telemetry, RFID event
    archive). Writes a CSV file per day; Node-RED pulls over HTTP on
    demand. Decouples "nice-to-have detail" from the MQTT upload
    path.
23. **Time-series compression before publish** — delta-of-delta +
    varint on numeric streams (Hall readings, RSSI, heap). ~5×
    reduction on telemetry bandwidth for sites with big fleets on
    metered links.
24. **NTP-backed monotonic timestamps** — every telemetry frame
    includes a `ts_ms` so Node-RED doesn't have to rely on broker
    receive time. Essential for any audit / compliance use (see
    conservative business cases).

### H. Power management

25. **Battery operation profile** — deep-sleep between wake cycles,
    beacon-only mode, configurable wake schedule via MQTT. Mechanics
    partially exist (`cmd/sleep` / `cmd/deep_sleep` shipped in
    v0.3.20); production-battery story needs a brown-out watchdog
    tune + an LED-off during-sleep + battery-voltage telemetry.
26. **PoE support** — daughterboard (IEEE 802.3af/at) for wired
    ceiling installs. No firmware change; hardware design + a
    power-source tag in the boot announcement so the dashboard can
    distinguish USB / PoE / battery-powered devices.
27. **Solar-harvested remote nodes** — LiFePO4 + 6 V panel + a TP4056
    charge controller + the existing sleep modes. Field-tested
    reference design for agricultural / ranch deployments.

### I. Protocol + ecosystem interop

28. **Home Assistant MQTT discovery** — publish a
    `homeassistant/sensor/<dev>/<entity>/config` payload at boot so
    every ESP32 node auto-registers in the customer's existing Home
    Assistant. ~1 day of Node-RED function-node work; unlocks the
    smart-home hobbyist market.
29. **Matter / Thread endpoint** — longer-term; needs an ESP32-H2 or
    C6 for native 802.15.4 radio. Would slot cleanly behind a
    `MATTER_ENABLED` compile flag once the hardware is in the
    roadmap.
30. **Modbus TCP server** — expose telemetry over Modbus so PLCs /
    SCADA systems can poll the ESP32 natively. Bridges our telemetry
    into existing OT infrastructure; ~2-3 days of library
    integration.
31. **REST API gateway mode** — a node configured as "gateway" serves
    a `/devices` JSON API over HTTP for apps that don't speak MQTT.
    Reuses the existing httpd_ssl from AP portal.
32. **Prometheus metrics endpoint** — `/metrics` endpoint scraped by
    Prometheus; alternative to MQTT for DevOps-heavy sites that
    already run Grafana.

### J. Developer experience + extensibility

33. **Runtime sensor-plugin system** — instead of compile-time
    `XXX_ENABLED` flags, a plugin descriptor NVS-stored that
    enumerates which peripherals to init. Cuts the "one firmware
    per hardware variant" problem; firmware stays generic, each
    device picks what it has at commissioning time.
34. **"Sensor module scaffold" generator** — a Python script that
    takes a module name + type (digital-out / analog-in / I2C) and
    generates a skeleton `include/<name>.h` matching the existing
    peripheral pattern (init, loop, NVS, MQTT cmd, LED feedback,
    TWDT comments). Lowers the barrier for contributors.
35. **Hot-reload Node-RED flows from firmware-side** — the device
    hosts a `flows.json` snippet announcing its capabilities; Node-RED
    auto-injects the matching UI tiles. Turns commissioning into
    "plug in device, tiles appear".
36. **Remote REPL over MQTT** — a `cmd/repl` topic that accepts a
    single-shot Lua / MicroPython expression and publishes the
    result. Controlled (non-privileged sandbox); huge for field
    debugging without a serial cable.

### K. Audio + voice

37. **I²S MEMS microphone** — sound-pressure-level telemetry for
    factory noise monitoring; FFT peak detection for machine-health
    canary (bearing tone, pump cavitation). Reuses the bounded-queue
    FreeRTOS-task pattern from WS2812.
38. **Wake-word / event-sound detection** — TinyML on the ESP32
    (esp-tflite-micro) listens for a trained pattern (glass break,
    fire alarm, specific word). Publishes an event on detection; no
    raw audio leaves the device (privacy-friendly).
39. **DAC buzzer / tone output** — simple speaker driver on DAC pin
    for local alerts ("tap card again", "zone entered"). Trivial;
    complements the LED-feedback story.

### L. Vision-adjacent (needs camera variant)

40. **ESP32-CAM SKU** — swap WROOM-32 for ESP32-CAM module (OV2640).
    Opens QR-code scanning, visitor counting (passive), licence-plate
    OCR with a cloud round-trip. Different module, same firmware
    architecture.
41. **Colour sensor (TCS34725) I²C** — cheaper than a camera,
    enough for "is this bottle red, green, or empty?" kinds of
    detection. Industrial conveyor sorting.

### M. Field diagnostics + observability (extends v0.4.03 story)

42. **`cmd/shell`-style field diagnostics** — one-shot diagnostic
    commands (`heap_stats`, `wifi_rssi`, `mqtt_roundtrip_test`,
    `pin_state <n>`) published as JSON replies. Turns every field
    issue into "ask the device" instead of "dispatch a tech with a
    laptop".
43. **Coredump-on-MQTT** — partition-table change + a boot-time
    hook: if an `esp_core_dump` partition holds a crash, publish its
    summary (PC, task, free heap, backtrace) on boot, then erase.
    Closes the biggest remaining observability gap (invisible
    field crashes).
44. **Live-plot heap history** — keep a 1 Hz circular buffer of free
    heap in RAM; on an MQTT request, dump the last 5 min. Useful
    when a device is drifting and you want to see the fragmentation
    curve without serial access.
45. **Fleet-wide A/B experiment switch** — retained
    `cmd/experiment {name, variant}` per device; Node-RED decides
    which half the fleet runs a new feature (e.g. a new scan
    interval) for a soak. Built on the existing `cmd/*` pattern.

## Glossary

- **Anchor** — a node with known fixed x/y/z coordinates, used as a
  reference for multilateration. Minimum 3 non-collinear anchors for
  2-D position estimation; 4 forming a convex polygon recommended.
- **Mobile** — any non-anchor node; position estimated by Node-RED
  from its distances to ≥ 3 anchors using Gauss-Newton multilateration.
- **Bootstrap** — the ESP-NOW credential exchange that happens on
  first boot of an unprovisioned device.
- **Rolling release** — the current deployment pattern: tag `vX.Y.Z`
  → CI builds → GitHub Pages serves `ota.json` → fleet auto-updates
  within one check interval.
- **Phase 2 validation gate** — the NVS-flag rollback mechanism that
  fires if a freshly-OTA'd firmware cannot reach MQTT within 5 min.
- **Ranging beacon** — a 9-byte ESP-NOW broadcast frame sent every
  ~3 s by each node. All nodes passively observe the RSSI of every
  received frame (not just beacons) to build their peer distance table.
- **EMA (Exponential Moving Average)** — the RSSI smoothing filter
  applied per peer. `rssi_ema = α × rssi_new + (1−α) × rssi_ema_prev`.
  Default α = 0.30 (biased toward history; slower but stable).
- **Outlier gate** — pre-filter applied before EMA. A frame whose RSSI
  deviates more than `outlier_db` from the current EMA is discarded
  and counted in `rejects`. Prevents momentary interference spikes
  from corrupting the distance estimate.
- **Path-loss exponent (n)** — environment-specific constant in the
  log-distance model. Lower = less attenuation (open space); higher =
  more obstruction (walls, racking). Calibrated per deployment.
- **txPower_dBm** — the expected RSSI when two nodes are exactly 1 m
  apart. Depends on antenna orientation and physical installation.
  Measured during the calibration wizard step 1.
