@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >/dev/null 2>&1
set "INCLUDE=%INCLUDE%;D:\Mission\VipleStream\moonlight-qt\libs\windows\nvofa\include"
cd /d "D:\Mission\VipleStream\tools\fruc_test"
cl /EHsc /O2 fruc_test.cpp /Fe:fruc_test.exe /link d3d11.lib dxgi.lib
if errorlevel 1 echo BUILD FAILED
