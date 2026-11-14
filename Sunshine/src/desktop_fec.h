/**
 * @file src/desktop_fec.h
 * @brief 2D（非 VR）串流的自適應 FEC 百分比規則（純邏輯）。
 *
 * 只依賴標準函式庫，單元測試直接 include（tests/unit/test_desktop_fec.cpp）。
 *
 * 升：client 回報的一個 500 ms 視窗有缺包 → +10%，上限 50%。
 * 降：距最後一次丟包跡象滿 6 秒、連續 3 輪沒有缺包之後，每 500 ms −5%，降回設定的基準值（fec_percentage）。
 *
 * client 只在有缺包時才送 FEC status，「沒有缺包的一輪」幾乎都來自 ping tick（stream.cpp 的 abr_zero_loss_tick）。
 * 回降以前只掛在 FEC status 路徑上、實際走不到：一段幾秒的丟包之後 FEC 停在 30～50% 到場次結束。
 * VR 的 FEC 跟著救不回的幀調（stream.cpp 的 §VR-FEC-LOSS），不用這個檔。
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>

namespace desktop_fec {
  constexpr int k_step_up = 10;  ///< 一個視窗有缺包時加的百分比
  constexpr int k_cap = 50;  ///< 自適應 FEC 的上限
  constexpr int k_step_down = 5;  ///< 每次回降的百分比
  constexpr int k_quiet_s = 5;  ///< 距最後一次丟包跡象要超過這麼多整秒才回降（無條件捨去後比較，所以是滿 6 秒）
  constexpr int k_clean_rounds = 3;  ///< 回降前要連續幾輪沒有缺包
  constexpr std::chrono::milliseconds k_decay_every {500};  ///< 兩次回降的最短間隔

  /**
   * @brief 一個視窗有缺包：升一階後的百分比（已在上限時回 cur）。
   * @param cur 目前實際使用的百分比（還沒升過時＝base）
   * @param base 設定的基準值。基準比 k_cap 還高時以基準為上限——拿 k_cap 去夾會把 FEC「升」到基準以下。
   */
  constexpr int raised(int cur, int base) {
    return std::max(cur, std::min(cur + k_step_up, std::max(k_cap, base)));
  }

  /**
   * @brief 沒有缺包的一輪：該回降時回降一階後的百分比，否則回 cur。
   * @param cur 自適應值（0＝還沒升過、用的是基準值）
   * @param base 設定的基準值
   * @param since_loss 距最後一次丟包跡象
   * @param zero_loss_streak 連續幾輪沒有缺包（含這一輪）
   * @param since_decay 距上一次回降。FEC status 與 ping tick 兩條路徑各自 500 ms 一輪、會交錯，靠它維持每 500 ms 最多一階
   */
  inline int decayed(int cur, int base, std::chrono::steady_clock::duration since_loss, int zero_loss_streak,
                     std::chrono::steady_clock::duration since_decay) {
    if (cur == 0 || cur <= base) {
      return cur;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(since_loss).count() <= k_quiet_s || zero_loss_streak < k_clean_rounds ||
        since_decay < k_decay_every) {
      return cur;
    }
    return std::max(cur - k_step_down, base);
  }
}  // namespace desktop_fec
