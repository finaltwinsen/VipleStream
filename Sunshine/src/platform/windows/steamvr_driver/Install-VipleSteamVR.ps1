# ============================================================================
# §VR M1b：Install-VipleSteamVR.ps1（設計 §G、K21）——只診斷與手動移除，不做安裝
#   -Status：目前使用者 openvrpaths 裡的 viplestream 註冊、<install>\config\steamvr 的版本目錄、是否被提升執行
#   -Remove：以「目前使用者」身分 vrpathreg removedriver 每個 *\config\steamvr\*\viplestream 註冊。
#            偵測到自己被提升（TokenElevation）時拒絕：vrpathreg 與 Steam\config 都是使用者可寫的路徑，
#            要由使用者自己的非提升身分改（不變式 9）。vrserver 在跑時也拒絕（SteamVR 結束時會把設定寫回）。
#            guard（steamvr.vrsettings 的 forcedDriver 等）的逐鍵還原依 server 寫的 marker 進行，那是 V5 的編排器；
#            V4 沒有 marker，只提示。
# 安裝與註冊由 server（V5 編排器）以使用者 token 代做；V4 由本機操作手冊（scripts\vr\）做。
#
# 注意：本檔必須存成 UTF-8 含 BOM（PowerShell 5.1 會以 cp950 讀無 BOM 的檔）。
# ============================================================================
param(
    [switch]$Status,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

function Test-Elevated {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object System.Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-OpenVrPaths {
    $f = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    return (Get-Content -LiteralPath $f -Raw -Encoding UTF8 | ConvertFrom-Json)
}

function Get-OurRegistrations($paths) {
    if (-not $paths -or -not $paths.external_drivers) { return @() }
    return @($paths.external_drivers | Where-Object { $_ -match '\\config\\steamvr\\[^\\]+\\viplestream$' })
}

$install = Split-Path -Parent $PSScriptRoot   # <install>\steamvr\Install-VipleSteamVR.ps1 → <install>
$elevated = Test-Elevated
$paths = Get-OpenVrPaths
$regs = Get-OurRegistrations $paths

if (-not $Remove) {
    Write-Host "elevated=$elevated"
    Write-Host "openvrpaths=$([bool]$paths) runtime=$(if ($paths -and $paths.runtime) { $paths.runtime[0] } else { '-' })"
    Write-Host "registrations=$($regs.Count)"
    foreach ($r in $regs) { Write-Host "  $r" }
    $cfg = Join-Path $install 'config\steamvr'
    if (Test-Path -LiteralPath $cfg) {
        Get-ChildItem -LiteralPath $cfg -Directory | ForEach-Object { Write-Host "version-dir=$($_.Name)" }
    }
    exit 0
}

if ($elevated) {
    Write-Host '[ERROR] 請從「非提升」的 PowerShell 執行 -Remove（vrpathreg 與 Steam 設定要由使用者自己的身分修改）' -ForegroundColor Red
    exit 2
}
if (Get-Process -Name vrserver -ErrorAction SilentlyContinue) {
    Write-Host '[ERROR] SteamVR 正在執行：請先關閉 SteamVR 再移除' -ForegroundColor Red
    exit 3
}
if (-not $paths -or -not $paths.runtime) {
    Write-Host '[ERROR] 找不到 openvrpaths.vrpath 或 runtime' -ForegroundColor Red
    exit 4
}
$vrpathreg = Join-Path $paths.runtime[0] 'bin\win64\vrpathreg.exe'
foreach ($r in $regs) {
    & $vrpathreg removedriver $r
    Write-Host "removedriver $r exit=$LASTEXITCODE"
}
$left = Get-OurRegistrations (Get-OpenVrPaths)
Write-Host "registrations-left=$($left.Count)"
Write-Host 'guard：V4 沒有 marker；若 steamvr.vrsettings 的 steamvr.forcedDriver 仍是 viplestream，請手動移除該鍵'
exit $(if ($left.Count -eq 0) { 0 } else { 1 })
