/**
 * @file src/vr/vr_clock.h
 * @brief VipleStream 2.0 §VR（M1b S1-12、設計 §F.1／§F.2）：client 時鐘 → server 時鐘的對映
 *        （`clock_map_t`）與 pacing 週期前饋（`pacing_controller_t`，M1b 子集）。
 *
 * 純模組：只依賴標準函式庫，不碰 platform／logging／config，也不 include vr_ipc_abi.h
 * （那個檔只允許 x64，這裡要在所有平台都能編譯）。時間一律由呼叫端取好傳入：
 * server 端是 `platf::vr_clock_ticks()` 的 tick（Windows = raw QPC，Linux／macOS =
 * CLOCK_MONOTONIC ns），頻率在建構時給定（`platf::vr_clock_frequency()`）。這樣 selftest
 * 能以合成時間軸決定性地驗證（`run_selftests()`），log 一律由呼叫端印。
 *
 * 執行緒：兩個類別都**不是**執行緒安全的，呼叫端負責序列化（vr::session_state_t 以自己的
 * mutex 包住 clock_map_t；0x5506 handler 會在 ENet control 與 picoquic IO 兩條執行緒上跑）。
 *
 * 估計器（§F.1）：
 *   d_i = arrival_ns − sample_time_ns（server ns − client ns）
 *   每個 transport（ENet／QUIC fallback）各一個估計器：250 ms 一桶記最小值，8 桶滑動 → m2s；
 *   每 2 s 存一個點（該 2 s 的最小 d 與它的到達時刻），保留 60 s，Theil–Sen 斜率 = skew
 *   （夾 ±1000 ppm）；各桶最小值以 skew 投影到「現在」再取最小，所以漂移不會讓 m2s 落後。
 *   offset_raw = m2s − rtt/2（rtt 為所選來源 10 s 內的最小值）。
 *   套用值（兩個 transport 共用一個，切換 transport 不會跳）：兩次樣本之間先以 skew 外插；
 *   差距 > 5 ms 一次跳（step）；往下（新最小值）立即接受；往上以 ≤ 0.5 ms/s slew 追。
 *   暖機（第一個 2 s）直接跟隨。
 *   target_server = sample_time + predict + offset，換成 server tick，夾在到達時刻 ±1 s 內。
 *
 * §F.1 之外的補強（本模組自訂，理由見各常數）：
 *   - 位準跳動：新的 2 s 點偏離外插 > 1.5 ms 就清空 Theil–Sen 歷史（skew 沿用），避免上行延遲
 *     改變後的點對把斜率中位數拉壞。
 *   - 離群：d 比長期下限低 50 ms 以上（單程延遲不可能是負的）→ 不進估計器，target 改用到達時刻
 *     推算；連續 8 個才當成 client 時鐘真的跳了：估計器重置並重新暖機。
 *   - RTT 以 10 s 最小值濾波，吃掉 ENet 15/16 ms 量化在「< 16 ms 當 0」門檻上的來回跳。
 *
 * 誤差模型：offset 誤差 = 上下行最小延遲不對稱量的一半；client 以 0x81 回傳的 render pose
 * 呈現，offset 誤差只影響 SteamVR 的外插量，所以目標是「穩定、不跳」勝過絕對準確。
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr::clk {

  // ── 整數時間換算（純函式；公式同 platf::qpc_ticks_to_ns）──────────────────

  /**
   * @brief tick → ns：(t/f)*1e9 + (t%f)*1e9/f，向 0 截斷（負值對稱），超出範圍對稱飽和到 ±INT64_MAX。
   * @param ticks tick 值（可為負：差值）。
   * @param freq 每秒 tick 數；≤ 0 或 > 9e9（公式會溢位）時回傳 0。
   */
  int64_t ticks_to_ns(int64_t ticks, int64_t freq);

  /**
   * @brief ns → tick：(n/1e9)*f + (n%1e9)*f/1e9，向 0 截斷（負值對稱），超出範圍對稱飽和到 ±INT64_MAX。
   * @param ns 奈秒（可為負）。
   * @param freq 每秒 tick 數；≤ 0 或 > 9e9 時回傳 0。
   */
  int64_t ns_to_ticks(int64_t ns, int64_t freq);

  // ── RTT 來源（§F.1：QUIC rtt_min 優先，ENet 後備）──────────────────────────

  enum class rtt_src_e : uint8_t {
    none = 0,  ///< 沒有可用的 RTT：offset 含整段單程延遲
    enet = 1,  ///< ENet `abr_rtt_baseline()`（ms，GetTickCount 15/16 ms 量化）
    quic = 2,  ///< QUIC path 的 rtt_min（µs 精確度）
  };

  /// log 用名稱：none／enet／quic
  const char *rtt_src_name(rtt_src_e src);

  struct rtt_input_t {
    rtt_src_e src = rtt_src_e::none;
    int64_t rtt_ns = 0;  ///< 來源為 none 時一律 0
  };

  /// ENet 值 < 16 ms 視為 0（GetTickCount 的 15/16 ms 量化，scout F B2）
  inline constexpr int64_t kEnetQuantMs = 16;

  /**
   * @brief 依 §F.1 的優先序選 RTT。
   * @param quic_rtt_min_ns QUIC path 的 rtt_min（ns）；≤ 0 表示沒有 QUIC session 或還沒有樣本
   *        （picoquic 的 rtt_min 在第一個樣本前是 0）。
   * @param enet_rtt_ms ENet 的 RTT 基線（ms）；< 0 表示還沒取樣過。
   * @return QUIC 可用 → quic；否則 ENet 可用 → enet（< 16 ms 當 0）；都沒有 → none、0。
   */
  rtt_input_t select_rtt(int64_t quic_rtt_min_ns, int64_t enet_rtt_ms);

  // ── clock_map_t ───────────────────────────────────────────────────────

  /**
   * @brief on_sample() 的結果。
   */
  struct sample_result_t {
    int64_t target_server_ticks = 0;  ///< sample_time + predict + offset，換成 server tick（已夾值）
    int64_t offset_ns = 0;  ///< 這個樣本使用的 offset（server − client，ns）
    bool step = false;  ///< 這次套用值一次跳了 > 5 ms（呼叫端記 `[VIPLE-VR-CLK] step=`）
    int64_t step_ns = 0;  ///< 跳動量（新 − 舊）
    bool transport_switch = false;  ///< 這個樣本的 transport 與上一個不同
    bool outlier = false;  ///< d 比長期下限低太多：不進估計器，target 改以到達時刻推算
    bool clock_reset = false;  ///< 連續離群被當成真正的 client 時鐘跳動：估計器重置、重新暖機
    bool target_clamped = false;  ///< target 超出到達時刻 ±1 s，已夾值
  };

  /**
   * @brief 統計快照（`[VIPLE-VR-CLK]` 10 秒 log 與 STATS TLV 用）。計數都是累計值。
   */
  struct stats_t {
    bool ready = false;  ///< 目前 transport 的估計器已暖機（累積 ≥ 2 s）
    bool transport_quic = false;  ///< 最近一個樣本走 QUIC fallback
    uint64_t samples = 0;  ///< 餵進來的樣本（含離群）
    int64_t offset_ns = 0;  ///< 目前套用的 offset（server − client）
    int64_t offset_raw_ns = 0;  ///< 最近一次的 m2s − rtt/2
    int64_t m2s_ns = 0;  ///< 目前 transport 的 2 s 視窗最小 d（投影到最近一個樣本的時刻）
    rtt_src_e rtt_src = rtt_src_e::none;  ///< 最近一個樣本使用的 RTT 來源
    int64_t rtt_ns = 0;  ///< 實際使用的 RTT（該來源 10 s 內的最小值）
    int64_t jitter95_ns = -1;  ///< 2 s 視窗內 p95(d − m2s)；-1 = 未就緒或沒有計算
    double skew_ppm = 0.0;  ///< 目前 transport 的漂移估計（server 相對 client 每秒多走的 ppm）
    uint32_t skew_points = 0;  ///< Theil–Sen 歷史點數
    uint64_t steps = 0;  ///< 暖機後 > 5 ms 的一次跳
    int64_t last_step_ns = 0;
    uint64_t transport_switches = 0;
    uint64_t outliers = 0;
    uint64_t level_resets = 0;  ///< 2 s 最小值偏離預測 > 5 ms（壅塞、路徑改變）而清空 Theil–Sen 歷史
    uint64_t clock_resets = 0;  ///< 見 sample_result_t::clock_reset
    uint64_t target_clamped = 0;
  };

  /**
   * @brief 每個 VR session 一個的時鐘對映估計器（§F.1）。非執行緒安全。
   */
  class clock_map_t {
  public:
    // 參數（§F.1；改動要同步更新 run_selftests() 的門檻說明）
    static constexpr int64_t kBucketNs = 250'000'000;  ///< 一桶 250 ms
    static constexpr int kWindowBuckets = 8;  ///< 8 桶 → 2 s 滑動最小值
    static constexpr int64_t kWarmupNs = 2'000'000'000;  ///< 暖機 2 s
    static constexpr int64_t kSkewPeriodNs = 2'000'000'000;  ///< 每 2 s 一個 Theil–Sen 點
    static constexpr int64_t kSkewHistoryNs = 60'000'000'000;  ///< 保留 60 s
    static constexpr std::size_t kSkewMaxPoints = 32;  ///< 60 s ÷ 2 s 再留一點餘裕
    static constexpr std::size_t kSkewMinPoints = 4;  ///< 少於這個點數沿用上一個 skew（初值 0）
    static constexpr int64_t kSkewMinPairSpanNs = 1'000'000'000;  ///< Theil–Sen 只用相距 ≥ 1 s 的點對
    static constexpr double kSkewClampPpm = 1000.0;
    static constexpr int64_t kSlewNsPerSec = 500'000;  ///< 往上追的速度上限 0.5 ms/s
    static constexpr int64_t kStepNs = 5'000'000;  ///< 差距 > 5 ms 才一次跳
    /// 新的 2 s 最小值偏離「上一點 + skew 外插」超過這個值 → 視為位準跳動（上行延遲改變、壅塞、
    /// 換路徑），清空 Theil–Sen 歷史（skew 沿用）。不清的話，位準跳動之後跨越跳動的點對會在
    /// 30 s 內超過一半，中位數斜率變成垃圾。200 ppm 漂移每 2 s 只有 0.4 ms，雜訊約 0.01–0.1 ms。
    static constexpr int64_t kLevelShiftNs = 1'500'000;
    /// 還沒學到 skew（第一次湊滿 kSkewMinPoints 之前）用寬門檻，±1000 ppm 的真實漂移每 2 s 才 2 ms，
    /// 才不會在學到斜率之前一直被自己清掉
    static constexpr int64_t kLevelShiftLooseNs = 5'000'000;
    static constexpr int64_t kOutlierNs = 50'000'000;  ///< d 比長期下限低 50 ms 以上 → 離群
    static constexpr uint32_t kOutlierAcceptRun = 8;  ///< 連續第 8 個離群 → 當成真正的時鐘跳動
    static constexpr int64_t kRttBucketNs = 1'000'000'000;  ///< RTT 最小值濾波：1 s 一桶
    static constexpr int kRttBuckets = 10;  ///< 10 桶 → 10 s
    static constexpr std::size_t kJitterCap = 1024;  ///< 2 s × 2 × 144 Hz = 576，留餘裕
    static constexpr int64_t kTargetClampNs = 1'000'000'000;  ///< target 夾在到達時刻 ±1 s

    /**
     * @param tick_frequency server 時鐘每秒 tick 數（`platf::vr_clock_frequency()`）；≤ 0 或 > 9e9 時當成 1e9。
     */
    explicit clock_map_t(int64_t tick_frequency);

    /**
     * @brief 餵一個 0x5506 樣本，回傳 target（server tick）。
     * @param arrival_ticks handler 一進來取的 `platf::vr_clock_ticks()`。
     * @param sample_time_ns 樣本的 client 單調時鐘（不可信）。
     * @param predict_ns 樣本的 predictNs。
     * @param via_quic 這個樣本經 QUIC fallback（flow 0x04）抵達。
     * @param rtt select_rtt() 的結果（control 執行緒發布的快照）。
     */
    sample_result_t on_sample(int64_t arrival_ticks, uint64_t sample_time_ns, uint32_t predict_ns, bool via_quic, rtt_input_t rtt);

    /**
     * @brief 統計快照。
     * @param with_jitter true 才計算 jitter95（要排序最多 kJitterCap 個值；1 Hz STATS 與 10 秒 log 用），
     *        false 時 jitter95_ns = -1，O(1)，適合每個 control tick 輪詢計數。
     */
    stats_t stats(bool with_jitter = true) const;

    int64_t tick_frequency() const {
      return freq_;
    }

  private:
    struct bucket_t {
      int64_t num = INT64_MIN;  ///< 桶編號（now / 桶寬）；INT64_MIN = 空
      int64_t min_d = 0;
      int64_t t_min = 0;  ///< 最小值樣本的到達時刻（ns）
    };

    struct point_t {
      int64_t t = 0;
      int64_t m = 0;
    };

    struct estimator_t {
      std::array<bucket_t, kWindowBuckets> win {};
      bool any = false;
      int64_t first_ns = 0;  ///< 第一個樣本（或重置後第一個樣本）的時刻
      bool period_has = false;
      int64_t period_start = 0;
      int64_t period_min = 0;
      int64_t period_t = 0;
      std::vector<point_t> hist;
      double skew_ppm = 0.0;
      bool skew_learned = false;  ///< Theil–Sen 至少成功算過一次（重置後保留）
      uint32_t outlier_run = 0;

      void reset_keep_skew();
      bool ready(int64_t now) const;
      int64_t proj(int64_t v, int64_t t, int64_t now) const;
      bool m2s(int64_t now, int64_t &out) const;
      bool floor_ref(int64_t now, int64_t &out) const;
    };

    struct rtt_filter_t {
      rtt_src_e src = rtt_src_e::none;
      std::array<bucket_t, kRttBuckets> win {};

      int64_t feed(int64_t now, rtt_input_t in);
    };

    struct jitter_entry_t {
      int64_t t = 0;
      int64_t d = 0;
      bool quic = false;
    };

    void feed_estimator(estimator_t &e, int64_t now, int64_t d, uint64_t &level_resets);
    void add_point(estimator_t &e, int64_t t, int64_t m, uint64_t &level_resets);
    static void update_skew(estimator_t &e, std::vector<double> &scratch);
    int64_t to_target_ticks(int64_t arrival_ns, int64_t target_ns, sample_result_t &res);

    int64_t freq_;
    std::array<estimator_t, 2> est_ {};  ///< [0] = ENet，[1] = QUIC fallback
    rtt_filter_t rtt_ {};
    std::vector<double> scratch_;  ///< Theil–Sen 斜率暫存（避免每 2 s 配置）

    std::array<jitter_entry_t, kJitterCap> jit_ {};
    std::size_t jit_next_ = 0;
    std::size_t jit_count_ = 0;

    bool have_now_ = false;
    int64_t last_now_ = 0;  ///< 單調化後的最近時刻（兩條執行緒搶鎖可能讓到達時刻小幅倒退）
    bool have_transport_ = false;
    bool cur_quic_ = false;

    bool applied_init_ = false;
    int64_t applied_ = 0;
    int64_t last_apply_ = 0;
    int64_t follow_until_ = 0;  ///< 這之前直接跟隨 offset_raw（初次暖機、時鐘重置後）
    int64_t last_raw_ = 0;
    int64_t last_rtt_ns_ = 0;
    rtt_src_e last_rtt_src_ = rtt_src_e::none;

    uint64_t samples_ = 0;
    uint64_t steps_ = 0;
    int64_t last_step_ns_ = 0;
    uint64_t transport_switches_ = 0;
    uint64_t outliers_ = 0;
    uint64_t level_resets_ = 0;
    uint64_t clock_resets_ = 0;
    uint64_t target_clamped_ = 0;
  };

  // ── STATS TLV（VipleVr.h VIPLE_VR_TLV_STATS）與 log 格式 ─────────────────

  /**
   * @brief clkOffsetUs：offset 的 µs 值取低 32 位（模 2^32）。
   *
   * 兩台機器的單調時鐘原點不同，offset 的絕對值約等於兩邊開機時間差，一般遠超過 int32 µs
   * 的 ±35.8 分鐘；取低 32 位保留「相鄰兩筆的差」（client 以 int32 相減得到區間變化量），
   * 飽和則會把變化量整個吃掉。
   */
  int32_t tlv_offset_us(int64_t offset_ns);

  /// clkOffsetJitterUs：未就緒 → 0xFFFFFFFF（§F.1 暖機）；否則 p95 µs，上限 0xFFFFFFFE
  uint32_t tlv_jitter_us(const stats_t &s);

  /**
   * @brief `[VIPLE-VR-CLK]` 10 秒行與 (final) 行的欄位（不含 tag 與前綴）：
   *        `offset=<±us> rttMin=<us> src=<none|enet|quic> jitter95=<us|na> skew=<±ppm> steps=<n>
   *        transport=<enet|quic> ready=<0|1> switches=<n> outliers=<n> levelResets=<n> clockResets=<n> clamped=<n>`。
   *        前七個欄位的順序與 §F.1 相同（F.10 分析腳本依賴）。
   */
  std::string format_stats(const stats_t &s);

  // ── pacing_controller_t（§F.2 的 M1b 子集）──────────────────────────────

  /**
   * @brief 寫進 vripc_pacing_t 的內容（bridge 負責 seqlock 寫入與 VRIPC_* 常數對映）。
   */
  struct pacing_params_t {
    uint64_t period_q32 = 0;  ///< 虛擬 vsync 週期：server tick × 2^32（已含 skew 修正）
    int64_t anchor_ticks = 0;  ///< M1b 沒有 LATCH：一律 0（flags 不帶 HAS_ANCHOR）
    uint32_t slew_ppm_max = 200;
    uint32_t flags = 0;  ///< M1b：0（沒有 anchor，ALLOW_SNAP 無意義）
    int32_t vsync_event_offset_us = 0;
    uint32_t mode = 0;  ///< 0 = VRIPC_PM_PRODUCTION；非 0 只在 selftest，由呼叫端覆寫
    uint32_t epoch = 0;
  };

  /**
   * @brief server 端的 pacing 週期前饋（§F.2）。M1b 子集：只做週期（client 顯示週期 × (1 + skew)
   *        換成 server tick），沒有 LATCH 相位回授（M4a）。idle generation 也要寫有效週期：
   *        週期沿用上一個 session，沒有就 90 Hz。非執行緒安全。
   */
  class pacing_controller_t {
  public:
    static constexpr int64_t kDefaultPeriodNs = 11'111'111;  ///< 90 Hz（1e9 / 90，截斷）
    static constexpr int64_t kPeriodNsMin = 1'000'000'000 / 500;  ///< 2 ms：比 VIPLE_VR_HZ_MAX 寬很多，只擋錯誤值
    static constexpr int64_t kPeriodNsMax = 1'000'000'000 / 24;  ///< ≈ 41.7 ms
    static constexpr uint32_t kSlewPpmMax = 200;  ///< §F.2 穩態 ±200 ppm
    static constexpr double kSkewClampPpm = clock_map_t::kSkewClampPpm;

    /// @param tick_frequency server 時鐘每秒 tick 數；≤ 0 或 > 9e9 時當成 1e9
    explicit pacing_controller_t(int64_t tick_frequency);

    /**
     * @brief session 的顯示週期（/launch 的 vrPeriodNs，client 時鐘）。
     * @return false 表示超出 [kPeriodNsMin, kPeriodNsMax]，忽略、沿用上一個值。
     */
    bool set_period_ns(int64_t period_ns);

    /// 來自 clock_map_t 的 skew（夾 ±1000 ppm）；session 結束時可以不清，idle 沿用無妨
    void set_skew_ppm(double skew_ppm);

    int64_t period_ns() const {
      return period_ns_;
    }

    double skew_ppm() const {
      return skew_ppm_;
    }

    pacing_params_t params() const;

    /**
     * @brief period_q32 = period_ns × (1 + skew·1e-6) × freq / 1e9 × 2^32（四捨五入）。
     *        skew 先夾 ±1000 ppm；period_ns ≤ 0 或 freq ≤ 0 回傳 0。
     */
    static uint64_t period_q32(int64_t period_ns, double skew_ppm, int64_t tick_frequency);

  private:
    int64_t freq_;
    int64_t period_ns_ = kDefaultPeriodNs;
    double skew_ppm_ = 0.0;
  };

  // ── selftest（T6；純函式、決定性、不印 log）──────────────────────────────

  struct selftest_case_t {
    std::string name;  ///< 例：clk.offset-jitter-drift50
    bool pass = false;
    std::string detail;  ///< 量測值與門檻，單行、只含 ASCII
  };

  /**
   * @brief 跑全部合成情境（10 MHz 合成時鐘、固定亂數種子），約 15 萬個樣本、< 100 ms。
   *        呼叫端逐項印 `[VIPLE-VR-SELFTEST] T6 <name> PASS|FAIL <detail>`。
   */
  std::vector<selftest_case_t> run_selftests();
}  // namespace vr::clk
