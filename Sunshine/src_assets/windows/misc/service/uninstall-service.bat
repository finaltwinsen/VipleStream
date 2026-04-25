@echo off
setlocal enabledelayedexpansion

set SERVICE_NAME=VipleStreamServer
set "SERVICE_CONFIG_DIR=%LOCALAPPDATA%\VipleStream"
set "SERVICE_CONFIG_FILE=%SERVICE_CONFIG_DIR%\service_start_type.txt"

rem Save the current service start type to a file if the service exists
sc qc %SERVICE_NAME% >nul 2>&1
if %ERRORLEVEL%==0 (
    if not exist "%SERVICE_CONFIG_DIR%\" mkdir "%SERVICE_CONFIG_DIR%\"

    rem Get the start type
    for /f "tokens=3" %%i in ('sc qc %SERVICE_NAME% ^| findstr /C:"START_TYPE"') do (
        set "CURRENT_START_TYPE=%%i"
    )

    rem Set the content to write
    if "!CURRENT_START_TYPE!"=="2" (
        sc qc %SERVICE_NAME% | findstr /C:"(DELAYED)" >nul
        if !ERRORLEVEL!==0 (
            set "CONTENT=2-delayed"
        ) else (
            set "CONTENT=2"
        )
    ) else if "!CURRENT_START_TYPE!" NEQ "" (
        set "CONTENT=!CURRENT_START_TYPE!"
    ) else (
        set "CONTENT=unknown"
    )

    rem Write content to file
    echo !CONTENT!> "%SERVICE_CONFIG_FILE%"
)

rem Stop and delete legacy services from upstream Sunshine packaging
net stop sunshinesvc 2>nul
sc delete sunshinesvc 2>nul
net stop SunshineService 2>nul
sc delete SunshineService 2>nul

rem Stop and delete the VipleStream service
net stop %SERVICE_NAME% 2>nul
sc delete %SERVICE_NAME% 2>nul
