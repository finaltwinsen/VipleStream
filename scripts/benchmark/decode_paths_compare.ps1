# =============================================================================
#  VipleStream — Decode Path Comparison Benchmark
# =============================================================================
#  比較四條 decode + render path 的 runtime metrics:
#   1. vulkan_sw     — VkFrucRenderer (FFmpeg SW decode → VkFruc-SW upload + Vulkan render)
#   2. vulkan_hw     — VkFrucRenderer (§J.3.e.2.i.8 native VK_KHR_video_decode + Vulkan render)
#   3. d3d11_sw      — D3D11VARenderer (FFmpeg SW decode → D3D11 staging + D3D11 render)
#   4. d3d11_hw      — D3D11VARenderer (D3D11VA hwaccel + D3D11 render)
#
#  Metrics 來源:
#   - VipleStream log (Global video stats): 網路 / 解碼 / 繪製 FPS, 解碼/繪製
#     latency, 丟幀率
#   - PresentMon CSV: 端到端 frame timing (presentation-rate)
#
#  Usage:
#    powershell -File decode_paths_compare.ps1 -Seconds 60
# =============================================================================

param(
    [string]$HostAddr     = '<host>',
    [string]$App          = 'Desktop',
    [int]   $Seconds      = 60,
    [int]   $WarmupSeconds = 10,
    [string[]]$Configs    = @('vulkan_sw','vulkan_hw','d3d11_sw','d3d11_hw'),
    [string]$VideoCodec   = 'HEVC',
    [string]$OutDir       = ''
)

$ErrorActionPreference = 'Continue'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)
$PresentMon = Join-Path $ProjectRoot 'scripts\PresentMon-2.4.1-x64.exe'
$ExePath = 'C:\Program Files\Moonlight Game Streaming\VipleStream.exe'

if (-not (Test-Path $PresentMon)) { Write-Warning "PresentMon not found — will skip CSV capture" }
if (-not (Test-Path $ExePath))    { throw "VipleStream client not installed at $ExePath" }

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $OutDir = Join-Path $ProjectRoot "scripts\benchmark\output\decode_compare_$stamp"
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Write-Host "Output: $OutDir" -ForegroundColor Cyan

# §J.3.e.2.i.8 — v1.2.44 rebrand changed QCoreApplication::setOrganizationName
# from "Moonlight Game Streaming Project" to "VipleStream", so QSettings registry
# path is now \Software\VipleStream\VipleStream (NOT the legacy Moonlight path).
# The old test_fruc_30s.ps1 still references the legacy path and is broken too.
$RegPath = 'HKCU:\Software\VipleStream\VipleStream'

# Save originals so we can restore at the end
$origRenderer = (Get-ItemProperty $RegPath -Name rendererSelection -ErrorAction SilentlyContinue).rendererSelection

# Config table — each entry decides registry tweaks + CLI args + env vars
# rendererSelection: 0 = RS_VULKAN, 1 = RS_D3D11
# videoDecoder CLI: 'software' or 'hardware'
$configMap = @{
    'vulkan_sw' = @{
        renderer  = 0
        decoder   = 'software'
        env       = @{ VIPLE_VKFRUC_NATIVE_DECODE = '0'; VIPLE_VKFRUC_NATIVE_DECODE_PARALLEL = '0' }
        desc      = 'Vulkan SW: FFmpeg software decode + VkFruc-SW staging upload + Vulkan render'
    }
    'vulkan_hw' = @{
        renderer  = 0
        decoder   = 'software'
        env       = @{ VIPLE_VKFRUC_NATIVE_DECODE = '1'; VIPLE_VKFRUC_NATIVE_DECODE_PARALLEL = '1' }
        desc      = 'Vulkan HW: §J.3.e.2.i.8 native VK_KHR_video_decode + Vulkan render (parallel mode)'
    }
    'd3d11_sw' = @{
        renderer  = 1
        decoder   = 'software'
        env       = @{ VIPLE_VKFRUC_NATIVE_DECODE = '0'; VIPLE_VKFRUC_NATIVE_DECODE_PARALLEL = '0' }
        desc      = 'D3D11 SW: FFmpeg software decode + D3D11 staging + D3D11 render'
    }
    'd3d11_hw' = @{
        renderer  = 1
        decoder   = 'hardware'
        env       = @{ VIPLE_VKFRUC_NATIVE_DECODE = '0'; VIPLE_VKFRUC_NATIVE_DECODE_PARALLEL = '0' }
        desc      = 'D3D11 HW: D3D11VA hwaccel + D3D11 render'
    }
}

function Stop-Stream {
    # Force-kill — earlier we tried `& $ExePath quit $host` for graceful end-of-
    # session stats but that launches a parallel VipleStream invocation that
    # races with our Set-ItemProperty rendererSelection write, occasionally
    # clobbering it with the in-memory value (which was the prior config).
    # Inline [VIPLE-*-Stats] log lines are sufficient for our metrics.
    Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 1500
}

function Restart-Server {
    Write-Host "  Restarting host service VipleStreamServer..."
    ssh -o BatchMode=yes <user>@<host> 'powershell -Command "Restart-Service VipleStreamServer; Start-Sleep -Seconds 4"' 2>$null | Out-Null
    Start-Sleep -Seconds 4
}

# Core: run one stream, capture log + return parsed metrics
function Invoke-OneRun {
    param(
        [string]$Name,
        [hashtable]$Cfg,
        [int]$DurationSeconds
    )
    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Yellow
    Write-Host "  $($Cfg.desc)"

    Stop-Stream
    # Apply registry — renderer selection.  Verify the write actually stuck
    # (Stop-Stream race + Moonlight UI processes still running can rewrite it).
    Set-ItemProperty -Path $RegPath -Name rendererSelection -Value $Cfg.renderer -Type DWord -ErrorAction SilentlyContinue
    $actual = (Get-ItemProperty $RegPath -Name rendererSelection -ErrorAction SilentlyContinue).rendererSelection
    if ($actual -ne $Cfg.renderer) {
        Write-Warning "  Registry rendererSelection wanted=$($Cfg.renderer) but read=$actual — retrying..."
        Start-Sleep -Milliseconds 500
        Set-ItemProperty -Path $RegPath -Name rendererSelection -Value $Cfg.renderer -Type DWord
        $actual = (Get-ItemProperty $RegPath -Name rendererSelection).rendererSelection
    }
    Write-Host "  Registry: rendererSelection=$actual (target=$($Cfg.renderer))"

    # Apply env vars
    foreach ($k in $Cfg.env.Keys) {
        Set-Item -Path "Env:\$k" -Value $Cfg.env[$k]
    }
    Write-Host "  Env: $($Cfg.env.Keys -join ', ')"

    Restart-Server

    # Track log file count BEFORE launch so we can find this run's log
    $logBefore = @(Get-ChildItem "$env:TEMP\VipleStream-*.log" -ErrorAction SilentlyContinue).Count

    $args = @('stream', $HostAddr, $App, '--video-codec', $VideoCodec, '--video-decoder', $Cfg.decoder)
    Write-Host "  Launch: VipleStream.exe $($args -join ' ')"
    $proc = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru
    Write-Host "  PID: $($proc.Id), warming up $WarmupSeconds s..."
    Start-Sleep -Seconds $WarmupSeconds

    # Optional PresentMon capture
    $pmCsv = Join-Path $OutDir "$Name`_presentmon.csv"
    if (Test-Path $PresentMon) {
        Write-Host "  PresentMon capture for $DurationSeconds s..."
        $pmArgs = @('--process_name','VipleStream.exe','--output_file',$pmCsv,'--timed',$DurationSeconds,'--terminate_after_timed','--stop_existing_session')
        & $PresentMon @pmArgs 2>&1 | Out-Null
    } else {
        Write-Host "  (no PresentMon; sleeping $DurationSeconds s)"
        Start-Sleep -Seconds $DurationSeconds
    }

    # Stop stream gracefully — wait a moment so it writes the final stats block
    Stop-Stream
    Start-Sleep -Seconds 2

    # Find this run's log: newest log created after launch
    $logFile = Get-ChildItem "$env:TEMP\VipleStream-*.log" -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
    if (-not $logFile) {
        Write-Warning "  No VipleStream log found after run."
        return $null
    }
    $logCopy = Join-Path $OutDir "$Name.log"
    Copy-Item $logFile.FullName $logCopy -Force
    Write-Host "  Log: $logCopy ($($logFile.Length) bytes)"

    # Parse stats block from log
    $logText = Get-Content $logFile.FullName -Raw -Encoding UTF8

    $metrics = [ordered]@{
        config              = $Name
        log_size_bytes      = $logFile.Length
        decode_submits      = ([regex]::Matches($logText, 'Phase 1\.3d decode submitted')).Count
        device_lost_count   = ([regex]::Matches($logText, 'vkQueueSubmit rc=-4')).Count
        renderer_chosen     = '?'
        net_recv_fps        = $null
        decode_fps          = $null
        render_fps          = $null
        host_lat_avg_ms     = $null
        net_drop_pct        = $null
        net_jitter_drop_pct = $null
        net_lat_ms          = $null
        avg_decode_ms       = $null
        avg_queue_ms        = $null
        avg_render_ms       = $null
    }

    # Renderer chosen
    if     ($logText -match '\[VIPLE-VKFRUC\] §J\.3\.e\.2\.i\.2 ctor') { $metrics.renderer_chosen = 'VkFruc' }
    elseif ($logText -match 'D3D11VARenderer|D3D11VA renderer chosen') { $metrics.renderer_chosen = 'D3D11VA' }

    # Match the localized stats block (zh-TW)
    function ExtractFloat([string]$pattern) {
        if ($logText -match $pattern) { return [double]$matches[1] } else { return $null }
    }
    $metrics.net_recv_fps     = ExtractFloat '網路接收幀率:\s*([0-9.]+)\s*FPS'
    $metrics.decode_fps       = ExtractFloat '解碼幀率:\s*([0-9.]+)\s*FPS'
    $metrics.render_fps       = ExtractFloat '繪製幀率:\s*([0-9.]+)\s*FPS'
    $metrics.host_lat_avg_ms  = ExtractFloat '主機處理延遲[^/]*/[^/]*/\s*([0-9.]+)\s*ms'
    $metrics.net_drop_pct     = ExtractFloat '網路丟幀:\s*([0-9.]+)%'
    $metrics.net_jitter_drop_pct = ExtractFloat '網路抖動丟幀:\s*([0-9.]+)%'
    $metrics.net_lat_ms       = ExtractFloat '平均網路延遲:\s*([0-9.]+)\s*ms'
    $metrics.avg_decode_ms    = ExtractFloat '平均解碼時間:\s*([0-9.]+)\s*ms'
    $metrics.avg_queue_ms     = ExtractFloat '平均佇列延遲:\s*([0-9.]+)\s*ms'
    $metrics.avg_render_ms    = ExtractFloat '平均繪製時間[^:]*:\s*([0-9.]+)\s*ms'

    # §J.3.e.2.i.8 — also parse the renderer's per-window inline stats lines
    # (logged by VkFruc + D3D11VA in the [VIPLE-VKFRUC-Stats] / [VIPLE-D3D11VA-Stats]
    # categories every ~5s).  These are reliable even when force-kill skips
    # the end-of-session block.  Take the LAST occurrence as steady-state.
    $statLines = [regex]::Matches($logText, '\[VIPLE-(?:VKFRUC|D3D11VA)-Stats\].*?n=(\d+)\s+fps=([0-9.]+)\s+ft_mean=([0-9.]+)ms\s+p50=([0-9.]+)\s+p95=([0-9.]+)\s+p99=([0-9.]+)\s+p99\.9=([0-9.]+)')
    if ($statLines.Count -gt 0) {
        $last = $statLines[$statLines.Count - 1]
        $metrics | Add-Member -NotePropertyName inline_n        -NotePropertyValue ([int]$last.Groups[1].Value)
        $metrics | Add-Member -NotePropertyName inline_fps      -NotePropertyValue ([double]$last.Groups[2].Value)
        $metrics | Add-Member -NotePropertyName inline_ft_mean  -NotePropertyValue ([double]$last.Groups[3].Value)
        $metrics | Add-Member -NotePropertyName inline_p50      -NotePropertyValue ([double]$last.Groups[4].Value)
        $metrics | Add-Member -NotePropertyName inline_p95      -NotePropertyValue ([double]$last.Groups[5].Value)
        $metrics | Add-Member -NotePropertyName inline_p99      -NotePropertyValue ([double]$last.Groups[6].Value)
        $metrics | Add-Member -NotePropertyName inline_p99_9    -NotePropertyValue ([double]$last.Groups[7].Value)
    }
    # Cumulative real/interp from the dual-present "cumul" line
    $cumLine = [regex]::Match($logText, 'cumul real=(\d+) interp=(\d+)(?:[^c]*compute_gpu_mean=([0-9.]+)ms)?')
    if ($cumLine.Success) {
        $metrics | Add-Member -NotePropertyName cumul_real      -NotePropertyValue ([int]$cumLine.Groups[1].Value)
        $metrics | Add-Member -NotePropertyName cumul_interp    -NotePropertyValue ([int]$cumLine.Groups[2].Value)
        if ($cumLine.Groups[3].Success) {
            $metrics | Add-Member -NotePropertyName compute_gpu_ms -NotePropertyValue ([double]$cumLine.Groups[3].Value)
        }
    }
    # Phase 1.4 native-decode copy count (only meaningful for vulkan_hw)
    $nativeCopies = [regex]::Matches($logText, 'Phase 1\.4 native-decode copy #')
    $metrics | Add-Member -NotePropertyName native_copies -NotePropertyValue $nativeCopies.Count

    # PresentMon stats (frame time stats from CSV)
    if ((Test-Path $pmCsv) -and ((Get-Item $pmCsv).Length -gt 100)) {
        try {
            $rows = Import-Csv $pmCsv -ErrorAction Stop
            # Column name may vary; try common ones
            $col = if ($rows[0].PSObject.Properties.Name -contains 'msBetweenPresents') { 'msBetweenPresents' }
                   elseif ($rows[0].PSObject.Properties.Name -contains 'MsBetweenPresents') { 'MsBetweenPresents' }
                   else { $null }
            if ($col -and $rows.Count -gt 30) {
                $vals = $rows | ForEach-Object { [double]$_.$col } | Where-Object { $_ -gt 0 -and $_ -lt 1000 }
                $sorted = $vals | Sort-Object
                $metrics | Add-Member -MemberType NoteProperty -Name pm_count       -Value $vals.Count
                $metrics | Add-Member -MemberType NoteProperty -Name pm_mean_ms     -Value ([Math]::Round(($vals | Measure-Object -Average).Average, 2))
                $metrics | Add-Member -MemberType NoteProperty -Name pm_p50_ms      -Value ([Math]::Round($sorted[[int]($sorted.Count * 0.50)], 2))
                $metrics | Add-Member -MemberType NoteProperty -Name pm_p95_ms      -Value ([Math]::Round($sorted[[int]($sorted.Count * 0.95)], 2))
                $metrics | Add-Member -MemberType NoteProperty -Name pm_p99_ms      -Value ([Math]::Round($sorted[[int]($sorted.Count * 0.99)], 2))
                $metrics | Add-Member -MemberType NoteProperty -Name pm_present_fps -Value ([Math]::Round(1000.0 / ($vals | Measure-Object -Average).Average, 2))
            }
        } catch {
            Write-Warning "  PresentMon CSV parse failed: $_"
        }
    }

    Write-Host "  → renderer=$($metrics.renderer_chosen) decode_fps=$($metrics.decode_fps) render_fps=$($metrics.render_fps) decode_ms=$($metrics.avg_decode_ms) render_ms=$($metrics.avg_render_ms)" -ForegroundColor Green

    return [pscustomobject]$metrics
}

# === Main loop ===
$results = @()
foreach ($cfg in $Configs) {
    if (-not $configMap.ContainsKey($cfg)) { Write-Warning "Unknown config: $cfg"; continue }
    $r = Invoke-OneRun -Name $cfg -Cfg $configMap[$cfg] -DurationSeconds $Seconds
    if ($r) { $results += $r }
}

# Restore original registry value
if ($origRenderer -ne $null) {
    Set-ItemProperty -Path $RegPath -Name rendererSelection -Value $origRenderer -Type DWord -ErrorAction SilentlyContinue
    Write-Host ""
    Write-Host "Restored rendererSelection=$origRenderer"
}

# Save results
$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $OutDir 'summary.json') -Encoding UTF8
$results | Export-Csv (Join-Path $OutDir 'summary.csv') -NoTypeInformation -Encoding UTF8

# Generate Markdown report
$md = @()
$md += "# VipleStream Decode Path Comparison"
$md += ""
$md += "**Date**: $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$md += "**Duration per config**: $Seconds s (warmup $WarmupSeconds s)"
$md += "**Codec**: $VideoCodec"
$md += "**Host**: $HostAddr"
$md += ""
$md += "## End-of-session stats (Moonlight) — only present if graceful quit succeeded"
$md += ""
$md += "| Config | Renderer | Net FPS | Decode FPS | Render FPS | Decode ms | Render ms | Queue ms | Net drop% | DeviceLost |"
$md += "|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $md += "| $($r.config) | $($r.renderer_chosen) | $($r.net_recv_fps) | $($r.decode_fps) | $($r.render_fps) | $($r.avg_decode_ms) | $($r.avg_render_ms) | $($r.avg_queue_ms) | $($r.net_drop_pct) | $($r.device_lost_count) |"
}
$md += ""
$md += "## Renderer inline stats (last 5s window before stop) — always present"
$md += ""
$md += "| Config | n | FPS | ft_mean ms | P50 | P95 | P99 | P99.9 | cumul real | cumul interp | compute GPU ms | native copies |"
$md += "|---|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $md += "| $($r.config) | $($r.inline_n) | $($r.inline_fps) | $($r.inline_ft_mean) | $($r.inline_p50) | $($r.inline_p95) | $($r.inline_p99) | $($r.inline_p99_9) | $($r.cumul_real) | $($r.cumul_interp) | $($r.compute_gpu_ms) | $($r.native_copies) |"
}
$md += ""
$md += "## PresentMon (graphics-side frame pacing)"
$md += ""
$md += "| Config | Present FPS | mean ms | P50 | P95 | P99 |"
$md += "|---|---|---|---|---|---|"
foreach ($r in $results) {
    $md += "| $($r.config) | $($r.pm_present_fps) | $($r.pm_mean_ms) | $($r.pm_p50_ms) | $($r.pm_p95_ms) | $($r.pm_p99_ms) |"
}
$md += ""
$md += "## Per-config descriptions"
foreach ($cfg in $Configs) {
    if ($configMap.ContainsKey($cfg)) {
        $md += "- **$cfg**: $($configMap[$cfg].desc)"
    }
}

$md -join "`r`n" | Out-File (Join-Path $OutDir 'report.md') -Encoding UTF8
Write-Host ""
Write-Host "Report: $(Join-Path $OutDir 'report.md')" -ForegroundColor Cyan
Get-Content (Join-Path $OutDir 'report.md') | Write-Host
