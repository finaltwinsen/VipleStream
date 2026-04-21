@echo off
setlocal enabledelayedexpansion

:: =============================================================================
::  VipleStream Moonlight-Android - Build + Package
::
::  Syncs the Android app's versionName to this repo's shared version.json
::  (same version number as the Qt/desktop build), assembles a debug APK,
::  and drops it into release/ as VipleStream-Android-<X.Y.Z>.apk.
::
::  Debug (not release) because:
::    - release requires a signing keystore that only the official Moonlight
::      maintainer has. Debug signs with the locally-generated debug cert,
::      installs on any device via adb or sideload.
::    - The user's ask was "編譯後安卓版同樣加上版號控制" (version-stamp the
::      Android build and ship it to release/), regardless of debug/release.
::
::  Prereqs (taken from your environment):
::    - ANDROID_HOME or ANDROID_SDK_ROOT, or SDK at %LOCALAPPDATA%\Android\Sdk
::    - NDK at %ANDROID_HOME%\ndk\<ndkVersion from build.gradle>
::    - Java / gradle wrapper (ships with moonlight-android/gradlew.bat)
:: =============================================================================

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "SRC=%ROOT%\moonlight-android"
set "RELEASE=%ROOT%\release"

echo =========================================================
echo   VipleStream Moonlight-Android - Build
echo =========================================================

:: -- 0a. Resolve Android SDK so local.properties isn't needed --
if not defined ANDROID_HOME (
    if defined ANDROID_SDK_ROOT (
        set "ANDROID_HOME=%ANDROID_SDK_ROOT%"
    ) else if exist "%LOCALAPPDATA%\Android\Sdk" (
        set "ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk"
    )
)
if not defined ANDROID_HOME (
    echo [ERROR] Android SDK not found. Set ANDROID_HOME or install Android Studio.
    exit /b 1
)
echo Android SDK: %ANDROID_HOME%

:: -- 0b. Resolve a JDK so gradlew doesn't bail with "JAVA_HOME not set" --
if not defined JAVA_HOME (
    for /d %%D in ("C:\Program Files\Microsoft\jdk-17*" ^
                   "C:\Program Files\Microsoft\jdk-21*" ^
                   "C:\Program Files\Eclipse Adoptium\jdk-17*" ^
                   "C:\Program Files\Eclipse Adoptium\jdk-21*" ^
                   "C:\Program Files\Java\jdk-17*" ^
                   "C:\Program Files\Java\jdk-21*" ^
                   "C:\Program Files\Android\Android Studio\jbr") do (
        if exist "%%D\bin\java.exe" set "JAVA_HOME=%%D"
    )
)
if not defined JAVA_HOME (
    echo [ERROR] JDK not found. Install JDK 17+ or set JAVA_HOME.
    exit /b 1
)
echo JDK: %JAVA_HOME%
set "PATH=%JAVA_HOME%\bin;%PATH%"

:: Gradle honours org.gradle.project.<name> as a project property. Passing
:: -Psdk.dir avoids needing to write local.properties (which is gitignored).
set "GRADLE_USER_PROPS=-Psdk.dir=%ANDROID_HOME%"

:: -- 1. Propagate shared version.json into all three targets, including
::     moonlight-android\app\build.gradle.  Uses the central propagator
::     in scripts\version.ps1 so the Sunshine / moonlight-qt / Android
::     versions can never drift apart here.
echo [1/4] Syncing version (propagate from version.json)...
for /f "delims=" %%V in ('powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\version.ps1" propagate') do (
    set "VER=%%V"
)
if not defined VER (
    echo [ERROR] Failed to propagate version
    exit /b 1
)
echo   versionName = %VER%

:: -- 2. Run gradle assembleDebug --
echo [2/4] Running gradlew assembleDebug...
pushd "%SRC%"
call "%SRC%\gradlew.bat" %GRADLE_USER_PROPS% --console=plain assembleDebug
set "GRADLE_RC=%ERRORLEVEL%"
popd
if not "%GRADLE_RC%"=="0" (
    echo [ERROR] gradlew failed: %GRADLE_RC%
    exit /b %GRADLE_RC%
)

:: -- 3. Locate and copy the APK --
echo [3/4] Collecting APK...
set "APK=%SRC%\app\build\outputs\apk\debug\app-debug.apk"
if not exist "%APK%" (
    echo [ERROR] APK not found at %APK%
    exit /b 1
)
if not exist "%RELEASE%" mkdir "%RELEASE%"
set "OUT_APK=%RELEASE%\VipleStream-Android-%VER%.apk"
copy /y "%APK%" "%OUT_APK%" >nul
if not exist "%OUT_APK%" (
    echo [ERROR] Failed to copy APK to %OUT_APK%
    exit /b 1
)

:: -- 4. Report --
for %%A in ("%OUT_APK%") do set /a "APK_MB=%%~zA / 1048576"
echo.
echo =========================================================
echo   VipleStream Android v%VER%
echo   %OUT_APK% (%APK_MB% MB)
echo =========================================================
echo Install on a connected device with:
echo   adb install -r "%OUT_APK%"

endlocal
