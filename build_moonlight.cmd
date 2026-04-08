@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream Moonlight - Build + Package
::  Bumps patch version, compiles via MSVC + Qt, packages to release/
:: =============================================================================

:: Load machine-specific paths from local config
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

:: -- 3. Collect files to temp --
echo [3/6] Collecting deploy files...
if exist "%TEMP_DIR%" rmdir /s /q "%TEMP_DIR%"
mkdir "%TEMP_DIR%"

copy /y "%RELDIR%\Moonlight.exe" "%TEMP_DIR%\" >nul
echo   Moonlight.exe

set "ANTIHOOK=%SRC%\AntiHooking\release\AntiHooking.dll"
if exist "%ANTIHOOK%" (
    copy /y "%ANTIHOOK%" "%TEMP_DIR%\" >nul
    echo   AntiHooking.dll
)

set "DLLDIR=%SRC%\libs\windows\lib\x64"
for %%F in (SDL2.dll SDL2_ttf.dll SDL3.dll avcodec-62.dll avutil-60.dll swscale-9.dll dav1d.dll opus.dll discord-rpc.dll libcrypto-3-x64.dll libssl-3-x64.dll libplacebo-360.dll) do (
    if exist "%DLLDIR%\%%F" (
        copy /y "%DLLDIR%\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    )
)

:: -- 4. D3D11VA shaders + data files --
echo [4/6] Copying shaders and data files...
for %%F in (d3d11_vertex.fxc d3d11_yuv420_pixel.fxc d3d11_ayuv_pixel.fxc d3d11_y410_pixel.fxc d3d11_overlay_pixel.fxc) do (
    if exist "%SRC%\app\shaders\%%F" (
        copy /y "%SRC%\app\shaders\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    )
)
if exist "%SRC%\app\ModeSeven.ttf" copy /y "%SRC%\app\ModeSeven.ttf" "%TEMP_DIR%\" >nul
if exist "%SRC%\app\SDL_GameControllerDB\gamecontrollerdb.txt" copy /y "%SRC%\app\SDL_GameControllerDB\gamecontrollerdb.txt" "%TEMP_DIR%\" >nul

:: -- 5. windeployqt + DirectX --
echo [5/6] Running windeployqt...
"%WINDEPLOYQT%" --release --qmldir "%SRC%\app\gui" --no-translations --compiler-runtime "%TEMP_DIR%\Moonlight.exe" 2>nul

for %%F in (dxcompiler.dll dxil.dll) do (
    if exist "%WINSDK_D3D%\%%F" (
        copy /y "%WINSDK_D3D%\%%F" "%TEMP_DIR%\" >nul
        echo   DirectX: %%F
    )
)

:: -- 6. Package to release --
echo [6/6] Packaging...
if not exist "%RELEASE%" mkdir "%RELEASE%"

set "OUT_ZIP=%RELEASE%\VipleStream-Client-%VER%.zip"
if exist "%OUT_ZIP%" del /f "%OUT_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%OUT_ZIP%" "%TEMP_DIR%\*" >nul

for %%A in ("%OUT_ZIP%") do set "ZIPSIZE=%%~zA"
set /a "ZIPSIZE_MB=!ZIPSIZE! / 1048576"

echo.
echo =========================================================
echo   VipleStream Client v%VER%
echo   %OUT_ZIP% (!ZIPSIZE_MB! MB)
echo =========================================================

endlocal
