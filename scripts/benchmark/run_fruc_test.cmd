@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream FRUC Automated Test Runner
::  Fully automated: deploy → test pattern → CLI stream → collect → report
:: =============================================================================

set "ROOT=%~dp0..\.."
set "SERVER=user@192.168.1.10"
set "SERVER_HOST=192.168.1.10"
set "SERVER_DIR=C:/Users/user/fruc_test"
set "SCRIPT_DIR=%~dp0"
set "PYTHON=python"
set "MOONLIGHT=C:\Program Files\Moonlight Game Streaming\Moonlight.exe"
set "APP_NAME=Desktop"
set "SCENE_DURATION=10"
set "SCENE_COUNT=6"
set "FRUC_BACKEND=generic"

:: Parse args
:parse_args
if "%1"=="" goto done_args
if "%1"=="--duration" (
    set "SCENE_DURATION=%2"
    shift & shift
    goto parse_args
)
if "%1"=="--backend" (
    set "FRUC_BACKEND=%2"
    shift & shift
    goto parse_args
)
if "%1"=="--app" (
    set "APP_NAME=%2"
    shift & shift
    goto parse_args
)
if "%1"=="--help" (
    echo Usage: run_fruc_test.cmd [options]
    echo   --duration N    Seconds per test scene (default: 10)
    echo   --backend X     FRUC backend: generic or nvidia (default: generic)
    echo   --app NAME      App to stream (default: Desktop)
    exit /b 0
)
shift
goto parse_args
:done_args

:: Calculate total test time: scenes + 15s buffer for connect/disconnect
set /a "TOTAL_SCENE_TIME=%SCENE_DURATION% * %SCENE_COUNT%"
set /a "COLLECT_TIMEOUT=%TOTAL_SCENE_TIME% + 20"

echo.
echo =========================================================
echo   VipleStream FRUC Automated Test
echo =========================================================
echo.
echo   Server:       %SERVER_HOST%
echo   App:          %APP_NAME%
echo   FRUC backend: %FRUC_BACKEND%
echo   Scene:        %SCENE_DURATION%s x %SCENE_COUNT% = %TOTAL_SCENE_TIME%s
echo   Timeout:      %COLLECT_TIMEOUT%s (with connection buffer)
echo.

:: -- 1. Deploy test pattern to server --
echo [1/6] Deploying test pattern to server...
ssh %SERVER% "if (!(Test-Path '%SERVER_DIR%')) { New-Item -ItemType Directory -Path '%SERVER_DIR%' | Out-Null }" 2>nul
scp -q "%SCRIPT_DIR%server_test_pattern.py" %SERVER%:%SERVER_DIR%/server_test_pattern.py
if errorlevel 1 (
    echo [ERROR] SCP deploy failed
    exit /b 1
)
echo   OK

:: -- 2. Start test pattern on server (detached) --
echo [2/6] Starting test pattern on server...
ssh %SERVER% "Start-Process python3 -ArgumentList '%SERVER_DIR%/server_test_pattern.py','--duration','%SCENE_DURATION%'" 2>nul
echo   Test pattern running (%SCENE_DURATION%s x %SCENE_COUNT% scenes)

:: -- 3. Start client metric collector (background) --
echo [3/6] Starting metric collector (background, timeout=%COLLECT_TIMEOUT%s)...
start "FRUC Collector" /MIN %PYTHON% "%SCRIPT_DIR%client_collect.py" --timeout %COLLECT_TIMEOUT% --output "%SCRIPT_DIR%fruc_test_report.json"

:: Brief pause to let collector initialize DBWIN capture
timeout /t 2 /nobreak >nul

:: -- 4. Launch Moonlight CLI streaming --
echo [4/6] Launching Moonlight CLI stream...
echo   Command: stream %SERVER_HOST% "%APP_NAME%" --1080 --fps 30 --frame-interpolation --fruc-backend %FRUC_BACKEND%
echo.

start "Moonlight" "%MOONLIGHT%" stream %SERVER_HOST% "%APP_NAME%" --1080 --fps 30 --frame-interpolation --fruc-backend %FRUC_BACKEND% --performance-overlay --display-mode borderless

:: -- 5. Wait for test to complete --
echo [5/6] Waiting for test (%TOTAL_SCENE_TIME%s)...
echo   Streaming in progress. DO NOT close the Moonlight window.
echo.

:: Progress bar
for /L %%i in (1,10,%TOTAL_SCENE_TIME%) do (
    set /a "pct=%%i * 100 / %TOTAL_SCENE_TIME%"
    echo   [!pct!%%] %%i / %TOTAL_SCENE_TIME%s
    timeout /t 10 /nobreak >nul
)

:: Wait a few more seconds for final data
timeout /t 5 /nobreak >nul

:: -- 6. Quit streaming and generate report --
echo [6/6] Stopping stream and generating report...
"%MOONLIGHT%" quit %SERVER_HOST% 2>nul

:: Kill test pattern on server if still running
ssh %SERVER% "Get-Process python3 -ErrorAction SilentlyContinue | Stop-Process -Force" 2>nul

:: Wait for collector to finish
echo   Waiting for collector to finish...
timeout /t 8 /nobreak >nul

:: Show report
echo.
if exist "%SCRIPT_DIR%fruc_test_report.json" (
    %PYTHON% "%SCRIPT_DIR%client_collect.py" --report "%SCRIPT_DIR%fruc_test_report.json"
) else (
    echo [WARN] Report file not found. Collector may still be running.
)

echo.
echo =========================================================
echo   Test complete.
echo   Report: %SCRIPT_DIR%fruc_test_report.json
echo   Re-analyze: python client_collect.py --report fruc_test_report.json
echo =========================================================

endlocal
