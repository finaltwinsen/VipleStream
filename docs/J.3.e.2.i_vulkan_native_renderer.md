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

---

## §J.3.e.2.i.8 Epilogue — Phase 1.6 / 1.7 系列收網（v1.3.295 ~ v1.3.308）

§J.3.e.2.i.8 Phase 1.x H.265 native 路徑 v1.3.251 ship 之後，**ONLY mode**
（`VIPLE_VKFRUC_NATIVE_DECODE_ONLY=1`，synth-frame Pacer drive）有個一直
無法解的痛點：跑 24~78 秒就會 NVDEC device-lost，validation layer 抓不
到，driver-internal NVDEC engine fault。Phase 1.5c-final（v1.3.295）先
補 graceful degrade（rc=-4 後 set m_DeviceLost flag、後續 render/decode
早 return、停 cascade noise）；接著 Phase 1.6 / 1.7 系列為**真正修 root
cause** 的努力跟最終放棄。

### Phase 1.6 — NVIDIA Nsight Aftermath SDK 整合（v1.3.298, 7cb4fb4）

**動機：** validation layer 看不到 NVDEC engine 內部，需要 driver-side
crash dump 機制。

**做法：**
- `moonlight-qt/3rdparty/aftermath_sdk/` 收 NV Nsight Aftermath SDK 1.6
  （EULA 限制 redistribution，`.gitignore` 排除）
- `vkfruc-aftermath.{h,cpp}` singleton：第一次建 instance 前呼
  `GFSDK_Aftermath_EnableGpuCrashDumps`；callback 把 dump bytes 寫到
  `%TEMP%\VipleStream-aftermath-<ts>.nv-gpudmp`
- `vkfruc.cpp::createLogicalDevice` 偵測 Aftermath active 時 push
  `VK_NV_DEVICE_DIAGNOSTICS_CONFIG` + `VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS`
  ext，把 `VkDeviceDiagnosticsConfigCreateInfoNV`（shader debug info /
  resource tracking / automatic checkpoints）chain 進 `dci.pNext`
- `app.pro` 偵測 SDK 存在才 link，CI / 沒裝 SDK 的開發者 build 仍能跑
- `tools/aftermath_decode/` standalone CLI：呼
  `GFSDK_Aftermath_GpuCrashDump_CreateDecoder` + `GenerateJSON(ALL_INFO)`
  把 `.nv-gpudmp` 轉成可讀 JSON，免裝 Nsight Graphics GUI 也能快速看
  page fault / device state

**第一發 dump（`1777691647.nv-gpudmp` 142932 bytes）解出根因候選：**

```
Device state    : Error_DMA_PageFault
Engine          : Video Decoder (NVDEC)
Access          : Read
Faulting GPU VA : 0x351200200
Resource        : 1D buffer, size 1207568 (~1.15 MB), Destroyed=false
Resources hist  : 180 1D buffer entries，179 個 Destroyed=true，
                  GPU VA 連續滑動 0x350D80000 → 0x351200200，
                  ~10 個 unique handle 在輪替
```

當時假設：parser 內部 ring 自然釋放 bsBuf shared_ptr 後，host vkDestroyBuffer
跟 NVDEC 仍在 deferred 讀 buffer 之間 race。修法目標：延長 bsBuf lifetime。

### Phase 1.7 — 五個變體輪流嘗試，全失敗

| Phase | 改動 | 結果 |
|---|---|---|
| **1.7a** v1.3.299 (5a7c9ff) | hold 上一幀 bsBuf shared_ptr，next submit 入口 reset | **4 frames 必死** ── 不是好轉，是嚴重 regression |
| **1.7b A** stash drop | hold-forever ring N=16，永遠不主動 destroy | **4 frames 必死** |
| **1.7c** v1.3.301 (fdbbf8c) | 加 `HOST_BIT/HOST_WRITE_BIT` → `VIDEO_DECODE_BIT_KHR/VIDEO_DECODE_READ_BIT_KHR` buffer memory barrier 在 vkCmdDecodeVideoKHR 之前；抄自 NV vk_video_samples 的 reference pattern | **4 frames 必死**，dump pattern 變 (17→72 KB resource list) 但 device-lost 仍發生 |
| **1.7d** stash drop | pool of N=16 pre-allocated VkBuffer + grow on demand；抄自 vk_video_samples `m_decodeFramesData.GetBitstreamBuffersQueue()` | **4 frames 必死** |
| **1.7e** v1.3.302 (029937c) | 上面四個都不修，rename `VIPLE_VKFRUC_NATIVE_DECODE_ONLY` → `*_DANGEROUS` 強迫舊 `setx` 失效，預設 PARALLEL mode | ✅ PARALLEL mode 真穩定 |

**關鍵觀察：** v1.3.298 (per-frame Create+Destroy, no barrier) 是已知最久
撐到 24-78s；任何「客戶端延長 bsBuf lifetime」的嘗試都讓 device-lost 從
「24~78s 偶發」變「4 frames 必發」。Pool reuse 加 barrier ── 抄完整
vk_video_samples pattern ── 也壞。

**結論：** 不是客戶端 use-after-free 也不是 missing barrier，是 NV driver
596.36 對 native VK_KHR_video_decode + ONLY mode 的內部 NVDEC engine 有
結構性 bug，從應用層繞不過。Phase 1.7c 的 buffer barrier 是 spec-correct
的補強，留在 source；Phase 1.7e 的 env rename 是 user-protection。

### Phase 1.7c stale-binary 事故（v1.3.299 ~ v1.3.306）

`scripts/build_moonlight_package.cmd` 的 staging step 之前**沒**檢查
errorlevel ── 如果 zombie `VipleStream.exe` process 卡住 file lock：

- `rmdir /s /q "%TEMP_DIR%"` 無聲失敗
- `mkdir "%TEMP_DIR%"` 失敗（dir 已在）
- `copy /y "%RELDIR%\VipleStream.exe" "%TEMP_DIR%\"` 無聲失敗
- 但 build 仍繼續打 zip（zip source 是 `%TEMP_DIR%\*`）

結果：v1.3.299 ~ v1.3.306 連續 8 個 release zip 內含的全是 2026-05-02
11:05:11 那次 build 的**同一份** `VipleStream.exe`，只有 metadata 版號
不同。Phase 1.7c barrier、1.7e env rename、v1.3.303 ycbcr ×4 等的「smoke
pass」/「stream test 95s 0 dumps」全跑同一個過時 binary，沒驗到任何源碼
改動。事後 reboot 清 zombie + 加 errorlevel guard 後（v1.3.307 5183cee）
才真正驗到 Phase 1.7 系列源碼改動的真實行為。教訓進「不可動的鐵律 #5」。

### AMD Vega 10 ycbcr descriptor pool sizing（v1.3.303~307）

VipleStream 在 AMD Vega 10 (Ryzen iGPU) 走到 `createDescriptorPool()` 的
`vkAllocateDescriptorSets` 就 `VK_ERROR_OUT_OF_POOL_MEMORY`，整個
`VkFrucRenderer init failed`。原因：AMD driver 對 ycbcr immutable sampler
在 NV12 (2 plane) 下視為多個 pool descriptor，我們之前 `poolSize.descriptorCount =
kFrucFramesInFlight = 2` 不夠，第二個 set alloc 失敗。NV driver 對 ycbcr
寬鬆計 1 個，所以開發機從沒撞到。

修法（v1.3.307）：動態查 `VkSamplerYcbcrConversionImageFormatProperties::
combinedImageSamplerDescriptorCount` via `vkGetPhysicalDeviceImageFormat
Properties2` (1.1 core) + `*KHR` 後綴 fallback；pool size = `kFrucFramesInFlight
× max(reported, 16)`。即使 driver 報 0 / PFN resolve 不到也保 ×16
over-provision。NV 端 query 報 1 → effective 16；AMD 端 query 報的若 ≤ 16
都 cover 得到。

### Vulkan 降級為實驗性（v1.3.308, 2a892e7）

考量 (a) NV 596.36 ONLY mode bug 無解、(b) AMD 整合顯卡邊界情況、(c)
PARALLEL+SW upload 路徑 80+ ms/frame perf 限制、(d) 上游 D3D11+DXVA
是穩定多年的路徑且 FRUC backend 選擇更廣，把 `RendererSelection` 預設
從 `RS_VULKAN` 改 `RS_D3D11`，GUI dropdown 把 D3D11 放第一、Vulkan 標
[實驗性] + ToolTip 詳述已知問題。enum 值不動（`RS_VULKAN=0 / RS_D3D11=1`）
保 QSettings backwards-compat ── 既有 user 之前 manual 選過 Vulkan 的設定
不被改動，仍依個人選擇。

### 後續還活著的觸發條件

- NV driver 升級（596.36 → 597.x+）後 retest ONLY mode device-lost
- vk_video_samples reference client binary 拿到、在同 driver / 同硬體跑：
  - 如果 NV sample 也 24~78s 必死 → 確認 driver bug，等 fix
  - 如果 NV sample 穩定 → 我們 client 的 sync pattern 還有 diff 沒抄到
- Phase 3d.6 AV1 grey 同樣等 RenderDoc + NV sample diff

---

## §B-NVOF — VK_NV_optical_flow HW 整合（commits b9da1cc → 8daf9e2, 2026-05-06）

§B2 TRIPLE 收尾後驗證發現 GenericFRUC 補幀視覺品質受 block-matching ME
精度上限影響：testufo 小快速物體在 disocclusion 邊界 ME 噪聲 → warp
ghost；user 主觀 60→180 vs 60fps 看不出明顯 smoothness 提升。新增 HW
optical flow path（NVIDIA Optical Flow SDK 5.0.7 Vulkan API）取代
block-matching ME stage，gated by `VIPLE_VKFRUC_NV_OF=1`。

### 架構

```
graphics queue                                      OF queue
─────────────                                       ────────
[main cmd buf]
  vkCmdCopyImage vkf->img[0] → m_NvOfInputCurr
  Stage 0  NV12→RGB
  Stage 3  warp (consumes m_FrucMvFilteredBuf)
  Stage 4  curr_rgb → prev_rgb copy
  ...
[vkQueueSubmit graphics]
[empty marker submit signal V_in]   ───signal──→    [SDK auto-submit]
                                                    nvOFExecuteVk waits V_in
                                                    reads (curr, prev) NV12
                                                    writes m_NvOfFlowImage
                                                    signals V_out
[CPU vkWaitSemaphores V_out]   ←──signal────       (~3-5 ms 1080p)
[swap m_NvOfInputCurr ↔ Prev for next frame]

frame N+1:
[main cmd buf]
  vkCmdCopyImage vkf->img[0] → m_NvOfInputCurr (= old prev image)
  Stage 0
  ★ if (m_NvOfTimelineValue > 0):
      vkCmdCopyImageToBuffer m_NvOfFlowImage → m_NvOfFlowStaging
      dispatch NvOFConvert compute (SFIXED5→Q1, 2x2 avg) → m_FrucMvFilteredBuf
    else (frame 0 fallback):
      Stage 1 ME (block-matching) + Stage 2 Median → m_FrucMvFilteredBuf
  Stage 3 warp 不知道 MV 來源
```

### 關鍵實作點

- **`VkPhysicalDeviceOpticalFlowFeaturesNV.opticalFlow=VK_TRUE` 必須在
  device create 時 pNext 鏈內** — 否則 `nvOFInit` 回 status=3
  (DEVICE_DOES_NOT_EXIST)。query+enable 全套 features 透過
  `vkGetPhysicalDeviceFeatures2`，OF feature 條件式塞進 chain 只在
  `m_OpticalFlowQueueFamily != UINT32_MAX` 時。
- **OF queue family probe** 用 bit `VK_QUEUE_OPTICAL_FLOW_BIT_NV =
  0x00000100`，NV Ampere RTX 3060 Laptop 暴露在 QF=5 count=1。
- **`nvofapi64.dll` runtime load**：NV driver shipped 在
  `C:\Windows\System32`，repo 不需新增 binary blob，只 SDK 5.0.7 的
  `nvOpticalFlowVulkan.h` 進 `libs/windows/nvofa/include/`。
- **3 個 input/output VkImages**：
  - `m_NvOfInputCurr` / `m_NvOfInputPrev`：`VK_FORMAT_G8_B8R8_2PLANE_420
    _UNORM` (NV12), full source size, layout 永遠 GENERAL（first use
    UNDEFINED→GENERAL，後續 stays），usage `TRANSFER_DST`。
  - `m_NvOfFlowImage`：`VK_FORMAT_R16G16_S10_5_NV` (Q10.5 fixed-point)
    flow grid 1/2/4 pixels per vec（我們選 grid=4），1080p flow
    dimensions 480×270，usage `TRANSFER_SRC`。
  - 三者 register 為 `NvOFGPUBufferHandle` via `nvOFRegisterResourceVk`.
- **跨 queue sync** 用單一 timeline semaphore + counter (`m_NvOfTimelineSem`
  + `m_NvOfTimelineValue`)。每幀 V_in/V_out 配對；CPU 在 swap 前
  `vkWaitSemaphores` 等 V_out 確保 OF 完成 reading 兩個 input image，
  swap 後 next frame 寫入舊 prev image (= 新 curr) 不會 race。
- **1-frame async lag**：本幀 chain 用上一幀 OF result。第 0 幀 fallback
  到 block-matching（`m_NvOfTimelineValue == 0`）穩定啟動。
- **SFIXED5→Q1 format converter compute shader** (`kVkFrucNvOfConvertShaderGlsl`
  in plvk.cpp)：讀 staging buffer，2×2 average + sign-extend 16 →
  divide-by-16 (= SFIXED5/2 in Q-units = Q1 in Q-units)，寫 Q1 int2 到
  既有 `m_FrucMvFilteredBuf`。warp shader 完全不用改。

### Quality benchmark (testufo 1080p60, 60s capture)

| Mode | SSIM | OF_30Hz | OF_cv | block_outlier |
|---|---|---|---|---|
| baseline_v3 (no median) | 0.89 | 5.56% | 1.62 | 4.91% |
| algo_a_only (median + (d) fix) | 0.88 | 4.43% | 1.55 | 4.7% |
| **algo_d_real_crgb (production)** | **0.998** | **2.75%** | 1.94 | 4.3% |
| **algo_e_nvof_v2 (HW OF)** | **0.999** | 6.61% | **0.796** | 4.7% |

- **SSIM 0.999** vs production 0.998：互角，frame-to-frame 連續性兩者
  都接近完美。
- **OF_cv 0.796** vs production 1.94：HW OF **明顯贏**（motion 軌跡
  smoothness 大幅提升，frame-to-frame 變異小一半）。
- **OF_30Hz 6.61%** vs production 2.75%：HW OF **輸**。30Hz alternation
  比 production 高，推測因 SFIXED5(1/32 px) → Q1(1/2 px) 量化時 4× 精
  度損失，sub-pixel snap 造成 alternate frame MV 跳動。
- **block_outlier 4.7%** ≈ production 4.3%：兩者都極低，沒有 outlier
  blocks 干擾。

主觀視覺（user 直接切換 nvof vs production）：「都還不錯，細節好像不
太一樣，說不出來」— 兩條 path 都達 production 可用品質，無明顯主觀
差異。

### env var
- `VIPLE_VKFRUC_NV_OF=1` 啟用整條 HW OF chain（extension + queue + 
  session + chain integration）。預設 OFF（chain 走 block-match Stage
  1+2）。
- 失敗 fallback 完整保留：DLL load / session create / image alloc /
  register / pipeline build 任一步失敗 → log warn + chain 自動退回
  block-matching。沒有 fatal path。

### 已知限制 / 未來改進

1. **SFIXED5→Q1 精度損失** — 4× 量化造成 OF_30Hz 升高。修法是把
   `m_FrucMvFilteredBuf` 從 Q1 (1/2 px) 升到 Q5 或更高精度，warp shader
   `loadMVRaw * 0.5` 改 `* (1/16.0)` 等對齊。要動 warp shader 跟 buffer
   format，留下次。
2. **CPU `vkWaitSemaphores` 阻塞 ~3-5 ms/frame** — Phase 4b 的 PoC 設計
   選了同步等 OF 完成；重構成完整 1-frame async lag (no CPU block) 等
   未來迭代。1080p60 預算 16.7 ms/frame，現況 5 ms 占用不超出但無法
   上 4K120。
3. **`perfLevel=MEDIUM` hardcoded** — 可加 env var
   `VIPLE_VKFRUC_NV_OF_PERF=slow|medium|fast` 讓 user 試 SLOW 看是否
   改善 OF_30Hz（trade-off：SLOW 耗時更多）。
4. **Output grid hardcoded=4** — 可動態查 `nvOFGetCaps` 看 device 支援
   grid=1（最高精度），1080p flow 變 1920×1080 = 8 MB/frame staging。
5. **Linux/AMD fallback** — `VK_NV_optical_flow` 僅 NVIDIA Ampere+。
   非 NV 機器 / 舊卡自動走 block-match 不受影響。Linux native nvofapi
   path 已 stub，等 §B-NVOF Linux ship 才實作 dlopen
   (`libnvidia-opticalflow.so.1`).

