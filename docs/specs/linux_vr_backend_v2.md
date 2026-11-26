# Linux VR 後端技術規格書（第二版 v2）

> 對應分支：`feat/linux-vr-backend`
> 前版文件：`docs/specs/linux_vr_backend.md`（第一版，保留原檔不修改）
> 審查依據：`docs/specs/linux_vr_backend_review.md`（共 18 項審查意見，含 6 項【致命阻斷】）
> 目標平台：**CachyOS (Arch Linux, x86_64)** 主機 + **Steam Frame (Arch Linux ARM64, Snapdragon 8 Gen 3, 2160×2160/眼 @ 90 Hz)** 頭盔
> 首波驗收遊戲：**Palworld（幻獸帕魯）**（Steam AppID `1623730`，Unreal Engine 5.1，D3D11/D3D12 經 Proton + DXVK/vkd3d-proton 轉 Vulkan，搭配 **UEVR** 注入器走 OpenXR **Synchronized AFR**）
> 硬體假設：主機插 **兩張 NVIDIA GeForce GTX 1660 Ti（各 6 GB GDDR6 VRAM）**（GPU 0 走 PCIe 3.0 x16、GPU 1 走晶片組 PCIe 3.0 x4）

---

## 審查意見逐項回應（18 項全數處理）

本節逐一回應 `docs/specs/linux_vr_backend_review.md` 所列之 18 項問題（R-1～R-3、T-1～T-5、W-1～W-6、V-1～V-4）。每項皆標明**處置結果（採納／部分採納／不採納）**、**理由與程式碼查證結果**，以及**寫入本版規格本文的具體章節**。

### 審查回應總覽表

| 編號 | 嚴重度 | 審查問題摘要 | 處置 | 修正落地章節 |
|---|---|---|---|---|
| **R-1** | 中（引用失實） | `SS/stream.cpp:6687` 並非寫入 `rtp.ssrc`；影片 RTP 實際在 `:6731-6733` 填標頭（未設 `ssrc`），音訊在 `:6965` 設 `ssrc = 0` | **採納** | §0.2、§2.2 |
| **R-2** | 低（引用失準） | `SW/display_vr.cpp:9-10` 只是檔案頭註解，CPU fence wait 實作在 `:1048-1088`；另更新 `xrcontext.cpp` 目前工作樹實際行號 | **採納** | §0.1、§0.2、§1.7 |
| **R-3** | 中（架構混淆） | `VrFrameMetaTracker` 是每解碼器實例成員，真正全行程 `static` 單例是 `VrRenderMetaRing` 與 `VrDecodeTiming`；左右眼同步把關在 `XrVideo`／`XrContext` | **採納** | §0.2、§2.4、§2.7 |
| **T-1** | **致命阻斷** | NVENC SDK 不直接吃 `VkDevice`/`VkImage`；Sunshine Linux 現況走 FFmpeg `avcodec` (`AV_HWDEVICE_TYPE_CUDA`) + `gl_cuda_vram_t`（寫死 GPU 0 且不支援 VR 隨選 Intra-Refresh），在遊戲行程內塞 CUDA 會吃掉每卡 250 MB VRAM | **採納** | §0.2、§1.1、§1.5、§1.6 |
| **T-2** | **致命阻斷** | `vrdual` 預設把 `VK_EXT_external_memory_dma_buf`、`VK_EXT_external_memory_host`、`VK_EXT_image_drm_format_modifier` 等擴充從列舉隱藏（`vrdual_layer.cpp:869-888`），會導致 `wineopenxr` 建 swapchain 失敗 | **採納** | §0.2、§1.2、§1.4 |
| **T-3** | **致命阻斷** | 靠 `vkQueuePresentKHR` 的 `present_index % 2` 奇偶判定左右眼與配對，遇載入畫面、單框重送或 `m_left_eye_interval` 切換會產生左右眼顛倒與跨 tick 錯配 `(R_{N-1}, L_N)` | **採納** | §1.3、§3.2、§3.3 |
| **T-4** | 高 | UEVR `OpenXR::QUEUE_SIZE = 3` 管線化下，`xrEndFrame` 讀共享記憶體最新 `0x5506` 會拿新 1～2 幀的姿態覆蓋舊畫面，破壞 ATW 重投影 | **採納** | §3.2、§3.4 |
| **T-5** | 高 | GTX 1660 Ti 僅 6 GB VRAM，雙倍寬 `2W×H` G-Buffer 易爆 VRAM；且 `vrdual` 鏡像模式的 `PAGEMAP_SCAN`/`userfaultfd` 每幀搬 dirty host page 過 PCIe 3.0 x4 會有頻寬瓶頸 | **採納** | §1.3、§1.4、§1.8 |
| **W-1** | **致命阻斷** | `moonlight-common-c` 的 `VideoStream.c`、`VideoDepacketizer.c`、`RtpVideoQueue.c` 充斥數十個 `static` 全域變數，不重構無法跑第二條視訊流 | **採納** | §2.4、§2.7 |
| **W-2** | **致命阻斷** | 新開 `eyePort` 若無獨立 UDP Ping 位址學習，伺服器不知往哪送封包；雙執行緒共用 `session->video.gcm_iv_counter` 與 `cipher` 會造成資料競爭或 AES-GCM Nonce 重用（NIST SP 800-38D 致命安全漏洞） | **採納** | §2.2、§2.5 |
| **W-3** | **致命阻斷** | 原規格將 NVENC 搬進遊戲 Layer 並跳過 `SS/video.cpp`，直接廢掉 IDR 300 ms 冷卻、隨選 Intra-Refresh Wave、`mail::bitrate_change` ABR、`§VR-RATE-AUTO` 除頻與 100 ms 重送幀 | **採納** | §1.5、§2.6 |
| **W-4** | 高 | `SL/vr_stub.cpp:216-228` 的 `vr::bridge::set_pacing_ppm` / `set_pacing_step` 為空函式，未接通 `0x5507/08 LATCH` 迴路會使端到端排隊延遲漂移 0～11.1 ms | **採納** | §3.5 |
| **W-5** | 中 | 修改 `Q/` 下的 `moonlight-common-c` 未同步 `A/`（Android）與 `S3/`（Sunshine），會被 `check_commonc_sync.ps1` 擋下 | **採納** | §0.1、§2.7 |
| **W-6** | 中 | `layout=dual` 下 `m_StreamConfig.width` 改為單眼寬度，會使 `MQ/streaming/session.cpp:2197-2204` 的預設位元率計算被誤砍半（150 Mbps → 75 Mbps） | **採納** | §2.1、§2.6 |
| **V-1** | 高 | 缺少單眼獨立丟包復原（`0x5507/11` `eye` 欄位 + 單眼 Intra-Refresh Wave）的自動化注入測試 | **採納** | §4.2、§4.3 |
| **V-2** | 高 | 缺少「左右眼顛倒／跨 tick 錯配／Pose-to-Pixel 對齊」的自動化光學或 bit-pattern 驗證（`vr_probe`） | **採納** | §4.2、§4.3 |
| **V-3** | 中 | P0 探針僅測 Vulkan 計算，未涵蓋「Vulkan External Memory FD → 伺服器雙 CUDA Context → 雙 NVENC 零拷貝編碼」與 6 GB VRAM / PCIe x4 dirty page 壓力 | **採納** | §4.1、§4.3 |
| **V-4** | 中 | 缺少 `0x5507/08 LATCH` 相位鎖定收斂驗證（`lockR >= 80%`）與 `check_commonc_sync.ps1` 建置閘門 | **採納** | §4.2、§4.3 |

---

### 18 項審查意見之逐項詳細說明

#### 1. R-1（中 — 引用失實）：`SS/stream.cpp:6687` 的 SSRC 引用錯誤
- **處置**：**採納**。
- **查證結果**：在目前工作樹 `Sunshine/src/stream.cpp` 中：
  - 第 `6684` 行是 `inspect->packet.flags |= FLAG_EOF;`；第 `6687` 行是 `frame_fec_latency_logger.first_point_now();`，完全沒有設定 `ssrc`。
  - 視訊 RTP 封包標頭實際在 `Sunshine/src/stream.cpp:6731-6733` 填寫 `packet->rtp.header`、`packet->rtp.sequenceNumber`、`packet->rtp.timestamp`，**並未寫入 `packet->rtp.ssrc`**（保留記憶體配置時的 0）。
  - 唯一明確寫入 `ssrc = 0` 的地方在音訊廣播執行緒 `Sunshine/src/stream.cpp:6965`（`audio_packet.rtp.ssrc = 0;`）。
- **規格修正**：已在 §0.2 與 §2.2 更正所有行號引用，並維持雙 UDP port（`videoPort` + `eyePort`）方案，不在單一 port 上依賴 SSRC 分流。

#### 2. R-2（低 — 引用失準）：`SW/display_vr.cpp` 與 `MQ/streaming/xr/xrcontext.cpp` 行號校正
- **處置**：**採納**。
- **查證結果**：
  - `Sunshine/src/platform/windows/display_vr.cpp:1-27` 為檔案頭架構註解；真正的 CPU-side fence wait 迴圈位於 `display_vr.cpp:1048-1088`（呼叫 `cs->waiter->wait(target.shared_handle, target.fence_value, wait_budget)` 與超時後的 `late_wait` 輪詢），而 D3D11 左右眼貼圖拷貝位於 `display_vr.cpp:1203`（`device_ctx->CopyResource(eye_staging[e].get(), src_tex)`）。
  - 此外，核對目前工作樹 `moonlight-qt/app/streaming/xr/xrcontext.cpp`，原版規格引用的行號因近期提交已有位移，實際位置為：
    - `frameThreadMain` 的 PCVR 投影分支與 stale 策略（500 ms 淡出、2000 ms 退回載入室）：`xrcontext.cpp:3064-3100`。
    - `XrContext::buildProjection`（設定 `views[e].subImage.imageRect` 與 `[VIPLE-XR] pcvr projection:` 日誌）：`xrcontext.cpp:5983-6046`（其中 `imageRect` 在 `:6020-6029`）。
    - `XrContext::buildInsetQuads`：`xrcontext.cpp:6090-6132`。
- **規格修正**：已在 §0.1、§0.2、§1.7、§2.4 全數更新為目前工作樹的精確行號。

#### 3. R-3（中 — 架構混淆）：`VrFrameMetaTracker` vs `VrRenderMetaRing` / `VrDecodeTiming`
- **處置**：**採納**。
- **查證結果**：在 `moonlight-qt/app/streaming/vr/vrframemeta.h` 與 `vrframemeta.cpp` 中：
  - `VrFrameMetaTracker`（`vrframemeta.h:70-135`、`vrframemeta.cpp:13-191`）是**每個解碼器實例各自擁有的成員物件**，其內部 `m_Ring[64]`（`vrframemeta.h:122`）天然就是 per-decoder 隔離的，只負責把 `0x81`/`0x82` wire header 配對到該解碼器輸出的 `AVFrame->pts`。
  - 真正全行程共用的 `static` 單例是：
    1. `VrRenderMetaRing`（`vrframemeta.h:26-46`、`vrframemeta.cpp:194-227`，靜態陣列 `s_Entries[128]` 與 `s_Mutex` 在 `:201-203`）：以 `pts` 為索引供渲染執行緒查詢 `VrFrameMeta`。
    2. `VrDecodeTiming`（`vrframemeta.h:50-68`、`vrframemeta.cpp:230-260`，靜態陣列 `s_Slots[128]` 與 `s_Lock` 在 `:233-234`）：以 `pts` 為索引記錄 `submitUs` 計算硬體解碼耗時。
  - 左右眼 swapchain 雙緩衝與投影合成把關位於 `moonlight-qt/app/streaming/xr/xrvideo.cpp:245-360` 與 `xrcontext.cpp:3064-3100, 5983-6046`。
- **規格修正**：已在 §2.4 與 §2.7 修正職責劃分：`VrRenderMetaRing` 與 `VrDecodeTiming` 擴充 `eyeIndex` 維度（`s_Entries[2][128]`、`s_Slots[2][128]`），而雙眼同帧同步（Left/Right Barrier）放在 `XrVideo` 與 `XrContext::buildProjection`。

#### 4. T-1（【致命阻斷】）：Linux 端 NVENC 實際路徑查證與跨卡零拷貝編碼架構
- **處置**：**採納（重新設計編碼管線，將 NVENC 移回 `viplestream-server`）**。
- **程式碼查證結果**：
  1. **NVENC API 硬體限制**：查證 `Sunshine/third-party/nv-codec-headers/include/ffnvcodec/nvEncodeAPI.h`，`_NV_ENC_OPEN_ENCODE_SESSIONEX_PARAMS::deviceType`（`:2943-2952`）僅支援 `NV_ENC_DEVICE_TYPE_DIRECTX` (`0x0`)、`NV_ENC_DEVICE_TYPE_CUDA` (`0x1`)、`NV_ENC_DEVICE_TYPE_OPENGL` (`0x2`)；`_NV_ENC_REGISTER_RESOURCE::resourceType`（`:2826-2869`）僅支援 `DIRECTX`、`CUDADEVICEPTR`、`CUDAARRAY`、`OPENGL_TEX`。**NVENC 不支援直接傳入 `VkDevice` 或 `VkImage`**。
  2. **Sunshine 現有 Linux 編碼路徑真相**：
     - 雖然 `Sunshine/cmake/compile_definitions/common.cmake:59-62` 將 `src/nvenc/*.cpp`（含與平台無關的 `Sunshine/src/nvenc/nvenc_base.h:56`、`nvenc_base.cpp:223-1413`）編入所有平台，但查證 `Sunshine/src/video.cpp:540-676` 可知，`encoder_t nvenc` 在 `#ifdef _WIN32`（`:540-578`）才使用原生 `encoder_platform_formats_nvenc`；在 Linux `#elif !defined(__APPLE__)`（`:579-676`）則是走 **`encoder_platform_formats_avcodec`**（透過 FFmpeg 的 `"h264_nvenc"`、`"hevc_nvenc"`、`"av1_nvenc"`，搭配 `AV_HWDEVICE_TYPE_CUDA`、`AV_PIX_FMT_CUDA` 與 `video::cuda_init_avcodec_hardware_input_buffer` 於 `video.cpp:4443-4454`）。
     - 在 `Sunshine/src/platform/linux/cuda.cpp` 中，Linux 抓圖轉編碼走的是 `cuda::gl_cuda_vram_t`（`:277-466`）：把 KMS DMA-BUF 匯入 EGL/OpenGL（`egl::import_source` `:392`），用 GL shader 做 RGB→NV12（`:401-403`），再透過 `cuGraphicsGLRegisterImage`（`:370-371`）與 `cuMemcpy2DAsync`（`:426`）拷貝進 `AVFrame` (`AV_PIX_FMT_CUDA`)。
     - **現有 Linux 路徑的兩大硬傷**：
       - 第一，`gl_cuda_vram_t::init` 在 `cuda.cpp:291-292` 寫死 `open_drm_fd_for_cuda_device(0)`（註解明確寫著 `// TODO: Support more than one CUDA device`），且 `video::cuda_init_avcodec_hardware_input_buffer`（`video.cpp:4447`）呼叫 `av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1)` 固定綁死預設 GPU 0。
       - 第二，`Sunshine/src/video.cpp:4307-4325` 明確寫死：非 `_WIN32` 平台的 `last_encoder_probe_supported_vr_intra_refresh = false`（`:4308-4309` 註解：`隨選 intra refresh（forceIntraRefreshWithFrameCnt）只有 Windows 的原生 NVENC（nvenc_base）實作；AMF、QSV、軟體編碼、Linux 走 avcodec 的 NVENC 都沒有`）。
  3. **為什麼不能在遊戲行程（Vulkan Layer）裡載入 CUDA + NVENC**：每個 `CUcontext` 初始化加上 NVENC internal surfaces 會直接在遊戲行程內吃掉每張卡 **200～350 MB VRAM**，在 6 GB 的 GTX 1660 Ti 上極易把 Palworld 推向 `VK_ERROR_OUT_OF_DEVICE_MEMORY`，且跨行程共享 `CUcontext` 不可行。
- **具體可行替代設計（寫入 §1.1、§1.5、§1.6）**：
  1. **遊戲行程零 CUDA 污染**：遊戲行程內的 Vulkan Layer + In-Process Pose Runtime **完全不載入 `libcuda.so` 或 `libnvidia-encode.so`**。Layer 只在雙卡（`VkDevice` A on GPU 0、`VkDevice` B on GPU 1）建立或重建擷取用 `VkImage`（`VK_FORMAT_R8G8B8A8_UNORM` 或 `VK_FORMAT_B8G8R8A8_UNORM`，Device-Local）與同步用 Timeline/Binary `VkSemaphore` 時，透過 `VK_KHR_external_memory_fd` (`vkGetMemoryFdKHR`) 與 `VK_KHR_external_semaphore_fd` (`vkGetSemaphoreFdKHR`) 匯出檔案描述子（FD），經本機 Unix Domain Socket (`SCM_RIGHTS`) 一次性傳給 `viplestream-server`。
  2. **伺服器端原生 `nvenc_cuda` 繼承 `nvenc::nvenc_base`**：在 `viplestream-server` 新增 `Sunshine/src/platform/linux/nvenc_cuda.h` 與 `nvenc_cuda.cpp`，直接繼承現有的 `nvenc::nvenc_base`（`Sunshine/src/nvenc/nvenc_base.h:56`，建構子傳入 `NV_ENC_DEVICE_TYPE_CUDA` `:223`），以 `dlopen("libnvidia-encode.so.1")` 載入 `NvEncodeAPICreateInstance`。如此可在 Linux 直接複用 `nvenc_base.cpp` 已實作完整的：
     - `configure_vr_intra_refresh`（`nvenc_base.cpp:679`）與隨選 `forceIntraRefreshWithFrameCnt`（`nvenc_base.cpp:953-967`）；
     - 動態位元率調整 `reconfigure_bitrate`（`nvenc_base.cpp:1216-1260`）；
     - 完整支援雙 GPU：左眼編碼執行緒綁定 `CUdevice 0` 的 `CUcontext`，右眼編碼執行緒綁定 `CUdevice 1` 的 `CUcontext`。
  3. **每卡就地零拷貝匯入與色彩轉換**：在 `viplestream-server` 的各卡 `CUcontext` 中：
     - 透過 `cuImportExternalMemory` 將遊戲傳來的 Vulkan FD 匯入為 `CUmipmappedArray` → `CUarray`，並綁定 `CUtexObject`；透過 `cuImportExternalSemaphore` 匯入同步號誌。
     - 每幀由 `cuWaitExternalSemaphoresAsync` 在該卡 `CUstream` 上無 CPU 阻塞等待 Vulkan 繪製完成，接著直接呼叫 `Sunshine/src/platform/linux/cuda.cu:158-206` 既有的 `RGBA_to_NV12` CUDA kernel（`cuda::sws_t::convert`），將 RGBA `CUtexObject` 轉寫入預先向 `nvEncRegisterResource` 註冊好的 NV12 `CUdeviceptr`（`NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR`），隨即在同卡呼叫 `nvEncEncodePicture`，完成後 `cuSignalExternalSemaphoresAsync` 釋放影像。**未壓縮像素 100% 留在各自 GPU VRAM 內，PCIe 匯流排只傳壓縮後的幾 KB bitstream**。

#### 5. T-2（【致命阻斷】）：不隱藏 `external_memory` 與 `external_semaphore` 的雙卡鏡像做法
- **處置**：**採納**。
- **程式碼查證結果**：查證 `temp/vrdual_src/layer/vrdual_layer.cpp:858-888`（`ext_hidden`）與 `ARCHITECTURE.md:253-259`，`vrdual` 為了防止應用程式透過外部記憶體繞過 `userfaultfd` 髒頁追蹤，在 `vrd_EnumerateDeviceExtensionProperties`（`vrdual_layer.cpp:950-966`）中把 `VK_EXT_external_memory_dma_buf`、`VK_EXT_external_memory_host`、`VK_EXT_image_drm_format_modifier`、`VK_EXT_queue_family_foreign` 全部過濾隱藏。這在 Proton VR 下會同時導致：(a) `wineopenxr` / DXVK 因找不到 `VK_KHR_external_memory_fd` / `VK_EXT_external_memory_dma_buf` 而無法建立 OpenXR swapchain；(b) T-1 的跨行程匯出 FD 被自己擋死。
- **具體可行替代設計（寫入 §1.2、§1.4）**：
  1. **精準分類外部擴充，絕不一刀切隱藏**：
     - **保持暴露且在雙卡強制啟用（Force-Enable on Device A & B）**：`VK_KHR_external_memory_capabilities`、`VK_KHR_external_memory`、`VK_KHR_external_memory_fd`、`VK_EXT_external_memory_dma_buf`、`VK_EXT_image_drm_format_modifier`、`VK_KHR_external_semaphore_capabilities`、`VK_KHR_external_semaphore`、`VK_KHR_external_semaphore_fd`、`VK_KHR_external_fence`、`VK_KHR_external_fence_fd`、`VK_EXT_queue_family_foreign`。
     - **僅對上層應用程式隱藏**：`VK_EXT_external_memory_host`（因為匯入既有 host pointer `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT` 會繞過 Layer 對 `vkMapMemory` 配置區段的 `userfaultfd` / `PAGEMAP_SCAN` 追蹤）。
  2. **攔截並雙卡獨立實體化 External Memory 配置**：
     - 當 `wineopenxr` 或 DXVK 在 `vkCreateImage` / `vkAllocateMemory` 的 `pNext` 鏈帶入 `VkExternalMemoryImageCreateInfo` 或 `VkExportMemoryAllocateInfo` 時，Layer 在 Primary（GPU 0）與 Secondary（GPU 1）**各自獨立配置具有相同 `VkExportMemoryAllocateInfo` 的 Device-Local 記憶體**，並在 `vrdual_mirror` 的 handle 表建立 `Primary VkDeviceMemory ↔ Secondary VkDeviceMemory` 映射。
     - 當 `wineopenxr` 內部進行 D3D11/Vulkan 轉接而呼叫 `vkGetMemoryFdKHR(deviceA, memA, &fdA)` 再 `vkImportMemoryFdInfoKHR` 時，Layer 的 `fd_to_primary_mem` 追蹤表會辨識出 `fdA` 來自本行程的 `memA`，於是在 Secondary `VkDevice` B 上自動改用對應的 `memB`（或 `memB` 匯出的 `fdB`）完成匯入，**不會誤把 GPU 0 的 DMA-BUF FD 跨卡匯入到 GPU 1**（避免觸發跨卡 PCIe peer mapping 或 `VK_ERROR_INVALID_EXTERNAL_HANDLE`）。

#### 6. T-3（【致命阻斷】）：廢除 `present_index % 2` 奇偶猜測，改用 OpenXR Swapchain 決定性標記
- **處置**：**採納**。
- **程式碼查證結果**：查證 UEVR 上游原始碼：
  - 在 `src/mods/VR.cpp:2070, 2129, 2202, 2220`，UEVR 雖然用 `m_frame_count % 2 == m_left_eye_interval` 計算 `is_left_eye_frame`，但 `m_left_eye_interval` 可被使用者或設定切換為 0 或 1，且遊戲啟動、載入畫面或視窗焦點切換期間會有任意次數的非 VR `vkQueuePresentKHR`。
  - 更關鍵的是，查證 `src/mods/vr/D3D11Component.cpp:531-559, 630-723` 與 `src/mods/vr/runtimes/OpenXR.cpp:1792-1905`：在 `SYNCHRONIZED` AFR 模式下，UEVR 的行為是**決定性的**：
    - **左眼 Present（`D3D11Component.cpp:531-559`）**：將左眼畫面寫入 `SwapchainIndex::AFR_LEFT_EYE`（即 `OpenXR` 的第 0 個 swapchain），此時**不呼叫 `end_frame()`**。
    - **右眼 Present（`D3D11Component.cpp:630-723`）**：將右眼畫面寫入 `SwapchainIndex::AFR_RIGHT_EYE`（即 `OpenXR` 的第 1 個 swapchain），並緊接著在 `:719` 呼叫 `openxr->end_frame()`（內部在 `OpenXR.cpp:1799-1804` 同時提交 `AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE` 兩個 projection views 並於 `:1898` 呼叫 `xrEndFrame`）。
- **具體可行替代設計（寫入 §1.3、§3.2、§3.3）**：
  1. **同行程 OpenXR Runtime 與 Vulkan Layer 共享顯式狀態機**：因為我們的輕量 OpenXR Pose Runtime（`libviple_openxr.so`）與 Vulkan Layer（`libVkLayer_viple_dualgpu.so`）都在同一個遊戲行程內，兩者透過行程內共享狀態結構 `VipleInProcState` 直接通訊。
  2. **以 OpenXR Swapchain 雙生影像作為擷取與眼別唯一權威來源**：
     - Pose Runtime 在 `xrCreateSwapchain` 時，將建立出來的 `VkImage` 向 Layer 註冊其明確角色：第 0 個 stereo swapchain 標記為 `EYE_LEFT (0)`（對應 UEVR `AFR_LEFT_EYE`），第 1 個標記為 `EYE_RIGHT (1)`（對應 `AFR_RIGHT_EYE`）。
     - 每當 `xrBeginFrame` 執行時，遞增決定性的 `world_tick = F`（64-bit 單調遞增整數）。
     - 當 UEVR 對 `AFR_LEFT_EYE` 呼叫 `xrAcquireSwapchainImage` → 渲染/拷貝 → `xrReleaseSwapchainImage` 時，Layer 直接從 **GPU 0 (Primary) 上的 `AFR_LEFT_EYE` `VkImage`** 擷取左眼，並打上決定性標籤 `(world_tick = F, eye = 0)`，同時通知 Layer 將下一次繪圖路由切換至 GPU 1。
     - 當 UEVR 對 `AFR_RIGHT_EYE` 呼叫 `xrAcquireSwapchainImage` → 渲染/拷貝 → `xrReleaseSwapchainImage` 並隨即呼叫 `xrEndFrame` 時，Layer 直接從 **GPU 1 (Secondary) 上的 `AFR_RIGHT_EYE` twin `VkImage`** 擷取右眼，並打上決定性標籤 `(world_tick = F, eye = 1)`，同時將兩眼配對描述符 `(world_tick = F, left_slot, right_slot, echoSampleId, renderPose)` 送交 `viplestream-server`。
  3. **徹底消除錯配**：無論載入畫面有多少次 `vkQueuePresentKHR`、無論 `m_left_eye_interval` 為何、甚至某幀是否未進入 VR 提交，只有真正完成 `AFR_LEFT_EYE` + `AFR_RIGHT_EYE` + `xrEndFrame` 的同一個 `world_tick F` 才會被配對送出，從根本上消滅左右眼顛倒與 `(R_{N-1}, L_N)` 跨 tick 錯配。

#### 7. T-4（高）：UEVR 3-Slot 管線化下的 `displayTime → (sampleId, renderPose)` 歷史配對環
- **處置**：**採納**。
- **程式碼查證結果**：查證 `UEVR/src/mods/vr/runtimes/OpenXR.cpp:115-290`：
  - UEVR 定義 `OpenXR::QUEUE_SIZE = 3`，在 Game Thread 的 `on_pre_render_game_thread`（`:115`）呼叫 `synchronize_frame`（`:141` `xrWaitFrame`）與 `update_poses`（`:268, 280` `xrLocateViews`），將預測的 `displayTime` 與查詢到的 view poses 存入 `m_frame_states[frame_count % 3]`。
  - 當 Render Thread 稍後執行 `end_frame`（`:1792-1905`）呼叫 `xrEndFrame(frameEndInfo.displayTime, ...)` 時，共享記憶體中的最新 `0x5506` 追蹤樣本早已推進了 1～2 幀。
- **具體可行替代設計（寫入 §3.2、§3.4）**：
  - 在 In-Process Pose Runtime 內建立 64-slot 的 `DisplayTimePoseRing`（並直接移植或連結 `Sunshine/src/platform/windows/steamvr_driver/pose_history.h:1-108` 與 `pose_history.cpp:105-187` 的四元數內積匹配邏輯作為雙重驗證）：
    1. `xrWaitFrame` 為每個新幀分配單調遞增的 `predictedDisplayTime`（精確對應當下從共享記憶體鎖定的 `0x5506` 樣本 `(sampleId, clientQuat, clientPos)`），寫入 `DisplayTimePoseRing[slot]`。
    2. `xrLocateViews(locateInfo->displayTime)` 以 `locateInfo->displayTime` 向 `DisplayTimePoseRing` 查詢鎖定的那筆姿態回傳給 UEVR（若為尚未見過的 timestamp 則鎖定當下最新樣本並寫入環中）。
    3. `xrEndFrame(frameEndInfo->displayTime, layers)` 以 `frameEndInfo->displayTime` 從 `DisplayTimePoseRing` 取出**當時給 Game Thread 渲染這一幀的那筆 `(sampleId, clientQuat, clientPos)`**（並用 `pose_history_t::match()` 比對 `XrCompositionLayerProjectionView::pose` 計算遊戲世界空間與頭盔 STAGE 空間的 yaw/offset delta），連同 `(world_tick F, eye 0/1)` 一起交給 `viplestream-server` 填入 `0x81` header 的 `echoSampleId` 與 `renderPose`。

#### 8. T-5（高）：6 GB VRAM 預算防護與 PCIe 3.0 x4 髒頁上傳頻寬控制
- **處置**：**採納**。
- **程式碼查證結果**：
  - 查證 `UEVR/src/mods/VR.cpp:2252-2266`（`get_hmd_width`）與 `src/mods/vr/D3D11Component.cpp:544-548`：若未啟用 `extreme_compatibility_mode` 或未覆寫渲染目標寬度，UEVR 為了相容 Native Stereo 預設可能回傳雙倍寬度；而在 AFR 模式下，每幀實際只渲染單眼視錐。
  - 查證 `temp/vrdual_src/ARCHITECTURE.md:178-228`：`vrdual` 鏡像模式透過 `PAGEMAP_SCAN`（Linux 6.7+）或 `userfaultfd` 在每次 `vkQueueSubmit` 前掃描 `HOST_VISIBLE` 記憶體的 4 KB dirty pages 並透過 `vkCmdCopyBuffer` 上傳至 Secondary GPU（PCIe 3.0 x4 實測頻寬約 3.2～3.5 GB/s，若每幀 dirty 超過 10 MB 就會耗時 > 3 ms）。
- **具體可行替代設計（寫入 §1.3、§1.4、§1.8）**：
  1. **擷取目標與解析度預算**：Layer 直接擷取單眼尺寸 `W×H` 的 OpenXR swapchain 影像（`AFR_LEFT_EYE` / `AFR_RIGHT_EYE`），不要求雙倍寬 swapchain；且在 GTX 1660 Ti (6 GB VRAM) 上將預設每眼傳輸解析度定為 **`1920×1920` @ 90 Hz**（可由 `sunshine.conf` 調至 `2160×2160` 或 `1728×1728`），並在 UEVR Palworld Profile 中啟用單眼尺寸渲染目標（避免 UE5 配置 `2W×H` G-Buffer）。
  2. **PCIe 3.0 x4 髒頁同步硬上限與遙測**：在 Layer 內建每幀 `dirty_pages_count`、`dirty_bytes`、`sync_duration_us` 計數器寫入共享記憶體。設定硬預算為 **每幀 dirty upload < 4 MB（耗時 < 1.2 ms）**；並在 P4 針對閒置卡跳過 draw 時一併跳過僅供當幀 Per-Frame Constant / Dynamic Ring Buffer 的跨卡 `vkCmdCopyBuffer`（僅同步跨幀持久資源如靜態頂點/貼圖 staging）。

#### 9. W-1（【致命阻斷】）：`moonlight-common-c` 去除視訊路徑 `static` 全域單例之具體重構設計
- **處置**：**採納**。
- **程式碼查證結果**：查證 `moonlight-qt/moonlight-common-c/moonlight-common-c/src/`：
  - `VideoStream.c`：包含 `rtpQueue`（`:16`）、`waitingForAllSlices`..`repairRrTimer`（`:19-60`）、`rtpSocket`、`firstFrameSocket`、`decryptionCtx`、`udpPingThread`（`:212-217`）、`quicVideoRing`..`quicLossReportLastMs`（`:226-231`）、`receiveThread`、`decoderThread`、`receivedDataFromPeer`、`firstDataTimeMs`、`receivedFullFrame`（`:299-304`）。
  - `VideoDepacketizer.c`：包含 `nalChainHead`..`consecutiveFrameDrops`（`:11-50`）、`vrLastHeader`、`vrLastHeaderValid`、`vrLastHeaderGaze`、`vrLastHeaderGazeValid`、`vrPoseGateDiscardCount`（`:61-65`）、`decodeUnitQueue`（`:228`）。
  - `RtpVideoQueue.c`：雖然大部分狀態已在 `RTP_VIDEO_QUEUE` 結構內，但 `:31-33` 仍有 `static PLT_MUTEX fbLock;`、`static bool fbLockReady = false;`、`static RTP_FRAME_BOUNDS_QUEUE fbQueue;` 三個全域單例（且 `VideoStream.c:936` 的 watchdog reset 會 `memset(&rtpQueue, 0, sizeof(rtpQueue))`）。
- **具體可行替代設計（寫入 §2.4、§2.7）**：
  1. **封裝為雙實例陣列 `g_VideoStreams[2]` 與 `g_VideoDepack[2]`（最小侵入、100% 向後相容）**：
     - 在 `VideoStream.c` 定義 `typedef struct _VIDEO_STREAM_CTX { int streamId; RTP_VIDEO_QUEUE rtpQueue; SOCKET rtpSocket; PPLT_CRYPTO_CONTEXT decryptionCtx; PLT_THREAD udpPingThread, receiveThread, decoderThread; ... } VIDEO_STREAM_CTX;`，宣告 `static VIDEO_STREAM_CTX g_VideoStreams[2];`（`0` = 左眼／主視訊、`1` = 右眼副視訊）。
     - 在 `VideoDepacketizer.c` 定義 `typedef struct _VIDEO_DEPACKETIZER_CTX { int streamId; LBQ decodeUnitQueue; PBUFFER_DESC nalChainHead; VipleVrFrameHeader vrLastHeader; ... } VIDEO_DEPACKETIZER_CTX;`，宣告 `static VIDEO_DEPACKETIZER_CTX g_VideoDepack[2];`。
     - 在 `RtpVideoQueue.c` 將 `fbQueue` 移入 `RTP_VIDEO_QUEUE` 結構內，而 `fbLock` 改為 `static PLT_MUTEX fbLocks[2];`（避免 watchdog reset `memset(&ctx->rtpQueue, 0, ...)` 時把已初始化的 `PLT_MUTEX` 清零毀損！）。
  2. **保留既有 API 作為 `streamId = 0` 的包裝函式，新增 `Ex` 多流 API**：
     - 既有 `LiWaitForNextVideoFrame`、`LiPollNextVideoFrame`、`LiCompleteVideoFrame` 內部直接轉呼 `LiWaitForNextVideoFrameEx(0, ...)` 等，確保桌面版、Android 版與 Windows `layout=sbs` 零修改相容。
     - 新增 `LiWaitForNextVideoFrameEx(int streamId, PDECODE_UNIT* du)`、`LiCompleteVideoFrameEx(int streamId, int drStatus)`，並在 `DECODE_UNIT`（`Limelight.h`）末尾新增 `uint8_t streamId;`（`0` = 左眼、`1` = 右眼），供 pull 模式與 push 模式（`drSubmitDecodeUnit`）辨識眼別。

#### 10. W-2（【致命阻斷】）：`eyePort` 的 UDP Ping 位址學習與 AES-GCM IV / Cipher 執行緒安全隔離
- **處置**：**採納**。
- **程式碼查證結果**：查證 `Sunshine/src/stream.cpp`：
  - **UDP 位址學習機制**：`recvThread`（`stream.cpp:5650-5743`）目前只將 `video_sock` 與 `audio_sock` 加入 `pollfd`（`:5734-5738`），收到客戶端 UDP Ping（4 bytes `"PING"` `:5714-5720` 或 16 bytes `SS_PING` payload `:5721-5730`）後呼叫 `recv_ping(session, ref, socket_e::video, ...)`（`:7339-7456`）填入 `session->video.peer` 並觸發 `session->ping_events->raise(true)`。若 `eyePort` 沒有綁定對應 socket 並走完 `recv_ping`，伺服器根本不知道客戶端右眼 socket 經過 NAT / 防火牆映射後的來源埠號！
  - **AES-GCM Nonce 重用與執行緒競爭**：在 `videoBroadcastThread`（`stream.cpp:6739-6757`），當 `config.encryptionFlagsEnabled & SS_ENC_VIDEO` 啟用時，每個 RTP 封包會將 `session->video.gcm_iv_counter++` 寫入 12-byte IV 的前 8 bytes，並將 `iv[11] = 'V'`（`:6748-6750`），接著呼叫 `session->video.cipher.encrypt(...)`。若左眼與右眼：
    - 各自從 `0` 遞增 `gcm_iv_counter` 且都用同一個 `rikey` 與 `iv[11] = 'V'` → **兩條流產生完全相同的 `(Key, IV)` 對不同明文做 AES-GCM 加密，直接違反 NIST SP 800-38D，攻擊者可透過密文 XOR 還原明文並偽造認證標籤（GHASH key 洩露）**；
    - 共用同一個 `session->video.cipher`（內部持有 OpenSSL `EVP_CIPHER_CTX*`）跨執行緒呼叫 `encrypt()` → **產生 C++ 未定義行為與記憶體損毀**。
- **具體可行替代設計（寫入 §2.2、§2.5）**：
  1. **獨立 `socket_e::eye_video` 與 UDP Ping 握手**：
     - 在 `Sunshine/src/stream.cpp` 的 `enum class socket_e : int` 新增 `eye_video`。
     - 當会话協商為 `layout=dual` 時，`start_broadcast`（`stream.cpp:7197`）額外綁定 `ctx.eye_video_sock` 於 `eyePort`（預設 `net::map_port(VIDEO_STREAM_PORT + 2)`，即 `47998` 起算 +2 = `48000`，避開 `CONTROL_PORT` `47999` 與 `AUDIO_STREAM_PORT` `48000` 的位移：見 §2.2 精確埠號分配表），並將 `eye_video_sock` 加入 `recvThread` 的 `pollfd` 陣列。
     - 客戶端 `g_VideoStreams[1]` 的 `udpPingThread` 向 `eyePort` 發送帶有同一組 `pingPayload`（`SS_PING.payload`）與遞增 `sequenceNumber` 的 UDP Ping；伺服器 `recv_ping` 比對 `ping.payload` 命中後，將來源位址寫入 `session->eye_video.peer` 並發出 `eye_video_ping_event`。
  2. **AES-GCM 密碼學域分離（Domain Separation）與獨立 Cipher 實例**：
     - `session->video`（左眼）與 `session->eye_video`（右眼）各自擁有獨立的 `crypto::cipher::gcm_t cipher` 實例（各自持有獨立的 `EVP_CIPHER_CTX`）與各自從 1 起算的 `uint64_t gcm_iv_counter`。
     - **Domain Separation 固定欄位**：左眼維持 `iv[11] = 'V'` (`0x56`)；右眼固定設定 **`iv[11] = 'R'` (`0x52`, Right-eye Video)**（客戶端 `VideoStream.c` 解密時，`streamId == 0` 填 `iv[11] = 'V'`，`streamId == 1` 填 `iv[11] = 'R'`）。
     - 根據 NIST SP 800-38D §8.2.1，在相同 `rikey` 下，只要 96-bit IV 的固定欄位（byte 11：`'V'` vs `'R'`）不同，加各自的 64-bit 單調計數器（bytes 0..7），兩條流在數學上保證 **零 Nonce 碰撞**，且完全無跨執行緒鎖爭用。

#### 11. W-3（【致命阻斷】）：完整保留並擴充 `SS/video.cpp` 與 `SS/stream.cpp` 的五大視訊控制迴路
- **處置**：**採納**。
- **程式碼查證結果**：查證 `Sunshine/src/video.cpp:2523-3253`（`encode_run`）與 `Sunshine/src/stream.cpp:2390-2961`：
  - `encode_run` 負責處理：
    1. `mail::idr` 與 VR 專用 300 ms IDR 冷卻（`kVrIdrCooldownMs = 300ms`，`video.cpp:2643, 2879-2940`）；
    2. `mail::vr_refresh` 隨選 Intra-Refresh Wave 與完成後標記 `VIPLE_VR_FF_REFRESH_DONE`（`video.cpp:2586-2598, 2957-3002, 3177-3200, 3231-3236`）；
    3. `mail::bitrate_change` 動態 ABR（呼叫 `session->reconfigure_bitrate`，`video.cpp:2734-2809, 3007-3017`）；
    4. `§VR-RATE-AUTO` 整數除頻（`vr::session().rate_div()`，`video.cpp:2718, 2811-2819, 3083-3118`）；
    5. 100 ms `max_frametime` 逾時重送上一幀並標記 `VIPLE_VR_FF_REPEATED`（`video.cpp:2567-2569, 3075-3133, 3166-3168`）。
- **具體可行替代設計（寫入 §1.5、§2.6）**：
  - 因為 T-1 已將 NVENC 移回 `viplestream-server`（透過繼承 `nvenc::nvenc_base` 的 `nvenc_cuda`），我們**完全不繞過 `SS/video.cpp`**！
  - 在 `layout=dual` 下，`viplestream-server` 啟動兩個 `encode_run` 執行緒實例（`eye_index = 0` 綁定 `CUdevice 0` 的 `nvenc_cuda`，`eye_index = 1` 綁定 `CUdevice 1` 的 `nvenc_cuda`），各自擁有獨立的信箱通道 `idr_events[eye]`、`vr_refresh_events[eye]`、`bitrate_change_events[eye]` 與獨立的輸出佇列 `packets[eye]`：
    - **單眼獨立丟包復原（LOSS → Intra-Refresh Wave）**：客戶端 `g_VideoStreams[eye]` 偵測到缺包或解碼掉幀時，送出 `0x5507/11 VIPLE_VR_C2S_LOSS_EYE`（帶 `eye = 0` 或 `1`）；`vr_handle_loss`（`stream.cpp:2390`）僅向對應的 `vr_refresh_events[eye]` 發出 `mail::vr_refresh`，只讓受損的那一眼跑 Intra-Refresh Wave 並在該眼的 `0x81` header 標記 `VIPLE_VR_FF_REFRESH_DONE`，另一眼完全不受干擾。
    - **同步 ABR、除頻與重送幀**：見 §2.6 完整規格。

#### 12. W-4（高）：實作 `SL/vr_linux.cpp` 接通 `0x5507/08 LATCH` 與 `virtual_vsync` 相位微調迴路
- **處置**：**採納**。
- **程式碼查證結果**：查證 `Sunshine/src/platform/linux/vr_stub.cpp:216-228`，目前 Linux 的 `vr::bridge::set_pacing_ppm(int32_t)`、`set_pacing_step(uint32_t)`、`set_pacing(int64_t, uint64_t)` 皆為空函式；而 `Sunshine/src/stream.cpp:2637-2699`（`vr_apply_latch`）在收到客戶端 `0x5507/08 LATCH` 後正是呼叫這三個函式來推動 `Sunshine/src/platform/windows/steamvr_driver/virtual_vsync.h:1-67` 的虛擬 VSync 相位鎖定。
- **具體可行替代設計（寫入 §3.5）**：
  - 新增 `Sunshine/src/platform/linux/vr_linux.cpp`（當 `config::video.vr_linux_backend` 啟用時取代 `vr_stub.cpp` 的 no-op 實作；macOS 或未啟用時保留 stub 行為）。
  - `vr::bridge::set_pacing_ppm`、`set_pacing_step`、`set_pacing` 直接以原子操作寫入與 In-Process Pose Runtime 共用的 `/dev/shm/viple_vr_linux_v2` 共享記憶體中的 `vripc_pacing_t` 區塊（直接複用 `Sunshine/src/vr/vr_ipc_abi.h:221-244` 的結構定義：`rate_adjust_ppm`、`step_frames`、`step_seq`、`period_ns`、`phase_offset_ns`）。
  - In-Process Pose Runtime 的 `xrWaitFrame` 直接引入 `Sunshine/src/platform/windows/steamvr_driver/virtual_vsync.h` 的 `step()` 演算法（該標頭檔為純 C++ `<cstdint>` / `<algorithm>`，100% 跨平台），搭配 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` 執行 `±200 ppm` 頻率微調與 `step_frames` 追趕脈衝，將遊戲 `xrWaitFrame` 鎖定在頭盔 `xrWaitFrame` 前固定提前量。

#### 13. W-5（中）：遵守 `check_commonc_sync.ps1` 三樹同步鐵律
- **處置**：**採納**。
- **程式碼查證結果**：查證 `moonlight-qt/moonlight-common-c/check_commonc_sync.ps1:1-200`：
  - 規則 1（`:54-97`）：`Q/`（`moonlight-qt/moonlight-common-c/moonlight-common-c/`）與 `A/`（`moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/`）的 `src/` 與 `enet/` 除白名單 `src/PlatformNetIf.h` 外必須 **byte-identical**。
  - 規則 2（`:99-145`）：`S3/`（`Sunshine/third-party/moonlight-common-c/`）與 `Q/` 針對 `$QsRequired` 清單（含 `src/VipleVr.h`、`src/VrRepairRtt.h`、`src/Video.h`、`enet/*` 等）必須 **byte-identical**。
- **規格修正**：已在 §2.7 的「修改／新增檔案清單」中將 `A/` 與 `S3/` 的同步路徑完整列出，並將 `pwsh moonlight-qt/moonlight-common-c/check_commonc_sync.ps1` 列為每一期的必要自動化檢查閘門（§4.3）。

#### 14. W-6（中）：統一 `layout=dual` 的位元率語意與修正客戶端預設位元率計算
- **處置**：**採納**。
- **程式碼查證結果**：查證 `moonlight-qt/app/streaming/session.cpp:2196-2204`：
  ```cpp
  if (m_IsVrMode) {
      double pixRate = double(m_StreamConfig.width) * m_StreamConfig.height * m_StreamConfig.fps;
      double ref = 3456.0 * 1728.0 * 90.0;
      int kbps = int(150000.0 * (pixRate / ref));
      m_StreamConfig.bitrate = std::clamp(kbps, 50000, 200000);
  }
  ```
  當 `layout=dual` 將 `m_StreamConfig.width` 設為單眼寬度（如 `1728` 或 `1920`）時，`pixRate` 只有單眼像素率，算出的預設位元率會從雙眼總計 150 Mbps 被誤砍成 75 Mbps（若是伺服器再除以 2 就會變成每眼 37.5 Mbps）。
- **規格修正（寫入 §2.1、§2.6）**：
  - **統一語意**：`m_StreamConfig.bitrate`、RTSP `x-nv-video[0].initialBitrateKbps` 以及 `0x5508/03 BITRATE` 回報的位元率，在 `layout=dual` 下**一律定義為「雙眼合計總位元率（Total Stereo Bitrate）」**。
  - **客戶端修正**：在 `MQ/streaming/session.cpp:2198`，當 `m_VrLaunchParams.layout == "dual"` 時，`double stereoFactor = 2.0; double pixRate = stereoFactor * double(m_StreamConfig.width) * m_StreamConfig.height * m_StreamConfig.fps;`，使 `1728×1728@90` 雙眼總位元率維持 150 Mbps（`1920×1920@90` 為 `std::clamp(185185, 50000, 200000) = 185 Mbps`）。
  - **伺服器端分配**：`viplestream-server` 的每個單眼 `encode_run`（左眼與右眼）各自拿 `total_bitrate_kbps / 2`（例如總計 150 Mbps 時，左右眼各配置 75 Mbps），ABR 升降速時亦將計算出的 `new_total_kbps / 2` 同步下發給左右兩眼的 `bitrate_change_events[0/1]`。

#### 15. V-1（高）：新增單眼獨立丟包復原（Per-Eye Loss & Intra-Refresh）自動化驗證
- **處置**：**採納**。
- **規格修正（寫入 §4.2、§4.3）**：擴充客戶端既有的 `--vr-inject-loss <N>` 與 `--vr-inject-drop <N>`（見 `docs/vr_architecture.md:803`），新增 `--vr-inject-eye <0|1|both>` 參數。在無人自動化測試中分別對 `eye=0` 與 `eye=1` 注入丟包，由日誌自動斷言：僅受損眼觸發 `0x5507/11 (eye=E) → REFRESH_START (eye=E) → REFRESH_DONE (eye=E)`，另一眼 `refreshCount == 0`，且雙眼全程 `idrCount == 1`。

#### 16. V-2（高）：新增 Pose-to-Pixel 與左右眼配對的自動化 Bit-Pattern 探針（`vr_probe`）
- **處置**：**採納**。
- **規格修正（寫入 §4.2、§4.3）**：在合成測試程式 `vr_linux_probe`（以及 Layer 的 `--vr-stamp-probe` 偵錯模式）中，於每眼影像左上角 `64×8` 像素區塊寫入決定性的 64-bit 二進位黑白方塊條碼 `(magic=0x5650, world_tick_u32, eye_u8, echoSampleId_u32)`。客戶端 `vrprobedecode.{h,cpp}` 在解碼出左右眼 `AVFrame` 後自動讀取該像素條碼，斷言：
  1. `left.eye == 0 && right.eye == 1`（**0 次左右眼顛倒**）；
  2. `left.world_tick == right.world_tick`（**0 次跨 tick 錯配 `(R_{N-1}, L_N)`**）；
  3. `pixel.echoSampleId == frameMeta.echoSampleId`（**0 次 Pose-to-Pixel 錯位**，證明 T-4 的 `DisplayTimePoseRing` 有效）。

#### 17. V-3（中）：強化 P0 探針涵蓋「雙 Vulkan FD → 雙 CUDA Context → 雙 NVENC 零拷貝」與 6 GB VRAM / PCIe x4 壓力
- **處置**：**採納**。
- **規格修正（寫入 §4.1、§4.3）**：將 P0 的 `vk_dualgpu_probe` 升級為完整測試「GPU 0/1 雙 `VkDevice` 匯出 DMA-BUF/opaque FD + Semaphore FD → `SCM_RIGHTS` → 雙 `CUcontext` (`cuImportExternalMemory` + `RGBA_to_NV12`) → 雙 `nvenc_cuda` (`NV_ENC_DEVICE_TYPE_CUDA`) 連續 60 秒 90 Hz 編碼」，並自動檢查每卡總 VRAM `< 5.0 GB`、PCIe 3.0 x4 髒頁上傳 p95 `< 1.5 ms`。

#### 18. V-4（中）：新增 `0x5507/08 LATCH` 相位鎖定收斂驗證與 `check_commonc_sync.ps1` 閘門
- **處置**：**採納**。
- **規格修正（寫入 §4.2、§4.3）**：在 15 分鐘無人烤機測試中，自動解析伺服器日誌 `[VIPLE-VR-LATCH]`，斷言 30 秒內收斂至 `lockR >= 80%` 且 `step` 不持續發散；並將 `pwsh moonlight-qt/moonlight-common-c/check_commonc_sync.ps1`（exit code 0）列為每期 CI/本地必過條件。

---

## 0. 現況事實與程式碼基準（全部於 repo 工作樹親手查證）

本章所有路徑、函式、結構與行號均已在 repo 工作樹（以及本機參考樹 `temp/vrdual_src`、外部的 UEVR 原始碼樹）實際核對。

### 0.1 路徑縮寫對照表

| 縮寫 | 實際相對路徑（相對於 repo 根目錄） |
|---|---|
| `SS/` | `Sunshine/src/` |
| `SL/` | `Sunshine/src/platform/linux/` |
| `SW/` | `Sunshine/src/platform/windows/` |
| `S3/` | `Sunshine/third-party/moonlight-common-c/src/` |
| `MQ/` | `moonlight-qt/app/` |
| `Q/` | `moonlight-qt/moonlight-common-c/moonlight-common-c/src/` |
| `A/` | `moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/src/` |
| `VRD/` | `temp/vrdual_src/`（位於主工作樹 `temp/`，本 worktree 未複製 gitignored 的 `temp/`） |
| `PW/` | 外部的 Palworld VR 專案樹（本機，不在本 repo） |

### 0.2 四套程式碼的查證基準

1. **VipleStream 主專案（repo 根目錄）**：
   - **Linux VR 現況為 Stub**：
     - `SL/vr_stub.cpp:21-23`：`vr::platform_supported()` 硬回傳 `false`。
     - `SL/vr_stub.cpp:44-49`：`vr::pcvr_available()` 填寫 `reason = "platform unsupported"` 並回傳 `false`。
     - `SL/vr_stub.cpp:216-228`：`vr::bridge::set_pacing_ppm`、`set_pacing_step`、`set_pacing` 皆為空函式（no-op）。
     - `SL/misc.cpp:1172-1177`：`platf::display()` 在 `config.captureSource != 0`（如 `captureSource == 2` VR 虛擬螢幕）時直接印出 `"Linux backend does not support virtual display capture source"` 並回傳 `nullptr`。
   - **Sunshine Linux 視訊與 CUDA/NVENC 現況**：
     - `SS/video.cpp:540-676`：`encoder_t nvenc` 在 `#ifdef _WIN32`（`:540-578`）使用原生 `encoder_platform_formats_nvenc`，但在 Linux `#elif !defined(__APPLE__)`（`:579-676`）使用 `encoder_platform_formats_avcodec`（FFmpeg `h264_nvenc` / `hevc_nvenc` / `av1_nvenc` + `AV_HWDEVICE_TYPE_CUDA`）。
     - `SS/video.cpp:4307-4325`：`last_encoder_probe_supported_vr_intra_refresh` 在非 `_WIN32` 分支硬設為 `false`。
     - `SS/video.cpp:4443-4454`：`cuda_init_avcodec_hardware_input_buffer` 呼叫 `av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1)`，固定只用預設 GPU 0。
     - `SL/cuda.cpp:277-466`：`cuda::gl_cuda_vram_t` 在 `:291-292` 硬寫死 `open_drm_fd_for_cuda_device(0)`（`// TODO: Support more than one CUDA device`）。
     - `SL/cuda.cu:158-206`：已有現成且支援任意色彩矩陣的 `RGBA_to_NV12` CUDA kernel（由 `cuda::sws_t::convert` 於 `SL/cuda.cpp:144-156` 呼叫）。
     - `SS/nvenc/nvenc_base.h:56` 與 `SS/nvenc/nvenc_base.cpp:223-1413`：`nvenc::nvenc_base` 本身完全跨平台（接受 `NV_ENC_DEVICE_TYPE` 建構參數 `:223`），且已實作 VR 隨選 Intra-Refresh（`configure_vr_intra_refresh` `:679`、`forceIntraRefreshWithFrameCnt` `:953-967`）與動態 ABR（`reconfigure_bitrate` `:1216-1260`）。
   - **Sunshine 串流與控制迴路現況**：
     - `SS/video.cpp:2523-3253`：`encode_run` 實作 IDR 300 ms 冷卻（`:2643, 2879-2940`）、隨選 Intra-Refresh Wave（`:2586-2598, 2957-3002, 3177-3200, 3231-3236`）、ABR `mail::bitrate_change`（`:2734-2809, 3007-3017`）、`§VR-RATE-AUTO` 除頻（`:2718, 2811-2819, 3083-3118`）、100 ms `max_frametime` 逾時重送幀 `VIPLE_VR_FF_REPEATED`（`:2567-2569, 3075-3133, 3166-3168`）。
     - `SS/stream.cpp:5650-5743`（`recvThread`）與 `:7339-7456`（`recv_ping`）：監聽 `video_sock` 與 `audio_sock` 的 UDP Ping 學習 `peer` 位址。
     - `SS/stream.cpp:6189-6945`（`videoBroadcastThread`）：`:6687` 為 `frame_fec_latency_logger.first_point_now()`；`:6731-6733` 填寫視訊 RTP 標頭（未設 `ssrc`）；`:6739-6757` 以 `session->video.gcm_iv_counter++` 與 `iv[11] = 'V'` 進行 AES-GCM 加密。音訊在 `:6965` 設 `audio_packet.rtp.ssrc = 0;`。
   - **Windows VR 伺服器端參考基準**：
     - `SW/display_vr.cpp:1048-1088`：CPU-side fence wait（`cs->waiter->wait(...)` 與超時後的 `late_wait` 輪詢），確保 GPU 渲染完成才在 `:1203` 執行 `CopyResource`，杜絕半渲染撕裂幀。
     - `SW/steamvr_driver/pose_history.h:1-108`、`pose_history.cpp:105-187`（`pose_history_t::match()`）與 `virtual_vsync.h:1-67`（`virtual_vsync_t::step()`）：純 C++ 演算法，可直接由 Linux In-Process Pose Runtime 共用。
   - **客戶端（Steam Frame `moonlight-qt` + `moonlight-common-c`）現況**：
     - `Q/VideoStream.c`、`Q/VideoDepacketizer.c`、`Q/RtpVideoQueue.c` 使用檔案作用域 `static` 全域變數管理單一視訊流。
     - `MQ/streaming/vr/vrframemeta.h` 與 `vrframemeta.cpp`：`VrFrameMetaTracker`（`vrframemeta.h:70-135`）為 per-decoder 成員；`VrRenderMetaRing`（`vrframemeta.h:26-46`、`vrframemeta.cpp:194-227`）與 `VrDecodeTiming`（`vrframemeta.h:50-68`、`vrframemeta.cpp:230-260`）為全行程 `static` 單例。
     - `MQ/streaming/xr/xrcontext.cpp`：`frameThreadMain` PCVR 投影與 stale 保護在 `:3064-3100`；`XrContext::buildProjection` 在 `:5983-6046`（目前寫死 SBS 半寬 `imageRect.extent.width = eyeW` 與右眼 `offset.x = int32_t(eyeW)` 於 `:6020-6029`）；`XrContext::buildInsetQuads` 在 `:6090-6132`。
     - `MQ/streaming/session.cpp:2196-2204`：以 `width * height * fps` 對比 `3456 * 1728 * 90` 計算預設位元率（150 Mbps）。
     - `moonlight-qt/moonlight-common-c/check_commonc_sync.ps1:1-200`：強制檢查 `Q/`、`A/`、`S3/` 三處 `moonlight-common-c` 同步。

2. **雙卡 Vulkan Layer 原型（`temp/vrdual_src`）**：
   - `VRD/layer/vrdual_layer.cpp:858-888`（`ext_hidden`）與 `:950-966`（`vrd_EnumerateDeviceExtensionProperties`）：目前將 `VK_EXT_external_memory_dma_buf`、`VK_EXT_external_memory_host`、`VK_EXT_image_drm_format_modifier`、`VK_EXT_queue_family_foreign` 隱藏（本規格 §1.2 修正此問題）。
   - `VRD/layer/vrdual_layer.cpp:644-687`（`lockstep_wait`）與 `:1939-1966`：在 `vkQueuePresentKHR` 做 `vkWaitForFences` + staging readback 進 `/dev/shm/vrdual_eyes`（本規格 §1.1 全面廢除 CPU staging readback，改走 GPU DMA-BUF/opaque FD 直入伺服器 `nvenc_cuda`）。
   - `VRD/layer/vrdual_mirror.hpp:1-130, 2091-2099`：雙 `VkDevice` handle 對照表與 `PAGEMAP_SCAN` / `userfaultfd` 髒頁追蹤。

3. **Palworld + UEVR + OpenXR（外部專案樹 `PW/`）**：
   - `PW/vendor/UEVR-src/src/mods/VR.hpp:28-32`：定義 `NATIVE_STEREO = 0`、`SYNCHRONIZED = 1`、`ALTERNATING = 2`。
   - `PW/vendor/UEVR-src/src/mods/vr/D3D11Component.cpp:531-559, 630-723`（注意實際路徑在 `src/mods/vr/D3D11Component.cpp`）：在 `SYNCHRONIZED` AFR 下，左眼 Present 寫入 `SwapchainIndex::AFR_LEFT_EYE`，右眼 Present 寫入 `SwapchainIndex::AFR_RIGHT_EYE` 並於 `:719` 呼叫 `openxr->end_frame()`。
   - `PW/vendor/UEVR-src/src/mods/vr/runtimes/OpenXR.cpp:115-290, 1792-1905`：`QUEUE_SIZE = 3` 管線化呼叫 `xrWaitFrame`（`:141`）、`xrLocateViews`（`:268, 280`）與 `xrEndFrame`（`:1898`）。

---

## 1. 第一軌：雙卡擷取、External Memory 穿透與伺服器端雙 CUDA NVENC 零拷貝管線（解決 T-1、T-2、T-3、T-5）

### 1.1 整體資料流架構（遊戲行程零 CUDA，伺服器雙卡就地 NVENC）

為同時滿足「**像素不過 PCIe、不落 CPU RAM**」、「**遊戲行程不載入 CUDA（省下每卡 ~250 MB VRAM）**」以及「**100% 保留 `SS/video.cpp` 的 IDR / Intra-Refresh / ABR / 除頻控制迴路**」，第二版採用以下跨行程 GPU 零拷貝架構：

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│ 遊戲行程：Proton + Palworld-Win64-Shipping.exe + UEVR (OpenXR SYNCHRONIZED AFR)   │
│                                                                                   │
│  ┌────────────────────────────────────┐   ┌────────────────────────────────────┐  │
│  │ In-Process Pose Runtime            │   │ Vulkan Layer: viple_dualgpu        │  │
│  │ (libviple_openxr.so)               │◄─►│ (libVkLayer_viple_dualgpu.so)      │  │
│  │ - xrWaitFrame (virtual_vsync.h)    │   │ - Primary VkDevice A (GPU 0, x16)  │  │
│  │ - xrLocateViews (DisplayTimeRing)  │   │ - Twin    VkDevice B (GPU 1, x4)   │  │
│  │ - xrCreateSwapchain:               │   │ - 保留並雙卡啟用 external_memory_fd │  │
│  │   Swapchain[0] = AFR_LEFT_EYE      │   │   & external_semaphore_fd          │  │
│  │   Swapchain[1] = AFR_RIGHT_EYE     │   │ - 左眼擷取: GPU 0 CaptureImage_L   │  │
│  │ - xrReleaseSwapchainImage /        │   │ - 右眼擷取: GPU 1 CaptureImage_R   │  │
│  │   xrEndFrame → 決定性標記 (F, 0/1) │   └─────────┬──────────────────┬───────┘  │
│  └──────────────────┬─────────────────┘             │                  │          │
└─────────────────────┼───────────────────────────────┼──────────────────┼──────────┘
                      │ /dev/shm/viple_vr_linux_v2    │ SCM_RIGHTS (一次性傳 FD)
                      │ (追蹤 0x5506 / LATCH / 幀描述符) │ CaptureFD_L + SemFD_L (GPU 0)
                      ▼                               ▼                  ▼ CaptureFD_R + SemFD_R (GPU 1)
┌───────────────────────────────────────────────────────────────────────────────────┐
│ 伺服器行程：viplestream-server (Sunshine)                                          │
│                                                                                   │
│  ┌────────────────────────────────────┐   ┌────────────────────────────────────┐  │
│  │ 左眼 encode_run (eye = 0)          │   │ 右眼 encode_run (eye = 1)          │  │
│  │ - CUcontext 0 (綁定 GPU 0)         │   │ - CUcontext 1 (綁定 GPU 1)         │  │
│  │ - cuImportExternalMemory(FD_L)     │   │ - cuImportExternalMemory(FD_R)     │  │
│  │ - cuWaitExternalSemaphoresAsync    │   │ - cuWaitExternalSemaphoresAsync    │  │
│  │ - cuda.cu RGBA_to_NV12 (GPU 0)     │   │ - cuda.cu RGBA_to_NV12 (GPU 1)     │  │
│  │ - nvenc_cuda (繼承 nvenc_base)     │   │ - nvenc_cuda (繼承 nvenc_base)     │  │
│  │   NV_ENC_DEVICE_TYPE_CUDA (GPU 0)  │   │   NV_ENC_DEVICE_TYPE_CUDA (GPU 1)  │  │
│  │ - 完整支援 forceIntraRefresh / ABR │   │ - 完整支援 forceIntraRefresh / ABR │  │
│  └──────────────────┬─────────────────┘   └──────────────────┬─────────────────┘  │
│                     ▼                                        ▼                    │
│  ┌────────────────────────────────────┐   ┌────────────────────────────────────┐  │
│  │ videoBroadcastThread (eye = 0)     │   │ eyeVideoBroadcastThread (eye = 1)  │  │
│  │ - UDP videoPort + 0x81 (eye=0)     │   │ - UDP eyePort + 0x81 (eye=1)       │  │
│  │ - AES-GCM cipher[0], iv[11] = 'V'  │   │ - AES-GCM cipher[1], iv[11] = 'R'  │  │
│  └────────────────────────────────────┘   └────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 不隱藏 `external_memory` / `external_semaphore` 的雙卡鏡像規範（解決 T-2）

修改 `VRD/layer/vrdual_layer.cpp:858-888`（`ext_hidden`）與 `VRD/layer/vrdual_mirror.hpp` 的擴充與記憶體攔截策略：

1. **擴充白名單與強制雙卡啟用**：
   - 在 `vrd_EnumerateDeviceExtensionProperties`（`vrdual_layer.cpp:950-966`）中，**移除**對以下擴充的隱藏，並在 `vrd_CreateDevice` 與 `mirror::create_twin` 建立 Primary `VkDevice` A（GPU 0）與 Secondary `VkDevice` B（GPU 1）時**強制同時啟用**：
     - `VK_KHR_external_memory_capabilities`、`VK_KHR_external_memory`、`VK_KHR_external_memory_fd`
     - `VK_EXT_external_memory_dma_buf`、`VK_EXT_image_drm_format_modifier`
     - `VK_KHR_external_semaphore_capabilities`、`VK_KHR_external_semaphore`、`VK_KHR_external_semaphore_fd`
     - `VK_KHR_external_fence_capabilities`、`VK_KHR_external_fence`、`VK_KHR_external_fence_fd`
     - `VK_EXT_queue_family_foreign`
   - **唯一保留隱藏的擴充**：`VK_EXT_external_memory_host`（防止 DXVK 將任意 host `malloc` 指標直接當成 `VkBuffer` 匯入而繞過 Layer 的 `vkMapMemory` 髒頁追蹤）。
2. **`vkAllocateMemory` 的外部記憶體雙卡獨立配置與 FD 映射表**：
   - **Case A（匯出型配置 `VkExportMemoryAllocateInfo`）**：當 `wineopenxr` 或 DXVK 在 `vkAllocateMemory` 的 `pNext` 鏈帶有 `VkExportMemoryAllocateInfo`（如 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` 或 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT`）時，Layer 保留該 `pNext` 節點，在 Device A（GPU 0）與 Device B（GPU 1）**各自獨立配置各卡自己的可匯出 Device-Local `VkDeviceMemory`**（`memA` 與 `memB`）。
   - **Case B（行程內自匯出再匯入 `vkGetMemoryFdKHR` → `VkImportMemoryFdInfoKHR`）**：
     - `wineopenxr` 在將 D3D11 OpenXR swapchain 轉接至 Vulkan 時，會在同一行程內呼叫 `vkGetMemoryFdKHR(deviceA, memA, &fdA)` 並在另一個 `VkDevice`（或同一 `VkDevice`）透過 `VkImportMemoryFdInfoKHR` 匯入 `fdA`。
     - Layer 攔截 `vkGetMemoryFdKHR`：用 `fstat(fdA, &st)` 取得 `(st.st_dev, st.st_ino)` 作為鍵值，在 Layer 全域表 `g_ExportedMemInodes[(st_dev, st_ino)] = { memA, memB }` 登記。
     - 當 Layer 在 `vkAllocateMemory` 看到 `pNext` 含有 `VkImportMemoryFdInfoKHR(fd)` 時，先對 `fd` 執行 `fstat(fd, &st)` 查表：
       - 若命中 `g_ExportedMemInodes`：Primary Device A 照常匯入 `fd`；而在 Secondary Device B 上，Layer 呼叫 `vkGetMemoryFdKHR(deviceB, entry.memB, &fdB)` 並將 Secondary 的 `VkImportMemoryFdInfoKHR::fd` 替換為 `fdB`！如此保證 **GPU 0 永遠只匯入 GPU 0 的 FD，GPU 1 永遠只匯入 GPU 1 的 FD**，徹底避免跨卡匯入失敗或透過 PCIe x4 遠端存取。
   - **Case C（外部號誌 `VkExportSemaphoreCreateInfo` / `VkImportSemaphoreFdInfoKHR`）**：同理，以 `(st_dev, st_ino)` 記錄 `semA ↔ semB`，在 Secondary Device B 上自動替換為 `semB` 匯出的 `fdB`。

### 1.3 以 OpenXR Swapchain 決定性標記取代 `present_index % 2`（解決 T-3、T-5）

1. **廢除 `present_index % 2` 奇偶判定**：
   - `vkQueuePresentKHR` **不再用於猜測左右眼或配對 frame**。遊戲視窗的 `vkQueuePresentKHR` 在載入畫面、選單或 UEVR 伴隨視窗（spectator window）上的呼叫只由 Primary GPU 0 正常處理顯示，不進入 VR 擷取配對器。
2. **擷取來源改為 OpenXR Swapchain 影像（`AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE`）**：
   - 在 `SYNCHRONIZED` AFR 模式下，UEVR（`PW/vendor/UEVR-src/src/mods/vr/D3D11Component.cpp:531-559, 630-723` 與 `OpenXR.cpp:1799-1804`）會透過 `xrCreateSwapchain` 建立兩個單眼尺寸 `W×H` 的 swapchain：
     - `swapchain_handle = 0`（`SwapchainIndex::AFR_LEFT_EYE`）
     - `swapchain_handle = 1`（`SwapchainIndex::AFR_RIGHT_EYE`）
   - 我們的 In-Process Pose Runtime（`libviple_openxr.so`）在實作 `xrCreateSwapchain` 時，直接呼叫底層（經由 `viple_dualgpu` Layer 包裝的）`vkCreateImage` 建立單眼尺寸 `W×H`（預設 `1920×1920`、`VK_FORMAT_R8G8B8A8_UNORM` 或遊戲請求之 sRGB/UNORM 格式）的 Swapchain Image Ring（每眼 3 張 `VkImage`）。
   - Layer 的 `vrdual_mirror` 自動在 GPU 0（Device A）與 GPU 1（Device B）同時建立這組 Swapchain `VkImage`。
3. **決定性眼別路由與 `world_tick` 配對狀態機**：
   - 當 Pose Runtime 的 `xrBeginFrame` 被呼叫時：
     - 指派新的單調遞增 `world_tick = F`（`uint64_t`），並將 `VipleInProcState::active_eye = EYE_LEFT (0)` 通知 Layer：接下來的主渲染指令路由至 **GPU 0（Device A）**。
   - 當 UEVR 完成左眼繪製並對 `AFR_LEFT_EYE` 呼叫 `xrReleaseSwapchainImage` 時：
     - Layer 在 **GPU 0（Device A）** 上對 `AFR_LEFT_EYE` 當下 active image 發出 `vkCmdCopyImage`（或 `vkCmdBlitImage` 若需 sRGB/UNORM 正規化）至 GPU 0 的匯出擷取環 `CaptureImage_L[F % 3]`，並 signal GPU 0 的外部號誌 `CaptureSem_L[F % 3]`。
     - 記錄 `LeftReady = { world_tick = F, slot = F % 3 }`。
     - 同時將 `VipleInProcState::active_eye = EYE_RIGHT (1)` 通知 Layer：接下來的右眼渲染指令路由至 **GPU 1（Device B）**。
   - 當 UEVR 完成右眼繪製、對 `AFR_RIGHT_EYE` 呼叫 `xrReleaseSwapchainImage` 並緊接著呼叫 `xrEndFrame(displayTime, ...)` 時：
     - Layer 在 **GPU 1（Device B）** 上對 `AFR_RIGHT_EYE` 當下 active twin image 發出 `vkCmdCopyImage` 至 GPU 1 的匯出擷取環 `CaptureImage_R[F % 3]`，並 signal GPU 1 的外部號誌 `CaptureSem_R[F % 3]`。
     - 驗證 `LeftReady.world_tick == F`（若因異常跳幀導致 `LeftReady.world_tick != F`，則整組丟棄並記錄 `unpaired_eye_drop_count++`，**絕不將 `R_F` 與 `L_{F-1}` 配對**）。
     - 配對成功後，將完整立體幀描述符 `VipleLinuxFrameDesc { world_tick = F, slot = F % 3, display_time = displayTime, echo_sample_id, render_pose, left_ready = 1, right_ready = 1 }` 寫入 `/dev/shm/viple_vr_linux_v2` 並透過 `eventfd` 喚醒 `viplestream-server`。

### 1.4 匯出擷取環（Exportable Capture Ring）與 `SCM_RIGHTS` 交握

每張 GPU（GPU 0 為左眼 `eye=0`，GPU 1 為右眼 `eye=1`）在 Layer 內維護一個固定 3-slot 的匯出擷取環（`kCaptureRingSize = 3`）：

1. **建立匯出影像（每卡 3 張 `W×H` `VK_FORMAT_R8G8B8A8_UNORM`）**：
   - `VkImageCreateInfo` 掛載 `VkExternalMemoryImageCreateInfo { .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT }`（同時支援 `DMA_BUF_BIT_EXT` 備援），`tiling = VK_IMAGE_TILING_OPTIMAL`，`usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`。
   - `vkAllocateMemory` 掛載 `VkExportMemoryAllocateInfo { .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT }` 與 `VkMemoryDedicatedAllocateInfo`（Dedicated Allocation 可確保 NVIDIA 驅動在 `cuImportExternalMemory` 時零對齊誤差）。
   - 每卡建立兩個外部同步號誌（或 1 個 Timeline Semaphore）：`render_done_sem[slot]`（Vulkan signal → CUDA wait）與 `encode_done_sem[slot]`（CUDA signal → Vulkan wait），掛載 `VkExportSemaphoreCreateInfo { .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT }`。
2. **透過 Unix Domain Socket (`SCM_RIGHTS`) 傳遞 FD 給 `viplestream-server`**：
   - 當擷取環建立或解析度變更（`layout_epoch++`）時，Layer 連線至 `/run/user/$UID/viplestream-vr.sock`，送出一個 `VipleLinuxCaptureInitMsg` 結構（含 `layout_epoch`、`width`、`height`、`format`、`alloc_size`、兩張卡的 `VkPhysicalDeviceIDProperties::deviceUUID[16]`），並在 `msghdr::msg_control` 附帶 `2 eyes × 3 slots × (1 mem_fd + 2 sem_fds) = 18` 個 FD。
   - `viplestream-server` 收到後比對 `deviceUUID` 與 `cuDeviceGetUuid(&cudaUuid, cuDev)`，確保 **GPU 0 的 FD 100% 匯入 `CUcontext 0`、GPU 1 的 FD 100% 匯入 `CUcontext 1`**，匯入完成後立即 `close()` 本地多餘的 FD 複本。

### 1.5 伺服器端原生 Linux CUDA NVENC 編碼器：`SL/nvenc_cuda.{h,cpp}`（解決 T-1、W-3）

在 `viplestream-server` 中新增 `Sunshine/src/platform/linux/nvenc_cuda.h` 與 `nvenc_cuda.cpp`，**直接繼承 `Sunshine/src/nvenc/nvenc_base.h:56` 的 `nvenc::nvenc_base`**：

1. **為什麼繼承 `nvenc::nvenc_base` 而不是走現有 FFmpeg `avcodec` (`h264_nvenc`/`hevc_nvenc`)**：
   - 現有 Linux `SS/video.cpp:579-676` 走 FFmpeg `avcodec` 包裝，不支援 VR 必備的隨選 Intra-Refresh（`forceIntraRefreshWithFrameCnt`，見 `SS/video.cpp:4308-4309` 註解），且寫死單一 GPU 0（`SL/cuda.cpp:291-292`、`SS/video.cpp:4447`）。
   - 直接繼承 `nvenc::nvenc_base(NV_ENC_DEVICE_TYPE_CUDA)`，只需約 220 行程式碼實作 4 個純虛擬函式，就能在 Linux 原生獲得與 Windows D3D11 NVENC 完全對齊的 VR 能力：
     - `init_library()`：`dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_LOCAL)` 並解析 `NvEncodeAPICreateInstance`。
     - `create_and_register_input_buffer()`：在該實例綁定的 `CUcontext`（GPU 0 或 GPU 1）中呼叫 `cuMemAllocPitch(&nv12_devptr, &nv12_pitch, width, height * 3 / 2, 16)` 配置 NV12 緩衝區，並以 `NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR`、`NV_ENC_BUFFER_FORMAT_NV12` 呼叫 `nvenc->nvEncRegisterResource(encoder, &register_params)`（對應 `nvEncodeAPI.h:2826-2869`）。
     - `synchronize_input_buffer()`：在該卡的 `CUstream` 上執行：
       1. `cuWaitExternalSemaphoresAsync(&ext_render_done_sem[slot], &wait_params, 1, cu_stream)`（等待該卡 Vulkan 完成 `CaptureImage` 拷貝）；
       2. 呼叫 `SL/cuda.cu:158-206` 既有的 `RGBA_to_NV12` CUDA kernel（`cuda::sws_t::convert`），將匯入的 `CUtexObject` (`CaptureImage[slot]`) 轉換寫入 `nv12_devptr`；
       3. `cuSignalExternalSemaphoresAsync(&ext_encode_done_sem[slot], &sig_params, 1, cu_stream)`（通知 Vulkan 該 slot 可於下一輪重用）；
       4. `cuStreamSynchronize(cu_stream)` 確保 NVENC 讀取 `nv12_devptr` 前轉換已完成（或交由 NVENC CUDA stream 串接）。
2. **解鎖 Linux 隨選 Intra-Refresh**：
   - 在 `SS/video.cpp:4307-4325` 將 `#ifdef _WIN32` 放寬為 `#if defined(_WIN32) || defined(__linux__)`（當 Linux 選用原生 `nvenc_cuda` 路徑時），使 `last_encoder_probe_supported_vr_intra_refresh = true`。

### 1.6 與 `SW/display_vr.cpp:1048-1088` 等價的防撕裂保證（解決 R-2）

在 Windows `SW/display_vr.cpp:1048-1088` 中，伺服器透過 CPU-side `ID3D11Fence` wait 確保客戶端永遠不會看到半渲染的撕裂畫面。在 Linux v2 架構下：
- Layer 在 GPU 0 / GPU 1 的 Vulkan Queue 上是先完成 `vkCmdCopyImage` 到 `CaptureImage[slot]`，並在同一個 `vkQueueSubmit` 的 `pSignalSemaphores` 發出 `render_done_sem[slot]`。
- 伺服器 `nvenc_cuda` 在 `CUstream` 上先執行 `cuWaitExternalSemaphoresAsync(render_done_sem[slot])` 才啟動 `RGBA_to_NV12` 與 `nvEncEncodePicture`；且在 Vulkan 重寫同一個 `slot` 前必須等待 `encode_done_sem[slot]`。
- 這在 GPU 硬體排程器層級達成了與 `SW/display_vr.cpp:1048-1088` 完全等價（且無需 CPU 介入輪詢）的讀寫互斥，保證 **0 撕裂、0 半渲染幀**。

### 1.7 延遲預算對比（Windows 單卡 SBS vs Linux 雙卡就地編碼）

對照 `docs/vr_architecture.md:770-788` 的基準預算表：

| 階段 | Windows 單卡 SBS (`docs/vr_architecture.md:770-788`) | Linux 雙卡就地編碼（v2 目標） | 差異來源 |
|---|---|---|---|
| 1. 姿態取樣 → 伺服器接收 (`0x5506`) | 2.0 ms | 2.0 ms | 相同（Wi-Fi 6/6E UDP） |
| 2. 遊戲渲染（單眼 vs 雙眼） | 2× 單眼串列渲染 | **每卡只算 1 眼**（AFR 交錯 + P4 跳過閒置眼 draw） | 渲染吞吐翻倍，每卡 GPU 時間減半 |
| 3. 合成與跨行程匯入 | SBS compose + D3D11 IPC: 1.5 ms (`:777`) | `vkCmdCopyImage` + External Sem + `RGBA_to_NV12`: **0.6 ms** | 無雙眼合併、無跨卡像素搬移 |
| 4. NVENC HEVC 編碼 | 單顆 NVENC 編 `3456×1728`: 4.0 ms (`:778`) | **兩顆 NVENC 平行各編單眼 `1920×1920`**: **2.2 ms** | 每顆 NVENC 像素量僅單眼，平行完成 |
| 5. 網路傳輸（RTP + FEC） | 3.0 ms (`:780`) | 3.0 ms（雙 UDP 流平行發送） | 總位元率相同（150 Mbps 拆為 2×75 Mbps） |
| 6. Steam Frame `hevc_v4l2m2m` 解碼 | 單路 `3456×1728`: 4.0 ms (`:783`) | 雙路 `1920×1920` 硬體時分多工: **3.8～4.5 ms** | 見 P0 實機 decode-bench 驗收 |
| 7. ATW 重投影與顯示 | 2.5 ms (`:784-786`) | 2.5 ms | 相同 |

### 1.8 6 GB VRAM 預算與 PCIe 3.0 x4 髒頁上傳預算（解決 T-5）

1. **GTX 1660 Ti (6 GB VRAM) 單卡記憶體分配上限**：
   - Palworld UE5 遊戲本體（High 貼圖降為 Medium/High、單眼 `1920×1920` G-Buffer）：`~3.9 GB`
   - DXVK / Vulkan Layer 雙卡鏡像開銷 + 3-slot 單眼 `1920×1920` RGBA 匯出環（`3 × 14.7 MB ≈ 45 MB`）：`~0.15 GB`
   - `viplestream-server` 單卡 `CUcontext` + `nvenc_cuda` 單眼 `1920×1920` NV12/reconstructed surfaces：`~0.28 GB`（因為只有 1 個 server context，不再在遊戲行程重複建立 CUDA context）
   - **單卡合計**：`~4.33 GB / 6.00 GB`（保留 `> 1.6 GB` 安全餘裕；P0 與 P2 自動監控每卡 VRAM 峰值必須 `< 5.0 GB`）。
2. **PCIe 3.0 x4（GPU 1）每幀頻寬預算**：
   - 壓縮後右眼 HEVC bitstream（75 Mbps @ 90 fps）：每幀僅 **`~104 KB`**（耗時 `< 0.03 ms`）。
   - `vrdual` 鏡像模式的 `HOST_VISIBLE` dirty-page 同步（`PAGEMAP_SCAN` / `userfaultfd`）：限制每幀平均 `< 4 MB`（在 PCIe 3.0 x4 3.3 GB/s 下耗時 **`< 1.2 ms`**）。Layer 在 `/dev/shm/viple_vr_linux_v2` 暴露 `pcie_dirty_bytes_per_frame` 與 `pcie_sync_us_per_frame` 供自動化測試斷言 `p95 < 1.5 ms`。

---

## 2. 第二軌：雙流協定、加密隔離、控制迴路與客戶端雙實例架構（解決 W-1～W-6、R-1、R-3）

### 2.1 能力位元、會話協商與位元率語意（解決 W-6）

1. **`/serverinfo` 能力位元（`S3/VipleVr.h`、`Q/VipleVr.h`、`A/VipleVr.h`）**：
   - 依 `docs/vr_protocol.md:34-37`，`<VipleStreamVR>` 目前已用到 b11 (`0x800`)，下一個可用位元為 **b12 (`0x1000`)**：
     ```c
     #define VIPLE_VR_SERVER_CAP_DUAL_STREAM  0x00001000u  /* b12: 支援 layout=dual 雙路單眼獨立視訊流 */
     #define VIPLE_VR_CLIENT_CAP_DUAL_STREAM  0x00001000u  /* b12: 客戶端支援雙 VideoStream + 雙解碼器 */
     ```
   - 維持不變量 8（`docs/vr_architecture.md:237`）：Linux 後端就緒前，`vr::pcvr_available()` 回傳 `false`，`/serverinfo` 不宣告 `b0 (0x01)` 與 `b12 (0x1000)`。
2. **`/vr/negotiate` 與 `/launch` XML 回應（`SS/nvhttp.cpp:1691-1782`、`SS/vr/vr_session.cpp:427-455`）**：
   - 當伺服器與客戶端同時具備 `DUAL_STREAM (0x1000)` 且伺服器設定 `vr_linux_dual_gpu = true` 時，`/vr/negotiate` 與 `/launch` 的 `<vrSession>` 回傳：
     ```xml
     <vrSession token="..." width="1920" height="1920" fps="90"
                controlPort="47999" videoPort="47998" eyePort="48002"
                layout="dual" packed="0" />
     ```
   - **向下相容**：若任一方無 `0x1000` 位元或處於 P1 單卡模式，回傳既有的 `layout="sbs"`、`width="3840"`（或 `"3456"`）、無 `eyePort`，走原單路 SBS 路徑。
3. **位元率語意統一（解決 W-6）**：
   - `m_StreamConfig.bitrate` 在 `layout="dual"` 下明確定義為**雙眼合計總位元率（Total Stereo Bitrate，單位 kbps）**。
   - 在 `MQ/streaming/session.cpp:2196-2204` 修正預設位元率計算：
     ```cpp
     if (m_IsVrMode) {
         double stereoScale = (m_VrLaunchParams.layout == "dual") ? 2.0 : 1.0;
         double pixRate = stereoScale * double(m_StreamConfig.width) * m_StreamConfig.height * m_StreamConfig.fps;
         double ref = 3456.0 * 1728.0 * 90.0;
         int kbps = int(150000.0 * (pixRate / ref));
         m_StreamConfig.bitrate = std::clamp(kbps, 50000, 200000);
     }
     ```
   - 伺服器端收到 RTSP `x-nv-video[0].initialBitrateKbps`（總位元率 $B_{\text{total}}$）後，左眼編碼器與右眼編碼器各自分配 $B_{\text{eye}} = B_{\text{total}} / 2$。

### 2.2 雙 UDP 埠位分配、UDP Ping 位址學習與 AES-GCM 密碼學隔離（解決 R-1、W-2）

1. **維持不變量 10（`docs/vr_architecture.md:239`、`docs/vr_protocol.md:26`）**：
   - **完全不擴充 RTSP / SDP**。`eyePort` 由 `/vr/negotiate` 與 `/launch` XML 的 `eyePort` 屬性直接告知客戶端。
   - **埠號計算**：查證 `Sunshine/src/network.h` 與 `rtsp.cpp`，Sunshine 預設埠位位移為 `VIDEO_STREAM_PORT = 47998`（offset -1）、`CONTROL_PORT = 47999`（offset 0）、`AUDIO_STREAM_PORT = 48000`（offset +1）、`MICROPHONE_STREAM_PORT = 48001`（offset +2）。因此新增 **`EYE_VIDEO_STREAM_PORT = 48002`（offset +3）**，由 `net::map_port(EYE_VIDEO_STREAM_PORT)` 映射，避免與音訊或控制埠衝突。
2. **`eyePort` 的 UDP Ping 位址學習（修正 W-2 前半）**：
   - 在 `Sunshine/src/stream.cpp` 的 `enum class socket_e : int` 新增 `eye_video`。
   - `start_broadcast`（`SS/stream.cpp:7197`）在 `layout == "dual"` 時開啟 `ctx.eye_video_sock` 綁定 `eyePort`，並註冊到 `populate_peer_to_session`。
   - `recvThread`（`SS/stream.cpp:5650-5743`）將 `ctx.eye_video_sock` 加入 `pollfd` 監聽陣列。
   - 客戶端 `g_VideoStreams[1]`（右眼流）啟動自己的 `udpPingThread`，向伺服器 `eyePort` 發送攜帶相同 16-byte `X-SS-Ping-Payload`（RTSP `SETUP streamid=video` 回傳的 `pingPayload`）的 `SS_PING` 封包（或 4-byte `"PING"` fallback）。
   - 伺服器 `recvThread` 在 `eye_video_sock` 收到 Ping 後呼叫 `recv_ping(session, ref, socket_e::eye_video, ...)`（`SS/stream.cpp:7339`），將經過 NAT 映射後的客戶端右眼 socket 位址寫入 `session->eye_video.peer` 並觸發 `session->eye_video.ping_events->raise(true)`。`eyeVideoBroadcastThread` 等待該事件觸發後才開始發送右眼 RTP/FEC 封包。
3. **AES-GCM 獨立 Cipher 與 Nonce / IV 域分離（修正 W-2 後半）**：
   - 在 `SS/stream.cpp` 的 `session_t` 中，`session->video`（左眼）與 `session->eye_video`（右眼）各自持有：
     - 獨立的 `crypto::cipher::gcm_t cipher` 實例（各自初始化同一個 `config.monitor.rikey`，但擁有完全獨立的 OpenSSL `EVP_CIPHER_CTX`，徹底消除多執行緒競爭）；
     - 獨立的 64-bit 計數器 `uint64_t gcm_iv_counter = 0`。
   - **12-byte AES-GCM IV 結構（對照 `SS/stream.cpp:6744-6750` 與 `Q/VideoStream.c:262-268`）**：
     - Bytes `0..7`：該眼各自的 `gcm_iv_counter++`（Little-Endian 64-bit）；
     - Bytes `8..10`：`0x00, 0x00, 0x00`；
     - Byte `11`（Stream Domain Tag）：
       - **左眼（`streamId = 0`）**：`iv[11] = 'V'` (`0x56`)（與既有協定 100% 相容）；
       - **右眼（`streamId = 1`）**：`iv[11] = 'R'` (`0x52`)。
   - 此設計嚴格符合 NIST SP 800-38D §8.2.1 規範：即使兩條流使用同一個会话金鑰 `rikey` 且各自的 `gcm_iv_counter` 同時從 1 開始遞增，由於第 11 byte 恆不相同（`'V' != 'R'`），兩條流在整個会话生命週期內的 96-bit IV 集合交集為空（$\text{IV}_L \cap \text{IV}_R = \emptyset$），保證密碼學安全。

### 2.3 `0x81` / `0x82` Frame Header 與 `0x5507` / `0x5508` 控制通道擴充

1. **`0x81` (`VipleVrFrameHeader`, 24 B) 標頭欄位（`S3/VipleVr.h`、`Q/VipleVr.h`、`A/VipleVr.h`）**：
   - 依 `docs/vr_protocol.md:115-131`，`0x81` 標頭大小固定為 24 bytes 不變：
     - Byte 5 (`streamFlags`)：目前 `b0` 為 `VIPLE_VR_SF_FOVEA_SCALED (0x01)`，新增 **`b1`: `VIPLE_VR_SF_DUAL_EYE (0x02)`**（代表此封包屬於 `layout=dual` 單眼獨立流）與 **`b2`: `VIPLE_VR_SF_RIGHT_EYE (0x04)`**（`0` = 左眼、`1` = 右眼）。
     - Bytes `2..3` (`frameNum`, u16 LE)：在 `layout=dual` 下填入由 `world_tick F` 映射的**雙眼共用同步幀序號**（即同一 `world_tick F` 的左眼與右眼擁有完全相同的 `frameNum`、`echoSampleId` 與 `renderPose[7]`），供客戶端雙解碼器做精確同幀配對！
2. **`0x5507` (C→S) 單眼丟包回報 TLV 子型別（`docs/vr_protocol.md:148-181`）**：
   - 目前 C→S 子型別已用到 `0x10` (`GAMMA`)，下一個可用子型別為 **`0x11`**：
     ```c
     #define VIPLE_VR_C2S_LOSS_EYE  0x11  /* 9B value: firstLost(u32), count(u16), flags(u8), reason(u8), eye(u8: 0=L, 1=R, 2=Both) */
     ```
   - 舊有 `0x09 LOSS`（8B）保留相容（伺服器收到舊 `0x09` 時視為 `eye = 2 (Both)`，同時觸發雙眼 Intra-Refresh Wave）。
3. **`0x5508` (S→C) 單眼 Refresh 通知**：
   - 既有 `0x06 REFRESH_START`（4B value: `waveFrames(u16), reason(u8), pad(u8)`）：將原本恆為 0 的第 4 byte `pad(u8)` 定義為 `eye(u8: 0=Left, 1=Right, 2=Both)`，不改變 TLV 長度（維持 4B）。

### 2.4 客戶端 `moonlight-common-c` 雙實例化與 `moonlight-qt` 雙解碼器同步（解決 W-1、R-3）

1. **`moonlight-common-c` 雙實例重構（`Q/`、`A/`、`S3/` 同步）**：
   - **`VideoStream.c`**：將所有檔案作用域 `static` 變數封裝入 `typedef struct _VIDEO_STREAM_CTX { ... } VIDEO_STREAM_CTX;`，宣告 `static VIDEO_STREAM_CTX g_VideoStreams[2];`（索引 `0` 與 `1`）。所有內部執行緒（`VideoReceiveThreadProc`、`VideoDecoderThreadProc`、`UdpPingThreadProc`）透過 `void* context` 傳入 `&g_VideoStreams[streamId]`。
   - **`VideoDepacketizer.c`**：將所有檔案作用域 `static` 變數封裝入 `typedef struct _VIDEO_DEPACKETIZER_CTX { ... } VIDEO_DEPACKETIZER_CTX;`，宣告 `static VIDEO_DEPACKETIZER_CTX g_VideoDepack[2];`。所有組包函式接收 `int streamId`（或 `PVIDEO_DEPACKETIZER_CTX ctx`）。
   - **`RtpVideoQueue.c`**：將 `fbQueue` 移入 `RTP_VIDEO_QUEUE` 結構作為成員 `RTP_FRAME_BOUNDS_QUEUE fbQueue;`，並將 `fbLock` 改為 `static PLT_MUTEX fbLocks[2];`（以 `queue->streamId` 索引，且在 `RtpvResetQueue` / watchdog reset 時**絕不 `memset` 互斥鎖**，只重設資料欄位並走完整 linked-list free，遵循 `docs/vr_protocol.md:306-310` 規範）。
   - **新增公開 API（`Limelight.h`）**：
     ```c
     int LiStartVideoStreamEx(int streamId, int port, void* rendererContext, PDECODER_RENDERER_CALLBACKS drCallbacks);
     void LiStopVideoStreamEx(int streamId);
     int LiWaitForNextVideoFrameEx(int streamId, PDECODE_UNIT* decodeUnit);
     bool LiPollNextVideoFrameEx(int streamId, PDECODE_UNIT* decodeUnit);
     void LiCompleteVideoFrameEx(int streamId, int drStatus);
     ```
2. **`VrRenderMetaRing` 與 `VrDecodeTiming` 擴充 `eyeIndex` 維度（解決 R-3）**：
   - 在 `MQ/streaming/vr/vrframemeta.h:26-68` 與 `vrframemeta.cpp:194-260`：
     - `VrRenderMetaRing::publish(int eyeIndex, int64_t pts, const VrFrameMeta& meta)` 與 `lookup(int eyeIndex, int64_t pts, VrFrameMeta& out)`：內部靜態陣列改為 `static Entry s_Entries[2][kSize];`（`eyeIndex` 取 `0` 或 `1`；`layout=sbs` 時固定傳 `0`）。
     - `VrDecodeTiming::recordSubmit(int eyeIndex, int64_t pts, uint64_t submitUs)` 與 `takeDecodeUs(int eyeIndex, int64_t pts)`：內部靜態陣列改為 `static Slot s_Slots[2][kSize];`。
   - `VrFrameMetaTracker`（`vrframemeta.h:70-135`）維持為每個 `FFmpegVideoDecoder` 實例各自的成員，新增 `m_EyeIndex`（`0` 或 `1`），在 `onFrameDecoded` 時呼叫 `VrRenderMetaRing::publish(m_EyeIndex, pts, meta)`。
3. **`XrVideo` 雙眼 Mailbox 與 `XrContext::buildProjection` 同幀屏障（Stereo Pair Barrier）**：
   - 在 `MQ/streaming/xr/xrvideo.cpp:245-360`：當 `layout == "dual"` 時，建立兩組單眼尺寸 `W×H` 的 OpenXR swapchain（`m_EyeSwapchains[0]` 左眼、`m_EyeSwapchains[1]` 右眼），各自接收 `FFmpegVideoDecoder[0]` 與 `FFmpegVideoDecoder[1]` 寫入的貼圖與 `(frameNum, pts, VrFrameMeta)`。
   - **同幀屏障（防單眼舊姿態閃爍）**：
     - 當左眼與右眼最新解碼完成的 `frameNum` 相等（`latestLeft.frameNum == latestRight.frameNum`）時，原子更新 `m_CommittedPair = { leftTex, rightTex, meta }`。
     - 若某一眼因網路抖動晚到超過 **2.5 ms**（例如左眼已到 `F`，右眼仍在 `F-1`），在未超過 **18 ms（約 1.5 幀）** 前，`XrContext::buildProjection` **維持上一組已成對的 `(L_{F-1}, R_{F-1}, pose_{F-1})` 進行 ATW 重投影**，絕不把「新左眼 `L_F` + 舊右眼 `R_{F-1}`」套用同一個 `pose_F`（避免產生雙眼視差撕裂與暈眩）；若單眼停滯超過 18 ms，才允許未受損眼先行推進並對停滯眼套用獨立 `renderPose` 的 ATW 重投影。
   - 在 `MQ/streaming/xr/xrcontext.cpp:6020-6029`（`XrContext::buildProjection`）：
     ```cpp
     if (m_IsDualStreamLayout) {
         views[e].subImage.swapchain = m_Video->eyeSwapchain(e);
         views[e].subImage.imageRect.offset = {0, 0};
         views[e].subImage.imageRect.extent = {int32_t(eyeW), int32_t(eyeH)};
     } else {
         views[e].subImage.swapchain = swapchain;
         views[e].subImage.imageRect.offset = {e == 0 ? 0 : int32_t(eyeW), 0};
         views[e].subImage.imageRect.extent = {int32_t(eyeW), int32_t(eyeH)};
     }
     ```

### 2.5 伺服器雙廣播執行緒與封包發送

在 `SS/stream.cpp` 中，當 `layout == "dual"` 時：
- `videoBroadcastThread`（左眼，`eye = 0`）從 `mail::man->queue<video::packet_t>(mail::video_packets)` 取左眼 bitstream，呼叫 `fill_vr_frame_header`（設定 `VIPLE_VR_SF_DUAL_EYE`、`VIPLE_VR_SF_RIGHT_EYE = 0`），以 `session->video.cipher`（`iv[11] = 'V'`）加密並由 `ctx.video_sock` 發往 `session->video.peer`。
- `eyeVideoBroadcastThread`（右眼，`eye = 1`）從 `mail::man->queue<video::packet_t>(mail::eye_video_packets)` 取右眼 bitstream，呼叫 `fill_vr_frame_header`（設定 `VIPLE_VR_SF_DUAL_EYE | VIPLE_VR_SF_RIGHT_EYE`），以 `session->eye_video.cipher`（`iv[11] = 'R'`）加密並由 `ctx.eye_video_sock` 發往 `session->eye_video.peer`。

### 2.6 五大視訊控制迴路在雙眼雙流下的完整運作規範（解決 W-3）

因為雙眼皆走 `SS/video.cpp` 的 `encode_run`（搭配 `SL/nvenc_cuda.cpp`），既有五大控制迴路完整保留並定義雙眼協調規則如下：

| 控制迴路 (`SS/video.cpp` / `SS/stream.cpp`) | 程式碼查證位置 | `layout=dual` 雙眼運作行為 |
|---|---|---|
| **1. 丟包復原 → 隨選 Intra-Refresh Wave** | `SS/stream.cpp:2390-2445` (`vr_handle_loss`)、`SS/video.cpp:2586-2598, 2957-3002, 3177-3200` | **單眼獨立觸發**：收到 `0x5507/11 (eye=E)` 時，只向 `vr_refresh_events[E]` 發信號，僅第 `E` 眼的 `nvenc_cuda` 執行 `request_intra_refresh(wave_frames)` 並發出 `0x5508/06 (eye=E)` 與 `0x81` `VIPLE_VR_FF_REFRESH_DONE`。另一眼維持正常 P-frame 編碼。 |
| **2. IDR 請求與 300 ms VR 冷卻** | `SS/video.cpp:2643, 2879-2940` (`kVrIdrCooldownMs = 300ms`) | **單眼獨立冷卻 + 初次連線雙眼同步 IDR**：每眼各自套用 300 ms `kVrIdrCooldownMs`，正常丟包由 Intra-Refresh 吸收，全程 `idrCount == 1`（僅連線首幀）。 |
| **3. 動態位元率控制（ABR / `vrAbrFast`）** | `SS/stream.cpp:2811-2961`、`SS/video.cpp:2734-2809, 3007-3017` | **雙眼對稱分配**：ABR 控制器彙整左右兩眼的 NACK/LOSS/Queue 指標計算總位元率 $B_{\text{total}}$，並同時向 `bitrate_change_events[0]` 與 `[1]` 下發 $B_{\text{total}} / 2$，呼叫兩張卡的 `nvenc_base::reconfigure_bitrate(B_total / 2, fps, false)`。 |
| **4. `§VR-RATE-AUTO` 整數除頻 (`÷1 / ÷2 / ÷3`)** | `SS/video.cpp:2718, 2811-2819, 3083-3118` | **依 `world_tick F` 雙眼同步除頻**：由擷取分派器在送入兩眼 `encode_run` 前統一判斷 `if (F % rate_div != 0) skip_both_eyes;`，保證左右眼永遠保留或跳過同一個 `world_tick F`。 |
| **5. 100 ms `max_frametime` 逾時重送幀** | `SS/video.cpp:2567-2569, 3075-3133, 3166-3168` (`VIPLE_VR_FF_REPEATED`) | **雙眼同步重送**：若遊戲卡頓超過 100 ms 未產出新立體幀，左右兩眼 `encode_run` 同時重編各自上一張 `CaptureImage` 並打上 `VIPLE_VR_FF_REPEATED` 與相同的 `frameNum`，維持客戶端 `XrContext` 不觸發 500 ms stale 淡出。 |

### 2.7 修改／新增檔案清單（含 `Q/`、`A/`、`S3/` 三樹同步，解決 W-5、R-3）

| 階層 | 檔案路徑（相對於 repo 根目錄） | 變更性質與重點 |
|---|---|---|
| **共用協定 (`Q/` + `A/` + `S3/` 必須 byte-identical)** | `moonlight-qt/moonlight-common-c/moonlight-common-c/src/VipleVr.h`<br>`moonlight-android/app/src/main/jni/moonlight-core/moonlight-common-c/src/VipleVr.h`<br>`Sunshine/third-party/moonlight-common-c/src/VipleVr.h` | 新增 `VIPLE_VR_SERVER_CAP_DUAL_STREAM (0x1000)`、`VIPLE_VR_CLIENT_CAP_DUAL_STREAM (0x1000)`、`VIPLE_VR_SF_DUAL_EYE (0x02)`、`VIPLE_VR_SF_RIGHT_EYE (0x04)`、`VIPLE_VR_C2S_LOSS_EYE (0x11)`。 |
| **客戶端核心 (`Q/` + `A/` 必須 byte-identical；若在 `$QsRequired` 則 `S3/` 亦同步)** | `.../src/Limelight.h`<br>`.../src/Limelight-internal.h`<br>`.../src/Video.h`（含 `S3/Video.h`）<br>`.../src/VideoStream.c`<br>`.../src/VideoDepacketizer.c`<br>`.../src/RtpVideoQueue.c`<br>`.../src/ RtpVideoQueue.h`<br>`.../src/ControlStream.c`<br>`.../src/Connection.c` | 將 `VideoStream.c`、`VideoDepacketizer.c`、`RtpVideoQueue.c` 狀態封裝為 `g_VideoStreams[2]`、`g_VideoDepack[2]` 與 `fbLocks[2]`；右眼解密設定 `iv[11] = 'R'`；新增 `Li*VideoStreamEx(int streamId, ...)` 與 `LiSendVrLossReportEye(...)`。 |
| **客戶端 Qt / XR (`MQ/`)** | `moonlight-qt/app/streaming/vr/vrlaunchparams.h` & `.cpp`<br>`moonlight-qt/app/streaming/vr/vrframemeta.h` & `.cpp`<br>`moonlight-qt/app/streaming/session.h` & `.cpp`<br>`moonlight-qt/app/streaming/video/ffmpeg.h` & `.cpp`<br>`moonlight-qt/app/streaming/xr/xrvideo.h` & `.cpp`<br>`moonlight-qt/app/streaming/xr/xrcontext.h` & `.cpp`<br>`moonlight-qt/app/streaming/vr/vrprobedecode.h` & `.cpp` | 解析 `eyePort` 與 `layout="dual"`；修正 `session.cpp:2198` `layout=="dual"` 預設位元率 `×2.0`；`VrRenderMetaRing` 與 `VrDecodeTiming` 擴充 `eyeIndex` 維度；雙 `FFmpegVideoDecoder` 實例；`XrVideo` 雙眼 swapchain 與同幀屏障；`XrContext::buildProjection` (`:6020-6029`) 單眼全幅 `imageRect`。 |
| **伺服器端 (`SS/`、`SL/`)** | `Sunshine/src/platform/linux/nvenc_cuda.h` & `.cpp`（**新增**）<br>`Sunshine/src/platform/linux/vr_linux.h` & `.cpp`（**新增**）<br>`Sunshine/src/platform/linux/vr_stub.cpp`<br>`Sunshine/src/platform/linux/misc.cpp`<br>`Sunshine/src/video.h` & `.cpp`<br>`Sunshine/src/stream.h` & `.cpp`<br>`Sunshine/src/nvhttp.cpp`<br>`Sunshine/src/vr/vr_session.h` & `.cpp` | 實作繼承 `nvenc::nvenc_base` 的 Linux `nvenc_cuda`（支援雙 `CUcontext`、DMA-BUF/opaque FD 零拷貝匯入與 `forceIntraRefreshWithFrameCnt`）；實作 `vr_linux.cpp` 接通 `/dev/shm/viple_vr_linux_v2` 與 LATCH pacing；支援雙 `encode_run` 與 `eye_video_sock`（UDP Ping 位址學習 + AES-GCM `iv[11] = 'R'`）。 |
| **Linux 遊戲端元件 (`Sunshine/tools/linux_vr/`)** | `Sunshine/tools/linux_vr/openxr_pose_runtime/`（**新增**）<br>`Sunshine/tools/linux_vr/vk_layer_dualgpu/`（**新增**，基於 `vrdual` 修正 T-1/T-2/T-3/T-5）<br>`Sunshine/tools/linux_vr/probes/vk_dualgpu_probe.cpp`（**新增**）<br>`Sunshine/tools/linux_vr/probes/vr_linux_probe.cpp`（**新增**） | 輕量 In-Process OpenXR Pose Runtime (`libviple_openxr.so`)、雙卡 Vulkan Layer (`libVkLayer_viple_dualgpu.so`)、P0 硬體零拷貝探針與 P1 無人端到端合成測試程式。 |

---

## 3. 第三軌：In-Process OpenXR Pose Runtime、決定性配對、Pose History 與 LATCH 相位微調（解決 T-3、T-4、W-4）

### 3.1 為什麼採用 In-Process OpenXR Pose Runtime（不依賴桌面 SteamVR / Monado 合成器）

在 CachyOS + Proton 環境下，UEVR 透過 `wineopenxr.dll` 轉呼叫 Linux 主機上的原生 OpenXR Loader (`libopenxr_loader.so`)。我們提供一個僅約 900 行 C++ 的極簡 OpenXR Runtime `libviple_openxr.so`（由環境變數 `XR_RUNTIME_JSON=/etc/viplestream/viple_openxr_runtime.json` 指定）：
- **零額外合成器開銷**：不做 barrel distortion、不做桌面鏡像合成，建立的 OpenXR swapchain 直接就是 `libVkLayer_viple_dualgpu.so` 在兩張卡上的 `VkImage`。
- **同行程零延遲協同**：與 `libVkLayer_viple_dualgpu.so` 位於同一個 Linux 行程位址空間，直接讀寫 `VipleInProcState` 全域結構，徹底解決 T-3（左右眼與 `world_tick` 決定性標記）與 T-4（UEVR 3-slot 管線化 `displayTime` 對齊）。

### 3.2 UEVR 3-Slot 管線化下的 `DisplayTimePoseRing` 與 `pose_history_t` 對齊（解決 T-4）

查證 `PW/vendor/UEVR-src/src/mods/vr/runtimes/OpenXR.cpp:115-290, 1792-1905`，UEVR 的 `OpenXR` 執行時期有 `QUEUE_SIZE = 3` 個 `FrameState` 槽位，因此 `xrWaitFrame` / `xrLocateViews`（Game Thread）與 `xrEndFrame`（Render Thread）之間相差 1～2 幀。Pose Runtime 內部實作如下：

```cpp
struct DisplayTimePoseEntry {
    XrTime      display_time;       // 單調遞增奈秒時戳
    uint64_t    world_tick;         // 對應的立體幀序號 F
    uint32_t    sample_id;          // 鎖定自 0x5506 的 sampleId
    float       client_pose[7];     // [qx, qy, qz, qw, px, py, pz] (STAGE space)
    bool        views_located;
};
```

1. **`xrWaitFrame(session, waitInfo, frameState)`**：
   - 執行 §3.5 的 `virtual_vsync_t::step()` 計算下一個虛擬 VSync 時刻 `target_ns` 並 `clock_nanosleep` 等待。
   - 醒來時從 `/dev/shm/viple_vr_linux_v2` 讀取當下最新的一筆 `0x5506` 追蹤樣本 `S`（若距離上次收到 `0x5506` 已超過 50 ms，則依 `docs/steamvr_driver.md:93-102` 凍結最後有效四元數並遞增 `stale_pose_count`）。
   - 將 `(display_time = target_ns + predict_ns, sample_id = S.sampleId, client_pose = S.hmdPose)` 寫入 `DisplayTimePoseRing[slot]`，並回傳 `frameState->predictedDisplayTime = display_time`。
2. **`xrLocateViews(session, locateInfo, viewState, viewCapacityInput, viewCountOutput, views)`**：
   - 以 `locateInfo->displayTime` 在 `DisplayTimePoseRing`（64 slots）中精確查找對應的 `entry`（若 UEVR 傳入自訂預測時間則匹配最接近的 `entry` 並將該 `locateInfo->displayTime` 綁定至同一 `sample_id`）。
   - 依 `entry.client_pose` 與雙眼 IPD（由 `/vr/negotiate` 傳入的 `ipdMm`，預設 63.0 mm）計算左右眼 `XrView::pose` 與对称/實機 `fov` 回傳給 UEVR。
3. **`xrEndFrame(session, frameEndInfo)`**：
   - 以 `frameEndInfo->displayTime` 從 `DisplayTimePoseRing` 查出**當初在 `xrLocateViews` 交給 Game Thread 渲染這一幀的 `entry`**。
   - 同時呼叫自 `SW/steamvr_driver/pose_history.cpp:105-187` 共用的 `pose_history_t::match()`，比對 `XrCompositionLayerProjectionView::pose` 與歷史環中的 `client_pose`，自動解算遊戲若自帶 yaw offset 的空間差（Space-Delta Synthesis，`docs/steamvr_driver.md:62-70`）。
   - 將 **`entry.sample_id`（作為 `0x81.echoSampleId`）與對齊後的 `entry.client_pose`（作為 `0x81.renderPose`）** 連同 `world_tick = F` 寫入幀描述符交給 `viplestream-server`。
   - **效果**：即使 UEVR 有 3-slot 管線延遲，`0x81` 標頭帶回頭盔的永遠是**實際用來渲染該張像素的那一筆 `sampleId` 與 `renderPose`**，頭盔端 ATW 重投影 `q_now * inverse(q_render)` 精確補償完整管線延遲。

### 3.3 `xrCreateSwapchain` / `xrReleaseSwapchainImage` 與雙卡路由的決定性握手（解決 T-3）

1. **`xrCreateSwapchain`**：
   - UEVR 在 `SYNCHRONIZED` AFR 模式下依序建立兩個顏色 swapchain（`AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE`，見 `PW/vendor/UEVR-src/src/mods/vr/runtimes/OpenXR.cpp:1799-1804`）。
   - Pose Runtime 將第 0 個顏色 swapchain 標記為 `eye_role = EYE_LEFT (0)`，第 1 個顏色 swapchain 標記為 `eye_role = EYE_RIGHT (1)`（若遊戲建立 depth swapchain，則標記為 `EYE_DEPTH_IGNORE`）。
2. **`xrAcquireSwapchainImage` / `xrReleaseSwapchainImage`**：
   - 當 UEVR 開始新的一幀（`xrBeginFrame`）時，`VipleInProcState` 將當前渲染目標卡設為 **GPU 0（左眼）**。
   - 當 `xrReleaseSwapchainImage` 在 `eye_role == EYE_LEFT (0)` 上被呼叫時：
     - 觸發 Layer 在 **GPU 0** 上將該 swapchain image 拷貝至 `CaptureImage_L[F % 3]` 並 signal `render_done_sem_L[F % 3]`；
     - 立即將 `VipleInProcState` 的當前渲染目標卡切換為 **GPU 1（右眼）**，使接下來 UEVR 第二次 `Present` 之前的右眼整個渲染 pass 在 GPU 1 上執行（P4 啟用時，GPU 0 在此階段可跳過非持久繪圖；P2 鏡像模式下兩卡同步執行但僅從 GPU 1 擷取右眼）。
   - 當 `xrReleaseSwapchainImage` 在 `eye_role == EYE_RIGHT (1)` 上被呼叫，且隨後的 `xrEndFrame` 確認提交了 `AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE` 時：
     - 觸發 Layer 在 **GPU 1** 上將該 swapchain twin image 拷貝至 `CaptureImage_R[F % 3]` 並 signal `render_done_sem_R[F % 3]`；
     - 送出成對的 `(world_tick = F, left_slot = F % 3, right_slot = F % 3, echoSampleId, renderPose)` 給 `viplestream-server`。

### 3.4 控制器與輸入轉發（6DoF 雙手控制器 + 按鍵軸向）

- 伺服器 `vr_publish_tracking`（`SS/stream.cpp:2586-2628`）將收到的 `0x5506`（232 bytes，含 HMD + 左右控制器 6DoF 姿態、按鈕位元遮罩、板機與搖桿軸向，見 `docs/vr_protocol.md:90-113`）透過 `vr::bridge::publish_tracking`（在 `SL/vr_linux.cpp` 實作）寫入 `/dev/shm/viple_vr_linux_v2`。
- Pose Runtime 實作 OpenXR `/interaction_profiles/oculus/touch_controller` 與 `/interaction_profiles/KHR/simple_controller`，在 `xrSyncActions`、`xrGetActionStateBoolean`、`xrGetActionStateFloat`、`xrGetActionStateVector2f`、`xrLocateSpace` 中直接從共享記憶體回傳左右手控制器狀態，使 UEVR 的 6DoF 瞄準與帕魯球投擲開箱即用。

### 3.5 接通 `0x5507/08 LATCH` 與 `virtual_vsync.h` 虛擬 VSync 相位鎖定（解決 W-4）

1. **`SL/vr_linux.cpp` 實作 `vr::bridge::set_pacing_*`**：
   - 在 `Sunshine/src/platform/linux/vr_linux.cpp` 中，實作原本在 `SL/vr_stub.cpp:216-228` 為空函式的三個介面：
     - `vr::bridge::set_pacing_ppm(int32_t rate_adjust_ppm)`：寫入 `shm->pacing.rate_adjust_ppm`（夾限 `[-200, +200]` ppm）。
     - `vr::bridge::set_pacing_step(uint32_t step_frames)`：將 `shm->pacing.step_frames = step_frames` 並原子遞增 `shm->pacing.step_seq`。
     - `vr::bridge::set_pacing(int64_t period_ns, uint64_t phase_offset_ns)`：寫入 `shm->pacing.period_ns` 與 `phase_offset_ns`。
2. **Pose Runtime `xrWaitFrame` 直接共用 `SW/steamvr_driver/virtual_vsync.h`**：
   - `Sunshine/src/platform/windows/steamvr_driver/virtual_vsync.h:1-67` 僅依賴 `<algorithm>` 與 `<cstdint>`，無任何 Windows 專屬標頭。
   - Pose Runtime 的 `xrWaitFrame` 每幀讀取 `shm->pacing`，呼叫 `virtual_vsync_t::step(now_ns, base_period_ns, rate_adjust_ppm, step_frames, step_seq)` 算出精確的 `next_vsync_ns`，再呼叫 `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)`。
   - 如此一來，`SS/stream.cpp:2637-2699`（`vr_apply_latch`）計算出的 `0x5507/08 LATCH` 相位誤差能直接微調 Linux 遊戲 `xrWaitFrame` 的喚醒相位，將幀排隊延遲穩定鎖死在最小值（消除 0～11.1 ms 的隨機相位漂移）。

---

## 4. 第四軌：重新編排之「無人優先（Unattended-First）」分期計畫與通過標準

依據任務要求，重新編排分期：**前兩期（P0 與 P1）及 P2 的核心驗收皆為 100% 可在無人配戴頭盔情況下透過 CLI／自動化腳本驗證的最小可行切片**，每期皆定義明確的量化通過標準（Pass Criteria）。

### 4.1 P0：無人硬體與跨行程零拷貝探針（Unattended Hardware & Zero-Copy Probe — 0.5 週）

在寫入大型系統整合前，先以兩支獨立無頭（Headless）CLI 探針驗證所有硬體與驅動假設：

1. **主機端 `vk_dualgpu_probe`（於 CachyOS 2× GTX 1660 Ti 無頭執行）**：
   - 驗證項目：
     1. 兩張 GTX 1660 Ti 皆支援 `bufferDeviceAddressCaptureReplay == VK_TRUE`、`VK_KHR_external_memory_fd`、`VK_KHR_external_semaphore_fd`。
     2. 建立雙行程（或 Child Process 模擬遊戲 + Parent Process 模擬伺服器）：Child 在 GPU 0 與 GPU 1 各建 3-slot `1920×1920` RGBA `VkImage` 並以計算著色器每幀寫入運動圖案與 `(tick, eye)` 條碼，透過 `SCM_RIGHTS` 傳 FD 給 Parent；Parent 在 `CUcontext 0` 與 `CUcontext 1` 以 `cuImportExternalMemory` + `cuImportExternalSemaphore` 匯入，跑 `RGBA_to_NV12` + 雙 `nvenc_cuda` (`NV_ENC_DEVICE_TYPE_CUDA`) 連續編碼 **60 秒（5,400 幀/眼 @ 90 Hz）**，並在途中第 10 秒、第 30 秒對單眼觸發 `forceIntraRefreshWithFrameCnt`。
     3. 同時模擬每幀 2 MB 的 `HOST_VISIBLE` dirty page 經 `PAGEMAP_SCAN` / `userfaultfd` 同步至 GPU 1（PCIe 3.0 x4）。
   - **P0 主機端無人通過標準**：
     - 連續 5,400 幀雙眼零掉幀，平均每眼處理時間（Vulkan Copy + Sem Wait + `RGBA_to_NV12` + `nvEncEncodePicture`）**`< 4.0 ms`**，p99 **`< 6.5 ms`**；
     - 每張 GPU 總 VRAM 佔用 **`< 1.0 GB`**（探針本身），且 PCIe 3.0 x4 2 MB dirty-page 同步耗時 **p95 `< 1.2 ms`**；
     - 輸出的兩條 HEVC 檔案經 `ffprobe` 驗證皆為 `1920×1920`、只含第 0 幀 1 個 IDR，第 10 秒與第 30 秒的 Intra-Refresh P-frame 解碼零錯誤（`ffmpeg -v error -i eye0.hevc -f null -` exit code 0）。
2. **頭盔端 `v4l2_dual_decode_bench`（於 Steam Frame 透過 SSH 無頭執行，或無實機時於 `.195` 跑雙 FFmpeg 軟硬解基準）**：
   - 在 Steam Frame 上同時開啟兩個 `hevc_v4l2m2m` `AVCodecContext` 平行解碼上述兩條 `1920×1920` @ 90 fps（各 75 Mbps）HEVC bitstream 60 秒。
   - **P0 頭盔端無人通過標準**：
     - 雙解碼器合計每幀牆鐘耗時（Wall-clock time per stereo pair）**平均 `< 5.5 ms`、p95 `< 7.5 ms`**，連續 5,400 對立體幀零 V4L2 buffer starvation 或 `EAGAIN` 卡死。
     - **自動退路觸發條件**：若 Snapdragon 8 Gen 3 的 `hevc_v4l2m2m` 雙實例切換開銷導致 p95 `> 8.5 ms`，則自動記錄需將預設每眼解析度降為 `1728×1728` 或啟用伺服器端 GPU 0 NVENC 單流編碼備援（見第六節待決問題 Q1）。

### 4.2 P1：第一期 — 無人端到端最小可行切片（Unattended End-to-End Single-GPU Synthetic Slice — 1 週）

**核心目標**：在不需人類戴頭盔、不依賴外部遊戲存檔的情況下，用無頭合成 OpenXR 測試程式 `vr_linux_probe` 跑通「**OpenXR Pose Runtime (`libviple_openxr.so`) + Vulkan Capture Layer (單卡 GPU 0 先行) + `viplestream-server` (`SL/vr_linux.cpp` + `SL/nvenc_cuda.cpp`) + `VipleStream` 客戶端 (`--display-target pcvr --vr-emulate --vr-synthetic-motion sine`)**」完整閉環！

- **P1 交付範圍**：
  1. 實作 `libviple_openxr.so`（含 `DisplayTimePoseRing`、`pose_history_t`、`virtual_vsync.h` LATCH 微調、`AFR_LEFT_EYE` / `AFR_RIGHT_EYE` 決定性標記）。
  2. 實作 `SL/vr_linux.cpp` 與 `SL/nvenc_cuda.cpp`（先在 GPU 0 單卡將 `AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE` 合成 SBS 或單路匯入 `nvenc_cuda` 編碼，驗證整個 Linux 伺服器 VR 管線與 `SS/video.cpp` 控制迴路）。
  3. 實作無頭 OpenXR 合成測試程式 `vr_linux_probe`：依 `xrLocateViews` 傳回的姿態渲染 3D 棋盤格與旋轉方塊，並在左右眼影像左上角 `64×8` 像素烙印 64-bit 二進位條碼 `(magic=0x5650, world_tick, eye_index, echoSampleId)`。
  4. 客戶端 `vrprobedecode.{h,cpp}` 在 `--vr-emulate` 模式下自動解讀每幀像素條碼並與 `0x81` `VrFrameMeta` 比對。
- **P1 無人自動化通過標準（Pass Criteria — 全部由腳本判讀 exit code 0）**：
  1. **建置與三樹同步閘門**：`pwsh moonlight-qt/moonlight-common-c/check_commonc_sync.ps1` 回傳 `exit code 0`。
  2. **15 分鐘無人閉環烤機**（`vr_linux_probe` + `viplestream-server` + `VipleStream --display-target pcvr --vr-emulate --vr-synthetic-motion sine`）：
     - `[VIPLE-VR] fps>=88`、`echoMatched>=99%`；
     - `vrprobedecode` 統計超過 80,000 幀：**`eyeInversionCount == 0`**（左眼恆為 `eye=0`、右眼恆為 `eye=1`）、**`tickMismatchCount == 0`**（`left.world_tick == right.world_tick`）、**`poseSampleMismatchCount == 0`**（像素烙印的 `echoSampleId` 100% 等於 `0x81` header 的 `echoSampleId`）。
  3. **LATCH 相位鎖定驗證（V-4）**：伺服器 `[VIPLE-VR-LATCH]` 日誌在 30 秒內收斂至 **`lockR >= 80%`**。
  4. **丟包復原與 Intra-Refresh 驗證（V-1）**：以 `--vr-inject-loss 5` 與 `--vr-inject-drop 3` 觸發復原，日誌確認 `LOSS → REFRESH_START → REFRESH_DONE` 完整走完，且全程 **`idrCount == 1`**（證明 `nvenc_cuda` 的 `forceIntraRefreshWithFrameCnt` 在 Linux 運作正常）。

### 4.3 P2：第二期 — 雙卡鏡像 + 雙路協定無人切片（Unattended Dual-GPU & Dual-Stream Slice — 1.5 週）

**核心目標**：開啟雙卡鏡像（GPU 0 左眼、GPU 1 右眼）與 `layout="dual"` 雙 UDP 視訊流（`videoPort` + `eyePort`），並在無人模式下先以 `vr_linux_probe`、再以 **Palworld + UEVR (`SYNCHRONIZED` AFR)** 完成自動化驗收。

- **P2 交付範圍**：
  1. `libVkLayer_viple_dualgpu.so`：啟用雙 `VkDevice` 鏡像、開放並雙卡映射 `external_memory_fd` / `external_semaphore_fd`（T-2）、左眼在 GPU 0 匯出 `CaptureFD_L`、右眼在 GPU 1 匯出 `CaptureFD_R`。
  2. 伺服器端：啟用雙 `encode_run`（`CUcontext 0` + `CUcontext 1`）、`eye_video_sock` UDP Ping 位址學習、AES-GCM `iv[11] = 'V'` vs `'R'`（W-2）、單眼獨立 `0x5507/11 LOSS_EYE` → 單眼 Intra-Refresh Wave（W-3）。
  3. 客戶端：`moonlight-common-c` 雙實例 `g_VideoStreams[2]` / `g_VideoDepack[2]`（W-1）、雙 `FFmpegVideoDecoder`、`VrRenderMetaRing` 雙眼索引（R-3）、`session.cpp` 雙眼總位元率計算（W-6）。
- **P2 無人自動化通過標準（Pass Criteria）**：
  1. **`check_commonc_sync.ps1` 閘門**：`exit code 0`。
  2. **雙卡雙流 Bit-Pattern 與加密驗證**：
     - 啟用 AES-GCM 視訊加密（`SS_ENC_VIDEO`），跑 `vr_linux_probe` + `VipleStream --vr-emulate --vr-synthetic-motion sine` 10,000 幀：雙路解密成功率 `100%`，`vrprobedecode` 斷言 **`eyeInversionCount == 0`**、**`tickMismatchCount == 0`**、**`poseSampleMismatchCount == 0`**。
  3. **單眼獨立丟包注入驗證（V-1）**：
     - 執行 `--vr-inject-loss 5 --vr-inject-eye 0`：僅 `eye=0` 觸發 `0x5507/11 (eye=0) → REFRESH_START (eye=0) → REFRESH_DONE (eye=0)`，`eye=1` 的 `refreshCount == 0`，雙眼 `idrCount == 1`。
     - 執行 `--vr-inject-loss 5 --vr-inject-eye 1`：僅 `eye=1` 觸發 Intra-Refresh，`eye=0` 的 `refreshCount == 0`，雙眼 `idrCount == 1`。
  4. **Palworld + UEVR 無人啟動與硬體預算驗證（V-3）**：
     - 以腳本自動啟動 Steam Proton Palworld + UEVR (`SYNCHRONIZED` AFR) + `VipleStream --vr-emulate --vr-synthetic-motion sine` 連續運行 15 分鐘：
     - `nvidia-smi --query-gpu=index,utilization.gpu,utilization.encoder,memory.used --format=csv` 自動採樣確認：**GPU 0 與 GPU 1 的 `utilization.encoder` 皆 `> 0%`**（雙卡 NVENC 同時工作）、**每張卡 `memory.used < 5000 MiB`**（6 GB VRAM 安全線內）、Layer 遙測 **`pcie_sync_us_per_frame` p95 `< 1.5 ms`**。

### 4.4 P3：第三期 — Steam Frame 實機頭盔雙解碼與配戴驗收（Headset Wearing Verification — 1 週）

- **P3 交付範圍**：
  1. Steam Frame 實機部署雙 `hevc_v4l2m2m` 解碼器 + `XrVideo` 雙眼 swapchain + `XrContext::buildProjection` 同幀屏障（Stereo Pair Barrier）與單眼全幅 `imageRect`（`xrcontext.cpp:6020-6029`）。
  2. 實機配戴遊玩 Palworld + UEVR。
- **P3 通過標準**：
  - 頭盔端 `[VIPLE-XR]` 日誌確認雙眼 `hevc_v4l2m2m` 穩定 90 fps、同幀配對率 `>= 99.5%`、無 500 ms stale 淡出；
  - 實機配戴轉頭與移動時，雙眼立體視差正確、無單眼殘影或閃爍、6DoF 控制器可正常瞄準與投擲帕魯球。

### 4.5 P4：第四期 — 閒置卡跳過繪圖與跨幀依賴調校（Idle-GPU Draw Skipping — 1 週）

- **P4 交付範圍**：
  1. 在 P2/P3 雙卡完整鏡像穩定後，於 `libVkLayer_viple_dualgpu.so` 啟用 `skip_idle_eye_draws`：在 `active_eye == EYE_LEFT` 期間，GPU 1 跳過對 transient color/depth render pass 的 `vkCmdDraw*` / `vkCmdDispatch*`（僅保留寫入跨幀持久資源如 shadow cache / TAA history / buffer copy 的指令）；在 `active_eye == EYE_RIGHT` 期間則由 GPU 0 跳過。
  2. 同時在閒置卡跳過當幀 Per-Eye Constant Buffer 的 dirty-page 跨 PCIe 3.0 x4 複製。
- **P4 通過標準**：
  - 對比 P2 鏡像模式，每卡 GPU render busy time 下降 `>= 30%`，且 Palworld 的 TAA、陰影與粒子效果無閃爍或遺失（若有特定 render pass 具跨幀依賴，支援透過白名單保留雙卡執行）。

---

## 5. 風險評估與退路設計

| 風險項目 | 偵測時機 | 具體退路（Fallback） |
|---|---|---|
| **R1：Snapdragon 8 Gen 3 `hevc_v4l2m2m` 雙實例 90 fps 時分多工耗時超過 8.5 ms** | P0 `v4l2_dual_decode_bench` | **退路 A**：將每眼傳輸解析度由 `1920×1920` 調為 `1728×1728`（像素量降 19%）。<br>**退路 B**：若 V4L2 驅動對雙 context 切換有固定開銷，則在伺服器端讓 GPU 1 僅將右眼 `CaptureImage_R`（或壓縮前 NV12，`1920×1920×1.5 ≈ 5.5 MB`）經 PCIe 匯入 GPU 0，由 GPU 0 單顆 NVENC 編碼單路 SBS，頭盔端退回單解碼器 `layout="sbs"`（仍保留雙卡 AFR 渲染算力翻倍）。 |
| **R2：Palworld (UE5) 在 High 畫質下搭配雙卡鏡像超過 GTX 1660 Ti 6 GB VRAM** | P2 `nvidia-smi` 自動監控 | 預設在 Palworld VR Profile 鎖定 `r.Streaming.PoolSize=2000`（2 GB 貼圖池）與單眼 `1920×1920` 渲染目標，並因 T-1 將 CUDA/NVENC 移出遊戲行程，每卡已先省下 ~250 MB VRAM。 |
| **R3：DXVK 的 Dynamic Ring Buffer 在每幀產生過多 4 KB dirty pages（> 8 MB/幀），拖慢 PCIe 3.0 x4** | P2 Layer `pcie_sync_us_per_frame` 遙測 | 啟用 `PAGEMAP_SCAN` 的 sub-page range 合併，並在 P4 對標記為 per-frame transient uniform buffer 的區段只在當前 `active_eye` 目標卡上傳，不再雙卡同步。 |
| **R4：UEVR 在特定過場動畫短暫切回單眼 2D Present** | P2/P3 遊戲過場 | §1.3 的決定性狀態機在未收到 `AFR_RIGHT_EYE` + `xrEndFrame` 時，可選擇將 `AFR_LEFT_EYE` 同時複製給左右眼（Mono Fallback）或由伺服器 100 ms `VIPLE_VR_FF_REPEATED` 維持畫面，不會導致左右眼奇偶錯位。 |

---

## 6. 仍待使用者決定的問題

在正式動工撰寫 P0 / P1 程式碼前，請使用者拍板以下 4 個決策點：

1. **【Q1：預設每眼串流解析度】**
   在兩張 **GTX 1660 Ti (6 GB VRAM)** 上跑 Palworld (UE5)，您希望預設每眼解析度定為：
   - **選項 A（建議）**：**`1920×1920` / 眼 @ 90 Hz**（雙眼總像素 `7.37 MP`，比 Windows 預設 SBS `3456×1728 = 5.97 MP` 高出 23% 清晰度，且每張 6 GB 顯卡保有 `> 1.6 GB` 安全 VRAM 餘裕）；
   - **選項 B**：**`1728×1728` / 眼 @ 90 Hz**（與目前 Windows 預設每眼像素完全相同，最省 VRAM 與解碼時間）；
   - **選項 C**：**`2160×2160` / 眼 @ 90 Hz**（Steam Frame 面板原生 1:1 解析度，雙眼總像素 `9.33 MP`，但 UE5 貼圖池需壓低至 `1500 MB` 以防 6 GB 爆 VRAM）。
2. **【Q2：CachyOS 主機與 Steam Frame 頭盔的 P0 探針執行方式】**
   CachyOS 雙卡主機與 Steam Frame 頭盔目前是否已連上同一區域網路且可由開發機 SSH 連線執行 P0 無頭探針（`vk_dualgpu_probe` 與 `v4l2_dual_decode_bench`），還是需要先在本地撰寫好 P0 探針腳本後由您手動在 CachyOS 上觸發？
3. **【Q3：P1 單卡最小可行切片的串流封裝】**
   在 P1（第一期：單卡 GPU 0 無人端到端切片）中，為了最小化變數，您是否同意 **P1 先走既有的單路 `layout="sbs"`**（由伺服器端 `nvenc_cuda` 把 Pose Runtime 擷取到的 `AFR_LEFT_EYE` 與 `AFR_RIGHT_EYE` 拼成單路 SBS 送出，先驗通 Linux Pose Runtime + External Memory FD + `nvenc_cuda` + LATCH + Intra-Refresh），到 **P2 再切入 `layout="dual"` 雙路 UDP + 雙解碼器**？
4. **【Q4：音訊擷取後端偏好】**
   CachyOS 預設音訊伺服器為 **PipeWire**。Sunshine Linux 現有音訊擷取支援 PulseAudio 相容層（`pipewire-pulse`）與 PipeWire 原生節點。您希望 VR 模式下沿用 Sunshine 現有的預設 PulseAudio sink monitor，還是自動建立專屬於 VR 遊戲行程的虛擬隔離音訊 sink（避免把主機桌面通知音一起串流進頭盔）？

### 6.1 使用者決定（2026-11-26）

1. Q1：**A**，預設每眼 `1920×1920` @ 90 Hz。
2. Q2：**同意**由開發機透過 SSH 在 CachyOS 與頭盔上跑 P0 無頭探針（不佔 CachyOS 畫面）。
3. Q3：**同意**，P1 先走單路 `layout="sbs"`，P2 才切 `layout="dual"`。
4. Q4：**獨立**，VR 模式自動為遊戲行程建立專屬虛擬音訊 sink。
