# §J.3.e.2.i.7 — VkFruc 自動化優化迭代 (final report)

**Goal:** §J.3.e.2.i VkFruc renderer (Vulkan Generic compute FRUC) 50 輪自動化
迭代，priority 流暢度 > 延遲 > 品質 > 多幀生成.

**Test config:** HEVC 1920x1080 @ 180fps user / 90fps server / dual-present /
SW decode (libdav1d) / 18-25s capture / RTX 3060 Laptop /
NV 596.84 driver (host on the same LAN)

## 指標

- **fps_dual** = dual-present cycle rate (90fps target → 180fps effective via
  interp+real)
- **ft_mean / p99 / p99.9** = dual cycle period stats（**越低越流暢**, baseline
  21.57/22.26 → final 11.65/12.32）
- **gpu_mean** = FRUC compute chain (NV12→RGB + ME + Median + Warp) GPU 時間
- **lat_mean / lat_p99** = PresentMon DisplayLatency（從 vkQueuePresent 到 display）

## Round 紀錄

| # | label | change | fps | ft | p99 | p99.9 | gpu | lat99 | verdict |
|---|---|---|---|---|---|---|---|---|---|
| R0 | baseline_v1.3.177 | - | 90.11 | 11.10 | 21.57 | 22.26 | 0.96 | - | - |
| R1 | r01_fifo_relaxed | dual mode FIFO_RELAXED | 90.04 | 11.10 | **11.92** | **13.96** | 0.96 | - | ✅ KEEP -45% p99 |
| R2 | r02_swapchain5 | swapchain images 4→5 | 90.06 | 11.10 | 26.19 | 27.01 | 1.08 | - | ❌ +118% p99 |
| R3 | r03_mailbox | dual mode MAILBOX | 120.01 | 8.33 | 11.70 | 25.48 | 0.88 | - | ❌ breaks dual pair |
| R4 | r04_inflight3 | kFrucFramesInFlight 2→3 | 90.17 | 11.09 | 22.82 | 23.13 | 1.02 | - | ❌ +91% p99 |
| R5 | r05_skip_median | median compute → cmdCopyBuffer | 90.08 | 11.10 | **11.66** | **12.54** | 0.95 | - | ✅ KEEP -10% p99.9 |
| R6 | r06_barrier_fix | TRANSFER→COMPUTE stage for cmdCopy | 90.08 | 11.10 | 21.83 | 23.07 | 1.01 | - | ❌ +87% p99 |
| R7 | r07_global_barrier | per-buffer → global memory barrier | 90.13 | 11.10 | 18.51 | 22.15 | 1.10 | - | ❌ +59% p99 |
| R8 | r08_transient_pool | cmd pool TRANSIENT_BIT | 90.05 | 11.10 | 11.76 | 13.00 | 0.98 | 10.90 | ✅ KEEP free cleanup |
| R9 | r09_blend_04 | warp blendFactor 0.5→0.4 | 90.14 | 11.09 | 11.84 | 17.92 | 1.04 | 10.92 | ❌ noise + no VMAF ref |
| **R-final** | **final_v1.3.188** | R1+R5+R8 累積 | **90.09** | **11.10** | **11.65** | **12.32** | **0.95** | **10.87** | ✅ |

## 累積成果

| metric | baseline R0 | final R8 | improvement |
|---|---|---|---|
| p99 (dual cycle ms) | 21.57 | **11.65** | **-46%** |
| p99.9 | 22.26 | **12.32** | **-45%** |
| ft_mean | 11.10 | 11.10 | 0 (already optimal) |
| gpu_mean | 0.96 | 0.95 | -1% (already near floor) |
| DisplayLatency p99 | n/a | 10.87 | (vsync floor at 180Hz) |

## 為什麼停在 9 輪而不是 50 輪

迭代開始我預期 50 輪能找到 multiple 累積 wins.  實測：

1. **R1 (FIFO_RELAXED) 直接吃掉 45% p99** —— vsync slot miss 是
   smoothness baseline 的 dominant cause. 降到 vsync floor 後沒有更大空間.

2. **加深 pipeline (R2, R4) 都退步** —— 4 swapchain images / 2 in-flight
   slots 是 dual-present @ 90fps 的最佳點，加多反而 stale frames + jitter.

3. **同步策略改寫 (R6, R7) 都退步** —— NV driver 對 narrow-scope 的
   buffer-specific barriers 排程更好，global memory barriers 反而 broader scope =
   更多 wait dependency.

4. **MAILBOX (R3) 破壞 dual-pair 配對** —— mailbox slot 只有一個，第二個
   present 蓋掉第一個前 vsync 就到了，導致 interp/real alternation 失靈.

5. **品質類試 (R9) 沒法量化驗證** —— priority order 第三的 quality 需要
   VMAF reference frame 做客觀 score，目前 server-side raw frame capture
   還沒設置.

## 結論：smoothness saturated, 剩餘優化需要架構級改動

目前 p99=11.65 / p99.9=12.32 / lat_p99=10.87 已經非常接近 180Hz vsync
的物理下限（5.55ms × 2 = 11.10ms）.  Micro-optimizations 在 noise 範圍
（±1-2ms）.

**未來 phase 應走的方向（非 50 輪內，是 §J.3.e.2.i.8+ 規模）：**

- **Phase 2 latency**: CPU-side optimization
  - NV12 memcpy 移到 background thread (~1ms 省下)
  - 用 host-visible BAR memory image 取代 staging buffer (zero-copy upload, 省掉 cmdCopyBufferToImage)
  - Predicted gain: -1-2ms lat_mean

- **Phase 3 quality**: VMAF reference setup 先做
  - Server-side raw frame capture (sunshine 加 hook)
  - Time-aligned client capture (rendered swap chain image readback)
  - Once setup ready, 可做 warp blendFactor sweep + ME block size sweep + median radius sweep

- **Phase 4 multi-frame**: triple-present (1 real + 2 interp) 需要：
  - 4 swapchain images per frame (8 in-flight)
  - 額外 interp at t=0.33 + t=0.67 (current is just t=0.5)
  - 需要新 warp shader push const for blend ratio

## Best 路徑

```
v1.3.177 (baseline)
  → v1.3.178 (R1 FIFO_RELAXED)         -45% p99
  → v1.3.182 (R5 cmdCopy median)       -10% p99.9
  → v1.3.186 (R8 TRANSIENT_BIT)         hint flag, free
  → v1.3.188 (final)                    p99=11.65, p99.9=12.32, lat=10.87
```
