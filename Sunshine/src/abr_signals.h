/**
 * @file src/abr_signals.h
 * @brief §ABR-ADAPT：編碼端給碼率控制器的訊號（行程內全域，跨執行緒用 atomic）。
 *
 * 只有一路編碼在跑（VR 或桌面串流同時只有一個 session 在編碼），所以用全域值就夠；
 * 沒有資料時是 -1，讀的一方要當成「不知道」。
 */
#pragma once

#include <atomic>

namespace abr_signals {
  /// 非 IDR 幀平均 QP 的 EMA（× 100）；-1＝還沒有資料或編碼器不提供
  inline std::atomic<int> avg_qp_x100 {-1};
  /// 目前編碼器的格式：0＝H.264、1＝HEVC、2＝AV1、-1＝未知（QP 尺度不同，飽和點要分開看）
  inline std::atomic<int> codec {-1};

  /// 編碼器每出一幀呼叫（IDR 的 QP 和 P 幀不同，不算）
  inline void note_frame_qp(int qp, bool idr) {
    if (idr || qp <= 0) {
      return;
    }
    const int cur = avg_qp_x100.load(std::memory_order_relaxed);
    const int next = cur < 0 ? qp * 100 : cur + (qp * 100 - cur) / 10;  // α = 0.1
    avg_qp_x100.store(next, std::memory_order_relaxed);
  }

  /// 編碼器建立時呼叫，清掉上一個 session 的值
  inline void reset(int codec_id) {
    avg_qp_x100.store(-1, std::memory_order_relaxed);
    codec.store(codec_id, std::memory_order_relaxed);
  }
}  // namespace abr_signals
