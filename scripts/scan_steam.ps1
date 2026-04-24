# =============================================================================
#  VipleStream — Steam library scanner (Phase 1 of Steam auto-import)
# =============================================================================
#  Read-only scan of a local Steam installation. Emits JSON to stdout with:
#    - Steam root path
#    - Currently-logged-in user (if any)
#    - All login users seen on this machine (+ RememberPassword flag = switchable)
#    - All installed apps + their per-user ownership ("which SteamID3 has this
#      app in their localconfig.vdf")
#
#  No write access. Safe to run as normal user (reads HKCU + user-readable files).
#
#  Usage:
#    powershell -NoProfile -File scripts\scan_steam.ps1 [-Pretty]
#
#  Output schema (JSON):
#  {
#    "steam_root": "C:\\Program Files (x86)\\Steam",
#    "steam_running": false,
#    "current_user": { "steam_id3": "105195245", "persona_name": "finaltwinsen" } | null,
#    "profiles": [
#      {
#        "steam_id3": "105195245",
#        "steam_id64": "76561198065460973",
#        "account_name": "finaltwinsen",
#        "persona_name": "finaltwinsen",
#        "avatar_path": "C:\\...\\avatarcache\\...jpg" | null,
#        "remember_password": true,
#        "switchable": true,
#        "most_recent": true,
#        "last_login": 1776996258
#      }, ...
#    ],
#    "apps": [
#      {
#        "app_id": "730",
#        "name": "Counter-Strike 2",
#        "install_dir": "E:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive",
#        "size_on_disk": 41234567890,
#        "image_header": "C:\\...\\librarycache\\730_header.jpg" | null,
#        "image_library": "C:\\...\\librarycache\\730_library_600x900.jpg" | null,
#        "owners": ["105195245", "...."],
#        "launch_url": "steam://rungameid/730"
#      }, ...
#    ],
#    "stats": {
#      "libraries": 1,
#      "installed_games": 74,
#      "known_profiles": 2
#    }
#  }
# =============================================================================

param(
    [switch]$Pretty
)

$ErrorActionPreference = 'Stop'

# ── Helpers ────────────────────────────────────────────────────────────────

# Minimal Valve KeyValues (VDF) parser. The format is:
#   "key"    "value"
#   "parent" { ... }
# Comments (// ...) exist but are rare in app VDFs. Escape sequences inside
# quoted values use backslash (\n, \t, \\). For our scanning needs — reading
# structure + a few string fields per node — this simple lexer is enough.
#
# Returns a nested hashtable. Leaf values are strings.
function Read-Vdf {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    $text = [IO.File]::ReadAllText($Path)
    $tokens = New-Object System.Collections.Generic.List[string]
    $i = 0; $len = $text.Length
    while ($i -lt $len) {
        $ch = $text[$i]
        if ($ch -eq '"') {
            $j = $i + 1
            $sb = New-Object System.Text.StringBuilder
            while ($j -lt $len -and $text[$j] -ne '"') {
                if ($text[$j] -eq '\' -and $j + 1 -lt $len) {
                    $next = $text[$j + 1]
                    switch ($next) {
                        'n' { [void]$sb.Append("`n") }
                        't' { [void]$sb.Append("`t") }
                        default { [void]$sb.Append($next) }
                    }
                    $j += 2
                } else {
                    [void]$sb.Append($text[$j]); $j++
                }
            }
            $tokens.Add($sb.ToString()); $i = $j + 1
        } elseif ($ch -eq '{' -or $ch -eq '}') {
            $tokens.Add([string]$ch); $i++
        } elseif ($ch -eq '/' -and $i + 1 -lt $len -and $text[$i + 1] -eq '/') {
            while ($i -lt $len -and $text[$i] -ne "`n") { $i++ }
        } else { $i++ }
    }
    # recursive parse
    $script:vdfPos = 0
    $script:vdfTokens = $tokens
    function _parse_vdf_node {
        $node = [ordered]@{}
        while ($script:vdfPos -lt $script:vdfTokens.Count) {
            $t = $script:vdfTokens[$script:vdfPos]
            if ($t -eq '}') { $script:vdfPos++; return $node }
            # expect key
            $key = $t; $script:vdfPos++
            if ($script:vdfPos -ge $script:vdfTokens.Count) { break }
            $next = $script:vdfTokens[$script:vdfPos]
            if ($next -eq '{') {
                $script:vdfPos++
                $node[$key] = _parse_vdf_node
            } else {
                $node[$key] = $next
                $script:vdfPos++
            }
        }
        return $node
    }
    return _parse_vdf_node
}

# Steam stores accounts with two IDs:
#   SteamID64 = 76561197960265728 + AccountID
#   SteamID3  = AccountID (what userdata/<folder>/ uses, also what the Steam URL `friends://<id>` uses)
function Convert-SteamId64-To-Id3 { param([string]$id64)
    try {
        $n = [uint64]$id64
        $base = [uint64]76561197960265728
        if ($n -gt $base) { return ($n - $base).ToString() }
    } catch {}
    return $id64
}

# ── Locate Steam root ──────────────────────────────────────────────────────

$steamRoot = $null
foreach ($path in @(
    'HKCU:\Software\Valve\Steam',
    'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
    'HKLM:\SOFTWARE\Valve\Steam'
)) {
    $v = (Get-ItemProperty -Path $path -Name 'SteamPath' -ErrorAction SilentlyContinue).SteamPath
    if ($v) { $steamRoot = $v; break }
    $v = (Get-ItemProperty -Path $path -Name 'InstallPath' -ErrorAction SilentlyContinue).InstallPath
    if ($v) { $steamRoot = $v; break }
}
if (-not $steamRoot) { throw 'Steam installation not found via registry' }
$steamRoot = ([IO.Path]::GetFullPath($steamRoot)).TrimEnd('\')

# ── Current session state ──────────────────────────────────────────────────

$activeUser = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam\ActiveProcess' -Name 'ActiveUser' -ErrorAction SilentlyContinue).ActiveUser
$autoLogin  = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name 'AutoLoginUser' -ErrorAction SilentlyContinue).AutoLoginUser
$steamRunning = @(Get-Process -Name 'steam' -ErrorAction SilentlyContinue).Count -gt 0
$currentUser = $null
if ($activeUser -and [int]$activeUser -ne 0) {
    $currentUser = @{ steam_id3 = [string]$activeUser; persona_name = $null }
}

# ── Profiles (loginusers.vdf + userdata/*/) ────────────────────────────────

$loginusers = Read-Vdf (Join-Path $steamRoot 'config\loginusers.vdf')
$profiles = New-Object System.Collections.Generic.List[object]
if ($loginusers -and $loginusers['users']) {
    foreach ($id64 in $loginusers['users'].Keys) {
        $u = $loginusers['users'][$id64]
        $id3 = Convert-SteamId64-To-Id3 $id64
        $userDataDir = Join-Path $steamRoot "userdata\$id3"
        $avatarPath = $null
        $avatarGlob = Join-Path $steamRoot "config\avatarcache\$id64.png"
        if (Test-Path $avatarGlob) { $avatarPath = $avatarGlob }
        $lastLogin = 0
        try { $lastLogin = [int64]$u['Timestamp'] } catch { $lastLogin = 0 }
        $udExists = (Test-Path $userDataDir)
        $profiles.Add([pscustomobject]@{
            steam_id3         = $id3
            steam_id64        = $id64
            account_name      = [string]$u['AccountName']
            persona_name      = [string]$u['PersonaName']
            avatar_path       = $avatarPath
            remember_password = ($u['RememberPassword'] -eq '1')
            switchable        = ($u['RememberPassword'] -eq '1')
            most_recent       = ($u['MostRecent'] -eq '1')
            last_login        = $lastLogin
            userdata_exists   = $udExists
        })
        # Resolve current_user persona_name if this is the active one
        if ($currentUser -and $currentUser['steam_id3'] -eq $id3) {
            $currentUser['persona_name'] = [string]$u['PersonaName']
        }
    }
    # Add userdata-only profiles (someone who logged in but whose loginusers entry
    # was later removed) as "non-switchable"
    $known = @{}; foreach ($p in $profiles) { $known[$p.steam_id3] = $true }
    if (Test-Path (Join-Path $steamRoot 'userdata')) {
        foreach ($d in Get-ChildItem (Join-Path $steamRoot 'userdata') -Directory -ErrorAction SilentlyContinue) {
            if (-not $known.ContainsKey($d.Name) -and $d.Name -ne '0' -and $d.Name -ne 'anonymous') {
                $profiles.Add([pscustomobject]@{
                    steam_id3         = $d.Name
                    steam_id64        = ([uint64]$d.Name + [uint64]76561197960265728).ToString()
                    account_name      = $null
                    persona_name      = "(unknown #$($d.Name))"
                    avatar_path       = $null
                    remember_password = $false
                    switchable        = $false
                    most_recent       = $false
                    last_login        = 0
                    userdata_exists   = $true
                })
            }
        }
    }
}

# ── Library roots (libraryfolders.vdf) ────────────────────────────────────

$libraryRoots = New-Object System.Collections.Generic.List[string]
$libFoldersVdf = Read-Vdf (Join-Path $steamRoot 'steamapps\libraryfolders.vdf')
if ($libFoldersVdf -and $libFoldersVdf['libraryfolders']) {
    foreach ($k in $libFoldersVdf['libraryfolders'].Keys) {
        $entry = $libFoldersVdf['libraryfolders'][$k]
        if ($entry -is [System.Collections.IDictionary] -and $entry['path']) {
            $libraryRoots.Add(([string]$entry['path']).TrimEnd('\'))
        }
    }
}
if ($libraryRoots.Count -eq 0) { $libraryRoots.Add($steamRoot) }

# ── Per-user entitlements (userdata/*/config/localconfig.vdf) ─────────────

$ownership = @{}  # app_id -> [list of steam_id3]
foreach ($p in $profiles) {
    if (-not $p.userdata_exists) { continue }
    $lc = Read-Vdf (Join-Path $steamRoot "userdata\$($p.steam_id3)\config\localconfig.vdf")
    if (-not $lc) { continue }
    # Path: UserLocalConfigStore > Software > Valve > Steam > apps > <AppID>
    $apps = $lc['UserLocalConfigStore']
    foreach ($seg in 'Software','Valve','Steam','apps') {
        if ($apps -is [System.Collections.IDictionary]) {
            # Case-insensitive key lookup
            $match = $null
            foreach ($k in $apps.Keys) { if ($k -ieq $seg) { $match = $k; break } }
            if ($match) { $apps = $apps[$match] } else { $apps = $null; break }
        } else { $apps = $null; break }
    }
    if ($apps -is [System.Collections.IDictionary]) {
        foreach ($aid in $apps.Keys) {
            if (-not $ownership.ContainsKey($aid)) {
                $ownership[$aid] = New-Object System.Collections.Generic.List[string]
            }
            $ownership[$aid].Add($p.steam_id3)
        }
    }
}

# ── Installed apps (appmanifest_*.acf per library) ────────────────────────

$apps = New-Object System.Collections.Generic.List[object]
foreach ($lib in $libraryRoots) {
    $sa = Join-Path $lib 'steamapps'
    if (-not (Test-Path $sa)) { continue }
    foreach ($f in Get-ChildItem $sa -Filter 'appmanifest_*.acf' -ErrorAction SilentlyContinue) {
        $m = Read-Vdf $f.FullName
        if (-not $m -or -not $m['AppState']) { continue }
        $st = $m['AppState']
        $appId = [string]$st['appid']
        if (-not $appId) { continue }
        # StateFlags: bit 2 (= 4) means "fully installed"
        $state = 0; try { $state = [int]$st['StateFlags'] } catch {}
        if (($state -band 4) -eq 0) { continue }  # skip in-progress / broken
        $installDir = Join-Path (Join-Path $lib 'steamapps\common') ([string]$st['installdir'])
        # Steam 2024+ moved librarycache to per-AppID subfolders.
        # Try new layout first, fall back to legacy flat filenames.
        $headerImg_new  = Join-Path $steamRoot "appcache\librarycache\$appId\header.jpg"
        $libraryImg_new = Join-Path $steamRoot "appcache\librarycache\$appId\library_600x900.jpg"
        $headerImg_old  = Join-Path $steamRoot "appcache\librarycache\${appId}_header.jpg"
        $libraryImg_old = Join-Path $steamRoot "appcache\librarycache\${appId}_library_600x900.jpg"
        $headerImg  = if (Test-Path $headerImg_new)  { $headerImg_new }  elseif (Test-Path $headerImg_old)  { $headerImg_old }  else { $null }
        $libraryImg = if (Test-Path $libraryImg_new) { $libraryImg_new } elseif (Test-Path $libraryImg_old) { $libraryImg_old } else { $null }
        $owners = @()
        if ($ownership.ContainsKey($appId)) { $owners = @($ownership[$appId]) }
        $szOnDisk = 0
        try { $szOnDisk = [int64]$st['SizeOnDisk'] } catch { $szOnDisk = 0 }
        $apps.Add([pscustomobject]@{
            app_id        = $appId
            name          = [string]$st['name']
            install_dir   = $installDir
            size_on_disk  = $szOnDisk
            image_header  = $headerImg
            image_library = $libraryImg
            owners        = $owners
            launch_url    = "steam://rungameid/$appId"
        })
    }
}

# ── Emit JSON ─────────────────────────────────────────────────────────────

$result = [ordered]@{
    steam_root    = $steamRoot
    steam_running = $steamRunning
    auto_login    = $autoLogin
    current_user  = $currentUser
    profiles      = @($profiles | Sort-Object -Property @{Expression='most_recent';Descending=$true}, @{Expression='last_login';Descending=$true})
    apps          = @($apps | Sort-Object -Property name)
    stats         = [ordered]@{
        libraries       = $libraryRoots.Count
        installed_games = $apps.Count
        known_profiles  = $profiles.Count
    }
}

$json = if ($Pretty) {
    $result | ConvertTo-Json -Depth 10
} else {
    $result | ConvertTo-Json -Depth 10 -Compress
}
[Console]::OutputEncoding = [Text.Encoding]::UTF8
Write-Output $json
