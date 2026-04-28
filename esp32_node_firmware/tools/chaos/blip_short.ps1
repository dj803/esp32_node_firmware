# tools/chaos/blip_short.ps1 — M1 short broker blip (5 s default)
#
# Stops mosquitto for $DownSeconds, restarts, returns. Exit code 0 on
# successful stop+start, 1 if the service did not return to Running.
# Used by tools/chaos/runner.sh as the "trigger" phase of an M1 scenario.
#
# Run elevated. The service control calls require admin.
#
# Usage:
#   powershell -File tools\chaos\blip_short.ps1                # 5 s default
#   powershell -File tools\chaos\blip_short.ps1 -DownSeconds 5

param(
    [int]$DownSeconds = 5
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
