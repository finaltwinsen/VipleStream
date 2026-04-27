@echo off
:: VipleStream §I.C.2: compile FRUC compute shaders to SPV + xxd-pack
:: into .spv.h for inclusion in vk_backend.c. Output sits next to
:: existing fullscreen.vert.spv.h / video_sample.frag.spv.h etc.
::
:: Re-run after editing any .comp source. The .spv.h files are
:: checked-in (mirrors the existing convention for the §I.B graphics
:: shaders), so end-users do NOT need to run this script.
::
:: Requirements:
::   - Android NDK on disk (auto-resolved from %ANDROID_HOME%\ndk\),
::     or override $GLSLC env var with an explicit glslc.exe path
::   - xxd on PATH (Git for Windows or vim ships it)
::   - PowerShell (Windows ships it; used for one regex pass)
::
:: Outputs (8 SPV + 8 .spv.h):
::   ycbcr_to_rgba.comp.spv[.h]
::   mv_median.comp.spv[.h]
::   motionest_compute_q{0,1,2}.comp.spv[.h]
::   warp_compute_q{0,1,2}.comp.spv[.h]

setlocal enabledelayedexpansion
cd /d "%~dp0"

:: --- Resolve glslc.exe ---
if defined GLSLC goto :have_glslc
if not defined ANDROID_HOME (
    echo [ERROR] %%ANDROID_HOME%% not set and %%GLSLC%% not set.
    exit /b 1
)
for /f "tokens=*" %%d in ('dir /b /ad /o-n "%ANDROID_HOME%\ndk\" 2^>nul') do (
    if exist "%ANDROID_HOME%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe" (
        set "GLSLC=%ANDROID_HOME%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe"
        goto :have_glslc
    )
)
echo [ERROR] glslc.exe not found in any %ANDROID_HOME%\ndk\*\shader-tools\windows-x86_64\.
exit /b 1
:have_glslc
echo Using glslc: !GLSLC!

where xxd >nul 2>&1
if errorlevel 1 (
    echo [ERROR] xxd not on PATH. Install Git for Windows.
    exit /b 1
)

:: --- Compile to SPV ---
echo.
echo === Compiling SPV ===
"!GLSLC!" -O --target-env=vulkan1.1 ycbcr_to_rgba.comp -o ycbcr_to_rgba.comp.spv     || goto :compile_failed
"!GLSLC!" -O --target-env=vulkan1.1 mv_median.comp     -o mv_median.comp.spv         || goto :compile_failed
for %%Q in (0 1 2) do (
    "!GLSLC!" -O --target-env=vulkan1.1 -DQUALITY_LEVEL=%%Q motionest_compute.comp -o motionest_compute_q%%Q.comp.spv || goto :compile_failed
    "!GLSLC!" -O --target-env=vulkan1.1 -DQUALITY_LEVEL=%%Q warp_compute.comp      -o warp_compute_q%%Q.comp.spv      || goto :compile_failed
)

:: --- Wrap to .spv.h ---
echo.
echo === Packing .spv.h ===
for %%F in (*.comp.spv) do (
    xxd -i %%F > %%F.h
    if errorlevel 1 goto :pack_failed
)
powershell -NoProfile -Command "Get-ChildItem '*.comp.spv.h' | ForEach-Object { (Get-Content $_.FullName -Raw) -replace '(?m)^unsigned ', 'static const unsigned ' | Set-Content $_.FullName -NoNewline }"
if errorlevel 1 goto :pack_failed

echo.
echo === Done. ===
for %%F in (*.comp.spv.h) do echo   %%F
exit /b 0

:compile_failed
echo [ERROR] glslc compile failed.
exit /b 1

:pack_failed
echo [ERROR] xxd / wrap step failed.
exit /b 1
