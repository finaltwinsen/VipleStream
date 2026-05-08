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

---

## Android port (§B-DUMP-ANDROID, 2026-05-08)

手機端 (`moonlight-android`) 平行實作的 frame dump。檔案命名與索引慣例
跟 PC flat layout 完全相同，PC 分析腳本（`verify_dump_interp.py` /
`analyze_fruc_compare.py` / `check_motion_direction.py`）已升級接受 PNG 與
BMP 兩種格式，因此**兩端 dump 可進同一套 pipeline 比對**。

實作於 `moonlight-android/app/src/main/java/com/limelight/binding/video/FrucDumpWriter.java`，
hook 進 `FrucRenderer.onFrameAvailable()`。

### 啟動方式（system property，非 env var）

Android shell 沒有 env var 等價物，改用 `setprop`（反射呼叫
`android.os.SystemProperties`，跟 `VkBackend.isOptedIn` 同樣模式，避開
hidden-API）：

```sh
# Release build (com.piinsta) 或 debug build (com.piinsta.debug)
adb shell setprop debug.viplestream.frucdump.dir /sdcard/Android/data/com.piinsta/files
adb shell setprop debug.viplestream.frucdump.frames 10
adb shell setprop debug.viplestream.frucdump.delay_ms 10000

# 啟動 app → 連線 host → 串流，dump 在 warmup 後自動進行
# 完成後：
adb pull /sdcard/Android/data/com.piinsta/files/fruc_dump_<TIMESTAMP> .
```

注意：property 在 `FrucRenderer` ctor / `initialize()` 讀一次，**必須
在串流開始前 `setprop`**。設定不會 persist 跨 reboot；release build 用
此 property 仍有效（無 user-facing UI toggle）。

### Property reference

| Property | 預設 | 作用 |
|---|---|---|
| `debug.viplestream.frucdump.dir` | unset | 啟動 dump（unset = 完全 disabled，零 overhead） |
| `debug.viplestream.frucdump.frames` | 10 | 抓多少 server frame（達標後自動停止） |
| `debug.viplestream.frucdump.delay_ms` | 10000 | warmup 延遲（讓 ME shader / FPS 穩下來再 dump） |

### 輸出 layout

```
fruc_dump_20260508_154233/
  manifest.json                ← 裝置/解析度/quality preset/版本資訊
  frame_0000_real.png          ← cycle 0: 第一幀 decoded 輸出（沒 interp）
  frame_0001_interp.png        ← cycle 1: warp(real_0, real_1) 中間幀
  frame_0002_real.png          ← cycle 1: real_1
  frame_0003_interp.png        ← cycle 2: warp(real_1, real_2)
  frame_0004_real.png          ← cycle 2: real_2
  …
  mv_frame_0001.bin            ← cycle 1 用的 MV field（int32 pair LE，Q1）
  mv_frame_0003.bin
  …
```

索引慣例 `file 2N=real_N, file 2N+1=interp(real_N, real_N+1)` 跟 PC
flat layout 完全相同。

### MV binary 格式（與 PC 兼容）

PC 寫盤是 `int32 pair (mvX, mvY)`，每 block 8 bytes，Q1 unit
（1 LSB = 0.5 px）。

手機端 ME shader 寫的是 R32I texture 單一 packed `(x<<16) | (y & 0xFFFF)`，
每 block 4 bytes。`FrucDumpWriter.writeMvBin()` 在 writer thread 把 packed
int32 unpack 成 PC 格式（int32 pair LE）後寫盤。檔案因此 byte-for-byte
跟 PC `mv_frame_NNNN.bin` 相容：

```python
import numpy as np
mv = np.fromfile('mv_frame_0001.bin', dtype=np.int32).reshape(-1, 2)
# mv[:,0] = x in Q1, mv[:,1] = y in Q1
print('non-zero:', (mv != 0).any(axis=1).sum(), '/', len(mv))
print('max abs:', np.abs(mv).max())  # 預期 <= 96 (clamp 上限)
```

### Manifest schema

`manifest.json` 是 Android 端獨有 sidecar（PC dump 沒寫，資訊散在 SDL
log），分析時直接讀 JSON 不必猜：

```json
{
  "schema_version": 1,
  "platform": "android",
  "app_version_name": "1.3.339",
  "app_version_code": 339,
  "device_model": "Pixel 5",
  "device_brand": "google",
  "android_sdk": 33,
  "gl_renderer": "Adreno (TM) 620",
  "gl_version": "OpenGL ES 3.2 …",
  "stream_width": 1920,
  "stream_height": 1080,
  "mv_width": 30,
  "mv_height": 16,
  "block_size": 64,
  "quality_preset": 0,
  "vsync_period_ns": 11111111,
  "frames_requested": 10,
  "delay_ms": 10000,
  "started_at_unix_ms": 1715165553123,
  "started_at_iso": "2026-05-08T07:32:33Z",
  "image_format": "png",
  "mv_format": "int32_pair_le",
  "mv_unit": "Q1",
  "index_convention": "file_2N=real_N, file_2N+1=interp(real_N, real_N+1)"
}
```

### ⚠️ Privacy 注意

Dump PNG 內容是 streaming pipeline 收到的 **host 完整畫面**（1080p × N
張）。若 host 桌面上顯示密碼、訊息、瀏覽器分頁、財務資料等，這些會
被原樣寫進 PNG 檔。預設 dump dir 是 app-private external storage（其他
app 讀不到），但若你 `adb pull` 出來再分享分析結果，記得先檢查內容。
Dump 機制是 debug-only（property unset 時 zero-overhead），不會在
release build 自動啟用。

### 端到端驗證 SOP

```sh
# 把 <DEVICE> 換成你的 device serial（adb devices 第一欄）
DEVICE=<DEVICE>

# 1. 啟用 dump
adb -s "$DEVICE" shell setprop debug.viplestream.frucdump.dir \
    /sdcard/Android/data/com.piinsta/files

# 2. 啟動 app, 串流 ~30 秒（足夠 10 frame × delay 10s + capture window）

# 3. pull
adb -s "$DEVICE" pull \
    /sdcard/Android/data/com.piinsta/files/fruc_dump_$(date +%Y%m%d)_* .

# 4. 用 PC 端原有工具分析（不必改 script）
python scripts/benchmark/verify_dump_interp.py fruc_dump_<TS>
python scripts/benchmark/check_motion_direction.py fruc_dump_<TS>
```

期望輸出：
- 目錄出現 ~10 對 `frame_NNNN_real.png` + `frame_NNNN_interp.png` 與
  對應的 `mv_frame_NNNN.bin`
- `manifest.json` 內容合理
- `verify_dump_interp.py` 的 `interp_works_count` 在 motion 場景 ≥ 3/5
  （手機端 GLES block-matching ME 在中等運動水位）
- `check_motion_direction.py` 的 mean shift 與 real motion 同向（`>= 0`）

### 跟 PC dump 的差異

| 項目 | PC §B-DUMP | Android §B-DUMP-ANDROID |
|---|---|---|
| 觸發 | `VIPLE_VKFRUC_DUMP_DIR` env var | `debug.viplestream.frucdump.dir` system property |
| 圖檔 | `.bmp` (uint8 RGBA, no compression) | `.png` (Bitmap.compress, ~4× 小) |
| 寫盤 | 單一 writer thread, `kDumpQueueCap=12` | 同 (HandlerThread, queue cap 12) |
| Readback | `vkCmdCopyBuffer` → HOST_CACHED staging | PBO ring (3 slot) + `glFenceSync` |
| MV format | int32 pair LE (寫盤時就是這格式) | R32I packed → unpack 寫盤 (兼容) |
| Manifest | 無 (資訊在 SDL log) | `manifest.json` |
| Late-frame skip | N/A (PC 沒這條路徑) | 整 cycle 不 commit，counter 不前進 |

### 風險 & 已知限制

1. **MV readback driver 差異** — 部分 Mali driver 拒絕
   `GL_RED_INTEGER + GL_INT` PBO readback。`FrucDumpWriter` 偵測到
   `glGetError != GL_NO_ERROR` 時 graceful 跳過 MV dump（PNG 仍正常
   產出），logcat 印 warning。
2. **首次 cycle 索引 parity** — 手機端 `frameCount==1` 跳 warp，與 PC
   `firstCycle` skip interp 對齊；兩端 file 0 都是 real_0。
3. **Late-frame / poor-conn skip** — 手機獨有，dump 期間整 cycle 不
   commit、counter 不動，保 2N/2N+1 慣例。代價：dump 蒐集時間可能
   長於預期。
4. **Property propagation 時機** — `FrucRenderer` ctor 讀一次；必須
   pre-stream 設好。
5. **磁碟空間** — 10 frame × 2 PNG (~2MB) + 10 mv.bin (~2KB) ≈ 40MB /
   session，遠低於 app-private quota。
