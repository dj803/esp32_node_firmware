# Fleet operations — daily-driver one-liners

Most-used commands from the operator's elevated PowerShell or any bash.

## Fleet snapshot — who's alive, what version

```bash
tools/fleet_status.sh
```

Subscribes for ~75 s, parses retained boot announcements, prints
per-device firmware_version + uptime + last boot_reason. Use as the
first thing every morning before drilling deeper.

## Daily health check

```bash
/operator-daily-health    # Claude slash command
# OR direct invocation:
python C:/Users/drowa/tools/daily_health_check.py
```

Compares today's snapshot against yesterday's for regressions. Reports
land in `C:\Users\drowa\operator-daily-health\`. Exit codes: `0=green`, `1=yellow`, `2=red`.

## Live mosquitto stream

```bash
mosquitto_sub -h 192.168.10.30 -t '#' -v
```

All traffic, all topics. Filter to a single device:

```bash
mosquitto_sub -h 192.168.10.30 -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/#' -v
```

## OTA-rollout

```bash
tools/dev/ota-rollout.sh <target_version>
```

Phased parallel rollout (v0.4.31+). Pre-flight probes broker + manifest,
skips devices already on target, then waves of 1 → 2 → 3 → all-remaining
with adaptive timeout.

Override wave pattern:

```bash
WAVE_SIZES="1 6" tools/dev/ota-rollout.sh 0.4.31    # canary, then everything-else
WAVE_SIZES="1"   tools/dev/ota-rollout.sh 0.4.31    # strict sequential
WAVE_SIZES="6"   tools/dev/ota-rollout.sh 0.4.31    # all parallel (no canary)
```

Skip specific devices (e.g. canary on a different version):

```bash
EXCLUDE_UUIDS="<uuid>" tools/dev/ota-rollout.sh 0.4.31
```

Force re-OTA on devices already showing target version (rare — only
when a partial-failure left the version reported correctly but the
validation didn't complete):

```bash
SKIP_VERSIONCHECK=1 tools/dev/ota-rollout.sh 0.4.31
```

Skip pre-flight (if manifest hasn't propagated to gh-pages yet):

```bash
SKIP_PREFLIGHT=1 tools/dev/ota-rollout.sh 0.4.31
```

## Single-device OTA poll

```bash
mosquitto_pub -h 192.168.10.30 -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/ota_check' -m '{}'
```

Then verify via:

```bash
tools/dev/ota-monitor.sh <target_version>
```

(monitors all devices for target heartbeat with uptime > 30 s; exits
when all match or after timeout).

## Trigger a synthetic broker blip (chaos / cascade-window-guard validation)

The blip-watcher must be running in the operator's elevated PS first
(`tools/Start Blip Watcher.bat`). Then from any non-elevated shell:

```bash
echo 5  > /c/ProgramData/mosquitto/blip-trigger.txt   # M1: 5 s blip
echo 30 > /c/ProgramData/mosquitto/blip-trigger.txt   # M2: 30 s blip
echo 180 > /c/ProgramData/mosquitto/blip-trigger.txt  # M3: 180 s cascade-class blip
```

Cycle is logged to `C:\ProgramData\mosquitto\blip.log`.

## Read mosquitto broker log

```bash
Get-Content C:\ProgramData\mosquitto\mosquitto.log -Tail 60
```

Requires either elevated PS, or one-time ACL grant via
`tools/Grant Mosquitto Log Read.bat` (run as admin once — durable).

## Drain a stale retained payload

Empty payload + retain flag clears the topic:

```bash
mosquitto_pub -h 192.168.10.30 -r -t '<full_topic>' -m ''
```

## Pre-release smoke test

```bash
tools/dev/release-smoke.sh           # M1 + M2 + M4 chaos sequence (~3 min)
tools/dev/release-smoke.sh --m3      # also include M3 (cascade-class, 5 min)
tools/dev/release-smoke.sh --quick   # M1 only (~90 s)
```

Requires elevated shell (chaos triggers stop/start mosquitto service).

## Bench-flash a device

```bash
# 1. Verify which COM port is which device — see "COM port assignments"
#    in CLAUDE.md.
Get-PnpDevice -Class Ports -PresentOnly | Select-Object Name,Status
# 2. Flash via the UTF-8 wrapper (avoids the cp1252 hang from #95):
tools/dev/pio-utf8.sh run -e esp32dev -t upload --upload-port COM4
# 3. Verify-after-action — boot announcement within 60 s:
mosquitto_sub -h 192.168.10.30 -t '<root>/<uuid>/status' -W 60
```

## Stub status

Add new daily-driver one-liners as patterns settle. Last sweep:
2026-04-29 PM after v0.4.31.
