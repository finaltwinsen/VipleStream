# =============================================================================
#  VipleStream FRUC 30-second test harness
# =============================================================================
#  For each FRUC configuration (OFF / Quality / Balanced / Performance):
#    1. Write Moonlight registry settings (frameInterpolation, frucQuality)
#    2. Launch `Moonlight.exe stream <host> <app>` in a background window
#    3. Wait 8s for session to stabilize (decoder init, first real frames)
#    4. Run PresentMon for 30s targeting Moonlight.exe
#    5. Parse CSV -> per-config metrics
#    6. Stop Moonlight via `Moonlight.exe quit` (falls back to kill)
#
#  Output:  scripts/benchmark/results/<timestamp>/
#           |- presentmon_<config>.csv   (raw frame data)
#           |- summary.json              (metrics table)
#           |- report.md                 (human-readable)
#
#  Usage:
#    powershell -File test_fruc_30s.ps1 -Host <host-ip> -App Desktop
#    powershell -File test_fruc_30s.ps1 -Configs off,balanced
# =============================================================================

param(
    [string]$HostAddr = '<host-ip>',
    [string]$App      = 'Desktop',
    [int]   $Seconds  = 30,
    [int]   $WarmupSeconds = 8,
    [string[]]$Configs = @('off','quality','balanced','performance','nvof_quality'),
    [int]   $Fps = 60,          # server-side FPS; with FRUC client should show 2x
    [int]   $Width = 3840,
    [int]   $Height = 2160,
    [switch]$KeepWindowed,      # stream in windowed mode so console is visible
    [string]$OutDir = '',
    # VipleStream: dev-build Moonlight path. Default points at the
    # build output, not Program Files, because deploy_client_now.cmd
    # requires admin. Registry pair state is shared between installed
    # and dev-build so `Moonlight.exe stream <host> Desktop` works out
    # of the box as long as the user already paired with that host
    # using any Moonlight version.
    [string]$ExePath  = '',
    # VipleStream: target display to host the Moonlight stream window.
    # Moonlight has no --display CLI arg to pick a display, so when
    # benchmarking on a secondary display (e.g. a VDD with 120 Hz
    # refresh, while the physical primary is only 60 Hz), we need to
    # move the window ourselves after the launcher spawns it. Accepts
    # either a 0-based display index or a substring of the device
    # name (e.g. 'DISPLAY3'). Empty = leave window on whichever
    # display Moonlight picked (usually primary).
    [string]$TargetDisplay = ''
)

# Don't let Qt/SDL stderr warnings from Moonlight terminate the script.
# We still want terminating errors from our own code, so use Continue globally
# and explicit -ErrorAction where we care.
$ErrorActionPreference = 'Continue'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$PresentMon = Join-Path $ProjectRoot 'scripts\PresentMon-2.4.1-x64.exe'
if ($ExePath) {
    $MoonlightExe = $ExePath
} else {
    # Prefer the fresh dev build; fall back to the installed copy.
    $devBuild = Join-Path $ProjectRoot 'temp\moonlight\Moonlight.exe'
    if (Test-Path $devBuild) {
        $MoonlightExe = $devBuild
    } else {
        $MoonlightExe = 'C:\Program Files\Moonlight Game Streaming\Moonlight.exe'
    }
}

if (-not (Test-Path $PresentMon)) { throw "PresentMon not found: $PresentMon" }
if (-not (Test-Path $MoonlightExe)) { throw "Moonlight not found: $MoonlightExe" }
Write-Host "Using Moonlight at: $MoonlightExe" -ForegroundColor Cyan

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $OutDir = Join-Path $ProjectRoot "temp\fruc_test_$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

# VipleStream: v1.2.44 rebrand changed QCoreApplication::setApplicationName
# from "Moonlight" to "VipleStream", so QSettings now live under
#   HKCU\Software\Moonlight Game Streaming Project\VipleStream
# instead of the upstream ...\Moonlight path. This harness used to
# write to the upstream path, which looked like "working" because
# Set-ItemProperty silently created the phantom key, but Moonlight
# read from the real VipleStream key and the FRUC config never
# actually changed between runs — every baseline config ended up
# streaming with whatever the user last set in the Settings UI.
# Fix: target the VipleStream subkey. If a future major version
# changes applicationName again, update here too.
$RegPath = 'HKCU:\Software\Moonlight Game Streaming Project\VipleStream'

# Save original settings so we can restore
$origInterp   = (Get-ItemProperty $RegPath -Name frameInterpolation -ErrorAction SilentlyContinue).frameInterpolation
$origBackend  = (Get-ItemProperty $RegPath -Name frucBackend        -ErrorAction SilentlyContinue).frucBackend
$origQuality  = (Get-ItemProperty $RegPath -Name frucQuality        -ErrorAction SilentlyContinue).frucQuality
$origVideoCfg = (Get-ItemProperty $RegPath -Name videocfg           -ErrorAction SilentlyContinue).videocfg
Write-Host "Original: frameInterpolation=$origInterp frucBackend=$origBackend frucQuality=$origQuality videocfg=$origVideoCfg"
Write-Host ""

# Map config name -> runtime settings.
#   interp / backend / quality   -> registry keys (frameInterpolation /
#                                   frucBackend / frucQuality)
#   videocodec                   -> --video-codec CLI arg ('auto' |
#                                   'H.264' | 'HEVC' | 'AV1'). Passed
#                                   per launch, doesn't touch registry.
#   VideoCodecConfig enum: 0=AUTO, 1=FORCE_H264, 2=FORCE_HEVC,
#                          3=FORCE_HEVC_HDR_DEPRECATED, 4=FORCE_AV1
# AV1 configs require host + client GPU to support AV1 HW decode:
#   - Ampere+ NVDEC (A1000 / RTX 30 series or newer) -> yes
#   - Older NVDEC (pre-Ampere) -> no, stream will fall back to HEVC
$presetMap = @{
    'off'           = @{ interp = 'false'; backend = 0; quality = 1; videocodec = 'auto' }  # FRUC disabled, codec auto (usually HEVC)
    'quality'       = @{ interp = 'true';  backend = 0; quality = 0; videocodec = 'auto' }  # FQ_QUALITY + HEVC
    'balanced'      = @{ interp = 'true';  backend = 0; quality = 1; videocodec = 'auto' }  # FQ_BALANCED + HEVC
    'performance'   = @{ interp = 'true';  backend = 0; quality = 2; videocodec = 'auto' }  # FQ_PERFORMANCE + HEVC
    'nvof_quality'  = @{ interp = 'true';  backend = 1; quality = 0; videocodec = 'auto' }  # NVIDIA OF + HEVC
    'av1_off'       = @{ interp = 'false'; backend = 0; quality = 1; videocodec = 'AV1' }   # Hardware AV1 decode, FRUC off
    'av1_balanced'  = @{ interp = 'true';  backend = 0; quality = 1; videocodec = 'AV1' }   # AV1 + Generic Balanced FRUC
    'av1_nvof'      = @{ interp = 'true';  backend = 1; quality = 0; videocodec = 'AV1' }   # AV1 + NVIDIA OF FRUC
}

function Stop-Moonlight {
    # Force-kill; graceful quit path currently races the stream launch and
    # Moonlight's own stderr spam trips ErrorActionPreference.
    Get-Process -Name 'Moonlight' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
}

# Resolve $TargetDisplay (numeric index or substring of device name)
# to a Screen object, or $null if no match / parameter empty.
function Resolve-TargetScreen {
    param([string]$spec)
    if (-not $spec) { return $null }
    Add-Type -AssemblyName System.Windows.Forms
    $screens = [System.Windows.Forms.Screen]::AllScreens
    # Numeric index
    $idx = 0
    if ([int]::TryParse($spec, [ref]$idx)) {
        if ($idx -ge 0 -and $idx -lt $screens.Count) { return $screens[$idx] }
        Write-Warning "TargetDisplay index $idx out of range (0..$($screens.Count-1)); ignoring"
        return $null
    }
    # Substring match against DeviceName
    foreach ($s in $screens) {
        if ($s.DeviceName -like "*$spec*") { return $s }
    }
    Write-Warning "TargetDisplay '$spec' didn't match any screen; ignoring. Screens: $(($screens | ForEach-Object { $_.DeviceName }) -join ',')"
    return $null
}

# Move a window (identified by its HWND) onto the given Screen's bounds.
# Reposition without resizing so Moonlight's backbuffer geometry stays
# intact. PowerShell 7 runs on .NET (Core), where System.Drawing.Rectangle
# lives in a separate assembly that isn't loaded by default — we use a
# plain Win32 RECT struct to stay self-contained.
function Move-WindowToScreen {
    param([IntPtr]$hwnd, $screen)
    if ($hwnd -eq [IntPtr]::Zero -or $null -eq $screen) { return }
    if (-not ('Win32.Mover' -as [type])) {
        Add-Type -Namespace Win32 -Name Mover -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetWindowPos(System.IntPtr hWnd, System.IntPtr hWndInsertAfter,
                                       int X, int Y, int cx, int cy, uint uFlags);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr hWnd, out RECT r);
'@
    }
    $rect = New-Object 'Win32.Mover+RECT'
    [Win32.Mover]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
    $w = $rect.Right  - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -le 0 -or $h -le 0) { $w = 1280; $h = 720 }
    $x = $screen.Bounds.X + [int](($screen.Bounds.Width  - $w) / 2)
    $y = $screen.Bounds.Y + [int](($screen.Bounds.Height - $h) / 2)
    # SWP_NOSIZE=0x0001  SWP_NOZORDER=0x0004 -> combined=0x0005 (keep size + z-order)
    [Win32.Mover]::SetWindowPos($hwnd, [IntPtr]::Zero, $x, $y, 0, 0, 0x0005) | Out-Null
    Write-Host "  Moved window HWND=$hwnd to screen $($screen.DeviceName) at ($x,$y), size ${w}x${h}" -ForegroundColor DarkGray
}

# PresentMon needs ETW access. Either admin, OR membership in
# "Performance Log Users" local group (one-time setup via
# scripts/benchmark/grant_presentmon_access.cmd).
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin  = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$hasPerfGroup = $false
# SID S-1-5-32-559 = BUILTIN\Performance Log Users. SID check avoids locale
# issues and bypasses the MSYS whoami.exe shim when run under a bash parent.
try {
    $targetSid = [Security.Principal.SecurityIdentifier]'S-1-5-32-559'
    foreach ($g in $identity.Groups) {
        if ($g -eq $targetSid) { $hasPerfGroup = $true; break }
    }
} catch { }

if (-not $isAdmin -and -not $hasPerfGroup) {
    Write-Error @"
PresentMon needs ETW access. You are neither an administrator nor a member
of the "Performance Log Users" group in this session.

One-time fix (no per-run UAC after this):
  1. Run  scripts\benchmark\grant_presentmon_access.cmd  (requires UAC once)
  2. Log off Windows and log back in
  3. Re-run this script - no UAC prompt needed

Or launch this script from an elevated PowerShell to run once with UAC.
"@
    exit 1
}
if ($isAdmin) {
    Write-Host "Running with administrator token." -ForegroundColor DarkGray
} else {
    Write-Host "Running as 'Performance Log Users' member (no UAC needed)." -ForegroundColor DarkGray
}

function Parse-PresentMonCsv {
    param(
        [string]$Csv,
        # VipleStream: filter to a single target PID when PresentMon
        # was run unfiltered (the workaround for --process_name being
        # admin-gated). Application column reads as '<unknown>' for
        # processes PresentMon couldn't query, but ProcessID remains
        # correct so PID is the reliable filter key. Pass 0 to disable.
        [int]$FilterPid = 0
    )

    if (-not (Test-Path $Csv)) { return $null }

    $rows = Import-Csv $Csv
    if ($rows.Count -eq 0) { return $null }

    if ($FilterPid -gt 0) {
        $totalRows = $rows.Count
        # Snapshot unique PIDs BEFORE filtering (diagnostic for the
        # no-match case). PowerShell parses `"..." + "..."` inside a
        # Write-Warning arg list as separate positional args, so we
        # must compose the diagnostic string first into a single var.
        $uniqPidList = ($rows | Select-Object -ExpandProperty ProcessID -Unique) -join ','
        $rows = @($rows | Where-Object {
            try { [int]$_.ProcessID -eq $FilterPid } catch { $false }
        })
        if ($rows.Count -eq 0) {
            $msg = "No rows matched PID=$FilterPid in $Csv (total rows: $totalRows). " +
                   "Either Moonlight never presented while PresentMon was recording, " +
                   "or the PID changed. CSV has unique PIDs: $uniqPidList"
            Write-Warning $msg
            return $null
        }
        Write-Host "  Filtered by PID=$FilterPid -> $($rows.Count) / $totalRows rows"
    }

    # Identify frame time column - varies between PresentMon versions
    $cols = $rows[0].PSObject.Properties.Name
    $msBetweenCol = $cols | Where-Object { $_ -in @('MsBetweenPresents','FrameTime','msBetweenPresents') } | Select-Object -First 1
    $displayedCol = $cols | Where-Object { $_ -in @('MsBetweenDisplayChange','DisplayedTime','MsUntilDisplayed') } | Select-Object -First 1
    $dropCol      = $cols | Where-Object { $_ -in @('Dropped','AllowsTearing','FrameType') } | Select-Object -First 1
    $gpuBusyCol   = $cols | Where-Object { $_ -in @('MsGPUActive','GPUBusy','msGPUActive') } | Select-Object -First 1
    $displayLatCol= $cols | Where-Object { $_ -in @('DisplayLatency','MsInPresentAPI') } | Select-Object -First 1
    $animErrCol   = $cols | Where-Object { $_ -in @('AnimationError','MsAnimationError') } | Select-Object -First 1
    $cpuBusyCol   = $cols | Where-Object { $_ -in @('CPUBusy','MsInPresentAPI') } | Select-Object -First 1
    $cpuWaitCol   = $cols | Where-Object { $_ -in @('CPUWait') } | Select-Object -First 1

    # Pull numeric series
    function Get-Series($col) {
        if (-not $col) { return @() }
        $vals = @()
        foreach ($r in $rows) {
            $v = $r.$col
            if ($null -ne $v -and $v -ne '' -and $v -ne 'NA') {
                try { $vals += [double]$v } catch { }
            }
        }
        return $vals
    }

    $frameTimes  = Get-Series $msBetweenCol
    $displayed   = Get-Series $displayedCol
    $gpuBusy     = Get-Series $gpuBusyCol
    $displayLat  = Get-Series $displayLatCol
    $animErr     = Get-Series $animErrCol
    $cpuBusy     = Get-Series $cpuBusyCol
    $cpuWait     = Get-Series $cpuWaitCol

    # Sort for percentiles
    function Pct($sorted, $p) {
        if ($sorted.Count -eq 0) { return 0.0 }
        $idx = [int][Math]::Ceiling(($p / 100.0) * $sorted.Count) - 1
        if ($idx -lt 0) { $idx = 0 }
        if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
        return [double]$sorted[$idx]
    }
    function Stats($name, $vals) {
        if ($vals.Count -eq 0) { return $null }
        $sorted = $vals | Sort-Object
        $mean = ($vals | Measure-Object -Average).Average
        $var = 0.0
        foreach ($v in $vals) { $var += [Math]::Pow($v - $mean, 2) }
        if ($vals.Count -gt 1) { $var /= ($vals.Count - 1) }
        [PSCustomObject]@{
            name  = $name
            count = $vals.Count
            mean  = [Math]::Round($mean, 3)
            std   = [Math]::Round([Math]::Sqrt($var), 3)
            min   = [Math]::Round([double]$sorted[0], 3)
            p50   = [Math]::Round((Pct $sorted 50), 3)
            p95   = [Math]::Round((Pct $sorted 95), 3)
            p99   = [Math]::Round((Pct $sorted 99), 3)
            p999  = [Math]::Round((Pct $sorted 99.9), 3)
            max   = [Math]::Round([double]$sorted[-1], 3)
        }
    }

    $ftStats   = Stats 'frame_time_ms'   $frameTimes
    $dispStats = Stats 'displayed_ms'    $displayed
    $gpuStats  = Stats 'gpu_busy_ms'     $gpuBusy
    $latStats  = Stats 'display_lat_ms'  $displayLat
    $animStats = Stats 'anim_error_ms'   $animErr
    $cpuStats  = Stats 'cpu_busy_ms'     $cpuBusy
    $cpuWaitStats = Stats 'cpu_wait_ms'  $cpuWait

    # Spike counting: how many frames exceed various thresholds above the mean.
    # p95 outliers are driven by a small fraction of frames; knowing the count
    # helps distinguish "occasional hiccup" from "systemic pacing problem".
    $spikes = $null
    if ($ftStats) {
        $thr1_5x = $ftStats.mean * 1.5
        $thr2x   = $ftStats.mean * 2.0
        $thr3x   = $ftStats.mean * 3.0
        $spikes = [PSCustomObject]@{
            over_1_5x = ($frameTimes | Where-Object { $_ -gt $thr1_5x }).Count
            over_2x   = ($frameTimes | Where-Object { $_ -gt $thr2x   }).Count
            over_3x   = ($frameTimes | Where-Object { $_ -gt $thr3x   }).Count
            total     = $frameTimes.Count
        }
    }

    # Effective FPS from mean frame time
    $effFps = 0.0
    if ($ftStats -and $ftStats.mean -gt 0) {
        $effFps = [Math]::Round(1000.0 / $ftStats.mean, 2)
    }

    # Dropped frames - logic depends on column present
    $dropped = 0
    if ($dropCol -eq 'Dropped') {
        $dropped = ($rows | Where-Object { $_.Dropped -eq '1' -or $_.Dropped -eq 1 }).Count
    } elseif ($dropCol -eq 'FrameType') {
        $dropped = ($rows | Where-Object { $_.FrameType -and $_.FrameType -ne 'Application' -and $_.FrameType -ne '' }).Count
    }

    [PSCustomObject]@{
        rows        = $rows.Count
        fps         = $effFps
        dropped     = $dropped
        dropped_pct = if ($rows.Count -gt 0) { [Math]::Round(100.0 * $dropped / $rows.Count, 2) } else { 0 }
        frame_time  = $ftStats
        displayed   = $dispStats
        gpu_busy    = $gpuStats
        display_lat = $latStats
        anim_error  = $animStats
        cpu_busy    = $cpuStats
        cpu_wait    = $cpuWaitStats
        spikes      = $spikes
        columns_used = [PSCustomObject]@{
            frame_time  = $msBetweenCol
            displayed   = $displayedCol
            dropped     = $dropCol
            gpu_busy    = $gpuBusyCol
            display_lat = $displayLatCol
            anim_error  = $animErrCol
            cpu_busy    = $cpuBusyCol
            cpu_wait    = $cpuWaitCol
        }
    }
}

# VipleStream: parse [VIPLE-PRESENT-Stats] lines from a Moonlight log
# into a summary structure. Each 5-second flush produces one "real"
# bucket and one "interp" bucket (possibly with n=0). We aggregate
# across buckets frame-count-weighted for rate/time metrics, and take
# the worst p95/p99/p99.9 across buckets (pessimistic — catches
# stall episodes that a simple mean would smooth out).
#
# This is the measurement path we use when the stream runs on a display
# whose Present events PresentMon can't observe (Indirect Display
# Drivers e.g. VDD don't emit the DXGI ETW flip events PresentMon
# subscribes to; the in-process QPC timing recordPresent() writes to
# the Moonlight log is immune to that whole tooling category.)
function Parse-MoonlightPresentLog {
    param([string]$logPath)
    if (-not (Test-Path $logPath)) { return $null }
    $content = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
    if (-not $content) { return $null }
    # Two line formats:
    #   non-empty: '[VIPLE-PRESENT-Stats] real n=300 fps=60.00 ft_mean=16.667ms p50=... p95=... p99=... p99.9=... call_mean=... p95=...'
    #   empty:     '[VIPLE-PRESENT-Stats] real n=0'
    $re = [regex]'\[VIPLE-PRESENT-Stats\] (real|interp) n=(\d+)(?: fps=([\d.]+) ft_mean=([\d.]+)ms p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) p99\.9=([\d.]+) call_mean=([\d.]+)ms p95=([\d.]+))?'
    $real   = @()
    $interp = @()
    foreach ($m in $re.Matches($content)) {
        $bucket = [PSCustomObject]@{
            label    = $m.Groups[1].Value
            n        = [int]$m.Groups[2].Value
            fps      = if ($m.Groups[3].Success) { [double]$m.Groups[3].Value } else { 0.0 }
            ft_mean  = if ($m.Groups[4].Success) { [double]$m.Groups[4].Value } else { 0.0 }
            p50      = if ($m.Groups[5].Success) { [double]$m.Groups[5].Value } else { 0.0 }
            p95      = if ($m.Groups[6].Success) { [double]$m.Groups[6].Value } else { 0.0 }
            p99      = if ($m.Groups[7].Success) { [double]$m.Groups[7].Value } else { 0.0 }
            p999     = if ($m.Groups[8].Success) { [double]$m.Groups[8].Value } else { 0.0 }
            call_mean= if ($m.Groups[9].Success) { [double]$m.Groups[9].Value } else { 0.0 }
            call_p95 = if ($m.Groups[10].Success){ [double]$m.Groups[10].Value } else { 0.0 }
        }
        if ($bucket.label -eq 'real') { $real += $bucket } else { $interp += $bucket }
    }
    function AggregateBuckets($buckets) {
        $nonEmpty = @($buckets | Where-Object { $_.n -gt 0 })
        if ($nonEmpty.Count -eq 0) { return $null }
        $totalN = 0; $wFps = 0.0; $wFt = 0.0
        foreach ($b in $nonEmpty) {
            $totalN += $b.n; $wFps += $b.fps * $b.n; $wFt += $b.ft_mean * $b.n
        }
        return [PSCustomObject]@{
            buckets      = $nonEmpty.Count
            total_frames = $totalN
            fps_avg      = [Math]::Round($wFps / $totalN, 2)
            ft_mean_avg  = [Math]::Round($wFt  / $totalN, 3)
            p95_worst    = [Math]::Round(($nonEmpty | Measure-Object p95  -Maximum).Maximum, 3)
            p99_worst    = [Math]::Round(($nonEmpty | Measure-Object p99  -Maximum).Maximum, 3)
            p999_worst   = [Math]::Round(($nonEmpty | Measure-Object p999 -Maximum).Maximum, 3)
        }
    }
    [PSCustomObject]@{
        real         = AggregateBuckets $real
        interp       = AggregateBuckets $interp
        real_buckets = $real
        interp_buckets = $interp
    }
}

# -----------------------------------------------------------------------------
$results = [ordered]@{}

try {
    foreach ($cfg in $Configs) {
        if (-not $presetMap.ContainsKey($cfg)) {
            Write-Warning "Unknown preset '$cfg', skipping"
            continue
        }
        $p = $presetMap[$cfg]
        # Older preset definitions may not carry a videocodec field;
        # default to 'auto' so legacy configs behave identically.
        $videocodec = if ($p.ContainsKey('videocodec')) { $p.videocodec } else { 'auto' }
        Write-Host "=============================================="
        Write-Host " FRUC config: $cfg" -ForegroundColor Cyan
        Write-Host "  interp=$($p.interp) backend=$($p.backend) quality=$($p.quality) codec=$videocodec"
        Write-Host "=============================================="

        # Apply registry. videocfg is persisted (registry key 'videocfg'
        # under the VipleStream subkey) so we also write it here — the
        # --video-codec CLI arg below takes precedence for the session
        # that launches, but leaving the registry value consistent
        # prevents a stale value from leaking into other code paths
        # (e.g. if commandlineparser resets bitrate using the stored
        # codec for default lookup).
        Set-ItemProperty $RegPath -Name frameInterpolation -Value $p.interp
        Set-ItemProperty $RegPath -Name frucBackend        -Value $p.backend -Type DWord
        Set-ItemProperty $RegPath -Name frucQuality        -Value $p.quality -Type DWord
        # Map codec name -> VideoCodecConfig enum for registry value
        $codecEnum = switch ($videocodec) {
            'auto'  { 0 }
            'H.264' { 1 }
            'HEVC'  { 2 }
            'AV1'   { 4 }
            default { 0 }
        }
        Set-ItemProperty $RegPath -Name videocfg -Value $codecEnum -Type DWord

        Stop-Moonlight
        # Snapshot the time we start this config, to later pick the
        # Moonlight log file produced during this config's run.
        $configStartTime = Get-Date

        # Launch streaming. We captured the Process object (-PassThru)
        # originally to pass a PID to PresentMon, but that turned out
        # to not be enough because without admin PresentMon can't
        # resolve the token for a cross-integrity-level process.
        # Instead we run PresentMon unfiltered and post-process the
        # CSV by PID (see Parse-PresentMonCsv -FilterPid).
        #
        # Do NOT start Moonlight with -WindowStyle Minimized. Empirical
        # observation: when the launcher process is Minimized, the
        # stream child window inherits a hidden/no-focus state and
        # the D3D11VA renderer's completeInitialization never runs —
        # the decoder queue fills up with unconsumed packets and the
        # stream loops forever on `Video decode unit queue overflow`
        # / `Waiting for IDR frame`. Letting the window appear
        # normally (in the windowed mode set by --display-mode) is
        # the only way the renderer initializes. Side effect: the
        # user sees the stream window pop up during the benchmark —
        # that's fine for a test harness; just don't give the
        # Moonlight window focus while typing elsewhere.
        $mlArgs = @('stream', $HostAddr, $App, '--fps', $Fps, '--resolution', "$($Width)x$($Height)")
        if ($KeepWindowed) { $mlArgs += '--display-mode','windowed' }
        # --video-codec expects one of: auto / H.264 / HEVC / AV1.
        # Pass it for every config so "auto" is explicit (no reliance
        # on the registry value the previous iteration left behind).
        $mlArgs += '--video-codec', $videocodec
        Write-Host "Launching: Moonlight.exe $($mlArgs -join ' ')"
        $mlProc = Start-Process -FilePath $MoonlightExe -ArgumentList $mlArgs -PassThru

        # Early warmup: give Moonlight ~3s to create the stream window
        # before we try to move it. If we wait for the full WarmupSeconds
        # the renderer has already initialized on the original display
        # and moving it cross-display may trigger a SwapChain resize
        # stall inside its hot path.
        $earlyWarmup = [Math]::Min(3, $WarmupSeconds)
        Start-Sleep -Seconds $earlyWarmup

        if ($TargetDisplay) {
            $targetScreen = Resolve-TargetScreen $TargetDisplay
            if ($targetScreen) {
                $mlEarly = Get-Process -Name 'Moonlight' -ErrorAction SilentlyContinue |
                           Where-Object { $_.MainWindowHandle -ne 0 } |
                           Sort-Object StartTime -Descending | Select-Object -First 1
                if ($mlEarly) {
                    Move-WindowToScreen $mlEarly.MainWindowHandle $targetScreen
                } else {
                    Write-Warning "No Moonlight window handle available yet to move to $TargetDisplay"
                }
            }
        }

        Write-Host "Warming up $($WarmupSeconds - $earlyWarmup)s more..."
        Start-Sleep -Seconds ($WarmupSeconds - $earlyWarmup)

        # Verify Moonlight is running — re-resolve by name because the
        # stream child process may have a different PID than the
        # initial launcher if Moonlight respawns itself for the stream.
        $ml = Get-Process -Name 'Moonlight' -ErrorAction SilentlyContinue
        if (-not $ml) {
            Write-Warning "Moonlight not running for cfg=$cfg"
            $results[$cfg] = @{ error = 'moonlight_exited_early' }
            continue
        }
        # Prefer the process that actually owns the stream window.
        # Moonlight doesn't fork for streaming, so typically there's
        # only one — take the most recently started one to be safe.
        $mlMain = $ml | Sort-Object StartTime -Descending | Select-Object -First 1
        $mlPid = $mlMain.Id
        Write-Host "Moonlight PID: $mlPid"

        $csvPath = Join-Path $OutDir "presentmon_$cfg.csv"
        Write-Host "Recording $Seconds s -> $csvPath"

        # Kill any orphaned ETW session left over from a previous force-kill.
        # Without this, the kernel-side session from a prior PresentMon still
        # exists and blocks capture even though the PresentMon process is gone.
        $sessionName = "viple_fruc_$cfg"
        & logman stop $sessionName -ets 2>$null | Out-Null

        # PresentMon unfiltered-capture workaround: when running without
        # admin, neither --process_name nor --process_id can target a
        # process whose token is outside the current session's query
        # scope (Claude-spawned Moonlight hits this on this host).
        # Capture EVERY process's Present events; we post-process the
        # CSV and filter on the ProcessID column ourselves. The CSV
        # gets larger but that's fine at 30s windows.
        $pmArgs = @(
            '--output_file', $csvPath,
            '--timed', $Seconds,
            '--terminate_after_timed',
            '--v2_metrics',
            '--no_console_stats',
            '--stop_existing_session',
            '--session_name', $sessionName
        )
        # Redirect PresentMon stdout/stderr to a separate log — its UTF-16 output
        # confuses PowerShell's pipeline and hangs Start-Process -Wait.
        $pmLog = Join-Path $OutDir "presentmon_$cfg.log"
        $pmProc = Start-Process -FilePath $PresentMon -ArgumentList $pmArgs `
                     -NoNewWindow -PassThru `
                     -RedirectStandardOutput $pmLog `
                     -RedirectStandardError  "$pmLog.err"
        # Bounded wait: timed+10s for flush, else force-kill
        $deadline = (Get-Date).AddSeconds($Seconds + 15)
        while (-not $pmProc.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
        }
        if (-not $pmProc.HasExited) {
            Write-Warning "PresentMon did not exit within ${Seconds}+15s; killing"
            try { $pmProc.Kill() } catch { }
            $pmProc.WaitForExit(3000) | Out-Null
        }
        if ($pmProc.ExitCode -ne 0) {
            Write-Warning "PresentMon exited $($pmProc.ExitCode)"
        }

        Stop-Moonlight

        # Parse
        $m = Parse-PresentMonCsv -Csv $csvPath -FilterPid $mlPid

        # VipleStream: also parse the Moonlight log's in-process
        # [VIPLE-PRESENT-Stats] for this config. Picks the newest
        # VipleStream-*.log modified since $configStartTime so we
        # don't pick up stats from previous runs. Complements the
        # PresentMon CSV (and replaces it when the stream runs on
        # an Indirect Display like a VDD, where ETW flip events
        # aren't emitted).
        $presentStats = $null
        $mlLogDir = $env:TEMP
        $mlLog = Get-ChildItem -Path $mlLogDir -Filter 'VipleStream-*.log' -ErrorAction SilentlyContinue |
                 Where-Object { $_.LastWriteTime -gt $configStartTime } |
                 Sort-Object LastWriteTime -Descending |
                 Select-Object -First 1
        if ($mlLog) {
            $presentStats = Parse-MoonlightPresentLog $mlLog.FullName
            if ($presentStats) {
                Write-Host "  Moonlight log : $($mlLog.Name)"
            }
        }

        if ($null -eq $m) {
            # PresentMon failed but Moonlight log may still have data
            # (this is the VDD / IDD case). Keep both paths available.
            if ($presentStats) {
                $results[$cfg] = [PSCustomObject]@{ present_stats = $presentStats }
                Write-Host "  (No PresentMon data — using Moonlight log)"
                if ($presentStats.real) {
                    Write-Host "  Real Present  : fps=$($presentStats.real.fps_avg) ft=$($presentStats.real.ft_mean_avg)ms p95=$($presentStats.real.p95_worst) p99=$($presentStats.real.p99_worst) (n=$($presentStats.real.total_frames))"
                }
                if ($presentStats.interp) {
                    Write-Host "  Interp Present: fps=$($presentStats.interp.fps_avg) ft=$($presentStats.interp.ft_mean_avg)ms p95=$($presentStats.interp.p95_worst) p99=$($presentStats.interp.p99_worst) (n=$($presentStats.interp.total_frames))"
                }
                Write-Host ""
                continue
            }
            $results[$cfg] = @{ error = 'no_data' }
            continue
        }
        # Attach present_stats to the PresentMon-derived result
        if ($presentStats) {
            $m | Add-Member -NotePropertyName 'present_stats' -NotePropertyValue $presentStats -Force
        }
        $results[$cfg] = $m
        Write-Host ""
        Write-Host "  Effective FPS : $($m.fps)"
        Write-Host "  Frame time    : mean=$($m.frame_time.mean)ms  p95=$($m.frame_time.p95)  p99=$($m.frame_time.p99)  p99.9=$($m.frame_time.p999)  std=$($m.frame_time.std)"
        if ($m.gpu_busy)    { Write-Host "  GPU busy      : mean=$($m.gpu_busy.mean)ms  p95=$($m.gpu_busy.p95)  p99=$($m.gpu_busy.p99)" }
        if ($m.display_lat) { Write-Host "  DisplayLat    : mean=$($m.display_lat.mean)ms  p95=$($m.display_lat.p95)  p99=$($m.display_lat.p99)" }
        if ($m.cpu_busy)    { Write-Host "  CPU busy      : mean=$($m.cpu_busy.mean)ms  p95=$($m.cpu_busy.p95)" }
        if ($m.cpu_wait)    { Write-Host "  CPU wait      : mean=$($m.cpu_wait.mean)ms  p95=$($m.cpu_wait.p95)" }
        if ($m.spikes) {
            $pct15 = [Math]::Round(100.0 * $m.spikes.over_1_5x / $m.spikes.total, 2)
            $pct2  = [Math]::Round(100.0 * $m.spikes.over_2x   / $m.spikes.total, 2)
            $pct3  = [Math]::Round(100.0 * $m.spikes.over_3x   / $m.spikes.total, 2)
            Write-Host "  Spikes        : >1.5x=$($m.spikes.over_1_5x) ($pct15 %)  >2x=$($m.spikes.over_2x) ($pct2 %)  >3x=$($m.spikes.over_3x) ($pct3 %)"
        }
        Write-Host "  Dropped       : $($m.dropped) ($($m.dropped_pct) %)"
        # VipleStream: also print in-process Present stats from Moonlight log.
        if ($presentStats) {
            if ($presentStats.real) {
                Write-Host "  [moonlight-log] real  : fps=$($presentStats.real.fps_avg) ft=$($presentStats.real.ft_mean_avg)ms p95=$($presentStats.real.p95_worst) p99=$($presentStats.real.p99_worst) (n=$($presentStats.real.total_frames))"
            }
            if ($presentStats.interp) {
                Write-Host "  [moonlight-log] interp: fps=$($presentStats.interp.fps_avg) ft=$($presentStats.interp.ft_mean_avg)ms p95=$($presentStats.interp.p95_worst) p99=$($presentStats.interp.p99_worst) (n=$($presentStats.interp.total_frames))"
            }
        }
        Write-Host ""
    }
}
finally {
    # Restore original settings
    if ($null -ne $origInterp)   { Set-ItemProperty $RegPath -Name frameInterpolation -Value $origInterp }
    if ($null -ne $origBackend)  { Set-ItemProperty $RegPath -Name frucBackend -Value $origBackend -Type DWord }
    if ($null -ne $origQuality)  { Set-ItemProperty $RegPath -Name frucQuality -Value $origQuality -Type DWord }
    if ($null -ne $origVideoCfg) { Set-ItemProperty $RegPath -Name videocfg    -Value $origVideoCfg -Type DWord }
    Stop-Moonlight
    # Sweep any surviving ETW sessions we created
    foreach ($c in $presetMap.Keys) {
        & logman stop "viple_fruc_$c" -ets 2>$null | Out-Null
    }
}

# Emit summary.json
$summary = [PSCustomObject]@{
    timestamp = (Get-Date).ToString('o')
    host_addr = $HostAddr
    app       = $App
    seconds   = $Seconds
    warmup    = $WarmupSeconds
    fps       = $Fps
    resolution= "$($Width)x$($Height)"
    results   = $results
}
$summaryPath = Join-Path $OutDir 'summary.json'
$summary | ConvertTo-Json -Depth 10 | Set-Content -Path $summaryPath -Encoding UTF8

# Emit report.md
$report = @()
$report += "# VipleStream FRUC 30s test"
$report += ""
$report += "- Host: $HostAddr | App: $App | $Fps fps request | $($Width)x$($Height) | $Seconds s + $WarmupSeconds s warmup"
$report += "- Timestamp: $($summary.timestamp)"
$report += ""
$report += "## Frame-time distribution"
$report += ""
$report += "| Config | FPS | mean | p50 | p95 | p99 | p99.9 | max | std | spikes >2x | dropped% |"
$report += "|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($cfg in $Configs) {
    if (-not $results.Contains($cfg)) { continue }
    $m = $results[$cfg]
    if ($m.error) {
        $report += "| $cfg | - | - | - | - | - | - | - | - | - | _$($m.error)_ |"
    } else {
        $ft = $m.frame_time
        $spikePct = if ($m.spikes -and $m.spikes.total -gt 0) {
            [Math]::Round(100.0 * $m.spikes.over_2x / $m.spikes.total, 2).ToString() + '%'
        } else { '-' }
        $report += "| $cfg | $($m.fps) | $($ft.mean) | $($ft.p50) | $($ft.p95) | $($ft.p99) | $($ft.p999) | $($ft.max) | $($ft.std) | $spikePct | $($m.dropped_pct) |"
    }
}
$report += ""
$report += "## GPU / latency"
$report += ""
$report += "| Config | GPU mean | GPU p95 | GPU p99 | DispLat mean | DispLat p95 | DispLat p99 | CPU busy mean |"
$report += "|---|---|---|---|---|---|---|---|"
foreach ($cfg in $Configs) {
    if (-not $results.Contains($cfg)) { continue }
    $m = $results[$cfg]
    if ($m.error) {
        $report += "| $cfg | - | - | - | - | - | - | _$($m.error)_ |"
    } else {
        function fv($s, $f) { if ($s) { $s.$f } else { '-' } }
        $report += "| $cfg | $(fv $m.gpu_busy 'mean') | $(fv $m.gpu_busy 'p95') | $(fv $m.gpu_busy 'p99') | $(fv $m.display_lat 'mean') | $(fv $m.display_lat 'p95') | $(fv $m.display_lat 'p99') | $(fv $m.cpu_busy 'mean') |"
    }
}
$reportPath = Join-Path $OutDir 'report.md'
$report -join "`r`n" | Set-Content -Path $reportPath -Encoding UTF8

Write-Host ""
Write-Host "=============================================="
Write-Host " Results written to $OutDir"
Write-Host "  - summary.json"
Write-Host "  - report.md"
Write-Host "=============================================="
Get-Content $reportPath
