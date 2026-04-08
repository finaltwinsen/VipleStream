@echo off
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
if not defined DEPLOY_CLIENT set "DEPLOY_CLIENT=C:\Program Files\Moonlight Game Streaming"
echo Deploying VipleStream Client...
xcopy /s /e /q /y "%ROOT%\temp\moonlight\*" "%DEPLOY_CLIENT%\" >nul
echo Done.
pause
