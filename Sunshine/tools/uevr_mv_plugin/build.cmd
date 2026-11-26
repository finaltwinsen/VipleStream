@echo off
setlocal EnableExtensions EnableDelayedExpansion

:: ============================================================================
:: build.cmd
:: VipleStream §VR-GAME-MV (M-MV0) — viple_uevr_mv_plugin.dll 獨立建置腳本
::
:: 說明：
::   1. 從 repo 根目錄的 build-config.local.cmd 讀取 VCVARS 路徑
::      （格式參考 docs/building.md §1.2 與 build-config.template.cmd）。
::   2. 啟動 vcvars64.bat 後，以 CMake (優先使用 Ninja，若無則使用 NMake Makefiles
::      或 Visual Studio 2022) 建置 x64 Release DLL。
::   3. 產出位置：<repo_root>\temp\uevr_mv_plugin\viple_uevr_mv_plugin.dll
:: ============================================================================

set "PLUGIN_DIR=%~dp0"
if "%PLUGIN_DIR:~-1%"=="\" set "PLUGIN_DIR=%PLUGIN_DIR:~0,-1%"

for %%I in ("%PLUGIN_DIR%\..\..\..") do set "REPO_ROOT=%%~fI"
set "CONFIG_FILE=%REPO_ROOT%\build-config.local.cmd"

if not exist "%CONFIG_FILE%" (
    echo [ERROR] build-config.local.cmd not found at: "%CONFIG_FILE%"
    echo         Please copy build-config.template.cmd to build-config.local.cmd and set VCVARS.
    exit /b 1
)

call "%CONFIG_FILE%"

if not defined VCVARS (
    echo [ERROR] VCVARS is not defined in "%CONFIG_FILE%".
    exit /b 1
)

if not exist "%VCVARS%" (
    echo [ERROR] VCVARS script does not exist at: "%VCVARS%"
    exit /b 1
)

echo [INFO] Initializing MSVC x64 environment via "%VCVARS%"...
call "%VCVARS%"
if errorlevel 1 (
    echo [ERROR] Failed to initialize MSVC environment.
    exit /b 1
)

set "OUT_DIR=%REPO_ROOT%\temp\uevr_mv_plugin"
set "BUILD_DIR=%OUT_DIR%\build"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

:: 優先使用 Ninja，若 PATH 中無 ninja 則退回 vcvars64 內建的 NMake Makefiles
where ninja >nul 2>nul
if %errorlevel% equ 0 (
    set "CMAKE_GEN=Ninja"
) else (
    set "CMAKE_GEN=NMake Makefiles"
)

echo [INFO] Configuring viple_uevr_mv_plugin with generator "%CMAKE_GEN%"...
cmake -S "%PLUGIN_DIR%" -B "%BUILD_DIR%" -G "%CMAKE_GEN%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="%OUT_DIR%" ^
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="%OUT_DIR%" ^
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="%OUT_DIR%"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

echo [INFO] Building viple_uevr_mv_plugin.dll (x64 Release)...
cmake --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

rem CMakeLists 把產出放在 build 目錄：複製到 OUT_DIR
copy /Y "%BUILD_DIR%\viple_uevr_mv_plugin.dll" "%OUT_DIR%\" >nul
if exist "%BUILD_DIR%\viple_uevr_mv_plugin.pdb" copy /Y "%BUILD_DIR%\viple_uevr_mv_plugin.pdb" "%OUT_DIR%\" >nul
copy /Y "%PLUGIN_DIR%\cs_downsample_mv_8x8.hlsl" "%OUT_DIR%\cs_downsample_mv_8x8.hlsl" >nul

if not exist "%OUT_DIR%\viple_uevr_mv_plugin.dll" (
    echo [ERROR] Expected output DLL not found at "%OUT_DIR%\viple_uevr_mv_plugin.dll"
    exit /b 1
)

echo [SUCCESS] Built "%OUT_DIR%\viple_uevr_mv_plugin.dll"
exit /b 0
