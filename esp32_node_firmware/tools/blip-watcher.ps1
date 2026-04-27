# blip-watcher.ps1 — chaos-test trigger for synthetic Mosquitto blips.
#
# Watches C:\ProgramData\mosquitto\blip-trigger.txt every 500 ms. When the
# file appears, reads its contents (an integer N), stops the mosquitto
# service, sleeps N seconds, restarts the service, deletes the trigger
# file, and logs the cycle to blip.log.
#
# This script must run elevated because `net stop/start mosquitto` requires
# Service Control Manager rights. Launch via `Start Blip Watcher.bat` which
# self-elevates via UAC.
#
# Trigger from any non-elevated shell:
#   echo 30 > C:\ProgramData\mosquitto\blip-trigger.txt
# (writes a 30-second blip)

$ErrorActionPreference = 'Continue'
$dir  = 'C:\ProgramData\mosquitto'
$trig = Join-Path $dir 'blip-trigger.txt'
$log  = Join-Path $dir 'blip.log'

if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

Write-Host ''
Write-Host '====================================================' -ForegroundColor Cyan
Write-Host ' Mosquitto Blip Watcher armed' -ForegroundColor Green
Write-Host "  trigger : $trig" -ForegroundColor Gray
Write-Host "  log     : $log"  -ForegroundColor Gray
Write-Host '  trigger by writing an integer N (seconds) to the trigger file:' -ForegroundColor Gray
Write-Host "    echo 30 > $trig" -ForegroundColor Yellow
Write-Host '  Ctrl+C to stop.' -ForegroundColor Gray
Write-Host '====================================================' -ForegroundColor Cyan

while ($true) {
    if (Test-Path $trig) {
        $secs = (Get-Content $trig -Raw -ErrorAction SilentlyContinue).Trim()
        if (-not [int]::TryParse($secs, [ref]$null)) {
            $secs = 5
            Write-Host "$(Get-Date -Format 'HH:mm:ss') trigger had non-integer content; defaulting to 5 s" -ForegroundColor DarkYellow
        }
        Remove-Item $trig -Force -ErrorAction SilentlyContinue

        $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        Add-Content $log "$stamp BLIP $secs s start"
        Write-Host "$(Get-Date -Format 'HH:mm:ss') BLIP $secs s..." -ForegroundColor Yellow

        net stop mosquitto | Out-Null
        Start-Sleep -Seconds $secs
        net start mosquitto | Out-Null

        $stamp2 = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        Add-Content $log "$stamp2 BLIP done"
        Write-Host "$(Get-Date -Format 'HH:mm:ss') done" -ForegroundColor Green
    }
    Start-Sleep -Milliseconds 500
}
