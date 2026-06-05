# ============================================================================
# §SC-HID: Build-ScHidDriver.ps1
# 在「沒有 WDK Visual Studio 整合（WDK.vsix）」的環境，直接用 cl.exe + link.exe
# 建出 UMDF2 HID minidriver DLL（VipleSCHid_Driver.dll），可被 in-box WUDFRd /
# WUDFHost 載入。
#
# 為什麼不用 .vcxproj + msbuild：本機只裝了 WDK 的 headers/libs/build-props，
# 但「WDK 的 VS 擴充套件」沒裝 → MSBuild 沒有 WindowsUserModeDriver10.0 toolset，
# driver .vcxproj 路線整條死。改用 BuildTools 的 cl/link 直呼 + 手動 WDK 路徑，
# 跟專案其他建置（vcvars64 + 直呼工具）一致，且不需要補裝任何東西。
#
# 已在本機實測（MSVC 14.44 + WDK SDK 10.0.28000.0 + UMDF 2.33）：
# 編譯/連結通過，產出 DLL 正確匯出 FxDriverEntryUm。
#
# 必要條件：
#   1. Visual Studio 2022 BuildTools（或任何含 VC x64 tools 的 VS）
#   2. WDK headers/libs（含 Include\<sdk>\km\hidport.h 與
#      Lib\wdf\umdf\x64\2.33\WdfDriverStubUm.lib）
#   3. 簽章 / 安裝步驟需系統管理員
# ============================================================================
param(
    [string]$Config   = "Release",
    [switch]$SkipSign = $false,
    [switch]$Install  = $false   # 安裝憑證 + driver 到本機（host 端用）
)

$ErrorActionPreference = "Stop"
function Fail([string]$m) { Write-Host ""; Write-Host "[SC-HID] $m" -ForegroundColor Red; Write-Host ""; exit 1 }

$scriptDir = $PSScriptRoot
$KitRoot   = "C:\Program Files (x86)\Windows Kits\10"

# ── 偵測 SDK 版本（需含 km\hidport.h，即 WDK SDK）────────────────────────────
$SdkVer = (Get-ChildItem "$KitRoot\Include" -Directory -ErrorAction SilentlyContinue |
           Where-Object { Test-Path "$($_.FullName)\km\hidport.h" } |
           Sort-Object Name -Descending | Select-Object -First 1).Name
if (-not $SdkVer) {
    Write-Host "[SC-HID] ❌ 找不到含 km\hidport.h 的 Windows SDK（需要 WDK）" -ForegroundColor Red
    Write-Host ""
    Write-Host "請安裝 WDK for Windows 11："
    Write-Host "  https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk"
    Write-Host "（只需 headers/libs；本腳本不依賴 WDK 的 VS 擴充套件）"
    Write-Host ""
    exit 1
}

# ── 偵測 UMDF 版本（對齊 INF 的 UmdfLibraryVersion=2.33）─────────────────────
$UmdfVer = "2.33"   # 必須與 VipleSCHid.inf 的 UmdfLibraryVersion 一致
$stubLib = "$KitRoot\Lib\wdf\umdf\x64\$UmdfVer\WdfDriverStubUm.lib"
if (-not (Test-Path $stubLib)) {
    $UmdfVer = (Get-ChildItem "$KitRoot\Lib\wdf\umdf\x64" -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^\d+\.\d+$' } |
                Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1).Name
    $stubLib = "$KitRoot\Lib\wdf\umdf\x64\$UmdfVer\WdfDriverStubUm.lib"
}
if (-not (Test-Path $stubLib)) { Fail "找不到 WdfDriverStubUm.lib（WDK UMDF2 lib 未安裝）" }
$umMajor, $umMinor = $UmdfVer.Split('.')

# ── 取得 cl/link 環境（vcvars64，pin 到偵測到的 SDK）────────────────────────
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = if (Test-Path $vsWhere) {
    & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
} else { $null }
if (-not $vsPath) { $vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" }
$vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { Fail "找不到 vcvars64.bat: $vcvars" }

Write-Host "[SC-HID] 載入 MSVC 環境（vcvars64 + SDK $SdkVer）..."
cmd /c "`"$vcvars`" $SdkVer >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# 補上 WDK 專屬路徑（vcvars 不含 km 與 wdf\umdf）
$env:INCLUDE = "$KitRoot\Include\wdf\umdf\$UmdfVer;$KitRoot\Include\$SdkVer\km;$env:INCLUDE"
$env:LIB     = "$KitRoot\Lib\wdf\umdf\x64\$UmdfVer;$env:LIB"

# ── 編譯 ─────────────────────────────────────────────────────────────────────
$outDir = Join-Path $scriptDir "out\x64\$Config"
New-Item $outDir -ItemType Directory -Force | Out-Null
$src = Join-Path $scriptDir "VipleSCHid_Driver.cpp"
$obj = Join-Path $outDir "VipleSCHid_Driver.obj"
$dll = Join-Path $outDir "VipleSCHid_Driver.dll"
$pdb = Join-Path $outDir "VipleSCHid_Driver.pdb"

# /utf-8：.cpp 含中文註解，host ANSI codepage=950，不加會 C4819→C2059
# /MD   ：UMDF driver 跑在 WUDFHost.exe 內，必須動態 CRT（不可 /MT）
# /DUMDF_VERSION_*：手呼 cl 沒人注入版本巨集，標頭才能對到 2.33 stub 的版本符號
$optFlags = if ($Config -eq "Debug") { @("/Od","/D_DEBUG") } else { @("/O2","/DNDEBUG") }

Write-Host "[SC-HID] cl 編譯（SDK=$SdkVer UMDF=$UmdfVer）..."
& cl.exe /c /nologo /utf-8 /MD /W3 /Zi /EHsc /GS @optFlags `
    /DUMDF_VERSION_MAJOR=$umMajor /DUMDF_VERSION_MINOR=$umMinor `
    /DUNICODE /D_UNICODE /DWIN32 /D_WINDLL `
    /Fo"$obj" /Fd"$pdb" "$src"
if ($LASTEXITCODE -ne 0) { Fail "cl 編譯失敗（exit $LASTEXITCODE）" }

# ── 連結 ─────────────────────────────────────────────────────────────────────
# WdfDriverStubUm.lib：提供 FxDriverEntryUm 匯出 + 對作者 DriverEntry 的橋接
# ntdll.lib          ：提供 __imp_DbgPrintEx（OneCoreUAP/OneCore.lib 都沒有）
# /EXPORT:FxDriverEntryUm：強制把 stub obj 帶入並對外匯出（CRT 進入點不會主動參考）
# 不需 /ENTRY、不需自寫 DllMain（CRT 提供 _DllMainCRTStartup）
Write-Host "[SC-HID] link..."
& link.exe /nologo /DLL /MACHINE:X64 /SUBSYSTEM:WINDOWS /DEBUG /OPT:REF /OPT:ICF `
    /EXPORT:FxDriverEntryUm `
    /OUT:"$dll" `
    "$obj" "$stubLib" ntdll.lib
if ($LASTEXITCODE -ne 0) { Fail "link 失敗（exit $LASTEXITCODE）" }
if (-not (Test-Path $dll)) { Fail "連結後找不到 $dll" }
Write-Host "[SC-HID] ✅ 建置完成 -> $dll"

if ($SkipSign) {
    Copy-Item $dll (Join-Path $scriptDir "VipleSCHid_Driver.dll") -Force
    Write-Host "[SC-HID] 跳過簽章（-SkipSign）"
    exit 0
}

# ── 自簽憑證 + 簽 DLL ────────────────────────────────────────────────────────
$signTool = (Get-ChildItem "$KitRoot\bin" -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending |
             ForEach-Object { "$($_.FullName)\x64\signtool.exe" } |
             Where-Object { Test-Path $_ } | Select-Object -First 1)
if (-not $signTool) { Fail "找不到 signtool.exe" }

$certSubject = "CN=VipleStream SC HID Driver, O=VipleStream, C=TW"
$certFile    = Join-Path $outDir "VipleSCHid_SelfSign.cer"
$cert = Get-ChildItem Cert:\LocalMachine\My |
        Where-Object { $_.Subject -eq $certSubject } | Select-Object -First 1
if (-not $cert) {
    Write-Host "[SC-HID] 建立自簽憑證..."
    $cert = New-SelfSignedCertificate -Subject $certSubject -CertStoreLocation Cert:\LocalMachine\My `
        -Type CodeSigningCert -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 `
        -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(10) `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
    Write-Host "[SC-HID] ✅ 新建憑證：$($cert.Thumbprint)"
} else {
    Write-Host "[SC-HID] ✅ 使用既有憑證：$($cert.Thumbprint)"
}
Export-Certificate -Cert $cert -FilePath $certFile -Type CERT | Out-Null

# /sm：用 machine store（憑證在 LocalMachine\My，不加 /sm 會找 CurrentUser）
# /sha1：用 thumbprint 精準選憑證（比 /n 子字串比對可靠）
$thumb = $cert.Thumbprint
Write-Host "[SC-HID] 簽署 DLL..."
& $signTool sign /sm /fd SHA256 /sha1 $thumb /t http://timestamp.digicert.com $dll
if ($LASTEXITCODE -ne 0) {
    Write-Host "[SC-HID] 時間戳失敗，改用無時間戳..." -ForegroundColor Yellow
    & $signTool sign /sm /fd SHA256 /sha1 $thumb $dll
}
if ($LASTEXITCODE -ne 0) { Fail "signtool 簽 DLL 失敗" }
Write-Host "[SC-HID] ✅ DLL 已簽署"

# ── 產 .cat（Inf2Cat 動態偵測；退而用 makecat）───────────────────────────────
$catFile = Join-Path $outDir "VipleSCHid.cat"
$infPath = Join-Path $scriptDir "VipleSCHid.inf"
$inf2cat = (Get-ChildItem "$KitRoot\bin" -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { @("$($_.FullName)\x86\Inf2Cat.exe", "$($_.FullName)\x64\Inf2Cat.exe") } |
            Where-Object { Test-Path $_ } | Select-Object -First 1)
if ($inf2cat) {
    Write-Host "[SC-HID] Inf2Cat 產生 .cat..."
    $tmp = Join-Path $outDir "cat_tmp"; New-Item $tmp -ItemType Directory -Force | Out-Null
    Copy-Item $infPath $tmp; Copy-Item $dll $tmp
    & $inf2cat /driver:"$tmp" /os:10_x64 /uselocaltime
    $g = Get-ChildItem $tmp -Filter *.cat -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($g) { Copy-Item $g.FullName $catFile -Force }
    Remove-Item $tmp -Recurse -Force
}
if (-not (Test-Path $catFile)) {
    Write-Host "[SC-HID] 改用 makecat..."
    $makeCat = (Get-ChildItem "$KitRoot\bin" -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending |
                ForEach-Object { "$($_.FullName)\x64\makecat.exe" } |
                Where-Object { Test-Path $_ } | Select-Object -First 1)
    $cdf = Join-Path $outDir "VipleSCHid.cdf"
    Set-Content $cdf -Encoding ASCII -Value (
        "[CatalogHeader]`r`nName=VipleSCHid.cat`r`nPublicVersion=0x0000001`r`n" +
        "EncodingType=0x00010001`r`nCATATTR1=0x10010001:OSAttr:2:6.3`r`n`r`n" +
        "[CatalogFiles]`r`n<hash>VipleSCHid_Driver.dll=$dll`r`n<hash>VipleSCHid.inf=$infPath`r`n")
    Push-Location $outDir; & $makeCat $cdf; Pop-Location
}
if (Test-Path $catFile) {
    Write-Host "[SC-HID] 簽署 .cat..."
    & $signTool sign /sm /fd SHA256 /sha1 $thumb /t http://timestamp.digicert.com $catFile
    if ($LASTEXITCODE -ne 0) { & $signTool sign /sm /fd SHA256 /sha1 $thumb $catFile }
    Write-Host "[SC-HID] ✅ .cat 已簽署"
} else {
    Write-Host "[SC-HID] ⚠ .cat 產生失敗（driver 仍可在 test-signing 模式安裝）" -ForegroundColor Yellow
}

# ── 安裝到本機（-Install，host 端，需系統管理員）─────────────────────────────
if ($Install) {
    Write-Host "[SC-HID] 安裝憑證到 Trusted Root CA + Trusted Publishers..."
    Import-Certificate -FilePath $certFile -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $certFile -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

    Write-Host "[SC-HID] 安裝 driver（pnputil）..."
    & pnputil /add-driver $infPath /install
    if ($LASTEXITCODE -ne 0) { Fail "pnputil 安裝失敗" }
    & pnputil /scan-devices
    Write-Host "[SC-HID] ✅ 安裝完成"
}

# ── 複製產物到模組根目錄（Sunshine 打包用）──────────────────────────────────
Copy-Item $dll (Join-Path $scriptDir "VipleSCHid_Driver.dll") -Force
if (Test-Path $catFile) { Copy-Item $catFile (Join-Path $scriptDir "VipleSCHid.cat") -Force }

Write-Host ""
Write-Host "[SC-HID] 完成。產物："
Write-Host "  DLL : $dll"
if (Test-Path $catFile) { Write-Host "  CAT : $catFile" }
Write-Host "  CER : $certFile（安裝到 host 時需要）"
