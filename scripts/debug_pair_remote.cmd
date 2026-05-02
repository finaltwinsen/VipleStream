@echo off
:: VipleStream Debug Pairing - REMOTE mode quick launcher.
::
:: Pulls cert via local adb, SCP+SSH to host, injects + restarts service.
:: Requires OpenSSH client on PATH and key-based auth to the host.
::
:: Set the target host once via the REMOTE_HOST env var (e.g. add to your
:: build-config.local.cmd or set it in the shell before invoking).  No
:: default is baked in; this is a per-developer setting.
::
:: Usage:
::   set REMOTE_HOST=user@your-host.lan
::   debug_pair_remote.cmd              - Pair (default device name)
::   debug_pair_remote.cmd remove       - Remove the debug pairing
::   debug_pair_remote.cmd "MyPhone"    - Pair with custom device name

if not defined REMOTE_HOST (
    echo [ERROR] REMOTE_HOST is not set.
    echo         Example: set REMOTE_HOST=user@your-host.lan
    exit /b 1
)

if /i "%~1"=="remove" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%" -Remove
) else if not "%~1"=="" (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%" -DeviceName "%~1"
) else (
    powershell -ExecutionPolicy Bypass -File "%~dp0debug_pair.ps1" -RemoteHost "%REMOTE_HOST%"
)
pause
