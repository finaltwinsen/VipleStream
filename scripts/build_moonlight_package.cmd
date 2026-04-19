@echo off
:: =============================================================================
::  VipleStream Moonlight - Packaging step (VCS-tracked)
::
::  The root build_moonlight.cmd is per-machine and gitignored, so the
::  canonical file/shader list lives here. Anyone whose own root wrapper
::  builds Moonlight can `call` this to do the packaging.
::
::  Expected environment (set by caller / build-config.local.cmd):
::    ROOT         VipleStream repo root (no trailing \)
::    SRC          %ROOT%\moonlight-qt
::    RELDIR       %SRC%\app\release  (Qt output)
::    TEMP_DIR     %ROOT%\temp\moonlight  (packaging staging dir; will be wiped)
::    RELEASE      %ROOT%\release  (where the final zip goes)
::    VER          version string (e.g. 1.1.74) - used for output filename
::    SEVENZIP     path to 7z.exe
::    WINDEPLOYQT  path to windeployqt.exe
::    WINSDK_D3D   path to Windows SDK\Redist\D3D\x64
:: =============================================================================
setlocal enabledelayedexpansion

:: ---- 1. Clean staging ----
echo [pkg 1/5] Collecting deploy files to %TEMP_DIR%
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

:: ---- 2. D3D11 shaders + data files ----
echo [pkg 2/5] Copying shaders and data files
::
:: IMPORTANT: GenericFRUC loads the _quality / _balanced / _performance
:: variants of motionest and warp (one per preset, compiled from
:: d3d11_motionest_compute.hlsl and d3d11_warp_compute.hlsl with different
:: QUALITY_LEVEL defines). If any of the six variants is missing at runtime,
:: GenericFRUC::initialize() logs "No FRUC backend available" and frame
:: interpolation is silently disabled. Ship them all.
::
for %%F in (d3d11_vertex.fxc d3d11_yuv420_pixel.fxc d3d11_ayuv_pixel.fxc d3d11_y410_pixel.fxc d3d11_overlay_pixel.fxc d3d11_motionest_compute.fxc d3d11_motionest_quality.fxc d3d11_motionest_balanced.fxc d3d11_motionest_performance.fxc d3d11_warp_compute.fxc d3d11_warp_quality.fxc d3d11_warp_balanced.fxc d3d11_warp_performance.fxc) do (
    if exist "%SRC%\app\shaders\%%F" (
        copy /y "%SRC%\app\shaders\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    ) else (
        echo   [WARN] %%F missing in %SRC%\app\shaders\
    )
)
if exist "%SRC%\app\ModeSeven.ttf" copy /y "%SRC%\app\ModeSeven.ttf" "%TEMP_DIR%\" >nul
if exist "%SRC%\app\SDL_GameControllerDB\gamecontrollerdb.txt" copy /y "%SRC%\app\SDL_GameControllerDB\gamecontrollerdb.txt" "%TEMP_DIR%\" >nul

:: ---- 3. windeployqt ----
echo [pkg 3/5] Running windeployqt
"%WINDEPLOYQT%" --release --qmldir "%SRC%\app\gui" --no-translations --compiler-runtime "%TEMP_DIR%\Moonlight.exe" 2>nul

:: ---- 4. DirectX runtime libs ----
echo [pkg 4/5] Copying DirectX runtime
for %%F in (dxcompiler.dll dxil.dll) do (
    if exist "%WINSDK_D3D%\%%F" (
        copy /y "%WINSDK_D3D%\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    )
)

:: ---- 4b. Debug symbols (PDBs) ----
::
:: Ship PDBs next to the .exe so WinDbg / cdb can symbolicate minidumps
:: without any symbol-path setup. Statically-linked submodules
:: (moonlight-common-c, h264bitstream, qmdnsengine) merge their symbols
:: into Moonlight.pdb at link time, so Moonlight.pdb alone covers all our
:: first-party code. AntiHooking is a separate DLL so its PDB ships too.
:: Third-party DLLs (FFmpeg, SDL, Qt, libcrypto, libplacebo) don't ship
:: PDBs with their binaries so those stay unsymbolicated in the dump —
:: which is fine, we rarely need internals of those.
echo [pkg 4b/5] Copying PDBs for crash symbolication
for %%F in ("%SRC%\app\release\Moonlight.pdb" "%SRC%\AntiHooking\release\AntiHooking.pdb") do (
    if exist %%F (
        copy /y %%F "%TEMP_DIR%\" >nul
        echo   %%~nxF
    ) else (
        echo   [WARN] %%~nxF missing - crash dumps will not symbolicate
    )
)

:: ---- 5. Zip it ----
echo [pkg 5/5] Creating zip
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
