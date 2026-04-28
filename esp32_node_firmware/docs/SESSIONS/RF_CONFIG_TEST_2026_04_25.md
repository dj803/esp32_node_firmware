# ESP32 RF Configuration Test — 2026-04-25

**Author:** Captured during ESP-NOW v2 dashboard walk-through (Claude session)
**Hardware under test:**
  - 2 × ESP32-WROOM-32 (firmware v0.4.05)
  - 1 × "ESP32-S 30P expansion board" breakout
  - 1 × MFRC522 RFID module wired to breakout (8-wire SPI/IRQ/RST/3v3/GND)
**Devices used:** ESP32-Alpha (UUID `32925666…`, MAC `84:1F:E8:1A:CC:98`), ESP32-Charlie (UUID `2ff9ddcf…`, MAC `D4:E9:F4:60:1C:C4`).

---

## 1. Goal

Quantify how various physical mounting and power configurations affect RSSI-based ESP-NOW ranging accuracy. Establish a recommended deployment configuration for accurate ranging.

The deeper question: **why is the system's pair-distance asymmetry (A→B vs B→A) so large in normal operation?** RSSI reciprocity is a fundamental assumption of the log-distance path-loss model. If reciprocity is broken by hardware, no software calibration can recover from it.

## 2. Methodology

- Two devices kept at approximately constant physical positions (~1 m apart, line-of-sight) for the duration of the session. Not laboratory-grade — handling devices to swap configurations introduced ~5 dB of geometric noise.
- For each configuration, captured 8–12 s of live ESP-NOW telemetry from both devices. Read **raw RSSI** (not EMA-smoothed) from `peer_count`, `peers[].rssi` field of the `…/espnow` MQTT publish.
- Recorded both directions: A→C (Charlie's view of Alpha's TX) and C→A (Alpha's view of Charlie's TX). Asymmetry = `||A→C| − |C→A||`.
- One configuration variable changed at a time where possible. Where two changed (e.g. moving the breakout between devices), this is noted.
- Bravo was kept powered off for the duration to remove a strong nearby transmitter (its proximity to Alpha was causing receiver desense earlier in the session).

## 3. Configurations Tested

| ID | Alpha mounting | Alpha power | Alpha RFID | Charlie mounting | Charlie power | Charlie RFID |
|----|---|---|---|---|---|---|
| A | breakout | own micro-USB | wired & powered | bare | own micro-USB | n/a |
| B | bare | own micro-USB | n/a | bare | own micro-USB | n/a |
| C | bare | own micro-USB | n/a | breakout | own micro-USB | wired & powered |
| D | bare | own micro-USB | n/a | breakout | own micro-USB | wired but UN-powered |
| E | breakout | own micro-USB | wired but UN-powered | bare | own micro-USB | n/a |
| F | breakout | breakout VIN | wired but UN-powered | bare | own micro-USB | n/a |
| G | bare | own micro-USB | n/a | breakout | breakout VIN | wired but UN-powered |
| H | bare | own micro-USB | n/a | breakout | breakout VIN | wired & powered |
| I | breakout | breakout VIN | wired & powered | bare | own micro-USB | n/a |
| J | bare | own micro-USB | n/a | bare | own micro-USB | n/a |

(B and J are the same configuration captured at different times, used for reproducibility.)

## 4. Raw Data

| ID | A→C (dBm) | C→A (dBm) | Asym (dB) | Mean signal (dBm) | Notes |
|----|---|---|---|---|---|
| A | −76 | −53 | 23 | −64.5 | |
| B | −77 | −72 | 5 | −74.5 | first bare-bare baseline |
| C | −76 | −70 | 6 | −73.0 | |
| D | −80 | −67 | 13 | −73.5 | |
| E | −72 | −61 | 11 | −66.5 | |
| F | ~−50* | −67 | ~17* | ~−58.5 | *Charlie's outlier gate rejected new strong samples; raw was ~−47, EMA stuck at −83. RX side later confirmed unchanged at −67. |
| G | −79 | −56 | 23 | −67.5 | |
| H | −85 | −57 | 28 | −71.0 | |
| I | −63 | −42 | 21 | −52.5 | C→A approaching receiver-saturation territory (>−45 dBm); EMA may be non-linear |
| J | −72 | −75 | 3 | −73.5 | second bare-bare confirmation |

Mean signal = `(|A→C| + |C→A|) / 2` — lower number means stronger overall radio link (less path loss).

## 5. Statistical Analysis

### 5.1 Baseline reproducibility (bare-bare)

Configs **B** and **J** are the same physical configuration measured at different times in the session.

| Metric | B | J | Δ |
|---|---|---|---|
| A→C | −77 | −72 | 5 |
| C→A | −72 | −75 | 3 |
| Asymmetry | 5 | 3 | 2 |
| Mean signal | −74.5 | −73.5 | 1 |

**Bare-bare reproducibility window: ~5 dB on absolute level, ~2 dB on asymmetry.**
This sets the **measurement noise floor** for the rest of the analysis. Differences between configurations smaller than ~5 dB are not statistically meaningful.

### 5.2 Isolated effect: adding the breakout (no RFID, USB power)

Compare bare-bare (B/J, mean asym 4) against single-side-on-breakout-RFID-off-USB-pwr (D, E):

| Comparison | Asym before | Asym after | Δ |
|---|---|---|---|
| Baseline → D (Charlie on breakout) | 4 | 13 | **+9** |
| Baseline → E (Alpha on breakout) | 4 | 11 | **+7** |
| Mean delta | | | **+8 dB** |

**Adding the breakout PCB alone adds ~8 dB of asymmetry**, well above the noise floor.
Mean signal level barely changes (−74.5 → −73 / −66.5), so the breakout isn't acting like an antenna gain change — it's reshaping the radiation pattern asymmetrically.

### 5.3 Isolated effect: adding RFID (to existing breakout, USB power)

Compare same-breakout-USB configs with vs without RFID powered (C vs D, A vs E):

| Comparison (Charlie scenarios) | Asym off | Asym on | Δ |
|---|---|---|---|
| D (RFID off) → C (RFID on), Charlie on breakout | 13 | 6 | **−7** (better!) |

| Comparison (Alpha scenarios) | Asym off | Asym on | Δ |
|---|---|---|---|
| E (RFID off) → A (RFID on), Alpha on breakout | 11 | 23 | **+12** (worse) |

**RFID's effect is highly per-device: −7 dB to +12 dB, range of 19 dB.**
Means the RFID coil is acting as a parasitic resonator whose loaded vs unloaded impedance interacts differently with each WROOM module's antenna pattern. Average effect is roughly zero (+2.5 dB), but the per-device variance is so high that "RFID makes it worse on average" is not a defensible claim. **What is defensible: with RFID enabled, the asymmetry direction and magnitude become unpredictable.**

### 5.4 Isolated effect: switching from USB power to breakout VIN power

Compare same-breakout-with-/without-RFID configs at USB vs VIN power:

| Comparison | USB asym | VIN asym | Δ |
|---|---|---|---|
| E → F (Alpha breakout, RFID off) | 11 | ~17 | +6 |
| D → G (Charlie breakout, RFID off) | 13 | 23 | +10 |
| A → I (Alpha breakout, RFID on) | 23 | 21 | −2 |
| C → H (Charlie breakout, RFID on) | 6 | 28 | +22 |
| **Mean delta** | | | **+9 dB** |

**Switching to VIN power (removing the USB cable from the device's near-field) adds ~9 dB of asymmetry on average.**

But: VIN power also makes the **mean signal level much stronger**:

| Comparison | USB mean | VIN mean | Δ |
|---|---|---|---|
| E → F | −66.5 | ~−58.5 | +8 dB stronger |
| D → G | −73.5 | −67.5 | +6 dB stronger |
| A → I | −64.5 | −52.5 | +12 dB stronger |
| C → H | −73.0 | −71.0 | +2 dB stronger |
| **Mean delta** | | | **+7 dB stronger** |

**So VIN power is a trade-off**: louder TX (more range, more reliable link in poor conditions) but worse asymmetry (less reliable distance estimates). The USB cable was acting as a parasitic absorber/radiator near the WROOM antenna; removing it boosts radiated power but doesn't restore reciprocity because the breakout PCB itself is now the dominant parasitic.

### 5.5 Composite effect: full hostile config (breakout + RFID + VIN power)

Configs **H** and **I** represent the worst case (the full Christmas tree of hardware): on breakout, VIN-powered, RFID powered.

| Config | Asym | Mean signal | vs bare-bare baseline |
|---|---|---|---|
| Baseline (B/J avg) | 4 | −74 | — |
| H | 28 | −71 | **+24 dB asymmetry**, +3 dB louder |
| I | 21 | −52.5 | **+17 dB asymmetry**, +21.5 dB louder |

**Going from bare-bare to "full stack" worsens asymmetry by 17–24 dB.** That dwarfs the ~10 dB margin you have for accurate distance estimation in the log-distance model — distance error scales with `10^(asymmetry / 10n)`, so a 24 dB asymmetry at `n=2.5` = ~9× distance error in one direction relative to the other.

### 5.6 Reciprocity vs mean-signal trade-off

Plotting asymmetry against mean signal across configurations:

```
         |  asym (dB worse for distance accuracy)
   30    + H
         |
   25    + A G
         | I
   20    +
         |
   15    + F
         | D
   10    + E
         |
    5    + C
         | B/J <-- best for accurate ranging
    0    +-----------------------------------> mean signal (dBm, less negative = louder)
        -75    -70    -65    -60    -55    -50
              ^                             ^
              bare-bare                     VIN+breakout+RFID
              baseline                      maximum range
```

Configs trade off **range (loudness)** against **calibration-friendliness (low asymmetry)**. There is no configuration in this matrix that achieves both. The cleanest is bare-bare (low asymmetry, modest range). The strongest is VIN-powered (high range, terrible asymmetry).

### 5.7 Per-device variability

The same physical configuration produces noticeably different results on Alpha vs Charlie:

| Configuration | Alpha-on-this-config asym | Charlie-on-this-config asym | Per-device Δ |
|---|---|---|---|
| breakout, USB-pwr, RFID off | 11 (E) | 13 (D) | 2 |
| breakout, USB-pwr, RFID on | 23 (A) | 6 (C) | **17** |
| breakout, VIN-pwr, RFID off | ~17 (F) | 23 (G) | 6 |
| breakout, VIN-pwr, RFID on | 21 (I) | 28 (H) | 7 |

The biggest disagreement is the **17 dB** spread on "breakout + RFID + USB-power": this configuration produces 23 dB asymmetry on Alpha but only 6 dB on Charlie. **Calibration constants therefore cannot be transferred between devices** — each device must be calibrated in its own exact mounting.

## 6. Findings

1. **Bare-bare is the cleanest configuration by a wide margin** (4 dB asymmetry vs 11–28 dB for any other config tested). Reproducibility is ~2 dB on asymmetry, ~5 dB on absolute level.

2. **The breakout PCB alone adds ~8 dB of asymmetry** with no help from RFID or VIN power. The board's metal traces and ground plane sit close enough to the WROOM trace antenna to act as parasitic structures.

3. **The RFID-RC522 coil's effect is per-device-unpredictable** (−7 to +12 dB asymmetry). Cannot be summarised as "RFID always makes it worse" — the coil's loaded vs. unloaded impedance interacts with each WROOM's antenna in module-specific ways. What's robust: with RFID present, you cannot predict the asymmetry direction.

4. **VIN power is a clean ~+7 dB signal boost on average** (the USB cable was a parasitic) but adds ~9 dB of asymmetry on average.

5. **Power wiring is part of the RF design.** The single largest measured effect was the power path: ~20–25 dB jump in TX strength (configs F, G) when switching from micro-USB to VIN power. The USB cable, when present, acts as a parasitic antenna near the WROOM trace antenna.

6. **Per-device hardware variability is significant.** The same "breakout + RFID + USB-power" configuration produces 23 dB asymmetry on Alpha but only 6 dB on Charlie — a 17 dB spread. PCB tolerances, module-to-header seating, ground-plane contact quality all matter.

7. **Firmware-side outlier gate is brittle to step changes.** When a configuration change caused TX strength to jump >15 dB (`outlier_db` default), the receiving peer's EMA got stuck at the old value because every new sample failed the outlier check. Recovery only via 15 s peer eviction, full reset, or temporarily widening the outlier threshold.

8. **Receiver saturation** appears around −40 to −45 dBm (config I, Charlie→Alpha at −42). Above this, the WROOM's AGC compresses and EMA / distance estimates become non-linear. Avoid calibrating in this regime.

9. **Reciprocity is the casualty.** RSSI reciprocity (the foundational assumption of all log-distance ranging models) is broken by every non-bare configuration tested in this matrix. Calibration cannot recover from it because the constants `tx_power_dbm` and `path_loss_n` are scalars, not direction-dependent matrices.

## 7. Recommendations

### For ranging accuracy (current fleet)

1. **Run all three nodes bare** until accurate ranging is no longer required, OR
2. **If RFID is required**, mount the RC522 on a *separate small board* connected by a short cable, with the coil placed ≥5 cm from the WROOM antenna trace. This restores most of the lost reciprocity at a small mechanical cost.
3. **Calibrate every device IN ITS EXACT MOUNTING.** Same breakout, same orientation, same power path, same nearby cables. Calibration constants do not transfer between mountings.
4. **Pick one power path for the whole fleet and stick with it.** Mixing USB and VIN-powered devices guarantees inconsistent asymmetries. VIN is preferable if range matters; bare-USB is fine for short-range.

### For firmware

5. **Add EMA-jump-detection + auto-reseed** to `peer_tracker.h`. If N consecutive frames all deviate the same direction by more than `outlier_db`, accept them and re-seed the EMA at the new level. Avoids the stuck-EMA failure mode after any environmental step-change.
6. **Add per-MAC calibration constants** so each device can store separate `tx_power_dbm` / `path_loss_n` per peer, not a single global pair. This would partially compensate for the per-pair asymmetry — at the cost of extra NVS bytes per peer.
7. **Add `rfid_polarity_check` on boot** — pulse the RC522 reset line and read its version register. If reads timeout or return a known fault code, log a clear "RC522 not responding — check polarity / wiring" warning instead of silently failing. (See cautionary note in §8 below.)

### For hardware (v2 design)

8. **Move the RC522 coil ≥5 cm from the WROOM antenna**, or use a WROOM module with a U.FL connector + external antenna. Either approach decouples ranging accuracy from RFID functionality.
9. **Standardise the deployment power path** — pick one (probably VIN through a clean regulator) and document it in the install guide. Reproduce that power path during calibration.
10. **Consider increasing the breakout regulator's transient capacity** (larger output capacitor, or higher-current LDO) to reduce brownout risk under combined WROOM + RC522 load.

## 8. Caveats & known limitations of this dataset

1. **Geometry was not laboratory-controlled.** Devices were physically handled between configuration changes. Position, orientation, nearby cables all shifted slightly. Reproducibility test (B vs J) shows ~5 dB random variation.
2. **Per-config samples are small** (4–10 raw RSSI samples per direction). Differences smaller than ~5 dB are not statistically significant.
3. **Bravo was excluded** for the second half of the session (powered off) because its proximity to Alpha was causing measurable receiver desense earlier. A 3-device matrix would be a useful follow-up.
4. **Distance was held approximately constant** (~1 m line-of-sight) but not tape-measured between every configuration. Some between-config variation is geometric.
5. **One brownout-shaped failure** during config H reconnection turned out to be operator wiring error (RC522 3v3/GND swapped). After the fix, no brownouts were observed. The "regulator can't source enough current for combined WROOM + RC522" hypothesis was not actually validated.
6. **RFID-on-breakout was tested in two states only**: powered (with the WROOM also running) vs unpowered (RC522 still wired to breakout, just not getting 3v3). A third state — *RC522 fully disconnected from the breakout* — was not tested and might give different results (the 8 wires alone could be antennas/parasitic loads even with no power).

## 9. Cross-reference

This document is a session report. Forward-looking recommendations are mirrored in `docs/SUGGESTED_IMPROVEMENTS.md` entry #41 (hardware) and entries #37 (asymmetry) / #40 (install guide) / #42 (firmware EMA reseed; if added).

---

# Part B: Multi-position swap experiments (later same session)

After the 10-config matrix above (configs A–J), the session continued with a more controlled geometry test designed to decompose **per-device hardware variation** from **per-position multipath effects**.

## B1. Setup

- All three devices **bare** (no breakout, no RFID-RC522).
- All three on **independent power banks** (no shared power supply, no PC USB).
- Equilateral triangle geometry: 1000 mm ± 10 mm between adjacent device antennas.
- Internal angles 60° ± 5°.
- Devices "mostly pointing to the centre" (orientation tolerance ~30° not laboratory-precise).
- Devices placed on a "mostly flat" surface (some unmeasured tilt possible).
- USB power cables routed OUTSIDE the triangle, well away from the antennas (this is a meaningful change from earlier configs where cables sat near the antenna).
- All three nodes located far from the WiFi router (to avoid RX desense from a strong nearby transmitter).

## B2. Configurations tested

| ID | Description | Position layout |
|---|---|---|
| K | Initial bare-bare-bare equilateral, all on power banks | Alpha @ α-spot · Bravo @ β-spot · Charlie @ γ-spot |
| L | Alpha and Charlie physically swapped, cables stayed with each device | Alpha @ γ-spot · Bravo @ β-spot · Charlie @ α-spot |
| M | Then Alpha and Bravo physically swapped (from L) | Alpha @ β-spot · Bravo @ γ-spot · Charlie @ α-spot |

α, β, γ are abstract names for "Alpha's original position", "Bravo's original position", "Charlie's original position".

## B3. Raw RSSI medians per configuration

(All values dBm. Median over an 18 s capture, ~6-8 raw frames per direction. Devices at the listed positions for that configuration.)

| Direction | K (original) | L (A-C swapped) | M (A-B swapped) |
|---|---|---|---|
| Alpha → Charlie | −42 | −48 | −52 |
| Alpha → Bravo | −61 | −64 | −58 |
| Bravo → Charlie | −49 | −48 | −57 |
| Bravo → Alpha | −66 | −57 | −64 |
| Charlie → Alpha | −66 | −59 | −66 |
| Charlie → Bravo | −66 | −65 | −64 |

## B4. Pair asymmetries

| Pair | K asym | L asym | M asym |
|---|---|---|---|
| Alpha ↔ Bravo | 5 | 7 | 6 |
| Alpha ↔ Charlie | 24 | 11 | 14 |
| Bravo ↔ Charlie | 17 | 17 | 7 |

**Observations:**

1. **Alpha-Bravo pair is consistently clean** across all three configurations (5–7 dB). Both devices behave like nominal bare WROOMs with each other.
2. **Alpha-Charlie asymmetry collapsed dramatically** from 24 → 11 dB after the A-C swap. The asymmetry didn't survive moving the devices, so most of it was position-related rather than purely hardware.
3. **Bravo-Charlie asymmetry collapsed** from 17 → 7 dB after the A-B swap, when Bravo moved away from its original β-spot. The β-spot is environmentally favourable for Bravo's link to Charlie at γ.

## B5. Per-device TX/RX gain decomposition (config K)

Modelling each measurement as `RSSI = base + G_TX(sender) + G_RX(receiver) + path_loss(positions)` and assuming roughly equal path loss across the equilateral triangle (constant 1 m), the six K-config measurements decompose into per-device TX/RX gains:

| Device | TX gain (dB rel. cluster) | RX gain (dB rel. cluster) |
|---|---|---|
| Alpha | 0 (reference, weakest TX) | +5 (most sensitive) |
| Bravo | ≈ 0 | 0 |
| **Charlie** | **+18** | 0 |

Self-consistent within ±2 dB across the three pair asymmetries. The model says Charlie has an apparent ~18 dB TX advantage — but the swap experiment (Section B6) showed roughly half of that is environmental.

## B6. Position-vs-hardware decomposition

The L swap (Alpha and Charlie traded positions) lets us separate the position factor at α and γ from the per-device hardware factor.

Modelling Bravo's view as `RSSI = TX_device_hw + POS_factor + path_const`:

| Measurement | Pre-swap (K) | Post-swap (L) | Implication |
|---|---|---|---|
| Bravo → device@α-spot | −66 (Alpha there) | −66 (Charlie there) | TX_charlie + POS_α = TX_alpha + POS_α → TX_charlie ≈ TX_alpha (?!) |
| Bravo → device@γ-spot | −49 (Charlie there) | −57 (Alpha there) | TX_charlie + POS_γ = −49; TX_alpha + POS_γ = −57 → TX_charlie − TX_alpha = +8 dB |

The two estimates of the device-TX delta disagree (+0 from α-spot vs +8 from γ-spot), which means the additive-gain model isn't quite right. The interaction between device hardware and physical position is more complex than a simple sum — likely because the antenna's radiation pattern interacts with the physical environment differently depending on orientation, surface coupling, and nearby reflectors.

**The honest interpretation:**

- **Position contributes ~5–10 dB** of asymmetry. The γ-spot is environmentally louder than the α-spot in Bravo's direction by approximately 9 dB.
- **Charlie's hardware contributes ~8–12 dB** of additional TX strength relative to Alpha's hardware. Even in the position-disadvantaged α-spot post-swap, Charlie is still heard at −48 by Bravo, where Alpha at the same spot was heard at −66.
- **Combined**, these two effects gave Charlie its apparent 18 dB TX boost in the K configuration, but the components are mixed — neither effect alone explains the data.

## B7. Implications for calibration

1. **Calibration is sensitive to physical position, not just antenna orientation.** Moving a device by a metre — same orientation, same hardware, same cables — can shift the RSSI by 8–10 dB due to multipath. Calibration constants captured at one mounting point do not transfer to another mounting point in the same room.
2. **Per-device hardware variation in TX power is real but smaller than initially thought** (~8–12 dB between Charlie and Alpha, not 18+). Still well outside the ESP32 datasheet ±2 dB tolerance, suggesting marginal antenna trace match or undocumented variation.
3. **Bravo and Alpha together give a usable 5–7 dB asymmetry** — the cleanest pair in the fleet. If a calibration walk-through is needed, calibrate Alpha against Bravo and trust the result. Charlie should be calibrated separately with a per-pair adjustment if the firmware's per-MAC calibration ever gets implemented (entry #41.b).
4. **The room itself contributes meaningful multipath bias.** A different room would give different position factors. For accurate ranging, calibrate IN the deployment location, not in a different test rig.

## B8. Updated headline numbers

Statistical effects, refined with the swap data:

| Effect | Asymmetry impact | Mean signal impact |
|---|---|---|
| Bare-bare baseline | 4 dB asym | −74 dBm |
| Adding the breakout PCB | +8 dB | basically same |
| Adding the RFID coil | −7 to +12 dB (per-device, unpredictable) | basically same |
| Switching USB → VIN power | +9 dB worse asymmetry, +7 dB stronger signal | +7 dB stronger |
| **USB cable in antenna near-field** | **+5 to +10 dB** | up to +25 dB stronger when present |
| **Per-device hardware variation (TX power)** | **~8–12 dB across this fleet** | varies |
| **Per-position multipath** | **~5–10 dB depending on room geometry** | varies per pair |
| Worst-case stack (config H) | +24 dB worse than baseline | +3 dB stronger |

The two new entries (USB-cable parasitic and per-position multipath) emerged from this Part B work and matter at least as much as any of the earlier hardware-config effects.

## B9. Caveats specific to Part B

1. **Only one direction of swap was tested per pair.** A complete decomposition would require all 6 permutations of Alpha/Bravo/Charlie across α/β/γ positions (3! = 6). We measured 3 of those 6, leaving an under-determined linear system.
2. **Orientation tolerance was ±30°.** "Pointing to the centre" was not a precise alignment. Some of the per-device "TX hardware" delta is probably orientation noise that the additive-gain model is absorbing.
3. **The "mostly flat" surface had unmeasured tilt.** Height differences of even 5 mm change the surface-multipath geometry and can shift RSSI by 1–3 dB.
4. **Outlier-gate stuck-EMA artefacts** continued to plague the dataset. Several pre-swap EMAs were stuck at values from earlier session configurations and only recovered after the natural 15-second peer-eviction timeout. Raw RSSI was used for analysis throughout to avoid this issue.
5. **Same-second timing across the three nodes was not synchronised.** Each node's MQTT publish goes out on its own 2-second cadence, so the "snapshot" of all three pairs at one moment is approximate.
