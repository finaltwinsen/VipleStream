@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  Moonlight Build + Package Script (MSVC 2022 + Qt 6.11)
::  Output: <project root>\Moonlight-portable.zip
:: =============================================================================

set "ROOT=D:\<user>\Project\VipleStream"
set "SRC=%ROOT%\moonlight-qt"
set "RELDIR=%SRC%\app\release"
set "DEPLOY=%ROOT%\moonlight-deploy"
set "OUT_ZIP=%ROOT%\Moonlight-portable.zip"
set "WINDEPLOYQT=C:\Qt\6.11.0\msvc2022_64\bin\windeployqt.exe"
set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"

echo.
echo =========================================================
echo   Moonlight Build + Package
echo =========================================================

:: -- 1. Setup MSVC environment --
echo [1/5] Setting up MSVC environment...
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Failed to load MSVC environment
    exit /b 1
)
where cl

:: -- 2. Build --
echo [2/5] Building moonlight-qt...
cd /d "%SRC%"
nmake release
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

if not exist "%RELDIR%\Moonlight.exe" (
    echo [ERROR] Moonlight.exe not found: %RELDIR%\Moonlight.exe
    exit /b 1
)
echo [2/5] Build succeeded

:: -- 3. Collect files to deploy folder --
echo [3/5] Collecting deploy files...

if exist "%DEPLOY%" rmdir /s /q "%DEPLOY%"
mkdir "%DEPLOY%"

copy /y "%RELDIR%\Moonlight.exe" "%DEPLOY%\" >nul
echo   Moonlight.exe

set "ANTIHOOK=%SRC%\AntiHooking\release\AntiHooking.dll"
if exist "%ANTIHOOK%" (
    copy /y "%ANTIHOOK%" "%DEPLOY%\" >nul
    echo   AntiHooking.dll
) else (
    echo   [WARN] AntiHooking.dll not found
)

set "DLLDIR=%SRC%\libs\windows\lib\x64"
for %%F in (SDL2.dll SDL2_ttf.dll SDL3.dll avcodec-62.dll avutil-60.dll swscale-9.dll dav1d.dll opus.dll discord-rpc.dll libcrypto-3-x64.dll libssl-3-x64.dll libplacebo-360.dll) do (
    if exist "%DLLDIR%\%%F" (
        copy /y "%DLLDIR%\%%F" "%DEPLOY%\" >nul
        echo   %%F
    ) else (
        echo   [WARN] Not found: %%F
    )
)

:: -- 4. windeployqt + DirectX DLLs --
echo [4/5] Running windeployqt...
"%WINDEPLOYQT%" --release --qmldir "%SRC%\app\gui" --no-translations --compiler-runtime "%DEPLOY%\Moonlight.exe" 2>nul

set "WINSDK_D3D=C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64"
for %%F in (dxcompiler.dll dxil.dll) do (
    if exist "%WINSDK_D3D%\%%F" (
        copy /y "%WINSDK_D3D%\%%F" "%DEPLOY%\" >nul
        echo   DirectX: %%F
    )
)

:: -- 5. Package ZIP --
echo [5/5] Creating ZIP...

if exist "%OUT_ZIP%" del /f "%OUT_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%OUT_ZIP%" "%DEPLOY%\*" >nul

for %%A in ("%OUT_ZIP%") do set "ZIPSIZE=%%~zA"
set /a "ZIPSIZE_MB=!ZIPSIZE! / 1048576"

echo.
echo =========================================================
echo   Moonlight package complete
echo   ZIP: %OUT_ZIP% (!ZIPSIZE_MB! MB)
echo =========================================================

endlocal
