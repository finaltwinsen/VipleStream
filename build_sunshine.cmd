@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream Sunshine - Build + Package
::  Bumps patch version, compiles via MSYS2 UCRT64, packages to release/
:: =============================================================================

if exist "%~dp0build-config.local.cmd" (
    call "%~dp0build-config.local.cmd"
) else (
    echo [ERROR] build-config.local.cmd not found.
    echo         Copy build-config.template.cmd to build-config.local.cmd and adjust paths.
    exit /b 1
)

set "SRC=%ROOT%\Sunshine"
set "BUILD=%SRC%\build_mingw"
set "TEMP_DIR=%ROOT%\temp\sunshine"
set "RELEASE=%ROOT%\release"

echo.
echo =========================================================
echo   VipleStream Sunshine - Build + Package
echo =========================================================

:: -- 0. Check dependencies --
if not exist "%MSYS2%" (
    echo [ERROR] MSYS2 not found at %MSYS2%
    exit /b 1
)

:: -- 1. Bump version --
echo [1/4] Bumping version...
call "%ROOT%\scripts\bump_version.cmd"
set /p VER=<"%ROOT%\temp\current_version.txt"
echo   Version: %VER%

:: -- 2. Compile via MSYS2 UCRT64 --
echo [2/4] Compiling via MSYS2 UCRT64...
"%MSYS2%" -l "%ROOT%\scripts\build_sunshine_inner.sh"
if errorlevel 1 (
    echo [ERROR] Compilation failed
    exit /b 1
)
if not exist "%BUILD%\sunshine.exe" (
    echo [ERROR] sunshine.exe not found after build
    exit /b 1
)
echo [2/4] Build succeeded

:: -- 3. Collect files to temp --
echo [3/4] Collecting deploy files...
if exist "%TEMP_DIR%" rmdir /s /q "%TEMP_DIR%"
mkdir "%TEMP_DIR%"

copy /y "%BUILD%\sunshine.exe" "%TEMP_DIR%\" >nul
echo   sunshine.exe

for %%F in (sunshinesvc.exe dxgi-info.exe audio-info.exe) do (
    if exist "%BUILD%\tools\%%F" (
        copy /y "%BUILD%\tools\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    )
)

xcopy /s /e /q /y "%BUILD%\assets\*" "%TEMP_DIR%\assets\" >nul
echo   assets\

:: -- 4. Package to release --
echo [4/4] Packaging...
if not exist "%RELEASE%" mkdir "%RELEASE%"

set "OUT_ZIP=%RELEASE%\VipleStream-Server-%VER%.zip"
if exist "%OUT_ZIP%" del /f "%OUT_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%OUT_ZIP%" "%TEMP_DIR%\*" >nul

for %%A in ("%OUT_ZIP%") do set "ZIPSIZE=%%~zA"
set /a "ZIPSIZE_MB=!ZIPSIZE! / 1048576"

echo.
echo =========================================================
echo   VipleStream Server v%VER%
echo   %OUT_ZIP% (!ZIPSIZE_MB! MB)
echo =========================================================

endlocal
