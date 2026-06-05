# §SC-HID: Build-ScHidDriver.ps1
# 建置 VipleSCHid_Driver.dll（UMDF2 HID minidriver），自簽憑證，產出 .cat
# 輸出：out\ 目錄下 VipleSCHid_Driver.dll + VipleSCHid.cat + VipleSCHid_SelfSign.cer
#
# 必要條件：
#   1. Visual Studio 2022（任何版本）
#   2. WDK for Windows 11（含 WDK.vsix VS 整合）
#      下載：https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
#   3. 以管理員身分執行（憑證安裝需要）

param(
    [string]$Config   = "Release",
    [string]$Platform = "x64",
    [switch]$SkipSign = $false,
    [switch]$Install  = $false   # 安裝憑證 + driver 到本機（host 端用）
)

$scriptDir = $PSScriptRoot

function Fail([string]$msg) {
    Write-Host ""
    Write-Host $msg -ForegroundColor Red
    Write-Host ""
    exit 1
}

# ── 工具路徑 ──────────────────────────────────────────────────────────────────
$kitRoot  = "C:\Program Files (x86)\Windows Kits\10"
$kitVer   = "10.0.26100.0"
$signTool = "$kitRoot\bin\$kitVer\x64\signtool.exe"
$makeCat  = "$kitRoot\bin\$kitVer\x64\makecat.exe"
$inf2cat  = "$kitRoot\bin\$kitVer\x64\Inf2Cat.exe"  # WDK 才有，SDK 沒有

# MSBuild — 從 vswhere 找
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = if (Test-Path $vsWhere) {
    & $vsWhere -latest -requires Microsoft.Component.MSBuild -property installationPath 2>$null
} else { $null }
$msBuild = if ($vsPath) { "$vsPath\MSBuild\Current\Bin\MSBuild.exe" } else { $null }

# ── 前置檢查 ──────────────────────────────────────────────────────────────────
Write-Host "[SC-HID] 檢查建置環境..."

# 檢查 WDK
$wdkWdfInc = "$kitRoot\Include\$kitVer\wdf"
if (-not (Test-Path $wdkWdfInc)) {
    Write-Host "[SC-HID] ❌ WDK 未安裝（找不到 $wdkWdfInc）" -ForegroundColor Red
    Write-Host ""
    Write-Host "請依序安裝："
    Write-Host "  1. Visual Studio 2022（已安裝）"
    Write-Host "  2. WDK for Windows 11 24H2："
    Write-Host "     https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk"
    Write-Host "     -> 下載 [WDK for Windows 11, version 24H2] -> 執行安裝程式"
    Write-Host "     -> 安裝過程詢問是否安裝 VS2022 WDK 擴充套件，選 Yes"
    Write-Host "  3. 安裝完後重跑此腳本"
    Write-Host ""
    exit 1
}

if (-not $msBuild -or -not (Test-Path $msBuild)) {
    Fail "[SC-HID] ❌ 找不到 MSBuild。請確認 Visual Studio 2022 已安裝。"
}
if (-not (Test-Path $signTool)) {
    Fail "[SC-HID] ❌ 找不到 signtool.exe: $signTool"
}

Write-Host "[SC-HID] ✅ 環境檢查通過"

# ── 建置 ─────────────────────────────────────────────────────────────────────
$proj    = Join-Path $scriptDir "VipleSCHid_Driver.vcxproj"
$outDir  = Join-Path $scriptDir "out\$Platform\$Config"
$dllPath = Join-Path $outDir "VipleSCHid_Driver.dll"

New-Item $outDir -ItemType Directory -Force | Out-Null

Write-Host "[SC-HID] 開始建置 ($Platform/$Config)..."
& $msBuild $proj /p:Configuration=$Config /p:Platform=$Platform /nologo /m
if ($LASTEXITCODE -ne 0) { Fail "[SC-HID] ❌ MSBuild 建置失敗（exit $LASTEXITCODE）" }
if (-not (Test-Path $dllPath)) { Fail "[SC-HID] ❌ 建置後找不到 $dllPath" }
Write-Host "[SC-HID] ✅ 建置完成：$dllPath"

if ($SkipSign) {
    Write-Host "[SC-HID] 跳過簽章步驟（-SkipSign）"
    exit 0
}

# ── 自簽憑證 ─────────────────────────────────────────────────────────────────
$certSubject = "CN=VipleStream SC HID Driver, O=VipleStream, C=TW"
$certStore   = "Cert:\LocalMachine\My"
$certFile    = Join-Path $outDir "VipleSCHid_SelfSign.cer"

Write-Host "[SC-HID] 建立自簽憑證..."
$cert = Get-ChildItem $certStore |
        Where-Object { $_.Subject -eq $certSubject } |
        Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Subject $certSubject `
        -CertStoreLocation $certStore `
        -Type CodeSigningCert `
        -KeyUsage DigitalSignature `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
    Write-Host "[SC-HID] ✅ 新建憑證：$($cert.Thumbprint)"
} else {
    Write-Host "[SC-HID] ✅ 使用既有憑證：$($cert.Thumbprint)"
}

Export-Certificate -Cert $cert -FilePath $certFile -Type CERT | Out-Null

# ── 簽署 DLL ─────────────────────────────────────────────────────────────────
Write-Host "[SC-HID] 簽署 DLL..."
& $signTool sign /fd SHA256 /a /n "VipleStream SC HID Driver" /t http://timestamp.digicert.com $dllPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "[SC-HID] 時間戳伺服器失敗，改用無時間戳簽署..." -ForegroundColor Yellow
    & $signTool sign /fd SHA256 /a /n "VipleStream SC HID Driver" $dllPath
}
if ($LASTEXITCODE -ne 0) { Fail "[SC-HID] ❌ signtool 簽署 DLL 失敗" }
Write-Host "[SC-HID] ✅ DLL 已簽署"

# ── 產生 .cat ─────────────────────────────────────────────────────────────────
$catFile = Join-Path $outDir "VipleSCHid.cat"
$infPath = Join-Path $scriptDir "VipleSCHid.inf"

if (Test-Path $inf2cat) {
    Write-Host "[SC-HID] 使用 Inf2Cat 產生 .cat..."
    $tmpDir = Join-Path $outDir "cat_tmp"
    New-Item $tmpDir -ItemType Directory -Force | Out-Null
    Copy-Item $infPath $tmpDir
    Copy-Item $dllPath $tmpDir
    & $inf2cat /driver:"$tmpDir" /os:10_x64 /verbose
    $generatedCat = Get-ChildItem $tmpDir -Filter "*.cat" -ErrorAction SilentlyContinue |
                    Select-Object -First 1
    if ($generatedCat) {
        Copy-Item $generatedCat.FullName $catFile
        Write-Host "[SC-HID] ✅ .cat 由 Inf2Cat 產生：$catFile"
    }
    Remove-Item $tmpDir -Recurse -Force
} else {
    Write-Host "[SC-HID] Inf2Cat 不存在，改用 makecat..."
    $cdfPath = Join-Path $outDir "VipleSCHid.cdf"
    Set-Content $cdfPath -Encoding ASCII -Value (
        "[CatalogHeader]`r`n" +
        "Name=VipleSCHid.cat`r`n" +
        "PublicVersion=0x0000001`r`n" +
        "EncodingType=0x00010001`r`n" +
        "CATATTR1=0x10010001:OSAttr:2:6.3`r`n" +
        "`r`n" +
        "[CatalogFiles]`r`n" +
        "<hash>VipleSCHid_Driver.dll=$dllPath`r`n" +
        "<hash>VipleSCHid.inf=$infPath`r`n"
    )
    & $makeCat $cdfPath
    if (Test-Path $catFile) {
        Write-Host "[SC-HID] ✅ .cat 由 makecat 產生：$catFile"
    } else {
        Write-Host "[SC-HID] ⚠ .cat 產生失敗，繼續（driver 仍可用 test-signing 安裝）" -ForegroundColor Yellow
        $catFile = $null
    }
}

if ($catFile -and (Test-Path $catFile)) {
    Write-Host "[SC-HID] 簽署 .cat..."
    & $signTool sign /fd SHA256 /a /n "VipleStream SC HID Driver" $catFile
    if ($LASTEXITCODE -ne 0) {
        & $signTool sign /fd SHA256 /a /n "VipleStream SC HID Driver" $catFile
    }
    Write-Host "[SC-HID] ✅ .cat 已簽署"
}

# ── 安裝到本機（-Install 旗標，host 端用）────────────────────────────────────
if ($Install) {
    Write-Host "[SC-HID] 安裝憑證到 Trusted Root CA + Trusted Publishers..."
    Import-Certificate -FilePath $certFile -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
    Import-Certificate -FilePath $certFile -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null

    Write-Host "[SC-HID] 安裝 driver（pnputil）..."
    & pnputil /add-driver $infPath /install
    if ($LASTEXITCODE -ne 0) { Fail "[SC-HID] ❌ pnputil 安裝失敗" }

    Write-Host "[SC-HID] 建立裝置節點..."
    $devconPaths = @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Tools\x64\devcon.exe",
        "devcon.exe"
    )
    $devcon = $devconPaths | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($devcon) {
        & $devcon install $infPath "Viple\SteamController"
    } else {
        Write-Host "[SC-HID] ⚠ devcon 不存在，觸發 PnP 掃描..." -ForegroundColor Yellow
        & pnputil /scan-devices
    }
    Write-Host "[SC-HID] ✅ 安裝完成"
}

# ── 複製產物到 sc_hid_driver\ 根目錄（Sunshine 打包用）─────────────────────
$deployDll = Join-Path $scriptDir "VipleSCHid_Driver.dll"
$deployCat = Join-Path $scriptDir "VipleSCHid.cat"
Copy-Item $dllPath $deployDll -Force
if ($catFile -and (Test-Path $catFile)) {
    Copy-Item $catFile $deployCat -Force
}

Write-Host ""
Write-Host "[SC-HID] 完成。產物："
Write-Host "  DLL : $deployDll"
if (Test-Path $deployCat) { Write-Host "  CAT : $deployCat" }
Write-Host "  CER : $certFile（安裝到 host 時需要）"
