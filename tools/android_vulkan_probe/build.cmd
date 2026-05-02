@echo off
rem VipleStream §I.D probe build helper.
rem
rem Cross-compiles probe.c to a Pixel-5-runnable aarch64 binary using the
rem Android NDK clang frontend.  Output: vk_probe (no extension), ELF binary
rem ready for `adb push /data/local/tmp/`.
rem
rem Defaults to NDK r30 at the standard Android Studio install path.
rem Override NDK_ROOT if your NDK lives elsewhere.

setlocal

if not defined NDK_ROOT set "NDK_ROOT=%LOCALAPPDATA%\Android\Sdk\ndk\30.0.14904198"

set "CLANG=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android30-clang.cmd"
if not exist "%CLANG%" (
    echo [ERROR] aarch64 clang not found at: %CLANG%
    echo         Set NDK_ROOT to your NDK install path or install NDK r30+.
    exit /b 1
)

set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"

call "%CLANG%" "%HERE%\probe.c" -o "%HERE%\vk_probe" -lvulkan
if errorlevel 1 (
    echo [FAIL] cross-compile failed
    exit /b 1
)

echo [OK] vk_probe built. Push and run with:
echo     adb push %HERE%\vk_probe /data/local/tmp/
echo     adb shell chmod +x /data/local/tmp/vk_probe
echo     adb shell /data/local/tmp/vk_probe

endlocal
