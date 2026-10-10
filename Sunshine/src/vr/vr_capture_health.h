/**
 * @file src/vr/vr_capture_health.h
 * @brief VipleStream 2.0 §VR-FENCE-LATE（2026-10-09）：VR 擷取端（display_vr_t）等 driver 的 sharedFence 時用的
 *        四個判定——等待預算、卡死／餓死、拆線的退避、過載旗標。
 *
 * 純邏輯：只用標準函式庫、不碰任何平台標頭、不讀時鐘（時間一律由呼叫端以奈秒整數傳入），可以脫離整個 server
 * 單獨編譯測試（tests/unit/test_vr_capture_health.cpp）。只給一條執行緒用（擷取執行緒）；要給別的執行緒看的
 * 只有 snapshot_t，經 vr::bridge::set_capture_health()／capture_health() 以 atomic 交換。
 *
 * 背景（2026-10-09 事故）：SteamVR 把遊戲貼圖直接當第 0 層交給 driver（直通）時，driver 的合成排在遊戲這一幀的
 * GPU 工作之後，server 等的 fence 實際上是在等「遊戲這一幀算完」。遊戲把 host 的 GPU 壓滿時，每一張的 fence 都比
 * 舊的上限（週期的 70%，夾在 2～6 ms）晚：每一張都被丟，「連續逾時滿 1 s 就拆線」於是每 1.5 s 重連一次，
 * 一張影像都送不出去。driver 沒有壞、fence 也都有做完，拆線完全沒有幫助，還讓 driver 每 1.5 s 進 standby 半秒。
 *
 *   (a) 等待預算 wait_budget_ns()：一張影像從被叫醒起最多等多久
 *       ＝ clamp(3 × 最近的發布間隔（指數平均）, 3 × 顯示週期, 設定的上限)；上限比 3 個顯示週期還短時以上限為準。
 *       直通時 fence 的延遲約等於遊戲一幀的 GPU 時間，而合成器的 Present 節拍跟著遊戲走，所以「一到兩個發布間隔」
 *       就該到；遊戲很慢時發布間隔也跟著變長，不能拿顯示週期當上限。上限防止 Signal 真的遺失時卡太久。
 *   (b) 卡死（stall）與餓死（starve）：取代「連續逾時滿 1 s」，只在開關開著時判定。兩者都從連續失敗的第一張起算、
 *       都要這段期間 driver 發布了至少 30 張（第一張之前發布的不算），都只回報；真正拆線的是呼叫端。
 *       卡死＝完成值連續 3 s 沒有增加。中間有一段沒有影像也照算：之前排著的 Signal 過了那麼久都沒做完，就是卡住。
 *       餓死＝連續沒有一張複製成功，累計滿 5 s（完成值在動、但一直追不上 descriptor 寫的值時，卡死條件抓不到，
 *       靠這一條）。這 5 s 只累計「有影像在來」的時間——相鄰兩張之間最多算 0.5 s：沒有影像的空檔裡沒有東西可以
 *       複製，不算失敗；完成值也不像卡死那樣能證明空檔期間仍然有問題。
 *   (c) 退避：因 (b) 拆線之後，下一次允許拆線的最早時間依序是 5 s、15 s、30 s 之後（之後都是 30 s）；
 *       連續 10 s 健康（複製成功的張數 ≥ 發布張數的一半；以對齊整秒的一秒格逐格判，開頭不滿一秒的那一格不算）
 *       才把階梯歸零。退避不隨 generation 重置。
 *   (d) 過載旗標：最近 1 s「晚到＋放棄」的張數 ≥ 被取用張數的 30% 就設起來，降到 5% 以下持續 2 s 才清掉。
 *       要連續看滿 1 s 的影像才判（視窗裡只有幾張時比例沒有意義；視窗空了就重新起算）。只在開關開著時計算
 *       （關著時恆為 false）。設著的時候 stream.cpp 不把 LATCH 樣本餵給頻率鎖、§VR-PREDICT 不採樣——那時量到的
 *       是 GPU 排隊，不是節拍或預測時間。
 *       已知的限制：「晚到」照規格是「第一段等待逾時」。fence 等待穩定超過第一段等待、但仍然跑滿更新率的遊戲，
 *       每一張都算晚到，旗標會整場設著。2026-10-10 量到了（解析度倍率 300%：仍是每秒 120 幀、旗標整場設著）。
 *       這個旗標的定義沒有改；要不要因此擋 LATCH，可以改看「Present 鎖不鎖得住虛擬 vsync」，見 (e)（預設不改）。
 *   (e) §VR-CADENCE（2026-10-10；config `vr_capture_overload`，只在開關開著時有作用）：LATCH／PREDICT 要不要取樣
 *       看哪個訊號。Present 的節拍由 bridge 執行緒從 driver 的 timing ring 算（vr_present_cadence.h），擷取端讀進來、
 *       隨 on_frame()／on_idle() 交給這裡；這裡只負責合併。
 *       late（預設）：兩者都看 (d) 的旗標——每一步的決定和加這個開關之前相同。節拍照記、照發布（影子），不影響任何決定。
 *       cadence：LATCH——節拍確定在滑動（slipping）就不取樣；確定鎖得住（locked）時只有放棄的影像 ≥ 30%
 *       （fence 真的不動，沒有新影像可量；abandon_overload）才不取樣；說不準（unsure，或沒有報告超過 2 s）照 (d) 的旗標。
 *       PREDICT——(d) 的旗標，或節拍在滑動。「跑滿更新率、每張都晚」因此是 LATCH 照追、PREDICT 不取樣
 *       （相位取模一個週期之後是穩定的，絕對延遲不是）。恆成立：latch_hold ⇒ predict_hold、abandon_overload ⇒ overload。
 *
 * 寫入方（driver）不可信：frame_id、present 時刻、完成值都可能是錯的。這裡不會因為它們而除以零、溢位或卡住；
 * 最壞的結果是預算被夾在設定的上限、或由餓死條件拆線（有退避）。
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vr::capture {

  constexpr int64_t k_ns_per_ms = 1'000'000;
  constexpr int64_t k_ns_per_s = 1'000'000'000;

  // config `vr_capture_late_max_ms` 的範圍與預設（config.cpp 的 int_strict_f 用同一組數字）
  constexpr int k_max_wait_ms_min = 20;
  constexpr int k_max_wait_ms_max = 250;
  constexpr int k_max_wait_ms_default = 100;

  // (a) 等待預算
  constexpr int64_t k_budget_intervals = 3;  ///< 預算＝幾個發布間隔（直通時 fence 約晚一到兩個間隔，留一個間隔的餘裕）
  constexpr int64_t k_interval_ema_div = 8;  ///< 發布間隔的指數平均：每筆樣本佔 1/8（120 Hz 下時間常數約 67 ms）
  constexpr int64_t k_period_default_ns = 11'111'111;  ///< 顯示週期不合理時的預設（90 Hz）
  constexpr int64_t k_period_min_ns = 2 * k_ns_per_ms;  ///< 顯示週期只接受 2～100 ms（500～10 Hz）
  constexpr int64_t k_period_max_ns = 100 * k_ns_per_ms;

  // (b) 卡死與餓死
  constexpr int64_t k_stall_ns = 3 * k_ns_per_s;  ///< 完成值這麼久沒增加＝卡死
  constexpr int64_t k_starve_ns = 5 * k_ns_per_s;  ///< 這麼久沒有一張複製成功＝餓死（只累計有影像在來的時間）
  /// 餓死的累計：相鄰兩張之間最多算這麼久，超過的部分是沒有影像的空檔。取最長的等待上限（250 ms）的兩倍——
  /// 連續失敗時相鄰兩張最遠就隔一個等待預算，中間夾一張過期而沒餵進來的也不會超過
  constexpr int64_t k_starve_gap_max_ns = 500 * k_ns_per_ms;
  constexpr uint64_t k_min_published = 30;  ///< 上面兩段時間內 driver 至少要發布這麼多張才算（沒在發布不是故障）
  constexpr uint64_t k_frame_gap_max = 1024;  ///< frame_id 一次最多跳這麼多（同 bridge 的 check_frame_desc）
  constexpr uint64_t k_fence_lost = std::numeric_limits<uint64_t>::max();  ///< GetCompletedValue() 的「device 掉了」

  // (c) 退避
  constexpr std::array<int64_t, 3> k_backoff_ns = {5 * k_ns_per_s, 15 * k_ns_per_s, 30 * k_ns_per_s};
  constexpr uint32_t k_healthy_seconds = 10;  ///< 連續這麼多個完整而健康的一秒格，退避的階梯才歸零

  // (d) 過載旗標（最近 1 s＝目前這一格＋前 9 格，每格 100 ms；實際涵蓋 0.9～1.0 s）
  constexpr int64_t k_window_bucket_ns = 100 * k_ns_per_ms;
  constexpr std::size_t k_window_buckets = 10;
  constexpr int64_t k_window_ns = k_window_bucket_ns * static_cast<int64_t>(k_window_buckets);
  constexpr uint32_t k_overload_on_pct = 30;  ///< 晚到＋放棄 ≥ 被取用張數的這個百分比：設起來
  /// 低於這個百分比……（嚴格小於。視窗裡的張數隨格子變動，平均剛好 5% 時比例在 5% 上下跳，含不含 5% 分不出來）
  constexpr uint32_t k_overload_off_pct = 5;
  constexpr int64_t k_overload_off_hold_ns = 2 * k_ns_per_s;  ///< ……持續這麼久才清掉
  /// 視窗裡至少要有這麼多張晚到／放棄才設旗標（幀率很低時，單獨一兩張晚到不算過載）。
  /// 另外要連續看滿一整個視窗的影像才判（generation 剛開、或視窗空了——整整 1 s 沒有影像——之後）：視窗裡只有
  /// 幾張時比例會亂跳——每 4 張晚到 1 張（25%）的節奏，頭 9 張裡就有 3 張（33%）。
  constexpr uint32_t k_overload_min_bad = 3;

  // (e) §VR-CADENCE
  /// 確定的節拍（locked／slipping）在報告變成 unknown 之後沿用這麼久，之後照 (d) 的旗標。
  /// （bridge 至少每 100 ms 重寫一次摘要；擷取端把超過 1 s 沒更新的摘要當成 unknown）
  constexpr int64_t k_cadence_keep_ns = 2 * k_ns_per_s;

  /// config `vr_capture_overload`
  enum class overload_mode_e : uint8_t {
    late,  ///< 預設：(d) 的旗標同時擋 LATCH 與 PREDICT（和加這個開關之前逐步相同）
    cadence,  ///< LATCH 看節拍（在滑動，或鎖得住而放棄的影像太多）；PREDICT 看 (d) 的旗標或節拍在滑動
  };

  /// bridge 執行緒算出來的 Present 節拍（vr_present_cadence.h 的 state_e），呼叫端已經確認過摘要夠新
  enum class cadence_e : uint8_t {
    unknown,  ///< 沒有夠新的報告
    locked,  ///< 確定鎖得住
    slipping,  ///< 確定鎖不住
    unsure,  ///< 有紀錄，但下不了結論：照 (d) 的旗標，而且不沿用之前確定的值
  };

  /// 一張影像的處理結果
  enum class outcome_e : uint8_t {
    on_time,  ///< fence 在第一段等待（fence_wait_ms）內到值，已複製
    late,  ///< 第一段逾時、在等待預算內到值，已複製
    abandoned,  ///< 等到預算用完（開關關著時＝第一段逾時）還沒到值：沒有複製
  };

  /// 該拆線的原因
  enum class teardown_e : uint8_t {
    none,
    stall,  ///< 完成值不動
    starve,  ///< 一直沒有複製成功
  };

  inline const char *teardown_name(teardown_e e) {
    switch (e) {
      case teardown_e::none:
        return "none";
      case teardown_e::stall:
        return "stall";
      case teardown_e::starve:
        return "starve";
    }
    return "?";
  }

  /// on_frame() 的輸入：一張走到「等 fence」這一步的影像（過期而沒等的、generation 被換掉而放棄的都不餵）
  struct frame_t {
    outcome_e outcome = outcome_e::on_time;
    int64_t now_ns = 0;  ///< 這一張處理完的時刻
    int64_t wait_ns = 0;  ///< 從被叫醒到 fence 等待結束（到值，或放棄的那一刻）
    int64_t present_ns = 0;  ///< descriptor 的 present 時刻（估發布間隔用；不可信）
    uint64_t frame_id = 0;  ///< descriptor 的 frame_id（估發布了幾張；不可信）
    uint64_t observed = 0;  ///< 等待結束時讀到的完成值（k_fence_lost＝讀不到，不拿來判斷）
    /// (e) 這一張處理完時 bridge 回報的節拍（加在尾端；預設＝沒有報告）。節拍只隨 on_frame()／on_idle() 進來、
    /// 不帶自己的時間：另外開一個帶時間的入口會推進這裡的時鐘，late 模式就不再和之前逐步相同
    cadence_e cadence = cadence_e::unknown;
  };

  /// on_frame() 的回覆
  struct decision_t {
    /// 不是 none＝現在就該拆線。退避已經記下（allowed_in_ns＝下一次最早多久以後才會再回報），
    /// 呼叫端負責 request_teardown 並放掉這個 generation。
    teardown_e teardown = teardown_e::none;
    teardown_e deferred = teardown_e::none;  ///< 判定成立、但還在退避期內：先不拆（allowed_in_ns＝還要多久）
    int64_t allowed_in_ns = 0;
    /// 這一張讓退避的階梯歸零了（連續健康滿 k_healthy_seconds；只在階梯不是 0 時回報）。
    /// 複製成功的與放棄的影像都可能帶這個旗標（一秒格在下一格的第一張結算）。
    bool backoff_reset = false;
  };

  /// log 用的現況（這個 generation）
  struct diag_t {
    /// 完成值上一次前進（讀到的值創新高，或有一張複製成功）到現在；這個 generation 還沒有過＝從 begin_generation 算起
    int64_t since_advance_ns = 0;
    int64_t since_copy_ns = 0;  ///< 上一次複製成功到現在（牆上時間，含沒有影像的空檔）；還沒有過＝從 begin_generation 算起
    uint64_t published = 0;  ///< driver 發布的張數（依 frame_id 的差累計，含被 latest-wins 跳過的）
    uint64_t copied = 0;  ///< 複製成功的張數
    uint64_t completed = 0;  ///< 看過的最大完成值（還沒看過＝0）
  };

  /// 退避狀態（可以存出來、之後還原：擷取端重建 display 物件時用）
  struct backoff_t {
    uint32_t step = 0;  ///< 已經因卡死／餓死拆過幾次（封頂在階梯的長度）
    bool armed = false;  ///< next_allowed_ns 有效
    int64_t next_allowed_ns = 0;  ///< 這個時刻之前不再拆線

    bool operator==(const backoff_t &) const = default;
  };

  /// 給其他執行緒看的快照（display_vr_t 寫、stream.cpp 讀）。預設值＝沒有擷取在跑。
  struct snapshot_t {
    bool overload = false;  ///< 過載旗標（開關關著時恆為 false）
    uint16_t late_permille = 0;  ///< 最近 1 s「晚到＋放棄」佔被取用張數的千分比
    /// 最近 1 s 推給編碼器的張數。視窗實際涵蓋 0.9～1.0 s（目前這一格還沒收滿），所以會比真正的每秒張數少最多
    /// 約一成（穩定每秒 120 張時讀到 109～120）；只供 log 與粗略判斷，要精確的張數看 10 s 行的 copied
    uint16_t pushed_fps = 0;
    // (e) §VR-CADENCE（加在尾端；上面三個欄位在任何模式下意義都不變。開關關著時這四個恆為 false／unknown）
    bool latch_hold = false;  ///< stream.cpp：這一則 LATCH 不餵進頻率鎖（late 模式下等於 overload）
    bool predict_hold = false;  ///< stream.cpp：§VR-PREDICT 不取樣（late 模式下等於 overload）
    bool abandon_overload = false;  ///< 和 overload 同一套 30%／5%／2 s 的規則，只算放棄的影像
    cadence_e cadence = cadence_e::unknown;  ///< 上面兩個決定用的節拍（含 k_cadence_keep_ns 的沿用）

    bool operator==(const snapshot_t &) const = default;
  };

  /// 快照打包成一個 64-bit 值（一個 atomic 就能整份交換）；預設值打包成 0。
  /// bit 0 overload、1 latch_hold、2 predict_hold、3 abandon_overload、4-5 cadence、16-31 late_permille、32-47 pushed_fps
  constexpr uint64_t pack(const snapshot_t &s) {
    return (s.overload ? uint64_t {1} : uint64_t {0}) | (s.latch_hold ? uint64_t {2} : uint64_t {0}) | (s.predict_hold ? uint64_t {4} : uint64_t {0}) |
           (s.abandon_overload ? uint64_t {8} : uint64_t {0}) | (static_cast<uint64_t>(static_cast<uint8_t>(s.cadence) & 3u) << 4) |
           (static_cast<uint64_t>(s.late_permille) << 16) | (static_cast<uint64_t>(s.pushed_fps) << 32);
  }

  constexpr snapshot_t unpack(uint64_t v) {
    snapshot_t s;
    s.overload = (v & 1u) != 0;
    s.latch_hold = (v & 2u) != 0;
    s.predict_hold = (v & 4u) != 0;
    s.abandon_overload = (v & 8u) != 0;
    s.cadence = static_cast<cadence_e>((v >> 4) & 3u);
    s.late_permille = static_cast<uint16_t>((v >> 16) & 0xFFFFu);
    s.pushed_fps = static_cast<uint16_t>((v >> 32) & 0xFFFFu);
    return s;
  }

  namespace detail {
    constexpr int64_t k_i64_max = std::numeric_limits<int64_t>::max();

    /// now − since；since 在未來回 0，差值超過 int64 就飽和（任何輸入都沒有有號溢位）
    constexpr int64_t elapsed(int64_t now, int64_t since) {
      if (now <= since) {
        return 0;
      }
      const uint64_t d = static_cast<uint64_t>(now) - static_cast<uint64_t>(since);
      return d > static_cast<uint64_t>(k_i64_max) ? k_i64_max : static_cast<int64_t>(d);
    }

    /// t + d（d ≥ 0），飽和在 INT64_MAX
    constexpr int64_t add_sat(int64_t t, int64_t d) {
      return t > k_i64_max - d ? k_i64_max : t + d;
    }

    /// floor(t ÷ unit)，unit > 0；負的 t 也往下取（格子的編號不會在 0 附近重複）
    constexpr int64_t floor_div(int64_t t, int64_t unit) {
      const int64_t q = t / unit;
      return t % unit < 0 ? q - 1 : q;
    }
  }  // namespace detail

  class health_t {
  public:
    /**
     * @brief 設定（capture() 進來時呼叫；不清任何狀態）。
     * @param on config `vr_capture_late`。false＝只記數（diag／快照的數字照算），不判卡死／餓死、過載旗標恆為 false
     * @param display_period_ns 串流的幀週期（1e9 ÷ 協商的幀率）；不在 2～100 ms 內就用 90 Hz
     * @param max_wait_ns config `vr_capture_late_max_ms`（夾在 20～250 ms）
     * @param mode config `vr_capture_overload`（(e)；只在 on 為 true 時有作用）。預設 late＝和加這個參數之前相同
     */
    void configure(bool on, int64_t display_period_ns, int64_t max_wait_ns, overload_mode_e mode = overload_mode_e::late) {
      enabled_ = on;
      mode_ = mode;
      period_ns_ = display_period_ns >= k_period_min_ns && display_period_ns <= k_period_max_ns ? display_period_ns : k_period_default_ns;
      max_wait_ns_ = std::clamp<int64_t>(max_wait_ns, k_max_wait_ms_min * k_ns_per_ms, k_max_wait_ms_max * k_ns_per_ms);
      if (!enabled_) {
        overload_ = false;
        below_active_ = false;
        abandon_ = false;
        abandon_below_active_ = false;
      }
    }

    /**
     * @brief 新的 generation 開好了（ring／fence 都是新的）：發布間隔、完成值、卡死／餓死、過載視窗、健康計數全部重來。
     *        退避（階梯與下一次允許拆線的時刻）不動——拆線就是換 generation，跟著重置的話等於沒有退避。
     */
    void begin_generation(int64_t now_ns) {
      const int64_t now = tick(now_ns);
      gen_start_ns_ = now;
      have_frame_ = false;
      last_frame_id_ = 0;
      last_present_ns_ = 0;
      last_fed_ns_ = 0;
      have_interval_ = false;
      interval_ema_ns_ = 0;
      gen_published_ = 0;
      gen_copied_ = 0;
      have_completed_ = false;
      last_completed_ = 0;
      have_advance_ = false;
      last_advance_ns_ = 0;
      have_copy_ = false;
      last_copy_ns_ = 0;
      stall_active_ = false;
      stall_since_ns_ = 0;
      stall_published_ = 0;
      starve_active_ = false;
      starve_run_ns_ = 0;
      starve_published_ = 0;
      win_ = {};
      win_have_ = false;
      win_slot_ = 0;
      win_consumed_ = 0;
      win_bad_ = 0;
      win_copied_ = 0;
      win_span_have_ = false;
      win_span_since_ns_ = 0;
      overload_ = false;
      below_active_ = false;
      below_since_ns_ = 0;
      win_abandoned_ = 0;
      abandon_ = false;
      abandon_below_active_ = false;
      abandon_below_since_ns_ = 0;
      cad_have_ = false;
      cad_slip_ = false;
      cad_seen_ns_ = 0;
      cad_last_ = cadence_e::unknown;
      hb_have_ = false;
      hb_partial_ = false;
      hb_slot_ = 0;
      hb_published_ = 0;
      hb_copied_ = 0;
      healthy_run_ = 0;
    }

    bool enabled() const {
      return enabled_;
    }

    /// (a) 一張影像從被叫醒起最多等多久（含第一段等待）。還沒有發布間隔的樣本時＝3 個顯示週期（不超過上限）。
    int64_t wait_budget_ns() const {
      const int64_t interval = have_interval_ ? interval_ema_ns_ : period_ns_;
      // 兩個乘數都不超過上限（250 ms）或顯示週期的上限（100 ms），乘 3 不會溢位
      const int64_t want = std::max(interval, period_ns_) * k_budget_intervals;
      return std::min(want, max_wait_ns_);  // 上限優先：3 個顯示週期比上限長時也只等到上限
    }

    /// 最近的發布間隔（指數平均）；還沒有樣本回 0
    int64_t interval_ema_ns() const {
      return have_interval_ ? interval_ema_ns_ : 0;
    }

    /**
     * @brief 一張影像處理完（準時／晚到／放棄）。更新所有判定，回報該不該拆線。
     *        準時的那條路徑上只有整數運算：沒有配置、沒有鎖、沒有系統呼叫。
     */
    decision_t on_frame(const frame_t &f) {
      const int64_t now = tick(f.now_ns);
      note_cadence(f.cadence, now);

      // 到這一張為止 driver 又發布了幾張：frame_id 的差（含被 latest-wins 跳過、過期而沒餵進來的）。
      // 寫入方不可信：倒退或沒變當成 1 張，跳太遠夾住。
      uint64_t published = 1;
      if (have_frame_ && f.frame_id > last_frame_id_) {
        published = std::min<uint64_t>(f.frame_id - last_frame_id_, k_frame_gap_max);
      }
      // 發布間隔＝相鄰兩張被取用的影像的 present 時刻差 ÷ 這中間發布的張數。present 沒有往前走（寫入方的錯）就不取樣；
      // 單筆夾在上限內（停了很久之後的第一張不會把平均拉爆，預算本來也不會超過上限）。
      if (have_frame_ && f.present_ns > last_present_ns_) {
        const int64_t span = detail::elapsed(f.present_ns, last_present_ns_);
        const int64_t sample = std::min(span / static_cast<int64_t>(published), max_wait_ns_);
        interval_ema_ns_ = have_interval_ ? interval_ema_ns_ + (sample - interval_ema_ns_) / k_interval_ema_div : sample;
        have_interval_ = true;
      }
      // 上一張餵進來到這一張隔多久（餓死的累計用）：超過 k_starve_gap_max_ns 的部分是沒有影像的空檔，不算
      const int64_t since_fed = have_frame_ ? std::min(detail::elapsed(now, last_fed_ns_), k_starve_gap_max_ns) : 0;
      last_fed_ns_ = now;
      have_frame_ = true;
      last_frame_id_ = f.frame_id;
      last_present_ns_ = f.present_ns;

      const bool copied = f.outcome != outcome_e::abandoned;
      gen_published_ += published;
      if (copied) {
        ++gen_copied_;
      }

      // 完成值有沒有前進。複製成功＝到值了，一定算前進（driver 先 Signal 到後面的值時，完成值可以連續幾張不變）。
      // 第一次看到的值只當基準。讀不到（k_fence_lost）不拿來比。
      bool advanced = copied;
      if (f.observed != k_fence_lost) {
        if (!have_completed_) {
          have_completed_ = true;
          last_completed_ = f.observed;
        } else if (f.observed > last_completed_) {
          last_completed_ = f.observed;
          advanced = true;
        }
      }
      // 卡死／餓死都從連續失敗的第一張起算，不是從上一次成功的時刻：driver 停止發布很久之後才恢復時，「距上一次成功」
      // 早就超過門檻，會在第 30 張就誤判。第一張只算 1 張——它的 published 是「上一張被取用的影像到這一張之間」發布的，
      // 那些在計時開始之前。
      if (advanced) {
        have_advance_ = true;
        last_advance_ns_ = now;
        stall_active_ = false;
        stall_published_ = 0;
      } else if (!stall_active_) {
        stall_active_ = true;
        stall_since_ns_ = now;  // 卡死用牆上時間：之後中間有沒有影像的空檔也照算（排著的 Signal 那段時間也沒做完）
        stall_published_ = 1;
      } else {
        stall_published_ += published;
      }
      if (copied) {
        have_copy_ = true;
        last_copy_ns_ = now;
        starve_active_ = false;
        starve_published_ = 0;
        starve_run_ns_ = 0;
      } else if (!starve_active_) {
        starve_active_ = true;
        starve_published_ = 1;
        starve_run_ns_ = 0;
      } else {
        starve_published_ += published;
        starve_run_ns_ = detail::add_sat(starve_run_ns_, since_fed);  // 餓死只累計有影像在來的時間
      }

      // 最近 1 s 的視窗（過載旗標與快照）
      window_advance(now);
      if (!win_span_have_) {
        win_span_have_ = true;  // 視窗從空的重新開始收：記下起點，收滿一整個視窗才判過載
        win_span_since_ns_ = now;
      }
      auto &b = win_[window_index(win_slot_)];
      ++b.consumed;
      ++win_consumed_;
      // 「晚到」照規格＝第一段等待逾時（檔頭 (d) 的已知限制：穩定但每一張都超過第一段等待的遊戲也會算進來）
      if (f.outcome != outcome_e::on_time) {
        ++b.bad;
        ++win_bad_;
      }
      if (copied) {
        ++b.copied;
        ++win_copied_;
      } else {
        ++b.abandoned;
        ++win_abandoned_;
      }
      b.wait_max_ns = std::max(b.wait_max_ns, f.wait_ns);
      update_overload(now);
      update_abandon(now);

      decision_t d;
      d.backoff_reset = healthy_advance(now, published, copied);
      if (!enabled_) {
        return d;
      }

      // (b) 卡死先判（3 s）；完成值在動但一直沒有複製成功的，由餓死（5 s）接手
      teardown_e why = teardown_e::none;
      if (stall_active_ && detail::elapsed(now, stall_since_ns_) >= k_stall_ns && stall_published_ >= k_min_published) {
        why = teardown_e::stall;
      } else if (starve_active_ && starve_run_ns_ >= k_starve_ns && starve_published_ >= k_min_published) {
        why = teardown_e::starve;
      }
      if (why == teardown_e::none) {
        return d;
      }
      // (c) 退避期內：判定留著（每一張都會再回報 deferred），時間到了的那一張才拆
      if (backoff_.armed && now < backoff_.next_allowed_ns) {
        d.deferred = why;
        d.allowed_in_ns = detail::elapsed(backoff_.next_allowed_ns, now);
        return d;
      }
      const int64_t wait = k_backoff_ns[std::min<std::size_t>(backoff_.step, k_backoff_ns.size() - 1)];
      if (backoff_.step < k_backoff_ns.size()) {
        ++backoff_.step;
      }
      backoff_.armed = true;
      backoff_.next_allowed_ns = detail::add_sat(now, wait);
      ++teardowns_;
      d.teardown = why;
      d.allowed_in_ns = wait;
      // 這個 generation 到此為止（呼叫端會放掉它）；判定歸零，呼叫端多餵一張也不會馬上再回報一次
      stall_active_ = false;
      stall_published_ = 0;
      starve_active_ = false;
      starve_published_ = 0;
      starve_run_ns_ = 0;
      return d;
    }

    /// 沒有影像餵進來的時候定期呼叫（約每 100 ms；影像在來、但都沒走到等 fence 這一步——過期、不合格——時也一樣）：
    /// 讓 1 s 視窗往前走，過載旗標才清得掉。有影像在餵的時候多呼叫也無妨（只推進視窗）。
    /// cadence＝這一刻 bridge 回報的節拍（(e)；同 frame_t::cadence，沒有就不必給）。
    void on_idle(int64_t now_ns, cadence_e cadence = cadence_e::unknown) {
      const int64_t now = tick(now_ns);
      note_cadence(cadence, now);
      window_advance(now);
      update_overload(now);
      update_abandon(now);
    }

    /// (d) 過載旗標（開關關著時恆為 false）
    bool overload() const {
      return overload_;
    }

    /// (e) 擷取端現在用的節拍：確定的報告（locked／slipping）在報告變成 unknown 之後沿用 k_cadence_keep_ns；
    /// unsure 立刻採用（不沿用之前確定的值）。開關關著時恆為 unknown
    cadence_e cadence() const {
      if (!enabled_) {
        return cadence_e::unknown;
      }
      if (cad_have_ && detail::elapsed(last_now_, cad_seen_ns_) <= k_cadence_keep_ns) {
        return cad_slip_ ? cadence_e::slipping : cadence_e::locked;
      }
      return cad_last_ == cadence_e::unsure ? cadence_e::unsure : cadence_e::unknown;
    }

    /// (e) cadence 模式對 LATCH 的決定。不論現在是哪個模式都照算（late 模式下 10 s log 的 cadHold 印的就是它）：
    /// 在滑動 → 不取樣；鎖得住 → 只有放棄的影像太多才不取樣；說不準 → 照 (d) 的旗標
    bool latch_hold_if_cadence() const {
      if (!enabled_) {
        return false;
      }
      switch (cadence()) {
        case cadence_e::slipping:
          return true;
        case cadence_e::locked:
          return abandon_;
        default:
          return overload_;
      }
    }

    /// (e) stream.cpp：LATCH 不餵進頻率鎖（開關關著時恆為 false；late 模式下就是 (d) 的旗標）
    bool latch_hold() const {
      if (!enabled_) {
        return false;
      }
      return mode_ == overload_mode_e::late ? overload_ : latch_hold_if_cadence();
    }

    /// (e) stream.cpp：§VR-PREDICT 不取樣（開關關著時恆為 false；late 模式下就是 (d) 的旗標）。
    /// cadence 模式仍然看 (d) 的旗標——它要的是絕對延遲穩定，晚到的影像在編碼段是雙峰——另外加上節拍在滑動
    bool predict_hold() const {
      if (!enabled_) {
        return false;
      }
      if (mode_ == overload_mode_e::late) {
        return overload_;
      }
      return overload_ || cadence() == cadence_e::slipping;
    }

    /// (e) config `vr_capture_overload`
    overload_mode_e mode() const {
      return mode_;
    }

    /// 給其他執行緒看的快照（數字是上一次 on_frame／on_idle 當下的視窗）
    snapshot_t snapshot() const {
      snapshot_t s;
      s.overload = overload_;
      s.late_permille = win_consumed_ > 0 ? static_cast<uint16_t>(std::min<uint64_t>(static_cast<uint64_t>(win_bad_) * 1000u / win_consumed_, 1000u)) : uint16_t {0};
      s.pushed_fps = static_cast<uint16_t>(std::min<uint32_t>(win_copied_, 0xFFFFu));
      s.latch_hold = latch_hold();
      s.predict_hold = predict_hold();
      s.abandon_overload = abandon_;
      s.cadence = cadence();
      return s;
    }

    /// 最近 1 s 內單張等 fence 最久多久（過載旗標變化時的 log 用）
    int64_t window_wait_max_ns() const {
      int64_t m = 0;
      for (const auto &b : win_) {
        m = std::max(m, b.wait_max_ns);
      }
      return m;
    }

    /// log 用的現況（不改狀態；now 比上一次看到的時刻早時用上一次的）
    diag_t diag(int64_t now_ns) const {
      const int64_t now = have_now_ ? std::max(now_ns, last_now_) : now_ns;
      diag_t g;
      g.since_advance_ns = detail::elapsed(now, have_advance_ ? last_advance_ns_ : gen_start_ns_);
      g.since_copy_ns = detail::elapsed(now, have_copy_ ? last_copy_ns_ : gen_start_ns_);
      g.published = gen_published_;
      g.copied = gen_copied_;
      g.completed = have_completed_ ? last_completed_ : 0;
      return g;
    }

    /// (c) 退避狀態（存出來、還原）
    backoff_t backoff() const {
      return backoff_;
    }

    void restore_backoff(const backoff_t &b) {
      backoff_ = b;
      backoff_.step = std::min<uint32_t>(backoff_.step, static_cast<uint32_t>(k_backoff_ns.size()));
    }

    /// 這個物件回報過幾次「該拆線」（累計）
    uint64_t teardowns() const {
      return teardowns_;
    }

  private:
    struct bucket_t {
      uint32_t consumed = 0;  ///< 走到等 fence 這一步的張數
      uint32_t bad = 0;  ///< 其中晚到＋放棄的
      uint32_t copied = 0;  ///< 其中複製成功的
      uint32_t abandoned = 0;  ///< 其中放棄的（(e)；已含在 bad 裡）
      int64_t wait_max_ns = 0;
    };

    /// 時間只往前：比上一次早的時刻當成和上一次相同（時鐘回繞時所有經過時間都是 0，不會是負的）
    int64_t tick(int64_t now_ns) {
      if (!have_now_ || now_ns > last_now_) {
        have_now_ = true;
        last_now_ = now_ns;
      }
      return last_now_;
    }

    static std::size_t window_index(int64_t slot) {
      const int64_t m = slot % static_cast<int64_t>(k_window_buckets);
      return static_cast<std::size_t>(m < 0 ? m + static_cast<int64_t>(k_window_buckets) : m);
    }

    /// 視窗前進到 now 所在的那一格：經過的格子從合計扣掉、清空（停超過 1 s 就是整個清空）。
    /// 視窗空了就重新起算「看滿 1 s」——不論是一次跳過去，還是 on_idle() 每 100 ms 一格一格走到空的
    void window_advance(int64_t now) {
      const int64_t slot = detail::floor_div(now, k_window_bucket_ns);
      if (!win_have_) {
        win_have_ = true;
        win_slot_ = slot;
        return;
      }
      if (slot <= win_slot_) {
        return;
      }
      const int64_t steps = std::min<int64_t>(slot - win_slot_, static_cast<int64_t>(k_window_buckets));
      for (int64_t i = 0; i < steps; ++i) {
        auto &b = win_[window_index(slot - i)];
        win_consumed_ -= b.consumed;
        win_bad_ -= b.bad;
        win_copied_ -= b.copied;
        win_abandoned_ -= b.abandoned;
        b = bucket_t {};
      }
      win_slot_ = slot;
      if (win_consumed_ == 0) {
        win_span_have_ = false;  // 視窗裡已經沒有任何影像：之後要重新收滿一整個視窗才判過載
      }
    }

    void update_overload(int64_t now) {
      if (!enabled_) {
        overload_ = false;
        return;
      }
      // 整數比較，沒有除法：視窗是空的（沒有影像）時算成 0%
      const uint64_t bad100 = static_cast<uint64_t>(win_bad_) * 100u;
      if (!overload_) {
        const bool full = win_span_have_ && detail::elapsed(now, win_span_since_ns_) >= k_window_ns;
        if (full && win_bad_ >= k_overload_min_bad && bad100 >= static_cast<uint64_t>(win_consumed_) * k_overload_on_pct) {
          overload_ = true;
          below_active_ = false;
        }
        return;
      }
      if (bad100 < static_cast<uint64_t>(win_consumed_) * k_overload_off_pct || win_consumed_ == 0) {
        if (!below_active_) {
          below_active_ = true;
          below_since_ns_ = now;
        } else if (detail::elapsed(now, below_since_ns_) >= k_overload_off_hold_ns) {
          overload_ = false;
          below_active_ = false;
        }
      } else {
        below_active_ = false;
      }
    }

    /// (e) 和 update_overload() 同一套規則（視窗看滿 1 s、≥ 30% 而且至少 3 張才設；< 5% 或視窗空了持續 2 s 才清），
    /// 只算放棄的影像——晚到而複製成功的不算。放棄的是「晚到＋放棄」的子集、門檻與計時都相同，而且緊接在
    /// update_overload() 之後以同一個 now 呼叫，所以 abandon_ 設著時 overload_ 一定也設著
    void update_abandon(int64_t now) {
      if (!enabled_) {
        abandon_ = false;
        return;
      }
      const uint64_t bad100 = static_cast<uint64_t>(win_abandoned_) * 100u;
      if (!abandon_) {
        const bool full = win_span_have_ && detail::elapsed(now, win_span_since_ns_) >= k_window_ns;
        if (full && win_abandoned_ >= k_overload_min_bad && bad100 >= static_cast<uint64_t>(win_consumed_) * k_overload_on_pct) {
          abandon_ = true;
          abandon_below_active_ = false;
        }
        return;
      }
      if (bad100 < static_cast<uint64_t>(win_consumed_) * k_overload_off_pct || win_consumed_ == 0) {
        if (!abandon_below_active_) {
          abandon_below_active_ = true;
          abandon_below_since_ns_ = now;
        } else if (detail::elapsed(now, abandon_below_since_ns_) >= k_overload_off_hold_ns) {
          abandon_ = false;
          abandon_below_active_ = false;
        }
      } else {
        abandon_below_active_ = false;
      }
    }

    /// (e) 這一次 on_frame()／on_idle() 帶進來的節拍。now 就是那次呼叫 tick() 的回傳值：這裡不會推進時鐘，
    /// 也不碰視窗與任何舊狀態（late 模式的決定因此和加這個功能之前逐步相同）
    void note_cadence(cadence_e c, int64_t now) {
      cad_last_ = c;
      if (c == cadence_e::locked || c == cadence_e::slipping) {
        cad_have_ = true;
        cad_slip_ = c == cadence_e::slipping;
        cad_seen_ns_ = now;
      } else if (c == cadence_e::unsure) {
        cad_have_ = false;  // 現在說不準：不沿用之前確定的值
      }
      // unknown：什麼都不改（之前確定的值沿用到 k_cadence_keep_ns 為止）
    }

    /// 每秒一格（對齊整秒）的健康計數；連續 k_healthy_seconds 個完整的格子都健康就把退避歸零。回傳這一次有沒有歸零。
    /// 結算是在「下一格的第一張」做的，所以歸零可能出現在複製成功的那一張上，也可能出現在放棄的那一張上。
    bool healthy_advance(int64_t now, uint64_t published, bool copied) {
      bool reset = false;
      const int64_t slot = detail::floor_div(now, k_ns_per_s);
      if (!hb_have_) {
        hb_have_ = true;
        hb_partial_ = true;  // generation 的第一格是從中途開始收的
        hb_slot_ = slot;
      } else if (slot > hb_slot_) {
        // 上一格結束。健康＝有發布、而且複製成功的張數 ≥ 發布張數的一半（等得比較久而跳過一半的影像仍然算健康）。
        // 不滿一秒的格子（從中途開始收）不健康照樣中斷連續紀錄，健康則不計入——否則「連續 10 s」最短 9 s 多就成立
        const bool healthy = hb_published_ > 0 && hb_copied_ * 2 >= hb_published_;
        if (!healthy) {
          healthy_run_ = 0;
        } else if (!hb_partial_) {
          healthy_run_ = std::min<uint32_t>(healthy_run_ + 1, k_healthy_seconds);
        }
        hb_partial_ = false;
        if (slot - hb_slot_ > 1) {
          healthy_run_ = 0;  // 中間有整秒沒有任何影像：不算連續
          hb_partial_ = true;  // 空檔之後的這一格也是從中途開始收的
        }
        hb_slot_ = slot;
        hb_published_ = 0;
        hb_copied_ = 0;
        if (healthy_run_ >= k_healthy_seconds && (backoff_.step != 0 || backoff_.armed)) {
          backoff_ = backoff_t {};
          reset = true;
        }
      }
      hb_published_ += published;
      if (copied) {
        ++hb_copied_;
      }
      return reset;
    }

    // 設定
    bool enabled_ = false;
    int64_t period_ns_ = k_period_default_ns;
    int64_t max_wait_ns_ = k_max_wait_ms_default * k_ns_per_ms;

    // 時鐘（只往前）
    bool have_now_ = false;
    int64_t last_now_ = 0;

    // 這個 generation
    int64_t gen_start_ns_ = 0;
    bool have_frame_ = false;
    uint64_t last_frame_id_ = 0;
    int64_t last_present_ns_ = 0;
    int64_t last_fed_ns_ = 0;  ///< 上一張餵進來的時刻（have_frame_ 才有意義）
    bool have_interval_ = false;
    int64_t interval_ema_ns_ = 0;
    uint64_t gen_published_ = 0;
    uint64_t gen_copied_ = 0;
    bool have_completed_ = false;
    uint64_t last_completed_ = 0;  ///< 看過的最大完成值
    bool have_advance_ = false;
    int64_t last_advance_ns_ = 0;
    bool have_copy_ = false;
    int64_t last_copy_ns_ = 0;

    // (b) 判定的視窗：從「沒前進／沒複製的第一張」起算
    bool stall_active_ = false;
    int64_t stall_since_ns_ = 0;  ///< 完成值沒前進的第一張的時刻（卡死用牆上時間）
    uint64_t stall_published_ = 0;
    bool starve_active_ = false;
    int64_t starve_run_ns_ = 0;  ///< 連續沒有複製成功的累計時間（只算有影像在來的時間）
    uint64_t starve_published_ = 0;

    // (c) 退避（不隨 generation 重置）與每秒的健康格
    backoff_t backoff_ {};
    uint64_t teardowns_ = 0;
    bool hb_have_ = false;
    bool hb_partial_ = false;  ///< 目前這一格是從中途開始收的（不滿一秒，健康也不計入 healthy_run_）
    int64_t hb_slot_ = 0;
    uint64_t hb_published_ = 0;
    uint64_t hb_copied_ = 0;
    uint32_t healthy_run_ = 0;

    // (d) 最近 1 s 的視窗（環狀，格子編號＝floor(now ÷ 100 ms)）與旗標
    std::array<bucket_t, k_window_buckets> win_ {};
    bool win_have_ = false;
    int64_t win_slot_ = 0;
    uint32_t win_consumed_ = 0;
    uint32_t win_bad_ = 0;
    uint32_t win_copied_ = 0;
    bool win_span_have_ = false;  ///< 視窗從空的開始收的起點有效
    int64_t win_span_since_ns_ = 0;
    bool overload_ = false;
    bool below_active_ = false;
    int64_t below_since_ns_ = 0;

    // (e) §VR-CADENCE：模式、放棄的影像的旗標（和 (d) 共用同一個視窗）、bridge 回報的節拍
    overload_mode_e mode_ = overload_mode_e::late;
    uint32_t win_abandoned_ = 0;
    bool abandon_ = false;
    bool abandon_below_active_ = false;
    int64_t abandon_below_since_ns_ = 0;
    bool cad_have_ = false;  ///< 看過確定的報告（locked／slipping），而且之後沒有被 unsure 取代
    bool cad_slip_ = false;  ///< 那一筆確定的報告是 slipping
    int64_t cad_seen_ns_ = 0;  ///< 最近一筆確定的報告的時刻
    cadence_e cad_last_ = cadence_e::unknown;  ///< 最近一次呼叫帶進來的報告（原樣）
  };

}  // namespace vr::capture
