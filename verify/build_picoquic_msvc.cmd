@echo off
:: [Q] 2026-05-23 - Build picoquic + picotls for Windows MSVC.
:: Prereq: vcpkg installed openssl:x64-windows + pkgconf:x64-windows.
:: Reads VCVARS + VCPKG_ROOT from build-config.local.cmd.
:: Outputs: Sunshine/third-party/picoquic/build/picoquic-core.lib +
::          _deps/picotls-build/picotls-{core,openssl,fusion,minicrypto}.lib
:: app.pro:647 contains(DEFINES, VIPLE_MPQUIC) { ... } picks these up via LIBS.

setlocal
set "ROOT=%~dp0.."
if exist "%ROOT%\build-config.local.cmd" (
    call "%ROOT%\build-config.local.cmd"
) else (
    echo [ERROR] build-config.local.cmd not found at %ROOT%
    echo         Copy build-config.template.cmd and fill in VCVARS + VCPKG_ROOT.
    exit /b 1
)
if not defined VCVARS (
    echo [ERROR] VCVARS not set in build-config.local.cmd
    exit /b 1
)
if not defined VCPKG_ROOT (
    echo [ERROR] VCPKG_ROOT not set in build-config.local.cmd
    exit /b 1
)

:: Snapshot VCPKG_ROOT BEFORE vcvars64.bat — it tends to overwrite VCPKG_ROOT
:: with Visual Studio's bundled vcpkg, which doesn't have our openssl/pkgconf.
set "USER_VCPKG=%VCPKG_ROOT%"

call "%VCVARS%"
if errorlevel 1 exit /b 1

cd /d "%ROOT%\Sunshine\third-party\picoquic"
if exist build rmdir /s /q build

:: cmake wants forward slashes in toolchain file path.
set "VCPKG_FWD=%USER_VCPKG:\=/%"

cmake -B build -G Ninja ^
    -DBUILD_DEMO=OFF -DBUILD_HTTP=OFF -DBUILD_PQBENCH=OFF ^
    -DBUILD_LOGLIB=OFF -DBUILD_LOGREADER=OFF ^
    -Dpicoquic_BUILD_TESTS=OFF -DBUILD_PICO_SIM=OFF ^
    -DPICOQUIC_FETCH_PTLS=ON ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_FWD%/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DCMAKE_C_FLAGS_RELEASE="/MD /O2 /guard:cf /guard:ehcont" ^
    -DCMAKE_C_FLAGS_DEBUG="/MDd /Zi /Od /guard:cf /guard:ehcont"
if errorlevel 1 (
    echo [ERROR] cmake configure failed
    exit /b 1
)

:: picotls sources #include "wincompat.h" but its CMakeLists.txt doesn't add
:: picotlsvs/picotls/ to the include path.  Copy picotls's OWN wincompat.h
:: (which has ws2tcpip.h + malloc.h) into include/ which IS listed.
if exist build\_deps\picotls-src\picotlsvs\picotls\wincompat.h (
    copy /y build\_deps\picotls-src\picotlsvs\picotls\wincompat.h build\_deps\picotls-src\include\wincompat.h >nul
    echo [PATCH] copied picotls wincompat.h into include/
)

:: picotls CMakeLists.txt injects GCC flags (-std=c99 -Wall -O2 -g) that break MSVC.
:: Strip them from the generated build.ninja.
powershell -NoProfile -Command ^
    "(Get-Content build\build.ninja -Raw) -replace '-std=c99 ','' -replace ' -Wall ','' | Set-Content build\build.ninja -NoNewline"
echo [PATCH] stripped GCC-only flags from build.ninja

ninja -C build picoquic-core picotls-core picotls-openssl picotls-fusion picotls-minicrypto
exit /b %ERRORLEVEL%
endlocal
