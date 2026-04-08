@echo off
if exist "%~dp0..\build-config.local.cmd" call "%~dp0..\build-config.local.cmd"
if not defined ROOT set "ROOT=%~dp0.."
call "%VCVARS%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] MSVC setup failed
    exit /b 1
)

set "PATH=%QT_DIR%\bin;%PATH%"

cd /d "%ROOT%\moonlight-qt"

echo Cleaning stale Makefiles for version refresh...
del /q app\Makefile app\Makefile.Release app\Makefile.Debug Makefile 2>nul

echo Running qmake...
qmake moonlight-qt.pro CONFIG+=release
if errorlevel 1 (
    echo [ERROR] qmake failed
    exit /b 1
)

echo Building with nmake...
nmake release
if errorlevel 1 (
    echo [ERROR] nmake failed
    exit /b 1
)

echo BUILD SUCCESS
