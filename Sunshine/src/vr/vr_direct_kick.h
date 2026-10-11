/**
 * @file src/vr/vr_direct_kick.h
 * @brief VipleStream 2.0 §VR-DIRECT-KICK（2026-10-10）：host 直通落在「多一格」的時序時，判斷什麼時候把它推回去、
 *        推完有沒有成功（config `vr_direct_kick`，預設關）。
 *
 * 純邏輯：只用標準函式庫、不碰任何平台標頭、不讀時鐘（時間一律由呼叫端以奈秒整數傳入）、穩態不配置記憶體
 * （樣本放在固定大小的環），可以脫離整個 server 單獨編譯測試（tests/unit/test_vr_direct_kick.cpp）。
 * 只給一條執行緒用（bridge 執行緒：drain_rings() 餵樣本、periodic() 呼叫 poll()），沒有鎖。回聲年齡是擷取執行緒
 * 算的，呼叫端要先交接到這條執行緒再餵（vr_bridge.h 的 report_echo_age()；中間的緩衝在 vr_echo_handoff.h）。
 *
 * 背景（docs/steam_frame_client.md「host 直通時的兩種時序」）：SteamVR 的合成器把遊戲的畫面直接交給 driver 時，
 * 有兩種穩定的時序——
 *   A：虛擬 vsync 到了，合成器手上沒有遊戲的新幀 → 等遊戲這一格的幀交出來 → 馬上送；
 *   B：虛擬 vsync 到了，合成器手上已經有一張遊戲上一格就交好的幀 → 立刻送那一張，遊戲這一格畫的要等下一個
 *      vsync 才送：多一個 host 週期的延遲（60 Hz 是 16.7 ms）。
 * 遊戲停頓很久或儀表板開過再關會進 B；60 Hz（半速串流）的 B 會一直留著。分得出來的量有兩個：
 *   presentPhase（driver 的 PRESENT_ENTER 距離上一次 POSTPRESENT_EXIT 的時間）：A 約等於遊戲一幀的時間（輕負載
 *     約 3.3 ms），B 只有合成器自己的時間（0.7～1.1 ms）；另有 S＝host 的 SteamVR 儀表板開著、合成器自己合成
 *     （中位數 1.2～2.1 ms）。
 *   回聲年齡（擷取端對每一張收下的影像算：這一張 Present 的時刻減掉「它用的那個姿態被發布給 driver 的時刻」；
 *     沒有對到姿態的影像沒有這個值）：60 Hz 的 A 中位數 4.4～7.7 ms（無人與配戴相同；120 Hz 的 A 是 5～7 ms），
 *     B 是 17.7～20.7 ms——多出來的就是那一個 host 週期。
 * 原本只看 presentPhase，2026-10-11 在實際的場次看到誤判：SteamVR 剛啟動、遊戲還在載入的那一兩分鐘，第 0 層是
 * 合成器自己的畫面（不是遊戲的貼圖），presentPhase 中位數 0.79～1.22 ms、第 90 百分位 1.3～1.4 ms，和 B 分不開——
 * 連 kick 三次、每次的結果都是沒有變化，才進入退避。那段時間回聲年齡的中位數只有 2.7～5.2 ms：根本沒有多一格。
 * 所以 (a) 改成兩個量都要成立；回聲年齡量不到的時候寧可不 kick。
 * 推回去的方法（kick）：讓 driver 的 PostPresent 立刻返回一小段時間，合成器在一個週期內多跑一輪，排著的那一張
 * 用掉了，下一輪只能等遊戲的新幀。實際寫節拍的是呼叫端（vr_bridge_win.cpp），這裡只做判斷：
 *
 *   (a) 判定為 B：最近 1 秒內的兩種樣本都成立——
 *         presentPhase：至少 24 筆，中位數 < 1.15 ms 而且第 90 百分位 < 2.2 ms；
 *         回聲年齡：至少 24 筆，中位數 ≥ 5.5 ms（A 的典型值）＋半個週期，也就是 A 與 B 的正中間：60 Hz 是
 *           13.8 ms、72 Hz 12.4、90 Hz 11.1（後兩個是照同一條式子算的，沒有量測）。
 *       沒有回聲年齡的樣本（呼叫端沒有餵、沒有串流在擷取、影像都沒有對到姿態）就不可能成立。兩邊的 24 筆是各自的
 *       常數；回聲年齡的那一個在 60 Hz 等於每秒要有四成的影像對到姿態（對到的比例掉到四成上下，真的在 B 也不一定
 *       kick 得出去；三成以下不會）。
 *       每 100 ms 評估一次，連續成立滿 3 秒（每一次評估都成立；有一次不成立、或兩次評估隔了超過 1 秒，就重新計時）
 *       → 回報 kick。沒有 Present 的時候兩個視窗都會自己變空（不到 24 筆＝不成立），不必另外清。
 *   (b) kick 的長度＝週期 × 1.25，夾在 20～40 ms。
 *   (c) kick 之後前 500 ms 的樣本不看（含 kick 本身那幾張）；接著 1.5 秒的樣本拿來確認：至少 24 筆而且中位數
 *       ≥ 2.0 ms＝回到 A（退避歸零），否則＝沒有變化，下一次最早要等 10、30、90、300 秒之後（之後都是 300 秒）。
 *       樣本不到 24 筆（那 1.5 秒幾乎沒有 Present）量不出結果，照「沒有變化」處理。
 *       確認只看 presentPhase；同一段的回聲年齡中位數只帶在結果裡給 log 用。
 *   (d) 每一次 kick 離上一次（含呼叫端自己另外做的，見 on_external_kick）至少 5 秒。
 *   (e) 只在週期 ≥ 11 ms（約 90 Hz 以下）時動作：120 Hz 的 B 自己會消失（常有一幀超過 8.3 ms）。
 *   (f) 重負載：成功的那一次，確認用的樣本第 90 百分位 > 週期的 80% → 這個 generation 之後不再 kick。A 要等遊戲
 *       這一幀的 GPU 做完，遊戲的幀佔掉大半個週期時留在 B 不一定比較差；我們沒有辦法把它推回 B，至少不要再動它。
 *   (g) 一個 generation 最多 kick 20 次；reset() 之後前 5 秒不評估（遊戲與 SteamVR 還在起來）。
 *   (h) presentPhase 那一邊已經連續成立滿 3 秒（只看它的舊規則這時就 kick 了），這一次評估回聲年齡卻不成立或根本
 *       沒有：不 kick，回報一次 withheld（帶兩邊的統計量）讓呼叫端印一行。沒有這一行的話，「回聲年齡正確地擋下來」
 *       和「回聲年齡沒有交接過來」在 log 裡一樣都是什麼都沒有；真的在 B 卻沒有 kick（頭不動、對到姿態的影像不到
 *       四成）時也無從查起。同一個 generation 最多每 60 秒一次。它說的是判定、不是 kick，所以不看退避與 (d) 的間隔；
 *       等結果的那 2 秒與「不再 kick」之後不評估，也就不會有。
 *
 * 寫入方（driver）不可信：樣本的時刻與數值都可能是錯的（回聲年齡用的 Present 時刻也是它寫的）。這裡不會因為它們
 * 而溢位、除以零或卡住——時刻在未來的樣本要等「現在」追上它才算，太舊的不算，負的數值直接丟；最壞的結果是誤判，
 * 而誤判的代價有上限（(d)、(g)）。
 * 回聲年齡的限制：
 *   - driver 是拿遊戲算圖用的姿態去比對它回報過的樣本，頭部靜止的無人場次每一筆樣本的姿態都一樣，分不出是哪一筆，
 *     回聲年齡不可靠——那種場次就算在 B 也可能不 kick（要驗這個功能得讓頭動起來）。
 *   - (a) 只比中位數的下限。回聲年齡是 Present 的時刻減掉姿態被發布的時刻；追蹤樣本一陣一陣到的時候，每一張都多出
 *     「離上一批多久」（driver 只在 Present 前 100 ms 內的樣本裡找）。用合成的序列試過（60 Hz、合成器自己的畫面、
 *     回聲年齡＝2.7 ms＋0～G 均勻）：G 在 20 ms 以下不會誤判，22 ms 時 20 場有 12 場，24 ms 以上每一場都和只看
 *     presentPhase 時一樣每 120 秒推 3 次——這個條件形同不存在（要持續 3 秒以上才會發生；代價仍由退避與 (d)、(g)
 *     封頂）。真的在 B 時每一張都至少舊一格，最小的那幾筆照理也在門檻以上，所以 kick 另外帶第 10 百分位給 log
 *     （decision_t::echo_p10_before_ns）。目前只印不判：還沒有實際的數據，等收到再決定要不要把它加進 (a)。
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vr::kick {

  inline constexpr int64_t k_ns_per_ms = 1'000'000;
  inline constexpr int64_t k_ns_per_s = 1'000'000'000;

  inline constexpr int64_t k_min_period_ns = 11 * k_ns_per_ms;  ///< (e) 週期比這個短就不動作

  // (a) 判定為 B
  inline constexpr int64_t k_window_ns = k_ns_per_s;  ///< 判定用的視窗：最近這麼久的樣本
  /// presentPhase：視窗（與確認用的那一段）裡至少要有這麼多筆
  inline constexpr int k_min_samples = 24;
  inline constexpr int64_t k_b_median_max_ns = 1'150'000;  ///< presentPhase 的中位數要小於這個值
  inline constexpr int64_t k_b_p90_max_ns = 2'200'000;  ///< presentPhase 的第 90 百分位要小於這個值
  /// 回聲年齡：視窗裡至少要有這麼多筆。和 k_min_samples 是兩個常數（要調可以各調各的）：回聲年齡只有對到姿態的影像
  /// 才有，所以這個數等於「每秒至少幾張影像要對到姿態」——60 Hz 是四成、72 Hz 三分之一、90 Hz 27%。
  /// 確認用的那一段不到這麼多筆時，帶在結果裡的中位數不可信（log 印 n/a）
  inline constexpr int k_echo_min_samples = 24;
  /// 回聲年齡的中位數要 ≥ 這個值＋週期 × 1/2。這個值＝等遊戲這一格（A）時的典型回聲年齡；多一格（B）時多一個
  /// 週期，門檻就在兩者的正中間（60 Hz：A 4.4～7.7 ms、門檻 13.8 ms、B 17.7～20.7 ms）
  inline constexpr int64_t k_echo_a_typical_ns = 5'500'000;
  inline constexpr int64_t k_echo_period_num = 1;
  inline constexpr int64_t k_echo_period_den = 2;
  inline constexpr int64_t k_dwell_ns = 3 * k_ns_per_s;  ///< 連續成立這麼久才 kick
  /// 多久評估一次。poll() 每 2 ms 就被呼叫一次，不必每次都排序；不可以超過 k_window_ns
  /// （兩次評估隔得比視窗長時，中間有樣本沒被看到，連續成立的計時會一直重來）
  inline constexpr int64_t k_eval_interval_ns = 100 * k_ns_per_ms;

  // (b) kick 的長度＝週期 × 5/4，夾在下面兩個值之間
  inline constexpr int64_t k_burst_period_num = 5;
  inline constexpr int64_t k_burst_period_den = 4;
  inline constexpr int64_t k_burst_min_ns = 20 * k_ns_per_ms;
  inline constexpr int64_t k_burst_max_ns = 40 * k_ns_per_ms;

  // (c) 確認與退避
  inline constexpr int64_t k_settle_ns = 500 * k_ns_per_ms;  ///< kick 之後這麼久的樣本不看（含 kick 本身）
  inline constexpr int64_t k_verify_ns = 1500 * k_ns_per_ms;  ///< 接著收這麼久的樣本來確認
  inline constexpr int64_t k_a_median_min_ns = 2'000'000;  ///< 確認時中位數 ≥ 這個值＝回到 A
  inline constexpr std::array<int64_t, 4> k_backoff_ns = {10 * k_ns_per_s, 30 * k_ns_per_s, 90 * k_ns_per_s, 300 * k_ns_per_s};

  inline constexpr int64_t k_min_gap_ns = 5 * k_ns_per_s;  ///< (d) 兩次 kick 之間至少隔這麼久

  // (f) 重負載：確認時第 90 百分位 > 週期 × 4/5
  inline constexpr int64_t k_heavy_period_num = 4;
  inline constexpr int64_t k_heavy_period_den = 5;

  // (g)
  inline constexpr int64_t k_startup_holdoff_ns = 5 * k_ns_per_s;  ///< reset() 之後這麼久不評估
  inline constexpr int k_max_kicks = 20;  ///< 一個 generation 最多 kick 這麼多次

  // (h)
  inline constexpr int64_t k_withheld_interval_ns = 60 * k_ns_per_s;  ///< 兩次 withheld 之間至少隔這麼久

  /// 樣本環的大小（presentPhase 與回聲年齡各一個）。要裝得下確認用的 1.5 秒：會動作的週期最短 11 ms，1.5 秒是
  /// 137 筆。寫入方送得比這個密時只留最新的這麼多筆，統計量就用它們算
  inline constexpr std::size_t k_ring_size = 256;

  enum class event_e {
    none,
    kick,  ///< 現在做一次 kick（長度在 decision_t::burst_ns）。做不了就呼叫 kick_not_done()
    result_ok,  ///< 上一次 kick 的結果：回到等遊戲這一格（A）
    result_no_change,  ///< 上一次 kick 的結果：沒有變化（或量不出來）；下一次最早要等 decision_t::next_try_ns
    heavy_stop,  ///< (f) 遊戲的幀佔掉大半個週期：這個 generation 之後不再 kick（跟在那一次的 result_ok 後面）
    limit_stop,  ///< (g) 做滿 k_max_kicks 次：這個 generation 之後不再 kick（跟在最後一次的結果後面）
    /// (h) 只有 presentPhase 像多一格（已經連續 k_dwell_ns）、回聲年齡不成立或沒有：不 kick。只是通知，給 log 用；
    /// 呼叫端什麼都不必做
    withheld,
  };

  inline const char *event_name(event_e e) {
    switch (e) {
      case event_e::none:
        return "none";
      case event_e::kick:
        return "kick";
      case event_e::result_ok:
        return "result_ok";
      case event_e::result_no_change:
        return "result_no_change";
      case event_e::heavy_stop:
        return "heavy_stop";
      case event_e::limit_stop:
        return "limit_stop";
      case event_e::withheld:
        return "withheld";
    }
    return "?";
  }

  /// poll() 的回覆。沒有提到的欄位是 0
  struct decision_t {
    event_e event = event_e::none;
    int64_t burst_ns = 0;  ///< kick：PostPresent 立刻返回多久
    /// kick 當下判定用的那個視窗（kick、兩種 result 都帶；result 帶的是那一次 kick 當下的）。withheld：這一次評估的視窗
    int64_t median_before_ns = 0;
    int64_t p90_before_ns = 0;
    /// 兩種 result：確認用的那 1.5 秒；heavy_stop 只帶 p90_after_ns。samples < k_min_samples 時量不出結果，這兩個數不可信
    int64_t median_after_ns = 0;
    int64_t p90_after_ns = 0;
    int samples = 0;  ///< kick、withheld：判定用的視窗裡有幾筆；兩種 result：確認用的那 1.5 秒裡有幾筆
    /// 回聲年齡的中位數，視窗和上面 presentPhase 的相同：before＝kick 當下判定用的那 1 秒（kick、兩種 result 都帶；
    /// withheld 帶的是這一次評估的視窗），after＝確認用的那 1.5 秒（兩種 result；只給 log 用，成不成功不看它）
    int64_t echo_before_ns = 0;
    int64_t echo_after_ns = 0;
    /// kick、withheld：判定用的那 1 秒裡回聲年齡的第 10 百分位。只給 log 用、不參與判定：真的多一格時每一張都舊一格，
    /// 它和中位數差不多；中位數是被追蹤樣本的到達間隔灌大的時候，它還是只有幾毫秒（檔頭「回聲年齡的限制」）
    int64_t echo_p10_before_ns = 0;
    /// 回聲年齡有幾筆。kick：判定用的視窗裡（一定 ≥ k_echo_min_samples）；withheld：同一個視窗裡（可以是 0，那時
    /// 上面兩個回聲年齡的數也是 0）；兩種 result：確認用的那 1.5 秒裡——< k_echo_min_samples 時 echo_after_ns 不可信
    /// （一筆都沒有時是 0）
    int echo_samples = 0;
    int kick_no = 0;  ///< 這個 generation 的第幾次 kick（所有事件都帶）
    int64_t next_try_ns = 0;  ///< result_no_change：下一次最早多久以後（從現在算）
  };

  /// 一段時間內的樣本的統計量（n == 0 時其餘都是 0）
  struct stats_t {
    int n = 0;
    int64_t median_ns = 0;
    int64_t p90_ns = 0;
    int64_t p10_ns = 0;  ///< 第 10 百分位（只有回聲年齡的 log 用）
  };

  namespace detail {
    inline constexpr int64_t k_i64_max = std::numeric_limits<int64_t>::max();
    inline constexpr int64_t k_i64_min = std::numeric_limits<int64_t>::min();

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

    /// t − d（d ≥ 0），飽和在 INT64_MIN
    constexpr int64_t sub_sat(int64_t t, int64_t d) {
      return t < k_i64_min + d ? k_i64_min : t - d;
    }

    /// floor(v × num ÷ den)，v ≥ 0、0 < num、0 < den。先除再乘：num ≤ den 時 v 再大也不會溢位
    /// （num > den 時呼叫端要先限制 v 的大小）
    constexpr int64_t mul_div(int64_t v, int64_t num, int64_t den) {
      return (v / den) * num + ((v % den) * num) / den;
    }

    /// 排好序的 n 筆（n ≥ 1）裡第 num/den 分位的位置＝round(num/den × (n − 1))，
    /// 和 bridge 10 秒摘要裡 presentPhase 的 p50／p95 同一種取法
    constexpr std::size_t quantile_index(std::size_t n, std::size_t num, std::size_t den) {
      return (num * (n - 1) + den / 2) / den;
    }
  }  // namespace detail

  class detector_t {
  public:
    /**
     * @brief 新的 generation（driver 重連、更新率改變）：全部重來，包含 kick 的次數、退避與「不再 kick」。
     * @param period_ns host 的顯示週期（1e12 ÷ refresh_mhz）；≤ 0 或比 k_min_period_ns 短＝這個 generation 不動作
     * @param now_ns 現在。之後 k_startup_holdoff_ns 內不評估
     */
    void reset(int64_t period_ns, int64_t now_ns) {
      *this = detector_t {};
      period_ns_ = std::max<int64_t>(period_ns, 0);
      active_ = period_ns_ >= k_min_period_ns;
      last_now_ = now_ns;
      holdoff_until_ns_ = detail::add_sat(now_ns, k_startup_holdoff_ns);
    }

    /**
     * @brief 每一幀一筆 presentPhase。
     * @param t_ns 這一筆的時刻（那筆 timing 紀錄自己的時刻，不是呼叫的當下）。不必照順序
     * @param phase_ns PRESENT_ENTER 距離上一次 POSTPRESENT_EXIT 多久；負的丟掉
     */
    void on_present_phase(int64_t t_ns, int64_t phase_ns) {
      if (!active_ || phase_ns < 0) {
        return;
      }
      phase_.push(t_ns, phase_ns);
    }

    /**
     * @brief 每一張對到姿態的擷取影像一筆回聲年齡（(a) 的第二個條件；確認時只給 log 用）。沒有對到姿態的不要餵。
     * @param t_ns 這一張影像 Present 的時刻（和 on_present_phase 的時刻同一個時鐘）。不必照順序
     * @param age_ns 那個時刻減掉「它用的那個姿態被發布給 driver 的時刻」；負的丟掉
     */
    void on_echo_age(int64_t t_ns, int64_t age_ns) {
      if (!active_ || age_ns < 0) {
        return;
      }
      echo_.push(t_ns, age_ns);
    }

    /**
     * @brief 呼叫端自己另外做了一次 kick（dev 的定時 kick）：之後 k_settle_ns 的樣本不看、連續成立的計時重來、
     *        (d) 的 5 秒從這裡起算。不算成功或失敗，退避與 kick 的次數都不動。
     *        正在確認上一次 kick 的結果時，改成從這裡重新等 k_settle_ns 再收 k_verify_ns（量到的是兩次之後的狀態）。
     * @param burst_ns 那一次的長度；比 k_burst_max_ns 長的部分加在不看樣本的時間上
     */
    void on_external_kick(int64_t now_ns, int64_t burst_ns) {
      const int64_t now = tick(now_ns);
      declinable_ = false;
      if (!active_) {
        return;
      }
      // 不看樣本的時間是照「kick 最長 k_burst_max_ns」定的；外面的 kick 更長時往後延（上限 10 秒，只為了不溢位）
      const int64_t extra = std::clamp<int64_t>(detail::sub_sat(burst_ns, k_burst_max_ns), 0, 10 * k_ns_per_s);
      const int64_t until = detail::add_sat(now, k_settle_ns + extra);
      last_kick_ns_ = now;
      dwell_active_ = false;
      phase_dwell_active_ = false;
      // until 之前的樣本都不拿來判定：視窗要重新收滿 k_min_samples 筆，B 才可能再成立
      floor_ns_ = std::max(floor_ns_, until);
      if (verifying_) {
        verify_begin_ns_ = std::max(verify_begin_ns_, until);
        verify_end_ns_ = detail::add_sat(verify_begin_ns_, k_verify_ns);
      }
    }

    /**
     * @brief 定期呼叫（bridge 的 periodic()，每 2 ms）。一次最多回一個事件，大多數時候是 none。
     *        回 kick 之後，呼叫端要嘛真的做（之後某一次 poll() 會回它的結果），要嘛馬上呼叫 kick_not_done()。
     *        其他事件都只是通知（給 log 用），呼叫端不必回應。
     */
    decision_t poll(int64_t now_ns) {
      const int64_t now = tick(now_ns);
      declinable_ = false;
      decision_t d;
      if (!active_) {
        return d;
      }
      d.kick_no = kicks_;

      // 「不再 kick」的通知排在造成它的那個結果後面，一次 poll() 一個
      if (pending_ != pending_e::none) {
        d.event = pending_ == pending_e::heavy ? event_e::heavy_stop : event_e::limit_stop;
        d.p90_after_ns = pending_ == pending_e::heavy ? heavy_p90_ns_ : 0;
        pending_ = pending_e::none;
        return d;
      }

      if (verifying_) {
        if (now < verify_end_ns_) {
          return d;
        }
        // (c) 確認：[kick + settle, kick + settle + verify) 這一段的樣本。之後回到 (a)：連續成立的計時在 kick 時
        // 已經歸零，所以又是 B 的話要再滿 3 秒
        const stats_t s = phase_.between(verify_begin_ns_, detail::sub_sat(verify_end_ns_, 1));
        const stats_t e = echo_.between(verify_begin_ns_, detail::sub_sat(verify_end_ns_, 1));
        verifying_ = false;
        d.samples = s.n;
        d.median_before_ns = kick_median_ns_;
        d.p90_before_ns = kick_p90_ns_;
        d.median_after_ns = s.median_ns;
        d.p90_after_ns = s.p90_ns;
        // 回聲年齡只帶出去給 log：成不成功下面只看 presentPhase
        d.echo_before_ns = kick_echo_ns_;
        d.echo_after_ns = e.median_ns;
        d.echo_samples = e.n;
        if (s.n >= k_min_samples && s.median_ns >= k_a_median_min_ns) {
          d.event = event_e::result_ok;
          backoff_step_ = 0;  // 退避的時刻不必清：能走到 kick 就表示它已經過了
          // (f) 重負載
          if (s.p90_ns > detail::mul_div(period_ns_, k_heavy_period_num, k_heavy_period_den)) {
            stopped_ = true;
            pending_ = pending_e::heavy;
            heavy_p90_ns_ = s.p90_ns;
          }
        } else {
          d.event = event_e::result_no_change;
          const int64_t wait = k_backoff_ns[std::min(backoff_step_, k_backoff_ns.size() - 1)];
          if (backoff_step_ < k_backoff_ns.size()) {
            ++backoff_step_;
          }
          not_before_ns_ = detail::add_sat(now, wait);
          d.next_try_ns = wait;
        }
        // (g) 次數用完
        if (!stopped_ && kicks_ >= k_max_kicks) {
          stopped_ = true;
          pending_ = pending_e::limit;
        }
        return d;
      }

      if (stopped_ || now < holdoff_until_ns_ || now < next_eval_ns_) {
        return d;
      }

      // (a) 評估。兩次評估隔了超過一個視窗（bridge 執行緒被卡住）：中間有樣本沒被看到，不算連續
      // （計時在跑的時候 last_eval_ns_ 一定是上一次評估的時刻；沒在跑的時候這一行沒有作用）
      if (detail::elapsed(now, last_eval_ns_) > k_window_ns) {
        dwell_active_ = false;
        phase_dwell_active_ = false;
      }
      last_eval_ns_ = now;
      next_eval_ns_ = detail::add_sat(now, k_eval_interval_ns);
      const stats_t s = recent(now);
      if (s.n < k_min_samples || s.median_ns >= k_b_median_max_ns || s.p90_ns >= k_b_p90_max_ns) {
        dwell_active_ = false;
        phase_dwell_active_ = false;
        return d;
      }
      // presentPhase 這一邊成立。它自己連續成立了多久另外記著，(h) 要用
      if (!phase_dwell_active_) {
        phase_dwell_active_ = true;
        phase_dwell_since_ns_ = now;
      }
      // 回聲年齡也要多了半個週期以上。合成器自己的畫面（遊戲還在載入）presentPhase 和 B 一樣小，只有這個量分得開；
      // 樣本不夠（沒有餵、沒有串流在擷取、影像沒有對到姿態）一律當成不成立
      const stats_t e = recent_echo(now);
      if (e.n < k_echo_min_samples || e.median_ns < echo_min_ns()) {
        dwell_active_ = false;
        // (h) 只看 presentPhase 的話這時已經連續成立滿 k_dwell_ns、該 kick 了：不 kick，通知一次（限速）
        if (detail::elapsed(now, phase_dwell_since_ns_) >= k_dwell_ns && detail::elapsed(now, last_withheld_ns_) >= k_withheld_interval_ns) {
          last_withheld_ns_ = now;
          d.event = event_e::withheld;
          d.median_before_ns = s.median_ns;
          d.p90_before_ns = s.p90_ns;
          d.samples = s.n;
          d.echo_before_ns = e.median_ns;
          d.echo_p10_before_ns = e.p10_ns;
          d.echo_samples = e.n;
        }
        return d;
      }
      if (!dwell_active_) {
        dwell_active_ = true;
        dwell_since_ns_ = now;
      }
      if (detail::elapsed(now, dwell_since_ns_) < k_dwell_ns) {
        return d;
      }
      // 該 kick 了，除非還在退避、或離上一次太近（B 繼續成立的話計時留著，時間到的那一次評估就做）。
      // 次數用完的不會走到這裡：最後一次的結果出來時 stopped_ 就設了
      if (now < not_before_ns_ || detail::elapsed(now, last_kick_ns_) < k_min_gap_ns) {
        return d;
      }

      prev_last_kick_ns_ = last_kick_ns_;  // 「沒做成」時要還原
      declinable_ = true;

      ++kicks_;
      last_kick_ns_ = now;
      kick_median_ns_ = s.median_ns;
      kick_p90_ns_ = s.p90_ns;
      kick_echo_ns_ = e.median_ns;
      // (c) 結果出來之前不評估 B：前 k_settle_ns 的樣本不看，接著 k_verify_ns 的拿來確認
      verifying_ = true;
      verify_begin_ns_ = detail::add_sat(now, k_settle_ns);
      verify_end_ns_ = detail::add_sat(verify_begin_ns_, k_verify_ns);
      dwell_active_ = false;
      phase_dwell_active_ = false;

      d.event = event_e::kick;
      d.burst_ns = burst_ns();
      d.median_before_ns = s.median_ns;
      d.p90_before_ns = s.p90_ns;
      d.samples = s.n;
      d.echo_before_ns = e.median_ns;
      d.echo_p10_before_ns = e.p10_ns;
      d.echo_samples = e.n;
      d.kick_no = kicks_;
      return d;
    }

    /**
     * @brief poll() 剛回的那個 kick 沒有做（另一段節拍調整正在進行、還沒有正式節拍可以寫回……）：當成沒發生過——
     *        不算次數、不影響退避與 (d) 的間隔、不會有結果、樣本照常看。連續成立的計時在回 kick 的時候就歸零了，
     *        所以 B 還在的話要再滿 3 秒才會再回一次 kick。
     *        只有緊接在回 kick 的那一次 poll() 之後呼叫才有作用。
     */
    void kick_not_done() {
      if (!declinable_) {
        return;
      }
      declinable_ = false;
      verifying_ = false;
      --kicks_;
      last_kick_ns_ = prev_last_kick_ns_;
    }

    // ── 觀察點（測試、log、之後的整合用；都不改狀態）──────────────────────────

    /// 這個 generation 會不會動作（週期 ≥ k_min_period_ns）
    bool active() const {
      return active_;
    }

    int64_t period_ns() const {
      return period_ns_;
    }

    /// (b) 一次 kick 的長度
    int64_t burst_ns() const {
      // 週期比上限還長時乘出來一定超過上限，直接給上限（也就不必擔心乘法溢位）
      const int64_t want = period_ns_ >= k_burst_max_ns ? k_burst_max_ns : detail::mul_div(period_ns_, k_burst_period_num, k_burst_period_den);
      return std::clamp(want, k_burst_min_ns, k_burst_max_ns);
    }

    /// 這個 generation 做了幾次 kick（呼叫端回報沒做成的不算；on_external_kick 的也不算）
    int kicks() const {
      return kicks_;
    }

    /// 連續幾次「沒有變化」（0＝沒有退避；封頂在退避表的長度）
    int backoff_step() const {
      return static_cast<int>(backoff_step_);
    }

    /// kick 做了、結果還沒出來（這段時間的樣本與平常不同，不評估 B）
    bool verifying() const {
      return verifying_;
    }

    /// 這個 generation 不再 kick 了（(f) 重負載，或 (g) 次數用完）
    bool stopped() const {
      return stopped_;
    }

    /// (a) 的判定視窗裡的 presentPhase：最近 k_window_ns 內的樣本（on_external_kick 之後那一段不看的除外）
    stats_t recent(int64_t now_ns) const {
      return phase_.between(window_begin(now_ns), now_ns);
    }

    /// 同一個視窗裡的回聲年齡（判定只看筆數與中位數；p10 給 log 用，p90 沒有人用、只是順便算出來）
    stats_t recent_echo(int64_t now_ns) const {
      return echo_.between(window_begin(now_ns), now_ns);
    }

    /// (a) 回聲年齡的中位數要 ≥ 這個值才算多一格（60 Hz 是 13.83 ms）
    int64_t echo_min_ns() const {
      return detail::add_sat(k_echo_a_typical_ns, detail::mul_div(period_ns_, k_echo_period_num, k_echo_period_den));
    }

    /// B 已經連續成立多久（目前不成立＝0；以上一次評估為準）
    int64_t dwell_ns(int64_t now_ns) const {
      return dwell_active_ ? detail::elapsed(now_ns, dwell_since_ns_) : 0;
    }

    /// (h) 只算 presentPhase 那一邊：已經連續成立多久（目前不成立＝0；以上一次評估為準）。一定 ≥ dwell_ns()
    int64_t phase_dwell_ns(int64_t now_ns) const {
      return phase_dwell_active_ ? detail::elapsed(now_ns, phase_dwell_since_ns_) : 0;
    }

  private:
    struct sample_t {
      int64_t t_ns = 0;
      int64_t value_ns = 0;
    };

    /// 樣本環：滿了之前有效的是 [0, count)，滿了之後是全部（只留最新的 k_ring_size 筆）
    struct ring_t {
      std::array<sample_t, k_ring_size> buf {};
      std::size_t head = 0;
      std::size_t count = 0;

      void push(int64_t t_ns, int64_t value_ns) {
        buf[head] = sample_t {t_ns, value_ns};
        head = (head + 1) % k_ring_size;
        count = std::min(count + 1, k_ring_size);
      }

      /// 時刻在 [lo_ns, hi_ns]（兩端都含）的樣本的統計量。環裡的順序不重要：每一筆都看自己的時刻
      stats_t between(int64_t lo_ns, int64_t hi_ns) const {
        std::array<int64_t, k_ring_size> v {};
        std::size_t n = 0;
        for (std::size_t i = 0; i < count; ++i) {
          if (buf[i].t_ns >= lo_ns && buf[i].t_ns <= hi_ns) {
            v[n++] = buf[i].value_ns;
          }
        }
        stats_t s;
        s.n = static_cast<int>(n);
        if (n == 0) {
          return s;
        }
        std::sort(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
        s.median_ns = v[detail::quantile_index(n, 1, 2)];
        s.p90_ns = v[detail::quantile_index(n, 9, 10)];
        s.p10_ns = v[detail::quantile_index(n, 1, 10)];
        return s;
      }
    };

    enum class pending_e {
      none,
      heavy,
      limit,
    };

    /// 時間只往前：比上一次早的時刻當成和上一次相同
    int64_t tick(int64_t now_ns) {
      last_now_ = std::max(last_now_, now_ns);
      return last_now_;
    }

    /// (a) 的判定視窗的起點（含）：1 秒前再加 1 ns，或外面做了 kick 之後不看的那一段的結尾，取晚的那個
    int64_t window_begin(int64_t now_ns) const {
      return std::max(detail::add_sat(detail::sub_sat(now_ns, k_window_ns), 1), floor_ns_);
    }

    // 設定
    int64_t period_ns_ = 0;
    bool active_ = false;

    // 時鐘（只往前）
    int64_t last_now_ = detail::k_i64_min;

    // 樣本
    ring_t phase_;  ///< presentPhase（on_present_phase）
    ring_t echo_;  ///< 回聲年齡（on_echo_age）
    int64_t floor_ns_ = detail::k_i64_min;  ///< 比這個時刻早的樣本不拿來判定 B（外面做了 kick 之後的那一段）

    // (a) 判定
    int64_t holdoff_until_ns_ = detail::k_i64_min;  ///< (g) 這個時刻之前不評估（剛 reset）
    int64_t last_eval_ns_ = 0;
    int64_t next_eval_ns_ = detail::k_i64_min;
    bool dwell_active_ = false;
    int64_t dwell_since_ns_ = 0;  ///< 這一段連續成立的第一次評估的時刻

    // (h) 只有 presentPhase 那一邊成立的通知
    bool phase_dwell_active_ = false;  ///< presentPhase 那一邊正在連續成立（回聲年齡成不成立都算）
    int64_t phase_dwell_since_ns_ = 0;  ///< 那一段的第一次評估的時刻
    int64_t last_withheld_ns_ = detail::k_i64_min;  ///< 上一次回 withheld 的時刻（還沒有過＝INT64_MIN，離現在無限久）

    // (c) kick 與確認
    bool verifying_ = false;
    int64_t verify_begin_ns_ = 0;
    int64_t verify_end_ns_ = 0;
    int64_t kick_median_ns_ = 0;  ///< kick 當下的視窗（結果的 log 要印「之前」）
    int64_t kick_p90_ns_ = 0;
    int64_t kick_echo_ns_ = 0;  ///< kick 當下的回聲年齡中位數（同上）
    int64_t last_kick_ns_ = detail::k_i64_min;  ///< 上一次 kick 的時刻（含外面做的；還沒有過＝INT64_MIN，離現在無限久）
    int kicks_ = 0;
    std::size_t backoff_step_ = 0;
    int64_t not_before_ns_ = detail::k_i64_min;  ///< 退避：這個時刻之前不 kick

    // (f)(g) 不再 kick
    bool stopped_ = false;
    pending_e pending_ = pending_e::none;
    int64_t heavy_p90_ns_ = 0;

    // kick_not_done()
    bool declinable_ = false;  ///< 上一次 poll() 回的是 kick，之後還沒有別的呼叫
    int64_t prev_last_kick_ns_ = detail::k_i64_min;  ///< 那個 kick 之前的 last_kick_ns_
  };

}  // namespace vr::kick
