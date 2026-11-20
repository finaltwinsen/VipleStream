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
 *                   同時只有一路編碼在跑（同 abr_signals.h 的前提）；提示 2 秒沒有更新就失效，
 *                   所以上一個 session 留下的值、ABR 停擺、沒有 RTT 樣本時，閘門都自動回到原本的規則。
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

  /// 這個等級要給 IDR 閘門的最短間隔（ms）；0＝不提示（照原本的規則）
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
  };

  class congestion_t {
  public:
    /// 每輪 ABR 呼叫一次
    cong_out_t update(const cong_in_t &in, steady::time_point now) {
      cong_out_t out;
      out.prev_level = level_;
      have_update_ = true;
      last_update_ = now;
      last_rtt_ms_ = in.sampled ? in.rtt_ms : 0;

      // 塞車點過期（和有沒有樣本無關）
      if (point_have_ && now - point_seen_at_ >= k_point_ttl) {
        out.point_expired = true;
        out.expired_kbps = point_kbps_;
        point_have_ = false;
        point_kbps_ = 0;
      }

      if (!in.sampled) {
        // 沒有 RTT 樣本（QUIC-only、peer 還沒連上）：等級歸零，行為與沒有這個類別時相同
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
      // 上一刀之後 RTT 已經降了一成以上＝管線正在排空（ENet 的平滑 RTT 降得慢），不再追加一刀；否則會一路砍到下限
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
     * 距上一次緊急降碼滿 3 秒。乾淨網路上單獨的 IDR 請求（L0）一律回 false。
     */
    bool idr_request_wants_cut(steady::time_point now) const {
      if (!have_update_ || now - last_update_ > k_level_fresh) {
        return false;
      }
      if (level_ < level_e::cut) {
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

    /// 目前等級要給 IDR 閘門的間隔（ms）；0＝不提示
    int idr_gap_hint() const {
      return idr_gap_hint_ms(level_, last_rtt_ms_);
    }

  private:
    static bool cooled(bool have, steady::time_point last, steady::time_point now, std::chrono::milliseconds gap) {
      return !have || now - last >= gap;
    }

    level_e level_ = level_e::clean;
    bool have_update_ = false;
    steady::time_point last_update_ {};
    uint32_t last_rtt_ms_ = 0;

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
    uint32_t emergency_rtt_ms_ = 0;  ///< 上一次緊急降碼當時的 RTT

    bool point_have_ = false;
    int point_kbps_ = 0;
    steady::time_point point_set_at_ {};
    steady::time_point point_seen_at_ {};
  };

}  // namespace abr_cong
