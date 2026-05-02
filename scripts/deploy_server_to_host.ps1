#requires -version 5.1
# VipleStream Server — local-side wrapper that uploads + deploys a release
# zip to the remote streaming host via SSH (BatchMode, OpenSSH default key).
#
# Usage:
#   pwsh scripts\deploy_server_to_host.ps1                    # latest release/
#   pwsh scripts\deploy_server_to_host.ps1 -Version 1.3.310   # specific zip
#   pwsh scripts\deploy_server_to_host.ps1 -Host alt-host.lan
#
# Prereqs:
#   - SSH key in ~/.ssh/id_ed25519 already in remote authorized_keys
#   - Remote host running OpenSSH server with sshd-launched session having
#     admin rights (default for our <host> setup; runs as
#     SYSTEM or elevated user)
#   - Remote install path = C:\Program Files\VipleStream-Server\
#   - Remote service name = VipleStreamServer
#
# What this does NOT touch:
#   - %ProgramData%\Sunshine\sunshine.conf (config, paired devices, certs)
#   - any user-level state in HKCU
# Only the binaries + assets/ directory are overwritten.

param(
    [string] $RemoteHost = '<user>@<host>',
    [string] $Version    = '',
    [string] $RemoteUser = ''  # optional: use 'user@' style instead of $RemoteHost
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $root) {
    $root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}
$releaseDir = Join-Path $root 'release'

# Pick zip
if ($Version) {
    $zip = Join-Path $releaseDir ("VipleStream-Server-$Version.zip")
    if (-not (Test-Path $zip)) {
        throw ('Zip not found: ' + $zip)
    }
} else {
    $zip = Get-ChildItem -Path $releaseDir -Filter 'VipleStream-Server-*.zip' -File |
           Sort-Object LastWriteTime -Descending |
           Select-Object -First 1 |
           ForEach-Object { $_.FullName }
    if (-not $zip) {
        throw ('No VipleStream-Server-*.zip found in ' + $releaseDir)
    }
}
$zipName = Split-Path -Leaf $zip
Write-Host ('Local zip : ' + $zipName + '  (' + [Math]::Round((Get-Item $zip).Length / 1MB, 1) + ' MB)')
Write-Host ('Remote    : ' + $RemoteHost)

# Files to upload
$remoteDeploy = Join-Path $PSScriptRoot 'deploy_server_remote.ps1'
if (-not (Test-Path $remoteDeploy)) {
    throw ('Sibling script not found: ' + $remoteDeploy)
}

# Remote staging dir on host's home
$remoteHome   = '<user-home>'  # Cygwin/OpenSSH-style path; works for scp
$remoteZip    = "C:" + $remoteHome + "/" + $zipName
$remoteScript = "C:" + $remoteHome + "/deploy_server_remote.ps1"

# 1/3 — upload zip + helper script
Write-Host '[upload 1/2] zip ...'
& scp -B -o ConnectTimeout=15 $zip ($RemoteHost + ':' + $remoteZip)
if ($LASTEXITCODE -ne 0) { throw ('scp zip failed (exit ' + $LASTEXITCODE + ')') }

Write-Host '[upload 2/2] deploy_server_remote.ps1 ...'
& scp -B -o ConnectTimeout=15 $remoteDeploy ($RemoteHost + ':' + $remoteScript)
if ($LASTEXITCODE -ne 0) { throw ('scp script failed (exit ' + $LASTEXITCODE + ')') }

# 2/3 — invoke remote deploy
$remoteCmd = "powershell -ExecutionPolicy Bypass -File $remoteScript -ZipPath $remoteZip"
Write-Host ('[deploy] ssh ' + $RemoteHost)
Write-Host ('[deploy] cmd : ' + $remoteCmd)
& ssh -o BatchMode=yes -o ConnectTimeout=15 $RemoteHost $remoteCmd
if ($LASTEXITCODE -ne 0) { throw ('remote deploy failed (exit ' + $LASTEXITCODE + ')') }

# 3/3 — clean up uploaded zip + script (keep nothing on host)
Write-Host '[cleanup] removing uploaded zip + script from host...'
& ssh -o BatchMode=yes $RemoteHost "powershell -Command `"Remove-Item '$remoteZip' -Force -ErrorAction SilentlyContinue; Remove-Item '$remoteScript' -Force -ErrorAction SilentlyContinue`""
# don't fail on cleanup errors

Write-Host ''
Write-Host 'Deploy complete.'
