/**
 * @file src/platform/windows/ime_state.h
 * @brief §KBD-IME-STATE：依前景視窗的輸入法狀態切換中／英，不送快速鍵。
 */
#pragma once

namespace platf {
  /**
   * @brief 把前景視窗的輸入法在中文模式與英文模式之間切換。
   * @details 中文模式＝IME 開著且轉換模式含 IME_CMODE_NATIVE。對別的行程一律用
   *          SendMessageTimeout（單次最多 200 ms，整段約 500 ms 封頂）。每次呼叫印一行
   *          `[VIPLE-IME]`，只帶視窗類別名，不帶視窗標題。
   *          只在輸入執行緒呼叫（task_pool 那一條）；裡面會 Sleep 與等別的行程，呼叫端不能再巢狀等輸入。
   * @return true＝等 30 ms 讀回，中／英模式真的換了。false＝沒有輸入法視窗、逾時，或讀回沒變。
   */
  bool ime_toggle_by_state();
}  // namespace platf
