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
if exist "%TEMP_DIR%" (
    rmdir /s /q "%TEMP_DIR%"
    if exist "%TEMP_DIR%" (
        echo [ERROR] Failed to remove %TEMP_DIR%
        echo         Likely a zombie VipleStream.exe is holding files.
        echo         Run: taskkill /F /IM VipleStream.exe ^&^& reboot if needed.
        exit /b 2
    )
)
mkdir "%TEMP_DIR%"

copy /y "%RELDIR%\VipleStream.exe" "%TEMP_DIR%\" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy VipleStream.exe into %TEMP_DIR%
    echo         Source : %RELDIR%\VipleStream.exe
    echo         If staging dir wasn't cleaned, the old exe was kept and
    echo         the resulting zip / temp\moonlight binary will be STALE.
    exit /b 2
)
echo   VipleStream.exe

set "ANTIHOOK=%SRC%\AntiHooking\release\AntiHooking.dll"
if exist "%ANTIHOOK%" (
    copy /y "%ANTIHOOK%" "%TEMP_DIR%\" >nul
    echo   AntiHooking.dll
)

set "DLLDIR=%SRC%\libs\windows\lib\x64"
:: §J.3.f — runtime deps of rebuilt FFmpeg 8.1 (mingw GCC built):
::   libdav1d-7.dll  libiconv-2.dll  zlib1.dll       — pulled in by avcodec
::   libwinpthread-1.dll                              — mingw threading runtime
::   libva.dll  libva_win32.dll                       — VAAPI loader stubs
::                                                      (link-time imports;
::                                                      no-op at runtime on
::                                                      Windows since avutil
::                                                      doesn't use VAAPI)
:: dav1d.dll (no -7 suffix) is the legacy pre-§J.3.f standalone dav1d build;
:: kept until we drop ABI compat with old plugins.
for %%F in (SDL2.dll SDL2_ttf.dll SDL3.dll avcodec-62.dll avutil-60.dll swscale-9.dll dav1d.dll libdav1d-7.dll libiconv-2.dll zlib1.dll libwinpthread-1.dll libva.dll libva_win32.dll opus.dll discord-rpc.dll libcrypto-3-x64.dll libssl-3-x64.dll libplacebo-360.dll) do (
    if exist "%DLLDIR%\%%F" (
        copy /y "%DLLDIR%\%%F" "%TEMP_DIR%\" >nul
        echo   %%F
    )
)

:: ---- 2. D3D11 shaders + data files ----
echo [pkg 2/5] Compiling + copying shaders and data files
::
:: §B-DUMP-BUILD 2026-05-07 — auto-recompile all .hlsl to .fxc before
:: staging, so shipped binary matches latest source.  Previously only
:: the manually-invoked build_hlsl.bat ran, letting .fxc drift behind
:: edits.  Delegated to compile_d3d11_shaders.ps1 because cmd batch
:: can't reliably escape /D macro args with embedded quotes.
::
where pwsh >nul 2>&1
if errorlevel 1 (
    powershell -ExecutionPolicy Bypass -File "%~dp0compile_d3d11_shaders.ps1"
) else (
    pwsh -ExecutionPolicy Bypass -File "%~dp0compile_d3d11_shaders.ps1"
)

::
:: IMPORTANT: GenericFRUC loads the _quality / _balanced / _performance
:: variants of motionest and warp (one per preset, compiled from
:: d3d11_motionest_compute.hlsl and d3d11_warp_compute.hlsl with different
:: QUALITY_LEVEL defines). If any of the six variants is missing at runtime,
:: GenericFRUC::initialize() logs "No FRUC backend available" and frame
:: interpolation is silently disabled. Ship them all.
::
for %%F in (d3d11_vertex.fxc d3d11_yuv420_pixel.fxc d3d11_ayuv_pixel.fxc d3d11_y410_pixel.fxc d3d11_overlay_pixel.fxc d3d11_motionest_compute.fxc d3d11_motionest_quality.fxc d3d11_motionest_balanced.fxc d3d11_motionest_performance.fxc d3d11_warp_compute.fxc d3d11_warp_quality.fxc d3d11_warp_balanced.fxc d3d11_warp_performance.fxc d3d11_mv_median.fxc d3d11_dml_pack_rgba8_fp16.fxc d3d11_dml_unpack_fp16_rgba8.fxc d3d11_fruc_blend_fp32.fxc) do (
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
::
:: §SLIM 2026-05-21 — release zip size trim:
::   - Drop `--compiler-runtime`: was pulling in vc_redist.x64.exe (~25 MB
::     packed) which is a user-facing installer, NOT a runtime dependency
::     of VipleStream.exe itself.  Modern Win10/11 ships VC++ 2015-2022
::     redist via Windows Update; users on older systems are pointed at
::     https://aka.ms/vs/17/release/vc_redist.x64.exe in README.
::   - `--no-opengl-sw`: skip opengl32sw.dll (~7 MB packed) Mesa software
::     OpenGL fallback.  Stream clients always have a GPU; the Qt Quick UI
::     uses the D3D11 RHI backend on Windows, not OpenGL, so this path
::     is never taken.
::   - `--no-virtualkeyboard`: we never embed Qt Virtual Keyboard.
::   - `--no-system-d3d-compiler`: we don't runtime-compile HLSL (all
::     D3D11 shaders ship as pre-compiled .fxc — see compile_d3d11_shaders.ps1)
::     and Qt Quick has no ShaderEffect items in our QML tree.
"%WINDEPLOYQT%" --release --qmldir "%SRC%\app\gui" --no-translations --no-compiler-runtime --no-opengl-sw --no-virtualkeyboard --no-system-d3d-compiler "%TEMP_DIR%\VipleStream.exe" 2>nul

::
:: §SLIM 2026-05-21 — prune unused Qt Quick Controls 2 styles.  main.cpp
:: line 1140 explicitly QQuickStyle::setStyle("Material"); the other
:: stock styles (Fusion / Imagine / Universal / FluentWinUI3) are never
:: instantiated.  windeployqt deploys them unconditionally for safety,
:: so we delete them post-hoc.  Total saving ~3 MB packed.
::
:: Basic style is kept because Qt Quick falls back to it when a Controls
:: type isn't overridden by the active style — removing Basic risks
:: subtle render glitches.  Windows native style impl is kept for same
:: reason (some Controls subclasses default to it).
echo [pkg 3b/5] Pruning unused QtQuick.Controls styles (Material-only)
for %%S in (Fusion Imagine Universal FluentWinUI3) do (
    if exist "%TEMP_DIR%\Qt6QuickControls2%%S.dll" del /f "%TEMP_DIR%\Qt6QuickControls2%%S.dll"
    if exist "%TEMP_DIR%\Qt6QuickControls2%%SStyleImpl.dll" del /f "%TEMP_DIR%\Qt6QuickControls2%%SStyleImpl.dll"
    if exist "%TEMP_DIR%\qml\QtQuick\Controls\%%S" rmdir /s /q "%TEMP_DIR%\qml\QtQuick\Controls\%%S"
)

:: ---- 4. DirectX runtime libs ----
::
:: §SLIM 2026-05-21 — dxcompiler.dll + dxil.dll removed (~7 MB packed).
:: These are the DXC (DirectX Shader Compiler) HLSL→DXIL toolchain used
:: for runtime HLSL 6.0+ compilation under D3D12.  Our codebase only uses
:: D3D11 with pre-compiled .fxc bytecode (compile_d3d11_shaders.ps1) and
:: never calls DxcCreateInstance / IDxcCompiler at runtime — grep across
:: moonlight-qt confirms no consumer (only this build script + docs
:: references them).  Were originally bundled defensively alongside the
:: DirectML / ORT-DML path but ORT-DML loads pre-compiled DML graphs via
:: D3D12 directly and likewise doesn't need DXC.
::
:: If a future renderer path actually starts using DXC, re-add by
:: dropping these lines back in.
:: echo [pkg 4/5] Copying DirectX runtime
:: for %%F in (dxcompiler.dll dxil.dll) do (
::     if exist "%WINSDK_D3D%\%%F" (
::         copy /y "%WINSDK_D3D%\%%F" "%TEMP_DIR%\" >nul
::         echo   %%F
::     )
:: )

:: VipleStream: ONNX Runtime DirectML DLL. Required at runtime ONLY
:: when the user picks the DirectML FRUC backend. Ship it unconditionally
:: so users can opt in without a separate download.
set "ORT_DLL=%SRC%\libs\windows\onnxruntime\runtimes\win-x64\native\onnxruntime.dll"
if exist "%ORT_DLL%" (
    copy /y "%ORT_DLL%" "%TEMP_DIR%\" >nul
    echo   onnxruntime.dll
) else (
    echo   [WARN] onnxruntime.dll missing - DirectML ONNX path disabled
)

:: VipleStream §G.4 — DirectML ONNX FRUC models 不再隨 release zip 出貨.
:: -------------------------------------------------------------------
:: 從 v1.3.311 起，下面三個 model 改為 DirectML backend 第一次 init 時
:: 透過 ModelFetcher (moonlight-qt/.../modelfetcher.cpp) 從 GitHub release
:: v1.3.310 attached assets 動態下載到
::   %LOCALAPPDATA%\VipleStream\fruc_models\
:: 並 sha-256 verify.  Release zip 因此縮小 ~39 MB (132 → ~93 MB).
::
:: 多數 user 走 NvOF / Generic / Vulkan-builtin FRUC backend，從沒踩到
:: DML path；既有設計就為這條路徑準備了 inline DML blend graph fallback，
:: 所以下載失敗 / 無網路也能跑（只是沒 RIFE 補幀）。
::
:: 三個 model:
::   tools/fruc.onnx        22 MB  RIFE v4.25 lite v2 fp32 (7-channel)
::   tools/fruc_fp16.onnx   11 MB  fp16 variant (Tensor Core 加速)
::   tools/fruc_ifrnet_s.onnx 5.5 MB IFRNet-S 較輕量 variant
::
:: dev 機本地仍可保留 tools/*.onnx，DirectMLFRUC 找 model 順序是先
:: install / data dirs 再 cache，dev build 就直接在原位跑不必 download.

:: VipleStream: NVIDIA Optical Flow FRUC helper DLL + its CUDA runtime
:: dependency. NvOFRUCWrapper LoadLibraryW's NvOFFRUC.dll from the exe
:: directory at runtime. NvOFFRUC.dll has a hard import on
:: cudart64_110.dll (CUDA 11 runtime), which normal NVIDIA driver
:: installs do NOT provide — so without shipping cudart alongside,
:: LoadLibrary returns ERROR_MOD_NOT_FOUND (126) even though the file
:: itself is present, and the NVIDIA Optical Flow backend silently
:: falls back to Generic. cudart64_110.dll is NVIDIA redistributable
:: per the CUDA Toolkit EULA, sourced from
:: developer.download.nvidia.com/compute/cuda/redist/cuda_cudart/
:: (cuda_cudart-windows-x86_64-11.8.89-archive.zip). Driver-level
:: NVOFA requirement still applies (Turing+ GPU with a recent
:: NVIDIA driver); nvcuda.dll + nvofapi64.dll ship with the driver.
set "NVOFFRUC_DLL=%SRC%\libs\windows\nvofa\lib\x64\NvOFFRUC.dll"
set "CUDART_DLL=%SRC%\libs\windows\nvofa\lib\x64\cudart64_110.dll"
if exist "%NVOFFRUC_DLL%" (
    copy /y "%NVOFFRUC_DLL%" "%TEMP_DIR%\" >nul
    echo   NvOFFRUC.dll
) else (
    echo   [WARN] NvOFFRUC.dll missing at %NVOFFRUC_DLL% - NVIDIA Optical Flow FRUC disabled
)

:: §J.3.e.2.i.8 Phase 1.6 - NVIDIA Nsight Aftermath SDK runtime DLL.
:: When linked, GFSDK_Aftermath_Lib.x64.dll must sit beside VipleStream.exe
:: (or anywhere on PATH).  Used to write GPU crash dump files when the
:: device goes into VK_ERROR_DEVICE_LOST so we can diagnose post-mortem
:: in Nsight Graphics.  Optional - if SDK is not present locally the lib
:: was not linked at compile time, so the DLL is unnecessary at runtime.
set "AFTERMATH_DLL=%SRC%\3rdparty\aftermath_sdk\lib\x64\GFSDK_Aftermath_Lib.x64.dll"
if exist "%AFTERMATH_DLL%" (
    copy /y "%AFTERMATH_DLL%" "%TEMP_DIR%\" >nul
    echo   GFSDK_Aftermath_Lib.x64.dll
) else (
    echo   [INFO] Aftermath SDK not present - GPU crash dump collection disabled
)
if exist "%CUDART_DLL%" (
    copy /y "%CUDART_DLL%" "%TEMP_DIR%\" >nul
    echo   cudart64_110.dll
) else (
    echo   [WARN] cudart64_110.dll missing at %CUDART_DLL% - NvOFFRUC.dll will fail to load
)

:: VipleStream v1.3.x: NCNN with Vulkan EP for the cross-vendor RIFE
:: backend.  Tencent ncnn-windows-vs2022-shared release.  Loaded
:: lazily by NcnnFRUC; if absent, FRUC cascade skips to DirectML /
:: Generic without crashing.  ncnn.dll dynamically links to
:: vulkan-1.dll (system-provided on Win10 1903+).
set "NCNN_DLL=%SRC%\libs\windows\ncnn\runtimes\win-x64\native\ncnn.dll"
if exist "%NCNN_DLL%" (
    copy /y "%NCNN_DLL%" "%TEMP_DIR%\" >nul
    echo   ncnn.dll
) else (
    echo   [WARN] ncnn.dll missing at %NCNN_DLL% - NCNN FRUC backend disabled
)

:: VipleStream v1.3.x: RIFE 4.25-lite NCNN model architecture.
:: §SLIM 2026-05-21 — only flownet.param (36 KB ascii) ships in the zip;
:: flownet.bin (11 MB fp16 weights) is now lazy-fetched by ModelFetcher
:: on first NCNN / Native-RIFE FRUC backend init.  See
:: moonlight-qt/app/streaming/video/ffmpeg-renderers/ncnnfruc.cpp
:: ensureRifeModelDir() — verifies SHA-256, caches under
:: %LOCALAPPDATA%\VipleStream\fruc_models\rife-v4.25-lite\flownet.bin.
:: Source: repo `raw` URL (file already committed to main).  One-time
:: ~3-5 s pause on broadband at first FRUC backend probe.  Cascade
:: degrades gracefully (NCNN / Native-RIFE off; Generic / NvOFFRUC /
:: DML continue) if the fetch fails (no network, offline).
::
:: To revert to bundling the .bin (e.g. for offline-install media),
:: uncomment the `copy ... flownet.bin` line below.
set "RIFE_NCNN_DIR=%SRC%\app\rife_models\rife-v4.25-lite"
if exist "%RIFE_NCNN_DIR%\flownet.param" (
    if not exist "%TEMP_DIR%\rife-v4.25-lite" mkdir "%TEMP_DIR%\rife-v4.25-lite"
    copy /y "%RIFE_NCNN_DIR%\flownet.param" "%TEMP_DIR%\rife-v4.25-lite\" >nul
    :: copy /y "%RIFE_NCNN_DIR%\flownet.bin"   "%TEMP_DIR%\rife-v4.25-lite\" >nul
    echo   rife-v4.25-lite/flownet.param  (flownet.bin lazy-fetched, see ensureRifeModelDir)
) else (
    echo   [WARN] RIFE 4.25-lite flownet.param missing at %RIFE_NCNN_DIR% - NCNN/Native-RIFE FRUC backend disabled
)

:: ---- 4b. Debug symbols (PDBs) → separate side-by-side zip ----
::
:: §SLIM 2026-05-21 — PDBs add ~9 MB packed to the main zip and are only
:: needed when symbolicating a minidump (a rare, dev-side operation).
:: Pack them into a separate `VipleStream-Client-X.Y.Z-debug.zip` next
:: to the main release zip.  WinDbg / cdb workflow:
::   1) unzip both client + debug zip into the SAME folder
::   2) PDBs end up alongside VipleStream.exe — `_NT_SYMBOL_PATH` finds
::      them automatically, same as before.
::
:: Statically-linked submodules (moonlight-common-c, h264bitstream,
:: qmdnsengine) merge their symbols into VipleStream.pdb at link time, so
:: VipleStream.pdb alone covers all our first-party code.  AntiHooking is
:: a separate DLL so its PDB lives in the debug zip too.
echo [pkg 4b/5] Staging PDBs into separate debug zip
set "PDB_STAGE=%TEMP_DIR%\..\moonlight_pdb"
if exist "%PDB_STAGE%" rmdir /s /q "%PDB_STAGE%"
mkdir "%PDB_STAGE%"
for %%F in ("%SRC%\app\release\VipleStream.pdb" "%SRC%\AntiHooking\release\AntiHooking.pdb") do (
    if exist %%F (
        copy /y %%F "%PDB_STAGE%\" >nul
        echo   %%~nxF (debug zip)
    ) else (
        echo   [WARN] %%~nxF missing - crash dumps will not symbolicate
    )
)

:: ---- 5. Zip it ----
echo [pkg 5/5] Creating zip
if not exist "%RELEASE%" mkdir "%RELEASE%"

set "OUT_ZIP=%RELEASE%\VipleStream-Client-%VER%.zip"
if exist "%OUT_ZIP%" del /f "%OUT_ZIP%"
"%SEVENZIP%" a -tzip -mx=9 -mmt=on "%OUT_ZIP%" "%TEMP_DIR%\*" >nul

set "OUT_PDB_ZIP=%RELEASE%\VipleStream-Client-%VER%-debug.zip"
if exist "%OUT_PDB_ZIP%" del /f "%OUT_PDB_ZIP%"
if exist "%PDB_STAGE%\*.pdb" (
    "%SEVENZIP%" a -tzip -mx=9 -mmt=on "%OUT_PDB_ZIP%" "%PDB_STAGE%\*" >nul
    for %%A in ("%OUT_PDB_ZIP%") do set "PDBSIZE=%%~zA"
    set /a "PDBSIZE_MB=!PDBSIZE! / 1048576"
)
rmdir /s /q "%PDB_STAGE%"

for %%A in ("%OUT_ZIP%") do set "ZIPSIZE=%%~zA"
set /a "ZIPSIZE_MB=!ZIPSIZE! / 1048576"

echo.
echo =========================================================
echo   VipleStream Client v%VER%
echo   %OUT_ZIP% (!ZIPSIZE_MB! MB)
if exist "%OUT_PDB_ZIP%" echo   %OUT_PDB_ZIP% (!PDBSIZE_MB! MB, debug symbols)
echo =========================================================

endlocal
