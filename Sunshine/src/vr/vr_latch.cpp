/**
 * @file src/vr/vr_latch.cpp
 * @brief VipleStream 2.0 §VR（M4a R3）：LATCH 相位回授的頻率鎖控制器，見 vr_latch.h。
 */
#include "vr_latch.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <mutex>
#include <system_error>
#include <vector>

namespace vr::latch {

  namespace {
    // §VR-LATCH-V3：相位運算（單位都是 µs）
    /// 折到 [−period/2, period/2)
    double wrap_half(double x, double period) {
      x = std::fmod(x + period / 2, period);
      if (x < 0) {
        x += period;
      }
      return x - period / 2;
    }

    /// 折到 [0, period)
    double wrap_pos(double x, double period) {
      x = std::fmod(x, period);
      if (x < 0) {
        x += period;
      }
      return x < period ? x : 0.0;  // 極小的負值加上 period 會捨入成 period 本身
    }

    /// 已排序的 n 筆的中位數（偶數筆取中間兩筆的平均）
    double median_sorted(const double *v, std::size_t n) {
      if (n == 0) {
        return 0.0;
      }
      return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    }

    // §VR-LATCH-V3：行程內記住的積分（依幀週期分開）
    constexpr double k_same_period_tol = 0.03;  ///< 週期差 ≤ 3% 視為同一個（同 vr_predict.cpp）

    struct learned_t {
      uint32_t period_ns;
      double integral_ppm;
    };

    std::mutex g_learned_mtx;
    std::vector<learned_t> g_learned;

    bool same_period(uint32_t a, uint32_t b) {
      return a != 0 && b != 0 && std::abs((double) a - (double) b) <= k_same_period_tol * (double) std::max(a, b);
    }

    // vr_latch_ab 的秒數範圍
    constexpr int k_ab_seconds_min = 20;
    constexpr int k_ab_seconds_max = 600;

    /// §VR-LATCH-V3：在結果上標「上一步量不到結果」（kEvalVoid）；cmd_us＝那一步要求的移動量
    result_t with_void_eval(result_t r, double cmd_us) {
      r.eval_valid = true;
      r.eval_moved_us = 0;
      r.eval_cmd_us = cmd_us;
      r.eval_err_us = 0;
      r.eval_outcome = kEvalVoid;
      return r;
    }
  }  // namespace

  circ_stat_t circ_median(const double *xs, std::size_t n, double period_us) {
    circ_stat_t out;
    if (xs == nullptr || n == 0 || !(period_us > 0)) {
      return out;
    }
    n = std::min(n, controller_t::kV3BufMax);  // 控制器最多留這麼多筆；多的不看
    // 參考點：到其他樣本的環狀距離總和最小的那一筆（同分取先出現的）
    std::size_t ref = 0;
    double ref_sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
      double sum = 0;
      for (std::size_t j = 0; j < n; ++j) {
        sum += std::abs(wrap_half(xs[j] - xs[i], period_us));
      }
      if (i == 0 || sum < ref_sum) {
        ref = i;
        ref_sum = sum;
      }
    }
    std::array<double, controller_t::kV3BufMax> u {};
    for (std::size_t i = 0; i < n; ++i) {
      u[i] = wrap_half(xs[i] - xs[ref], period_us);
    }
    std::sort(u.begin(), u.begin() + (std::ptrdiff_t) n);
    out.center_us = wrap_pos(xs[ref] + median_sorted(u.data(), n), period_us);
    for (std::size_t i = 0; i < n; ++i) {
      u[i] = std::abs(wrap_half(xs[i] - out.center_us, period_us));
    }
    std::sort(u.begin(), u.begin() + (std::ptrdiff_t) n);
    out.mad_us = median_sorted(u.data(), n);
    return out;
  }

  double controller_t::target_for_period(uint32_t display_period_ns) {
    const double quarter_us = (double) display_period_ns / 4000.0;
    return std::clamp(quarter_us, 1000.0, 4000.0);
  }

  void controller_t::configure(mode_e mode, int target_pct, uint32_t host_period_ns, const options_t &options) {
    mode_ = mode;
    target_pct_ = std::clamp(target_pct, kTargetPctMin, mode == mode_e::v3 ? kTargetPctMaxV3 : kTargetPctMax);
    host_period_ns_ = host_period_ns >= 5'000'000 && host_period_ns <= 50'000'000 ? host_period_ns : 0;
    options_ = options;
    options_.step_ppm = std::clamp(options_.step_ppm, kV3StepPpmMin, kV3StepPpmMax);
    if (!std::isfinite(options_.seed_integral_ppm)) {
      options_.have_seed = false;
      options_.seed_integral_ppm = 0;
    }
    // §VR-LATCH-V3：上一個 session 學到的時鐘差當積分的起點（legacy／v2 不用）
    if (mode_ == mode_e::v3 && options_.have_seed) {
      integral_ = std::clamp(options_.seed_integral_ppm, -kIntegralMax, kIntegralMax);
    }
  }

  void controller_t::reset() {
    const auto mode = mode_;
    const auto pct = target_pct_;
    const auto host = host_period_ns_;
    const auto options = options_;
    *this = controller_t {};
    mode_ = mode;
    target_pct_ = pct;
    host_period_ns_ = host;
    options_ = options;
    if (mode_ == mode_e::v3 && options_.have_seed) {
      integral_ = std::clamp(options_.seed_integral_ppm, -kIntegralMax, kIntegralMax);  // 回到剛 configure() 完的狀態
    }
  }

  double controller_t::modulus_us() const {
    uint32_t p = period_ns_;
    if (host_period_ns_ != 0 && host_period_ns_ < p) {
      p = host_period_ns_;
    }
    return (double) p / 1000.0;
  }

  double controller_t::target_v2_us() const {
    return modulus_us() * (double) target_pct_ / 100.0;
  }

  void controller_t::fill_observability(result_t &r) const {
    r.integral_ppm = integral_;
    r.slips = slips_;
    r.dropped = dropped_;
    r.mode = mode_;
    if (mode_ == mode_e::v3) {
      // log 的欄位名沿用 v2：slackEma＝目標＋誤差（誤差＝環狀中位數到目標的最短路徑，方向鎖存之後）
      r.target_us = target_v2_us();
      r.err_us = err_v3_;
      r.slack_ema_us = r.target_us + r.err_us;
      r.phase = phase_;
      r.gate_open = gate_open_;
      r.steps = steps_;
      r.bad_steps = bad_steps_;
      r.center_us = center_;
      return;
    }
    if (mode_ == mode_e::v2) {
      r.target_us = target_v2_us();
      r.err_us = have_ema_ ? ema_ : 0.0;
      r.slack_ema_us = r.target_us + r.err_us;
    } else {
      r.target_us = target_for_period(period_ns_);
      r.slack_ema_us = ema_;
      r.err_us = have_ema_ ? ema_ - r.target_us : 0.0;
    }
  }

  void controller_t::count_slip(double slack_us, double period_us) {
    if (have_prev_slack_ && std::abs(slack_us - prev_slack_us_) > kSlipPeriods * period_us) {
      ++slips_;
    }
    prev_slack_us_ = slack_us;
    have_prev_slack_ = true;
  }

  result_t controller_t::decide(int64_t now_ns, int32_t ppm) {
    result_t r;
    r.ppm = ppm;
    const bool changed = std::abs(ppm - sent_ppm_) >= kMinDelta;
    const bool due = !sent_once_ || now_ns - last_sent_ns_ >= kRefreshNs;
    if (changed || due) {
      r.update = true;
      sent_ppm_ = ppm;
      sent_once_ = true;
      last_sent_ns_ = now_ns;
    }
    fill_observability(r);
    return r;
  }

  result_t controller_t::on_latch(int32_t slack_us, uint32_t display_period_ns, int64_t now_ns, uint32_t frame_id) {
    // 顯示週期只接受 5 ms～50 ms（200～20 Hz）；其他值沿用上次
    if (display_period_ns >= 5'000'000 && display_period_ns <= 50'000'000) {
      // §VR-LATCH-V2：換更新率（例如 120→90 Hz）時舊的誤差與積分都不再適用
      // （v3 也適用；have_ema_ 在 v3 表示「這個顯示週期已經收過樣本」，第一則 LATCH 不會把起點的積分清掉）
      if (mode_ != mode_e::legacy && have_ema_ &&
          std::abs((double) display_period_ns - (double) period_ns_) > kPeriodChange * (double) period_ns_) {
        have_ema_ = false;
        integral_ = 0;
        have_prev_slack_ = false;
        if (mode_ == mode_e::v3) {
          // 樣本是取模舊週期的相位、閘門的幀率也是舊的：全部重來；走到一半的步進不再評估（不帶出結果）
          v3_to_wait();
          v3_clear_gate();
          v3_abort_round();
        }
      }
      period_ns_ = display_period_ns;
    }
    switch (mode_) {
      case mode_e::v3:
        return on_latch_v3(slack_us, now_ns, frame_id);
      case mode_e::v2:
        return on_latch_v2(slack_us, now_ns, frame_id);
      default:
        return on_latch_legacy(slack_us, now_ns);
    }
  }

  result_t controller_t::on_latch_legacy(int32_t slack_us, int64_t now_ns) {
    const double period_us = (double) period_ns_ / 1000.0;
    // 單筆離群值夾在 ±1 個週期內（晚到超過一個週期時，幀本來就會被下一個 latch 接走）
    const double s = std::clamp((double) slack_us, -period_us, period_us);
    count_slip((double) slack_us, period_us);  // 只做觀測，不影響控制

    const double dt_s = last_latch_ns_ > 0 ? std::clamp((double) (now_ns - last_latch_ns_) / 1e9, 0.0, 1.0) : 0.1;
    last_latch_ns_ = now_ns;
    ++count_;

    if (!have_ema_) {
      ema_ = s;
      have_ema_ = true;
    } else {
      ema_ += kEmaAlpha * (s - ema_);
    }

    const double err = ema_ - target_for_period(period_ns_);  // 正值：幀太早到 → 放慢（ppm > 0）
    integral_ = std::clamp(integral_ + kKi * err * dt_s, -kIntegralMax, kIntegralMax);
    const double raw = kKp * err + integral_;
    const int32_t ppm = (int32_t) std::lround(std::clamp(raw, (double) -VR_LATCH_PPM_MAX, (double) VR_LATCH_PPM_MAX));
    return decide(now_ns, ppm);
  }

  result_t controller_t::on_latch_v2(int32_t slack_us, int64_t now_ns, uint32_t frame_id) {
    // client 沒有新幀時每 100 ms 重送上一筆（同一個 frameId）：不是新的相位量測
    if (have_frame_ && frame_id == last_frame_id_) {
      ++dropped_;
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    have_frame_ = true;
    last_frame_id_ = frame_id;

    const double period_us = modulus_us();
    const double s = (double) slack_us;
    if (std::abs(s) > kOutlierPeriods * period_us) {
      ++dropped_;
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    count_slip(s, period_us);

    const double dt_s = last_latch_ns_ > 0 ? std::clamp((double) (now_ns - last_latch_ns_) / 1e9, 0.0, 1.0) : 0.1;
    last_latch_ns_ = now_ns;
    ++count_;

    // slack 是取模一個週期的相位：晚到 δ 的幀被量成 T−δ。誤差只在上側折返（→ −δ−target），
    // 下側只夾不折返，協定「負值＝晚到」的語意不變
    double e = s - target_v2_us();
    if (e >= period_us / 2) {
      e -= period_us;
    }
    e = std::max(e, -period_us / 2);

    if (!have_ema_) {
      ema_ = e;
      have_ema_ = true;
    } else {
      ema_ += kEmaAlpha * (e - ema_);
    }
    const double err = ema_;  // 正值：幀太早到 → 放慢（ppm > 0）

    // 條件式 anti-windup：輸出已經飽和、而且誤差同向時不再積分（legacy 的積分在飽和時照樣累加，鋸齒的成因之一）
    const double max = (double) VR_LATCH_PPM_MAX;
    const double raw_pre = kKpV2 * err + integral_;
    const bool saturated_same = (raw_pre >= max && err > 0) || (raw_pre <= -max && err < 0);
    if (!saturated_same) {
      integral_ = std::clamp(integral_ + kKiV2 * err * dt_s, -kIntegralMax, kIntegralMax);
    }
    const double raw = kKpV2 * err + integral_;
    const int32_t ppm = (int32_t) std::lround(std::clamp(raw, -max, max));
    return decide(now_ns, ppm);
  }

  result_t controller_t::on_idle(int64_t now_ns) {
    if (mode_ == mode_e::v3) {
      return on_idle_v3(now_ns);
    }
    if (!sent_once_ || last_latch_ns_ == 0 || now_ns - last_latch_ns_ < kStaleNs) {
      result_t r;
      r.ppm = sent_ppm_;
      fill_observability(r);
      return r;
    }
    // 沒有新的相位資訊：每次 idle 把修正減半，最後回到 0（標稱週期）
    integral_ *= 0.5;
    int32_t ppm = sent_ppm_ / 2;
    if (std::abs(ppm) < kMinDelta) {
      ppm = 0;
      integral_ = 0;
    }
    have_ema_ = false;
    have_prev_slack_ = false;
    return decide(now_ns, ppm);
  }

  result_t controller_t::on_hold(int64_t now_ns, uint32_t frame_id) {
    // §VR-FENCE-LATE：LATCH 還在來，只是這一筆不當相位量測。時刻與 frameId 照常記（理由見標頭）
    last_latch_ns_ = now_ns;
    have_frame_ = true;
    last_frame_id_ = frame_id;
    have_prev_slack_ = false;  // 相位滑移只比相鄰兩筆量測；隔著暫停的兩筆不比
    if (mode_ != mode_e::v3) {
      // legacy／v2：比例項拿掉，輸出只留積分；積分不動。過載旗標要看滿一段時間才設，設起來之前已經有幾筆過載的樣本
      // 進了控制器——v2 的增益下 3 筆「晚 4 ms」就把輸出推到 ±200 ppm，而積分只動 2 ppm 上下。照原樣凍結的話，
      // 那個被帶偏的輸出會維持到過載結束（200 ppm＝每秒漂 0.2 ms）。誤差的平滑值也是髒的：作廢，恢復後從第一筆
      // 新樣本重新開始。
      have_ema_ = false;
      return decide(now_ns, (int32_t) std::lround(integral_));
    }
    // v3：和節拍閘門關著同樣處理（v3_gate_closed），另外把閘門的紀錄清掉——暫停前的間隔與樣本都不沿用
    const bool voided = v3_abort_round();
    v3_to_wait();
    v3_clear_gate();
    const result_t r = now_ns < blind_until_ns_ ? hold() : decide(now_ns, (int32_t) std::lround(integral_));  // 盲區內不寫 pacing
    return voided ? with_void_eval(r, step_cmd_us_) : r;
  }

  // ── §VR-LATCH-V3 ──────────────────────────────────────────────────────

  int64_t controller_t::stream_period_ns() const {
    return host_period_ns_ != 0 ? (int64_t) host_period_ns_ : (int64_t) period_ns_;
  }

  result_t controller_t::hold() const {
    result_t r;
    r.ppm = sent_ppm_;
    fill_observability(r);
    return r;
  }

  void controller_t::v3_to_wait() {
    phase_ = kPhaseWait;
    buf_n_ = 0;
    buf_head_ = 0;
    dir_ = 0;
    reacq_run_ = 0;
    err_v3_ = 0;
    center_ = 0;
  }

  void controller_t::v3_clear_gate() {
    gate_n_ = 0;
    gate_head_ = 0;
    gate_open_ = false;
  }

  bool controller_t::v3_abort_round() {
    const bool pending = step_pending_;
    step_pending_ = false;
    iters_ = 0;
    return pending;
  }

  result_t controller_t::v3_gate_closed(int64_t now_ns) {
    // 閘門關著：沒有可用的相位資訊，輸出只留積分（學到的時鐘差）。
    // 走到一半的那一輪步進到此為止：burst 之後、量完之前節拍就斷了，等閘門再開時量到的相位已經和那一步無關
    // （隔了多久不一定，host 的管線也可能變了），拿來評估只會得到失真的 step result、把新的一輪當成修整。
    // 不評估、不算成敗（kEvalVoid），閘門再開時從新的一輪開始（進入門檻、兩步的額度都重來；60 s 的步數上限照算）。
    const bool voided = v3_abort_round();
    if (phase_ != kPhaseWait) {
      v3_to_wait();
    }
    const result_t r = decide(now_ns, (int32_t) std::lround(integral_));
    return voided ? with_void_eval(r, step_cmd_us_) : r;
  }

  void controller_t::v3_push_sample(double phase_us) {
    buf_[buf_head_] = phase_us;
    buf_head_ = (buf_head_ + 1) % kV3BufMax;
    buf_n_ = std::min(buf_n_ + 1, kV3BufMax);
  }

  circ_stat_t controller_t::v3_center(std::size_t last_n) const {
    const std::size_t n = std::min(last_n, buf_n_);
    std::array<double, kV3BufMax> xs {};
    for (std::size_t i = 0; i < n; ++i) {
      xs[i] = buf_[(buf_head_ + kV3BufMax - n + i) % kV3BufMax];  // 依時間順序（舊 → 新）
    }
    return circ_median(xs.data(), n, modulus_us());
  }

  bool controller_t::unsettled(int64_t now_ns) const {
    return mode_ == mode_e::v3 && (phase_ != kPhaseTrack || now_ns - track_since_ns_ < kV3SettledNs);
  }

  std::optional<double> controller_t::learned(int64_t now_ns) const {
    if (mode_ != mode_e::v3 || phase_ != kPhaseTrack || now_ns - track_since_ns_ < kV3LearnTrackNs ||
        std::abs(integ_avg_) > kV3LearnMaxPpm) {
      return std::nullopt;
    }
    return integ_avg_;
  }

  void controller_t::switch_mode(mode_e mode, int target_pct) {
    mode_ = mode;
    target_pct_ = std::clamp(target_pct, kTargetPctMin, mode == mode_e::v3 ? kTargetPctMaxV3 : kTargetPctMax);
    // 積分（時鐘差）、最後送出的 ppm、顯示週期、frameId 的去重狀態都留著；誤差的平滑值與 v3 的樣本不能沿用
    have_ema_ = false;
    ema_ = 0;
    have_prev_slack_ = false;
    v3_to_wait();
    v3_clear_gate();
    // 走到一半的 burst 不再評估。盲區（blind_until_ns_）不取消：bridge 那邊的 burst 還在走，換過去的模式是 v3 時
    // 照舊等它走完、盲區過了才重新取樣本（提早取會量到移動中的相位）；v2／legacy 不看盲區，它們的輸出一變就寫
    // pacing（通常是切換後第一則新幀的 LATCH），那一次寫入會讓 bridge 把還沒結束的 burst 取消掉。
    v3_abort_round();
  }

  bool controller_t::can_step(int64_t now_ns) const {
    if (!options_.acquire || now_ns < no_step_until_ns_) {
      return false;
    }
    // 任何 60 s 內最多 kV3StepsPerMin 步
    const std::size_t stored = std::min<std::size_t>(steps_, kV3StepsPerMin);
    std::size_t recent = 0;
    for (std::size_t i = 0; i < stored; ++i) {
      if (now_ns - step_times_[i] < kV3StepWindowNs) {
        ++recent;
      }
    }
    return recent < kV3StepsPerMin;
  }

  result_t controller_t::start_step(double err_us, double center_us, int64_t now_ns, uint8_t why) {
    // 基準＝積分（學到的時鐘差）。誤差 > 0＝影像太早到 → 週期暫時變長（之後的幀晚到），反之變短。
    // 以 step_ppm 走 |誤差|／step_ppm 這麼久，相位剛好移 |誤差|：4000 ppm 下 3000 µs → 750 ms、半格 4167 µs → 1.04 s。
    const int32_t base = (int32_t) std::lround(integral_);
    const int sgn = err_us > 0 ? 1 : -1;
    const double step_ppm = (double) options_.step_ppm;
    const int64_t dur_us = std::min<int64_t>(std::llround(std::abs(err_us) * 1e6 / step_ppm), kV3StepMaxUs);

    result_t r;
    r.update = true;
    r.ppm = base;
    r.burst_ppm = base + sgn * options_.step_ppm;
    r.burst_us = (uint32_t) dur_us;
    r.step_why = why;
    r.step_err_us = err_us;

    blind_until_ns_ = now_ns + dur_us * 1000 + kV3SettleNs;
    last_step_end_ns_ = now_ns + dur_us * 1000;
    step_c_before_ = center_us;
    step_cmd_us_ = sgn * (double) dur_us * step_ppm / 1e6;
    step_pending_ = true;
    ++iters_;
    step_times_[steps_ % kV3StepsPerMin] = now_ns;
    ++steps_;
    phase_ = kPhaseBurst;
    buf_n_ = 0;
    buf_head_ = 0;
    reacq_run_ = 0;
    dir_ = 0;
    err_v3_ = err_us;
    center_ = center_us;
    // burst 結束時由呼叫端（bridge）改回基準。這裡把「已送出」記成基準，並把例行重送排在盲區一結束：
    // 盲區後第一則 LATCH 會再寫一次基準（第二道保險）
    sent_ppm_ = base;
    sent_once_ = true;
    last_sent_ns_ = blind_until_ns_ - kRefreshNs;

    fill_observability(r);
    return r;
  }

  result_t controller_t::on_latch_v3(int32_t slack_us, int64_t now_ns, uint32_t frame_id) {
    const double T = modulus_us();
    const double s = (double) slack_us;
    const bool dup = have_frame_ && frame_id == last_frame_id_;

    // 節拍閘門：最近 kV3GateN 個 LATCH 間隔裡 frameId 前進的張數。黑幀（host 沒有畫面時每 100 ms 一張）與 host
    // 沒在出幀的時候不取樣本——那些幀的到達相位和正常出幀時無關。重複的 frameId 也記一筆間隔（前進 0 張）。
    if (last_latch_ns_ != 0 && have_frame_) {
      const int64_t dt = std::max<int64_t>(now_ns - last_latch_ns_, 0);
      uint32_t adv = dup ? 0u : frame_id - last_frame_id_;
      const double expected = (double) dt / (double) stream_period_ns();
      if ((double) adv > 4.0 * expected + 8.0) {
        adv = 0;  // 序號重來（client 重建）或倒退：不當成前進
      }
      gate_[gate_head_] = gate_rec_t {dt, adv};
      gate_head_ = (gate_head_ + 1) % kV3GateN;
      gate_n_ = std::min(gate_n_ + 1, kV3GateN);
    }
    last_latch_ns_ = now_ns;
    {
      int64_t dt_sum = 0;
      int64_t dt_max = 0;
      uint64_t adv_sum = 0;
      for (std::size_t i = 0; i < gate_n_; ++i) {
        dt_sum += gate_[i].dt_ns;
        dt_max = std::max(dt_max, gate_[i].dt_ns);
        adv_sum += gate_[i].adv;
      }
      gate_open_ = gate_n_ == kV3GateN && dt_max <= kV3GateGapNs &&
                   (double) adv_sum >= kV3GateRatio * (double) dt_sum / (double) stream_period_ns();
    }

    // client 沒有新幀時每 100 ms 重送上一筆（同一個 frameId）：不是新的相位量測
    if (dup) {
      ++dropped_;
      // 影像停住（收不到幀、host 停止出幀又沒有黑幀）時 LATCH 只剩重複的 frameId：上面照樣記成「前進 0 張」的間隔，
      // 兩三則之後閘門就關。這時和「有新幀但閘門關著」同樣處理——比例項拿掉、回到等待。只回 hold() 的話，重複的
      // LATCH 一直更新 last_latch_ns_，on_idle_v3 的「停 2 s」永遠不成立，最後一次的比例項（最多 ±200 ppm）會一路
      // 留到影像恢復，相位每秒漂 0.2 ms。盲區內照舊不寫 pacing；已經在等待狀態就不必再做一次。
      if (!gate_open_ && phase_ != kPhaseWait && now_ns >= blind_until_ns_) {
        return v3_gate_closed(now_ns);
      }
      return hold();
    }
    have_frame_ = true;
    last_frame_id_ = frame_id;
    ++count_;
    if (std::abs(s) > kOutlierPeriods * T) {
      ++dropped_;
      return hold();
    }
    have_ema_ = true;  // v3 不做 EMA；這個旗標只表示「這個顯示週期已經收過樣本」（on_latch 換更新率的判斷用）
    count_slip(s, T);  // 只做觀測，定義與 v2 相同

    // 盲區：burst 進行中與結束後的 kV3SettleNs。相位正在被移動、管線裡還有舊節拍的幀，不取樣本；也不寫 pacing
    // （每 1 s 的例行重送會把走到一半的 burst 蓋掉）。閘門的紀錄照常更新（在上面）。
    if (now_ns < blind_until_ns_) {
      return hold();
    }

    const int32_t base = (int32_t) std::lround(integral_);  // 沒有相位資訊時的輸出：只有積分（學到的時鐘差）
    if (!gate_open_) {
      return v3_gate_closed(now_ns);
    }
    if (phase_ == kPhaseWait || phase_ == kPhaseBurst) {
      phase_ = kPhaseMeasure;
      buf_n_ = 0;
      buf_head_ = 0;
    }
    v3_push_sample(wrap_pos(s, T));
    const double target = target_v2_us();

    // 上一步的結果：量完、決定了下一步才帶出一次
    struct eval_t {
      bool valid = false;
      double moved_us = 0;
      double cmd_us = 0;
      double err_us = 0;
      uint8_t outcome = kEvalTrack;
    };

    const auto take_eval = [this, T](double center_us, double err_us, uint8_t outcome) {
      eval_t ev;
      if (step_pending_) {
        step_pending_ = false;
        ev.valid = true;
        ev.moved_us = wrap_half(step_c_before_ - center_us, T);  // 與 cmd 同號＝往要求的方向動
        ev.cmd_us = step_cmd_us_;
        ev.err_us = err_us;
        ev.outcome = outcome;
      }
      return ev;
    };
    const auto attach = [](result_t r, const eval_t &ev) {
      r.eval_valid = ev.valid;
      r.eval_moved_us = ev.moved_us;
      r.eval_cmd_us = ev.cmd_us;
      r.eval_err_us = ev.err_us;
      r.eval_outcome = ev.outcome;
      return r;
    };
    eval_t ev;

    if (phase_ == kPhaseMeasure) {
      if (buf_n_ < kV3AcqN) {
        return decide(now_ns, base);
      }
      if (options_.acquire) {
        const auto cs = v3_center(kV3AcqN);
        const double e = wrap_half(cs.center_us - target, T);
        const bool scattered = cs.mad_us > kV3MadMaxFrac * T;
        const double thr = (iters_ == 0 ? kV3StepEnterFrac : kV3TrimFrac) * T;
        if (std::abs(e) > thr && !scattered && iters_ < kV3MaxIter && can_step(now_ns)) {
          const uint8_t why = iters_ == 0 ? kStepAcq : kStepTrim;
          ev = take_eval(cs.center_us, e, kEvalTrim);  // 要在 start_step 改掉步進紀錄之前取
          return attach(start_step(e, cs.center_us, now_ns, why), ev);
        }
        if (scattered && buf_n_ < kV3BufMax) {
          return decide(now_ns, base);  // 相位太散（樣本落在邊緣兩側、或正在被別的東西移動）：再量
        }
        uint8_t outcome = kEvalTrack;
        if (iters_ > 0) {
          // 這一輪步進成不成功，只看最後有沒有進到進入門檻內。不用「實際移動量 ÷ 要求的量」判：讀數落在 latch 邊緣
          // 或頭盔 GPU 的忙碌期時，這個比例會因為取模而失真；它只進 log。
          if (std::abs(e) > kV3StepEnterFrac * T) {
            ++bad_steps_;
            no_step_until_ns_ = now_ns + backoff_ns_;
            backoff_ns_ = std::min<int64_t>(2 * backoff_ns_, kV3BackoffMaxNs);
            outcome = kEvalBackoff;
          } else {
            backoff_ns_ = kV3BackoffNs;
          }
        }
        ev = take_eval(cs.center_us, e, outcome);
      }
      phase_ = kPhaseTrack;
      iters_ = 0;
      reacq_run_ = 0;
      track_since_ns_ = now_ns;
      integ_avg_ = integral_;
    }

    // 追蹤：最近 kV3MedianN 筆的環狀中位數到目標的最短路徑
    const auto cs = v3_center(kV3MedianN);
    double e = wrap_half(cs.center_us - target, T);

    if (options_.acquire) {
      // 負載改變（例如遊戲開始出幀，host 管線變長）：誤差連續 kV3ReacqRun 筆超過進入門檻就重新步進
      reacq_run_ = std::abs(e) > kV3StepEnterFrac * T ? reacq_run_ + 1 : 0;
      if (reacq_run_ >= kV3ReacqRun && (steps_ == 0 || now_ns - last_step_end_ns_ >= kV3StepGapNs) && can_step(now_ns)) {
        iters_ = 0;
        return attach(start_step(e, cs.center_us, now_ns, kStepReacq), ev);
      }
    }

    // 方向鎖存：誤差靠近反相點（目標 ± T/2）時，一點雜訊就讓最短路徑的方向翻面；鎖住之後維持同一個方向走完
    if (dir_ == 0 && std::abs(e) > kV3DirLatchFrac * T) {
      dir_ = e > 0 ? 1 : -1;
    }
    if (dir_ != 0) {
      if (std::abs(e) < kV3DirReleaseFrac * T) {
        dir_ = 0;
      } else if (e * dir_ < 0) {
        e += dir_ * T;
      }
    }

    const double dt_s = last_pi_ns_ != 0 ? std::clamp((double) (now_ns - last_pi_ns_) / 1e9, 0.0, 1.0) : 0.1;
    last_pi_ns_ = now_ns;

    // 條件式 anti-windup（同 v2）：輸出已經飽和、而且誤差同向時不再積分
    const double max = (double) VR_LATCH_PPM_MAX;
    const double raw_pre = kKpV2 * e + integral_;
    const bool saturated_same = (raw_pre >= max && e > 0) || (raw_pre <= -max && e < 0);
    if (!saturated_same) {
      integral_ = std::clamp(integral_ + kKiV2 * e * dt_s, -kIntegralMax, kIntegralMax);
    }
    integ_avg_ += 0.01 * (integral_ - integ_avg_);
    err_v3_ = e;
    center_ = cs.center_us;
    const double raw = kKpV2 * e + integral_;
    return attach(decide(now_ns, (int32_t) std::lround(std::clamp(raw, -max, max))), ev);
  }

  result_t controller_t::on_idle_v3(int64_t now_ns) {
    // 盲區內任何路徑都不寫 pacing（見 on_latch_v3）
    if (now_ns < blind_until_ns_) {
      return hold();
    }
    if (!sent_once_ || last_latch_ns_ == 0 || now_ns - last_latch_ns_ < kStaleNs) {
      return hold();
    }
    // LATCH 停了（XR 失焦、斷線）：沒有新的相位資訊。比例項歸零，積分是學到的時鐘差、留著；停很久才衰減。
    // 走到一半的那一輪步進到此為止（理由同 v3_gate_closed）：LATCH 回來時量到的相位不拿來評估那一步
    const bool voided = v3_abort_round();
    v3_to_wait();
    v3_clear_gate();
    have_ema_ = false;
    have_prev_slack_ = false;
    if (now_ns - last_latch_ns_ >= kV3IdleDecayNs) {
      integral_ *= 0.5;
      if (std::abs(integral_) < 0.5) {
        integral_ = 0;
      }
    }
    const result_t r = decide(now_ns, (int32_t) std::lround(integral_));
    return voided ? with_void_eval(r, step_cmd_us_) : r;
  }

  // ── §VR-LATCH-V3：行程內記住的積分 ─────────────────────────────────────

  void remember(uint32_t period_ns, double integral_ppm) {
    if (period_ns == 0 || !std::isfinite(integral_ppm)) {
      return;
    }
    const double v = std::clamp(integral_ppm, -controller_t::kIntegralMax, controller_t::kIntegralMax);
    std::lock_guard lk {g_learned_mtx};
    for (auto &l : g_learned) {
      if (same_period(l.period_ns, period_ns)) {
        l.integral_ppm = v;
        return;
      }
    }
    g_learned.push_back({period_ns, v});
  }

  std::optional<double> recall(uint32_t period_ns) {
    std::lock_guard lk {g_learned_mtx};
    for (const auto &l : g_learned) {
      if (same_period(l.period_ns, period_ns)) {
        return l.integral_ppm;
      }
    }
    return std::nullopt;
  }

  void reset_learned_for_test() {
    std::lock_guard lk {g_learned_mtx};
    g_learned.clear();
  }

  // ── §VR-LATCH-V3：vr_latch_ab ─────────────────────────────────────────

  std::optional<ab_spec_t> parse_ab(std::string_view spec) {
    // <秒>:<模式>:<target%>，剛好三段
    const auto c1 = spec.find(':');
    if (c1 == std::string_view::npos) {
      return std::nullopt;
    }
    const auto c2 = spec.find(':', c1 + 1);
    if (c2 == std::string_view::npos || spec.find(':', c2 + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    const auto to_int = [](std::string_view tok, int &out) {
      if (tok.empty() || tok.front() == '+') {
        return false;
      }
      const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), out, 10);
      return res.ec == std::errc {} && res.ptr == tok.data() + tok.size();
    };

    ab_spec_t ab;
    if (!to_int(spec.substr(0, c1), ab.seconds) || !to_int(spec.substr(c2 + 1), ab.target_pct)) {
      return std::nullopt;
    }
    const std::string_view mode = spec.substr(c1 + 1, c2 - c1 - 1);
    if (mode == "legacy") {
      ab.mode = mode_e::legacy;
    } else if (mode == "v2") {
      ab.mode = mode_e::v2;
    } else if (mode == "v3") {
      ab.mode = mode_e::v3;
    } else {
      return std::nullopt;
    }
    const int pct_max = ab.mode == mode_e::v3 ? controller_t::kTargetPctMaxV3 : controller_t::kTargetPctMax;
    if (ab.seconds < k_ab_seconds_min || ab.seconds > k_ab_seconds_max || ab.target_pct < controller_t::kTargetPctMin || ab.target_pct > pct_max) {
      return std::nullopt;
    }
    return ab;
  }

}  // namespace vr::latch
