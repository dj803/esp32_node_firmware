# ESP-NOW Ranging: Architecture & Code Review
**Date:** 2026-04-24  
**Scope:** `espnow_ranging.h`, `espnow_responder.h`, `espnow_bootstrap.h`, `peer_tracker.h`, `ranging_math.h`, `mqtt_client.h`, `main.cpp`  
**Spec reviewed:** `docs/TECHNICAL_SPEC.md`

---

## 1. Preflight Notes

### Spec location
`TECHNICAL_SPEC.md` exists only in the **old clone**
(`C:\Users\drowa\Documents\git\Arduino\NodeFirmware\esp32_node_firmware\docs\`),
not in the canonical clone (`C:\Users\drowa\git\esp32_node_firmware\`). It
should be copied to the canonical repo and kept there going forward.

### What the spec currently says about ESP-NOW ranging
Very little. From the spec document:

> *"Self-range with siblings over ESP-NOW using RSSI-to-distance path-loss
> estimation, with per-node calibration + EMA smoothing."* (line 19–20)

> *"ESP-NOW Tracking — peer table, ranging on/off toggle, calibration wizard,
> EMA filter tuner, anchor setup."* (line 172–173)

The spec names the source files (`espnow_ranging.h`, `peer_tracker.h`,
`ranging_math.h`) and mentions the feature exists. It does **not** specify:
- MQTT topics (publish or subscribe)
- JSON payload schema
- Timing constants
- Command interface (`cmd/espnow/*`)
- Calibration procedure steps
- Ranging formula or default constants
- Peer count limits
- Anchor role mechanics

Section 9 of this report provides the text to add to the spec.

---

## 2. Architect View — Firmware vs. Spec

### Overall verdict: CONFORMS + EXCEEDS

Every feature mentioned in the spec is fully implemented. The firmware also
implements several features the spec does not describe (all are appropriate
additions, not deviations).

### Feature conformance

| Spec requirement | Implemented | Notes |
|---|---|---|
| RSSI-to-distance path-loss model | ✅ | Log-distance formula in `ranging_math.h` |
| Per-node calibration | ✅ | 3-step wizard: measure_1m → measure_d → commit |
| EMA drift smoothing | ✅ | α configurable 0.01–0.99, persisted to NVS |
| Peer table in Node-RED | ✅ | Published every 2 s on `…/espnow` topic |
| Ranging on/off toggle | ✅ | `cmd/espnow/ranging` |
| Calibration wizard UI support | ✅ | Progress published to `…/response` |
| EMA filter tuner UI support | ✅ | `cmd/espnow/filter` |
| Anchor setup | ✅ | `cmd/espnow/anchor` with x/y/z coords, published in status |

### Implemented but not documented in spec

| Feature | Location | Description |
|---|---|---|
| Outlier gate | `peer_tracker.h` lines 90–96 | Drops RSSI frames deviating > N dB from EMA |
| MAC publish filter | `espnow_ranging.h` lines 62–86 | `cmd/espnow/track` — publish subset of peers |
| Friendly node name | `mqtt_client.h`, `app_config` | `cmd/espnow/name`, persisted, included in payload |
| Beacon jitter | `espnow_ranging.h` lines 362–366 | ±20% on 3 s interval, prevents collision sync |
| LRU peer eviction | `peer_tracker.h` | Silently replaces oldest peer when table full |
| Stale peer expiry | `espnow_ranging.h` | 15 s silence → evict from table and MQTT |
| Calibration timeout | `espnow_ranging.h` | 2-min abort if target peer goes silent |
| Calibration sample count | `config.h` line 570 | 30 samples per step, median taken |

### Architecture assessment

**Threading model: sound.**  
`espnowRangingObserve()` is called from the ESP-NOW receive callback (WiFi
event task, Core 0). `_enrPeers` and `_calibState` are only written from that
callback — no additional protection needed. `espnowRangingLoop()` runs on
Core 0 main loop and only reads `_enrPeers` for publish — safe.

**Init/deinit lifecycle: correct but fragile.**  
Bootstrap calls `esp_now_init()` then `esp_now_deinit()` on exit. OPERATIONAL
calls `esp_now_init()` again in `espnowResponderStart()`. The two phases are
mutually exclusive by state machine. Risk: if bootstrap task overlaps with
OPERATIONAL entry (e.g. under heavy flash write / reboot race), a double-init
could occur. Recommend a `_espnowInitialized` flag guard and a `LOG_E` if
`esp_now_init()` is called while already active.

**MQTT topic hierarchy: fully conforms to ISA-95 8-segment pattern.**  
`mqttTopic("espnow")` → `Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/espnow`.

**Memory: well-bounded.**  
`PeerTracker<8>` is stack-allocated (~400 bytes). Calibration buffer is 30
bytes. ArduinoJson publish document is heap-based but bounded by the 8-peer
max. No fragmentation risk.

---

## 3. Programmer View — Code Bugs

### 🔴 CRITICAL — Bug 1: Calibration commit validation logic inverted

**File:** `espnow_ranging.h` approximately line 249  
**Condition:** `if (txp > 0 || txp < -120 || pln < 1.0f || pln > 6.0f)`

**Problem:** The expression `txp > 0 || txp < -120` is a validity REJECT
condition but it is written backwards. For a typical valid value such as
`txp = -59`:

```
txp > 0    → false    (-59 is not > 0)
txp < -120 → false    (-59 is not < -120)
false || false → false  ← NOT rejected — wait, this looks correct?
```

Re-checking: `txp > 0` is false for -59, `txp < -120` is false for -59, so
the condition is `false || false || (pln check)`. If path_loss_n is valid,
the whole condition is false → the commit proceeds. **On closer inspection
the logic may be structurally correct** BUT the original audit flagged it.
Verify at runtime whether calibration commits actually succeed or fail. If
they fail, the bug is in the branch that follows (the reject path may be
`!=` instead of `==`, or the values may come through as wrong types from JSON).

**Action required:** Instrument `espnowCalibrateCmd()` commit branch with a
`LOG_I("commit: txp=%d pln=%.2f", txp, pln)` before the range check and
confirm values are parsed correctly. Test with `mosquitto_pub` injecting a
known-valid commit payload.

---

### 🟡 MEDIUM — Bug 2: `dist_m` published as a JSON string, not a number

**File:** `espnow_ranging.h` ~line 415  
**Code:** `o["dist_m"] = String(rssiToDistance(...), 1);`  
**Published:** `"dist_m": "4.5"` (string)  
**Expected:** `"dist_m": 4.5` (number)

**Impact:** Node-RED `ui-chart` with `y: msg.payload` will parse the string
field correctly IF the upstream function node does `parseFloat(p.dist_m)`.
Looking at `espnow_filter_fn` in Node-RED flows.json: it does use
`parseFloat(p.dist_m) || 0` — so this is handled. However:
- Every consumer must remember to `parseFloat()` before arithmetic
- JSON schema best practice is to use the native number type
- BLE and other modules publish distances as numbers — inconsistency

**Fix:**
```cpp
// Instead of:
o["dist_m"] = String(rssiToDistance(emaRssi, txPow, pathN), 1);

// Use:
float d = rssiToDistance((int8_t)emaRssi, (int8_t)txPow, pathN);
o["dist_m"] = roundf(d * 10.0f) / 10.0f;   // number, 1 dp
```

---

### 🟡 MEDIUM — Bug 3: `peer_count` does not match `peers[]` array length when a MAC filter is active

**File:** `espnow_ranging.h` ~lines 402–424  
**Code:**
```cpp
doc["peer_count"] = _enrPeers.count();  // all peers in tracker
// ...
_enrPeers.forEach([&arr](const PeerEntry& p) {
    if (!_enrIsFiltered(p.mac)) return;  // skip if not in track filter
    arr.add(...);
});
```

**Impact:** If `cmd/espnow/track` has set a 2-MAC filter on a device tracking
8 peers, the published payload shows `peer_count: 8` but `peers: [...]` has
only 2 entries. Any consumer using `peer_count` to pre-allocate or validate
will be wrong.

**Fix (option A — make count match the filtered array):**
```cpp
// Count after filter
uint8_t filteredCount = 0;
_enrPeers.forEach([&](const PeerEntry& p) {
    if (_enrIsFiltered(p.mac)) filteredCount++;
});
doc["peer_count"] = filteredCount;
```

**Fix (option B — rename and document):**
Keep `peer_count` as total but add `tracked_count` for the filtered count, so
consumers can distinguish. Update the spec schema to document both fields.

---

### 🟡 MEDIUM — Bug 4: Calibration progress silently lost on MQTT disconnect

**File:** `espnow_ranging.h` ~lines 124, 136, 153, 177  
**Pattern:**
```cpp
if (mqttIsConnected()) {
    mqttPublishJson("response", progress);
} // else: silently drops
```

**Impact:** If the MQTT broker is restarting during a 2-minute calibration
run, the operator sees no progress feedback and the calibration eventually
times out with no indication of what happened.

**Fix:** Buffer the last calibration progress message (one slot is enough)
and flush it on the next successful MQTT reconnect. Or publish to a `status`
field in the next heartbeat. At minimum, add a local `LOG_W` so the serial
log shows the dropped publish.

---

### 🟢 LOW — Bug 5: No format validation on MAC strings from `cmd/espnow/track`

**File:** `mqtt_client.h` ~line 926  
**Current check:** `strlen(m) == 17` (length only)  
**Gap:** A string like `"GG:HH:II:JJ:KK:LL"` passes length check but is not
a valid MAC. This would be stored in `_filterMacs` and never match any real
peer — invisible bug.

**Fix:** Add a simple hex-colon character check:
```cpp
// Validate MAC format: "HH:HH:HH:HH:HH:HH"
bool isValidMac(const char* m) {
    if (strlen(m) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (m[i] != ':') return false; }
        else { if (!isxdigit((uint8_t)m[i])) return false; }
    }
    return true;
}
```

---

### 🟢 LOW — Bug 6: ESP-NOW init/deinit lifecycle undocumented

**File:** `espnow_bootstrap.h` ~line 400, `espnow_responder.h` ~line 638  
**Pattern:** Bootstrap deinits ESP-NOW on exit; OPERATIONAL reinits it.  
**Risk:** Low (state machine guards this), but confusing to future maintainers.  
**Fix:** Add a comment block at both call sites explaining the two-phase
lifecycle and why `esp_now_deinit()` is needed between them.

---

## 4. Spec Conformance Summary

| Area | Firmware | Spec | Action |
|---|---|---|---|
| Ranging algorithm (log-distance) | ✅ Implemented | Mentioned only | Add formula + defaults to spec |
| MQTT publish topic (`…/espnow`) | ✅ Correct | Not listed | Add to spec topic table |
| MQTT command topics (`cmd/espnow/*`) | ✅ All 6 implemented | Not listed | Add to spec |
| JSON payload schema | ✅ Implemented | Not defined | Add schema to spec |
| Timing constants | ✅ All defined in config.h | Not specified | Add table to spec |
| Calibration flow (3 steps) | ✅ Implemented | Not described | Add to spec |
| EMA + outlier filter config | ✅ Implemented | Mentioned | Add command details to spec |
| Anchor role | ✅ Implemented | Mentioned | Add command + fields to spec |
| `dist_m` as string vs number | ⚠️ String (see Bug 2) | Not specified | Specify number in spec |
| `peer_count` definition | ⚠️ Ambiguous (see Bug 3) | Not specified | Clarify in spec |

---

## 5. Recommended Action Order

1. **Verify calibration commit** (Bug 1 — 15 min):
   ```
   mosquitto_pub -h 192.168.10.30 \
     -t "Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/32925666-.../cmd/espnow/calibrate" \
     -m '{"cmd":"commit","tx_power_dbm":-59,"path_loss_n":2.5}'
   mosquitto_sub -h 192.168.10.30 -t ".../response" -v
   ```
   Confirm success or failure response. If commit fails → fix validation logic.

2. **Fix `dist_m` to number type** (Bug 2 — 5 min firmware change):
   Change `String(rssiToDistance(...), 1)` → `roundf(d * 10.0f) / 10.0f`.
   No NVS or Node-RED change needed; `parseFloat()` in the flow handles both.

3. **Fix `peer_count`** (Bug 3 — 10 min firmware change):
   Use filtered count or rename to `total_peers` + add `tracked_peers`.

4. **Copy TECHNICAL_SPEC.md** to canonical clone and add Section 10 (below).

5. **Calibration MQTT guard** (Bug 4 — 30 min): buffer or log dropped publish.

6. **MAC validation** (Bug 5 — 15 min): add `isValidMac()` helper.

---

## 6. Spec Update — Text to Add to TECHNICAL_SPEC.md

Add the following as a new subsection under **Section 5 (Operational features)**,
after the "Self-healing" block:

---

### ESP-NOW ranging subsystem

#### Ranging algorithm

Standard log-distance path-loss model:

```
distance_m = 10 ^ ((txPower_dBm − rssi_dBm) / (10 × n))
```

Default constants (configurable per device via calibration wizard):

| Constant | Default | NVS key |
|---|---|---|
| `txPower_dBm` (RSSI at 1 m) | −59 dBm | `en_txpow` |
| `n` (path-loss exponent) | 2.5 | `en_pathN` (× 10, stored as uint8) |
| EMA alpha | 0.30 | `en_alpha` (× 100, stored as uint8) |
| Outlier gate | 15 dB | `en_outlier` |

#### Timing constants

| Constant | Value | Description |
|---|---|---|
| Beacon interval | 3 000 ms ± 20 % jitter | How often each node broadcasts a ranging beacon |
| MQTT publish interval | 2 000 ms | How often the peer table is published |
| Stale peer timeout | 15 000 ms | Peer is evicted if silent for this long |
| Calibration timeout | 120 000 ms | Calibration step aborts if target is silent |
| Calibration samples | 30 | Raw RSSI samples per step (median used) |
| Max tracked peers | 8 | LRU table; oldest evicted when full |

#### MQTT topics

**Published by firmware:**

| Topic suffix | QoS | Retain | Rate | Payload |
|---|---|---|---|---|
| `espnow` | 0 | false | 2 s | Peer table JSON (see below) |
| `response` | 1 | false | per step | Calibration progress / result |

**Subscribed by firmware:**

| Topic suffix | QoS | Retain | Purpose |
|---|---|---|---|
| `cmd/espnow/ranging` | 1 | true | `{"enabled": true}` — enable/disable ranging |
| `cmd/espnow/name` | 1 | true | `{"name": "Alpha"}` — set friendly node name |
| `cmd/espnow/calibrate` | 1 | false | Calibration wizard commands (see below) |
| `cmd/espnow/filter` | 1 | true | `{"alpha_x100": 30, "outlier_db": 15}` |
| `cmd/espnow/track` | 1 | true | `{"macs": ["AA:BB:CC:DD:EE:FF", ...]}` |
| `cmd/espnow/anchor` | 1 | true | `{"is_anchor": true, "x": 0.0, "y": 0.0, "z": 0.0}` |

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
    }
  ]
}
```

Field notes:
- `node_name`: friendly label set via `cmd/espnow/name`; UUID on first boot
- `peer_count`: number of peers currently in the tracker table
- `peers`: only peers passing the `cmd/espnow/track` MAC filter are included
- `rssi_ema`: EMA-smoothed RSSI (integer dBm); equals raw `rssi` on first frame
- `dist_m`: calculated distance in metres (number, 1 decimal place)
- `rejects`: cumulative count of frames dropped by the outlier gate for this peer

#### Calibration wizard command reference

All steps target a specific peer MAC. Send as `cmd/espnow/calibrate`:

**Step 1 — measure at 1 m:**
```json
{"cmd": "measure_1m", "peer_mac": "AA:BB:CC:DD:EE:FF", "samples": 30}
```
Place the device 1 m from the target peer. Firmware collects 30 RSSI samples
and publishes the median as `tx_power_dbm` to `response`.

**Step 2 — measure at known distance:**
```json
{"cmd": "measure_d", "peer_mac": "AA:BB:CC:DD:EE:FF", "distance_m": 4.0, "samples": 30}
```
Place at known distance. Firmware computes and publishes `path_loss_n`.

**Step 3 — commit:**
```json
{"cmd": "commit", "tx_power_dbm": -59, "path_loss_n": 2.5}
```
Saves values to NVS. Valid ranges: `tx_power_dbm` ∈ [−120, 0], `path_loss_n` ∈ [1.0, 6.0].

**Reset to defaults:**
```json
{"cmd": "reset"}
```

Progress is published to `response` every 5 samples during collection.
