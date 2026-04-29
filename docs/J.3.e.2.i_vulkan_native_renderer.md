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
