/**
 * @file src/vr/vr_latch_coherence.h
 * @brief VipleStream 2.0 §VR-CADENCE（2026-10-10）：LATCH 樣本的相位一致性（log 的 lockR）。只是診斷，不參與任何決定。
 *
 * R＝最近的 LATCH 樣本取模一個週期之後，各自當成單位向量的平均向量的長度：0＝相位均勻散開（樣本沒有眾數，
 * 頻率鎖無從追起），1＝全部同相。印在 `[VIPLE-VR-LATCH] 10s` 行尾，用來回答「這種負載下樣本還能不能鎖」——
 * 例如 Present 偶爾跳一格的臨界負載。被擋掉（hold）的 LATCH 也照餵，所以擋著的時候也看得到。
 * 參考值（頭盔 log 的 10 秒直方圖換算的；母體是「所有發布的影像」，和每 100 ms 一筆的 LATCH 不完全相同）：
 * 鎖住的視窗 0.31～0.80（中位數 0.63），解析度倍率 400% 時 0.08～0.30。v3 有定時步進的視窗，相位是被刻意移動的，
 * 那個視窗的最小值會偏低。每秒只有 10 筆、門檻的餘裕很小，所以只印不判。
 *
 * 純邏輯：只用標準函式庫、時間由呼叫端傳入、不配置。一次只給一條執行緒用（session_state_t 在 latch_mtx_ 內呼叫）。
 * 輸入來自 client（不可信）：任何值都不會造成除以零或越界。
 */
#pragma once

// standard includes
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vr::latch {

  class coherence_t {
  public:
    static constexpr std::size_t kWindow = 40;  ///< 留最新幾筆樣本（10 Hz 下是 4 s）
    static constexpr std::size_t kMinSamples = 20;  ///< 不到這麼多筆不算 R
    static constexpr int64_t kMaxAgeNs = 8'000'000'000;  ///< 比這更舊的樣本不用
    static constexpr double kOutlierPeriods = 1.5;  ///< |slack| 超過這麼多個週期的樣本丟掉（同 v2 控制器）
    static constexpr double kPeriodChange = 0.02;  ///< 模數變化超過這個比例（換更新率）就重來
    static constexpr double kModulusMinUs = 2'000.0;  ///< 模數只接受 2～100 ms
    static constexpr double kModulusMaxUs = 100'000.0;

    /// 一個 log 視窗（10 s）的統計
    struct window_t {
      int r_last = -1;  ///< 最後一筆樣本之後的 R × 1000（−1＝樣本不夠）
      int r_min = -1;  ///< 這個視窗裡算出來過的最小值（−1＝沒有算出來過）
      int r_max = -1;  ///< 最大值
      uint32_t samples = 0;  ///< 這個視窗收了幾筆樣本
    };

    /// 全部重來（換模式或目標時；之後要重新收滿 kMinSamples 筆）
    void reset() {
      *this = coherence_t {};
    }

    /**
     * @brief 一則 LATCH（不論有沒有餵進控制器）。
     * @param slack_us LATCH.slackUs
     * @param frame_id LATCH.frameId（和上一則相同＝client 沒有新幀、重送舊值，不是樣本）
     * @param modulus_us 取模用的週期（控制器的 modulus_us()：host 虛擬 vsync 週期與顯示週期的較小值）
     * @param now_ns 單調時鐘（ns）
     */
    void on_latch(int32_t slack_us, uint32_t frame_id, double modulus_us, int64_t now_ns) {
      if (!(modulus_us >= kModulusMinUs && modulus_us <= kModulusMaxUs)) {
        return;  // 不合理的模數（含 NaN）
      }
      if (have_mod_ && std::abs(modulus_us - mod_us_) > kPeriodChange * mod_us_) {
        n_ = 0;  // 換更新率了：之前的樣本是對另一個週期取模的
        head_ = 0;
      }
      mod_us_ = modulus_us;
      have_mod_ = true;
      if (have_frame_ && frame_id == last_frame_id_) {
        return;
      }
      have_frame_ = true;
      last_frame_id_ = frame_id;
      const double slack = static_cast<double>(slack_us);
      if (std::abs(slack) > kOutlierPeriods * modulus_us) {
        return;
      }
      const double a = 6.283185307179586 * std::fmod(slack, modulus_us) / modulus_us;
      v_[head_] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)), now_ns};
      head_ = (head_ + 1) % kWindow;
      if (n_ < kWindow) {
        ++n_;
      }
      if (win_.samples < 0xFFFFFFFFu) {
        ++win_.samples;
      }
      const int r = r_permille(now_ns);
      win_.r_last = r;
      if (r >= 0) {
        win_.r_min = win_.r_min < 0 || r < win_.r_min ? r : win_.r_min;
        win_.r_max = r > win_.r_max ? r : win_.r_max;
      }
    }

    /// 不比 kMaxAgeNs 舊的樣本的 R × 1000（0～1000）；不到 kMinSamples 筆回 −1
    int r_permille(int64_t now_ns) const {
      double sx = 0;
      double sy = 0;
      std::size_t k = 0;
      for (std::size_t i = 0; i < n_; ++i) {
        const auto &e = v_[(head_ + kWindow - 1 - i) % kWindow];  // 由新到舊
        if (now_ns > e.t_ns && static_cast<uint64_t>(now_ns) - static_cast<uint64_t>(e.t_ns) > static_cast<uint64_t>(kMaxAgeNs)) {
          break;
        }
        sx += static_cast<double>(e.c);
        sy += static_cast<double>(e.s);
        ++k;
      }
      if (k < kMinSamples) {
        return -1;
      }
      const double r = std::sqrt(sx * sx + sy * sy) / static_cast<double>(k) * 1000.0;
      return r >= 1000.0 ? 1000 : (r <= 0.0 ? 0 : static_cast<int>(r + 0.5));
    }

    /// 10 s log 用：取走這個視窗的統計並歸零（樣本留著）
    window_t take_window() {
      const window_t w = win_;
      win_ = window_t {};
      return w;
    }

  private:
    struct entry_t {
      float c = 0;  ///< 單位向量（角度＝2π × 相位 ÷ 週期）
      float s = 0;
      int64_t t_ns = 0;
    };

    std::array<entry_t, kWindow> v_ {};  ///< 環狀：最新的 kWindow 筆
    std::size_t n_ = 0;
    std::size_t head_ = 0;
    bool have_mod_ = false;
    double mod_us_ = 0;
    bool have_frame_ = false;
    uint32_t last_frame_id_ = 0;
    window_t win_ {};
  };

}  // namespace vr::latch
