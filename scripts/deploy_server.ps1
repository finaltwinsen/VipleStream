#requires -version 5.1
#
# VipleStream Server localhost deploy script.
#
# Use case: dev workstation also runs the streaming host (rare); copy
# the freshly-built binaries from temp\sunshine\ into the canonical
# install path and restart the local service.  For deploying TO A
# DIFFERENT MACHINE, use scripts\deploy_server_to_host.ps1 instead
# (SSH-based remote deploy).
#
# Service / exe names are the post-rebrand VipleStream names; the
# pre-v1.2.43 SunshineService / sunshine.exe / sunshinesvc.exe paths
# don't apply.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }

$configPath = Join-Path $root 'build-config.local.cmd'
$dest = 'C:\Program Files\VipleStream-Server'
if (Test-Path $configPath) {
    $content = Get-Content $configPath -Raw
    if ($content -match 'DEPLOY_SERVER=(.+)') { $dest = $Matches[1].Trim('"') }
}

$src = Join-Path $root 'temp\sunshine'

if (-not (Test-Path $src)) {
    throw ("Build staging not found: $src — run build_sunshine.cmd first")
}
if (-not (Test-Path $dest)) {
    throw ("Install path not found: $dest — install VipleStream-Server first or set DEPLOY_SERVER in build-config.local.cmd")
}

Write-Host '[1/3] Stopping service...'
Stop-Service VipleStreamServer -Force -ErrorAction SilentlyContinue
$tries = 0
while ((Get-Service VipleStreamServer -ErrorAction SilentlyContinue).Status -ne 'Stopped' -and $tries -lt 15) {
    Start-Sleep -Seconds 1
    $tries++
}
Get-Process viplestream-server, viplestream-svc -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Write-Host '[2/3] Deploying files...'
$exes = @('viplestream-server.exe', 'viplestream-svc.exe', 'viple-splash.exe')
foreach ($exe in $exes) {
    $sp = Join-Path $src $exe
    if (Test-Path $sp) {
        Copy-Item $sp -Destination (Join-Path $dest $exe) -Force
        Write-Host "  copied $exe"
    }
}
# Diagnostic helpers under tools\ subdir if the install layout has it
foreach ($f in @('dxgi-info.exe', 'audio-info.exe')) {
    $sp = Join-Path $src $f
    if (-not (Test-Path $sp)) { continue }
    $toolsDir = Join-Path $dest 'tools'
    if (Test-Path $toolsDir) {
        Copy-Item $sp -Destination (Join-Path $toolsDir $f) -Force
        Write-Host "  copied tools\$f"
    } else {
        # Fall back to install root if no tools/ subdir exists
        Copy-Item $sp -Destination (Join-Path $dest $f) -Force
        Write-Host "  copied $f"
    }
}
$assetsSrc = Join-Path $src 'assets'
$assetsDst = Join-Path $dest 'assets'
if (Test-Path $assetsSrc) {
    Copy-Item (Join-Path $assetsSrc '*') -Destination $assetsDst -Recurse -Force
    Write-Host '  copied assets/'
}

Write-Host '[3/3] Starting service...'
Start-Service VipleStreamServer
Start-Sleep -Seconds 3

$p = Get-Process viplestream-server -ErrorAction SilentlyContinue
if ($p) {
    Write-Host "Deploy complete. VipleStream-Server PID: $($p.Id)"
} else {
    Write-Host 'Warning: viplestream-server process not found after start'
}
