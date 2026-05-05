# §J.3.e.X / §J.3.e.Y — 手刻 RIFE Vulkan inference pipeline

`moonlight-qt/app/streaming/video/ffmpeg-renderers/rife_native_vk.{h,cpp}`

獨立於 ncnn 的 RIFE-v4.25-lite 推論實作。動機：把 ~10 MB ncnn DLL
變成 ship-time 可選依賴（Linux distro 沒打包 ncnn-vulkan 時降級到
native），順便建立未來 perf 工程的乾淨基底。

兩個 sub-arc：

- **§J.3.e.X** correctness milestone（17 commits，2026-05-04..05-05）—
  從零實作 .param parser → 11 個 op shader → graph executor → 完整
  inference 通過 vs-ncnn correctness gate
- **§J.3.e.Y** perf workstream（6 commits + 1 negative experiment，
  2026-05-05..05-06）— 從 86 ms baseline 壓到 ~22 ms (4×)，correctness
  進一步精準到 fp32 epsilon 等級

## §J.3.e.X 完整工程地圖

| Phase | Commit | 內容 | 驗收 |
|---|---|---|---|
| 1 | `8807886` | scaffold + .param text parser + 14 OpKind enum | layer/blob count match header (389 / 485) |
| 2 | `f5e90f0` | .bin weight loader (fp16 flag-aware: 0x01306B47) | total bytes match .bin file size |
| 3a | `9cd71ed` | CPU reference Conv2D (fp32 acc) | smoke against Conv_16 fp16 unpack |
| 3b.1 | `4b6b3de` | GLSL Conv2D 3×3 shader source（push const-driven） | source compile via ncnn::compile_spirv_module |
| 3b.2 | `d32af7e` | Vulkan dispatch + correctness gate | max_abs_err vs CPU ref ≤ 1e-4 (Conv_16 達 2e-7) |
| 4a | `90a942d` | parameterise conv test → 56 layer audit + 3 representative gates | Conv_16/18/50 (s∈{1,2}, n∈{16,192}) all PASS |
| 4b | `61c272e` | BinaryOp shader (4 ops × 3 broadcast modes 含 plane-bcast) + 4-case gate | 92 layer 涵蓋 |
| 4c | `7d653c9` | Activation shader (ReLU/LeakyReLU/Sigmoid) + 3-case gate | 41 layer 涵蓋 |
| 4d | `b67eea9` | Concat / Crop / PixelShuffle / Interp / EltwiseSum + runComputeOnce shared helper | 76 layer 涵蓋 |
| 4e | `23f1101` | Deconvolution shader + 2-layer gate | 7 layer 涵蓋（later 4g.6 抓到 weight layout bug 需修） |
| 4f | `3453dac` | rife.Warp custom op (從 ncnn_rife_warp.cpp 抽算法) | 18 layer 涵蓋；shape-agnostic 1 case 即足 |
| 4g.1 | `4302da1` | tensor shape inference（每個 OpKind 一條規則） | 485 blob shape 全推完，3 anchor 對 |
| 4g.2 | `4f3461c` | 9 個 shader pipeline persistent cache | 9/9 build PASS |
| 4g.3 | `56d5263` | buffer pool（485 blob + 166 weight tensor）+ MemoryData / Conv weight prefill | spot-check anchor 對 |
| 4g.4 | `77e10bc` | graph executor: 14 個 dispatchLayer handler + 389-dispatch traversal + RAW barriers | submit OK，346 dispatch + 43 skip = 389 |
| 4g.5 | `7d55762` | end-to-end out0 readback + sanity (NaN/Inf/range 檢查) | range [0.004, 0.995] mean 0.499 ✓ |
| 4g.6 | `89c50b9` | vs-ncnn correctness gate **+ Deconv weight layout fix** | mean=4.23e-04 max=0.10 frac>1e-2=0.51% PASS |
| Final.1 | `7a97802` | RifeNativeExecutor 公開 class（init/run/shutdown lifecycle, pImpl）| determinism 0.0, vs-ncnn 同 4g.6 PASS |
| Final.2 | `d385c0f` | VkPipelineCache 跨啟動持久化（caller-supplied path）| 88 KB cache write/load round-trip bit-identical |
| Final.3a | `cba641d` | native vs ncnn latency benchmark + 結論 commit | cold-GPU: native 86 ms, ncnn 8.6 ms, 10× 慢 |

**Phase 4g.6 的兩個關鍵 bug fix**（前面 phase 沒抓到，因為單元 test 只
比對 native shader vs native CPU ref，self-consistent 通過）：

1. **Deconvolution weight layout**：之前假設 `(in_ch, out_ch, kH, kW)`，
   實際 ncnn 慣例是 `(out_ch, in_ch, kH, kW)` 同 Conv。修了 weight 索引
   `(c*outN + n)` → `(n*inC + c)`。
2. **BinaryOp plane-broadcast (mode 3)**：`(C, H, W) × (1, H, W)` 的
   sigmoid mask × warped image 形狀我們 audit 漏掉，shader 加 mode 3
   `b_buf[i % hw]` 跟 dispatch 的 detect 邏輯。

## §J.3.e.Y 性能工程

從 86 ms baseline 壓到 ~22 ms（cold GPU benchmark @ RTX 3060 Laptop,
256×256 input, 15 iter median）：

| Phase | Commit | 改動 | Per-frame median | 累積 |
|---|---|---|---|---|
| 4Y.1a | `876ce45` | `findHostVisibleMemoryType` 偏好 `DEVICE_LOCAL \| HOST_VISIBLE` (BAR / ReBAR) | 86 → 43.6 ms | 2.0× |
| 4Y.0 | `85430c6` | `RifeNativeExecutor::lastTiming` per-phase wall-clock instrument | (no perf Δ) | 2.0× |
| 4Y.1b | `413a0ac` | out0 host-cached staging + `vkCmdCopyBuffer` 在 cmdbuf 末端（避 WC slow CPU read） | 43.6 → 24.0 ms | 3.6× |
| 4Y.4 | `5edba90` | tiled shared-memory Conv2D for k=3 stride=1 pad=1 dilation=1（44/56 conv layers） | 24.0 → 21.5 ms | 4.0× |
| 4Y.4-stride2 | `58d7268` | s=2 tiled 變體（**reverted from dispatch path**：per-channel barrier overhead 在小 output spatial dims 下勝過 bandwidth saving） | (revert) | 4.0× |
| 4Y.5a | `4be1a6b` | fp16 weight storage + 4 個 conv/deconv shader 改 `float16_t w_buf[]` | 21.5 → ~22.5 ms (噪音範圍) | 4.0× |

### Per-phase profile（4Y.5a 後）

| Phase | Avg ms | % of total |
|---|---|---|
| seed (memcpy in) | 0.18 | 0.4% |
| record (cmdbuf) | 0.79 | 1.8% |
| **gpu compute** | **20.0** | **89%** |
| readback (memcpy out) | 0.06 | 0.3% |

剩下 89% 是純 GPU compute，shader 級優化才有空間繼續壓。

### Correctness 演進（vs-ncnn 對比）

| Phase | mean_abs_err | max_abs_err | frac > 1e-2 |
|---|---|---|---|
| 4g.6 first PASS | 4.23e-04 | 0.10 | 0.51% |
| 4Y.4 (perf 改但 fp32) | 4.23e-04 | 0.10 | 0.51% |
| **4Y.5a (fp16 weights)** | **4.72e-05** | **0.014** | **0.00%** |

4Y.5a 之前 native unpack fp16→fp32 做 fp32 全程算術，跟 ncnn 內部某些
conv 走 fp16 storage shader 有 systematic truncation difference。
兩端都改 fp16 storage 之後，**diff 收斂到 fp32 epsilon × sqrt(389
sequential layer accumulation)**，幾乎已是極限。對 §J.3.e.X 的「Linux
fallback when ncnn unavailable」用途，**precision-wise 已是 production
ready**：99.49% pixels diff ≤ 0.01（視覺上看不出），0% pixels diff
> 0.01。

## Thermal-throttle 噪音（benchmark 警告）

cold-GPU 跟 hot-GPU 對比結果差別劇烈，連續三次跑同一 binary：

| Run | Native median | ncnn median | ratio |
|---|---|---|---|
| Cold (1st) | 21.5 ms | 6.0 ms | native 0.28× |
| Warming (2nd) | 47.3 ms | 11.1 ms | native 0.23× |
| Hot (3rd) | 47.3 ms | 60.8 ms | **native 1.29×** |

ncnn 在 thermal throttle 下退化 ~10× 比 native ~2× 嚴重。實際 streaming
（GPU 持續滿載熱機）下 native **可能已經跟 ncnn 平手或更快**，但短時間
benchmark 拿冷機數字會誤判 ncnn 大勝。所有 §J.3.e.Y commit message
裡的數字都是 cold-GPU first-run reading；relative deltas within one cold
run 是可信的（4Y.4 21.5 vs 4Y.1b 24.0 那種），但 cross-engine ratio 噪
音很大。

## 下一步：Tensor Core / cooperative_matrix（**新 session 工事**）

§J.3.e.Y 4Y.6 的目標是利用 RTX 30/40 Tensor Core 進一步壓 GPU compute
時間。涉及 `VK_KHR_cooperative_matrix` extension（or NV-specific
`VK_NV_cooperative_matrix`）。Tensor Core 對 16×16 fp16 matrix mul
有特殊指令，理論吞吐量是 fp32 ALU 的 8-16×。

### 關鍵 references

- Vulkan extension spec：[VK_KHR_cooperative_matrix](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_cooperative_matrix.html)
- GLSL extension：`GL_KHR_cooperative_matrix` 提供 `coopmat<T, scope, M, N, use>` 型別 + `coopMatLoad` / `coopMatStore` / `coopMatMulAdd` 內建函式
- ncnn 的 `nihui/ncnn` 對應實作：`src/layer/vulkan/convolution_pack8.comp` (legacy) → `convolution_3x3_winograd23_transform_kernel.comp` (Winograd) → 後來 cooperative_matrix variants
- rife-ncnn-vulkan 用的 pack8 fast path 在 RTX 3060 Laptop 是 ~30-45 ms/frame for RIFE 4.25-lite at 1080p；那條 path 走 cooperative_matrix

### Phase 4Y.6 預期工程

1. **enable extension**：`VkPhysicalDeviceCooperativeMatrixFeaturesKHR::cooperativeMatrix = VK_TRUE`，要求 device support `VK_KHR_cooperative_matrix`（RTX 20/30/40 都支援；AMD RDNA3+ / Intel Arc 也有）
2. **query supported matrix shapes**：`vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR` 回傳 (M, N, K, A type, B type, C type, D type, scope) tuples，挑符合 16×16×16 fp16-A/B fp32-C/D 的 properties
3. **重寫 Conv shader**：把 conv reformulate 成 GEMM（im2col + matmul），用 `coopmat` 跑 16×16 tile
4. **可能需要 layout 改動**：cooperative_matrix 要求 input/weight memory layout 16×16 對齊 + 特定 stride，跟現有 NCHW packed 不相容，要在 layer 邊界做轉置
5. **correctness gate 重跑** 4g.6 + Final.1
6. **benchmark vs ncnn pack8 cooperative_matrix path**

預期收益：1.5-2.5× 加速（22 → ~9-15 ms）。挑戰：cooperative_matrix shader
寫起來 verbose，layout 對齊容易出 bug。

### 風險

- Hardware-specific：cooperative_matrix 在 NV 全支援，AMD RDNA3+ 才有，Intel Arc 部分支援。**需保留現有 4Y.4 tiled path 當 fallback**
- Verification path：thermal noise 已壓不過 1.5× 量級的速度差。需要更嚴格的 benchmark protocol：手動 cool-down + lock GPU clocks via `nvidia-smi --lock-gpu-clocks` 取得穩定數字
- ncnn 上游同樣的 path 已有不錯實作可參考（pack8 + cooperative_matrix），但跟我們 NCHW-packed 純 fp32 的架構基底差距大；要漸進式遷移避免一次寫太多 shader 帶來 correctness 風險

## 程式進入點

- 公開 API：`viple::rife_native_vk::RifeNativeExecutor`（`rife_native_vk.h`）
  - `initialize(InitOptions)` — load model, build pipelines/buffers, enable storage 16-bit feature
  - `runInference(in0, in1, t, out0)` — per-frame inference, populates `LastTiming`
  - `shutdown()` — persist VkPipelineCache to disk if path given
- Standalone test entry：`VIPLE_RIFE_NATIVE_VK_TEST=1 VipleStream.exe` 跑全套
  correctness gate + Final.3a benchmark + cache write/load
- 內部執行緒：14 個 `dispatchXxx(ExecState&, const Layer&)` handler 對應
  各 OpKind，由 `dispatchLayer` switch dispatch
- 11 個 GPU pipelines（Conv2D / Deconv2D / BinaryOp / Activation / Copy /
  PixelShuffle / InterpBilinear / EltwiseSum / RifeWarp / Conv2D_3x3_s1 /
  Conv2D_3x3_s2-not-dispatched）由 `PipelineCache` 持有

## 為什麼這個 milestone 不接 production streaming

`Final.3b` (wire native into NcnnFRUC submitFrame as Linux fallback) 計畫
過但未做。原因：

1. NcnnFRUC submitFrame 的 hot path 跟 D3D11 ↔ Vulkan shared image bridge
   緊耦合，用 `ncnn::VkMat` 接介面；改 native fp32 buffer 介面是 4-6h
   的整合工事，medium-high risk
2. Linux 是 native fallback 的觸發場景，但目前沒 Linux test VM 跑 ncnn
   build failure 的環境（§K.1 Linux build 探勘 task 是設這個的前置）
3. cold-GPU benchmark 時 native 22 ms vs ncnn 6 ms = 3.7× 慢，雖然 hot-GPU
   下可能持平，但對「ncnn 可用時優先 ncnn」的決策而言沒理由切換 default

實際用法：

- ncnn 可用 → production 路徑保持走 NcnnFRUC（沒任何改動）
- ncnn 不可用 → Final.3b 之後降級到 RifeNativeExecutor（functional 但
  thermal-state-dependent perf）
- §J.3.e.Y 4Y.6 後，native 跟 ncnn 在 RTX 30+ 上 perf 差距收斂 → 那
  時候 Final.3b 自動有 production-grade fallback，可以考慮 Linux distro
  build 主動 disable ncnn 來簡化打包

## 知識點 / 踩雷記錄

- **Deconvolution weight layout**：ncnn 是 `(out_ch, in_ch, kH, kW)`
  跟 Conv 同；別被 ONNX 的 `(in_ch, out_ch, ...)` 帶歪
- **BAR / ReBAR memory**：`DEVICE_LOCAL | HOST_VISIBLE` 給 GPU full-speed
  read/write + CPU mmap 寫；但 CPU **讀** WC memory ~75 MB/s，所以
  output blob 要走 host-cached staging（4Y.1b）
- **GPU thermal throttle**：connect-stream 級別的 sustained workload 跟
  short benchmark 的 GPU 行為差很多，單次 cold reading 只能拿來做
  same-binary regression check
- **fp16 weight storage 不是 perf 大頭**：weights 太小被 L1 cache 命中。
  真正的 bandwidth 大頭是 activation buffers（30 MB+ working set）
- **GLSL `barrier()` 不便宜**：tiled conv 的 per-channel barrier 在小
  output spatial dims（< 16×16 = 1 workgroup × 大 inC）下會吃光 tile
  的 bandwidth saving。stride=2 layers 因 spatial 小掉到這條負面領域
- **`#extension GL_EXT_shader_16bit_storage`**：要對應 device 的
  `VkPhysicalDevice16BitStorageFeatures::storageBuffer16BitAccess`
  feature flag enabled；Vulkan 1.1+ core，RTX 30+ / AMD RDNA2+ 都支援
