# VipleStream 渲染全路徑表

本文件追蹤 VipleStream client (moonlight-qt + moonlight-android) 跟 server
(Sunshine) 在 **Windows / Linux / Android** 三個 ship target 上完整的「解碼 →
補幀 → 顯示」管線。

新增 renderer / 改變預設選擇 / 引入新平台時 **同 commit 更新本表**。

---

## 1. 解碼器路徑（Decoder × Platform）

| Codec / Mode | Windows | Linux | Android |
|---|---|---|---|
| H.264 SW | libavcodec (FFmpeg n8.0.1) | 同左 | 同左 |
| H.264 HW（default） | DXVA2 / D3D11VA + NVDEC | VAAPI / VDPAU / V4L2 (RPi) | MediaCodec |
| H.264 HW（Vulkan）| **§J.3.f** `h264_vulkan` (FFmpeg 8.1) | 同（FFmpeg 8.0.1 source build） | NvVideoParser §J.3.e.2.i.8 Phase 2 |
| HEVC HW（default） | D3D11VA + NVDEC | VAAPI / VDPAU / V4L2 | MediaCodec |
| HEVC HW（Vulkan）| **§J.3.f** `hevc_vulkan` 1440p120 0.3ms | 同 | NvVideoParser §J.3.e.2.i.8 Phase 1.x |
| AV1 SW | libdav1d | 同 | 同 |
| AV1 HW（default） | D3D11VA + NVDEC | VAAPI（Mesa 24+）| MediaCodec（Pixel 8+）|
| AV1 HW（Vulkan）| **§J.3.f** `av1_vulkan` 5ms 底線 | 同 | §J.3.e.2.i.8 Phase 3d.6 grey（停在 libdav1d） |

---

## 2. Renderer 類別（IFFmpegRenderer subclass × Platform）

| Renderer | Windows | Linux | Android | macOS | 用途 |
|---|---|---|---|---|---|
| `SdlRenderer` | ✓ fallback | ✓ fallback | — | ✓ | 軟體 SDL2 blit |
| `D3D11VARenderer` | ✓ **default (RS_D3D11)** | — | — | — | DXVA2/D3D11VA HW decode + present |
| `DXVA2Renderer` | ✓ legacy | — | — | — | Vista/Win7 fallback |
| `PlVkRenderer` | ✓（RS_VULKAN ❶）| ✓（RIFE/Generic-override 全 disable）| — | ✓ | libplacebo Vulkan video render |
| `VkFrucRenderer` | ✓（RS_VULKAN auto-FRUC）| ✓ **FRUC primary** | — | — | Vulkan native FRUC（ME / median / warp 內建 shader）|
| `MmalRenderer` | — | RPi 32-bit only | — | — | Broadcom MMAL legacy（Pi 3/4）|
| `DrmRenderer` | — | ✓ headless KMS | — | — | DRM/KMS direct（無 X11 / Wayland）|
| `VAAPIRenderer` | — | ✓ HW decode | — | — | Intel/AMD VAAPI |
| `VDPAURenderer` | — | ✓ HW decode | — | — | NV legacy VDPAU |
| `EGLRenderer` | — | ✓ EGL display | — | — | GLES 經 EGL，PlVkRenderer fallback |
| `CUDARenderer` | — | (disabled，§J.3.e issue #1314) | — | — | ffnvcodec interop（已被 VDPAU/Vulkan 取代）|
| `GenericHwAccelRenderer` | — | ✓ 通用 ffmpeg hwaccel | — | — | 通用 hwaccel passthrough |
| `VulkanVideoRenderer` | — | ✓ Linux 主用 | — | — | 純 vkCmdDecodeVideoKHR 路徑 |
| `VTBaseRenderer` (vt_avsamplelayer / vt_metal) | — | — | — | ✓ | macOS VideoToolbox |
| Android Vulkan (`VkBackend.java` + `vk_backend.c`) | — | — | ✓ §I.D opt-in | — | `debug.viplestream.vkprobe=1` 啟用 |
| Android GLES (`FrucRenderer.java` 內建)| — | — | ✓ default | — | MediaCodec → SurfaceTexture → GLES |

**❶ RS_VULKAN auto-trigger（v1.3.336 b2b7afd）**：使用者在 Settings 選 Vulkan，
`shouldPreferVulkanDecoderCascade()` + `shouldUseVkFrucRendererForVulkanHwaccel()`
自動 chain Vulkan HW decode + VkFrucRenderer FRUC + DUAL，不需 env var。

---

## 3. FRUC backend 路徑

### 3.a `IFrucBackend` 抽象（D3D11VARenderer / Android FrucRenderer 用）

| Backend (`enum FrucBackend`) | Windows D3D11 path | Linux | Android |
|---|---|---|---|
| `FB_GENERIC` | ✓ `genericfruc.cpp` D3D11 compute（ME → median → warp） | — | ✓ `FrucRenderer.java` GLES 等價 |
| `FB_NVIDIA_OF` | ✓ `nvofruc.cpp` NVOFA 硬體光流（Pascal+） | — | — |
| `FB_DIRECTML` | ✓ **default** `directmlfruc.cpp` ONNX RIFE × DirectML（D3D12） | — | — |
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

| 機制 | Windows | Linux | Android |
|---|---|---|---|
| D3D11 swapchain | ✓ D3D11VARenderer | — | — |
| Vulkan WSI（Win32 surface ext） | ✓ PlVk / VkFruc | — | — |
| Vulkan WSI（XLib + XCB + Wayland surface ext） | — | ✓（§K.1 Phase 1b 加） | — |
| Vulkan WSI（Android surface ext） | — | — | ✓ |
| KMS/DRM direct（無 compositor） | — | ✓ DrmRenderer | — |
| EGL（X11 / GBM） | — | ✓ EGLRenderer | — |
| GLES via SDL | ✓ SdlRenderer | ✓ SdlRenderer | — |
| AHardwareBuffer + GLES Surface | — | — | ✓ MediaCodec output |
| VideoToolbox CALayer | — | — | — | （macOS 用） |

---

## 5. HDR support × Platform

| 階段 | Windows | Linux | Android |
|---|---|---|---|
| HDR10 swapchain（A2B10G10R10 + ST.2084） | ✓ via PlVkRenderer / VkFrucRenderer §I HDR2 | ✓ 同源 code | ✓ §I HDR1 Android 13+ |
| BT.2020 1000nits metadata（`vkSetHdrMetadataEXT`） | ✓ | ✓ if compositor 支援 | ✓ Surface API |
| SDR-on-HDR fragment shader（sRGB → linear → PQ） | ✓ §I HDR3 (v1.2.189) | ✓ 同 | ✓ |

---

## 6. Default 選擇 + Fallback 鏈

| Platform | 預設 | RS_VULKAN 觸發行為 | Fallback 鏈 |
|---|---|---|---|
| Windows | `RS_D3D11`（D3D11VARenderer + DirectML FRUC） | 切 RS_VULKAN → VkFrucRenderer + Vulkan HW decode + FRUC + DUAL（v1.3.336 起） | RS_VULKAN init 失敗 → fallback D3D11VA → fallback DXVA2 → SdlRenderer |
| Linux | `RS_VULKAN` 預設（PlVkRenderer 主、VkFrucRenderer 補幀） | 同 desktop | PlVkRenderer 失敗 → VAAPI / VDPAU → DRM / EGL → SdlRenderer |
| Android | GLES (`FrucRenderer.java`) | `debug.viplestream.vkprobe=1` opt-in 切 VkBackend | VkBackend init 失敗 → SIGSEGV canary 落回 GLES |

---

## 7. 關鍵相依矩陣（Linux ship 之後要打包進 AppImage 的 .so）

| 元件 | Windows ship | Linux AppImage ship |
|---|---|---|
| FFmpeg 8.1 vulkan hwaccel | `avcodec-62.dll` 5.2 MB（client zip 內） | `libavcodec.so.62` source-build（AppImage 內） |
| libplacebo | `libplacebo-360.dll` | `libplacebo.so.360`（haasn/libplacebo v7.360.0 source build） |
| dav1d | `libdav1d-7.dll` | `libdav1d.so.7` |
| SDL3 | `SDL3.dll` | `libSDL3.so.0`（`libsdl-org/SDL` release-3.4.2） |
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
- 改 default renderer / FRUC 順序 → 更新 §6
- 修 Linux / 其他平台 build bug → 對應條目進 §8
