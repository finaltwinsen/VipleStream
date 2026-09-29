# ============================================================================
# §VR M1b：Build-SteamVRDriver.ps1
# 以 MSVC（cl／link 直呼，不用 msbuild）建：
#   -Target driver：driver_viplestream.dll（SteamVR driver，/MT 靜態 CRT）
#   -Target probe ：vr_probe.exe（Sunshine\tools\vr_probe，不出貨；連同一份 ipc_client.cpp）
#   -Target all   ：兩者（預設）；driver_main.cpp 還不存在（V4 之前）時只建 vr_probe，-Target driver 則直接失敗
# 設計依據：M1b 設計 §G、§E.1、§F.4。沒有 PoC 旗標；不需要 WDK、不需要管理員。
#
# 輸出：
#   out\x64\<Config>\driver_viplestream.dll（+ .pdb）
#   out\x64\<Config>\vr_probe\{vr_probe.exe, openvr_api.dll}（+ vr_probe.pdb、dependents.txt）
#   <repo>\temp\vr_probe\VipleStream-VrProbe-dev.zip（只在 .195 ↔ host 之間 scp，不進 release）
#
# 編譯規則（§G）：
#   - vendored 的 OpenVR 標頭必須原樣保存，它的預設實作會觸發 C4100，所以只能以
#     /external:I + /external:W0 隔離；我們自己的檔（含移植檔）一律 /W4 /WX，警告直接修。
#   - driver 不可用 std::atomic::wait／std::latch／std::counting_semaphore／std::barrier
#     （會匯入 api-ms-win-core-synch-*）；dumpbin /dependents 白名單在這裡先擋一次，Stage 再擋一次。
#   - ipc_client.cpp 的異常注入掛鉤只在 probe 以 /DVRDRV_PEER_HOOKS 編譯；driver 沒有那些程式碼路徑。
#
# 注意：本檔必須存成 UTF-8 含 BOM（PowerShell 5.1 會以 cp950 讀無 BOM 的檔）。
# ============================================================================
param(
    [ValidateSet('driver', 'probe', 'all')]
    [string]$Target = 'all',
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host ''
    Write-Host "[VR-BUILD] [ERROR] $msg" -ForegroundColor Red
    exit 1
}

function Say([string]$msg) {
    Write-Host "[VR-BUILD] $msg"
}

# openvr_api.dll 的 FileVersion 是上游沒更新的 1.1.1，不能拿來驗版；一律比 sha256
# （Sunshine\third-party\openvr\README.VipleStream.md，v2.15.6）。
$OpenVrDllSha256 = 'bab8ac6ef64e68a9ca53315b0014d131088584b2efdfa6db511d67ec03cfcb4a'
$OpenVrLibSha256 = 'a0bf57c5920f569e8d21ab3e5bc95bac4b73e2016217f8b5b93495a2a7197bbb'

# driver DLL 的相依白名單（§G Stage 第 3 步）
$DriverDllWhitelist = @('KERNEL32.dll', 'ADVAPI32.dll', 'd3d11.dll', 'dxgi.dll')

# ── 路徑 ──────────────────────────────────────────────────────────────────────
$DrvDir = $PSScriptRoot                                                   # SW\steamvr_driver
$SunshineDir = (Resolve-Path (Join-Path $DrvDir '..\..\..\..')).Path     # Sunshine
$RepoRoot = Split-Path $SunshineDir -Parent
$VrIncDir = Join-Path $SunshineDir 'src\vr'
$OpenVrDir = Join-Path $SunshineDir 'third-party\openvr'
$OpenVrInc = Join-Path $OpenVrDir 'headers'
$OpenVrLibDir = Join-Path $OpenVrDir 'lib\win64'
$OpenVrDll = Join-Path $OpenVrDir 'bin\win64\openvr_api.dll'
$ProbeDir = Join-Path $SunshineDir 'tools\vr_probe'
$CommonCDir = Join-Path $SunshineDir 'third-party\moonlight-common-c\src'   # VipleVr.h（scene 圖案的 Quat48）
$OutDir = Join-Path $DrvDir "out\x64\$Config"
$GenDir = Join-Path $DrvDir 'out\gen'

foreach ($p in @($VrIncDir, $OpenVrInc, (Join-Path $VrIncDir 'vr_ipc_abi.h'), (Join-Path $OpenVrInc 'openvr_driver.h'))) {
    if (-not (Test-Path -LiteralPath $p)) { Fail "找不到 $p" }
}

# ── MSVC 環境 ────────────────────────────────────────────────────────────────
try {
    $vc = & (Join-Path $DrvDir 'Import-VcVars.ps1')
} catch {
    Fail $_.Exception.Message
}
if ($vc.Skipped) {
    Say "使用既有的 MSVC x64 環境（SDK $($vc.SdkVersion)）"
} else {
    Say "已載入 vcvars64（$($vc.VsPath)，SDK $($vc.SdkVersion)）"
}

New-Item -ItemType Directory -Force -Path $OutDir, $GenDir | Out-Null

# ── 共用旗標（§G）─────────────────────────────────────────────────────────────
$CommonCl = @(
    '/nologo', '/c', '/MT', '/std:c++20', '/EHsc', '/utf-8', '/W4', '/WX', '/permissive-',
    '/Zi', '/FS', '/guard:cf', '/MP',
    '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_WIN32_WINNT=0x0A00',
    "/external:I$OpenVrInc", '/external:W0',
    "/I$VrIncDir", "/I$GenDir"
)
if ($Config -eq 'Release') {
    $CommonCl += @('/O2', '/DNDEBUG')
} else {
    $CommonCl += @('/Od', '/D_DEBUG')
}

$CommonLink = @('/nologo', '/DEBUG', '/OPT:REF', '/OPT:ICF', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT', '/CETCOMPAT', '/MACHINE:X64')

function Invoke-Native([string]$exe, [string[]]$argv, [string]$what) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { Fail "$what 失敗（exit $LASTEXITCODE）" }
}

# dumpbin 的輸出（字串陣列）
function Get-Dumpbin([string]$flag, [string]$file) {
    $out = & dumpbin.exe /nologo $flag $file
    if ($LASTEXITCODE -ne 0) { Fail "dumpbin $flag $file 失敗（exit $LASTEXITCODE）" }
    return $out
}

function Get-Dependents([string]$file) {
    $lines = Get-Dumpbin '/dependents' $file
    $deps = New-Object System.Collections.Generic.List[string]
    $in = $false
    foreach ($l in $lines) {
        if ($l -match 'Image has the following dependencies') { $in = $true; continue }
        if ($in) {
            if ($l -match '^\s*Summary\s*$') { break }
            $t = $l.Trim()
            if ($t -ne '') { $deps.Add($t) }
        }
    }
    return , $deps.ToArray()
}

function Test-Machine([string]$file) {
    $h = Get-Dumpbin '/headers' $file
    if (-not ($h | Select-String -SimpleMatch 'machine (x64)')) { Fail "$file 不是 x64" }
}

# ── shader（driver；V4 起才有 shaders\*.hlsl）─────────────────────────────────
function Build-Shaders {
    $shaderDir = Join-Path $DrvDir 'shaders'
    if (-not (Test-Path -LiteralPath $shaderDir)) { return }
    $hlsl = @(Get-ChildItem -LiteralPath $shaderDir -Filter '*.hlsl' -File)
    foreach ($f in $hlsl) {
        $base = [IO.Path]::GetFileNameWithoutExtension($f.Name)
        if ($base -match '_vs$') { $shaderProfile = 'vs_5_0' }
        elseif ($base -match '_ps$') { $shaderProfile = 'ps_5_0' }
        else { Fail "shader 檔名要以 _vs 或 _ps 結尾：$($f.Name)" }
        $hdr = Join-Path $GenDir "$base.h"
        Say "fxc $($f.Name) -> out\gen\$base.h（$shaderProfile）"
        Invoke-Native 'fxc.exe' @('/nologo', '/T', $shaderProfile, '/E', 'main', '/O3', '/Vn', "g_$base", '/Fh', $hdr, $f.FullName) "fxc $($f.Name)"
    }
}

# ── driver ───────────────────────────────────────────────────────────────────
function Test-DriverSources {
    # driver 本體（HmdDriverFactory 等）V4 才加入；V2 只有與 vr_probe 共用的 ipc_client／driver_log
    return (Test-Path -LiteralPath (Join-Path $DrvDir 'driver_main.cpp'))
}

function Build-Driver {
    Say '=== driver_viplestream.dll ==='
    if (-not (Test-DriverSources)) {
        Fail 'driver_main.cpp 還不存在：driver 本體（HmdDriverFactory）是 V4 的工作；V2 請用 -Target probe'
    }
    $verHdr = Join-Path $DrvDir 'driver_version.h'
    if (-not (Test-Path -LiteralPath $verHdr)) {
        Fail '找不到 driver_version.h：請先跑 build-tools\propagate_version.cmd（由 version.ps1 產生，不可手寫）'
    }
    Build-Shaders
    $objDir = Join-Path $OutDir 'obj\driver'
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    Get-ChildItem -LiteralPath $objDir -Include '*.obj', '*.res' -File -Recurse | Remove-Item -Force
    $srcs = @(Get-ChildItem -LiteralPath $DrvDir -Filter '*.cpp' -File | ForEach-Object { $_.FullName })
    if ($srcs.Count -eq 0) { Fail 'steamvr_driver 下沒有任何 .cpp' }
    $pdbObj = Join-Path $objDir 'vc.pdb'
    $cl = $CommonCl + @("/I$DrvDir", "/Fo$objDir/", "/Fd$pdbObj") + $srcs
    Say "cl：$($srcs.Count) 個來源檔"
    Invoke-Native 'cl.exe' $cl 'cl（driver）'

    $objs = @(Get-ChildItem -LiteralPath $objDir -Filter '*.obj' -File | ForEach-Object { $_.FullName })
    $rcFile = Join-Path $DrvDir 'driver.rc'
    if (Test-Path -LiteralPath $rcFile) {
        $res = Join-Path $objDir 'driver.res'
        Say 'rc driver.rc'
        Invoke-Native 'rc.exe' @('/nologo', '/c65001', "/I$DrvDir", "/I$GenDir", '/fo', $res, $rcFile) 'rc'
        $objs += $res
    }
    $dll = Join-Path $OutDir 'driver_viplestream.dll'
    $pdb = Join-Path $OutDir 'driver_viplestream.pdb'
    $link = $CommonLink + @('/DLL', "/OUT:$dll", "/PDB:$pdb") + $objs + @('d3d11.lib', 'dxgi.lib', 'advapi32.lib', 'kernel32.lib')
    Say 'link driver_viplestream.dll'
    Invoke-Native 'link.exe' $link 'link（driver）'

    # 快速檢查（Stage-VipleSteamVR.ps1 會再完整檢查一次）
    Test-Machine $dll
    $exp = Get-Dumpbin '/exports' $dll
    $names = New-Object System.Collections.Generic.List[string]
    $inTable = $false
    foreach ($l in $exp) {
        if ($l -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
        if ($inTable) {
            if ($l -match '^\s*Summary\s*$') { break }
            if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $names.Add($matches[1]) }
        }
    }
    if ($names.Count -ne 1 -or $names[0] -ne 'HmdDriverFactory') {
        Fail "driver 的匯出必須只有 HmdDriverFactory，實際：$($names -join ', ')"
    }
    $deps = Get-Dependents $dll
    Say "driver exports: $($names -join ', ')"
    Say "driver dependents: $($deps -join ', ')"
    foreach ($d in $deps) {
        if (-not ($DriverDllWhitelist | Where-Object { $_ -ieq $d })) {
            Fail "driver 相依白名單外的 DLL：$d（白名單：$($DriverDllWhitelist -join ', ')）"
        }
    }
    Say "OK -> $dll"
}

# ── vr_probe ─────────────────────────────────────────────────────────────────
function Build-Probe {
    Say '=== vr_probe.exe ==='
    if (-not (Test-Path -LiteralPath $ProbeDir)) { Fail "找不到 $ProbeDir" }
    $checks = @(
        @{ Path = $OpenVrDll; Sha = $OpenVrDllSha256 },
        @{ Path = (Join-Path $OpenVrLibDir 'openvr_api.lib'); Sha = $OpenVrLibSha256 }
    )
    foreach ($c in $checks) {
        if (-not (Test-Path -LiteralPath $c.Path)) { Fail "找不到 $($c.Path)" }
        $h = (Get-FileHash -LiteralPath $c.Path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($h -ne $c.Sha) { Fail "$($c.Path) 的 sha256 不符（$h）；vendored OpenVR 被改過？" }
    }
    $objDir = Join-Path $OutDir 'obj\vr_probe'
    $binDir = Join-Path $OutDir 'vr_probe'
    New-Item -ItemType Directory -Force -Path $objDir, $binDir | Out-Null
    Get-ChildItem -LiteralPath $objDir -Filter '*.obj' -File | Remove-Item -Force

    $srcs = @(Get-ChildItem -LiteralPath $ProbeDir -Filter '*.cpp' -File | ForEach-Object { $_.FullName })
    # driver 端與 probe 共用的模組（同一份原始碼）；V4 的純模組存在時一起連（unit 模式要用）
    foreach ($shared in @('ipc_client.cpp', 'driver_log.cpp', 'virtual_vsync.cpp', 'pose_history.cpp')) {
        $p = Join-Path $DrvDir $shared
        if (Test-Path -LiteralPath $p) { $srcs += $p }
    }
    $pdbObj = Join-Path $objDir 'vc.pdb'
    $cl = $CommonCl + @('/DVRDRV_PEER_HOOKS', "/I$DrvDir", "/I$ProbeDir", "/external:I$CommonCDir", "/Fo$objDir/", "/Fd$pdbObj") + $srcs
    Say "cl：$($srcs.Count) 個來源檔"
    Invoke-Native 'cl.exe' $cl 'cl（vr_probe）'

    $objs = @(Get-ChildItem -LiteralPath $objDir -Filter '*.obj' -File | ForEach-Object { $_.FullName })
    $exe = Join-Path $binDir 'vr_probe.exe'
    $pdb = Join-Path $binDir 'vr_probe.pdb'
    # openvr_api.dll 延遲載入：ipcpeer／unit 不需要它（V4 的 whoami／scene／… 才會真的載入）
    $link = $CommonLink + @('/SUBSYSTEM:CONSOLE', "/OUT:$exe", "/PDB:$pdb", "/LIBPATH:$OpenVrLibDir", '/DELAYLOAD:openvr_api.dll') + $objs + @(
        'openvr_api.lib', 'delayimp.lib', 'd3d11.lib', 'dxgi.lib', 'advapi32.lib', 'kernel32.lib', 'shell32.lib', 'ole32.lib')
    Say 'link vr_probe.exe'
    Invoke-Native 'link.exe' $link 'link（vr_probe）'
    Copy-Item -LiteralPath $OpenVrDll -Destination (Join-Path $binDir 'openvr_api.dll') -Force

    Test-Machine $exe
    $depLines = Get-Dumpbin '/dependents' $exe
    $depLines | Set-Content -LiteralPath (Join-Path $binDir 'dependents.txt') -Encoding ASCII
    $deps = Get-Dependents $exe
    Say "vr_probe dependents: $($deps -join ', ')"
    foreach ($d in $deps) {
        if ($d -match '^(VCRUNTIME|MSVCP|ucrtbase|api-ms-win-crt-)') { Fail "vr_probe 不該動態連 CRT：$d（應為 /MT）" }
    }

    $zipDir = Join-Path $RepoRoot 'temp\vr_probe'
    New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
    $zip = Join-Path $zipDir 'VipleStream-VrProbe-dev.zip'
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -LiteralPath @($exe, (Join-Path $binDir 'openvr_api.dll')) -DestinationPath $zip
    Say "OK -> $exe"
    Say "dev zip -> $zip（不進 release；scp 到 host 的 <install>\tools\vr_probe\）"
}

switch ($Target) {
    'driver' { Build-Driver }
    'probe' { Build-Probe }
    'all' {
        if (Test-DriverSources) {
            Build-Driver
        } else {
            Say '略過 driver：driver_main.cpp 還不存在（V4）；只建 vr_probe'
        }
        Build-Probe
    }
}
Say "完成（Target=$Target Config=$Config）"
exit 0
