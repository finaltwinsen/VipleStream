@echo off
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
call "%VCVARS%" >nul 2>&1
"%WINDEPLOYQT%" --release --qmldir "%ROOT%\moonlight-qt\app\gui" --no-translations --compiler-runtime "%ROOT%\temp\moonlight\Moonlight.exe"
