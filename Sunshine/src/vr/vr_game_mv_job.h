/**
 * @file src/vr/vr_game_mv_job.h
 * @brief §VR-GAME-MV（M-MV1）：一張編碼影像配到的遊戲 MV 的非同步壓縮工作。
 *
 * 擷取執行緒只把 SHM 的 payload 複製出來（seqlock 驗證需要），量化＋Planar＋LZ4 交給背景執行緒：
 * 360×180 一幀在 host 上約 0.4 ms（變化大的畫面約 1.1 ms），放在擷取執行緒會直接加在延遲上；
 * 而 MV 要到影像編碼完、送網路時（M-MV2）才用得到，那時壓縮早就做完。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace video {

  struct vr_game_mv_job_t {
    enum : uint8_t {
      pending = 0,
      ready = 1,
      failed = 2,
    };

    std::atomic<uint8_t> state {pending};  ///< 背景執行緒寫完 compressed 之後才以 release 設成 ready／failed
    std::vector<uint8_t> compressed;  ///< vr::mv_codec 的壓縮流（含 12 B 標頭）；state == ready 之後唯讀
    double encode_us = 0.0;  ///< 背景執行緒的量化＋壓縮耗時

    /// 等到不是 pending（最多 timeout）；回傳是否 ready。只給送出端（M-MV2）與傾印用，擷取執行緒不可呼叫。
    bool wait_ready(std::chrono::microseconds timeout) const noexcept {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (;;) {
        const uint8_t s = state.load(std::memory_order_acquire);
        if (s != pending) {
          return s == ready;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        std::this_thread::yield();
      }
    }
  };

}  // namespace video
