# §J.3.e.X Path β — Native RIFE Vulkan 補幀

## 是什麼

Path β 是 VipleStream 的 Vulkan 補幀 backend 新路線（**beta opt-in**）：在
Vulkan compute pipeline 跑 [RIFE-v4.25-lite](https://github.com/megvii-research/ECCV2022-RIFE)
ML model 抽出 motion flow + blend mask，再用自家 warp+blend shader 在 native
1080p / 1440p / 4K 解析度合成 interp frame.

跟既有路徑差別：

| 路徑 | Motion 估計 | Warp+blend | Quality | 適用 |
|---|---|---|---|---|
| Block-match (原 Vulkan FRUC 預設) | software 4-neighbor / 8-neighbor block | 自家 shader native res | ⚠️ Frame doubling 主導，effective interp 0% | LP fallback |
| §B-NVOF (NV Vulkan ext) | NVOFA hardware | 自家 shader native res | ✓ ~30% effective interp | RTX 20+ |
| D3D11 + NvOFFRUC.dll | NVOFA + NV proprietary blend | NV closed pipeline | ✓✓ ~47% effective interp | RTX 20+ |
| **Path β (這份)** | **RIFE ML flow** | **自家 shader native res** | **✓✓✓ score 0.95 ≈ perfect midpoint** | **RTX 30+ (限 NV driver < TBD)** |

## 啟用方式

### UI

`Settings → Video → Frame Interpolation` 勾「Native RIFE 補幀 (β beta)」。
僅在 Vulkan renderer 路徑（rendererSelection = RS_VULKAN）顯示.

「RIFE 推論解析度」下拉選單 4 個選項：
- 128 — 最快，~10ms 整 chain，144-180Hz panel 適用
- **256** — 預設平衡，~14ms，60fps DUAL OK
- 384 — 品質佳，~25ms，需 RTX 4070+
- 512 — 最高品質，~40ms，高階卡

### Env var (dev / regression bisect 用)

```cmd
set VIPLE_VKFRUC_FRUC=1
set VIPLE_VKFRUC_DUAL=1
set VIPLE_VKFRUC_NATIVE_RIFE=1
set VIPLE_VKFRUC_RIFE_INFER_DIM=256   :: optional, default 256
set VIPLE_VKFRUC_RIFE_BETA5=0         :: optional, fallback to β.4 bilinear-up RGB
VipleStream.exe stream <host> Desktop
```

env var 優先於 prefs（讓 dev 在 prefs disable 的狀態下還是能 bisect）.

## 架構（如何運作）

每個 server frame 進來後：

```
1. NV12 → RGB compute       (m_FrucCurrRgbBuf at native source dim)
2. Bilinear DOWN ×2          (m_FrucPrevRgbBuf, m_FrucCurrRgbBuf → infer dim)
3. RIFE inference            (256×128 default, ~12ms hot)
   └→ 抽出 tensor 714 (4ch flow concat) + tensor 727 (1ch sigmoid mask)
4. Native warp+blend shader  (samples flow+mask at infer dim with bilinear in shader,
                              warps prev/curr at native res, blends with mask → m_FrucInterpRgbBuf)
5. Curr → prev copy          (next frame's prev)
```

關鍵設計：**warp 在 native res** 確保 edges 銳利度跟 real frame 一致，
**flow 上採在 shader 內 bilinear** 避免額外 VRAM allocation 和 memory bandwidth.

對比錯誤的 β.4 (deprecated)：β.4 是 RIFE 跑全 graph 算 RGB 結果，再 bilinear up
RGB 7.5×到 1080p — interp frame 變模糊跟 sharp real frame strobe.

## Quality 量測（vs 其他 engines）

`scripts/benchmark/verify_dump_interp.py` 跑 §B-DUMP 抓的 5 對 (real, interp, real)：

| Engine | pair_adj | midpoint score | verdict |
|---|---|---|---|
| Block-match | 0.38 | 0.61 | OK 但 motion% = 0 (frame doubling) |
| β.4 (bilinear up RGB) | **4.05** | **1.89** | **FAIL** (上採模糊) |
| **β.5.1 native warp** | **0.94** | **0.95** | **OK ✓ perfect midpoint 5/5** |

bilinear roundtrip 1080p→256×128→1080p 自身 mean abs intensity loss = 4.48，
β.5.1 殘留 0.94 比這個 baseline 還小 4.7× — 證明 RIFE 推論本身正確 + native
warp 保留了原圖細節.

## Latency 量測（RTX 3060 Laptop GPU + power unlock + 1080p source）

`[VIPLE-VKFRUC-GPU-PROF]` 五階段 timing：

| inferDim | nv12rgb | DOWN | RIFE | warp+UP | copy | total | fps DUAL | fps display |
|---|---|---|---|---|---|---|---|---|
| 128×128 | 0.10ms | 0.04ms | 9.9ms | 0.4ms | 0.18ms | **10.7ms** | 77 | **154** |
| **256×128** | 0.10ms | 0.05ms | 13.2ms | 0.4ms | 0.17ms | **13.8ms** | 62 | **124** |
| 512×256 | 0.13ms | 0.12ms | 29.1ms | 0.4ms | 0.17ms | 29.7ms | 33 | 66 (drop) |

60fps DUAL budget = 16.7ms per server frame.  RTX 3060 撐到 256×128. RTX 4070+
應撐 384×192. RTX 4090 級別才撐 512×256 不掉幀.

## 已知問題

### ✅ RESOLVED: 30-60s device-lost crash (β.6 fix 2026-05-08)

**症狀（已修補）**：streaming ~30-60s 後 GPU 撞 `VK_ERROR_DEVICE_LOST`，
FFmpeg HEVC 解碼失敗 → stream 終止.  Aftermath 寫 `.nv-gpudmp` (~500 KB)
跟 shader debug (~3 KB) 到 `%TEMP%\VipleStream-aftermath-*`.

**root cause（透過 Nsight Graphics `nv-aftermath-format -p VipleStream.pdb -g
shader-debug-info-search-path` symbolize 找出）**：

```
"Faulted Warps": [{
  "Fault Description": "MMU Fault Error - shader instruction MMU fault accessing memory",
  "Shader GPU PC Address": "fragment_01 @ 0x00000150"
}]
Symbolized callstack:
  fragment_01 → VkFrucRenderer::drawOverlayInRenderPass(VkCommandBuffer*)
              → VkFrucRenderer::renderFrame(AVFrame*)
              → Pacer::renderFrame(AVFrame*)
              → Pacer::renderThread(void*)
"Page fault info":
  "Resource": { "Destroyed": true, "Width": 930, "Height": 502, "Size": 1933312 }
```

關鍵：`fragment_01` 不是 `vkfruc.frag` (real frame sampler)，而是
`vkfruc_overlay.frag`，sampler bound 到 `m_OverlayView[type]`.  930×502 是
overlay surface (perf overlay / SDL_TTF rendered text) 的尺寸，會隨字串長度
動態變化.

`drainOverlayStash()` 偵測到 surface 尺寸變動就**立刻** destroy 舊的 VkImage /
VkImageView — 但 descriptor set 仍指著舊 view，且尚未執行完的 cmd buffers
（Pacer renderThread 同時可能有 2-3 個 in-flight）還會 sample 它，page-fault
觸發 device-lost.  Block-match path chain 4ms in-flight 少所以撞不到，
Path β chain 14ms in-flight 多所以一個 overlay resize 就有 race window.

**修法（commit 待跑）**：`vkfruc.cpp::drainOverlayStash()` line 3317-3341，
在 dim 變動時先 `vkDeviceWaitIdle(m_Device)` 再 destroy 舊 image，確保
所有 in-flight cmd buffers 都已執行完 + descriptor set 之後 update 也安全.
Cost: overlay resize 觸發時 ~1 frame GPU wait（一次 stream 通常 0-2 次）,
hot-path 邊際 ~0.

**驗測（2026-05-08 17:00）**：8 min 連續 stream 1080p60 HEVC + dual-present
+ Path β rifeNative=1 inferDim=256，**0 crash, 0 dump, 0 device-lost log**.
chain time 13.5ms 全程 ±0.1ms 穩定，fps 30.00 ±0.04，WS 317MB 無 leak.
resize-fix 訊號剛好 fire 一次（perf overlay 從 845×502 縮到 835×502）.

**先前無效的 fix（皆 reverted）**：
- β.5.0 → β.5.1 — 移除 41 MB 1080p flow buffers + 2 個 bilinear UP dispatches.
  撐久一點但仍撞.
- β.6 v1 — descSet pre-alloc + cache 取消 per-frame churn.
  Latency +9% 且仍撞 → reverted.
- β.6 v2 — 周期性 vkDeviceWaitIdle 25s.  Crash 從 30s 推到 78s 仍撞 → reverted.
- AVFrame ref-keep v1/v2 (`av_frame_clone` / `av_frame_alloc+ref`) — v1 不撞
  但 fps 30× 退化 (1.89 vs 60), v2 仍撞.  事後才知這只是「副作用減壓」
  讓 race window 變窄, 不是 root cause.
- 從 v1/v2 fps 退化同時 crash 消失暗示 race 是 dispatch 密度觸發 — 確實
  正確（drainOverlayStash 每 perf overlay 文字長度變動觸發一次 = dispatch
  rate 越高 race window 越大）.

完整 root cause + 修法的 takeaway：Vulkan 對 in-flight resource 的 lifetime
管理 driver 不保證自動 wait, 你必須自己 sync.  Nsight Graphics 的 PDB +
shader debug info symbolization 直接抓到正確的 sampler 來源, aftermath
JSON 自身只給 shader hash 不夠.

### TRIPLE 60→180 暫不支援

Path β 跟 m_TripleMode 共存時 ctor force m_RifeNativeMode = false. TRIPLE
需要 2 次 RIFE inference (t=1/3 + t=2/3) 在 16.7ms server budget 內 = 每次
需 < 8ms — 256×128 fits 但 stability 沒解前不 enable.  詳見 docs/TODO.md
§J.3.e.X β.9.

### 平台限制

只在 Windows + RTX 30 系列（596.144 driver）驗測.  Linux build 路徑沒驗，
AMD/Intel GPU 沒測.

## 開發 / 除錯

### Live profile

```powershell
# Stream + grep GPU-PROF 看 latency breakdown
$env:VIPLE_VKFRUC_FRUC=1; $env:VIPLE_VKFRUC_DUAL=1; $env:VIPLE_VKFRUC_NATIVE_RIFE=1
VipleStream.exe stream <host> Desktop
# log at %LOCALAPPDATA%\VipleStream\VipleStream\moonlight*.log
```

關鍵 log lines：
- `[VIPLE-VKFRUC-RIFE-β5] β.5.1 chain active` — RIFE 真的在跑
- `[VIPLE-VKFRUC-RIFE-β5] β.5.1 ready` — init 完成
- `[VIPLE-VKFRUC-GPU-PROF] nv12rgb=Xus me=Yus median=Zus warp=Wus copy=Vus total=Tus` —
  per-stage GPU time（對 β.5 path: me = bilinear DOWN, median = RIFE inference,
  warp = bilinear UP flow + bilinear UP mask + native warp）
- `RIFE flow+mask at NxN` — 確認 inferDim
- `runRifeNativeStage failed` — Path β 跌回 block-match
- `DISABLING Path β` — first-frame 失敗永久關掉

### Quality dump 比對

```powershell
$env:VIPLE_VKFRUC_DUMP_DIR='D:\temp\fruc_dump_beta'
$env:VIPLE_VKFRUC_DUMP_FRAMES=20
$env:VIPLE_VKFRUC_DUMP_DELAY_MS=5000
VipleStream.exe stream <host> Desktop  # 跑 25s 抓 20 對 (real, interp)
python scripts\benchmark\verify_dump_interp.py D:\temp\fruc_dump_beta
```

注意：dump 路徑 force m_SwMode=true，跟 Path β 共存時 commit 2a5732e
guard 會跳掉 dump cmd 命令避免 race（但仍可能撞 30-60s crash 提早）.

### 模型路徑

```
moonlight-qt/app/rife_models/rife-v4.25-lite/
  flownet.param   — ncnn text format network topology (~70 KB)
  flownet.bin     — fp16 packed weights (~11.3 MB)
```

build 期透過 .qrc 包進 binary（看 `app/app.pro`）, runtime 透過
`Path::getDataFilePath("rife-v4.25-lite")` 取 install dir 的 copy.

Pipeline cache 持久化在 `%LOCALAPPDATA%\VipleStream\VipleStream\cache\
rife_vkfruc_path_b_pipe.cache` — 第二次啟動省 ~50-300ms 蓋 SPIR-V → driver
binary 編譯.

## Roadmap

- ✅ β.1 init proof of life (commit 67a7c89)
- ✅ β.2 chain swap (67a7c89)
- ✅ β.4 bilinear down/up wrapper (20507da)
- ✅ β.4 v4 /128 dim alignment (a5bbc5c)
- ✅ β.4 v5 GPU-PROF timing (d5f2812)
- ✅ β.4 v6 dump-skip guard (2a5732e)
- ✅ β.4 v7 revert v6's dump-skip (cc4a97e)
- ✅ β.5 RIFE flow extraction + native warp (f38fc10)
- ✅ β.5 timing isolation (25fd4b1)
- ✅ β.5.1 sample flow at infer dim (7d37f7a)
- ❌ β.6 descSet cache (reverted; didn't fix crash)
- ❌ β.6 周期性 vkDeviceWaitIdle 25s (reverted; only delayed crash)
- ❌ AVFrame ref-keep v1/v2 (reverted; fps 30× regression)
- ✅ β.8 prefs UI + docs (commit 9d52afa)
- ✅ **β.6 stability fix — overlay resize use-after-free in drainOverlayStash**
  **(2026-05-08, this commit) — symbolized via Nsight Graphics nv-aftermath-format**
- ⏳ β.9 TRIPLE 60→180
- ⏳ β.10 Linux + AMD/Intel platform validation
