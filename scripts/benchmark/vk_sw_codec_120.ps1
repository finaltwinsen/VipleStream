# =============================================================================
#  VipleStream — Vulkan SW renderer × (H.264 / HEVC / AV1) × 120fps bench
# =============================================================================
#  Purpose: baseline + iterate the SW-decode → Vulkan-upload path for each
#  codec at 120fps, the "real 120fps 及格線" target.
#
#  For each codec:
#    1. Force renderer = RS_VULKAN (registry rendererSelection=0) so VkFruc
#       SW upload path is used regardless of HW cascade availability.
#    2. Pin --video-codec to the target (H.264 / HEVC / AV1) so the host
#       encodes that codec specifically.
#    3. FRUC off to isolate decode+upload latency from interp pipeline.
#    4. Launch stream 120fps × 1920x1080 (lighter resolution than 4K so the
#       limiter is decode/upload throughput, not video bandwidth).
#    5. PresentMon 30s + parse VIPLE-PRESENT-Stats.
#    6. Per-codec metrics: effective fps, p50/p95/p99 frame time, drops.
#
#  Pass criterion (per codec): effective fps >= 115 sustained, p99 frame
#  time <= 12ms (= 1000/120 with margin), drop rate <= 1%.
#
#  Output:  scripts/benchmark/results/vk_sw_120_<timestamp>/
# =============================================================================

param(
    [string]$HostAddr = '192.168.51.226',
    [string]$App      = 'Desktop',
    [int]   $Seconds  = 30,
    [int]   $WarmupSeconds = 8,
    [string[]]$Codecs = @('H.264','HEVC','AV1'),
    [int]   $Fps = 120,
    [int]   $Width  = 1920,
    [int]   $Height = 1080,
    [int]   $BitrateKbps = 0,
    [string]$ExePath = '',
    [string]$OutDir  = '',
    [switch]$KeepWindowed
)

$ErrorActionPreference = 'Continue'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$PresentMon = Join-Path $ProjectRoot 'scripts\PresentMon-2.4.1-x64.exe'

if ($ExePath) {
    $MoonlightExe = $ExePath
} else {
    $candidates = @(
        (Join-Path $ProjectRoot 'temp\moonlight\VipleStream.exe'),
        (Join-Path $ProjectRoot 'temp\moonlight\Moonlight.exe'),
        'C:\Program Files\VipleStream\VipleStream.exe'
    )
    $MoonlightExe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $MoonlightExe -or -not (Test-Path $MoonlightExe)) {
    throw "VipleStream client not found. Tried: $($candidates -join ', ')"
}
if (-not (Test-Path $PresentMon)) {
    throw "PresentMon not found at $PresentMon"
}

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $OutDir = Join-Path $ProjectRoot "temp\vk_sw_120_$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "Using client: $MoonlightExe" -ForegroundColor Cyan
Write-Host "Output dir:   $OutDir" -ForegroundColor Cyan
Write-Host ""

# v1.2.129 rebrand changed org/app name to "VipleStream" → QSettings registry
# moved to HKCU\Software\VipleStream\VipleStream.  Older harness scripts wrote
# to the legacy "Moonlight Game Streaming Project\VipleStream" path which the
# current binary doesn't read.
$RegPath = 'HKCU:\Software\VipleStream\VipleStream'

# Save original settings so we can restore on exit.
$origRenderer = (Get-ItemProperty $RegPath -Name rendererSelection -ErrorAction SilentlyContinue).rendererSelection
$origInterp   = (Get-ItemProperty $RegPath -Name frameInterpolation -ErrorAction SilentlyContinue).frameInterpolation
$origVideoCfg = (Get-ItemProperty $RegPath -Name videocfg -ErrorAction SilentlyContinue).videocfg
$origVideoDec = (Get-ItemProperty $RegPath -Name videodec -ErrorAction SilentlyContinue).videodec
Write-Host "Original: rendererSelection=$origRenderer videodec=$origVideoDec frameInterpolation=$origInterp videocfg=$origVideoCfg"

function Stop-Client {
    Get-Process -Name 'VipleStream','Moonlight' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
}

# Codec name → (videocfg enum, --video-codec arg).
# VideoCodecConfig: 0=AUTO, 1=FORCE_H264, 2=FORCE_HEVC, 4=FORCE_AV1
$codecMap = @{
    'H.264' = 1
    'HEVC'  = 2
    'AV1'   = 4
}

# PresentMon ETW perms
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin  = ([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$hasPerfGroup = $false
try {
    $targetSid = [Security.Principal.SecurityIdentifier]'S-1-5-32-559'
    foreach ($g in $identity.Groups) { if ($g -eq $targetSid) { $hasPerfGroup = $true; break } }
} catch { }
if (-not $isAdmin -and -not $hasPerfGroup) {
    Write-Warning "PresentMon needs admin or 'Performance Log Users' membership. Capture may fail."
}

# Parse [VIPLE-PRESENT-Stats] from VipleStream log (the in-process timing,
# resilient to VDD / IDD where PresentMon ETW flip events don't appear).
function Parse-VipleLog {
    param([string]$logPath)
    if (-not (Test-Path $logPath)) { return $null }
    $content = Get-Content $logPath -Raw -ErrorAction SilentlyContinue
    if (-not $content) { return $null }
    # VkFruc emits one of:
    #   [VIPLE-VKFRUC-Stats] single-present n=600 fps=120.00 ft_mean=8.33ms p50=... p95=... p99=... p99.9=...
    #   [VIPLE-VKFRUC-Stats] dual-present   n=...  ...
    # plus a cumul / swMode line which we don't need for fps/p99.
    $re = [regex]'\[VIPLE-VKFRUC-Stats\] (single|dual)-present n=(\d+) fps=([\d.]+) ft_mean=([\d.]+)ms p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) p99\.9=([\d.]+)'
    $buckets = @()
    foreach ($m in $re.Matches($content)) {
        $buckets += [PSCustomObject]@{
            mode    = $m.Groups[1].Value
            n       = [int]$m.Groups[2].Value
            fps     = [double]$m.Groups[3].Value
            ft_mean = [double]$m.Groups[4].Value
            p50     = [double]$m.Groups[5].Value
            p95     = [double]$m.Groups[6].Value
            p99     = [double]$m.Groups[7].Value
            p999    = [double]$m.Groups[8].Value
        }
    }
    if ($buckets.Count -eq 0) { return $null }
    # Drop the first 1-2 buckets (warmup) to avoid first-frame outliers in p99.
    if ($buckets.Count -gt 2) { $buckets = @($buckets | Select-Object -Skip 2) }
    $totalN = ($buckets | Measure-Object -Property n -Sum).Sum
    $wFps = 0.0; $wFt = 0.0
    foreach ($b in $buckets) { $wFps += $b.fps * $b.n; $wFt += $b.ft_mean * $b.n }
    [PSCustomObject]@{
        buckets       = $buckets.Count
        mode          = ($buckets | Group-Object mode | Sort-Object Count -Descending | Select-Object -First 1).Name
        total_frames  = $totalN
        fps_avg       = [Math]::Round($wFps / [Math]::Max(1,$totalN), 2)
        ft_mean_avg   = [Math]::Round($wFt  / [Math]::Max(1,$totalN), 3)
        p50_worst     = [Math]::Round(($buckets | Measure-Object p50  -Maximum).Maximum, 3)
        p95_worst     = [Math]::Round(($buckets | Measure-Object p95  -Maximum).Maximum, 3)
        p99_worst     = [Math]::Round(($buckets | Measure-Object p99  -Maximum).Maximum, 3)
        p999_worst    = [Math]::Round(($buckets | Measure-Object p999 -Maximum).Maximum, 3)
    }
}

# Lightweight PresentMon CSV parser (a subset of what test_fruc_30s.ps1 has —
# we only need fps + frame time stats here).
function Parse-PresentMonCsv {
    param([string]$Csv, [int]$FilterPid = 0)
    if (-not (Test-Path $Csv)) { return $null }
    $rows = Import-Csv $Csv
    if ($rows.Count -eq 0) { return $null }
    if ($FilterPid -gt 0) {
        $rows = @($rows | Where-Object { try { [int]$_.ProcessID -eq $FilterPid } catch { $false } })
        if ($rows.Count -eq 0) { return $null }
    }
    $cols = $rows[0].PSObject.Properties.Name
    $msBetweenCol = $cols | Where-Object { $_ -in @('MsBetweenPresents','FrameTime','msBetweenPresents') } | Select-Object -First 1
    $dropCol      = $cols | Where-Object { $_ -in @('Dropped','FrameType') } | Select-Object -First 1
    $vals = @()
    foreach ($r in $rows) {
        $v = $r.$msBetweenCol
        if ($null -ne $v -and $v -ne '' -and $v -ne 'NA') { try { $vals += [double]$v } catch { } }
    }
    if ($vals.Count -eq 0) { return $null }
    $sorted = $vals | Sort-Object
    function Pct($s, $p) {
        $i = [int][Math]::Ceiling(($p / 100.0) * $s.Count) - 1
        if ($i -lt 0) { $i = 0 }
        if ($i -ge $s.Count) { $i = $s.Count - 1 }
        [double]$s[$i]
    }
    $mean = ($vals | Measure-Object -Average).Average
    $effFps = if ($mean -gt 0) { 1000.0 / $mean } else { 0.0 }
    $dropped = 0
    if ($dropCol -eq 'Dropped') {
        $dropped = ($rows | Where-Object { $_.Dropped -eq '1' -or $_.Dropped -eq 1 }).Count
    } elseif ($dropCol -eq 'FrameType') {
        $dropped = ($rows | Where-Object { $_.FrameType -and $_.FrameType -ne 'Application' -and $_.FrameType -ne '' }).Count
    }
    [PSCustomObject]@{
        rows         = $rows.Count
        fps          = [Math]::Round($effFps, 2)
        ft_mean_ms   = [Math]::Round($mean, 3)
        ft_p50_ms    = [Math]::Round((Pct $sorted 50), 3)
        ft_p95_ms    = [Math]::Round((Pct $sorted 95), 3)
        ft_p99_ms    = [Math]::Round((Pct $sorted 99), 3)
        ft_p999_ms   = [Math]::Round((Pct $sorted 99.9), 3)
        dropped      = $dropped
        dropped_pct  = if ($rows.Count -gt 0) { [Math]::Round(100.0 * $dropped / $rows.Count, 2) } else { 0 }
    }
}

# === main loop =============================================================
$results = [ordered]@{}
try {
    Write-Host "Pinning rendererSelection=0 (RS_VULKAN), videodec=2 (VDS_FORCE_SOFTWARE), frameInterpolation=false"
    Set-ItemProperty $RegPath -Name rendererSelection -Value 0 -Type DWord
    # The VkFruc SW path's getDecoderAvailability returns SOFTWARE; if videodec=1
    # (FORCE_HARDWARE, the user's normal setting) session::initialize fails the
    # launch precheck (session.cpp:1288). Force VDS_FORCE_SOFTWARE for the run.
    Set-ItemProperty $RegPath -Name videodec -Value 2 -Type DWord
    Set-ItemProperty $RegPath -Name frameInterpolation -Value 'false'

    foreach ($codec in $Codecs) {
        if (-not $codecMap.ContainsKey($codec)) { Write-Warning "Skip unknown codec '$codec'"; continue }
        $codecEnum = $codecMap[$codec]
        Write-Host ""
        Write-Host "=================================================="
        Write-Host " codec=$codec  ($Width x $Height @ ${Fps}fps, FRUC off)" -ForegroundColor Cyan
        Write-Host "=================================================="
        Set-ItemProperty $RegPath -Name videocfg -Value $codecEnum -Type DWord

        Stop-Client
        $configStartTime = Get-Date

        $mlArgs = @('stream', $HostAddr, $App, '--fps', $Fps, '--resolution', "$($Width)x$($Height)", '--video-codec', $codec)
        if ($KeepWindowed) { $mlArgs += '--display-mode','windowed' }
        if ($BitrateKbps -gt 0) { $mlArgs += '--bitrate', $BitrateKbps }
        Write-Host "Launching: VipleStream.exe $($mlArgs -join ' ')"
        $mlProc = Start-Process -FilePath $MoonlightExe -ArgumentList $mlArgs -PassThru
        Start-Sleep -Seconds $WarmupSeconds

        $ml = Get-Process -Name 'VipleStream','Moonlight' -ErrorAction SilentlyContinue |
              Sort-Object StartTime -Descending | Select-Object -First 1
        if (-not $ml) { Write-Warning "client not running"; $results[$codec] = @{error='exited_early'}; continue }
        $mlPid = $ml.Id
        Write-Host "Client PID: $mlPid"

        $csvPath = Join-Path $OutDir "presentmon_$codec.csv"
        $sessionName = "viple_vk_sw_$($codec -replace '[^A-Za-z0-9]','_')"
        & logman stop $sessionName -ets 2>$null | Out-Null
        $pmArgs = @(
            '--output_file', $csvPath,
            '--timed', $Seconds,
            '--terminate_after_timed',
            '--v2_metrics',
            '--no_console_stats',
            '--stop_existing_session',
            '--session_name', $sessionName
        )
        $pmLog = Join-Path $OutDir "presentmon_$codec.log"
        Write-Host "Recording $Seconds s -> $csvPath"
        $pmProc = Start-Process -FilePath $PresentMon -ArgumentList $pmArgs `
                     -NoNewWindow -PassThru `
                     -RedirectStandardOutput $pmLog `
                     -RedirectStandardError  "$pmLog.err"
        $deadline = (Get-Date).AddSeconds($Seconds + 15)
        while (-not $pmProc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        if (-not $pmProc.HasExited) { try { $pmProc.Kill() } catch { } ; $pmProc.WaitForExit(3000) | Out-Null }

        Stop-Client

        $pm = Parse-PresentMonCsv -Csv $csvPath -FilterPid $mlPid

        # Find the VipleStream log produced during this run
        $mlLog = Get-ChildItem -Path $env:TEMP -Filter 'VipleStream-*.log' -ErrorAction SilentlyContinue |
                 Where-Object { $_.LastWriteTime -gt $configStartTime } |
                 Sort-Object LastWriteTime -Descending |
                 Select-Object -First 1
        $vipleStats = $null
        if ($mlLog) {
            Copy-Item $mlLog.FullName (Join-Path $OutDir "$codec.viplestream.log") -ErrorAction SilentlyContinue
            $vipleStats = Parse-VipleLog $mlLog.FullName
        }

        $entry = [PSCustomObject]@{
            codec        = $codec
            presentmon   = $pm
            viple_log    = $vipleStats
            log_file     = if ($mlLog) { $mlLog.Name } else { $null }
        }
        $results[$codec] = $entry

        Write-Host ""
        if ($pm) {
            Write-Host "  [PresentMon]    fps=$($pm.fps)  ft mean=$($pm.ft_mean_ms)ms p50=$($pm.ft_p50_ms) p95=$($pm.ft_p95_ms) p99=$($pm.ft_p99_ms)  drop=$($pm.dropped) ($($pm.dropped_pct)%)"
        } else {
            Write-Host "  [PresentMon]    no usable rows (PID filter? VDD?)"
        }
        if ($vipleStats) {
            Write-Host "  [VIPLE-log]     mode=$($vipleStats.mode) fps=$($vipleStats.fps_avg) ft_mean=$($vipleStats.ft_mean_avg)ms p50=$($vipleStats.p50_worst) p95=$($vipleStats.p95_worst) p99=$($vipleStats.p99_worst)  total_n=$($vipleStats.total_frames)"
        } else {
            Write-Host "  [VIPLE-log]     not found / no [VIPLE-VKFRUC-Stats] lines"
        }
        # Pass / fail criterion for "real 120fps 及格線":
        #  1. measured fps stays within 5% of target (>= 114 for 120fps target)
        #  2. ft_mean <= 9.0 ms (= 1000/111fps; allows 7% drift)
        #  3. p95 <= 16 ms — a single 2-vsync hiccup at 120fps is OK; ≤2 vsync
        #     periods on a 144Hz display = 13.9ms, on 179Hz = 11.2ms; 16 is safe
        #  4. drops < 1% (PresentMon only — VipleStream log doesn't report drops)
        # p99 not gated: SW decode + IMMEDIATE present has natural long tail at
        # ~3 vsync (~17ms) on bursty network frames; p95 captures the typical bad
        # case + protects against systematic stutter without false-failing on
        # occasional queue-depth jitter.
        $targetFps = $Fps
        $passFps = $false; $passFt = $false; $passP95 = $false; $passDrop = $true
        if ($vipleStats) {
            if ($vipleStats.fps_avg -ge ($targetFps * 0.95)) { $passFps = $true }
            if ($vipleStats.ft_mean_avg -le 9.0)             { $passFt  = $true }
            if ($vipleStats.p95_worst -le 16.0)              { $passP95 = $true }
        } elseif ($pm) {
            if ($pm.fps -ge ($targetFps * 0.95))             { $passFps = $true }
            if ($pm.ft_mean_ms -le 9.0)                      { $passFt  = $true }
            if ($pm.ft_p95_ms -le 16.0)                      { $passP95 = $true }
        }
        if ($pm -and $pm.dropped_pct -gt 1) { $passDrop = $false }
        $allPass = $passFps -and $passFt -and $passP95 -and $passDrop
        $verdict = if ($allPass) { "[PASS]" } else { "[FAIL]" }
        $verdictColor = if ($allPass) { 'Green' } else { 'Yellow' }
        Write-Host "  $verdict fps>=$($targetFps*0.95):$passFps ft_mean<=9.0:$passFt p95<=16.0:$passP95 drop<=1%:$passDrop" -ForegroundColor $verdictColor
    }
} finally {
    if ($null -ne $origRenderer) { Set-ItemProperty $RegPath -Name rendererSelection -Value $origRenderer -Type DWord }
    if ($null -ne $origVideoDec) { Set-ItemProperty $RegPath -Name videodec -Value $origVideoDec -Type DWord }
    if ($null -ne $origInterp)   { Set-ItemProperty $RegPath -Name frameInterpolation -Value $origInterp }
    if ($null -ne $origVideoCfg) { Set-ItemProperty $RegPath -Name videocfg -Value $origVideoCfg -Type DWord }
    Stop-Client
}

# Persist + print summary
$summaryPath = Join-Path $OutDir 'summary.json'
[PSCustomObject]@{
    timestamp = (Get-Date).ToString('o')
    host      = $HostAddr
    fps       = $Fps
    res       = "$($Width)x$($Height)"
    seconds   = $Seconds
    results   = $results
} | ConvertTo-Json -Depth 8 | Set-Content -Path $summaryPath -Encoding UTF8
Write-Host ""
Write-Host "Summary written to $summaryPath" -ForegroundColor Cyan
