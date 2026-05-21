# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

只列**待辦 / 進行中 / 等硬體 / 等驅動**的條目。已完成、negative result、won't-fix 移到下方對應章節 OR git log。

**戰略：** §B / §B-NVOF / §B2 自家 ME→warp→blend pipeline ceiling = ~20-30% warp ratio。NvOFFRUC.dll 47.7% 是架構性差距無法 tweak 追平。**ML-based interpolation (§J.3.e.X Native RIFE Path β)** 是 ceiling 60-80% 的唯一路 — **v1.4.0 已 ship**。current status: §B/§A' = Linux/AMD/Intel fallback；Path β = NV Vulkan flagship quality (beta opt-in)。

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **Active (verify)** | **§J.3.e.X Path β.11** FRUC interp quality 微調 | **§β.11.b DONE v1.4.194**：EDGE_AWARE_MV_THRESHOLD 改為 push constant（push_constant 40→44 bytes，`p.edgeMvThreshold`）。預設 8.0f，override via `VIPLE_VKFRUC_EDGE_MV_THRESHOLD=N`。待使用者實測：keep 8.0 / 試 N=4（更多邊緣保護）/ 試 N=12（更 smooth）/ 加 UI slider |
| **Deferred (test-machine-bound)** | **§B Phase B restart** D3D11VA HEVC → Vulkan shared-image bridge | v1.4.184 B4 fenceless + v1.4.185 device flags 對齊 D3D11VARenderer。**阻塞原因：AMD 780M 測試機 HEVC D3D11VA 本身不可用**（VK_KHR_video_decode_h265 MISSING + D3D11VA/DXVA2 HEVC 全 SW decode，driver 最新仍如此）。Code 正確，需換一台「VK_KHR_video_decode_h265 missing 但 D3D11VA HEVC HW decode 可用」的 AMD 機器才能驗 B7 import + B9 FRUC chain |
| **Active (test pending)** | **§B-NVOF Phase B autotier 整合** NVOF 當 NV best tier | Phase B prerequisite 全套 ship — v1.4.117 §J.3.e.2.i.52 VK_SHARING_MODE_CONCURRENT for flow images + v1.4.118 §J.3.e.2.i.53 NVOF dispatch on worker thread (Option B 消 SDK CPU latency) + v1.4.138 §J.3.e.2.i.54 in-flight/drop counter。等 user PixArk 20+ 分鐘實測 v1.4.118+ handoff bimodal 是否消除（預期 stable <2ms vs v1.4.116 134us/15ms bimodal）+ decode latency 不再 cycle。若 OK → reapply early-kickoff 拿回 5% async perf win + 把 NVOF 列 NV best tier。若 bimodal 仍存在 → 代表純 GPU NVOF execute variance 主導，需 Option E (NVOF skip 機制) |
| **Active (follow-up)** | **§R2 PASSIVE FRUC** ratio controller | v1.4.169 §R2-η-2 ratio alignment gate + v1.4.170 §R2-θ 對齊 display Hz + 動態調 server fps via LiRequestFpsChange。**v1.4.186 §R2-ζ-3** extreme floor：latency > budget×2.0 連 3 frame → 直接 floor T0（補全 T5/T4/T3 不需等 320 frame 的路徑）。(b) getRendererDisplayHz() 分析完成：其他 renderer 回 0 是正確行為（只有 VkFrucRenderer 啟用 PASSIVE FRUC）。剩：alignment gate 生效後觀察 latency pattern |
| **Active (verify pending)** | **§M.parity Android wire/UX** | v1.4.160 ship W1-W5+U6+U7 (ServerCodecModeSupport bit field + DisplayMode 陣列 parse + 4:4:4 chroma variants + packetSize 讀 pref + enableYUV444 + in-session tap-to-cancel file transfer + Custom WxH 解析度)。Pixel 5 待 install + adb logcat 確認 `[VIPLE-PARITY] supportedVideoFormats masked by server caps 0x... -> 0x... (SCM=0x...)` 真實生效 |
| **Active (verify pending)** | **§K.dd.revert.1** display device revert ghost-protection | v1.4.156 ship `settings_manager_revert.cpp` ghost-check skip 避免 `setAsPrimary` 卡死 listener thread group (192.168.51.226 上 22:33:42 + 23:54:38 兩次 repro)。待 user 把 `dd_configuration_option` 從 `disabled` 改回 `ensure_only_display` + stream + disconnect 確認不再卡死 |
| **✅ Completed** | **§K.linux VAAPI→Vulkan bridge** (DMA-BUF interop) | **✅ K.3 PASS (2026-05-21)** v1.4.188：Tailscale Vega 10 實機通過。frame#0~#600 全數 OK（無 crash，無 device-lost）。frame#120+ dual=1 fruc=1（autotier 升 T2，FRUC dual-present 正常）。VAAPI LINEAR NV12 → m_SwFrucNv12Buf → FRUC compute chain → dual-present end-to-end 通。 |
| **Active (P0, post-v1.4.35)** | **§N.5.bug** Android file transfer SSL `TLSV1_ALERT_INTERNAL_ERROR` post-handshake | (b) server stale state ✅ FIXED v1.4.34 (sweep_stale_locked). (a) 仍 active — v1.4.42 Scenario A diagnostic ship (`xfer_result`/`xfer_blob_post`/`xfer_progress` 加 try-catch ENTER/EXIT/EXCEPTION log + per-MB progress)。等 user 在 Android 重現 + 拿 server log 確認 alert 是 request 起前 / chunk loop 中 (第幾 MB) / finalize 後哪一段送出 |
| **Active (verify)** | **§N.5** moonlight-android FileTransferClient runtime | 建置 + JNI hooked + Game.java lifecycle + `MediaStore.Downloads` scoped-storage 都通；待實機在串流 session 跑 Send/Receive flow 看 Pixel 5/9 specific Android quirks (backgrounding OkHttp 連線 / power manager 影響 polling 等) |
| **Active (post-v1.4.12)** | **§M.2** Phase 2 雙 user 並發 streaming 驗測 | Ubuntu VM 上 per-user systemd + Xdummy + PipeWire 端到端跑通；NVENC 並發等實體機到位 |
| **Active (long-running)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | per-slot buffer 改善大半，殘留小 race 等 J.5 整體切換時補完，不擋使用 |
| **Active** | **§J.3.e** SW Vulkan path 持續優化 | 1080p120 × 3 codec 全 PASS；4K AV1 SSE2 後 62→76fps；4K H.264/HEVC decoder-bound（CPU 上限） |
| **Active (β.10)** | **§J.3.e.X Path β.10** Linux / AMD / Intel 平台覆蓋 | **§β.12.fix PASS (2026-05-21 v1.4.193)**：Vega 10 RIFE native init OK，tiled Conv path（無 coopmat）。`RIFE init OK inferDim=128x128`。K.3 frame#720 dual=1 fruc=1。ncnn bug 確認：fp16_arithmetic=1 → WMMA opcode emit，無論 net.opt；fix 在 buildPipelineCache 提前 skip Conv2D_CoopMat（留 null → dispatchConvolution 走 tiled）。剩：視覺品質主觀確認（tiled vs coopmat quality diff）+ Intel iGPU 驗測。 |
| **Maintenance** | **§B / §B-NVOF / §B2** 自家 ME→warp→blend pipeline | A' 修完 (luma + range + consensus-max) 從 0% 推到 7-23% warp ratio；Path B 全套不做（ceiling ~30% 跟 ML 60-80% 沒法比）；現狀作 Linux/AMD/Intel fallback 已堪用 |
| **HW-pending** | **§I.D** Android Vulkan FRUC async compute | D.2.0–D.2.5 已 ship + Pixel 5 verify；剩自然 45fps→90Hz ideal 1:2 比例 — 需 LTPO panel hw 才能驗 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 3d.6 — AV1 native VK decode grey | 9 個 parser bug 修完仍 grey；NV 596.36 driver bug；§J.3.f ffmpeg 路徑 cover AV1，deprecated |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 1.7 ONLY-mode NVDEC device-lost | 5 個變體都繞不過，NV 596.36 結構性 bug，預設 PARALLEL 穩 |
| **Deferred** | **§K.4** Wayland XDG portal teardown root-cause | `Restart=always` 緩解已 ship；需可重現 streaming 環境（GPU Linux 機）才能修進 wayland.cpp/portalgrab.cpp 的 EPIPE 路徑 |
| **Medium** | **§F** DirectML 搬 D3D12 / command bundles | 27.6% warp ratio 已驗，但 latency 慢「不堪用」；Path β 已是 Vulkan 端 ML flagship，優先級降 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗；Path β 走通後優先級降 |
| **Medium** | **§K.2** Raspberry Pi 5 client (aarch64) | Pi 5 + V3DV mesa Vulkan + V4L2 HW decode + DRM/KMS render；沒 CI prebuilt；FRUC backend 全 disable |
| **Low** | **§G.1** RIFE v1 11-channel | A1000 launch overhead bound (§G.3 negative result)；RTX 30/40+ 才有意義；Path β shipped 後優先級降 |
| **Low** | **§J.3.e.Y 4Y.5b** native RIFE activation fp16 | 2026-05-09 嘗試 + revert：256x256 chain +1.7ms 是負收益。重做要先解 §β.5.3 D 全套讓小 dim 真的可選 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Low** | **§D** HelpLauncher URL → 結構化 docs | docs/setup_guide.md + troubleshooting.md 已寫；HelpLauncher 切過去等 doc site stand 起來 |
| **Low (partial-fixed v1.4.40)** | **§N.7** Sunshine Linux fs_picker (zenity) | v1.4.40 ship stub 讓 Linux server build link；real zenity subprocess 仍 TODO — 需 native Linux GUI session 驗 DISPLAY / DBUS_SESSION_BUS_ADDRESS env 透過 sunshinesvc 正確繼承 |
| **Low (post-v1.4.20+)** | **§H.4-perf** VkFrucRenderer AMD draw-time perf | OSD 標籤已 ship；m_FrucMode=false ~13ms 偏高，要 user `[VIPLE-VKFRUC-GPU-PROF]` log 5-10 行量化才能 dive in (AMD-only，NV 未複現) |
| ❌ **Won't-fix (Linux mouse scale)** | **§N.5.linux** Linux mouse scale 跟 Windows 不一致 | v1.4.171-175 連環嘗試 SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE=0 / SPEED_SCALE=150 hardcode / Settings UI slider 50-300% 全失敗 (user 實測 ubuntu RADV/Vega 10 「不管用多少都亂飄」)。User 明確 ack「滑鼠的部分不改了, 用原本的就好」。**Linux client mouse forward works** (SDL3 RawMotion raw mickey 直接送 host)，跟 Windows native 略慢但穩定可用。完整嘗試史在 `input.cpp` 註解。真實 root cause (Linux raw delta vs Windows raw delta scale 差異) 留 future task。Overlay CJK font ✅ FIXED v1.4.41 |
| ❌ **Won't-fix (NVOF SDK limitation)** | **§B-NVOF Option D** NVOF buffer 輸出 取代 image 輸出 | 2026-05-18 spike NV OF SDK header (`nvOpticalFlowVulkan.h`) 確認 `NV_OF_REGISTER_RESOURCE_PARAMS_VK` struct 只有 VkImage 欄位 + 唯一 register API `nvOFRegisterResourceVk`(image only)。SDK 沒提供 buffer 變體。NVOFA HW 內部用 image tiling layout 跑，SDK 把硬體限制硬綁進 API |

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

D.2.0–D.2.5 全部 ship + Pixel 5 / Adreno 620 真機 verify 過。詳情見 git log（`3dc860a` / `81e50d4` / `0217426` / `1a5cc5b` 等）。

**Dual entry threshold** 已從 1.05× 放寬到 1.40×（`1a5cc5b`），60fps@90Hz 預設進 dual 92.6%。

### 還沒做的 — 自然 45fps source on 90Hz panel ideal 1:2 比例測試

Pixel 5 panel 只支援 60Hz/90Hz mode，GameManagerService 又把 app default frame rate override 為 60fps，現有 hardware combo 拿不到 ≤ 45fps stable input。要驗 ideal 比例得換 LTPO panel 手機（S24+/Pixel 8）。這是 hardware-pending，**不是 client codebase 缺東西**。

### 鐵律

1. 每 Phase 都要有 baseline 對比。沿用 `scripts/benchmark/android/`，量出來不如預期就停。
2. **GLES path 至少保留到「Vulkan 穩定 6 個月」之後才移除**。期間 FRUC bug 修兩次（GLES + Vulkan）是已知 +20% 維護成本。

### 已就位的診斷工具

- `scripts/benchmark/android/android_baseline.sh` — thermal + fps + jank 採集（**local-only**，scripts/ 自 v1.4.0 起 gitignored）
- `scripts/benchmark/android/analyze_baseline.py` — 30s bucket fps + pixel-thermal 高頻交叉驗證（**local-only**）
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

**2026-05-08 strategic priority bump — Final.3b 升 active：** §B / §B-NVOF
量化驗測（compare_fruc_engines warp ratio）發現我們所有自家 ME→warp→blend
pipeline ceiling ~30%，跟 NvOFFRUC.dll 47.7% 沒法追平（架構性 N-stage gap）。
**ML-based interpolation 是唯一 ceiling 60-80% 的路**。Final.3b 從 deferred
升到 active，原 deferral reason（Linux test VM 驗測前不啟動）由「production
default 切換前才需要 Linux 驗測」取代 — 現在先做 Windows production
integration（env-var gated default-OFF），Linux 驗測併入後續 ship 流程。

**Final.3b 工程計劃：**
1. 在 NcnnFRUC 加 `RifeNativeExecutor` 替代路徑（drop-in，相同 input/output 介面）
2. 由 env var `VIPLE_RIFE_NATIVE_VK_PROD=1` 切換 ncnn ↔ native，預設 ncnn 不變
3. Per-frame 跑 native inference，輸出餵下游 D3D11→Vulkan upload + present 跟 NCNN path 共用
4. 第一輪只在現有 RIFE-v4.25-lite model 跑 1080p / 720p，4K 等 §J.3.e.Y 4Y.5b/4Y.6 latency 優化降到 budget 內再 unlock

**預期 warp ratio**：60-80%（基於 DirectML 同 RIFE 模型 27.6% baseline + 推估 native 沒 D3D 框架 overhead 的進一步贏面）。

**2026-05-08 Path β 實做 + 量測（commits `67a7c89` β.1+β.2 / `20507da` β.4）：**

NCNN drop-in 那條（Final.3b）撞到 NV driver 596.144 dual-VkDevice
`VK_ERROR_INITIALIZATION_FAILED` —— 已被 **Path β** 取代：直接把
`RifeNativeExecutor` 接到 `VkFrucRenderer` 既有的 VkInstance/VkDevice/
universal queue，繞過 NCNN 整條路。架構落地的三個 commit：

- β.1 (env-var gate `VIPLE_VKFRUC_NATIVE_RIFE=1`) + β.2 (chain swap via
  新的 `runInferenceGpu(cmd, slotIdx, in0, in1, t, out)` API + per-slot
  descPool —— `runFrucComputeChain` 在 stage 0 NV12→RGB 之後 RIFE 接管，
  ME/median/warp 完全跳過)
- β.4 (bilinear down/up wrapper)：1920×1080 → 256×128 → RIFE → 256×128
  → bilinear up → 1920×1080。default infer dim 256×128（更高 dim 撞
  RIFE-v4.25-lite 自身的 layer `Add_503` shape constraint —— a.h round
  到 next /128 multiple，跟 b.h 不對齊；inferShapes 應該也跟著計算 padding，
  TODO 修）

**RIFE 真的在跑**（live test stderr 出 `[VIPLE-VKFRUC-RIFE-β] chain swap
active`，無 SDL Error/Warn，無 dispatchBinaryOp fail）.

**Quality 量測 (256×128 default)：** verify_dump_interp.py 抓 5 對
real/interp 看 `diff(real_N, interp) / motion_mag` score 都 1.7-2.7（理想
1.0 = 完美 midpoint）.  乍看 FAIL，但 root cause 拆解後不是 RIFE 錯：

  pair_adj (mean |real_N - interp|) ≈ 4.04
  bilinear roundtrip 1080p→256×128→1080p 自身就 4.48 mean abs loss

也就是 **bilinear 7.5× downscale → upscale 自帶的 detail loss 已經完整
解釋住觀察到的 residual** —— RIFE 在 256×128 推論本身是有效 midpoint，
但接出來的圖被 bilinear up smooth 掉細節（高頻內容遺失）使得跟 1080p
real frame 的 pixel-level diff 偏大。

**已 ship（commits 67a7c89 → d5f2812 + 3.4 v6 dump-skip）：**

- β.4 v4 解開 Add_503 root cause：`Resize_47` hardcoded /32 + 兩個 stride-2
  conv = encoder /128 → decoder ×128，inputH 不是 /128 整除時 a.h round 上
  跟 b.h 不對齊。修法 inferW/H round 到 /128 multiple（不是 /32）。
- β.4 v5 timing instrumentation：runRifeNativeStage 寫 ts[2..4] 進
  m_FrucTimerPool，[VIPLE-VKFRUC-GPU-PROF] 顯示 nv12rgb / DOWN / RIFE / UP /
  copy 分別 latency.
- β.4 v6 dump-skip：m_RifeNativeReady 時跳過 §B-DUMP 的 vkCmdCopyBuffer
  流程（避免 m_FrucMvFilteredBuf 未寫 + interp barrier mask 對不上）。

**Latency table on RTX 3060 Laptop (live test 量得)：**

| inferDim | RIFE inference | total chain | 60fps DUAL 吃得起？ |
|---|---|---|---|
| 256×128 | 11.9ms | 12.4ms | ✓ 60fps 穩 |
| 512×256 | 29.1ms | 29.7ms | ✗ ~33fps drop |
| 768×384 | ~50ms 推估 | ~51ms 推估 | ✗ |
| 1024×512 | ~80ms+ 推估 | ✗ |

預設 256×128。更強 GPU 可 `VIPLE_VKFRUC_RIFE_INFER_DIM=512` 拉到 quality
明顯提升的 dim（必須 /128，否則撞 Add_503）。

**已知問題：**

- ~~**Path β 30-90s device-lost crash**~~ ✅ **RESOLVED v1.4.0 (β.6 stability fix)** — Nsight Graphics `nv-aftermath-format -p VipleStream.pdb -g <shader-dbg>` symbolize 出 `fragment_01` 不是 `vkfruc.frag` 而是 `vkfruc_overlay.frag`，page-fault site 是 `VkFrucRenderer::drawOverlayInRenderPass` 的 use-after-free（`drainOverlayStash` 在 perf overlay surface dim 變動時 immediate destroy 舊 `m_OverlayImage[type]`，但 Pacer renderThread 同時可能有 2-3 個 in-flight cmd buffer 還會 sample 舊 view）。Path β chain 14ms 比 block-match 4ms 多 in-flight cmd buffers，所以一個 overlay resize 就有 race window。修法：`drainOverlayStash` line 3317-3341 在 destroy 舊 image 前 `vkDeviceWaitIdle`。Cost: overlay resize 才 fire，一次 stream 通常 0-2 次。**驗測 7m49s 連續 0 crash 0 dump 0 device-lost log**。詳見 commit `a72b886`。

- ~~**SW decode mode + §B-DUMP 加速 crash**~~ — 跟上一條同根因（drainOverlayStash race），β.6 fix 一併解。`commit 2a5732e` 的 dump-skip guard 仍保留作 belt-and-braces。

**還沒做的下一步候選：**
1. 修 SW-mode device lost — 使 §B-DUMP 自動驗測 work
2. **§β.5.3 D 全套** — inferShapes / dispatchBinaryOp 處理 asymmetric
   center-crop pattern 解開「inferDim 不對齊 /128 但仍 valid」的情形
   （解開 192/320/448 中間 dim + 讓 4Y.5b 重做時有真的可選的小 dim）.
   目前 D-lite UP-rounding (β.5.3, `9d4c237`) 是 stop-gap，副作用是
   1080p source 配 cfg 256 變 256x256 不是 256x128（pixels 翻倍 + 4Y.5b
   negative result 直接源於這個）.
3. ~~β.3 TRIPLE 支援~~ ✅ **β.9 Phase 1 ship in v1.4.6 (`99f86b5`)** —
   t=1/3 + t=2/3 兩次 inference 已落地，互斥鎖解除. 剩 β.9 Phase 2
   (graph executor timestep-shared frontend split) active，使 chain × 2
   從 200% 降到 ~130%
4. 找更聰明的 upscaler 取代 bilinear up，減 8× 上採 blur — ✅ **β.5.2
   Catmull-Rom bicubic ship in v1.4.1 (`a8b3204`)** for flow + mask
   upsampling. warp shader 對 source RGB 仍用 bilinear，可進一步升 bicubic
   （使用者回報的「補幀畫面糊」可能是這條造成）→ §β.11 active item

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
- **4Y.7 Conv→ReLU dispatch fusion** — C.1 (Conv→Mul channel-bcast beta)
  已 ship in 0e6240a (v1.4.1)；Conv→ReLU 合併還沒做，預估 chain -10%

**Negative result (走錯後 revert，2026-05-09)：**

- **4Y.5b activation fp16 storage** — wip/4Y.5b-activation-fp16 branch
  留檔 (82afee5)。256x256 (1080p 配 D-lite UP) 場景下 chain 反而 +1.7ms
  是負收益，跟 4Y.5a postmortem「weight L1-cache-bound 不是 bandwidth
  bound」結論吻合。要重做必須先解 D-lite asymmetric center-crop（§β.5.3
  D 全套）讓 256x128 真的可選；目前 cfg=256 強制變 256x256，bandwidth
  mention 的 ~485 blobs × 2 reads + 1 write 跟 weight L1 比仍不是 dominant

**Final.3b 接 NcnnFRUC：** 仍 deferred until Linux env。預估 4-6h 工程，
medium-high risk（動 production hot path）。

### §B Vulkan HW path FRUC integration（**📦 maintenance 2026-05-08，A' 收尾、Path B 不啟動**）

**2026-05-08 strategic decision — pipeline 收尾 + ML pivot：**

`compare_fruc_engines.ps1` 量化驗測 7 engines 後得 warp ratio：

| Engine | Warp Ratio | 說明 |
|---|---|---|
| 03 d3d11_nvidia_of (NvOFFRUC.dll) | **47.7%** | 標竿，NV 私有 SDK 全套 N-stage pipeline |
| 04 d3d11_directml (RIFE ML) | 27.6% | ML-based，但「慢到不堪用」latency |
| **06 vkfruc_nvof (A' 修後)** | **7.5%** | A'.2 consensus-max NVOF converter |
| **01 vkfruc_bm (A' 修後)** | **6.8%** | A'.1 luma + range expansion |
| 02 d3d11_generic (HLSL 沒改) | 3.0% | A' 沒動到 D3D11 HLSL shader |
| 05 d3d11_ncnn | 2.4% | known broken (§B-DUMP NCNN all-zero output) |

**A' (commit `XXX`，2026-05-08)** 兩條 shader 改動：

- **A'.1** [plvk.cpp ME shader](../moonlight-qt/app/streaming/video/ffmpeg-renderers/plvk.cpp)：R-channel-only census → luma census；diamond steps `[3, 1]` (±4 px) → `[32, 16, 8, 4, 2, 1]` (±63 px)
- **A'.2** [vkfruc.cpp NVOF converter](../moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.cpp)：4×4 average → consensus-max with noise floor

具體量化：MV stats `max|x|` 從 5 LSB Q1 (=2.5 px) → 58 LSB Q1 (=29 px)，符合 testufo / 遊戲場景 30+ px/frame UFO motion 的真實值。

**Path B 預估完成後仍只 ~20% warp ratio**（occlusion mask + reverse-flow + multi-scale + Q5 sub-pixel 全做完，[詳細評估在這次 commit 的對話中]），跟 NvOFFRUC 47.7% / ML 60-80% 都差太遠。**結構性 capacity gap**：NvOFFRUC 是 8-stage 整合 pipeline + NV 多年調教，我們 4-stage stub-level 實作；加 4 stage 變 6-stage 仍不如 8-stage。

**結論：A' 是現有 ME-based pipeline 的合理 ceiling。Path B 不啟動，資源轉投 ML 路線（§J.3.e.X Final.3b production integration）。**

---

直到 §B 之前，RS_VULKAN HW mode 雖然 frucMode/dualMode 都自動為 true，
但 `renderFrame()` 從來沒接 `runFrucComputeChain` 或 dual-present —
等於 production HW path 完全沒在補幀（解釋了使用者觀察「testufo
30→60 補幀沒效果」的根因）。

| Step | Commit | 內容 | 狀態 |
|---|---|---|---|
| A baseline | — | 1080p60 SW path 量 GPU-PROF baseline (15 samples) | ✅ nv12rgb=410 me=411 warp=214 total=1248us |
| B1a | `03a962f` | HW path 加 image-to-buffer copy + FRUC chain dispatch | ✅ total=750us（比 SW 快 40%） |
| B1b | `a86df9f` | HW path 加 dual-present（real + interp 兩次 swapchain acquire/render/present） | ✅ overlay「已啟用」、cumul real:interp = 1:1 |
| B-quality (a)(b)(d) | `cfe5396` `1f774b9` `24e9895` | 演算法品質里程碑（見子章節） | ✅ SSIM 0.88 → 0.998 |
| B-quality benchmark infra | `8898e1d` `fa2a2fd` `1cf94ed` `c18d0a6` | 2-stage 抖動量測腳本 + analyze_motion.py + DIAG-OVERLAY log | ✅ ship |
| **B2** TRIPLE 60→180 | `bc88eba` | infrastructure 全套（兩個 interp output / 第三 swapchain acquire / pacer +2） | ✅ env-var opt-in，**UI 整合 pending** |
| **B-NVOF** | Phase 1..7B + Phase A v1.4.108-111 | NVIDIA Optical Flow Vulkan extension 取代 software block-match ME | ✅ **Phase A production ship v1.4.108** (NV GPU 預設 ON + UI + PROF + 30-consec demote); ❌ v1.4.110 early-kickoff failed (data race on m_NvOfFlowImage), revert v1.4.111; 🔒 **Phase B autotier (NVOF 當 NV best tier) block 在 m_NvOfFlowImage double-buffer future work** |

#### §B-quality 補幀演算法品質（主要里程碑：commit `24e9895` algo_d）

**(d) real path 改走 compute-NV12→RGB（治本，2026-05-XX `24e9895`）**

dual-present 兩條 path 之前用不同 NV12→RGB conversion：interp 走 compute
shader 寫 fp32 buffer 經 vkfruc_interp.frag 顯示；real 直接吃
`VkSamplerYcbcrConversion`。色彩矩陣 / chroma 上採樣 / sub-pixel 對齊都
可能不一致，30Hz dual-present 交替時看到「同幀內容、不同 RGB convert」
造成的 Y 軸抖動。先前的 warp 演算法疊代（cheap-adaptive / fixed 50/50 /
d3d11 Quality / no-MV）全抖 — 因為跟 warp 完全無關。

修法：real render pass 也改吃 `m_FrucCurrRgbBuf`（compute shader 的 fp32
output），跟 interp 走同條 path。`VIPLE_VKFRUC_REAL_USE_CRGB` default ON，
escape hatch 設 0 退舊 ycbcr。Single-mode 不抖（沒 dual-present），維持
ycbcr sampler 路徑。

量化（testufo 1080p60 1920×40 band，60s）：

| run | SSIM | OF_30Hz |
|---|---|---|
| baseline_v3 (median noop) | 0.88 | 5.5% |
| algo_a (median ON, `cfe5396`) | 0.88 | 4.4% |
| algo_b (block 8→16, `1f774b9`) | 0.84 | 1.93%（block 16 對水平 motion Y 軸引 noise，後 (d) revert） |
| **algo_d (real_use_crgb, `24e9895`)** | **0.998** | **2.75%** |

User 主觀：「不抖、顏色看不出異常」。block_size 16 在 (d) 一併 revert 回 8。

**還沒做的（B-quality 收尾）：**

- (c) 從未 commit；blend factor 0.5 hardcoded、occluded/disocclusion 沒
  特殊處理 —— `24e9895` 後改由 §B-NVOF 用 HW optical flow 取代 software
  block-match 來解 ME 噪聲，`(c)` 概念上由 NVOF 路線吸收
- 自然 video 驗測（目前都 testufo trajectory band，非真實內容）

#### §B2 TRIPLE 60→180（infrastructure ship at `bc88eba`，opt-in）

VkFrucRenderer dual-present (60→120) 擴展為 triple-present (60→180)。
每 server frame compute 出 2 張 interp（1/3 + 2/3 點）+ real，3 張
swapchain image present 給 180Hz display。`VIPLE_VKFRUC_TRIPLE=1` 啟用，
gated by m_DualMode + m_FrucMode + 180Hz display，預設 OFF。

關鍵改動 (`bc88eba`)：
- warp shader push constant 6→8 欄，加 `tFraction` 控制 sample offset +
  blend weight（0.5 = DUAL midpoint，1/3+2/3 = TRIPLE 兩張 interp）
- `runFrucComputeChain` TRIPLE 模式 dispatch warp 兩次（不同 desc set +
  tFraction，輸出 `m_FrucInterpRgbBuf` / `m_FrucInterpRgbBuf2`，GPU 並行）
- `renderFrame` 加第三個 swapchain acquire (`m_SlotAcquireSem[][2]`) +
  第三 render pass + submit 4-sem 變體 + 第三 present；order: interp_1 →
  interp_2 → real
- swapchain image count 4→5（防 burst-acquire 3 張撞 caps.minImageCount）
- session.cpp：server fps = user_fps / 3（DUAL 時是 / 2）；toggle FRUC
  在 TRIPLE 模式不發 LiRequestFpsChange（NVENC encoder timebase 不可動
  態升 ~2× 上限會卡頓，Sunshine `video.cpp:2154` 提示需 stream restart）
- `lastFrameInterpolatedCount()` 介面取代 `lastFrameHadFRUCInterp`，
  TRIPLE 回傳 2 讓 effectiveFps 算對（overlay 顯示 ~180）
- `scripts/benchmark/launch_warp.cmd` 加 triple 入口

驗測（1080p60 testufo，`bc88eba`）：
- GPU profile：warp 兩次 dispatch GPU 並行，total chain 0.93ms（DUAL ~0.9ms）
- client present rate：cumul real:interp = 1:2，60+120 = 180 presents/sec
- DIAG tint test：3 張 interp 確實獨立 present，紅 (interp_1) + 藍
  (interp_2) + 正常 (real) 在 60Hz fusion 邊界 blend 為紫色點狀

**已知限制（`bc88eba` 寫進 commit）：** block-matching ME 精度上限影響補幀
品質 — testufo 小快速物體在 disocclusion 邊界 ME 噪聲 → warp ghost。User
主觀對比 60→180 vs 60fps 看不出明顯 smoothness 提升（block-matching 本
質限制，非實作 bug）。下一步：探勘 NVIDIA Optical Flow Vulkan extension
取代 block-matching → §B-NVOF。

**還沒做的（B2 收尾）：**

- Settings UI 整合：`VIPLE_VKFRUC_TRIPLE` env var → SettingsView.qml
  選項（與 fps mode 一起判斷顯示 / 隱藏）；目前只能 env-var 開
- TRIPLE × NVOF cross-test（§B-NVOF Phase 7E，analyze_motion.py 已 fix
  OOM bug `83157fa`，可跑了）
- 自然 video 驗測（非 testufo）

#### §B-NVOF NVIDIA Optical Flow Vulkan（**Phase A ✅ v1.4.108 default ON / Phase B 等實測**）

**動機：** §B2 TRIPLE 跟 §B-quality (d) 後，剩下的補幀品質瓶頸在
software block-match ME 本身（411us / frame，但 disocclusion 邊界
產 noisy MV，跨幀 alternation 高）。NV Ada / Ampere 起 Vulkan 新增
`VK_NV_optical_flow` extension，可用 NVOFA HW 直接給 SFIXED5 (1/32 px)
sub-pixel flow image。整套 §B-NVOF 把 Stage 1 (block-match ME) +
Stage 2 (median) 替換成「flow image → SFIXED5→Q1 converter compute」，
warp shader (Stage 3) 跟 mv_filtered buffer contract 不變，所以 §B-quality
的 blend mode 都自動受惠。

| Phase | Commit | 內容 |
|---|---|---|
| 1+2 | `b9da1cc` | VK_NV_optical_flow extension probe + queue scaffolding |
| 3a+3b | `de2095e` | SDK header import + nvofapi64.dll load + funcList init |
| 3c | `b7a19b5` | OF session lifecycle (`nvCreateOpticalFlowVk` + `nvOFInit`) |
| 3d | `67f6146` | input/output VkImage alloc + `nvOFRegisterResourceVk` |
| 4a | `fcb0e06` | OF queue cmd pool + timeline sem + flow staging buffer |
| 4b | `61ea72e` | `nvOFExecuteVk` smoke test，OF runs each frame，SUCCESS |
| 5 + 4d | `8daf9e2` | SFIXED5→Q1 converter compute shader + chain consumes HW OF flow |
| 6 | `3de8507` | docs entry + 收尾，`VIPLE_VKFRUC_NV_OF=1` opt-in 整段 path |
| 7C+7D | `71d1592` | `VIPLE_VKFRUC_NV_OF_PERF` (slow/medium/fast) + `_GRID` (1/2/4) env，**default grid=2 perf=med 是甜蜜點** |
| 7F | `49b736c` | `nvOFGetCaps` device probe（informational log）+ Phase 7A Q5 嘗試 revert（precision != quality，Q1 quantisation 意外當 temporal smoother） |
| 7B | `c82196e` | async cross-queue：CPU `vkWaitSemaphores` ~3-5ms 阻塞改 1-frame async lag，timeline sem 在下幀 main submit wait list 攔住 race |

**Quality benchmark（testufo 1080p60 60s，2026-05-07）：**

| 配置 | SSIM | OF_30Hz | OF_cv | block_outlier |
|---|---|---|---|---|
| production block-match (algo_d, `24e9895`) | 0.998 | 2.75% | 1.94 | 4.3% |
| NVOF grid=4 perf=med（前預設）| 0.999 | 5.99% | 0.86 | 4.6% |
| NVOF grid=4 perf=slow | 0.999 | 4.25% | 0.78 | 4.8% |
| **NVOF grid=2 perf=med（新預設）🏆** | **0.999** | **2.66%** | **0.86** | 4.6% |
| NVOF grid=1 perf=slow | 0.999 | 3.17% | 1.75 | 4.3% |

NVOF grid=2 perf=med 在三個關鍵指標全面 ≤ production：OF_30Hz 終於不
輸 block-match（2.66% < 2.75%），SSIM 持平（0.999 ≥ 0.998），motion
smoothness 大幅勝（cv 0.86 << 1.94）。grid=1（更高解析度 flow）反而
cv 升回接近 production，因 8×8 average 在 converter 過度 smoothing motion
magnitude variation。**證明更高解析度 flow 不一定更好，要跟 converter 端
averaging 互動。**

**7B async（`c82196e`）後實測：**
- nvOFExecuteVk #0/#300/#600/#900 status=0 持續成功
- chain consumes HW OF flow each frame
- 沒 VK_ERROR_DEVICE_LOST，沒 race
- Frame timing: n=152 fps=30.24 ft_mean=33.07ms p50=33.36 p95=34.16 p99=46.72
- p99 比 sync 路徑稍寬但無明顯 perf regression

**Phase 7B 之後進度（2026-05-15+）：**

- **Phase A production-ready（v1.4.108-111）** — `vkfrucEnableNvOf` 預設 true (NV GPU fresh install 自動啟用)、SettingsView 文字「(NV GPU 預設啟用)」、6 個 m_NvOfProf* PROF counter + 30-consecutive fail 自我 demote。實測 19 min PixArk session NVOF dispatches=3601/s fails=0、chain_mean=0.94ms (vs block-match 3.79ms ~4× 快)。詳見優先級表 §B-NVOF Phase B autotier 整合 row。
- **Phase B prerequisite（v1.4.117-118+138）✅ shipped** — §J.3.e.2.i.52 VK_SHARING_MODE_CONCURRENT for flow images (修 driver implicit cross-QF wait) + §J.3.e.2.i.53 NVOF dispatch worker thread (Option B 消 SDK CPU 1-15ms latency) + §J.3.e.2.i.54 in-flight/drop counter。**待 user PixArk 20+ 分鐘實測 v1.4.118+ handoff bimodal 是否從 134us/15ms 隨機切變 stable <2ms + decode latency 不再 cycle**。若 OK → reapply early-kickoff (Option E 路徑) 拿回 5% async perf win + 把 NVOF 列 NV best tier。
- **Phase 7E TRIPLE × NVOF cross-test**：analyze_motion.py OOM 已 fix (`83157fa`)，benchmark 可跑了；但 60s × 180fps Farneback OF 分析 ~10+ min CPU，建議改 20s window 或 chunk OF。仍待跑。
- **4K120 × NVOF stress**：未量過。
- **Linux nvofapi.so.1 dlopen 路徑**：仍 Windows-only；Linux 需要 `libnvidia-opticalflow.so.1`（Sunshine apt deps 有 nvidia driver 通常自帶）。

**Dead-end 紀錄（不要再走）：**

- **v1.4.110 NVOF early-kickoff**（§J.3.e.2.i.46 → revert v1.4.111）：把 NVOF execute 從 partC 之後搬到 partA 之後讓 NVOF 跟 cmpCmd 並行，**8.8% drops 嚴重退化**。Root cause: cmpCmd 等 PREVIOUS frame N-1 outSig 但本幀 N 的 NVOF 正在 WRITE m_NvOfFlowImage，cmpCmd READ vs frame N WRITE 跨 QF 並發 access 同一張 image **Vulkan 同步 hazard**。修法是 Phase B prerequisite 整套（CONCURRENT + Option B + double-buffer）。
- **Option D NVOF buffer 輸出取代 image 輸出** — NV OF SDK header 確認 `NV_OF_REGISTER_RESOURCE_PARAMS_VK` 只有 VkImage 欄位，SDK 沒提供 buffer 變體。NVOFA HW 內部用 image tiling layout 跑，動不了。詳見優先級表 §B-NVOF Option D won't-fix row。

#### §B Phase B restart — D3D11VA HEVC → Vulkan shared-image bridge（**🟡 active 2026-05-20，B9 待 AMD smoke**）

**動機：** AMD Radeon 780M iGPU 的 Windows Vulkan driver 沒實作 `VK_KHR_video_decode_h265`（`vkGetPhysicalDeviceVideoCapabilitiesKHR` 對 H.265 回 `VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR`），HEVC stream 全部走 SW decode 卡 ~50fps。同 driver 下 H.264 / AV1 vulkan decode 都 OK，是 H.265 capability AMD 沒給。從零實作 HEVC decoder 不切實際；繞 driver 不可能。

**設計：** HEVC 走 D3D11VA HW decode（driver 100% 支援），output NV12 array texture 透過 SHARED_NTHANDLE + `VK_KHR_external_memory_win32` import 到 Vulkan 給 VkFrucRenderer 做 FRUC + present。H.264 / AV1 走 Vulkan-native path 不變。

**為什麼這次值得重啟（vs project_phase_b_dead_end）：** v1.3.41-47 §I.F / §J.1 在 NV 596.84 driver 4 種 ablation 全 device-lost 後標 dead-end，結論「換 GPU 理論上路 1+ 應該能 work」。AMD 780M 是新驗證機會（AMD 對 D3D11/Vulkan interop sync primitive 的支援歷史上比 NV 早）。把舊 §J.1 機制剝出來包成通用 `D3D11VkBridge` class，VkFruc 跟 NCNN 之後可共用。

**已 ship（v1.4.167）：** B1-B8 + B10 skeleton

- B1: `d3d11_vk_bridge.{h,cpp}` skeleton + LUID match + Win32 PFN load
- B2: `exportTextureSlice` — per-array-texture SHARED_NTHANDLE pool cache（解 decoder pool 重用一個 array texture）
- B3: `importToVulkan` — `VK_KHR_external_memory_win32` + dedicated alloc + D3D11_TEXTURE_BIT / D3D12_RESOURCE_BIT 兩種 handle type
- B4: `createFenceSync` — ID3D11Fence + Vulkan timeline VkSemaphore (`D3D12_FENCE_BIT`) + `signalFromD3D11` / `waitOnD3D11` helpers
- B5: VkFrucRenderer `enum class CompositeMode { None, D3D11_HEVC, ProbeOnly }` + ctor param + bridge state
- B6: `prepareDecoderContext` / `getPreferredPixelFormat` / `isPixelFormatSupported` 接 D3D11 path + `initializeCompositeD3D11` 完整流程
- B7: `renderFrameD3D11Import` 最小版（export + import + fence sync demo，尚未 present）
- B8: `ffmpeg.cpp` cascade D3D11VA case 加 HEVC composite 分支 + `shouldUseVkFrucForD3D11Composite` (env opt-in `VIPLE_VKFRUC_D3D11_HEVC=1`)
- B10: thread_local sticky 失敗 latch + RAII teardown — bridge init 任一步 fail 自動 cleanup + cascade fallback 到 D3D11VARenderer

**Ablation mode (`VIPLE_VKFRUC_D3D11_HEVC_MODE`):** A1 default (D3D11_TEXTURE + D3D12_FENCE_BIT) / A2 (D3D12_RESOURCE + D3D12_FENCE_BIT) / A3 (D3D11_TEXTURE + caller TOP_OF_PIPE wait) / A4 (D3D12 reopen bridge, B7 follow-up).

**B9 進度條件：** AMD 780M 機器 `set VIPLE_VKFRUC_D3D11_HEVC=1` 跑 HEVC stream，log 出現 `[VIPLE-VKFRUC-COMPOSITE] B6 initializeCompositeD3D11 OK` + `B7 first frame imported tex=...`，連續 60 秒不 device-lost → 過關進 B9 (FRUC chain + present)。device-lost → 換下一個 ablation mode 試。

**NV laptop client / 其他 GPU：** composite path 預設關閉 (env unset → false)，既有 D3D11VA / Vulkan-native 行為完全不變。

### §J.3.f AV1 / HEVC / H.264 Vulkan hwaccel via ffmpeg（**✅ DONE 2026-05-03 / integration b2b7afd**）

整合 commit `b2b7afd` 起 RS_VULKAN preference 自動走 FFmpeg 8.1 (`avcodec-62.dll` 5.2 MB) 的 `h264_vulkan`/`hevc_vulkan`/`av1_vulkan` hwaccel，4K120+FRUC+DUAL × 3 codec 全 PASS（HEVC 101 fps / decodeMean 0.4-0.7ms）。env var (`VIPLE_USE_VK_DECODER=1`) 仍保留作 debug fallback。

**仍 open：**
- AV1 5ms decode 底線是 NV driver `vulkan_decode.c:1368` 強制 dedicated_dpb 造成；等 NV driver 升級或 AMD/Intel 試。
- AV1 wall-clock latency 80-106ms（HEVC 0.3ms 對比差 200×），建議 user 預設挑 HEVC。

詳細實測表格 / 涉及檔案改動 / Phase 3d.6 對比都在 commit `b2b7afd` message 跟 git log。

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

### §R2 PASSIVE FRUC ratio controller（**🟡 active follow-up 2026-05-21**）

**設計：** PASSIVE 模式下 client 主動把畫面對齊 display Hz — 量測 received fps / display Hz 百分比，動態決定 1x/2x/3x ratio 並透過 `LiRequestFpsChange` 通知 server 推 `display_Hz × ratio_new` 對齊。

**狀態機（v1.4.170 §R2-θ ship 後）：**

```
recv ≥ 95% of display, 連 10s, ratio > 1  → 降 ratio + LiRequestFpsChange(display × new)
recv < 80% of display, 連 3s, ratio < 2   → 升 2x + LiRequestFpsChange(display / 2)
recv < 40% of display, 連 3s, ratio < 3   → 升 3x + LiRequestFpsChange(display / 3)
```

切換進 cooldown 5s 避免 encoder reconfig 連環震盪；`targetServerFps` clamp 到 LiRequestFpsChange 範圍 10-360。

**已 ship sub-phase：**

- **§R2-α-1**（v1.4.151）— `toggleFRUC` 內 `LiRequestFpsChange` 移除（wire + server 端 capability 保留）
- **§R2-γ-5** — ffmpeg.cpp PASSIVE block 既有 above80/below70/below40 counter
- **§R2-η-2**（v1.4.169）— ratio alignment gate：升 ratio 前算 `target = server_fps × newRatio`，若 > `display_Hz + 5fps tolerance` → 拒絕升 + log warn。修「server 180 想升 2x = 360 炸 T0」bug。`renderer.h` 加 `virtual getRendererDisplayHz()`，`vkfruc.cpp` override 回 `StreamUtils::getDisplayRefreshRate(m_Window)`。
- **§R2-θ**（v1.4.170）— PASSIVE block 整段重寫：metric 從 `recv / server_fps` 改 `recv / display_Hz × 100%`；切換 ratio 同時呼 LiRequestFpsChange；`m_StreamFps` 同步 update 讓 overlay 跟 ACTIVE recvRatio 顯示新值；鬆綁 §R2-η-2 alignment gate（新邏輯 target == display 永遠對齊）。

**剩 follow-up：**

- **autotier T2→T0 跳 T1** — log 觀察 latency 一次破 budget 太多時直接 floor（T5 → T4 → T3 → T2 → T0 跳過 T1）。fix path 是看是否要中間階梯但 cost 不對稱；先觀察 §R2-θ alignment gate 生效後 latency 還會不會破到那麼大。
- **display Hz=0 legacy fallback** — renderer 沒實作 `getRendererDisplayHz()` 回 0 走舊 `recv/server_fps` 行為。需追蹤哪些 renderer path 該補（D3D11VARenderer / VAAPIRenderer / DRM/KMS / Android Vulkan / etc.）。
- **進 2x 後 server recv 從 ~180 掉到 ~140 持續** — 像是 server 反壓。alignment gate 把不合理 2x 擋掉後應該也消失，沒消失再追。

### 不可動的鐵律 (§J)

1. **Fallback 機制保留** — v1.3.41 的 3-fail fallback、v1.3.44 的 process-lifetime singleton 都不能移除。Phase J 改動失敗時不能讓 user crash。
2. **預設 D3D11 (v1.3.308 起)** — Vulkan 改實驗性次要選項。Phase J.5 真正切 Vulkan 為預設前，新 user 第一次啟動只看到 D3D11；既有 user 設定不被動。
3. **D3D11 renderer 是穩定主線**（不再只是 legacy fallback）— Phase 1.7 系列確認 NV driver 596.36 對 native VK_KHR_video_decode + ONLY mode 有結構性 bug，五個變體都繞不過。D3D11 + DXVA hardware decode 是所有 NV / AMD / Intel Windows 環境的穩定路徑。
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版見 `scripts/benchmark/vk_sw_codec_120.ps1` (v1.3.318 起)。
5. **build script 不能無聲拿舊 binary** — `build-tools/build_moonlight_package.cmd` 的 staging step 必須 errorlevel-check rmdir / copy，否則 zombie process 鎖檔會讓 release zip 內含過時 binary（v1.3.299~306 連 8 個 zip 都中招的事故，見 v1.3.307 commit message 5183cee）。

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

### §B-DUMP NCNN-Vulkan RIFE 輸出全 0 — known issue（**deferred 2026-05-07**）

**症狀：** D3D11VARenderer + frucBackend=NCNN 在 RTX 3060 Laptop + NVIDIA
596.144 driver + ncnn 20220729 build vintage 上：
- NCNN init 全部 OK（model load PASS、Vulkan device PASS、phase B.3 shared
  handle export PASS、probe `~25ms inference time` PASS）
- 但 `ex.extract("out0", out_mat)` 回傳的 `out_mat` 是**全 0 fp32 buffer**
- 結果：interp Present 出來的 BMP 全黑（只看得到 HUD overlay 在黑底上）

**已驗：**
1. `matToStaging → CopyResource → OutputSRV → blit` pipeline 完全正常
   （`VIPLE_FRUC_NCNN_DIAG=1` 強制 out_mat 寫 magenta，BMP 確實顯示 magenta）
2. fp16 → fp32 fallback (`VIPLE_FRUC_NCNN_FP16=0` default) 不能修，依然 0
3. SHARED_NTHANDLE / UAV bind flag drop 無效
4. Probe 25ms 是 GPU dispatch 時間，沒驗證 output 正確性 → 沒攔住 cascade

**最可能元兇（沒實際確認）：**
1. `rife.Warp` custom Vulkan layer 的 pack4/pack8 SPIR-V `set_optimal_local_size_xyz`
   在這台 GPU 上挑到產生 0 的 workgroup size
2. ncnn 20220729 跟 NV 596.144 driver 之間 fp16 storage path 有問題
3. RIFE-v4.25-lite model 跟此 ncnn 版本的 op set 微小相容性差

**為什麼 deferred：**
- `§J.3.e.X / §J.3.e.Y` 的手刻 native RIFE Vulkan pipeline 已經 milestone
  完成，準備在 production 路徑取代 NCNN（Final.3b NcnnFRUC integration）。
  花時間修 NCNN-Vulkan dependence 是反方向。
- D3D11 frucBackend=Generic + NVIDIA OF 兩條路在這台 GPU 上 production-ready，
  使用者不需要 NCNN
- 真要修需要：重新 compile rife.Warp Vulkan compute shaders 並強制 fixed
  workgroup size + 寫 ncnn-vulkan output validation harness（pack1 fallback
  vs pack4/pack8 的逐 pixel 比對）→ 1-2 天工程

**現有 mitigation：**
- 第一個觸發時 `[VIPLE-FRUC-NCNN] RIFE output is all-zero` warning log 印
  在 SDL log，告訴 user 切去 Generic backend
- frucBackend cascade 沒攔住但 user 切 backend 即可避開

**清的時候要做的：**
- Final.3b 整合 native RIFE pipeline 進 NcnnFRUC（drop ncnn-vulkan dep）
- 同時把這條從 todo.md 拿掉，§J.3.e.X integration milestone 替代

---

## §M. 多使用者並發 streaming

### §M.1 Phase 1 — Defensive guards + ownership（**✅ SHIPPED in v1.4.12, 2026-05-12**）

Multi-user ownership guards + Web UI session dashboard 已 ship。架構保留供 §M.1.f / §M.2 / §M.3 後續延伸。詳細設計 + 測試 SOP：`~/.claude/plans/steam-linked-volcano.md`，commit 細節見 git log。

### §M.1.f Follow-up — stale ownership lock（**✅ FIXED 2026-05-21 via §M.1.f.1 → .4**）

歷史症狀：使用者實機回報「沒有任何人連線，其中一台設備要連線時顯示已有設備佔線」。即 single user 自己也被 §M.1 的 ownership check 誤鎖。

修法分四階段 ship：

- **§M.1.f.1（v1.4.38 + v1.4.40 Android leg）** — Client-side quit key combo 路徑拆 `shouldNotifyServerCancel`（graceful exit always send /cancel）+ `shouldQuitClientApp`（`quitAppAfter` pref）；moonlight-android `Game.java stopConnection()` 加同 pattern，在 `conn.stop()` 之前建臨時 NvHTTP 呼叫 `quitApp()`。
- **§M.1.f.2 idle watchdog** — 背景 thread 偵測 idle session 後呼 `clear_owner_uuid()` 釋放 ownership，但**不收 process state**（thread safety：背景 thread 不能安全碰 `display_device::revert_configuration` / `system_tray::update_tray_stopped` 這些 UI thread IO）。產生「owner 空 + running()>0」孤兒狀態待 §M.1.f.4 收尾。
- **§M.1.f.3（v1.4.139）** — `confighttp.cpp:1838` `savePin` server-side name 驗證（trim whitespace + 空字串 400 + 長度 > 64 400 + PIN bad_request 補 return）。修「`named_devices.json` owner_name 變空 → /launch 503 訊息變 'Server in use by another paired device' + Web UI 顯示 '(unnamed device)'」的源頭。
- **§M.1.f.4（v1.4.173）** — /cancel / /launch / /resume 三個 handler 加 `no_owner = owner_uuid.empty()` 旁路。修 §M.1.f.2 idle watchdog 留下的孤兒狀態 — guard 不再誤判，已驗證 caller 都可清掉孤兒。同 commit 順手帶 `[PAIR-PHASE]/[PAIR-FAIL]/[PAIR-PIN]/[PAIR-OK]` diag log。

完整 commit 細節見 git log。

### §M.2 Phase 2 — Ubuntu VM 雙 user 並發 streaming 驗測

**目標：** 在 VipleStream 既有的 Ubuntu test VM (`<test-user>@<linux-test-vm>`) 上端到端跑通雙 user 並發 streaming 流程，驗證 Sunshine + per-user systemd + Xdummy/EVDI + PipeWire stack 可行。**不在 Phase 1 改 Sunshine multi-session、不在這階段驗 NVENC 並發 license — 那等實體測試機。**

**前提：** 使用者「盡快生出測試用實體設備」表態 (2026-05-11)。

**可在 VM 上驗（軟編碼路線，無 GPU passthrough）：**
- 雙 user account 建立 (`viple_a`, `viple_b`)
- 每 user systemd `--user` viplestream-server.service（port 47989 / 48089，利用 `network.cpp:197 map_port = config::sunshine.port + offset` 既有機制）
- Xdummy virtual display per user (X server :1 / :2)
- PipeWire per-user audio sink
- udev rule per-user input device
- 兩個 Moonlight client 同時連 → libx264 軟編 → 走通 RTSP / control / video / audio / input 全 path
- 兩個 streaming session 之間是否互相干擾（display / audio / input 串擾）

**必須等實體機（有 NVIDIA GPU passthrough 或 spare hardware）：**
- NVENC 並發 2 session frame rate / latency / VRAM 占用 (RTX 5060 Ti, driver R522+ 支援 5 concurrent NVENC)
- NVIDIA Linux driver Wayland 穩定性 (driver 545+ explicit sync GA)
- 實際遊戲 Proton 相容性 / HDR / DLSS / RT / anti-cheat (areweanticheatyet.com)

**進度：** v1.4.12 ship 後等 §M.1 取得使用者實測 sign-off (Task 11 Scenario 1-4) 再開工。

**Phase 2 預期產出：**
- `docs/multi-user-linux.md` setup SOP
- `scripts/setup_second_user_linux.sh` 自動化雙 user setup（不入 git，per `reference_linux_build_pipeline.md`）

### §M.3 Phase 3 — Linux host migration（待 §M.2 通過 + 實體機到位）

延後規劃，視 §M.2 結果 + 使用者實體機到位後評估：distro 選擇 (Bazzite KDE NVIDIA / Ubuntu LTS / 其他) / X11 vs Wayland 路線 / Proton 相容性實測 / 是否真執行 host OS migration。

---

## §H. AMD client + 解析度誠實化

§H.4.send / §H.4.osd 已 ship in v1.4.14 → v1.4.20 (詳見上面 patch series 條目)。剩下：

### §H.4-perf VkFrucRenderer AMD draw-time perf fix

**現況：** OSD 「Frame draw time (含 V-sync; 60Hz 下限 ~16.7ms)」標籤已說明 60Hz 為什麼一張 frame 至少 16.7ms。AMD client 上 m_FrucMode=false 的真實 GPU work 仍 ~13ms（NV 同 path 通常 ~2-4ms），明顯偏高。

**何時清：** 需要 user 提供 `[VIPLE-VKFRUC-GPU-PROF]` + `[VIPLE-VKFRUC-SW-PROF]` 5-10 行 log 才能精準改 — 不知熱點落在 nv12rgb / descriptor update / pipeline barrier / present-block 哪段就盲改 9843 行的 hot path 太冒險。

**修法分支：**
- Step 2A `m_FrucMode=false` fast-path — 跳過 FRUC-only descriptor updates + cmd records（ME/median/warp/copy）
- Step 2B 縮減冗餘 GPU pass — sampler chroma reconstruction LINEAR→NEAREST、tone-mapping `m_HdrActive` 守衛
- Step 2C swapchain present mode — image_count ≥ 3 或 MAILBOX 避免 driver-side stall

---

## §N. In-stream 雙向檔案傳輸

§N.1 / §N.2 / §N.3 / §N.4 / §N.4b 已 ship in v1.4.14 → v1.4.32 (詳見上面 patch series 條目)。剩下：

### §N.5 moonlight-android FileTransferClient runtime verify

**現況：** APK 建置 + JNI hooked + Game.java lifecycle 通；`MediaStore.Downloads` scoped-storage 路徑寫對；但實機未在串流 session 中真正跑過 Send/Receive flow，可能有 Pixel 5 / Pixel 9 specific Android quirks（譬如 backgrounding 中 OkHttp 連線維持、power manager 影響 polling）。

**何時清：** 下次 Android client 整段 streaming session 驗測時順帶過一輪 Send 小檔 → 完成後再回頭看 Receive listing UI 是否需要任何 Android 端的調整（譬如 listing 預設路徑該不該是 `MediaStore.Downloads.EXTERNAL_CONTENT_URI` 而非 host-style absolute path）。

### §N.6 moonlight-qt Cancel hotkey — visual verify pending

Ctrl+Alt+Shift+T hotkey 已 ship in v1.4.103（見優先級總覽表）。剩串流中 Send to client + 中途按 hotkey，verify (a) overlay text 出現「取消」/「Canceled」、(b) server log `[VIPLE-XFER]` 收到 cancel POST。Web UI 重新整理 / stream 結束 都仍是 fallback 路徑。

### §N.7 Sunshine Linux fs_picker (zenity)

**現況：** Windows server 端 `IFileOpenDialog` (COM) 已實作。Linux server 端「Send to client」流程觸發 file picker 還沒有實作 — `Sunshine/src/platform/linux/fs_picker.cpp` 該檔尚未建立，呼叫會回 nullopt → tray callback 直接 return（沒檔可送）。

**何時清：** Linux server 真實使用者出現時。用 `zenity --file-selection` subprocess 做最簡解（跨 GTK/Qt/各 DE 都行），預估 ~80 行。

**Receive 方向 Linux server 已可用：** 沒走 native file picker；server 端只負責接 client 上傳的 blob 寫到 `~/Downloads`，路徑解析跟 Windows 共用 `manager::downloads_dir()`（XDG_DOWNLOAD_DIR / HOME/Downloads fallback）。

---

## §K. Linux build pipeline

GitHub release 規範改成「每個 release 必 ship 完整三件、全同版號」（見 `CLAUDE.md` Release 規範）後，下一步把 Linux artifact 也加進完整 ship。

### §K.1 Linux x86_64 兩端（**SHIPPED 2026-05-05 in v1.3.337**）

Linux artifact 已正式進入 release：`VipleStream-Client-X.Y.Z-linux-x64.AppImage` + `VipleStream-Server-X.Y.Z-linux-x64.deb`。Source fix 詳細列表（Sunshine `src/stream.cpp`/`src/relay.cpp`/`src/nvenc_base.cpp` / moonlight-qt `plvk.{h,cpp}`/`vkfruc.cpp`/`3rdparty/nvvideoparser` / packaging rename `dev.lizardbyte.app.Sunshine.*` → `app.viplestream.server.*` 等）都在 git log。**剩 nice-to-have：** `release_full.cmd` 一鍵化包進 Linux 兩端。

### §K.4 Wayland XDG portal teardown root-cause（追隨 §K.1，deferred）

§K.1 ship 了 systemd `Restart=always` 緩解 client 斷線後 server 死掉的問題（option A），但 libwayland EPIPE 從何處發出未根因。需可重現 streaming 環境（VM 沒 GPU 不能驗 encoder + portal grab combo）。Pi 5 / 真 GPU Linux 機驗到時再修進 `wayland.cpp` / `portalgrab.cpp` 的 EPIPE 路徑。

### §K.linux VAAPI→Vulkan bridge (DMA-BUF interop)（**🟡 K.2 smoke test pending**）

**動機：** Ubuntu 100.117.251.20（RADV / Vega 10）跑 viplestream client 補幀未啟用。Root cause：RADV driver 對 Vega 10 沒實作 `VK_KHR_video_decode_h264/h265/av1` 三個 ext，FFmpeg vulkan hwaccel init fail，cascade 跌到 `VAAPIRenderer`（VAAPI HW decode 走 v4l2 path）— `VAAPIRenderer` 路徑沒接 VkFrucRenderer，補幀完全沒跑。

**Pre-req spike (2026-05-21) 結果：**
- modifier = `DRM_FORMAT_MOD_LINEAR` (0x0) — `VK_IMAGE_TILING_LINEAR` 直接用
- 全部需要的 Vulkan ext 確認 ✅（VK_KHR_external_memory_fd / VK_EXT_image_drm_format_modifier / VK_KHR_external_semaphore_fd / VK_KHR_bind_memory2）

**v1.4.180 K.1+K.2 ship（2026-05-21）：**
- `vaapi_vk_bridge.{h,cpp}` — `importFrame(dmaFd, dmaSize, modifier)` → LINEAR NV12 VkImage
- `VkFrucRenderer::CompositeMode::VAAPI_VK` — getPreferredPixelFormat / isPixelFormatSupported / prepareDecoderContext / renderFrameVAAPIImport (K.2 passthrough)
- `ffmpeg.cpp` cascade — VAAPI pass==0 → VkFrucRenderer(VAAPI_VK)；pass==1 仍 VAAPIRenderer fallback
- WSL Linux build PASS（g++ Ubuntu noble）

**K.2 smoke test（待 user AppImage 部署到 Tailscale）：**
```
stream H.264 / HEVC → 看 log：[VIPLE-VAAPI-VK] frame#X import OK
```

**K.3（接 FRUC chain）** — 依賴 §B Phase B B9 完成（AMD 780M Windows side）。

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
