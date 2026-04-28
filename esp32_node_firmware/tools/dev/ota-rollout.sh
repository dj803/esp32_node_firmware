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
#
# Skip specific devices via EXCLUDE_UUIDS (mirror semantic — useful for
# canary builds, devices under investigation, etc.):
#   EXCLUDE_UUIDS="2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd" tools/dev/ota-rollout.sh 0.4.24
#
# EXCLUDE_UUIDS applies on top of either FLEET_UUIDS or auto-discovered
# fleet — it's always a subtractive filter.

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
    # (Bug A fix, 2026-04-28) `timeout 10 mosquitto_sub` exits 124 on the
    # 10-second timer. Under `set -euo pipefail` the 124 propagates through
    # the pipe and `set -e` aborts the parent script — so the if-empty
    # guard below never runs, the script just exits silently with no log.
    # Mask the timeout's non-zero exit with `|| true` so we keep whatever
    # output was captured before the timer fired. The if-empty guard is
    # still authoritative for the "no broker / no fleet" case.
    UUIDS=( $(timeout 10 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t' \
              | awk -F/ '{print $(NF-1)}' | sort -u || true) )
fi

# (Q6 follow-up, 2026-04-28) EXCLUDE_UUIDS env lets the operator skip
# specific devices that should not receive the OTA — typically a canary
# build (e.g. Charlie on v0.4.20.0 with OTA_DISABLE) or a device under
# active investigation. Space-separated UUIDs, mirror semantic of
# FLEET_UUIDS. Without this, the rollout fires cmd/ota_check on the
# excluded device anyway (which is benign for OTA_DISABLE devices) but
# then spins for the full TIMEOUT_PER_DEVICE_S waiting for a
# target-version heartbeat that will never arrive.
if [ -n "${EXCLUDE_UUIDS:-}" ]; then
    declare -A _EXCLUDE_MAP
    for u in $EXCLUDE_UUIDS; do _EXCLUDE_MAP[$u]=1; done
    FILTERED=()
    for u in "${UUIDS[@]}"; do
        if [ -n "${_EXCLUDE_MAP[$u]:-}" ]; then
            echo "  excluding ${u:0:8} (in EXCLUDE_UUIDS)"
        else
            FILTERED+=( "$u" )
        fi
    done
    UUIDS=( "${FILTERED[@]}" )
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
        # (Bug B fix, 2026-04-28) `-R` suppresses retained messages so the
        # loop only sees LIVE publishes — i.e. status payloads emitted AFTER
        # we triggered cmd/ota_check above. Without this, a stale retained
        # boot announcement from before the OTA (e.g. a WDT-class boot from
        # an earlier outage) tripped the abnormal-boot guard and bailed the
        # whole rollout, even though the device was already healthy and the
        # OTA fired cleanly. The retained boot lingers on the broker forever
        # until overwritten — `-R` lets us ignore it.
        # `|| true` masks the timeout-no-message exit code (mosquitto_sub
        # exits non-zero on -W timeout) so set -e doesn't abort the loop.
        OUT=$(timeout 6 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/$UUID/status" -R -W 5 -F '%p' 2>/dev/null \
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
" 2>/dev/null || true)

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
