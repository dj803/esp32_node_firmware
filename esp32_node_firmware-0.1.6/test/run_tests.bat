@echo off
:: =============================================================================
:: run_tests.bat  —  Build and run host-side firmware unit tests (Windows)
::
:: Requires Visual Studio Build Tools (cl.exe on PATH via vcvars64.bat), OR
:: run this script from a "Developer Command Prompt for VS".
::
:: Usage:
::   cd esp32_node_firmware-0.1.6\test
::   run_tests.bat
::
:: Exit code 0 = all tests passed; non-zero = build failed or tests failed.
:: =============================================================================

setlocal

:: Locate cl.exe — try the Developer Command Prompt's PATH first, then a known path
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    set "CL_PATH=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64"
    if exist "%CL_PATH%\cl.exe" (
        set "PATH=%CL_PATH%;%PATH%"
    ) else (
        echo ERROR: cl.exe not found. Open a "Developer Command Prompt for VS" and re-run.
        exit /b 1
    )
)

:: Paths relative to this script's location (test\)
set "REPO=%~dp0.."
set "INC=%REPO%\esp32_firmware\esp32_firmware"
set "SRC=%~dp0test_all.cpp"
set "OUT=%~dp0run_tests.exe"

:: MSVC + Windows SDK include paths (auto-resolved by cl.exe when invoked from vcvars)
cl /std:c++14 /EHsc /nologo /I"%INC%" /Fe:"%OUT%" "%SRC%" >nul
if %ERRORLEVEL% neq 0 (
    echo BUILD FAILED
    exit /b 1
)

echo Build OK — running tests:
"%OUT%"
exit /b %ERRORLEVEL%
