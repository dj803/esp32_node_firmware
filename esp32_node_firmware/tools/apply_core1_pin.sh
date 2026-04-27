#!/usr/bin/env bash
# apply_core1_pin.sh — Path C Phase 1 mitigation #2 toggle.
#
# Pins the BTDM controller and Bluedroid host to Core 1 instead of Core 0,
# moving them off the same core as async_tcp / lwIP / Wi-Fi.
#
# Editing sdkconfig.defaults is invasive — it affects ALL envs. After flashing
# the bench rig, revert with `git checkout -- esp32_node_firmware/sdkconfig.defaults`.
#
# Caveat: BTDM_CTRL_HLI=y means the controller's high-level interrupts are
# hardware-pinned to Core 0 regardless of task affinity. This patch moves the
# task scheduling, not the ISR. If the deadlock is in ISR context this won't help.

set -euo pipefail
SDK="esp32_node_firmware/sdkconfig.defaults"

if [ ! -f "$SDK" ]; then
  echo "Run from repo root (need $SDK)" >&2
  exit 1
fi

echo "Before:"
grep -E '^(CONFIG_BTDM_CTRL_PINNED_TO_CORE|CONFIG_BT_BLUEDROID_PINNED_TO_CORE)' "$SDK" | head -6

# Swap _0 → _1 for both the boolean and integer settings.
sed -i \
  -e 's/^CONFIG_BTDM_CTRL_PINNED_TO_CORE_0=y$/# CONFIG_BTDM_CTRL_PINNED_TO_CORE_0 is not set/' \
  -e 's/^# CONFIG_BTDM_CTRL_PINNED_TO_CORE_1 is not set$/CONFIG_BTDM_CTRL_PINNED_TO_CORE_1=y/' \
  -e 's/^CONFIG_BTDM_CTRL_PINNED_TO_CORE=0$/CONFIG_BTDM_CTRL_PINNED_TO_CORE=1/' \
  -e 's/^CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0=y$/# CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0 is not set/' \
  -e 's/^# CONFIG_BT_BLUEDROID_PINNED_TO_CORE_1 is not set$/CONFIG_BT_BLUEDROID_PINNED_TO_CORE_1=y/' \
  -e 's/^CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0$/CONFIG_BT_BLUEDROID_PINNED_TO_CORE=1/' \
  "$SDK"

echo
echo "After:"
grep -E '^(CONFIG_BTDM_CTRL_PINNED_TO_CORE|CONFIG_BT_BLUEDROID_PINNED_TO_CORE|# CONFIG_BTDM_CTRL_PINNED_TO_CORE|# CONFIG_BT_BLUEDROID_PINNED_TO_CORE)' "$SDK" | head -6

echo
echo "Now rebuild the bench env:"
echo "  PATH=/c/mingw64/bin:\$PATH pio run -e esp32dev_ble_bench"
echo
echo "After flashing, revert with:"
echo "  git checkout -- $SDK"
