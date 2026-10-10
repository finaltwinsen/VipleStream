/**
 * @file src/vr/vr_capture_publish.h
 * @brief VipleStream 2.0 §VR-CADENCE（2026-10-10）：擷取端什麼時候把健康判定的快照（vr_capture_health.h）發布給其他
 *        執行緒，以及狀態變化要印哪幾行 log。
 *
 * 這些判斷原本寫在 display_vr_t::capture() 的幾個 lambda 裡。那個檔只有 Windows 的正式建置才編得到，沒有測試擋著，
 * 而 stream.cpp 讀到的決定（LATCH、§VR-PREDICT 要不要取樣）什麼時候換，就是這裡決定的。所以搬成純邏輯：只用標準
 * 函式庫、不碰 atomic、不印 log、不讀時鐘（時間由呼叫端傳入，單位自訂），可以脫離整個 server 單獨編譯測試
 * （tests/unit/test_vr_capture_publish.cpp）。呼叫端照回傳的 action_t 去寫快照、印 log。只給擷取執行緒用。
 *
 * 規則：
 *   發布：三個旗標（過載、LATCH 不取樣、PREDICT 不取樣）任何一個和已發布的不同就立刻發布；其餘每秒發布一次
 *         （快照裡的數字與節拍狀態）。late 模式（預設）與開關關著時三個旗標相等，所以和加後兩個旗標之前相同：
 *         只看過載旗標。
 *   `overload on／off`：已發布的過載旗標每變一次一行（不限速：它設要 ≥ 30%、清要 < 5% 持續 2 s，本來就換不快）。
 *   `cadence` 行：已發布的節拍狀態和上一行印的不同就印，同一秒內最多一行。來不及印的先放著，下一次發布時如果還是
 *         不同才補印；行尾的 skipped＝上一行之後還有幾次變化完全沒有印出來（延後補印的那一次不算）。
 *   `hold` 行：只有 cadence 模式印。已發布的兩個決定和上一行印的不同就印，同樣同一秒內最多一行、同樣有 skipped
 *         （節拍在「鎖得住」與「說不準」之間來回時決定會跟著換，不限速的話一秒可以有好幾行）。
 *   清除（沒有 frame source、換 generation、離開 capture()）：有設著的各印一行、發預設快照；之後的第一行不受限速。
 * 只看得到發布當下的狀態：兩次發布之間變了又變回來的，這裡不知道。
 */
#pragma once

// standard includes
#include <cstdint>

// local includes
#include "vr_capture_health.h"

namespace vr::capture {

  class publisher_t {
  public:
    /// 一次呼叫之後呼叫端要做的事
    struct action_t {
      bool publish = false;  ///< 寫給其他執行緒看的快照（on_snapshot()：寫那一份；clear()：寫預設值）
      bool log_overload = false;  ///< 印 `overload on／off`（on_snapshot()：照那一份的 overload；clear()：off）
      bool log_cadence = false;  ///< 印 `cadence` 行（on_snapshot()：那一份的 cadence；clear()：unknown）
      uint32_t cadence_skipped = 0;  ///< 那一行的 skipped=（0＝不印這一欄）
      bool log_hold = false;  ///< 印 `hold` 行（on_snapshot()：那一份的兩個決定；clear()：兩個都是 0）
      uint32_t hold_skipped = 0;  ///< 那一行的 skipped=（0＝不印這一欄）
    };

    /**
     * @brief capture() 進來時呼叫一次：全部重來（目前給其他執行緒看的當成預設快照）。
     * @param mode config `vr_capture_overload`（`hold` 行只有 cadence 模式印）
     * @param ticks_per_s on_snapshot() 的 now 用的單位，一秒是多少（display_vr.cpp 給 QPC 的頻率）
     */
    void configure(overload_mode_e mode, int64_t ticks_per_s) {
      *this = publisher_t {};
      mode_ = mode;
      ticks_per_s_ = ticks_per_s;
    }

    /**
     * @brief 每次餵完健康判定（health_t::on_frame()／on_idle()）之後呼叫。
     * @param snap health_t::snapshot()
     * @param now 呼叫端的單調時鐘（單位見 configure()）
     * @return 沒有要發布時全部是 false：呼叫端什麼都不做
     */
    action_t on_snapshot(const snapshot_t &snap, int64_t now) {
      action_t a;
      const bool flags_same = snap.overload == overload_ && snap.latch_hold == latch_hold_ && snap.predict_hold == predict_hold_;
      if (flags_same && published_ && detail::elapsed(now, pub_at_) < ticks_per_s_) {
        return a;
      }
      a.publish = true;
      published_ = true;
      pub_at_ = now;
      if (snap.overload != overload_) {
        overload_ = snap.overload;
        a.log_overload = true;
      }
      abandon_ = snap.abandon_overload;

      if (snap.cadence != cadence_) {
        cadence_ = snap.cadence;
        bump(cadence_changes_);
      }
      if (cadence_ != cadence_logged_ && !limited(have_cadence_log_, cadence_log_at_, now)) {
        a.log_cadence = true;
        a.cadence_skipped = skipped(cadence_changes_);
        cadence_logged_ = cadence_;
        cadence_changes_ = 0;
        have_cadence_log_ = true;
        cadence_log_at_ = now;
      }

      if (snap.latch_hold != latch_hold_ || snap.predict_hold != predict_hold_) {
        latch_hold_ = snap.latch_hold;
        predict_hold_ = snap.predict_hold;
        if (mode_ == overload_mode_e::cadence) {
          bump(hold_changes_);
        }
      }
      // late 模式下兩個決定就是過載旗標，`overload on／off` 已經說了，不另外印
      if (mode_ == overload_mode_e::cadence && (latch_hold_ != latch_logged_ || predict_hold_ != predict_logged_) &&
          !limited(have_hold_log_, hold_log_at_, now)) {
        a.log_hold = true;
        a.hold_skipped = skipped(hold_changes_);
        latch_logged_ = latch_hold_;
        predict_logged_ = predict_hold_;
        hold_changes_ = 0;
        have_hold_log_ = true;
        hold_log_at_ = now;
      }
      return a;
    }

    /**
     * @brief 沒有 frame source、換 generation、離開 capture()：回到預設快照。已經是預設的話全部是 false。
     *        這裡印的行不受限速（它們帶的是清除的原因），之後的第一行也不受限速；沒印出來的變化不帶到下一個 generation。
     */
    action_t clear() {
      action_t a;
      if (overload_) {
        overload_ = false;
        a.log_overload = true;
      }
      if (latch_hold_ || predict_hold_) {
        latch_hold_ = false;
        predict_hold_ = false;
        if (mode_ == overload_mode_e::cadence) {
          bump(hold_changes_);
        }
      }
      if (mode_ == overload_mode_e::cadence && (latch_logged_ || predict_logged_ || hold_changes_ > 0)) {
        a.log_hold = true;
        a.hold_skipped = skipped(hold_changes_);
      }
      latch_logged_ = false;
      predict_logged_ = false;
      hold_changes_ = 0;
      have_hold_log_ = false;
      abandon_ = false;
      if (cadence_ != cadence_e::unknown) {
        cadence_ = cadence_e::unknown;
        bump(cadence_changes_);
      }
      if (cadence_logged_ != cadence_e::unknown || cadence_changes_ > 0) {
        a.log_cadence = true;
        a.cadence_skipped = skipped(cadence_changes_);
      }
      cadence_logged_ = cadence_e::unknown;
      cadence_changes_ = 0;
      have_cadence_log_ = false;
      if (published_) {
        published_ = false;
        a.publish = true;
      }
      return a;
    }

    /// 目前給其他執行緒看的不是預設快照
    bool published() const {
      return published_;
    }

    // 已發布的值（stream.cpp 現在讀到的；10 s log 印的就是這幾個）

    bool overload() const {
      return overload_;
    }

    bool latch_hold() const {
      return latch_hold_;
    }

    bool predict_hold() const {
      return predict_hold_;
    }

    bool abandon_overload() const {
      return abandon_;
    }

    cadence_e cadence() const {
      return cadence_;
    }

  private:
    /// 同一秒內已經印過一行
    bool limited(bool have, int64_t at, int64_t now) const {
      return have && detail::elapsed(now, at) < ticks_per_s_;
    }

    /// 這一行印的是最後一次變化之後的狀態：之前的 changes − 1 次沒有印出來
    static uint32_t skipped(uint32_t changes) {
      return changes > 0 ? changes - 1 : 0;
    }

    static void bump(uint32_t &v) {
      if (v < 0xFFFFFFFFu) {
        ++v;
      }
    }

    overload_mode_e mode_ = overload_mode_e::late;
    int64_t ticks_per_s_ = k_ns_per_s;

    bool published_ = false;
    int64_t pub_at_ = 0;  ///< 上一次發布的時刻
    bool overload_ = false;
    bool latch_hold_ = false;
    bool predict_hold_ = false;
    bool abandon_ = false;
    cadence_e cadence_ = cadence_e::unknown;

    // `cadence` 行
    cadence_e cadence_logged_ = cadence_e::unknown;  ///< 上一行印的狀態
    uint32_t cadence_changes_ = 0;  ///< 上一行之後已發布的狀態變了幾次
    bool have_cadence_log_ = false;
    int64_t cadence_log_at_ = 0;

    // `hold` 行（只有 cadence 模式）
    bool latch_logged_ = false;
    bool predict_logged_ = false;
    uint32_t hold_changes_ = 0;
    bool have_hold_log_ = false;
    int64_t hold_log_at_ = 0;
  };

}  // namespace vr::capture
