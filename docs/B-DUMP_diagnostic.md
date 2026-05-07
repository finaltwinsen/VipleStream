# §B-DUMP — In-Renderer Frame Dump 診斷工具

從 streaming swapchain backbuffer / FRUC compute buffer 直接抓 BMP，繞過
gdigrab 在 fullscreen exclusive / DWM 雙端 sync 的問題。用來：

1. **逐 frame 視覺確認 FRUC 補幀是否真的有產生不同於 real frame 的內容**
2. **跨 backend 比對不同 FRUC 引擎的 interp 品質**（VkFruc / D3D11 Generic
   / NVIDIA OF / DirectML / NCNN）
3. **debug 渲染管線的格式 / sync issue**

## 兩條 dump path

| Path | Trigger env | 抓的內容 | 輸出檔名 |
|---|---|---|---|
| **VkFruc** §B-DUMP | `VIPLE_VKFRUC_DUMP_DIR=path` | `m_FrucCurrRgbBuf` (real, fp32 RGB) + `m_FrucInterpRgbBuf` (warp output, fp32 RGB) | `<dir>/real/frame_NNNN.bmp` + `<dir>/all/frame_NNNN.bmp`（real + interp 交錯）|
| **D3D11VA** §B-DUMP-D3D11 | `VIPLE_RENDERER_DUMP_DIR=path` | swapchain backbuffer at `Present()` time (BGRA / RGBA8) | `<dir>/frame_NNNN_real.bmp` + `<dir>/frame_NNNN_interp.bmp` |

## 快速啟動

```powershell
# 跑全部 6 個 engine 比對（host 上要先有動的內容）
pwsh scripts\benchmark\compare_fruc_engines.ps1 -WarmupSec 8 -Seconds 1 -Fps 60

# 結果在 D:\temp\fruc_compare\<engine_name>\
# 跑分析驗證
python scripts\benchmark\analyze_fruc_compare.py D:\temp\fruc_compare
python scripts\benchmark\check_motion_direction.py D:\temp\fruc_compare\01_vkfruc_bm
python scripts\benchmark\verify_dump_interp.py D:\temp\fruc_compare\01_vkfruc_bm
```

## 環境變數 reference

### Dump trigger

| Env | 預設 | 作用 |
|---|---|---|
| `VIPLE_VKFRUC_DUMP_DIR` | unset | 啟動 VkFruc dump；同時隱式啟動 VkFruc SW path（force renderer cascade）+ FRUC + DUAL mode + force frameInterpolation=true |
| `VIPLE_VKFRUC_DUMP_FRAMES` | 10 | VkFruc dump server frames 數量 |
| `VIPLE_VKFRUC_DUMP_DELAY_MS` | 10000 | warmup delay (ms) before VkFruc dump 啟動 |
| `VIPLE_RENDERER_DUMP_DIR` | unset | 啟動 D3D11VARenderer dump |
| `VIPLE_RENDERER_DUMP_FRAMES` | 30 | D3D11 dump 抓總共幾張 BMP（real + interp 加總）|
| `VIPLE_RENDERER_DUMP_DELAY_MS` | 8000 | warmup delay (ms) |

**注意**：兩個 DUMP env var 不要同時設，會撞到 cascade override 邏輯
（`VIPLE_VKFRUC_DUMP_DIR` 強制走 VkFruc SW，會 short-circuit D3D11 路徑）。
`compare_fruc_engines.ps1` 已經處理這個 — 只在 vkfruc engine 設前者，
其他 engine 設後者。

### FRUC backend tuning（dump 期間 / production）

| Env | 預設 | 作用 |
|---|---|---|
| `VIPLE_FRUC_BYPASS_BUDGET` | 0 | DirectML / NCNN 即使 inference 超過 frame budget 仍 force-enable（dump 比對用，會掉 frame）|
| `VIPLE_FRUC_NCNN_FORCE` | 0 | 即使 NCNN probe 偵測到 RIFE 輸出全 0，仍強制使用 NCNN backend（會看到黑色 interp） |
| `VIPLE_FRUC_NCNN_FP16` | 0 | NCNN 啟用 fp16 packed/storage/arithmetic（預設 fp32，因為 fp16 有 driver-level 全 0 issue） |
| `VIPLE_FRUC_NCNN_SHARED` | 0 | NCNN Phase B.4 D3D11↔Vulkan shared texture path（experimental，Memory `project_phase_b_dead_end.md` 有提）|
| `VIPLE_FRUC_NCNN_DIAG` | 0 | NCNN 把 RIFE 輸出強制覆寫為 magenta 測試 pattern，用來確認 matToStaging→CopyResource→OutputSRV→blit pipeline |
| `VIPLE_VKFRUC_WARP_NO_MV` | 0 | warp shader 強制 50/50 cross-fade，不用 MV — 無方向但可見的 ghost effect |
| `VIPLE_VKFRUC_WARP_PURE50` | 0 | warp shader 用 PURE50 fixed-blend（無 luma-bias） |
| `VIPLE_VKFRUC_WARP_QUALITY` | 0 | VkFruc 強制 c1 Quality adaptive blend |
| `VIPLE_VKFRUC_NV_OF` | 0 | VkFruc 啟用 NVIDIA Optical Flow（HW）取代 block-matching ME — Phase B-NVOF pipeline |

### 其他 env

| Env | 作用 |
|---|---|
| `VIPLE_VKFRUC_FRUC` | 1 強制啟用 VkFrucRenderer FRUC compute（不需要 Settings 開啟）|
| `VIPLE_VKFRUC_SW` | 1 強制 VkFruc SW upload path |
| `VIPLE_VKFRUC_DUAL` | 1 強制 DUAL mode |
| `VIPLE_VKFRUC_TRIPLE` | 1 切到 60→180 TRIPLE 模式（比 DUAL 多補一張） |

## Dump 機制 internals

### VkFruc §B-DUMP

- 在 `VkFrucRenderer::createFrucComputeResources()` 結尾呼叫 `initFrameDump()`
- 每個 chain run 結尾（warp dispatch 之後、curr→prev copy 之後）執行：
  ```cpp
  pfnCmdCopyBuffer(cmd, m_FrucCurrRgbBuf,    m_DumpStagingReal[slotIdx],    1, &dumpRegion);
  pfnCmdCopyBuffer(cmd, m_FrucInterpRgbBuf,  m_DumpStagingInterp1[slotIdx], 1, &dumpRegion);
  if (m_TripleMode) pfnCmdCopyBuffer(cmd, m_FrucInterpRgbBuf2, m_DumpStagingInterp2[slotIdx], 1, &dumpRegion);
  ```
- 在下一次 slot fence wait 後 `flushDumpSlotIfPending(slotIdx)` 把 staging 內容 push 到 writer thread queue
- Writer thread 用 stbi_write_bmp 寫檔
- Staging buffer 偏好 `HOST_CACHED | HOST_VISIBLE | HOST_COHERENT` memory type（NV 預設 coherent-only 是 WC，CPU 讀取 ~200MB/s 會卡爆 1FPS；CACHED 走 ~10GB/s）

### D3D11VA §B-DUMP-D3D11

- 在 `D3D11VARenderer::initialize()` 結尾呼叫 `initFrameDump()`
- staging texture format 從 `m_SwapChain->GetDesc1()` 動態讀取，避免 BGRA vs RGBA 不一致 → CopyResource 失敗 → 全黑 BMP
- 在 `renderFrame()` 內每個 `Present()` 之前呼叫 `dumpBackbufferIfActive("real" | "interp")`
- 用 `CopyResource(staging, backbuffer)` + `Map(D3D11_MAP_READ)` 拉 pixels
- Bypass `frameLate skip` 檢查（當 `VIPLE_FRUC_BYPASS_BUDGET=1`）讓 ML backend 即使太慢也能 dump
- 10-bit HDR (`R10G10B10A2_UNORM`) 不支援，dump init 會 disable

### Indexing convention（重要！）

| Path | bmps[2N] | bmps[2N+1] |
|---|---|---|
| D3D11 (00, 02-05) | real_N | mid(real_N, real_N+1) — 向前 |
| VkFruc (01) | real_N | **mid(real_N-1, real_N)** — 向後 |

不同的 indexing 是因為 dump 時機不同。VkFruc 在 chain run for frame N 結尾抓
`m_FrucInterpRgbBuf`，那是用 `prev=N-1, curr=N` warp 出來的 → midpoint
between N-1 and N（時序上在 N 之前）。`analyze_fruc_compare.py` 已經處理這個
asymmetry。

## Known issues

### NCNN-Vulkan RIFE 輸出全 0（deferred，已寫 todo.md）

RTX 3060 Laptop + NV 596.144 driver + ncnn 20220729 + RIFE-v4.25-lite forward
pass 回傳全 0 mat。`probeInferenceCost()` 的新版會偵測到並 reject 這個
backend，cascade 會 fallback 到 Generic compute。Override 用
`VIPLE_FRUC_NCNN_FORCE=1`。

詳細在 `docs/todo.md` §B-DUMP NCNN-Vulkan RIFE 輸出全 0 章節。

### Block-matching ME 對快速 motion 的設計極限

VkFruc + D3D11 Generic 都用 8×8 block Census ME with diamond search
[3, 1] reaching ±4 px。對遊戲鏡頭快速 pan（>10 px/server-frame at 30fps）
會 fallback 到 MV=0 → warp 走「mid(prev_pp, curr_pp, 0.5)」same-pixel
cross-fade，產生輕微 ghost。

要做真正的 motion-aware interp 對快速場景，**改用 D3D11 + NVIDIA Optical Flow
backend**（`frucBackend = 1`）— RTX 30+ HW accelerator，sub-pixel 精度。

### MV sign convention 修正（已做）

ME shader 輸出的是 backward MV（`mv = prev_pos - curr_pos`），但 warp 原本
假設 forward MV。沒 negate 之前，sparkle 在 interp 顯示反向移動。
2026-05-07 修：在 warp shader 開頭加 `mv = -mv`（VkFruc plvk.cpp 1772 +
D3D11 d3d11_warp_compute.hlsl）。3 個 quality variant 重編。

## Scripts

| Script | 功能 |
|---|---|
| `scripts/benchmark/compare_fruc_engines.ps1` | 6 engine sequential dump，保存到 `D:\temp\fruc_compare\<engine>\` |
| `scripts/benchmark/analyze_fruc_compare.py` | 計算每 engine 的 doubling / interp_resid / verdict（VkFruc / D3D11 indexing 各自處理） |
| `scripts/benchmark/check_motion_direction.py` | 檢查 interp 的 motion 方向是否跟 real_N-1 → real_N 同向（catches reverse-direction artifact） |
| `scripts/benchmark/verify_dump_interp.py` | 詳細 per-frame midpoint analysis（real_N / interp / midpoint distance） |
| `scripts/benchmark/restore_fruc_settings.ps1` | compare 中斷後 cleanup registry settings 回 user 預設 |
| `scripts/compile_d3d11_shaders.ps1` | 重編所有 .hlsl 到 .fxc（已被 build_moonlight_package.cmd 自動呼叫） |

## 常見診斷流程

### 「補幀沒效果，看起來跟 real frame 一樣」

1. 跑 `compare_fruc_engines.ps1` 抓 6 engine 樣本
2. 跑 `analyze_fruc_compare.py` 看 `interp_r` —— `<0.3` 是 OK midpoint，
   `~0.5` 是 curr-copy fallback，`>0.7` 是 artifact
3. 看 BMP：選靜止場景看 sparkle / 邊緣 ghost；選 dynamic 場景看 motion smoothness
4. 如果是 sub-pixel motion：屬設計極限（block-matching ME 量化到 0），
   改用 NVIDIA OF backend

### 「補幀方向反了 / 抖動」

1. 跑 `check_motion_direction.py` 看 shift sign
2. mean shift `<0` 是 reverse direction（已修，2026-05-07）
3. 如果還反向：可能 driver / shader 重編沒生效；確認 .fxc timestamp 比 .hlsl 新

### 「全黑 BMP」

1. D3D11 path：通常是 swapchain format 跟 staging texture mismatch（修法：用 `m_SwapChain->GetDesc1()` 動態查 format）；10-bit HDR swapchain 不支援
2. NCNN path：先看 log 有無「RIFE output is all-zero」warning（已修，cascade 會自動 fallback）
3. 強制 verify pipeline：`VIPLE_FRUC_NCNN_DIAG=1` 寫 magenta，BMP 該變紫紅
