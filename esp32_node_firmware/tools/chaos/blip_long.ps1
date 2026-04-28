# tools/chaos/blip_long.ps1 — M2 long broker blip (30 s default)
#
# Same shape as blip_short.ps1 but with the longer outage that has
# historically exposed the cascade-class bugs (#51, #67/v0.4.16). Use
# blip_burst.ps1 for the M4 rapid-succession variant.
#
# Run elevated.
#
# Usage:
#   powershell -File tools\chaos\blip_long.ps1                  # 30 s default (M2)
#   powershell -File tools\chaos\blip_long.ps1 -DownSeconds 180 # M3 (180 s)

param(
    [int]$DownSeconds = 30
)

$ErrorActionPreference = 'Stop'
try {
    net stop mosquitto 2>&1 | Out-Null
    Start-Sleep -Seconds $DownSeconds
    net start mosquitto 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $svc = Get-Service mosquitto -ErrorAction SilentlyContinue
    if (-not $svc -or $svc.Status -ne 'Running') {
        Write-Error "mosquitto did not return to Running (status=$($svc.Status))"
        exit 1
    }
    exit 0
} catch {
    Write-Error $_
    exit 1
}
