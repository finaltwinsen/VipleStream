@echo off
:: VipleStream Debug Pairing - Quick launcher (LOCAL mode)
:: Requires: ADB device + Sunshine installed on THIS machine
::
:: Usage:
::   debug_pair.cmd              - Pair the connected phone
::   debug_pair.cmd remove       - Remove the debug pairing
::   debug_pair.cmd "MyPhone"    - Pair with custom device name
::
:: For dev setup with phone on laptop + Sunshine on remote host, see
:: debug_pair_remote.cmd instead.

if /i "%~1"=="remove" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -Remove
) else if not "%~1"=="" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -DeviceName "%~1"
) else (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1"
)
pause
