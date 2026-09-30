# VipleStream 2.0 §VR M3a X6 — S2：Windows 本機 SteamVR null driver（dev-only）
#
#   pwsh xr-s2-steamvr-null.ps1 -Mode on|start|status|off [-SteamDir <Steam 安裝目錄>]
#
#   on      先停 SteamVR，備份 steamvr.vrsettings（只在第一次 on 時備份），再寫入 null driver 設定：
#           driver_null（1344x1344、90 Hz）、forcedDriver=null、activateMultipleDrivers、requireHmd=false、
#           關閉 standby（null HMD 不會動，約 12 s 就 standby，session 退回 SYNCHRONIZED）、
#           停用 dashboard（沒有控制器時 dashboard 搶走輸入焦點，session 停在 VISIBLE）
#   start   完整啟動 SteamVR 並等 vrcompositor 出現。OpenXR app 自己只會拉起 vrserver（等不到
#           vrmonitor 20 s 後自關、沒有 compositor 時 xrGetVulkanGraphicsDevice2KHR 回 RUNTIME_FAILURE）
#   status  顯示備份是否存在與目前的 forcedDriver 等設定
#   off     停 SteamVR，以備份覆蓋還原 steamvr.vrsettings（之後刪除備份）
#
# 只改本機（Steam 目錄下）的 steamvr.vrsettings，不改系統的 OpenXR active runtime（HKLM）。
# 驗測時用 VipleStream 的 dev 參數 --xr-runtime-json 指向 SteamVR 的 steamxr_win64.json。
# SteamVR 會跳出「未設為 OpenXR 預設 runtime」的通知，不影響驗測，不要按「設為預設」。
param(
    [ValidateSet('on', 'off', 'status', 'start')][string]$Mode = 'status',
    [string]$SteamDir = 'C:\Program Files (x86)\Steam',
    [switch]$Help
)

if ($Help) {
    Get-Content $PSCommandPath | Select-Object -First 20 | ForEach-Object { $_ -replace '^# ?', '' }
    exit 0
}

$f   = Join-Path $SteamDir 'config\steamvr.vrsettings'
$bak = Join-Path $env:LOCALAPPDATA 'VipleStream\s2\steamvr.vrsettings.bak'
$startup = Join-Path $SteamDir 'steamapps\common\SteamVR\bin\win64\vrstartup.exe'

if (-not (Test-Path $f)) { Write-Error "找不到 $f（-SteamDir？）"; exit 2 }

function Stop-VR {
    foreach ($n in 'vrmonitor', 'vrserver', 'vrcompositor', 'vrdashboard', 'vrwebhelper') {
        Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep 2
}

function Set-Prop($obj, [string]$name, $value) {
    $obj | Add-Member -Force -NotePropertyName $name -NotePropertyValue $value
}

switch ($Mode) {
    'on' {
        Stop-VR
        New-Item -ItemType Directory -Force (Split-Path $bak) | Out-Null
        if (-not (Test-Path $bak)) { Copy-Item $f $bak }
        $j = Get-Content $f -Raw | ConvertFrom-Json
        $null_ = [pscustomobject]@{
            enable = $true; serialNumber = 'VIPLE-S2'; modelNumber = 'VipleS2'
            windowX = 0; windowY = 0; windowWidth = 1344; windowHeight = 720
            renderWidth = 1344; renderHeight = 1344; secondsFromVsyncToPhotons = 0.01; displayFrequency = 90
        }
        Set-Prop $j 'driver_null' $null_
        Set-Prop $j.steamvr 'forcedDriver' 'null'
        Set-Prop $j.steamvr 'activateMultipleDrivers' $true
        Set-Prop $j.steamvr 'requireHmd' $false
        $pw = if ($j.PSObject.Properties.Name -contains 'power') { $j.power } else { [pscustomobject]@{} }
        Set-Prop $pw 'turnOffScreensTimeout' 86400
        Set-Prop $pw 'pauseCompositorOnStandby' $false
        Set-Prop $j 'power' $pw
        $db = if ($j.PSObject.Properties.Name -contains 'dashboard') { $j.dashboard } else { [pscustomobject]@{} }
        Set-Prop $db 'enableDashboard' $false
        Set-Prop $j 'dashboard' $db
        $j | ConvertTo-Json -Depth 8 | Set-Content $f -Encoding UTF8
        "S2 on（備份：$bak）"
    }
    'start' {
        if (-not (Get-Process vrcompositor -ErrorAction SilentlyContinue)) {
            Start-Process $startup
            $t = 0
            while (-not (Get-Process vrcompositor -ErrorAction SilentlyContinue) -and $t -lt 60) { Start-Sleep 1; $t++ }
            Start-Sleep 5
        }
        "SteamVR running: compositor=$([bool](Get-Process vrcompositor -ErrorAction SilentlyContinue))"
    }
    'off' {
        Stop-VR
        if (Test-Path $bak) { Copy-Item $bak $f -Force; Remove-Item $bak; 'S2 off（已還原）' } else { '沒有備份，未還原任何東西' }
    }
    'status' {
        "backup exists: $(Test-Path $bak)"
        (Get-Content $f -Raw | ConvertFrom-Json).steamvr | Select-Object forcedDriver, activateMultipleDrivers, requireHmd
    }
}
