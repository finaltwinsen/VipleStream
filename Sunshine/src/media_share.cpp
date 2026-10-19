/**
 * @file src/media_share.cpp
 * @brief VipleStream 2.0 §VR-PLAYER（V3）：見 media_share.h。
 */
#include "media_share.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <system_error>

#include "config.h"
#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

namespace media_share {

  namespace {

    const char *const k_video_exts[] = {".mp4", ".m4v", ".mkv", ".webm", ".mov", ".avi", ".ts", ".m2ts", ".mts", ".flv", ".mpg", ".mpeg"};

    std::string lower(std::string s) {
      for (auto &c : s) {
        c = (char) std::tolower((unsigned char) c);
      }
      return s;
    }

    bool is_video(const fs::path &p) {
      const auto ext = lower(p.extension().string());
      return std::any_of(std::begin(k_video_exts), std::end(k_video_exts), [&](const char *e) {
        return ext == e;
      });
    }

    std::string to_utf8(const fs::path &p) {
      const auto u8 = p.u8string();
      return std::string(u8.begin(), u8.end());
    }

    fs::path from_utf8(const std::string &s) {
      return fs::path(std::u8string(s.begin(), s.end()));
    }

    // p 是不是 root 本身或在它底下（兩者都已正規化）
    bool within(const fs::path &root, const fs::path &p) {
      auto r = root.begin();
      auto q = p.begin();
      for (; r != root.end(); ++r, ++q) {
        if (q == p.end() || *r != *q) {
          // Windows 的磁碟代號大小寫可能不同；其他元件照原樣比
          return false;
        }
      }
      return true;
    }

    // 相對路徑不可以是絕對路徑、不可以有 ..（正規化之後還會再檢查一次，這裡先擋明顯的）
    bool safe_relative(const std::string &rel) {
      if (rel.empty()) {
        return true;
      }
      if (rel.find('\0') != std::string::npos || rel[0] == '/' || rel[0] == '\\' || rel.find(':') != std::string::npos) {
        return false;
      }
      std::string part;
      for (std::size_t i = 0; i <= rel.size(); i++) {
        if (i == rel.size() || rel[i] == '/' || rel[i] == '\\') {
          if (part == "..") {
            return false;
          }
          part.clear();
        } else {
          part += rel[i];
        }
      }
      return true;
    }

  }  // namespace

  std::vector<root_t> roots() {
    std::vector<root_t> out;
    if (!config::nvhttp.media_share) {
      return out;
    }
    if (config::nvhttp.media_dirs.empty()) {
      // 整台 PC。編號用磁碟機代號（A＝0…Z＝25），隨身碟插拔不會讓其他磁碟機的編號錯位
#ifdef _WIN32
      const DWORD mask = GetLogicalDrives();
      for (int d = 0; d < 26; d++) {
        if (!(mask & (1u << d))) {
          continue;
        }
        wchar_t rootw[] = {wchar_t(L'A' + d), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(rootw);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
          continue;  // 光碟、網路磁碟機（service 看不到使用者對應的網路磁碟機）、RAM disk 等略過
        }
        wchar_t label[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(rootw, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0)) {
          continue;  // 沒有插卡的讀卡機等
        }
        auto name = std::string(1, char('A' + d)) + ":";
        const auto lab = to_utf8(fs::path(label));
        if (!lab.empty()) {
          name += " " + lab;
        }
        out.push_back({d, name, fs::path(rootw), true});
      }
#else
      out.push_back({0, "/", fs::path("/"), true});
#endif
      return out;
    }
    int i = 0;
    for (const auto &dir : config::nvhttp.media_dirs) {
      std::error_code ec;
      const auto p = fs::canonical(from_utf8(dir), ec);
      if (!ec && fs::is_directory(p, ec)) {
        auto name = to_utf8(p.filename());
        if (name.empty()) {
          name = to_utf8(p);
        }
        out.push_back({i, name, p, false});
      }
      i++;  // 編號照設定裡的順序（不存在的也佔一個號，client 記住的編號才不會因為某個資料夾暫時不在而錯位）
    }
    return out;
  }

  bool enabled() {
    return !roots().empty();
  }

  bool resolve(int root, const std::string &relative, fs::path *out, std::string *error) {
    if (!safe_relative(relative)) {
      *error = "bad path";
      return false;
    }
    for (const auto &r : roots()) {
      if (r.index != root) {
        continue;
      }
      std::error_code ec;
      const auto p = fs::weakly_canonical(r.path / from_utf8(relative), ec);
      if (ec || !within(r.path, p)) {
        *error = "path outside the shared folder";
        return false;
      }
      if (!fs::exists(p, ec)) {
        *error = "not found";
        return false;
      }
      *out = p;
      return true;
    }
    *error = "unknown folder";
    return false;
  }

  bool list(int root, const std::string &relative, std::vector<entry_t> *out, std::string *error) {
    fs::path dir;
    if (!resolve(root, relative, &dir, error)) {
      return false;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      *error = "not a folder";
      return false;
    }
    std::vector<entry_t> dirs, files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      const auto name = to_utf8(it->path().filename());
      if (name.empty() || name[0] == '.' || name[0] == '$') {
        continue;  // 隱藏檔、Windows 的系統資料夾（$RECYCLE.BIN）
      }
#ifdef _WIN32
      // 隱藏或系統屬性的（System Volume Information、Documents and Settings 這類連結、pagefile 等）不列
      const DWORD attrs = GetFileAttributesW(it->path().c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        continue;
      }
#endif
      std::error_code ec2;
      const auto st = it->status(ec2);
      if (ec2) {
        continue;
      }
      std::int64_t mtime = 0;
      const auto ft = it->last_write_time(ec2);
      if (!ec2) {
        // file_clock 的起點各平台不同（MSVC 是 1601 年）：換成 system_clock 再取秒數
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
        mtime = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
      }
      if (fs::is_directory(st)) {
        dirs.push_back({name, true, 0, mtime});
      } else if (fs::is_regular_file(st) && is_video(it->path())) {
        files.push_back({name, false, (std::uint64_t) it->file_size(ec2), mtime});
      }
    }
    auto by_name = [](const entry_t &a, const entry_t &b) {
      return lower(a.name) < lower(b.name);
    };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    out->clear();
    out->insert(out->end(), dirs.begin(), dirs.end());
    out->insert(out->end(), files.begin(), files.end());
    return true;
  }

  std::string content_type(const fs::path &path) {
    const auto ext = lower(path.extension().string());
    if (ext == ".mp4" || ext == ".m4v" || ext == ".mov") {
      return "video/mp4";
    }
    if (ext == ".mkv") {
      return "video/x-matroska";
    }
    if (ext == ".webm") {
      return "video/webm";
    }
    if (ext == ".ts" || ext == ".m2ts" || ext == ".mts") {
      return "video/mp2t";
    }
    return "application/octet-stream";
  }

  bool parse_range(const std::string &header, std::uint64_t size, std::uint64_t max_len, std::uint64_t *begin, std::uint64_t *end) {
    if (size == 0) {
      return false;
    }
    std::uint64_t b = 0, e = size - 1;
    if (!header.empty()) {
      const std::string prefix = "bytes=";
      if (header.compare(0, prefix.size(), prefix) != 0 || header.find(',') != std::string::npos) {
        return false;
      }
      const auto spec = header.substr(prefix.size());
      const auto dash = spec.find('-');
      if (dash == std::string::npos) {
        return false;
      }
      const auto a = spec.substr(0, dash), z = spec.substr(dash + 1);
      auto num = [](const std::string &s, std::uint64_t *v) {
        if (s.empty() || s.size() > 19 || !std::all_of(s.begin(), s.end(), [](char c) {
              return c >= '0' && c <= '9';
            })) {
          return false;
        }
        *v = std::stoull(s);
        return true;
      };
      if (a.empty()) {
        std::uint64_t n = 0;  // bytes=-n：最後 n 個位元組
        if (!num(z, &n) || n == 0) {
          return false;
        }
        b = n >= size ? 0 : size - n;
      } else {
        if (!num(a, &b) || b >= size) {
          return false;
        }
        if (!z.empty()) {
          std::uint64_t zz = 0;
          if (!num(z, &zz) || zz < b) {
            return false;
          }
          e = std::min(zz, size - 1);
        }
      }
    }
    if (max_len > 0 && e - b + 1 > max_len) {
      e = b + max_len - 1;
    }
    *begin = b;
    *end = e;
    return true;
  }

  int self_test() {
    int failures = 0;
    auto check = [&](const char *name, bool ok) {
      if (!ok) {
        failures++;
        BOOST_LOG(error) << "[VIPLE-MEDIA] selftest FAIL "sv << name;
      }
    };
    std::uint64_t b = 0, e = 0;
    check("range-open", parse_range("bytes=100-", 1000, 0, &b, &e) && b == 100 && e == 999);
    check("range-capped", parse_range("bytes=100-", 1000, 64, &b, &e) && b == 100 && e == 163);
    check("range-closed", parse_range("bytes=10-19", 1000, 0, &b, &e) && b == 10 && e == 19);
    check("range-suffix", parse_range("bytes=-100", 1000, 0, &b, &e) && b == 900 && e == 999);
    check("range-none", parse_range("", 1000, 0, &b, &e) && b == 0 && e == 999);
    check("range-past-end", !parse_range("bytes=1000-", 1000, 0, &b, &e));
    check("range-multi", !parse_range("bytes=0-1,5-6", 1000, 0, &b, &e));
    check("range-garbage", !parse_range("bytes=x-", 1000, 0, &b, &e));
    check("path-dotdot", !safe_relative("a/../../b") && !safe_relative("..") && !safe_relative("/etc/passwd") &&
                           !safe_relative("C:/x") && !safe_relative("a\\..\\b"));
    check("path-ok", safe_relative("") && safe_relative("Movies/a.mkv") && safe_relative("a..b/c"));
    check("within", within(fs::path("/a/b"), fs::path("/a/b/c")) && within(fs::path("/a/b"), fs::path("/a/b")) &&
                      !within(fs::path("/a/b"), fs::path("/a/bc")) && !within(fs::path("/a/b"), fs::path("/a")));
    return failures;
  }

}  // namespace media_share
