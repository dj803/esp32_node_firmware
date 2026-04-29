#!/usr/bin/env bash
# tools/dev/pio-utf8.sh — Windows-safe PlatformIO wrapper (#95, v0.4.29)
#
# WHY:
#   PlatformIO's _safe_echo path crashes with UnicodeEncodeError when
#   stdout is the Windows cp1252 console and esptool prints non-ASCII
#   progress chars (the "█" block character used during write-flash, plus
#   "·" / "→" elsewhere). Manifests as a 23+ minute "hang" with NO
#   visible output, then a buried Python traceback once you Ctrl-C —
#   AND a stuck esptool.exe process holding COM5/COM4 that can only be
#   freed by physically unplugging + replugging the device.
#
#   Caught 2026-04-29 morning during the v0.4.27 USB-flash to Alpha. The
#   fix is to force Python's I/O encoding to UTF-8 before invoking pio,
#   which makes _safe_echo's str.encode(...).decode(...) round-trip a
#   no-op instead of a crash.
#
# USAGE:
#   ./tools/dev/pio-utf8.sh run -e esp32dev -t upload --upload-port COM4
#   ./tools/dev/pio-utf8.sh device monitor -p COM4 -b 115200
#   ./tools/dev/pio-utf8.sh test -e native -v
#
#   Anywhere a CLAUDE.md or runbook would say "pio ...", you can prefix
#   with this wrapper instead. Same arguments, same exit code.
#
# RELATED:
#   See SUGGESTED_IMPROVEMENTS_ARCHIVE.md #95 for the full traceback +
#   reproduction recipe + why the inline `PYTHONIOENCODING=utf-8 PYTHONUTF8=1
#   pio ...` form also works (this wrapper is just the canonical entrypoint).

set -e
export PYTHONIOENCODING="${PYTHONIOENCODING:-utf-8}"
export PYTHONUTF8="${PYTHONUTF8:-1}"
exec pio "$@"
