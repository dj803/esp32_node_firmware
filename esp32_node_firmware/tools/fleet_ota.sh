#!/usr/bin/env bash
#
# DEPRECATED 2026-04-28 — use tools/dev/ota-rollout.sh instead.
#
# This script targets v0.4.10 specifically and hardcodes a stale Bravo
# UUID (`6cfe177f-...`, rotated 2026-04-27 after the NVS-wipe test in
# #50; current is `ece1ed31-...`). The 3-second mosquitto_sub windows
# also hit the LWT-offline-shadow gotcha documented in
# docs/MONITORING_PRACTICE.md "Capturing fleet snapshots — gotcha".
#
# Replaced by:
#   - tools/dev/ota-rollout.sh   — generic per-version rollout with
#                                  EXCLUDE_UUIDS support (#79 + #84)
#   - tools/dev/version-watch.sh — long-running watcher for live fw
#                                  version transitions
#   - tools/dev/release-smoke.sh — pre-tag chaos smoke (M1+M2+M4)
#
# Kept in repo for git history / audit; do not invoke. If something
# really only works in this script, file a SUGGESTED_IMPROVEMENTS entry
# and have the new tools absorb it.
#
# Original purpose: staggered fleet OTA orchestrator with canary checks.
# Order: Charlie -> Delta -> Echo. Alpha + Bravo already on v0.4.10.
BROKER=192.168.10.30
TARGET="0.4.10"
TOPIC_BASE="Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox"

declare -A DEVICES=(
    ["Alpha"]="32925666-155a-4a67-bf50-27c1ffa22b11"
    ["Bravo"]="6cfe177f-92eb-4699-a9a6-8a3603aae175"
    ["Charlie"]="2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd"
    ["Delta"]="2b89f43c-2fd8-4ed6-ac9d-fb0d8f97c282"
    ["Echo"]="2fdd4112-9255-42a8-a099-ada0075a677b"
)
ORDER=(Delta Echo)

# Resolve UUIDs from live retained status messages so we don't hardcode wrong ones.
echo "[$(date +%H:%M:%S)] resolving live UUIDs..."
declare -A UUID_BY_NAME
while IFS= read -r line; do
    t=$(echo "$line" | awk '{print $1}')
    p=$(echo "$line" | cut -d' ' -f2-)
    name=$(echo "$p" | python -c "import sys,json;d=json.loads(sys.stdin.read() or '{}');print(d.get('node_name',''))" 2>/dev/null)
    uuid=$(echo "$t" | awk -F/ '{print $(NF-1)}')
    [ -n "$name" ] && [ -n "$uuid" ] && UUID_BY_NAME[$name]=$uuid
done < <(timeout 4 mosquitto_sub -h $BROKER -t "$TOPIC_BASE/+/status" -F "%t %p" -W 3 2>/dev/null)

verify() {
    local name=$1 expected=$2
    local uuid="${UUID_BY_NAME[$name]:-${DEVICES[$name]:-}}"
    [ -z "$uuid" ] && { echo "[$(date +%H:%M:%S)] $name: UUID unknown, skip verify"; return 1; }
    local payload
    payload=$(timeout 4 mosquitto_sub -h $BROKER -t "$TOPIC_BASE/$uuid/status" -W 3 -C 1 2>/dev/null | tail -1)
    local fw br ev
    read -r fw ev br < <(echo "$payload" | python -c "
import sys,json
try:
    d=json.loads(sys.stdin.read())
    print(d.get('firmware_version',''), d.get('event',''), d.get('boot_reason',''))
except Exception:
    print('','','')
")
    echo "[$(date +%H:%M:%S)] $name: fw=$fw event=$ev boot_reason=$br"
    [[ "$br" == "task_wdt" || "$br" == "int_wdt" || "$br" == "panic" || "$br" == "brownout" ]] && return 2
    [[ "$fw" == "$expected" ]] && return 0
    return 1
}

for i in "${!ORDER[@]}"; do
    name=${ORDER[$i]}
    uuid=${UUID_BY_NAME[$name]:-${DEVICES[$name]}}
    echo "[$(date +%H:%M:%S)] step $((i+1))/${#ORDER[@]}: triggering OTA on $name ($uuid)"
    mosquitto_pub -h $BROKER -t "$TOPIC_BASE/$uuid/cmd/ota_check" -m "" -q 0
    if [ $i -lt $((${#ORDER[@]}-1)) ]; then
        echo "[$(date +%H:%M:%S)] waiting 300s for $name to download+validate before next..."
        sleep 300
        verify "$name" "$TARGET"
        rc=$?
        if [ $rc -eq 2 ]; then
            echo "[$(date +%H:%M:%S)] ABORT: $name boot reason looks like crash. Stopping chain."
            exit 1
        elif [ $rc -ne 0 ]; then
            echo "[$(date +%H:%M:%S)] WARN: $name not yet on $TARGET; continuing anyway (may still be downloading)"
        else
            echo "[$(date +%H:%M:%S)] OK: $name on $TARGET"
        fi
    else
        echo "[$(date +%H:%M:%S)] last device queued. Waiting 240s for it to settle..."
        sleep 240
        verify "$name" "$TARGET" || true
    fi
done
echo "[$(date +%H:%M:%S)] DONE"
