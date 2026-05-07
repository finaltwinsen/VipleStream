# §B-DUMP-CLEANUP 2026-05-07
# Restore VipleStream registry settings to defaults after compare_fruc_engines.ps1
# Crashed / aborted mid-run.
#
# compare_fruc_engines.ps1 modifies these registry values to switch engines:
#   - rendererSelection (DWord)     : 0=RS_VULKAN, 1=RS_D3D11
#   - frucBackend       (DWord)     : 0=GENERIC, 1=NVIDIA_OF, 2=DIRECTML, 3=NCNN
#   - frameInterpolation (String)   : "true" / "false"
#   - videoDecoderSelection (DWord) : 0=AUTO, 1=FORCE_SW, 2=FORCE_HW
#
# Normal exit restores via try/finally.  But if PowerShell is Ctrl+C'd
# or process-killed mid-run, registry stays at the last-iteration's
# experimental value (e.g. NCNN, FORCE_HW) → next interactive launch
# user sees broken settings.
#
# This script puts everything back to safe defaults (D3D11VA + Generic
# + interpolation off + auto decoder).
#
# Usage:
#   pwsh scripts\benchmark\restore_fruc_settings.ps1 [-RestoreInterp:$true]

param(
    [bool] $RestoreInterp = $false   # default: leave frameInterpolation off
)

$reg = "HKCU:\SOFTWARE\VipleStream\VipleStream"
if (-not (Test-Path $reg)) {
    Write-Host "[restore-fruc] No VipleStream registry key — nothing to restore." -ForegroundColor DarkYellow
    exit 0
}

function Set-Reg($name, $value, $type) {
    $cur = (Get-ItemProperty -Path $reg -Name $name -ErrorAction SilentlyContinue).$name
    if ($null -eq $cur) {
        Write-Host "[restore-fruc] $name was unset (no change needed)" -ForegroundColor DarkGray
        return
    }
    Set-ItemProperty -Path $reg -Name $name -Value $value -Type $type
    Write-Host "[restore-fruc] $name : $cur -> $value ($type)" -ForegroundColor Cyan
}

Write-Host "=== VipleStream FRUC settings restore ===" -ForegroundColor Cyan
Write-Host "(safe defaults: D3D11VA renderer + Generic FRUC backend + frameInterpolation=$RestoreInterp + auto decoder)"
Write-Host ""

Set-Reg rendererSelection      1                                "DWord"
Set-Reg frucBackend            0                                "DWord"
Set-Reg frameInterpolation     ([string]$RestoreInterp.ToString().ToLower()) "String"
Set-Reg videoDecoderSelection  0                                "DWord"

Write-Host ""
Write-Host "[restore-fruc] done" -ForegroundColor Green
Write-Host "Verify via: Get-ItemProperty -Path '$reg' | Select-Object rendererSelection, frucBackend, frameInterpolation, videoDecoderSelection"
