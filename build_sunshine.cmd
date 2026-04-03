@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  Sunshine Build + Package Script
::  Uses MSYS2 UCRT64 for compilation, then packages to ZIP
::  Output: <project root>\Sunshine-portable.zip
:: =============================================================================

set "ROOT=D:\<user>\Project\VipleStream"
set "SRC=%ROOT%\Sunshine"
set "BUILD=%SRC%\build_mingw"
set "DEPLOY=%ROOT%\sunshine-deploy"
set "OUT_ZIP=%ROOT%\Sunshine-portable.zip"
set "MSYS2=C:\msys64\usr\bin\bash.exe"
set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"

echo.
echo =========================================================
echo   Sunshine Build + Package
echo =========================================================

:: -- 1. Check dependencies --
if not exist "%MSYS2%" (
    echo [ERROR] MSYS2 not found at %MSYS2%
    exit /b 1
)

:: -- 2. Compile via MSYS2 UCRT64 --
echo [1/3] Compiling via MSYS2 UCRT64...

"%MSYS2%" -l "%ROOT%\scripts\build_sunshine_inner.sh"
if errorlevel 1 (
    echo [ERROR] Compilation failed
    exit /b 1
)

if not exist "%BUILD%\sunshine.exe" (
    echo [ERROR] sunshine.exe not found after build
    exit /b 1
)
echo [1/3] Build succeeded

:: -- 3. Collect files --
echo [2/3] Collecting deploy files...

if exist "%DEPLOY%" rmdir /s /q "%DEPLOY%"
mkdir "%DEPLOY%"

copy /y "%BUILD%\sunshine.exe" "%DEPLOY%\" >nul
echo   sunshine.exe

for %%F in (sunshinesvc.exe dxgi-info.exe audio-info.exe) do (
    if exist "%BUILD%\tools\%%F" (
        copy /y "%BUILD%\tools\%%F" "%DEPLOY%\" >nul
        echo   %%F
    )
)

xcopy /s /e /q /y "%BUILD%\assets\*" "%DEPLOY%\assets\" >nul
echo   assets\

:: -- 4. Package ZIP --
echo [3/3] Creating ZIP...

if exist "%OUT_ZIP%" del /f "%OUT_ZIP%"
"%SEVENZIP%" a -tzip -mx=7 -mmt=on "%OUT_ZIP%" "%DEPLOY%\*" >nul

for %%A in ("%OUT_ZIP%") do set "ZIPSIZE=%%~zA"
set /a "ZIPSIZE_MB=!ZIPSIZE! / 1048576"

echo.
echo =========================================================
echo   Sunshine package complete
echo   ZIP: %OUT_ZIP% (!ZIPSIZE_MB! MB)
echo =========================================================

endlocal
