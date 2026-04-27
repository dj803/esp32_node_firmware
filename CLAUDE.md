# ESP32 Node Firmware

## Repo Layout
- Canonical working clone: `C:\Users\drowa\Documents\git\Arduino\NodeFirmware\` (git root).
- Firmware source is in the `esp32_node_firmware/` subfolder, NOT the repo root.
- Working branch is `master`. Do not push to `main`.
- Open VS Code at `C:\Users\drowa\Documents\git\Arduino\NodeFirmware\esp32_node_firmware\` so PlatformIO IDE picks up `platformio.ini`.
- GitHub: `dj803/esp32_node_firmware`. Tags `v*.*.*` trigger CI release builds.
- OTA manifest: `https://dj803.github.io/esp32_node_firmware/ota.json` (served from `gh-pages` branch).

## Build & Test (from `C:\Users\drowa\Documents\git\Arduino\NodeFirmware\esp32_node_firmware\`)
- Compile for ESP32:        `pio run -e esp32dev`
- Flash a specific port:    `pio run -e esp32dev -t upload --upload-port COMx`  ← see COM port verification below
- Serial monitor:           `pio device monitor -p COMx -b 115200`
- Host-side unit tests:     `pio test -e native -v`   ← run before every commit
- Default baud for serial monitor is 115200.
- **Always verify which device is on COMx BEFORE flashing** — see "COM port assignments are NOT fixed" in Device Fleet below.

## Device Fleet (firmware v0.4.10)
- ESP32-Alpha   — UUID `32925666-155a-4a67-bf50-27c1ffa22b11`, MAC `84:1F:E8:1A:CC:98`
- ESP32-Bravo   — UUID `6cfe177f-92eb-4699-a9a6-8a3603aae175`, MAC `F4:2D:C9:73:D3:CC` (8 WS2812 LEDs)
- ESP32-Charlie — UUID `2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd`, MAC `D4:E9:F4:60:1C:C4`
- **COM port assignments are NOT fixed.** Multiple ESPs are on the bench at any time and Windows assigns COM ports in plug-in order. Before flashing or monitoring, ALWAYS verify which device is on which port:
  ```powershell
  Get-PnpDevice -Class Ports -PresentOnly | Select-Object Name,Status
  ```
  Then resolve to MAC by either reading the device's USB serial descriptor, or by running `esptool chip-id` and matching MAC against the table above:
  ```bash
  PYTHONIOENCODING=utf-8 "C:/Users/drowa/.platformio/penv/Scripts/python.exe" \
    "C:/Users/drowa/.platformio/packages/tool-esptoolpy/esptool.py" \
    --chip esp32 --port COM5 chip-id
  ```
  Cross-check the printed MAC against the fleet table before pulling the trigger on `pio run -t upload --upload-port COMx`.
- ESP32-Delta   — UUID `2b89f43c-2fd8-4ed6-ac9d-fb0d8f97c282`
- ESP32-Echo    — UUID `2fdd4112-9255-42a8-a099-ada0075a677b`
- ESP32-Foxtrot — UUID `3b3b7342-80e7-43dd-afc7-78d0470861e2`, MAC `28:05:A5:32:50:44` (RFID reader; bootstrap-bypass anomaly noted in #50)
- Always resolve UUIDs from live MQTT before scripting fleet ops — don't hardcode from memory; cross-check against `tail -n 200 C:/ProgramData/mosquitto/mosquitto.log`.
- MQTT broker: `192.168.10.30:1883` (Mosquitto on this host, service name `mosquitto`)
- Status topic: `Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status`
- Daily health retained summary: `Enigma/JHBDev/Office/Line/Cell/health/daily`

## Monitoring Routine
- Run `/daily-health` (Claude slash command) each morning or via `/loop 24h /daily-health`.
- Backed by `C:\Users\drowa\tools\daily_health_check.py` + `daily_health_config.json`.
- Reports land in `C:\Users\drowa\daily-health\`. Exit codes: 0=green, 1=yellow, 2=red.
- Mosquitto file logging lives at `C:\ProgramData\mosquitto\mosquitto.log` once
  `apply-logging-config.ps1` has been run elevated.

## Diagnostic process (for fleet issues — silent OTAs, missing devices, stuck states)
Always run these in order before assuming firmware bugs:
1. `tail -n 200 C:/ProgramData/mosquitto/mosquitto.log` — confirms which UUIDs are
   actually publishing right now and what topics they hit. Catches stale-UUID,
   client-disconnect, and broker-side issues before any device-side digging.
2. `tools/fleet_status.sh` — snapshot retained boot announcements per device;
   cross-check UUIDs against the log.
3. **Never hardcode device UUIDs in scripts.** Resolve from live MQTT first;
   fall back to a hardcoded map only as a last resort. UUID drift has been
   observed on Delta/Echo (#48) — see SUGGESTED_IMPROVEMENTS.

## Monitoring sessions — always run the silent watcher
For any session where stability is a concern (post-flash soak, post-OTA,
overnight runs, Phase B-style diagnostics), arm both watchers:

- **`tools/silent_watcher.sh`** as a Monitor task — alerts on LWT offline
  (silent deadlocks that don't reboot) AND abnormal boot reasons (panic,
  task_wdt, int_wdt, brownout). Catches the failure modes the boot_history
  watcher misses. Documented in #60.
- **boot_history poller** as a separate Monitor task — alerts on
  net-new abnormal entries in the Node-RED `boot_history` flow context.

Without the silent watcher, deadlock-class failures (#51 failure mode (b))
go unnoticed until someone eyeballs the LEDs.

## Capturing fresh-device first boot (provisioning / bootstrap debugging)
PySerial's default DTR-on-open RESETS the ESP32, so opening serial right
after a `write-flash` captures the SECOND boot, not the first. By that
time ESP-NOW bootstrap from siblings has often completed (sub-second on
the active Wi-Fi channel — much faster than the 6.5 s worst-case spec).
That cost us hours on 2026-04-25 (#50).

To capture a TRULY first boot:
1. `esptool ... --after no-reset erase-flash`
2. `esptool ... --after no-reset write-flash ...`  (chip stays in download mode)
3. Open serial with controlled RTS reset (NOT default DTR-toggle):
       s = serial.Serial(); s.port='COM5'; s.baudrate=115200
       s.dtr=False; s.rts=True; s.open()
       time.sleep(0.2); s.rts=False     # release reset → first boot
4. Or isolate from siblings: power-down the rest of the fleet so any
   bootstrap that does fire returns "no sibling response" rather than
   silently completing in the gap.

## Environment Notes
- Git repos must NOT live inside OneDrive (cloud placeholders cause .git lock failures). Canonical repo is at `C:\Users\drowa\Documents\git\Arduino\NodeFirmware\`.
- esptool installed version is 4.5.1 — verify the actual path before suggesting commands.
- MCP config fixes go in `~/.claude/.mcp.json` (user level), not project folder.
- Node-RED is **Dashboard 2.0** (Vue.js templates use `<template>`, `v-if`, `v-for`) — NOT legacy Angular (`ng-if`, `ng-repeat`).
- Node-RED flows live in a Projects directory at `C:\Users\drowa\.node-red\projects\esp32-node-firmware\flows.json`. The top-level `C:\Users\drowa\.node-red\flows.json*` files are STALE relics — do not read them.
- Node-RED admin API is open (no auth) at `http://127.0.0.1:1880/flows`. Use `Node-RED-Deployment-Type: nodes` on POST to redeploy only changed nodes without MQTT reconnect churn.

## Firmware Dependencies

### Library Versions
- MFRC522 library is v2.x — use v2 API (not MFRC522v1).
- NimBLE is 2.x — account for API changes from 1.x.
- Verify library/SDK versions before writing code that depends on APIs.

## Workflow

### Verification Before Commit
Always wait for user to provide compile output or serial logs confirming a fix works before moving to commit/push. Do not assume code changes succeed.

## Release Pipeline
When user requests a version release or OTA deployment, ALWAYS execute the full pipeline in one flow: commit → tag → push → GitHub release → update OTA manifest. Do not stop after code changes.
