@echo off
:: Compile all custom shaders
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
set "SHADERS=%ROOT%\moonlight-qt\app\shaders"
set "FXC=fxc.exe"

for %%S in (d3d11_bicubic_scale_pixel d3d11_yuv420_bicubic_pixel) do (
    echo Compiling %%S (ps_5_0)...
    "%FXC%" /T ps_5_0 /O3 /Fo "%SHADERS%\%%S.fxc" "%SHADERS%\%%S.hlsl"
    if errorlevel 1 (echo   FAILED) else (echo   OK)
)

:: FRUC compute shaders — 3 quality levels each (6 .fxc total)
echo.
echo === FRUC Multi-Quality Shader Compilation ===

for %%Q in (0 1 2) do (
    if %%Q==0 (set "QNAME=quality") else if %%Q==1 (set "QNAME=balanced") else (set "QNAME=performance")

    echo Compiling motionest_!QNAME! (cs_5_0, QUALITY_LEVEL=%%Q)...
    "%FXC%" /T cs_5_0 /O1 /D QUALITY_LEVEL=%%Q /Fo "%SHADERS%\d3d11_motionest_!QNAME!.fxc" "%SHADERS%\d3d11_motionest_compute.hlsl"
    if errorlevel 1 (echo   FAILED) else (echo   OK)

    echo Compiling warp_!QNAME! (cs_5_0, QUALITY_LEVEL=%%Q)...
    "%FXC%" /T cs_5_0 /O1 /D QUALITY_LEVEL=%%Q /Fo "%SHADERS%\d3d11_warp_!QNAME!.fxc" "%SHADERS%\d3d11_warp_compute.hlsl"
    if errorlevel 1 (echo   FAILED) else (echo   OK)
)

:: MV field median filter (no quality variants — deterministic).
echo.
echo Compiling mv_median (cs_5_0)...
"%FXC%" /T cs_5_0 /O1 /Fo "%SHADERS%\d3d11_mv_median.fxc" "%SHADERS%\d3d11_mv_median.hlsl"
if errorlevel 1 (echo   FAILED) else (echo   OK)

:: DirectML backend pack/unpack (D3D11 compute shaders that bridge
:: the RGBA8 render texture and the shared planar-FP16 tensor
:: buffer read/written by DML).
echo.
echo Compiling dml_pack_rgba8_fp16 (cs_5_0)...
"%FXC%" /T cs_5_0 /O1 /Fo "%SHADERS%\d3d11_dml_pack_rgba8_fp16.fxc" "%SHADERS%\d3d11_dml_pack_rgba8_fp16.hlsl"
if errorlevel 1 (echo   FAILED) else (echo   OK)
echo Compiling dml_unpack_fp16_rgba8 (cs_5_0)...
"%FXC%" /T cs_5_0 /O1 /Fo "%SHADERS%\d3d11_dml_unpack_fp16_rgba8.fxc" "%SHADERS%\d3d11_dml_unpack_fp16_rgba8.hlsl"
if errorlevel 1 (echo   FAILED) else (echo   OK)
