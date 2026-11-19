/**
 * @file src/platform/windows/stream_menu.h
 * @brief §STREAM-MENU：2D 串流期間，把 Windows 的電源選單換成串流控制選單。
 *
 * 為什麼：遠端使用者在開始選單按電源圖示、或在桌面按 Alt+F4，想做的多半是「離開這一場串流」，
 * 但 Windows 給的只有睡眠／關機／重新啟動——選了就把 host 關掉，之後連不回來。串流期間改成顯示
 * 自己的選單，把「中斷串流」「結束 App」放在最前面，原本的電源動作仍然保留。
 *
 * 只在「有串流 session、而且不是 VR session」時生效；設定 `stream_menu = disabled` 可整個關掉。
 * 任何一步認不出來（UIA 建不起來、逾時、Windows 改版）都當成沒攔到，輸入照原樣注入。
 */
#pragma once

// standard includes
#include <cstdint>

namespace platf::stream_menu {
  /// 第一個串流 session 開始（platf::streaming_will_start）。啟動背景執行緒、開始看「關閉 Windows」對話框。
  void session_started();

  /// 最後一個串流 session 結束（platf::streaming_will_stop）。停止攔截，還開著的選單一併關掉。
  void session_stopped();

  /**
   * @brief 遠端滑鼠按鍵注入前呼叫（platf::button_mouse）。
   * @param button 1＝左鍵（其他按鍵一律不處理）。
   * @param release true＝放開。
   * @return true＝這一下不要注入（按下時攔到開始選單的電源圖示，或是那一下對應的放開）。
   */
  bool on_mouse_button(int button, bool release);

  /**
   * @brief 遠端鍵盤注入前呼叫（platf::keyboard_update）。
   * @param vk Windows 虛擬鍵碼。
   * @param release true＝放開。
   * @return true＝這個按鍵不要注入（在桌面按 Alt+F4 的那個 F4，含重複與放開）。
   */
  bool on_key(std::uint16_t vk, bool release);
}  // namespace platf::stream_menu
