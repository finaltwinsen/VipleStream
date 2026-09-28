# ============================================================================
# §VR M1b：Import-VcVars.ps1
# 以 vswhere 找 Visual Studio（BuildTools 亦可）的 vcvars64.bat，把它設定的環境變數
# 匯入目前的 PowerShell 行程（env: 是行程層級，呼叫端直接看得到）。
# Build-SteamVRDriver.ps1 與 Stage-VipleSteamVR.ps1 共用；比照 Build-ScHidDriver.ps1:62-75。
#
# 用法（兩種都可以）：
#   & "$PSScriptRoot\Import-VcVars.ps1"               # 匯入；已在 x64 開發環境就略過
#   & "$PSScriptRoot\Import-VcVars.ps1" -SdkVersion 10.0.26100.0
# 成功時回傳一個物件（VsPath、VcVars、SdkVersion、Skipped）；失敗時 throw。
#
# 不需要 WDK、不需要管理員。只讀 vswhere 與 vcvars 的輸出，不寫任何檔案。
# 注意：本檔必須存成 UTF-8 含 BOM（PowerShell 5.1 會以 cp950 讀無 BOM 的檔，中文註解會壞）。
# ============================================================================
param(
    [string]$SdkVersion = ''
)

$ErrorActionPreference = 'Stop'

function Test-VcEnvReady {
    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') { return $false }
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    $link = Get-Command link.exe -ErrorAction SilentlyContinue
    return ($null -ne $cl) -and ($null -ne $link)
}

if ((Test-VcEnvReady) -and ([string]::IsNullOrEmpty($SdkVersion) -or $env:WindowsSDKVersion -eq "$SdkVersion\")) {
    return [pscustomobject]@{
        VsPath     = $env:VSINSTALLDIR
        VcVars     = ''
        SdkVersion = ($env:WindowsSDKVersion -replace '\$', '')
        Skipped    = $true
    }
}

$vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path -LiteralPath $vsWhere) {
    $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsPath -is [array]) { $vsPath = $vsPath[0] }
}
if ([string]::IsNullOrEmpty($vsPath)) {
    # vswhere 不在或找不到：退回 BuildTools 的預設位置（與 Build-ScHidDriver.ps1 相同）
    $vsPath = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "[VR-BUILD] 找不到 vcvars64.bat：$vcvars（需要 Visual Studio 2022 BuildTools 的 VC x64 工具）"
}

# cmd /c "vcvars64.bat [sdk] && set"：只取 NAME=VALUE 行匯入。vcvars 本身的訊息丟掉。
$vcArgs = ''
if (-not [string]::IsNullOrEmpty($SdkVersion)) { $vcArgs = " $SdkVersion" }
$lines = & cmd.exe /c "`"$vcvars`"$vcArgs >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "[VR-BUILD] vcvars64.bat 執行失敗（exit $LASTEXITCODE）"
}
foreach ($line in $lines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}
if (-not (Test-VcEnvReady)) {
    throw '[VR-BUILD] 匯入 vcvars64 後仍找不到 x64 的 cl.exe／link.exe'
}

return [pscustomobject]@{
    VsPath     = $vsPath
    VcVars     = $vcvars
    SdkVersion = ($env:WindowsSDKVersion -replace '\$', '')
    Skipped    = $false
}
