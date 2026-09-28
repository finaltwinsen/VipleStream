/**
 * @file src/vr/vr_clock.cpp
 * @brief VipleStream 2.0 §VR（M1b S1-12、§F.1／§F.2）：時鐘對映估計器、pacing 週期前饋與 T6 合成測試。
 *
 * 純模組（見 vr_clock.h 檔頭）：只依賴標準函式庫，時間由呼叫端傳入，log 由呼叫端印。
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <string_view>

// local includes
#include "vr_clock.h"

namespace vr::clk {

  namespace {
    constexpr int64_t kNsPerSec = 1'000'000'000;
    constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();

    /// RTT 輸入的合理上限：更大的值（單位寫錯之類的垃圾）一律夾到這裡
    constexpr int64_t kRttMaxNs = 10'000'000'000;

    /// 換算公式的頻率上限：(t % f) * 1e9 與 (n % 1e9) * f 要在 int64 內（實際的 QPC 頻率是 10 MHz，
    /// 舊機器以 TSC 當 QPC 時也只有數 GHz）
    constexpr int64_t kFreqMax = 9'000'000'000;

    /// 向負無限大取整的除法（b > 0）
    int64_t floor_div(int64_t a, int64_t b) {
      int64_t q = a / b;
      if ((a % b) != 0 && a < 0) {
        --q;
      }
      return q;
    }

    std::size_t slot_of(int64_t num, int n) {
      return (std::size_t) (((num % n) + n) % n);
    }

    /// 以 double 判斷是否會溢位：|真值| < 9.2e18 < 2^63 時 int64 運算安全，否則對稱飽和到 ±INT64_MAX
    int64_t sat_add(int64_t a, int64_t b) {
      const double r = (double) a + (double) b;
      if (r >= 9.2e18) {
        return kI64Max;
      }
      if (r <= -9.2e18) {
        return -kI64Max;
      }
      return a + b;
    }

    int64_t sat_sub(int64_t a, int64_t b) {
      const double r = (double) a - (double) b;
      if (r >= 9.2e18) {
        return kI64Max;
      }
      if (r <= -9.2e18) {
        return -kI64Max;
      }
      return a - b;
    }

    /// v + skew·dt（四捨五入，飽和）
    int64_t add_ppm(int64_t v, double skew_ppm, int64_t dt_ns) {
      const double corr = skew_ppm * 1e-6 * (double) dt_ns;
      if (!std::isfinite(corr)) {
        return v;
      }
      return sat_add(v, (int64_t) std::llround(std::clamp(corr, -9.0e18, 9.0e18)));
    }
  }  // namespace

  // ── 換算 ───────────────────────────────────────────────────────────────

  int64_t ticks_to_ns(int64_t ticks, int64_t freq) {
    if (freq <= 0 || freq > kFreqMax) {
      return 0;
    }
    // 同 platf::qpc_ticks_to_ns：整秒與餘數分開換算；(ticks % f) * 1e9 在 f < 9.2e9 時不溢位。
    // 上限先扣一秒，sec * 1e9 再加餘數項也不會溢位；用 −INT64_MAX 保持 f(−t) == −f(t)。
    constexpr int64_t max_sec = kI64Max / kNsPerSec - 1;
    const int64_t sec = ticks / freq;
    if (sec > max_sec) {
      return kI64Max;
    }
    if (sec < -max_sec) {
      return -kI64Max;
    }
    return sec * kNsPerSec + (ticks % freq) * kNsPerSec / freq;
  }

  int64_t ns_to_ticks(int64_t ns, int64_t freq) {
    if (freq <= 0 || freq > kFreqMax) {
      return 0;
    }
    // 對稱於 ticks_to_ns：sec ≤ INT64_MAX / f − 1 時 sec * f ≤ INT64_MAX − f，加上餘數項（< f）不溢位；
    // (ns % 1e9) * f 在 f < 9.2e9 時不溢位。
    const int64_t max_sec = kI64Max / freq - 1;
    const int64_t sec = ns / kNsPerSec;
    if (sec > max_sec) {
      return kI64Max;
    }
    if (sec < -max_sec) {
      return -kI64Max;
    }
    return sec * freq + (ns % kNsPerSec) * freq / kNsPerSec;
  }

  // ── RTT ────────────────────────────────────────────────────────────────

  const char *rtt_src_name(rtt_src_e src) {
    switch (src) {
      case rtt_src_e::enet:
        return "enet";
      case rtt_src_e::quic:
        return "quic";
      case rtt_src_e::none:
        break;
    }
    return "none";
  }

  rtt_input_t select_rtt(int64_t quic_rtt_min_ns, int64_t enet_rtt_ms) {
    rtt_input_t out;
    if (quic_rtt_min_ns > 0) {
      out.src = rtt_src_e::quic;
      out.rtt_ns = std::min(quic_rtt_min_ns, kRttMaxNs);
      return out;
    }
    if (enet_rtt_ms >= 0) {
      out.src = rtt_src_e::enet;
      // GetTickCount 15/16 ms 量化：LAN 上 0/15/16 只是雜訊，直接當 0（scout F B2）
      out.rtt_ns = enet_rtt_ms < kEnetQuantMs ? 0 : std::min(enet_rtt_ms, kRttMaxNs / 1'000'000) * 1'000'000;
      return out;
    }
    return out;
  }

  // ── clock_map_t：估計器 ────────────────────────────────────────────────

  void clock_map_t::estimator_t::reset_keep_skew() {
    const double skew = skew_ppm;
    const bool learned = skew_learned;
    *this = estimator_t {};
    skew_ppm = skew;
    skew_learned = learned;
  }

  bool clock_map_t::estimator_t::ready(int64_t now) const {
    return any && now - first_ns >= kWarmupNs;
  }

  int64_t clock_map_t::estimator_t::proj(int64_t v, int64_t t, int64_t now) const {
    return add_ppm(v, skew_ppm, now - t);
  }

  bool clock_map_t::estimator_t::m2s(int64_t now, int64_t &out) const {
    const int64_t b = floor_div(now, kBucketNs);
    bool found = false;
    int64_t best = 0;
    for (const auto &w : win) {
      if (w.num == INT64_MIN || w.num <= b - kWindowBuckets || w.num > b) {
        continue;
      }
      // 各桶最小值以 skew 投影到 now 再取最小：漂移時 2 s 前的桶不會讓 m2s 落後 skew×2 s
      const int64_t v = proj(w.min_d, w.t_min, now);
      if (!found || v < best) {
        best = v;
        found = true;
      }
    }
    if (found) {
      out = best;
    }
    return found;
  }

  bool clock_map_t::estimator_t::floor_ref(int64_t now, int64_t &out) const {
    bool found = m2s(now, out);
    for (const auto &p : hist) {
      // 這個 transport 閒置很久後再用時，歷史只在下一次 add_point 才修剪；超過 60 s 的點不拿來比
      if (now - p.t > kSkewHistoryNs) {
        continue;
      }
      const int64_t v = proj(p.m, p.t, now);
      if (!found || v < out) {
        out = v;
        found = true;
      }
    }
    return found;
  }

  int64_t clock_map_t::rtt_filter_t::feed(int64_t now, rtt_input_t in) {
    if (in.src != src) {
      // 換來源（QUIC 出現／消失）：舊來源的最小值不能拿來比
      src = in.src;
      win = {};
    }
    if (src == rtt_src_e::none) {
      return 0;
    }
    const int64_t v = std::clamp<int64_t>(in.rtt_ns, 0, kRttMaxNs);
    const int64_t b = floor_div(now, kRttBucketNs);
    auto &slot = win[slot_of(b, kRttBuckets)];
    if (slot.num != b) {
      slot.num = b;
      slot.min_d = v;
    } else if (v < slot.min_d) {
      slot.min_d = v;
    }
    int64_t best = v;
    for (const auto &w : win) {
      if (w.num != INT64_MIN && w.num > b - kRttBuckets && w.num <= b) {
        best = std::min(best, w.min_d);
      }
    }
    return best;
  }

  clock_map_t::clock_map_t(int64_t tick_frequency):
      freq_ {tick_frequency > 0 && tick_frequency <= kFreqMax ? tick_frequency : kNsPerSec} {
    scratch_.reserve(kSkewMaxPoints * (kSkewMaxPoints - 1) / 2);
  }

  void clock_map_t::update_skew(estimator_t &e, std::vector<double> &scratch) {
    if (e.hist.size() < kSkewMinPoints) {
      return;
    }
    scratch.clear();
    for (std::size_t i = 0; i < e.hist.size(); ++i) {
      for (std::size_t j = i + 1; j < e.hist.size(); ++j) {
        const int64_t dt = e.hist[j].t - e.hist[i].t;
        if (dt < kSkewMinPairSpanNs) {
          continue;
        }
        scratch.push_back(((double) e.hist[j].m - (double) e.hist[i].m) / (double) dt * 1e6);
      }
    }
    if (scratch.size() < 3) {
      return;
    }
    // Theil–Sen：所有點對斜率的中位數（偶數個取中間兩個的平均）
    const std::size_t mid = scratch.size() / 2;
    std::nth_element(scratch.begin(), scratch.begin() + (std::ptrdiff_t) mid, scratch.end());
    double med = scratch[mid];
    if (scratch.size() % 2 == 0) {
      const double lo = *std::max_element(scratch.begin(), scratch.begin() + (std::ptrdiff_t) mid);
      med = (lo + med) / 2.0;
    }
    if (!std::isfinite(med)) {
      return;
    }
    e.skew_ppm = std::clamp(med, -kSkewClampPpm, kSkewClampPpm);
    e.skew_learned = true;
  }

  void clock_map_t::add_point(estimator_t &e, int64_t t, int64_t m, uint64_t &level_resets) {
    if (!e.hist.empty()) {
      const auto &last = e.hist.back();
      const int64_t pred = e.proj(last.m, last.t, t);
      const double limit = (double) (e.skew_learned ? kLevelShiftNs : kLevelShiftLooseNs);
      if (std::fabs((double) m - (double) pred) > limit) {
        e.hist.clear();
        ++level_resets;
      }
    }
    e.hist.push_back({t, m});
    while (!e.hist.empty() && t - e.hist.front().t > kSkewHistoryNs) {
      e.hist.erase(e.hist.begin());
    }
    while (e.hist.size() > kSkewMaxPoints) {
      e.hist.erase(e.hist.begin());
    }
    update_skew(e, scratch_);
  }

  void clock_map_t::feed_estimator(estimator_t &e, int64_t now, int64_t d, uint64_t &level_resets) {
    if (!e.any) {
      e.any = true;
      e.first_ns = now;
    }

    const int64_t b = floor_div(now, kBucketNs);
    auto &w = e.win[slot_of(b, kWindowBuckets)];
    if (w.num != b) {
      w.num = b;
      w.min_d = d;
      w.t_min = now;
    } else if (d < w.min_d) {
      w.min_d = d;
      w.t_min = now;
    }

    // Theil–Sen 的點：每 2 s 一個（該段的最小 d 與它的到達時刻；用實際時刻，漂移下仍無偏）
    if (!e.period_has) {
      e.period_has = true;
      e.period_start = now;
      e.period_min = d;
      e.period_t = now;
    } else if (now - e.period_start >= kSkewPeriodNs) {
      add_point(e, e.period_t, e.period_min, level_resets);
      e.period_start = now;
      e.period_min = d;
      e.period_t = now;
    } else if (d < e.period_min) {
      e.period_min = d;
      e.period_t = now;
    }
  }

  int64_t clock_map_t::to_target_ticks(int64_t arrival_ns, int64_t target_ns, sample_result_t &res) {
    // target 一定在到達時刻附近（predict 是幾個顯示週期）；夾值擋掉不可信的 sampleTime／predictNs
    const int64_t lo = sat_sub(arrival_ns, kTargetClampNs);
    const int64_t hi = sat_add(arrival_ns, kTargetClampNs);
    if (target_ns < lo || target_ns > hi) {
      target_ns = std::clamp(target_ns, lo, hi);
      res.target_clamped = true;
      ++target_clamped_;
    }
    return ns_to_ticks(target_ns, freq_);
  }

  sample_result_t clock_map_t::on_sample(int64_t arrival_ticks, uint64_t sample_time_ns, uint32_t predict_ns, bool via_quic, rtt_input_t rtt) {
    sample_result_t res;
    ++samples_;

    const int64_t arrival_ns = ticks_to_ns(arrival_ticks, freq_);
    // 視窗與週期用單調化的時刻；d 用真實到達時刻
    int64_t now = arrival_ns;
    if (have_now_ && now < last_now_) {
      now = last_now_;
    }
    have_now_ = true;
    last_now_ = now;

    // server ns − client ns（模 2^64 相減再轉回有號：C++20 起是定義行為）
    const int64_t d = (int64_t) ((uint64_t) arrival_ns - sample_time_ns);

    if (have_transport_ && via_quic != cur_quic_) {
      res.transport_switch = true;
      ++transport_switches_;
    }
    have_transport_ = true;
    cur_quic_ = via_quic;
    auto &e = est_[via_quic ? 1 : 0];

    const int64_t rtt_ns = rtt_.feed(now, rtt);
    last_rtt_ns_ = rtt_ns;
    last_rtt_src_ = rtt_.src;

    // 離群：d 比長期下限低 50 ms 以上（上行延遲不可能是負的）→ 多半是 sampleTime 壞掉，不進估計器；
    // 連續 kOutlierAcceptRun 個才承認是 client 時鐘真的跳了
    if (e.ready(now)) {
      int64_t ref = 0;
      if (e.floor_ref(now, ref) && (double) d < (double) ref - (double) kOutlierNs) {
        ++e.outlier_run;
        if (e.outlier_run < kOutlierAcceptRun) {
          ++outliers_;
          res.outlier = true;
          res.offset_ns = applied_;
          // sampleTime 不可信：假設這個樣本沒有排隊延遲，target ≈ 到達 + predict − rtt/2
          const int64_t target_ns = sat_sub(sat_add(arrival_ns, (int64_t) predict_ns), rtt_ns / 2);
          res.target_server_ticks = to_target_ticks(arrival_ns, target_ns, res);
          return res;
        }
        // 真正的時鐘跳動：兩個 transport 的估計器都作廢（skew 保留），重新暖機並直接跟隨
        for (auto &x : est_) {
          x.reset_keep_skew();
        }
        jit_count_ = 0;
        jit_next_ = 0;
        follow_until_ = now + kWarmupNs;
        ++clock_resets_;
        res.clock_reset = true;
      } else {
        e.outlier_run = 0;
      }
    }

    feed_estimator(e, now, d, level_resets_);
    jit_[jit_next_] = {now, d, via_quic};
    jit_next_ = (jit_next_ + 1) % kJitterCap;
    jit_count_ = std::min(jit_count_ + 1, kJitterCap);

    int64_t m2s = d;
    e.m2s(now, m2s);
    const int64_t raw = sat_sub(m2s, rtt_ns / 2);
    last_raw_ = raw;

    if (!applied_init_) {
      applied_init_ = true;
      applied_ = raw;
      follow_until_ = now + kWarmupNs;
    } else if (now < follow_until_) {
      // 暖機（或時鐘重置後）：視窗還在填，m2s 只會往下收斂，直接跟隨
      applied_ = raw;
    } else {
      // 兩次樣本之間以 skew 外插，slew 的預算只用來追真正的偏差
      const int64_t dt = std::min(now - last_apply_, (int64_t) 3600 * kNsPerSec);
      applied_ = e.proj(applied_, last_apply_, now);
      const int64_t diff = sat_sub(raw, applied_);
      if (diff > kStepNs || diff < -kStepNs) {
        res.step = true;
        res.step_ns = diff;
        ++steps_;
        last_step_ns_ = diff;
        applied_ = raw;
      } else if (diff < 0) {
        // 新最小值立即接受
        applied_ = raw;
      } else {
        applied_ += std::min(diff, dt * kSlewNsPerSec / kNsPerSec);
      }
    }
    last_apply_ = now;

    res.offset_ns = applied_;
    // sampleTime + predict + offset（模 2^64；offset 可以是負的）
    const int64_t target_ns = (int64_t) (sample_time_ns + (uint64_t) predict_ns + (uint64_t) applied_);
    res.target_server_ticks = to_target_ticks(arrival_ns, target_ns, res);
    return res;
  }

  stats_t clock_map_t::stats(bool with_jitter) const {
    stats_t s;
    const auto &e = est_[cur_quic_ ? 1 : 0];
    s.ready = e.ready(last_now_);
    s.transport_quic = cur_quic_;
    s.samples = samples_;
    s.offset_ns = applied_;
    s.offset_raw_ns = last_raw_;
    int64_t m = 0;
    const bool have_m = e.m2s(last_now_, m);
    s.m2s_ns = have_m ? m : 0;
    s.rtt_src = last_rtt_src_;
    s.rtt_ns = last_rtt_ns_;
    s.skew_ppm = e.skew_ppm;
    s.skew_points = (uint32_t) e.hist.size();
    s.steps = steps_;
    s.last_step_ns = last_step_ns_;
    s.transport_switches = transport_switches_;
    s.outliers = outliers_;
    s.level_resets = level_resets_;
    s.clock_resets = clock_resets_;
    s.target_clamped = target_clamped_;

    if (with_jitter && s.ready && have_m) {
      // jitter95 = 2 s 視窗內 p95(d_i − m2s)，各樣本先以 skew 投影到同一時刻
      constexpr int64_t window_ns = kBucketNs * kWindowBuckets;
      std::vector<int64_t> r;
      r.reserve(jit_count_);
      for (std::size_t i = 0; i < jit_count_; ++i) {
        const auto &j = jit_[i];
        if (j.quic != cur_quic_ || last_now_ - j.t >= window_ns) {
          continue;
        }
        const double v = (double) e.proj(j.d, j.t, last_now_) - (double) m;
        r.push_back(v <= 0.0 ? 0 : (int64_t) std::min(v, 9.0e18));
      }
      if (!r.empty()) {
        // nearest-rank：第 ceil(0.95·n) 小
        const std::size_t rank = (r.size() * 95 + 99) / 100;
        const std::size_t idx = rank == 0 ? 0 : rank - 1;
        std::nth_element(r.begin(), r.begin() + (std::ptrdiff_t) idx, r.end());
        s.jitter95_ns = r[idx];
      }
    }
    return s;
  }

  // ── STATS TLV 與 log ───────────────────────────────────────────────────

  int32_t tlv_offset_us(int64_t offset_ns) {
    const int64_t us = offset_ns / 1000;  // 向 0 截斷
    return (int32_t) (uint32_t) (uint64_t) us;  // 模 2^32（C++20 起有號轉換是定義行為）
  }

  uint32_t tlv_jitter_us(const stats_t &s) {
    if (!s.ready || s.jitter95_ns < 0) {
      return 0xFFFFFFFFu;
    }
    return (uint32_t) std::min<int64_t>(s.jitter95_ns / 1000, 0xFFFFFFFEll);
  }

  std::string format_stats(const stats_t &s) {
    return std::format(
      "offset={:+} rttMin={} src={} jitter95={} skew={:+.1f} steps={} transport={} ready={} switches={} outliers={} levelResets={} clockResets={} clamped={}",
      s.offset_ns / 1000,
      s.rtt_ns / 1000,
      rtt_src_name(s.rtt_src),
      s.jitter95_ns < 0 ? std::string {"na"} : std::to_string(s.jitter95_ns / 1000),
      s.skew_ppm,
      s.steps,
      s.transport_quic ? "quic" : "enet",
      s.ready ? 1 : 0,
      s.transport_switches,
      s.outliers,
      s.level_resets,
      s.clock_resets,
      s.target_clamped
    );
  }

  // ── pacing_controller_t ────────────────────────────────────────────────

  pacing_controller_t::pacing_controller_t(int64_t tick_frequency):
      freq_ {tick_frequency > 0 && tick_frequency <= kFreqMax ? tick_frequency : kNsPerSec} {
  }

  bool pacing_controller_t::set_period_ns(int64_t period_ns) {
    if (period_ns < kPeriodNsMin || period_ns > kPeriodNsMax) {
      return false;
    }
    period_ns_ = period_ns;
    return true;
  }

  void pacing_controller_t::set_skew_ppm(double skew_ppm) {
    if (!std::isfinite(skew_ppm)) {
      return;
    }
    skew_ppm_ = std::clamp(skew_ppm, -kSkewClampPpm, kSkewClampPpm);
  }

  uint64_t pacing_controller_t::period_q32(int64_t period_ns, double skew_ppm, int64_t tick_frequency) {
    if (period_ns <= 0 || tick_frequency <= 0) {
      return 0;
    }
    const uint64_t p = (uint64_t) period_ns;
    const uint64_t f = (uint64_t) tick_frequency;
    if (p > std::numeric_limits<uint64_t>::max() / f) {
      return 0;
    }
    // 基準值用整數算到四捨五入：A = p·f（ns·tick/s），q0 = A·2^32 / 1e9
    const uint64_t a = p * f;
    const uint64_t ns = (uint64_t) kNsPerSec;
    const uint64_t whole = a / ns;
    if (whole >= (1ull << 31)) {
      return 0;  // 週期 ≥ 2^31 tick：不合理
    }
    const uint64_t rem = a % ns;  // < 1e9，左移 32 位 < 4.3e18，不溢位
    const uint64_t q0 = (whole << 32) + ((rem << 32) + ns / 2) / ns;
    // skew 修正（|skew| ≤ 1000 ppm，修正量 ≤ q0/1000；double 的相對誤差 1e-16 遠小於 1 個單位）
    const double skew = std::isfinite(skew_ppm) ? std::clamp(skew_ppm, -kSkewClampPpm, kSkewClampPpm) : 0.0;
    const double corr = (double) q0 * skew * 1e-6;
    return (uint64_t) ((int64_t) q0 + (int64_t) std::llround(corr));
  }

  pacing_params_t pacing_controller_t::params() const {
    pacing_params_t p;
    p.period_q32 = period_q32(period_ns_, skew_ppm_, freq_);
    p.slew_ppm_max = kSlewPpmMax;
    return p;
  }

  // ── T6 合成測試 ────────────────────────────────────────────────────────

  namespace {
    /// 決定性亂數（splitmix64）：不用 <random> 的分佈，libstdc++／MSVC 的結果才會一致
    struct rng_t {
      uint64_t s;

      uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
      }

      /// [0, 1)
      double uniform() {
        return (double) (next() >> 11) * (1.0 / 9007199254740992.0);
      }
    };

    constexpr int64_t kSimFreq = 10'000'000;  // 10 MHz（兩台測試機實測的 QPF）
    constexpr int64_t kMs = 1'000'000;
    constexpr int64_t kSec = kNsPerSec;

    /// 每個樣本時刻由情境決定的條件
    struct sim_ctl_t {
      bool send = true;  ///< false：這個時刻沒有樣本（中斷）
      bool via_quic = false;
      int64_t extra_up_ns = 0;  ///< 額外上行延遲（不對稱注入、QUIC 路徑差異）
      int64_t client_jump_ns = 0;  ///< client 時鐘的額外位移（sample_time += jump）
      bool garbage_time = false;  ///< sample_time 改成比真值晚 10 s（d ≈ −10 s，離群）
      int64_t quic_rtt_ns = -1;  ///< 給 select_rtt；-1 = 用預設（2·d_min），0 = 不可用
      int64_t enet_rtt_ms = -1;  ///< 給 select_rtt；-1 = 不可用
      uint32_t predict_ns = 33 * kMs;
    };

    struct sim_cfg_t {
      int64_t duration_ns = 60 * kSec;
      int hz = 90;  ///< 樣本率 2×hz（0x5506 的送出率）
      int64_t server_start_ns = 3LL * 86400 * kSec;  ///< server 開機 3 天
      int64_t client_start_ns = 3600LL * kSec;  ///< client 開機 1 小時（θ0 = server − client）
      double skew_ppm = 0.0;  ///< θ(T) = θ0 + skew·(T − start)
      int64_t d_min_ns = 3 * kMs;  ///< 單程最小延遲（上下行相同）
      int64_t jitter_ns = 4 * kMs;  ///< 上行排隊延遲 U[0, jitter)：±2 ms 抖動
      uint64_t seed = 1;
    };

    struct sim_rec_t {
      int64_t t_rel = 0;  ///< 相對開始的 server 時間
      int64_t err_ns = 0;  ///< 套用的 offset − 真實 θ（離群樣本不計）
      int64_t target_err_ns = 0;  ///< target（ns）− 真實目標時刻
      bool outlier = false;
    };

    struct sim_out_t {
      stats_t st;
      std::vector<sim_rec_t> recs;
    };

    using ctl_fn_t = std::function<void(int64_t t_rel, sim_ctl_t &)>;

    sim_out_t run_sim(const sim_cfg_t &cfg, const ctl_fn_t &ctl_fn) {
      clock_map_t cm {kSimFreq};
      rng_t rng {cfg.seed};
      sim_out_t out;
      const int64_t period = kSec / (2 * cfg.hz);
      const int64_t theta0 = cfg.server_start_ns - cfg.client_start_ns;
      out.recs.reserve((std::size_t) (cfg.duration_ns / period) + 1);

      for (int64_t t_rel = 0; t_rel < cfg.duration_ns; t_rel += period) {
        sim_ctl_t c;
        if (ctl_fn) {
          ctl_fn(t_rel, c);
        }
        const double jit = rng.uniform() * (double) cfg.jitter_ns;
        if (!c.send) {
          continue;
        }
        const int64_t t_emit = cfg.server_start_ns + t_rel;
        const int64_t theta = add_ppm(theta0, cfg.skew_ppm, t_rel);
        const int64_t theta_eff = theta - c.client_jump_ns;  // server − client（跳動後）
        const uint64_t sample_time_true = (uint64_t) (t_emit - theta_eff);
        const uint64_t sample_time = c.garbage_time ? sample_time_true + (uint64_t) (10 * kSec) : sample_time_true;
        const int64_t t_arr = t_emit + cfg.d_min_ns + c.extra_up_ns + (int64_t) jit;
        const int64_t quic = c.quic_rtt_ns < 0 ? 2 * cfg.d_min_ns : c.quic_rtt_ns;
        const auto rtt = select_rtt(quic, c.enet_rtt_ms);

        const auto res = cm.on_sample(ns_to_ticks(t_arr, kSimFreq), sample_time, c.predict_ns, c.via_quic, rtt);

        sim_rec_t r;
        r.t_rel = t_rel;
        r.outlier = res.outlier;
        r.err_ns = res.offset_ns - theta_eff;
        const int64_t true_target = (int64_t) (sample_time_true + c.predict_ns) + theta_eff;
        r.target_err_ns = ticks_to_ns(res.target_server_ticks, kSimFreq) - true_target;
        out.recs.push_back(r);
      }
      out.st = cm.stats(true);
      return out;
    }

    /// [from, to) 內非離群樣本的 |err| 最大值
    int64_t max_abs_err(const sim_out_t &o, int64_t from, int64_t to, bool target = false) {
      int64_t m = 0;
      for (const auto &r : o.recs) {
        if (r.outlier || r.t_rel < from || r.t_rel >= to) {
          continue;
        }
        const int64_t v = target ? r.target_err_ns : r.err_ns;
        m = std::max(m, v < 0 ? -v : v);
      }
      return m;
    }

    /// [from, to) 內非離群樣本的 err 平均
    double mean_err(const sim_out_t &o, int64_t from, int64_t to) {
      double sum = 0;
      int64_t n = 0;
      for (const auto &r : o.recs) {
        if (r.outlier || r.t_rel < from || r.t_rel >= to) {
          continue;
        }
        sum += (double) r.err_ns;
        ++n;
      }
      return n ? sum / (double) n : 0.0;
    }

    double us(int64_t ns) {
      return (double) ns / 1000.0;
    }

    std::string common_detail(const sim_out_t &o) {
      return std::format(
        "steps={} switches={} outliers={} levelResets={} clockResets={} clamped={} skew={:+.2f}ppm jitter95={}us",
        o.st.steps,
        o.st.transport_switches,
        o.st.outliers,
        o.st.level_resets,
        o.st.clock_resets,
        o.st.target_clamped,
        o.st.skew_ppm,
        o.st.jitter95_ns < 0 ? -1 : o.st.jitter95_ns / 1000
      );
    }

    // 設計 §S1-12 的門檻：60 s 後 offset 誤差 < 0.3 ms、skew 誤差 < 5 ppm、steps=0。
    // 「60 s 後的 offset 誤差」取最後 5 s 內所有樣本的 |誤差| 最大值（比單點嚴格）。
    constexpr int64_t kErrMaxNs = 300'000;
    constexpr double kSkewErrMaxPpm = 5.0;

    selftest_case_t case_drift(const char *name, double skew_ppm, int64_t client_start_ns, uint64_t seed) {
      sim_cfg_t cfg;
      cfg.skew_ppm = skew_ppm;
      cfg.client_start_ns = client_start_ns;
      cfg.seed = seed;
      const auto o = run_sim(cfg, {});
      const int64_t err = max_abs_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      const int64_t terr = max_abs_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns, true);
      const double skew_err = std::fabs(o.st.skew_ppm - skew_ppm);
      selftest_case_t c;
      c.name = name;
      c.pass = err < kErrMaxNs && terr < kErrMaxNs + 200 && skew_err < kSkewErrMaxPpm && o.st.steps == 0 && o.st.ready && o.st.rtt_src == rtt_src_e::quic;
      c.detail = std::format("offsetErr={:.1f}us targetErr={:.1f}us skewErr={:.2f}ppm (limits 300us/5ppm/steps=0) {}", us(err), us(terr), skew_err, common_detail(o));
      return c;
    }

    selftest_case_t case_convert() {
      int fails = 0;
      std::string first;
      auto check = [&](bool ok, const std::string &what) {
        if (!ok) {
          if (!fails) {
            first = what;
          }
          ++fails;
        }
      };
      for (const int64_t f : {10'000'000LL, 3'579'545LL, 24'000'000LL, 1'000'000'000LL, 2'900'000'000LL}) {
        check(ticks_to_ns(f, f) == kNsPerSec, std::format("ticks_to_ns(1s) f={}", f));
        check(ns_to_ticks(kNsPerSec, f) == f, std::format("ns_to_ticks(1s) f={}", f));
        for (const int64_t t : {1LL, 12'345LL, 987'654'321LL, 2'592'000'000'000'000LL / 1000, 123'456'789'012'345LL}) {
          check(ticks_to_ns(-t, f) == -ticks_to_ns(t, f), std::format("symmetry t={} f={}", t, f));
          // ns 向 0 截斷：換回來最多少 ceil(f/1e9) 個 tick（f ≤ 1e9 時最多少 1）
          const int64_t back = ns_to_ticks(ticks_to_ns(t, f), f);
          const int64_t tol = std::max<int64_t>(1, (f + kNsPerSec - 1) / kNsPerSec);
          check(back <= t && t - back <= tol, std::format("roundtrip t={} f={} back={}", t, f, back));
        }
        check(ticks_to_ns(kI64Max, f) == (f <= kNsPerSec ? kI64Max : ticks_to_ns(kI64Max, f)), std::format("saturate f={}", f));
        check(ns_to_ticks(kI64Max, f) == (f >= kNsPerSec ? kI64Max : ns_to_ticks(kI64Max, f)), std::format("saturate2 f={}", f));
        check(ticks_to_ns(-kI64Max, f) == -ticks_to_ns(kI64Max, f), std::format("saturate sym f={}", f));
      }
      check(ticks_to_ns(10'000'000, 0) == 0 && ns_to_ticks(1, -5) == 0, "freq<=0");
      // 10 MHz：1 tick = 100 ns 恰好
      check(ticks_to_ns(3, kSimFreq) == 300 && ns_to_ticks(299, kSimFreq) == 2, "10MHz exact");
      selftest_case_t c;
      c.name = "clk.convert";
      c.pass = fails == 0;
      c.detail = fails ? std::format("fails={} first={}", fails, first) : "fails=0";
      return c;
    }

    selftest_case_t case_rtt_select() {
      int fails = 0;
      auto check = [&](rtt_input_t r, rtt_src_e src, int64_t ns) {
        if (r.src != src || r.rtt_ns != ns) {
          ++fails;
        }
      };
      check(select_rtt(5 * kMs, 30), rtt_src_e::quic, 5 * kMs);  // QUIC 優先
      check(select_rtt(0, 30), rtt_src_e::enet, 30 * kMs);  // rtt_min 0 = 還沒有樣本
      check(select_rtt(-1, 15), rtt_src_e::enet, 0);  // < 16 ms 當 0
      check(select_rtt(-1, 16), rtt_src_e::enet, 16 * kMs);
      check(select_rtt(-1, 0), rtt_src_e::enet, 0);
      check(select_rtt(-1, -1), rtt_src_e::none, 0);
      check(select_rtt(60 * kSec, -1), rtt_src_e::quic, 10 * kSec);  // 夾上限
      check(select_rtt(-1, 1'000'000), rtt_src_e::enet, 10 * kSec);
      selftest_case_t c;
      c.name = "clk.rtt-select";
      c.pass = fails == 0 && std::string_view {rtt_src_name(rtt_src_e::quic)} == "quic";
      c.detail = std::format("fails={}", fails);
      return c;
    }

    selftest_case_t case_rtt_none() {
      sim_cfg_t cfg;
      cfg.seed = 11;
      const auto o = run_sim(cfg, [](int64_t, sim_ctl_t &c) {
        c.quic_rtt_ns = 0;  // 沒有 QUIC、也沒有 ENet → src=none，offset 含整段單程延遲
      });
      const double bias = mean_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      selftest_case_t c;
      c.name = "clk.rtt-none";
      c.pass = std::fabs(bias - (double) cfg.d_min_ns) < (double) kErrMaxNs && o.st.steps == 0 && o.st.rtt_src == rtt_src_e::none;
      c.detail = std::format("bias={:.1f}us expect={}us+-300 {}", us((int64_t) bias), cfg.d_min_ns / 1000, common_detail(o));
      return c;
    }

    selftest_case_t case_rtt_noise_quic() {
      sim_cfg_t cfg;
      cfg.seed = 12;
      rng_t noise {99};
      const auto o = run_sim(cfg, [&noise, &cfg](int64_t, sim_ctl_t &c) {
        // rtt_min 本來不該跳，這裡故意每個樣本加 U[0, 2 ms) 的雜訊：10 s 最小值濾波要吃掉它
        c.quic_rtt_ns = 2 * cfg.d_min_ns + (int64_t) (noise.uniform() * 2.0 * kMs);
      });
      const int64_t err = max_abs_err(o, 30 * kSec, cfg.duration_ns);
      selftest_case_t c;
      c.name = "clk.rtt-noise-quic";
      c.pass = err < kErrMaxNs && o.st.steps == 0;
      c.detail = std::format("offsetErr(30-60s)={:.1f}us (limit 300us, steps=0) {}", us(err), common_detail(o));
      return c;
    }

    selftest_case_t case_rtt_enet_quantized() {
      sim_cfg_t cfg;
      cfg.seed = 13;
      cfg.d_min_ns = 10 * kMs;  // 真實 RTT 20 ms
      const auto o = run_sim(cfg, [](int64_t t_rel, sim_ctl_t &c) {
        // 沒有 QUIC；ENet 基線在 16／15／31 ms 之間跳（GetTickCount 量化），每 500 ms 換一次
        static constexpr int64_t seq[] = {16, 15, 31};
        c.quic_rtt_ns = 0;
        c.enet_rtt_ms = seq[(t_rel / (500 * kMs)) % 3];
      });
      // 15 → 0，10 s 最小值濾波後固定用 0：offset 偏 +RTT/2 = +10 ms（粗後備的已知偏差），但要穩定
      const double bias = mean_err(o, 30 * kSec, cfg.duration_ns);
      const int64_t lo = 10 * kMs - kErrMaxNs;
      const int64_t hi = 10 * kMs + kErrMaxNs;
      int64_t worst = 0;
      for (const auto &r : o.recs) {
        if (r.t_rel >= 30 * kSec && (r.err_ns < lo || r.err_ns > hi)) {
          worst = std::max(worst, r.err_ns < lo ? lo - r.err_ns : r.err_ns - hi);
        }
      }
      selftest_case_t c;
      c.name = "clk.rtt-enet-quantized";
      c.pass = worst == 0 && o.st.steps == 0 && o.st.rtt_src == rtt_src_e::enet && o.st.rtt_ns == 0;
      c.detail = std::format("bias={:.1f}us expect=10000us+-300 (30-60s) rtt={}us {}", us((int64_t) bias), o.st.rtt_ns / 1000, common_detail(o));
      return c;
    }

    selftest_case_t case_asym(const char *name, int64_t extra_up_ns, uint64_t max_steps, uint64_t seed) {
      sim_cfg_t cfg;
      cfg.seed = seed;
      const auto o = run_sim(cfg, [extra_up_ns, &cfg](int64_t t_rel, sim_ctl_t &c) {
        // 30 s 起只有上行多 extra_up：RTT 跟著變大（rtt_min 反映新路徑），誤差模型 = 不對稱量的一半
        if (t_rel >= 30 * kSec) {
          c.extra_up_ns = extra_up_ns;
          c.quic_rtt_ns = 2 * cfg.d_min_ns + extra_up_ns;
        }
      });
      const int64_t before = max_abs_err(o, 5 * kSec, 30 * kSec);
      const double after = mean_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      const double expect = (double) extra_up_ns / 2.0;
      selftest_case_t c;
      c.name = name;
      c.pass = before < kErrMaxNs && std::fabs(after - expect) < (double) kErrMaxNs && o.st.steps <= max_steps;
      c.detail = std::format("errBefore={:.1f}us errAfter={:.1f}us expect={:.1f}us+-300 stepsLimit={} {}", us(before), after / 1000.0, expect / 1000.0, max_steps, common_detail(o));
      return c;
    }

    selftest_case_t case_transport_switch() {
      sim_cfg_t cfg;
      cfg.seed = 21;
      const auto o = run_sim(cfg, [](int64_t t_rel, sim_ctl_t &c) {
        // 20–40 s 改走 QUIC fallback，那條路的上行多 1 ms
        if (t_rel >= 20 * kSec && t_rel < 40 * kSec) {
          c.via_quic = true;
          c.extra_up_ns = 1 * kMs;
        }
      });
      const int64_t during = max_abs_err(o, 20 * kSec, 40 * kSec);
      const int64_t after = max_abs_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      selftest_case_t c;
      c.name = "clk.transport-switch";
      c.pass = o.st.transport_switches == 2 && o.st.steps == 0 && during < 1 * kMs + kErrMaxNs && after < kErrMaxNs && !o.st.transport_quic;
      c.detail = std::format("errQuicPhase={:.1f}us (limit 1300us) errEnd={:.1f}us (limit 300us) {}", us(during), us(after), common_detail(o));
      return c;
    }

    selftest_case_t case_outlier() {
      sim_cfg_t cfg;
      cfg.seed = 31;
      const int64_t period = kSec / (2 * cfg.hz);
      const auto o = run_sim(cfg, [period](int64_t t_rel, sim_ctl_t &c) {
        // 30 s 起連續 3 個樣本的 sampleTime 壞掉（晚 10 s）
        if (t_rel >= 30 * kSec && t_rel < 30 * kSec + 3 * period) {
          c.garbage_time = true;
        }
      });
      const int64_t err = max_abs_err(o, 5 * kSec, cfg.duration_ns);
      int64_t outlier_target_err = 0;
      for (const auto &r : o.recs) {
        if (r.outlier) {
          // 離群樣本的 target 以到達時刻推算，誤差 = 這個樣本的排隊延遲（< 4 ms）
          const int64_t v = r.target_err_ns < 0 ? -r.target_err_ns : r.target_err_ns;
          outlier_target_err = std::max(outlier_target_err, v);
        }
      }
      selftest_case_t c;
      c.name = "clk.outlier";
      c.pass = o.st.outliers == 3 && o.st.clock_resets == 0 && o.st.steps == 0 && err < kErrMaxNs && outlier_target_err < 5 * kMs;
      c.detail = std::format("offsetErr(5-60s)={:.1f}us outlierTargetErr={:.1f}us (limits 300us/5000us) {}", us(err), us(outlier_target_err), common_detail(o));
      return c;
    }

    selftest_case_t case_clock_jump() {
      sim_cfg_t cfg;
      cfg.seed = 41;
      const auto o = run_sim(cfg, [](int64_t t_rel, sim_ctl_t &c) {
        // 30 s 起 client 時鐘往前跳 1 s（d 一次少 1 s）：先當離群，連續第 8 個才承認並重置
        if (t_rel >= 30 * kSec) {
          c.client_jump_ns = 1 * kSec;
        }
      });
      const int64_t err = max_abs_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      selftest_case_t c;
      c.name = "clk.clock-jump";
      c.pass = o.st.outliers == clock_map_t::kOutlierAcceptRun - 1 && o.st.clock_resets == 1 && err < kErrMaxNs;
      c.detail = std::format("offsetErrEnd={:.1f}us (limit 300us) expect outliers={} clockResets=1 {}", us(err), clock_map_t::kOutlierAcceptRun - 1, common_detail(o));
      return c;
    }

    selftest_case_t case_gap() {
      sim_cfg_t cfg;
      cfg.seed = 51;
      cfg.duration_ns = 70 * kSec;
      cfg.skew_ppm = 50.0;
      const auto o = run_sim(cfg, [](int64_t t_rel, sim_ctl_t &c) {
        // 20–50 s 完全沒有樣本（client 暫停、網路中斷）
        if (t_rel >= 20 * kSec && t_rel < 50 * kSec) {
          c.send = false;
        }
      });
      const int64_t err = max_abs_err(o, cfg.duration_ns - 5 * kSec, cfg.duration_ns);
      selftest_case_t c;
      c.name = "clk.gap";
      c.pass = o.st.steps == 0 && err < kErrMaxNs;
      c.detail = std::format("offsetErrEnd={:.1f}us (limit 300us, steps=0) {}", us(err), common_detail(o));
      return c;
    }

    selftest_case_t case_target_clamp() {
      clock_map_t cm {kSimFreq};
      const auto rtt = select_rtt(6 * kMs, -1);
      const int64_t t0 = 1000 * kSec;
      // 正常樣本 → target ≈ 到達 + predict − 排隊（這裡 0）− rtt/2
      auto r1 = cm.on_sample(ns_to_ticks(t0 + 3 * kMs, kSimFreq), (uint64_t) (t0 - 500 * kSec), 33 * kMs, false, rtt);
      // predictNs = 3 s（不合理）→ 夾到到達 + 1 s
      auto r2 = cm.on_sample(ns_to_ticks(t0 + 10 * kMs, kSimFreq), (uint64_t) (t0 + 7 * kMs - 500 * kSec), 3000 * kMs, false, rtt);
      // d = 500 s + 3 ms，rtt/2 = 3 ms → offset = 500 s；target = (t0 − 500 s) + 33 ms + 500 s
      const int64_t exp1 = ns_to_ticks(t0 + 33 * kMs, kSimFreq);
      const int64_t exp2 = ns_to_ticks(t0 + 10 * kMs + kSec, kSimFreq);
      const auto st = cm.stats(false);
      selftest_case_t c;
      c.name = "clk.target-clamp";
      c.pass = !r1.target_clamped && r1.target_server_ticks == exp1 && r2.target_clamped && r2.target_server_ticks == exp2 && st.target_clamped == 1 && st.jitter95_ns == -1;
      c.detail = std::format("target1Diff={}ticks target2Diff={}ticks clamped={} (expect 0/0/1)", r1.target_server_ticks - exp1, r2.target_server_ticks - exp2, st.target_clamped);
      return c;
    }

    selftest_case_t case_tlv() {
      int fails = 0;
      const int64_t base = 3LL * 86400 * kSec - 3600 * kSec;  // 遠超過 int32 µs
      const int32_t a = tlv_offset_us(base);
      const int32_t b = tlv_offset_us(base + 5 * kMs);
      if ((int32_t) ((uint32_t) b - (uint32_t) a) != 5000) {
        ++fails;
      }
      if (tlv_offset_us(-1234 * 1000) != -1234 || tlv_offset_us(0) != 0) {
        ++fails;
      }
      const int32_t n1 = tlv_offset_us(-base);
      const int32_t n2 = tlv_offset_us(-base - 7 * kMs);
      if ((int32_t) ((uint32_t) n2 - (uint32_t) n1) != -7000) {
        ++fails;
      }
      stats_t s;
      s.ready = false;
      s.jitter95_ns = 5000;
      if (tlv_jitter_us(s) != 0xFFFFFFFFu) {
        ++fails;
      }
      s.ready = true;
      s.jitter95_ns = -1;
      if (tlv_jitter_us(s) != 0xFFFFFFFFu) {
        ++fails;
      }
      s.jitter95_ns = 1'234'567;
      if (tlv_jitter_us(s) != 1234) {
        ++fails;
      }
      s.jitter95_ns = kI64Max;
      if (tlv_jitter_us(s) != 0xFFFFFFFEu) {
        ++fails;
      }
      // log 欄位順序（F.10 分析腳本依賴前七個欄位）
      const auto line = format_stats(s);
      if (line.rfind("offset=+0 rttMin=0 src=none jitter95=", 0) != 0 || line.find(" skew=+0.0 steps=0 transport=enet ready=1 ") == std::string::npos) {
        ++fails;
      }
      selftest_case_t c;
      c.name = "clk.tlv-format";
      c.pass = fails == 0;
      c.detail = std::format("fails={}", fails);
      return c;
    }

    selftest_case_t case_pacing() {
      int fails = 0;
      std::string first;
      auto check = [&](bool ok, const std::string &what) {
        if (!ok) {
          if (!fails) {
            first = what;
          }
          ++fails;
        }
      };
      // 90 Hz @ 10 MHz：11111111 ns × 1e7 / 1e9 = 111111.1111 tick；× 2^32 四捨五入。
      // 參考值另用浮點算（4.77e14 < 2^53，double 可精確到整數）
      const uint64_t q90 = pacing_controller_t::period_q32(11'111'111, 0.0, kSimFreq);
      const uint64_t q90_ref = (uint64_t) std::llround(11'111'111.0 * 1e7 / 1e9 * 4294967296.0);
      check(q90 == q90_ref, std::format("q90={} ref={}", q90, q90_ref));
      // 1e9 頻率（Linux）：tick = ns
      check(pacing_controller_t::period_q32(8'333'333, 0.0, kNsPerSec) == (8'333'333ull << 32), "linux 120Hz");
      // skew：+1000 ppm → × 1.001；超出夾值
      const double r_pos = (double) pacing_controller_t::period_q32(11'111'111, 1000.0, kSimFreq) / (double) q90;
      const double r_clamp = (double) pacing_controller_t::period_q32(11'111'111, 1.0e6, kSimFreq) / (double) q90;
      const double r_neg = (double) pacing_controller_t::period_q32(11'111'111, -200.0, kSimFreq) / (double) q90;
      check(std::fabs(r_pos - 1.001) < 1e-9, std::format("skew+1000 ratio={:.12f}", r_pos));
      check(std::fabs(r_clamp - 1.001) < 1e-9, "skew clamp");
      check(std::fabs(r_neg - 0.9998) < 1e-9, "skew-200");
      check(pacing_controller_t::period_q32(0, 0.0, kSimFreq) == 0 && pacing_controller_t::period_q32(1, 0.0, 0) == 0, "zero inputs");
      check(pacing_controller_t::period_q32(kI64Max, 0.0, kSimFreq) == 0, "overflow guard");
      // controller：預設 90 Hz；不合理的週期被忽略；輸出永遠在 driver 的 [0.95, 1.05] × 1/Hz 內
      pacing_controller_t pc {kSimFreq};
      check(pc.params().period_q32 == q90 && pc.params().slew_ppm_max == 200 && pc.params().flags == 0 && pc.params().mode == 0, "default 90Hz");
      check(!pc.set_period_ns(0) && !pc.set_period_ns(1'000'000'000) && pc.period_ns() == pacing_controller_t::kDefaultPeriodNs, "reject bad period");
      check(pc.set_period_ns(8'333'333), "accept 120Hz");
      pc.set_skew_ppm(std::numeric_limits<double>::quiet_NaN());
      check(pc.skew_ppm() == 0.0, "nan skew ignored");
      for (const double sk : {-5000.0, -1000.0, -200.0, 0.0, 200.0, 1000.0, 5000.0}) {
        pc.set_skew_ppm(sk);
        const double ratio = (double) pc.params().period_q32 / (double) pacing_controller_t::period_q32(8'333'333, 0.0, kSimFreq);
        check(ratio > 0.95 && ratio < 1.05 && std::fabs(ratio - 1.0) <= 0.001 + 1e-12, std::format("window skew={}", sk));
      }
      selftest_case_t c;
      c.name = "pacing.period";
      c.pass = fails == 0;
      c.detail = fails ? std::format("fails={} first={}", fails, first) : std::format("fails=0 q90={}", q90);
      return c;
    }
  }  // namespace

  std::vector<selftest_case_t> run_selftests() {
    std::vector<selftest_case_t> out;
    out.push_back(case_convert());
    out.push_back(case_rtt_select());
    out.push_back(case_tlv());
    out.push_back(case_target_clamp());
    out.push_back(case_pacing());
    // 設計 §S1-12 的門檻情境：固定偏移 + ±2 ms 上行抖動 + 50 ppm 漂移
    out.push_back(case_drift("clk.offset-jitter-drift50", 50.0, 3600LL * kSec, 1));
    // 以下門檻是本模組自訂（設計只給上面那一組）
    out.push_back(case_drift("clk.drift+200", 200.0, 3600LL * kSec, 2));
    out.push_back(case_drift("clk.drift-200", -200.0, 10LL * 86400 * kSec, 3));  // client 開機比 server 久：θ < 0
    out.push_back(case_drift("clk.const-offset", 0.0, 3600LL * kSec, 4));
    out.push_back(case_rtt_none());
    out.push_back(case_rtt_noise_quic());
    out.push_back(case_rtt_enet_quantized());
    out.push_back(case_asym("clk.asym-small", 4 * kMs, 0, 5));
    out.push_back(case_asym("clk.asym-large", 20 * kMs, 2, 6));
    out.push_back(case_transport_switch());
    out.push_back(case_outlier());
    out.push_back(case_clock_jump());
    out.push_back(case_gap());
    return out;
  }
}  // namespace vr::clk
