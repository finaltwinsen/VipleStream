<#
.SYNOPSIS
    VipleStream Debug Pairing Tool
    Extracts the Moonlight client certificate from an Android device via ADB
    and injects it into Sunshine's trusted device list for PIN-free pairing.

.DESCRIPTION
    This script enables quick remote debugging by:
    1. Pulling the client certificate from the Android device (via run-as)
    2. Adding it to Sunshine's sunshine_state.json as a trusted device
    3. Restarting Sunshine to apply changes

    After running this script, the phone can connect to Sunshine without
    needing to enter a PIN on the server side.

    Two operating modes:

    LOCAL MODE (default):
        Adb device + Sunshine install must be on the same machine that runs
        the script. Uses local file paths and local Service control.

    REMOTE MODE (-RemoteHost <user@host>):
        Adb device is on the machine running the script (e.g. dev laptop)
        but Sunshine lives on another machine reached via SSH (e.g. test
        host). The script will:
            1. Pull the cert locally via adb
            2. SCP the cert + an inline inject script to the remote host
            3. SSH-invoke the inject script to update sunshine_state.json
               and restart the service on the remote
        Requires OpenSSH client (ssh + scp on PATH) and key-based auth to
        the remote (script uses -o BatchMode=yes; no password prompts).

.PARAMETER DeviceName
    The display name for this debug device (default: "VipleStream-Debug")

.PARAMETER Remove
    Remove the debug device entry instead of adding it. Honours -RemoteHost.

.PARAMETER RemoteHost
    OpenSSH-style target spec (e.g. "<user>@<host>"). When set,
    sunshine_state.json on the remote is updated; nothing is touched on
    the local machine besides pulling the cert via adb.

.PARAMETER AdbDeviceId
    Restrict adb to a specific device serial (passed as `adb -s <id>`).
    Default: "<device-serial>" (Pixel 5 / VipleStream test device, per
    feedback_test_workflow memory). Use empty string ("") to use whatever
    `adb devices` returns first.

.EXAMPLE
    .\debug_pair.ps1
    .\debug_pair.ps1 -DeviceName "MyPhone"
    .\debug_pair.ps1 -Remove
    .\debug_pair.ps1 -RemoteHost <user>@<host>
    .\debug_pair.ps1 -RemoteHost <user>@<host> -Remove
#>

param(
    [string]$DeviceName = "VipleStream-Debug",
    [switch]$Remove,
    [string]$RemoteHost = "",
    [string]$AdbDeviceId = "<device-serial>"
)

$ErrorActionPreference = "Stop"

# --- Configuration ---
# Paths / names match the v1.2.93–95 rebrand. If you point this at a vanilla
# Sunshine install instead of VipleStream-Server, override $SunshineState
# manually. Service name has a fallback below for the same reason.
$ADB = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$SunshineState = "C:\Program Files\VipleStream-Server\config\sunshine_state.json"
$SunshineService = "VipleStreamServer"
$TempCert = "$env:TEMP\viplestream_debug_client.crt"

# --- Helpers ---
function Write-Status($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Write-OK($msg)     { Write-Host "[+] $msg" -ForegroundColor Green }
function Write-Err($msg)    { Write-Host "[-] $msg" -ForegroundColor Red }
function Write-Warn($msg)   { Write-Host "[!] $msg" -ForegroundColor Yellow }

# --- adb wrapper that respects -AdbDeviceId ---
function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Args)
    if ([string]::IsNullOrEmpty($AdbDeviceId)) {
        & $ADB @Args
    } else {
        & $ADB -s $AdbDeviceId @Args
    }
}

# --- Remote host inject script body ---
# Self-contained; reads cert from $env:TEMP\viplestream_debug_client.crt,
# updates the JSON, restarts the service. Designed to be SCP'd to the
# remote $env:TEMP and invoked via `ssh ... powershell -File <path>`.
function Get-RemoteInjectScript {
    param(
        [string]$DeviceName,
        [string]$StatePath,
        [string]$ServiceName,
        [string]$RemoteCertPath,
        [bool]$RemoveMode
    )
    $action = if ($RemoveMode) { "remove" } else { "add" }
    return @"
`$ErrorActionPreference = "Stop"
`$statePath = '$StatePath'
`$serviceName = '$ServiceName'
`$deviceName = '$DeviceName'
`$certPath = '$RemoteCertPath'
`$action = '$action'
if (-not (Test-Path `$statePath)) { Write-Host "[-] State file not found: `$statePath"; exit 1 }
`$state = Get-Content `$statePath -Raw | ConvertFrom-Json
if (`$action -eq 'remove') {
    `$before = `$state.root.named_devices.Count
    `$state.root.named_devices = @(`$state.root.named_devices | Where-Object { `$_.name -ne `$deviceName })
    `$after = `$state.root.named_devices.Count
    if (`$before -eq `$after) { Write-Host "[!] Device '`$deviceName' not found"; exit 0 }
    Write-Host "[+] Removed '`$deviceName'"
} else {
    `$cert = (Get-Content `$certPath -Raw).Trim() -replace "``r``n","``n"
    if (-not `$cert.EndsWith("``n")) { `$cert += "``n" }
    `$existing = `$state.root.named_devices | Where-Object { `$_.name -eq `$deviceName }
    if (`$existing) {
        `$existing.cert = `$cert
        Write-Host "[+] Updated existing entry: `$deviceName"
    } else {
        `$newDev = [PSCustomObject]@{
            name    = `$deviceName
            cert    = `$cert
            uuid    = ([guid]::NewGuid().ToString().ToUpper())
            enabled = "true"
        }
        `$state.root.named_devices += `$newDev
        Write-Host "[+] Added new entry: `$deviceName"
    }
}
`$state | ConvertTo-Json -Depth 10 | Set-Content `$statePath -Encoding UTF8
try {
    Restart-Service -Name `$serviceName -Force
    Write-Host "[+] Service '`$serviceName' restarted"
} catch {
    Write-Host "[!] Could not restart service '`$serviceName': `$_"
}
"@
}

# --- Run the inject script either locally or via SSH ---
function Invoke-Inject {
    param([bool]$RemoveMode)
    if ($RemoteHost) {
        # Remote mode: SCP the cert (if not Remove) + inject script, then SSH-invoke.
        # SCP doesn't expand env vars on the remote side — it treats the path
        # literally. Probe `$env:TEMP` once via SSH so we can use the real
        # Windows temp path for both SCP and the inject script.
        Write-Status "Probing remote TEMP path..."
        $remoteTemp = (& ssh -o BatchMode=yes $RemoteHost 'powershell -NoProfile -Command "Write-Output $env:TEMP"' 2>$null | Select-Object -First 1).Trim()
        if (-not $remoteTemp) { Write-Err "Failed to probe remote TEMP path via SSH"; exit 1 }
        $remoteTemp = $remoteTemp -replace '\\','/'
        $remoteCert   = "$remoteTemp/viplestream_debug_client.crt"
        $remoteScript = "$remoteTemp/viplestream_debug_inject.ps1"

        $scriptBody = Get-RemoteInjectScript -DeviceName $DeviceName -StatePath $SunshineState `
                          -ServiceName $SunshineService -RemoteCertPath $remoteCert -RemoveMode $RemoveMode
        $localScript = "$env:TEMP\viplestream_debug_inject.ps1"
        $scriptBody | Set-Content $localScript -Encoding UTF8

        if (-not $RemoveMode) {
            Write-Status "SCP cert to ${RemoteHost}:${remoteCert}..."
            & scp -o BatchMode=yes $TempCert "${RemoteHost}:${remoteCert}"
            if ($LASTEXITCODE -ne 0) { Write-Err "scp cert failed (exit $LASTEXITCODE)"; exit 1 }
        }

        Write-Status "SCP inject script to ${RemoteHost}:${remoteScript}..."
        & scp -o BatchMode=yes $localScript "${RemoteHost}:${remoteScript}"
        if ($LASTEXITCODE -ne 0) { Write-Err "scp script failed (exit $LASTEXITCODE)"; exit 1 }

        Write-Status "SSH invoke inject on remote..."
        & ssh -o BatchMode=yes $RemoteHost "powershell -ExecutionPolicy Bypass -File `"$remoteScript`""
        if ($LASTEXITCODE -ne 0) { Write-Err "remote inject failed (exit $LASTEXITCODE)"; exit 1 }

        # Cleanup remote artefacts
        & ssh -o BatchMode=yes $RemoteHost "powershell -Command `"Remove-Item -Force '$remoteCert','$remoteScript' -ErrorAction SilentlyContinue`"" | Out-Null
        Remove-Item $localScript -Force -ErrorAction SilentlyContinue
        return
    }

    # Local mode: do the work in-process.
    if (-not (Test-Path $SunshineState)) {
        Write-Err "Sunshine state file not found at: $SunshineState"
        exit 1
    }
    $state = Get-Content $SunshineState -Raw | ConvertFrom-Json
    if ($RemoveMode) {
        $before = $state.root.named_devices.Count
        $state.root.named_devices = @($state.root.named_devices | Where-Object { $_.name -ne $DeviceName })
        $after = $state.root.named_devices.Count
        if ($before -eq $after) { Write-Warn "Device '$DeviceName' not found"; return }
        Write-OK "Removed '$DeviceName'"
    } else {
        $cert = (Get-Content $TempCert -Raw).Trim() -replace "`r`n","`n"
        if (-not $cert.EndsWith("`n")) { $cert += "`n" }
        $existing = $state.root.named_devices | Where-Object { $_.name -eq $DeviceName }
        if ($existing) {
            $existing.cert = $cert
            Write-OK "Updated existing entry: $DeviceName"
        } else {
            $uuid = [guid]::NewGuid().ToString().ToUpper()
            $newDev = [PSCustomObject]@{
                name    = $DeviceName
                cert    = $cert
                uuid    = $uuid
                enabled = "true"
            }
            $state.root.named_devices += $newDev
            Write-OK "Added new entry: $DeviceName ($uuid)"
        }
    }
    $state | ConvertTo-Json -Depth 10 | Set-Content $SunshineState -Encoding UTF8
    try {
        $svc = Get-Service -Name $SunshineService -ErrorAction SilentlyContinue
        if ($svc) {
            Restart-Service -Name $SunshineService -Force
            Write-OK "$SunshineService service restarted"
        } else {
            $svcLegacy = Get-Service -Name "SunshineService" -ErrorAction SilentlyContinue
            if ($svcLegacy) {
                Restart-Service -Name "SunshineService" -Force
                Write-OK "Legacy SunshineService restarted"
            } else {
                Write-Warn "Service not found locally; restart server manually"
            }
        }
    } catch {
        Write-Warn "Could not restart service: $_"
    }
}

# --- Check prerequisites ---
if (-not (Test-Path $ADB)) {
    Write-Err "ADB not found at: $ADB"
    exit 1
}
if ($RemoteHost) {
    foreach ($tool in @("ssh","scp")) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            Write-Err "$tool not on PATH (-RemoteHost requires OpenSSH client)"
            exit 1
        }
    }
}

# --- Remove mode short-circuit ---
if ($Remove) {
    Write-Status "Removing debug device '$DeviceName' from Sunshine ($(if ($RemoteHost) { "remote: $RemoteHost" } else { "local" }))..."
    Invoke-Inject -RemoveMode $true
    exit 0
}

# --- Check ADB device ---
Write-Status "Checking adb devices..."
$devices = & $ADB devices 2>&1
$connected = $devices | Select-String -Pattern "^\w+\s+device$"
if (-not $connected) {
    Write-Err "No Android device connected via adb"
    Write-Warn "Connect via USB and enable USB debugging, or use:"
    Write-Warn "  adb connect <phone-ip>:5555"
    exit 1
}
$useDev = if ([string]::IsNullOrEmpty($AdbDeviceId)) { ($connected[0] -split "\s+")[0] } else { $AdbDeviceId }
Write-OK "Using device: $useDev"

# --- Pull client certificate ---
Write-Status "Extracting Moonlight client certificate..."

# Try debug package first, then release. Java namespace inside the APK is
# still "com.limelight" by design (avoids per-file package edits) but the
# applicationId from app/build.gradle is "com.piinsta", which is what
# /data/data/<id>/ is keyed on. Keep the legacy paths as fallback in case
# someone is running an older build.
$pkgs = @("com.piinsta.debug","com.piinsta","com.limelight.debug","com.limelight")
$pulled = $false

foreach ($pkg in $pkgs) {
    # Method 1: run-as (works on debuggable builds)
    $certContent = Invoke-Adb shell "run-as $pkg cat files/client.crt" 2>&1
    if ($certContent -match "BEGIN CERTIFICATE") {
        $certContent | Out-File -FilePath $TempCert -Encoding ASCII
        $pulled = $true
        Write-OK "Certificate extracted via run-as ($pkg)"
        break
    }
    # Method 2: direct pull (requires root or permissive SELinux)
    Invoke-Adb pull "/data/data/$pkg/files/client.crt" $TempCert 2>$null | Out-Null
    if ((Test-Path $TempCert) -and (Get-Content $TempCert -Raw) -match "BEGIN CERTIFICATE") {
        $pulled = $true
        Write-OK "Certificate extracted via direct pull ($pkg)"
        break
    }
}

if (-not $pulled) {
    Write-Err "Could not extract client certificate from device"
    Write-Warn "Possible causes:"
    Write-Warn "  - App not installed or never launched (cert not generated)"
    Write-Warn "  - Need debuggable build (run-as requires android:debuggable=true)"
    Write-Warn "  - Try: adb shell run-as com.piinsta.debug cat files/client.crt"
    exit 1
}

# --- Validate certificate ---
$certPem = (Get-Content $TempCert -Raw).Trim()
if ($certPem -notmatch "-----BEGIN CERTIFICATE-----") {
    Write-Err "Invalid certificate format"
    exit 1
}
$certPem = $certPem -replace "`r`n", "`n"
if (-not $certPem.EndsWith("`n")) { $certPem += "`n" }
$certPem | Set-Content $TempCert -Encoding ASCII -NoNewline
Write-OK "Certificate validated ($(($certPem -split "`n").Count - 2) lines)"

# --- Inject (local or remote) ---
Write-Status "Injecting certificate ($(if ($RemoteHost) { "remote: $RemoteHost" } else { "local" }))..."
Invoke-Inject -RemoveMode $false

# --- Cleanup local temp cert ---
Remove-Item $TempCert -Force -ErrorAction SilentlyContinue

# --- Summary ---
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Debug Pairing Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Device: $DeviceName"
if ($RemoteHost) { Write-Host "  Target: $RemoteHost (remote)" }
Write-Host "  The phone can now connect without entering a PIN."
Write-Host ""
Write-Host "  To remove after debugging:" -ForegroundColor Yellow
if ($RemoteHost) {
    Write-Host "    .\debug_pair.ps1 -Remove -RemoteHost $RemoteHost" -ForegroundColor Yellow
} else {
    Write-Host "    .\debug_pair.ps1 -Remove" -ForegroundColor Yellow
}
Write-Host ""
