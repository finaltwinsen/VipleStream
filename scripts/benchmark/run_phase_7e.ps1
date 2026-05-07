# run_phase_7e.ps1 — §B-NVOF Phase 7E TRIPLE × NVOF cross-test orchestrator
#
# 跑 4 個 config 各 60s，每個 config 之間 cool-down 30s，共 ~7-8 分鐘:
#
#   A: dual_bm     — DUAL  60→120 + software block-match ME (production baseline)
#   B: dual_nvof   — DUAL  60→120 + NVOF HW optical flow      (Phase 7CD-C 已知)
#   C: triple_bm   — TRIPLE 60→180 + software block-match ME  (TRIPLE control)
#   D: triple_nvof — TRIPLE 60→180 + NVOF HW                   (Phase 7E 真正目標)
#
# 比較重點:
#   - C vs A: TRIPLE 對 software ME 的影響 (預期: ME 噪聲在 TRIPLE 放大、品質差於 A)
#   - D vs B: TRIPLE 對 NVOF 的影響 (預期: NVOF 平滑性 hold 住 TRIPLE 不爛)
#   - D vs A: cross-test 命題 — 「TRIPLE×NVOF 是否值得 ship 到 production?」
#
# 前置條件 (使用者要做):
#   1. Server 端 testufo / motion source 全螢幕 + 在 trajectory band y=510±20
#   2. 既有 VipleStream session 全部關掉 (Stop-Process 不會自動處理 — 怕踩
#      到使用者的串流)
#   3. Client 端面板 144Hz+ 比較有意義 (TRIPLE 在 60Hz panel 上會 dropped)
#   4. 跑前 GPU cool-down >= 5 分鐘 (避免 thermal throttle 噪聲蓋過 NVOF
#      cv 0.86 vs 1.94 的差異)
#
# 用法:
#   pwsh scripts\benchmark\run_phase_7e.ps1
#   pwsh scripts\benchmark\run_phase_7e.ps1 -CaptureSeconds 30  # 加速試跑
#   pwsh scripts\benchmark\run_phase_7e.ps1 -OnlyConfigs C,D    # 只跑 TRIPLE 兩條

param(
    [int]      $CaptureSeconds = 60,
    [int]      $CooldownSeconds = 30,
    [string]   $StreamCodec = "H.264",
    [string]   $ServerHost  = "192.168.51.226",
    [string]   $Region = "0,510,1920,40",     # full-width 40px band on UFO trajectory
    [ValidateSet("video", "ufo")]
    [string]   $Mode = "ufo",                  # Phase 7E 量化基準是 testufo trajectory
    [string[]] $OnlyConfigs = @("A","B","C","D"),
    [string]   $LabelPrefix = "",
    # Server fps (NVENC encode rate).  4 configs 都用同 server fps 來保證
    # source motion sampling 一致；client display fps 由 mode 決定:
    #   DUAL  → ServerFps * 2  (預設 30→60)
    #   TRIPLE→ ServerFps * 3  (預設 30→90)
    # 30 fps source 對 FRUC 是邊界 stress：motion gap 大、interp 工作更辛苦.
    # 跟 Phase 7CD 量過的 testufo 60fps 比較會有偏差，但同次 run 內 4 個
    # config 互相對比仍 valid (相對指標、不是絕對).
    [int]      $ServerFps = 30
)

$ErrorActionPreference = "Stop"
$repoRoot   = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$qualScript = Join-Path $PSScriptRoot "run_fruc_quality.ps1"
if (-not (Test-Path $qualScript)) {
    Write-Error "run_fruc_quality.ps1 missing"; exit 1
}

if (-not $LabelPrefix) {
    $LabelPrefix = "phase7e_" + (Get-Date -Format "yyyyMMdd_HHmmss")
}

# Config table.  Client display fps 由 ServerFps × mode-multiplier 算出
# (DUAL ×2, TRIPLE ×3)，每 config 顯式 -StreamFps 傳給 run_fruc_quality.ps1
# 避免它的 -Triple 自動升 180 蓋掉.
$dualClient   = $ServerFps * 2
$tripleClient = $ServerFps * 3
$configs = @(
    @{ key = "A"; label = "dual_bm";     nvof = $false; triple = $false; clientFps = $dualClient;
       desc = "DUAL  ${ServerFps}->${dualClient} + software block-match (baseline)" },
    @{ key = "B"; label = "dual_nvof";   nvof = $true;  triple = $false; clientFps = $dualClient;
       desc = "DUAL  ${ServerFps}->${dualClient} + NVOF HW" },
    @{ key = "C"; label = "triple_bm";   nvof = $false; triple = $true;  clientFps = $tripleClient;
       desc = "TRIPLE ${ServerFps}->${tripleClient} + software block-match (TRIPLE control)" },
    @{ key = "D"; label = "triple_nvof"; nvof = $true;  triple = $true;  clientFps = $tripleClient;
       desc = "TRIPLE ${ServerFps}->${tripleClient} + NVOF HW (Phase 7E target)" }
)
$configs = @($configs | Where-Object { $OnlyConfigs -contains $_.key })

if ($configs.Count -eq 0) {
    Write-Error "No configs match -OnlyConfigs $($OnlyConfigs -join ',')"
    exit 1
}

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  §B-NVOF Phase 7E cross-test" -ForegroundColor Cyan
Write-Host "  configs : $(($configs | ForEach-Object {$_.key + '=' + $_.label}) -join ', ')" -ForegroundColor Cyan
Write-Host "  capture : ${CaptureSeconds}s each, cooldown ${CooldownSeconds}s between" -ForegroundColor Cyan
$totalSec = ($configs.Count * ($CaptureSeconds + 8 + 5)) + ($configs.Count - 1) * $CooldownSeconds
Write-Host "  ETA     : ~$([math]::Ceiling($totalSec / 60)) min total" -ForegroundColor Cyan
Write-Host "  prefix  : $LabelPrefix" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# Pre-flight checks
Write-Host "`nPre-flight:" -ForegroundColor Yellow
$running = Get-Process VipleStream -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "  ⚠ VipleStream already running (PID(s): $(($running.Id) -join ',')). Phase 7E 會 Stop-Process 它們." -ForegroundColor Yellow
    Write-Host "    Ctrl+C abort if 你正在串流，否則 5s 後繼續..." -ForegroundColor Yellow
    Start-Sleep -Seconds 5
}
$exe = Join-Path $repoRoot "temp\moonlight\VipleStream.exe"
if (-not (Test-Path $exe)) {
    Write-Error "Client not built: $exe"; exit 1
}
Write-Host "  client: $exe" -ForegroundColor DarkGray
Write-Host "  motion: 確認 testufo 在 server fullscreen 跑 (mode=$Mode)" -ForegroundColor DarkGray

$results = @()
$startTime = Get-Date

for ($i = 0; $i -lt $configs.Count; $i++) {
    $cfg = $configs[$i]
    $label = "${LabelPrefix}_$($cfg.label)"

    Write-Host ""
    Write-Host "----------------------------------------------------------" -ForegroundColor Cyan
    Write-Host "  [$($i+1)/$($configs.Count)] config $($cfg.key): $($cfg.label)" -ForegroundColor Cyan
    Write-Host "                  $($cfg.desc)" -ForegroundColor DarkGray
    Write-Host "                  label = $label" -ForegroundColor DarkGray
    Write-Host "----------------------------------------------------------" -ForegroundColor Cyan

    $qualArgs = @{
        Label           = $label
        CaptureSeconds  = $CaptureSeconds
        Region          = $Region
        Mode            = $Mode
        ServerHost      = $ServerHost
        StreamCodec     = $StreamCodec
        SkipServerSetup = $true
        StreamFps       = $cfg.clientFps  # ServerFps × (DUAL=2 / TRIPLE=3)
    }
    if ($cfg.nvof)   { $qualArgs.NvOf   = $true }
    if ($cfg.triple) { $qualArgs.Triple = $true }

    try {
        & pwsh -ExecutionPolicy Bypass -File $qualScript @qualArgs
        $rc = $LASTEXITCODE
        if ($rc -eq 0) {
            $results += [pscustomobject]@{ config = $cfg.key; label = $cfg.label; status = "OK"; bin = "temp\fruc_quality\capture_${label}.bin" }
            Write-Host "  ✅ $($cfg.label) DONE" -ForegroundColor Green
        } else {
            $results += [pscustomobject]@{ config = $cfg.key; label = $cfg.label; status = "FAIL rc=$rc"; bin = "" }
            Write-Host "  ❌ $($cfg.label) FAILED rc=$rc" -ForegroundColor Red
        }
    } catch {
        $results += [pscustomobject]@{ config = $cfg.key; label = $cfg.label; status = "ERROR: $_"; bin = "" }
        Write-Host "  ❌ $($cfg.label) EXCEPTION: $_" -ForegroundColor Red
    }

    # Make sure no zombie before next config
    Get-Process VipleStream -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    # Cool-down between runs to avoid thermal effects skewing perf samples
    if ($i -lt $configs.Count - 1) {
        Write-Host "  cooldown ${CooldownSeconds}s (GPU thermal settle)..." -ForegroundColor DarkGray
        Start-Sleep -Seconds $CooldownSeconds
    }
}

$endTime = Get-Date
$elapsed = ($endTime - $startTime).TotalMinutes

Write-Host ""
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Phase 7E cross-test summary  ($([math]::Round($elapsed,1)) min)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
$results | Format-Table -AutoSize

Write-Host ""
Write-Host "Reports + raw captures in temp\fruc_quality\:" -ForegroundColor Cyan
foreach ($r in $results | Where-Object { $_.status -eq 'OK' }) {
    $md = Join-Path $repoRoot "temp\fruc_quality\motion_$($LabelPrefix)_$($r.label).md"
    if (Test-Path $md) {
        Write-Host "  - $md" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "Side-by-side metric comparison (manual):" -ForegroundColor Cyan
Write-Host "  Compare ssim_mean / OF_30Hz / OF_cv / block_outlier between configs." -ForegroundColor DarkGray
Write-Host "  Phase 7E pass criterion: D (triple_nvof) cv ≤ 1.0  AND  OF_30Hz ≤ 4%" -ForegroundColor DarkGray
Write-Host "    (ie. NVOF holds smoothness in TRIPLE; ME 噪聲沒被 TRIPLE 放大)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "Done." -ForegroundColor Green
