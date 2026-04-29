# AP-mode setup walkthrough

Every fresh device boots into AP mode on first power-on (no credentials
in NVS yet). Same path is reachable mid-deployment via `cmd/config_mode`
or after the firmware drops to AP mode following 3+ consecutive
`mqtt_unrecoverable` reboots (#76 sub-D).

## When AP mode fires

- **First boot** — NVS empty, no SSID/password to try.
- **Operator request** — `cmd/config_mode` published to `<root>/<uuid>/cmd/config_mode`.
- **Self-recovery** — 3+ consecutive `mqtt_unrecoverable` causes in
  RestartHistory triggers AP-mode fallback on next boot. Self-clears
  after 5 min of stable MQTT once reconnected.
- **Bad credentials suspected** — `WIFI_AUTH_FAIL_CYCLES` consecutive
  AUTH_EXPIRE / HANDSHAKE_TIMEOUT disconnects → CredentialStore::setCredStale(true)
  → restart into AP mode.

## Visual cue

The onboard LED shows the **AP_MODE pattern**: 50/50/50/850 ms
double-blink-pause. See [LED_REFERENCE.md](LED_REFERENCE.md). Distinct
from any other state — if you see this, the device is waiting for you.

## Step-by-step

1. **Connect a phone or laptop to the device's AP network.**
   - SSID: `ESP32-Config-XXXX` (where `XXXX` = last 4 hex of the device MAC)
   - Password: `password` (default — change in production via config.h's
     `AP_PASSWORD`)
   - On iOS / modern Android the captive-portal sheet pops automatically
     (v0.4.24 / #34 — DNS hijack + port-80 redirect to `https://192.168.4.1/`).

2. **Browse to `https://192.168.4.1/`** if the captive sheet didn't pop.
   - You'll get a self-signed cert warning. Accept it — the device
     generates a fresh per-device key on first boot.

3. **Fill the form:**
   - WiFi SSID + password (the production network)
   - MQTT broker URL (or leave blank to let the firmware auto-discover
     via mDNS / port scan; manual fallback is the failsafe)
   - OTA JSON URL (the gh-pages manifest path —
     `https://dj803.github.io/esp32_node_firmware/ota.json` in the
     current deployment)
   - Optional: friendly node name, MQTT topic hierarchy segments

4. **Submit.** The device saves to NVS and restarts into normal
   operation.

5. **Verify reconnect** — `tools/fleet_status.sh` should show the device
   within 60 s post-reboot, on a fresh `event=boot` with `boot_reason=software`
   or `poweron`. The onboard LED should transition from AP_MODE
   double-blink → BOOT solid → WIFI_CONNECTING → MQTT_CONNECTED
   mostly-on.

## Idle timeout

`AP_MODE_IDLE_TIMEOUT_MS` (5 min default) — if no admin HTTPS handler
hits and no STA reconnect succeeds within this window, the device
hard-restarts. Prevents a transient-failure-induced AP-mode fall from
stranding a device invisible forever.

## Background STA scan (v0.3.15+)

Even while in AP mode, the radio also scans for the configured SSID
every `AP_STA_SCAN_INTERVAL_MS` (30 s default). If found, the device
restarts into OPERATIONAL after a 5 s grace period. Means: you can fix
a broken router and have the device come back without re-running the
portal walkthrough.

## Troubleshooting

- **AP doesn't appear after first boot** — double-check the LED
  pattern. If it's BOOT solid for >30 s the firmware is hung pre-AP;
  power-cycle.
- **Form submit returns error** — likely an invalid character in the
  password (the form sanitizes most but not all). Try a shorter
  alphanumeric password to confirm.
- **Device restarts immediately after submit but doesn't come back** —
  the credentials were wrong. After 2 cycles of AUTH_EXPIRE, the
  firmware re-enters AP mode automatically (cred-stale flag).
- **Phone doesn't show captive sheet** — old Android or some Linux
  phones don't probe captive URLs reliably. Just browse manually to
  `https://192.168.4.1/`.

## Stub status

Add new flow steps as the AP portal evolves. Last sweep:
2026-04-29 PM after v0.4.31.
