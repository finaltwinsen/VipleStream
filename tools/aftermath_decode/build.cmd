@echo off
rem Phase 1.6b - build the Aftermath dump decoder CLI.
rem
rem Requires:
rem   - VipleStream\build-config.local.cmd (for VCVARS path)
rem   - moonlight-qt\3rdparty\aftermath_sdk\ (the unzipped SDK)
rem
rem Output: tools\aftermath_decode\aftermath_decode.exe

setlocal enabledelayedexpansion

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
set "ROOT=%HERE%\..\.."

if not exist "%ROOT%\build-config.local.cmd" (
    echo [ERROR] build-config.local.cmd not found at %ROOT%
    echo         copy build-config.template.cmd build-config.local.cmd and edit it.
    exit /b 1
)
call "%ROOT%\build-config.local.cmd"

set "AM_SDK=%ROOT%\moonlight-qt\3rdparty\aftermath_sdk"
if not exist "%AM_SDK%\include\GFSDK_Aftermath.h" (
    echo [ERROR] Aftermath SDK not found at %AM_SDK%
    echo         Download NVIDIA Nsight Aftermath SDK and unzip into that folder.
    exit /b 1
)

if not exist "!VCVARS!" (
    echo [ERROR] VCVARS not found: !VCVARS!
    exit /b 1
)
call "!VCVARS!" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64.bat failed
    exit /b 1
)

pushd "%HERE%"

cl /nologo /std:c++17 /EHsc /O2 /W3 ^
   /I"%AM_SDK%\include" ^
   main.cpp ^
   /Fe:aftermath_decode.exe ^
   /link "%AM_SDK%\lib\x64\GFSDK_Aftermath_Lib.x64.lib"

set "RC=%ERRORLEVEL%"

if %RC% EQU 0 (
    if exist "%AM_SDK%\lib\x64\GFSDK_Aftermath_Lib.x64.dll" (
        copy /y "%AM_SDK%\lib\x64\GFSDK_Aftermath_Lib.x64.dll" . >nul
    )
    echo [OK] aftermath_decode.exe built.
) else (
    echo [FAIL] cl returned %RC%.
)

popd
endlocal & exit /b %RC%
