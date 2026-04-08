@echo off
:: Compile all custom shaders
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
set "SHADERS=%ROOT%\moonlight-qt\app\shaders"
set "FXC=fxc.exe"

for %%S in (d3d11_bicubic_scale_pixel d3d11_yuv420_bicubic_pixel) do (
    echo Compiling %%S...
    "%FXC%" /T ps_5_0 /O3 /Fo "%SHADERS%\%%S.fxc" "%SHADERS%\%%S.hlsl"
    if errorlevel 1 (echo   FAILED) else (echo   OK)
)
