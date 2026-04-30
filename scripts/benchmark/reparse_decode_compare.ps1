# Re-parse existing decode-compare run logs (no streaming) and rebuild report.
# Usage:
#   powershell -File reparse_decode_compare.ps1 -Dir scripts\benchmark\output\decode_compare_<stamp>
param(
    [string]$Dir = ''
)
if (-not $Dir) {
    $Dir = (Get-ChildItem -Directory 'scripts\benchmark\output' -Filter 'decode_compare_*' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
Write-Host "Re-parsing: $Dir"

function Parse-Log {
    param([string]$path, [string]$cfgName)
    $logText = Get-Content $path -Raw -Encoding UTF8
    $r = [ordered]@{ config = $cfgName }
    $r.log_size_bytes      = (Get-Item $path).Length
    $r.decode_submits      = ([regex]::Matches($logText, 'Phase 1\.3d decode submitted')).Count
    $r.device_lost_count   = ([regex]::Matches($logText, 'vkQueueSubmit rc=-4')).Count
    $r.renderer_chosen     = '?'
    # Renderer detection: VkFruc has a custom ctor log, D3D11VA tags itself
    # via [VIPLE-FRUC-Stats] lines (its FRUC compute chain), and SDL fallback
    # is what kicks in when D3D11VA HW path can't satisfy SW request.
    if     ($logText -match '\[VIPLE-VKFRUC\] [^\n]*i\.2 ctor') { $r.renderer_chosen = 'VkFruc' }
    elseif ($logText -match '\[VIPLE-FRUC-Stats\]')             { $r.renderer_chosen = 'D3D11VA' }
    elseif ($logText -match "Renderer 'SDL' chosen")            { $r.renderer_chosen = 'SDL' }
    elseif ($logText -match 'D3D11VARenderer|D3D11VA renderer chosen') { $r.renderer_chosen = 'D3D11VA' }

    function ExtractFloat([string]$pattern) {
        if ($logText -match $pattern) { return [double]$matches[1] } else { return $null }
    }
    $r.net_recv_fps     = ExtractFloat '網路接收幀率:\s*([0-9.]+)'
    $r.decode_fps       = ExtractFloat '解碼幀率:\s*([0-9.]+)'
    $r.render_fps       = ExtractFloat '繪製幀率:\s*([0-9.]+)'
    $r.host_lat_avg_ms  = ExtractFloat '主機處理延遲[^/]*/[^/]*/\s*([0-9.]+)\s*ms'
    $r.net_drop_pct     = ExtractFloat '網路丟幀:\s*([0-9.]+)%'
    $r.net_jitter_drop_pct = ExtractFloat '網路抖動丟幀:\s*([0-9.]+)%'
    $r.net_lat_ms       = ExtractFloat '平均網路延遲:\s*([0-9.]+)\s*ms'
    $r.avg_decode_ms    = ExtractFloat '平均解碼時間:\s*([0-9.]+)\s*ms'
    $r.avg_queue_ms     = ExtractFloat '平均佇列延遲:\s*([0-9.]+)\s*ms'
    $r.avg_render_ms    = ExtractFloat '平均繪製時間[^:]*:\s*([0-9.]+)\s*ms'

    # Two stats line formats:
    # - VkFruc:  "[VIPLE-VKFRUC-Stats] dual-present n=N fps=X ft_mean=Yms p50=A p95=B p99=C p99.9=D"
    # - D3D11VA: "[VIPLE-PRESENT-Stats] real n=N fps=X ft_mean=Yms p50=A p95=B p99=C p99.9=D ..."
    # - SDL (fallback for d3d11_sw): no inline stats, must rely on PresentMon
    $statLines = [regex]::Matches($logText, '\[VIPLE-(?:VKFRUC-Stats|PRESENT-Stats)\][^\[]*?n=(\d+)\s+fps=([0-9.]+)\s+ft_mean=([0-9.]+)ms\s+p50=([0-9.]+)\s+p95=([0-9.]+)\s+p99=([0-9.]+)\s+p99\.9=([0-9.]+)')
    if ($statLines.Count -gt 0) {
        # Average over the last N stats windows (more representative than just final)
        $vals = @()
        $start = [Math]::Max(0, $statLines.Count - 10)
        for ($i = $start; $i -lt $statLines.Count; $i++) {
            $vals += @{
                n  = [int]$statLines[$i].Groups[1].Value
                fps = [double]$statLines[$i].Groups[2].Value
                ft  = [double]$statLines[$i].Groups[3].Value
                p50 = [double]$statLines[$i].Groups[4].Value
                p95 = [double]$statLines[$i].Groups[5].Value
                p99 = [double]$statLines[$i].Groups[6].Value
                p999 = [double]$statLines[$i].Groups[7].Value
            }
        }
        $r.windows           = $statLines.Count
        $r.inline_fps_avg    = [Math]::Round(($vals.fps | Measure-Object -Average).Average, 2)
        $r.inline_ft_mean_ms = [Math]::Round(($vals.ft  | Measure-Object -Average).Average, 2)
        $r.inline_p50_ms     = [Math]::Round(($vals.p50 | Measure-Object -Average).Average, 2)
        $r.inline_p95_ms     = [Math]::Round(($vals.p95 | Measure-Object -Average).Average, 2)
        $r.inline_p99_ms     = [Math]::Round(($vals.p99 | Measure-Object -Average).Average, 2)
        $r.inline_p999_ms    = [Math]::Round(($vals.p999| Measure-Object -Average).Average, 2)
    }
    $cumulMatches = [regex]::Matches($logText, 'cumul real=(\d+) interp=(\d+)(?:[^c]*compute_gpu_mean=([0-9.]+)ms)?')
    if ($cumulMatches.Count -gt 0) {
        $last = $cumulMatches[$cumulMatches.Count - 1]
        $r.cumul_real      = [int]$last.Groups[1].Value
        $r.cumul_interp    = [int]$last.Groups[2].Value
        if ($last.Groups[3].Success) { $r.compute_gpu_ms = [double]$last.Groups[3].Value }
    }
    $r.native_copies = ([regex]::Matches($logText, 'Phase 1\.4 native-decode copy #')).Count
    $r.native_decode_submits = $r.decode_submits  # alias for clarity
    return [pscustomobject]$r
}

$results = @()
foreach ($cfg in @('vulkan_sw','vulkan_hw','d3d11_sw','d3d11_hw')) {
    $log = Join-Path $Dir "$cfg.log"
    if (Test-Path $log) {
        Write-Host "  Parsing $cfg.log ($((Get-Item $log).Length) bytes)..."
        $results += Parse-Log -path $log -cfgName $cfg
    } else {
        Write-Host "  $cfg.log MISSING"
    }
}

$results | ConvertTo-Json -Depth 4 | Out-File (Join-Path $Dir 'summary.json') -Encoding UTF8
$results | Export-Csv (Join-Path $Dir 'summary.csv') -NoTypeInformation -Encoding UTF8

# Rebuild Markdown report
$md = @()
$md += "# VipleStream Decode Path Comparison (re-parsed)"
$md += ""
$md += "**Run dir**: $Dir"
$md += ""
$md += "## Renderer inline stats (averaged over last ~10 × 5s windows = ~50s steady state)"
$md += ""
$md += "| Config | Renderer | Windows | FPS | ft_mean ms | P50 | P95 | P99 | P99.9 | cumul real | cumul interp | compute GPU ms | native copies | decode submits | DeviceLost |"
$md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $md += "| $($r.config) | $($r.renderer_chosen) | $($r.windows) | $($r.inline_fps_avg) | $($r.inline_ft_mean_ms) | $($r.inline_p50_ms) | $($r.inline_p95_ms) | $($r.inline_p99_ms) | $($r.inline_p999_ms) | $($r.cumul_real) | $($r.cumul_interp) | $($r.compute_gpu_ms) | $($r.native_copies) | $($r.decode_submits) | $($r.device_lost_count) |"
}
$md += ""
$md += "## End-of-session stats (Moonlight global) — only present when graceful quit succeeded"
$md += ""
$md += "| Config | Net FPS | Decode FPS | Render FPS | Decode ms | Queue ms | Render ms | Net drop% | Net jitter drop% | Net lat ms |"
$md += "|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $md += "| $($r.config) | $($r.net_recv_fps) | $($r.decode_fps) | $($r.render_fps) | $($r.avg_decode_ms) | $($r.avg_queue_ms) | $($r.avg_render_ms) | $($r.net_drop_pct) | $($r.net_jitter_drop_pct) | $($r.net_lat_ms) |"
}

$md -join "`r`n" | Out-File (Join-Path $Dir 'report.md') -Encoding UTF8
Write-Host ""
Write-Host "Report regenerated: $(Join-Path $Dir 'report.md')" -ForegroundColor Cyan
Get-Content (Join-Path $Dir 'report.md') | Write-Host
