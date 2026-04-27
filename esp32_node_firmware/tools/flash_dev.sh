#!/usr/bin/env bash
# flash_dev.sh — bypass `pio run -t upload` Unicode/path quirks on Windows.
#
# Usage:
#   tools/flash_dev.sh [COMx]        # default COM5
#   tools/flash_dev.sh COM4
#
# Requires: PlatformIO Python venv (which ships esptool.py). Resolves bin paths
# relative to the firmware repo so it works from any cwd inside the repo.

set -eu

PORT="${1:-COM5}"
BAUD="${BAUD:-460800}"

# Resolve repo root (parent of tools/) regardless of cwd
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$FW_DIR/.pio/build/esp32dev"

# PlatformIO Python venv lives in user home — same path on every dev box
PIO_PY="$HOME/.platformio/penv/Scripts/python.exe"
[ ! -x "$PIO_PY" ] && PIO_PY="/c/Users/drowa/.platformio/penv/Scripts/python.exe"

BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
[ ! -f "$BOOT_APP0" ] && BOOT_APP0="/c/Users/drowa/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# Sanity: bin files present
for f in "$BUILD_DIR/bootloader.bin" "$BUILD_DIR/partitions.bin" "$BUILD_DIR/firmware.bin" "$BOOT_APP0"; do
    if [ ! -f "$f" ]; then
        echo "[flash_dev] missing: $f"
        echo "[flash_dev] run \`pio run -e esp32dev\` first."
        exit 1
    fi
done

echo "[flash_dev] port=$PORT baud=$BAUD"
echo "[flash_dev] firmware=$BUILD_DIR/firmware.bin"
PYTHONIOENCODING=utf-8 "$PIO_PY" -m esptool \
    --chip esp32 --port "$PORT" --baud "$BAUD" write-flash \
    0x1000  "$BUILD_DIR/bootloader.bin" \
    0x8000  "$BUILD_DIR/partitions.bin" \
    0xe000  "$BOOT_APP0" \
    0x10000 "$BUILD_DIR/firmware.bin"
