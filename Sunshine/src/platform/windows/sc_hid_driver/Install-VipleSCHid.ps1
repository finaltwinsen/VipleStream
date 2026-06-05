# §SC-HID: Install VipleStream Steam Controller Virtual HID Driver
# Requires elevation (admin).  Called once by Sunshine at first startup
# if the VipleSCHid device node is not already present.
#
# Usage: powershell -ExecutionPolicy Bypass -File Install-VipleSCHid.ps1

param(
    [string]$DriverDir = $PSScriptRoot
)

$inf  = Join-Path $DriverDir "VipleSCHid.inf"
$hwid = "Viple\SteamController"

if (-not (Test-Path $inf)) {
    Write-Error "[SC-HID] VipleSCHid.inf not found at: $inf"
    exit 1
}

# Check if already installed
$existing = Get-PnpDevice -InstanceId "*Viple*SteamController*" -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "[SC-HID] Virtual Steam Controller device already present — skipping install."
    exit 0
}

Write-Host "[SC-HID] Installing VipleSCHid UMDF2 driver..."
& pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0) {
    Write-Error "[SC-HID] pnputil failed (exit $LASTEXITCODE)"
    exit 1
}

# Create the device node (triggers driver binding)
& devcon install $inf $hwid 2>&1
if ($LASTEXITCODE -ne 0) {
    # devcon may not be present; fall back to using pnputil with devnode creation
    Write-Warning "[SC-HID] devcon not found or failed — trying PnpUtil..."
    & pnputil /scan-devices 2>&1
}

Write-Host "[SC-HID] VipleSCHid driver installed successfully."
exit 0
