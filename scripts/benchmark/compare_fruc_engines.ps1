# compare_fruc_engines.ps1 - 對所有 FRUC engine 截圖比對
#
# 對 8 種 engine 配置依序（或用 -Engine filter 跑單一 / 子集）：
#   1. 設 registry / CLI args 切換 engine
#   2. 啟動 VipleStream.exe stream（fullscreen）
#   3. 等 warmup
#   4. §B-DUMP path（VkFruc 寫 all/+real/ 子目錄；D3D11VA 寫 flat
#      frame_NNNN_real/_interp.bmp），長度 = $Fps * $Seconds 張
#   5. 殺 stream，存到 <OutRoot>\<engine_name>\
#   6. 同時存 launch.log / launch.log.err
#
# 用法（host 上要先開遊戲，之後維持遊戲在跑）:
#   # 跑全部 7 種 engine
#   pwsh scripts\benchmark\compare_fruc_engines.ps1
#
#   # 只跑 NVOF Vulkan 路徑，三種 grid (1/2/4) all medium perfLevel
#   pwsh scripts\benchmark\compare_fruc_engines.ps1 -Engine 06_vkfruc_nvof -NvOfGrid 1
#   pwsh scripts\benchmark\compare_fruc_engines.ps1 -Engine 06_vkfruc_nvof -NvOfGrid 2
#   pwsh scripts\benchmark\compare_fruc_engines.ps1 -Engine 06_vkfruc_nvof -NvOfGrid 4
#
#   # 只跑 VkFruc 兩條 path 對比（block-match vs NVOF）
#   pwsh scripts\benchmark\compare_fruc_engines.ps1 -Engine 01_vkfruc_bm,06_vkfruc_nvof
#
#   # 只跑 D3D11 backend 全部四種
#   pwsh scripts\benchmark\compare_fruc_engines.ps1 -Engine "0[2345]_d3d11_*"
#
# 結果在 D:\temp\fruc_compare\<engine_name>\
# 用任何 image viewer 順序播放即可肉眼比較流暢度

param(
    [int]      $Seconds    = 1,
    [int]      $Fps        = 60,
    [int]      $WarmupSec  = 10,
    [string]   $StreamHost = "192.168.51.226",
    [string]   $App        = "Desktop",
    [string]   $OutRoot    = "D:\temp\fruc_compare",
    [int]      $Width      = 1920,
    [int]      $Height     = 1080,
    # Filter to a subset of engines.  Wildcards (-like) allowed:
    #   -Engine 06_vkfruc_nvof
    #   -Engine "*nvof*"
    #   -Engine 01_vkfruc_bm,06_vkfruc_nvof
    # 預設空 = 跑全部.
    [string[]] $Engine     = @(),
    # NVOF Vulkan tuning (僅作用於 06_vkfruc_nvof).  與 §B-NVOF Phase 7C+7D
    # 的 VIPLE_VKFRUC_NV_OF_PERF / _GRID env var 一一對應.  Grid 2 + medium
    # 是 testufo 1080p60 量到的甜蜜點 (commit 71d1592)，要 cross-test
    # 其他 config 改這兩個 param.
    [ValidateSet("slow","medium","fast")]
    [string]   $NvOfPerf   = "medium",
    [ValidateSet("1","2","4")]
    [string]   $NvOfGrid   = "2"
)

$ErrorActionPreference = "Stop"

# 7 種 engine config (registry path: HKCU:\SOFTWARE\VipleStream\VipleStream):
#   rendererSelection: 0=RS_VULKAN, 1=RS_D3D11
#   frucBackend:       0=GENERIC, 1=NVIDIA_OF, 2=DIRECTML, 3=NCNN
#   frameInterpolation: "true"/"false" (Qt String; lowercase!)
#   videoDecoderSelection: 0=AUTO, 1=FORCE_SW, 2=FORCE_HW (force HW for D3D11VA path)
#   nvof: $true → set VIPLE_VKFRUC_NV_OF=1 (only for VkFruc renderer; HW
#         optical flow via VK_NV_optical_flow extension replaces software
#         block-match ME).  See §B-NVOF section in docs/TODO.md.
#
# 03_d3d11_nvidia_of 跟 06_vkfruc_nvof 是兩條完全不同的 NVIDIA Optical
# Flow 路徑：
#   - 03 = D3D11VARenderer + NvOFFRUC.dll wrapper（封閉的 SDK，整段 FRUC
#     pipeline 由 NV 提供，吃 D3D11 texture）
#   - 06 = VkFrucRenderer + VK_NV_optical_flow extension（我們自己寫的
#     compute pipeline 接 NVOFA 出的 SFIXED5 flow image，warp shader 跟
#     §B-quality 的 blend mode 都是我們的）
$engines = @(
    @{ name = "00_baseline_no_fruc";   rendererSel = 1; fi = "false"; backend = 0; vds = 2 },
    @{ name = "01_vkfruc_bm";          rendererSel = 0; fi = "true";  backend = 0; vds = 0 },
    @{ name = "02_d3d11_generic";      rendererSel = 1; fi = "true";  backend = 0; vds = 2 },
    @{ name = "03_d3d11_nvidia_of";    rendererSel = 1; fi = "true";  backend = 1; vds = 2 },
    @{ name = "04_d3d11_directml";     rendererSel = 1; fi = "true";  backend = 2; vds = 2 },
    @{ name = "05_d3d11_ncnn";         rendererSel = 1; fi = "true";  backend = 3; vds = 2 },
    @{ name = "06_vkfruc_nvof";        rendererSel = 0; fi = "true";  backend = 0; vds = 0; nvof = $true },
    # §J.3.e.X Path β (commits 67a7c89 β.1+β.2 / 20507da β.4) —— RIFE
    # native Vulkan inference 接到 VkFrucRenderer 既有 VkInstance/VkDevice.
    # 預設 inferDim 256×128 (其他 dim 撞 model 'Add_503' 約束).
    # bilinear up 7.5× 損失約 4 MAE intensity；RIFE 推論本身有效 midpoint，
    # 殘留主要來自 down/up roundtrip.  待 inferShapes 修好 解鎖 ≥ 384.
    @{ name = "07_vkfruc_native_rife"; rendererSel = 0; fi = "true";  backend = 0; vds = 0; nativeRife = $true }
)

# Apply -Engine filter (wildcards via -like).  Empty = run all.
if ($Engine.Count -gt 0) {
    $allNames = @($engines | ForEach-Object { $_.name })
    $engines  = @($engines | Where-Object {
        $n = $_.name
        $hit = $false
        foreach ($pat in $Engine) {
            if ($n -like $pat) { $hit = $true; break }
        }
        $hit
    })
    if ($engines.Count -eq 0) {
        Write-Error ("No engines matched filter: {0}`nAvailable: {1}" -f ($Engine -join ', '), ($allNames -join ', '))
        exit 1
    }
    $selectedNames = @($engines | ForEach-Object { $_.name }) -join ', '
    Write-Host ("Filter -Engine {0} → {1} engine(s) selected: {2}" -f ($Engine -join ','), $engines.Count, $selectedNames) -ForegroundColor Cyan
}

# Path setup
$repoRoot = (Get-Item $PSScriptRoot).Parent.Parent.FullName
$exePath  = Join-Path $repoRoot "temp\moonlight\VipleStream.exe"
if (-not (Test-Path $exePath)) {
    Write-Error "VipleStream.exe not found at $exePath - run build_moonlight.cmd first"
    exit 1
}

$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    Write-Error "ffmpeg not found in PATH. Install via 'winget install Gyan.FFmpeg'."
    exit 1
}

$regKey = "HKCU:\SOFTWARE\VipleStream\VipleStream"
$origRendererSel = (Get-ItemProperty -Path $regKey -Name rendererSelection -ErrorAction SilentlyContinue).rendererSelection
$origFrucBackend = (Get-ItemProperty -Path $regKey -Name frucBackend -ErrorAction SilentlyContinue).frucBackend
$origFrameInterp = (Get-ItemProperty -Path $regKey -Name frameInterpolation -ErrorAction SilentlyContinue).frameInterpolation
$origVds         = (Get-ItemProperty -Path $regKey -Name videoDecoderSelection -ErrorAction SilentlyContinue).videoDecoderSelection
Write-Host "Original registry: rendererSelection=$origRendererSel  frucBackend=$origFrucBackend  frameInterpolation=$origFrameInterp  videoDecoderSelection=$origVds" -ForegroundColor DarkYellow

# Cleanup output
if (Test-Path $OutRoot) { Remove-Item -Recurse -Force $OutRoot }
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null

function Kill-VipleStream {
    Get-CimInstance Win32_Process -Filter "Name='VipleStream.exe'" -ErrorAction SilentlyContinue | ForEach-Object {
        Invoke-CimMethod -InputObject $_ -MethodName Terminate -ErrorAction SilentlyContinue | Out-Null
    }
    Start-Sleep -Seconds 2
}

Write-Host "`n=========================================================" -ForegroundColor Cyan
Write-Host "  FRUC engine comparison: $($engines.Count) configs" -ForegroundColor Cyan
Write-Host "  warmup=${WarmupSec}s  capture=${Seconds}s @ ${Fps}fps" -ForegroundColor Cyan
Write-Host "  output: $OutRoot" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan

$summary = @()

try {
    foreach ($eng in $engines) {
        $outDir = Join-Path $OutRoot $eng.name
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null

        Write-Host "`n=== [$($eng.name)] ===" -ForegroundColor Cyan
        Write-Host "  rendererSelection=$($eng.rendererSel) frame-interp=$($eng.fi) fruc-backend=$($eng.backend)" -ForegroundColor DarkGray

        # 1. Set ALL relevant registry values (Qt QSettings reads from
        #    HKCU:\SOFTWARE\VipleStream\VipleStream).  CLI args override
        #    isn't reliable for the renderer cascade; force everything via
        #    registry.
        Set-ItemProperty -Path $regKey -Name rendererSelection      -Value $eng.rendererSel -Type DWord
        Set-ItemProperty -Path $regKey -Name frucBackend            -Value $eng.backend     -Type DWord
        Set-ItemProperty -Path $regKey -Name frameInterpolation     -Value $eng.fi          -Type String
        Set-ItemProperty -Path $regKey -Name videoDecoderSelection  -Value $eng.vds         -Type DWord

        # §B-DUMP-BYPASS: for ML backends (DirectML / NCNN), force-enable
        # even if probe exceeds frame budget.  RTX 3060 Laptop can't hit
        # 30fps × 0.85 budget for full RIFE @ 1080p, but for diagnostic
        # comparison we want to SEE the output regardless of perf.
        if ($eng.backend -ge 2) {
            $env:VIPLE_FRUC_BYPASS_BUDGET = "1"
        } else {
            Remove-Item env:VIPLE_FRUC_BYPASS_BUDGET -ErrorAction SilentlyContinue
        }

        # §B-NVOF: 06_vkfruc_nvof opts into VK_NV_optical_flow path inside
        # VkFrucRenderer.  Stage 1 (block-match ME) + Stage 2 (median) are
        # replaced by NVOFA HW flow → SFIXED5→Q1 converter; warp / blend
        # / dual-present 不變.  PERF / GRID env var 對應 commit 71d1592
        # 的 7C+7D 調參，預設 grid=2 perf=med 在 testufo 1080p60 是甜蜜點.
        if ($eng.nvof) {
            $env:VIPLE_VKFRUC_NV_OF      = "1"
            $env:VIPLE_VKFRUC_NV_OF_PERF = $NvOfPerf
            $env:VIPLE_VKFRUC_NV_OF_GRID = $NvOfGrid
            Write-Host "  §B-NVOF: VIPLE_VKFRUC_NV_OF=1 PERF=$NvOfPerf GRID=$NvOfGrid" -ForegroundColor DarkGray
        } else {
            Remove-Item env:VIPLE_VKFRUC_NV_OF      -ErrorAction SilentlyContinue
            Remove-Item env:VIPLE_VKFRUC_NV_OF_PERF -ErrorAction SilentlyContinue
            Remove-Item env:VIPLE_VKFRUC_NV_OF_GRID -ErrorAction SilentlyContinue
        }

        # §J.3.e.X Path β: RIFE native Vulkan integration into VkFrucRenderer.
        # Requires VIPLE_VKFRUC_FRUC + VIPLE_VKFRUC_NATIVE_RIFE both =1
        # (DUAL is implicit when frame-interp pref enables).
        if ($eng.nativeRife) {
            $env:VIPLE_VKFRUC_FRUC         = "1"
            $env:VIPLE_VKFRUC_DUAL         = "1"
            $env:VIPLE_VKFRUC_NATIVE_RIFE  = "1"
            Write-Host "  §J.3.e.X Path β: VIPLE_VKFRUC_NATIVE_RIFE=1 (default 256x128 infer dim)" -ForegroundColor DarkGray
        } else {
            Remove-Item env:VIPLE_VKFRUC_NATIVE_RIFE -ErrorAction SilentlyContinue
        }

        # 2. Kill stale process
        Kill-VipleStream

        # 3. Build CLI args.  rendering config comes from registry
        #    (rendererSelection / frucBackend / videoDecoderSelection).
        #    --video-decoder hardware required for D3D11 cascade to pick
        #    D3D11VARenderer instead of falling through to SDL.  VkFruc
        #    engine uses SW decode + must NOT have --video-decoder hardware.
        $args = @(
            "stream", $StreamHost, $App,
            "--1080", "--fps", "60", "--no-vsync",
            "--display-mode", "fullscreen"
        )
        if ($eng.rendererSel -eq 1) {
            $args += @("--video-decoder", "hardware")
        }

        # 4. Set in-renderer dump env vars CONDITIONALLY.
        #    - VkFruc engine (rendererSel=0): VIPLE_VKFRUC_DUMP_DIR triggers
        #      the §B-DUMP path inside VkFrucRenderer (captures fp32 RGB
        #      compute buffer to real/ + all/ subdirs).
        #    - D3D11 engines (rendererSel=1): VIPLE_RENDERER_DUMP_DIR
        #      triggers §B-DUMP-D3D11 inside D3D11VARenderer (captures
        #      swapchain backbuffer post-FRUC composite).
        #    Setting BOTH would cause the cascade override (in ffmpeg.cpp)
        #    to force everything into VkFruc SW path — which we don't want.
        if ($eng.rendererSel -eq 0) {
            $env:VIPLE_VKFRUC_DUMP_DIR        = $outDir
            $env:VIPLE_VKFRUC_DUMP_FRAMES     = "$($Fps * $Seconds)"
            $env:VIPLE_VKFRUC_DUMP_DELAY_MS   = "$($WarmupSec * 1000)"
        } else {
            $env:VIPLE_RENDERER_DUMP_DIR      = $outDir
            $env:VIPLE_RENDERER_DUMP_FRAMES   = "$($Fps * $Seconds)"
            $env:VIPLE_RENDERER_DUMP_DELAY_MS = "$($WarmupSec * 1000)"
        }

        # 5. Launch (log to file).  Stream runs WarmupSec + capture +
        #    drain window.  Cascade may instantiate renderer multiple
        #    times (probe + real); the dump's session_start_ms resets
        #    each time, so we wait until "capture complete" log appears
        #    rather than fixed time.
        $logPath = Join-Path $outDir "launch.log"
        $logErr  = "$logPath.err"
        $maxStreamSec = $WarmupSec + $Seconds + 15  # 15s safety budget for writer drain
        Write-Host "  launching VipleStream (waiting for capture complete, max ${maxStreamSec}s)..." -ForegroundColor DarkGray
        $proc = Start-Process -FilePath $exePath -ArgumentList $args `
                              -RedirectStandardOutput $logPath `
                              -RedirectStandardError $logErr `
                              -PassThru -WindowStyle Normal

        # 6. Poll log for "capture complete" message; then wait additional
        #    5s for writer to drain queue; then kill.  Fall back to fixed
        #    total time if capture never completes.
        $captureComplete = $false
        $deadline = (Get-Date).AddSeconds($maxStreamSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            if (Test-Path $logErr) {
                $matches = Select-String -Path $logErr -Pattern "capture complete" -ErrorAction SilentlyContinue
                if ($matches.Count -gt 0) {
                    $captureComplete = $true
                    Write-Host "    -> capture complete signal received" -ForegroundColor DarkGreen
                    break
                }
            }
        }
        if (-not $captureComplete) {
            Write-Host "    -> capture complete TIMEOUT (after ${maxStreamSec}s)" -ForegroundColor Yellow
        }

        # 7. Drain time for writer thread to flush remaining BMPs to disk.
        Write-Host "    waiting 6s for writer to drain queue..." -ForegroundColor DarkGray
        Start-Sleep -Seconds 6

        # 8. Stop stream
        Kill-VipleStream

        # 9. Clear env vars before next iteration
        Remove-Item env:VIPLE_RENDERER_DUMP_DIR     -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_RENDERER_DUMP_FRAMES  -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_RENDERER_DUMP_DELAY_MS -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_DUMP_DIR       -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_DUMP_FRAMES    -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_DUMP_DELAY_MS  -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_NV_OF          -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_NV_OF_PERF     -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_VKFRUC_NV_OF_GRID     -ErrorAction SilentlyContinue
        Remove-Item env:VIPLE_FRUC_BYPASS_BUDGET    -ErrorAction SilentlyContinue

        $count = (Get-ChildItem $outDir -Filter "frame_*.bmp" -Recurse -ErrorAction SilentlyContinue).Count
        $bytes = (Get-ChildItem $outDir -Filter "frame_*.bmp" -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum
        $mb    = if ($bytes) { [math]::Round($bytes / 1MB, 1) } else { 0 }

        # Parse log for the actual selected backend (in case fallback)
        $rendererLine = (Select-String -Path $logErr -Pattern "VkFrucRenderer selected|Using D3D11VA accelerated renderer|Using.*Renderer" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
        if (-not $rendererLine) {
            $rendererLine = (Select-String -Path $logPath -Pattern "VkFrucRenderer selected|Using D3D11VA accelerated renderer|Using.*Renderer" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
        }
        if (-not $rendererLine) { $rendererLine = "(unknown)" }

        # For 06_vkfruc_nvof: check NVOF actually came up (not just env-var
        # set but DLL load / session init / image alloc could all silently
        # fall back to block-match).  Log a clear PASS/FAIL alongside
        # renderer detection.
        $nvofStatus = ""
        if ($eng.nvof) {
            $nvofOk = (Select-String -Path $logErr -Pattern "VIPLE-VKFRUC-NVOF\] OF session init OK" -ErrorAction SilentlyContinue | Select-Object -First 1)
            if ($nvofOk) {
                $nvofStatus = "NVOF=OK"
            } else {
                $nvofFail = (Select-String -Path $logErr -Pattern "VIPLE-VKFRUC-NVOF\].*(failed|fall back|NULL)" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
                $nvofStatus = if ($nvofFail) { "NVOF=FAIL ($nvofFail)" } else { "NVOF=??" }
            }
        }

        Write-Host "  saved $count PNG ($mb MB) -> $outDir" -ForegroundColor Green
        Write-Host "  detected: $rendererLine $(if ($nvofStatus) { "[$nvofStatus]" })" -ForegroundColor DarkGray

        $summary += [PSCustomObject]@{
            engine   = $eng.name
            status   = "OK"
            frames   = $count
            sizeMB   = $mb
            detected = $rendererLine
            nvof     = $nvofStatus
        }
    }
}
finally {
    Kill-VipleStream
    # Restore registry
    if ($origRendererSel -ne $null) { Set-ItemProperty -Path $regKey -Name rendererSelection      -Value $origRendererSel -Type DWord }
    if ($origFrucBackend -ne $null) { Set-ItemProperty -Path $regKey -Name frucBackend            -Value $origFrucBackend -Type DWord }
    if ($origFrameInterp -ne $null) { Set-ItemProperty -Path $regKey -Name frameInterpolation     -Value $origFrameInterp -Type String }
    if ($origVds         -ne $null) { Set-ItemProperty -Path $regKey -Name videoDecoderSelection  -Value $origVds         -Type DWord }
    Write-Host "`nRegistry restored." -ForegroundColor DarkYellow
}

Write-Host "`n=========================================================" -ForegroundColor Cyan
Write-Host "  Summary" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
$summary | Format-Table -AutoSize

Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "  1. Open each folder in image viewer to flip through PNGs" -ForegroundColor DarkGray
Write-Host "     explorer.exe `"$OutRoot`"" -ForegroundColor DarkGray
Write-Host "  2. Or run pairwise diff analyzer:" -ForegroundColor DarkGray
Write-Host "     python scripts\benchmark\analyze_fruc_compare.py $OutRoot" -ForegroundColor DarkGray
