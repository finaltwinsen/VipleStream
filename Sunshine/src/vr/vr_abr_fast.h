/**
 * @file src/vr/vr_abr_fast.h
 * @brief VipleStream 2.0 §ABR-ADAPT：VR 自適應快速碼率控制（第 1、2、3 期：純邏輯＋單元測試）。
 *
 * 專為 VR 高速動態環境設計（流暢度第一、寧可糊不要卡）。依環境自適應：
 *   - 延遲趨勢（delay_trend_t）：自適應門檻 γ 追蹤排隊延遲膨脹，進入 overuse 即快速降碼。
 *   - 單向延遲（on_owd）：2 秒內有樣本時優先以 owd_trend_ 判定壅塞、降碼與暫停回升。
 *   - 實際送達速率（on_rates）：鏈路飽和（send >= recv * 1.05）時，壅塞降碼一步落至 clamp(0.95 * recv / send, 0.5, 0.85)。
 *     §ABR-SHORTFALL（11-05）：送出比收到多 15% 以上連續 2 次回報（約 1 秒）＝已經在排隊，直接降（原因 rate-shortfall+delay），
 *     不等延遲趨勢累積（模擬頭盔實測：送 84.8／收 58.6 Mbps 時單向延遲 p95 83 ms，趨勢要約 10 秒才判 overuse）。
 *   - 畫質飽和（on_quality）：H.264/HEVC QP <= 22 時暫停回升；連續 10 秒 QP <= 17 時每 3 秒小幅回收 5%（quality-trim）。
 *   - 解碼負載（on_decode）：p95 > 0.85 * period 時暫停回升（有效 3 秒）；連 2 筆 > 1.0 * period 觸發 decode-overload 中度降碼。
 *   - 丟包分類：
 *     * 延遲過載時丟包（或剛脫離過載 1.5 秒內）-> 視為網路壅塞降碼（原因加註 +delay）。
 *     * 延遲平穩時零星丟包 -> 視為無線隨機丟包，不降碼，累計至 random_losses()。
 *     * 延遲平穩但 3 秒內嚴重累積丟包 >= 12 -> 例外判定為持續送不動，降碼並加註 +persistent。
 *   - 下限：k_floor_tech_kbps = 2000 kbps（技術最低值），不超過設定值。
 *   - 冷卻與回升隨 base RTT 動態調整：
 *     * HOLD 冷卻長度 = clamp(8 * base_rtt, 600 ms, 2000 ms)。
 *     * RAMP 回升：applied < 0.8 * last_congested 時採乘法快速探頂（每秒約 +8%）；接近時改加法慢爬（每秒 +0.5%），
 *       超過上次塞車點後更慢（每秒 +0.25%）。上次塞車點只記同一次塞車的第一刀（5 秒內的後續刀不覆寫），
 *       且 30 秒沒再塞車後，慢爬的上界每秒放寬 0.1%（最多 +50%，§ABR-RATCHET），避免一路往下記。
 *   - 丟包率預算（§ABR-LOSSRATE，11-04 桌球實測）：延遲沒升的「隨機」丟包若 20 秒內 >= 4 次且橫跨至少 10 秒（約每 10 秒 2 張救不回），
 *     代表丟包跟碼率有關（20 Mbps 幾乎不丟、75 Mbps 每 10 秒 2 張）→ 降 10% 並把上限暫壓在降後的值 60 秒；
 *     低於 10 Mbps 不再因此降（那種丟包多半是無線環境本身，降碼救不了）。
 *     * 延遲趨勢過載或 trend > 0.5γ、解碼吃緊、畫質飽和時暫停回升。
 *   - 防振盪懲罰：RAMP 中 5 秒內再次降碼 -> 上限暫壓至 applied * 0.85 維持 30 秒。
 *
 * 純邏輯、無 I/O、無 log、無全域狀態，可單元測試。呼叫端（stream.cpp）持 abrMutex 加鎖呼叫並負責記錄 log。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>

#include "abr_delay_trend.h"

namespace vr::abr {

  using steady = std::chrono::steady_clock;

  // 狀態定義
  enum class fast_state_e {
    healthy,  ///< 健康狀態
    cut,      ///< 降碼狀態（當輪有降碼事件生效）
    hold,     ///< 冷卻冷凍期（禁止回升）
    ramp,     ///< 平滑回升期
  };

  inline const char *fast_state_name(fast_state_e s) {
    switch (s) {
      case fast_state_e::healthy: return "healthy";
      case fast_state_e::cut:     return "cut";
      case fast_state_e::hold:    return "hold";
      case fast_state_e::ramp:    return "ramp";
    }
    return "unknown";
  }

  // 常數定義
  constexpr int k_floor_tech_kbps = 2000;                                    ///< 技術最低碼率 2000 Kbps (2 Mbps)
  constexpr auto k_fast_cut_window = std::chrono::milliseconds(1000);        ///< 嚴重降碼同秒判定視窗 1 秒
  constexpr auto k_fast_anti_osc_window = std::chrono::milliseconds(5000);   ///< 防振盪 RAMP 觸發視窗 5 秒
  constexpr auto k_fast_anti_osc_duration = std::chrono::seconds(30);        ///< 防振盪懲罰維持 30 秒
  constexpr float k_fast_anti_osc_cap_factor = 0.85f;                        ///< 防振盪懲罰上限係數 applied × 0.85
  constexpr auto k_fast_persistent_window = std::chrono::milliseconds(3000); ///< 持續丟包判定視窗 3 秒
  constexpr int k_fast_persistent_threshold = 12;                            ///< 持續丟包權重門檻 12
  constexpr auto k_fast_delay_overuse_recent_window = std::chrono::milliseconds(1500); ///< 丟包分類之 delay 關聯視窗 1.5 秒

  // §ABR-ADAPT 第 2、3 期常數
  constexpr auto k_fast_owd_active_window = std::chrono::milliseconds(2000);   ///< OWD 樣本有效視窗 2 秒
  constexpr auto k_fast_rates_fresh_window = std::chrono::milliseconds(1000);  ///< 送達速率有效視窗 1 秒
  constexpr double k_fast_rates_saturated_ratio = 1.05;                        ///< 鏈路飽和判定倍數 1.05
  constexpr double k_fast_shortfall_ratio = 1.15;    ///< §ABR-SHORTFALL：送出 ≥ 收到 × 1.15 才算排隊（避開無線零星掉包的小落差）
  constexpr int k_fast_shortfall_reports = 2;        ///< 連續幾次回報（每 500 ms 一次）
  constexpr int k_fast_shortfall_min_kbps = 5000;    ///< 送出太少時不判（量測誤差大）
  constexpr auto k_fast_quality_recent_window = std::chrono::milliseconds(1500); ///< QP 飽和判定視窗 1.5 秒
  constexpr auto k_fast_quality_trim_duration = std::chrono::seconds(10);      ///< quality-trim 連續門檻 10 秒
  constexpr auto k_fast_quality_trim_interval = std::chrono::seconds(3);       ///< quality-trim 間隔 3 秒
  constexpr int k_fast_qp_saturated_x100 = 2000;  ///< 畫質飽和門檻 QP 20（×100）；11-04 實測 22 對 VR 偏鬆（鏡片放大壓縮瑕疵）
  constexpr int k_fast_qp_trim_threshold_x100 = 1500;  ///< 畫質多到用不完的門檻 QP 15（×100）
  constexpr auto k_fast_decode_tight_duration = std::chrono::seconds(3);       ///< 解碼吃緊有效時間 3 秒

  // §ABR-LOSSRATE／慢爬常數（11-04）
  constexpr auto k_fast_congest_episode = std::chrono::seconds(5);         ///< 同一次塞車的視窗：5 秒內後續刀不覆寫塞車點
  constexpr auto k_fast_loss_rate_window = std::chrono::seconds(20);       ///< 丟包率視窗 20 秒
  constexpr int k_fast_loss_rate_threshold = 4;                            ///< 視窗內隨機丟包次數門檻
  constexpr auto k_fast_loss_rate_cap_duration = std::chrono::seconds(60); ///< 降後上限維持 60 秒
  constexpr int k_fast_loss_rate_min_kbps = 10000;                         ///< 低於此值不再因丟包率降碼

  class fast_controller_t {
  public:
    enum class severity_e {
      moderate,
      fast,
    };

    explicit fast_controller_t(int configured_kbps)
      : configured_kbps_(std::max(1, configured_kbps)),
        cap_kbps_(std::max(1, configured_kbps)),
        applied_kbps_(std::max(1, configured_kbps)),
        current_target_kbps_(std::max(1, configured_kbps)) {}

    void set_configured_kbps(int configured_kbps) {
      if (configured_kbps > 0) {
        configured_kbps_ = configured_kbps;
        if (cap_kbps_ > configured_kbps_) {
          cap_kbps_ = configured_kbps_;
        }
      }
    }

    void set_cap_kbps(int cap_kbps) {
      if (cap_kbps > 0) {
        cap_kbps_ = std::min(configured_kbps_, cap_kbps);
      }
    }

    void set_applied_kbps(int applied_kbps) {
      if (applied_kbps > 0) {
        applied_kbps_ = applied_kbps;
      }
    }

    int configured_kbps() const { return configured_kbps_; }
    int cap_kbps() const { return cap_kbps_; }
    int applied_kbps() const { return applied_kbps_; }
    int current_target_kbps() const { return current_target_kbps_; }
    fast_state_e state() const { return state_; }
    const char *state_str() const { return fast_state_name(state_); }
    const char *last_reason() const { return last_reason_.c_str(); }
    int cuts() const { return cuts_count_; }
    int ramps() const { return ramps_count_; }
    int trims() const { return trims_count_; }
    int random_losses() const { return random_loss_events_; }
    int loss_rate_cuts() const { return loss_rate_cuts_; }
    int last_congested_kbps() const { return last_congested_kbps_; }
    /// 塞車點隨「多久沒再塞車」往上放寬後的值（§ABR-RATCHET）；log 與回升分區用
    int effective_congested_kbps() const { return eff_congested_kbps(); }
    const delay_trend_t &trend() const { return trend_; }
    const delay_trend_t &owd_trend() const { return owd_trend_; }
    bool owd_active(steady::time_point now) const {
      if (last_owd_time_ == steady::time_point{}) return false;
      if (now < last_owd_time_) return true;
      return (now - last_owd_time_) <= k_fast_owd_active_window;
    }
    int recv_kbps() const { return recv_kbps_; }

    /// §ABR-ADAPT（審查）：延遲沒上升的成串丟包要降碼，只限「真的送不動」：有新鮮的收／送速率時必須是鏈路飽和
    /// （送出 ≥ 收到 × 1.05）；成串缺包但收得到（無線空檔）交給 FEC／補包。沒有速率資料（舊 client）時照舊用累計門檻
    bool persistent_allowed(steady::time_point now) const {
      const bool fresh = last_rates_time_ != steady::time_point {} && now >= last_rates_time_ && now - last_rates_time_ <= k_fast_rates_fresh_window &&
                         send_kbps_ > 0;
      if (!fresh) {
        return true;
      }
      return static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_rates_saturated_ratio;
    }
    int send_kbps() const { return send_kbps_; }

    /// 回傳目前是否處於網路壅塞（或剛脫離過載 1.5 秒內），與內部 is_congested(now) 相同
    bool congested(steady::time_point now) const {
      return is_congested(now);
    }

    bool quality_saturated(steady::time_point now) const {
      if (last_quality_time_ == steady::time_point{}) return false;
      if (now < last_quality_time_ || (now - last_quality_time_ > k_fast_quality_recent_window)) {
        return false;
      }
      if (last_codec_ == 2) {
        return false;  // AV1 先不判斷，一律視為未飽和
      }
      return last_qp_x100_ <= k_fast_qp_saturated_x100;
    }

    const char *ramp_block_reason(steady::time_point now) const {
      const auto &t = owd_active(now) ? owd_trend_ : trend_;
      // §ABR-ADAPT（審查）：underuse＝佇列正在排空，也要等它回到 normal 再加碼（設計第 5 節「排空中維持」）
      if (t.state() == trend_state_e::overuse || t.state() == trend_state_e::underuse || t.trend_ms() > 0.5 * t.gamma_ms()) {
        return "delay";
      }
      if (decode_tight_until_ != steady::time_point{} && now <= decode_tight_until_) {
        return "decode";
      }
      if (quality_saturated(now)) {
        return "quality";
      }
      return "none";
    }

    int floor_kbps() const {
      const int f = k_floor_tech_kbps;
      // §REVIEW-1103（B039-2）：設定碼率比技術最低值更低時，下限不可超過設定值
      return configured_kbps_ > 0 ? std::min(f, configured_kbps_) : f;
    }

    int effective_cap_kbps(steady::time_point now) const {
      int cap = cap_kbps_;
      if (penalty_until_ != steady::time_point {} && now < penalty_until_) {
        cap = std::min(cap, penalty_cap_);
      }
      if (loss_cap_until_ != steady::time_point {} && now < loss_cap_until_) {
        cap = std::min(cap, loss_cap_);
      }
      return std::max(floor_kbps(), cap);
    }

    /// 接收 RTT 回報，餵給 delay_trend_t 估計器；進入 overuse 時觸發降碼（OWD 可用時不觸發）
    void on_rtt(int rtt_ms, steady::time_point now) {
      if (rtt_ms < 0) return;
      // §ABR-ADAPT（審查）：次毫秒 LAN 上 ENet 的 RTT 是 0，這是合法樣本（delay_trend 只收正值，用 0.5 ms 代表）
      const auto prev_state = trend_.state();
      trend_.on_rtt(rtt_ms == 0 ? 0.5 : static_cast<double>(rtt_ms), now);
      const auto cur_state = trend_.state();

      if (!owd_active(now)) {
        if (cur_state == trend_state_e::overuse) {
          if (prev_state != trend_state_e::overuse) {
            trigger_cut(severity_e::fast, "delay-trend", now);
            last_delay_trend_cut_time_ = now;
          } else if (last_delay_trend_cut_time_ == steady::time_point{} ||
                     now - last_delay_trend_cut_time_ >= std::chrono::milliseconds(1000)) {
            trigger_cut(severity_e::fast, "delay-trend", now);
            last_delay_trend_cut_time_ = now;
          }
        }
      }
    }

    /// 接收單向延遲（OWD）回報，餵給 owd_trend_ 估計器；進入 overuse 時觸發降碼
    void on_owd(double owd_ms, steady::time_point now) {
      if (!std::isfinite(owd_ms)) return;
      if (!owd_have_offset_) {
        owd_have_offset_ = true;
        owd_offset_ms_ = owd_ms;
      }
      // 扣除首筆固定時鐘偏移並平移 +1000 ms，確保餵給 delay_trend_t 的相對值恆為正數
      // §ABR-ADAPT（審查）：餘裕 10 s——第一筆若是慢的（大 IDR、開場擁塞），之後較快的幀不會被夾成同一個值
      const double rel_owd_ms = std::max(1.0, (owd_ms - owd_offset_ms_) + 10000.0);
      last_owd_time_ = now;
      const auto prev_state = owd_trend_.state();
      owd_trend_.on_rtt(rel_owd_ms, now);
      const auto cur_state = owd_trend_.state();

      if (cur_state == trend_state_e::overuse) {
        if (prev_state != trend_state_e::overuse) {
          trigger_cut(severity_e::fast, "owd-trend", now);
          last_owd_trend_cut_time_ = now;
        } else if (last_owd_trend_cut_time_ == steady::time_point{} ||
                   now - last_owd_trend_cut_time_ >= std::chrono::milliseconds(1000)) {
          trigger_cut(severity_e::fast, "owd-trend", now);
          last_owd_trend_cut_time_ = now;
        }
      }
    }

    /// 接收實際送達速率回報（每 500 ms 一次，單位 kbps）
    void on_rates(int recv_kbps, int send_kbps, steady::time_point now) {
      recv_kbps_ = std::max(0, recv_kbps);
      send_kbps_ = std::max(0, send_kbps);
      last_rates_time_ = now;
      // §ABR-SHORTFALL：收到的明顯少於送出＝鏈路已經塞住在排隊，直接降（降幅由 trigger_cut 依收／送比算）
      if (send_kbps_ >= k_fast_shortfall_min_kbps &&
          static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_shortfall_ratio) {
        if (++shortfall_reports_ >= k_fast_shortfall_reports) {
          shortfall_reports_ = 0;
          trigger_cut(severity_e::moderate, "rate-shortfall+delay", now);
        }
      } else {
        shortfall_reports_ = 0;
      }
    }

    /// 接收畫質回報（每 500 ms 一次，codec：0=H.264、1=HEVC、2=AV1）
    void on_quality(int avg_qp_x100, int codec, steady::time_point now) {
      if (avg_qp_x100 < 0) return;  // 沒有資料、忽略

      const bool gap_too_long = (last_quality_time_ != steady::time_point{} &&
                                 now > last_quality_time_ &&
                                 (now - last_quality_time_ > k_fast_quality_recent_window));
      last_quality_time_ = now;
      last_qp_x100_ = avg_qp_x100;
      last_codec_ = codec;

      // 檢查是否符合 quality-trim 條件（codec != 2 且 QP <= 17）
      if (codec != 2 && avg_qp_x100 <= k_fast_qp_trim_threshold_x100) {
        if (qp_low_start_ == steady::time_point{} || gap_too_long) {
          qp_low_start_ = now;
          last_trim_time_ = {};
        }
      } else {
        qp_low_start_ = {};
        last_trim_time_ = {};
      }
    }

    /// 接收 client 解碼負載回報（每 1 s 一次）
    void on_decode(int decode_p95_us, int frame_period_us, steady::time_point now) {
      if (frame_period_us <= 0) return;

      // p95 > 0.85 × period 視為解碼吃緊，有效 3 秒
      if (static_cast<double>(decode_p95_us) > 0.85 * static_cast<double>(frame_period_us)) {
        decode_tight_until_ = now + k_fast_decode_tight_duration;
      } else {
        decode_tight_until_ = {};
      }

      // 連續 2 筆 p95 > 1.0 × period 觸發 moderate 降碼，原因 decode-overload
      if (static_cast<double>(decode_p95_us) > 1.0 * static_cast<double>(frame_period_us)) {
        ++consecutive_decode_overload_;
        if (consecutive_decode_overload_ >= 2) {
          trigger_cut(severity_e::moderate, "decode-overload", now);
          consecutive_decode_overload_ = 0;
        }
      } else {
        consecutive_decode_overload_ = 0;
      }
    }

    /// FEC 救不回回報（丟包分類處理）
    void on_fec_failed(int n, steady::time_point now) {
      if (n <= 0) return;
      clean_old_fec_fails(now);
      fec_fails_.push_back({n, now});
      int total = 0;
      for (const auto &item : fec_fails_) {
        total += item.count;
      }

      if (is_congested(now)) {
        if (total >= 2) {
          trigger_cut(severity_e::fast, "fec-failed-ge-2+delay", now);
        } else if (total == 1) {
          trigger_cut(severity_e::moderate, "fec-failed-1+delay", now);
        }
      } else {
        // 延遲平穩時視為隨機丟包，累計至 persistent 評估
        clean_old_persistent_losses(now);
        persistent_losses_.push_back({n, now});
        int p_sum = get_persistent_loss_sum();
        if (p_sum >= k_fast_persistent_threshold && persistent_allowed(now)) {
          persistent_losses_.clear();
          if (total >= 2) {
            trigger_cut(severity_e::fast, "fec-failed-ge-2+persistent", now);
          } else {
            trigger_cut(severity_e::moderate, "fec-failed-1+persistent", now);
          }
        } else {
          note_random_frame_loss(n, now);
        }
      }
    }

    /// LOSS 封包丟失回報（丟包分類處理）
    void on_loss(uint32_t lost_frames, steady::time_point now) {
      if (lost_frames >= 2) {
        if (is_congested(now)) {
          trigger_cut(severity_e::fast, "loss-ge-2+delay", now);
        } else {
          clean_old_persistent_losses(now);
          persistent_losses_.push_back({static_cast<int>(lost_frames), now});
          int p_sum = get_persistent_loss_sum();
          if (p_sum >= k_fast_persistent_threshold && persistent_allowed(now)) {
            persistent_losses_.clear();
            trigger_cut(severity_e::fast, "loss-ge-2+persistent", now);
          } else {
            note_random_frame_loss(static_cast<int>(lost_frames), now);
          }
        }
      }
    }

    /// 送影像連線 dropBlock 回報（算 3 權重）
    void on_drop_block(steady::time_point now) {
      if (is_congested(now)) {
        trigger_cut(severity_e::fast, "drop-block+delay", now);
      } else {
        clean_old_persistent_losses(now);
        persistent_losses_.push_back({3, now});
        int p_sum = get_persistent_loss_sum();
        if (p_sum >= k_fast_persistent_threshold && persistent_allowed(now)) {
          persistent_losses_.clear();
          trigger_cut(severity_e::fast, "drop-block+persistent", now);
        } else {
          ++random_loss_events_;
        }
      }
    }

    /// LATCH 連續 2 筆 slack < −1 ms -> 中度 (MODERATE)，不改 latch-slack-late 行為
    void on_latch_slack(int64_t slack_us, steady::time_point now) {
      if (slack_us < -1000) {
        ++consecutive_late_slack_;
        if (consecutive_late_slack_ >= 2) {
          trigger_cut(severity_e::moderate, "latch-slack-late", now);
          consecutive_late_slack_ = 0;
        }
      } else {
        consecutive_late_slack_ = 0;
      }
    }

    /// 主連線送達率 < 90% 回報（丟包分類處理）
    void on_delivery(double pct, steady::time_point now) {
      if (pct >= 0.0 && pct < 90.0) {
        if (is_congested(now)) {
          trigger_cut(severity_e::moderate, "delivery-low+delay", now);
        } else {
          clean_old_persistent_losses(now);
          persistent_losses_.push_back({1, now});
          int p_sum = get_persistent_loss_sum();
          if (p_sum >= k_fast_persistent_threshold && persistent_allowed(now)) {
            persistent_losses_.clear();
            trigger_cut(severity_e::moderate, "delivery-low+persistent", now);
          } else {
            ++random_loss_events_;
          }
        }
      }
    }

    /// 推進狀態機，返回新目標碼率（若無調整則為 std::nullopt）
    std::optional<int> tick(steady::time_point now) {
      last_tick_ = now;
      // 1. 若有 pending 的降碼要求，立刻回傳並轉入 HOLD 冷卻
      if (pending_target_.has_value()) {
        const int target = *pending_target_;
        pending_target_.reset();
        applied_kbps_ = target;
        state_ = fast_state_e::hold;
        return target;
      }

      // 1b. 上限往下調時立刻壓到上限
      if (const int eff_cap = effective_cap_kbps(now); applied_kbps_ > eff_cap) {
        applied_kbps_ = eff_cap;
        current_target_kbps_ = eff_cap;
        last_reason_ = "cap";
        return eff_cap;
      }

      // 1c. 上限放寬而目前在 HEALTHY、低於上限且未畫質飽和 -> 進 RAMP 慢慢爬回去
      if (state_ == fast_state_e::healthy && applied_kbps_ < effective_cap_kbps(now) && !quality_saturated(now)) {
        state_ = fast_state_e::ramp;
        ramp_start_ = {};
        next_ramp_ = now + get_ramp_interval();
        current_target_kbps_ = applied_kbps_;
      }

      // 2. 若處於 CUT，進入 HOLD
      if (state_ == fast_state_e::cut) {
        state_ = fast_state_e::hold;
      }

      // 3. 冷卻觀測期（HOLD）：未滿 hold_duration 禁止回升
      if (state_ == fast_state_e::hold) {
        if (now >= hold_until_) {
          state_ = fast_state_e::ramp;
          ramp_start_ = now;
          next_ramp_ = now + get_ramp_interval();
        } else {
          return std::nullopt;
        }
      }

      // 3b. 畫質多到用不完時小幅回收（quality-trim，省流量）
      // 連續 >= 10 s 的 QP 樣本都 <= 17 且狀態為 HEALTHY 或 RAMP -> 每 3 s 降 5%
      if ((state_ == fast_state_e::healthy || state_ == fast_state_e::ramp) && check_quality_trim_eligible(now)) {
        if (last_trim_time_ == steady::time_point{} || now - last_trim_time_ >= k_fast_quality_trim_interval) {
          const int eff_floor = floor_kbps();
          int new_target = static_cast<int>(std::round(applied_kbps_ * 0.95));
          new_target = std::max(eff_floor, new_target);
          if (new_target < applied_kbps_) {
            last_trim_time_ = now;
            applied_kbps_ = new_target;
            current_target_kbps_ = new_target;
            last_reason_ = "quality-trim";
            ++trims_count_;
            return new_target;
          }
        }
      }

      // 4. 平滑回升期（RAMP）
      if (state_ == fast_state_e::ramp) {
        if (now >= next_ramp_) {
          const int eff_cap = effective_cap_kbps(now);
          if (applied_kbps_ >= eff_cap) {
            current_target_kbps_ = eff_cap;
            applied_kbps_ = eff_cap;
            state_ = fast_state_e::healthy;
            return std::nullopt;
          }

          // 依優先序檢查是否被擋住（delay / decode / quality）
          const char *block = ramp_block_reason(now);
          if (std::strcmp(block, "none") != 0) {
            next_ramp_ = now + get_ramp_interval();
            return std::nullopt;
          }

          const auto interval = get_ramp_interval();
          const int step = calculate_ramp_step(interval);
          int new_target = applied_kbps_ + step;
          if (new_target >= eff_cap) {
            new_target = eff_cap;
            state_ = fast_state_e::healthy;
          }
          current_target_kbps_ = new_target;
          applied_kbps_ = new_target;
          last_reason_ = "ramp-clean";
          next_ramp_ = now + get_ramp_interval();
          ++ramps_count_;
          return new_target;
        }
      }

      return std::nullopt;
    }

  private:
    struct fec_record_t {
      int count;
      steady::time_point at;
    };

    struct loss_record_t {
      int weight;
      steady::time_point at;
    };

    bool is_congested(steady::time_point now) const {
      const auto &t = owd_active(now) ? owd_trend_ : trend_;
      return t.state() == trend_state_e::overuse ||
             t.overuse_recently(now, k_fast_delay_overuse_recent_window);
    }

    bool check_quality_trim_eligible(steady::time_point now) const {
      if (qp_low_start_ == steady::time_point{}) return false;
      if (last_quality_time_ == steady::time_point{}) return false;
      if (now < last_quality_time_ || (now - last_quality_time_ > k_fast_quality_recent_window)) {
        return false;
      }
      return (now >= qp_low_start_) && (now - qp_low_start_ >= k_fast_quality_trim_duration);
    }

    void clean_old_fec_fails(steady::time_point now) {
      while (!fec_fails_.empty() && now - fec_fails_.front().at > k_fast_cut_window) {
        fec_fails_.pop_front();
      }
    }

    void clean_old_persistent_losses(steady::time_point now) {
      while (!persistent_losses_.empty() && now - persistent_losses_.front().at > k_fast_persistent_window) {
        persistent_losses_.pop_front();
      }
    }

    int get_persistent_loss_sum() const {
      int sum = 0;
      for (const auto &item : persistent_losses_) {
        sum += item.weight;
      }
      return sum;
    }

    std::chrono::milliseconds get_hold_duration() const {
      double base = trend_.base_ms();
      if (base <= 0.0) {
        return std::chrono::milliseconds(2000);
      }
      int hold_ms = std::clamp(static_cast<int>(std::round(8.0 * base)), 600, 2000);
      return std::chrono::milliseconds(hold_ms);
    }

    bool is_multiplicative_ramp() const {
      return last_congested_kbps_ == 0 || applied_kbps_ < static_cast<int>(0.8 * last_congested_kbps_);
    }

    std::chrono::milliseconds get_ramp_interval() const {
      double base = trend_.base_ms();
      int base_int = (base > 0.0) ? static_cast<int>(std::round(4.0 * base)) : 250;
      int interval_ms = std::clamp(base_int, 250, 1000);
      if (!is_multiplicative_ramp()) {
        interval_ms = 1000;  // 接近／超過上次塞車點：每秒最多一步
      }
      return std::chrono::milliseconds(interval_ms);
    }

    int calculate_ramp_step(std::chrono::milliseconds interval) const {
      if (is_multiplicative_ramp()) {
        double sec = interval.count() / 1000.0;
        int step = static_cast<int>(std::round(applied_kbps_ * 0.08 * sec));
        return std::max(50, step);  // §ABR-ADAPT（審查）：窄路徑上 200 kbps 下限會讓每秒回升遠超過 8%
      } else {
        // 11-04 桌球實測：舊的每 0.5 s +2%（約 4%/s）9 秒就從 54 衝回 75 Mbps 再塞一次，形成鋸齒、每輪丟幾張
        const int lc = eff_congested_kbps();
        const double frac = (lc > 0 && applied_kbps_ >= lc) ? 0.0025 : 0.005;
        int step = static_cast<int>(std::round(applied_kbps_ * frac));
        return std::max(50, step);
      }
    }

    // §ABR-RATCHET（11-04 第 3 場桌球）：每次塞車都把塞車點記成當下（較低）的碼率，網路一時變差就一路往下記
    // （67→59→55→52→50 Mbps），之後只能在越來越低的點附近慢爬。改成：30 秒沒再塞車後，「超過塞車點就再減半」的
    // 那條界線每秒往上放寬 0.1%（最多 +50%）。乘法快爬的分界仍用原本的塞車點（不然塞車後幾秒就又全速衝回去）。
    // 真的塞車時照樣重新記。
    int eff_congested_kbps() const {
      if (last_congested_kbps_ <= 0 || last_congested_time_ == steady::time_point {} || last_tick_ <= last_congested_time_) {
        return last_congested_kbps_;
      }
      const double quiet_s = std::chrono::duration<double>(last_tick_ - last_congested_time_).count() - 30.0;
      if (quiet_s <= 0.0) {
        return last_congested_kbps_;
      }
      return static_cast<int>(last_congested_kbps_ * std::min(1.5, 1.0 + 0.001 * quiet_s));
    }

    void note_congested(int kbps, steady::time_point now) {
      if (last_congested_time_ != steady::time_point {} && now - last_congested_time_ <= k_fast_congest_episode) {
        last_congested_kbps_ = std::max(last_congested_kbps_, kbps);
      } else {
        last_congested_kbps_ = kbps;
      }
      last_congested_time_ = now;
    }

    /// 延遲平穩時救不回的幀：照舊算隨機丟包，但 20 秒內太多次就視為跟碼率有關（§ABR-LOSSRATE）
    void note_random_frame_loss(int n, steady::time_point now) {
      ++random_loss_events_;
      while (!loss_rate_events_.empty() && now - loss_rate_events_.front() > k_fast_loss_rate_window) {
        loss_rate_events_.pop_front();
      }
      (void) n;  // 一筆回報算一次：要的是「多常出事」，同一筆裡幾張由 persistent 規則處理
      loss_rate_events_.push_back(now);
      if (static_cast<int>(loss_rate_events_.size()) < k_fast_loss_rate_threshold) return;
      // 要「持續」：最早一筆至少 10 秒前，短時間的密集丟包交給 persistent／延遲規則
      if (now - loss_rate_events_.front() < k_fast_loss_rate_window / 2) return;
      if (applied_kbps_ <= k_fast_loss_rate_min_kbps) return;
      if (now < hold_until_ || pending_target_.has_value()) return;  // 剛降過，等它生效再看
      loss_rate_events_.clear();
      const int before = applied_kbps_;
      trigger_cut(severity_e::moderate, "loss-rate", now);
      if (pending_target_.has_value()) {
        // trigger_cut 的中度降碼＝×0.90；不要因為這條規則掉到 10 Mbps 以下
        pending_target_ = std::max(*pending_target_, std::min(before, k_fast_loss_rate_min_kbps));
        current_target_kbps_ = *pending_target_;
        loss_cap_ = *pending_target_;
        loss_cap_until_ = now + k_fast_loss_rate_cap_duration;
        note_congested(before, now);
        ++loss_rate_cuts_;
      }
    }

    void trigger_cut(severity_e sev, const char *reason, steady::time_point now) {
      // 降碼後的冷卻期（hold_until_ 之前）只接受嚴重訊號：送達率每 250 ms、LATCH 每 100 ms 就回報一次，
      // 中度訊號不擋的話一兩秒內會連降好幾次直接壓到下限（HOLD 期間只有嚴重訊號能二度打折）
      if (sev == severity_e::moderate && now < hold_until_) {
        return;
      }

      // 防振盪：RAMP 中 5 秒內又降碼 -> 上限暫時壓到 applied × 0.85 維持 30 秒
      if (state_ == fast_state_e::ramp && ramp_start_ != steady::time_point {} &&
          now - ramp_start_ <= k_fast_anti_osc_window) {
        penalty_cap_ = std::max(floor_kbps(), static_cast<int>(applied_kbps_ * k_fast_anti_osc_cap_factor));
        penalty_until_ = now + k_fast_anti_osc_duration;
      }

      // 記住最後一次因 delay / persistent / owd 降碼前的 applied
      // 11-04：同一次塞車常連降兩刀（例：owd-trend 75→63.5，1.6 s 後 fec-failed+delay 63.5→54），
      // 舊寫法第二刀把塞車點覆寫成 63.5，回升時就不知道真正塞住的是 75。5 秒內的後續刀只取較大值。
      if (std::strstr(reason, "delay") != nullptr ||
          std::strstr(reason, "persistent") != nullptr ||
          std::strstr(reason, "owd") != nullptr) {
        note_congested(applied_kbps_, now);
      }

      int new_target = applied_kbps_;
      bool is_delay_trend = (std::strcmp(reason, "delay-trend") == 0 || std::strcmp(reason, "owd-trend") == 0);
      bool is_congestion_cut = (is_delay_trend ||
                                std::strstr(reason, "+delay") != nullptr ||
                                std::strstr(reason, "+persistent") != nullptr);

      // 檢查鏈路是否飽和（1 秒內有新鮮 rates 且 send_kbps >= recv_kbps * 1.05）
      bool has_saturated_rates = false;
      double saturated_factor = 0.85;
      if (is_congestion_cut &&
          last_rates_time_ != steady::time_point{} &&
          now >= last_rates_time_ &&
          (now - last_rates_time_ <= k_fast_rates_fresh_window) &&
          send_kbps_ > 0 &&
          recv_kbps_ >= 0 &&
          static_cast<double>(send_kbps_) >= static_cast<double>(recv_kbps_) * k_fast_rates_saturated_ratio) {
        has_saturated_rates = true;
        double raw_f = 0.95 * static_cast<double>(recv_kbps_) / static_cast<double>(send_kbps_);
        saturated_factor = std::clamp(raw_f, 0.50, 0.85);
      }

      if (sev == severity_e::fast) {
        if (last_fast_cut_time_ != steady::time_point {} && now - last_fast_cut_time_ <= k_fast_cut_window) {
          // 同一秒內第二次嚴重降碼
          if (has_saturated_rates) {
            double factor = std::min(saturated_factor, 0.70);
            new_target = static_cast<int>(std::round(fast_cut_base_applied_ * factor));
          } else {
            float factor = is_delay_trend ? 0.70f : 0.60f;
            new_target = static_cast<int>(fast_cut_base_applied_ * factor);
          }
        } else {
          // 視窗內第一次嚴重降碼
          fast_cut_base_applied_ = applied_kbps_;
          if (has_saturated_rates) {
            new_target = static_cast<int>(std::round(applied_kbps_ * saturated_factor));
          } else {
            float factor = is_delay_trend ? 0.85f : 0.75f;
            new_target = static_cast<int>(applied_kbps_ * factor);
          }
          last_fast_cut_time_ = now;
        }
      } else {
        // 中度降碼
        if (has_saturated_rates) {
          new_target = static_cast<int>(std::round(applied_kbps_ * saturated_factor));
        } else {
          new_target = static_cast<int>(applied_kbps_ * 0.90f);
        }
      }

      const int eff_cap = effective_cap_kbps(now);
      const int eff_floor = floor_kbps();
      new_target = std::clamp(new_target, eff_floor, eff_cap);

      state_ = fast_state_e::cut;
      last_reason_ = reason;
      pending_target_ = new_target;
      current_target_kbps_ = new_target;
      hold_until_ = now + get_hold_duration();
      ++cuts_count_;
    }

    int configured_kbps_ = 0;
    int cap_kbps_ = 0;
    int applied_kbps_ = 0;
    int current_target_kbps_ = 0;
    fast_state_e state_ = fast_state_e::healthy;
    std::string last_reason_ = "none";
    std::optional<int> pending_target_;

    // 嚴重降碼同秒判定
    steady::time_point last_fast_cut_time_ {};
    int fast_cut_base_applied_ = 0;

    // 冷卻與回升定時
    steady::time_point hold_until_ {};
    steady::time_point ramp_start_ {};
    steady::time_point next_ramp_ {};

    // 防振盪懲罰
    int penalty_cap_ = 0;
    steady::time_point penalty_until_ {};

    // 統計與壅塞記錄
    int cuts_count_ = 0;
    int ramps_count_ = 0;
    int trims_count_ = 0;
    int random_loss_events_ = 0;
    int last_congested_kbps_ = 0;
    steady::time_point last_congested_time_ {};
    steady::time_point last_tick_ {};

    // §ABR-LOSSRATE
    std::deque<steady::time_point> loss_rate_events_;
    int loss_cap_ = 0;
    steady::time_point loss_cap_until_ {};
    int loss_rate_cuts_ = 0;

    // 丟包與延遲追蹤
    std::deque<fec_record_t> fec_fails_;
    std::deque<loss_record_t> persistent_losses_;

    int consecutive_late_slack_ = 0;
    steady::time_point last_delay_trend_cut_time_{};
    delay_trend_t trend_;

    // §ABR-ADAPT 第 2、3 期新增追蹤狀態
    bool owd_have_offset_ = false;
    double owd_offset_ms_ = 0.0;
    steady::time_point last_owd_time_{};
    steady::time_point last_owd_trend_cut_time_{};
    delay_trend_t owd_trend_;

    steady::time_point last_rates_time_{};
    int recv_kbps_ = 0;
    int send_kbps_ = 0;
    int shortfall_reports_ = 0;  ///< §ABR-SHORTFALL：連續幾次收／送落差超過門檻

    steady::time_point last_quality_time_{};
    int last_qp_x100_ = -1;
    int last_codec_ = -1;
    steady::time_point qp_low_start_{};
    steady::time_point last_trim_time_{};

    steady::time_point decode_tight_until_{};
    int consecutive_decode_overload_ = 0;
  };

}  // namespace vr::abr
