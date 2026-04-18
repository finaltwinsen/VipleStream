@echo off
:: VipleStream Build Configuration
:: Copy this file to build-config.local.cmd and adjust paths for your machine.
:: build-config.local.cmd is in .gitignore and will NOT be committed.

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

set "MSYS2=C:\msys64\usr\bin\bash.exe"
set "SEVENZIP=C:\Program Files\7-Zip\7z.exe"
set "QT_DIR=C:\Qt\6.10.3\msvc2022_64"
set "WINDEPLOYQT=%QT_DIR%\bin\windeployqt.exe"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "WINSDK_D3D=C:\Program Files (x86)\Windows Kits\10\Redist\D3D\x64"
set "DEPLOY_CLIENT=C:\Program Files\Moonlight Game Streaming"
set "DEPLOY_SERVER=C:\Program Files\Sunshine"
