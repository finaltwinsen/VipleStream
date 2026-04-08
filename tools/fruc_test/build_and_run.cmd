@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set INCLUDE=%INCLUDE%;D:\Mission\VipleStream\moonlight-qt\libs\windows\nvofa\include
cl /EHsc /O2 fruc_test.cpp /Fe:fruc_test.exe /link d3d11.lib dxgi.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo.
copy "C:\Program Files\Moonlight Game Streaming\NvOFFRUC.dll" . >nul 2>&1
copy "C:\Program Files\Moonlight Game Streaming\cudart64_110.dll" . >nul 2>&1
fruc_test.exe
