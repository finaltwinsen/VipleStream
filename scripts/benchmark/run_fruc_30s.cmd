@echo off
:: VipleStream FRUC 30s test launcher
:: Requires administrator (PresentMon needs ETW)
::
:: Usage (from elevated cmd, or double-click and accept UAC):
::   run_fruc_30s.cmd
::   run_fruc_30s.cmd 192.168.1.10 Desktop
::
setlocal
set "HOSTADDR=%~1"
if "%HOSTADDR%"=="" set "HOSTADDR=192.168.1.10"
set "APP=%~2"
if "%APP%"=="" set "APP=Desktop"

:: Self-elevate if not admin
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] Requesting admin elevation...
    powershell -Command "Start-Process cmd -ArgumentList '/c','\"%~f0\" %HOSTADDR% \"%APP%\"' -Verb RunAs"
    exit /b
)

cd /d "%~dp0..\.."
set "LOGDIR=%~dp0..\..\temp\fruc_test_runs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
set "STAMP=%DATE:~-4%%DATE:~3,2%%DATE:~0,2%_%TIME:~0,2%%TIME:~3,2%%TIME:~6,2%"
set "STAMP=%STAMP: =0%"
set "LOG=%LOGDIR%\run_%STAMP%.log"

echo [*] Running test (host=%HOSTADDR% app=%APP%) - log to %LOG%
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0test_fruc_30s.ps1' -HostAddr '%HOSTADDR%' -App '%APP%' -KeepWindowed 2>&1 | Tee-Object -FilePath '%LOG%'"

echo.
echo [*] Done. Log: %LOG%
pause
endlocal
