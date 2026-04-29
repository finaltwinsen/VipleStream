# §J.3.e.2.i.7 — VkFruc 50 輪自動化優化追蹤

優先序：**流暢度 > 延遲 > 品質 > 多幀生成**

測試條件：HEVC 1920x1080 @ 180fps user / 90fps server / dual-present /
SW decode (libdav1d → VkFruc) / 18s capture + 8s warmup / host <host-ip>

## 指標解讀

- **fps_dual**：dual-present cycle rate (90fps target → 180fps effective)
- **ft_mean**：dual cycle 平均週期，理想 11.11ms (= 1000/90)
- **p99 / p99.9**：1% / 0.1% worst dual cycle ms（**越低越流暢**）
- **gpu_mean**：FRUC compute chain (NV12→RGB + ME + Median + Warp) 平均 GPU 時間

## Round 紀錄

| # | label | change | fps | ft | p99 | p99.9 | gpu | verdict |
|---|---|---|---|---|---|---|---|---|
| R0 | baseline_v1.3.177 | (baseline) | 90.11 | 11.10 | 21.57 | 22.26 | 0.96 | — |
| R1 | r01_fifo_relaxed | dual mode FIFO_RELAXED present | 90.04 | 11.10 | **11.92** | **13.96** | 0.96 | ✅ KEEP (-45% p99) |
| R2 | r02_swapchain5 | swapchain images 4→5 | 90.06 | 11.10 | 26.19 | 27.01 | 1.08 | ❌ REVERT (+118%) |
| R3 | r03_mailbox | dual mode MAILBOX present | 120.01 | 8.33 | 11.70 | 25.48 | 0.88 | ❌ REVERT (p99.9 +83%, breaks dual pair) |
| R4 | r04_inflight3 | kFrucFramesInFlight 2→3 | 90.17 | 11.09 | 22.82 | 23.13 | 1.02 | ❌ REVERT (+91% p99) |

## Best so far (current main HEAD)

- v1.3.178 (R1): p99=11.92, p99.9=13.96

## Insight 累積

- 加深 pipeline (swapchain images 5, in-flight slots 3) **均退步** —
  diminishing returns，dual-present at vsync 90fps 已經很緊
- MAILBOX **破壞 dual-pair 配對**（第二個 present 蓋掉第一個 in mailbox slot）
- FIFO_RELAXED **大勝**：1% jitter spike 直接掉 vsync slot 改用 tearing
