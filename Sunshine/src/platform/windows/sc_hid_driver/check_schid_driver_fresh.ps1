#requires -Version 5
<#
  §Q.r17 / build rule 4 (鐵律 4: build script 不能無聲帶舊 binary)

  Fail the Sunshine build if the packaged SC-HID driver DLL is OLDER than its
  source translation unit (VipleSCHid_Driver.cpp). Re-signing the driver needs
  an elevated (admin) shell, so when a source change was compiled but the signed
  DLL was NOT refreshed, an OUTDATED driver had previously shipped silently
  (the §SC-QUEUE-SERIAL-FIX episode). This guard makes that loud.

  Lives next to the driver (tracked) rather than in the gitignored build-tools/,
  so the freshness logic is version-controlled + travels to any builder. It is
  invoked by build_sunshine.cmd (which is itself local/gitignored) before the
  driver files are copied into the package staging dir.

  Driver build inputs (per Build-ScHidDriver.ps1): ONLY VipleSCHid_Driver.cpp is
  compiled (cl.exe /c VipleSCHid_Driver.cpp), and that TU has no local #include.
  VipleSCHid.cpp / VipleSCHid.h are the SERVER-SIDE caller, NOT part of the
  driver binary — deliberately excluded so a server-side edit does not
  false-fail the driver freshness check.

  Exit codes:
    0  fresh, or no driver sources in this checkout  -> caller proceeds
    1  STALE: a driver source is newer than the DLL   -> caller FAILS the build
    2  DLL missing but driver sources present         -> caller WARNS, continues
#>
param(
    [Parameter(Mandatory = $true)][string]$SrcDir,
    [Parameter(Mandatory = $true)][string]$Dll
)

$ErrorActionPreference = 'Stop'

# The only compiled input is VipleSCHid_Driver.cpp; track that stem (plus any
# future header / .rc of the same driver TU). Source extensions only — the
# -Filter also matches VipleSCHid_Driver.dll/.pdb/.obj, which we drop here.
$srcExt  = @('.cpp', '.c', '.h', '.hpp', '.rc')
$sources = @()
if (Test-Path -LiteralPath $SrcDir) {
    $sources = Get-ChildItem -LiteralPath $SrcDir -File -Filter 'VipleSCHid_Driver.*' |
        Where-Object { $srcExt -contains $_.Extension.ToLower() }
}

if ($sources.Count -eq 0) {
    # No driver translation unit in this checkout — nothing to guard.
    exit 0
}

if (-not (Test-Path -LiteralPath $Dll)) {
    Write-Host ("   [WARN] {0} missing - SC-HID controller passthrough will NOT ship" -f [System.IO.Path]::GetFileName($Dll))
    exit 2
}

$dllItem   = Get-Item -LiteralPath $Dll
$newestSrc = $sources | Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($newestSrc.LastWriteTime -gt $dllItem.LastWriteTime) {
    Write-Host ("   [STALE] driver source '{0}' ({1:yyyy-MM-dd HH:mm:ss}) is NEWER than built DLL ({2:yyyy-MM-dd HH:mm:ss})" -f `
        $newestSrc.Name, $newestSrc.LastWriteTime, $dllItem.LastWriteTime)
    exit 1
}

$sigStatus = 'unknown'
try { $sigStatus = (Get-AuthenticodeSignature -LiteralPath $Dll).Status } catch { }
Write-Host ("   [OK] SC-HID driver DLL fresh ({0:yyyy-MM-dd HH:mm:ss}, signature={1})" -f $dllItem.LastWriteTime, $sigStatus)
exit 0
