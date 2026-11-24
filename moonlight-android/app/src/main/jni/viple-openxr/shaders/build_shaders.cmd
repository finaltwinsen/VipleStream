@echo off
:: Compile quad.vert / quad.frag to SPIR-V and wrap them as C headers.
:: Same approach as moonlight-core\shadersuild_shaders.cmd: the generated
:: *.spv.h are checked in, normal builds do not run glslc. Re-run after
:: editing the GLSL. Symbols: quad_vert_spv(_len), quad_frag_spv(_len).
:: Needs ANDROID_HOME (or GLSLC pointing at glslc.exe), xxd, PowerShell.
:: Comments stay ASCII: cmd.exe misparses UTF-8 comment lines under cp950.
::   quad_vert_spv / quad_vert_spv_len
::   quad_frag_spv / quad_frag_spv_len
::
::   - PowerShell

setlocal enabledelayedexpansion
cd /d "%~dp0"

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
echo [ERROR] glslc.exe not found under %%ANDROID_HOME%%\ndk\*\shader-tools\windows-x86_64\
exit /b 1
:have_glslc
echo Using glslc: !GLSLC!

where xxd >nul 2>&1
if errorlevel 1 (
    echo [ERROR] xxd not on PATH. Install Git for Windows.
    exit /b 1
)

echo === Compiling SPV ===
"!GLSLC!" -O --target-env=vulkan1.1 quad.vert -o quad.vert.spv || goto :compile_failed
"!GLSLC!" -O --target-env=vulkan1.1 quad.frag -o quad.frag.spv || goto :compile_failed

echo === Packing .spv.h ===
for %%F in (quad.vert.spv quad.frag.spv) do (
    xxd -i %%F > %%F.h
    if errorlevel 1 goto :pack_failed
)
powershell -NoProfile -Command "Get-ChildItem 'quad.vert.spv.h','quad.frag.spv.h' | ForEach-Object { (Get-Content $_.FullName -Raw) -replace '(?m)^unsigned ', 'static const unsigned ' | Set-Content $_.FullName -NoNewline }"
if errorlevel 1 goto :pack_failed

echo === Done. Check in quad.vert.spv.h and quad.frag.spv.h ===
exit /b 0

:compile_failed
echo [ERROR] glslc compile failed.
exit /b 1

:pack_failed
echo [ERROR] xxd / wrap step failed.
exit /b 1
