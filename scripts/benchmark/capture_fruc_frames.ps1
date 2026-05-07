# capture_fruc_frames.ps1 — VipleStream FRUC 補幀效果驗測（階段 2 capture）
#
# 用 ffmpeg gdigrab 抓 client 端螢幕的 320×180 sub-region 60 秒，存 raw RGBA。
# 配合 analyze_motion.py 做 optical flow + UFO trajectory R² 分析。
#
# 用法：
#   pwsh scripts\benchmark\capture_fruc_frames.ps1 [-X 800] [-Y 540] [-Width 320] [-Height 180] [-Seconds 60] [-Output temp\fruc_capture\test.bin]
#
# 預設 (1920×1080 fullscreen client)：
#   X=800 Y=540  →  client 中央偏左下，testufo UFO 軌跡通常經過此處
#   320×180 = sub-region 大小，剛好 1 個 UFO + 一些空間
#
# 注意：
#   - client window 必須在 fullscreen 或固定位置（不要動視窗）
#   - testufo 必須在 server 端正在跑 (UFO test pattern)
#   - capture overhead 0.5-1ms / frame (gdigrab 在 RTX 3060 約 200us)
#   - output 60s × 60fps × 320 × 180 × 4 bytes = 1.32 GB raw RGBA

param(
    [int]    $X       = 800,
    [int]    $Y       = 540,
    [int]    $Width   = 320,
    [int]    $Height  = 180,
    [int]    $Seconds = 60,
    [int]    $Fps     = 60,
    [string] $Output  = ""
)

$ErrorActionPreference = "Stop"

# ---- Resolve ffmpeg path ----
$ffmpegPaths = @(
    "ffmpeg.exe",  # PATH first
    "C:\Users\final\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe"
)
$ffmpeg = $null
foreach ($p in $ffmpegPaths) {
    if (Get-Command $p -ErrorAction SilentlyContinue) {
        $ffmpeg = (Get-Command $p).Source
        break
    }
    if (Test-Path $p) {
        $ffmpeg = $p
        break
    }
}
if (-not $ffmpeg) {
    Write-Error "ffmpeg.exe not found.  Install via 'winget install ffmpeg' or add to PATH."
    exit 1
}
Write-Host "[capture] ffmpeg: $ffmpeg" -ForegroundColor Cyan

# ---- Default output path ----
if (-not $Output) {
    $repo = Split-Path -Parent $PSScriptRoot
    $repo = Split-Path -Parent $repo  # repo root
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $Output = Join-Path $repo "temp\fruc_quality\capture_${stamp}.bin"
}
$outDir = Split-Path -Parent $Output
if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

# ---- Sidecar metadata file (analyze_motion.py reads this) ----
$meta = @{
    width   = $Width
    height  = $Height
    fps     = $Fps
    seconds = $Seconds
    x       = $X
    y       = $Y
    pixfmt  = "rgba"
    bytes_per_frame = $Width * $Height * 4
    expected_frames = $Fps * $Seconds
    captured_at = (Get-Date -Format "o")
}
$metaPath = $Output -replace "\.bin$", ".json"
$meta | ConvertTo-Json | Out-File -FilePath $metaPath -Encoding utf8

Write-Host "[capture] region: ${Width}×${Height} @ (${X},${Y})" -ForegroundColor Cyan
Write-Host "[capture] duration: ${Seconds}s @ ${Fps}fps  (≈ $($Width*$Height*4*$Fps*$Seconds / 1MB | ForEach-Object {[int]$_}) MB raw)" -ForegroundColor Cyan
Write-Host "[capture] output: $Output" -ForegroundColor Cyan
Write-Host "[capture] meta: $metaPath" -ForegroundColor Cyan
Write-Host "[capture] starting ffmpeg gdigrab in 3 seconds — bring stream window to focus + don't move it"
Start-Sleep -Seconds 3

# ---- Run ffmpeg gdigrab ----
# -f gdigrab        : Windows GDI screen capture
# -framerate $Fps   : INPUT-side capture rate request (gdigrab tries to deliver)
# -offset_x/y       : top-left of sub-region
# -video_size       : capture region size
# -i desktop        : input = whole desktop, sub-rect via offset+size
# -t $Seconds       : stop after N seconds (OUTPUT-side, PTS-driven)
# -r $Fps           : OUTPUT-side rate enforcement (drops/dups frames to match)
# -frames:v $count  : HARD frame-count cap (independent of PTS / wall clock)
# -c:v rawvideo     : no encoding, raw frames
# -pix_fmt rgba     : 4 bytes/pixel, easy to numpy reshape
# -y                : overwrite output
#
# 2026-05-07 §B-NVOF Phase 7E debug: at -framerate 90 on a 179Hz VDD client
# display, gdigrab ignored -t and ran ~607 fps for 5+ minutes (188K frames /
# 55 GB before cooldown kicked in).  Suspected reason: rawvideo output
# muxer has no PTS so OUTPUT -t doesn't enforce; gdigrab + BitBlt at
# small (1920×40) size can iterate much faster than -framerate request,
# and ffmpeg does not throttle without -r enforcement.  Added two extra
# safeguards: -frames:v as a hard count cap, and a wall-clock kill timeout
# in the PowerShell wrapper below in case ffmpeg still runs away.
$expectedFrames = $Fps * $Seconds
$ffmpegArgs = @(
    "-f", "gdigrab",
    "-framerate", $Fps,
    "-offset_x", $X,
    "-offset_y", $Y,
    "-video_size", "${Width}x${Height}",
    "-i", "desktop",
    "-r", $Fps,                       # force output rate = input target
    "-t", $Seconds,                   # output wall-clock duration
    "-frames:v", $expectedFrames,     # HARD frame-count cap (failsafe)
    "-c:v", "rawvideo",
    "-pix_fmt", "rgba",
    "-f", "rawvideo",                 # explicit output muxer (.bin ext unrecognized)
    "-y",
    $Output
)

# Wall-clock kill: if ffmpeg somehow still doesn't exit within 1.5x the
# requested duration, force terminate.  Otherwise a runaway like the one
# above (5 min instead of 60s) can fill disk + skew benchmarks.
$killAfterSec = [int]([math]::Ceiling($Seconds * 1.5)) + 10
Write-Host "[capture] safeguards: -t $Seconds  -frames:v $expectedFrames  wallclock-kill ${killAfterSec}s" -ForegroundColor DarkGray

$elapsed = Measure-Command {
    $ffProc = Start-Process -FilePath $ffmpeg -ArgumentList $ffmpegArgs `
                            -PassThru -NoNewWindow `
                            -RedirectStandardOutput "${Output}.ffstdout.log" `
                            -RedirectStandardError  "${Output}.ffstderr.log"
    $exited = $ffProc.WaitForExit($killAfterSec * 1000)
    if (-not $exited) {
        Write-Host "[capture] WARNING ffmpeg exceeded ${killAfterSec}s wall-clock — killing pid=$($ffProc.Id)" -ForegroundColor Yellow
        Stop-Process -Id $ffProc.Id -Force -ErrorAction SilentlyContinue
        $ffProc.WaitForExit(5000) | Out-Null
    }
    $ffmpegOutput = (Get-Content "${Output}.ffstderr.log" -ErrorAction SilentlyContinue) -join "`n"
}

# ---- Verify output ----
if (-not (Test-Path $Output)) {
    Write-Error "ffmpeg did not produce $Output.  ffmpeg output:"
    $ffmpegOutput | Write-Host
    exit 1
}
$size = (Get-Item $Output).Length
$expectedSize = $Width * $Height * 4 * $Fps * $Seconds
$frameCount = $size / ($Width * $Height * 4)
Write-Host "" -ForegroundColor Green
Write-Host "[capture] DONE  ${elapsed}" -ForegroundColor Green
Write-Host "[capture]   size      = $($size / 1MB | ForEach-Object {'{0:F1}' -f $_}) MB" -ForegroundColor Green
Write-Host "[capture]   frames    = $frameCount  (expected $($Fps * $Seconds))" -ForegroundColor Green
Write-Host "[capture]   coverage  = $(($frameCount / ($Fps * $Seconds) * 100) | ForEach-Object {'{0:F1}' -f $_})%" -ForegroundColor Green
Write-Host ""
Write-Host "Next:  python scripts/benchmark/analyze_motion.py `"$Output`""
