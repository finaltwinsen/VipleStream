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

:: -- 2. Build Sunshine (use propagate, NOT bump — already bumped above) --
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

:: -- 3. Build Moonlight (propagate only — already bumped above) --
echo.
echo ===========================================================
echo   Building Client (Moonlight)...
echo ===========================================================

set "SRC_M=%ROOT%\moonlight-qt"
set "RELDIR_M=%SRC_M%\app\release"
set "TEMP_M=%ROOT%\temp\moonlight"

echo [M-1/4] Setting up MSVC environment...
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to load MSVC environment
    exit /b 1
)

echo [M-2/4] Building moonlight-qt...
set "PATH=%QT_DIR%\bin;%PATH%"
cd /d "%SRC_M%"
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
if not exist "%RELDIR_M%\Moonlight.exe" (
    echo [ERROR] Moonlight.exe not found
    exit /b 1
)
echo [M-2/4] Client build succeeded

echo [M-3/4] Collecting deploy files...
if exist "%TEMP_M%" rmdir /s /q "%TEMP_M%"
mkdir "%TEMP_M%"
copy /y "%RELDIR_M%\Moonlight.exe" "%TEMP_M%\" >nul

set "ANTIHOOK=%SRC_M%\AntiHooking\release\AntiHooking.dll"
if exist "%ANTIHOOK%" copy /y "%ANTIHOOK%" "%TEMP_M%\" >nul

set "DLLDIR=%SRC_M%\libs\windows\lib\x64"
for %%F in (SDL2.dll SDL2_ttf.dll SDL3.dll avcodec-62.dll avutil-60.dll swscale-9.dll dav1d.dll opus.dll discord-rpc.dll libcrypto-3-x64.dll libssl-3-x64.dll libplacebo-360.dll) do (
    if exist "%DLLDIR%\%%F" copy /y "%DLLDIR%\%%F" "%TEMP_M%\" >nul
)

for %%F in (d3d11_vertex.fxc d3d11_yuv420_pixel.fxc d3d11_ayuv_pixel.fxc d3d11_y410_pixel.fxc d3d11_overlay_pixel.fxc) do (
    if exist "%SRC_M%\app\shaders\%%F" copy /y "%SRC_M%\app\shaders\%%F" "%TEMP_M%\" >nul
)
if exist "%SRC_M%\app\ModeSeven.ttf" copy /y "%SRC_M%\app\ModeSeven.ttf" "%TEMP_M%\" >nul
if exist "%SRC_M%\app\SDL_GameControllerDB\gamecontrollerdb.txt" copy /y "%SRC_M%\app\SDL_GameControllerDB\gamecontrollerdb.txt" "%TEMP_M%\" >nul

echo [M-4/4] Running windeployqt + packaging...
"%WINDEPLOYQT%" --release --qmldir "%SRC_M%\app\gui" --no-translations --compiler-runtime "%TEMP_M%\Moonlight.exe" 2>nul

for %%F in (dxcompiler.dll dxil.dll) do (
    if exist "%WINSDK_D3D%\%%F" copy /y "%WINSDK_D3D%\%%F" "%TEMP_M%\" >nul
)

set "CLIENT_ZIP=%RELEASE%\VipleStream-Client-%VER%.zip"
if exist "%CLIENT_ZIP%" del /f "%CLIENT_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%CLIENT_ZIP%" "%TEMP_M%\*" >nul
for %%A in ("%CLIENT_ZIP%") do set "CLIENT_SIZE=%%~zA"
set /a "CLIENT_MB=!CLIENT_SIZE! / 1048576"
echo   Client: %CLIENT_ZIP% (!CLIENT_MB! MB)

:: -- Summary --
echo.
echo ===========================================================
echo   VipleStream v%VER% - Build Complete
echo -----------------------------------------------------------
echo   Server: %SERVER_ZIP% (!SERVER_MB! MB)
echo   Client: %CLIENT_ZIP% (!CLIENT_MB! MB)
echo ===========================================================

cd /d "%ROOT%"
endlocal
