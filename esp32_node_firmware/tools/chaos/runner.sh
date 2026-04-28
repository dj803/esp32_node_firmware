#!/usr/bin/env bash
# tools/chaos/runner.sh — chaos scenario orchestrator
#
# Phases per run:
#   1. Snapshot pre-state — fleet UUIDs, current uptime per device, current
#      firmware_version per device. Provides the baseline for "did anyone
#      reboot during this scenario".
#   2. Trigger — invoke the named scenario script (e.g. blip_short.ps1).
#   3. Observe — subscribe to +/status for $WINDOW seconds, capturing
#      every event. Specifically watching for `event=boot` (a device
#      rebooted) and `boot_reason=panic|task_wdt|int_wdt|brownout`
#      (abnormal reboot — fail).
#   4. Decide — pass if every device emitted at least one event=online
#      OR event=heartbeat after the trigger AND no abnormal boots
#      occurred. Fail otherwise.
#   5. Report — write JSON to $REPORT_DIR/chaos-<scenario>-<ts>.json,
#      print one-line summary.
#
# The runner does NOT need to be elevated. The scenario script may
# need elevation (broker_blip variants do). Run this from an elevated
# shell so the trigger phase doesn't pause for UAC.
#
# Usage:
#   tools/chaos/runner.sh M1                    # default 60 s observation window
#   tools/chaos/runner.sh M2 --window 90        # 30 s blip + 60 s observation
#   tools/chaos/runner.sh M3 --window 300       # 180 s blip + 120 s observation
#   tools/chaos/runner.sh M4 --window 90        # 3 × short blips
#
# Scenarios registered: M1 M2 M3 M4. Add more by extending the case
# below. wifi_cycle is intentionally NOT registered until the AP-control
# hook lands (see wifi_cycle.ps1 header).

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────

BROKER_HOST="${BROKER_HOST:-192.168.10.30}"
BROKER_PORT="${BROKER_PORT:-1883}"
TOPIC_ROOT="${TOPIC_ROOT:-Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox}"
REPORT_DIR="${REPORT_DIR:-$HOME/daily-health}"
DEFAULT_WINDOW=60

mkdir -p "$REPORT_DIR"

# ── Args ──────────────────────────────────────────────────────────────────────

if [[ $# -lt 1 ]]; then
    echo "usage: runner.sh <scenario> [--window N]" >&2
    echo "       scenarios: M1 M2 M3 M4" >&2
    exit 64
fi

SCENARIO="$1"; shift
WINDOW="$DEFAULT_WINDOW"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --window) WINDOW="$2"; shift 2 ;;
        *)        echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

# Resolve scenario → script + outage seconds.
case "$SCENARIO" in
    M1) TRIGGER_SCRIPT="blip_short.ps1";  TRIGGER_ARGS="-DownSeconds 5";   SCENARIO_DESC="5 s broker blip" ;;
    M2) TRIGGER_SCRIPT="blip_long.ps1";   TRIGGER_ARGS="-DownSeconds 30";  SCENARIO_DESC="30 s broker blip" ;;
    M3) TRIGGER_SCRIPT="blip_long.ps1";   TRIGGER_ARGS="-DownSeconds 180"; SCENARIO_DESC="180 s broker blip" ;;
    M4) TRIGGER_SCRIPT="blip_burst.ps1";  TRIGGER_ARGS="";                  SCENARIO_DESC="3-burst short blip" ;;
    *)  echo "unknown scenario: $SCENARIO (use M1 / M2 / M3 / M4)" >&2; exit 64 ;;
esac

TS="$(date +%Y%m%d-%H%M%S)"
REPORT="$REPORT_DIR/chaos-${SCENARIO}-${TS}.json"
EVENTS="$REPORT_DIR/chaos-${SCENARIO}-${TS}.events.txt"

echo "[runner] scenario=$SCENARIO  $SCENARIO_DESC"
echo "[runner] window=${WINDOW}s  report=$REPORT"

# ── Phase 1: pre-state snapshot (3 s of retained reads) ──────────────────────

PRE="$(timeout 3 mosquitto_sub -h "$BROKER_HOST" -p "$BROKER_PORT" \
        -t "$TOPIC_ROOT/+/status" -v 2>/dev/null || true)"

# Extract one line per UUID.
declare -A PRE_UPTIME PRE_FW PRE_NAME
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    uuid="$(echo "$line" | sed -nE 's|^.+/([0-9a-f-]{36})/status .*|\1|p')"
    [[ -z "$uuid" ]] && continue
    payload="${line#* }"
    # Skip LWT-offline retained payloads.
    if echo "$payload" | grep -q '"online":false'; then continue; fi
    fw="$(  echo "$payload" | sed -nE 's/.*"firmware_version":"([^"]+)".*/\1/p')"
    up="$(  echo "$payload" | sed -nE 's/.*"uptime_s":([0-9]+).*/\1/p')"
    name="$(echo "$payload" | sed -nE 's/.*"node_name":"([^"]+)".*/\1/p')"
    [[ -n "$fw"   ]] && PRE_FW["$uuid"]="$fw"
    [[ -n "$up"   ]] && PRE_UPTIME["$uuid"]="$up"
    [[ -n "$name" ]] && PRE_NAME["$uuid"]="$name"
done <<< "$PRE"

declare -i PRE_COUNT=${#PRE_UPTIME[@]}
echo "[runner] pre-state: $PRE_COUNT devices"
if [[ $PRE_COUNT -eq 0 ]]; then
    echo "[runner] FAIL: no fleet baseline — broker unreachable or fleet offline" >&2
    exit 1
fi

# ── Phase 2: start observer in background, then fire trigger ─────────────────

# Subscribe to ALL events under the fleet root for $WINDOW seconds.
mosquitto_sub -h "$BROKER_HOST" -p "$BROKER_PORT" \
    -t "$TOPIC_ROOT/+/status" \
    -t "$TOPIC_ROOT/+/diag/coredump" \
    -v -W "$WINDOW" > "$EVENTS" 2>/dev/null &
SUB_PID=$!
sleep 1   # let the subscription settle before firing

echo "[runner] firing trigger: $TRIGGER_SCRIPT $TRIGGER_ARGS"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TRIGGER_PATH="$SCRIPT_DIR/$TRIGGER_SCRIPT"

# Run the PowerShell trigger. The runner does not pretend to elevate;
# operator must already be in an elevated shell. If the trigger fails,
# we still drain the observer and write a report flagging trigger error.
TRIGGER_RC=0
powershell -NoProfile -ExecutionPolicy Bypass -File "$TRIGGER_PATH" $TRIGGER_ARGS \
    || TRIGGER_RC=$?

# ── Phase 3: wait for observer to drain ──────────────────────────────────────

wait "$SUB_PID" 2>/dev/null || true

# ── Phase 4: decide ──────────────────────────────────────────────────────────

declare -A POST_SEEN POST_BOOT_REASON
ABNORMAL_BOOTS=0
COREDUMPS=0
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    topic="${line%% *}"
    payload="${line#* }"
    uuid="$(echo "$topic" | sed -nE 's|^.+/([0-9a-f-]{36})/.*|\1|p')"
    [[ -z "$uuid" ]] && continue
    case "$topic" in
        */diag/coredump) COREDUMPS=$((COREDUMPS+1)); continue ;;
    esac
    event="$(echo "$payload" | sed -nE 's/.*"event":"([^"]+)".*/\1/p')"
    [[ -z "$event" ]] && continue
    POST_SEEN["$uuid"]=1
    if [[ "$event" == "boot" ]]; then
        reason="$(echo "$payload" | sed -nE 's/.*"boot_reason":"([^"]+)".*/\1/p')"
        POST_BOOT_REASON["$uuid"]="$reason"
        case "$reason" in
            panic|task_wdt|int_wdt|brownout)
                ABNORMAL_BOOTS=$((ABNORMAL_BOOTS+1))
                ;;
        esac
    fi
done < "$EVENTS"

# Pass = every pre-known device emitted at least one post-trigger event AND
# no abnormal boot reasons AND no new coredumps appeared.
MISSING=()
for uuid in "${!PRE_UPTIME[@]}"; do
    if [[ -z "${POST_SEEN[$uuid]:-}" ]]; then
        MISSING+=("${PRE_NAME[$uuid]:-$uuid}")
    fi
done

PASS=true
[[ $ABNORMAL_BOOTS -gt 0 ]] && PASS=false
[[ $COREDUMPS      -gt 0 ]] && PASS=false
[[ ${#MISSING[@]}  -gt 0 ]] && PASS=false
[[ $TRIGGER_RC     -ne 0 ]] && PASS=false

# ── Phase 5: report ──────────────────────────────────────────────────────────

{
    printf '{\n'
    printf '  "scenario": "%s",\n'         "$SCENARIO"
    printf '  "description": "%s",\n'      "$SCENARIO_DESC"
    printf '  "timestamp": "%s",\n'        "$TS"
    printf '  "window_s": %d,\n'           "$WINDOW"
    printf '  "trigger_rc": %d,\n'         "$TRIGGER_RC"
    printf '  "pre_devices": %d,\n'        "$PRE_COUNT"
    printf '  "post_seen": %d,\n'          "${#POST_SEEN[@]}"
    printf '  "abnormal_boots": %d,\n'     "$ABNORMAL_BOOTS"
    printf '  "coredumps": %d,\n'          "$COREDUMPS"
    printf '  "missing_devices": ['
    first=1
    for n in "${MISSING[@]}"; do
        [[ $first -eq 0 ]] && printf ', '
        printf '"%s"' "$n"
        first=0
    done
    printf '],\n'
    printf '  "boot_reasons": {'
    first=1
    for u in "${!POST_BOOT_REASON[@]}"; do
        [[ $first -eq 0 ]] && printf ', '
        printf '"%s": "%s"' "${PRE_NAME[$u]:-$u}" "${POST_BOOT_REASON[$u]}"
        first=0
    done
    printf '},\n'
    if $PASS; then
        printf '  "result": "PASS"\n'
    else
        printf '  "result": "FAIL"\n'
    fi
    printf '}\n'
} > "$REPORT"

if $PASS; then
    echo "[runner] $SCENARIO PASS  (window=${WINDOW}s, ${#POST_SEEN[@]}/${PRE_COUNT} devices observed, $ABNORMAL_BOOTS abnormal boots, $COREDUMPS coredumps)"
    exit 0
else
    echo "[runner] $SCENARIO FAIL  (trigger_rc=$TRIGGER_RC abnormal=$ABNORMAL_BOOTS coredumps=$COREDUMPS missing=${#MISSING[@]})" >&2
    [[ ${#MISSING[@]} -gt 0 ]] && echo "  missing: ${MISSING[*]}" >&2
    exit 1
fi
