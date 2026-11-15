/**
 * @file src/vr/vr_edge_hold.h
 * @brief §VR-EDGE-HOLD（2026-11-14）：按鍵的變化晚到時，先讓遊戲看到「變化那一刻」的手，再跟上最新的樣本。
 *        純邏輯、不依賴平台，bridge 的 tracking 寫入端與單元測試共用。
 *
 * 問題。頭盔每 1.7 ms 送一筆追蹤樣本（姿態＋按鍵）。上行有幾十毫秒的空窗時（外地經 VPN 每秒約 2 次、每次 27～70 ms），
 * 空窗裡的樣本在空窗結束時一次全到，driver 只讀最新的一筆。空窗期間姿態照速度外插、接得起來，但按鍵不會外插：
 * 「放開扳機」要等到空窗結束才被遊戲看到，而那一刻遊戲拿到的手是最新的樣本——比放開的那一刻晚了一整個空窗。
 * 拋球就是這樣壞的：手往上、放開、馬上停住；遊戲在放開之後 60 ms 才看到放開，那時手速已經剩一半以下，球就拋不起來
 * 甚至往下掉（2026-11-14 無人量測：45 次拋球有 3～4 次放開的訊號晚 28～70 ms）。
 *
 * 做法。一筆樣本同時滿足「比平常晚到」和「按鍵或扳機／握把的類比值相對上一次有變」時，寫進 ring 之後接下來一小段時間
 * （預設 12 ms，約一個半顯示格）不寫沒有變化的樣本：遊戲下一格同時讀到這個變化和這一筆的姿態與速度。同一批裡後面還有
 * 變化（例如扳機從 1 一路放到 0）就照樣寫、時間重新起算，所以最後停在「變化結束」的那一筆。時間到了之後的第一筆樣本
 * 照常寫（那時已經是準時到的樣本）。
 *   - 姿態仍然帶自己的時刻：SteamVR 照它的速度外插到顯示時刻，等速時位置和空窗期間外插出來的一樣，不會跳；
 *     遊戲看到的速度是「變化那一刻」的速度。
 *   - 準時到的變化不處理（差距本來就不到一格）。
 *   - 一筆樣本裡頭、兩手是一起的：這 12 ms 內頭的姿態也晚 12 ms 更新（照樣由 SteamVR 外插）。只發生在晚到的按鍵變化，
 *     一場裡很少。
 *   - 連續處理不超過 100 ms（保險；正常情況一次就是 12 ms）。
 */
#pragma once

#include <cstdint>
#include <cstdlib>

namespace vr::edge_hold {

  /// 兩手的輸入裡會被遊戲當成「事件」的部分（搖桿與觸碰不算）
  struct inputs_t {
    uint32_t buttons[2] = {0, 0};
    uint16_t trigger[2] = {0, 0};
    uint16_t grip[2] = {0, 0};
  };

  struct hold_t {
    static constexpr int k_analog_step = 0x0800;  ///< 類比值相對上一次算「有變」的幅度（滿刻度的 3.1%）
    static constexpr int k_analog_zero = 0x0400;  ///< 低於這個值（1.6%）當成完全放開：放著不動時在 0 附近跳一兩格的雜訊不算變化
    static constexpr double k_typ_up = 1.0 / 8.0;  ///< 「平常的提前量」往上跟得快（準時的樣本決定它）
    static constexpr double k_typ_down = 1.0 / 512.0;  ///< 往下跟得慢（成批晚到的樣本不該把它拉下來）

    // 設定（tick；hold_ticks ≤ 0＝關）
    int64_t hold_ticks = 0;  ///< 寫了晚到的變化之後，這麼久之內不寫沒有變化的樣本
    int64_t late_ticks = 0;  ///< 提前量比平常少這麼多以上＝晚到
    int64_t max_ticks = 0;  ///< 連續處理的上限

    // 狀態
    bool have_ref = false;
    inputs_t ref;  ///< 上一次算「有變」時的輸入
    bool have_typ = false;
    double typ_off = 0.0;  ///< 平常的提前量（target − arrival，tick）
    int64_t hold_until = 0;  ///< 0＝沒有在處理
    int64_t hold_start = 0;
    int64_t block_until = 0;  ///< 連續處理到上限之後，這個時刻之前不再開始

    // 統計
    uint64_t holds = 0;  ///< 開始處理的次數
    uint64_t deferred = 0;  ///< 因此沒寫的樣本數
    int64_t last_late_ticks = 0;  ///< 最近一次開始處理的那一筆晚了多少

    void reset() {
      have_ref = false;
      have_typ = false;
      hold_until = 0;
      hold_start = 0;
      block_until = 0;
    }

    bool changed(const inputs_t &in) const {
      for (int h = 0; h < 2; ++h) {
        if (in.buttons[h] != ref.buttons[h]) {
          return true;
        }
        // 幅度夠大，或是「完全放開」與「有按」之間的轉換（放到底的最後一小段不到 3.1% 也要算）
        if (std::abs((int) in.trigger[h] - (int) ref.trigger[h]) >= k_analog_step || (in.trigger[h] < k_analog_zero) != (ref.trigger[h] < k_analog_zero)) {
          return true;
        }
        if (std::abs((int) in.grip[h] - (int) ref.grip[h]) >= k_analog_step || (in.grip[h] < k_analog_zero) != (ref.grip[h] < k_analog_zero)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @param in 這一筆樣本的輸入
     * @param off_ticks 這一筆的提前量（target_server − arrival）
     * @param now 現在（和 off_ticks 同一個時鐘的 tick）
     * @return true＝寫進 ring；false＝這一筆不寫（正在讓遊戲看上一筆有變化的樣本）
     */
    bool on_sample(const inputs_t &in, int64_t off_ticks, int64_t now) {
      const bool late = have_typ && (typ_off - (double) off_ticks) > (double) late_ticks;
      const int64_t late_by = have_typ ? (int64_t) (typ_off - (double) off_ticks) : 0;
      if (!have_typ) {
        typ_off = (double) off_ticks;
        have_typ = true;
      } else {
        typ_off += ((double) off_ticks - typ_off) * ((double) off_ticks > typ_off ? k_typ_up : k_typ_down);
      }
      const bool had_ref = have_ref;
      const bool chg = !have_ref || changed(in);
      if (chg) {
        ref = in;
        have_ref = true;
      }
      if (hold_ticks <= 0) {
        hold_until = 0;
        return true;
      }
      if (hold_until != 0 && now < hold_until) {
        if (now - hold_start >= max_ticks) {
          // 保險：晚到的變化一個接一個（例如線路的延遲突然變長、同時扳機在慢慢按）時不讓姿態一直只跟著變化更新
          hold_until = 0;
          block_until = now + max_ticks;
          return true;
        }
        if (!chg) {
          ++deferred;
          return false;
        }
        // 同一批裡又有變化：照樣寫。還是晚到的就重新起算（最後停在變化結束的那一筆），準時的就結束
        hold_until = late ? now + hold_ticks : 0;
        return true;
      }
      hold_until = 0;
      if (chg && had_ref && late && now >= block_until) {
        hold_until = now + hold_ticks;
        hold_start = now;
        last_late_ticks = late_by;
        ++holds;
      }
      return true;
    }
  };

}  // namespace vr::edge_hold
