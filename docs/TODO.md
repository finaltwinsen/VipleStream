# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **Done (perf milestone)** | **§J.3.e.Y** native RIFE perf — 4Y.1..4Y.6 全收尾 | 9 commit 86→22ms 4× cumulative + Final.1 vs-ncnn mean 4.7e-5；4Y.6 Tensor Core path 完整實作但**預設關閉**（perf 僅 7%、precision 70× 差），`VIPLE_RIFE_VK_COOPMAT=1` opt-in；後續往別的方向走 |
| **Done (correctness)** | **§J.3.e.X** 手刻 RIFE Vulkan inference | 17 commits 8807886..cba641d，0 ncnn dep；通過 vs-ncnn correctness gate，標準 standalone via `VIPLE_RIFE_NATIVE_VK_TEST=1`；production NcnnFRUC integration (Final.3b) deferred |
| **Active** | **§J.3.e** SW Vulkan path 持續優化 | 1080p120 × 3 codec 全 PASS；4K AV1 SSE2 後 62→76fps；4K H.264/HEVC decoder-bound（CPU 上限）|
| **Negative result (§J.3.g)** | **FRUC ME 解析度下放：不是 bottleneck** | 2026-05-03 實作完整 ME→1080p / 720p downsample，4K120 + FRUC 仍卡 75-81 fps；DUAL off 也一樣。ME / DUAL 都不是限制；推測 warp shader (per-pixel @ 4K) + memory bandwidth + sync barriers。需要 GPU per-stage timing 才能定位。實作已 revert |
| **HW-pending** | **§I.D** Android Vulkan FRUC async compute | D.2.0–D.2.5 已 ship + Pixel 5 verify；剩自然 45fps→90Hz ideal 1:2 比例 — Pixel 5 panel 鎖 60/90Hz、GameManagerService 鎖 60fps，需 LTPO panel hw 才能驗 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 3d.6 — AV1 native VK decode grey | 9 個 parser bug 修完仍 grey；NV driver 596.36 + AV1 vkCmdDecodeVideoKHR 黑/灰；§J.3.f ffmpeg 包裝路徑已 cover AV1，這條 deprecated |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 1.7 ONLY-mode NVDEC device-lost | 5 個變體都繞不過，NV 596.36 結構性 bug，預設 PARALLEL 穩 |
| **Active (long-running)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗 |
| **Done (v1.3.337)** | **§K.1** Linux x86_64 兩端 ship | `.deb` server + AppImage client 正式進入 release，五件對齊。fix 清單見 §K. 章節 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | Pi 5 + V3DV mesa Vulkan + V4L2 HW decode + DRM/KMS render；上游 source-level 支援，沒 CI prebuilt；FRUC backend 全 disable（Vulkan 補幀 Pi 5 GPU 不夠力），純 streaming 應 OK |
| **Deferred** | **§K.4** Wayland XDG portal teardown root-cause | `Restart=always` 緩解已 ship；需可重現 streaming 環境（GPU Linux 機）才能修進 wayland.cpp/portalgrab.cpp 的 EPIPE 路徑 |
| **Low** | **§F** DirectML 搬 D3D12 / command bundles | 4K120 real-time 才需要 |
| **Low** | **§G.1** RIFE v1 11-channel | A1000 launch overhead bound (§G.3 negative result)；RTX 30/40+ 才有意義 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Low** | **§D** HelpLauncher URL → 結構化 docs | docs/setup_guide.md + docs/troubleshooting.md 已寫；HelpLauncher 切過去等 doc site stand 起來 |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計 |

---

## §A. 品牌遷移相容性債

### §A.2 WiX installer 的 registry / install paths

**目前：** WiX installer 的 `HKCU\Software\Moonlight Game Streaming Project` 安裝狀態旗標、`InstallFolder = "Moonlight Game Streaming"`、`APPDATAFOLDER = %LOCALAPPDATA%\Moonlight Game Streaming Project` 全沒動。

**影響檔案：** `moonlight-qt/wix/Moonlight/Product.wxs`、`moonlight-qt/wix/MoonlightSetup/Bundle.wxs`。

**注意：** 我們**目前根本沒在用 WiX MSI installer 出貨**，client 是 zip + 手動解開、server 用 `deploy_sunshine.ps1` 部署。所以 WiX 的相容性債是「如果哪天要改用 MSI 出貨才需要清」，不是現在的痛點。

**清的時候要做：**
1. 新 InstallFolder = `VipleStream`、新 APPDATAFOLDER = `%LOCALAPPDATA%\VipleStream`
2. 新 HKCU 路徑 = `HKCU\Software\VipleStream`
3. WiX 升級邏輯：偵測舊 key / 路徑存在 → copy → 砍舊
4. 配新的 `UpgradeCode` GUID，否則 MSI 嘗試 in-place upgrade 找不到舊路徑

### §A.7 協定 / wire-format 字串（**不該清**）

**目前：** `"NVIDIA GameStream"`、cert CN `"Sunshine Gamestream Host"`、UA header `Moonlight/...`、mDNS service type `_nvstream._tcp`、serverinfo 的 `state` 字串 `SUNSHINE_SERVER_FREE` / `SUNSHINE_SERVER_BUSY`、HTTP `/serverinfo` `/launch` 等 endpoint 路徑全部維持上游原名。

**理由：** 這是 GameStream wire protocol 的一部分，改了等於跟整個 Moonlight / Sunshine 生態脫鉤——使用者沒辦法用原版 Moonlight 連 VipleStream-Server，也沒辦法用 VipleStream client 連別人家的 vanilla Sunshine。User 已明確要求「混搭互聯」（v1.2.93 討論）。

**保留到永遠。** 這條留著就是要提醒未來想動的人：不要動。

### §A.8 內部 class names

**目前：** `NvHTTP` / `NvComputer` / `NvApp` / `SunshineHTTPS` / `SunshineHTTPSServer` / `SunshineSessionMonitorClass`（Windows window class）等內部命名沒改。

**清的時候要做：** 真的想改就在獨立 refactor PR 裡 batch rename，**不要**跟 feature / bugfix 混在一起（diff 會無法 review）。優先度極低。

---

## §D. 上游 wiki 連結

**狀態：** 🟡 部分 ship — `moonlight-android/.../HelpLauncher.java` 的 setup guide + troubleshooting 目前指 `https://github.com/finaltwinsen/VipleStream#readme`；GameStream EOL FAQ 維持上游 `moonlight-stream/moonlight-docs` wiki。

**已加但未連線：** [`docs/setup_guide.md`](./setup_guide.md) + [`docs/troubleshooting.md`](./troubleshooting.md) 已寫（pairing / config / FRUC / common failure modes 都有），等 GitHub Pages 或類似 doc site stand 起來後，把 HelpLauncher URL 從 README anchor 換成 docs/* 頁面 anchor。

---

## §F. DirectML FRUC — 架構級優化

DirectML auto-cascade（v1.2.91）已經把「中低階 GPU 跑 DML 會掉幀」的痛點解掉，hot path 已 ship。下面是「如果要推 DML 到 4K120 real-time」才需要的架構改動。

### §F.1 FRUC pipeline 整個搬進 D3D12

**Gain：** 消除 `ctx4->Signal`（D3D11 UMD flush）每 frame 60-80 μs。
**Risk：** 高。整個 renderer 的 present / blit / overlay 管線都要重寫成 D3D12 path。Moonlight 原本的 d3d11va / dxva2 / vaapi 多 backend 抽象要重新設計。
**工時估：** 1-2 週。
**觸發條件：** 4K120 real-time DML 需求，目前不急。

### §F.2 Command bundles for pre-recorded DML pipeline

**Gain：** 30-40 μs / frame。
**Risk：** 中。Bundle 建立後 PSO / root sig 不能改、binding 限制多。
**工時估：** 2-3 天。

### §F.3 Zero-copy D3D11→D3D12 heap alias

**Gain：** 10-20 μs / frame。
**Risk：** 中。Heap alias 的 state 管理比 texture share 嚴格。

---

## §G. DirectML FRUC — 更多 ONNX model variants

§G.2（fp16 model + cascade rewrite）跟 §G.3（IFRNet-S 試水）已 ship。重要的 negative result：**A1000 launch overhead 25 ms floor 不可繞過，任何 ML 模型 op count > 150 都上不了 14 ms budget**。

### §G.1 11-channel 輸入 (RIFE v4 v1)

**Gain：** v1 node count 少 50%（216 vs 456），可能比 v2 快 30-50%。
**重新評估：** A1000 仍可能達不到 14 ms（216 ops × 0.1 ms launch overhead = 21.6 ms 已超），但對 RTX 30/40+ 可能值得；要 ship 還是得搭 §G.4 ModelFetcher 路徑避免 release zip 再加肥（自 v1.3.311–312 起 onnx 已 on-demand download）。
**做法：** `tryLoadOnnxModel` 加 11-channel branch；pack 前加 optical flow CS 或塞 zero flow。

### 已就位的診斷工具

- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs
- `[VIPLE-FRUC-DML-ORT]` log — ort_concat / ort_run / ort_post / wait_pack / wait_post / wait_concat 拆解

---

## §I.D Android Vulkan FRUC — async compute queue

§I 整套（A recon → B passthrough → C Vulkan FRUC port → C.6 display timing → C.7 settings UI）都已 ship（v1.2.134–161），thermal regression（dual present + per-frame `vkQueueWaitIdle` → +4°C in 60s）的 root-cause fix 是 §I.D.

### Phase D.2.x — multi-queue + cross-queue sem（**已完成**）

D.2.0–D.2.5 全部 ship + Pixel 5 / Adreno 620 真機 verify 過：

- **D.2.0**（3dc860a + 81e50d4）: multi-queue acquisition. Adreno 620 = 1 family / queueCount=3 / [GFX CMP]，driver 真給 3 distinct VkQueue handles + 接受 concurrent same-family submit。
- **D.2.1**: init-time `compute init clear` submit 拆 gfx→compute 兩個 cmd buffer 用 binary `VkSemaphore` chain。
- **D.2.2 / D.2.3**: hot-path `render_ahb_frame` ring 拆兩 cmd buffer。compute 側收 AHB import barrier + (dual mode) dispatch_fruc，submit 在 `computeQueue` signal `fSlotComputeDoneSem[slot]`。gfx 側收 render passes，wait acquireSem(s) + computeDoneSem。Single mode 仍 submit compute 的 barrier-only cmd buf 維持 sem 對稱。
- **D.2.4**: 60fps single-mode pre-vs-post split baseline — p50 +0.07ms (1.29→1.36ms)，p99 +0.52ms (2.94→3.46ms)，120s GPU +0.52°C。Cost < 1ms p99 可接受。
- **D.2.5 mailbox opt-in** (commit `0217426`): `debug.viplestream.mailbox=1` sysprop 切 MAILBOX → FIFO_RELAXED → FIFO，default off。Pixel 5 真機 A/B 3 runs × 60s × dual mode @ 90Hz：FIFO p50 14.13/p90 14.79/p99 15.70 vs MAILBOX p50 14.21/p90 14.88/p99 15.86，Δ +0.08/+0.09/+0.16 ms（noise 內），無顯著 win。

**Dual entry threshold 從 1.05× 放寬到 1.40×**（commit `1a5cc5b`）：60fps Sunshine input + 90Hz panel 在原 1.05× 永遠進不了 dual（2×60=120 > 94.5）；FIFO present 對 surplus 是 driver-side drop 不是 stutter，p50 跟 ideal 1:2 dual 一致。改後 60fps@90Hz 預設進 dual 92.6%，使用者只要設「90 fps + 開 FRUC」就能拿到 interp。

### 還沒做的 — 自然 45fps source on 90Hz panel ideal 1:2 比例測試

Pixel 5 panel 只支援 60Hz/90Hz mode，GameManagerService 又把 app default frame rate override 為 60fps，現有 hardware combo 拿不到 ≤ 45fps stable input。要驗 ideal 比例得換 LTPO panel 手機（S24+/Pixel 8）。這是 hardware-pending，**不是 client codebase 缺東西**。

### 鐵律

1. 每 Phase 都要有 baseline 對比。沿用 `scripts/benchmark/android/`，量出來不如預期就停。
2. **GLES path 至少保留到「Vulkan 穩定 6 個月」之後才移除**。期間 FRUC bug 修兩次（GLES + Vulkan）是已知 +20% 維護成本。

### 已就位的診斷工具

- `scripts/benchmark/android/android_baseline.sh` — thermal + fps + jank 採集
- `scripts/benchmark/android/analyze_baseline.py` — 30s bucket fps + pixel-thermal 高頻交叉驗證
- `debug.viplestream.vkprobe=1` system property — opt-in Vulkan FRUC backend
- `debug.viplestream.mailbox=1` system property — opt-in MAILBOX present mode (D.2.5)
- Settings UI toggle「FRUC 後端：GLES (預設) / Vulkan (實驗)」（v1.2.161 ship）
- `[VKBE-D20]` / `[VKBE-D21]` / `[VKBE-D22]` log lines 證明 multi-queue / cross-queue handoff 真在 runtime 跑

---

## §J. Desktop Client Vulkan-native（active）

**動機：** §I.F NCNN-Vulkan FRUC backend 在 D3D11 主 renderer 上踩到結構性瓶頸：

1. **D3D11 → Vulkan bridge cost ~30-40ms/frame**（CPU staging 路徑），讓 RIFE inference 21ms 只佔總耗時的 33%。
2. **Shared texture 路線在 NVIDIA 596.84 driver device lost** — `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT` 任何 raw Vulkan command 都被 reject。要 ID3D11Fence + VkSemaphore 完整同步基礎設施才有機會（v1.3.43 三次嘗試都失敗，見 §J.1）。
3. **D3D11 deprecation pressure** — Microsoft 主推 D3D12 + WinUI；D3D11 video decoder 不太可能加新 codec（AV2 / VVC）。
4. **Cross-platform 一致性** — Android client 已是 Vulkan-native，desktop 也走 Vulkan 後 codebase 大幅簡化。

### 階段表

| Phase | 內容 | 狀態 |
|---|---|---|
| **§J.1** D3D11.4 fence ↔ VkSemaphore bridge | NCNN shared path 同步基礎建設 | ❌ DEAD END NV 596.84 — 4 種 ablation 全 device lost |
| **§J.1 路線 A** | 完整 ID3D12Device bridge (~250 LOC) | ⏳ 未驗 |
| **§J.2** Vulkan post-processing pipeline | FRUC、HDR shader、blit、swapchain 全進 Vulkan | 🟡 跟 §J.3.e 大幅重疊 |
| **§J.3** VK_KHR_video_decode 完整 Vulkan-native pipeline | 透過 ffmpeg 8.x Vulkan hwaccel + PlVkRenderer | ✅ 基礎已存（HEVC 直通）；§J.3.e FRUC 整合進行中 |
| **§J.4** HEVC + H264 decode + 跨平台驗證 | Linux full coverage；macOS 路徑決策 | 規劃中 |
| **§J.5** 整合測試 + fallback hardening + 預設切換 | NV / AMD / Intel bench；舊 driver 退回 D3D11；預設改 Vulkan | 規劃中 |

### §J HEVC 1440p120 SW decode cap — **resolved by §J.3.f**

歷史症狀：RS_VULKAN + VkFrucRenderer 走 SW HEVC decode 在 mobile CPU 上 1440p120 卡 ~50 fps（FRAME-threading throughput 限制）；2026-05-03 兩端 pktmon pcap 確認 wire 0 loss、root cause 在 client decoder。

**§J.3.f rebuild（FFmpeg 8.1 + `hevc_vulkan` hwaccel）後解掉**：HEVC HW Vulkan 1440p120 跑 119–122 fps、decodeMean 0.30 ms、networkDropped 0。整合 commit b2b7afd 起 RS_VULKAN 預設走這條，使用者無須額外勾選。SW HEVC 1440p cap 仍存在（CPU 上限），但走 RS_D3D11 (DXVA HW) 或 RS_VULKAN (HW Vulkan) 都能避開。

**保留的診斷工具**（之後其他 throughput 問題還會用）：[`docs/diag_wire_loss.md`](./diag_wire_loss.md) 雙端 pktmon SOP、`[VIPLE-NVENC-RATE]` / `[VIPLE-BCAST-RATE]` / `[VIPLE-NET]` 三段 rate trace。

### §J.3.e VkFrucRenderer（Android architecture port，**目前主戰場**）

既有 `moonlight-android/.../jni/moonlight-core/vk_backend.c` ~4220 行 production-tested native Vulkan FRUC，PC 端 port 70% 直接可用。

#### Sub-phase 表

| Sub-phase | 內容 | 狀態 |
|---|---|---|
| **§J.3.e.2.i.1-7** Init / device / swapchain / FFmpeg-Vulkan bridge | full set | ✅ ship |
| **§J.3.e.2.i.8 Phase 1.x** H.265 native VK_KHR_video_decode | NvVideoParser + VkVideoSessionKHR + cross-queue timeline sem | ✅ v1.3.251（84.78fps PARALLEL stable）|
| **§J.3.e.2.i.8 Phase 1.5c** ONLY mode device-lost graceful degrade | DEVICE_LOST → m_DeviceLost flag → render/decode 早 return | ✅ v1.3.295 |
| **§J.3.e.2.i.8 Phase 1.6** Aftermath GPU crash dump | NV Nsight Aftermath SDK 1.6 + checkpoints + auto-write `.nv-gpudmp` | ✅ v1.3.298 |
| **§J.3.e.2.i.8 Phase 1.7e** ONLY env rename → `*_DANGEROUS` | 5 個變體都繞不過 NV 596.36 bug，預設 PARALLEL | ✅ v1.3.302 |
| **§J.3.e.2.i.8 Phase 2** H.264 native VK decode port | nvvideoparser H.264 sources + submitDecodeFrameH264 | ✅ v1.3.275 |
| **§J.3.e.2.i.8 Phase 2.5** FRUC NV12 source 整合 native decode | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完 | 🟡 v1.3.277，不擋使用 |
| **§J.3.e.2.i.8 Phase 3** AV1 native VK decode plumbing | parser + submitDecodeFrameAv1 | ✅ v1.3.272，submit 預設 OFF（pending Phase 3d.6）|
| **§J.3.e.2.i.8 Phase 3d.6** AV1 native decode GPU-side grey | 9 個 parser real bug 修了仍 grey，driver-level | 🟡 deferred indefinitely — AV1 走 libdav1d SW |
| **AMD ycbcr** descriptor pool dynamic sizing | `VkSamplerYcbcrConversionImageFormatProperties::combinedImageSamplerDescriptorCount` × N | ✅ v1.3.307 |
| **Vulkan demoted to experimental** | 預設 `RS_D3D11`、Vulkan 標 [實驗性] | ✅ v1.3.308 |

#### SW Vulkan path 優化（**1080p120 × 3 codec 全 PASS at v1.3.323**）

最近一波收尾：

- **v1.3.318** libdav1d threads + `max_frame_delay=1`：AV1 1080p120 p95 16.32 → 14.52ms（commit `83cc409`）
- **v1.3.320** HEVC FRAME threading + 1440p/2160p × 3 codec measurement（commit `0786791`）：H.264 + AV1 1440p120 PASS；HEVC 1440p120 卡 60fps 是 §J HEVC 1440p server cap（上面）
- **v1.3.321** per-slot staging buffer + async memcpy ↔ GPU upload pipelining（commit `0666a1d`）：close 了 v1.3.276 註解承認的 staging-buffer WAW race，4K 場景 marginal win（decoder-bound）
- **v1.3.323** SSE2 YUV420P→NV12 UV interleave（commit `440204b`）：4K UV memcpy 1240us → 370us，**4K AV1 SW decode 62 → 76fps**

1080p120 final state（30s × 3 codec, RS_VULKAN + VDS_FORCE_SOFTWARE, FRUC off）：

| codec | mean fps | p50 ms | p95 ms |
|---|---|---|---|
| H.264 | 120.02 | 8.47 | 15.95 |
| HEVC | 119.89 | 8.39 | 11.18 |
| AV1 | 120.01 | 7.35 | 14.52 |

4K@120 SW decode 仍是 CPU codec-bound（H.264 90fps / AV1 76fps / HEVC 28fps），需要更新世代 CPU（Zen 5 / Raptor Lake）才有可能往上推；client SW path 的 Vulkan 上傳路徑本身已不是瓶頸（renderFrameSw 整段 < 3ms 即使 4K）。

### §J.3.e.X 手刻 RIFE Vulkan pipeline（**✅ correctness milestone DONE 2026-05-05**）

**達成：** 從零實作 RIFE-v4.25-lite 全套 Vulkan inference，不依賴 ncnn。
17 個 commit (`8807886..cba641d`) 涵蓋 .param parser → 11 種 op shader →
389-layer graph executor → vs-ncnn correctness gate。完整 milestone 細節
跟 commit map 在 [`docs/J.3.e.XY_native_rife_pipeline.md`](J.3.e.XY_native_rife_pipeline.md)。

**Correctness：** Final.1 vs-ncnn `mean=4.72e-05 max=0.014 frac>1e-2=0.00%`
（4Y.5a 後）。99.49% pixel diff ≤ 0.01 視覺等價，0% pixel diff > 0.01.

**Production 整合狀態：** 標準的 standalone 路徑可由 env var
`VIPLE_RIFE_NATIVE_VK_TEST=1` 觸發跑全套 self-test。production
streaming 路徑**沒有改動**，仍走 NcnnFRUC. Final.3b（接成 NcnnFRUC
的 Linux fallback）刻意 deferred 直到 Linux test VM 有 ncnn-build-fail
環境可重現驗測。

### §J.3.e.Y native RIFE perf optimisation（**✅ milestone DONE 2026-05-06**）

**達成：** §J.3.e.X 的 86 ms cold-GPU baseline 壓到 ~22 ms（4× 加速），
9 個 commit (`876ce45..068c9b2` + 文件 `e4e4dfc`)。詳細 phase map 在
[`docs/J.3.e.XY_native_rife_pipeline.md`](J.3.e.XY_native_rife_pipeline.md)。

| Phase | 內容 | 結果 |
|---|---|---|
| 4Y.1a | `findHostVisibleMemoryType` 偏好 BAR/ReBAR | 86 → 43.6 ms (2.0×) |
| 4Y.0 | per-phase wall-clock instrument | (找出 readback 是 24% 大頭) |
| 4Y.1b | out0 host-cached staging buffer | 43.6 → 24.0 ms (3.6×) |
| 4Y.4 | tiled shared-mem Conv2D for k=3 s=1 | 24.0 → 21.5 ms (4.0×) |
| 4Y.4-stride2 | s=2 tiled — barrier overhead | (negative, reverted) |
| 4Y.5a | fp16 weight storage | perf 持平、**vs-ncnn 9× 收緊** |
| 4Y.6 Step 1+2 | cooperative_matrix probe + hello-world | infrastructure 完成 |
| 4Y.6 Step 3 | Conv2D-via-GEMM coopmat shader + unit test | isolated PASS |
| 4Y.6 Step 4 | dispatch path 整合 + env-var gate | **opt-in default-OFF** |

**4Y.6 為什麼 opt-in（RTX 3060 Laptop 量到的 trade-off）：**

| 指標 | OFF（4Y.4 路徑） | ON（coopmat） |
|---|---|---|
| Final.1 mean abs err | 4.72e-05 | 3.21e-03（70× 差） |
| Final.1 max abs err | 0.0135 | 0.530（39× 差） |
| Final.1 verdict | PASS | **FAIL** |
| Final.3a native median | 19.25 ms | 17.95 ms（**僅 7% 加速**） |

Precision regression 是 double-fp16 quantisation 跨 40 layer 累積（σ × √40
≈ 3.2e-3，數學可預期，shader 本身正確）；perf 只贏 7% 是因為 RIFE-v4-lite
每層 conv 太小，Tensor Core fixed-cost 吃掉大半 throughput。7% 遠小於
thermal-throttle 噪音 ~30%，不值得 ship 預設。`VIPLE_RIFE_VK_COOPMAT=1`
opt-in 留著當 known-working capability，未來推 4090 或更大 RIFE 模型時
直接 build on。

**Thermal noise 警告：** GPU sustained workload vs short cold-GPU benchmark
行為差很大（ncnn 退化 10× vs native 2×）。進一步 1.5× 量級的優化 benchmark
驗測需要 fixed clock (`nvidia-smi --lock-gpu-clocks`) 或 cool-down protocol。

**還沒做但有 ROI 的候選（沒人推之前 deferred）：**

- **4Y.6 shader 改 multi-subgroup WG** — 1 WG = 1 subgroup 的設計使 SM
  利用率僅 ~50%；改成 4 subgroups/WG 共享 im2col tile load 預期再 1.5-2×
  攤平 fixed cost。但 RIFE-v4-lite 規模太小，可能仍贏不過 4Y.4 多少
- **4Y.5b activation fp16 storage** — bandwidth 大頭，但 11 個 shader 的
  input/output binding 全要改，4-6h 工程，dynamic range 風險低
- **4Y.7 dispatch fusion** — Conv→ReLU→BinOp×beta chain 合併，預期
  1.1-1.2×（小，但 shader 改動量適中）

**Final.3b 接 NcnnFRUC：** 仍 deferred until Linux env。預估 4-6h 工程，
medium-high risk（動 production hot path）。

### §B Vulkan HW path FRUC integration（**🟡 active 2026-05-06**）

直到 §B 之前，RS_VULKAN HW mode 雖然 frucMode/dualMode 都自動為 true，
但 `renderFrame()` 從來沒接 `runFrucComputeChain` 或 dual-present —
等於 production HW path 完全沒在補幀（解釋了使用者觀察「testufo
30→60 補幀沒效果」的根因）。

| Step | Commit | 內容 | 驗測 |
|---|---|---|---|
| A baseline | — | 1080p60 SW path 量 GPU-PROF baseline (15 samples) | nv12rgb=410 me=411 warp=214 total=1248us |
| B1a | `03a962f` | HW path 加 image-to-buffer copy + FRUC chain dispatch | total=750us（比 SW 快 40%）；overlay 仍「未啟用」 |
| B1b | `a86df9f` | HW path 加 dual-present（real + interp 兩次 swapchain acquire/render/present） | overlay「已啟用」、cumul real:interp = 1:1 |
| **B-quality** | TBD | 補幀演算法品質優化（ME / median / warp / blend） | **active，待規劃驗測腳本** |
| **B2** | TBD | TRIPLE FRUC 60→180（兩個 interp frame） | 推遲到 B-quality 完成後 |

**B-quality 動機：** B1b ship 後 overlay 顯示正確、dual-present 跑了，
但使用者主觀感覺補幀效果不夠好。需要客觀驗測 + 演算法疊代：
- ME block size = 8（可能太小、產生 noisy MV）
- MV median filter 目前是 noop copy（之前實驗為了量 GPU 時間留下的）
- Warp blend factor = 0.5 hardcoded（沒有 motion confidence weighting）
- 對 occluded / 新 pixel 沒特別處理

**B2 TRIPLE 推遲到 B-quality 完成後：** A 階段量到 1080p60 GPU compute
total 1.31ms (估 TRIPLE 1.58ms)，180fps budget 5.55ms 還剩 4ms，硬體
完全 ready。但目前 DUAL 視覺品質都還沒滿意，TRIPLE 只會讓品質問題
更嚴重（3 個 frame 中 2 個是 interp）。先把 DUAL 改好再推 TRIPLE。

### §J.3.f AV1 / HEVC / H.264 Vulkan hwaccel via ffmpeg（**✅ DONE 2026-05-03 / integration b2b7afd**）

**達成：** rebuild 出 minimal FFmpeg 8.1 client DLL（`avcodec-62.dll` 5.2 MB），含 `--enable-vulkan --enable-hwaccel=h264_vulkan,hevc_vulkan,av1_vulkan --enable-libdav1d`。整合 commit `b2b7afd` 起，**RS_VULKAN preference 自動觸發 Vulkan HW decode + FRUC + DUAL**（不再需要 `VIPLE_USE_VK_DECODER=1` / `VIPLE_VK_FRUC_GENERIC` / `VIPLE_VKFRUC_HW` env 三件組）。env var 仍保留作 explicit override / debug fallback。

#### 1440p120 即時實測（v1.3.333，env-var opt-in 路徑）

| Codec | received fps | decodeMeanMs | networkDropped | hostLatencyAvgMs | Vulkan HW |
|---|---|---|---|---|---|
| **H.264** | 121–123 | **0.26–0.55 ms** | 0 | 7.3 ms | ✅ |
| **HEVC** | 119–122 | **0.30 ms** | 0 | 3.5 ms | ✅ |
| **AV1** | 121–125 | **5–8 ms** | 0 | 2.7 ms | ✅ |

#### 4K120 + FRUC + DUAL × 3 codec（v1.3.333+ b2b7afd integration，full-power GPU）

| Codec | fps | decode | 備註 |
|---|---|---|---|
| **H.264** | 92 | 0.6 ms | host NVENC 4K H.264 編碼上限 |
| **HEVC** | 101–103 | 0.4–0.7 ms | host NVENC 4K HEVC 編碼上限 |
| **AV1** | 116–119 | 4.2–4.8 ms | 達 120fps target，dedicated_dpb 5ms 底線 |

5-env-var 路徑跟 RS_VULKAN auto-detect 路徑量到完全相同 fps，整合無 regression。

**對比之前 SW HEVC 1440p120 cap：** received 50→122 fps（2.4×）、decodeMean 100ms→0.3ms（**300×加速**）、networkDropped 34–51→0。§J HEVC 1440p120 cap 完全解決。

**AV1 latency tune（2026-05-03）：** ffmpeg native AV1 decoder 在 [`vulkan_decode.c:1368`](https://git.ffmpeg.org/gitweb/ffmpeg.git/blob/HEAD:/libavcodec/vulkan_decode.c#l1368) 硬寫死 `|| AV_CODEC_ID_AV1` 強制 dedicated_dpb（out-of-place decode + DPB→output copy）。此設計對 game streaming 有 trade-off：copy step 成本 ~5ms，但讓 Pacer 持有 output 不會 block 解碼器。`extra_hw_frames=4` 的預設 pool 太深（max_refs 8 + 1 + 4 = 13 slots × Pacer 持有 ~4 = 9 frames pipeline = 75–100ms）；改 `extra_hw_frames=1` 後 pool=10、pipeline 1 frame，降到 5ms steady。試過 `=0`（pool 太緊 → 卡 Pacer + 78ms startup spike）、`=2 + FLAG2_FAST`（variance 大）、`EXPORT_DATA_FILM_GRAIN`（無效）、patch ffmpeg `vulkan_decode.c:1368` 拿掉強制 dedicated_dpb（**反而更糟** — Pacer 持有 output 會 block 下一 frame 的解碼，因為 output 即 DPB 共用）。**5ms = AV1 vulkan 在這條 driver+架構下的硬底線**，HEVC 0.3ms 是因為它的 reference pattern 不衝突 Pacer hold。

#### 跟 §J.3.e.2.i.8 Phase 3d.6 對比

Phase 3d.6 自製 raw `vkCmdDecodeVideoKHR` + 我們自己跑 nvvideoparser，9 個 parser bug 修完仍 grey。**ffmpeg 包裝走 ffmpeg 內建 parser + DPB 管理，避開了我們自製 parser 的問題**，同樣的 NV driver 上對 H.264/HEVC/AV1 三個 codec 都能 init + decode 成功。Phase 3d.6 可以 deprecate（Vulkan 路線改走 ffmpeg 包裝）。

#### AV1 latency 警告

AV1 雖 throughput 達標，但 NV driver 的 `av1_vulkan` 解碼路徑單 frame wall-clock 80–106 ms（HEVC 是 0.3 ms，差 200×）。Pipeline depth 撐住 120fps throughput，但端到端遊戲延遲偏高。建議使用者預設挑 HEVC，AV1 留作頻寬限制下的選項。NV driver 升級或許可改善。

#### 涉及的檔案 / 改動

- **rebuilt FFmpeg 8.1** in `moonlight-qt/libs/windows/{include,lib}/x64/`：`avcodec-62.dll` (5.2 MB)、`avutil-60.dll` (1.8 MB)、`swscale-9.dll` (2.7 MB) + matching `.lib` + headers (libavcodec 62.28.100)
- **6 個 mingw runtime DLL**：`libdav1d-7.dll` / `libiconv-2.dll` / `zlib1.dll` / `libwinpthread-1.dll` / `libva.dll` / `libva_win32.dll`（總計 +5 MB ship）
- [`scripts/build_moonlight_package.cmd`](../scripts/build_moonlight_package.cmd) 加上述 6 個 DLL 到 deploy allowlist
- [`ffmpeg.cpp:1590-1605`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L1590) + [`:2029-2096`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L2029) 兩處 SW-force escape：`VIPLE_USE_VK_DECODER=1` 時跳過短路
- [`ffmpeg.cpp:2098+`](../moonlight-qt/app/streaming/video/ffmpeg.cpp#L2098) §J.3.f auto-prefer：`VIPLE_USE_VK_DECODER=1` 時自動走 native h264 / hevc / av1 by name（繞過 av_codec_iterate 不會走到 native 的問題）
- [`plvk.cpp:4492-4540`](../moonlight-qt/app/streaming/video/ffmpeg-renderers/plvk.cpp#L4492) `prepareDecoderContextInGetFormat` override：在 get_format() callback 時 alloc `AVHWFramesContext`（鏡 d3d11va 同名 method），ffmpeg 的 *_vulkan hwaccel 才能 init

#### 後續

- [x] ~~Settings UI toggle 取代 env var~~ — done by b2b7afd（RS_VULKAN preference 直接 auto-trigger，不需要勾選框）
- [x] ~~把 §J.3.e.2.i.8 Phase 3d.6 標 deprecated~~ — 在 priority overview 標記
- [x] ~~4K AV1 baseline benchmark~~ — done in b2b7afd 整合測試（4K120+FRUC+DUAL × 3 codec 全 PASS）
- [ ] AV1 5ms 底線等 NV driver 升級 / AMD / Intel 試或 ffmpeg patch 重 design dedicated_dpb 行為

### §J.3.g FRUC ME 解析度下放 — **NEGATIVE RESULT（2026-05-03，已 revert）**

**原假設（錯）：** Vulkan HW + FRUC + DUAL 在 4K120 卡 77-84 fps，是因為 RTX 3060 mobile FRUC ME compute (block matching @ 480×270 blocks @ 4K) 太慢。預估 ME 下放到 1080p 解析度 4× 加速，總 FRUC time 0.30 + 0.70/4 = 0.47× → 2.1× speedup → 預期 110-120 fps。

**實作完成，未達預期：** 完整實作 NV12→RGB 雙 shader（含新增 `kVkFrucNv12RgbDownsampleShaderGlsl`、bilinear-Y / nearest-chroma downsample）+ buffer 重新 size（`m_FrucMeWidth/Height` + `sizeRGBMe` vs `sizeRGBSrc` 拆分）+ warp shader 改造（PC 加 `meWidthF/meHeightF`、`mvScale = src/me`、`fetchPrev/CurrRGB` 改用 me dims）+ cascade 決策（src ≤ 1440p 不下放、4K 下放 1080p、env `VIPLE_FRUC_ME_RES=540|720|1080`）。

**實測（2026-05-03）：**

| 配置 | 4K120 + FRUC + DUAL fps |
|---|---|
| §J.3.g 前（ME @ 4K） | H.264 77-81 / HEVC 79-84 / AV1 80-84 |
| §J.3.g（ME @ 1080p）| H.264 77-78 / HEVC 75-80 / AV1 不可用（host 卡 720p） |
| §J.3.g（ME @ 720p）| HEVC 75-81 |
| §J.3.g + DUAL=0（FRUC 但單 present）| HEVC 76-81 |

**結論：fps 完全沒變**。隔離測試也排除 DUAL present 是 cost。**ME compute / DUAL 都不是 bottleneck**。

**真正的 bottleneck 推測（未驗證）：**

1. **Warp shader（per-pixel @ 4K）— `§J.3.g` 沒動到** — 8.3M threads × bilinear sample + adaptive blend，估 2-3 ms/frame
2. **Memory bandwidth** — 4K RGB buffer 95 MB × 多次 read/write × 120fps，bandwidth 壓力大
3. **Sync barriers between cmd buffers** — `computeBufBarrier()` 在 4 個 dispatch 之間
4. **Vsync / Pacer 互動** — FIFO_RELAXED 在 GPU 趕不上時 tearing，但 GPU 很慢時 fps 還是受 vsync 拖

**下次定位需要的工具：** GPU timestamp queries（`VkQueryPool` + `VK_QUERY_TYPE_TIMESTAMP`）量出 NV12→RGB / ME / Median / Warp 各自時間。沒這個資料純猜瓶頸只會繼續錯。

**已 revert 的程式碼：**
- `vkfruc.h`: `m_FrucMeWidth/Height/Downsample` 成員拿掉
- `vkfruc.cpp`: cascade decision、buffer sizing 兩 size 變數、`kVkFrucNv12RgbDownsampleShaderGlsl` shader、dispatch 參數改動全部 revert
- `plvk.cpp`: warp shader 的 `meWidthF/meHeightF` PC、`mvScale`、fetch/sampleBilinear 改用 me dims 全 revert

**保留的觀察：**
- `VIPLE_VKFRUC_DUAL=0`（單 present、FRUC compute on）跟 DUAL=1 同 fps — DUAL 不是 cost
- 1080p120 + FRUC 在 §J.3.g 前後也都 ~90 fps，不到 120 — FRUC compute 在 1080p120 也碰邊界
- HEVC 4K decode 0.3-0.6ms / AV1 4K decode 4-7ms — 解碼確實不是瓶頸

**下次接手這條路時：** 不要再先動 ME。**先加 GPU timestamp 量出 per-stage 時間**，看到 warp 真的占大頭，再針對 warp 動（例：dispatch 拆成 8×8 tile + scratchpad、或 warp shader inline bilinear、或 pre-baked MV→pixel-offset 表）。

### 不可動的鐵律 (§J)

1. **Fallback 機制保留** — v1.3.41 的 3-fail fallback、v1.3.44 的 process-lifetime singleton 都不能移除。Phase J 改動失敗時不能讓 user crash。
2. **預設 D3D11 (v1.3.308 起)** — Vulkan 改實驗性次要選項。Phase J.5 真正切 Vulkan 為預設前，新 user 第一次啟動只看到 D3D11；既有 user 設定不被動。
3. **D3D11 renderer 是穩定主線**（不再只是 legacy fallback）— Phase 1.7 系列確認 NV driver 596.36 對 native VK_KHR_video_decode + ONLY mode 有結構性 bug，五個變體都繞不過。D3D11 + DXVA hardware decode 是所有 NV / AMD / Intel Windows 環境的穩定路徑。
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版見 `scripts/benchmark/vk_sw_codec_120.ps1` (v1.3.318 起)。
5. **build script 不能無聲拿舊 binary** — `scripts/build_moonlight_package.cmd` 的 staging step 必須 errorlevel-check rmdir / copy，否則 zombie process 鎖檔會讓 release zip 內含過時 binary（v1.3.299~306 連 8 個 zip 都中招的事故，見 v1.3.307 commit message 5183cee）。

### 已就位的診斷工具（會用到）

- `[VIPLE-FRUC-NCNN]` / `[VIPLE-VKFRUC]` / `[VIPLE-VKFRUC-SW-PROF]` / `[VIPLE-AFTERMATH]` log family
  - SW-PROF 細到 `mem_Y` / `mem_UV` / `fence` / `submit` / `present` 五段 percentile
- `[VIPLE-NVENC] / [VIPLE-NVENC-RATE] / [VIPLE-BCAST-RATE] / [VIPLE-NET]` (server + client 端 packet pipeline rate trace, v1.3.327)
- `VIPLE_USE_VK_DECODER=1` — opt-in Vulkan-first cascade（HEVC 完整 Vulkan-native pipeline）
- `VIPLE_VKFRUC_NATIVE_DECODE=1` — opt-in nvvideoparser feed + native VkVideoSessionKHR
- `VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS=1` — opt-in ONLY mode（NV 596.36 NVDEC device-lost 已知）
- `VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1` — opt-in AV1 vkCmdDecodeVideoKHR submit（**default OFF**，pending §J.3.e.2.i.8 Phase 3d.6 grey 修法）
- `VIPLE_VKFRUC_VULKAN_DEBUG=1` — VK_EXT_debug_utils + 路 validation 訊息進 SDL log
- `VIPLE_VKFRUC_NO_FRUC=1` — 暫時關 FRUC + dual mode（diagnostic）
- `tools/aftermath_decode/` — standalone CLI 解 `.nv-gpudmp` → JSON
- `scripts/benchmark/vk_sw_codec_120.ps1` — RS_VULKAN + VDS_FORCE_SOFTWARE × {H.264, HEVC, AV1} 在 parametric res/fps 下的 PresentMon + [VIPLE-VKFRUC-Stats] 採集 + pass/fail verdict
- `tools/android_vulkan_probe/` — Android Vulkan queue-family probe (probe.c + probe2.c)，cross-compile via NDK，用來在新硬體 verify §I.D 假設

每個子 phase 的 commit message 用 `vX.Y.Z: §J.N.M — <短摘要>` 格式。

---

## §K. Linux build pipeline

GitHub release 規範改成「每個 release 必 ship 完整三件、全同版號」（見 `CLAUDE.md` Release 規範）後，下一步把 Linux artifact 也加進完整 ship。

### §K.1 Linux x86_64 兩端（**SHIPPED 2026-05-05 in v1.3.337**）

**Artifact 已正式進入 release：**
- `VipleStream-Client-1.3.337-linux-x64.AppImage`（59 MB，從 `moonlight-qt/scripts/build-appimage.sh`，三段式手工組裝繞過 linuxdeployqt-on-noble 限制）
- `VipleStream-Server-1.3.337-linux-x64.deb`（9.4 MB，從 `Sunshine/scripts/linux_build.sh` + CPack DEB generator + `--skip-libva` workaround）

**完成的 source fix（一次性 rebase 後 land 進 main）：**
- Sunshine `src/stream.cpp:1054` — `std::max<long long>` 顯式 template
- Sunshine `src/relay.cpp` / `src/stun.cpp` — `closesocket` macro 改 `(::close(fd))` 避 unqualified lookup
- Sunshine `src/nvenc/nvenc_base.cpp` — NVENC API v12 vs v13 dual-support，`#if NVENCAPI_MAJOR_VERSION >= 13` gate
- Sunshine `packaging/linux/` — 5 個 `dev.lizardbyte.app.Sunshine.*` rename 為 `app.viplestream.server.*`
- Sunshine `scripts/linux_build.sh` — `SUNSHINE_EXECUTABLE_PATH=/usr/bin/viplestream-server`、`CMAKE_PREFIX_PATH=/usr/local`、`SUNSHINE_ENABLE_VAAPI=OFF`-on-skip-libva
- Sunshine `.gitattributes` — 強制 LF on `src_assets/linux/misc/{postinst,preinst,prerm,postrm}` 避 dpkg `#!/bin/sh\r` ENOENT
- Sunshine `src_assets/linux/misc/postinst` — auto-enable + `loginctl enable-linger` for `$SUDO_USER`，配 `postrm` 清 dangling symlink
- Sunshine `src_assets/linux/assets/apps.json` — drop `Low Res Desktop` (xrandr HDMI-1 hardcode) + `Steam Big Picture` (setsid dep)
- Sunshine `src/platform/linux/wayland.cpp` — `wl_log_set_handler_client` libwayland EPIPE 防護 + dispatch() defensive
- Sunshine `app-app.viplestream.server.service.in` — `Restart=always` + `StartLimitBurst=30`，client 斷線後 8 秒自動回來
- moonlight-qt `app/streaming/video/ffmpeg-renderers/plvk.{h,cpp}` — ncnn 整段 `#ifdef VIPLESTREAM_HAVE_NCNN` 隔離 + Linux 用系統 ncnn `/usr/local/lib/libncnn.so`
- moonlight-qt `vkfruc.cpp` — Linux POSIX 等價 `_wfopen` / `strncpy_s` / VK_KHR_X*_SURFACE 字串字面量
- moonlight-qt `3rdparty/nvvideoparser/nvvideoparser.pro` — `*-msvc { /arch:AVX2 } else { -mssse3 -mavx ... }` gate
- moonlight-qt `scripts/build-appimage.sh` — qmake6-on-noble moc race workaround：build 前先 `make compiler_moc_source_make_all`
- moonlight-qt `app/deploy/linux/com.piinsta.{desktop,appdata.xml}` — appdata id 改 reverse-DNS + Exec= 修正

**§K.1 Linux pipeline 視為 done。** `release_full.cmd` 一鍵化包進 Linux 兩端是後續 nice-to-have。

### §K.4 Wayland XDG portal teardown root-cause（追隨 §K.1，deferred）

§K.1 ship 了 systemd `Restart=always` 緩解 client 斷線後 server 死掉的問題（option A），但 libwayland EPIPE 從何處發出未根因。需可重現 streaming 環境（VM 沒 GPU 不能驗 encoder + portal grab combo）。Pi 5 / 真 GPU Linux 機驗到時再修進 `wayland.cpp` / `portalgrab.cpp` 的 EPIPE 路徑。

### §K.2 Raspberry Pi 5 client（aarch64，等 §K.1 通後）

**目標 artifact：**
- `VipleStream-Client-X.Y.Z-rpi-aarch64.deb`（Pi 5 + 64-bit RPi OS Bookworm 用）

**為什麼選 Pi 5、放掉 Pi 3 / Pi 4 / armhf 32-bit：**
- Pi 5 自家 video block + V3DV mesa Vulkan + Bookworm 64-bit 是 RPi 主流現在式
- Pi 3 / Pi 4 32-bit RPi OS 走 MMAL legacy 路徑，現代用戶幾乎不在這條，不值得三條 ARM target 都 ship
- Pi 4 64-bit Bookworm 走 V4L2 + DRM/KMS，跟 Pi 5 部分共用，但 GPU 跑 Vulkan 邊緣，FRUC 補幀絕對沒救

**Pi 上的 fork 改動相容性：**
- FRUC backend 全 disable（Pi 5 V3DV 跑不動 4K compute，1080p 也勉強）—— 退回 vanilla Moonlight 體驗
- DirectML / NCNN-Vulkan / Aftermath 全 win32 only，Pi 自動 skip
- core streaming + DRM/KMS render + V4L2 / Vulkan HW decode 走上游現成 path

**Build pipeline 候選（待選）：**
1. **GitHub Actions arm64 runner** —— 現在 GH-hosted arm64 runner 已 GA，但 build time 跟 cost 待測
2. **Native build on Pi 5** —— 一次 setup，每次 release SSH 觸發，build time ~30 min；可靠
3. **QEMU cross-compile (aarch64-linux-gnu-gcc + Qt sysroot)** —— 設 sysroot 痛苦，但本機 build time 接近 native x86 速度
4. **Docker buildx multi-arch** —— `Sunshine/docker/ubuntu-24.04.dockerfile` 改 cross-build，可一次出 amd64 + arm64

**進度：** 待 §K.1 通過再開工。

### §K.3 macOS client（optional，hw-pending）

上游 `build-win-mac.yml` GitHub Actions matrix 含 macOS-15 + Qt 6.10.2 + create-dmg；產出 `Moonlight-X.Y.Z.dmg`。本地 fork 沒 Mac 機器驗測，**won't-add** 直到使用者有 mac dev 環境。

---

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 完成的事項移到 git log（`vX.Y.Z: §X.N — short summary` commit message 是唯一紀錄來源）
- 新發現的債照 §A.N / §J.N 風格加進來
- 真的不打算做的（§A.7）就明確寫「不該清」並給理由
