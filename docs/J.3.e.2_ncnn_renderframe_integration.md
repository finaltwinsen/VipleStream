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
