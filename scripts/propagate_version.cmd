@echo off
:: Propagate current version to source files (no bump)
:: Result written to temp\current_version.txt
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not exist "%ROOT%\temp" mkdir "%ROOT%\temp"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\version.ps1" propagate > "%ROOT%\temp\current_version.txt"
endlocal
