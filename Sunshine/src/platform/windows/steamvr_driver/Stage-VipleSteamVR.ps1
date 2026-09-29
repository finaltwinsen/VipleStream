# ============================================================================
# §VR M1b：Stage-VipleSteamVR.ps1（設計 §G、S2-10）
# 把 SteamVR driver 套件整理進 server zip 的 steamvr\ 目錄：
#   <OutDir>\viplestream\{driver.vrdrivermanifest, bin\win64\driver_viplestream.dll,
#                         resources\settings\default.vrsettings, resources\input\viplestream_hmd_profile.json,
#                         LICENSE-OpenVR.txt, THIRD_PARTY_NOTICES.md}
#   <OutDir>\Install-VipleSteamVR.ps1（只診斷與手動移除；K21）
#   <OutDir>\viplestream.sha256（每行 "<sha256>  viplestream/<relpath>"，依路徑排序、LF、ASCII）
#   <OutDir>\viplestream.version（一行 "<X.Y.Z>-h<hash8>"；部署時的 config\steamvr\<ver>\ 目錄名）
#
# 步驟：
#   1. 刪掉並重建 OutDir
#   2. 新鮮度：DLL 不存在、比任何來源舊、或 FileVersionRaw（VS_FIXEDFILEINFO）≠ <ExpectVersion>.0
#      → 呼叫 Build-SteamVRDriver.ps1 -Target driver；重建後仍不符 → exit 1
#   3. dumpbin：machine (x64)、匯出只有 HmdDriverFactory、相依白名單 {KERNEL32, ADVAPI32, d3d11, dxgi}
#   4. 版面；缺任何一個 → exit 1
#   5. NVIDIA EULA 防呆
#   6. sha256 清單與 hash8
# 沒有 PoC 旗標（PoC-5a A4 的 manifest 變體由 server 的 selftest preset 合成，V6）。
#
# V4 偏差：本版沒有移植任何 ALVR 程式碼（本機沒有 ALVR 原始碼，元件都是獨立撰寫），所以不附 LICENSE-ALVR.txt；
#          之後若移植 ALVR 程式碼，再把它加回必備清單。
#
# 注意：本檔必須存成 UTF-8 含 BOM（PowerShell 5.1 會以 cp950 讀無 BOM 的檔）。
# Get-FileHash 在某些 5.1 環境（PSModulePath 指到 pwsh 7 的模組）載不起來，雜湊一律用 .NET。
# ============================================================================
param(
    [Parameter(Mandatory = $true)][string]$DriverDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$ExpectVersion
)

$ErrorActionPreference = 'Stop'

function Fail([string]$msg) {
    Write-Host ''
    Write-Host "[VR-STAGE] [ERROR] $msg" -ForegroundColor Red
    exit 1
}

function Say([string]$msg) {
    Write-Host "[VR-STAGE] $msg"
}

function Get-Sha256Hex([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead($path)
        try {
            $bytes = $sha.ComputeHash($fs)
        } finally {
            $fs.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

if ($ExpectVersion -notmatch '^\d+\.\d+\.\d+$') { Fail "ExpectVersion 格式錯誤：$ExpectVersion" }
$DriverDir = (Resolve-Path -LiteralPath $DriverDir).Path
$SunshineDir = (Resolve-Path (Join-Path $DriverDir '..\..\..\..')).Path
$Dll = Join-Path $DriverDir 'out\x64\Release\driver_viplestream.dll'
$Pkg = Join-Path $DriverDir 'package'
$Whitelist = @('KERNEL32.dll', 'ADVAPI32.dll', 'd3d11.dll', 'dxgi.dll')

# ── 1. 重建 OutDir ────────────────────────────────────────────────────────
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ── 2. 新鮮度 ─────────────────────────────────────────────────────────────
function Get-Staleness {
    if (-not (Test-Path -LiteralPath $Dll)) { return '沒有 DLL' }
    $dllTime = (Get-Item -LiteralPath $Dll).LastWriteTimeUtc
    # -LiteralPath 與 -Include 在 PowerShell 5.1 不可靠（會把 .ps1 也算進來），副檔名一律自己比
    $exts = @('.cpp', '.h', '.hlsl', '.rc', '.json', '.vrsettings', '.vrdrivermanifest')
    $srcs = @(Get-ChildItem -LiteralPath $DriverDir -Recurse -File |
        Where-Object { $exts -contains $_.Extension.ToLowerInvariant() -and $_.FullName -notlike (Join-Path $DriverDir 'out\*') })
    $srcs += Get-Item -LiteralPath (Join-Path $SunshineDir 'src\vr\vr_ipc_abi.h')
    $srcs += Get-Item -LiteralPath (Join-Path $SunshineDir 'third-party\openvr\headers\openvr_driver.h')
    foreach ($s in $srcs) {
        if ($s.LastWriteTimeUtc -gt $dllTime) { return "來源比 DLL 新：$($s.Name)" }
    }
    $vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Dll)
    $raw = "$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)"
    if ($raw -ne "$ExpectVersion.0") { return "FileVersionRaw $raw ≠ $ExpectVersion.0" }
    return $null
}

$why = Get-Staleness
if ($why) {
    Say "driver 需要重建（$why）"
    & (Join-Path $DriverDir 'Build-SteamVRDriver.ps1') -Target driver
    if ($LASTEXITCODE -ne 0) { Fail "Build-SteamVRDriver.ps1 -Target driver 失敗（exit $LASTEXITCODE）" }
    $why = Get-Staleness
    if ($why) { Fail "重建後仍不符：$why" }
}
Say "driver DLL OK（$ExpectVersion.0）"

# ── 3. dumpbin ────────────────────────────────────────────────────────────
try {
    $null = & (Join-Path $DriverDir 'Import-VcVars.ps1')
} catch {
    Fail $_.Exception.Message
}
$hdr = & dumpbin.exe /nologo /headers $Dll
if ($LASTEXITCODE -ne 0) { Fail 'dumpbin /headers 失敗' }
if (-not ($hdr | Select-String -SimpleMatch 'machine (x64)')) { Fail 'driver DLL 不是 x64' }
$exp = & dumpbin.exe /nologo /exports $Dll
$names = New-Object System.Collections.Generic.List[string]
$inTable = $false
foreach ($l in $exp) {
    if ($l -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if ($inTable) {
        if ($l -match '^\s*Summary\s*$') { break }
        if ($l -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $names.Add($matches[1]) }
    }
}
if ($names.Count -ne 1 -or $names[0] -ne 'HmdDriverFactory') { Fail "匯出必須只有 HmdDriverFactory，實際：$($names -join ', ')" }
$dep = & dumpbin.exe /nologo /dependents $Dll
$deps = New-Object System.Collections.Generic.List[string]
$in = $false
foreach ($l in $dep) {
    if ($l -match 'Image has the following dependencies') { $in = $true; continue }
    if ($in) {
        if ($l -match '^\s*Summary\s*$' -or $l -match 'delay load') { break }
        $t = $l.Trim()
        if ($t -ne '') { $deps.Add($t) }
    }
}
foreach ($d in $deps) {
    if (-not ($Whitelist | Where-Object { $_ -ieq $d })) { Fail "相依白名單外的 DLL：$d" }
}
Say "dumpbin OK：exports=HmdDriverFactory dependents=$($deps -join ',')"

# ── 4. 版面 ───────────────────────────────────────────────────────────────
$Root = Join-Path $OutDir 'viplestream'
$layout = @(
    @{ Src = (Join-Path $Pkg 'driver.vrdrivermanifest'); Dst = 'driver.vrdrivermanifest' },
    @{ Src = $Dll; Dst = 'bin\win64\driver_viplestream.dll' },
    @{ Src = (Join-Path $Pkg 'resources\settings\default.vrsettings'); Dst = 'resources\settings\default.vrsettings' },
    @{ Src = (Join-Path $Pkg 'resources\input\viplestream_hmd_profile.json'); Dst = 'resources\input\viplestream_hmd_profile.json' },
    @{ Src = (Join-Path $Pkg 'LICENSE-OpenVR.txt'); Dst = 'LICENSE-OpenVR.txt' },
    @{ Src = (Join-Path $Pkg 'THIRD_PARTY_NOTICES.md'); Dst = 'THIRD_PARTY_NOTICES.md' }
)
foreach ($f in $layout) {
    if (-not (Test-Path -LiteralPath $f.Src)) { Fail "缺檔：$($f.Src)" }
    $dst = Join-Path $Root $f.Dst
    New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
    Copy-Item -LiteralPath $f.Src -Destination $dst -Force
}
$inst = Join-Path $DriverDir 'Install-VipleSteamVR.ps1'
if (-not (Test-Path -LiteralPath $inst)) { Fail "缺檔：$inst" }
Copy-Item -LiteralPath $inst -Destination (Join-Path $OutDir 'Install-VipleSteamVR.ps1') -Force

# ── 5. NVIDIA EULA 防呆 ───────────────────────────────────────────────────
$scan = @(Get-ChildItem -LiteralPath $DriverDir -Recurse -File | Where-Object { $_.FullName -notlike (Join-Path $DriverDir 'out\*') })
$scan += @(Get-ChildItem -LiteralPath $OutDir -Recurse -File)
foreach ($f in $scan) {
    if ($f.Name -match '^Nv.*\.(h|cpp)$') { Fail "NVIDIA 原始碼檔名：$($f.FullName)" }
    if ($f.Extension -in @('.dll', '.pdb', '.obj', '.res', '.exe')) { continue }
    if ($f.Name -eq 'Stage-VipleSteamVR.ps1') { continue }  # 本檔自己列了關鍵字
    $hit = Select-String -LiteralPath $f.FullName -Pattern 'nvEncodeAPI', 'NvEncoder', 'NVIDIA CORPORATION' -SimpleMatch -List
    if ($hit) { Fail "NVIDIA EULA 內容：$($f.FullName)" }
}

# ── 6. sha256 清單與 hash8 ────────────────────────────────────────────────
$rows = New-Object System.Collections.Generic.List[string]
$files = @(Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($Root.Length + 1).Replace('\', '/')
        [pscustomobject]@{ Rel = $rel; Full = $_.FullName }
    } | Sort-Object -Property Rel -CaseSensitive)
foreach ($f in $files) {
    $rows.Add("$(Get-Sha256Hex $f.Full)  viplestream/$($f.Rel)")
}
$manifest = Join-Path $OutDir 'viplestream.sha256'
[System.IO.File]::WriteAllText($manifest, (($rows -join "`n") + "`n"), [System.Text.Encoding]::ASCII)
$hash8 = (Get-Sha256Hex $manifest).Substring(0, 8)
$ver = "$ExpectVersion-h$hash8"
[System.IO.File]::WriteAllText((Join-Path $OutDir 'viplestream.version'), "$ver`n", [System.Text.Encoding]::ASCII)
Say "staged $($files.Count) 個檔 -> $OutDir"
Say "hash8=$hash8 ver=$ver"
exit 0
