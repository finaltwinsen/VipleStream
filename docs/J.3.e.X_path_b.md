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

`Settings → Video → Frame Interpolation` 勾「Native RIFE 補幀 (β beta — 30-60s 後可能崩潰)」。
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

### 30-60s device-lost crash

**症狀**：streaming ~30-60s 後 GPU 撞 `VK_ERROR_DEVICE_LOST`，FFmpeg HEVC 解碼
失敗 → stream 終止. Aftermath 寫 `.nv-gpudmp` (~500 KB) 跟 shader debug
(~3 KB) 到 `%TEMP%\VipleStream-aftermath-*`.

**復現**：HW + SW decode mode 都會撞.  跨 inferDim (128/256/512) 都會撞.
Block-match path 不會撞 — 排除 VkFrucRenderer 自身問題.  獨立於 fps (60fps server
撞較晚 ~1m59s, 180fps server 撞較快 ~30s — 載荷越高越快).

**已嘗試但無效的 fix**：
- β.5.0 → β.5.1 — 移除 41 MB 1080p flow buffers / 2 個 bilinear UP dispatches.
  撐久一點但仍撞.
- β.6 — descriptor set pre-alloc + cache 取消 per-frame churn.
  Latency 反退化 9% 且仍撞 → reverted.

**Aftermath crash dump 解碼結果（5 個 dump 100% 一致）**：
```
"Faulted Warps": [{
  "Fault Description": "MMU Fault Error - shader instruction MMU fault accessing memory",
  "Shader GPU PC Address": "fragment_01 @ 0x00000150"
}]
"Shader infos": { "Shader name": "fragment_01", "Shader type": "Fragment" }
"Page fault info": {
  "Resource": { "Destroyed": true, "Width": 930, "Height": 502, "Size": 1933312 }
}
```

`fragment_01` = `vkfruc.frag` (real-frame 顯示 shader, 採樣 AVVkFrame.img[0]
透過 ycbcr sampler).  Fault PC 0x150 在 `OpImageSampleImplicitLod`.
Resource 930×502 RGBA8 (= UV plane of HEVC 1860×1004 coded ref frame).
"Destroyed=true" 是 post-mortem (engine reset 後 driver 清掉的)，故這個
不直接代表 fault 時 resource 已 destroy.

**修法嘗試（皆 reverted）**：
1. `av_frame_clone(frame)` 持有 ref 跨 slot fence — 不撞了但 fps 30× 退化 (1.89 vs 60)
2. `av_frame_alloc` + `av_frame_ref/unref` — 仍撞 + 同樣 fps 退化
3. β.6 descSet cache pre-alloc — 不撞但 latency +9%, reverted
4. β.6 周期性 vkDeviceWaitIdle 25s — crash 從 30-60s 略晚到 78s 仍撞

從 v1/v2 觀察 fps 退到 1.89 同時 crash 消失 (v1)：暗示 crash 是高 dispatch
密度觸發，AVFrame ref 不是真正 root cause；ref-keep 只是「副作用」減壓.

**短時段使用 OK** (< 30s). 完整修法需要：
- Nsight Graphics（GUI 不只 aftermath_decode CLI）載 .nv-gpudmp 看 last
  in-flight cmd buffer 完整 state 跟 page-fault 前的 access pattern
- 或 NV driver 升版過 596.36 試
- 或 RIFE-v4-lite 389 dispatch 重組成 fewer fused dispatches (大工程)

當前 ship 狀態：opt-in beta default-OFF，UI + docs 已備齊，使用者可短時段
試用享受 quality 提升.

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
- ✅ β.8 prefs UI + docs (this commit)
- ⏳ Crash root cause — needs Nsight Graphics
- ⏳ β.9 TRIPLE 60→180
- ⏳ β.10 Linux + AMD/Intel platform validation
