@echo off
REM Grant Mosquitto Log Read.bat — durable read-only ACL grant for the
REM current user on C:\ProgramData\mosquitto\mosquitto.log
REM
REM WHY: The mosquitto service runs as SYSTEM and creates its log file
REM with SYSTEM/Admin-only ACLs. Non-elevated user processes
REM (including Claude Code's Bash and PowerShell tools) get
REM "Permission denied" when reading. Run this .bat once elevated
REM and the user gets durable read-only access — same pattern as
REM the blip-watcher's elevated launch.
REM
REM USAGE:
REM   Right-click → Run as Administrator
REM   (or double-click; UAC will prompt and self-elevate via the
REM   self-check below)
REM
REM Self-elevation pattern mirrors "Start Blip Watcher.bat".

REM --- Self-elevation check ---
net session >nul 2>&1
if %errorLevel% NEQ 0 (
    echo Requesting admin elevation...
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)

REM --- Grant read on the log file ---
set LOGPATH=C:\ProgramData\mosquitto\mosquitto.log

if not exist "%LOGPATH%" (
    echo ERROR: %LOGPATH% does not exist.
    echo Mosquitto may not be running, or file logging may not be enabled.
    pause
    exit /b 1
)

echo Granting read access on %LOGPATH% to %USERNAME%...
icacls "%LOGPATH%" /grant %USERNAME%:(R)
if %errorLevel% EQU 0 (
    echo.
    echo SUCCESS - %USERNAME% now has read access to mosquitto.log.
    echo Future non-elevated agent sessions can tail the log directly.
) else (
    echo.
    echo FAILED with errorlevel %errorLevel%.
)
echo.
pause
