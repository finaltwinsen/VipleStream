# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **High** | **§J.3.e.2.i.8** Phase 3d.5 — AV1 native decode GPU-side grey 診斷 | 9 個 parser/picture-info bug 修了仍 grey；剩 driver-level，需 validation layer 或 vk_video_samples 對比 |
| **High (in progress)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | v1.3.275 H.264 native ship 後發現 FRUC 從 m_SwStagingBuffer (FFmpeg SW)、real frame 從 m_SwUploadImage (native) 兩條 pipeline source 不對稱 → dual-present 3-4 Hz blur/sharp。v1.3.276 image→buffer copy + descset 切換改善但有 WAW race；v1.3.277 per-slot buffer 修，runtime 驗證中 |
| **Medium-High** | **§I.D** Android Vulkan FRUC async compute | C.5.b 量到 dual-present + waitIdle thermal regression；async compute queue 是真正解 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) — 解 NCNN-Vulkan shared path | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗 |
| **Medium** | **§A.1 + A.11** QSettings org / ini 遷移 | 升級 v1.2.43 起使用者設定看起來像 reset；migration 寫好且測試後再動 |
| **Medium** | **§J.3.e.2.i.8** Phase 1.5c — ONLY mode swapchain depth | 第二個 env var 開的 sole-decoder 模式 swapchain over-acquire；不影響預設 PARALLEL |
| **Medium-Low** | **§A.6** HTTP Basic auth realm | 改了使用者要重登 Web UI；併進其他改動才划算 |
| **Low** | **§C / §D / §E** 其他語系 / wiki 連結 / Android themed icon | 純品牌 / 視覺一致性 |
| **Low** | **§F** DirectML 搬 D3D12 / command bundles | 4K120 real-time 才需要 |
| **Low** | **§G.1 / §G.4** RIFE v1 11-channel / 模型下載管理 | A1000 launch overhead bound（§G.3 negative result）；RTX 30/40+ 才有意義 |
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計 |

---

## §A. 品牌遷移相容性債

### §A.1 QSettings organizationName + Windows registry 主路徑

**目前：** `moonlight-qt/app/main.cpp` 仍是 `setOrganizationName("Moonlight Game Streaming Project")`。Windows 上 paired hosts、偏好設定全部存在 `HKCU\Software\Moonlight Game Streaming Project\VipleStream`。

**現象：** `applicationName` 已是 `VipleStream`（v1.2.43）但 organizationName 沒動，registry 路徑像是「上游品牌名底下的 VipleStream key」。改了 → 路徑換成 `HKCU\Software\VipleStream\VipleStream`，原來的 key 變無主，使用者體感是所有 paired hosts 一夕消失。

**清的時候要做：**
1. 新版啟動時先用舊 org / app name 構造 `QSettings`，檢查 `HKCU\Software\Moonlight Game Streaming Project\VipleStream` 在不在
2. 在 → 讀全部 key/value、寫到新的 `QSettings("VipleStream", "VipleStream")`
3. 砍掉舊 registry 樹（或留 migrated-flag 避免重複搬）
4. macOS / Linux 同樣處理 `~/.config/Moonlight Game Streaming Project/` → `~/.config/VipleStream/`
5. unit test 覆蓋初始 / 已遷移 / 部分遷移三種情境

**併合進 A.11**：v1.2.43 把 `applicationName` 改成 `VipleStream` 已經造成升級時設定看起來「回預設值」（檔名從 `Moonlight.ini` 變 `VipleStream.ini`）。這份 migration 也要處理。

### §A.2 WiX installer 的 registry / install paths

**目前：** WiX installer 的 `HKCU\Software\Moonlight Game Streaming Project` 安裝狀態旗標 (`Installed` / `DesktopShortcutInstalled` 等)、`InstallFolder = "Moonlight Game Streaming"`、`APPDATAFOLDER = %LOCALAPPDATA%\Moonlight Game Streaming Project` 全沒動。

**影響檔案：** `moonlight-qt/wix/Moonlight/Product.wxs`、`moonlight-qt/wix/MoonlightSetup/Bundle.wxs`。

**注意：** 我們**目前根本沒在用 WiX MSI installer 出貨**，client 是 zip + 手動解開、server 用 `deploy_sunshine.ps1` 部署。所以 WiX 的相容性債是「如果哪天要改用 MSI 出貨才需要清」，不是現在的痛點。

**清的時候要做：**
1. 新 InstallFolder = `VipleStream`、新 APPDATAFOLDER = `%LOCALAPPDATA%\VipleStream`
2. 新 HKCU 路徑 = `HKCU\Software\VipleStream`
3. WiX 升級邏輯：偵測舊 key / 路徑存在 → copy → 砍舊
4. 配新的 `UpgradeCode` GUID，否則 MSI 嘗試 in-place upgrade 找不到舊路徑

### §A.6 HTTP Basic auth realm `"Sunshine Gamestream Host"`

**目前：** `Sunshine/src/httpcommon.cpp:132` 用這個字串當 HTTP Basic auth realm。Web UI 登入時 browser 把 username/password 存在這個 realm 下。

**影響：** realm 一改，browser 已存的憑證對不上新 realm，使用者要重新登入。

**清的時候要做：**
1. 新 realm = `"VipleStream-Server Web UI"`
2. Release notes 寫「升級後 Web UI 會要求重新登入」

### §A.7 協定 / wire-format 字串（**不該清**）

**目前：** `"NVIDIA GameStream"`、cert CN `"Sunshine Gamestream Host"`、UA header `Moonlight/...`、mDNS service type `_nvstream._tcp`、serverinfo 的 `state` 字串 `SUNSHINE_SERVER_FREE` / `SUNSHINE_SERVER_BUSY`、HTTP `/serverinfo` `/launch` 等 endpoint 路徑全部維持上游原名。

**理由：** 這是 GameStream wire protocol 的一部分，改了等於跟整個 Moonlight / Sunshine 生態脫鉤——使用者沒辦法用原版 Moonlight 連 VipleStream-Server，也沒辦法用 VipleStream client 連別人家的 vanilla Sunshine。User 已明確要求「混搭互聯」（v1.2.93 討論）。

**保留到永遠。** 這條留著就是要提醒未來想動的人：不要動。

### §A.8 內部 class names

**目前：** `NvHTTP` / `NvComputer` / `NvApp` / `SunshineHTTPS` / `SunshineHTTPSServer` / `SunshineSessionMonitorClass`（Windows window class）等內部命名沒改。

**清的時候要做：** 真的想改就在獨立 refactor PR 裡 batch rename，**不要**跟 feature / bugfix 混在一起（diff 會無法 review）。優先度極低。

---

## §C. Sunshine Web UI 其他語系檔

**目前：** v1.2.43 只改了 `en.json` / `zh_TW.json` 的 Sunshine / Moonlight → VipleStream-Server / VipleStream。另外 20 個語系檔還是舊字串。

**要做：** 下一次 i18n sync commit 順便對 20 個檔做 bulk replace。**不影響功能，只是品牌一致性。**

---

## §D. 上游 wiki 連結

**目前：** `moonlight-android/.../HelpLauncher.java` 的 setup guide / troubleshooting 指 `finaltwinsen/VipleStream#readme`；GameStream EOL FAQ 仍指上游。

**要做：** VipleStream 自己的 docs（setup guide、troubleshooting、錯誤碼表）有實際內容後，HelpLauncher URL 換成 VipleStream 自己的 docs。

---

## §E. moonlight-android Icon 補完

**目前：** v1.2.30 已用 lime VipleStream icon，但：
- Splash screen 需確認用新 icon
- 「最近應用程式」列表 icon 在各版本 Android 上的顯示要驗證
- Android 12+ themed icon（Material You 單色化）沒提供 monochrome mask → 會 fallback 到 adaptive-icon。要支援需加 `ic_launcher_monochrome.xml`

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

§G.2（fp16 model + cascade rewrite）跟 §G.3（IFRNet-S 試水）已 ship（git history v1.2.124-128）。重要的 negative result：**A1000 launch overhead 25 ms floor 不可繞過，任何 ML 模型 op count > 150 都上不了 14 ms budget**。這影響下面剩下的條目：

### §G.1 11-channel 輸入 (RIFE v4 v1)

**Gain：** v1 node count 少 50%（216 vs 456），可能比 v2 快 30-50%。
**重新評估（§G.3 之後）：** A1000 仍可能達不到 14 ms（216 ops × 0.1 ms launch overhead = 21.6 ms 已經超了），但對 RTX 30/40+ 可能值得；要 ship 還是得搭 §G.4 的「下載管理」避免 release zip 再加肥。
**做法：** `tryLoadOnnxModel` 加 11-channel branch；pack 前加 optical flow CS 或塞 zero flow。

### §G.4 模型下載管理

**動機：** `fruc.onnx` 22 MB 跟 release zip 一起出。多數 NVIDIA user 用 NvOF / Generic，DML 是少數派。
**做法：** Moonlight 第一次選 DML 時跳「下載 22 MB」dialog，model 放 `%LOCALAPPDATA%\VipleStream\fruc_models\`。低優先度。

### 已就位的診斷工具

- `VIPLE_DIRECTML_DEBUG=1` — D3D12 debug layer + DML validation
- `VIPLE_DML_RES=540|720|1080|native` — tensor 解析度 override
- `[VIPLE-FRUC-DML]` log — 每 120 frame 印 per-stage EMA μs
- `[VIPLE-FRUC-DML-ORT]` log — ort_concat / ort_run / ort_post / wait_pack / wait_post / wait_concat 拆解

---

## §I.D Android Vulkan FRUC — async compute queue（剩下唯一 active 工作）

§I 整套（A recon → B passthrough → C Vulkan FRUC port → C.6 display timing → C.7 settings UI）都已 ship（v1.2.134–161），但 **C.5.b baseline 量到 thermal regression**：dual present + per-frame `vkQueueWaitIdle` 強迫 CPU+GPU serialize → GPU 一直在等同步 → CPU usage 高 → thermal +4°C in 60s。GLES 同期 trend 平。

### §I.D 內容

**動機：** §I.C.5.b 已識別 root cause — dual present + waitIdle 是結構性瓶頸。Async compute queue 是真正解法。

**做法：** ME / warp 移到 dedicated compute queue，跟 graphics queue 平行；移除 host-side `vkQueueWaitIdle`；ME 跟前一幀 present 重疊；mailbox present 取代 FIFO。

**狀態：** v1.2.162 ship in-flight ring（host-side waitIdle 移除）但 perf 平 — 真要 latency gain 還要 mailbox present 或 multi-queue，**Adreno 620 queue family 數量待 probe**。

**鐵律：**
1. 每 Phase 都要有 baseline 對比。沿用 `scripts/benchmark/android/`，量出來不如預期就停。
2. **GLES path 至少保留到「Vulkan 穩定 6 個月」之後才移除**。期間 FRUC bug 修兩次（GLES + Vulkan）是已知 +20% 維護成本。

**已就位的診斷工具：**
- `scripts/benchmark/android/android_baseline.sh` — thermal + fps + jank 採集
- `scripts/benchmark/android/analyze_baseline.py` — 30s bucket fps + pixel-thermal 高頻交叉驗證
- `debug.viplestream.vkprobe=1` system property — opt-in Vulkan FRUC backend
- Settings UI toggle「FRUC 後端：GLES (預設) / Vulkan (實驗)」（v1.2.161 ship）

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
| **§J.1** D3D11.4 fence ↔ VkSemaphore bridge | NCNN shared path 同步基礎建設 | ❌ DEAD END NV 596.84 — 4 種 ablation 全 device lost；driver-level 拒絕 |
| **§J.1 路線 A** | 完整 ID3D12Device bridge (~250 LOC) — D3D11→D3D12→Vulkan 三段橋 | ⏳ 未驗（NV 對 D3D11-origin GPU memory 是否視為 D3D12 lineage 不確定） |
| **§J.2** Vulkan post-processing pipeline | Generic FRUC、HDR shader、blit、swapchain present 全部移到 Vulkan | 🟡 跟 §J.3.e 大幅重疊 — VkFrucRenderer 同時 cover 兩者 |
| **§J.3** VK_KHR_video_decode 完整 Vulkan-native pipeline | 透過 ffmpeg 8.x Vulkan hwaccel + PlVkRenderer | ✅ 基礎已存（HEVC 直通）；§J.3.e FRUC 整合進行中 |
| **§J.4** HEVC + H264 decode + 跨平台驗證 | Linux full coverage；macOS 路徑決策 | 規劃中 |
| **§J.5** 整合測試 + fallback hardening + 預設切換 | NV / AMD / Intel bench；舊 driver 退回 D3D11；預設改 Vulkan | 規劃中 |

### §J.3.e VkFrucRenderer（Android architecture port，**目前主戰場**）

既有 `moonlight-android/.../jni/moonlight-core/vk_backend.c` 4117 行 production-tested native Vulkan FRUC，PC 端 port 70% 直接可用。Phase 表（每子 phase 獨立 commit、可獨立 ship）：

| Sub-phase | 內容 | 狀態 |
|---|---|---|
| **§J.3.e.2.i.1-7** Init / device / swapchain / FFmpeg-Vulkan bridge / ext probe | Vulkan device + swapchain + FFmpeg AVHWDeviceContext + RGB upload pipeline 全套 | ✅ ship |
| **§J.3.e.2.i.8 Phase 1.x** H.265 native VK_KHR_video_decode | NvVideoParser + VkVideoSessionKHR + DPB pool + vkCmdDecodeVideoKHR + cross-queue timeline sem | ✅ v1.3.251（84.78 fps PARALLEL stable，5000+ frame ZERO device-lost）|
| **§J.3.e.2.i.8 Phase 1.5c** ONLY mode swapchain depth | `VIPLE_VKFRUC_NATIVE_DECODE_ONLY=1` 跳過 FFmpeg、純 native 驅動 Pacer。Synth-frame submission rate 高 → swapchain over-acquire (`VUID-vkAcquireNextImageKHR-surface-07783`) | 🟡 deferred — 預設 PARALLEL mode 不受影響；要修需 Pacer integration / swapchain depth bump |
| **§J.3.e.2.i.8 Phase 2** H.264 native VK decode port | nvvideoparser 把 H264 sources 加進來、createNvVideoParser + DecodePicture H.264 dispatch、onH264PictureParametersFromParser SPS/PPS 增量上傳、submitDecodeFrameH264 (DPB iterate dpb[17] / 自 alloc StdVideoDecodeH264ReferenceInfo / flat picture-info)、移除過度保守的 intra_pic_flag RESET trigger | ✅ v1.3.275 — H.264 stream 走 native VK_KHR_video_decode，VIPLE_VKFRUC_NATIVE_DECODE=1 開啟 |
| **§J.3.e.2.i.8 Phase 2.5** FRUC NV12 source 整合 native decode | dual-present 下 real frame 從 m_SwUploadImage (native VK decode 寫的) sample，但 FRUC 的 NV12→RGB compute 從 m_SwStagingBuffer (FFmpeg parallel SW decode 用 memcpy 寫的) 讀 → source asymmetry → 3-4 Hz blur/sharp flicker。修法：m_SwUploadImage 加 TRANSFER_SRC usage、graphics queue 在 renderFrameSw cmd buf 開頭做 image→buffer copy 進 m_SwFrucNv12Buf[slot]、FRUC binding 0 透過 m_FrucNv12RgbDescSetNative[slot] 改指這個 buffer、timeline sem wait stage 改 TRANSFER_BIT 涵蓋 copy | 🟡 in progress — v1.3.276 single-buffer 改善 (decode 41→31ms, drops 7.77→5.02%) 但有 WAW race 殘留偶爾模糊；v1.3.277 per-slot buffer + per-slot descset 修 race，runtime 驗證中 |
| **§J.3.e.2.i.8 Phase 3** AV1 native VK decode | parser library 加 AV1 sources、createNvVideoParser AV1 路徑、submitDecodeFrameAv1 | ✅ source 全到位（v1.3.272），AV1 stream 走 FFmpeg libdav1d + Vulkan render @84fps，overlay 顯示精準 |
| **§J.3.e.2.i.8 Phase 3d.5** AV1 native decode GPU-side grey 診斷 | 9 個 parser/picture-info real bug 修了仍吐 constant grey；剩下 driver-level 問題 | 🟡 阻塞 — 需 Vulkan validation layer (LunarG SDK) 或 vk_video_samples 上游 client 逐行 diff |

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
2. **預設行為跟 v1.3.44 等價** — 直到 Phase J.5 預設切 Vulkan，預設 user 走 D3D11 主路徑（CLI flag opt-in）。
3. **D3D11 renderer 留作 legacy fallback** — driver 不支援 VK_KHR_video_decode 的 user 退回 D3D11VA。預計支援到 2027 年（Win10 EOL + 主流 driver 都更新到 VK 1.3.274+）。
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版自寫一份。

### 已就位的診斷工具（會用到）

- `[VIPLE-FRUC-NCNN]` / `[VIPLE-VKFRUC]` log family
- `VIPLE_USE_VK_DECODER=1` — opt-in Vulkan-first cascade（HEVC 完整 Vulkan-native pipeline）
- `VIPLE_VKFRUC_NATIVE_DECODE=1` — opt-in nvvideoparser feed + native VkVideoSessionKHR
- `VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1` — opt-in AV1 vkCmdDecodeVideoKHR submit（**目前 default OFF**，pending §J.3.e.2.i.8 Phase 3d.5）
- `VIPLE_VKFRUC_VULKAN_DEBUG=1` — VK_EXT_debug_utils + 路 validation 訊息進 SDL log
- `VIPLE_VKFRUC_NO_FRUC=1` — 暫時關 FRUC + dual mode（diagnostic）
- `loadModel: step N/6` trace log — v1.3.39 加，定位 init crash 位置
- 3-fail fallback counter — v1.3.41 機制

每個子 phase 的 commit message 用 `vX.Y.Z: §J.N.M — <短摘要>` 格式。

---

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 完成的事項移到 git log（`vX.Y.Z: §X.N — short summary` commit message 是唯一紀錄來源）
- 新發現的債照 §A.N / §J.N 風格加進來
- 真的不打算做的（§A.7）就明確寫「不該清」並給理由
