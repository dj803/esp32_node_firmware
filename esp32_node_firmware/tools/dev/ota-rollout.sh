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
# (#100, 2026-04-29 PM) Adaptive timeout. Starts at TIMEOUT_INITIAL_S for
# the first device (and any retry — the wide ceiling is the safety net).
# After each device succeeds, subsequent devices get a tighter
# timeout = max(TIMEOUT_FLOOR_S, ADAPTIVE_FACTOR × first_device_duration).
# Rationale: the v0.4.30 rollout (2026-04-29) showed Delta + Echo finishing
# in ~60 s, Charlie in ~240 s. A fleet that's all in similar shape lands
# within the same band, so 2× the first observation is a tight-but-safe
# ceiling — fast feedback when one device hangs, no false-positive trips
# on healthy outliers. Keep TIMEOUT_INITIAL_S liberal so a slow first
# device doesn't doom the whole run; the second device onward feels the
# adaptive squeeze.
TIMEOUT_INITIAL_S=${TIMEOUT_INITIAL_S:-300}      # liberal for first device
TIMEOUT_FLOOR_S=${TIMEOUT_FLOOR_S:-90}           # never drop below this
ADAPTIVE_FACTOR=${ADAPTIVE_FACTOR:-2}            # multiplier on first observation
TIMEOUT_PER_DEVICE_S=$TIMEOUT_INITIAL_S          # current per-device deadline
SAFETY_GAP_S=15
LOG="${LOG:-./ota-rollout-$(date +%Y%m%d-%H%M%S).jsonl}"
ROLLOUT_START_S=$(date +%s)

log_event() {
    printf '{"ts":"%s","%s":%s}\n' \
        "$(date -Iseconds)" "$1" "$2" >> "$LOG"
}

log_event start "{\"target\":\"$TARGET_VERSION\",\"broker\":\"$BROKER\"}"

# ── 0. Pre-flight: broker + OTA manifest reachability (#100 #6) ──────────────
# Fail fast if either broker:1883 or the gh-pages OTA manifest URL is
# unreachable. Without this we don't notice a misconfigured manifest /
# offline broker until the FIRST device hits its TIMEOUT_INITIAL_S
# (300 s by default) — wasting 5 min before the operator can retry.
# Skip pre-flight via SKIP_PREFLIGHT=1 if needed (e.g. operator knows
# the manifest is fresh and just wants to push to a single offline-
# during-flight device).
if [ -z "${SKIP_PREFLIGHT:-}" ]; then
    echo "Pre-flight: probing broker ${BROKER}:1883..."
    if ! timeout 3 bash -c "echo > /dev/tcp/${BROKER}/1883" 2>/dev/null; then
        echo "  ✗ Broker ${BROKER}:1883 unreachable. Aborting."
        log_event preflight_fail "{\"stage\":\"broker\"}"
        exit 1
    fi
    echo "  ✓ broker reachable"

    OTA_MANIFEST_URL="${OTA_MANIFEST_URL:-https://dj803.github.io/esp32_node_firmware/ota.json}"
    echo "Pre-flight: probing OTA manifest ${OTA_MANIFEST_URL}..."
    MANIFEST_VERSION=$(timeout 8 curl -fsSL "$OTA_MANIFEST_URL" 2>/dev/null \
                        | python -c "import sys, json; d = json.load(sys.stdin); print(d['Configurations'][0]['Version'])" 2>/dev/null || echo "")
    if [ -z "$MANIFEST_VERSION" ]; then
        echo "  ✗ OTA manifest unreachable or malformed. Aborting."
        log_event preflight_fail "{\"stage\":\"manifest\"}"
        exit 1
    fi
    if [ "$MANIFEST_VERSION" != "$TARGET_VERSION" ]; then
        echo "  ✗ Manifest version (${MANIFEST_VERSION}) does not match TARGET_VERSION (${TARGET_VERSION}). Aborting."
        echo "    Either wait for CI/gh-pages to publish ${TARGET_VERSION}, OR retarget."
        log_event preflight_fail "{\"stage\":\"version_mismatch\",\"manifest\":\"$MANIFEST_VERSION\",\"target\":\"$TARGET_VERSION\"}"
        exit 1
    fi
    echo "  ✓ manifest at v${MANIFEST_VERSION}"
fi

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

# ── 1.5. Skip devices already on target version (#100 #3) ────────────────────
# Pre-flight version snapshot per UUID: subscribe to retained heartbeats
# and read the last-known firmware_version. Devices already on the target
# are removed from the rollout list — they self-OTA'd via the periodic
# 1-hour check and don't need a redundant trigger. Today's v0.4.30
# rollout would have skipped Alpha + Bravo (both self-OTA'd before the
# script reached them), saving ~480 s on Bravo's wait alone.
# Skip via SKIP_VERSIONCHECK=1 (e.g. forcing a re-OTA after a partial
# failure where the device reports the version but the validation didn't
# complete cleanly).
if [ -z "${SKIP_VERSIONCHECK:-}" ]; then
    echo "Pre-flight: checking which devices are already on v${TARGET_VERSION}..."
    declare -A _CURRENT_VERSION
    # 4-second passive sub captures retained boot announcements from every
    # subscribed UUID. -R discards retained (we want LIVE retained on
    # subscribe — but not retained-flag-set, which is the reverse). Use
    # the default behaviour without -R so we DO see retained.
    while IFS= read -r LINE; do
        [ -z "$LINE" ] && continue
        UUID_FROM_TOPIC=$(echo "$LINE" | awk -F/ '{print $(NF-1)}')
        VER=$(echo "$LINE" | python -c "
import sys, json
for ln in sys.stdin:
    parts = ln.split(' ', 1)
    if len(parts) < 2: continue
    try: d = json.loads(parts[1])
    except: continue
    print(d.get('firmware_version','?'))
    break" 2>/dev/null || echo "")
        # Only first version per UUID wins
        if [ -n "$VER" ] && [ -z "${_CURRENT_VERSION[$UUID_FROM_TOPIC]:-}" ]; then
            _CURRENT_VERSION[$UUID_FROM_TOPIC]=$VER
        fi
    done < <(timeout 4 mosquitto_sub -h "$BROKER" -t "$TOPIC_BASE/+/status" -F '%t %p' 2>/dev/null || true)

    SKIPPED=()
    REMAINING=()
    for u in "${UUIDS[@]}"; do
        cur="${_CURRENT_VERSION[$u]:-?}"
        if [ "$cur" = "$TARGET_VERSION" ]; then
            echo "  skip ${u:0:8} (already on v${cur})"
            SKIPPED+=( "$u" )
        else
            REMAINING+=( "$u" )
        fi
    done
    if [ ${#SKIPPED[@]} -gt 0 ]; then
        log_event skipped "[$(printf '"%s",' "${SKIPPED[@]}" | sed 's/,$//')]"
    fi
    UUIDS=( "${REMAINING[@]}" )
    if [ ${#UUIDS[@]} -eq 0 ]; then
        echo
        echo "=== All ${#SKIPPED[@]} discovered device(s) already on v${TARGET_VERSION} — nothing to do. ==="
        log_event complete "{\"count\":0,\"skipped\":${#SKIPPED[@]},\"total_s\":$(($(date +%s) - ROLLOUT_START_S))}"
        exit 0
    fi
    echo "  ${#UUIDS[@]} device(s) need OTA, ${#SKIPPED[@]} already current"
fi

# ── 2. Per-device rollout ────────────────────────────────────────────────────
DEVICE_DURATIONS=()  # (#100) per-device wall-clock seconds, for end-of-run summary
FIRST_OBSERVED_S=0   # adaptive-timeout reference

DEVICE_INDEX=0
TOTAL_DEVICES=${#UUIDS[@]}
for UUID in "${UUIDS[@]}"; do
    DEVICE_INDEX=$((DEVICE_INDEX + 1))
    echo
    echo "=== Rolling out to ${UUID:0:8} (${DEVICE_INDEX}/${TOTAL_DEVICES}, timeout ${TIMEOUT_PER_DEVICE_S}s) ==="
    log_event device_start "{\"uuid\":\"$UUID\",\"timeout_s\":${TIMEOUT_PER_DEVICE_S}}"
    DEVICE_START_S=$(date +%s)

    # Trigger OTA
    mosquitto_pub -h "$BROKER" -t "$TOPIC_BASE/$UUID/cmd/ota_check" -m '{}' \
        || { echo "  publish failed"; log_event publish_fail "\"$UUID\""; continue; }
    echo "  ota_check triggered, waiting up to ${TIMEOUT_PER_DEVICE_S}s for heartbeat at v${TARGET_VERSION} uptime>${HEALTHY_UPTIME_S}s..."

    # Wait for healthy heartbeat
    DEADLINE=$((DEVICE_START_S + TIMEOUT_PER_DEVICE_S))
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
            DEVICE_DURATION_S=$(( $(date +%s) - DEVICE_START_S ))
            echo "  ✓ $OUT (took ${DEVICE_DURATION_S}s)"
            DEVICE_DURATIONS+=( "${UUID:0:8}=${DEVICE_DURATION_S}s" )
            log_event device_ok "{\"uuid\":\"$UUID\",\"info\":\"$OUT\",\"duration_s\":${DEVICE_DURATION_S}}"
            SUCCESS=true

            # (#100, 2026-04-29 PM) Adaptive timeout — after the first device
            # succeeds, tighten subsequent timeouts to ADAPTIVE_FACTOR ×
            # observed duration (floored at TIMEOUT_FLOOR_S). Healthy fleet
            # = healthy first device, so 2× the first observation gives
            # tight feedback on outliers without false-positive trips.
            if [ "$FIRST_OBSERVED_S" -eq 0 ]; then
                FIRST_OBSERVED_S=$DEVICE_DURATION_S
                NEW_TIMEOUT=$(( ADAPTIVE_FACTOR * FIRST_OBSERVED_S ))
                if [ "$NEW_TIMEOUT" -lt "$TIMEOUT_FLOOR_S" ]; then
                    NEW_TIMEOUT=$TIMEOUT_FLOOR_S
                fi
                if [ "$NEW_TIMEOUT" -lt "$TIMEOUT_PER_DEVICE_S" ]; then
                    echo "  → adaptive timeout: ${TIMEOUT_PER_DEVICE_S}s → ${NEW_TIMEOUT}s for remaining devices"
                    log_event adaptive_timeout "{\"old_s\":${TIMEOUT_PER_DEVICE_S},\"new_s\":${NEW_TIMEOUT},\"first_observed_s\":${FIRST_OBSERVED_S}}"
                    TIMEOUT_PER_DEVICE_S=$NEW_TIMEOUT
                fi
            fi
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

    # (#100 #5) Skip safety gap on the LAST device — no "next device" to
    # protect from broker-side burst, and the gap delays the rollout
    # complete signal pointlessly. Save 15 s per rollout.
    if [ "$DEVICE_INDEX" -lt "$TOTAL_DEVICES" ]; then
        echo "  Safety gap ${SAFETY_GAP_S}s before next device..."
        sleep "$SAFETY_GAP_S"
    fi
done

ROLLOUT_END_S=$(date +%s)
TOTAL_S=$(( ROLLOUT_END_S - ROLLOUT_START_S ))
echo
echo "=== Rollout complete: ${#UUIDS[@]}/${#UUIDS[@]} on v${TARGET_VERSION} in ${TOTAL_S}s ==="
echo "Per-device durations: ${DEVICE_DURATIONS[*]}"
log_event complete "{\"count\":${#UUIDS[@]},\"total_s\":${TOTAL_S},\"durations\":\"${DEVICE_DURATIONS[*]}\"}"
