/**
 * @file src/vr/vr_abr_outage.h
 * @brief VipleStream 2.0 §VR-ABR-OUTAGE：VR 串流斷訊後把位元率直接拉回斷訊前的值。
 *
 * 2026-10-04 Frame 第六輪 c：適配器連線一次斷訊（500 ms 掉 2110 個封包），ABR 把它當成壅塞，每個視窗
 * 砍一半，一路砍到底（140 → 11 Mbps）；連線恢復後只能每 2.5 s 加 5% 慢慢爬，中間再閃幾下又被打回去，
 * 約 3 分 40 秒才第一次回到 129 Mbps、4 分鐘才穩定回到 140 Mbps，後半場又卡又糊。專用 Wi‑Fi 連線上的
 * 斷訊（熱點重建、短暫干擾）不是壅塞：恢復後連線容量和斷訊前一樣，慢慢爬只是白白損失畫質。
 *
 * 做法（只影響 VR session；斷訊期間照常降碼，反正封包也送不過去）：
 *   - 斷訊訊號：丟包比例 ≥ 30% 的 ABR 視窗，或 client 回報連掉 3 幀以上的 LOSS。事件開始時記下斷訊前的
 *     已套用位元率（若 1 s 內剛被別的視窗砍過，用砍之前的值）。
 *   - 拉回條件（全部成立才拉，每次斷訊只拉一次）：最後一個斷訊訊號之後已過 1 s、之後有 2 個乾淨視窗、
 *     影像確實在送達（client LATCH 的幀號 300 ms 內有前進；從沒收過 LATCH 的舊 client 不看這項）、
 *     而且參考值確實高於目前的 target（沒東西可拉時不消耗這次拉回）。
 *   - 拉回後 5 s 內又斷：只有「這次事件裡低位元率（≤ 參考值一半）時從沒斷過」才認定是拉回值太高，參考值打
 *     75 折；低位元率時也斷過＝連線閃斷，與位元率無關，不打折（審查重播：第六輪 c 在 18 Mbps 時一樣會斷）。
 *   - client 重送的 LOSS（RESEND）只延長事件，不開新事件、不打折。30 s 沒有斷訊訊號 → 事件結束。
 * 真的壅塞（位元率超過連線容量）時丟包比例通常在個位數，照原本的 AIMD 處理。
 *
 * 純函式、不碰 session／log，可單元測試；呼叫端（stream.cpp）持 abrMutex 呼叫並負責記 log。
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace vr::abr {

  using steady = std::chrono::steady_clock;

  constexpr float k_outage_loss_pct = 30.0f;  ///< 一個 ABR 視窗丟這麼多（%）以上算斷訊
  constexpr int k_restore_clean_windows = 2;  ///< 最後一個斷訊訊號之後要有幾個乾淨視窗
  constexpr auto k_restore_min_quiet = std::chrono::seconds(1);  ///< 最後一個斷訊訊號之後至少這麼久
  constexpr auto k_progress_fresh = std::chrono::milliseconds(300);  ///< LATCH 幀號前進要在這麼近以內
  constexpr auto k_restore_guard = std::chrono::seconds(5);  ///< 拉回後這段時間內又斷 → 可能打折
  constexpr auto k_episode_timeout = std::chrono::seconds(30);  ///< 這麼久沒有斷訊訊號 → 事件結束
  constexpr auto k_recent_cut = std::chrono::seconds(1);  ///< 事件開始前這段時間內的降碼，用降碼前的值當參考

  struct outage_state_t {
    // 事件（ref_kbps == 0＝不在事件中）
    int ref_kbps = 0;  ///< 要拉回的值
    steady::time_point last_outage {};
    steady::time_point restored_at {};  ///< 這次斷訊之後已拉回的時間；{}＝還沒拉回
    int clean_windows = 0;  ///< 最後一個斷訊訊號之後的乾淨視窗數
    int min_outage_applied = 0;  ///< 這次事件裡出現斷訊訊號時的最低已套用位元率
    // 事件外也一直維護
    std::uint32_t last_latch_frame = 0;
    steady::time_point last_progress {};  ///< 最近一次 LATCH 幀號前進；{}＝從沒收過 LATCH
    int last_cut_from_kbps = 0;  ///< 最近一次降碼前的已套用位元率
    steady::time_point last_cut_at {};
  };

  enum class outage_e {
    started,  ///< 新的斷訊事件（ref_kbps＝要拉回的值）
    extended,  ///< 同一個事件裡又一個斷訊訊號
    lowered,  ///< 拉回後很快又斷、且低位元率時沒斷過：參考值打 75 折
  };

  inline void end_episode(outage_state_t &s) {
    s.ref_kbps = 0;
    s.restored_at = {};
    s.clean_windows = 0;
    s.min_outage_applied = 0;
  }

  /// client LATCH（約 100 ms 一筆）：幀號前進＝影像確實在送達。
  inline void on_progress(outage_state_t &s, std::uint32_t frame_id, steady::time_point now) {
    if (frame_id != s.last_latch_frame || s.last_progress == steady::time_point {}) {
      s.last_latch_frame = frame_id;
      s.last_progress = now;
    }
  }

  /// ABR 決定降碼（target < applied）：記下降碼前的值。
  inline void note_cut(outage_state_t &s, int applied_before_kbps, steady::time_point now) {
    s.last_cut_from_kbps = applied_before_kbps;
    s.last_cut_at = now;
  }

  /// 斷訊訊號。applied_kbps＝這個視窗降碼「之前」的已套用位元率；resend＝client 重送的 LOSS（只延長事件）。
  inline outage_e on_outage(outage_state_t &s, int applied_kbps, steady::time_point now, bool resend = false) {
    if (resend) {
      if (s.ref_kbps != 0 && now - s.last_outage <= k_episode_timeout) {
        s.last_outage = now;
        s.clean_windows = 0;
      }
      return outage_e::extended;
    }
    outage_e r = outage_e::extended;
    const bool restored = s.restored_at != steady::time_point {};
    if (s.ref_kbps != 0 && restored && now - s.restored_at < k_restore_guard) {
      // 拉回後很快又斷。低位元率時也斷過＝閃斷，與位元率無關，不打折
      if (s.min_outage_applied == 0 || static_cast<long long>(s.min_outage_applied) * 2 > s.ref_kbps) {
        s.ref_kbps = std::max(s.ref_kbps * 3 / 4, 1);
        r = outage_e::lowered;
      }
    } else if (s.ref_kbps == 0 || now - s.last_outage > k_episode_timeout || restored) {
      // 新事件：沒有進行中的事件、上一個已逾時，或拉回後已穩定一段時間（這是另一次斷訊）
      int ref = applied_kbps;
      if (s.last_cut_from_kbps > ref && s.last_cut_at != steady::time_point {} && now - s.last_cut_at <= k_recent_cut) {
        ref = s.last_cut_from_kbps;  // 稀釋過的視窗先砍了一刀、斷訊訊號後到
      }
      end_episode(s);
      s.ref_kbps = std::max(ref, 1);
      r = outage_e::started;
    }
    s.min_outage_applied = s.min_outage_applied == 0 ? applied_kbps : std::min(s.min_outage_applied, applied_kbps);
    s.restored_at = {};
    s.last_outage = now;
    s.clean_windows = 0;
    return r;
  }

  /// 乾淨視窗（零丟包回升的分支）。@return 拉回後的 target；不拉回時回傳原本的 target。
  inline int on_clean(outage_state_t &s, int target, int max_kbps, steady::time_point now) {
    if (s.ref_kbps == 0) {
      return target;
    }
    if (now - s.last_outage > k_episode_timeout) {
      end_episode(s);
      return target;
    }
    ++s.clean_windows;
    if (s.restored_at != steady::time_point {} || s.clean_windows < k_restore_clean_windows || now - s.last_outage < k_restore_min_quiet) {
      return target;
    }
    if (s.last_progress != steady::time_point {} && now - s.last_progress > k_progress_fresh) {
      return target;  // 有 LATCH 的 client，影像卻沒在前進：連線可能還沒恢復
    }
    const int want = std::min(s.ref_kbps, max_kbps);
    if (want <= target) {
      return target;  // 沒有東西可拉：不消耗這次拉回
    }
    s.restored_at = now;
    return want;
  }

}  // namespace vr::abr
