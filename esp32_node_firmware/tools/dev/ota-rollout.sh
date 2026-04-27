#!/usr/bin/env bash
# ota-rollout.sh — ack-driven OTA rollout (#68).
#
# Triggers ota_check on each fleet device, waits for the device to publish a
# heartbeat on the target version with uptime > 30 s (proves the OTA succeeded
# AND the new build is healthy), then moves to the next device. PAUSE on any
# abnormal boot reason during the rollout so the operator can decide whether
# to retry, roll back, or escalate.
#
# Usage:
#   tools/dev/ota-rollout.sh 0.4.16
#
# Discovers fleet UUIDs from a 10 s passive subscription so you don't need to
# hardcode them. Override via FLEET_UUIDS env var (space-separated):
#   FLEET_UUIDS="uuid1 uuid2" tools/dev/ota-rollout.sh 0.4.16

set -euo pipefail

TARGET_VERSION="${1:?usage: ota-rollout.sh <target_version>}"
BROKER="${MQTT_BROKER:-192.168.10.30}"
TOPIC_BASE='Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox'
HEALTHY_UPTIME_S=30
TIMEOUT_PER_DEVICE_S=180
SAFETY_GAP_S=15
LOG="${LOG:-./ota-rollout-$(date +%Y%m%d-%H%M%S).jsonl}"

log_event() {
    printf '{"ts":"%s","%s":%s}\n' \
        "$(date -Iseconds)" "$1" "$2" >> "$LOG"
}

log_event start "{\"target\":\"$TARGET_VERSION\",\"broker\":\"$BROKER\"}"

# ── 1. Discover fleet UUIDs ──────────────────────────────────────────────────
if [ -n "${FLEET_UUIDS:-}" ]; then
    UUIDS=( $FLEET_UUIDS )
    echo "Using FLEET_UUIDS env: ${UUIDS[*]}"
else
    echo "Discovering fleet (10 s passive subscribe)..."
    UUIDS=( $(timeout 10 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t' \
              | awk -F/ '{print $(NF-1)}' | sort -u) )
fi

if [ ${#UUIDS[@]} -eq 0 ]; then
    echo "No fleet devices found. Aborting."
    log_event abort '"no_devices"'
    exit 1
fi
echo "Fleet: ${#UUIDS[@]} device(s)"
log_event fleet "[$(printf '"%s",' "${UUIDS[@]}" | sed 's/,$//')]"

# ── 2. Per-device rollout ────────────────────────────────────────────────────
for UUID in "${UUIDS[@]}"; do
    echo
    echo "=== Rolling out to ${UUID:0:8} ==="
    log_event device_start "\"$UUID\""

    # Trigger OTA
    mosquitto_pub -h "$BROKER" -t "$TOPIC_BASE/$UUID/cmd/ota_check" -m '{}' \
        || { echo "  publish failed"; log_event publish_fail "\"$UUID\""; continue; }
    echo "  ota_check triggered, waiting up to ${TIMEOUT_PER_DEVICE_S}s for heartbeat at v${TARGET_VERSION} uptime>${HEALTHY_UPTIME_S}s..."

    # Wait for healthy heartbeat
    DEADLINE=$(($(date +%s) + TIMEOUT_PER_DEVICE_S))
    SUCCESS=false

    while [ "$(date +%s)" -lt "$DEADLINE" ]; do
        # Single-shot subscribe with -W 5 timeout
        OUT=$(timeout 6 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/$UUID/status" -W 5 -F '%p' 2>/dev/null \
              | python -c "
import sys, json
for line in sys.stdin:
    line=line.strip()
    if not line.startswith('{'): continue
    try: d=json.loads(line)
    except: continue
    v=d.get('firmware_version','?')
    ev=d.get('event','?')
    up=int(d.get('uptime_s',0) or 0)
    br=d.get('boot_reason','-')
    if br in ('panic','task_wdt','int_wdt','other_wdt','brownout'):
        print(f'ABNORMAL {br} v{v}', flush=True); sys.exit(0)
    if ev=='heartbeat' and v=='$TARGET_VERSION' and up>=$HEALTHY_UPTIME_S:
        print(f'HEALTHY upt={up}', flush=True); sys.exit(0)
" 2>/dev/null)

        if echo "$OUT" | grep -q '^HEALTHY'; then
            echo "  ✓ $OUT"
            log_event device_ok "{\"uuid\":\"$UUID\",\"info\":\"$OUT\"}"
            SUCCESS=true
            break
        elif echo "$OUT" | grep -q '^ABNORMAL'; then
            echo "  ✗ $OUT"
            log_event device_abnormal "{\"uuid\":\"$UUID\",\"info\":\"$OUT\"}"
            break
        fi
        sleep 5
    done

    if ! $SUCCESS; then
        echo "  ✗ TIMEOUT or ABNORMAL — pausing rollout."
        log_event pause "\"$UUID\""
        echo "Operator action required:"
        echo "  - Investigate ${UUID:0:8} state"
        echo "  - Resume by re-running with FLEET_UUIDS='<remaining_uuids>'"
        exit 2
    fi

    echo "  Safety gap ${SAFETY_GAP_S}s before next device..."
    sleep "$SAFETY_GAP_S"
done

echo
echo "=== Rollout complete: ${#UUIDS[@]}/${#UUIDS[@]} on v${TARGET_VERSION} ==="
log_event complete "{\"count\":${#UUIDS[@]}}"
