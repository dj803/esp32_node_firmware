#!/usr/bin/env bash
# silent_watcher.sh — unified live alert stream.
#
# Subscribes to every device's /status topic and prints one line per event:
#   - LWT offline (silent deadlock or WiFi drop)
#   - Boot announcement with abnormal boot_reason (panic, *_wdt, brownout)
#
# Anything else is suppressed to keep the stream signal-only.
#
# Usage (foreground, Ctrl-C to stop):
#   tools/silent_watcher.sh
#
# Usage as a Monitor task (alerts stream into the Claude session):
#   pass this script as the Monitor command.
#
# EXCLUDE_UUIDS env: space-separated UUIDs to silently ignore. Useful
# when a device is intentionally off the soak (canary build, hardware
# work in progress, etc.). Mirrors the EXCLUDE_UUIDS pattern in
# tools/dev/ota-rollout.sh.
#
#   EXCLUDE_UUIDS="2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd" tools/silent_watcher.sh

BROKER="${MQTT_BROKER:-192.168.10.30}"
TOPIC_BASE="Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox"
EXCLUDE_UUIDS="${EXCLUDE_UUIDS:-}"

if [ -n "$EXCLUDE_UUIDS" ]; then
    echo "[silent_watcher] excluding: $EXCLUDE_UUIDS"
fi

EXCLUDE_UUIDS="$EXCLUDE_UUIDS" mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t %p' -R 2>/dev/null \
    | python -u -c "
import sys, json, time, os

# Track one normal boot per device so a single 'poweron' / 'software' boot
# doesn't echo a noisy line every time it's republished. Abnormal reasons
# (panic, *_wdt, brownout, other) always alert.
ABNORMAL = {'panic','task_wdt','int_wdt','other_wdt','wdt','brownout','external','sw_reset','deepsleep'}
NORMAL_QUIET = {'poweron','software'}
EXCLUDE = set(os.environ.get('EXCLUDE_UUIDS','').split())

last_state = {}     # uuid -> 'online' | 'offline' | 'unknown'

for line in sys.stdin:
    line = line.strip()
    if not line.startswith('Enigma'): continue
    sp = line.split(' ', 1)
    if len(sp) < 2: continue
    topic, payload = sp
    uuid = topic.split('/')[-2]
    if uuid in EXCLUDE: continue
    name = uuid[:8]
    try:
        d = json.loads(payload)
    except Exception:
        continue
    ev = d.get('event','')
    online = d.get('online', True)
    nm = d.get('node_name', '') or name

    # LWT offline → silent failure / disconnect
    if online is False or ev == 'offline':
        if last_state.get(uuid) != 'offline':
            print(f'[{time.strftime(\"%H:%M:%S\")}] OFFLINE: {nm:14s} (LWT — silent failure or WiFi drop)', flush=True)
            last_state[uuid] = 'offline'
        continue

    # Boot event — alert if abnormal reason
    if ev == 'boot':
        reason = d.get('boot_reason','')
        fw = d.get('firmware_version','?')
        if reason in ABNORMAL:
            print(f'[{time.strftime(\"%H:%M:%S\")}] ABNORMAL BOOT: {nm:14s} reason={reason} fw={fw}', flush=True)
        last_state[uuid] = 'online'
        continue

    # Heartbeat / other → silent unless transitioning from offline
    if last_state.get(uuid) == 'offline':
        print(f'[{time.strftime(\"%H:%M:%S\")}] RECOVERED: {nm:14s} ({ev})', flush=True)
        last_state[uuid] = 'online'
"
