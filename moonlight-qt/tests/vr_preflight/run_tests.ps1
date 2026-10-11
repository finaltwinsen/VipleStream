# §VR-PREFLIGHT（開 VR 串流前的往返時間檢查）的測試——Windows 開發機用。
#
# 用法（在任何目錄都可以）：
#   pwsh moonlight-qt\tests\vr_preflight\run_tests.ps1
#   pwsh moonlight-qt\tests\vr_preflight\run_tests.ps1 -QtDir <Qt 的 msvc 目錄> -VcVars <vcvars64.bat> -OutDir <目錄> -Gxx <g++.exe>
#
# 五組，任何一組失敗結束碼就不是 0：
#   1. logic        test_logic.cpp：判斷邏輯（app\backend\vrpreflight.h），只用標準函式庫、時間是假的。
#                   MSVC 一定跑；找得到 g++（-Gxx，或 MSYS2 UCRT64 的預設位置）就再用 g++ 跑一次
#                   （頭盔上的建置用的是 GCC）。
#   2. check        test_check.cpp：真的 VrPreflightCheck／VrRttProbe／VrLauncher，對本機的 TCP listener 量；
#                   太慢的那幾條路用 --vr-test-preflight 的腳本給往返時間。Qt Core＋Network，這裡自己跑 moc。約 30 秒。
#   3. qml-dialog   qml\tst_preflight_dialog.qml：真的 VrPreflightDialog.qml（VrLauncher 是替身）。約 25 秒。
#   4. qml-appview  qml\tst_appview_launch.qml：真的 AppView.qml 開 VR 串流的三條路（C++ 的型別都是替身）。
#   5. qml-zh       qml\zh_TW\tst_preflight_dialog_zh.qml：對話框載入 app\languages\qml_zh_TW.qm 之後的字與版面。
#   QML 的三組用 qmltestrunner -platform offscreen，不開視窗。
#
# 只編這個目錄的測試程式（另外編進 app\backend 的 vrpreflightcheck.cpp、vrlauncher.cpp 兩個檔），產物全部放 -OutDir
# （預設在系統暫存目錄），不寫進 repo，也不碰 temp\、release\；跟專案的 build script 無關。
#
# 別的平台：logic 那一組可以直接編——
#   g++ -std=c++17 -Wall -Wextra -o test_logic test_logic.cpp && ./test_logic
# check 與 qml 兩組目前只有這支腳本（Windows＋MSVC＋Qt 的 msvc 套件）跑過。

param(
    [string]$QtDir = "C:\Qt\6.10.3\msvc2022_64",
    [string]$VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "viple-vr-preflight-tests"),
    [string]$Gxx = "C:\msys64\ucrt64\bin\g++.exe"
)

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$qtRoot = (Resolve-Path (Join-Path $here "..\..")).Path          # moonlight-qt
$backend = Join-Path $qtRoot "app\backend"

foreach ($need in @("$QtDir\bin\moc.exe", "$QtDir\bin\qmltestrunner.exe", $VcVars)) {
    if (-not (Test-Path $need)) {
        Write-Host "[ERROR] not found: $need (see the parameters at the top of this script)"
        exit 2
    }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null

$results = [ordered]@{}

# 在 vcvars 的環境裡跑一行命令（寫成暫時的 .cmd，避開巢狀引號）
function Invoke-Msvc([string]$commandLine) {
    $cmdFile = Join-Path $OutDir "msvc_step.cmd"
    Set-Content -Path $cmdFile -Encoding ascii -Value @(
        "@echo off",
        "call `"$VcVars`" >nul 2>&1",
        "if errorlevel 1 exit /b 90",
        $commandLine
    )
    # 輸出直接給主控台：留在管線裡會變成這個函式回傳值的一部分
    cmd /c "`"$cmdFile`"" | Out-Host
    return $LASTEXITCODE
}

# ---- 1. logic ----
Write-Host "=== logic (MSVC) ==="
$rc = Invoke-Msvc "cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 /O2 /Fo`"$OutDir\test_logic.obj`" /Fe`"$OutDir\test_logic_msvc.exe`" `"$here\test_logic.cpp`""
if ($rc -eq 0) {
    & "$OutDir\test_logic_msvc.exe"
    $rc = $LASTEXITCODE
}
$results["logic (MSVC)"] = $rc

if (Test-Path $Gxx) {
    Write-Host "=== logic (g++) ==="
    $savedPath = $env:PATH
    $env:PATH = (Split-Path $Gxx) + ";" + $env:PATH               # g++ 要找得到自己的 DLL 與 as／ld
    & $Gxx -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -o "$OutDir\test_logic_gcc.exe" "$here\test_logic.cpp"
    $rc = $LASTEXITCODE
    if ($rc -eq 0) {
        & "$OutDir\test_logic_gcc.exe"
        $rc = $LASTEXITCODE
    }
    $env:PATH = $savedPath
    $results["logic (g++)"] = $rc
}
else {
    Write-Host "=== logic (g++): skipped, $Gxx not found ==="
}

# ---- 2. check ----
Write-Host "=== check (MSVC + Qt) ==="
& "$QtDir\bin\moc.exe" "$backend\vrpreflightcheck.h" -o "$OutDir\moc_vrpreflightcheck.cpp"
$rc = $LASTEXITCODE
if ($rc -eq 0) {
    & "$QtDir\bin\moc.exe" "$backend\vrlauncher.h" -o "$OutDir\moc_vrlauncher.cpp"
    $rc = $LASTEXITCODE
}
if ($rc -eq 0) {
    # stubs 排最前面：vrpreflightcheck.cpp、vrlauncher.cpp 的 #include "SDL_compat.h" 會拿到測試用的替身
    $includes = "/I `"$here\stubs`" /I `"$qtRoot\app`" /I `"$backend`" /I `"$QtDir\include`" /I `"$QtDir\include\QtCore`" /I `"$QtDir\include\QtNetwork`""
    $sources = "`"$here\test_check.cpp`" `"$backend\vrpreflightcheck.cpp`" `"$backend\vrlauncher.cpp`" `"$OutDir\moc_vrpreflightcheck.cpp`" `"$OutDir\moc_vrlauncher.cpp`""
    # 這裡不加 /WX：換一版 Qt 時標頭自己的警告不該讓測試編不過（警告照樣會印出來）
    $rc = Invoke-Msvc "cl /nologo /std:c++17 /permissive- /Zc:__cplusplus /EHsc /MD /utf-8 /W4 /O2 $includes /Fo`"$OutDir\\`" /Fe`"$OutDir\test_check.exe`" $sources /link /LIBPATH:`"$QtDir\lib`" Qt6Core.lib Qt6Network.lib"
}
if ($rc -eq 0) {
    $savedPath = $env:PATH
    $env:PATH = "$QtDir\bin;" + $env:PATH
    & "$OutDir\test_check.exe" $qtRoot
    $rc = $LASTEXITCODE
    $env:PATH = $savedPath
}
$results["check"] = $rc

# ---- 3、4. qml ----
function Invoke-QmlTest([string]$name, [string]$testFile, [string[]]$extraArgs) {
    Write-Host "=== $name ==="
    $log = Join-Path $OutDir "$name.txt"
    $argList = @("-platform", "offscreen", "-input", "`"$testFile`"", "-import", "`"$here\qml\imports`"", "-o", "`"$log,txt`"") + $extraArgs
    $saved = @{}
    foreach ($name in @("QT_QUICK_CONTROLS_STYLE", "QT_QUICK_CONTROLS_MATERIAL_THEME", "QT_QPA_FONTDIR")) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
    }
    # 樣式與深色主題跟產品一樣（main.cpp）
    $env:QT_QUICK_CONTROLS_STYLE = "Material"
    $env:QT_QUICK_CONTROLS_MATERIAL_THEME = "Dark"
    # offscreen 平台自己沒有字型：不指定的話每個字都畫成一個方框、寬度是一個字級，量出來的版面跟實際的差很多
    # （英文字會寬一倍）。指到系統字型，換行與寬度的檢查才有意義
    if (-not $env:QT_QPA_FONTDIR -and (Test-Path "$env:WINDIR\Fonts")) {
        $env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"
    }
    $p = Start-Process -FilePath "$QtDir\bin\qmltestrunner.exe" -ArgumentList $argList -NoNewWindow -Wait -PassThru `
                       -RedirectStandardError "$log.stderr"
    foreach ($name in $saved.Keys) {
        [Environment]::SetEnvironmentVariable($name, $saved[$name])
    }
    if (Test-Path $log) {
        Get-Content $log | Out-Host
    }
    if ((Test-Path "$log.stderr") -and (Get-Item "$log.stderr").Length -gt 0) {
        Write-Host "--- stderr ---"
        Get-Content "$log.stderr" | Out-Host
    }
    return $p.ExitCode
}

$results["qml-dialog"] = Invoke-QmlTest "qml-dialog" "$here\qml\tst_preflight_dialog.qml" @()
$results["qml-appview"] = Invoke-QmlTest "qml-appview" "$here\qml\tst_appview_launch.qml" @()
$results["qml-zh"] = Invoke-QmlTest "qml-zh" "$here\qml\zh_TW\tst_preflight_dialog_zh.qml" `
                                    @("-translation", "`"$qtRoot\app\languages\qml_zh_TW.qm`"")

# ---- 總結 ----
Write-Host "=== summary ==="
$failed = 0
foreach ($name in $results.Keys) {
    $rc = $results[$name]
    Write-Host ("{0,-14} {1}" -f $name, $(if ($rc -eq 0) { "PASS" } else { "FAIL (rc=$rc)" }))
    if ($rc -ne 0) {
        $failed++
    }
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
