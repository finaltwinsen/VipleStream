@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream - Build All (Server + Client)
::  Bumps version ONCE, then builds both Sunshine and Moonlight
:: =============================================================================

if exist "%~dp0build-config.local.cmd" (
    call "%~dp0build-config.local.cmd"
) else (
    echo [ERROR] build-config.local.cmd not found.
    echo         Copy build-config.template.cmd to build-config.local.cmd and adjust paths.
    exit /b 1
)
set "RELEASE=%ROOT%\release"

echo.
echo ===========================================================
echo   VipleStream - Build All (Server + Client)
echo ===========================================================

:: -- 1. Bump version once --
echo.
echo [Step 1] Bumping version...
call "%ROOT%\scripts\bump_version.cmd"
set /p VER=<"%ROOT%\temp\current_version.txt"
echo   Version: %VER%

:: -- 2. Build Sunshine --
echo.
echo ===========================================================
echo   Building Server (Sunshine)...
echo ===========================================================

set "SRC_S=%ROOT%\Sunshine"
set "BUILD_S=%SRC_S%\build_mingw"
set "TEMP_S=%ROOT%\temp\sunshine"

if not exist "%MSYS2%" (
    echo [ERROR] MSYS2 not found at %MSYS2%
    exit /b 1
)

echo [S-1/2] Compiling via MSYS2 UCRT64...
"%MSYS2%" -l "%ROOT%\scripts\build_sunshine_inner.sh"
if errorlevel 1 (
    echo [ERROR] Sunshine compilation failed
    exit /b 1
)
if not exist "%BUILD_S%\sunshine.exe" (
    echo [ERROR] sunshine.exe not found after build
    exit /b 1
)
echo [S-1/2] Server build succeeded

echo [S-2/2] Collecting + packaging...
if exist "%TEMP_S%" rmdir /s /q "%TEMP_S%"
mkdir "%TEMP_S%"
copy /y "%BUILD_S%\sunshine.exe" "%TEMP_S%\" >nul
for %%F in (sunshinesvc.exe dxgi-info.exe audio-info.exe) do (
    if exist "%BUILD_S%\tools\%%F" copy /y "%BUILD_S%\tools\%%F" "%TEMP_S%\" >nul
)
xcopy /s /e /q /y "%BUILD_S%\assets\*" "%TEMP_S%\assets\" >nul

if not exist "%RELEASE%" mkdir "%RELEASE%"
set "SERVER_ZIP=%RELEASE%\VipleStream-Server-%VER%.zip"
if exist "%SERVER_ZIP%" del /f "%SERVER_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%SERVER_ZIP%" "%TEMP_S%\*" >nul
for %%A in ("%SERVER_ZIP%") do set "SERVER_SIZE=%%~zA"
set /a "SERVER_MB=!SERVER_SIZE! / 1048576"
echo   Server: %SERVER_ZIP% (!SERVER_MB! MB)

:: -- 3. Build Moonlight --
echo.
echo ===========================================================
echo   Building Client (Moonlight)...
echo ===========================================================

set "SRC=%ROOT%\moonlight-qt"
set "RELDIR=%SRC%\app\release"
set "TEMP_DIR=%ROOT%\temp\moonlight"

echo [M-1/3] Setting up MSVC environment...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to load MSVC environment
    exit /b 1
)

echo [M-2/3] Building moonlight-qt...
set "PATH=%QT_DIR%\bin;%PATH%"
cd /d "%SRC%"

:: Touch app.pro so qmake re-runs and regenerates version_string.h from
:: the current version.txt.  The generated header is a real build dep,
:: so nmake recompiles only the files that #include it (no blanket
:: .obj deletes needed, unlike the old -DVERSION_STR macro path).
copy /b "%SRC%\app\app.pro"+,, "%SRC%\app\app.pro" >nul 2>&1

qmake moonlight-qt.pro CONFIG+=release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)
nmake release
if errorlevel 1 (
    echo [ERROR] Moonlight build failed
    exit /b 1
)
if not exist "%RELDIR%\Moonlight.exe" (
    echo [ERROR] Moonlight.exe not found
    exit /b 1
)
echo [M-2/3] Client build succeeded

:: Delegate packaging (including the canonical shader list) to the
:: VCS-tracked inner script so the shader manifest lives in one place.
echo [M-3/3] Packaging...
call "%ROOT%\scripts\build_moonlight_package.cmd"
if errorlevel 1 (
    echo [ERROR] Moonlight packaging failed
    exit /b 1
)

:: -- Summary --
echo.
echo ===========================================================
echo   VipleStream v%VER% - Build Complete
echo -----------------------------------------------------------
echo   Server: %SERVER_ZIP% (!SERVER_MB! MB)
echo   Client: %RELEASE%\VipleStream-Client-%VER%.zip
echo ===========================================================

cd /d "%ROOT%"
endlocal
