/**
 * @file src/fs_picker.h
 * @brief 跨平台 native file-open dialog 抽象（用於 server tray「Send to client」）。
 *
 * 平台實作：
 *   src/platform/windows/fs_picker.cpp  — IFileOpenDialog (COM)
 *   src/platform/linux/fs_picker.cpp    — zenity --file-selection subprocess
 *
 * 一律 blocking — 必須由 user session 的 thread 呼叫（譬如 tray callback）。
 * 沒選或被取消時回傳 std::nullopt。
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace fs_picker {

  struct open_options {
    std::string title;             ///< Window title
    std::string default_dir;       ///< 起始目錄 — 空字串代表 OS 預設
  };

  /**
   * 開啟「選擇一個現有檔」對話框。
   *
   * @return 使用者選擇的絕對路徑；nullopt 表示取消或失敗
   */
  std::optional<std::filesystem::path> pick_open_file(const open_options &opts);

}  // namespace fs_picker
