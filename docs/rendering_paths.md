# VipleStream 渲染全路徑表

本文件追蹤 VipleStream client (moonlight-qt + moonlight-android) 跟 server
(Sunshine) 在 **Windows / Linux / Android** 三個 ship target 上完整的「解碼 →
補幀 → 顯示」管線。

**Steam Frame 欄位是佔位**：標「3.0 規劃中」的內容來自 3.0 設計定稿，尚未實作，
細節與變更一律以 [`vr_architecture.md`](vr_architecture.md)（§2.1、§2.3、§2.4）為準。
實作落地時把該欄改成實際狀態，並拿掉「規劃中」字樣。

新增 renderer / 改變預設選擇 / 引入新平台時 **同 commit 更新本表**。

---

## 1. 解碼器路徑（Decoder × Platform）

| Codec / Mode | Windows | Linux | Android | Steam Frame（3.0 規劃中，見 `vr_architecture.md` §2.3） |
|---|---|---|---|---|
| H.264 SW | libavcodec (FFmpeg n8.0.1) | 同左 | 同左 | 同左（L4 保底：只保證桌面 1080p60，PCVR 拒絕） |
| H.264 HW（default） | DXVA2 / D3D11VA + NVDEC | VAAPI / VDPAU / V4L2 (RPi) | MediaCodec | cgutman FFmpeg `h264_v4l2m2m` → DRM_PRIME（L2） |
| H.264 HW（Vulkan）| **§J.3.f** `h264_vulkan` (FFmpeg 8.1) | 同（FFmpeg 8.0.1 source build） | NvVideoParser §J.3.e.2.i.8 Phase 2 | — |
| HEVC HW（default） | D3D11VA + NVDEC | VAAPI / VDPAU / V4L2 | MediaCodec | `hevc_v4l2m2m` → DRM_PRIME（L2 主路徑；失敗退 L3 mmap 上傳） |
| HEVC HW（Vulkan）| **§J.3.f** `hevc_vulkan` 1440p120 0.3ms | 同 | NvVideoParser §J.3.e.2.i.8 Phase 1.x | — |
| AV1 SW | libdav1d | 同 | 同 | 同 |
| AV1 HW（default） | D3D11VA + NVDEC | VAAPI（Mesa 24+）| MediaCodec（Pixel 8+）| 待定：需要 L1 自寫 `V4l2Decoder`（視 PoC） |
| AV1 HW（Vulkan）| **§J.3.f** `av1_vulkan` 5ms 底線 | 同 | §J.3.e.2.i.8 Phase 3d.6 grey（停在 libdav1d） | — |

---

## 2. Renderer 類別（IFFmpegRenderer subclass × Platform）

| Renderer | Windows | Linux | Android | macOS | Steam Frame（3.0 規劃中，見 `vr_architecture.md` §2.3–2.4） | 用途 |
|---|---|---|---|---|---|---|
| `SdlRenderer` | ✓ fallback | ✓ fallback | — | ✓ | fallback | 軟體 SDL2 blit |
| `D3D11VARenderer` | ✓ **default**（RS_AUTO cascade 首選；選 RS_D3D11 時強制）| — | — | — | — | DXVA2/D3D11VA HW decode + present |
| `DXVA2Renderer` | ✓ legacy | — | — | — | — | Vista/Win7 fallback |
| `PlVkRenderer` | ✓（RS_VULKAN ❶）| ✓（RIFE/Generic-override 全 disable）| — | ✓ | **α 平面 frontend**（`pl_map_avframe_ex` 吃 DRM_PRIME）；F6 的 `linuxVideoFrontend=auto` 在 aarch64 選它 | libplacebo Vulkan video render |
| `VkFrucRenderer` | ✓（RS_VULKAN auto-FRUC）| ✓ **FRUC primary**（RS_VULKAN 時）| — | — | —（Frame 上 FRUC 預設關閉，XR 模式不做 FRUC） | Vulkan native FRUC（ME / median / warp 內建 shader）|
| `MmalRenderer` | — | RPi 32-bit only | — | — | — | Broadcom MMAL legacy（Pi 3/4）|
| `DrmRenderer` | — | ✓ headless KMS（出貨的 x64 AppImage 沒有，見 ❷） | — | — | **backend**（v4l2m2m 的 DRM_PRIME 幀），不當 frontend | DRM/KMS direct（無 X11 / Wayland）|
| `VAAPIRenderer` | — | ✓ HW decode | — | — | — | Intel/AMD VAAPI |
| `VDPAURenderer` | — | ✓ HW decode | — | — | — | NV legacy VDPAU |
| `EGLRenderer` | — | ✓ EGL display | — | — | 可選（`linuxVideoFrontend=egl`） | GLES 經 EGL，PlVkRenderer fallback |
| `CUDARenderer` | — | (disabled，§J.3.e issue #1314) | — | — | — | ffnvcodec interop（已被 VDPAU/Vulkan 取代）|
| `GenericHwAccelRenderer` | — | ✓ 通用 ffmpeg hwaccel | — | — | — | 通用 hwaccel passthrough |
| `VulkanVideoRenderer` | 只編譯，未接進 renderer cascade（僅 `ncnnfruc.cpp` 的 §J.3.b probe 會建立）| —（`app.pro` 只在 `win32:!winrt` 區塊編譯）| — | — | — | §J.3.b 純 vkCmdDecodeVideoKHR skeleton；實際的 Vulkan Video 解碼走 FFmpeg `*_vulkan` hwaccel（§J.3.f） |
| `XrRenderer`（3.0 新增） | 規劃：S2 開發模擬（`build_moonlight.cmd --openxr`） | 規劃：x86_64 dev 版對 Monado（S1） | — | — | **β／PCVR frontend**（OpenXR quad／projection） | 依賴 `streaming/xr/` 的 XrContext（該目錄只在 `CONFIG+=openxr` 時編譯） |
| `VTBaseRenderer` (vt_avsamplelayer / vt_metal) | — | — | — | ✓ | — | macOS VideoToolbox |
| Android Vulkan (`VkBackend.java` + `vk_backend.c`) | — | — | ✓ §I.D opt-in | — | — | `debug.viplestream.vkprobe=1` 啟用 |
| Android GLES (`FrucRenderer.java` 內建)| — | — | ✓ default | — | — | MediaCodec → SurfaceTexture → GLES |

**❶ RS_VULKAN auto-trigger（v1.3.336 b2b7afd）**：使用者在 Settings 選 Vulkan，
`shouldPreferVulkanDecoderCascade()` + `shouldUseVkFrucRendererForVulkanHwaccel()`
自動 chain Vulkan HW decode + VkFrucRenderer FRUC + DUAL，不需 env var。

**❷ 出貨的 x64 AppImage 與原始碼支援的差別**：`moonlight-qt/scripts/build-appimage-native.sh` 以
`CONFIG+=disable-libdrm CONFIG+=disable-wayland` 建置。因此沒有定義 `HAVE_DRM`、不編譯 `drm.cpp`，
也就沒有 DrmRenderer 與 KMS/DRM 直出。moonlight 自己的 Wayland 整合也不在裡面，包括 `HAS_WAYLAND`
的 Wayland vsync source 與 `WMUtils::isRunningWayland()` 偵測，以及 `HAVE_LIBVA_WAYLAND` 讓 VAAPI 在
Wayland 視窗上開 display 的路徑。Vulkan surface 由 SDL 建立，不受這兩個旗標影響。本文件 Linux 欄的
DRM／KMS 路徑與上述 Wayland 專屬路徑，只適用於從原始碼或 distro 套件自建的版本。

---

## 3. FRUC backend 路徑

### 3.a `IFrucBackend` 抽象（D3D11VARenderer / Android FrucRenderer 用）

| Backend (`enum FrucBackend`) | Windows D3D11 path | Linux | Android |
|---|---|---|---|
| `FB_GENERIC` | ✓ **default**（`streamingpreferences.cpp` 的 `SER_FRUCBACKEND` 預設值）`genericfruc.cpp` D3D11 compute（ME → median → warp） | — | ✓ `FrucRenderer.java` GLES 等價 |
| `FB_NVIDIA_OF` | ✓ `nvofruc.cpp` NVOFA 硬體光流（Pascal+） | — | — |
| `FB_DIRECTML` | ✓ `directmlfruc.cpp` ONNX RIFE × DirectML（D3D12） | — | — |
| `FB_NCNN` | ✓ `ncnnfruc.cpp` NCNN-Vulkan RIFE custom layer | — | — |

ONNX models（`fruc.onnx` 22 MB + `fruc_fp16.onnx` 11 MB + `fruc_ifrnet_s.onnx`
5.5 MB）固定 pin 在 `assets-onnx-v1` GitHub release tag，runtime ModelFetcher
依需下載到 `%LOCALAPPDATA%\VipleStream\fruc_models\`。

### 3.b PlVkRenderer 內建 FRUC override（不走 `IFrucBackend`）

PlVkRenderer 自己有一條 §J.3.e.2.e1b override path，跑 NV12 → RGB → VkImage
compute pipeline + 可選 RIFE Phase B（§J.3.e.2.e2）注入 ncnn::Net forward：

- **Windows：** 完整可用，`libs/windows/ncnn/` prebuilt + DML cascade fallback
- **Linux：** ncnn 經 `wsl_build_moonlight.sh` source build 進 `/usr/local/`
  （+9 MB AppImage，`NCNN_SIMPLEVK=OFF` 避免跟系統 vulkan SDK 撞 type）；技術上
  RIFE Phase B 可跑，但 stream pipeline 預設仍走 VkFrucRenderer
- **Android：** N/A（Android 走獨立的 `vk_backend.c` stack）

### 3.c VkFrucRenderer 內建 FRUC（**cross-platform，Linux 主力**）

獨立 IFFmpegRenderer subclass，自帶完整 ME / median / warp compute pipeline，
**不依賴 ncnn / DirectML / D3D11**：

- **ME**：4 個 `NextStartCode{C, SSSE3, AVX2, AVX512}` CPU detect-dispatch（runtime CPUID）
- **補幀 compute**：純 Vulkan SPIR-V shader，glslang at build-time 預編成
  `vkfruc_*.spv.h` header（`include` 進 vkfruc.cpp）
- **DUAL present**：FIFO + dual swapchain image
- **Windows：** 整合進 RS_VULKAN（v1.3.336 b2b7afd 起 auto-trigger）
- **Linux：** 單獨可選 renderer，§J.3.f 同款 FFmpeg 8.1 vulkan hwaccel +
  VkFrucRenderer 補幀
- **Android：** 不直接用 — Android 走自己的 `vk_backend.c` (§I.D)

### 3.d Android FRUC（獨立 stack）

| Path | 啟用條件 | 內容 |
|---|---|---|
| **GLES (`FrucRenderer.java`)** | default | MediaCodec → SurfaceTexture → GL compute → present |
| **Vulkan (`VkBackend.java` + `jni/.../vk_backend.c`)** | `debug.viplestream.vkprobe=1` opt-in | AHardwareBuffer 零拷貝 import → ncnn-free Vulkan compute（§I.D Phase D.2.x multi-queue async）→ swapchain |

---

## 4. Display / Swapchain × Platform

| 機制 | Windows | Linux | Android | Steam Frame（3.0 規劃中，見 `vr_architecture.md` §1.1、§2.4） |
|---|---|---|---|---|
| D3D11 swapchain | ✓ D3D11VARenderer | — | — | — |
| Vulkan WSI（Win32 surface ext） | ✓ PlVk / VkFruc | — | — | — |
| Vulkan WSI（XLib + XCB + Wayland surface ext） | — | ✓（§K.1 Phase 1b 加） | — | α：PlVk → Wayland（gamescope nested，SteamVR dashboard overlay） |
| Vulkan WSI（Android surface ext） | — | — | ✓ | — |
| KMS/DRM direct（無 compositor） | — | ✓ DrmRenderer（出貨的 x64 AppImage 沒有，見 §2 的 ❷） | — | — |
| EGL（X11 / GBM） | — | ✓ EGLRenderer | — | 可選 |
| GLES via SDL | ✓ SdlRenderer | ✓ SdlRenderer | — | fallback |
| AHardwareBuffer + GLES Surface | — | — | ✓ MediaCodec output | — |
| OpenXR swapchain（`XR_KHR_vulkan_enable2`，3.0 新增） | 規劃：S2 開發模擬 | 規劃：S1 Monado | — | β／PCVR：XrRenderer |
| VideoToolbox CALayer（macOS 用） | — | — | — | — |

---

## 5. HDR support × Platform

| 階段 | Windows | Linux | Android | Steam Frame（3.0 規劃中） |
|---|---|---|---|---|
| HDR10 swapchain（A2B10G10R10 + ST.2084） | ✓ via PlVkRenderer / VkFrucRenderer §I HDR2 | ✓ 同源 code | ✓ §I HDR1 Android 13+ | 未規劃；XR 模式與 VR 編碼 profile 固定 8-bit SDR（`vr_architecture.md` §2.6、§3.4） |
| BT.2020 1000nits metadata（`vkSetHdrMetadataEXT`） | ✓ | ✓ if compositor 支援 | ✓ Surface API | 同上 |
| SDR-on-HDR fragment shader（sRGB → linear → PQ） | ✓ §I HDR3 (v1.2.189) | ✓ 同 | ✓ | 同上 |

---

## 6. Default 選擇 + Fallback 鏈

桌面 client 的 renderer 預設值是 **`RS_AUTO`**（全平台共用，`streamingpreferences.cpp` 讀
`SER_RENDERERSEL` 的預設值；§J.3.e.2.i，v1.4.137 起）：交給 FFmpeg 依 decoder 的 hw_config
順序挑第一個能 init 的 hwaccel，等同上游 Moonlight 行為。已在 Settings 選過 Vulkan／D3D11 的
使用者維持原設定。FRUC backend 預設是 `FB_GENERIC`（§3.a）。

| Platform | 預設 | RS_VULKAN 觸發行為 | Fallback 鏈 |
|---|---|---|---|
| Windows | `RS_AUTO`（實際多半落在 D3D11VARenderer；FRUC backend 預設 Generic） | 切 RS_VULKAN → VkFrucRenderer + Vulkan HW decode + FRUC + DUAL（v1.3.336 起） | RS_AUTO：D3D11VA（D3D11VARenderer）→ Vulkan（GPU 有 Vulkan video decode queue 時，PlVkRenderer 當 backend）→ DXVA2（DXVA2Renderer）→ D3D11VA 重試 → SW decode。SW decode 的 renderer 是 PlVkRenderer；Vulkan 被判定為慢、或選了 RS_D3D11（只在 decoder 沒列出 pix_fmts 的路徑檢查，原生 h264／hevc 屬於這種）時才改用 SdlRenderer。RS_VULKAN：Vulkan hwaccel＋VkFrucRenderer init 失敗 → 回到同一條 cascade（D3D11VA → DXVA2 → SW decode） |
| Linux | `RS_AUTO`：backend 依 hw_config 順序挑 VAAPIRenderer／VDPAURenderer，GPU 有 Vulkan video decode queue 時也可能是 PlVkRenderer；frontend 由 `createFrontendRenderer` 決定（見表下「Linux RS_AUTO」）。出貨的 x64 AppImage 沒有 DrmRenderer（❷） | RS_VULKAN → Vulkan hwaccel 解碼 + VkFrucRenderer 補幀 | backend：hwaccel 全部失敗 → SW decode（DrmRenderer〔慢 GPU〕→ PlVkRenderer〔Vulkan 不慢時〕→ SdlRenderer）。frontend：alternate frontend 不成 → backend 能直接顯示就用它，否則 DrmRenderer〔慢 GPU〕→ EGLRenderer〔GL 慢時才在這裡試〕→ SdlRenderer |
| Android | GLES (`FrucRenderer.java`) | `debug.viplestream.vkprobe=1` opt-in 切 VkBackend | VkBackend init 失敗 → SIGSEGV canary 落回 GLES |
| Steam Frame（3.0 規劃中，見 `vr_architecture.md` §2.3） | `RS_AUTO` + `linuxVideoFrontend=auto`（aarch64 → PlVk），這組預設寫進 G-α | 沒有 Vulkan Video 的裝置要快速略過 Vulkan hwaccel、記 log 後退回 cascade | L2 v4l2m2m DRM_PRIME → L3 mmap 上傳 → L4 軟體解碼；β 的 XrRenderer 失敗退 PlVk 平面 |

**Linux RS_AUTO**（`ffmpeg.cpp` 的 `createHwAccelRenderer`、`createFrontendRenderer`、
`tryInitializeRendererForUnknownDecoder`）：
- backend 依 FFmpeg hw_config 順序挑：VAAPI → VAAPIRenderer，VDPAU → VDPAURenderer。GPU 有 Vulkan video
  decode queue 時，Vulkan hwaccel 由 **PlVkRenderer 當 backend**；它自己就能顯示，frontend 也是它。
- 每個 backend 先試 alternate frontend。10-bit HDR 依序試 PlVkRenderer（Vulkan 不慢時）→ DrmRenderer（❷）→
  PlVkRenderer（Vulkan 慢時的最後手段），都要真的回報 HDR 支援才採用。SDR 只在 dev 用的 `PREFER_VULKAN=1`
  下試 PlVkRenderer。兩種情況最後都會試 EGLRenderer（GL 不慢、backend 能 export EGL 時）。alternate frontend
  都不成，才退回上表 Fallback 欄的一般 frontend 選法。
- 所以「SDR 下 PlVkRenderer 不會被優先挑成 frontend」只適用於 alternate-frontend 分支。在 RS_AUTO 下，
  PlVkRenderer 仍會以 Vulkan hwaccel backend 或 SW 解碼 renderer 的身分出現。
- 3.0 的 F6（`linuxVideoFrontend`）會改這裡的 frontend 決策，落地時同 commit 更新本段。

---

## 7. 關鍵相依矩陣（Linux ship 之後要打包進 AppImage 的 .so）

| 元件 | Windows ship | Linux AppImage ship |
|---|---|---|
| FFmpeg 8.1 vulkan hwaccel | `avcodec-62.dll` 5.2 MB（client zip 內） | `libavcodec.so.62` source-build（AppImage 內） |
| libplacebo | `libplacebo-360.dll` | `libplacebo.so.360`（haasn/libplacebo v7.360.0 source build） |
| dav1d | `libdav1d-7.dll` | `libdav1d.so.7` |
| SDL | `SDL2.dll` + `SDL3.dll`（`build_moonlight_package.cmd` 的 `[pkg 1/5]` 清單） | `libSDL2-2.0.so.0`：x64 AppImage 連結真正的 SDL2（不是 sdl2-compat），見 `build-appimage-native.sh` 檔頭 |
| ncnn (Vulkan EP) | `ncnn.dll`（libs/windows/ncnn prebuilt） | `libncnn.so.1` source build with `NCNN_SIMPLEVK=OFF`（+9 MB） |
| DirectML / ONNX Runtime | `DirectML.dll` + `onnxruntime.dll` | — Windows-only |
| Aftermath SDK | `GFSDK_Aftermath_Lib.x64.dll` | — Windows-only |
| nvvideoparser | （static linked，MSVC `/arch:AVX2`）| （static linked，gcc `-mavx2 -mfma -mavx512*`） |
| Sunshine NVENC | `viplestream-server.exe` 內 | `libnvidia-encode.so`（system runtime） |
| Sunshine VAAPI | — | **OFF**（FetchContent FFmpeg 用 `vaMapBuffer2` 未在 noble libva 2.20 上；server 走 NVENC）|

---

## 8. §K.1 Linux build 卡點對照

| 卡點 | 路徑 / 類別 | 解法（最小改動）|
|---|---|---|
| `plvk.h #include <ncnn/mat.h>` not found | PlVkRenderer 整段 RIFE / Override 編譯依賴 | 裝 ncnn from source 進 `/usr/local`（+9 MB），源碼一行不動。`scripts/wsl_build_moonlight.sh` 自動處理 |
| ncnn `vulkan_header_fix.h` 跟 system Vulkan SDK 撞 type | `NCNN_SIMPLEVK=ON` 預設讓 ncnn 用自己的 vulkan stub | 改用 `-DNCNN_SIMPLEVK=OFF`，ncnn 直接 include 系統 `<vulkan/vulkan.h>`，走 modern `VK_HEADER_VERSION=313` 路徑 |
| Sunshine `vaMapBuffer2 undefined` | FetchContent FFmpeg `libavutil.a` 用 libva 2.21+ symbol，noble 系統 libva 2.20 沒 | `linux_build.sh --skip-libva` 同時 trigger `-DSUNSHINE_ENABLE_VAAPI=OFF`；server 走 NVENC，VAAPI 取消 |
| `nv-codec-headers v13` field renames (4 處) | `Sunshine/src/nvenc/nvenc_base.cpp` | ✓ 已 patch v12/v13 dual via `#if NVENCAPI_MAJOR_VERSION >= 13` |
| `closesocket close` macro 撞 class member | `relay.cpp` / `stun.cpp` | ✓ 已改 `(::close(fd))` |
| `chrono::ratio rep != 1ll` | `stream.cpp std::max` | ✓ 已加 `<long long>` template arg |
| `/arch:AVX2` 沒 `*-msvc` gate | `nvvideoparser.pro` | ✓ 已拆 msvc / gcc 兩條，gcc 改 `-mssse3 -mavx -mavx2 -mfma -mavx512{f,bw,dq,vl}` |
| qmake `.qmake.cache` 帶 MSVC spec from rsync | `rsync` 從 Windows 帶過來的 qmake artifact | ✓ 已 gitignore + 清理腳本 |
| Win32-only Vulkan ext 沒 Linux elif | `vkfruc.cpp` 兩處 | ✓ 已加 X11/XCB/Wayland surface + FD external memory ext |
| Sunshine packaging FQDN 不對齊 PROJECT_FQDN | `dev.lizardbyte.app.Sunshine.*` 7 檔 | ✓ 已 `git mv` 到 `app.viplestream.server.*`，`linux_build.sh` validation step hardcoded path 同步改 |

---

## 9. 修改本表的時機

- 新增 IFFmpegRenderer subclass → 加進 §2 Renderer 表
- 新增 IFrucBackend → 加進 §3.a 表
- 新增 ship target（macOS / FreeBSD / Pi 5 等）→ 加 column
- Steam Frame（3.0）規劃項目實作或設計變更 → 更新各表的 Steam Frame 欄，落地的格子拿掉「規劃」字樣
- 改 default renderer / FRUC 順序 → 更新 §6
- 修 Linux / 其他平台 build bug → 對應條目進 §8
