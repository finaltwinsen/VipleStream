# run_fruc_quality.ps1 — VipleStream FRUC 補幀品質端到端驗測 orchestration
#
# 一鍵跑完整流程：
#   1. SSH server 端啟動 testufo (Edge full-screen)
#   2. detach-launch VipleStream client (HW path, FRUC ON, dual-present)
#   3. 等 5s connect + 預備
#   4. ffmpeg gdigrab 320×180 sub-region capture 60s
#   5. kill VipleStream
#   6. 跑 analyze_fruc_timing.py (階段 1 — log timing analysis)
#   7. 跑 analyze_motion.py (階段 2 — UFO trajectory analysis)
#   8. 把報告路徑印出來
#
# 用法：
#   pwsh scripts\benchmark\run_fruc_quality.ps1 [-Label baseline_b1b]
#                                                [-CaptureSeconds 60]
#                                                [-Region "x,y,w,h"]
#                                                [-SkipServerSetup]
#                                                [-NoFlow]

param(
    [string] $Label = "",
    [int]    $CaptureSeconds = 60,
    [string] $Region = "800,540,320,180",  # x,y,w,h — assumes 1920×1080 fullscreen, UFO traverses
    [switch] $SkipServerSetup,
    [switch] $NoFlow,                      # skip optical flow (faster analyze)
    [string] $ServerHost = "192.168.51.226",
    [string] $StreamCodec = "H.264",
    [int]    $StreamFps = 60
)

$ErrorActionPreference = "Stop"
$repoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName

# ---- Defaults ----
if (-not $Label) {
    $Label = "fruc_" + (Get-Date -Format "yyyyMMdd_HHmmss")
}
$parts = $Region -split ","
if ($parts.Count -ne 4) {
    Write-Error "Region must be 'x,y,w,h' (got: $Region)"
    exit 1
}
$rx = [int]$parts[0]
$ry = [int]$parts[1]
$rw = [int]$parts[2]
$rh = [int]$parts[3]

$captureBin = Join-Path $repoRoot "temp\fruc_quality\capture_${Label}.bin"
$clientExe  = Join-Path $repoRoot "temp\moonlight\VipleStream.exe"

if (-not (Test-Path $clientExe)) {
    Write-Error "Client not built: $clientExe — run build_moonlight.cmd --no-bump first."
    exit 1
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  FRUC quality run — label: $Label" -ForegroundColor Cyan
Write-Host "  client: $clientExe" -ForegroundColor Cyan
Write-Host "  region: ${rw}×${rh} @ (${rx},${ry})" -ForegroundColor Cyan
Write-Host "  capture: ${CaptureSeconds}s @ ${StreamFps}fps" -ForegroundColor Cyan
Write-Host "  server: $ServerHost ($StreamCodec)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

# ---- Step 1: server-side testufo ----
if (-not $SkipServerSetup) {
    Write-Host "`n[1/7] starting testufo on server via SSH …" -ForegroundColor Yellow
    try {
        ssh -o BatchMode=yes "final@$ServerHost" 'powershell -Command "Stop-Process -Name msedge -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 800; Start-Process msedge -ArgumentList ''--start-fullscreen'',''https://www.testufo.com''"' 2>&1 | Out-Null
        Write-Host "      OK — Edge fullscreen testufo on server" -ForegroundColor Green
    } catch {
        Write-Warning "SSH testufo launch failed: $_  (continuing anyway, you may need to set up testufo manually)"
    }
    Start-Sleep -Seconds 3
} else {
    Write-Host "`n[1/7] -SkipServerSetup → assuming testufo already running on server" -ForegroundColor DarkYellow
}

# ---- Step 2: kill stale client + launch ----
Write-Host "`n[2/7] launching VipleStream client (HW path, FRUC ON, dual) …" -ForegroundColor Yellow
Stop-Process -Name VipleStream -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$clientArgs = @(
    "stream", $ServerHost, "Desktop",
    "--1080", "--fps", $StreamFps,
    "--video-codec", $StreamCodec,
    "--frame-interpolation"
)
$proc = Start-Process -FilePath $clientExe -ArgumentList $clientArgs -PassThru
Write-Host "      PID=$($proc.Id) StartTime=$($proc.StartTime)" -ForegroundColor Green

# ---- Step 3: warm-up ----
Write-Host "`n[3/7] warm-up 8s (let stream connect + first frames render) …" -ForegroundColor Yellow
Start-Sleep -Seconds 8

# ---- Step 4: capture ----
Write-Host "`n[4/7] ffmpeg gdigrab capture …" -ForegroundColor Yellow
$captureScript = Join-Path $PSScriptRoot "capture_fruc_frames.ps1"
$capArgs = @(
    "-X", $rx, "-Y", $ry,
    "-Width", $rw, "-Height", $rh,
    "-Seconds", $CaptureSeconds,
    "-Fps", $StreamFps,
    "-Output", $captureBin
)
& pwsh -ExecutionPolicy Bypass -File $captureScript @capArgs

# ---- Step 5: kill client cleanly ----
Write-Host "`n[5/7] stopping client …" -ForegroundColor Yellow
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800

# ---- Step 6: stage 1 timing analysis ----
Write-Host "`n[6/7] analyzing log frame-timing (Stage 1) …" -ForegroundColor Yellow
$timingScript = Join-Path $PSScriptRoot "analyze_fruc_timing.py"
& python $timingScript --latest --label $Label

# ---- Step 7: stage 2 motion analysis ----
Write-Host "`n[7/7] analyzing UFO motion (Stage 2) …" -ForegroundColor Yellow
$motionScript = Join-Path $PSScriptRoot "analyze_motion.py"
$motionArgs = @($captureBin, "--label", $Label)
if ($NoFlow) {
    $motionArgs += "--no-flow"
}
& python $motionScript @motionArgs

# ---- Summary ----
$outDir = Join-Path $repoRoot "temp\fruc_quality"
Write-Host "`n============================================================" -ForegroundColor Cyan
Write-Host "  DONE — reports in $outDir" -ForegroundColor Cyan
Write-Host "  - fruc_quality_${Label}.md / .png   (frame timing)" -ForegroundColor Cyan
Write-Host "  - motion_${Label}.md / .png / .json (UFO motion)" -ForegroundColor Cyan
Write-Host "  - capture_${Label}.bin              (raw RGBA, $($CaptureSeconds * $StreamFps * $rw * $rh * 4 / 1MB | ForEach-Object {[int]$_}) MB — delete to save disk)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
