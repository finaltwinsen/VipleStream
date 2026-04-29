# §J.3.e.2.i — VkFrucRenderer (Android architecture port)

## 緣由

§J.3.e.2.e1b ~ §J.3.e.2.h 的 PC 路線一直撞 libplacebo 對 wrapped pl_tex
的內部 sync 黑箱（v1.3.107 / 111 / 116 三輪測試 frame#2+ 都 stall），
g.A/B/C 三條 application-side 對策都無法繞開。

`moonlight-android/app/src/main/jni/moonlight-core/vk_backend.c` 是
**4117 行已 production 跑的 native Vulkan FRUC 實作**：

  • 自己 `vkCreateSwapchainKHR` + `vkAcquireNextImageKHR` + `vkQueuePresentKHR`
  • 完全不依賴 libplacebo
  • Per-slot acquire + renderDone 雙 sem（dual-present 用）
  • 單 `vkQueueSubmit` 帶 2 組 sem，2× `vkQueuePresentKHR`，正確同步
  • VK_GOOGLE_display_timing 排程 interp / real 的顯示時間

這證實 Vulkan FRUC + dual-present 是**做得起來的設計**，PC 卡的不是
演算法或 Vulkan 限制，而是 libplacebo wrapper 那層 sync 黑箱。

§J.3.e.2.i 把 Android 架構 port 到 PC，跳過 libplacebo wrapper。

## 範圍

新的 PC 端 renderer class **`VkFrucRenderer`**（新檔案 `vkfruc.cpp/h`），
平行於既有 `PlVkRenderer`，**只在 `VIPLE_VK_FRUC_GENERIC=1` opt-in 時
使用**。預設 user 不踩它，沿用 PlVkRenderer / D3D11VARenderer cascade。

VkFrucRenderer 的職責：
1. 自己 build VkInstance + VkDevice（或借 ffmpeg HWAccel 的）
2. 自己 build VkSwapchainKHR、acquire / present
3. ffmpeg AVVkFrame（NV12 from Vulkan video decoder）→ 自家 graphics +
   compute pipeline → swapchain
4. Reuse §J.3.e.2.h.a 已 ship 的 GLSL compute shaders（motionest /
   median / warp）
5. 雙 present pacing — interp frame 在 t+8ms、real frame 在 t+16ms
6. 跟 D3D11+GenericFRUC baseline 比 latency / fps p99

## 既有 vs Android vs 新 PC 三條對照

| 構件 | PlVkRenderer (PC, 現況) | vk_backend.c (Android) | VkFrucRenderer (PC, 新) |
|---|---|---|---|
| Render layer | libplacebo | 直接 Vulkan WSI | 直接 Vulkan WSI |
| Swapchain | libplacebo 管 | `vkCreateSwapchainKHR` | `vkCreateSwapchainKHR` |
| Tone-mapping / HDR | libplacebo 自帶 | 沒做（Pixel 5 SDR 即可）| 不做（先 SDR 為主，§J.3.e.2.j 再加）|
| 解碼幀來源 | AVVkFrame (FFmpeg vulkan) | AHardwareBuffer (MediaCodec) | AVVkFrame (FFmpeg vulkan) |
| Compute (FRUC) | §J.3.e.2.h.c 已有 | vk_backend.c §I.C | **重用 §J.3.e.2.h.c** |
| 雙 present sync | hold/release timeline ❌ stall | per-slot 雙 sem 對 ✅ | **抄 Android per-slot 雙 sem** |
| Frame timing | libplacebo swapchain 隱 | VK_GOOGLE_display_timing | NVIDIA: `VK_KHR_present_wait` + `present_id`，fallback 純 vsync |

## Code reuse 估算

| 來源 | 內容 | 重用率 |
|---|---|---|
| `vk_backend.c` create_instance/device/swapchain | ~200 行 | **80%** — 抽掉 Android 的 Surface + AHB 部分 |
| `vk_backend.c` graphics pipeline + render pass | ~400 行 | **70%** — fragment shader 要小改（PC sRGB swapchain vs Android 的 SRGB+SDR）|
| `vk_backend.c` compute init + dispatch | ~600 行 | **40%** — 已被 §J.3.e.2.h.c 取代，這裡只 port quality 變體切換邏輯 |
| `vk_backend.c` render_ahb_frame | ~500 行 dual-present 邏輯 | **80%** — 把 AHB import 換成 AVVkFrame import 即可 |
| `vk_backend.c` in-flight ring | ~250 行 | **95%** — 直接 port |
| `vk_backend.c` GOOGLE_display_timing | ~200 行 | **20%** — PC 桌機 NVIDIA driver 不支援，要換成 `VK_KHR_present_wait` 或 `VK_NV_present_barrier`，或退回純 vsync 估算 |
| Android 的 GLSL shaders | motionest_compute / warp_compute | **100%** — 已是 §J.3.e.2.h.a port 的 source |

PC 端新 code 估：~1500 行（從 4117 行 Android 抽 portable 部分 +
桌機 differences glue）。

## 切割成可獨立驗證的 sub-phases

### §J.3.e.2.i.1 — VkFrucRenderer 類別 scaffold + cascade hook

  • 新增 `vkfruc.h` / `vkfruc.cpp`，class 繼承 `IFFmpegRenderer`
  • `initialize()` / `prepareDecoderContext()` / `renderFrame()` /
    `getRendererAttributes()` 全部 stub return false / no-op
  • `ffmpeg.cpp::tryInitializeRenderer` cascade 在 PlVkRenderer 之前
    試 VkFrucRenderer（gated `VIPLE_VK_FRUC_GENERIC=1`）；失敗就 fall
    through

成功標準：build 過，cascade 跑下去（VkFrucRenderer 拒絕 → fall back
PlVkRenderer），不破壞既有行為。

### §J.3.e.2.i.2 — Vulkan instance / device / swapchain

  • 從 `vk_backend.c` `create_instance` / `pick_physical_device_and_queue`
    / `create_device` / `create_swapchain` port 過來
  • PC 上 surface 來源是 SDL2 window（用 `SDL_Vulkan_CreateSurface`），
    不是 Android Surface
  • 跟 ffmpeg HWAccel 共用 device — 用 `av_vkfmt_from_pixfmt` /
    `AVVulkanDeviceContext` 拿 ffmpeg 已建好的 VkDevice，避免兩個
    VkDevice 在同一 GPU 上互踩
  • Probe `VK_GOOGLE_display_timing` / `VK_KHR_present_wait` —
    沒就退回 vsync 估算

成功標準：renderer 能 init，吃一張黑色 swapchain image present 出去。

### §J.3.e.2.i.3 — Graphics pipeline (NV12 → swapchain)

  • 一條 graphics pipeline，render pass 1 attachment（swapchain
    image, format 跟 swapchain 一致）
  • Fragment shader 取 AVVkFrame.img[0] 經 `VkSamplerYcbcrConversion`
    sample 成 RGB（BT.709 limited range）
  • Vertex shader 全螢幕 triangle
  • 每 frame：`vkAcquireNextImageKHR` → record cmd buf → submit →
    `vkQueuePresentKHR` → 60 fps stable

成功標準：跟 Setup D 一樣 stable 60s（單 present，無 FRUC）。

進度（v1.3.123 / 即將 ship）：
  • i.3.a — AVHWDeviceContext 建好，bridge 給 ffmpeg Vulkan 解碼器 ✅
  • i.3.b — `VkSamplerYcbcrConversion` + immutable sampler + descriptor
    set layout + pipeline layout ✅
  • i.3.c — render pass + framebuffer per swapchain image + graphics
    pipeline（pre-compiled SPIR-V，xxd 包成 .spv.h；mirrors Android
    `moonlight-core/shaders/` pattern）✅
    - Source `vkfruc-shaders/vkfruc.vert` + `vkfruc.frag`
    - `build_shaders.cmd` 用 Android NDK 內建 glslc.exe 編譯
    - 結果 .spv.h 簽進 git，end-user 不需要重跑
  • i.3.d — per-slot in-flight ring（2 slots × 1 cmdbuf + 2 acquireSem
    + 2 renderDoneSem + 1 fence；signaled-init fence；mirrors
    `vk_backend.c:init_in_flight_ring`）✅
  • i.3.e — `renderFrame()` 完整實作 ✅
    - VkDescriptorPool + per-slot DescriptorSet pre-alloc
    - Render-time PFN cache (m_RtPfn) 避免 hot-path 查表
    - Per-frame 流程：fence wait → destroy slot's pending view →
      vkAcquireNextImageKHR → `lock_frame` → vkCreateImageView (with
      VkSamplerYcbcrConversionInfo) → vkUpdateDescriptorSets →
      cmd record (QFOT acquire barrier + render pass + bind + draw 3) →
      vkQueueSubmit (waits on acquireSem + AVVkFrame.sem@v, signals
      renderDoneSem + AVVkFrame.sem@v+1, signals fence) →
      AVVkFrame state update → `unlock_frame` → vkQueuePresentKHR
    - Cascade hook (`ffmpeg.cpp`) 改成 pass-based：pass=0 →
      VkFrucRenderer，init 失敗 → pass=1 fallback 到 PlVkRenderer
    - `initialize()` 改回 true，VIPLE_VK_FRUC_GENERIC=1 直接走新 renderer

i.3 全部完成，目前狀態：single-present、QFOT 接 ffmpeg Vulkan
decoder、graphics pipeline 在跑。下一步進 i.4（compute）/ i.5
（dual-present）/ i.6（benchmark）。

### §J.3.e.2.i.3.e — runtime status：known-broken（v1.3.123-136）

**Init / setup 全部 OK，但開串流第一個 frame 進 renderFrame 後 crash。**

驗證流程：
```
set VIPLE_USE_VK_DECODER=1
set VIPLE_VK_FRUC_GENERIC=1
VipleStream.exe stream <host> Desktop --1080 --fps 60 --video-codec H.264
```

Crash signature（cdb minidump 分析）：
```
nvoglv64+0x14ca62  mov rax, [rcx+0xF0]  ← rcx=NULL, EXCEPTION_ACCESS_VIOLATION
nvoglv64+0xe02
avutil_60+0xe9e4c (FFmpeg hwcontext_vulkan)
avutil_60+0x6d0c8 / +0x70408 / +0x3e824 / +0x68a89 / +0x604d9
avcodec_62+... (avcodec_send_packet → vulkan h264 decode submit)
VipleStream!FFmpegVideoDecoder::submitDecodeUnit+0x455
```

→ FFmpeg decoder thread 在 submit decode 時，呼叫 NV driver 內 Vulkan
PFN，第一個 arg（`rcx`）是 NULL → driver 內部 deref 0xF0 offset → SEGV.

**已試過的修法（都不解）：**
1. **Persistent feature struct chain** — 把 `VkPhysicalDeviceXxxFeatures`
   從 stack 改成 class member（`m_DevFeat2 / m_YcbcrFeat / m_TimelineFeat
   / m_Sync2Feat`）讓 `device_features.pNext` 不 dangling
2. **`VK_API_VERSION_1_3`** — FFmpeg 文件第 73 行寫 "Must be at least
   version 1.3"，原本是 1.1，改 1.3
3. **同時填新式 `qf[]` + 舊式 `queue_family_*_index`** —
   `FF_API_VULKAN_FIXED_QUEUES`（libavutil 60 還是 1）相容欄位
4. **`vkGetPhysicalDeviceFeatures2` query + enable 全部** — mirror
   libplacebo `*m_Vulkan->features` 全 device feature set

**沒解的可能 root cause：**
- FFmpeg hwcontext_vulkan 期待我們沒提供的某個 device extension
  （e.g. `VK_EXT_external_memory_host`、`VK_KHR_external_memory_*`）
- 我們的 VkInstance 跟 SDL 共用 surface，而 FFmpeg 認為應該獨立 instance
- AVVulkanDeviceContext.alloc 應該指向某個 callback 而不是 NULL
- 其他 internal 狀態 mismatch，要 step into FFmpeg `hwcontext_vulkan.c`
  source 才能定位

**現況的影響：**
- VkFrucRenderer.initialize() 仍 return true，cascade flip 給它，跑串
  流→ crash. **預設 user 不踩它**：`VIPLE_VK_FRUC_GENERIC` 沒設就
  走原本的 PlVkRenderer，無問題。
- 設了 env var → init OK + 第一個 frame crash. cascade pass=1 是
  PlVkRenderer 但因為是 process-level crash，整個 client 死掉，cascade
  retry 沒機會。

**下一步建議：**
- (a) 進 FFmpeg `libavutil/hwcontext_vulkan.c` source 找 PFN 表初始化
  時 dispatch state 是怎麼 build 的，跟我們提供的 `vkCtx` 欄位對得上
  哪些 — 這是嚴肅的 multi-day deep dive
- (b) 改 strategy：放棄手刻 VkInstance/VkDevice，piggyback 在
  PlVkRenderer 的 libplacebo 實例上，只 override renderFrame 路徑做
  我們的 swapchain + present，省掉 hwcontext bridge
- (c) 暫停 §J.3.e.2.i 路線，回到 §J.3.e.2.h（D3D11+GenericFRUC）+
  §J.3.e.2.f benchmark 收尾，單獨 ship

### §J.3.e.2.i.4 — Compute pipeline 接上（重用 §J.3.e.2.h.c）

  • initFrucGenericResources() 從 PlVkRenderer 搬到 VkFrucRenderer
  • Per-frame 在 acquire 後跑 compute chain：bufRGB（從 NV12 forward）
    → ME → Median → Warp → interpRGB
  • 還是單 present（real frame 走 graphics pipeline，interp 計算後
    暫不 present）

成功標準：compute 每 frame 跑、log 裡看到 ME/Median/Warp 計時，real
frame 60 fps stable。

### §J.3.e.2.i.5 — 雙 present 接上（核心目標）

  • 第二條 graphics pipeline 取 interpRGB sample 成 swapchain image
  • Per slot 配 acquire+renderDone 雙 sem（[pass1] + [pass2]）
  • Per-frame：acquire 兩張 swapchain image，single
    `vkQueueSubmit` 帶 2 組 sem 兩個 cmd buf，2× `vkQueuePresentKHR`
  • Frame timing：interp `present_id=N`，real `present_id=N+1`，間隔
    `swapchain refresh_duration_ns / 2`

成功標準：60 秒 0 skip / fence-fail / queue overflow，60+60=120Hz
（在 120Hz panel）或 30+30=60Hz（在 60Hz panel）。

### §J.3.e.2.i.6 — 60s benchmark vs D3D11+GenericFRUC baseline

跟 §J.3.e.2.f Setup A 對等的指標：cumul real / interp、ft_mean、p95、
p99、p99.9、submit / skip / skip_ratio、me_gpu / warp_gpu。

**結果（v1.3.153，2026-04-29）**：

測試環境：H.264 1080p @ 30 source fps，Desktop 串流 65 秒，10 個 5 秒
window steady-state 平均，NVIDIA RTX 3060 Laptop。

| Metric | Baseline (D3D11+GenericFRUC) | VkFruc (SW+FRUC+DUAL) | Δ |
|---|---|---|---|
| Source FPS | ~30.0 | ~30.0 | tie |
| ft_mean | 33.42 ms | 33.32 ms | -0.10 ms |
| p50 | 33.55 ms | 33.35 ms | -0.20 ms |
| p95 | 42.23 ms | **37.26 ms** | **-4.97 ms** ✅ |
| p99 | 55.71 ms | **49.44 ms** | **-6.27 ms** ✅ |
| p99.9 | 63.04 ms | **56.13 ms** | **-6.91 ms** ✅ |
| Skip ratio | 0% | 0% | tie |
| Cumul source frames | 1803 (66s) | 1960 (70s) | similar |

關鍵發現：

1. **VkFruc dual-present frame time 尾巴更緊** — p95/p99/p99.9 比 D3D11
   baseline 低 5–7 ms。Median/mean 約等。
   - 推測原因：
     - 單 cmdbuf 單 submit（vs D3D11 baseline 多次 present call）
     - Software decode 時序穩定，不跟 GPU graphics 搶 cycles
     - Compute chain（NV12→RGB + ME + Median + Warp）共 ~1ms 沒成 bottleneck
2. **跳動感未量化解決** — p99 數據雖然較好，但 dual-present 兩 image
   緊貼（沒 frame pacing），人眼觀感是「閃兩張、空一段」的群聚。i.5
   設計的 frame pacing（interp 排在 real 後 ~8 ms）尚未實作，列入
   future enhancement。
3. **VkFruc 整套 native Vulkan FRUC 架構 viable** — 即使用 software
   decode 也能達到 D3D11+GenericFRUC 同等以上的 frame timing。

未量化項目：
  - GPU timestamp queries（ME/Median/Warp 個別時間，baseline 有
    me_gpu=0.51 ms / median=0.01 ms / warp=0.05 ms = 0.57 ms 總計）
  - Motion estimation quality（MV vector field 是否合理）
  - 跳動 frequency / temporal distribution（要 high-speed camera 或
    `present_id` 時序 trace）

Stats logging 實作：`renderFrameSw` 每 5 秒 emit 一個
`[VIPLE-VKFRUC-Stats]` line，格式對齊 D3D11 的 `[VIPLE-PRESENT-Stats]`
方便 grep + 對比。

## 風險

  • **Probe 結果不確定**：NVIDIA Windows desktop driver 不見得支援
    `VK_GOOGLE_display_timing` 或 `VK_KHR_present_wait`，要靠純 vsync
    估算 — 精度可能不如 Android 上的 PTS-based pacing。Mitigation：
    測完看實際 spike 大小再決定要不要找代替方案
  • **跟 ffmpeg HWAccel 共用 VkDevice 的 queue family 競爭**：ffmpeg
    decode 跟我們 graphics+compute 同 device 上跑，如果 queue family
    不夠分開可能要排隊。NVIDIA 通常多 queue family，Android port
    的設計（一個 graphics+present queue）對 PC NVIDIA 也適用
  • **HDR / 10-bit 不支援**：第一版只做 SDR sRGB。HDR 留 §J.3.e.2.j
  • **既有 PlVkRenderer 仍是 default**：開發新 renderer 不破壞既有行為
    （opt-in），fallback 都還在
  • **ffmpeg AVVkFrame 的 sem 行為**：跟 Android 上 AHB 很不一樣，
    第一次 import 可能會踩到一些細節。Mitigation：先看現有
    `PlVkRenderer::renderFrame` 怎麼 acquire AVVkFrame.sem 的

## 不可動的鐵律

1. **預設 user 行為不變** — `VIPLE_VK_FRUC_GENERIC` 沒設不踩 VkFrucRenderer
2. **PlVkRenderer / D3D11VARenderer 都保留** — VkFrucRenderer 是
   opt-in 額外 path，不取代既有 cascade
3. **3-fail fallback 機制保留** — 沿用 §I.F 既有
4. **Cross-renderer 共用的 GLSL shader 字串**（§J.3.e.2.h.a）
   留在 plvk.cpp 或抽到 shared header — 不重複維護兩份

## Sub-phase 對應 commit / version 估算

| sub-phase | LOC est. | est. commits | est. version |
|---|---|---|---|
| i.1 scaffold | ~200 | 1 | v1.3.117 |
| i.2 instance/device/swapchain | ~600 | 2 | v1.3.118-119 |
| i.3 graphics pipeline | ~400 | 1 | v1.3.120 |
| i.4 compute integration | ~150 | 1 | v1.3.121 |
| i.5 dual present | ~250 | 1-2 | v1.3.122-123 |
| i.6 benchmark | doc | 1 | v1.3.124 |

總計 ~1600 行，~7 commits，預期 ~1 週工作量（Claude 連續推）。

## 命名

`docs/J.3.e.2.i_vulkan_native_renderer.md` — 本文件
`moonlight-qt/app/streaming/video/ffmpeg-renderers/vkfruc.h/cpp` — 新
renderer class（命名沿用 ffmpeg-renderers 目錄慣例）

## §J.3.e.2.i.7 — HW path retry（v1.3.189-190）+ pivot

v1.3.189 attempt 1：把 device extension list 從 9 個（minimal subset）
改成 mirror libplacebo 的 `k_OptionalDeviceExtensions`（plvk.cpp:38），
加 `VK_KHR_PUSH_DESCRIPTOR / VK_EXT_EXTERNAL_MEMORY_HOST /
VK_KHR_EXTERNAL_MEMORY_WIN32 / VK_KHR_EXTERNAL_SEMAPHORE_WIN32`。
做法：query 物理裝置支援哪些 ext，跟 wanted 列表取交集，存 class
member `m_EnabledDevExts`，createLogicalDevice + populateAvHwDeviceCtx
用同一份。

v1.3.190 attempt 2：`getPreferredPixelFormat` HW 模式回 `AV_PIX_FMT_VULKAN`
（之前 fallback 到 base class 的 yuv420p）；`isPixelFormatSupported`
HW 模式只接受 `AV_PIX_FMT_VULKAN`。

**結果：兩個 attempt 都沒讓 HW 真正啟動。**

實測 log（VIPLE_VKFRUC_HW=1, HEVC, RS_VULKAN）：
```
[VIPLE-VKFRUC] §J.3.e.2.i.3.e initialize SUCCESS ... (HW mode: AV_PIX_FMT_VULKAN)
[VIPLE-VKFRUC] §J.3.e.2.i.3.a prepareDecoderContext OK — ffmpeg will decode into AVVkFrame
FFmpeg: [hevc] Format yuv420p chosen by get_format().    ← FFmpeg 還是選了 SW
Test decode SUCCEEDED for format=0x100 hw=0 pixfmt=0
[VIPLE-VKFRUC-SW] VkFrucRenderer SW chosen, skipping HW cascade
```

Root cause 不在 hwctx 的 ext list 或 pixel format signal，而在
**ffmpeg.cpp:1486 cascade trigger**：
```cpp
if (tryInitializeRenderer(decoder, AV_PIX_FMT_YUV420P, ...,
                           []() -> IFFmpegRenderer* { return new VkFrucRenderer(0); })) {
```

`tryInitializeRenderer` 第二個參數 `requiredFormat=AV_PIX_FMT_YUV420P`
強制 SW codec format → FFmpeg 永遠 pick SW codec → 沒走 HW decode.

要走 FFmpeg-Vulkan HW 路線，cascade 要新增一個 slot 用
`AV_PIX_FMT_VULKAN` 為 requiredFormat 並用 HEVC HW decoder。那是
attempt #3 + 整個 cascade restructure。

## §J.3.e.2.i.8 — pivot：自己 VK_KHR_video_decode（跳 FFmpeg）

v1.3.190 之後決定 pivot —— 跳過 FFmpeg-Vulkan wrapper，自己接
VK_KHR_video_decode。

### 動機

- FFmpeg cascade restructure 風險高（影響 D3D11 / Pl / 其他 backend
  的選擇邏輯）
- VK_KHR_video_decode 直接用 driver-level Vulkan API，繞掉 FFmpeg
  hwcontext 的 dispatch table mis-init bug 整套
- VkFruc 已有的 swapchain + compute pipeline + dual-present 都能直接
  在 decoded VkImage 上接

### 範圍

| 子任務 | 預估 LOC | 風險 |
|---|---|---|
| NAL unit reception (intercept moonlight pre-FFmpeg) | ~50 | 低，現成 hook |
| H.264/H.265 SPS/PPS parser (用 h264_stream lib 或 ffmpeg parser) | ~150 | 中 |
| VkVideoSessionKHR + VkVideoSessionParametersKHR | ~250 | 中-高 |
| DPB (Decoded Picture Buffer) management | ~250 | 高（reference frame tracking） |
| Per-frame vkCmdDecodeVideoKHR + barriers | ~150 | 中 |
| Output VkImage → 既有 graphics pipeline | ~50 | 低（已現成） |
| 整合 dual-present + FRUC compute | ~100 | 低 |
| **TOTAL** | **~1000 LOC** | **multi-session** |

### Phase 規劃

- **Phase 1**: H.264 only baseline（H.264 比 H.265 簡單，先跑通流程）
- **Phase 2**: H.265 (HEVC, 跟 stream 主要 codec 對齊)
- **Phase 3**: AV1（FFmpeg 6.1+ 才有 VK_KHR_video_decode_av1）
- **Phase 4**: 整合 dual-present + FRUC compute path

### 關鍵 Vulkan API

```c
// Setup
vkCreateVideoSessionKHR
vkBindVideoSessionMemoryKHR
vkCreateVideoSessionParametersKHR
vkUpdateVideoSessionParametersKHR  // SPS/PPS 變更時

// Per-frame decode (in cmd buffer)
vkCmdBeginVideoCodingKHR
vkCmdControlVideoCodingKHR  // first time after session create
vkCmdDecodeVideoKHR
vkCmdEndVideoCodingKHR
```

### 失敗風險預演

- **NV 596.84 driver 對 VK_KHR_video_decode_h265 支援程度** —
  RTX 30 系列應該支援，要 query VkVideoCapabilitiesKHR 確認
- **Bitstream parsing 邊角案例** — 重排序 frame、B frame、slice mode、
  HEVC 10-bit
- **DPB management bug** — 錯的 reference frame → 視覺壞畫面、不會 crash
  但難 debug
- **Replace FFmpeg 的 connection 流程** — moonlight 既有 NAL hook 點是
  `submitDecodeUnit`（限制：必須在 FFmpeg path 走完前 intercept）

### 動工前必確認

1. NV 596.84 對 VK_KHR_video_decode_h265 + decode H.265 Main 的支援
   (vkGetPhysicalDeviceVideoCapabilitiesKHR query)
2. moonlight-qt 哪一層拿到 raw NAL bytes（看 `submitDecodeUnit` 上游）
3. 既有 h264_stream library import 是否能 reuse for H.264 parsing
   （`#include <h264_stream.h>` 已在 ffmpeg.cpp 第 7 行）

實作工作排在 §J.3.e.2.i.8.* sub-phase，超出單 session 範圍.
