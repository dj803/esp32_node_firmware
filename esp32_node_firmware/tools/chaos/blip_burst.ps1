# tools/chaos/blip_burst.ps1 — M4 rapid-succession broker blips
#
# Stops mosquitto repeatedly with $UpSeconds gap between each cycle.
# Stresses the half-open connection cleanup path: AsyncTCP is mid-error-
# handler when a new disconnect arrives.
#
# Run elevated.
#
# Usage:
#   powershell -File tools\chaos\blip_burst.ps1                       # 3 × 2 s down / 5 s up
#   powershell -File tools\chaos\blip_burst.ps1 -Cycles 5 -DownSeconds 3 -UpSeconds 4

param(
    [int]$Cycles      = 3,
    [int]$DownSeconds = 2,
    [int]$UpSeconds   = 5
)

$ErrorActionPreference = 'Stop'
try {
    for ($i = 0; $i -lt $Cycles; $i++) {
        net stop mosquitto 2>&1 | Out-Null
        Start-Sleep -Seconds $DownSeconds
        net start mosquitto 2>&1 | Out-Null
        if ($i -lt ($Cycles - 1)) {
            Start-Sleep -Seconds $UpSeconds
        }
    }
    Start-Sleep -Seconds 2
    $svc = Get-Service mosquitto -ErrorAction SilentlyContinue
    if (-not $svc -or $svc.Status -ne 'Running') {
        Write-Error "mosquitto did not return to Running after burst (status=$($svc.Status))"
        exit 1
    }
    exit 0
} catch {
    Write-Error $_
    exit 1
}
