#requires -version 5.1
# VipleStream Server remote deploy script.
#
# Runs on the streaming host (<host>) via SSH.  The
# accompanying client-side wrapper `scripts/deploy_server_to_host.ps1`
# uploads the zip + this script and invokes it.  Idempotent: stage →
# stop service → copy → start → verify realm.
#
# Hard-codes the install path to the canonical `C:\Program Files\
# VipleStream-Server`; if you ever change that, also update CLAUDE.md
# and `scripts/deploy_server.ps1` (the localhost variant).

param(
    [Parameter(Mandatory = $true)]
    [string] $ZipPath,

    [string] $InstallPath = 'C:\Program Files\VipleStream-Server',

    [string] $StagePath   = 'C:\Users\<user>\.viplestream-server-staging',

    [string] $ServiceName = 'VipleStreamServer'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $ZipPath)) {
    throw ('Zip not found: ' + $ZipPath)
}
if (-not (Test-Path $InstallPath)) {
    throw ('Install path not found: ' + $InstallPath)
}

Write-Output ('Server : ' + $env:COMPUTERNAME)
Write-Output ('Service: ' + $ServiceName)
Write-Output ('Install: ' + $InstallPath)
Write-Output ('Zip    : ' + $ZipPath)

# 1/6 — Expand archive
Write-Output '[1/6] Expanding archive...'
if (Test-Path $StagePath) { Remove-Item $StagePath -Recurse -Force }
New-Item -ItemType Directory -Force -Path $StagePath | Out-Null
Expand-Archive -Path $ZipPath -DestinationPath $StagePath -Force

$top = Get-ChildItem $StagePath
Write-Output 'Stage entries:'
$top | Format-Table Name, Mode, Length -AutoSize | Out-String | Write-Output
if ($top.Count -eq 1 -and $top[0].PSIsContainer) {
    $Src = $top[0].FullName
} else {
    $Src = $StagePath
}
Write-Output ('Using src: ' + $Src)

# 2/6 — Stop service
Write-Output '[2/6] Stopping service...'
Stop-Service $ServiceName -Force
$tries = 0
while ((Get-Service $ServiceName).Status -ne 'Stopped' -and $tries -lt 20) {
    Start-Sleep -Seconds 1
    $tries++
}
Write-Output ('Service status after ' + $tries + 's: ' + (Get-Service $ServiceName).Status)
Get-Process viplestream-server, viplestream-svc -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# 3/6 — Copy binaries
Write-Output '[3/6] Copying binaries...'
$exes = @('viplestream-server.exe', 'viplestream-svc.exe', 'viple-splash.exe',
          'dxgi-info.exe', 'audio-info.exe')
foreach ($exe in $exes) {
    $srcPath = Join-Path $Src $exe
    $dstPath = Join-Path $InstallPath $exe
    if (Test-Path $srcPath) {
        Copy-Item $srcPath -Destination $dstPath -Force
        Write-Output ('  copied ' + $exe)
    } else {
        Write-Output ('  skip (no source) ' + $exe)
    }
}

# 4/6 — Copy assets/
Write-Output '[4/6] Copying assets/...'
$assetsSrc = Join-Path $Src 'assets'
$assetsDst = Join-Path $InstallPath 'assets'
if (Test-Path $assetsSrc) {
    Copy-Item (Join-Path $assetsSrc '*') -Destination $assetsDst -Recurse -Force
    Write-Output '  assets/ overwritten'
} else {
    Write-Output '  skip (no assets/ in zip)'
}

# 5/6 — Start service
Write-Output '[5/6] Starting service...'
Start-Service $ServiceName
$tries = 0
while ((Get-Service $ServiceName).Status -ne 'Running' -and $tries -lt 15) {
    Start-Sleep -Seconds 1
    $tries++
}
Write-Output ('Service status after ' + $tries + 's: ' + (Get-Service $ServiceName).Status)

# 6/6 — Smoke check via curl (PS 5.1 doesn't have -SkipCertificateCheck)
Write-Output '[6/6] Web UI realm probe via curl...'
Start-Sleep -Seconds 3
try {
    $headers = & curl.exe -k -s -o NUL -D - --max-time 10 'https://localhost:47990/'
    foreach ($line in $headers) {
        if ($line -match '^HTTP/' -or $line -match '^WWW-Authenticate') {
            Write-Output ('  ' + $line)
        }
    }
    if ($headers -match 'VipleStream-Server Web UI') {
        Write-Output 'OK realm rebrand verified live'
    } elseif ($headers -match 'Sunshine Gamestream Host') {
        Write-Output 'WARNING old realm still served — exe may not be refreshed'
    }
} catch {
    Write-Output ('  curl probe failed: ' + $_.Exception.Message)
}

# Cleanup staging (zip is left in place — caller's responsibility)
Remove-Item $StagePath -Recurse -Force -ErrorAction SilentlyContinue

Write-Output ''
Write-Output 'Deployed exe:'
Get-Item (Join-Path $InstallPath 'viplestream-server.exe') |
    Format-Table Name, LastWriteTime, Length -AutoSize | Out-String | Write-Output
Write-Output 'Done.'
