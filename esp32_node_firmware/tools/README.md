# tools/

Operational scripts that pair with the firmware. Promoted from session-only `.dummy/` artefacts so they're discoverable and committable.

## Scripts

### `flash_dev.sh [COM-port]`
USB-flash the locally-built firmware to the given COM port (default `COM5`). Wraps `esptool.py write-flash` with the right offsets, baud, and the `PYTHONIOENCODING=utf-8` workaround for Windows' default console encoding.

```bash
pio run -e esp32dev          # build first
tools/flash_dev.sh           # flash COM5
tools/flash_dev.sh COM4      # flash COM4
```

Faster than `pio run -t upload` because it skips the rebuild check + handles relative paths consistently from any cwd inside the repo.

### `fleet_status.sh`
One-shot snapshot of every device's retained MQTT status, sorted by node_name. Used to be a 30-line hand-typed JSON-decoder block — now a one-liner.

```bash
tools/fleet_status.sh
```

Output format: `name  fw=...  ev=...  br=...  up=...s  heap=free/largest`. The `heap` columns populate once devices are on v0.4.11+ (per #53).

### `fleet_ota.sh`
Staggered fleet OTA orchestrator with canary cancel-on-failure. Triggers `cmd/ota_check` on each device in sorted order with a 5-min stagger; aborts the chain if any previously-triggered device logs an abnormal reboot in `boot_history`.

Edit the `ORDER=(...)` array near the top of the script to control the sequence (e.g. exclude Charlie if it's currently flaky).

```bash
tools/fleet_ota.sh
```

Note: the fleet OTA button in the Node-RED Device Status tab does the same thing using `boot_history` from flow context. This script is the same-network shell-side equivalent for headless / pre-Node-RED diagnostics.

## node_red/

Node-RED admin-API patch scripts. Each script idempotently fetches the current flows, modifies one or more nodes, and re-deploys via `Node-RED-Deployment-Type: nodes` (so MQTT subscriptions don't churn).

Run from the repo root or from `tools/node_red/` directly:

```bash
python tools/node_red/patch_stagger.py        # 5-min OTA + restart stagger (#45)
python tools/node_red/patch_canary.py         # canary cancel-on-failure (#35 partial)
python tools/node_red/patch_boot_reason.py    # descriptive Reason column in boot_history table
```

Each script is self-contained and includes the node IDs + replacement `func` source inline. Run them again any time the dashboard's behavior drifts (e.g. after a Node-RED Projects pull from a different branch).

## Conventions

- Scripts read MQTT_BROKER from env var, default `192.168.10.30`. Override per-call: `MQTT_BROKER=192.168.10.31 tools/fleet_status.sh`.
- Scripts assume PlatformIO Python venv at `~/.platformio/penv/Scripts/python.exe` (Windows default install).
- All scripts are idempotent (safe to re-run).

## Adding new tools

1. Drop the script in `tools/` (or `tools/node_red/` for admin-API patches).
2. Add an entry to this README with usage + purpose.
3. `chmod +x` if it's a shell script.
4. Commit alongside whatever firmware/flow change motivated it.

The bar for promoting a script from session scratchpad into `tools/` is "would a future session benefit from running this without rediscovering the API". If a script is a one-shot fix for a one-time event, leave it in `.dummy/` (gitignored) and let it die.
