# Analyze existing PresentMon CSVs without re-running the full test.
# Points at a directory that already contains presentmon_*.csv files.
param(
    [Parameter(Mandatory=$true)][string]$Dir
)

$ErrorActionPreference = 'Continue'
# Self-contained parser; doesn't import test_fruc_30s.ps1

function Get-Series($rows, $col) {
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
function Pct($sorted, $p) {
    if ($sorted.Count -eq 0) { return 0.0 }
    $idx = [int][Math]::Ceiling(($p / 100.0) * $sorted.Count) - 1
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return [double]$sorted[$idx]
}
function Stats($vals) {
    if ($vals.Count -eq 0) { return $null }
    $sorted = $vals | Sort-Object
    $mean = ($vals | Measure-Object -Average).Average
    $var = 0.0
    foreach ($v in $vals) { $var += [Math]::Pow($v - $mean, 2) }
    if ($vals.Count -gt 1) { $var /= ($vals.Count - 1) }
    [PSCustomObject]@{
        count=$vals.Count
        mean=[Math]::Round($mean,3)
        std=[Math]::Round([Math]::Sqrt($var),3)
        min=[Math]::Round($sorted[0],3)
        p50=[Math]::Round((Pct $sorted 50),3)
        p95=[Math]::Round((Pct $sorted 95),3)
        p99=[Math]::Round((Pct $sorted 99),3)
        p999=[Math]::Round((Pct $sorted 99.9),3)
        max=[Math]::Round($sorted[-1],3)
    }
}

Write-Host ("Analyzing: {0}" -f $Dir)
Write-Host ""

$configs = 'off','quality','balanced','performance'
foreach ($cfg in $configs) {
    $csv = Join-Path $Dir "presentmon_$cfg.csv"
    if (-not (Test-Path $csv)) { continue }
    $rows = Import-Csv $csv
    if ($rows.Count -eq 0) { continue }

    $cols = $rows[0].PSObject.Properties.Name
    $ft   = Get-Series $rows ($cols | Where-Object { $_ -in @('FrameTime','MsBetweenPresents','msBetweenPresents') } | Select-Object -First 1)
    $gpu  = Get-Series $rows ($cols | Where-Object { $_ -in @('GPUBusy','MsGPUActive') } | Select-Object -First 1)
    $dl   = Get-Series $rows ($cols | Where-Object { $_ -in @('DisplayLatency') } | Select-Object -First 1)
    $cpu  = Get-Series $rows ($cols | Where-Object { $_ -in @('CPUBusy') } | Select-Object -First 1)
    $cpuw = Get-Series $rows ($cols | Where-Object { $_ -in @('CPUWait') } | Select-Object -First 1)

    $ftS  = Stats $ft
    $gpuS = Stats $gpu
    $dlS  = Stats $dl
    $cpuS = Stats $cpu
    $cpuwS= Stats $cpuw

    $spikesOver2x = if ($ftS) { ($ft | Where-Object { $_ -gt $ftS.mean * 2.0 }).Count } else { 0 }
    $spikesOver3x = if ($ftS) { ($ft | Where-Object { $_ -gt $ftS.mean * 3.0 }).Count } else { 0 }

    Write-Host "=== $cfg ===" -ForegroundColor Cyan
    Write-Host ("  FPS          : {0:F2}" -f (1000.0 / $ftS.mean))
    Write-Host ("  FrameTime    : mean={0}  p50={1}  p95={2}  p99={3}  p99.9={4}  max={5}  std={6}" `
        -f $ftS.mean, $ftS.p50, $ftS.p95, $ftS.p99, $ftS.p999, $ftS.max, $ftS.std)
    if ($gpuS) {
        Write-Host ("  GPU busy     : mean={0}  p50={1}  p95={2}  p99={3}  max={4}" `
            -f $gpuS.mean, $gpuS.p50, $gpuS.p95, $gpuS.p99, $gpuS.max)
    }
    if ($dlS) {
        Write-Host ("  DisplayLat   : mean={0}  p95={1}  p99={2}  max={3}" `
            -f $dlS.mean, $dlS.p95, $dlS.p99, $dlS.max)
    }
    if ($cpuS) { Write-Host ("  CPU busy     : mean={0}  p95={1}  p99={2}" -f $cpuS.mean, $cpuS.p95, $cpuS.p99) }
    if ($cpuwS){ Write-Host ("  CPU wait     : mean={0}  p95={1}  p99={2}" -f $cpuwS.mean, $cpuwS.p95, $cpuwS.p99) }
    $totFT = $ft.Count
    $pct2x = if ($totFT -gt 0) { [Math]::Round(100.0 * $spikesOver2x / $totFT, 2) } else { 0 }
    $pct3x = if ($totFT -gt 0) { [Math]::Round(100.0 * $spikesOver3x / $totFT, 2) } else { 0 }
    Write-Host ("  Spikes       : >2x mean = $spikesOver2x ({0}%)  >3x = $spikesOver3x ({1}%)" -f $pct2x, $pct3x)
    Write-Host ""
}
