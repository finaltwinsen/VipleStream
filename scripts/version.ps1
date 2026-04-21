# ---------------------------------------------------------
#  VipleStream version utility (PowerShell)
#  Pure Windows, no MSYS2 dependency
#
#  Usage:
#    version.ps1 get                 Read current version
#    version.ps1 bump                Bump patch + propagate
#    version.ps1 propagate           Propagate only (no bump)
#    version.ps1 bump -Part minor    Bump minor (reset patch)
#    version.ps1 bump -Part major    Bump major (reset all)
# ---------------------------------------------------------
param(
    [Parameter(Position = 0)]
    [ValidateSet('get', 'bump', 'propagate')]
    [string]$Action = 'get',

    [ValidateSet('patch', 'minor', 'major')]
    [string]$Part = 'patch'
)

$ErrorActionPreference = 'Stop'
$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$Root = Split-Path -Parent $ScriptDir
$VerFile = Join-Path $Root 'version.json'

function Read-Version {
    $json = Get-Content $VerFile -Raw | ConvertFrom-Json
    return @{
        Major = [int]$json.major
        Minor = [int]$json.minor
        Patch = [int]$json.patch
    }
}

function Write-Version($v) {
    $json = @"
{
  "major": $($v.Major),
  "minor": $($v.Minor),
  "patch": $($v.Patch)
}
"@
    [System.IO.File]::WriteAllText($VerFile, $json, (New-Object System.Text.UTF8Encoding $false))
}

function Bump-Version($v, [string]$part) {
    switch ($part) {
        'major' { $v.Major++; $v.Minor = 0; $v.Patch = 0 }
        'minor' { $v.Minor++; $v.Patch = 0 }
        'patch' { $v.Patch++ }
    }
    return $v
}

function Propagate-Version($ver) {
    $verStr   = "$($ver.Major).$($ver.Minor).$($ver.Patch)"
    # Android versionCode must be monotonically increasing across the Play
    # store lifetime. Encode the three SemVer components so minor bumps
    # always sort above any patch run of the prior minor.
    #   major*10000 + minor*1000 + patch
    # e.g. 1.2.12 -> 12012,  1.3.0 -> 13000,  2.0.0 -> 20000
    # Safe up to minor=9 / patch=999 which is well beyond what we ship.
    $verCode  = [int]$ver.Major * 10000 + [int]$ver.Minor * 1000 + [int]$ver.Patch
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false

    # 1) Sunshine CMakeLists.txt — project(VipleStream VERSION X.Y.Z ...)
    $cmake = Join-Path $Root 'Sunshine\CMakeLists.txt'
    if (Test-Path $cmake) {
        $content = Get-Content $cmake -Raw
        $content = $content -replace 'project\(VipleStream VERSION \d+\.\d+\.\d+', "project(VipleStream VERSION $verStr"
        [System.IO.File]::WriteAllText($cmake, $content, $utf8NoBom)
    }

    # 2) moonlight-qt app/version.txt — read by qmake (app.pro $$cat()).
    # Generated header version_string.h re-emits on next qmake run.
    $mlVer = Join-Path $Root 'moonlight-qt\app\version.txt'
    if (Test-Path $mlVer) {
        [System.IO.File]::WriteAllText($mlVer, $verStr, $utf8NoBom)
    }

    # 3) moonlight-android app/build.gradle — versionName + versionCode.
    # Previously inlined in build_android.cmd; centralised here so
    # any version propagation touches all three targets uniformly.
    $gradle = Join-Path $Root 'moonlight-android\app\build.gradle'
    if (Test-Path $gradle) {
        $content = Get-Content $gradle -Raw -Encoding UTF8
        $content = [regex]::Replace($content,
            'versionName\s+"[^"]+"',
            "versionName `"$verStr`"")
        $content = [regex]::Replace($content,
            'versionCode\s*=\s*\d+',
            "versionCode = $verCode")
        [System.IO.File]::WriteAllText($gradle, $content, $utf8NoBom)
    }

    return $verStr
}

function Format-Version($v) {
    return "$($v.Major).$($v.Minor).$($v.Patch)"
}

# -- Main --
$ver = Read-Version

switch ($Action) {
    'get' {
        Write-Output (Format-Version $ver)
    }
    'bump' {
        $oldStr = Format-Version $ver
        $ver = Bump-Version $ver $Part
        Write-Version $ver
        $newStr = Propagate-Version $ver
        # Use stderr so CMD's > redirect doesn't capture this line
        [Console]::Error.WriteLine("  $oldStr -> $newStr")
        Write-Output $newStr
    }
    'propagate' {
        $verStr = Propagate-Version $ver
        Write-Output $verStr
    }
}
