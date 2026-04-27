#!/usr/bin/env bash
# rollback_to_v0_4_12.sh — emergency OTA-manifest rollback
#
# Created 2026-04-27 during the v0.4.13 panic cascade triage. Updates the
# Pages OTA manifest to point at v0.4.12 (= retag of v0.4.11, last known
# stable before #44/#61 changes) and triggers ota_check on every device.
#
# Usage:
#   bash tools/rollback_to_v0_4_12.sh
#
# Requirements: gh CLI authenticated, mosquitto_pub on PATH.

set -euo pipefail

REPO=dj803/esp32_node_firmware
TARGET_VERSION=0.4.12
TARGET_URL="https://github.com/dj803/esp32_node_firmware/releases/download/v${TARGET_VERSION}/firmware.bin"
BROKER=192.168.10.30
TOPIC_BASE='Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox'

echo "1. Checkout gh-pages branch into worktree..."
WT=$(mktemp -d)
git fetch origin gh-pages:gh-pages || true
git worktree add -B gh-pages "$WT" origin/gh-pages

cd "$WT"

echo "2. Rewrite ota.json..."
cat > ota.json <<EOF
{
  "Configurations": [
    {
      "Version": "${TARGET_VERSION}",
      "URL": "${TARGET_URL}"
    }
  ]
}
EOF

git add ota.json
git -c user.email=ops@local -c user.name=ops commit -m "EMERGENCY rollback: OTA manifest -> v${TARGET_VERSION}

v0.4.13 panic cascade observed 2026-04-27 10:42 SAST. 4/6 fleet recovered
with boot_reason=panic. Pending root-cause analysis. Rolling back to
v0.4.12 (= retagged v0.4.11 with heap-guard fix) until v0.4.13 panic is
diagnosed. See memory/v0_4_13_panic_cascade_2026_04_27.md."

echo "3. Push gh-pages..."
git push origin gh-pages

cd - >/dev/null
git worktree remove "$WT"

echo "4. Wait 30s for Pages CDN to propagate..."
sleep 30

echo "5. Verify manifest..."
curl -sS "https://dj803.github.io/esp32_node_firmware/ota.json"
echo

echo "6. Trigger ota_check on each device..."
for UUID in \
    32925666-155a-4a67-bf50-27c1ffa22b11 \
    6cfe177f-92eb-4699-a9a6-8a3603aae175 \
    2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd \
    2b89f43c-2fd8-4ed6-ac9d-fb0d8f97c282 \
    2fdd4112-9255-42a8-a099-ada0075a677b \
    c1278367-21af-478d-8a8b-0b84a4de60df ; do
    mosquitto_pub -h "$BROKER" -t "${TOPIC_BASE}/${UUID}/cmd/ota_check" -m '{}'
    echo "  triggered ${UUID:0:8}"
done

echo "Rollback initiated. Watch silent_watcher for boot reasons."
