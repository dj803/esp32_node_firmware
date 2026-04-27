#!/usr/bin/env bash
# fleet_status.sh — one-shot snapshot of every device's retained MQTT status.
# Usage: tools/fleet_status.sh
# Output: one line per device, sorted by node_name.

BROKER="${MQTT_BROKER:-192.168.10.30}"
TOPIC_BASE="Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox"

timeout 8 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F "%t %p" -W 7 2>/dev/null \
    | python -c "
import sys, json
seen = {}
for line in sys.stdin:
    line = line.strip()
    if not line.startswith('Enigma'): continue
    sp = line.split(' ', 1)
    if len(sp) < 2: continue
    t, pl = sp
    uuid = t.split('/')[-2]
    try:
        d = json.loads(pl)
        seen[uuid] = d
    except Exception:
        pass

if not seen:
    print('no retained status — broker reachable?'); sys.exit(1)

for uuid, d in sorted(seen.items(), key=lambda x: x[1].get('node_name', 'zzz')):
    n  = d.get('node_name', '') or '(unset)'
    fw = d.get('firmware_version', '?')
    ev = d.get('event', '?')
    br = d.get('boot_reason', '?')
    up = d.get('uptime_s', '?')
    hf = d.get('heap_free', '?')
    hl = d.get('heap_largest', '?')
    print(f'{n:14}  fw={fw:14}  ev={ev:10}  br={br:10}  up={up}s  heap={hf}/{hl}')
"
