# Hardware wiring — expansion modules

How to add the 4x4 NeoPixel matrix + 2-channel relay to an existing
breakout-board ESP32 (e.g. Alpha 2026-04-29). Pin assignments and
firmware gates are already in `config.h` — this doc shows the physical
side and the post-wiring firmware steps.

## ESP32 pin map (Alpha breakout, v0.4.31)

| Function | GPIO | Direction | Constraint |
|---|---|---|---|
| WS2812 data | **27** | OUT | `LED_STRIP_PIN`. Existing pin for the 8-LED strip. |
| Relay CH1 | **25** | OUT | `RELAY_CH1_PIN`. Active-LOW. No strapping conflict. |
| Relay CH2 | **26** | OUT | `RELAY_CH2_PIN`. Active-LOW. No strapping conflict. |
| RFID SS (SPI CS) | 5 | OUT | `RFID_SS_PIN` — already wired on Alpha if RC522 fitted. |
| RFID RST | 22 | OUT | `RFID_RST_PIN` |
| RFID IRQ | 4 | IN  | `RFID_IRQ_PIN` |
| Status LED | 2 | OUT | onboard |
| (reserved) Hall AO | 32 | IN ADC | `HALL_AO_PIN` — v0.5.0+ |
| (reserved) Hall DO | 33 | IN | `HALL_DO_PIN` — v0.5.0+ |
| SPI default bus (RFID) | SCK 18, MISO 19, MOSI 23 | — | VSPI default |

## 4x4 NeoPixel matrix (16 LEDs, WS2812B)

The matrix is itself a chain — same protocol as the existing strip.
Two install options:

### Option A — replace the existing 8-LED strip with the matrix
- Disconnect the strip from the breakout's WS2812 header.
- Connect matrix's pins to the same header:

| Matrix pin | Wire to |
|---|---|
| **DIN** | GPIO 27 (data line) |
| **5V**  | 5V rail |
| **GND** | GND |

- Bump runtime LED count to 16:
  ```bash
  mosquitto_pub -h 192.168.10.30 -r \
    -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<alpha_uuid>/cmd/led' \
    -m '{"count":16}'
  ```

### Option B — cascade the matrix after the existing strip (24 LEDs total)
- Connect the strip's **DOUT** (data-out) to the matrix's **DIN**.
- Common 5V + GND across both modules.
- Bump runtime count to 24 same way (`{"count":24}`).
- Brightness cap (`LED_MAX_BRIGHTNESS = 80` in config.h) keeps mean
  current sane: 24 × 60 mA × 80/255 ≈ **450 mA mean**, ~1.4 A peak
  if the firmware ever draws full white. The peak is the risk.

### Power notes (both options)

- **Bulk capacitor: 1000 µF electrolytic across 5V / GND, physically
  close to the matrix DIN pin.** Smooths the inrush when many pixels
  flip from off to bright. Required if not already on the breakout.
- **Data-line series resistor: 470 Ω in line with DIN.** Optional on
  short runs; recommended whenever the data wire is >10 cm or runs
  near a relay coil.
- **5V source capacity ≥1.5 A peak.** USB ports vary — most laptop
  ports are 500 mA fused, which is fine for the strip (~480 mA
  peak) but marginal for matrix-alone (960 mA peak) and brown-out-
  prone for the cascaded 24-LED config.
  - Alpha had brownouts traced to LED inrush in v0.4.26 with just
    8 LEDs. Doubling peak draw will revisit that risk if the
    supply isn't upgraded.
- **Level-shift consideration**: ESP32 GPIO is 3.3 V logic; WS2812B
  Vih spec is 0.7×Vdd = 3.5 V at 5 V Vdd. In practice 3.3 V drives
  WS2812B reliably at room temperature with a short data wire (<30 cm).
  If you see flickering or wrong colours, drop in a 74HCT245 / 74HCT04
  level shifter on the data line.

## 2-channel relay module (JQC-3FF-S-Z)

Module pinout (typical 5V active-LOW board):

| Module pin | Wire to | Notes |
|---|---|---|
| **VCC** | 5V rail | The relay coils + opto-isolators. ~70 mA per active channel. |
| **GND** | GND | |
| **IN1** | GPIO 25 (`RELAY_CH1_PIN`) | LOW = relay energised (NO closes) |
| **IN2** | GPIO 26 (`RELAY_CH2_PIN`) | LOW = relay energised |
| **JD-VCC + VCC jumper** | leave default (VCC=JD-VCC) | unless powering relay coils from a separate isolated supply |

Relay output side (per channel):

| Output pin | Wire to |
|---|---|
| **COM** | One side of the load |
| **NO**  | Live / 5V to switch when energised |
| **NC**  | Live / 5V to switch when de-energised (rarely used in this design) |

### Boot-time chatter

`RELAY_ACTIVE_LOW = 1` means LOW energises. Before the firmware sets
GPIO 25/26 to OUTPUT and writes HIGH (de-energise), the pin floats and
the relay can briefly chatter. Two mitigations:

1. **Firmware-side** (preferred): the v0.5.0 plan has the init
   sequence write HIGH BEFORE `pinMode(OUTPUT)` so the chatter window
   is closed. Confirm `relay.h::relayInit()` does this before going
   live with safety-critical loads. See [../PLAN_RELAY_HALL_v0.5.0.md](../PLAN_RELAY_HALL_v0.5.0.md).
2. **Hardware-side**: add a 10 kΩ pull-up to 3.3 V on each IN line.
   GPIO floats high → relay stays de-energised through the boot
   transient. Cheap insurance.

### Firmware enable

The relay module is gated behind `#define RELAY_ENABLED` in
[`config.h`](../../include/config.h). Two ways to turn it on:

- **Per-device, with current default firmware:** uncomment
  `// #define RELAY_ENABLED` (config.h:672), rebuild + USB-flash that
  device. The OTA path won't deliver this — it's a compile-time gate.
- **Variant build (preferred):** flash the `[env:esp32dev_relay_hall]`
  variant from PlatformIO locally:
  ```bash
  cd esp32_node_firmware
  python -m platformio run -e esp32dev_relay_hall
  python -m platformio run -e esp32dev_relay_hall -t upload --upload-port COM4
  # (use the pio-utf8.sh wrapper if you hit the cp1252 console hang — #95)
  ```
  CI builds this variant on every release as a compile gate (build.yml
  `build-variants` job) but **does NOT upload the variant binary to
  the GitHub release** — the published `firmware.bin` is the standard
  esp32dev only. So the path right now is local-build + USB-flash. A
  backlog entry to publish variant binaries alongside `firmware.bin`
  is queued. Once OTA-routing per device variant lands (post-v0.5.0),
  the operator can pin a device to `BUILD_VARIANT=relay_hall` in NVS
  and OTA delivers the right binary.

After enabling, control via MQTT. Schema is `{"ch":<n>,"state":<bool>}`
for a single channel, or `{"ch":"all","state":<bool>}` to set both
together (verified against `mqtt_client.h::onMqttMessage` — the
PLAN doc's schema is canonical; an earlier draft of this doc had the
wrong combined-fields example):

```bash
# Energise CH1
mosquitto_pub -h 192.168.10.30 \
  -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/relay' \
  -m '{"ch":1,"state":true}'

# De-energise CH2
mosquitto_pub -h 192.168.10.30 \
  -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/relay' \
  -m '{"ch":2,"state":false}'

# All channels off (also accepts "all" for ch)
mosquitto_pub -h 192.168.10.30 \
  -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/relay' \
  -m '{"ch":"all","state":false}'
```

State publishes back to `<root>/<uuid>/status/relay` (retained) as
`{"ch1":<bool>,"ch2":<bool>}` — that's the **status** payload (combined
view of both channels), not the **command** payload.

## Quick smoke test (after firmware flash + wiring)

Once Alpha (or any device) is on the relay+hall variant build with the
hardware physically attached, this 5-step sequence proves end-to-end
that the firmware sees the modules:

```bash
UUID="<your_device_uuid>"   # resolve from /fleet-status if unsure
T="Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/$UUID"

# 1. Confirm relay_enabled + hall_enabled in heartbeat (75 s window)
timeout 75 mosquitto_sub -h 192.168.10.30 -W 75 -t "$T/status" \
  | grep -oE '"(relay|hall)_enabled":[a-z]+' | head -4

# 2. Click relay 1 closed (audible click) and verify status echo
mosquitto_pub -h 192.168.10.30 -t "$T/cmd/relay" -m '{"ch":1,"state":true}'
sleep 1
timeout 5 mosquitto_sub -h 192.168.10.30 -W 5 -t "$T/status/relay"

# 3. Click relay 1 open
mosquitto_pub -h 192.168.10.30 -t "$T/cmd/relay" -m '{"ch":1,"state":false}'

# 4. Watch live hall telemetry for 10 s — should publish at 1 Hz default
timeout 10 mosquitto_sub -h 192.168.10.30 -W 10 -t "$T/telemetry/hall"

# 5. Wave a magnet near the sensor — expect threshold-edge events
timeout 30 mosquitto_sub -h 192.168.10.30 -W 30 -t "$T/telemetry/hall/edge"
```

If any step is silent: re-check `RELAY_ENABLED`/`HALL_ENABLED` is in
the binary (`firmware-relay_hall.bin` from the GH release artefacts,
NOT plain `firmware.bin`), wiring per the table above, and that the
heap-largest didn't drop significantly post-flash (#84-style
verify-after-action discipline).

## Bench checklist before powering up

1. [ ] Multi-meter continuity check on every wire — especially the
   ground path between the breakout, the relay module, and the matrix
   header.
2. [ ] Verify NO short between 5V and GND with the modules connected
   but ESP32 unplugged.
3. [ ] Confirm relay IN lines have either firmware-side write-HIGH-
   before-pinMode OR a hardware pull-up.
4. [ ] Bulk cap polarity — electrolytics blow if reversed.
5. [ ] Confirm USB or external 5V supply rated for ≥1.5 A.
6. [ ] Re-flash Alpha with relay-enabled build (`RELAY_ENABLED` define
   or `esp32dev_relay_hall` variant).
7. [ ] Power-on with NO load on the relay output — verify relay
   doesn't chatter at boot, that you can hear individual click on
   `cmd/relay {"ch1":true}`.
8. [ ] Power-on the matrix — verify all 16 pixels light correctly via
   `cmd/led {"r":255,"g":0,"b":0}` (red), `{"r":0,"g":255,"b":0}` (green),
   etc. Watch for any pixel showing wrong colour (suggests data-line
   integrity issue → add the 470 Ω series resistor).
9. [ ] Heap check on Alpha post-flash — `heap_largest` in heartbeat
   should match v0.4.31 baseline (81908 bytes). If significantly lower,
   investigate before going live.

## Failure modes to watch

- **Brownout under combined load** — most likely failure on Alpha.
  Watch for `boot_reason=brownout` in the next 24 h after wiring.
- **WS2812 flicker / wrong colours** — data-line integrity (level
  shift, series resistor, longer wire than expected).
- **Relay click on every device boot** — boot-chatter window not
  closed. Add the pull-up OR verify the firmware init sequence.
- **`cmd/relay` ack but no physical click** — `RELAY_ENABLED` not
  compiled in. Re-check your binary path.

## Related docs

- [INSTALL_GUIDE.md](INSTALL_GUIDE.md) — physical install discipline (antenna orientation, RC522 distance)
- [LED_REFERENCE.md](LED_REFERENCE.md) — what each WS2812 pattern means
- [LED_COMMANDS.md](LED_COMMANDS.md) — `cmd/led` schema
- [MQTT_COMMAND_REFERENCE.md](MQTT_COMMAND_REFERENCE.md) — full `cmd/*` topic list
- [../PLAN_RELAY_HALL_v0.5.0.md](../PLAN_RELAY_HALL_v0.5.0.md) — v0.5.0 hardware plan and rationale
- [../../include/config.h](../../include/config.h) — pin definitions, brightness cap, intervals

Last updated: 2026-04-29 (v0.4.31).
