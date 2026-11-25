/**
 * @file src/platform/windows/console_session.h
 * @brief §CONSOLE-LOGIN：主控台與已中斷的使用者工作階段。
 *
 * 遠端桌面關掉視窗後，工作階段是 WTSDisconnected，主控台沒有使用者。
 * 以 SYSTEM 執行 tscon 可以把該工作階段接回主控台，但不鎖定的話實體螢幕會直接顯示桌面。
 */
#pragma once

#include <string>

#include <Windows.h>

namespace platf::console {
  inline constexpr DWORD k_no_session = 0xFFFFFFFFu;

  struct state_t {
    bool user_on_console = false;  ///< 主控台工作階段有使用者名稱
    bool locked = false;  ///< 主控台使用者的工作階段是鎖定的
    DWORD console_id = k_no_session;
    DWORD disconnected_id = k_no_session;  ///< 有使用者名稱的 WTSDisconnected；沒有就是 k_no_session
    std::string user;  ///< 主控台使用者；主控台沒人時改記要接回的那個使用者
  };

  /// 只查 WTS，不改工作階段。
  state_t query();

  /**
   * @brief 主控台沒有使用者、且有中斷的使用者工作階段時，把它接回主控台。
   * @param lock_after true：接上之後以該使用者身分鎖定。鎖定失敗就把工作階段中斷回去。
   *        寧可回到沒人的主控台，也不留未鎖定的桌面。
   * @param why 失敗時寫入原因（英文、不含密碼）。可為 nullptr。
   * @return 接回成功；要求鎖定時，鎖定也要成功。
   */
  bool reclaim_disconnected(bool lock_after, std::string *why);
}  // namespace platf::console
