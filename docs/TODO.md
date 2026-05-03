# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **Active (主戰場)** | **§J.3.e** SW Vulkan path 持續優化 | 1080p120 × 3 codec 全 PASS；4K AV1 SSE2 後 62→76fps；4K H.264/HEVC decoder-bound（CPU 上限）|
| **Diagnosed (not LAN)** | **§J HEVC 1440p decoder-throughput cap** | pktmon 雙端 pcap 證實 wire 0 loss；root cause 是 RS_VULKAN + VkFrucRenderer SW HEVC 1440p decode throughput ~50fps；非 FRUC 預設 D3D11 + DXVA HW decode 不受影響 |
| **Done + open hw-pending** | **§I.D** Android Vulkan FRUC async compute | D.2.0–D.2.5 全部 verified on Pixel 5；剩自然 45fps→90Hz ideal 1:2 比例 — Pixel 5 panel 鎖 60/90Hz、GameManagerService 鎖 60fps，需 LTPO panel hw 才能驗 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 3d.6 — AV1 native VK decode grey | 9 個 parser bug 修完仍 grey；NV driver 596.36 + AV1 vkCmdDecodeVideoKHR 黑/灰，要 RenderDoc + vk_video_samples diff，AV1 預設走 libdav1d SW 不擋使用 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 1.7 ONLY-mode NVDEC device-lost | 5 個變體都繞不過，NV 596.36 結構性 bug，預設 PARALLEL 穩 |
| **Active (long-running)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗 |
| **Low** | **§F** DirectML 搬 D3D12 / command bundles | 4K120 real-time 才需要 |
| **Low** | **§G.1** RIFE v1 11-channel | A1000 launch overhead bound (§G.3 negative result)；RTX 30/40+ 才有意義 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Low** | **§D** HelpLauncher URL → 結構化 docs | docs/setup_guide.md + docs/troubleshooting.md 已寫；HelpLauncher 切過去等 doc site stand 起來 |
| **Done** | **§E** Android icon 三處驗證 | home / recents / splash 全在 Pixel 5 (Android 15) 確認正確 |
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

## §E. moonlight-android Icon 補完

**狀態：** ✅ 已驗 on Pixel 5 (Android 15) — home screen launcher、recents screen card、splash screen 三處全部正確使用 VipleStream "V" 圖示（lime on black circle）。Android 12+ themed icon (Material You) 在使用者啟用「Themed icons」時會自動切換為 `<monochrome>` 黑白剪影；預設關閉時用全彩 adaptive icon。`docs/_drafts/android_icon_eval/` 留有截圖佐證（不入版控）。

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

### §J HEVC 1440p120 decoder-throughput cap（**diagnosed 2026-05-03，root cause 不是 LAN**）

**現象：** RS_VULKAN + VkFrucRenderer (SW HEVC decode) 跑 1440p120 時 client 只收到 ~50 fps real。預設 RS_D3D11 + DXVA HW decode 在同樣 1440p120 順跑滿 120fps。

**2026-05-03 雙端 pktmon pcap 結論：wire 完全乾淨，cap 在 client decoder pipeline。**

| Stage | 工具 | 量到的 rate | loss |
|---|---|---|---|
| Server NVENC | `[VIPLE-NVENC-RATE]` | 122 calls/sec | — |
| Server broadcast pop | `[VIPLE-BCAST-RATE]` | 122 fps × ~38 Mbps | — |
| Server NIC TX | pktmon `--comp <id>` (physical NIC, dup_factor=1×) | 13525 packets / 6.6s, 2040 pps | **0 missing** |
| Client NIC RX | pktmon (UAC elevated, all NDIS 5 layers, 5× dedup) | 35636 unique seqs / 13.3s, 2682 pps | **0 missing** |
| Client received | `[VIPLE-NET]` | ~50 fps | — |
| Client networkDropped | `[VIPLE-NET]` | ~34–51 fps | — |
| Client total = received + networkDropped | `[VIPLE-NET]` | ~85–105 fps | — |
| Vulkan upload + present | `[VIPLE-VKFRUC-Stats]` | 46–52 fps | totalfn ~1ms |

**真正 root cause：**

- libavcodec HEVC SW decode（FRAME threading × 8 thread）在這台 mobile CPU 上 1440p HEVC 的 throughput 上限就是 ~50 fps。1080p HEVC 11.18ms p95 的測量結果在 1440p（pixel 1.78×）對應 ~20ms，但實測 wall-clock decodeMean 90–110ms（FRAME-threading pipeline depth latency；steady-state throughput 還是 ~50fps）。
- `networkDropped` 是 moonlight-common-c 的 frame-number gap 計數，**不是封包遺失**。是 decoder back-pressure stall 住 RTP draining → FEC reassembly window 內 partially-formed frame 被 evict → 下一個收完的 frame 看到 frameNumber 跳號 → 計入 networkDropped。
- Vulkan upload path 本身沒問題：`totalfn ~1ms`（SSE2 UV interleave 後 mem_UV ~220us、submit ~200us）。

**為什麼 Vulkan + FRUC 強制 SW decode：** [`ffmpeg-renderers/vkfruc.cpp`](../moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.cpp) 在 ctor 標 `[VIPLE-VKFRUC-SW] forcing software decoder` — FRUC ME/warp shader 需要 CPU-side frame 做 staging buffer upload，HW decoder 沒有方便的 zero-copy 路徑進 Vulkan。Phase B（D3D11/NVDEC → Vulkan shared image）已知 NV 596.84 device-lost，見 `project_phase_b_dead_end.md`。

**已嘗試但無效（2026-04-30~05-02）：**

- Round 1: `RTP_RECV_PACKETS_BUFFERED` 2048→8192 + always-on getsockopt 驗證，OS 真的給滿 11.5MB buffer，networkDropped 沒改 — **不是 socket overflow**
- Round 2: `VIPLE_SMOOTH_PACING=1` env opt-in（Sunshine ratecontrol line-rate→bitrate-shaped），networkDropped 沒改，且 AV1 1080p120 退化 120→94fps（I-frame 需要 fast burst），確認不能設預設 on
- Round 5: FEC 10%→35%，networkDropped 變更糟（更大 frame size → 更多 burst 壓力），revert
- 之前推估「server NIC↔client NIC 間 16% 封包遺失 → 50% frame 遺失」**錯了** — pcap 證明 wire 0 loss

**fix path（依工時排序）：**

1. **小：UI / log warn** — 偵測 `received_fps < 0.75 × target_fps` 連續 5s 時印 `[VIPLE-NET-WARN]` 並（可選）overlay 提示「客戶端解碼跟不上 — 改用預設 D3D11 渲染器或降低解析度」。純診斷增益，不改行為。
2. **中：1440p120 + Vulkan + FRUC 自動回退** — `Session::populateAppropriateBitrate` 或 renderer-selection 階段 detect 該 combo，提示使用者並建議 1080p120 或 1440p60。
3. **大（重啟 Phase B）：HW decode 直接灌 Vulkan via shared image** — 解掉 SW decode 瓶頸的根本路。要重新攻 NV 596.84 ID3D11Fence + VkSemaphore 同步路徑或 ID3D12Device intermediary（§J.1 路線 A）。已知 dead-end，但今年 NV driver 已從 596.84→596.36→更新版，值得 re-verify。

**用 D3D11 預設不踩這個坑：** 一般 user 不選 Vulkan + FRUC 就沒影響；目前 v1.3.308 起 Vulkan 標 [實驗性]。**FRUC + 1440p120 + HEVC** 才是踩 cap 的特定組合。

**已就位 instrumentation：**

- `[VIPLE-NVENC] encode_frame total / wait_async_evt / lock_bitstream / bitstream_size` (5s window)
- `[VIPLE-NVENC-RATE]` 每 5s N calls/sec
- `[VIPLE-BCAST-RATE]` 每 5s popped fps + Mbps
- `[VIPLE-NET]` client-side received / decoded / networkDropped / decodeMeanMs / hostLatency 每秒一行
- `[VIPLE-VKFRUC-SW-PROF]` mem_Y / mem_UV / fence / submit / present 五段 percentile
- `Actual receive buffer size: N (requested: M)` + 不到一半時的 WARNING — moonlight-common-c PlatformSockets always-on
- `VIPLE_SMOOTH_PACING=1` env opt-in（已驗證會傷 AV1 burst，**不能設預設 on**）
- [`docs/diag_wire_loss.md`](./diag_wire_loss.md) — pktmon 雙端 capture + tshark 序號 dedup 分析 SOP。任何「`networkDropped > 0` 是不是 LAN 丟包」的疑問，**第一步**走這套流程：兩端 capture，`Missing packets: 0` 就確認 wire 乾淨，往 OS UDP / FEC reassembly / decoder throughput 找。具體 .ps1 / .py 工具在開發者本機（IP / 路徑 / NIC id 個人化），不入版本管。

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

### §J.3.e.X 手刻 RIFE Vulkan pipeline（long-term）

**動機：** 跳過 ncnn 整個依賴，自己用 ~10-15 個 raw VkPipeline 實作 RIFE forward；權重 mmap 進 VkBuffer，PipelineCache binary 開機建一次。

**預期：** latency 從 Path B 的 ~12ms 壓到 ~3-5ms。

**觸發條件：**
- Path B 量測後 PCIe staging cost 占 budget 30%+
- 或要推 1080p/4K
- 或要砍 ncnn DLL 依賴

### §J.3.f AV1 Vulkan hwaccel via ffmpeg（long-term）

重 build ffmpeg 把 libdav1d → vulkan_hwaccel 註冊（或改用 ffmpeg 內建 av1 decoder 加 vulkan）。需要 ffmpeg source + build 環境。**注意：跟 §J.3.e.2.i.8 Phase 3 是不同路線** — Phase 3 是 raw vkCmdDecodeVideoKHR + 自己跑 nvvideoparser，§J.3.f 是 ffmpeg 包裝。

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

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 完成的事項移到 git log（`vX.Y.Z: §X.N — short summary` commit message 是唯一紀錄來源）
- 新發現的債照 §A.N / §J.N 風格加進來
- 真的不打算做的（§A.7）就明確寫「不該清」並給理由
