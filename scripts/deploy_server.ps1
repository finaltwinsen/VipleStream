$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }

$configPath = Join-Path $root 'build-config.local.cmd'
$dest = 'C:\Program Files\Sunshine'
if (Test-Path $configPath) {
    $content = Get-Content $configPath -Raw
    if ($content -match 'DEPLOY_SERVER=(.+)') { $dest = $Matches[1].Trim('"') }
}

$src = Join-Path $root 'temp\sunshine'

Write-Host '[1/3] Stopping service...'
Stop-Service SunshineService -Force -ErrorAction SilentlyContinue
Start-Sleep 2
Stop-Process -Name sunshine -Force -ErrorAction SilentlyContinue
Stop-Process -Name sunshinesvc -Force -ErrorAction SilentlyContinue
Start-Sleep 1

Write-Host '[2/3] Deploying files...'
Copy-Item "$src\sunshine.exe" "$dest\sunshine.exe" -Force
if (Test-Path "$src\sunshinesvc.exe") {
    Copy-Item "$src\sunshinesvc.exe" "$dest\sunshinesvc.exe" -Force
}
foreach ($f in @('dxgi-info.exe','audio-info.exe')) {
    if (Test-Path "$src\$f") {
        Copy-Item "$src\$f" "$dest\tools\$f" -Force
    }
}
Copy-Item "$src\assets\*" "$dest\assets\" -Recurse -Force

Write-Host '[3/3] Starting service...'
Start-Service SunshineService
Start-Sleep 3

$p = Get-Process sunshine -ErrorAction SilentlyContinue
if ($p) {
    Write-Host "Deploy complete. Sunshine PID: $($p.Id)"
} else {
    Write-Host 'Warning: Sunshine process not found after start'
}
