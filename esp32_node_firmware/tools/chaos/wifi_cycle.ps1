# tools/chaos/wifi_cycle.ps1 — STUB
#
# W2 (AP reboot) is the realistic fleet-wide outage scenario but requires
# operator-side AP control that this repo does not yet have a hook for.
#
# PLANNED HOOK (operator confirmed 2026-04-28): once v0.5.0 relay hardware
# ships, a dedicated ESP32 fitted with the BDD 2CH relay board will
# control power to other bench devices (and to the AP). At that point this
# script becomes a thin MQTT publisher:
#
#     mosquitto_pub -h <broker> -t '<...>/<relay-ctrl>/cmd/relay' \
#                   -m '{"ch":1,"state":false}'
#     Start-Sleep -Seconds $DownSeconds
#     mosquitto_pub -h <broker> -t '<...>/<relay-ctrl>/cmd/relay' \
#                   -m '{"ch":1,"state":true}'
#
# Same pattern extends to per-device power cycling (B-tier chaos: kill
# one device mid-OTA, kill the broker host, etc.) — each gets its own
# relay channel mapping baked into the runner config.
#
# Until v0.5.0 ships, this script just notes the limitation. A "host
# Wi-Fi adapter cycle" (W1 from CHAOS_TESTING.md) is NOT a substitute —
# that drops the host's Wi-Fi, not the devices'.

param(
    [int]$DownSeconds = 60
)

Write-Host "wifi_cycle.ps1 is a stub — no AP control configured yet."
Write-Host "See script header for the supported wiring paths."
exit 2   # exit 2 = "scenario not implemented" (distinct from runner failure)
