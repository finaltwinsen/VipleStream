@echo off
:: VipleStream Debug Pairing - Quick launcher
:: Requires: ADB connected Android device + Sunshine installed
::
:: Usage:
::   debug_pair.cmd              - Pair the connected phone
::   debug_pair.cmd remove       - Remove the debug pairing
::   debug_pair.cmd "MyPhone"    - Pair with custom device name

if /i "%~1"=="remove" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -Remove
) else if not "%~1"=="" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -DeviceName "%~1"
) else (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1"
)
pause
