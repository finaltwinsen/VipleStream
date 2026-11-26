正在讀取設計稿 `docs/specs/linux_vr_backend.md`、協定與架構文件（`docs/vr_protocol.md`、`docs/vr_architecture.md`），以及 Windows 端 VR 擷取實作與目錄結構，開始進行對照檢查。
接著讀取協定與架構文件的後半部、`display_vr.cpp` 後半部，以及設計稿中引用的 SteamVR Driver、VR Session、IPC ABI、Linux 平台檔案與 `VipleVr.h`，逐一核對引用的行號、函式與結構。
已完成第一階段架構文件與 Windows VR 核心檔案讀取，接下來讀取 Linux 平台原始碼（`misc.cpp`、`kmsgrab.cpp`）、協定定義檔 `VipleVr.h`、Client 解包與 XR 投影程式碼（`VideoDepacketizer.c`、`xrcontext.cpp`、`xrvideo.cpp`、`vrlaunchparams.cpp`），以及 Server 串流與編碼模組（`stream.cpp`、`nvhttp.cpp`、`video.cpp`）。
繼續讀取 `VipleVr.h` 剩餘部分、`temp/vrdual_src` 與 UEVR 參考原始碼，以及 Linux 平台目錄與 `stream.cpp` 相關段落，核對設計稿的所有引用與假設。
接著讀取 Linux 平台擷取與編碼實作（`cuda.cpp`、`vaapi.cpp`、`nvenc_base.cpp`）、RTSP 與 Video/Control Stream 實作（`rtsp.cpp`、`VideoStream.c`、`RtpVideoQueue.c`、`ControlStream.c`），以及 Client 端的 `session.cpp`、`ffmpeg.cpp` 與 `vrframemeta`。
繼續讀取 `stream.cpp` 的網路與音視訊封包收發邏輯、`video.cpp` 的擷取與編碼執行緒架構，以及 `ffmpeg.cpp` 與 `xrvideo.cpp` 的影像解碼與 metadata 傳遞細節。
接著讀取 `nvhttp.cpp`（`/launch` 協商流程）、`rtsp.cpp`（RTSP 視訊埠與 session 啟動）、`video.cpp`（`captureThread` 與編碼流程）、`session.cpp`（Client 連線啟動流程）與 `ControlStream.c`（控制通道與 LOSS 回報）。
以下為對 [docs/specs/linux_vr_backend.md](/docs/specs/linux_vr_backend.md)（`§LINUX-VR`）對照實際程式碼與協定文件後的逐節審查結果（工作區維持完全只讀未修改）。

---

## 一、引用之函式、檔案、結構與行號查核

### 1. [R-1] 音訊與視訊 RTP SSRC 設定行號錯誤（偏差 278 行）
- **位置**：[linux_vr_backend.md:186](/docs/specs/linux_vr_backend.md#L186)（`§2.2` 資料流）
- **問題**：設計稿稱「`SS/stream.cpp:6684` 只看得到音訊 SSRC 被設成 0」，但 `:6684` 實際上是 `videoBroadcastThread` 內的 `frame_fec_latency_logger.first_point_now();`。音訊封包設定 `audio_packet.rtp.ssrc = 0;` 實際位於 `:6962`，而視訊封包在 `:6728-6730` 只填了 `packetType`、`seq`、`timestamp`，從未顯式初始化 `inspect->rtp.ssrc`。
- **證據（檔案:行號）**：
  - [Sunshine/src/stream.cpp:6684](/Sunshine/src/stream.cpp#L6684)
  - [Sunshine/src/stream.cpp:6728-6730](/Sunshine/src/stream.cpp#L6728-L6730)
  - [Sunshine/src/stream.cpp:6962](/Sunshine/src/stream.cpp#L6962)
- **建議**：將引用修正為 `SS/stream.cpp:6728-6730`（視訊未設 SSRC）與 `SS/stream.cpp:6962`（音訊設 SSRC 為 0）。

### 2. [R-2] Windows 端 CPU 等候 GPU Fence 的行號指向檔頭註解而非實作
- **位置**：[linux_vr_backend.md:39](/docs/specs/linux_vr_backend.md#L39)（`§1.1` Windows 現況）
- **問題**：設計稿稱「server 擷取端在 CPU 確認 fence 之後才 `CopyResource`（`SW/display_vr.cpp:9-10`、`:1203`）」。其中 `:1203` 確為 `device_ctx->CopyResource(...)`，但 `:9-10` 只是檔頭註解；真正執行 CPU fence 等候（`cs->waiter->wait`）與逾時補等的實作位於 `:1049` 與 `:1070-1088`。
- **證據（檔案:行號）**：
  - [Sunshine/src/platform/windows/display_vr.cpp:9-10](/Sunshine/src/platform/windows/display_vr.cpp#L9-L10)
  - [Sunshine/src/platform/windows/display_vr.cpp:1049-1088](/Sunshine/src/platform/windows/display_vr.cpp#L1049-L1088)
  - [Sunshine/src/platform/windows/display_vr.cpp:1203](/Sunshine/src/platform/windows/display_vr.cpp#L1203)
- **建議**：將引用修正為 `SW/display_vr.cpp:1049-1088, 1203`。

### 3. [R-3] 客戶端 `vrframemeta.*` 結構職責與行程全域靜態單例（Singleton）描述失實
- **位置**：[linux_vr_backend.md:280-283](/docs/specs/linux_vr_backend.md#L280-L283)（`§3.4` `moonlight-qt` 變更表）
- **問題**：設計稿引用 [docs/vr_architecture.md:295-297](/docs/vr_architecture.md#L295-L297) 稱「0x81 的 pts／frame number 查表分眼各做一份」，並將「配對鍵 `(eye, frameNumber)`；只有兩眼都齊才讓 projection 換圖」寫在 `MQ/streaming/vr/vrframemeta.*`。實際上 `vrframemeta.h` 分為三個結構：
  1. `VrFrameMetaTracker`（`m_Slots[64]`）：由 `FFmpegVideoDecoder` 實例成員持有，開第二個解碼器會自動有第二份。
  2. `VrRenderMetaRing`（`s_Entries[128]`）與 `VrDecodeTiming`（`s_Samples[64]`）：**皆為行程全域 `static` 單例**！`VrRenderMetaRing::publish(pts, meta)` 與 `lookup(pts, &meta)` 僅以 `pts`（即 `frameNumber`）為鍵，沒有 `eye` 欄位；當左右兩眼解碼器使用相同的 `frameNumber` 呼叫 `publish` 時，兩眼會在同一個 `pts` 發生碰撞覆寫。
  3. 控制 projection 換圖與材質發佈的邏輯實際位於 `XrVideo`（`xrvideo.cpp`）與 `XrContext`（`xrcontext.cpp`），不在 `vrframemeta.*`。
- **證據（檔案:行號）**：
  - [moonlight-qt/app/streaming/vr/vrframemeta.h:26-68, 70-135](/moonlight-qt/app/streaming/vr/vrframemeta.h#L26-L68)
  - [moonlight-qt/app/streaming/vr/vrframemeta.cpp:194-227](/moonlight-qt/app/streaming/vr/vrframemeta.cpp#L194-L227)
  - [moonlight-qt/app/streaming/xr/xrvideo.cpp:250-355](/moonlight-qt/app/streaming/xr/xrvideo.cpp#L250-L355)
- **建議**：在 `§3.4` 明確區分：`VrRenderMetaRing` 與 `VrDecodeTiming` 必須加上 `eye` 維度（配對鍵 `(eye, pts)`），而「兩眼都到齊才推進 projection 換圖」應移至 `MQ/streaming/xr/xrvideo.*` 與 `xrcontext.cpp`。

*(註：其餘引用如 `direct_mode.cpp:365`、`tracking.cpp:131`、`vr_session.cpp:429`、`vr_session.h:161`、`vr_ipc_abi.h:322, 348`、`vr_stub.cpp:21-23, 44-49`、`misc.cpp:1172-1177`、`kmsgrab.cpp:638-644`、`VipleVr.h:45, 67`、`VideoDepacketizer.c:1248-1302`、`vrlaunchparams.cpp:104-106`、`xrcontext.cpp:2614-2644, 5553-5561, 5570, 5623`、`xrvideo.cpp:1916`，以及 `temp/vrdual_src` 與 UEVR 原始碼經核對皆存在且行號正確。)*

---

## 二、技術上不成立或未考慮周全之處

### 4. [T-1] 【致命阻斷】NVENC SDK 不接受 Vulkan `VkDevice`／`VkImage`，且 Linux 端 Sunshine 並未使用 `nvenc_base.cpp`
- **位置**：[linux_vr_backend.md:86, 89, 126-127, 269, 272](/docs/specs/linux_vr_backend.md#L86-L127)（`§1.3`、`§1.4`、`§3.3`）
- **問題**：
  1. 設計稿稱「layer 在那張卡登錄 NVENC」、「像素從產生到 NVENC 都在同一張 `VkDevice`」、「各開一個 NVENC session，把 `VkImage` 壓成 HEVC」。但 **NVIDIA Video Codec SDK (`nvEncodeAPI.h`) 根本不支援 Vulkan 裝置或 `VkImage` 作為輸入資源**！`NV_ENC_DEVICE_TYPE` 僅有 `DIRECTX`、`CUDA`、`OPENGL`；`NV_ENC_INPUT_RESOURCE_TYPE` 僅有 `DIRECTX`、`CUDADEVICEPTR`、`CUDAARRAY`、`OPENGL_TEX`。
  2. 設計稿在 `:127` 稱「桌面路徑已經用 NVENC SDK」，但 Linux 端 Sunshine **並未編譯 `Sunshine/src/nvenc/`**，而是走 FFmpeg `avcodec` + [Sunshine/src/platform/linux/cuda.cpp](/Sunshine/src/platform/linux/cuda.cpp#L277-L466)，且 `cuda.cpp:291-292` 硬編碼了 `open_drm_fd_for_cuda_device(0)`（註記 `// TODO: Support more than one CUDA device`）。
  3. 在 Linux 若要零拷貝把 `VkImage` 送進 NVENC，必須在兩張卡各自建立 `CUcontext`，透過 `VK_KHR_external_memory_fd` 與 `VK_KHR_external_semaphore_fd` 將 `VkDeviceMemory` 與 `VkSemaphore` 匯入 CUDA（`cuImportExternalMemory` / `cuImportExternalSemaphore`），並轉換色彩空間（或以 pitch-linear / `CUarray` 註冊給 `NV_ENC_DEVICE_TYPE_CUDA`）。此外，在 Proton 遊戲行程內載入兩張卡的 `CUcontext` 會額外消耗每卡約 200–300 MB VRAM（GTX 1660 Ti 僅 6 GB）。
- **證據（檔案:行號）**：
  - [Sunshine/third-party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h:2826-2848, 2943-2956](/Sunshine/third-party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h#L2826-L2848)
  - [Sunshine/src/nvenc/nvenc_base.cpp:89-105](/Sunshine/src/nvenc/nvenc_base.cpp#L89-L105)
  - [Sunshine/src/platform/linux/cuda.cpp:277-315](/Sunshine/src/platform/linux/cuda.cpp#L277-L315)
- **建議**：
  - 明確補齊 **Vulkan (`VK_KHR_external_memory_fd` / `DMA_BUF`) → CUDA (`CUcontext` on GPU 0 & GPU 1) → NVENC (`NV_ENC_DEVICE_TYPE_CUDA`)** 的零拷貝匯入與同步管線。
  - 更佳做法：Layer 不載入 CUDA/NVENC，只將兩張卡擷取到的 `VkImage` 以 DMA-BUF FD + sync FD 透過 `SCM_RIGHTS` 傳給 `viplestream-server`，由 Server 在外部行程管理兩張卡的 `CUcontext` 與 NVENC session（一併解決下方問題 W-3）。

### 5. [T-2] 【致命阻斷】`vrdual` 攔截並隱藏 `external_memory` 擴充，直接破壞 Proton `wineopenxr` 與 Vulkan→CUDA 匯出
- **位置**：[linux_vr_backend.md:57-60, 86, 133-138, 307-314](/docs/specs/linux_vr_backend.md#L57-L60)（`§1.2`、`§1.4`、`§3.6`）
- **問題**：
  1. UEVR 在 Proton 內使用 `XR_KHR_D3D11_enable` 將 DXVK 的 `ID3D11Device*` 傳給 `wineopenxr`，`wineopenxr` 則透過 DXVK 的 Vulkan interop 與 Linux 端 OpenXR runtime（`XR_KHR_vulkan_enable` / `XR_KHR_vulkan_enable2`）建立共享 swapchain `VkImage`。
  2. 無論是 `wineopenxr` 跨界交換材質或問題 T-1 的 Vulkan→CUDA 匯入，都需要 `VK_KHR_external_memory_fd` 與 `VK_KHR_external_semaphore_fd`。
  3. 但設計稿直接沿用的 `temp/vrdual_src` mirror layer 在 `vkEnumerateDeviceExtensionProperties` **主動隱藏了 `external_memory (dma_buf, host)`**！一旦隱藏，`wineopenxr` 與 Vulkan→CUDA 外部記憶體匯出將直接失敗。
- **證據（檔案:行號）**：
  - [temp/vrdual_src/docs/ARCHITECTURE.md:253-257](/temp/vrdual_src/docs/ARCHITECTURE.md#L253-L257)
  - [temp/vrdual_src/layer/vrdual_mirror.hpp:1-120](/temp/vrdual_src/layer/vrdual_mirror.hpp#L1-L120)
  - UEVR/src/mods/vr/d3d11/D3D11Component.cpp:74-135
- **建議**：在 `§3.3` 與 `§3.6` 明確規範：Layer 底層對驅動呼叫 `pfn_CreateDevice` 建立 Device A 與 Device B 時，必須強制啟用 `VK_KHR_external_memory`、`VK_KHR_external_memory_fd`、`VK_EXT_external_memory_dma_buf`、`VK_KHR_external_semaphore_fd`；同時自製 pose runtime 作為 in-process OpenXR runtime `.so` 實作 `XR_KHR_vulkan_enable2` 時，其 swapchain `VkImage` 必須經由 Layer 在雙卡上對應建立。

### 6. [T-3] 【致命阻斷】以 `present_index` 奇偶（`2F` vs `2F+1`）判定左右眼並配對，存在 50% 左右眼反轉與跨世界 Tick 錯配風險
- **位置**：[linux_vr_backend.md:101-102, 121-125, 203, 412](/docs/specs/linux_vr_backend.md#L101-L125)（`§1.3`、`§1.4`、`§2.3`、`§5`）
- **問題**：
  設計稿假設在 UEVR `SYNCHRONIZED` 下，Vulkan layer 的 `vkQueuePresentKHR` 次數 `present_index` 必然是「偶數 `2F` = 左眼（GPU A）、奇數 `2F+1` = 右眼（GPU B），配對鍵為 `present_index / 2`」。
  對照 UEVR 原始碼，此假設不成立：
  1. UEVR 在 VR.cpp:2070, 2129, 2202 判定左眼的條件是：`is_left_eye_frame = is_using_afr() ? (m_render_frame_count % 2 == m_left_eye_interval) : true;`。
  2. 遊戲啟動、載入畫面、OpenXR 尚未 `ready()` 前，DXVK 已經呼叫過任意次 `vkQueuePresentKHR`，且使用者可在 UEVR 切換 `m_left_eye_interval`（0 或 1）。因此 Layer 的 `present_index` 與 UEVR 的 `m_render_frame_count` 有 50% 機率奇偶相反！
  3. 一旦奇偶相反，Layer 會把「Tick $N$ 的右眼」當成左眼交給 GPU A，把「Tick $N+1$ 的左眼」當成右眼交給 GPU B，並配成同一個 `frameNumber = F`。這不僅造成**左右眼立體視覺永久反轉**，還會將兩個不同世界 Tick 的左右眼湊成一幀！
- **證據（檔案:行號）**：
  - UEVR/src/mods/VR.cpp:2070-2135, 2190-2246
  - UEVR/src/mods/vr/d3d11/D3D11Component.cpp:530-563
- **建議**：廢除以 `present_index % 2` 盲猜左右眼。在 UEVR `SYNCHRONIZED` 模式下（D3D11Component.cpp:542-562），左眼固定 acquire/release OpenXR `swapchain_0`（不呼叫 `xrEndFrame`），右眼固定 acquire/release `swapchain_1` 並呼叫 `xrEndFrame`。由自製 pose runtime 根據被 acquire 的 swapchain index 與 `xrEndFrame` 明確通知 Layer 當前 Present 是「Tick $F$ 的左眼」還是「Tick $F$ 的右眼」。

### 7. [T-4] 【嚴重缺陷】缺少從 Pose Runtime 回傳「該幀實際使用的 `renderPose` 與 `echoSampleId`」至 `0x81` 標頭的機制
- **位置**：[linux_vr_backend.md:106-118, 188, 310-312, 323](/docs/specs/linux_vr_backend.md#L106-L118)（`§1.3`、`§2.2`、`§3.6`）
- **問題**：
  設計稿規定 `0x81` 標頭每路攜帶相同的頭部 `renderPose` 與 `echoSampleId`（`:188`），卻規定「layer 不讀姿態」（`:323`），且「pose runtime 只讀 shm 裡的最新姿態回給 `xrLocateViews`」（`:109, 310-312`），完全沒設計從 pose runtime 把「Frame `F` 實際使用的 `echoSampleId` 與 `renderPose`」傳回 `viplestream-server` 的通道！
  對照 UEVR OpenXR.cpp:116-288，Unreal Engine 的 Game / Render / RHI 執行緒有 3 槽位管線（`OpenXR::QUEUE_SIZE = 3`），當 RHI 執行緒完成 `vkQueuePresentKHR` 時，shm 裡的最新姿態已經比該幀在 Game 執行緒呼叫 `xrLocateViews` 的姿態超前 1～2 幀（11～22 ms）。若 Server 直接拿 shm 當下最新姿態填進 `0x81`，頭戴顯示器做 ATW 旋轉補償時會因 `renderPose` 超前而產生嚴重的畫面拉扯（swimming）。
- **證據（檔案:行號）**：
  - UEVR/src/mods/vr/runtimes/OpenXR.cpp:116-288
  - [Sunshine/src/platform/windows/steamvr_driver/direct_mode.cpp:389-425](/Sunshine/src/platform/windows/steamvr_driver/direct_mode.cpp#L389-L425)
  - [docs/steamvr_driver.md:58-74](/docs/steamvr_driver.md#L58-L74)
- **建議**：在自製 pose runtime 維護 `displayTime -> (sampleId, renderPose)` 查詢表（或套用 `pose_history.cpp` 的 `match()` 比對 `xrEndFrame` 提交的 `projection_layer_views[i].pose`），在 `xrEndFrame` 時將該輪立體幀 `F` 對應的 `(echoSampleId, renderPose)` 寫入 shm 供 Server 封裝 `0x81`。

### 8. [T-5] 【嚴重缺陷】UEVR `SYNCHRONIZED` 預設配置雙倍寬（`2W×H`）Backbuffer 浪費 VRAM，且主機端髒頁透過 PCIe 3.0 x4 雙份上傳
- **位置**：[linux_vr_backend.md:58-59, 64-73, 121-125, 166](/docs/specs/linux_vr_backend.md#L58-L73)（`§1.2`、`§1.4`、`§2.1`）
- **問題**：
  1. 設計稿注意到 `D3D11Component.cpp:547` 裁切了 backbuffer 左半邊 `[0, width/2)`，但未注意到 UEVR 預設將 Unreal Engine backbuffer 撐成 `2W×H`（例如 `3456×1728`），在 AFR 下右半邊 `[W, 2W)` 完全閒置（D3D11Component.cpp:633-647）。在 6 GB VRAM 的 GTX 1660 Ti 上，以 `2W×H` 配置所有 G-Buffer / Render Target 極易耗盡 VRAM。
  2. 根據 [temp/vrdual_src/docs/ARCHITECTURE.md:178-228](/temp/vrdual_src/docs/ARCHITECTURE.md#L178-L228)，即使在 P4 每張卡只執行單眼繪製指令，所有 Host-Visible 記憶體（Dynamic Constant Buffer / Staging Upload）在每次 `vkQueueSubmit` 時仍須透過 `PAGEMAP_SCAN` / `userfaultfd` 掃描髒頁並複製到兩張卡，第二張卡受限於晶片組 PCIe 3.0 x4（約 3.5 GB/s）。
- **證據（檔案:行號）**：
  - UEVR/src/mods/vr/d3d11/D3D11Component.cpp:547, 633-647
  - [temp/vrdual_src/docs/ARCHITECTURE.md:178-228](/temp/vrdual_src/docs/ARCHITECTURE.md#L178-L228)
- **建議**：擷取點可直接取用 UEVR 在 `D3D11Component.cpp:550` 寫入的 OpenXR swapchain `VkImage`（本身即為單眼 `W×H`），並評估啟用 UEVR `extreme_compat_mode` 讓引擎 backbuffer 維持 `W×H` 以節省 6 GB VRAM；同時在 P3/P4 納入每幀 host dirty-page 同步耗時與頻寬監控。

---

## 三、與既有 Wire／IPC 相容性的衝突

### 9. [W-1] 【致命阻斷】`moonlight-common-c` 的 `VideoStream.c` 與 `VideoDepacketizer.c` 全為檔案級 `static` 全域變數，無法直接「掛第二個實例」
- **位置**：[linux_vr_backend.md:186, 249, 279](/docs/specs/linux_vr_backend.md#L186)（`§2.2`、`§3.1`、`§3.4`）
- **問題**：設計稿稱「兩個埠各接一個現成的 depacketizer，序號與 FEC 自然分開，**改動面比較小**」（`:186`）。但實際上 `VideoStream.c` 與 `VideoDepacketizer.c` 並非物件或 struct 實例，而是使用了數十個檔案範圍 `static` 全域變數（如 `rtpQueue`、`rtpSocket`、`decryptionCtx`、`nalChainHead`、`nextFrameNumber`、`waitingForIdrFrame`、`frameVrMeta`、`decodeUnitQueue`），且 `LiWaitForNextVideoFrame` 等 API 無任何 stream/eye 索引參數。
- **證據（檔案:行號）**：
  - [moonlight-qt/moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c:11-50, 228](/moonlight-qt/moonlight-common-c/moonlight-common-c/src/VideoDepacketizer.c#L11-L50)
  - [moonlight-qt/moonlight-common-c/moonlight-common-c/src/VideoStream.c:16-70, 212-225, 299-305](/moonlight-qt/moonlight-common-c/moonlight-common-c/src/VideoStream.c#L16-L70)
- **建議**：在 `§3.1` 明確規劃將 `VideoStream.c` 與 `VideoDepacketizer.c` 的 `static` 全域狀態重構為雙實例陣列 `ctx[2]`（含帶 `eyeIndex` 的 API 與 callback），並同步套用至 `Q/`、`A/`、`S3/`。

### 10. [W-2] 【致命阻斷】新增 `eyePort` 忽略了 Sunshine 的 UDP Ping 客戶端位址學習與 AES-GCM IV 計數器安全
- **位置**：[linux_vr_backend.md:183, 185-186, 250, 260](/docs/specs/linux_vr_backend.md#L183-L186)（`§2.2`、`§3.1`、`§3.2`）
- **問題**：
  1. **UDP Ping 位址學習**：Sunshine 在客戶端從其視訊 UDP socket 發送帶有 `X-SS-Ping-Payload` 的 UDP Ping 封包前，根本不知道客戶端視訊 socket 的來源位址與埠號（[stream.cpp:5711-5736](/Sunshine/src/stream.cpp#L5711-L5736)）。設計稿只在 `/launch` XML 回傳 `eyePort`，未定義客戶端如何對 `eyePort` 發送 UDP Ping，也未讓 `recvThread` 監聽 `eyePort`。
  2. **AES-GCM Nonce Reuse 與執行緒競爭**：視訊加密在 [stream.cpp:6736-6754](/Sunshine/src/stream.cpp#L6736-L6754) 使用同一把 `rikey` 與 `session->video.gcm_iv_counter`（`iv[11] = 'V'`）。若左右兩路共用同一把 key 且各自從 0 計數 IV，將造成 **AES-GCM Nonce Reuse（嚴重密碼學漏洞）**；若共用同一個 `cipher` 物件則會引發多執行緒 Data Race。
- **證據（檔案:行號）**：
  - [Sunshine/src/stream.cpp:5711-5736, 6186-6330, 6736-6754](/Sunshine/src/stream.cpp#L5711-L5736)
  - [Sunshine/src/rtsp.cpp:1019-1050](/Sunshine/src/rtsp.cpp#L1019-L1050)
- **建議**：在 `recvThread` 加入 `eye_video_sock` 接收右眼 UDP Ping 以學習客戶端右眼 `sockaddr`；為右眼建立獨立的 `video::cipher` 實例並隔離 IV 命名空間（例如左眼 `iv[11] = 'V'`、右眼 `iv[11] = 'R'`）。

### 11. [W-3] 【致命阻斷】跳過 `SS/video.cpp` 且將 NVENC 放入遊戲 Layer，切斷了 IDR、Intra-Refresh Wave、ABR、動態降頻與 100 ms 重送幀等所有控制迴路
- **位置**：[linux_vr_backend.md:129, 196-197, 261](/docs/specs/linux_vr_backend.md#L129)（`§1.4`、`§2.2`、`§3.2`）
- **問題**：設計稿將 NVENC 放在遊戲行程的 Vulkan Layer，透過單向 shm 送出 HEVC 位元流，並讓 `SS/video.cpp`「跳過桌面編碼執行緒」（`:261`）。但既有的 [Sunshine/src/video.cpp:2523-3253](/Sunshine/src/video.cpp#L2523-L3253) 負責處理：
  1. `0x11` LOSS 觸發的 `mail::vr_refresh` Intra-Refresh Wave 狀態機（`forceIntraRefreshWithFrameCnt` 與 `VIPLE_VR_FF_REFRESH_DONE` 標記）；
  2. 連線初期與重置時的 `mail::idr` 請求及 300 ms VR IDR Cooldown；
  3. `mail::bitrate_change`（ABR 動態調整 NVENC 位元率）；
  4. `§VR-RATE-AUTO`（`0x5507/0E` 動態半幀率跳幀與 `S2C_RATE` 回應）；
  5. 遊戲卡頓超過 100 ms 時重送上一幀（`VIPLE_VR_FF_REPEATED`）。
  在單向 shm 且跳過 `video.cpp` 的設計下，遊戲 Layer 內的兩顆 NVENC 完全收不到上述任何控制指令！
- **證據（檔案:行號）**：
  - [Sunshine/src/video.cpp:2567, 2879-3017, 3075-3164](/Sunshine/src/video.cpp#L2879-L3017)
  - [Sunshine/src/stream.cpp:2008-2065](/Sunshine/src/stream.cpp#L2008-L2065)
- **建議**：將 NVENC 移回 `viplestream-server`（Layer 僅透過 DMA-BUF FD 傳遞雙卡擷取影像，Server 跑雙眼 `encode_run` 執行緒）；若堅持在 Layer 內編碼，則必須在 shm 定義完整的雙向控制通道（`per_eye_encoder_ctrl_t[2]`）以傳遞 IDR、Intra-Refresh Wave、ABR 與半幀率指令。

### 12. [W-4] 【嚴重缺陷】LATCH（`0x5507/08`）相位鎖定在 Linux 端為 No-op 空殼，未接通至 Pose Runtime 的 `xrWaitFrame`
- **位置**：[linux_vr_backend.md:198, 258, 309-310](/docs/specs/linux_vr_backend.md#L198)（`§2.2`、`§3.2`、`§3.6`）
- **問題**：設計稿稱「LATCH、時鐘對映都沿用」（`:198`），但 `vr_session.cpp:740-746` 收到 `0x5507/08 LATCH` 後呼叫的 `vr::bridge::set_pacing_ppm` 與 `set_pacing_step` 在 Linux [vr_stub.cpp:216-220](/Sunshine/src/platform/linux/vr_stub.cpp#L216-L220) 是空的 no-op，且設計稿未規定將 `vripc_pacing_t` 寫入 shm 供 pose runtime 的 `xrWaitFrame` 微調幀週期，長時間串流必然因時鐘漂移發生週期性掉幀。
- **證據（檔案:行號）**：
  - [Sunshine/src/vr/vr_session.cpp:740-746](/Sunshine/src/vr/vr_session.cpp#L740-L746)
  - [Sunshine/src/platform/linux/vr_stub.cpp:216-220](/Sunshine/src/platform/linux/vr_stub.cpp#L216-L220)
  - [Sunshine/src/vr/vr_ipc_abi.h:221-244](/Sunshine/src/vr/vr_ipc_abi.h#L221-L244)
- **建議**：在 `SL/vr_linux.cpp` 實作 `set_pacing_ppm` 與 `set_pacing_step` 寫入 shm `vripc_pacing_t`，並讓 pose runtime 的 `xrWaitFrame` 比照 Windows `virtual_vsync.h` 微調週期。

### 13. [W-5] 【嚴重缺陷】違反 `check_commonc_sync.ps1` 三庫位元組同步規定（漏列 `A/` 與 `S3/` 的 `.c`/`.h` 同步）
- **位置**：[linux_vr_backend.md:236, 246-249](/docs/specs/linux_vr_backend.md#L236-L249)（`§3.1` 協定與共用標頭）
- **問題**：設計稿在 `:249` 只列出修改 `Q/VideoDepacketizer.c`，卻忽略 [docs/vr_protocol.md:300-305](/docs/vr_protocol.md#L300-L305) 規定 `check_commonc_sync.ps1` 會強制檢查 `Q/`（`moonlight-qt`）與 `A/`（`moonlight-android`）下所有 `src/*.c`、`src/*.h` 是否 byte-identical。只改 `Q/` 會導致建置直接失敗（`exit /b 1`）。
- **證據（檔案:行號）**：
  - [docs/vr_protocol.md:300-305](/docs/vr_protocol.md#L300-L305)
  - [linux_vr_backend.md:246-249](/docs/specs/linux_vr_backend.md#L246-L249)
- **建議**：在 `§3.1` 增列 `A/`（`moonlight-android`）與 `S3/`（`Sunshine`）所有對應之 `moonlight-common-c/src/*` 檔案同步更新。

### 14. [W-6] 【中度衝突】變更 `packed` 寬度語意（從 `2W×H` 改為 `W×H`）會導致客戶端 VR 預設總位元率被砍半
- **位置**：[linux_vr_backend.md:166, 251, 278](/docs/specs/linux_vr_backend.md#L166)（`§2.1`、`§3.1`、`§3.4`）
- **問題**：客戶端在 [session.cpp:2197-2204](/moonlight-qt/app/streaming/session.cpp#L2197-L2204) 依 `m_StreamConfig.width * m_StreamConfig.height * fps` 對照 `3456 * 1728 * 90 = 150 Mbps` 計算預設位元率。若 `layout=dual` 將 `packed` 改為單眼 `1728x1728`，總位元率會被自動砍半為 75 Mbps，且設計稿未定義每顆 NVENC 是分配 `bitrate / 2` 還是 `bitrate`。
- **證據（檔案:行號）**：
  - [moonlight-qt/app/streaming/session.cpp:2197-2204](/moonlight-qt/app/streaming/session.cpp#L2197-L2204)
  - [Sunshine/src/vr/vr_session.h:160-166](/Sunshine/src/vr/vr_session.h#L160-L166)
- **建議**：明確定義 `StreamConfig.bitrate` 為「雙眼總位元率」，客戶端在 `layout=dual` 下計算預設位元率時將畫素面積乘以 2，伺服器端為每顆 NVENC 分配 `total_bitrate / 2`。

---

## 四、缺漏的驗證方法

### 15. [V-1] 缺少封包遺失恢復（`0x11` 分眼 LOSS → Intra-Refresh Wave → `REFRESH_DONE`）與 IDR 注入測試
- **位置**：[linux_vr_backend.md:423-435](/docs/specs/linux_vr_backend.md#L423-L435)（`§6` 驗證方法）
- **問題**：設計稿擴充了 `0x11` 第 10 byte `eye` 欄位，但 `§6` 完全沒有測試單眼掉包恢復流程（對照 [docs/vr_architecture.md:803](/docs/vr_architecture.md#L803) 強制要求的 `--vr-inject-loss` / `--vr-inject-drop` 驗證）。
- **證據（檔案:行號）**：
  - [docs/vr_architecture.md:803](/docs/vr_architecture.md#L803)
  - [linux_vr_backend.md:196-197, 423-435](/docs/specs/linux_vr_backend.md#L423-L435)
- **建議**：在 `§6` 加入分別對左眼埠與右眼埠執行 `--vr-inject-loss`，驗證 `0x11` 的 `eye` 欄位正確且僅觸發該眼 `REFRESH_START → REFRESH_DONE`、另一眼不受影響且 `idrCount == 1`。

### 16. [V-2] 缺少 `0x81` `echoSampleId`／`renderPose` 與畫面實際視角一致性的自動化探針驗證
- **位置**：[linux_vr_backend.md:367-370, 429-431](/docs/specs/linux_vr_backend.md#L367-L370)（`§4` P1/P2、`§6`）
- **問題**：設計稿僅靠肉眼觀察「左紅右藍、轉頭畫面跟著動」，無法檢出問題 T-3 的跨 Tick 左右眼錯配 `(R_{N-1}, L_N)`，也無法檢出問題 T-4 的 1～2 幀姿態超前。
- **證據（檔案:行號）**：
  - [docs/vr_architecture.md:316, 745, 803](/docs/vr_architecture.md#L316)
  - [linux_vr_backend.md:429-431](/docs/specs/linux_vr_backend.md#L429-L431)
- **建議**：比照 Windows `vr_probe` / `vrprobedecode.cpp`，在合成畫面與 Layer 測試左上角嵌入 `(tick, eye, sampleId)` 位元圖樣，於客戶端解碼後自動斷言 `left.tick == right.tick`、`left.eye == 0 && right.eye == 1` 且畫面內嵌 `sampleId == 0x81.echoSampleId`（0 mismatch）。

### 17. [V-3] P0 探針未測試「Vulkan → CUDA → NVENC 外部記憶體零拷貝匯入」與 6 GB VRAM 預算
- **位置**：[linux_vr_backend.md:346-358, 425-427](/docs/specs/linux_vr_backend.md#L346-L358)（`§4` P0、`§6`）
- **問題**：P0 只獨立測 NVENC 與 `vulkaninfo`，未驗證雙 `VkDevice` 透過 `VK_KHR_external_memory_fd` 匯入雙 `CUcontext` 再送 NVENC 的零拷貝路徑，亦未規定 6 GB GTX 1660 Ti 的 VRAM 與 PCIe 3.0 x4 髒頁上傳預算。
- **證據（檔案:行號）**：
  - [linux_vr_backend.md:346-358, 425-427](/docs/specs/linux_vr_backend.md#L346-L358)
- **建議**：將 P0 探針升級為「雙 `VkDevice` (`VK_KHR_external_memory_fd`) → 雙 `CUcontext` → 雙 NVENC 零拷貝編碼 60 秒」，並在 P2/P3 加入每卡 VRAM 峰值（< 5.5 GB）與 PCIe x4 每幀上傳耗時門檻。

### 18. [V-4] 缺少 LATCH 閉迴路收斂（`lockR`）與 `check_commonc_sync.ps1` 建置檢查
- **位置**：[linux_vr_backend.md:423-435](/docs/specs/linux_vr_backend.md#L423-L435)（`§6` 驗證方法）
- **問題**：缺少長時間串流下 `[VIPLE-VR-LATCH]` 相位鎖定收斂驗證（`lockR >= 80%`）以及 `check_commonc_sync.ps1` 三庫同步檢查。
- **證據（檔案:行號）**：
  - [docs/vr_architecture.md:803](/docs/vr_architecture.md#L803)
  - [docs/vr_protocol.md:300-305](/docs/vr_protocol.md#L300-L305)
- **建議**：在 `§6` 補齊 `check_commonc_sync.ps1` 檢查與 15 分鐘串流 `[VIPLE-VR-LATCH]` 收斂指標。

---

## 五、整體結論

1. **大方向正確，但底層實作假設存在 5 項阻斷級（Blocker）缺陷**：
   - 設計稿針對雙 GTX 1660 Ti（其中一張位於 PCIe 3.0 x4）提出「不在雙卡間搬移未壓縮像素、於各卡就地執行 NVENC 壓縮、以雙路 RTP 分送頭戴顯示器雙解碼器配對」的頂層架構非常合理，也正確鎖定了 `bufferDeviceAddressCaptureReplay` 與 UEVR `SYNCHRONIZED` 作為前置收斂條件。
   - 但撰稿模型誤以為 **NVENC SDK 可直接吃 Vulkan `VkDevice`／`VkImage`**、忽略了 **`vrdual` 隱藏 `external_memory` 擴充**、誤判了 **UEVR `SYNCHRONIZED` 下 `present_index` 的奇偶對齊與 3-frame 姿態管線延遲**，並嚴重低估了 **跳過 `video.cpp` 與重構 `moonlight-common-c` 全域 `static` 狀態** 對既有協定與控制迴路的破壞。若不先修正這些缺陷，實作在 P1 與 P2 就會卡死。
2. **核心架構修正建議**：
   - **將 NVENC 從遊戲行程的 Vulkan Layer 移回 `viplestream-server`**：讓 Vulkan Layer 只負責雙卡指令分派與擷取單眼 `VkImage`，並將兩張卡的擷取影像以 DMA-BUF / opaque FD + semaphore FD（`SCM_RIGHTS`）交給 `viplestream-server`；由 `viplestream-server` 啟動兩個 `encode_run` 執行緒（分別綁定 GPU 0 與 GPU 1 的 `CUcontext` + NVENC）。如此既能維持「零 CPU 像素拷貝、不跨 PCIe 搬未壓縮畫面」，又能 100% 沿用 `video.cpp` 的 IDR、Intra-Refresh Wave、ABR、`§VR-RATE-AUTO` 與 100 ms 重送幀機制，並省去在 Proton 遊戲行程內載入 CUDA/NVENC 的相容性風險。

---

## 六、優先修正的前 5 項

1. **【第 1 優先｜技術阻斷】補齊 Linux 上 Vulkan `VkImage` 進 NVENC 的 CUDA 外部記憶體橋接，並將 NVENC 移回 `viplestream-server`（或建立雙向控制 IPC）**（對應 **T-1、T-2、W-3**）
   - 修正「NVENC 直接吃 `VkImage`」的假設，定義 `VK_KHR_external_memory_fd` + `VK_KHR_external_semaphore_fd` → `cuImportExternalMemory` / `cuImportExternalSemaphore` → NV12 → NVENC (`NV_ENC_DEVICE_TYPE_CUDA`) 管線；在 Layer 底層對驅動強制啟用 external memory/semaphore 擴充，並恢復 `video.cpp` 的 IDR／Intra-Refresh Wave／ABR 控制迴路。
2. **【第 2 優先｜協定與資安阻斷】補齊雙路 UDP (`eyePort`) 的客戶端 Ping 位址學習、AES-GCM IV 隔離，以及 `moonlight-common-c` 全域 `static` 狀態的多實例化重構**（對應 **W-1、W-2、W-5、R-3**）
   - 將 `VideoStream.c` 與 `VideoDepacketizer.c` 的 `static` 全域變數重構為雙實例 context（同步更新 `Q/`、`A/`、`S3/`），為 `eyePort` 補上 UDP Ping 位址學習與獨立的 AES-GCM IV 命名空間（杜絕 Nonce Reuse），並為 `VrRenderMetaRing` 加上 `eye` 維度。
3. **【第 3 優先｜姿態與節拍同步】補齊 Pose Runtime → Server 的 `(frameNumber, echoSampleId, renderPose)` 回傳通道與 LATCH（`0x5507/08`）節拍微調**（對應 **T-4、W-4**）
   - 由 Pose Runtime 在 `xrLocateViews`／`xrEndFrame` 記錄該幀 `displayTime` 實際對應的 `echoSampleId` 與 `renderPose` 寫入 shm 供 `0x81` 使用（消除 UE5 3-frame pipeline 造成的 1～2 幀姿態超前），並將 LATCH `vripc_pacing_t` 接通至 Pose Runtime 的 `xrWaitFrame`。
4. **【第 4 優先｜立體視覺正確性】廢除盲目的 `present_index % 2` 奇偶左右眼判定，改由 Pose Runtime 的 OpenXR Swapchain / `xrEndFrame` 事件精確標定左右眼與世界 Tick**（對應 **T-3**）
   - 利用 UEVR `SYNCHRONIZED` 模式下左眼寫入 `swapchain_0`、右眼寫入 `swapchain_1` 並呼叫 `xrEndFrame` 的明確邊界標定 `(tick, eye)`，消除 50% 左右眼反轉與跨 Tick 錯配 `(R_{N-1}, L_N)`。
5. **【第 5 優先｜驗證門檻】補齊雙卡 Vulkan→CUDA→NVENC 零拷貝探針、Pose-to-Pixel 位元圖樣自動化斷言、以及分眼掉包（`--vr-inject-loss`）恢復驗證**（對應 **V-1、V-2、V-3、V-4**）
   - 在 `§6` 增列雙卡 Vulkan→CUDA→NVENC 零拷貝探針、6 GB VRAM 與 PCIe 3.0 x4 頻寬驗證、畫面內嵌 `(tick, eye, sampleId)` 自動化比對（0 mismatch），以及單眼掉包僅觸發該眼 Intra-Refresh Wave 的閉迴路驗證。
