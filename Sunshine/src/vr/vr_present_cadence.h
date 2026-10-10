/**
 * @file src/vr/vr_present_cadence.h
 * @brief VipleStream 2.0 §VR-CADENCE（2026-10-10）：從 driver 的 timing ring 判斷「合成器的 Present 鎖不鎖得住虛擬 vsync」。
 *
 * 純邏輯：只用標準函式庫、不碰任何平台標頭、不讀時鐘（時間一律由呼叫端以奈秒整數傳入）、不配置、不上鎖、沒有浮點數、
 * 沒有除以輸入值的除法，可以脫離整個 server 單獨編譯測試（tests/unit/test_vr_present_cadence.cpp）。只給一條執行緒用
 * （bridge 執行緒）；要給別的執行緒看的只有 info_t，打包成一個 64-bit 值經 atomic 交換。
 * 刻意不 include vr_ipc_abi.h（那個檔在非 x64 會 #error）：用到的事件代碼在這裡另外定義，vr_bridge_win.cpp 以
 * static_assert 對住。成員與常數不可用 Windows 標頭的巨集名（near、far、small、interface、min、max、ERROR……）——
 * 這個標頭會被先 include <Windows.h> 的檔案 include，所以「落在格子尾端」的診斷欄位叫 edge。
 *
 * 背景：§VR-FENCE-LATE 的過載旗標（vr_capture_health.h 的 (d)）看的是「fence 晚到的比例」。遊戲仍然跑滿更新率、只是
 * 每一張的 fence 都固定晚幾毫秒時（2026-10-10：解析度倍率 300%），旗標整場設著，LATCH 的頻率鎖整場不追蹤——可是那種
 * 影像到達的相位是穩定的。真正不能追的是「Present 跟不上虛擬 vsync」：影像相對格點一直滑動。
 *
 * 訊號：driver 每次 PostPresent 寫一筆 VRIPC_TE_VSYNC_VIRTUAL，arg1＝這次跨了幾個虛擬 vsync 格。**準時是 1**
 * （vr_ipc_abi.h 那一行的註解寫的是「跳過的 vsync 數」，實際的值是跨的格數；那個檔和 driver 共用，等下次動 driver 再改）。
 * 相對目前的節拍多跨一格＝滑動（slip）；多跨兩格以上、而且至少多 16 ms＝卡頓（hitch：整個停住，不是跟不上）。
 * 所有時間視窗用的都是 bridge 排空 ring 當下自己的時鐘，紀錄裡的時戳只拿來算一個診斷欄位（edge）。
 *
 * 對外四種狀態，只在有把握時下結論：
 *   slipping（確定鎖不住）：10 s 內有 4 個不屬於卡頓的滑動，之後 12 s 沒有才清；或 3 s 內的滑動達格數的 1/15
 *                         （120 Hz 是 24 個），之後 3 s 沒有才清
 *   locked  （確定鎖得住）：以上都不成立、已經連續觀察滿 10 s、而且沒有下面任何一種「說不準」
 *   unsure  （說不準）    ：最近 12 s 內有「卡頓後 3 s 內的滑動」（可能是卡頓的餘波，也可能是被卡頓蓋住的持續滑動）；
 *                         穩定在更新率的整數分之一、但 driver 回報的節流值解釋不了；紀錄裡有不可能的值；觀察還不滿 10 s
 *   unknown （沒有紀錄）  ：driver 沒在寫、紀錄停了、剛重來
 * 只有 locked 與 slipping 算「有把握」；unknown／unsure 時呼叫端（vr_capture_health.h）照原本的 late 旗標決定，
 * 所以任何說不準的情形都和沒有這個偵測器時相同。
 *
 * 寫入方（driver）不可信：slots、exit_code、throttle、frame_id、時戳都可能是錯的。這裡不會因為它們而除以零、溢位或
 * 跑不完；不可能的值（slots ≤ 0、一次跨超過 2 s）10 s 內出現 3 筆就整個當成說不準（unsure／suspect）。
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vr::cadence {

  constexpr int64_t k_ns_per_ms = 1'000'000;
  constexpr int64_t k_ns_per_s = 1'000'000'000;

  // 標稱週期只用 server 自己知道的值（session config 的 refresh_mhz），不用紀錄裡的週期
  constexpr int64_t k_period_default_ns = 11'111'111;  ///< 標稱週期不合理時的預設（90 Hz；同 vr_capture_health.h）
  constexpr int64_t k_period_min_ns = 2 * k_ns_per_ms;  ///< 標稱週期只接受 2～100 ms
  constexpr int64_t k_period_max_ns = 100 * k_ns_per_ms;

  // timing ring 的事件代碼與值（和 vr_ipc_abi.h 的 VRIPC_TE_* 是同一組數字；vr_bridge_win.cpp 有 static_assert 對住）
  constexpr uint32_t k_ev_vsync_virtual = 1;
  constexpr uint32_t k_ev_present_enter = 3;
  constexpr uint32_t k_ev_present_exit = 4;
  constexpr uint32_t k_ev_postpresent_enter = 5;
  constexpr uint32_t k_ev_postpresent_exit = 6;
  constexpr int64_t k_exit_no_target = 4;  ///< PRESENT_EXIT.arg0：沒有輸出目標（idle、standby、TEARDOWN）

  // 事件的分類。卡頓＝一次 PostPresent 比節拍多跨至少 2 格、而且至少多 16 ms（60～120 Hz 是 2 格，144 Hz 是 3 格）；
  // 多一格（或少一格）＝滑動。貼著更新率的持續過載每次只少一格（實測最大的 Present 間隔 12.5～17.1 ms，多跨 2 格要
  // 約 21 ms 以上）；更長的間隔都是停頓。用時間而不是固定格數：固定「多跨 4 格」在 60 Hz 會變成 67 ms
  constexpr int64_t k_hitch_extra_ns = 16 * k_ns_per_ms;
  constexpr int64_t k_hitch_slots_min = 2;
  /// 遮罩：卡頓之後這麼久以內的滑動可能只是它的餘波（實測餘波最晚到 2.2 s）。它們記成 masked：不算進臨界的 4 個、
  /// 不擋清除，但會讓狀態變成 unsure
  constexpr int64_t k_mask_ns = 3 * k_ns_per_s;
  /// 沒有節拍可回：連續這麼多張都不在節拍上之後，長間隔也算滑動（遠低於更新率時每一張都偏離；餘波裡連續偏離只有 1～4 張）
  constexpr uint32_t k_no_cadence_run = 12;

  // 臨界負載（持續、稀疏的滑動）：最近 10 s 有 4 個不屬於卡頓的滑動就設；12 s 沒有才清。
  // 鎖住時 10 s 最多 3 個（配戴實測）；340% 兩簇之間最久 8.3 s，所以清除取 12 s
  constexpr std::size_t k_marginal_events = 4;
  constexpr int64_t k_marginal_window_ns = 10 * k_ns_per_s;
  constexpr int64_t k_marginal_clear_ns = 12 * k_ns_per_s;

  // 重度（密集的滑動）：最近 3 s 的滑動（含 masked）達「3 s 的格數 ÷ 15」（夾在 8～32：120 Hz 24、90 Hz 18、60 Hz 12）
  // 就設；不再成立之後，3 s 沒有不屬於卡頓的滑動就清（和 late 旗標清掉的時間同級）
  constexpr int64_t k_heavy_window_ns = 3 * k_ns_per_s;
  constexpr int64_t k_heavy_slots_div = 15;
  constexpr std::size_t k_heavy_events_min = 8;
  constexpr std::size_t k_heavy_events_max = 32;
  constexpr int64_t k_recover_quiet_ns = 3 * k_ns_per_s;

  // 說不準
  /// 最後一個 masked 滑動之後這麼久以內狀態是 unsure。和臨界的清除時間同值：被卡頓蓋住的持續滑動，間隔和沒被蓋住時一樣
  constexpr int64_t k_masked_keep_ns = 12 * k_ns_per_s;
  /// 10 s 內這麼多筆不可能的值（slots ≤ 0、一次跨超過 2 s）就不信這些紀錄。合法的只有 Activate 後第一筆與停頓超過 2 s 之後的一筆
  constexpr std::size_t k_suspect_events = 3;
  constexpr int64_t k_suspect_window_ns = 10 * k_ns_per_s;
  /// 連續觀察滿這麼久才可以報 locked（等於臨界的視窗：「10 s 內不到 4 個」要看滿 10 s 才說得出口）
  constexpr int64_t k_prove_ns = 10 * k_ns_per_s;

  // 整數分之一：連續這麼多張都跨 1 + nFramesToThrottle 格＝SteamVR 把遊戲固定在更新率的整數分之一，當成新的節拍
  constexpr uint32_t k_divisor_run = 24;
  constexpr int64_t k_throttle_max = 4;  ///< driver 把 nFramesToThrottle 夾在這個值以內
  constexpr int64_t k_divisor_max = 1 + k_throttle_max;

  // 有效性
  constexpr int64_t k_stale_ns = 500 * k_ns_per_ms;  ///< 這麼久沒有紀錄＝unknown（旗標先留著；實測最長的卡頓 187～216 ms）
  constexpr int64_t k_forget_ns = 2 * k_ns_per_s;  ///< 這麼久沒有紀錄：全部重來
  constexpr uint32_t k_no_target_run = 12;  ///< 連續這麼多筆沒有輸出目標：全部重來
  constexpr uint32_t k_known_run = 24;  ///< 重來之後要有這麼多筆可用的紀錄才有任何輸出
  constexpr int64_t k_phase_max_ns = 100 * k_ns_per_ms;  ///< Present 相位達到這個值的樣本不用

  // 診斷：Present 落在格內最後 14% 的張數（鎖住時 presentPhase p99 ≤ 6.83 ms，120 Hz 的界線是 7.17 ms）。只印不判
  constexpr int64_t k_edge_num = 86;
  constexpr int64_t k_edge_den = 100;

  enum class state_e : uint8_t {
    unknown,  ///< 沒有紀錄、紀錄停了、剛重來
    locked,  ///< 每 divisor 格一次 Present，而且已經連續觀察滿 k_prove_ns
    slipping,  ///< 鎖不住
    unsure,  ///< 有紀錄，但兩邊都證明不了（原因見 why_e）
  };

  enum class why_e : uint8_t {
    none,
    marginal,  ///< slipping：持續、稀疏
    heavy,  ///< slipping：密集
    recovering,  ///< slipping：重度剛結束
    recent,  ///< unsure：最近 12 s 內有卡頓後 3 s 內的滑動
    fraction,  ///< unsure：穩定在整數分之一，但 driver 回報的節流值解釋不了
    suspect,  ///< unsure：紀錄裡有不可能的值
    proving,  ///< unsure：觀察得還不夠久
  };

  /// 一次 PostPresent（一筆 VRIPC_TE_VSYNC_VIRTUAL，加上同一幀前幾筆紀錄帶的值）
  struct present_t {
    int64_t now_ns = 0;  ///< 排空 ring 當下呼叫端的單調時鐘（紀錄裡的時戳不拿來判定）
    int64_t slots = 0;  ///< VSYNC_VIRTUAL.arg1 原值（準時＝1）
    int64_t exit_code = 0;  ///< 同一幀 PRESENT_EXIT.arg0 原值（4＝沒有輸出目標）
    bool have_phase = false;
    int64_t phase_ns = 0;  ///< PRESENT_ENTER − 上一筆 POSTPRESENT_EXIT（只給診斷欄位 edge）
    int64_t throttle = 0;  ///< 同一幀 POSTPRESENT_ENTER.arg0 原值（nFramesToThrottle）
  };

  /// 給其他執行緒看的摘要（打包成 64 bit）。預設值＝不知道
  struct info_t {
    state_e state = state_e::unknown;
    why_e why = why_e::none;
    uint8_t divisor = 0;  ///< 每幾格一次 Present（unknown 時是 0）
    uint8_t slips_10s = 0;  ///< 最近 10 s 的滑動數，含 masked（封頂 32）
    uint32_t stamp_ms = 0;  ///< 這份摘要是什麼時候做的（ms，只留低 32 bit）

    bool operator==(const info_t &) const = default;
  };

  /// bit 0-1 state、2-4 why、5-7 divisor、8-13 slips_10s、32-63 stamp_ms；預設值打包成 0
  constexpr uint64_t pack(const info_t &i) {
    return static_cast<uint64_t>(static_cast<uint8_t>(i.state) & 3u) | (static_cast<uint64_t>(static_cast<uint8_t>(i.why) & 7u) << 2) |
           (static_cast<uint64_t>(i.divisor & 7u) << 5) | (static_cast<uint64_t>(i.slips_10s & 0x3Fu) << 8) | (static_cast<uint64_t>(i.stamp_ms) << 32);
  }

  constexpr info_t unpack(uint64_t v) {
    info_t i;
    i.state = static_cast<state_e>(v & 3u);
    i.why = static_cast<why_e>((v >> 2) & 7u);
    i.divisor = static_cast<uint8_t>((v >> 5) & 7u);
    i.slips_10s = static_cast<uint8_t>((v >> 8) & 0x3Fu);
    i.stamp_ms = static_cast<uint32_t>(v >> 32);
    return i;
  }

  /// 摘要夠不夠新（給讀的人用：寫的人卡住或不在了的時候，舊的值不能一直用下去）。unknown 一律不算新。
  /// 讀的人可能比寫的人蓋時戳的那一刻稍早讀了自己的時鐘：「來自未來」100 ms 以內也算新。
  /// age 以 2^32 ms 取模，兩邊的時戳都只留低 32 bit 也沒關係
  constexpr bool fresh(const info_t &i, uint32_t now_ms, uint32_t max_age_ms = 1000) {
    if (i.state == state_e::unknown) {
      return false;
    }
    const uint32_t age = now_ms - i.stamp_ms;
    return age <= max_age_ms || age >= 0xFFFFFFFFu - 99u;
  }

  /// 10 s log 行的計數（take_window() 取走後歸零；每一欄都會飽和，不會繞回）
  struct window_t {
    uint32_t records = 0;  ///< 配得到同一幀其他紀錄的 VSYNC_VIRTUAL（含沒有輸出目標的）
    uint32_t ok = 0;  ///< 在節拍上的
    uint32_t slips = 0;  ///< 不屬於卡頓的滑動
    uint32_t masked = 0;  ///< 卡頓後 k_mask_ns 內的滑動
    uint32_t hitches = 0;  ///< 卡頓
    uint32_t skipped = 0;  ///< 所有可用紀錄多跨的格數合計（divisor＝1 時就是 driver 自己那一行的 missed）
    uint32_t early = 0;  ///< 比節拍少跨（divisor ≥ 2 才有）
    uint32_t edge = 0;  ///< 在節拍上、但 Present 落在格內最後 14% 的
    uint32_t no_target = 0;  ///< 沒有輸出目標的
    uint32_t invalid = 0;  ///< slots 是負值
    uint32_t zeros = 0;  ///< 有輸出目標而 slots＝0
    uint32_t restarts = 0;  ///< 一次跨超過 2 s（driver 重新起算了格點）
    uint32_t orphans = 0;  ///< 配不到同一幀的 PRESENT_EXIT／POSTPRESENT_ENTER 的 VSYNC_VIRTUAL（不用）
    uint32_t divisor_changes = 0;  ///< 節拍換了幾次
    uint32_t slip_secs = 0;  ///< 有 slips 的整秒數（對齊整秒）
    uint32_t burst_max = 0;  ///< 單一整秒內最多幾個 slips
    int64_t slip_gap_max_ns = 0;  ///< 這個視窗內相鄰兩個 slips 最久隔多久
    int64_t throttle_max = 0;  ///< 看到的最大節流值（夾在 0～255）
  };

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

    /// floor(t ÷ unit)，unit > 0；負的 t 也往下取
    constexpr int64_t floor_div(int64_t t, int64_t unit) {
      const int64_t q = t / unit;
      return t % unit < 0 ? q - 1 : q;
    }

    /// v += n，飽和在 UINT32_MAX
    constexpr void bump(uint32_t &v, uint32_t n = 1) {
      v = v > std::numeric_limits<uint32_t>::max() - n ? std::numeric_limits<uint32_t>::max() : v + n;
    }

    /// 固定大小的時刻環：只留最新的 N 筆
    template<std::size_t N>
    class stamps_t {
    public:
      void clear() {
        n_ = 0;
        head_ = 0;
      }

      /// 同一個時刻記 count 筆（count 封頂在容量）
      void push(int64_t t, std::size_t count = 1) {
        count = std::min(count, N);
        for (std::size_t i = 0; i < count; ++i) {
          v_[head_] = t;
          head_ = (head_ + 1) % N;
          n_ = std::min(n_ + 1, N);
        }
      }

      /// 第 k 新的那一筆（k 從 1 起）距 now 不超過 window
      bool has_within(std::size_t k, int64_t now, int64_t window) const {
        if (k == 0 || k > n_) {
          return false;
        }
        return elapsed(now, v_[(head_ + N - k) % N]) <= window;
      }

      /// 距 now 不超過 window 的有幾筆（最多 N）
      std::size_t count_within(int64_t now, int64_t window) const {
        std::size_t c = 0;
        for (std::size_t k = 1; k <= n_; ++k) {
          if (elapsed(now, v_[(head_ + N - k) % N]) > window) {
            break;
          }
          ++c;
        }
        return c;
      }

    private:
      std::array<int64_t, N> v_ {};
      std::size_t n_ = 0;
      std::size_t head_ = 0;
    };
  }  // namespace detail

  class detector_t {
  public:
    /**
     * @brief 新的連線：全部重來。
     * @param period_ns server 自己知道的標稱虛擬 vsync 週期（不是紀錄裡的值）；不在 2～100 ms 內就用 90 Hz
     * @param now_ns 呼叫端的單調時鐘
     * @param ticks_per_s on_record() 收到的紀錄時戳的單位（QPC 頻率）；0 或不合理＝不算 Present 相位（edge 恆為 0）
     */
    void reset(int64_t period_ns, int64_t now_ns, int64_t ticks_per_s = 0) {
      *this = detector_t {};
      period_ns_ = period_ns >= k_period_min_ns && period_ns <= k_period_max_ns ? period_ns : k_period_default_ns;
      // 3 s 的格數 ÷ 15（120 Hz 24、90 Hz 18、60 Hz 12），夾在 8～32
      heavy_events_ = static_cast<std::size_t>(std::clamp<int64_t>(k_heavy_window_ns / period_ns_ / k_heavy_slots_div, static_cast<int64_t>(k_heavy_events_min), static_cast<int64_t>(k_heavy_events_max)));
      restart_slots_ = 2 * k_ns_per_s / period_ns_ + 1;  // driver 停頓超過 2 s 就重新起算格點
      // 卡頓＝比節拍多跨至少 2 格、而且至少多 16 ms
      hitch_slots_ = std::max<int64_t>(k_hitch_slots_min, (k_hitch_extra_ns + period_ns_ - 1) / period_ns_);
      edge_ns_ = period_ns_ / k_edge_den * k_edge_num;
      ticks_per_s_ = ticks_per_s >= 1000 && ticks_per_s <= 4'000'000'000LL ? ticks_per_s : 0;
      tick(now_ns);
    }

    /**
     * @brief 一筆 timing ring 的紀錄。把同一幀的紀錄配起來（PRESENT_EXIT、POSTPRESENT_ENTER、VSYNC_VIRTUAL 帶同一個
     *        frame_id），每筆 VSYNC_VIRTUAL 呼叫一次 on_present()。配不到同伴的 VSYNC_VIRTUAL（ring 滿了、bridge 卡住而
     *        掉紀錄）只計數（orphans）、不用——只會少算事件。不認得的 event 忽略。
     * @param now_ns 排空 ring 當下呼叫端的單調時鐘
     * @param rec_ticks 紀錄的時戳（driver 的時鐘，不可信；只拿來算 Present 相位這個診斷量）
     */
    void on_record(int64_t now_ns, uint32_t event, uint32_t frame_id, int64_t arg0, int64_t arg1, int64_t rec_ticks) {
      switch (event) {
        case k_ev_present_enter:
          if (stage_.have_ppexit && ticks_per_s_ > 0 && rec_ticks >= stage_.ppexit_ticks) {
            const uint64_t d = static_cast<uint64_t>(rec_ticks) - static_cast<uint64_t>(stage_.ppexit_ticks);
            // 只收不到 100 ms 的（d × 1e9 不會溢位：d < ticks_per_s ÷ 10 ≤ 4e8）
            if (d < static_cast<uint64_t>(ticks_per_s_) / 10u) {
              stage_.have_phase = true;
              stage_.phase_ns = static_cast<int64_t>(d * 1'000'000'000ull / static_cast<uint64_t>(ticks_per_s_));
            }
          }
          stage_.have_ppexit = false;  // 一個格點只配一筆 Present
          break;
        case k_ev_present_exit:
          stage_.have_exit = true;
          stage_.exit_code = arg0;
          stage_.exit_frame = frame_id;
          break;
        case k_ev_postpresent_enter:
          stage_.have_throttle = true;
          stage_.throttle = arg0;
          stage_.throttle_frame = frame_id;
          break;
        case k_ev_vsync_virtual:
          {
            const bool paired = stage_.have_exit && stage_.have_throttle && stage_.exit_frame == frame_id && stage_.throttle_frame == frame_id;
            present_t p;
            p.now_ns = now_ns;
            p.slots = arg1;
            p.exit_code = stage_.exit_code;
            p.have_phase = stage_.have_phase;
            p.phase_ns = stage_.phase_ns;
            p.throttle = stage_.throttle;
            // 用不用都清：暫存的值只屬於這一幀
            stage_.have_exit = false;
            stage_.have_throttle = false;
            stage_.have_phase = false;
            if (!paired) {
              tick(now_ns);
              detail::bump(win_.orphans);
              break;
            }
            on_present(p);
            break;
          }
        case k_ev_postpresent_exit:
          stage_.have_ppexit = true;
          stage_.ppexit_ticks = rec_ticks;
          break;
        default:
          break;
      }
    }

    /// 一次 PostPresent（on_record() 配好之後呼叫；單元測試可以直接餵）
    void on_present(const present_t &p) {
      const int64_t now = tick(p.now_ns);
      if (have_rec_ && detail::elapsed(now, last_rec_ns_) >= k_forget_ns) {
        forget();  // 紀錄停了很久（呼叫端沒有照節拍呼叫 on_tick 的時候也要成立）
      }
      have_rec_ = true;
      last_rec_ns_ = now;
      stale_ = false;
      detail::bump(win_.records);
      win_.throttle_max = std::max(win_.throttle_max, std::clamp<int64_t>(p.throttle, 0, 255));

      if (p.exit_code == k_exit_no_target) {
        // 沒有輸出目標：這一筆不代表串流的節拍
        detail::bump(win_.no_target);
        known_run_ = 0;
        if (no_target_run_ < k_no_target_run) {
          ++no_target_run_;
          if (no_target_run_ == k_no_target_run) {
            forget();
          }
        }
        evaluate(now);
        return;
      }
      no_target_run_ = 0;

      const int64_t m = p.slots;
      if (m <= 0) {
        // 準時是 1：0 只可能出現在 Activate 或重新起算之後的第一筆，負值不可能
        detail::bump(m < 0 ? win_.invalid : win_.zeros);
        anomalies_.push(now);
        evaluate(now);
        return;
      }
      if (m > restart_slots_) {
        // 停頓超過 2 s：格點的相位重新起算，之前的事件都不算了
        detail::bump(win_.restarts);
        anomalies_.push(now);
        forget();
        have_hitch_ = true;
        last_hitch_ns_ = now;
        evaluate(now);
        return;
      }

      if (!known_ && ++known_run_ >= k_known_run) {
        known_ = true;
        observe_since_ns_ = now;
      }

      if (m > divisor_) {
        detail::bump(win_.skipped, static_cast<uint32_t>(m - divisor_));  // 到這裡 m ≤ restart_slots_
      }
      if (m == divisor_) {
        commit_run(now);
        off_run_ = 0;
        detail::bump(win_.ok);
        if (on_run_ < k_divisor_run && ++on_run_ == k_divisor_run) {
          fraction_ = false;
        }
        if (p.have_phase && p.phase_ns >= edge_ns_ && p.phase_ns < k_phase_max_ns) {
          detail::bump(win_.edge);
        }
      } else {
        on_run_ = 0;
        if (off_run_ < k_no_cadence_run) {
          ++off_run_;
        }
        // 節流值解釋得了這個格數：多睡 throttle 格，每一張就跨 1 + throttle 格。節流值是不可信的欄位，
        // 但它只能讓狀態變成 locked——最壞等於退回沒有這個偵測器的時候
        const bool explained = p.throttle >= 0 && p.throttle <= k_throttle_max && m == 1 + p.throttle;
        if (m <= k_divisor_max) {
          // 一段同樣格數的連續紀錄（run）等它結束才記事件，所以節流剛開始的那 24 張一個事件都不會產生
          if (run_len_ > 0 && m == run_slots_) {
            if (run_len_ < k_divisor_run) {
              ++run_len_;
            }
          } else {
            commit_run(now);
            run_slots_ = m;
            run_len_ = 1;
            run_explained_ = 0;
          }
          run_explained_ = explained ? std::min<uint32_t>(run_explained_ + 1, k_divisor_run) : 0;
          if (run_explained_ >= k_divisor_run) {
            adopt(m, now);
          } else if (run_len_ >= k_divisor_run) {
            fraction_ = true;  // 穩定，但沒有東西說明它是刻意的整數分之一：說不準
          }
        } else {
          commit_run(now);
          record(now, m - divisor_, 1);  // 長到不可能是節拍：這一筆立刻分類
        }
      }
      evaluate(now);
    }

    /// 每輪排空 ring 之後呼叫（至少每 100 ms 一次）：紀錄停了要變成 unknown，各個計時也要繼續走
    void on_tick(int64_t now_ns) {
      const int64_t now = tick(now_ns);
      if (!have_rec_) {
        return;
      }
      const int64_t idle = detail::elapsed(now, last_rec_ns_);
      if (idle >= k_forget_ns) {
        forget();
        have_rec_ = false;
        return;
      }
      if (idle >= k_stale_ns) {
        stale_ = true;
        return;
      }
      stale_ = false;
      evaluate(now);
    }

    state_e state() const {
      if (!have_rec_ || stale_) {
        return state_e::unknown;
      }
      if (suspect_) {
        return state_e::unsure;  // 紀錄本身不可信時，從它算出來的 slipping 也不可信
      }
      if (!known_) {
        return state_e::unknown;
      }
      if (st_ != st_e::locked) {
        return state_e::slipping;  // 有確定的證據：說不準的那幾種不蓋過它
      }
      return fraction_ || masked_recent_ || proving_ ? state_e::unsure : state_e::locked;
    }

    why_e why() const {
      switch (state()) {
        case state_e::slipping:
          return st_ == st_e::heavy ? why_e::heavy : (st_ == st_e::marginal ? why_e::marginal : why_e::recovering);
        case state_e::unsure:
          return suspect_ ? why_e::suspect : (fraction_ ? why_e::fraction : (masked_recent_ ? why_e::recent : why_e::proving));
        default:
          return why_e::none;
      }
    }

    /// 目前的節拍：每幾格一次 Present（1～k_divisor_max）
    int64_t divisor() const {
      return divisor_;
    }

    /// 給其他執行緒看的摘要（不改狀態）
    info_t info(int64_t now_ns) const {
      info_t i;
      i.state = state();
      i.why = why();
      i.divisor = i.state == state_e::unknown ? uint8_t {0} : static_cast<uint8_t>(divisor_);
      i.slips_10s = static_cast<uint8_t>(all_.count_within(now_ns, k_marginal_window_ns));
      i.stamp_ms = static_cast<uint32_t>(static_cast<uint64_t>(detail::floor_div(now_ns, k_ns_per_ms)));
      return i;
    }

    /// 10 s log 用：取走計數並歸零
    window_t take_window() {
      const window_t w = win_;
      win_ = window_t {};
      win_sec_have_ = false;
      win_sec_count_ = 0;
      win_last_slip_have_ = false;
      return w;
    }

    /// 紀錄開始出現不可能的值（每次由「可信」變成「不可信」只回一次 true；印警告用）
    bool take_suspect_alert() {
      const bool a = suspect_alert_;
      suspect_alert_ = false;
      return a;
    }

  private:
    enum class st_e : uint8_t {
      locked,
      marginal,
      heavy,
      recovering,
    };

    /// 正在組的那一幀
    struct stage_t {
      bool have_exit = false;
      int64_t exit_code = 0;
      uint32_t exit_frame = 0;
      bool have_throttle = false;
      int64_t throttle = 0;
      uint32_t throttle_frame = 0;
      bool have_phase = false;
      int64_t phase_ns = 0;
      bool have_ppexit = false;
      int64_t ppexit_ticks = 0;
    };

    /// 時間只往前：比上一次早的時刻當成和上一次相同（所有時間差是 0，不會清旗標也不會設旗標）
    int64_t tick(int64_t now_ns) {
      if (!have_now_ || now_ns > last_now_) {
        have_now_ = true;
        last_now_ = now_ns;
      }
      return last_now_;
    }

    /// 重來。不動：設定、時鐘、10 s 計數、不可能的值的紀錄（anomalies_、suspect_、suspect_alert_）、
    /// no_target_run_、正在組的那一幀、have_rec_
    void forget() {
      known_ = false;
      stale_ = false;
      proving_ = false;
      known_run_ = 0;
      divisor_ = 1;
      run_len_ = 0;
      run_slots_ = 0;
      run_explained_ = 0;
      off_run_ = 0;
      on_run_ = 0;
      fraction_ = false;
      st_ = st_e::locked;
      um_.clear();
      all_.clear();
      have_hitch_ = false;
      have_unmasked_ = false;
      have_masked_ = false;
      masked_recent_ = false;
    }

    /// 一段跨同樣格數（不是節拍）的連續紀錄結束了。事件的時刻是 run 結束時的 now
    void commit_run(int64_t now) {
      if (run_len_ == 0) {
        return;
      }
      const uint32_t n = run_len_;
      const int64_t dev = run_slots_ - divisor_;
      run_len_ = 0;
      run_explained_ = 0;
      if (n >= k_divisor_run) {
        return;  // 那是一段穩定的平台（已經用 fraction 表示），不是一串滑動
      }
      record(now, dev, n);
    }

    /// n 張各比節拍多（或少）跨 dev 格
    void record(int64_t now, int64_t dev, uint32_t n) {
      // 長間隔只有在前後大多數的 Present 都在節拍上時才算卡頓：連續 k_no_cadence_run 張都不在節拍上之後，
      // 已經沒有節拍可回，長間隔和短的一樣算滑動
      if (dev >= hitch_slots_ && off_run_ < k_no_cadence_run) {
        detail::bump(win_.hitches, n);
        have_hitch_ = true;
        last_hitch_ns_ = now;
        return;
      }
      if (dev < 0) {
        detail::bump(win_.early, n);
      }
      all_.push(now, n);
      if (have_hitch_ && detail::elapsed(now, last_hitch_ns_) < k_mask_ns) {
        // 可能是卡頓的餘波，也可能是真的滑動：只算進「重度」，並讓狀態變成說不準。不擋清除——
        // 清除只看不屬於卡頓的滑動，否則每幾秒一次有餘波的卡頓會讓旗標永遠清不掉
        detail::bump(win_.masked, n);
        have_masked_ = true;
        last_masked_ns_ = now;
        return;
      }
      um_.push(now, n);
      have_unmasked_ = true;
      last_unmasked_ns_ = now;
      detail::bump(win_.slips, n);
      // 10 s log：有滑動的整秒數、單一整秒內最多幾個、相鄰兩個最久隔多久
      const int64_t sec = detail::floor_div(now, k_ns_per_s);
      if (!win_sec_have_ || sec != win_sec_) {
        win_sec_have_ = true;
        win_sec_ = sec;
        win_sec_count_ = 0;
        detail::bump(win_.slip_secs);
      }
      detail::bump(win_sec_count_, n);
      win_.burst_max = std::max(win_.burst_max, win_sec_count_);
      if (win_last_slip_have_) {
        win_.slip_gap_max_ns = std::max(win_.slip_gap_max_ns, detail::elapsed(now, win_last_slip_ns_));
      }
      win_last_slip_have_ = true;
      win_last_slip_ns_ = now;
    }

    /// 連續 k_divisor_run 張都跨 1 + nFramesToThrottle 格：節拍換了（SteamVR 開始或結束整數分之一的節流）
    void adopt(int64_t m, int64_t now) {
      divisor_ = m;
      run_len_ = 0;
      run_explained_ = 0;
      off_run_ = 0;
      on_run_ = 0;
      fraction_ = false;
      detail::bump(win_.divisor_changes);
      um_.clear();
      all_.clear();
      have_masked_ = false;
      masked_recent_ = false;
      if (st_ != st_e::locked) {
        // 新的節拍是穩定的：比照重度結束（3 s 沒有滑動就清）
        st_ = st_e::recovering;
        have_unmasked_ = true;
        last_unmasked_ns_ = now;
      }
    }

    void evaluate(int64_t now) {
      const bool heavy_now = all_.has_within(heavy_events_, now, k_heavy_window_ns);  // 含 masked：卡頓很密又滑得很密也算重度
      const bool marginal_now = um_.has_within(k_marginal_events, now, k_marginal_window_ns);
      // 清除只看距上一個不屬於卡頓的滑動多久
      const int64_t quiet = have_unmasked_ ? detail::elapsed(now, last_unmasked_ns_) : detail::k_i64_max;
      switch (st_) {
        case st_e::locked:
          if (heavy_now) {
            st_ = st_e::heavy;
          } else if (marginal_now) {
            st_ = st_e::marginal;
          }
          break;
        case st_e::marginal:
          if (heavy_now) {
            st_ = st_e::heavy;
          } else if (quiet >= k_marginal_clear_ns) {
            st_ = st_e::locked;
          }
          break;
        case st_e::heavy:
          if (!heavy_now) {
            um_.clear();  // 重度期間的滑動不帶進臨界的判定（否則短的重度結束後還要多設 12 s）
            st_ = st_e::recovering;
          }
          break;
        case st_e::recovering:
          if (heavy_now) {
            st_ = st_e::heavy;
          } else if (marginal_now) {
            st_ = st_e::marginal;
          } else if (quiet >= k_recover_quiet_ns) {
            st_ = st_e::locked;
          }
          break;
      }
      masked_recent_ = have_masked_ && detail::elapsed(now, last_masked_ns_) < k_masked_keep_ns;
      const bool suspect = anomalies_.has_within(k_suspect_events, now, k_suspect_window_ns);
      if (suspect && !suspect_) {
        suspect_alert_ = true;
      }
      suspect_ = suspect;
      proving_ = known_ && detail::elapsed(now, observe_since_ns_) < k_prove_ns;
    }

    // 設定（reset() 算好；預設值＝90 Hz）
    int64_t period_ns_ = k_period_default_ns;
    std::size_t heavy_events_ = 18;
    int64_t restart_slots_ = 181;
    int64_t hitch_slots_ = 2;
    int64_t edge_ns_ = k_period_default_ns / k_edge_den * k_edge_num;
    int64_t ticks_per_s_ = 0;

    // 時鐘（只往前）
    bool have_now_ = false;
    int64_t last_now_ = 0;

    // 正在組的那一幀
    stage_t stage_ {};

    // 有效性
    bool have_rec_ = false;
    int64_t last_rec_ns_ = 0;
    bool known_ = false;
    bool stale_ = false;
    bool proving_ = false;
    int64_t observe_since_ns_ = 0;
    uint32_t known_run_ = 0;
    uint32_t no_target_run_ = 0;
    detail::stamps_t<k_suspect_events> anomalies_ {};  ///< 不可能的值出現的時刻
    bool suspect_ = false;
    bool suspect_alert_ = false;

    // 節拍
    int64_t divisor_ = 1;
    int64_t run_slots_ = 0;
    uint32_t run_len_ = 0;  ///< 連續幾張都跨 run_slots_ 格（封頂在 k_divisor_run）
    uint32_t run_explained_ = 0;  ///< 其中最近連續幾張是節流值解釋得了的
    uint32_t off_run_ = 0;  ///< 連續幾張不在節拍上（封頂在 k_no_cadence_run）
    uint32_t on_run_ = 0;  ///< 連續幾張在節拍上（封頂在 k_divisor_run）
    bool fraction_ = false;

    // 事件
    detail::stamps_t<8> um_ {};  ///< 不屬於卡頓的滑動
    detail::stamps_t<k_heavy_events_max> all_ {};  ///< 全部的滑動（含 masked）
    bool have_hitch_ = false;
    int64_t last_hitch_ns_ = 0;
    bool have_unmasked_ = false;
    int64_t last_unmasked_ns_ = 0;
    bool have_masked_ = false;
    int64_t last_masked_ns_ = 0;  ///< 最近一個卡頓後 k_mask_ns 內的滑動
    bool masked_recent_ = false;  ///< 最近 k_masked_keep_ns 內有 masked 的滑動

    // 旗標
    st_e st_ = st_e::locked;

    // 10 s log
    window_t win_ {};
    bool win_sec_have_ = false;
    int64_t win_sec_ = 0;
    uint32_t win_sec_count_ = 0;
    bool win_last_slip_have_ = false;
    int64_t win_last_slip_ns_ = 0;
  };

}  // namespace vr::cadence
