# §I.C — GLES FRUC → Vulkan SPIR-V port spec

**Status：** §I.C.1 recon (此份文件)。實作從 §I.C.2 起。

**目的：** 把 `moonlight-android/app/src/main/assets/shaders/` 下的 GLES 3.1
FRUC pipeline 完整 port 到 Vulkan compute pipeline，跑進 §I.B 已建好的
`VkBackend`（`moonlight-android/app/src/main/jni/moonlight-core/vk_backend.c`）。
實作分 §I.C.2 → §I.C.7（見 `docs/TODO.md` 子計畫表）。

---

## 1. Shader 清單

| GLES 來源 (`assets/shaders/`) | 行數 | 變體 | Vulkan 對應 (`jni/moonlight-core/shaders/`) |
|---|---|---|---|
| `motionest_compute.glsl` | 279 | × 3 (`QUALITY_LEVEL=0/1/2`) | `motionest_compute_q{0,1,2}.comp` → 3 SPV |
| `mv_median.glsl` | 88 | × 1 | `mv_median.comp` |
| `warp_compute.glsl` | 220 | × 3 (`QUALITY_LEVEL=0/1/2`) | `warp_compute_q{0,1,2}.comp` → 3 SPV |
| `oes_to_rgba.frag` | 9 | — | **不 port**（VkSamplerYcbcrConversion 取代，需要新的 `ycbcr_to_rgba.comp` 把 sampler 結果寫到 storage image） |
| `blit.frag` | 8 | — | **沿用 `video_sample.frag`**（功能相同，B.2c.3c.3 已有） |
| `fullscreen.vert` | 9 | — | **已 port**（B.2c.3c.2 起在用） |

**新增：** `ycbcr_to_rgba.comp`（替代 OES→RGBA 的角色）— Vulkan 的
`VkSamplerYcbcrConversion` 只能用在 combined image sampler，不能直接寫
storage image，所以中間需要一個 compute pass 從 YCbCr-sampled tex 寫到
`currFrameRgba` storage image。

---

## 2. 共享 storage images / sampled images

跨 shader 持有的 GPU resource，在 §I.C.3 一次性配置：

| 名稱 | 格式 | 解析度 | 用途 | layout 需求 |
|---|---|---|---|---|
| `ycbcrSampledImage` | external (AHB) | 1920×1088 | YCbCr sampler 輸入（每 frame import + 用完釋放） | `SHADER_READ_ONLY_OPTIMAL` |
| `currFrameRgba` | `R8G8B8A8_UNORM` | 1920×1080 | YCbCr→RGBA 寫入；ME / warp 讀取 | `GENERAL`（write）→ `SHADER_READ_ONLY_OPTIMAL`（sample） |
| `prevFrameRgba` | `R8G8B8A8_UNORM` | 1920×1080 | 上一 frame 的 RGBA；ME / warp 讀取 | `SHADER_READ_ONLY_OPTIMAL` |
| `motionField` | `R32_SINT` | 30×17（block 64） | ME 寫，mv_median 讀 | `GENERAL` |
| `prevMotionField` | `R32_SINT` | 30×17 | ME 讀（temporal predictor） | `SHADER_READ_ONLY_OPTIMAL` |
| `filteredMotionField` | `R32_SINT` | 30×17 | mv_median 寫，warp 讀 | `GENERAL` ↔ `SHADER_READ_ONLY_OPTIMAL` |
| `interpFrame` | `R8G8B8A8_UNORM` | 1920×1080 | warp 寫，最後 blit 到 swapchain | `GENERAL` ↔ `SHADER_READ_ONLY_OPTIMAL` |

Block size = 64 (跟 GLES 一致)。MV 解析度 = `ceil(W/64) × ceil(H/64)`，
1920×1088 → 30×17。

每 frame 結束後需要：`copyTexture(currFrameRgba → prevFrameRgba)` +
`copyTexture(motionField → prevMotionField)`。Vulkan 走 `vkCmdCopyImage`（同
format 同尺寸），不需要再寫 blit shader。

---

## 3. GLSL 310 ES → GLSL 450（Vulkan 方言）逐項差異

| GLES 310 ES 寫法 | Vulkan GLSL 450 寫法 | 備註 |
|---|---|---|
| `#version 310 es` | `#version 450` | `#extension GL_GOOGLE_include_directive : enable`（如要 include） |
| `precision highp float;` 等 | **整段刪掉** | Vulkan 預設 highp |
| `uniform sampler2D foo;` | `layout(set=0, binding=N) uniform sampler2D foo;` | binding 必須顯式 |
| `uniform uint frameWidth;` | 改用 push constant block | Vulkan 不支援 loose uniform |
| `layout(r32i, binding=0) writeonly uniform highp iimage2D mv;` | `layout(set=0, binding=N, r32i) writeonly uniform iimage2D mv;` | `highp` 拿掉，`set=` 加上 |
| `#extension GL_OES_EGL_image_external_essl3 : require` | **刪掉**（OES 不存在） | OES 路徑由 YCbCr sampler 取代 |
| `samplerExternalOES` | `sampler2D`（含 `VkSamplerYcbcrConversion`） | sampler 端在 host 程式碼設好，shader 看到的是普通 `sampler2D`，回傳 RGB |
| `gl_GlobalInvocationID` / `imageSize` / `texelFetch` / `bitCount` | 完全相同 | GLSL 450 全支援 |

**Push constant block 範例（取代 ME 的 3 個 `uniform uint`）：**
```glsl
layout(push_constant) uniform PC {
    uint frameWidth;   // 4
    uint frameHeight;  // 4
    uint blockSize;    // 4
} pc;
// shader 內：pc.frameWidth 取代 frameWidth
```

每個 shader push constant ≤ 16 bytes，遠低於 128 bytes 的 Vulkan 1.0 保證下限。

---

## 4. 各 shader 的 binding layout（descriptor set 規劃）

統一 set=0，binding 從 0 開始遞增。Vulkan compute pipeline 一個
descriptor set layout 對應一個 pipeline；4 個 pipeline = 4 個 layout，
但實作可以用一個 set 同時包含上限的 binding 給所有 pipeline，多餘的
binding 在不需要的 pipeline 留空（trade-off：簡化 vs 浪費 descriptor）。
建議走「每 pipeline 各一個 layout」較乾淨。

| Shader | binding 0 | binding 1 | binding 2 | binding 3 (storage) |
|---|---|---|---|---|
| `ycbcr_to_rgba` | `sampler2D yuv` (YCbCr immutable sampler) | — | — | `image2D currFrameRgba` (rgba8) |
| `motionest` (Q0/Q1) | `sampler2D prevFrame` | `sampler2D currFrame` | `isampler2D prevMotionField` | `iimage2D motionField` (r32i) |
| `motionest` (Q2) | `sampler2D prevFrame` | `sampler2D currFrame` | — *(無 temporal)* | `iimage2D motionField` (r32i) |
| `mv_median` | `isampler2D mvIn` | — | — | `iimage2D mvOut` (r32i) |
| `warp` | `sampler2D prevFrame` | `sampler2D currFrame` | `isampler2D motionField` | `image2D interpFrame` (rgba8) |

**注意：** Q2 的 motionest 沒有 prevMotionField binding —
descriptor set layout 在編譯時就決定，要嘛三個 shader 共用一個 layout
（Q2 也宣告 binding 2 但不讀），要嘛 Q2 用獨立 layout。**選共用** 比較
省 descriptor 設定碼。

---

## 5. Push constant 規劃

| Shader | Push constants | 大小 |
|---|---|---|
| `ycbcr_to_rgba` | `uvec2 dims` | 8 B |
| `motionest` | `uvec3 (frameWidth, frameHeight, blockSize)` | 12 B |
| `mv_median` | `uvec2 (mvWidth, mvHeight)` | 8 B |
| `warp` | `uvec3 (frameWidth, frameHeight, mvBlockSize)` + `float blendFactor` | 16 B |

Pipeline layout 統一宣告 `push_constant` range = 16 bytes / `STAGE_COMPUTE_BIT`。

---

## 6. Per-frame command buffer 結構（§I.C.4 才實作，這裡先列）

```
import AHB → VkImage (ycbcrSampledImage)
  barrier: UNDEFINED → SHADER_READ_ONLY_OPTIMAL (foreign queue → graphics queue)

dispatch ycbcr_to_rgba:
  workgroups = (1920/8, 1080/8, 1)
  barrier: currFrameRgba GENERAL → SHADER_READ_ONLY_OPTIMAL

dispatch motionest_q{0|1|2}:
  workgroups = (30/8, 17/8, 1) = (4, 3, 1)
  barrier: motionField GENERAL → SHADER_READ_ONLY_OPTIMAL

dispatch mv_median:
  workgroups = (4, 3, 1)
  barrier: filteredMotionField GENERAL → SHADER_READ_ONLY_OPTIMAL

dispatch warp_q{0|1|2}:
  workgroups = (1920/8, 1080/8, 1) = (240, 135, 1)
  barrier: interpFrame GENERAL → SHADER_READ_ONLY_OPTIMAL

graphics renderpass (interp → swapchain image i):
  bind video_sample.frag with sampler bound to interpFrame
  draw 3 (fullscreen triangle)
  vkQueuePresentKHR → present #1

graphics renderpass (real → swapchain image i+1):
  bind video_sample.frag with sampler bound to currFrameRgba
  draw 3
  vkQueuePresentKHR → present #2

vkCmdCopyImage: currFrameRgba → prevFrameRgba
vkCmdCopyImage: motionField → prevMotionField
vkQueueWaitIdle (暫時，Phase D 移除)
```

對比 GLES path 的 `eglSwapBuffers` × 2 + `eglPresentationTimeANDROID`，
Vulkan path 走兩次 `vkQueuePresentKHR`（搭 §I.C.6 的
`VK_GOOGLE_display_timing` 帶 PTS）。

---

## 7. 編譯流程

### 7.1 glslc 路徑

NDK 內建 glslc：
```
$ANDROID_HOME/ndk/<version>/shader-tools/windows-x86_64/glslc.exe
```
本機 NDK 27 路徑（CLAUDE.md `build_android.cmd` 已 resolve `ANDROID_HOME`）。

### 7.2 編譯腳本（§I.C.2 一起 ship）

新增 `moonlight-android/app/src/main/jni/moonlight-core/shaders/build_shaders.cmd`：

```cmd
@echo off
set GLSLC=%ANDROID_HOME%\ndk\27.0.12077973\shader-tools\windows-x86_64\glslc.exe
%GLSLC% ycbcr_to_rgba.comp        -o ycbcr_to_rgba.comp.spv
%GLSLC% mv_median.comp            -o mv_median.comp.spv
for %%Q in (0 1 2) do (
  %GLSLC% -DQUALITY_LEVEL=%%Q motionest_compute.comp -o motionest_compute_q%%Q.comp.spv
  %GLSLC% -DQUALITY_LEVEL=%%Q warp_compute.comp      -o warp_compute_q%%Q.comp.spv
)
:: xxd -i 包成 .spv.h
for %%F in (*.comp.spv) do (
  xxd -i %%F | sed "s|^unsigned |static const unsigned |" > %%F.h
)
```

### 7.3 為什麼不在 build 時用 CMake `add_custom_command` 自動編？

ndk-build（不是 cmake）目前的 `Android.mk` 沒有跑 glslc 的 hook；走
checked-in `.spv.h` 跟 §I.B 的 `fullscreen.vert.spv.h` 一致，diff 看得到
shader 改動。用 cmake 自動編 → 切換到 cmake 是 §I.D 之後再做的事。

---

## 8. 風險點清單（§I.C.2~ 動工時要注意）

1. **Adreno 620 對 `r32i` storage image 的支援**
   `vkGetPhysicalDeviceFormatProperties(VK_FORMAT_R32_SINT)` 必須回傳
   `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT`。Adreno 620 / Vulkan 1.1.128
   理應支援，但 §I.C.3 init 時 probe 一下，failed 就退回 GLES。

2. **YCbCr sampler 跟 sampled-image-only-storage 的衝突**
   YCbCr sampler **不能** combined 進 storage image binding。所以
   `ycbcr_to_rgba.comp` 一定要 read combined image sampler + write
   separate storage image，**不能** 寫 in-place。

3. **`bitCount()` 在 GLSL 450 沒問題**，但 **在 SPIR-V 1.0 對應 `OpBitCount`，需要 `Shader` capability**（預設有）。glslc 會自動處理。

4. **Push constant 大小一致性**
   Vulkan pipeline layout 的 push constant range size 必須 ≥ shader
   宣告的 block 大小，且 stage flags 要對。若 host 寫 16 bytes 但
   shader 宣告 12 bytes，validation layer 會 warning（不 crash），但
   driver 行為 undefined。對齊到 16 B 最安全。

5. **`vkQueueWaitIdle` 的 perf impact**
   GLES path 整套 ME+warp 在 Pixel 5 約 4-6 ms；Vulkan 不應該更慢，
   但 `vkQueueWaitIdle` 強迫等所有 GPU 工作完成才能回 CPU，**§I.C.4
   先用 it**，§I.C.5 baseline 對比可能要改成 `VkFence`。

6. **GLES path 的 `eglSwapInterval(1)` ↔ Vulkan 的 present mode**
   GLES 強制每 swap 等 vsync；Vulkan FIFO present mode 行為等價。已在
   §I.B 確認 Pixel 5 用 FIFO（`vk_backend.c` log: `picked present mode
   = FIFO`）。

7. **「每 phase baseline 對比」鐵律 (§I 鐵律 #2)**
   §I.C.5 必須跑 `scripts/benchmark/android/android_baseline.sh` 兩次
   （GLES + Vulkan）並比對。Vulkan FRUC fps / latency 不能比 GLES 退步。

---

## 9. §I.C.1 完成判準

- [x] 全部 5 個 GLSL 都讀完並列出 binding / uniform / size
- [x] 列出 GLSL 310 ES → 450 差異點清單
- [x] 列出 storage image / sampled image 規劃
- [x] 列出每 frame 的 command buffer 結構草案
- [x] 列出 5 個風險點 + mitigations

**下一步：** §I.C.2 — 把 `motionest_compute.glsl` 翻成 GLSL 450 + glslc
產 `.spv` + xxd 包成 `.spv.h`（**只進 build、不接 pipeline**）。
