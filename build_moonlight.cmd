@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream Moonlight - Build + Package (root wrapper)
::
::  Per-machine paths live here (this file is gitignored).
::  Canonical packaging list lives in scripts\build_moonlight_package.cmd
::  (VCS-tracked), so the shader set can be updated without touching each
::  developer's local copy of this file.
:: =============================================================================

if exist "%~dp0build-config.local.cmd" (
    call "%~dp0build-config.local.cmd"
) else (
    echo [ERROR] build-config.local.cmd not found.
    echo         Copy build-config.template.cmd to build-config.local.cmd and adjust paths.
    exit /b 1
)

set "SRC=%ROOT%\moonlight-qt"
set "RELDIR=%SRC%\app\release"
set "TEMP_DIR=%ROOT%\temp\moonlight"
set "RELEASE=%ROOT%\release"

echo.
echo =========================================================
echo   VipleStream Moonlight - Build + Package
echo =========================================================

:: -- 0. Bump version --
echo [0/6] Bumping version...
call "%ROOT%\scripts\bump_version.cmd"
set /p VER=<"%ROOT%\temp\current_version.txt"
echo   Version: %VER%

:: -- 1. Setup MSVC environment --
echo [1/6] Setting up MSVC environment...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to load MSVC environment
    exit /b 1
)

:: -- 2. Build --
echo [2/6] Building moonlight-qt...
set "PATH=%QT_DIR%\bin;%PATH%"
cd /d "%SRC%"

:: Force qmake to regenerate Makefile (version.txt changed but .pro didn't)
copy /b "%SRC%\app\app.pro"+,, "%SRC%\app\app.pro" >nul 2>&1

qmake moonlight-qt.pro CONFIG+=release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)
if not exist "%RELDIR%\Moonlight.exe" (
    echo [ERROR] Moonlight.exe not found: %RELDIR%\Moonlight.exe
    exit /b 1
)
echo [2/6] Build succeeded

:: -- 3..6. Delegate packaging to VCS-tracked inner script --
call "%ROOT%\scripts\build_moonlight_package.cmd"
if errorlevel 1 (
    echo [ERROR] Packaging failed
    exit /b 1
)

endlocal
