$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }

$configPath = Join-Path $root 'build-config.local.cmd'
$dest = 'C:\Program Files\Moonlight Game Streaming'
if (Test-Path $configPath) {
    $content = Get-Content $configPath -Raw
    if ($content -match 'DEPLOY_CLIENT=(.+)') { $dest = $Matches[1].Trim('"') }
}

$src = Join-Path $root 'temp\moonlight'

Write-Host 'Deploying VipleStream Client (full sync)...'

if (Test-Path "$dest\Moonlight.exe") {
    Copy-Item "$dest\Moonlight.exe" "$dest\Moonlight.exe.bak" -Force
}

Copy-Item "$src\*" "$dest\" -Recurse -Force

$count = (Get-ChildItem "$dest" -File).Count
Write-Host "Deploy complete: $count files in $dest"
