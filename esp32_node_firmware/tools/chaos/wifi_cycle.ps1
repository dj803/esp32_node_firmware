# tools/chaos/wifi_cycle.ps1 — STUB
#
# W2 (AP reboot) is the realistic fleet-wide outage scenario but requires
# operator-side AP control that this repo does not yet have a hook for.
# Options to wire this up:
#   1. Tasmota smart-plug controlling the AP — call its HTTP /cm?cmnd=Power0
#      twice with a sleep between. Operator: provide the plug's IP+token.
#   2. Manual — operator power-cycles the AP, presses ENTER when ready.
#   3. ATEM-style scriptable PDU — same shape as Tasmota.
#
# Until one is wired, this script just notes the limitation. A "host
# Wi-Fi adapter cycle" (W1 from CHAOS_TESTING.md) is NOT a substitute —
# that drops the host's Wi-Fi, not the devices'.
#
# When a hook is ready, replace the body with the actual cycle and
# reference it from runner.sh as a registered scenario.

param(
    [int]$DownSeconds = 60
)

Write-Host "wifi_cycle.ps1 is a stub — no AP control configured yet."
Write-Host "See script header for the supported wiring paths."
exit 2   # exit 2 = "scenario not implemented" (distinct from runner failure)
