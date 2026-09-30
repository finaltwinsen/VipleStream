/**
 * @file src/vr/vr_session.cpp
 * @brief VipleStream 2.0 §VR：/launch VR 參數的嚴格解析、協商結果格式化與 VR session 共享狀態。
 *
 * 只依賴標準函式庫與 VipleVr.h（見 vr_session.h 檔頭），log 由呼叫端負責。
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

// local includes
#include "vr_session.h"

namespace vr {

  namespace {
    // vrPeriodNs 的合理性範圍：VipleVr.h 沒有定義，這裡取「最高 Hz 的一半週期」到
    // 「最低 Hz 的兩倍週期」，只擋明顯錯誤的值（0、負值、單位寫錯）。
    constexpr int64_t kPeriodNsMin = 1000000000LL / (VIPLE_VR_HZ_MAX * 2);
    constexpr int64_t kPeriodNsMax = 2LL * 1000000000LL / VIPLE_VR_HZ_MIN;

    // vrEyeToHead：位置分量（mm×100）的合理範圍借用 IPD 上限（±80 mm；眼睛相對頭部中心
    // 的位移不會比這個大），四元數分量（×10000）不可超過 1，且長度要接近 1（容許 2% 的
    // 定點數捨入誤差）。
    constexpr int kEyePosMax = VIPLE_VR_IPD_MAX;
    constexpr int kQuatUnit = 10000;
    constexpr int64_t kQuatNormSqTolerance = 2000000;  // 1e8 的 2%

    // 已排定但 encoder 遲遲沒開始的 wave、或開始後遲遲沒結束的 wave，逾時就當作不存在，
    // 避免 encoder 重建之類的意外讓 LOSS 永遠被吸收。
    constexpr auto kWavePendingTimeout = std::chrono::milliseconds {1000};
    constexpr auto kWaveActiveTimeout = std::chrono::milliseconds {3000};

    // sampleId 一次倒退超過這麼多，視為 client 重新開始計數（例如 tracking 執行緒重啟），
    // 不然 server 會把之後所有樣本都當成亂序、回聲永遠停在舊樣本。
    constexpr int32_t kSampleIdResetThreshold = 1024;

    // outbox 上限：control 執行緒至少每 150 ms（VR session 4 ms）drain 一次，正常不會堆積。
    constexpr std::size_t kOutboxMax = 64;

    /// 十進位整數：可帶一個 '-'（allow_negative 時），不接受 '+'、空白與尾端垃圾
    bool parse_dec(std::string_view s, bool allow_negative, int64_t &out) {
      if (s.empty() || s[0] == '+') {
        return false;
      }
      if (s[0] == '-' && !allow_negative) {
        return false;
      }
      auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out, 10);
      return ec == std::errc {} && ptr == s.data() + s.size();
    }

    /// 十六進位（可帶 0x／0X 前綴），範圍 0..0xFFFFFFFF
    bool parse_hex_u32(std::string_view s, uint32_t &out) {
      if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
      }
      if (s.empty()) {
        return false;
      }
      uint64_t value = 0;
      auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, 16);
      if (ec != std::errc {} || ptr != s.data() + s.size() || value > 0xFFFFFFFFULL) {
        return false;
      }
      out = (uint32_t) value;
      return true;
    }

    std::string range_error(std::string_view name, std::string_view value, int64_t lo, int64_t hi) {
      return std::format("{}={} out of range [{},{}]", name, value, lo, hi);
    }

    /// 解析單一整數欄位並檢查範圍；失敗時寫 error 回傳 false
    bool parse_int_field(std::string_view name, std::string_view value, bool allow_negative, int64_t lo, int64_t hi, int64_t &out, std::string &error) {
      if (!parse_dec(value, allow_negative, out)) {
        error = allow_negative ? std::format("{}='{}' is not a decimal integer", name, value) :
                                 std::format("{}='{}' is not a non-negative decimal integer", name, value);
        return false;
      }
      if (out < lo || out > hi) {
        error = range_error(name, value, lo, hi);
        return false;
      }
      return true;
    }

    /// 解析逗號分隔的整數清單，個數必須剛好 N
    template<std::size_t N>
    bool parse_int_list(std::string_view name, std::string_view value, int64_t lo, int64_t hi, std::array<int, N> &out, std::string &error) {
      std::size_t count = 0;
      std::size_t pos = 0;
      while (true) {
        auto comma = value.find(',', pos);
        auto item = value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        if (count >= N) {
          error = std::format("{} needs exactly {} comma-separated values, got more", name, N);
          return false;
        }
        int64_t v = 0;
        if (!parse_dec(item, true, v)) {
          error = std::format("{}[{}]='{}' is not a decimal integer", name, count, item);
          return false;
        }
        if (v < lo || v > hi) {
          error = std::format("{}[{}]={} out of range [{},{}]", name, count, v, lo, hi);
          return false;
        }
        out[count++] = (int) v;
        if (comma == std::string_view::npos) {
          break;
        }
        pos = comma + 1;
      }
      if (count != N) {
        error = std::format("{} needs exactly {} comma-separated values, got {}", name, N, count);
        return false;
      }
      return true;
    }

    std::string join_ints(const int *values, std::size_t count) {
      std::string out;
      for (std::size_t i = 0; i < count; ++i) {
        if (i) {
          out += ',';
        }
        out += std::to_string(values[i]);
      }
      return out;
    }
  }  // namespace

  parse_result_t parse_launch_params(const query_lookup_t &lookup) {
    parse_result_t result;
    launch_params_t p;
    auto &error = result.error;

    auto fail = [&result](std::string message) {
      result.params.reset();
      result.error = std::move(message);
      return result;
    };

    // vr=1（必填，只接受 "1"）
    {
      auto v = lookup("vr");
      if (!v) {
        return fail("missing vr");
      }
      if (*v != "1") {
        return fail(std::format("vr='{}' must be 1", *v));
      }
    }

    // vrEye=WxH（必填）
    {
      auto v = lookup("vrEye");
      if (!v) {
        return fail("missing vrEye");
      }
      std::string_view eye = *v;
      auto x = eye.find('x');
      if (x == std::string_view::npos || eye.find('x', x + 1) != std::string_view::npos) {
        return fail(std::format("vrEye='{}' must be WxH", eye));
      }
      int64_t w = 0;
      int64_t h = 0;
      if (!parse_int_field("vrEye.W", eye.substr(0, x), false, VIPLE_VR_EYE_DIM_MIN, VIPLE_VR_EYE_DIM_MAX, w, error) ||
          !parse_int_field("vrEye.H", eye.substr(x + 1), false, VIPLE_VR_EYE_DIM_MIN, VIPLE_VR_EYE_DIM_MAX, h, error)) {
        return fail(error);
      }
      p.eye_width = (int) w;
      p.eye_height = (int) h;
    }

    // vrHz（必填）
    {
      auto v = lookup("vrHz");
      if (!v) {
        return fail("missing vrHz");
      }
      int64_t hz = 0;
      if (!parse_int_field("vrHz", *v, false, VIPLE_VR_HZ_MIN, VIPLE_VR_HZ_MAX, hz, error)) {
        return fail(error);
      }
      p.hz = (int) hz;
    }

    // vrPeriodNs（選填，預設 1e9/hz）
    if (auto v = lookup("vrPeriodNs")) {
      if (!parse_int_field("vrPeriodNs", *v, false, kPeriodNsMin, kPeriodNsMax, p.period_ns, error)) {
        return fail(error);
      }
    } else {
      p.period_ns = 1000000000LL / p.hz;
    }

    // vrFov（選填，預設 8×10000；元素可為負值）
    if (auto v = lookup("vrFov")) {
      if (!parse_int_list("vrFov", *v, -VIPLE_VR_FOV_TAN_MAX, VIPLE_VR_FOV_TAN_MAX, p.fov, error)) {
        return fail(error);
      }
    } else {
      p.fov.fill(10000);
    }

    // vrIpd（選填，預設 6300）
    if (auto v = lookup("vrIpd")) {
      int64_t ipd = 0;
      if (!parse_int_field("vrIpd", *v, false, 0, VIPLE_VR_IPD_MAX, ipd, error)) {
        return fail(error);
      }
      p.ipd = (int) ipd;
    }

    // vrEyeToHead（選填；預設左眼 (-ipd/2,0,0)、右眼 (+ipd/2,0,0)、單位四元數）
    if (auto v = lookup("vrEyeToHead")) {
      // 先以最寬的範圍解析（個數、語法、±10000），再逐欄檢查位置與四元數
      if (!parse_int_list("vrEyeToHead", *v, -std::max(kEyePosMax, kQuatUnit), std::max(kEyePosMax, kQuatUnit), p.eye_to_head, error)) {
        return fail(error);
      }
      for (int eye = 0; eye < 2; ++eye) {
        const int *e = &p.eye_to_head[eye * 7];
        for (int i = 0; i < 3; ++i) {
          if (e[i] < -kEyePosMax || e[i] > kEyePosMax) {
            return fail(std::format("vrEyeToHead[{}]={} out of range [{},{}] (position, mm*100)", eye * 7 + i, e[i], -kEyePosMax, kEyePosMax));
          }
        }
        int64_t norm_sq = 0;
        for (int i = 3; i < 7; ++i) {
          if (e[i] < -kQuatUnit || e[i] > kQuatUnit) {
            return fail(std::format("vrEyeToHead[{}]={} out of range [{},{}] (quaternion, x10000)", eye * 7 + i, e[i], -kQuatUnit, kQuatUnit));
          }
          norm_sq += (int64_t) e[i] * e[i];
        }
        const int64_t unit_sq = (int64_t) kQuatUnit * kQuatUnit;
        if (norm_sq < unit_sq - kQuatNormSqTolerance || norm_sq > unit_sq + kQuatNormSqTolerance) {
          return fail(std::format("vrEyeToHead {} eye quaternion is not unit length", eye == 0 ? "left" : "right"));
        }
      }
    } else {
      const int half = p.ipd / 2;
      p.eye_to_head = {-half, 0, 0, 0, 0, 0, kQuatUnit, half, 0, 0, 0, 0, 0, kQuatUnit};
    }

    // vrCaps（必填，十六進位）
    {
      auto v = lookup("vrCaps");
      if (!v) {
        return fail("missing vrCaps");
      }
      if (!parse_hex_u32(*v, p.caps)) {
        return fail(std::format("vrCaps='{}' is not a 32-bit hexadecimal value", *v));
      }
    }

    // vrCodecs（必填，十進位 bitmask）
    {
      auto v = lookup("vrCodecs");
      if (!v) {
        return fail("missing vrCodecs");
      }
      int64_t codecs = 0;
      if (!parse_int_field("vrCodecs", *v, false, 0, 0xFFFFFFFFLL, codecs, error)) {
        return fail(error);
      }
      p.codecs = (uint32_t) codecs;
    }

    // vrCtrl（選填，預設 touch）
    if (auto v = lookup("vrCtrl")) {
      if (*v != "touch" && *v != "index") {
        return fail(std::format("vrCtrl='{}' must be touch or index", *v));
      }
      p.ctrl = *v;
    }

    // vrOverscan（選填，預設 0）
    if (auto v = lookup("vrOverscan")) {
      int64_t overscan = 0;
      if (!parse_int_field("vrOverscan", *v, false, 0, VIPLE_VR_OVERSCAN_MAX, overscan, error)) {
        return fail(error);
      }
      p.overscan = (int) overscan;
    }

    // vrForce（選填，預設 0）
    if (auto v = lookup("vrForce")) {
      if (*v != "0" && *v != "1") {
        return fail(std::format("vrForce='{}' must be 0 or 1", *v));
      }
      p.force = (*v == "1");
    }

    result.params = std::move(p);
    result.error.clear();
    return result;
  }

  std::string format_launch_params(const launch_params_t &p) {
    return std::format(
      "vr=1&vrEye={}x{}&vrHz={}&vrPeriodNs={}&vrFov={}&vrIpd={}&vrEyeToHead={}&vrCaps={:x}&vrCodecs={}&vrCtrl={}&vrOverscan={}&vrForce={}",
      p.eye_width,
      p.eye_height,
      p.hz,
      p.period_ns,
      join_ints(p.fov.data(), p.fov.size()),
      p.ipd,
      join_ints(p.eye_to_head.data(), p.eye_to_head.size()),
      p.caps,
      p.codecs,
      p.ctrl,
      p.overscan,
      p.force ? 1 : 0
    );
  }

  const char *codec_name(codec_e codec) {
    return codec == codec_e::hevc ? "hevc" : "h264";
  }

  std::string format_session_element(const negotiated_t &neg) {
    return std::format(
      "proto={};packed={}x{};hz={};codec={};layout=sbs;overscan={};recovery={};irFrames={};transport=rtp;universeId={};session={};mode={}",
      VIPLE_VR_PROTO_VERSION,
      neg.packed_width(),
      neg.packed_height(),
      neg.params.hz,
      codec_name(neg.codec),
      neg.params.overscan,
      neg.recovery_intra ? "intra" : "idr",
      neg.recovery_intra ? neg.ir_frames : 0,
      // M1b S1-11（K24）：pcvr 的 universeId 固定 0x5649504C（uint32）；stub 維持 0
      neg.pcvr ? 1447645260u : 0u,
      neg.guid,
      neg.pcvr ? "pcvr" : "stub"
    );
  }

  const char *loss_action_name(loss_action_e action) {
    switch (action) {
      case loss_action_e::new_wave:
        return "wave";
      case loss_action_e::duplicate:
        return "absorbed(dup)";
      case loss_action_e::absorbed:
        return "absorbed";
      case loss_action_e::idr:
        return "idr";
    }
    return "?";
  }

  // ── session_state_t ──────────────────────────────────────────────────

  session_state_t::session_state_t(negotiated_t neg, const void *stream_session, int64_t clock_frequency):
      neg_ {std::move(neg)},
      stream_session_ {stream_session} {
    if (clock_frequency > 0) {
      clock_.emplace(clock_frequency);
    }
  }

  bool session_state_t::on_tracking(const VIPLE_VR_TRACKING &sample, bool via_quic, int64_t arrival_ticks, tracking_timing_t *timing) {
    if (timing) {
      *timing = {};
      timing->clock_enabled = clock_.has_value();
      timing->arrival_ticks = arrival_ticks;
    }

    if (sample.version < VIPLE_VR_TRACKING_VERSION || sample.sampleId == 0) {
      pose_bad_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    pose_rx_.fetch_add(1, std::memory_order_relaxed);
    if (via_quic) {
      pose_via_quic_.fetch_add(1, std::memory_order_relaxed);
    }

    // §M1b S1-12：時鐘對映。亂序／重複的樣本也餵（d 仍是有效量測）。估計器的鎖與 pose 的鎖分開、
    // 不巢狀：兩條執行緒搶鎖的順序可能與到達順序相反，估計器內部會把時刻單調化。
    int64_t target_ticks = 0;
    if (clock_) {
      const auto rtt = clk::select_rtt(rtt_quic_ns_.load(std::memory_order_relaxed), rtt_enet_ms_.load(std::memory_order_relaxed));
      clk::sample_result_t res;
      {
        std::lock_guard clk_lk {clock_mtx_};
        res = clock_->on_sample(arrival_ticks, sample.sampleTimeNs, sample.predictNs, via_quic, rtt);
      }
      target_ticks = res.target_server_ticks;
      if (timing) {
        timing->clock_fed = true;
        timing->target_server_ticks = target_ticks;
        timing->clock = res;
      }
    }

    std::lock_guard lk {pose_mtx_};
    if (latest_) {
      const int32_t delta = (int32_t) (sample.sampleId - latest_->sample_id);
      if (delta <= 0 && delta > -kSampleIdResetThreshold) {
        // 倒退或重複：只計數，不覆蓋最新樣本
        pose_ooo_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (delta > 1) {
        pose_gap_.fetch_add((uint64_t) (delta - 1), std::memory_order_relaxed);
      } else if (delta <= -kSampleIdResetThreshold) {
        pose_resets_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    pose_sample_t s;
    s.sample_id = sample.sampleId;
    s.space_epoch = sample.spaceEpoch;
    s.sample_time_ns = sample.sampleTimeNs;
    s.predict_ns = sample.predictNs;
    const auto &hmd = sample.pose[VIPLE_VR_POSE_HMD];
    std::memcpy(s.pos, hmd.pos, sizeof(s.pos));
    std::memcpy(s.rot, hmd.rot, sizeof(s.rot));
    s.arrival_ticks = arrival_ticks;
    s.target_server_ticks = target_ticks;
    latest_ = s;
    last_sample_id_.store(sample.sampleId, std::memory_order_relaxed);
    return true;
  }

  void session_state_t::count_bad_tracking() {
    pose_bad_.fetch_add(1, std::memory_order_relaxed);
  }

  std::optional<pose_sample_t> session_state_t::snapshot() const {
    std::lock_guard lk {pose_mtx_};
    return latest_;
  }

  std::optional<clk::stats_t> session_state_t::clock_stats(bool with_jitter) const {
    if (!clock_) {
      return std::nullopt;
    }
    std::lock_guard lk {clock_mtx_};
    return clock_->stats(with_jitter);
  }

  void session_state_t::expire_stale_wave_locked(std::chrono::steady_clock::time_point now) {
    if (wave_phase_ == wave_phase_e::pending && now - wave_since_ > kWavePendingTimeout) {
      wave_phase_ = wave_phase_e::idle;
    } else if (wave_phase_ == wave_phase_e::active && now - wave_since_ > kWaveActiveTimeout) {
      wave_phase_ = wave_phase_e::idle;
    }
  }

  // client 的兩份 LOSS 相隔約 5 ms（VR_LOSS_TWIN_DELAY_MS）；抖動再大也遠小於這個窗
  constexpr std::chrono::milliseconds kLossTwinWindow {50};

  loss_action_e session_state_t::on_loss(uint32_t first_lost, uint32_t last_lost, uint8_t reason) {
    loss_rx_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lk {wave_mtx_};
    const auto now = std::chrono::steady_clock::now();

    // client 一次送 2 份完全相同的 LOSS（unsequenced 互為備援，相隔約 5 ms），只在短時間窗內
    // 把第二份當重複。RESEND 一律不去重：client 每次重送的 (first, last, reason) 都相同，
    // 沒有時間窗會把第 2、3 次重送全部吞掉，client 只能等 500 ms 退回 IDR。
    if (reason != VIPLE_VR_LOSS_RESEND && have_last_loss_ &&
        last_loss_first_ == first_lost && last_loss_last_ == last_lost && last_loss_reason_ == reason &&
        now - last_loss_time_ < kLossTwinWindow) {
      return loss_action_e::duplicate;
    }
    have_last_loss_ = true;
    last_loss_first_ = first_lost;
    last_loss_last_ = last_lost;
    last_loss_reason_ = reason;
    last_loss_time_ = now;

    if (!neg_.recovery_intra) {
      return loss_action_e::idr;
    }

    expire_stale_wave_locked(now);

    // 已排定的 wave 一定從還沒編碼的幀開始，client 能回報的掉幀都在它之前
    if (wave_phase_ == wave_phase_e::pending) {
      return loss_action_e::absorbed;
    }
    if (wave_phase_ == wave_phase_e::active && is_before32(last_lost, wave_.start_frame)) {
      return loss_action_e::absorbed;
    }

    // 沒有 wave，或 wave 進行中卻掉了 wave 內的幀 → 開新的一波（重啟 wave）
    wave_phase_ = wave_phase_e::pending;
    wave_since_ = now;
    return loss_action_e::new_wave;
  }

  void session_state_t::on_wave_begin(uint32_t start_frame, uint8_t frame_cnt, uint8_t reason) {
    refresh_waves_.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard lk {wave_mtx_};
    wave_phase_ = wave_phase_e::active;
    wave_ = {start_frame, frame_cnt, reason};
    wave_since_ = std::chrono::steady_clock::now();
  }

  void session_state_t::on_wave_end() {
    std::lock_guard lk {wave_mtx_};
    if (wave_phase_ == wave_phase_e::active) {
      wave_phase_ = wave_phase_e::idle;
    }
  }

  void session_state_t::on_wave_abort() {
    std::lock_guard lk {wave_mtx_};
    wave_phase_ = wave_phase_e::idle;
  }

  std::optional<wave_info_t> session_state_t::active_wave() const {
    std::lock_guard lk {wave_mtx_};
    if (wave_phase_ != wave_phase_e::active) {
      return std::nullopt;
    }
    return wave_;
  }

  bool session_state_t::loss_log_gate(uint32_t &suppressed) {
    std::lock_guard lk {wave_mtx_};
    const auto now = std::chrono::steady_clock::now();
    if (loss_log_last_ != std::chrono::steady_clock::time_point {} && now - loss_log_last_ < std::chrono::seconds {1}) {
      ++loss_log_suppressed_;
      return false;
    }
    suppressed = loss_log_suppressed_;
    loss_log_suppressed_ = 0;
    loss_log_last_ = now;
    return true;
  }

  void session_state_t::count_frame(bool tagged) {
    (tagged ? frames_tagged_ : frames_fallback_).fetch_add(1, std::memory_order_relaxed);
  }

  void session_state_t::count_c2s(uint8_t subtype) {
    switch (subtype) {
      case VIPLE_VR_C2S_LATCH:
        latch_rx_.fetch_add(1, std::memory_order_relaxed);
        break;
      case VIPLE_VR_C2S_CLIENT_TIMING:
        timing_rx_.fetch_add(1, std::memory_order_relaxed);
        break;
      default:
        other_c2s_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
  }

  latch::result_t session_state_t::on_latch(const VIPLE_VR_TLV_LATCH &latch, int64_t now_ns) {
    std::lock_guard lk {latch_mtx_};
    latch_last_ = latch_ctl_.on_latch(latch.slackUs, latch.displayPeriodNs, now_ns);
    return latch_last_;
  }

  latch::result_t session_state_t::latch_idle(int64_t now_ns) {
    std::lock_guard lk {latch_mtx_};
    auto r = latch_ctl_.on_idle(now_ns);
    if (r.update) {
      latch_last_ = r;
    }
    return r;
  }

  latch::result_t session_state_t::latch_snapshot() const {
    std::lock_guard lk {latch_mtx_};
    auto r = latch_last_;
    r.update = false;
    r.ppm = latch_ctl_.last_sent_ppm();
    return r;
  }

  void session_state_t::on_client_timing(const uint8_t *body, std::size_t len) {
    VIPLE_VR_TLV_CLIENT_TIMING t {};
    std::memcpy(&t, body, std::min(len, sizeof(t)));
    std::lock_guard lk {timing_mtx_};
    timing_last_ = t;
  }

  std::optional<VIPLE_VR_TLV_CLIENT_TIMING> session_state_t::last_client_timing() const {
    std::lock_guard lk {timing_mtx_};
    return timing_last_;
  }

  bool session_state_t::queue_haptic(uint8_t device, uint32_t duration_us, float frequency_hz, float amplitude) {
    if (device != VIPLE_VR_POSE_LEFT && device != VIPLE_VR_POSE_RIGHT) {
      return false;
    }
    VIPLE_VR_TLV_HAPTIC h {};
    h.device = device;
    h.durationUs = duration_us;
    h.frequencyHz = frequency_hz;
    h.amplitude = amplitude;
    h.eventId = haptic_event_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    queue_s2c(make_tlv(VIPLE_VR_S2C_HAPTIC, &h, sizeof(h)), false);
    haptic_tx_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void session_state_t::count_c2s_truncated() {
    c2s_truncated_.fetch_add(1, std::memory_order_relaxed);
  }

  stats_snapshot_t session_state_t::stats() const {
    stats_snapshot_t s;
    s.pose_rx = pose_rx_.load(std::memory_order_relaxed);
    s.pose_bad = pose_bad_.load(std::memory_order_relaxed);
    s.pose_ooo = pose_ooo_.load(std::memory_order_relaxed);
    s.pose_gap = pose_gap_.load(std::memory_order_relaxed);
    s.pose_via_quic = pose_via_quic_.load(std::memory_order_relaxed);
    s.pose_resets = pose_resets_.load(std::memory_order_relaxed);
    s.last_sample_id = last_sample_id_.load(std::memory_order_relaxed);
    s.frames_tagged = frames_tagged_.load(std::memory_order_relaxed);
    s.frames_fallback = frames_fallback_.load(std::memory_order_relaxed);
    s.loss_rx = loss_rx_.load(std::memory_order_relaxed);
    s.refresh_waves = refresh_waves_.load(std::memory_order_relaxed);
    s.latch_rx = latch_rx_.load(std::memory_order_relaxed);
    s.timing_rx = timing_rx_.load(std::memory_order_relaxed);
    s.other_c2s = other_c2s_.load(std::memory_order_relaxed);
    s.c2s_truncated = c2s_truncated_.load(std::memory_order_relaxed);
    s.s2c_dropped = s2c_dropped_.load(std::memory_order_relaxed);
    return s;
  }

  std::vector<uint8_t> session_state_t::make_tlv(uint8_t subtype, const void *payload, std::size_t len) {
    std::vector<uint8_t> tlv(2 + len);
    tlv[0] = subtype;
    tlv[1] = (uint8_t) len;
    if (len) {
      std::memcpy(tlv.data() + 2, payload, len);
    }
    return tlv;
  }

  void session_state_t::queue_s2c(std::vector<uint8_t> tlv, bool reliable) {
    if (tlv.empty() || tlv.size() > VIPLE_VR_MAX_CTRL_PAYLOAD) {
      s2c_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::lock_guard lk {outbox_mtx_};
    if (outbox_.size() >= kOutboxMax) {
      outbox_.erase(outbox_.begin());
      s2c_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    outbox_.push_back({std::move(tlv), reliable});
  }

  void session_state_t::queue_refresh_start(const wave_info_t &wave) {
    VIPLE_VR_TLV_REFRESH_START rs {};
    rs.startFrame = wave.start_frame;
    rs.frameCnt = wave.frame_cnt;
    rs.reason = wave.reason;
    auto tlv = make_tlv(VIPLE_VR_S2C_REFRESH_START, &rs, sizeof(rs));
    queue_s2c(tlv, false);
    // 第二份延到下一次 drain（下一個 control tick，≥ 一次 enet_host_service 之後）：同一輪
    // 排進去的兩份會被 ENet 打包進同一個 datagram，那個 datagram 掉了就兩份一起掉
    std::lock_guard lk {outbox_mtx_};
    outbox_next_.push_back({std::move(tlv), false});
  }

  void session_state_t::queue_state(uint8_t state, uint8_t progress, uint16_t code) {
    VIPLE_VR_TLV_STATE st {};
    st.state = state;
    st.progress = progress;
    st.code = code;
    queue_s2c(make_tlv(VIPLE_VR_S2C_STATE, &st, sizeof(st)), true);
  }

  void session_state_t::queue_stats() {
    // 所有計數欄位都是 session 開始以來的累計值（VipleVr.h：STATS 走 UNSEQUENCED，
    // 掉一筆也不會漏算，client 自行取差分）。
    // clkOffsetUs／clkOffsetJitterUs（§M1b S1-12）：offset 是兩台機器單調時鐘的原點差，遠超過
    // int32 µs 的範圍，所以取 µs 值的低 32 位（client 只能用相鄰兩筆的 int32 差）；jitter 未就緒
    // （暖機 2 s 內、時鐘對映停用）是 0xFFFFFFFF。staleCount 屬於 driver 端（之後的切片），固定 0。
    VIPLE_VR_TLV_STATS st {};
    st.poseRx = (uint32_t) pose_rx_.load(std::memory_order_relaxed);
    st.poseOutOfOrder = (uint32_t) pose_ooo_.load(std::memory_order_relaxed);
    st.poseGap = (uint32_t) pose_gap_.load(std::memory_order_relaxed);
    st.framesTagged = (uint32_t) frames_tagged_.load(std::memory_order_relaxed);
    st.framesFallback = (uint32_t) frames_fallback_.load(std::memory_order_relaxed);
    st.lossRx = (uint32_t) loss_rx_.load(std::memory_order_relaxed);
    st.refreshWaves = (uint32_t) refresh_waves_.load(std::memory_order_relaxed);
    if (const auto cs = clock_stats(true)) {
      st.clkOffsetUs = clk::tlv_offset_us(cs->offset_ns);
      st.clkOffsetJitterUs = clk::tlv_jitter_us(*cs);
    } else {
      st.clkOffsetUs = 0;
      st.clkOffsetJitterUs = 0xFFFFFFFFu;
    }
    st.staleCount = 0;
    queue_s2c(make_tlv(VIPLE_VR_S2C_STATS, &st, sizeof(st)), false);
  }

  std::vector<s2c_msg_t> session_state_t::drain_s2c() {
    std::vector<s2c_msg_t> out;
    std::lock_guard lk {outbox_mtx_};
    out.swap(outbox_);
    // 延後的那一份留到下一次 drain
    outbox_.swap(outbox_next_);
    return out;
  }

  // ── 全域 active／預約 ────────────────────────────────────────────────

  namespace {
    std::mutex g_active_mtx;
    std::shared_ptr<session_state_t> g_active;
    bool g_reserved = false;
    std::string g_reserved_owner;
    std::chrono::steady_clock::time_point g_reserved_until {};
  }  // namespace

  void set_active(std::shared_ptr<session_state_t> state) {
    std::lock_guard lk {g_active_mtx};
    g_active = std::move(state);
    g_reserved = false;
    g_reserved_owner.clear();
  }

  std::shared_ptr<session_state_t> active() {
    std::lock_guard lk {g_active_mtx};
    return g_active;
  }

  void clear_active_if(const session_state_t *state) {
    std::lock_guard lk {g_active_mtx};
    if (g_active && g_active.get() == state) {
      g_active.reset();
    }
  }

  void reserve(const std::string &owner_uuid, std::chrono::steady_clock::duration ttl) {
    std::lock_guard lk {g_active_mtx};
    g_reserved = true;
    g_reserved_owner = owner_uuid;
    g_reserved_until = std::chrono::steady_clock::now() + ttl;
  }

  void clear_reservation() {
    std::lock_guard lk {g_active_mtx};
    g_reserved = false;
    g_reserved_owner.clear();
  }

  bool busy_for(const std::string &caller_uuid) {
    std::lock_guard lk {g_active_mtx};
    if (g_active) {
      return caller_uuid.empty() || g_active->negotiated().owner_uuid != caller_uuid;
    }
    if (g_reserved && std::chrono::steady_clock::now() < g_reserved_until) {
      return caller_uuid.empty() || g_reserved_owner != caller_uuid;
    }
    return false;
  }
}  // namespace vr
