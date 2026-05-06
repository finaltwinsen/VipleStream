# capture_full_frames.ps1 — 抓 client display 完整 frame 為 PNG sequence
#
# 用法:
#   pwsh scripts\benchmark\capture_full_frames.ps1 -OutLabel with_fruc
#   (在 stream 視窗有焦點且 FRUC ON 時跑)
#
#   pwsh scripts\benchmark\capture_full_frames.ps1 -OutLabel real_only
#   (Ctrl+Alt+Shift+F 切 FRUC OFF 後跑)
#
# 結果在 temp\full_frames\<label>\frame_NNNN.png — 1080p 60fps × 1s
# = 60 張 PNG (約 90~180 MB), 用 image viewer 順序看可見 motion 跳動.
#
# 比對方式:
#   with_fruc/  ← 60 張: real + interp 交替顯示, 每 16.7ms 一張
#   real_only/  ← 60 張: 全 real frame (FRUC OFF, server fps 直顯示),
#                cadence 跟 server encoder 設定有關 (NVENC 不會即時改 fps)
#
# 設計考量:
# - 用 gdigrab 一致抓 client 顯示, 避免 server-side ffmpeg launch session-1 雷
# - 1s × 60fps = 60 frames, RGBA → PNG 壓縮約 1-3 MB / frame
# - %04d.png ffmpeg 內建 sequence 命名, 無需 wrapper

param(
    [Parameter(Mandatory=$true)]
    [string] $OutLabel,
    [int]    $Seconds = 1,
    [int]    $Fps     = 60,
    [int]    $X       = 0,
    [int]    $Y       = 0,
    [int]    $Width   = 1920,
    [int]    $Height  = 1080
)

$ErrorActionPreference = "Stop"
$repoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$outDir   = Join-Path $repoRoot "temp\full_frames\$OutLabel"

if (Test-Path $outDir) {
    Write-Host "[capture_full] cleaning existing $outDir" -ForegroundColor DarkYellow
    Remove-Item -Recurse -Force $outDir
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    Write-Error "ffmpeg not found in PATH. Install via 'winget install Gyan.FFmpeg'."
    exit 1
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  capture_full_frames — label: $OutLabel" -ForegroundColor Cyan
Write-Host "  region: ${Width}x${Height} @ (${X},${Y})" -ForegroundColor Cyan
Write-Host "  ${Seconds}s @ ${Fps}fps -> ${outDir}\frame_%04d.png" -ForegroundColor Cyan
Write-Host "  starting in 3 seconds — bring stream window to focus + don't move it" -ForegroundColor DarkYellow
Write-Host "============================================================" -ForegroundColor Cyan
Start-Sleep -Seconds 3

$args = @(
    "-y",
    "-f",          "gdigrab",
    "-framerate",  $Fps,
    "-t",          $Seconds,
    "-offset_x",   $X,
    "-offset_y",   $Y,
    "-video_size", "${Width}x${Height}",
    "-i",          "desktop",
    (Join-Path $outDir "frame_%04d.png")
)

$swt = [System.Diagnostics.Stopwatch]::StartNew()
& $ffmpeg @args 2>&1 | Where-Object { $_ -match "frame=|fps=|time=|size=" } | Select-Object -Last 3
$swt.Stop()

$saved = (Get-ChildItem -Path $outDir -Filter "frame_*.png").Count
$bytes = (Get-ChildItem -Path $outDir -Filter "frame_*.png" | Measure-Object -Property Length -Sum).Sum
$mb    = [math]::Round($bytes / 1MB, 1)

Write-Host ""
Write-Host "[capture_full] DONE  $($swt.Elapsed.ToString())" -ForegroundColor Green
Write-Host "[capture_full]   saved   = $saved frames ($mb MB) in $outDir" -ForegroundColor Green
Write-Host ""
Write-Host "Compare via:" -ForegroundColor DarkCyan
Write-Host "  explorer.exe `"$outDir`"" -ForegroundColor DarkGray
Write-Host "  or open frame_0001.png ... frame_0060.png in any image sequence viewer" -ForegroundColor DarkGray
