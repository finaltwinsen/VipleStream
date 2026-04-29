@echo off
:: VipleStream J.3.e.2.i.3.c -- compile vkfruc.vert / vkfruc.frag (GLSL)
:: into .spv.h headers for inclusion in vkfruc.cpp.  Mirrors the Android
:: shaders/build_shaders.cmd pattern; resulting .spv.h files are
:: checked-in so end-users do NOT need to run this script.
::
:: Re-run after editing vkfruc.vert / vkfruc.frag.
::
:: Requirements:
::   - Android NDK on disk (auto-resolved from %ANDROID_HOME%\ndk\), or
::     %ANDROID_SDK_ROOT%\ndk\, or %LOCALAPPDATA%\Android\Sdk\ndk\, or
::     override %GLSLC% env var with explicit glslc.exe path.
::   - xxd on PATH (Git for Windows or vim ships it).
::
:: Outputs (2 SPV + 2 .spv.h):
::   vkfruc.vert.spv[.h]
::   vkfruc.frag.spv[.h]

setlocal enabledelayedexpansion
cd /d "%~dp0"

:: --- Resolve glslc.exe ---
if defined GLSLC goto :have_glslc

if defined ANDROID_HOME (
    for /f "tokens=*" %%d in ('dir /b /ad /o-n "%ANDROID_HOME%\ndk\" 2^>nul') do (
        if exist "%ANDROID_HOME%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe" (
            set "GLSLC=%ANDROID_HOME%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe"
            goto :have_glslc
        )
    )
)

if defined ANDROID_SDK_ROOT (
    for /f "tokens=*" %%d in ('dir /b /ad /o-n "%ANDROID_SDK_ROOT%\ndk\" 2^>nul') do (
        if exist "%ANDROID_SDK_ROOT%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe" (
            set "GLSLC=%ANDROID_SDK_ROOT%\ndk\%%d\shader-tools\windows-x86_64\glslc.exe"
            goto :have_glslc
        )
    )
)

for /f "tokens=*" %%d in ('dir /b /ad /o-n "%LOCALAPPDATA%\Android\Sdk\ndk\" 2^>nul') do (
    if exist "%LOCALAPPDATA%\Android\Sdk\ndk\%%d\shader-tools\windows-x86_64\glslc.exe" (
        set "GLSLC=%LOCALAPPDATA%\Android\Sdk\ndk\%%d\shader-tools\windows-x86_64\glslc.exe"
        goto :have_glslc
    )
)

echo [ERROR] glslc.exe not found.  Set %%GLSLC%% to glslc.exe path,
echo         or install Android NDK under %%ANDROID_HOME%%\ndk\.
exit /b 1

:have_glslc
echo Using glslc: !GLSLC!

where xxd >nul 2>&1
if errorlevel 1 (
    echo [ERROR] xxd not on PATH.  Install Git for Windows.
    exit /b 1
)

echo.
echo === Compiling SPV ===
"!GLSLC!" -O --target-env=vulkan1.1 vkfruc.vert        -o vkfruc.vert.spv        || goto :compile_failed
"!GLSLC!" -O --target-env=vulkan1.1 vkfruc.frag        -o vkfruc.frag.spv        || goto :compile_failed
"!GLSLC!" -O --target-env=vulkan1.1 vkfruc_interp.frag -o vkfruc_interp.frag.spv || goto :compile_failed

echo.
echo === Packing .spv.h ===
xxd -i vkfruc.vert.spv        > vkfruc.vert.spv.h        || goto :pack_failed
xxd -i vkfruc.frag.spv        > vkfruc.frag.spv.h        || goto :pack_failed
xxd -i vkfruc_interp.frag.spv > vkfruc_interp.frag.spv.h || goto :pack_failed
powershell -NoProfile -Command "Get-ChildItem 'vkfruc*.spv.h' | ForEach-Object { (Get-Content $_.FullName -Raw) -replace '(?m)^unsigned ', 'static const unsigned ' | Set-Content $_.FullName -NoNewline }"
if errorlevel 1 goto :pack_failed

echo.
echo === Done. ===
echo   vkfruc.vert.spv.h
echo   vkfruc.frag.spv.h
echo   vkfruc_interp.frag.spv.h
exit /b 0

:compile_failed
echo [ERROR] glslc compile failed.
exit /b 1

:pack_failed
echo [ERROR] xxd / wrap step failed.
exit /b 1
