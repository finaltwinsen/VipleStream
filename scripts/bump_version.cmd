@echo off
:: Bump patch version and propagate to source files
:: Result written to temp\current_version.txt
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not exist "%ROOT%\temp" mkdir "%ROOT%\temp"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\version.ps1" bump > "%ROOT%\temp\current_version.txt"
endlocal
