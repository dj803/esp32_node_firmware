#!/usr/bin/env bash
# version-watch.sh — print one line per (device, version) transition.
#
# Long-running mosquitto_sub on +/status. Filters down to first event per
# (device, firmware_version) pair so the output is signal-only — perfect for
# leaving in a pane during dev work or piping to a Monitor task.
#
# Usage (foreground, Ctrl+C to stop):
#   tools/dev/version-watch.sh
#
# Usage as a Monitor task (every transition becomes a notification):
#   pass this script as the Monitor command.

BROKER="${MQTT_BROKER:-192.168.10.30}"
TOPIC_BASE="Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox"

mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t %p' 2>/dev/null \
    | python -u -c "
import sys, json, time

# Suppress repeats of the same (uuid, version, ev_class) pair.
# ev_class collapses heartbeat -> 'live' so we get ONE 'live' line per
# (device, version) transition, not per heartbeat.
seen = set()
for line in sys.stdin:
    line = line.strip()
    if not line.startswith('Enigma'): continue
    sp = line.split(' ', 1)
    if len(sp) < 2: continue
    topic, payload = sp
    uuid = topic.split('/')[-2]
    name = uuid[:8]
    try:
        d = json.loads(payload)
    except Exception:
        continue
    ev = d.get('event','')
    v  = d.get('firmware_version','?')
    nm = d.get('node_name','') or name
    up = d.get('uptime_s','?')
    br = d.get('boot_reason','-')

    # Skip retained 'offline' (LWT) — those are noise on subscribe
    if ev == 'offline':
        continue
    # Collapse heartbeat to 'live'
    ev_class = 'live' if ev == 'heartbeat' else ev
    key = (uuid, v, ev_class)
    if key in seen:
        continue
    seen.add(key)

    # Format: time | name | version | event | uptime | boot_reason
    print(f'[{time.strftime(\"%H:%M:%S\")}] {nm:14s} v{v:14s} {ev_class:10s} upt={up}s br={br}', flush=True)
"
