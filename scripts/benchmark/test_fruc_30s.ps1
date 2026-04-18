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
    [string[]]$Configs = @('off','quality','balanced','performance'),
    [int]   $Fps = 60,          # server-side FPS; with FRUC client should show 2x
    [int]   $Width = 1920,
    [int]   $Height = 1080,
    [switch]$KeepWindowed,      # stream in windowed mode so console is visible
    [string]$OutDir = ''
)

# Don't let Qt/SDL stderr warnings from Moonlight terminate the script.
# We still want terminating errors from our own code, so use Continue globally
# and explicit -ErrorAction where we care.
$ErrorActionPreference = 'Continue'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$PresentMon = Join-Path $ProjectRoot 'scripts\PresentMon-2.4.1-x64.exe'
$MoonlightExe = 'C:\Program Files\Moonlight Game Streaming\Moonlight.exe'

if (-not (Test-Path $PresentMon)) { throw "PresentMon not found: $PresentMon" }
if (-not (Test-Path $MoonlightExe)) { throw "Moonlight not found: $MoonlightExe" }

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $OutDir = Join-Path $ProjectRoot "temp\fruc_test_$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$RegPath = 'HKCU:\Software\Moonlight Game Streaming Project\Moonlight'

# Save original settings so we can restore
$origInterp = (Get-ItemProperty $RegPath -Name frameInterpolation -ErrorAction SilentlyContinue).frameInterpolation
$origBackend = (Get-ItemProperty $RegPath -Name frucBackend -ErrorAction SilentlyContinue).frucBackend
$origQuality = (Get-ItemProperty $RegPath -Name frucQuality -ErrorAction SilentlyContinue).frucQuality
Write-Host "Original: frameInterpolation=$origInterp frucBackend=$origBackend frucQuality=$origQuality"
Write-Host ""

# Map config name -> registry values
$presetMap = @{
    'off'         = @{ interp = 'false'; backend = 0; quality = 1 }  # FRUC disabled
    'quality'     = @{ interp = 'true';  backend = 0; quality = 0 }  # FQ_QUALITY
    'balanced'    = @{ interp = 'true';  backend = 0; quality = 1 }  # FQ_BALANCED
    'performance' = @{ interp = 'true';  backend = 0; quality = 2 }  # FQ_PERFORMANCE
    'nvof_quality'= @{ interp = 'true';  backend = 1; quality = 0 }  # NVIDIA OF quality
}

function Stop-Moonlight {
    # Force-kill; graceful quit path currently races the stream launch and
    # Moonlight's own stderr spam trips ErrorActionPreference.
    Get-Process -Name 'Moonlight' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
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
    param([string]$Csv)

    if (-not (Test-Path $Csv)) { return $null }

    $rows = Import-Csv $Csv
    if ($rows.Count -eq 0) { return $null }

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

# -----------------------------------------------------------------------------
$results = [ordered]@{}

try {
    foreach ($cfg in $Configs) {
        if (-not $presetMap.ContainsKey($cfg)) {
            Write-Warning "Unknown preset '$cfg', skipping"
            continue
        }
        $p = $presetMap[$cfg]
        Write-Host "=============================================="
        Write-Host " FRUC config: $cfg" -ForegroundColor Cyan
        Write-Host "  interp=$($p.interp) backend=$($p.backend) quality=$($p.quality)"
        Write-Host "=============================================="

        # Apply registry
        Set-ItemProperty $RegPath -Name frameInterpolation -Value $p.interp
        Set-ItemProperty $RegPath -Name frucBackend        -Value $p.backend -Type DWord
        Set-ItemProperty $RegPath -Name frucQuality        -Value $p.quality -Type DWord

        Stop-Moonlight

        # Launch streaming
        $mlArgs = @('stream', $HostAddr, $App, '--fps', $Fps, '--resolution', "$($Width)x$($Height)")
        if ($KeepWindowed) { $mlArgs += '--display-mode','windowed' }
        Write-Host "Launching: Moonlight.exe $($mlArgs -join ' ')"
        Start-Process -FilePath $MoonlightExe -ArgumentList $mlArgs -WindowStyle Minimized | Out-Null

        Write-Host "Warming up $WarmupSeconds s..."
        Start-Sleep -Seconds $WarmupSeconds

        # Verify Moonlight is running
        $ml = Get-Process -Name 'Moonlight' -ErrorAction SilentlyContinue
        if (-not $ml) {
            Write-Warning "Moonlight not running for cfg=$cfg"
            $results[$cfg] = @{ error = 'moonlight_exited_early' }
            continue
        }

        $csvPath = Join-Path $OutDir "presentmon_$cfg.csv"
        Write-Host "Recording $Seconds s -> $csvPath"

        # Kill any orphaned ETW session left over from a previous force-kill.
        # Without this, the kernel-side session from a prior PresentMon still
        # exists and blocks capture even though the PresentMon process is gone.
        $sessionName = "viple_fruc_$cfg"
        & logman stop $sessionName -ets 2>$null | Out-Null

        $pmArgs = @(
            '--process_name','Moonlight.exe',
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
        $m = Parse-PresentMonCsv $csvPath
        if ($null -eq $m) {
            $results[$cfg] = @{ error = 'no_data' }
            continue
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
        Write-Host ""
    }
}
finally {
    # Restore original settings
    if ($null -ne $origInterp) { Set-ItemProperty $RegPath -Name frameInterpolation -Value $origInterp }
    if ($null -ne $origBackend){ Set-ItemProperty $RegPath -Name frucBackend -Value $origBackend -Type DWord }
    if ($null -ne $origQuality){ Set-ItemProperty $RegPath -Name frucQuality -Value $origQuality -Type DWord }
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
