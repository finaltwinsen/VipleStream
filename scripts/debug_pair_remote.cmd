@echo off
:: VipleStream Debug Pairing - REMOTE mode quick launcher
:: For VipleStream's standard dev setup:
::   - Phone (Pixel 5, serial <device-serial>) on this laptop's USB
::   - Sunshine on test host <user>@<host>
::
:: Pulls cert via local adb, SCP+SSH to host, injects + restarts service.
:: Requires OpenSSH client on PATH and key-based auth to host.
::
:: Usage:
::   debug_pair_remote.cmd              - Pair (default device name)
::   debug_pair_remote.cmd remove       - Remove the debug pairing
::   debug_pair_remote.cmd "MyPhone"    - Pair with custom device name
::
:: Override target host:
::   set REMOTE_HOST=other-user@other-host & debug_pair_remote.cmd

if not defined REMOTE_HOST set "REMOTE_HOST=<user>@<host>"

if /i "%~1"=="remove" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%" -Remove
) else if not "%~1"=="" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%" -DeviceName "%~1"
) else (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%"
)
pause
