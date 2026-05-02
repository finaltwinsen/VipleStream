# §J.3.e.2 — NCNN-Vulkan 整合進 PlVkRenderer renderFrame

**Workstream:** §J 桌面 Vulkan-native client → §J.3.e NCNN-Vulkan 整合進 PlVkRenderer
**Status:** Design 階段（v1.3.72+）
**Predecessors:**
- v1.3.63 (36e046f) §J.3.e.0 — AVVkFrame access probe（在 renderFrame 入口確認 img[0] 有效）
- v1.3.65-71 §J.3.e.1.a-d — ncnn external VkInstance/VkDevice API + PlVkRenderer handoff
**Successors:** §J.3.e.3+ — frame pacing / RIFE 輸出 → libplacebo present / 多解析度

---

## 已就位的零件（§J.3.e.0/1 累積）

1. **AVVkFrame.img[0] 拿得到** — `PlVkRenderer::renderFrame` 入口已驗證
   `frame->format == AV_PIX_FMT_VULKAN` + `frame->data[0]` cast 成 `AVVkFrame*`
   後 img[0] 是有效 VkImage handle。layout = `VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR`
   (1000024002)，sw_format = NV12 (190)。
2. **ncnn 跟 libplacebo 共用 VkDevice** — `VIPLE_PLVK_NCNN_HANDOFF=1` 時
   `initializeNcnnExternalHandoff()` 確認 `ncnn::get_gpu_device(0)->vkdevice() ==
   m_Vulkan->device`。ncnn 後續 vkCreate* 全在 libplacebo 的 device 上。
3. **Queue family 資訊** — `m_Vulkan->queue_compute.{index,count}` /
   `queue_graphics` / `queue_transfer` 給 ncnn external API；libplacebo
   也 expose `queue_family_decode_index`（通過 AVVulkanDeviceContext）。
4. **Locking 機制** — `pl_vulkan_t::lock_queue / unlock_queue` 是 libplacebo
   內部 sync 的鑰匙；任何用 m_Vulkan->device 上 queue submit 都要跨進去。

## §J.3.e.2 整體 architecture

```
                ┌──────────────────── m_Vulkan->device (libplacebo) ──────────────┐
                │                                                                  │
[ffmpeg decoder]─→ AVVkFrame.img[0]            ┌── ncnn external instance ──┐    │
[VK_QUEUE_VIDEO_DECODE]    │                    │ (claimed VIPLE_PLVK_NCNN_  │    │
   layout=VIDEO_DECODE_DPB │                    │  HANDOFF=1 path)           │    │
                           ▼                    │                            │    │
                    ┌──────────────┐            │ ┌────────────────────────┐ │    │
                    │ §J.3.e.2.b   │ src        │ │ ncnn::Net (RIFE)       │ │    │
                    │ Layout xition│ (NV12)     │ │ - load_param/load_model│ │    │
                    │ DPB → SHADER │            │ │ - extractor on m_Vulkan│ │    │
                    │_READ_OPTIMAL │            │ │   ->device queue_compute│ │    │
                    └──────┬───────┘            │ └─────────┬──────────────┘ │    │
                           │                    │           │                │    │
                           ▼                    │           ▲                │    │
            ┌─────────────────────────────┐     │   in0/in1/in2 ncnn::VkMat │    │
            │ §J.3.e.2.c                  │     │   (planar fp32 RGB)        │    │
            │ NV12 → planar fp32 RGB shader│────→│                            │    │
            │ (compute, on queue_compute)  │     │                            │    │
            └─────────────────────────────┘     │           │                │    │
                                                │           ▼                │    │
                                                │   out0 ncnn::VkMat         │    │
                                                │   (planar fp32 RGB,        │    │
                                                │    interp midpoint)        │    │
                                                └───────────┬────────────────┘    │
                                                            │                     │
                                                            ▼                     │
                                              ┌─────────────────────────┐         │
                                              │ §J.3.e.2.e              │         │
                                              │ planar RGB → output 給  │         │
                                              │ libplacebo present      │         │
                                              │ (pl_tex_create from     │         │
                                              │  VkImage 或 wrap NV12)  │         │
                                              └────────────┬────────────┘         │
                                                           │                      │
                                                           ▼                      │
                                              [pl_render_image → swapchain present]│
                └──────────────────────────────────────────────────────────────────┘
```

---

## 重要 architectural 抉擇

### A. Queue ownership transfer（VIDEO_DECODE → COMPUTE）

ffmpeg 的 hwcontext_vulkan 解碼完一張 frame 後，img[0] 留在
`queue_family_decode_index` 上，layout 是 `VIDEO_DECODE_DPB_KHR`。Vulkan spec
規定 image 要被另一個 queue family 用，**通常**必須 issue **queue ownership transfer
barrier**（一對 release-on-source / acquire-on-destination）。

實作上有兩條路：

**A1.** 走 **`VK_SHARING_MODE_CONCURRENT`** 跨 decode + compute family（簡單、
可能有效能 penalty，但 frame size 中等下無感）。需要 ffmpeg 創 VkImage 時就
列舉兩個 family — 我們不控 ffmpeg 的 create flow。

**A2.** 走 **正式的 ownership transfer barrier** — 兩條 command buffer，一條
在 decode queue（release ownership），一條在 compute queue（acquire ownership）。
ncnn 的 `VkCompute` API 不直接支援，但底層可用 `vkCmdPipelineBarrier2` 自己跑。

**A3 (實測選用，§J.3.e.2.a 驗證).** `VkImageMemoryBarrier` 用
**`srcQueueFamilyIndex = dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED`** —
不做 ownership transfer，靠 AVVkFrame.sem[0] timeline semaphore 跨 queue family
sync。ffmpeg's hwcontext_vulkan 顯然把 AVVkFrame 創建為 sharing flags 允許跨
family 使用（A1 變體），**NV 596.84 driver 直接吃**，§J.3.e.2.a probe 連續 8 次
PASS、無 validation error、無 crash。這條路最簡單、code 最少、效能最好（沒額外
release/acquire submit 開銷）。

**結論：A3 — VK_QUEUE_FAMILY_IGNORED + timeline semaphore sync。** §J.3.e.2.b/c
照此走，不需要 cross-queue-family release/acquire 機制。Mitigation：若未來 driver
update 或不同 vendor 抓到 validation error，再回 A2。

### B. NV12 multi-plane 處理

AVVkFrame.img[0] 是 single VkImage 但 multi-planar (Y + UV interleaved)。
取 sample 的方式：

- **B1.** `VkImageView` 對 plane 0 (`VK_IMAGE_ASPECT_PLANE_0_BIT`) 跟 plane 1
  (`VK_IMAGE_ASPECT_PLANE_1_BIT`)，shader 內各取 sample 然後做 YUV→RGB 轉換。
- **B2.** 創建 `VkSamplerYcbcrConversion`，shader 用 `samplerYCbCr` GLSL extension
  自動處理 conversion。乾淨但要 `VK_KHR_sampler_ycbcr_conversion` device extension —
  ncnn external 模式 device 是否 enable 此 ext 不確定（libplacebo 通常 enable）。

**選 B1** — 顯式 plane access，shader 自己做 limited-range BT.709 → linear sRGB
conversion，control 完整，跨 driver 可預測。後續若要 HDR 再考慮 B2。

### C. RIFE forward 餵料

ncnn::Net 的 input layer 是 in0/in1/in2 = prev RGB / curr RGB / timestep。
`net.forward()` 接受 `ncnn::Mat` (CPU) 或 `ncnn::VkMat` (GPU)；有 `ncnn::Extractor`
封裝。GPU path：

```cpp
ncnn::Extractor ex = net.create_extractor();
ex.set_vulkan_compute(true);
ex.input("in0", prevRgbVkMat);   // ncnn::VkMat
ex.input("in1", currRgbVkMat);
ex.input("in2", timestepVkMat);  // 常量 0.5 預先填好
ncnn::VkMat output;
ex.extract("out0", output, cmd);  // cmd = ncnn::VkCompute
cmd.submit_and_wait();
```

NcnnFRUC 已經有 RIFE model 載入 + Extractor 配 (在 D3D11VARenderer cascade)。
搬過來時要保留：
- 模型檔路徑 `rife-v4.25-lite/flownet.{param,bin}`
- prev frame 緩存（第一張 frame 沒有 prev → skip interp，只 present real frame）
- timestep 預先 fill 0.5 的 `ncnn::VkMat` (W,H,1)

### D. 輸出回 libplacebo present

RIFE 輸出 ncnn::VkMat (planar fp32 RGB, layout=GENERAL)。要走 libplacebo
swapchain present，最直接：

**D1.** 把 VkMat 的 underlying VkBuffer 包成 `pl_tex` 透過 `pl_vulkan_wrap`，
然後 `pl_render_image(renderer, &mappedFrame, &targetFrame, ...)`。但
`pl_vulkan_wrap` 吃 VkImage，VkMat 是 buffer-storage — 要先一個 fp32 buffer →
RGB VkImage 的 reverse-converter compute shader (§J.3.e.2.d)。

**D2.** 直接寫一個 Vulkan compute shader 把 fp32 RGB VkBuffer 寫進
swapchain VkImage（bypass pl_render_image）。簡單但 lose libplacebo 的 HDR /
overlay / scaling 能力。

**選 D1** — 多寫 ~80 LOC reverse converter，但保留 libplacebo 處理輸出
pipeline。等 §J.3.e.3 frame pacing 穩定再考慮 D2 fast path。

### E. Frame pacing — interp at midpoint

stream 30fps→render 60fps。每收到 frame N 排程：
- T=N/30: present real frame N
- T=N/30 + 1/60: present interp(N-1, N) midpoint

第一張沒 prev 跳 interp。前置 condition：RIFE forward latency 必須 < 16.67ms (60fps
半 budget) — 720p RTX 3060 實測 ~14ms ([VIPLE-FRUC-NCNN] probe @ 1080x720 median=11.84ms)
夠 budget。1080p 預估 25-30ms，超 budget — `pl_renderer_create` 的 `frame_mixer`
要 fallback 不開 interp，或降到 GenericFRUC。

§J.3.e.2 先**不**做 frame pacing — 把每 real frame 直接過 RIFE forward 然後
present，看 latency 是否 stable。pacing 留 §J.3.e.3。

---

## Sub-commit 規劃

| Commit | 內容 | LOC | 風險 |
|---|---|---|---|
| **§J.3.e.2.a** Design doc + AVVkFrame layout transition probe | docs/J.3.e.2_*.md + plvk.cpp 加 transitionProbe() 在 renderFrame 跑 VIDEO_DECODE_DPB → SHADER_READ_OPTIMAL，readback 1 pixel 驗 access works，gated VIPLE_VK_FRUC_PROBE2=1 | ~150 | 中 — queue ownership transfer 第一次 |
| **§J.3.e.2.b** NV12 plane VkImageView + sampler 探測 | 加 helper 創 plane-0 / plane-1 VkImageView，1 個 dispatch 把 NV12 中心點轉 RGB log 出來。gated VIPLE_VK_FRUC_PROBE3=1 | ~120 | 中 — multi-planar VkImageView 第一次 |
| **§J.3.e.2.c** NV12→planar fp32 RGB compute shader | 寫 GLSL .comp + 整合進 build (qmake glslangValidator)；輸出 ncnn-compatible VkMat layout (W,H,3) | ~200 | 中-高 — shader correctness |
| **§J.3.e.2.d** Reverse converter (planar fp32 RGB → RGB VkImage) | 第二顆 compute shader，輸出 swapchain-friendly VkImage 給 libplacebo wrap | ~150 | 中 |
| **§J.3.e.2.e** Wire RIFE forward 進 renderFrame + libplacebo present | 把 NcnnFRUC 的 model load + Extractor 搬出來，做成 PlVkRenderer 內部一層；renderFrame 跑 NV12→RGB → forward → RGB→VkImage → pl_render_image | ~250 | 高 — 整條 pipeline 對接 |
| **§J.3.e.2.f** Performance + correctness 驗證 | A/B 拿 reference frame 比對 PSNR，量 latency budget，benchmark RTX 3060 / Intel Arc / AMD RDNA | ~50 | 低 |

**總工：** ~900 LOC，預估 1-2 週

每個 sub-commit 過 regression test (default user 不踩 VIPLE_PLVK_NCNN_HANDOFF / 不踩
VIPLE_USE_VK_DECODER 走 D3D11VA + 既有 NCNN cascade，行為跟 v1.3.71 等價)。

---

## Risk inventory

### R1: Queue ownership transfer barrier 第一次 ship

ncnn 內部從沒做過 cross-queue-family barrier；libplacebo lock_queue 機制怎麼跟
ncnn 的 `acquire_queue` / `reclaim_queue` 互動沒驗。

**Mitigation:** §J.3.e.2.a 先做 layout transition + 1-pixel readback probe，
獨立於 ncnn — 直接用 `vkCmdPipelineBarrier2` 跑。確認 NV / Intel / AMD 都吃。

### R2: ncnn::VkMat layout 跟 GLSL shader 對齊

ncnn 的 fp32 RGB VkMat 內部 layout 是 packed (W*H*3 floats, channel-last 還是
channel-first?) — 沒文件確認。要透過 NcnnFRUC 既有的 phase-B.4a 互轉 shader
參考。

**Mitigation:** §J.3.e.2.c 寫 shader 時直接看 ncnn::VkMat::buffer_offset() 跟
`elemsize` 反推；可以用既有 selftest pattern (4×4 known input) 驗 round-trip。

### R3: RIFE 第一張 frame skip interp

renderFrame 第一張 AVFrame 沒 prev RGB — RIFE forward 不能跑（要兩個 input frame
才能 interp midpoint）。第一張 + skip interp 的 control flow 要正確。

**Mitigation:** PlVkRenderer 加 `m_HasPrevRgb` flag + `m_PrevRgbVkMat` member；
第一張只做 NV12→RGB 存進 m_PrevRgbVkMat，不 forward；第二張開始才 interp。

### R4: 解析度切換 / format 變動 invalidate VkMat 跟 shader pipeline

mid-stream 解析度切換（720p → 1080p）時，VkMat 跟 ncnn::Pipeline 的 specialization
constants (W, H) 失效要重建。

**Mitigation:** PlVkRenderer 已有 frame size 監控（`m_LastColorspace` 變動處理），
延伸做 size invalidation；§J.3.e.2.e 收尾時加。

### R-impl-2: ncnn::Net::load_model 在 libplacebo external VkDevice 上 crash（§J.3.e.2.e2a 實測）

把 RIFE 4.25-lite 在 PlVkRenderer 的 §J.3.e.1.d external ncnn instance 上
load 時 — `ncnn::Net::load_model(binFp)` 內部 0xC0000005 access violation。
同樣 model + 同樣 fp16/fp32 opt 在 NcnnFRUC 的 internal ncnn instance（自家
vkCreateDevice）上 load 完全正常。差別只在 VkDevice 是誰建的。

懷疑原因：ncnn::Pipeline::create per-layer 載 RIFE shader 時假設某些 device
extension 已 enable（VK_KHR_shader_float16_int8 / VK_KHR_16bit_storage 等），
這些在 ncnn 自己的 vkCreateDevice 路徑會根據 GpuInfo support 自動 enable，
但 libplacebo 的 pl_vulkan_create 可能沒打開所有 ext，造成 ncnn 編出來的 SPIR-V
跟 device 實際 enabled feature set 不對齊。

`GpuInfo::support_fp16_*` 是「device capability」flag，不代表「device extension
enabled」。populate_gpu_info_from_external 抄 standard create_gpu_instance 的
loop body，照著 device-level vkEnumerateDeviceExtensionProperties 結果填，但
那是 ENUMERATE，跟 ENABLED 不同概念。

**Mitigation (§J.3.e.2.e2a 短期):** initRifeModel 走完 file 驗證 + custom layer
register + load_param 之後 graceful skip load_model，return false 停在這個
milestone。RIFE 永遠 disable，§J.3.e.2.e1b 的 NV12→RGB→VkImage pass-through
path 還是工作（已 ship）。

**Mitigation (§J.3.e.2.e2b 進度，三條路擇一):**
1. **Patch ncnn fork** 加 per-layer pipeline-create 失敗 log。**已試 (v1.3.88)**：
   patch external VulkanDevice ctor 在 init_device_extension 之後檢查 PFN，
   PFN 沒載到的 support_VK_KHR_xxx flag 強制 mask 為 0，逼 ncnn 走 legacy
   path（避免 NULL PFN deref）。實機 [ext-mask] log 確認 libplacebo 只 enable
   `push_descriptor` (v2) / `sampler_ycbcr_conversion` (v14) / `swapchain` (v70)；
   `bind_memory2` / `create_renderpass2` / `descriptor_update_template` /
   `get_memory_requirements2` / `maintenance1` / `maintenance3` 全 mask=0。
   **load_model 仍然 0xC0000005 crash** — 推測是 §J.3.e.2.c1 同類的
   `ncnn::Pipeline::create()` SPIR-V reflection 對 binding layout 的內部假設，
   不只 PFN 缺失問題。需要 patch ncnn 的 Pipeline::create 本身加防禦
   （NULL check before deref / try-catch）。
2. **強迫 libplacebo enable 額外 extensions**：在 §J.3.e.1.d handoff 之前看
   libplacebo 是否能 expose enabled-extension list，比對 ncnn 預期的清單，
   缺的 ext 走 pl_vulkan_create 的 opt-in 補上。需要 libplacebo API 配合，
   但比 patch ncnn 風險低。**未試** — pl_vulkan_create 接受 opt_extensions
   參數但要列舉 ncnn 全部需要哪些 ext 才能 work，工程量未估。
3. **第二個 ncnn-owned VkDevice** — 接受 cross-device 成本；§J.3.e.1.d
   external mode 的 forward/reverse shader 仍走 libplacebo VkDevice，但 RIFE
   專門開另一個 ncnn::create_gpu_instance() 用自家 device。bufRGB 跟 RIFE input
   之間 vkCmdCopyBuffer 跨 device（要 OPAQUE_WIN32 export/import，回到
   §J.3.e.1 一開始想避免的工程）。最後手段。

§J.3.e.2.e2b 階段性 ship：v1.3.88 ncnn fork PFN mask + v1.3.89 plvk graceful
skip。RIFE 還沒能在 external VkDevice load。下一步繼續路徑 1 (深入 patch
Pipeline::create) 或路徑 2 (libplacebo opt-in)。

**§J.3.e.2.e2c RESOLVED (v1.3.92):** 走路徑 1 加 per-layer trace log 進
ncnn fork 的 net.cpp `Net::load_model` create_pipeline loop。實機跑出來
**load_model 其實 OK** — 全部 389 個 layer (Convolution / Crop / BinaryOp /
Sigmoid / Split / rife.Warp / etc.) PRE+POST trace 都 rc=0 通過。意思是
v1.3.88 的 PFN-mask patch 已經默默 fix 了 load_model 的 crash — 我之前
推測是 §J.3.e.2.c1 同類 Pipeline::create bug 是錯的，**真正的問題就是
PFN unloading**，mask 之後 ncnn 走 legacy code path 完全工作。

剩下的 crash 不在 load_model — 在後面的 timestep VkMat upload 步驟。
我原本用 `ncnn::VkCompute::record_upload` (錯的 class)，crash 在 record_upload
裡面。改抄 NcnnFRUC `phase-B.4b` 的工作 pattern：用 `ncnn::VkTransfer`
（一次性 CPU→GPU upload class，不是 compute pipeline 的 VkCompute）+
顯式 raw `ncnn::Option` (`use_packing_layout=false` 等全關，blob+staging
allocator 都填好) 之後，record_upload + submit_and_wait rc=0 OK。

整條鍊 ship 點 v1.3.92：
  loadRife: step 1/6 new ncnn::Net + opt
  loadRife: step 2/6 set_vulkan_device(0)
  loadRife: step 3/6 register_custom_layer(rife.Warp)
  loadRife: step 4/6 load_param
  loadRife: step 5/6 load_model (per-layer trace OK: 389/389 PASS)
  loadRife: step 6/6 load_model OK
  step 6a-e: VkMat::create prev/curr/timestep OK (blob_allocator)
  step 6f-j: VkTransfer record_upload + submit_and_wait rc=0
  PASS — RIFE 4.25-lite loaded on external ncnn instance, VkMats
  prev/curr=1280x720x3 fp32, timestep=1280x720x1 fp32 (=0.5)

下一步 §J.3.e.2.e2d: per-frame RIFE forward integration — bufRGB →
m_RifeCurrVkMat (vkCmdCopyBuffer) → ncnn::Extractor::extract → outputVkMat
→ bufRGB (vkCmdCopyBuffer) → reverse converter → swapchain。

### §J.3.e.2.e2d 三相結構通了，但 ncnn::VkCompute::record_clone crash (v1.3.97)

把 runFrucOverridePass 拆成三相 (Phase A: NV12→bufRGB + 拷 bufRGB→curr.buffer
+ plane layout 還原；Phase B: ncnn::Extractor extract / first-frame clone；
Phase C: 拷 out→bufRGB + reverse shader)。Phase A、C 各一個我們自家
cmd buffer submit (timeline sem chain V → V+1 → V+2 跟 AVVkFrame.sem)，
Phase B 是 ncnn::VkCompute::submit_and_wait host-blocking。每 phase host-fenced。

實機跑 first frame：
  e2d ENTRY #0 (override=1 nv12rgb=1 rgbimg=1 rife=1 hasPrev=0)
  e2d #0 — Phase A start
  e2d #0 — Phase A done, Phase B start
  e2d #0 — Phase B (first frame): VkCompute ctor ← OK
  e2d #0 — Phase B (first frame): record_clone ← 0xC0000005

`ncnn::VkCompute::record_clone(m_RifeCurrVkMat, m_RifePrevVkMat, ncnn::Option())`
在 ncnn 內部 access violation。輸入 VkMat 都 well-formed (allocated via
`vkdev->acquire_blob_allocator()`, empty()=0, shape=W×H×3 fp32)，default-constructed
Option，跟 NcnnFRUC phase-B.4b 工作 pattern (line 1960) 一樣。

差別找不到，但 workaround：Phase B 跳過 record_clone，outVkMat 直接
alias m_RifeCurrVkMat (real frame pass-through，沒真實 interp)。Phase A→C
鏈接通暢，frame#1/60/120 都 OK，clean kill 25s 後 stream。

實作改動 (v1.3.93-97 一系列 iteration)：
- plvk.h: runFrucOverridePassWithRife 方法宣告
- plvk.cpp:
  - initRifeModel m_RifeNet->opt 加 use_packing_layout=false / shader_pack8=false
  - VkMat::create 從 6-arg (有 explicit elempack=1) 改 5-arg (default
    elempack) 跟 NcnnFRUC pattern 一致
  - runFrucOverridePassWithRife ~280 LOC 三相結構，Phase B 暫 skip
- renderFrame: 加 m_RifeReady gate，Ready 走 RIFE-injected pass，沒 ready 走
  §J.3.e.2.e1b pass-through (回退路)

下一步 §J.3.e.2.e2e: debug record_clone crash — 加 ncnn fork per-step
trace 進 record_clone，找實際 fail 點。或試 ex.extract pattern 跟
record_clone 不同的 RIFE forward 路徑。

### R-impl-1: ncnn::Pipeline::create() 對 binding layout 有限制（§J.3.e.2.c1 實測）

NcnnFRUC Phase B.4a 用 `ncnn::Pipeline + Pipeline::create(spirv, size, specs)` 走得通是
因為 shader 有 ncnn-Mat-style 的 2 binding (in / out) layout。把同樣 API 套到我們
3-binding (Y_in / UV_in / RGB_out) 的 NV12→RGB shader，`pipe->create()` 內部 SPIR-V
reflection 直接 access violation 0xC0000005，沒回 error code、沒 log，整個 process 死。

**Mitigation:** 走 raw Vulkan VkPipeline 路徑 — 用 `ncnn::compile_spirv_module()` 純做
GLSL → SPIR-V 編譯（此函式只用 glslang，跟 ncnn pipeline 無關），然後自己叫
`vkCreateShaderModule` / `vkCreateDescriptorSetLayout` / `vkCreatePipelineLayout` /
`vkCreateComputePipelines` 建管線。多 ~80 LOC，但完全 control + 不被 ncnn 內部
Mat layout 假設 bite。實機 v1.3.77 wire 通：
  raw VkPipeline=0x...CD30 (DSL=... PL=... ShaderMod=...), target 1280x720

**Lesson learned:** 寫非 ncnn-Mat-style shader 一律走 raw Vulkan，不要碰 ncnn::Pipeline。

### R5: ncnn forward 失敗 / driver 拒絕 fall through

ncnn forward 失敗時 PlVkRenderer 不能炸 — 要 fallback 到「直接 present real frame
without interp」。3-fail counter 機制 (v1.3.41) 從 NcnnFRUC 搬過來。

**Mitigation:** §J.3.e.2.e 加 `m_FrucFailCount`，3 次失敗永久 disable 該 path，
跑純 PlVkRenderer 行為。

---

## 不可動的鐵律 (§J.3.e.2 沿用 §J 既有)

1. **預設 user 行為跟 v1.3.71 等價** — VIPLE_PLVK_NCNN_HANDOFF / VIPLE_VK_FRUC_PROBE2
   等 env vars 沒設時，走原本 PlVkRenderer / D3D11VARenderer cascade，不踩到任何
   §J.3.e.2 程式碼。
2. **不要回頭做 §J.1 的 D3D11→Vulkan bridge** — DEAD END (v1.3.47 已記錄)。
   §J.3.e.2 整條 pipeline 純 Vulkan，AVVkFrame 直接餵 ncnn。
3. **3-fail fallback 機制不可移除** — 從 §I.F 沿用，§J.3.e.2.e 把同樣機制套
   到 RIFE forward 失敗 path。
4. **ncnn process-lifetime singleton 不可破** — §J.3.e.1.d 已透過
   teardownNcnnExternalHandoff 在 dtor 走 destroy_gpu_instance，sub-phase 不要
   再加 destroy/recreate cycle。

---

## 立刻可以動的 § J.3.e.2.a — layout transition probe

最小可 ship 的下一步。在 PlVkRenderer::renderFrame 加：

```cpp
if (probe2Enabled() && frame->format == AV_PIX_FMT_VULKAN && (frameCount % 60) == 0) {
    auto* vkFrame = (AVVkFrame*)frame->data[0];
    transitionAndReadback(vkFrame->img[0], vkFrame->layout[0],
                          frame->width, frame->height);
}
```

`transitionAndReadback`:
1. 創 transient VkBuffer (1 byte) for readback
2. Allocate VkCommandBuffer on m_Vulkan->queue_compute
3. lock_queue(decode) → release-ownership barrier on img[0]
4. lock_queue(compute) → acquire-ownership barrier on img[0],
   layout VIDEO_DECODE_DPB → SHADER_READ_OPTIMAL
5. vkCmdCopyImageToBuffer (single texel copy)
6. submit + wait
7. lock_queue(decode) → release-ownership back to decode for next frame
8. log byte readback succeeded

通過後我們就確認 cross-queue-family ownership transfer + layout transition
在 NV / Intel / AMD 都吃，§J.3.e.2.b/c 才有立足點。

---

## 命名一致性

`docs/J.3.e.1_ncnn_external_vkdevice.md` — §J.3.e.1 設計
`docs/J.3.e.2_ncnn_renderframe_integration.md` — 本文件

之後若有 §J.3.e.3 (frame pacing)，新文件 `docs/J.3.e.3_*.md`。

---

## §J.3.e.X — 長期備案：手刻 RIFE Vulkan pipeline（跳過 ncnn）

### 動機

§J.3.e.2.e2e 嘗試把 RIFE 4.25-lite 透過 ncnn::Net 的 VkMat input 路徑
跑在 libplacebo 建出來的外部 VkDevice 上時，撞到 ncnn 自身的 MSVC
ABI 雷區：

- **L1：** `Padding_final : virtual public Padding, virtual public Padding_vulkan, virtual public Padding_x86`
  在 conv 內部 sub-layer (`padding->forward(VkMat,...)`) 經由 `Layer*`
  做 virtual dispatch 時無法正確分派到 `Padding_vulkan::forward`。手動
  `dynamic_cast<Padding_vulkan*>(padding)->Padding_vulkan::forward(...)`
  繞過虛擬分派可進入函式。
- **L2：** 進入 `Padding_vulkan::forward` 後，跑到 `std::vector<vk_constant_type> constants(13)`
  附近又掛掉。深層原因未確認，疑似跨 DLL/EXE 邊界的 vbtable thunk 與
  ncnn 內部 utility allocator 互動。
- **影響面：** RIFE 4.25-lite 有 389 個 layer，幾乎每個 conv 都有 padding
  sub-layer。逐一 patch ncnn 是無底洞工作。

§J.3.e.2 後續正式走 **Path B（CPU ncnn::Mat 中介）**：在 Phase A 寫到
GPU bufRGB 之後，download 到 HOST_VISIBLE staging buffer，wrap 成
`ncnn::Mat` 餵 ex.input，extract 後拷回 GPU bufRGB，再走 Phase C
RGB→VkImage compute。每幀多一份 W×H×3×4 bytes 的 PCIe 來回 (~2ms @ 720p
on PCIe 4.0)，但避開整個外部 VkDevice ncnn ABI 雷區。

### 手刻 pipeline 的時機

當 Path B 跑通且穩定，**未來**若：
- (a) 量測發現 PCIe 來回 + CPU 推論 是 latency 瓶頸（半率 16ms budget 實
  測 30%+ 花在 staging copy）
- (b) 想把 RIFE 推到 1080p / 4K（PCIe 來回 cost scales linearly）
- (c) 想擺脫對 ncnn 整個依賴（DLL size、編譯時間、license
  考量）

可以開新 phase **§J.3.e.X — 手刻 RIFE Vulkan pipeline** 重寫整條
forward path，直接用 raw VkPipeline。

### 手刻 pipeline 的範圍估算

RIFE 4.25-lite 真實計算量（fuse 後）：
- ~50 個 Conv2d（不同 kernel/stride/pad 組合，但都是 fp32 GEMM-like）
- 4-6 個 Resize / PixelShuffle（upsample）
- 4-6 個 Warp（grid_sample 等價，需要 sampler）
- ~20 個 Eltwise add / mul（trivially fused into adjacent shader）
- 1 個 Sigmoid（output）

對應 VkPipeline 數估算 **10~15 個 dispatch**（融合 conv-relu-add-mul 後）。
比 ncnn 的 389 layer 少兩個數量級。

### 實作 sketch

1. **權重轉換**：寫 Python script 把 RIFE 的 `.pth` 轉成 fp16 packed
   binary blob（每個 conv 一段，weight + bias 連續排）。Load 時 mmap
   進 VkBuffer。
2. **Shader 模板**：寫 N 個 GLSL compute shader 模板（conv1x1, conv3x3
   stride1, conv3x3 stride2, conv1x1 GEMM, warp, resize2x, sigmoid+blend）。
   用 specialization constants 控 channel 數/dim。
3. **PipelineCache**：所有 VkPipeline 開機建一次，cache 到 disk（VkPipelineCache
   binary blob），下次啟動直接 load。
4. **Forward 函式**：500~800 行 C++，每個 layer 一個 `recordConvXxx(cb, in,
   out, weights_offset, ...)` helper，串成一條 command buffer。

工作量估：1 週 senior dev (full-time)。風險：
- Shader 正確性驗證（要對著 ncnn CPU output 做 bit-exact / SSIM > 0.99 比對）
- 不同 GPU vendor 的 cooperative-matrix / subgroup 支援差異（先不用 coop-matrix，
  純 SIMD WMMA fp16）

### 與 Path B 的相對位置

| 路線 | latency 預期 (720p 60fps) | 工作量 | 維護成本 |
|---|---|---|---|
| Path B (CPU mat) | ~10ms 推論 + 2ms staging = 12ms | 1~2 天 | 低（依賴 ncnn upstream）|
| §J.3.e.X 手刻 | ~3-5ms (純 GPU) | 5~7 天 | 高（每個 RIFE 版本要重做）|

Path B 是「**現在能用**」的 baseline。§J.3.e.X 是「**要更快才動**」的優化。
不要在 Path B 還沒驗品質前就跳到手刻。

---

## §J.3.e.2.f — Path B benchmark vs D3D11 baseline (v1.3.107)

### Setup

兩條 path 在同一硬體（RTX 3060 mobile / NVIDIA 596.84 driver）跑 720p
60Hz Desktop streaming，同一 LAN streaming host，60 秒 steady-state。

| Setup | 環境變數 | --fruc-backend |
|---|---|---|
| A — D3D11 baseline | （清空 VIPLE_*） | generic |
| B — PlVkRenderer + Path B pass-through | VIPLE_USE_VK_DECODER=1, VIPLE_PLVK_NCNN_HANDOFF=1, VIPLE_VK_FRUC_OUTPUT_OVERRIDE=1, VIPLE_VK_FRUC_RIFE=1 | ncnn |

兩 setup 都 `--fps 60 --frame-interpolation --no-vsync --display-mode
windowed`，跟生產環境一致。

### 結果

**Setup A — D3D11 + GenericFRUC：穩定**

```
real  fps=30.00  ft_mean=33.33ms p50=33.34 p95=34.6 p99=51.8 p99.9=69.6
interp fps=30.00  ft_mean=33.33ms p50=33.33 p95=34.4 p99=52.0 p99.9=69.2
FRUC-Stats: submit=1511 skip=0 skip_ratio=0.0% gapAvg=33ms (expected=33ms)
            me_gpu=0.66ms warp_gpu=0.04ms (total GPU=0.70ms)
cumul real=1511 interp=1510  →  60s 內 3021 frame total
```

跑 60 秒 0 個 dropped frame，p99 ~52ms 表示偶發 vsync spike 但沒有
sustained jank。Generic FRUC 的 ME (motion estimation) GPU cost 0.66ms
非常輕。

**Setup B — PlVkRenderer + Path B pass-through：不穩定**

連跑 3 次都在 frame#2 掛掉：

```
00:00:07 frame#1 OK (first frame)
00:00:08 Phase C fence wait failed   ← ★
00:00:08-onwards: "Waiting for IDR frame" 直到 timeout
```

Phase C 的 `pfnWaitFences` 1 秒超時，代表 GPU 沒在 1 秒內 signal 我們
共用的 `m_FrucOverrideFence`。之前 v1.3.107 build 完直接測一次有跑出
480 frame，現在三次重測都掛 —— 是 **systematic race**，不是隨機現象。

**初步推測：** Phase A 跟 Phase C 共用同一個 `m_FrucOverrideCmdBuf` +
`m_FrucOverrideFence`。Phase A 拿 fence 用 → wait → reset，Phase C 再
拿同一個 fence + reset 同一個 cmd buf。這個 reset+reuse pattern 跟
libplacebo 的 swapchain present sync 排程互相打架（libplacebo 可能還
沒釋放對 AVVkFrame.sem 的 hold，而我們已經要進下一輪），第二 frame 之
後 timeline semaphore 鏈條卡住 → Phase C 永遠等不到 Phase A 的 V+1
signal。

### 結論

1. **Path B pass-through 結構在 cold-start 後跑不穩 —— 不能 ship 成 default**
2. 這是 **Phase A↔C 同步問題**，跟 RIFE forward 無關。所以 §J.3.e.X
   手刻 RIFE 也救不了這個 path（除非順便重設計 cmd buf / fence 拆分）
3. v1.3.107 的價值在於把 Path B **結構打通**並驗證 cold-start 路徑可
   跑（loadRife PASS、staging buffer 配置 OK、Phase A→B→C handshake
   邏輯完整）。但 steady-state 穩定性留待後續 phase 解決

### Next sub-phase 建議：§J.3.e.2.g — Phase A/C 同步重整

不要急著推 §J.3.e.X 手刻 RIFE。先把 §J.3.e.2.g 補進來：

| 子題 | 目的 |
|---|---|
| **g.1** Phase A / Phase C 用獨立 cmd buf | 避開 cmd buf reset/reuse 競賽 |
| **g.2** Phase A / Phase C 用獨立 fence | 同上 |
| **g.3** AVVkFrame.sem 取代 local fence | 走 timeline semaphore 整條，跟 libplacebo 的 sync model 對齊 |
| **g.4** 連跑 60 秒 stable + p99 跟 D3D11 baseline 比 | 驗證修好了 |

§J.3.e.2.g 完成後再評估 §J.3.e.X 是否值得做。如果 Path B 修好但仍然
沒 RIFE，至少 PlVkRenderer override 是實用的 pass-through path（NV12
→ libplacebo render 全程 Vulkan-native，省掉 D3D11 cascade 的 NV12 →
ANGLE → swapchain 路徑），即使沒 frame interp 還是有 latency 改善。

---

## §J.3.e.2.g v1.3.108-111 嘗試與深度發現

### v1.3.108 — Phase A/C cmd buf + fence 分開 (g.1 + g.2)

加 `m_FrucOverridePhaseCCmdBuf` + `m_FrucOverridePhaseCFence`，Phase C
只用這兩個。Phase A 仍用原 `m_FrucOverrideCmdBuf` + `m_FrucOverrideFence`。

**結果：壽命從 frame#2 hang 拉到 frame#240 hang。** 改善 120 倍但
還是會掛。代表「同 cmd buf reset+reuse」是真的雷但**不是唯一雷**。

### v1.3.109 — fence wait timeout 從 1s 拉到 5s

理論：可能只是 GPU jitter under load，1s 太緊。實測：5s 也 timeout，
**真 GPU hang 不是 jitter**。

### v1.3.110 — pass-through 改走 single-cmd-buf path (`runFrucOverridePass`)

當 RIFE 不會跑時 (warmup 沒完成或 hasPrev=false)，dispatch 直接走原
v1.3.83 的 `runFrucOverridePass`（forward + reverse 一條 cmd buf 一次
submit），完全避開 Phase A/C 分裂。理論上 v1.3.83 ship 時測過穩定。

**結果：仍然只跑了 frame#1，第二 frame 就 silently 不再 dispatch。**
Phase A/C 分裂不是根因 —— `runFrucOverridePass` 本身就有問題。

### v1.3.111 — 加 ENTRY trace 看真正發生什麼

確認 `runFrucOverridePass` 每 frame ENTRY 都有跑（HoldVal=0,1,2,3,4），
代表 dispatch 都「成功」（從 caller 角度看），只是 `frame#N OK` log 是
modulo 60 才出。但 ENTRY count 30 秒只到 5 次 —— 真實 fps **0.16fps**，
不是 30。

### 翻轉的根因認定

跟 RIFE init 也沒關係：用 `VIPLE_VK_FRUC_RIFE=0`（完全不 init RIFE）
跑也一樣 —— 3 個 ENTRY 後 silently drop frame，queue overflow。

所以問題在 **`runFrucOverridePass` 跟 libplacebo 的 hold/release 互動**：
- frame#0：override 跑，hold_ex 在 HoldVal=1 hold image
- frame#1：override 等 HoldVal=1 timeline sem signaled
- libplacebo 每 frame 應該在 pl_swapchain_submit_frame 內 signal 那個
  sem，但實測似乎沒有 reliable 在每 frame signal，或 signal 跟我們的
  期待 value 對不上

### §J.3.e.2.g 真正的範圍其實是 §J.3.e.2.e1b 的整段重做

不是 cmd buf / fence 拆分（那只是表象 fix）。真正要動的：

| 子題 | 內容 |
|---|---|
| **g.A** dump pl_vulkan_hold_ex / release_ex 的真實 sem signal 行為 | trace libplacebo 每 frame 是否真的在 pl_swapchain_submit_frame 結束時 signal m_FrucOverrideHoldSem 至 m_FrucOverrideHoldVal |
| **g.B** 移除 hold/release timeline，改用 simple ownership transfer | 每 frame：override 跑→ image GENERAL → pl_render_image **不 hold** → pl_swapchain_submit_frame → 下一 frame override 用 pipeline barrier transit ownership 即可 |
| **g.C** 或：每 frame 自己重 wrap pl_tex（disposable wrap） | 避開跨 frame 的 hold/release 生命週期 |

### v1.3.107~v1.3.111 結論

Path B 結構（HOST_VISIBLE staging + Phase A/B/C）是正確的
**抽象**，但實際實作 dependent on `runFrucOverridePass` 的穩定性，
而 §J.3.e.2.e1b 的 hold/release timeline 設計**從第一版就有 latent
bug**，只是早期測試短不到 5 秒看不到。

**建議手段：先解 §J.3.e.2.e1b 的根本同步問題（g.A/g.B/g.C 三選一），
再回頭推 Path B / §J.3.e.X。** 否則任何 override 路徑都會在 steady-
state 撞上同一面牆。

短期可用方案：disable `VIPLE_VK_FRUC_OUTPUT_OVERRIDE`，讓 PlVkRenderer
跑原本的 libplacebo direct render（不過 override），就跟 §J.3.c.1
shipped 的 Vulkan-native 一樣。沒 FRUC，但 latency 跟 D3D11 cascade
比有保障。

---

## §J.3.e.2.h — Generic FRUC port to Vulkan-native（v1.3.113~114）

### h.a (v1.3.113) — HLSL → GLSL 三條 shader port

把 D3D11 GenericFRUC 的三條 compute shader 從 HLSL 轉成 GLSL/SPIR-V，
algorithm 1:1 維持，resource 改 storage buffer：
  • `motionest_compute.hlsl` Q=1 → `kFrucMotionEstShaderGlsl` (19.3KB SPIR-V)
  • `mv_median.hlsl` → `kFrucMvMedianShaderGlsl` (15.9KB)
  • `warp_compute.hlsl` Q=1 → `kFrucWarpShaderGlsl` (18.5KB)

`VIPLE_VK_FRUC_GENERIC_PROBE=1` 觸發 init-time `compile_spirv_module`
驗證 — 三條都通過 glslang。

### h.b — IFRUCBackend 抽象化（跳過）

既有 `IFRUCBackend` interface 完全 D3D11-shaped（`ID3D11Device*`、
`ID3D11Texture2D*` 等），不適合給 Vulkan path 用。Vulkan-side
`runFrucGenericComputePass` 直接 PlVkRenderer member function，沒
透過抽象。D3D11 path 不動。

### h.c (v1.3.114) — Compute pipeline 結構就位

  • plvk.h 加 30+ 個 §J.3.e.2.h member（3 pipeline、5 buffer、
    descriptor pool/sets、cmd buf/fence）
  • `initFrucGenericResources()` 250 行：lambda `buildPipeline` 把
    GLSL 編譯 + DSL + PipelineLayout + VkPipeline 一次性建好；3 條
    shader binding count + push-constant size 不同（ME 4/24, Median
    2/16, Warp 4/24）
  • `runFrucGenericComputePass()` 120 行：3 stage dispatch + buffer
    barrier + 2 vkCmdCopyBuffer（bufRGB→prevRGB, mvFiltered→prevMV
    給下 frame temporal predictor 用），1s fence wait
  • `currRGB` 直接 alias `m_FrucNv12RgbBufRGB`，不重複配置

`VIPLE_VK_FRUC_GENERIC=1` 觸發 init 加 1 次 smoke dispatch 驗證實測：

```
[VIPLE-VK-FRUC] §J.3.e.2.h.c init: enter (W=1280 H=720 block=8 mv=160x90)
[VIPLE-VK-FRUC] §J.3.e.2.h.c init: PASS — pipelines/buffers/desc-sets ready
[VIPLE-VK-FRUC] §J.3.e.2.h.c run: frame#1 OK (interp ready in m_FrucInterpRgbBuf)
[VIPLE-VK-FRUC] §J.3.e.2.h.c init: smoke dispatch PASS
```

3 pipeline build + 5 buffer alloc + 3 dispatch + fence wait 全部 cleanly
過。**Vulkan-side compute infrastructure 完全就位**。

### h.d — 阻塞於 §J.3.e.2.g（libplacebo hold/release sync bug）

要把 §J.3.e.2.h.c 的 compute output 接到 libplacebo render 路徑，需要
其中一個：
  • 雙 `pl_render_image` + 雙 `pl_swapchain_submit_frame`（每 frame
    server 對應 client present 一個 interp + 一個 real）
  • 或修改 `pl_render_image` 的 source plane override 路徑

兩種都會走過 `pl_vulkan_wrap` + `pl_render_image` + 跨 frame ownership
管理，**直接撞上 §J.3.e.2.g 的根因**：libplacebo 對我們 wrap 出來的
`pl_tex` 的 hold/release timeline signal 行為跟我們期望的 value 對不上，
frame#2+ silently drop。

§J.3.e.2.h.c 的 compute pipeline **本身正常**（smoke dispatch 證實），
但要看到實際畫面 output 必須先解 §J.3.e.2.g.A/B/C 三選一。

### h.e — Benchmark vs D3D11 baseline（gated by h.d）

無法跑（h.d 沒接通 → 沒實際 interp output 可量測）。

### Status (v1.3.114)

| 子題 | 狀態 |
|---|---|
| h.a GLSL 移植 | ✅ ship v1.3.113 |
| h.b 抽象化 | ⏭ skip（不需要）|
| h.c Vulkan compute pipeline | ✅ ship v1.3.114（init + smoke 通）|
| h.d 雙 present 整合 | 🔴 阻塞於 §J.3.e.2.g |
| h.e benchmark | 🔴 阻塞於 h.d |

**Vulkan FRUC 的演算法層完全就位**（GLSL ports + compute infrastructure），
**但渲染整合卡在 §J.3.e.2.e1b 的 libplacebo timeline bug**。後者跟手刻
RIFE / Path B / Generic FRUC 都共用同一條 override 路徑 ——
**§J.3.e.2.g 是這條 workstream 真正的 bottleneck**。

### 對使用者的可見效果（v1.3.114 ship）

  • 預設沒設 env 完全沒影響（向下相容）
  • `VIPLE_VK_FRUC_GENERIC_PROBE=1` 跑 init-time GLSL→SPIR-V 驗證
  • `VIPLE_VK_FRUC_GENERIC=1` 加上 `VIPLE_VK_FRUC_OUTPUT_OVERRIDE=1`
    可看 init log 確認 compute pipeline 建構成功，但實際畫面還是
    走 v1.3.111 的 e1b 路徑 — **不會比之前更好**（一樣會在
    frame#2 後 stall 到 fallback 進 D3D11 cascade）
