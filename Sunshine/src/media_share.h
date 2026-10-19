/**
 * @file src/media_share.h
 * @brief VipleStream 2.0 §VR-PLAYER（V3）：把 host 上指定的資料夾分享給已配對的 client（頭盔影片模式）。
 *
 * 只開放 sunshine.conf 的 `media_dirs`（逗號分隔的資料夾清單；預設空＝整個功能關閉）裡的資料夾。所有路徑先正規化
 * （解開 . . 與符號連結）再確認還在某一個分享資料夾之內，跑出去的一律拒絕。端點在 nvhttp 的 HTTPS 伺服器上，
 * 只有配對過、而且沒被停用的 client 憑證連得進來（同 /launch）。
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace media_share {

  struct root_t {
    int index;
    std::string name;  // 顯示用：資料夾名稱（UTF-8）
    std::filesystem::path path;  // 正規化後的絕對路徑
  };

  struct entry_t {
    std::string name;  // UTF-8
    bool is_dir;
    std::uint64_t size;
    std::int64_t mtime;  // 秒（epoch）；拿不到為 0
  };

  /// 功能有沒有開（至少一個存在的分享資料夾）
  bool enabled();

  /// 存在的分享資料夾（設定裡不存在的略過）
  std::vector<root_t> roots();

  /**
   * @brief 把「分享資料夾編號＋相對路徑（UTF-8，用 / 分隔）」換成實際路徑。
   * @return false＝編號不對、路徑跑出分享資料夾、或不存在；*error 是英文原因
   */
  bool resolve(int root, const std::string &relative, std::filesystem::path *out, std::string *error);

  /// 資料夾內容：子資料夾與影片檔（隱藏檔略過），子資料夾在前，各自照名稱排序
  bool list(int root, const std::string &relative, std::vector<entry_t> *out, std::string *error);

  /// 依副檔名給 Content-Type
  std::string content_type(const std::filesystem::path &path);

  /**
   * @brief 解析 HTTP Range（只支援單一範圍 bytes=a-、bytes=a-b、bytes=-n），並把長度限制在 max_len 之內。
   * @return false＝格式不對或超出檔案（呼叫端回 416）；沒有 Range 時回 true、範圍從 0 開始
   */
  bool parse_range(const std::string &header, std::uint64_t size, std::uint64_t max_len, std::uint64_t *begin, std::uint64_t *end);

  /// 純邏輯的自測（路徑檢查與 Range 解析）；回傳失敗數
  int self_test();

}  // namespace media_share
