# Operator install guide — antenna + power layout

> Field-installation reference for ESP32-WROOM nodes. The wrong physical
> arrangement of breakout, RFID coil, and USB cabling has been observed to
> add up to **24 dB of asymmetric path loss** between two nominally-identical
> devices, translating to ~9× distance error in one direction relative to
> the other. This guide captures the rules of thumb derived from the
> 10-config sweep documented in
> [SESSIONS/RF_CONFIG_TEST_2026_04_25.md](SESSIONS/RF_CONFIG_TEST_2026_04_25.md)
> and the resolved finding under [#41](SUGGESTED_IMPROVEMENTS_ARCHIVE.md#41).
> Tracking entry: [#40](SUGGESTED_IMPROVEMENTS_ARCHIVE.md#40).

## TL;DR

If you only follow three rules:

1. **Don't mount the RC522 RFID module on the same breakout as the WROOM.**
   Move it ≥ 5 cm away on a separate small board, antenna-side facing
   away from the WROOM.
2. **Pick one power path for the whole fleet — USB *or* VIN — and don't
   mix.** Calibration constants do not transfer between USB-powered and
   VIN-powered devices.
3. **Route USB power cables ≥ 10 cm from the WROOM antenna**, perpendicular
   if possible. USB cables in the antenna near-field can act as a
   parasitic radiator and shift TX in one direction by up to 25 dB.

If the application uses ESP-NOW ranging or relies on RSSI-based decisions,
**re-run calibration** after any physical re-arrangement. RSSI is sensitive
to layout to a degree that surprises most engineers.

## Why this matters

The ESP32-WROOM module integrates a PCB trace antenna at one end of the
module. The radiation pattern is approximately omnidirectional in the
horizontal plane, but the near-field is sensitive to:

- Conductive surfaces close to the antenna (breakout PCB ground planes,
  metal shielding cans on neighbouring components).
- Resonant structures near the antenna (RFID antennas tuned at 13.56 MHz
  still couple weakly to the 2.4 GHz path through the module's internal
  matching network and the breakout's ground plane).
- Trace routing on the breakout that's not impedance-matched to the
  WROOM's antenna feed.
- Any conductor in the antenna's near-field that can re-radiate the
  signal — chiefly USB cables.

The net effect is **per-device, per-mounting** path-loss variation that
no firmware-side calibration can fully correct. Calibration constants
captured on a bench rig stop being valid the moment the device moves to
its installed location.

## Recommended physical layout (descending impact)

### 1. RFID coil placement

If the application uses an MFRC522 (or compatible) RFID reader:

- **Mount the RC522 module on a separate small carrier**, not on the
  same breakout PCB as the WROOM.
- **Distance: ≥ 5 cm WROOM antenna ↔ RC522 coil.** Less than this and
  the RX sensitivity of the WROOM drops by ~6 dB even with the regulator
  healthy.
- **Orientation: RC522 antenna parallel to the floor, WROOM antenna
  perpendicular.** Cross-polarisation reduces inter-coupling.
- **If the RC522 is unpowered**, it still couples — physical separation
  matters more than the power state.

### 2. Power path consistency

The fleet has historically had a mix of USB-powered and VIN-powered
nodes. The 10-config sweep showed VIN-power adds ~7 dB to TX and ~9 dB
to asymmetry vs USB-power. That asymmetry is **deterministic per
configuration but cannot be removed** by software-side calibration when
the fleet is mixed.

- **Pick one**: either every device on USB, or every device on VIN-via-
  breakout. Do not mix.
- If using VIN, the regulator must be one of the known-good types (LDO
  with at least 800 mA headroom). Cheap breakout regulators sag under
  the WiFi PA's burst current and add their own asymmetric noise floor.
- Re-run calibration after any power-path change.

### 3. Breakout vs bare module

Breakout PCBs add ~+8 dB asymmetry over a bare WROOM module due to the
ground plane proximity and edge-launch antenna feed. If the application
demands the lowest-asymmetry RF (e.g. precise distance estimation):

- **Prefer bare WROOM modules** soldered onto a custom carrier with the
  antenna edge cleared per the WROOM datasheet (no copper or components
  within 5 mm of the antenna footprint).
- **If using breakouts is non-negotiable** (e.g. dev environment, RFID
  required), be consistent: same breakout type across the fleet.

### 4. USB cable routing

USB cables are a known parasitic radiator at 2.4 GHz. The 10-config
sweep showed a 25 dB TX boost in one direction when a USB cable lay in
the antenna near-field — and that direction was unpredictable per
device.

- **Keep USB cables ≥ 10 cm from the WROOM antenna.**
- **Run USB cables perpendicular to the antenna axis** if forced into
  the near-field (e.g. tight enclosure).
- **Don't coil USB cables** near the antenna — coiled cable acts as a
  resonant loop antenna at unpredictable frequencies.
- **A ferrite choke on the USB cable** within 5 cm of the device port
  reduces the parasitic effect ~3 dB but does not eliminate it.

### 5. Mounting orientation

The WROOM antenna is most efficient when the module's edge with the
antenna trace is **away from any large conductor** (metal enclosure,
breakout ground plane, mounting plate). When mounting:

- **Antenna edge points at empty space**, not at a wall, mounting
  bracket, or another device.
- **Avoid conductive enclosures.** Plastic is best. If a metal enclosure
  is required, an external antenna (U.FL connector to an external
  whip) is the right answer — not a bare-module-in-metal compromise.
- **Two devices in line-of-sight should have their antenna edges facing
  each other**, not facing perpendicular.

## Calibration discipline

Calibration constants captured on a bench rig depend on every factor
above. After any of:

- Moving a device to a new install location
- Changing the breakout / RFID / power-path combination
- Replacing a cable
- Adding or removing a metal enclosure / mounting bracket

…the calibration is **stale**. Either:

1. Re-run calibration in-place via `cmd/espnow/calibrate` (per-peer,
   per-device), OR
2. Accept that distance estimates will have a per-direction bias of
   up to ~9× until calibration is refreshed.

For most fleet deployments, in-place calibration is the only sustainable
discipline. Bench-calibration is a starting point, not a final state.

## Diagnosing an asymmetry

If a device pair shows persistent asymmetry > 5 dB after calibration:

1. Check the asymmetry direction. If the same device "always wins" in
   both pairs it participates in, that device is the outlier.
2. Verify power path. A USB-powered device next to a VIN-powered device
   pair is the most common cause.
3. Verify USB cable routing. Try moving the cable; recheck.
4. Verify RFID coil distance. If < 5 cm, fix that first.
5. If all of the above are clean and asymmetry persists, the device
   may have a damaged WROOM module — physical inspection for
   mechanical damage near the antenna (cracked PCB, bent shielding
   can, solder splash on the antenna feed) is warranted.

The wiring-polarity caveat from #41 also applies: a 3v3/GND-swapped
RC522 looks identical to a brownout from the dashboard side. **Verify
RC522 polarity first** before assuming a software / RF issue.

## What this guide is NOT

- Not a calibration procedure. See `cmd/espnow/calibrate` and the
  `docs/SESSIONS/ESPNOW_*` audit reports.
- Not a hardware-design reference. The WROOM datasheet is the
  authoritative source for antenna keep-out zones.
- Not a substitute for in-place RF measurement. RSSI is the ground
  truth; this guide is rules of thumb to get close to a good starting
  point.
