/**
 * @file src/abr_congestion.h
 * @brief §P2 壅塞感知 ABR（2D／桌面 session，第 1 期）：壅塞等級、分級降幅、緊急降碼冷卻、塞車點記憶，
 *        以及給編碼迴圈 IDR 閘門的間隔提示。
 *
 * 純邏輯：沒有 I/O、沒有 log、時間由呼叫端傳入，可以單元測試（tests/unit/test_abr_congestion.cpp）。
 * VR session 完全不用這個檔（VR 有自己的控制器、intra refresh 與 300 ms 冷卻）。
 *
 * 執行緒：
 *   congestion_t ── 放在 session_t::video，所有成員函式都要握 video.abrMutex 才能呼叫
 *                   （ENet control 執行緒與 picoquic IO 執行緒都會進來）。
 *   idr_gap_hint ── 行程內唯一的 atomic。寫：握 abrMutex 的 ABR 那一側（run_abr_aimd、abr_on_idr_request）；
 *                   讀：編碼執行緒（video.cpp 的 IDR 閘門）。值與寫入時刻包在同一個 64 位元裡，讀寫各一次、不會撕裂。
 *                   只有一份，所以只在「行程內只有一場 session」時有效：同時超過一場時寫入端一律寫 0
 *                   （idr_hint_for_publish；stream.cpp 的 abr_publish_idr_hint 用 running_sessions 判斷），每一場的閘門
 *                   都回到原本的規則，不會把一場的間隔套到另一場。提示 2 秒沒有更新就失效，
 *                   所以上一個 session 留下的值、ABR 停擺、沒有 RTT 樣本時，閘門也自動回到原本的規則。
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace abr_cong {

  using steady = std::chrono::steady_clock;

  // ── 常數（全部寫死，沒有設定項）────────────────────────────────────────────
  /// L3 進入：RTT 高出基線 max(500 ms, 8 × 抖動)，持續 2 輪且至少 k_deep_sustain
  constexpr int k_deep_enter_excess_ms = 500;
  constexpr double k_deep_jitter_mult = 8.0;
  /// L3 退出：RTT 高出基線不到 300 ms，持續 2 輪且至少 k_deep_sustain（500 與 300 之間是遲滯帶）
  constexpr int k_deep_exit_excess_ms = 300;
  /// ABR 大約 500 ms 一輪；其他入口（FEC status、loss stats）可能插進來，所以除了輪數也看時間
  constexpr std::chrono::milliseconds k_deep_sustain {450};
  /// L1／L2 往下掉要先連續這麼久都在較低的等級（往上升立刻生效）
  constexpr std::chrono::milliseconds k_level_exit_hold {1000};
  /// 延遲降碼（趨勢 ×0.85、4 倍且多 200 ms ×0.70）兩刀之間的最短間隔
  constexpr std::chrono::milliseconds k_cut_cooldown {2000};
  /// 緊急降碼（×0.50）兩刀之間的最短間隔
  constexpr std::chrono::milliseconds k_emergency_cooldown {3000};
  constexpr int k_cut_trend_pct = 85;
  constexpr int k_cut_severe_pct = 70;
  constexpr int k_cut_emergency_pct = 50;
  /// 塞車點：記下後 5 秒內的後續刀不覆寫；30 秒沒有再到 L2 以上就清掉；快回升只能到它的 9 成
  constexpr std::chrono::milliseconds k_point_keep {5000};
  constexpr std::chrono::milliseconds k_point_ttl {30000};
  constexpr int k_point_cap_pct = 90;
  /// IDR 間隔提示：L1 不提示、L2 是 1500 ms（＝原本的冷卻），L3 是 clamp(1.5 × RTT + 500, 1500, 6000)。
  /// L1（暫停回升）的條件很容易成立——Wi-Fi 一陣丟包時 RTT 常常短暫高出基線一倍——那種時候正是 §IDR-FAST
  /// 要快速補送的場合，所以 L1 不改 IDR 的間隔，也不觸發緊急降碼。
  constexpr int k_idr_gap_base_ms = 1500;
  constexpr int k_idr_gap_max_ms = 6000;
  /// IDR 的長間隔（提示）與「IDR 請求觸發的緊急降碼」只在 RTT 本身夠長時才用：RTT 到 150 ms 開、回到 100 ms 以下關
  /// （遲滯）。RTT 比這個短時，§IDR-FAST 的 250 ms 間隔本來就比一個來回加排隊長，上一張 IDR 到不了的話 250 ms 後
  /// 的請求就是新的一次丟失，不是同一次凍結的重複請求——照 §IDR-FAST 快速補送才對。看的是量到的 RTT，不分路徑種類。
  constexpr uint32_t k_idr_slow_enter_rtt_ms = 150;
  constexpr uint32_t k_idr_slow_exit_rtt_ms = 100;
  /// RTT 樣本超過這麼久沒有更新就當成沒有樣本（ENet 的 ping 是 reliable 封包，丟包時會被重傳延後，所以不能太短）
  constexpr std::chrono::milliseconds k_rtt_fresh {3000};
  /// 審查 11-20 第二輪（D1／D3）：超過 k_rtt_fresh、但還在「ping 被 ENet 重傳擋住」可能的範圍內時，壅塞狀態維持原樣
  /// （不歸零、也不依凍結的值做任何新動作）。範圍＝max(3 秒, min(4 × RTT, 4 秒) + 1 秒)：
  /// client 的 ENet 重傳逾時是 RTT + min(RTT, 4 × 變異)、上限 timeoutMaximum/5＝2 秒（protocol.c；enet_peer_timeout
  /// 10000），所以同一個 ping 連丟兩次最多擋 min(4 × RTT, 4 秒)，再加 ping 間隔與上行排隊的餘量 1 秒。
  /// RTT 500 ms 以下時範圍就是 3 秒（＝沒有維持段，行為與以前相同）；最長 5 秒，遠短於 ENet 的斷線逾時（10 秒）。
  constexpr uint32_t k_rtt_hold_rto_cap_ms = 2000;
  constexpr uint32_t k_rtt_hold_slack_ms = 1000;
  inline std::chrono::milliseconds rtt_hold_window(uint32_t last_rtt_ms) {
    const uint64_t two_rto = std::min<uint64_t>((uint64_t) last_rtt_ms * 4, (uint64_t) k_rtt_hold_rto_cap_ms * 2);
    const auto w = std::chrono::milliseconds {(int64_t) (two_rto + k_rtt_hold_slack_ms)};
    return std::max(w, k_rtt_fresh);
  }
  /// 提示寫入後超過這麼久沒有更新就當成沒有提示
  constexpr int64_t k_idr_hint_ttl_us = 2'000'000;
  /// IDR 請求觸發緊急降碼時，等級必須是這麼久以內算出來的
  constexpr std::chrono::milliseconds k_level_fresh {2000};

  enum class level_e : int {
    clean = 0,  ///< L0：沒有壅塞跡象（或沒有 RTT 樣本）
    hold = 1,  ///< L1：暫停回升
    cut = 2,  ///< L2：延遲降碼的條件成立
    deep = 3  ///< L3：深度壅塞
  };

  enum class action_e {
    none,
    cut_trend,  ///< 延遲趨勢過載：×0.85
    cut_severe,  ///< RTT ≥ 4 倍基線且多 200 ms 連 2 輪：×0.70
    cut_emergency  ///< 深度壅塞，或壅塞中收到 IDR 請求：×0.50
  };

  constexpr int action_pct(action_e a) {
    switch (a) {
      case action_e::cut_trend:
        return k_cut_trend_pct;
      case action_e::cut_severe:
        return k_cut_severe_pct;
      case action_e::cut_emergency:
        return k_cut_emergency_pct;
      case action_e::none:
        break;
    }
    return 100;
  }

  constexpr const char *action_name(action_e a) {
    switch (a) {
      case action_e::cut_trend:
        return "cut-trend";
      case action_e::cut_severe:
        return "cut-severe";
      case action_e::cut_emergency:
        return "emergency-cut";
      case action_e::none:
        break;
    }
    return "none";
  }

  /// L3 的進入門檻（ms）：max(500, 8 × 抖動)
  inline double deep_enter_threshold_ms(double jitter_ms) {
    return std::max((double) k_deep_enter_excess_ms, k_deep_jitter_mult * std::max(0.0, jitter_ms));
  }

  /// 這個等級要給 IDR 閘門的最短間隔（ms）；0＝不提示（照原本的規則）。
  /// congestion_t::idr_gap_hint() 另外要求 RTT 夠長（k_idr_slow_*）才會用到這個值
  inline int idr_gap_hint_ms(level_e level, uint32_t rtt_ms) {
    switch (level) {
      case level_e::clean:
      case level_e::hold:
        return 0;
      case level_e::cut:
        return k_idr_gap_base_ms;
      case level_e::deep:
        {
          // 請求上行約 RTT/2、IDR 下行約 RTT/2 再加排隊：上一張還沒到之前的重複請求都屬於同一次凍結
          const int64_t gap = (int64_t) rtt_ms * 3 / 2 + 500;
          return (int) std::clamp<int64_t>(gap, k_idr_gap_base_ms, k_idr_gap_max_ms);
        }
    }
    return 0;
  }

  /**
   * @brief IDR 閘門這一刻的最短間隔（ms）。純函式，video.cpp 的編碼迴圈用。
   * @param calm §IDR-FAST 的「平常」狀態（最近 10 秒送出的 IDR 不到 3 張）。VR 恆為 false。
   * @param cooldown_ms 這個 session 的冷卻（桌面 1500、VR 300）。
   * @param fast_ms §IDR-FAST 的快速間隔（250）。
   * @param hint_ms 壅塞提示；0＝沒有。VR 一律傳 0。
   * 沒有提示時與 §IDR-FAST 原本的規則完全相同；有提示時不走快速間隔，間隔＝提示值（不短於冷卻、不長於 6 秒）。
   */
  constexpr int idr_gate_gap_ms(bool calm, int cooldown_ms, int fast_ms, int hint_ms) {
    if (hint_ms > 0) {
      const int capped = hint_ms > k_idr_gap_max_ms ? k_idr_gap_max_ms : hint_ms;
      return capped > cooldown_ms ? capped : cooldown_ms;
    }
    return calm ? fast_ms : cooldown_ms;
  }

  /**
   * @brief 實際要發布的提示：行程內同時超過一場 session 時一律 0（提示只有一份，不能把一場的間隔套到另一場）。
   * 寫 0 會立刻清掉另一場留下的值，所以第二場加入後最慢一輪 ABR（約 500 ms）內兩場都回到原本的規則。
   */
  constexpr int idr_hint_for_publish(int hint_ms, unsigned running_sessions) {
    return running_sessions > 1 ? 0 : hint_ms;
  }

  /**
   * @brief host 自己發了一張不經閘門的 IDR（RFI 做不到退回 IDR）時，延後中的 client 請求要不要取消。
   * 有壅塞提示時取消：那一張就是完整的恢復點，再補一張只是往塞住的佇列多排一張大幀；它丟了的話 client 會再要。
   * 沒有提示（平常的 250 ms 快速路徑）時不取消，行為與以前相同——寧可多一張也不要回到 1 秒凍結。
   */
  constexpr bool host_idr_cancels_deferred(int hint_ms) {
    return hint_ms > 0;
  }

  /// RTT 樣本還算不算新（沒有樣本、時刻不合理、超過 k_rtt_fresh 沒更新都回 false）
  inline bool rtt_sample_fresh(bool sampled, steady::time_point sampled_at, steady::time_point now) {
    return sampled && now >= sampled_at && now - sampled_at <= k_rtt_fresh;
  }

  /// 樣本已經不新（超過 k_rtt_fresh），但還在 rtt_hold_window(last_rtt_ms) 內：壅塞狀態維持原樣（cong_in_t::held）
  inline bool rtt_sample_held(bool sampled, steady::time_point sampled_at, steady::time_point now, uint32_t last_rtt_ms) {
    return sampled && now >= sampled_at && now - sampled_at > k_rtt_fresh && now - sampled_at <= rtt_hold_window(last_rtt_ms);
  }

  /// 丟包分支與壅塞建議同一輪都要降碼時怎麼合併（stream.cpp 的 run_abr_aimd 用）
  struct loss_merge_t {
    int target = 0;  ///< 合併後的目標（還沒夾下限）
    bool cong_wins = false;  ///< 壅塞建議降得比較多
    bool commit = false;  ///< 要不要呼叫 commit_cut 記帳
  };

  /**
   * @param loss_target 丟包分支算出來的目標；cong_target＝壅塞建議的目標；applied＝已套用的碼率；floor＝下限。
   * 目標取兩者較小的。記帳：壅塞建議降得多時照舊一定記（已在下限由 commit_cut 自己擋塞車點）；丟包降得一樣多或
   * 更多時，只要這一輪真的有降（夾過下限後低於已套用值）也要記——否則冷卻與塞車點都沒有起算，下一輪又砍一次、
   * 回升時直接衝回剛才塞住的碼率。
   */
  constexpr loss_merge_t merge_loss_cut(int loss_target, int cong_target, int applied, int floor) {
    loss_merge_t m;
    m.cong_wins = cong_target < loss_target;
    m.target = m.cong_wins ? cong_target : loss_target;
    const int after = m.target > floor ? m.target : floor;
    m.commit = m.cong_wins || after < applied;
    return m;
  }

  // ── 給編碼執行緒的提示（唯一的跨執行緒狀態）──────────────────────────────
  /// 低 13 位元＝間隔（ms，1～8191）、其餘＝寫入時刻（steady_clock µs）；0＝沒有提示
  constexpr int64_t k_idr_hint_ms_span = 8192;
  inline std::atomic<int64_t> idr_gap_hint {0};

  constexpr int64_t idr_hint_pack(int gap_ms, int64_t at_us) {
    if (gap_ms <= 0 || at_us <= 0) {
      return 0;
    }
    const int ms = gap_ms >= k_idr_hint_ms_span ? (int) k_idr_hint_ms_span - 1 : gap_ms;
    return at_us * k_idr_hint_ms_span + ms;
  }

  /// 解出還有效的提示（ms）；沒有提示、過期（超過 2 秒）或時刻不合理都回 0
  constexpr int idr_hint_unpack(int64_t packed, int64_t now_us) {
    if (packed <= 0) {
      return 0;
    }
    const int64_t at_us = packed / k_idr_hint_ms_span;
    const int ms = (int) (packed % k_idr_hint_ms_span);
    if (now_us < at_us || now_us - at_us > k_idr_hint_ttl_us) {
      return 0;
    }
    return ms;
  }

  inline int64_t steady_us(steady::time_point tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
  }

  inline void idr_hint_store(int gap_ms, steady::time_point now) {
    idr_gap_hint.store(idr_hint_pack(gap_ms, steady_us(now)), std::memory_order_relaxed);
  }

  inline int idr_hint_load(steady::time_point now) {
    return idr_hint_unpack(idr_gap_hint.load(std::memory_order_relaxed), steady_us(now));
  }

  // ── 壅塞等級 ─────────────────────────────────────────────────────────────
  /// 每一輪 ABR 的輸入。hold／trend_over／severe 是 run_abr_aimd 既有的判斷，這裡不重算
  struct cong_in_t {
    bool sampled = false;  ///< 有沒有 RTT 樣本；沒有就固定 L0、所有新規則不動作
    /// sampled＝false 時才看：樣本只是被 ENet 重傳暫時擋住（rtt_sample_held）。等級與事件狀態維持原樣、不建議動作、
    /// IDR 請求不降碼；超過維持範圍（呼叫端不再設這個旗標）才照「沒有樣本」歸零
    bool held = false;
    uint32_t rtt_ms = 0;  ///< 最近一次 RTT
    uint32_t base_ms = 0;  ///< 基線（兩段 30 秒視窗最小值）
    double jitter_ms = 0.0;  ///< 延遲趨勢估計器的抖動（p90−p10）
    bool hold = false;  ///< 暫停回升的條件（趨勢過載或剛過載、排空中、2 倍且多 50 ms、QUIC 背壓）
    bool trend_over = false;  ///< 趨勢現在是過載
    bool severe = false;  ///< RTT ≥ 4 倍基線且多 200 ms，已經連 2 輪
  };

  struct cong_out_t {
    level_e level = level_e::clean;
    level_e prev_level = level_e::clean;
    action_e action = action_e::none;  ///< 這一輪建議的降碼；真的降了要呼叫 commit_cut
    bool point_expired = false;  ///< 這一輪塞車點過期被清掉
    int expired_kbps = 0;
    bool held = false;  ///< 這一輪是「樣本被擋住、狀態維持」
    bool prev_held = false;  ///< 上一輪是不是（給 log 判斷進出）
  };

  class congestion_t {
  public:
    /// 每輪 ABR 呼叫一次
    cong_out_t update(const cong_in_t &in, steady::time_point now) {
      cong_out_t out;
      out.prev_level = level_;
      out.prev_held = held_;
      have_update_ = true;
      last_update_ = now;

      // 塞車點過期（和有沒有樣本無關）
      if (point_have_ && now - point_seen_at_ >= k_point_ttl) {
        out.point_expired = true;
        out.expired_kbps = point_kbps_;
        point_have_ = false;
        point_kbps_ = 0;
      }

      // 審查 11-20 第二輪（D1／D3）：樣本只是被 ENet 重傳擋住——等級、L3 鎖定、排空基準、rtt_slow_、上一個 RTT
      // （IDR 提示用）全部維持，也不往下掉（below_ 計時不動）；不建議動作。凍結的值不拿來做新判斷，
      // 但也不因為「暫時量不到」就把深度壅塞當成乾淨網路（IDR 回到 250 ms 快速補送、下一個樣本來時重新數輪再砍一刀）
      held_ = !in.sampled && in.held;
      if (held_) {
        out.held = true;
        out.level = level_;
        return out;
      }
      last_rtt_ms_ = in.sampled ? in.rtt_ms : 0;

      // IDR 長間隔與 IDR 請求降碼用的「RTT 夠長」：150 ms 開、100 ms 以下關；沒有樣本就關
      if (!in.sampled || in.rtt_ms < k_idr_slow_exit_rtt_ms) {
        rtt_slow_ = false;
      } else if (in.rtt_ms >= k_idr_slow_enter_rtt_ms) {
        rtt_slow_ = true;
      }

      if (!in.sampled) {
        // 沒有 RTT 樣本（QUIC-only、peer 還沒連上、樣本太舊）：等級歸零，行為與沒有這個類別時相同
        emergency_rtt_ms_ = 0;  // 「排空中」的比較基準只在同一次壅塞事件內有效
        deep_latched_ = false;
        deep_rounds_ = 0;
        exit_rounds_ = 0;
        below_ = false;
        level_ = level_e::clean;
        out.level = level_;
        return out;
      }

      const double excess = in.rtt_ms > in.base_ms ? (double) (in.rtt_ms - in.base_ms) : 0.0;
      const bool deep_now = excess >= deep_enter_threshold_ms(in.jitter_ms);

      // L3 進入：條件連續成立 2 輪且滿 k_deep_sustain
      if (deep_now) {
        if (deep_rounds_ == 0) {
          deep_since_ = now;
        }
        ++deep_rounds_;
      } else {
        deep_rounds_ = 0;
      }
      const bool deep_sustained = deep_now && deep_rounds_ >= 2 && now - deep_since_ >= k_deep_sustain;
      if (deep_sustained) {
        deep_latched_ = true;
      }

      // L3 退出：低於退出門檻連續 2 輪且滿 k_deep_sustain
      bool deep_left = false;
      if (deep_latched_ && excess < (double) k_deep_exit_excess_ms) {
        if (exit_rounds_ == 0) {
          exit_since_ = now;
        }
        ++exit_rounds_;
        if (exit_rounds_ >= 2 && now - exit_since_ >= k_deep_sustain) {
          deep_latched_ = false;
          deep_left = true;
          // 這一次壅塞事件結束：「排空中」的比較基準作廢。不清的話，之後 RTT 較低的深度壅塞（例如上次 3000 ms、
          // 這次 900 ms）永遠被當成「還在排空」，緊急降碼整場不再觸發。冷卻（have_emergency_／last_emergency_）不動
          emergency_rtt_ms_ = 0;
          exit_rounds_ = 0;
        }
      } else {
        exit_rounds_ = 0;
      }

      level_e raw = level_e::clean;
      if (deep_latched_) {
        raw = level_e::deep;
      } else if (in.trend_over || in.severe) {
        raw = level_e::cut;
      } else if (in.hold) {
        raw = level_e::hold;
      }

      if (deep_left && level_ == level_e::deep) {
        // L3 自己有進出門檻的遲滯；出來先落在 L2，再照下面的規則往下掉
        level_ = std::max(raw, level_e::cut);
        below_ = false;
      }
      if (raw >= level_) {
        level_ = raw;
        below_ = false;
      } else {
        if (!below_) {
          below_ = true;
          below_since_ = now;
        }
        if (now - below_since_ >= k_level_exit_hold) {
          level_ = raw;
          below_ = false;
        }
      }

      if (raw >= level_e::cut && point_have_) {
        point_seen_at_ = now;  // 還在塞（看這一輪的條件，不看遲滯撐住的等級）：塞車點的 30 秒重新起算
      }

      // 這一輪建議的動作
      // 上一刀之後 RTT 已經降了一成以上＝管線正在排空（ENet 的平滑 RTT 降得慢），不再追加一刀；否則會一路砍到下限。
      // 基準（emergency_rtt_ms_）離開 L3 或沒有樣本時歸零，歸零後這個條件恆不成立
      const bool draining = have_emergency_ && (uint64_t) in.rtt_ms * 10 < (uint64_t) emergency_rtt_ms_ * 9;
      if (deep_sustained && !draining && cooled(have_emergency_, last_emergency_, now, k_emergency_cooldown)) {
        out.action = action_e::cut_emergency;
      } else if (cooled(have_cut_, last_cut_, now, k_cut_cooldown)) {
        if (in.severe) {
          out.action = action_e::cut_severe;
        } else if (in.trend_over) {
          out.action = action_e::cut_trend;
        }
      }

      out.level = level_;
      return out;
    }

    /**
     * @brief 收到 client 的 IDR 請求時問：要不要當場做一次緊急降碼。
     * 要同時成立：等級是最近 2 秒內算出來的、已經在 L2 以上（延遲降碼的條件成立＋畫面已經壞掉，兩個獨立訊號）、
     * RTT 夠長（k_idr_slow_*；RTT 只有幾十毫秒時趨勢短暫過載加上丟一張不算深度壅塞，砍半並鎖進 L3 會把
     * §IDR-FAST 的快速補送關掉）、距上一次緊急降碼滿 3 秒。乾淨網路上單獨的 IDR 請求（L0）一律回 false。
     */
    bool idr_request_wants_cut(steady::time_point now) const {
      if (!have_update_ || now - last_update_ > k_level_fresh || held_) {
        return false;  // held_：RTT 量不到的那幾秒不做新的降碼（等級照舊撐住、IDR 間隔照舊）
      }
      if (!congested_for_recovery()) {
        return false;
      }
      return cooled(have_emergency_, last_emergency_, now, k_emergency_cooldown);
    }

    /**
     * @brief 真的降碼了（或緊急降碼被觸發）時記帳：冷卻起算、記塞車點。
     * @param kbps_before 降碼前已套用的碼率。
     * @param kbps_after 降碼後的碼率；已經在下限、實際沒有降（≥ kbps_before）時不記塞車點——
     *                   否則下限會被記成「塞車的碼率」，回升一開始就被它壓住。−1＝不知道（照有降算）。
     * @return 這一次有沒有寫入（覆寫）塞車點。
     */
    bool commit_cut(action_e a, int kbps_before, steady::time_point now, int kbps_after = -1) {
      if (a == action_e::none) {
        return false;
      }
      have_cut_ = true;
      last_cut_ = now;
      if (a == action_e::cut_emergency) {
        have_emergency_ = true;
        last_emergency_ = now;
        emergency_rtt_ms_ = last_rtt_ms_;
        // IDR 請求觸發的那一種也進 L3（IDR 閘門改用長間隔）；之後照 L3 的退出規則出來
        deep_latched_ = true;
        exit_rounds_ = 0;
        level_ = level_e::deep;
        below_ = false;
      }
      bool recorded = false;
      const bool lowered = kbps_after < 0 || kbps_after < kbps_before;
      if (lowered && kbps_before > 0 && (!point_have_ || now - point_set_at_ >= k_point_keep)) {
        point_kbps_ = kbps_before;
        point_set_at_ = now;
        recorded = true;
      }
      if (kbps_before > 0 || point_have_) {
        point_have_ = point_have_ || recorded;
        point_seen_at_ = now;
      }
      return recorded;
    }

    level_e level() const {
      return level_;
    }

    /// 快回升的上限（kbps）；0＝沒有塞車點或已經過期
    int ramp_cap_kbps(steady::time_point now) const {
      if (!point_have_ || now - point_seen_at_ >= k_point_ttl) {
        return 0;
      }
      return (int) ((int64_t) point_kbps_ * k_point_cap_pct / 100);
    }

    int congested_kbps() const {
      return point_have_ ? point_kbps_ : 0;
    }

    /// 目前等級要給 IDR 閘門的間隔（ms）；0＝不提示。RTT 不夠長（k_idr_slow_*）時不提示，閘門照 §IDR-FAST 走
    int idr_gap_hint() const {
      return congested_for_recovery() ? idr_gap_hint_ms(level_, last_rtt_ms_) : 0;
    }

    /**
     * @brief 「這是壅塞，恢復手段要讓路」的共用判斷：等級 L2 以上，而且 RTT 夠長（k_idr_slow_*，150 ms 開、100 ms 以下關）。
     * 三處用同一個條件，免得又分岔：IDR 長間隔提示（idr_gap_hint）、IDR 請求觸發的緊急降碼（idr_request_wants_cut）、
     * 平面補包拒補（refuse_repair）。RTT 只有幾毫秒時，趨勢短暫過載加上一陣丟包是 Wi-Fi 的停頓不是佇列滿出來，
     * 補包與 §IDR-FAST 都照常。只看量到的 RTT 與等級，不分路徑種類。L3 的進入門檻（高出基線 ≥ 500 ms）本身就讓 RTT ≥ 150。
     */
    bool congested_for_recovery() const {
      return level_ >= level_e::cut && rtt_slow_;
    }

    /// §FLAT-REPAIR：平面補包要不要拒補（審查 11-20 第二輪 C09：以前只看 L2 以上，區網一陣丟包就把補包關掉）
    bool refuse_repair() const {
      return congested_for_recovery();
    }

    /// 這一輪是不是「樣本被擋住、狀態維持」；給 log 與測試看
    bool held() const {
      return held_;
    }

    /// RTT 是否夠長（150 ms 開、100 ms 以下關）；給 log 與測試看
    bool rtt_slow() const {
      return rtt_slow_;
    }

  private:
    static bool cooled(bool have, steady::time_point last, steady::time_point now, std::chrono::milliseconds gap) {
      return !have || now - last >= gap;
    }

    level_e level_ = level_e::clean;
    bool have_update_ = false;
    steady::time_point last_update_ {};
    uint32_t last_rtt_ms_ = 0;
    bool rtt_slow_ = false;  ///< RTT ≥ 150 ms 開、< 100 ms 關：IDR 長間隔、IDR 請求降碼、拒補才生效
    bool held_ = false;  ///< 最近一輪是 cong_in_t::held（樣本被 ENet 重傳擋住）

    bool deep_latched_ = false;
    int deep_rounds_ = 0;
    steady::time_point deep_since_ {};
    int exit_rounds_ = 0;
    steady::time_point exit_since_ {};

    bool below_ = false;  ///< 原始等級低於目前等級，正在等 k_level_exit_hold
    steady::time_point below_since_ {};

    bool have_cut_ = false;
    steady::time_point last_cut_ {};
    bool have_emergency_ = false;
    steady::time_point last_emergency_ {};
    uint32_t emergency_rtt_ms_ = 0;  ///< 這一次壅塞事件裡上一次緊急降碼當時的 RTT；0＝沒有基準

    bool point_have_ = false;
    int point_kbps_ = 0;
    steady::time_point point_set_at_ {};
    steady::time_point point_seen_at_ {};
  };

}  // namespace abr_cong
