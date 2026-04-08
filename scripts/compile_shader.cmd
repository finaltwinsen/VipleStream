@echo off
:: Compile a single HLSL shader to FXC
:: Usage: compile_shader.cmd <name> (e.g. d3d11_yuv420_bicubic_pixel)
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
set "SHADERS=%ROOT%\moonlight-qt\app\shaders"
set "FXC=fxc.exe"
"%FXC%" /T ps_5_0 /O3 /Fo "%SHADERS%\%~1.fxc" "%SHADERS%\%~1.hlsl"
if errorlevel 1 (
    echo SHADER COMPILE FAILED
) else (
    echo SHADER COMPILE OK
)
