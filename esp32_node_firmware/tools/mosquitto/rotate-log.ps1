# rotate-log.ps1 — Mosquitto log rotation (run daily via Task Scheduler, elevated)
# Renames mosquitto.log to mosquitto.log.YYYY-MM-DD, keeps last 5 generations,
# then restarts the service so it opens a fresh log file.
#
# Setup: schtasks /create /tn "MosquittoLogRotate" /tr "powershell -File C:\ProgramData\mosquitto\rotate-log.ps1" /sc daily /st 02:00 /ru SYSTEM
#
# 2026-04-28 (#83): added size-based rotation. Mosquitto on Windows
# silently fails to re-open the log file on service restart once it
# crosses ~200 MB, leaving the freeze pattern that #83 documents.
# Size-cap rotation prevents that even if the daily task is missed.

$ErrorActionPreference = 'Stop'
$logFile = 'C:\ProgramData\mosquitto\mosquitto.log'
$maxGenerations = 5
$maxSizeMB = 100

if (-not (Test-Path $logFile)) {
    Write-Host "No log file found at $logFile — nothing to rotate."
    exit 0
}

$stamp = Get-Date -Format 'yyyy-MM-dd'
$rotated = "$logFile.$stamp"
$sizeMB = [int]((Get-Item $logFile).Length / 1MB)
$rotateBySize = ($sizeMB -ge $maxSizeMB)
$alreadyRotatedToday = (Test-Path $rotated)

# If size-driven and we already rotated today, append a counter so we
# don't clobber today's earlier rotated file.
if ($rotateBySize -and $alreadyRotatedToday) {
    $counter = 1
    while (Test-Path "$rotated.$counter") { $counter++ }
    $rotated = "$rotated.$counter"
}

if ((-not $alreadyRotatedToday) -or $rotateBySize) {
    Rename-Item -Path $logFile -NewName $rotated
    Write-Host "Rotated ($sizeMB MB, sizeRotate=$rotateBySize): $logFile -> $rotated"
} else {
    Write-Host "Already rotated today and under $maxSizeMB MB ($sizeMB MB): $rotated exists. Skipping."
    exit 0
}

# Prune old generations beyond $maxGenerations
$old = Get-ChildItem "$logFile.*" | Sort-Object Name -Descending | Select-Object -Skip $maxGenerations
foreach ($f in $old) {
    Remove-Item $f.FullName -Force
    Write-Host "Pruned: $($f.Name)"
}

# Restart service so Mosquitto reopens a fresh log file
try {
    Restart-Service -Name mosquitto -Force
    Write-Host "Mosquitto service restarted."
} catch {
    Write-Warning "Service restart failed: $_"
}
