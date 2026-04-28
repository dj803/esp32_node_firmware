#!/usr/bin/env bash
# ota-monitor.sh — passive watcher for a fleet OTA rollout (#84 part B).
#
# Subscribes to all fleet /status topics and prints one line per device
# as each picks up the target firmware version. Exits when every
# device-of-interest matches the target, OR after a timeout, OR on first
# abnormal boot reason. Does NOT trigger any OTAs — pair with
# `mosquitto_pub cmd/ota_check` or `tools/dev/ota-rollout.sh`.
#
# Difference from ota-rollout.sh: rollout drives the OTA (one device at a
# time, ack-driven). ota-monitor is purely an observer — useful when OTAs
# were already triggered (cmd/ota_check broadcast, periodic timer) or when
# verifying that fleet picked up an OTA-manifest change.
#
# Usage:
#   tools/dev/ota-monitor.sh 0.4.22                    # auto-discover fleet
#   tools/dev/ota-monitor.sh 0.4.22 30925666 ece1ed31  # specific UUIDs (prefix-match OK)
#   FLEET_UUIDS="uuid1 uuid2" tools/dev/ota-monitor.sh 0.4.22
#
# Output is line-per-event so it tails cleanly:
#   [00:23] alpha    0.4.22 (uptime=4s)              ← MATCH
#   [00:27] bravo    0.4.22 (uptime=2s)              ← MATCH
#   [00:35] charlie  0.4.20.0 (uptime=37000s)        ← still old (canary?)
#   [03:05] DONE     5/6 matched, 1 stuck
#
# Exit codes:
#   0 — all devices-of-interest match target
#   1 — timeout reached with stragglers
#   2 — abnormal boot reason observed during the window
#   3 — usage error / no fleet discovered

set -euo pipefail

TARGET="${1:?usage: ota-monitor.sh <target_version> [uuid1 uuid2 ...]}"
shift || true

BROKER="${MQTT_BROKER:-192.168.10.30}"
TOPIC_BASE='Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox'
TIMEOUT_S="${OTA_MONITOR_TIMEOUT:-600}"   # 10 min default; longer for slow OTAs
ABORT_ON_PANIC="${OTA_MONITOR_ABORT_ON_PANIC:-1}"

# ── 1. Discover or accept fleet UUIDs ────────────────────────────────────────
if [ -n "${FLEET_UUIDS:-}" ]; then
    UUIDS=( $FLEET_UUIDS )
elif [ "$#" -gt 0 ]; then
    UUIDS=( "$@" )
else
    echo "Discovering fleet (10 s passive subscribe)..." >&2
    UUIDS=( $(timeout 10 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t' 2>/dev/null \
              | awk -F/ '{print $(NF-1)}' | sort -u) )
fi

if [ "${#UUIDS[@]}" -eq 0 ]; then
    echo "ERROR: no fleet UUIDs (passive discovery returned empty + no -e arg)" >&2
    exit 3
fi

echo "Watching ${#UUIDS[@]} device(s) for firmware_version=$TARGET" >&2
echo "  broker=$BROKER  timeout=${TIMEOUT_S}s  abort_on_panic=$ABORT_ON_PANIC" >&2
for u in "${UUIDS[@]}"; do echo "  - $u" >&2; done

# ── 2. Build a fast lookup table + tracker ───────────────────────────────────
declare -A INTERESTED MATCHED
for u in "${UUIDS[@]}"; do INTERESTED[$u]=1; done

START_EPOCH="$(date +%s)"
TS() { printf "%02d:%02d" $(( ($(date +%s) - START_EPOCH) / 60 )) $(( ($(date +%s) - START_EPOCH) % 60 )); }

# ── 3. Subscribe + filter + report ───────────────────────────────────────────
# We tee stdin from mosquitto_sub into the loop; both mosquitto_sub and the
# loop run for at most TIMEOUT_S before we kill the subscriber.
( exec timeout "$TIMEOUT_S" mosquitto_sub -h "$BROKER" \
    -t "$TOPIC_BASE/+/status" -F '%t %p' 2>/dev/null ) | \
while IFS= read -r line; do
    # parse: <topic> <json-payload>
    topic="${line%% *}"
    payload="${line#* }"
    uuid="${topic%/status}"
    uuid="${uuid##*/}"

    [ -n "${INTERESTED[$uuid]:-}" ] || continue

    name="$(printf '%s' "$payload" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('node_name',''))" 2>/dev/null || true)"
    ver="$(printf '%s'  "$payload" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('firmware_version',''))" 2>/dev/null || true)"
    upt="$(printf '%s'  "$payload" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('uptime_s',''))"     2>/dev/null || true)"
    evt="$(printf '%s'  "$payload" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('event',''))"        2>/dev/null || true)"
    br="$(printf  '%s'  "$payload" | python -c "import json,sys; print(json.loads(sys.stdin.read()).get('boot_reason',''))" 2>/dev/null || true)"

    [ -z "$ver" ] && continue   # offline LWT or non-status message

    short="${name:-${uuid:0:8}}"

    if [ "$ver" = "$TARGET" ] && [ -z "${MATCHED[$uuid]:-}" ]; then
        MATCHED[$uuid]=1
        echo "[$(TS)] $short  $ver (uptime=${upt}s)              ← MATCH"
    elif [ "$evt" = "boot" ] && [ -n "$br" ] && [[ "$br" =~ ^(panic|task_wdt|int_wdt|other_wdt|brownout)$ ]]; then
        echo "[$(TS)] $short  $ver boot_reason=$br              ← ABNORMAL"
        if [ "$ABORT_ON_PANIC" = "1" ]; then
            echo "[$(TS)] DONE     ABORT on abnormal boot — operator action required"
            exit 2
        fi
    fi

    # Done? Compare matched count vs interested count.
    if [ "${#MATCHED[@]}" -ge "${#UUIDS[@]}" ]; then
        echo "[$(TS)] DONE     ${#MATCHED[@]}/${#UUIDS[@]} matched"
        exit 0
    fi
done

# Got here only if the timeout hit or sub died. Summarise stragglers.
matched_count=${#MATCHED[@]:-0}
stuck=$(( ${#UUIDS[@]} - matched_count ))
echo "[$(TS)] DONE     ${matched_count}/${#UUIDS[@]} matched, $stuck stuck"
[ "$stuck" -eq 0 ] && exit 0 || exit 1
