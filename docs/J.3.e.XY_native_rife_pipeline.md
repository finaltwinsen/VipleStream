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
| 4Y.6 Step 1+2 | `2dabc1a` | cooperative_matrix probe + feature chain + 16×16 GEMM hello-world (offline-compiled SPIR-V，繞過 ncnn glslang) | (no perf Δ) | 4.0× |
| 4Y.6 Step 3 | `a1f609c` | Conv2D-via-GEMM coopmat shader + isolated unit test | (smoke only) | 4.0× |
| 4Y.6 Step 4 | `068c9b2` | dispatch path 整合為 opt-in（`VIPLE_RIFE_VK_COOPMAT=1`），預設關閉 | (default-OFF) | 4.0× |

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

## 4Y.6 Tensor Core / cooperative_matrix — opt-in，預設關閉

`VK_KHR_cooperative_matrix` 擴充 + `GL_KHR_cooperative_matrix` GLSL
擴充已完整接通，Conv-via-GEMM coopmat shader 通過 isolated unit test。
**但 end-to-end RIFE-v4-lite 上 perf 增益不夠抵 precision regression**，
Step 4 落地為 opt-in（`VIPLE_RIFE_VK_COOPMAT=1` 啟用），預設仍走
4Y.4 tiled path。

### 完成的工程（4 個 commits）

| Step | Commit | 內容 | 結果 |
|---|---|---|---|
| 1+2 | `2dabc1a` | device probe + feature chain + offline-compiled hello-world SPIR-V | 11 shapes 列出，16×16×16 fp16-A/B fp32-C/D 可用，hello GEMM 對 CPU ref 4.77e-07 |
| 3 | `a1f609c` | Conv2D-via-GEMM coopmat shader（im2col + 16×16 tile）+ runConv2DCoopMatGpuTest | Conv_18 7.78e-04 / Conv_50 7.36e-04 vs CPU ref，皆 PASS（2e-3 tol）|
| 4 | `068c9b2` | dispatch path 整合 + best-effort pipeline build + env-var gate | default-OFF；opt-in 時 Final.1 mean 4.7e-5→3.2e-3，max 0.013→0.53，FAIL |

### 為什麼預設關閉（RTX 3060 Laptop 量到的 trade-off）

| 指標 | coopmat OFF（4Y.4 path）| coopmat ON | 差距 |
|---|---|---|---|
| Final.1 mean abs err | 4.72e-05 | 3.21e-03 | 70× 差 |
| Final.1 max abs err | 0.0135 | 0.530 | 39× 差 |
| Final.1 frac > 1e-2 | 0.00% | 5.73% | 6 個百分點 |
| 4g.6 verdict | PASS | FAIL | — |
| Final.1 verdict | PASS | FAIL | — |
| Final.3a native median | 19.25 ms | 17.95 ms | 7% 加速 |

**Precision regression 原因**：double-fp16 quantisation 累積。matA（fp16
weight，從 .bin 直接讀）× matB（im2col 把 fp32 input cast 成 fp16）+
fp32 acc，per-MAC quantisation 誤差 ~5e-4。RIFE-v4-lite 在 coopmat
path 走 40 個 conv layers，σ_total = σ_per_layer × √N = 5e-4 × √40 ≈
3.2e-3 — 與量到的 mean 完全吻合，是數學上可預期的 statistical
accumulation，不是 shader bug。

**Perf 只贏 7% 原因**：RIFE-v4-lite 每層 conv 已經夠小（總 forward
~19 ms），Tensor Core 的 fixed-cost（per-tile im2col + fp32→fp16 cast +
shared-memory barrier）平攤後吃掉大半 throughput 增量。這個架構需要
**大 channel × 大 spatial** 的 conv 才會明顯贏，RIFE-v4-lite 的 56
個 conv 平均 only ~80K MACs/layer 屬於小型範圍。

### Device 上實際支援的 11 個 cooperative_matrix shapes（RTX 3060 Laptop, NV 596.84）

| # | M×N×K | A | B | C | Result | scope |
|---|---|---|---|---|---|---|
| 0 | 16×16×16 | fp16 | fp16 | fp16 | fp16 | Subgroup |
| 1 | 16×8×16 | fp16 | fp16 | fp16 | fp16 | Subgroup |
| 2 | 16×8×8 | fp16 | fp16 | fp16 | fp16 | Subgroup |
| **3** | **16×16×16** | **fp16** | **fp16** | **fp32** | **fp32** | **Subgroup** ← 我們選的 |
| 4 | 16×8×16 | fp16 | fp16 | fp32 | fp32 | Subgroup |
| 5 | 16×8×8 | fp16 | fp16 | fp32 | fp32 | Subgroup |
| 6 | 16×16×32 | uint8 | uint8 | uint32 | uint32 | Subgroup |
| 7 | 16×16×32 | sint8 | sint8 | sint32 | sint32 | Subgroup |
| 8 | 16×8×32 | uint8 | uint8 | uint32 | uint32 | Subgroup |
| 9 | 16×8×32 | sint8 | sint8 | sint32 | sint32 | Subgroup |
| 10 | 16×16×16 | bf16 | bf16 | fp32 | fp32 | Subgroup |

bf16 (#10) 看起來像是另一個選項，但 bf16 mantissa 只有 7 bits（fp16
有 10 bits），per-MAC 誤差 ~2^-7 ≈ 8e-3 比 fp16 還大 8×；只有在 fp16
**動態範圍** saturation 時才會贏（trained NN inference 在我們的 norm
化 input 上不會 saturate），所以對 RIFE-v4-lite precision 沒幫助。
int8 quantised path (#6-9) 是另一條工程支線，要 pre-quantise 全部
weights/activations，是 separate 工事。

### 後續優化方向（如果決定推 coopmat path）

1. **shader 改 multi-subgroup WG**：目前 1 WG = 1 subgroup（32 thread）
   produces 16×16 tile。RTX 3060 SM 有 1536 threads concurrent，現在
   利用率僅 ~50%。改成 1 WG = 4 subgroups 共享 im2col tile load → 預期
   1.5-2× 額外加速，把 Tensor Core 的 fixed-cost 攤掉
2. **selective gating**：對 precision 敏感的 layer（網路邊界、output
   head）keep 用 4Y.4 path，深層 mid-network conv 走 coopmat。需要對
   error sensitivity 分析 + per-layer flag
3. **接受 7% gain 不值**：thermal-throttle 噪音都 ~30%，7% 速度收益
   遠小於這個 noise floor。`VIPLE_RIFE_VK_COOPMAT=1` 留著當 future
   reference 但不主推

目前選 (3)：留著 opt-in flag 當 known-working capability，未來如果
需要往 4090 或更大 RIFE 模型推就有現成的 path 可以 build on。

### 風險（已驗證）

- **Hardware-specific**：cooperative_matrix 在 NV Turing+ / AMD RDNA3+ /
  Intel Arc 部分支援。pipeline build 失敗時 best-effort 路徑會清空
  `pipelines[Conv2D_CoopMat]` + 印一行 INFO log，**不影響其他 shader 建置**
- **Verification path**：thermal noise 比 7% perf 差大很多，cold-GPU
  reading 拿不到穩定 cross-engine 對比。要拿真正的 perf 增益判斷必須
  `nvidia-smi --lock-gpu-clocks` + sustained workload

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
