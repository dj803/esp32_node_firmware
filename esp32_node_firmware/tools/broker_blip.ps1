# broker_blip.ps1 — synthetic broker disconnect for Path C Phase 1 bench testing
#
# Stops the local mosquitto Windows service for ~5 seconds then restarts it.
# Forces every connected device to traverse the LWT-then-reconnect path that
# the BLE silent-deadlock manifests in (~70 min after a Wi-Fi/broker reconnect).
#
# Usage (elevated PowerShell):
#   # one shot:
#   powershell -File C:\path\to\broker_blip.ps1
#
#   # repeating loop, every 30 min for 24 h (48 cycles):
#   powershell -File C:\path\to\broker_blip.ps1 -LoopMinutes 30 -DurationHours 24
#
# Logs every cycle to C:\Users\drowa\operator-daily-health\broker_blip.log so we can
# correlate device boots against blip timestamps after the fact.

param(
    [int]$LoopMinutes = 0,        # 0 = one-shot. >0 = repeat every N minutes.
    [int]$DurationHours = 0,      # 0 = forever (until Ctrl-C).
    [int]$DownSeconds = 5         # how long the broker is stopped.
)

$ErrorActionPreference = 'Continue'
$logDir  = 'C:\Users\drowa\operator-daily-health'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
$logFile = Join-Path $logDir 'broker_blip.log'

function Write-BlipLog($msg) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $msg"
    Add-Content -Path $logFile -Value $line
    Write-Host $line
}

function Do-OneBlip {
    Write-BlipLog "BLIP start (down=${DownSeconds}s)"
    net stop mosquitto 2>&1 | Out-Null
    Start-Sleep -Seconds $DownSeconds
    net start mosquitto 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    $svc = Get-Service mosquitto -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq 'Running') {
        Write-BlipLog "BLIP done (mosquitto running)"
    } else {
        Write-BlipLog "BLIP FAILED (mosquitto status=$($svc.Status))"
    }
}

if ($LoopMinutes -le 0) {
    Do-OneBlip
    exit 0
}

$endAt = if ($DurationHours -gt 0) { (Get-Date).AddHours($DurationHours) } else { [DateTime]::MaxValue }
Write-BlipLog "LOOP start (every ${LoopMinutes} min until $endAt)"
while ((Get-Date) -lt $endAt) {
    Do-OneBlip
    $sleepSeconds = $LoopMinutes * 60
    Start-Sleep -Seconds $sleepSeconds
}
Write-BlipLog "LOOP end"
