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
 *
 * §VR-LATCH-V2（2026-10-05，Frame 實測）：client 量到的 slack 其實是「取模一個週期」的相位——晚到 δ 的幀
 * 會被下一個 latch 接走，量成 T−δ，不會是負值。legacy 把它當線性量（夾在 ±T、對 slack 做 EMA、輸出飽和時
 * 照樣積分），結果 ppm 長時間頂在 +200、每 35～60 s 整格滑移一次（每次 MTP 與姿態落後跳一個週期），
 * 鎖住時 T/4 的目標餘裕也讓約每秒一張幀趕不上。v2：誤差在上側折返（e ≥ T/2 時減 T，負值夾在 −T/2，
 * 協定「負值＝晚到」的語意不變）、EMA 作用在誤差上、離群值與重複 frameId 丟棄、更新率改變時重來、
 * 條件式 anti-windup、目標改成週期的 40%（25～50% 可設）、增益提高（阻尼比約 0.4→0.67）。
 * 模式由 config `vr_latch_mode` 決定（預設 legacy，等 Frame A/B 後再改預設）。
 */
#pragma once

#include <cstdint>

namespace vr::latch {

  constexpr int32_t VR_LATCH_PPM_MAX = 200;

  /// §VR-LATCH-V2：控制器版本
  enum class mode_e : uint8_t {
    legacy,  ///< M4a R3 原版
    v2,  ///< 相位取模、anti-windup、目標 0.4T
  };

  struct result_t {
    bool update = false;  ///< 要不要更新 pacing（ppm 變化夠大、或距上次送出 ≥ 1 s）
    int32_t ppm = 0;  ///< 建議的週期修正（正值＝週期變長）
    double slack_ema_us = 0;  ///< 平滑後的 slack（v2：目標＋平滑後的誤差）
    double target_us = 0;  ///< 目前的目標 slack
    double err_us = 0;  ///< 平滑後的誤差（正值＝幀太早到）；legacy 為 slack_ema − target
    double integral_ppm = 0;  ///< 積分項（ppm）
    uint64_t slips = 0;  ///< 相位滑移次數（相鄰兩筆新量測跳超過 kSlipPeriods 個週期）
    uint64_t dropped = 0;  ///< v2 丟掉的樣本（重複 frameId、離群值）
    mode_e mode = mode_e::legacy;
  };

  class controller_t {
  public:
    /**
     * @brief session 開始時設定（config：vr_latch_mode、vr_latch_target_pct）。不呼叫＝legacy。
     * @param host_period_ns 協商的 host 虛擬 vsync 週期（0＝不知道）；v2 的模數取它與顯示週期的較小值
     */
    void configure(mode_e mode, int target_pct, uint32_t host_period_ns);

    /**
     * @brief 處理一則 LATCH。
     * @param slack_us LATCH.slackUs
     * @param display_period_ns LATCH.displayPeriodNs（0 或不合理時沿用上次，預設 11.1 ms）
     * @param now_ns 單調時鐘（ns）
     * @param frame_id LATCH.frameId（v2：與上一筆相同＝client 沒有新幀、重送舊值，不當新量測）
     */
    result_t on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns, uint32_t frame_id = 0);

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

    /// 目標 slack：顯示週期的 1/4，夾在 [1, 4] ms（legacy）
    static double target_for_period(uint32_t display_period_ns);

    // §VR-LATCH-V2 參數
    static constexpr double kKpV2 = 0.12;  ///< ppm／µs
    static constexpr double kKiV2 = 0.008;  ///< ppm／(µs·s)
    static constexpr double kOutlierPeriods = 1.5;  ///< |slack| 超過 1.5 個週期的樣本丟掉
    static constexpr double kSlipPeriods = 0.6;  ///< 相鄰兩筆新量測跳超過 0.6 個週期＝一次相位滑移
    static constexpr double kPeriodChange = 0.02;  ///< 顯示週期變化超過 2%（換更新率）就重來
    static constexpr int kTargetPctMin = 25;
    static constexpr int kTargetPctMax = 50;
    static constexpr int kTargetPctDefault = 40;

    mode_e mode() const {
      return mode_;
    }

    /// v2 的模數（µs）：host 虛擬 vsync 週期與顯示週期的較小值
    double modulus_us() const;

    /// v2 的目標 slack（µs）
    double target_v2_us() const;

  private:
    result_t decide(int64_t now_ns, int32_t ppm);
    result_t on_latch_legacy(int32_t slack_us, int64_t now_ns);
    result_t on_latch_v2(int32_t slack_us, int64_t now_ns, uint32_t frame_id);
    void fill_observability(result_t &r) const;
    void count_slip(double slack_us, double period_us);

    // 設定（reset() 保留）
    mode_e mode_ = mode_e::legacy;
    int target_pct_ = kTargetPctDefault;
    uint32_t host_period_ns_ = 0;

    bool have_ema_ = false;
    double ema_ = 0;  ///< legacy：slack 的 EMA；v2：誤差的 EMA
    double integral_ = 0;
    uint32_t period_ns_ = 11'111'111;
    int64_t last_latch_ns_ = 0;
    int64_t last_sent_ns_ = 0;
    int32_t sent_ppm_ = 0;
    bool sent_once_ = false;
    uint64_t count_ = 0;

    bool have_prev_slack_ = false;
    double prev_slack_us_ = 0;
    uint64_t slips_ = 0;
    uint64_t dropped_ = 0;
    bool have_frame_ = false;
    uint32_t last_frame_id_ = 0;
  };

}  // namespace vr::latch
