/**
 * @file src/abr_local.h
 * @brief §ABR-LOCAL：2D 多連線送出端自己看得到的「送不出去」訊號。
 *
 * 純邏輯：不依賴 Sunshine 其他標頭、沒有 I/O、沒有 log，時間由呼叫端傳入。
 * 單元測試直接 include（tests/unit/test_abr_local.cpp）。
 *
 * 多連線是每條各送一份完整影像（冗餘），不是把流量拆開。只要有一條全程送得出去，就不該降。
 * 呼叫端每輪交上每一條的累計 tx／drop，以及這一刻是否在送影像。本類別逐條記基準、滿窗才結算。
 * 這一窗的代表值是「整窗都在送、而且封包數夠」的連線裡，丟棄比例最低的那一條。
 */
#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace abr_local {

  using steady = std::chrono::steady_clock;

  // ── 常數（全部寫死，沒有設定項）────────────────────────────────────────────
  /// 一窗至少這麼長才結算：短於半秒的尾批抖動不當成飽和。控制迴圈閒置時約 150 ms 一輪，滿窗大約 3～4 輪
  constexpr std::chrono::milliseconds k_window {500};
  /// 兩次 cut 至少隔這麼久：送出端每窗都砍會把碼率一路打到下限；2 秒和既有的一般降碼冷卻同長
  constexpr std::chrono::milliseconds k_cut_cooldown {2000};
  /// 最近這麼久內有過飽和窗就暫停回升：比一窗長，也蓋得住「控制連線被掐住的那幾秒」結束後立刻加碼
  constexpr std::chrono::milliseconds k_recent {3000};
  /// client 回報超過這麼久沒來才算斷：比控制迴圈一輪長、比 RTT 樣本的 3 秒新鮮窗短，掐斷的頭 1.5 秒就能自己降
  constexpr std::chrono::milliseconds k_feedback_stale {1500};
  /// 一窗至少丟掉這麼多個封包才算飽和：個位數是尾批過期的雜訊，不值得降碼
  constexpr uint64_t k_min_drop = 20;
  /// 丟掉的佔（送出＋丟掉）至少這個千分比才算飽和（5%）
  constexpr int k_sat_permille = 50;
  /// 比例到這個千分比就是重度飽和（30%）：一窗就降，不等第二窗
  constexpr int k_severe_permille = 300;
  /// 一條連線這一窗至少要有這麼多個封包（送出＋丟掉）才有資格當代表。
  /// 下限碼率約 1.5 Mbps、500 ms 大約六七十個封包；少於 50 的是探測尾段、剛加入或幾乎沒在送，比例不可信
  constexpr uint64_t k_min_link_pkts = 50;
  /// 和 vr::multilink::kMaxLinks（VIPLE_VR_LINK_MAX）相同。這個標頭不 include 那個檔，stream.cpp 用 static_assert 對齊
  constexpr int k_max_links = 4;

  /// 一條連線這一輪的累計。tx／drop 是這條連線自己的生命週期計數，不是各條加總
  struct link_in_t {
    uint64_t tx_pkts = 0;  ///< 累計送出封包數（含補包）
    uint64_t drop_pkts = 0;  ///< 累計 dropAge + dropBlock + dropFull（不含故障注入、不含暫停清佇列）
    bool sending = false;  ///< 這一刻 send_video 會把影像排進這條連線
    int id = 0;  ///< 給 log 的連線 id（判斷用的是陣列位置，id 只印出來）
  };

  enum class verdict_e {
    none,  ///< 這一窗沒有飽和（或還沒滿窗、沒有合格連線、或時間倒退）
    hold,  ///< 飽和了，但還沒到降碼條件，或降碼冷卻中
    cut  ///< 要降一刀
  };

  struct local_out_t {
    verdict_e verdict = verdict_e::none;
    uint64_t win_tx = 0;  ///< 代表連線這一窗送出的封包。沒有合格連線時是 0
    uint64_t win_drop = 0;
    int win_ms = 0;  ///< 這一窗的實際長度（輪詢晚了就會長於 k_window）。未滿窗時也填，但不拿來判
    int drop_permille = 0;  ///< 代表連線的 win_drop / (win_tx + win_drop)，千分比；沒有代表連線時是 0
    bool feedback_stale = false;
    int link_id = -1;  ///< 代表連線的 id；沒有合格連線是 −1
    int links = 0;  ///< 這一窗合格的條數（整窗都在送、且封包數夠）
  };

  /**
   * @brief 逐條連線的飽和視窗。
   * 不是執行緒安全的：呼叫端要自己序列化（2D 放在 video.abrMutex 下）。
   * 同一個窗的時鐘是各條共用的。一條在這一窗中途改了「在不在送」、或計數器倒退，只有那一條這一窗不合格。
   */
  class local_signal_t {
  public:
    /**
     * @param links  前 count 格有效。同一格跨輪要是同一條連線（slot 穩定；換線由呼叫端換格或讓計數倒退）
     * @param count  0～k_max_links。超出的部分不看，負的當成 0
     * @param feedback_stale  這一場的 client 回報是否已超過 k_feedback_stale。各條共用，不是每條一個
     */
    local_out_t update(const std::array<link_in_t, k_max_links> &links, int count, bool feedback_stale, steady::time_point now) {
      local_out_t out;
      out.feedback_stale = feedback_stale;
      const int n = clamp_count(count);

      // 第一筆只取基準。時間倒退：這一窗作廢、連續窗數歸零，最近飽和與降碼冷卻保留（倒退不是「已經不飽和」）
      if (!have_ || now < window_start_) {
        if (have_) {
          sat_streak_ = 0;
        }
        open_window(links, n, now);
        return out;
      }

      // 先看這一筆有沒有讓某條失去資格（中途退出、倒退）。基準仍停在窗起點，所以滿窗時減出來的是整窗
      observe(links, n);

      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_);
      out.win_ms = to_win_ms(elapsed);
      if (elapsed < k_window) {
        return out;  // 未滿窗：資格的變化先記著，下一筆仍從同一個窗起算。飽和記憶不動
      }

      int best = -1;
      uint64_t best_tx = 0;
      uint64_t best_drop = 0;
      int qualified = 0;
      for (int i = 0; i < n; ++i) {
        if (!slot_[(size_t) i].candidate) {
          continue;
        }
        const uint64_t tx = links[(size_t) i].tx_pkts - slot_[(size_t) i].base_tx;
        const uint64_t drop = links[(size_t) i].drop_pkts - slot_[(size_t) i].base_drop;
        const uint64_t tot = tx > UINT64_MAX - drop ? UINT64_MAX : tx + drop;
        if (tot < k_min_link_pkts) {
          continue;  // 封包太少，比例不可信
        }
        ++qualified;
        // 比例相同留較小的 slot（嚴格小於才換）。log 印的是該格的 id
        if (best < 0 || ratio_less(drop, tot, best_drop, best_tx > UINT64_MAX - best_drop ? UINT64_MAX : best_tx + best_drop)) {
          best = i;
          best_tx = tx;
          best_drop = drop;
        }
      }

      out.links = qualified;
      if (best >= 0) {
        out.link_id = links[(size_t) best].id;
        out.win_tx = best_tx;
        out.win_drop = best_drop;
        out.drop_permille = permille(best_drop, best_tx);
      }

      const bool sat = best >= 0 && out.win_drop >= k_min_drop && out.drop_permille >= k_sat_permille;
      const bool severe = best >= 0 && out.win_drop >= k_min_drop && out.drop_permille >= k_severe_permille;
      if (best < 0) {
        sat_streak_ = 0;  // 沒有合格連線：這一窗不判、不算飽和
      } else if (sat) {
        if (sat_streak_ < 100) {
          ++sat_streak_;  // 封頂：連續飽和很久也不要讓 int 溢位；2 就夠判 cut
        }
        last_sat_ = now;
        have_sat_ = true;
      } else {
        sat_streak_ = 0;  // 合格連線裡最好的那條是乾淨的：連續窗被打斷
      }

      // 重度一窗就降；一般飽和要連續兩窗。另外：飽和而且 client 回報斷了，沒有別人會降，一窗就降。
      // 判的是代表連線（比例最低的合格連線）。另一條更差不會把這一刀抬成重度
      const bool want_cut = severe || sat_streak_ >= 2 || (sat && feedback_stale);
      // now < last_cut_（時間倒退之後的新窗）不算冷卻中，否則負的時間差會把冷卻卡死
      const bool cooling = have_cut_ && now >= last_cut_ && now - last_cut_ < k_cut_cooldown;
      if (want_cut && cooling) {
        out.verdict = verdict_e::hold;  // 冷卻中不重複砍；連續窗數留著，冷卻一過下一窗就能降
      } else if (want_cut) {
        out.verdict = verdict_e::cut;
        have_cut_ = true;
        last_cut_ = now;
      } else if (sat) {
        out.verdict = verdict_e::hold;
      }
      // 只有 feedback_stale、代表連線沒有飽和：none。client 卡住由既有的 held 處理，不要因此降碼

      // 下一窗從這一筆起算。現在正在送的連線重新取得資格；倒退點到這一筆之間的增量不另開子窗
      open_window(links, n, now);
      return out;
    }

    /// 最近 k_recent 內有過飽和窗（含剛好滿 k_recent 的那一毫秒）。給「暫停回升」用；未滿窗的呼叫不改這個結果
    bool saturated_recently(steady::time_point now) const {
      return have_sat_ && now >= last_sat_ && now - last_sat_ <= k_recent;
    }

    void reset() {
      *this = local_signal_t {};
    }

  private:
    struct slot_t {
      bool candidate = false;  ///< 從這個窗的第一筆到目前每一筆都在送，而且計數沒有倒退
      uint64_t base_tx = 0;
      uint64_t base_drop = 0;
    };

    static int clamp_count(int count) {
      if (count < 0) {
        return 0;
      }
      if (count > k_max_links) {
        return k_max_links;
      }
      return count;
    }

    static int permille(uint64_t drop, uint64_t tx) {
      if (drop == 0) {
        return 0;
      }
      if (tx > UINT64_MAX - drop) {
        return 1000;
      }
      const uint64_t denom = tx + drop;
      if (drop >= denom || drop > UINT64_MAX / 1000) {
        return 1000;
      }
      return (int) (drop * 1000 / denom);
    }

    static int to_win_ms(std::chrono::milliseconds elapsed) {
      const auto ms = elapsed.count();
      if (ms <= 0) {
        return 0;
      }
      if (ms > 86400000) {
        return 86400000;  // 只給 log 看：迴圈停擺超過一天也沒有必要印更大的數字
      }
      return (int) ms;
    }

    /// drop_a/tot_a < drop_b/tot_b。用 64×64 的高低位比較，窗內封包數再大也不溢位
    static bool ratio_less(uint64_t drop_a, uint64_t tot_a, uint64_t drop_b, uint64_t tot_b) {
      uint64_t a_hi = 0;
      uint64_t a_lo = 0;
      uint64_t b_hi = 0;
      uint64_t b_lo = 0;
      mul_u64(drop_a, tot_b, a_hi, a_lo);
      mul_u64(drop_b, tot_a, b_hi, b_lo);
      return a_hi < b_hi || (a_hi == b_hi && a_lo < b_lo);
    }

    static void mul_u64(uint64_t a, uint64_t b, uint64_t &hi, uint64_t &lo) {
      const uint64_t a0 = a & 0xffffffffu;
      const uint64_t a1 = a >> 32;
      const uint64_t b0 = b & 0xffffffffu;
      const uint64_t b1 = b >> 32;
      const uint64_t p0 = a0 * b0;
      const uint64_t p1 = a1 * b0;
      const uint64_t p2 = a0 * b1;
      const uint64_t p3 = a1 * b1;
      const uint64_t cy = (p0 >> 32) + (p1 & 0xffffffffu) + (p2 & 0xffffffffu);
      lo = (p0 & 0xffffffffu) | (cy << 32);
      hi = p3 + (p1 >> 32) + (p2 >> 32) + (cy >> 32);
    }

    void open_window(const std::array<link_in_t, k_max_links> &links, int n, steady::time_point now) {
      window_start_ = now;
      have_ = true;
      for (int i = 0; i < k_max_links; ++i) {
        auto &s = slot_[(size_t) i];
        if (i < n && links[(size_t) i].sending) {
          s.candidate = true;
          s.base_tx = links[(size_t) i].tx_pkts;
          s.base_drop = links[(size_t) i].drop_pkts;
        } else {
          s.candidate = false;
          s.base_tx = 0;
          s.base_drop = 0;
        }
      }
    }

    /// 這一筆讓某條失去「整窗都在送」的資格。退出的那一段增量不補進任何合計
    void observe(const std::array<link_in_t, k_max_links> &links, int n) {
      for (int i = 0; i < k_max_links; ++i) {
        auto &s = slot_[(size_t) i];
        if (i >= n || !links[(size_t) i].sending) {
          s.candidate = false;
          continue;
        }
        if (!s.candidate) {
          continue;  // 這一窗中途才開始送，或先前已不合格：維持到這個窗結算
        }
        if (links[(size_t) i].tx_pkts < s.base_tx || links[(size_t) i].drop_pkts < s.base_drop) {
          s.base_tx = links[(size_t) i].tx_pkts;
          s.base_drop = links[(size_t) i].drop_pkts;
          s.candidate = false;
        }
      }
    }

    bool have_ = false;
    std::array<slot_t, k_max_links> slot_ {};
    steady::time_point window_start_ {};
    int sat_streak_ = 0;
    bool have_sat_ = false;
    steady::time_point last_sat_ {};
    bool have_cut_ = false;
    steady::time_point last_cut_ {};
  };

}  // namespace abr_local
