# VipleStream — 待辦 / 技術債清單

這份文件記錄**明確知道之後要做、但目前刻意沒做**的事。每一條都要
寫清楚「為什麼先這樣保留 / 何時該清 / 清的時候要做什麼遷移」，不
要只寫「以後改」。

完成項紀錄在 git log；這份只列尚待 / 進行中 / won't-fix。

---

## 優先級總覽

| 優先級 | 條目 | 一句話 |
|---|---|---|
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 3d.6 — AV1 native decode GPU-side grey | 9 個 parser bug 修完仍 grey；NV driver 596.36 + AV1 vkCmdDecodeVideoKHR 黑/灰畫面，需 RenderDoc + vk_video_samples client diff，目前 AV1 預設走 libdav1d SW 不擋使用 |
| **Deferred (driver-bound)** | **§J.3.e.2.i.8** Phase 1.7 ONLY-mode NVDEC device-lost | v1.3.298~301 五個變體（fence-pin / hold-ring / pool-reuse / barrier）都無法繞過，NV driver 596.36 對 native VK_KHR_video_decode 內部 NVDEC 有結構性 bug；v1.3.302 把 `*_ONLY` env rename 成 `*_ONLY_DANGEROUS` 強迫舊 `setx` 失效，預設 PARALLEL mode 穩 |
| **Active (long-running)** | **§J.3.e.2.i.8** Phase 2.5 — FRUC native source 整合 | v1.3.275 H.264 native ship 後 dual-present 3-4 Hz blur/sharp；v1.3.276/277 per-slot buffer 改善大半，殘留小 race 等 Phase J.5 整體切換 Vulkan 才補完整 |
| **Medium-High** | **§I.D** Android Vulkan FRUC async compute | C.5.b 量到 dual-present + waitIdle thermal regression；async compute queue 是真正解 |
| **Medium** | **§J.1** 路線 A (ID3D12 bridge) — 解 NCNN-Vulkan shared path | NV 596.84 對 D3D11_TEXTURE_BIT 死路；ID3D12Device intermediary 未驗 |
| **Done / partial** | **§A.6 / §C / §D / §E** brand consistency 收工 | A.6/C 已 ship；D HelpLauncher URL 已指 finaltwinsen/VipleStream README，等正式 docs；E themed icon 自 v1.2.36 已 wire |
| **Low** | **§F** DirectML 搬 D3D12 / command bundles | 4K120 real-time 才需要 |
| **Low** | **§G.1** RIFE v1 11-channel | A1000 launch overhead bound（§G.3 negative result）；RTX 30/40+ 才有意義（§G.4 已 ship at v1.3.311–312）|
| **Low** | **§A.2 / §A.8** WiX installer / 內部 class rename | 沒用 MSI 出貨 / 純內部 |
| **Won't fix** | **§A.7** Wire-protocol 字串 | 動了等於跟 Moonlight 生態斷線，違背「混搭互聯」設計 |

---

## §A. 品牌遷移相容性債

### §A.1 + §A.11 QSettings organizationName + Windows registry 主路徑

**狀態：** ✅ 已於 v1.2.129 (ff6b5ce) ship — `setOrganizationName("VipleStream")` +
`setApplicationName("VipleStream")`，啟動時 one-shot migration 從兩個 legacy 位置
(`Moonlight Game Streaming Project / VipleStream` v1.2.43..128、`Moonlight Game
Streaming Project / Moonlight` 純 vanilla) 取較新的 paired-hosts snapshot 寫入新
位置；`migration/v129_org_done` flag 防重複跑；macOS 因 reverse-domain 無 path
影響不需要 migration（見 main.cpp:431-541 註解）。

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

**目前：** `Sunshine/src/confighttp.cpp:131` 用這個字串當 Web UI 401 回應的
`WWW-Authenticate: Basic realm=...` header。Web UI 登入時 browser 把 username/
password 存在這個 realm 下。

> 註：之前 TODO 寫成 `httpcommon.cpp:132` 是錯的 — 該行是 cert generation 的
> Common Name (`gen_creds("Sunshine Gamestream Host", 2048)`)，屬於 §A.7「不該清」
> 的 wire-format 範圍。realm 跟 cert CN 共用同一個字串只是巧合。

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

**狀態：** ✅ 已於 v1.3.310 ship — 19 個 locale (`bg / cs / de / en_GB / es /
fr / hu / it / ja / ko / pl / pt / pt_BR / ru / sv / tr / uk / vi / zh`)
共 1270 處 `Sunshine` → `VipleStream-Server` 跟 `Moonlight` → `VipleStream`
bulk replace。en.json / zh_TW.json / en_US.json 自 v1.2.43 起就已 sync。

---

## §D. 上游 wiki 連結

**狀態：** 🟡 部分 ship — `moonlight-android/.../HelpLauncher.java` 的
setup guide + troubleshooting 已改指 `https://github.com/finaltwinsen/
VipleStream#readme`（GitHub README 當文件入口）；GameStream EOL FAQ 維持
upstream `moonlight-stream/moonlight-docs` wiki（這個是 NVIDIA-side 議題，
跟我們無關）。

**還沒做：** 等 VipleStream 自己有結構化 setup guide / troubleshooting /
錯誤碼表時，把 HelpLauncher URL 從 README 換成那些頁面 anchor。

---

## §E. moonlight-android Icon 補完

**狀態：** 🟡 部分 ship — Android 12+ themed icon (Material You) 自 v1.2.36
(1ec4445) 已 wire — `mipmap-anydpi-v26/ic_launcher.xml` 內 `<monochrome>`
element reuse `ic_launcher_foreground` 當 silhouette mask。剩下兩件事
需 Android 機驗證才知道是否需動：

- Splash screen 在 Android 12+ SplashScreen API 下顯示是否用新 icon
- 「最近應用程式」列表 icon 在各 Android 版本（10/11/12/13/14）顯示

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

**狀態：** ✅ 已於 v1.3.311–312 ship — `moonlight-qt/.../modelfetcher.{h,cpp}`
new ~250 LOC class，DirectMLFRUC::tryLoadOnnxModel() 找不到 install/data
copy 時觸發 ModelFetcher::ensureModelPath()，從 GitHub release v1.3.310
attached assets 同步下載到 `%LOCALAPPDATA%\VipleStream\fruc_models\`，
SHA-256 verify、失敗 retry 一次。Release zip 132 → 102 MB（少 30 MB 對應
fruc.onnx + fruc_fp16.onnx）。Build script 拿掉 onnx copy block。實機驗
過 dev 機 NV laptop 走 D3D11+DML cascade 觸發兩次下載 + verify 成功。

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
| **§J.3.e.2.i.8 Phase 1.5c** ONLY mode device-lost graceful degrade | rc=-4 (DEVICE_LOST) cascade 接管：set m_DeviceLost flag → 後續 render/decode 早 return → log 一條停 | ✅ v1.3.295 (86adc5d) — 之前每次 device-lost 都連續吐幾百行 validation cascade |
| **§J.3.e.2.i.8 Phase 1.6** Aftermath GPU crash dump 整合 | NVIDIA Nsight Aftermath SDK 1.6 + VK_NV_device_diagnostics_config + VK_NV_device_diagnostic_checkpoints；device-lost 自動寫 `%TEMP%\VipleStream-aftermath-<ts>.nv-gpudmp` | ✅ v1.3.298 (7cb4fb4) — 配 `tools/aftermath_decode/` standalone CLI 解 dump 為 JSON |
| **§J.3.e.2.i.8 Phase 1.7a** fence-pin bsBuf | 在 next submit 入口 reset 上一幀 bsBuf shared_ptr | ❌ v1.3.299 (5a7c9ff)，4 frames 必死，revert 為 v1.3.300 (7d126b3) |
| **§J.3.e.2.i.8 Phase 1.7b** hold-forever ring N=16 | 永遠不主動 destroy，等 16 幀後 round-robin 自然回收 | ❌ stash drop，4 frames 必死 |
| **§J.3.e.2.i.8 Phase 1.7c** host→video_decode buffer barrier | `VK_PIPELINE_STAGE_2_HOST_BIT` + `HOST_WRITE_BIT` → `VIDEO_DECODE_READ_BIT_KHR` 在 vkCmdDecodeVideoKHR 之前；抄自 vk_video_samples reference client | ✅ v1.3.301 (fdbbf8c) — spec-correct，dump pattern 變但 ONLY mode 仍 4 frames 死 |
| **§J.3.e.2.i.8 Phase 1.7d** pool-reuse bsBuf | N=16 pre-allocated VkBuffer + grow on demand；抄自 vk_video_samples `m_decodeFramesData.GetBitstreamBuffersQueue()` | ❌ stash drop，4 frames 必死 |
| **§J.3.e.2.i.8 Phase 1.7e** ONLY env rename to *_DANGEROUS | 五個變體都無法修 NV 596.36 driver bug；rename `VIPLE_VKFRUC_NATIVE_DECODE_ONLY` → `*_DANGEROUS` 強迫舊 setx 失效，預設 PARALLEL | ✅ v1.3.302 (029937c) — PARALLEL mode 穩定走 native VK_KHR_video_decode（FFmpeg AVFrame 驅動 Pacer，native VK sample 顯示） |
| **§J.3.e.2.i.8 Phase 2** H.264 native VK decode port | nvvideoparser H.264 sources、submitDecodeFrameH264、onH264PictureParametersFromParser | ✅ v1.3.275 — H.264 stream 走 native VK_KHR_video_decode |
| **§J.3.e.2.i.8 Phase 2.5** FRUC NV12 source 整合 native decode | m_SwUploadImage 加 TRANSFER_SRC、image→buffer copy 進 m_SwFrucNv12Buf[slot]、per-slot descset；殘留小 race 在 Phase J.5 整體切換時補完 | 🟡 v1.3.277 per-slot buffer 改善大半，runtime 仍偶爾抖；不擋使用 |
| **§J.3.e.2.i.8 Phase 3** AV1 native VK decode plumbing | parser AV1 sources、submitDecodeFrameAv1 | ✅ v1.3.272 — source 全到位，submit 預設 OFF（pending Phase 3d.6） |
| **§J.3.e.2.i.8 Phase 3d.6** AV1 native decode GPU-side grey 診斷 | 9 個 parser/picture-info real bug 修了仍吐 constant grey；driver-level，需 RenderDoc + vk_video_samples 對比 | 🟡 deferred indefinitely — AV1 預設走 libdav1d SW，Vulkan render 不擋使用 |
| **AMD ycbcr** descriptor pool sizing | 動態 query `VkSamplerYcbcrConversionImageFormatProperties::combinedImageSamplerDescriptorCount`，pool size = N × max(reported, 16) | ✅ v1.3.307 (5183cee) — 在 AMD Vega 10 整合顯卡上能 init 過 createDescriptorPool |
| **Vulkan demoted to experimental** | 預設 renderer 改 `RS_D3D11`、Settings dropdown D3D11 在前、Vulkan 標 [實驗性] | ✅ v1.3.308 (2a892e7) — 既有 user 設定不變、新 user 預設 D3D11 |

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
4. **每 Phase 都要有 baseline 對比** — 沿用 §I 的 baseline.sh 設計，desktop 版自寫一份。
5. **build script 不能無聲拿舊 binary** — `scripts/build_moonlight_package.cmd` 的 staging step 必須 errorlevel-check rmdir / copy，否則 zombie process 鎖檔會讓 release zip 內含過時 binary（v1.3.299~306 連 8 個 zip 都中招的事故，見 v1.3.307 commit message 5183cee）。

### 已就位的診斷工具（會用到）

- `[VIPLE-FRUC-NCNN]` / `[VIPLE-VKFRUC]` / `[VIPLE-AFTERMATH]` log family
- `VIPLE_USE_VK_DECODER=1` — opt-in Vulkan-first cascade（HEVC 完整 Vulkan-native pipeline）
- `VIPLE_VKFRUC_NATIVE_DECODE=1` — opt-in nvvideoparser feed + native VkVideoSessionKHR
- `VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS=1` — opt-in ONLY mode（synth-frame Pacer drive）。已知在 NV 596.36 上會 NVDEC device-lost；舊 `*_ONLY` 自 v1.3.302 失效。
- `VIPLE_VKFRUC_NATIVE_AV1_SUBMIT=1` — opt-in AV1 vkCmdDecodeVideoKHR submit（**目前 default OFF**，pending §J.3.e.2.i.8 Phase 3d.6 grey 修法）
- `VIPLE_VKFRUC_VULKAN_DEBUG=1` — VK_EXT_debug_utils + 路 validation 訊息進 SDL log
- `VIPLE_VKFRUC_NO_FRUC=1` — 暫時關 FRUC + dual mode（diagnostic）
- `tools/aftermath_decode/` — standalone CLI 解 `.nv-gpudmp` → JSON（不需要 Nsight Graphics GUI）；自 v1.3.298 起 device-lost 自動寫到 `%TEMP%\VipleStream-aftermath-<ts>.nv-gpudmp`
- `loadModel: step N/6` trace log — v1.3.39 加，定位 init crash 位置
- 3-fail fallback counter — v1.3.41 機制

每個子 phase 的 commit message 用 `vX.Y.Z: §J.N.M — <短摘要>` 格式。

---

## 如何使用這份清單

- 動某條相容性債 → **不要**單獨 commit「改名」，要一起 ship migration code + release notes
- 完成的事項移到 git log（`vX.Y.Z: §X.N — short summary` commit message 是唯一紀錄來源）
- 新發現的債照 §A.N / §J.N 風格加進來
- 真的不打算做的（§A.7）就明確寫「不該清」並給理由
