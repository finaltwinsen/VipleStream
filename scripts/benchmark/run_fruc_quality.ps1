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
    [string] $Region = "0,510,1920,40",    # x,y,w,h — full-width 40px band on UFO trajectory (1080p y=510±20)
                                            # central 320×180 noise too high vs sub-second UFO sweep phase
    [ValidateSet("video", "ufo")]
    [string] $Mode = "video",              # video: PotPlayer auto-launch; ufo: testufo trajectory
    [string] $VideoPath = "C:\Temp\videoplayback.webm",  # video mode: server-side video file
    [switch] $SkipServerSetup,             # bypass auto server-side launch (use existing source)
    [switch] $NoFlow,                      # ufo mode only — skip OF magnitude (faster analyze)
    [string] $ServerHost = "192.168.51.226",
    [string] $StreamCodec = "H.264",
    [int]    $StreamFps = 60,
    # §B-NVOF / §B2 cross-test (Phase 7E) — opt-in flags propagated to client
    # via the new --vk-nvof / --vk-triple CLI args (UI 整合 commit 2026-05-07).
    # `-Triple` 自動把 StreamFps 升到 180 (TRIPLE 模式 client display target).
    # 兩個都加 = TRIPLE × NVOF (Phase 7E 真正想量的 cross-test).
    [switch] $NvOf,
    [switch] $Triple
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

# §B2: TRIPLE 模式 client display target = 180fps; server fps 由 client 端
# 算 user_fps/3.  其他模式維持 60fps (DUAL → server 30fps).  使用者可顯式
# 指定 -StreamFps override.
if ($Triple -and $PSBoundParameters.ContainsKey('StreamFps') -eq $false) {
    $StreamFps = 180
}

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

# ---- Step 1: server-side motion source ----
# Auto-launching GUI apps (PotPlayer / Edge) into a different Windows
# session via SSH proved unreliable: even Sysinternals PsExec -i 1 reports
# success but the streaming session sometimes captures the wrong session.
# Just trust the user to set up the motion source manually on server,
# and assume it's ready before this script runs.
#
# Acceptable sources:
#   - testufo.com fullscreen in Chrome/Edge (best for synthetic motion)
#   - PotPlayer / VLC fullscreen with a video file looping
#   - any animation that fills 1080p screen
if ($Mode -eq "video") {
    Write-Host "`n[1/7] video mode — using user-managed motion source on server" -ForegroundColor DarkYellow
    Write-Host "       (make sure your video / testufo / motion source is fullscreen + playing)" -ForegroundColor DarkGray
} else {
    Write-Host "`n[1/7] ufo mode — using user-managed testufo source on server" -ForegroundColor DarkYellow
    Write-Host "       (open https://www.testufo.com fullscreen in browser before running)" -ForegroundColor DarkGray
}

# ---- Step 2: kill stale client + launch ----
Write-Host "`n[2/7] launching VipleStream client (HW path, FRUC ON, dual) …" -ForegroundColor Yellow
Stop-Process -Name VipleStream -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
$clientArgs = @(
    "stream", $ServerHost, "Desktop",
    "--1080", "--fps", $StreamFps,
    "--video-codec", $StreamCodec,
    "--video-decoder", "hardware",   # 2026-05-07 Phase 7E debug: 沒 force HW
                                      # AUTO cascade 會挑 SW H.264 + SDL renderer，
                                      # SDL renderer **不支援 FRUC** → log 出
                                      # "isFRUCActive=0 backendName=None"，整個
                                      # 量測等於跑 raw 30fps stream，不是補幀.
    "--display-mode", "fullscreen",  # explicit fullscreen — 沒指定就吃使用者
                                      # QSettings 預設，可能是 windowed 小視窗，
                                      # gdigrab 在固定 (0,510,1920,40) 抓不到串流內容.
    "--renderer", "vulkan",          # §J.3.e.2.i — Phase 7E NVOF/TRIPLE 只在
                                      # Vulkan path 生效；user 預設 RS_D3D11 會
                                      # 讓 vk-nvof/vk-triple flag 完全沒用.
    "--frame-interpolation",
    "--no-yuv444"   # 強制 4:2:0 — Vulkan video decode 不支援 4:4:4，
                     # 否則 cascade 會 fall back 到 PlVkRenderer (libplacebo) 沒接 FRUC，
                     # 導致 quality 跑出來其實是「無 FRUC」狀態。2026-05-06 踩雷。
)
# §B-NVOF / §B2 — Phase 7E cross-test 用的 vk-only 進階 flag.  RS_D3D11 path
# 完全 ignore，所以為 Vulkan 量測 dedicated.  這兩條跟既有的 Settings UI
# checkbox 等價，只是腳本裡用 CLI 形式覆蓋 user 既有設定 (一次 run，不
# 動 QSettings 永久值).
if ($NvOf)   { $clientArgs += "--vk-nvof"  } else { $clientArgs += "--no-vk-nvof"  }
if ($Triple) { $clientArgs += "--vk-triple" } else { $clientArgs += "--no-vk-triple" }
$proc = Start-Process -FilePath $clientExe -ArgumentList $clientArgs -PassThru
Write-Host "      PID=$($proc.Id) StartTime=$($proc.StartTime) (renderer=vulkan video-decoder=hardware vk-nvof=$NvOf vk-triple=$Triple)" -ForegroundColor Green

# ---- Step 3: warm-up ----
Write-Host "`n[3/7] warm-up 8s (let stream connect + first frames render) …" -ForegroundColor Yellow
Start-Sleep -Seconds 8

# ---- Step 3b: FRUC sanity gate ----
# 2026-05-07 Phase 7E debug: 之前一次 run 全 4 個 config 都跑完才發現 client
# 因為 saved RS_D3D11 + AUTO decode + windowed default 結果走 SDL fallback
# renderer (沒 FRUC)、又被本機桌面遮住 — 4×60s 量測完全廢.
# 解決：warmup 後直接 grep 最新 VipleStream log 的 stringifyVideoStats 行，
# 確認 isFRUCActive=1 + backendName ≠ None.  否則直接 abort 不浪費 60s.
$logDir = "$env:TEMP"
$latestLog = Get-ChildItem $logDir -Filter "VipleStream-*.log" -ErrorAction SilentlyContinue |
             Where-Object { $_.LastWriteTime -gt $proc.StartTime } |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestLog) {
    $stats = Select-String -Path $latestLog.FullName -Pattern 'isFRUCActive=(\d).+?backendName=(\w+).+?renderedFrames=(\d+)' |
             Select-Object -Last 1
    if ($stats) {
        $active = $stats.Matches[0].Groups[1].Value
        $backend = $stats.Matches[0].Groups[2].Value
        $rendered = $stats.Matches[0].Groups[3].Value
        Write-Host "  FRUC sanity: isFRUCActive=$active backendName=$backend renderedFrames=$rendered  (log=$($latestLog.Name))" -ForegroundColor DarkGray
        if ($active -ne '1' -or $backend -eq 'None') {
            Write-Host "  ❌ FRUC NOT ACTIVE — aborting capture (sample would be useless)" -ForegroundColor Red
            Write-Host "     check log $($latestLog.FullName) — likely renderer cascade fell to SDL fallback" -ForegroundColor Red
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            exit 2
        }
    } else {
        Write-Host "  ⚠ couldn't parse stringifyVideoStats from log $($latestLog.Name) yet — proceeding (may be too early)" -ForegroundColor Yellow
    }
} else {
    Write-Host "  ⚠ no VipleStream log found newer than client launch — proceeding blind" -ForegroundColor Yellow
}

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
Write-Host "`n[7/7] analyzing motion (Stage 2, mode=$Mode) …" -ForegroundColor Yellow
$motionScript = Join-Path $PSScriptRoot "analyze_motion.py"
$motionArgs = @($captureBin, "--label", $Label, "--mode", $Mode)
if ($NoFlow -and $Mode -eq "ufo") {
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
