#!/usr/bin/env bash
# tools/dev/release-smoke.sh — pre-release chaos smoke wrapper
#
# Runs the documented M1 + M2 + M4 chaos sequence (see
# tools/chaos/README.md "Pre-release smoke") and exits non-zero if any
# scenario fails. Intended to be invoked from an elevated bash shell
# *before* tagging a release, after `pio test -e native -v` has passed.
#
# Why this is not a GitHub Actions job:
#   - The mosquitto broker is on the operator's LAN (192.168.10.30:1883),
#     not reachable from GitHub-hosted runners.
#   - The chaos PowerShell triggers do `net stop/start mosquitto`, which
#     needs an elevated Windows shell — not a Linux runner.
# A self-hosted Windows runner could do it, but is overkill for a
# 5-device fleet. This wrapper is the intended "CI hook" for #75.
#
# Usage:
#   tools/dev/release-smoke.sh                  # full M1 + M2 + M4 sequence
#   tools/dev/release-smoke.sh --m3             # also include M3 (cascade-class)
#   tools/dev/release-smoke.sh --quick          # M1 only (~90 s)
#
# Exit codes:
#   0 — every scenario PASSed
#   1 — one or more scenarios FAILed (operator must inspect reports)
#   2 — invocation error (missing runner, not elevated, broker unreachable)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNER="${SCRIPT_DIR}/../chaos/runner.sh"

if [[ ! -x "$RUNNER" ]]; then
  echo "release-smoke: runner not found or not executable: $RUNNER" >&2
  exit 2
fi

BROKER_HOST="${BROKER_HOST:-192.168.10.30}"
BROKER_PORT="${BROKER_PORT:-1883}"
if ! timeout 3 bash -c "echo > /dev/tcp/${BROKER_HOST}/${BROKER_PORT}" 2>/dev/null; then
  echo "release-smoke: broker ${BROKER_HOST}:${BROKER_PORT} not reachable — abort" >&2
  exit 2
fi

# Default sequence per the chaos README:
SEQUENCE=("M1:30" "M2:90" "M4:90")

case "${1:-}" in
  --m3)    SEQUENCE+=("M3:300") ;;
  --quick) SEQUENCE=("M1:30") ;;
  "")      ;;  # default
  *)       echo "release-smoke: unknown arg '$1'" >&2 ; exit 2 ;;
esac

FAIL=0
for entry in "${SEQUENCE[@]}"; do
  scenario="${entry%%:*}"
  window="${entry##*:}"
  echo "─── ${scenario} (window ${window}s) ───"
  if ! "$RUNNER" "$scenario" --window "$window"; then
    echo "release-smoke: ${scenario} FAILED"
    FAIL=$((FAIL + 1))
  fi
  echo
done

if [[ $FAIL -gt 0 ]]; then
  echo "release-smoke: ${FAIL} scenario(s) FAILED — see ~/daily-health/chaos-*.json for breakdowns" >&2
  exit 1
fi

echo "release-smoke: all ${#SEQUENCE[@]} scenario(s) PASSED — ok to tag"
exit 0
