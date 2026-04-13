<#
.SYNOPSIS
    VipleStream Debug Pairing Tool
    Extracts the Moonlight client certificate from an Android device via ADB
    and injects it into Sunshine's trusted device list for PIN-free pairing.

.DESCRIPTION
    This script enables quick remote debugging by:
    1. Pulling the client certificate from the Android device
    2. Adding it to Sunshine's sunshine_state.json as a trusted device
    3. Restarting Sunshine to apply changes

    After running this script, the phone can connect to Sunshine without
    needing to enter a PIN on the server side.

.PARAMETER DeviceName
    The display name for this debug device (default: "VipleStream-Debug")

.PARAMETER Remove
    Remove the debug device entry instead of adding it

.EXAMPLE
    .\debug_pair.ps1
    .\debug_pair.ps1 -DeviceName "MyPhone"
    .\debug_pair.ps1 -Remove
#>

param(
    [string]$DeviceName = "VipleStream-Debug",
    [switch]$Remove
)

$ErrorActionPreference = "Stop"

# --- Configuration ---
$ADB = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$SunshineState = "C:\Program Files\Sunshine\config\sunshine_state.json"
$SunshineService = "SunshineService"
$TempCert = "$env:TEMP\moonlight_debug_client.crt"

# --- Helpers ---
function Write-Status($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Write-OK($msg)     { Write-Host "[+] $msg" -ForegroundColor Green }
function Write-Err($msg)    { Write-Host "[-] $msg" -ForegroundColor Red }
function Write-Warn($msg)   { Write-Host "[!] $msg" -ForegroundColor Yellow }

# --- Check prerequisites ---
if (-not (Test-Path $ADB)) {
    Write-Err "ADB not found at: $ADB"
    exit 1
}
if (-not (Test-Path $SunshineState)) {
    Write-Err "Sunshine state file not found at: $SunshineState"
    exit 1
}

# --- Remove mode ---
if ($Remove) {
    Write-Status "Removing debug device '$DeviceName' from Sunshine..."
    $state = Get-Content $SunshineState -Raw | ConvertFrom-Json

    $before = $state.root.named_devices.Count
    $state.root.named_devices = @($state.root.named_devices | Where-Object { $_.name -ne $DeviceName })
    $after = $state.root.named_devices.Count

    if ($before -eq $after) {
        Write-Warn "Device '$DeviceName' not found in Sunshine config."
        exit 0
    }

    $state | ConvertTo-Json -Depth 10 | Set-Content $SunshineState -Encoding UTF8
    Write-OK "Removed '$DeviceName'. Restart Sunshine to apply."
    exit 0
}

# --- Check ADB device ---
Write-Status "Checking for connected Android device..."
$devices = & $ADB devices 2>&1
$connected = $devices | Select-String -Pattern "^\w+\s+device$"
if (-not $connected) {
    Write-Err "No Android device connected via ADB."
    Write-Warn "Connect via USB and enable USB debugging, or use:"
    Write-Warn "  adb connect <phone-ip>:5555"
    exit 1
}
$deviceId = ($connected -split "\s+")[0]
Write-OK "Found device: $deviceId"

# --- Pull client certificate ---
Write-Status "Extracting Moonlight client certificate..."

# Try debug package first, then release
$certPaths = @(
    "/data/data/com.limelight.debug/files/client.crt",
    "/data/data/com.limelight/files/client.crt"
)
$pulled = $false

foreach ($remotePath in $certPaths) {
    # Use run-as to access app-private storage
    $packageName = if ($remotePath -match "com\.limelight\.debug") { "com.limelight.debug" } else { "com.limelight" }

    # Method 1: run-as (works on debuggable builds)
    $certContent = & $ADB shell "run-as $packageName cat files/client.crt" 2>&1
    if ($certContent -match "BEGIN CERTIFICATE") {
        $certContent | Out-File -FilePath $TempCert -Encoding ASCII
        $pulled = $true
        Write-OK "Certificate extracted via run-as ($packageName)"
        break
    }

    # Method 2: direct pull (requires root or permissive SELinux)
    & $ADB pull $remotePath $TempCert 2>$null
    if ((Test-Path $TempCert) -and (Get-Content $TempCert -Raw) -match "BEGIN CERTIFICATE") {
        $pulled = $true
        Write-OK "Certificate extracted via direct pull"
        break
    }
}

if (-not $pulled) {
    Write-Err "Could not extract client certificate from device."
    Write-Warn "Possible causes:"
    Write-Warn "  - App not installed or never launched (certificate not generated)"
    Write-Warn "  - Need debuggable build (run-as requires android:debuggable=true)"
    Write-Warn "  - Try: adb shell run-as com.limelight.debug cat files/client.crt"
    exit 1
}

# --- Read and validate certificate ---
$certPem = (Get-Content $TempCert -Raw).Trim()
if ($certPem -notmatch "-----BEGIN CERTIFICATE-----") {
    Write-Err "Invalid certificate format."
    exit 1
}
# Normalize line endings for JSON embedding
$certPem = $certPem -replace "`r`n", "`n"
if (-not $certPem.EndsWith("`n")) { $certPem += "`n" }

Write-OK "Certificate validated ($(($certPem -split "`n").Count - 2) lines)"

# --- Inject into Sunshine state ---
Write-Status "Injecting certificate into Sunshine config..."
$state = Get-Content $SunshineState -Raw | ConvertFrom-Json

# Check for existing entry with same name
$existing = $state.root.named_devices | Where-Object { $_.name -eq $DeviceName }
if ($existing) {
    Write-Warn "Device '$DeviceName' already exists, updating certificate..."
    $existing.cert = $certPem
} else {
    # Generate a UUID for the new device
    $uuid = [guid]::NewGuid().ToString().ToUpper()
    $newDevice = [PSCustomObject]@{
        name    = $DeviceName
        cert    = $certPem
        uuid    = $uuid
        enabled = "true"
    }
    $state.root.named_devices += $newDevice
    Write-OK "Added new device: $DeviceName (UUID: $uuid)"
}

# Write back
$state | ConvertTo-Json -Depth 10 | Set-Content $SunshineState -Encoding UTF8
Write-OK "Sunshine config updated."

# --- Restart Sunshine ---
Write-Status "Restarting Sunshine service..."
try {
    $svc = Get-Service -Name $SunshineService -ErrorAction SilentlyContinue
    if ($svc) {
        Restart-Service -Name $SunshineService -Force
        Write-OK "Sunshine service restarted."
    } else {
        # Try restarting the process directly
        $proc = Get-Process -Name "sunshine" -ErrorAction SilentlyContinue
        if ($proc) {
            $sunshineExe = $proc.Path
            Stop-Process -Name "sunshine" -Force
            Start-Sleep -Seconds 2
            Start-Process -FilePath $sunshineExe
            Write-OK "Sunshine process restarted."
        } else {
            Write-Warn "Sunshine not running as service or process. Please restart manually."
        }
    }
} catch {
    Write-Warn "Could not restart Sunshine automatically: $_"
    Write-Warn "Please restart Sunshine manually."
}

# --- Cleanup ---
Remove-Item $TempCert -Force -ErrorAction SilentlyContinue

# --- Summary ---
Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  Debug Pairing Complete!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "  Device: $DeviceName"
Write-Host "  The phone can now connect to this PC"
Write-Host "  without entering a PIN."
Write-Host ""
Write-Host "  To remove after debugging:" -ForegroundColor Yellow
Write-Host "    .\debug_pair.ps1 -Remove" -ForegroundColor Yellow
Write-Host ""
