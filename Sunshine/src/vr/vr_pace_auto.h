/**
 * @file src/vr/vr_pace_auto.h
 * @brief §VR-PACE-AUTO（2026-11-14）：攤開送（vr_pace_spread_pct）什麼時候該開，由線路的狀況自動決定。純邏輯、不依賴平台，
 *        stream.cpp 的 10 秒統計與單元測試共用。
 *
 * 攤開送把每一幀的封包平均攤開在幀週期的一部分時間內送，讓線路上幾毫秒的全斷只吃掉一幀的一小部分、FEC 補得回來。
 * 它適合的場景（2026-11-14 凌晨外地經 VPN 的同場對照，temp/launchertest/ml/venue_1113/pace_ab/report.md）：
 *   - 72 fps：丟包事件每 10 秒 8 → 3、連續重複 3 格以上少四到五成、碼率還能高 20～40%，延遲多 7～9 ms；
 *   - 144 fps：丟包事件 9.9 → 6.2、碼率目標高六成，延遲多 3.6 ms。
 * 不適合的場景：
 *   - 線路乾淨（沒有救不回的幀）：只多延遲，沒有好處；
 *   - 補包（§VR-LINK-REPAIR）在用的線路（區網）：幀中途 3 ms 的空檔會讓頭盔把還沒送出的封包報成缺的，host 會重複送；
 *   - 兩條以上的連線都在送影像：另一條本來就各送一份，短暫的空檔由它補。
 * 所以自動的條件是：只有一條連線在送、這 10 秒沒有收到任何補包請求、而且救不回的 block 持續出現才開；
 * 連續兩分鐘幾乎沒有救不回的 block 才關（開著時丟包本來就會變少，門檻要比開的門檻低很多，否則會來回切換——
 * 每切換一次，幀到齊的時刻移大半個幀週期，頭盔的顯示相位要十幾秒才追上）。
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace vr::pace {

  /// 目標攤開時間。實測 72 fps 攤 6.9 ms（50%）與 11.8 ms（85%）效果相同、延遲代價前者少 2.4 ms；144 fps 攤 5.9 ms（85%）也有效。
  /// 線路的短中斷多半是 1～12 ms，攤到 6～7 ms 就夠讓 FEC 補回大部分
  constexpr int64_t k_target_spread_us = 6500;
  constexpr int k_max_pct = 85;  ///< 幀尾和下一幀幀頭之間至少留 15% 的幀週期
  constexpr int k_min_pct = 20;

  /// 這個幀週期下要攤開幾 %（攤開時間約 k_target_spread_us，不超過 k_max_pct）。144 fps → 85、120 → 78、90 → 58、72 → 46、60 → 39
  inline int pct_for_period(int64_t period_us) {
    if (period_us <= 0) {
      return 0;
    }
    return (int) std::clamp<int64_t>(k_target_spread_us * 100 / period_us, k_min_pct, k_max_pct);
  }

  /// 自動開關。每 10 秒（一個統計視窗）餵一次
  struct auto_t {
    static constexpr uint32_t k_on_failed = 4;  ///< 一個視窗裡救不回的 block ≥ 這麼多＝這個視窗「有成串丟包」
    static constexpr int k_on_windows = 2;  ///< 連續這麼多個視窗都有才開（20 秒）
    static constexpr uint32_t k_off_failed = 1;  ///< 一個視窗裡救不回的 block ≤ 這麼多＝這個視窗「乾淨」
    static constexpr int k_off_windows = 12;  ///< 連續這麼多個視窗都乾淨才關（2 分鐘）

    bool on = false;
    int bad_run = 0;
    int clean_run = 0;
    uint32_t switches = 0;
    const char *reason = "";  ///< 最近一次切換的原因（log 用）

    /**
     * @param eligible 只有一條連線在送影像，而且這個視窗沒有收到補包請求
     * @param failed 這個視窗裡頭盔回報救不回的 block 數（含整幀沒到的）
     * @return 狀態有沒有變
     */
    bool on_window(bool eligible, uint32_t failed) {
      if (!eligible) {
        bad_run = 0;
        clean_run = 0;
        if (on) {
          on = false;
          ++switches;
          reason = "resend requests seen or more than one link";
          return true;
        }
        return false;
      }
      if (!on) {
        bad_run = failed >= k_on_failed ? bad_run + 1 : 0;
        if (bad_run >= k_on_windows) {
          on = true;
          bad_run = 0;
          clean_run = 0;
          ++switches;
          reason = "frames keep being lost on a single link without resends";
          return true;
        }
        return false;
      }
      clean_run = failed <= k_off_failed ? clean_run + 1 : 0;
      if (clean_run >= k_off_windows) {
        on = false;
        bad_run = 0;
        clean_run = 0;
        ++switches;
        reason = "the path has been clean for two minutes";
        return true;
      }
      return false;
    }
  };

}  // namespace vr::pace
