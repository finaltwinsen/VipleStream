/**
 * @file src/vr/vr_latch.h
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 相位回授的頻率鎖控制器（純函式，可單元測試）。
 *
 * client 每 100 ms 送一則 0x5507/08 LATCH（`slackUs`：串流幀比 XR latch 早到多少，正值＝早到）。
 * slack 太大代表幀在 client 端白等（多了延遲），太小或負值代表幀常趕不上 latch（會重用舊幀）。
 * 控制器把 slack 拉向目標值：slack 偏大就讓 server 的虛擬 vsync 稍微放慢（ppm > 0，週期變長，
 * 之後的幀漸漸晚到），偏小就加快。輸出的 ppm 夾在 ±VR_LATCH_PPM_MAX（設計：±200 ppm），由
 * server 換成 pacing.period_q32 交給 driver；driver 端對週期本身不再 slew，所以上限只由這裡保證。
 *
 * 這是頻率回授：兩台機器的時鐘差（通常數十 ppm）由積分項吸收，比例項處理相位誤差。
 */
#pragma once

#include <cstdint>

namespace vr::latch {

  constexpr int32_t VR_LATCH_PPM_MAX = 200;

  struct result_t {
    bool update = false;  ///< 要不要更新 pacing（ppm 變化夠大、或距上次送出 ≥ 1 s）
    int32_t ppm = 0;  ///< 建議的週期修正（正值＝週期變長）
    double slack_ema_us = 0;  ///< 平滑後的 slack
    double target_us = 0;  ///< 目前的目標 slack
  };

  class controller_t {
  public:
    /**
     * @brief 處理一則 LATCH。
     * @param slack_us LATCH.slackUs
     * @param display_period_ns LATCH.displayPeriodNs（0 或不合理時沿用上次，預設 11.1 ms）
     * @param now_ns 單調時鐘（ns）
     */
    result_t on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns);

    /**
     * @brief 沒有新 LATCH 時定期呼叫（例如 control 迴圈的 1 s tick）：超過 2 s 沒有 LATCH 就讓 ppm 衰減回 0，
     *        避免 client 暫停送 LATCH（XR 失焦、斷線）時 server 一直維持舊的修正。
     */
    result_t on_idle(int64_t now_ns);

    int32_t last_sent_ppm() const {
      return sent_ppm_;
    }

    uint64_t latch_count() const {
      return count_;
    }

    void reset();

    // 參數（公開給單元測試）
    static constexpr double kEmaAlpha = 0.2;  ///< 10 Hz 取樣下約 0.5 s 的時間常數
    static constexpr double kKp = 0.05;  ///< ppm／µs：誤差 4 ms 時飽和
    static constexpr double kKi = 0.004;  ///< ppm／(µs·s)
    static constexpr double kIntegralMax = 150.0;  ///< 積分項上限（ppm）
    static constexpr int32_t kMinDelta = 2;  ///< ppm 變化 ≥ 這個值才更新
    static constexpr int64_t kRefreshNs = 1'000'000'000;  ///< 即使沒變化也每 1 s 更新一次
    static constexpr int64_t kStaleNs = 2'000'000'000;  ///< 超過這麼久沒 LATCH 就衰減

    /// 目標 slack：顯示週期的 1/4，夾在 [1, 4] ms
    static double target_for_period(uint32_t display_period_ns);

  private:
    result_t decide(int64_t now_ns, int32_t ppm);

    bool have_ema_ = false;
    double ema_ = 0;
    double integral_ = 0;
    uint32_t period_ns_ = 11'111'111;
    int64_t last_latch_ns_ = 0;
    int64_t last_sent_ns_ = 0;
    int32_t sent_ppm_ = 0;
    bool sent_once_ = false;
    uint64_t count_ = 0;
  };

}  // namespace vr::latch
