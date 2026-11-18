/**
 * @file src/media_share.cpp
 * @brief VipleStream 2.0 §VR-PLAYER（V3）：見 media_share.h。§MEDIA-FILEOPS 的複製、刪除、移動也在這裡。
 */
#include "media_share.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <cerrno>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
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

  fs::path default_dir() {
#ifdef _WIN32
    // 「公用影片」（預設在 Public 使用者底下的 Videos）：每台 Windows 都有、每個使用者都能放檔案進去、服務帳號讀得到
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetEnvironmentVariableW(L"PUBLIC", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
      return {};
    }
    return fs::path(buf) / L"Videos";
#else
    const char *home = std::getenv("HOME");
    return home && *home ? fs::path(home) / "Videos" : fs::path {};
#endif
  }

  std::vector<root_t> roots() {
    std::vector<root_t> out;
    if (!config::nvhttp.media_share) {
      return out;
    }
    // §MEDIA-DEFAULT（2026-11-11，使用者定案）：沒設 media_dirs 時只開放一個預設資料夾，不再是整台 PC——
    // 配對過的 client 原本能讀到服務帳號讀得到的任何檔案。要開放別的資料夾或整台 PC，必須在 server 這邊設定：
    // media_dirs = <資料夾>,<資料夾>；整台 PC 是 media_dirs = *（單獨一個星號）。
    const bool whole_pc = config::nvhttp.media_dirs.size() == 1 && config::nvhttp.media_dirs[0] == "*";
    if (config::nvhttp.media_dirs.empty()) {
      std::error_code ec;
      const auto def = default_dir();
      const auto p = def.empty() ? fs::path {} : fs::canonical(def, ec);
      if (!def.empty() && !ec && fs::is_directory(p, ec)) {
        auto name = to_utf8(p.filename());
        out.push_back({0, name.empty() ? to_utf8(p) : name, p, false});
      }
      return out;
    }
    if (whole_pc) {
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

  // ── §MEDIA-FILEOPS ────────────────────────────────────────────────────────
  // 預設唯讀。寫入只允許「資料夾」根，而且路徑一定先經過 resolve。遞迴時不跟隨符號連結與
  // Windows 的 junction（reparse point）：刪除只刪連結本身，複製直接略過，避免順著連結跑出分享資料夾。

  op_result_t result_of(int status, std::string message, std::uint64_t job = 0) {
    op_result_t r;
    r.status = status;
    r.message = std::move(message);
    r.job = job;
    return r;
  }

  std::string one_line(std::string s) {
    for (auto &c : s) {
      if (c == '\r' || c == '\n') {
        c = ' ';
      }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) {
      // FormatMessage 的英文訊息結尾常帶空白與句點；留著句點沒關係，只去掉換行後剩下的空白
      if (s.back() == ' ') {
        s.pop_back();
      } else {
        break;
      }
    }
    if (s.empty()) {
      s = "unknown error";
    }
    return s;
  }

  std::string failed_text(std::string message) {
    return "failed: " + one_line(std::move(message));
  }

#ifdef _WIN32
  // 系統錯誤訊息要英文：client 拿去對翻譯，跟語系無關。英文語言包沒有時才退回系統語系。
  std::string win_failed(DWORD code) {
    auto load = [&](DWORD lang) -> std::string {
      wchar_t *buf = nullptr;
      const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
      const DWORD n = FormatMessageW(flags, nullptr, code, lang, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
      std::string msg;
      if (n != 0 && buf != nullptr) {
        std::wstring wide(buf, n);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
          wide.pop_back();
        }
        msg = to_utf8(fs::path(wide));
        LocalFree(buf);
      }
      return msg;
    };
    std::string msg = load(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
    if (msg.empty()) {
      msg = load(0);
    }
    if (msg.empty()) {
      msg = "win32 error " + std::to_string(code);
    }
    return failed_text(std::move(msg));
  }
#endif

  std::string failed_ec(const std::error_code &ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category()) {
      return win_failed(static_cast<DWORD>(ec.value()));
    }
#endif
    return failed_text(ec ? ec.message() : "unknown error");
  }

  // 檔案不存在。MSVC 多半轉成 errc，有的版本仍留 Win32 的 system_category，兩邊都認。
  bool is_absent(const std::error_code &ec) {
    if (ec == std::errc::no_such_file_or_directory) {
      return true;
    }
#ifdef _WIN32
    if (ec.category() == std::system_category() && (ec.value() == ERROR_FILE_NOT_FOUND || ec.value() == ERROR_PATH_NOT_FOUND)) {
      return true;
    }
#endif
    return false;
  }

#ifndef _WIN32
  std::string failed_errno(int err) {
    return failed_ec(std::error_code(err, std::generic_category()));
  }

  // §MEDIA-FILEOPS：Linux 的 bind mount 不是符號連結，directory_iterator 會走進去，可能離開分享資料夾。
  // 跟走訪起點不同的 st_dev 就不展開。查不到裝置時當成不安全。
  enum class mount_e { same, foreign, error };

  std::string capture_device(const fs::path &p, dev_t *out, bool *have) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
      *have = false;
      if (errno == ENOENT) {
        return {};
      }
      return failed_errno(errno);
    }
    *out = st.st_dev;
    *have = true;
    return {};
  }

  mount_e mount_relation(const fs::path &p, dev_t dev, std::string *err) {
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
      *err = errno == ENOENT ? std::string("not found") : failed_errno(errno);
      return mount_e::error;
    }
    return st.st_dev == dev ? mount_e::same : mount_e::foreign;
  }
#endif

  // 目的資料夾是來源資料夾自己或它的子資料夾。兩者都要是已經正規化的路徑。
  bool into_itself(const fs::path &source_dir, const fs::path &dest_dir) {
    return within(source_dir, dest_dir);
  }

  bool name_ok(const std::string &name) {
    if (name.empty() || name == "." || name == "..") {
      return false;
    }
    if (name.size() > 255) {
      return false;
    }
    // Windows 會悄悄拿掉結尾的空白與句點，實際檔名就跟檢查過的不一樣
    if (name.back() == ' ' || name.back() == '.') {
      return false;
    }
    const auto n = name.size();
    for (std::size_t i = 0; i < n;) {
      const auto c = static_cast<unsigned char>(name[i]);
      std::uint32_t cp = 0;
      std::size_t len = 0;
      if (c <= 0x7F) {
        cp = c;
        len = 1;
      } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        len = 2;
      } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        len = 3;
      } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        len = 4;
      } else {
        return false;
      }
      if (i + len > n) {
        return false;
      }
      for (std::size_t j = 1; j < len; j++) {
        const auto cc = static_cast<unsigned char>(name[i + j]);
        if ((cc & 0xC0) != 0x80) {
          return false;
        }
        cp = (cp << 6) | (cc & 0x3F);
      }
      // 過長編碼與代理項都不是合法 UTF-8，不能拿去當路徑
      if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
          (cp >= 0xD800 && cp <= 0xDFFF)) {
        return false;
      }
      if (cp <= 0x1F || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
        return false;
      }
      if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' || cp == '|') {
        return false;
      }
      i += len;
    }
    // CON、NUL 這類在 Windows 會開到裝置而不是檔案。Linux 上同樣拒絕，避免同一條協定兩邊行為不同。
    std::string base;
    for (unsigned char ch : name) {
      if (ch == '.') {
        break;
      }
      base.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
      return false;
    }
    if (base.size() == 4 && base[3] >= '1' && base[3] <= '9' && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0)) {
      return false;
    }
    return true;
  }

  // GetFileAttributesW 看得到 junction 的 FILE_ATTRIBUTE_REPARSE_POINT；std::filesystem::is_symlink 看不到。
  bool is_reparse_path(const fs::path &p) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && fs::is_symlink(st);
#endif
  }

  enum class kind_e {
    missing,
    reparse,
    dir,
    file,
    other,
    error,
  };

  struct kind_t {
    kind_e type = kind_e::error;
    std::uint64_t size = 0;
    std::string error;
  };

  kind_t classify(const fs::path &p) {
    kind_t k;
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      const DWORD e = GetLastError();
      if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
        k.type = kind_e::missing;
        return k;
      }
      k.error = win_failed(e);
      return k;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
      k.type = kind_e::reparse;
      return k;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
      k.type = kind_e::dir;
      return k;
    }
    WIN32_FILE_ATTRIBUTE_DATA info {};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &info)) {
      k.error = win_failed(GetLastError());
      return k;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      k.type = kind_e::reparse;
      return k;
    }
    k.type = kind_e::file;
    k.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return k;
#else
    struct stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
      if (errno == ENOENT) {
        k.type = kind_e::missing;
        return k;
      }
      k.error = failed_errno(errno);
      return k;
    }
    if (S_ISLNK(st.st_mode)) {
      k.type = kind_e::reparse;
      return k;
    }
    if (S_ISDIR(st.st_mode)) {
      k.type = kind_e::dir;
      return k;
    }
    if (S_ISREG(st.st_mode)) {
      if (st.st_size < 0) {
        k.error = "failed: bad file size";
        return k;
      }
      k.type = kind_e::file;
      k.size = static_cast<std::uint64_t>(st.st_size);
      return k;
    }
    k.type = kind_e::other;
    return k;
#endif
  }

  bool path_missing_or_error(const kind_t &k, std::string *err, bool missing_is_error) {
    if (k.type == kind_e::error) {
      *err = k.error.empty() ? "failed: unknown error" : k.error;
      return true;
    }
    if (k.type == kind_e::missing && missing_is_error) {
      *err = "not found";
      return true;
    }
    return false;
  }

  std::string list_children(const fs::path &dir, std::vector<fs::path> *out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
      return failed_ec(ec);
    }
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) {
        return failed_ec(ec);
      }
      const auto name = it->path().filename();
      if (name.empty() || name == "." || name == "..") {
        return "failed: bad child name";
      }
      out->push_back(it->path());
    }
    return {};
  }

  // 只刪這個項目。目錄必須已經是空的；reparse（含 junction）用目錄或檔案 API 刪連結本身，不走進目標。
  std::string remove_leaf(const fs::path &p) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      const DWORD e = GetLastError();
      if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
        return {};
      }
      return win_failed(e);
    }
    if ((attrs & FILE_ATTRIBUTE_READONLY) && !SetFileAttributesW(p.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
      return win_failed(GetLastError());
    }
    const BOOL ok = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str());
    if (!ok) {
      const DWORD e = GetLastError();
      if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
        return {};
      }
      return win_failed(e);
    }
    return {};
#else
    std::error_code ec;
    fs::remove(p, ec);
    if (ec && !is_absent(ec)) {
      return failed_ec(ec);
    }
    return {};
#endif
  }

  struct job_slot_t {
    std::uint64_t id = 0;
    std::string state;
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::string error;
    bool cancel = false;
    bool running = false;
  };

  struct runner_t {
    std::mutex mu;
    std::thread thr;
    job_slot_t job;
    std::uint64_t next_id = 1;

    runner_t() = default;
    runner_t(const runner_t &) = delete;
    runner_t &operator=(const runner_t &) = delete;

    ~runner_t() {
      std::thread old;
      {
        std::lock_guard<std::mutex> lk(mu);
        job.cancel = true;
        old = std::move(thr);
      }
      // 程序結束時把工作收掉，避免執行緒物件還 joinable 時解構直接 terminate
      if (old.joinable()) {
        old.join();
      }
    }
  };

  runner_t &runner() {
    static runner_t r;
    return r;
  }

  bool job_cancelled(std::uint64_t id) {
    auto &r = runner();
    std::lock_guard<std::mutex> lk(r.mu);
    return r.job.id == id && r.job.cancel;
  }

  bool job_running() {
    auto &r = runner();
    std::lock_guard<std::mutex> lk(r.mu);
    return r.job.running;
  }

  void job_set_total(std::uint64_t id, std::uint64_t n) {
    auto &r = runner();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.job.id == id) {
      r.job.total = n;
    }
  }

  void job_add_done(std::uint64_t id, std::uint64_t n) {
    auto &r = runner();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.job.id == id && n <= std::uint64_t(-1) - r.job.done) {
      r.job.done += n;
    }
  }

  void job_set_done(std::uint64_t id, std::uint64_t n) {
    auto &r = runner();
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.job.id == id) {
      r.job.done = n;
    }
  }

  void log_op(const std::string &op, int root, bool has_dest, int droot, const std::string &result) {
    const std::string shown = result.empty() ? std::string("ok") : result;
    if (has_dest) {
      BOOST_LOG(info) << "[VIPLE-MEDIA] op="sv << op << " root="sv << root << " droot="sv << droot << " result="sv << shown;
    } else {
      BOOST_LOG(info) << "[VIPLE-MEDIA] op="sv << op << " root="sv << root << " result="sv << shown;
    }
  }

  // 走訪時用自己的堆疊。分享資料夾可能被人刻意做得很深，遞迴會把 server 的堆疊撐爆。
#ifndef _WIN32
  struct walk_dev_t {
    dev_t dev = 0;
    bool have = false;
  };

  std::string load_walk_dev(const fs::path &p, walk_dev_t *out) {
    return capture_device(p, &out->dev, &out->have);
  }

  // 1＝別的裝置（不要走進去），0＝同一個，-1＝錯誤（*err 已寫好）
  int mount_kind(const fs::path &p, const walk_dev_t &w, std::string *err) {
    if (!w.have) {
      *err = "not found";
      return -1;
    }
    const auto rel = mount_relation(p, w.dev, err);
    if (rel == mount_e::error) {
      return -1;
    }
    return rel == mount_e::foreign ? 1 : 0;
  }
#endif

  std::string count_bytes(const fs::path &root, std::uint64_t id, std::uint64_t *total) {
    *total = 0;
#ifndef _WIN32
    walk_dev_t walk;
    if (const auto derr = load_walk_dev(root, &walk); !derr.empty()) {
      return derr;
    }
#endif
    std::vector<fs::path> stack;
    stack.push_back(root);
    while (!stack.empty()) {
      if (job_cancelled(id)) {
        return "cancelled";
      }
      const fs::path path = std::move(stack.back());
      stack.pop_back();
      const auto k = classify(path);
      std::string err;
      if (path_missing_or_error(k, &err, true)) {
        return err;
      }
      if (k.type == kind_e::reparse) {
        continue;  // 複製略過連結，大小也不算進去
      }
      if (k.type == kind_e::other) {
        return "failed: unsupported file type";
      }
      if (k.type == kind_e::file) {
        if (*total > std::uint64_t(-1) - k.size) {
          return "failed: size overflow";
        }
        *total += k.size;
        continue;
      }
#ifndef _WIN32
      {
        std::string merr;
        const int mk = mount_kind(path, walk, &merr);
        if (mk < 0) {
          return merr;
        }
        if (mk > 0) {
          continue;  // 掛載點：複製略過，跟連結一樣，不把別的檔案系統算進來
        }
      }
#endif
      if (is_reparse_path(path)) {
        return "failed: path changed during copy";
      }
      std::vector<fs::path> kids;
      err = list_children(path, &kids);
      if (!err.empty()) {
        return err;
      }
      if (is_reparse_path(path)) {
        return "failed: path changed during copy";
      }
#ifndef _WIN32
      {
        std::string merr;
        const int mk = mount_kind(path, walk, &merr);
        if (mk != 0) {
          return mk < 0 ? merr : std::string("failed: path changed during copy");
        }
      }
#endif
      for (auto &c : kids) {
        stack.push_back(std::move(c));
      }
    }
    return {};
  }

  std::string count_items(const fs::path &root, std::uint64_t id, std::uint64_t *total) {
    *total = 0;
#ifndef _WIN32
    walk_dev_t walk;
    if (const auto derr = load_walk_dev(root, &walk); !derr.empty()) {
      return derr;
    }
#endif
    std::vector<fs::path> stack;
    stack.push_back(root);
    while (!stack.empty()) {
      if (job_cancelled(id)) {
        return "cancelled";
      }
      const fs::path path = std::move(stack.back());
      stack.pop_back();
      const auto k = classify(path);
      std::string err;
      if (path_missing_or_error(k, &err, true)) {
        return err;
      }
      if (*total == std::uint64_t(-1)) {
        return "failed: size overflow";
      }
      *total += 1;
      if (k.type != kind_e::dir) {
        continue;  // 檔案、連結、其他項目都算 1，不往下走
      }
#ifndef _WIN32
      {
        std::string merr;
        const int mk = mount_kind(path, walk, &merr);
        if (mk < 0) {
          return merr;
        }
        if (mk > 0) {
          continue;  // 掛載點只算自己，不把別的檔案系統的項目算進來
        }
      }
#endif
      if (is_reparse_path(path)) {
        return "failed: path changed during delete";
      }
      std::vector<fs::path> kids;
      err = list_children(path, &kids);
      if (!err.empty()) {
        return err;
      }
      if (is_reparse_path(path)) {
        return "failed: path changed during delete";
      }
#ifndef _WIN32
      {
        std::string merr;
        const int mk = mount_kind(path, walk, &merr);
        if (mk != 0) {
          return mk < 0 ? merr : std::string("failed: path changed during delete");
        }
      }
#endif
      for (auto &c : kids) {
        stack.push_back(std::move(c));
      }
    }
    return {};
  }

  // honor_cancel＝false 用在收尾：取消之後還是要把已經建立的目的項目刪乾淨。
  std::string erase_tree(const fs::path &root, std::uint64_t id, bool honor_cancel, bool count_progress) {
    struct frame_t {
      fs::path path;
      bool expanded;
    };
#ifndef _WIN32
    walk_dev_t walk;
    if (const auto derr = load_walk_dev(root, &walk); !derr.empty()) {
      return derr;
    }
#endif
    std::vector<frame_t> stack;
    stack.push_back(frame_t {root, false});
    while (!stack.empty()) {
      if (honor_cancel && job_cancelled(id)) {
        return "cancelled";
      }
      if (!stack.back().expanded) {
        stack.back().expanded = true;
        const fs::path path = stack.back().path;
        const auto k = classify(path);
        if (k.type == kind_e::error) {
          return k.error.empty() ? "failed: unknown error" : k.error;
        }
        if (k.type == kind_e::missing) {
          if (count_progress) {
            job_add_done(id, 1);
          }
          stack.pop_back();
          continue;
        }
        bool descend = k.type == kind_e::dir;
#ifndef _WIN32
        if (descend) {
          std::string merr;
          const int mk = mount_kind(path, walk, &merr);
          if (mk < 0) {
            return merr;
          }
          if (mk > 0) {
            descend = false;  // 掛載點只刪掛載點本身，不刪裡面（多半會因為忙碌而失敗）
          }
        }
#endif
        if (descend) {
          if (is_reparse_path(path)) {
            return "failed: path changed during delete";
          }
          std::vector<fs::path> kids;
          const auto err = list_children(path, &kids);
          if (!err.empty()) {
            return err;
          }
          if (is_reparse_path(path)) {
            return "failed: path changed during delete";
          }
#ifndef _WIN32
          {
            std::string merr;
            const int mk = mount_kind(path, walk, &merr);
            if (mk != 0) {
              return mk < 0 ? merr : std::string("failed: path changed during delete");
            }
          }
#endif
          for (auto &c : kids) {
            stack.push_back(frame_t {std::move(c), false});
          }
          continue;
        }
        const auto err = remove_leaf(path);
        if (!err.empty()) {
          return err;
        }
        if (count_progress) {
          job_add_done(id, 1);
        }
        stack.pop_back();
        continue;
      }
      const fs::path path = stack.back().path;
      const auto err = remove_leaf(path);
      if (!err.empty()) {
        return err;
      }
      if (count_progress) {
        job_add_done(id, 1);
      }
      stack.pop_back();
    }
    return {};
  }

  std::string make_dir_new(const fs::path &p) {
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    if (!ec && fs::exists(st)) {
      return "exists";
    }
    if (ec && !is_absent(ec)) {
      return failed_ec(ec);
    }
    ec.clear();
    if (!fs::create_directory(p, ec)) {
      std::error_code ec2;
      const auto again = fs::symlink_status(p, ec2);
      if (!ec2 && fs::exists(again)) {
        return "exists";
      }
      return failed_ec(ec ? ec : ec2);
    }
    if (is_reparse_path(p)) {
      return "failed: created path is a link";
    }
    return {};
  }

#ifdef _WIN32
  struct handle_t {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit handle_t(HANDLE x):
        h(x) {}
    ~handle_t() {
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
      }
    }
    handle_t(const handle_t &) = delete;
    handle_t &operator=(const handle_t &) = delete;
    explicit operator bool() const {
      return h != INVALID_HANDLE_VALUE;
    }
  };

  bool handle_is_reparse(HANDLE h) {
    FILE_ATTRIBUTE_TAG_INFO tag {};
    if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
      return true;  // 查不到就當成不安全，呼叫端會放棄
    }
    return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  }
#else
  struct fd_t {
    int fd = -1;
    explicit fd_t(int x):
        fd(x) {}
    ~fd_t() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    fd_t(const fd_t &) = delete;
    fd_t &operator=(const fd_t &) = delete;
  };
#endif

  // 開檔不跟隨連結。建立目的檔用「已存在就失敗」，避免蓋掉別人的檔。*created 在目的檔真的建出來之後才是 true。
  std::string copy_file(const fs::path &src, const fs::path &dst, std::uint64_t id, bool *created) {
    *created = false;
    // 不能用 DWORD：這個變數在 Windows／Linux 兩邊都要編，DWORD 只有 Windows 有
    constexpr std::size_t k_chunk = 4u * 1024u * 1024u;
    std::vector<char> buf(k_chunk);
#ifdef _WIN32
    handle_t in(CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!in) {
      return win_failed(GetLastError());
    }
    if (handle_is_reparse(in.h) || GetFileType(in.h) != FILE_TYPE_DISK) {
      return "failed: path changed during copy";
    }
    handle_t out(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!out) {
      const DWORD e = GetLastError();
      if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) {
        return "exists";
      }
      return win_failed(e);
    }
    *created = true;
    if (handle_is_reparse(out.h) || GetFileType(out.h) != FILE_TYPE_DISK) {
      return "failed: path changed during copy";
    }
    for (;;) {
      if (job_cancelled(id)) {
        return "cancelled";
      }
      DWORD n = 0;
      if (!ReadFile(in.h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
        return win_failed(GetLastError());
      }
      if (n == 0) {
        break;
      }
      DWORD off = 0;
      while (off < n) {
        DWORD w = 0;
        if (!WriteFile(out.h, buf.data() + off, n - off, &w, nullptr) || w == 0) {
          return win_failed(GetLastError());
        }
        off += w;
      }
      job_add_done(id, n);
    }
    return {};
#else
    const int raw_in = ::open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (raw_in < 0) {
      if (errno == ELOOP) {
        return "failed: path changed during copy";
      }
      return failed_errno(errno);
    }
    fd_t in(raw_in);
    struct stat st {};
    if (::fstat(in.fd, &st) != 0) {
      return failed_errno(errno);
    }
    if (!S_ISREG(st.st_mode)) {
      return "failed: path changed during copy";
    }
    const int raw_out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (raw_out < 0) {
      // 目的名稱已經在了。O_NOFOLLOW 碰到連結回的是 ELOOP，不是 EEXIST，一樣不能蓋
      if (errno == EEXIST || errno == ELOOP) {
        return "exists";
      }
      return failed_errno(errno);
    }
    fd_t out(raw_out);
    *created = true;
    for (;;) {
      if (job_cancelled(id)) {
        return "cancelled";
      }
      const ssize_t n = ::read(in.fd, buf.data(), buf.size());
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return failed_errno(errno);
      }
      if (n == 0) {
        break;
      }
      std::size_t off = 0;
      while (off < static_cast<std::size_t>(n)) {
        const ssize_t w = ::write(out.fd, buf.data() + off, static_cast<std::size_t>(n) - off);
        if (w < 0) {
          if (errno == EINTR) {
            continue;
          }
          return failed_errno(errno);
        }
        if (w == 0) {
          return "failed: short write";
        }
        off += static_cast<std::size_t>(w);
      }
      job_add_done(id, static_cast<std::uint64_t>(n));
    }
    return {};
#endif
  }

  std::string copy_tree(const fs::path &src_root, const fs::path &dst_root, std::uint64_t id, bool *created) {
    *created = false;
#ifndef _WIN32
    walk_dev_t walk;
    if (const auto derr = load_walk_dev(src_root, &walk); !derr.empty()) {
      return derr;
    }
#endif
    struct frame_t {
      fs::path src;
      fs::path dst;
      bool expanded;
    };
    std::vector<frame_t> stack;
    stack.push_back(frame_t {src_root, dst_root, false});
    bool top = true;
    while (!stack.empty()) {
      if (job_cancelled(id)) {
        return "cancelled";
      }
      if (!stack.back().expanded) {
        stack.back().expanded = true;
        const bool is_top = top;
        top = false;
        const fs::path src = stack.back().src;
        const fs::path dst = stack.back().dst;
        const auto k = classify(src);
        std::string err;
        if (path_missing_or_error(k, &err, true)) {
          return err;
        }
        if (k.type == kind_e::reparse) {
          stack.pop_back();  // 複製略過連結本身
          continue;
        }
        if (k.type == kind_e::other) {
          return "failed: unsupported file type";
        }
        if (k.type == kind_e::dir) {
#ifndef _WIN32
          {
            std::string merr;
            const int mk = mount_kind(src, walk, &merr);
            if (mk < 0) {
              return merr;
            }
            if (mk > 0) {
              stack.pop_back();  // 掛載點略過：不建立目的、也不複製裡面
              continue;
            }
          }
#endif
          err = make_dir_new(dst);
          if (!err.empty()) {
            return err;
          }
          if (is_top) {
            *created = true;
          }
          if (is_reparse_path(src)) {
            return "failed: path changed during copy";
          }
          std::vector<fs::path> kids;
          err = list_children(src, &kids);
          if (!err.empty()) {
            return err;
          }
          if (is_reparse_path(src)) {
            return "failed: path changed during copy";
          }
#ifndef _WIN32
          {
            std::string merr;
            const int mk = mount_kind(src, walk, &merr);
            if (mk != 0) {
              return mk < 0 ? merr : std::string("failed: path changed during copy");
            }
          }
#endif
          for (auto &c : kids) {
            stack.push_back(frame_t {c, dst / c.filename(), false});
          }
          continue;
        }
        bool file_created = false;
        err = copy_file(src, dst, id, &file_created);
        if (is_top && file_created) {
          *created = true;
        }
        if (!err.empty()) {
          return err;
        }
        stack.pop_back();
        continue;
      }
      stack.pop_back();
    }
    return {};
  }

  bool cross_device(const std::error_code &ec) {
    if (ec == std::errc::cross_device_link) {
      return true;
    }
#ifdef _WIN32
    // generic_category 的 17 在 Windows 是 EEXIST，不是 ERROR_NOT_SAME_DEVICE。只認 system_category。
    if (ec.category() == std::system_category() && ec.value() == ERROR_NOT_SAME_DEVICE) {
      return true;
    }
#endif
    return false;
  }

  std::string no_space(const fs::path &dest_dir, std::uint64_t bytes) {
    std::error_code ec;
    const auto sp = fs::space(dest_dir, ec);
    if (ec) {
      return failed_ec(ec);
    }
    if (static_cast<std::uintmax_t>(bytes) > sp.available) {
      return "no space";
    }
    return {};
  }

  std::string run_op(std::uint64_t id, const std::string &op, const fs::path &src, const fs::path &dst, bool src_is_link, bool *need_cleanup, fs::path *cleanup) {
    *need_cleanup = false;
    if (op == "delete") {
      std::uint64_t n = 0;
      auto err = count_items(src, id, &n);
      if (!err.empty()) {
        return err;
      }
      job_set_total(id, n);
      return erase_tree(src, id, true, true);
    }
    std::uint64_t n = 0;
    auto err = count_bytes(src, id, &n);
    if (!err.empty()) {
      return err;
    }
    job_set_total(id, n);
    if (job_cancelled(id)) {
      return "cancelled";
    }
    if (op == "move") {
      std::error_code ec;
      fs::rename(src, dst, ec);
      if (!ec) {
        job_set_done(id, n);
        return {};
      }
      // 目的地在我們檢查之後被建出來：跟同步的 409 用同一個字，不覆蓋
      if (ec == std::errc::file_exists) {
        return "exists";
      }
#ifdef _WIN32
      if (ec.category() == std::system_category() && (ec.value() == ERROR_ALREADY_EXISTS || ec.value() == ERROR_FILE_EXISTS)) {
        return "exists";
      }
#endif
      if (!cross_device(ec)) {
        return failed_ec(ec);
      }
      // 跨磁碟區才複製再刪。連結沒辦法這樣搬：複製會略過它，接著刪來源就會把連結弄丟。
      if (src_is_link || is_reparse_path(src)) {
        return "failed: cannot move a link across volumes";
      }
    }
    // 空間檢查放在工作執行緒、真正複製之前：HTTPS 只有一條執行緒，受理時不能把整棵樹走完。
    // 失敗字串仍是 "no space"，由 opstatus 的 Error 送出（State=failed）。
    err = no_space(dst.parent_path(), n);
    if (!err.empty()) {
      return err;
    }
    bool created = false;
    try {
      err = copy_tree(src, dst, id, &created);
    } catch (...) {
      // 例外發生時目的資料夾可能已經建出來；旗標寫進呼叫端，外層才清得掉
      if (created) {
        *need_cleanup = true;
        *cleanup = dst;
      }
      throw;
    }
    if (created && !err.empty()) {
      *need_cleanup = true;
      *cleanup = dst;
    }
    if (!err.empty()) {
      return err;
    }
    if (op == "move") {
      // 複製已經完成，這裡不再看取消：停在半路會同時留下目的與刪到一半的來源
      return erase_tree(src, id, false, false);
    }
    return {};
  }

  void finish_job(std::uint64_t id, const std::string &op, int root, int droot, bool has_dest, const std::string &result) {
    {
      auto &r = runner();
      std::lock_guard<std::mutex> lk(r.mu);
      if (r.job.id != id) {
        return;
      }
      if (result.empty()) {
        r.job.state = "done";
        r.job.error.clear();
      } else if (result == "cancelled") {
        r.job.state = "cancelled";
        r.job.error.clear();
      } else {
        r.job.state = "failed";
        r.job.error = result;
      }
      r.job.running = false;
      r.job.cancel = false;
    }
    log_op(op, root, has_dest, droot, result.empty() ? "ok" : result);
  }

  void perform_job(std::uint64_t id, std::string op, int root_index, int droot, bool has_dest, fs::path src, fs::path dst, bool src_is_link) {
    bool need_cleanup = false;
    fs::path cleanup;
    std::string result;
    try {
      result = run_op(id, op, src, dst, src_is_link, &need_cleanup, &cleanup);
    } catch (const std::exception &ex) {
      result = failed_text(ex.what());
    } catch (...) {
      result = "failed: unknown error";
    }
    if (need_cleanup && !result.empty()) {
      try {
        const auto left = erase_tree(cleanup, id, false, false);
        if (!left.empty()) {
          BOOST_LOG(warning) << "[VIPLE-MEDIA] op="sv << op << " root="sv << root_index << " cleanup failed"sv;
        }
      } catch (...) {
        BOOST_LOG(warning) << "[VIPLE-MEDIA] op="sv << op << " root="sv << root_index << " cleanup failed"sv;
      }
    }
    finish_job(id, op, root_index, droot, has_dest, result);
  }

  std::vector<std::string> split_rel(const std::string &rel) {
    std::vector<std::string> parts;
    std::string part;
    for (std::size_t i = 0; i <= rel.size(); i++) {
      if (i == rel.size() || rel[i] == '/' || rel[i] == '\\') {
        if (!part.empty() && part != ".") {
          parts.push_back(part);
        }
        part.clear();
      } else {
        part.push_back(rel[i]);
      }
    }
    return parts;
  }

  struct located_t {
    fs::path node;
    bool is_link = false;
    bool ok = false;
    op_result_t err;
  };

  // resolve 通過之後才用。最後一個元件若是連結，回傳連結自己（不回目標）；中間的連結不穿越。
  located_t locate_source(const root_t &root, const std::string &relative) {
    located_t out;
    fs::path resolved;
    std::string e;
    if (!resolve(root.index, relative, &resolved, &e)) {
      out.err = result_of(404, "not found");
      return out;
    }
    fs::path direct = root.path;
    if (!relative.empty()) {
      const auto parts = split_rel(relative);
      for (std::size_t i = 0; i < parts.size(); i++) {
        if (parts[i] == "..") {
          out.err = result_of(404, "not found");
          return out;
        }
        direct /= from_utf8(parts[i]);
        if (!within(root.path, direct)) {
          out.err = result_of(404, "not found");
          return out;
        }
        if (is_reparse_path(direct)) {
          if (i + 1 != parts.size()) {
            out.err = result_of(404, "not found");
            return out;
          }
          out.node = direct;
          out.is_link = true;
          out.ok = true;
          return out;
        }
      }
    }
    out.node = resolved;
    out.ok = true;
    return out;
  }

  bool load_root(int index, root_t *out) {
    for (const auto &r : roots()) {
      if (r.index == index) {
        *out = r;
        return true;
      }
    }
    return false;
  }

  // media_write 關著時不區分根編號對不對，避免用錯誤碼探詢。
  op_result_t reject_unwritable(int index, root_t *out) {
    if (!config::nvhttp.media_write) {
      return result_of(403, "read-only");
    }
    if (!load_root(index, out)) {
      return result_of(404, "not found");
    }
    if (!root_writable(*out)) {
      return result_of(403, "read-only");
    }
    return result_of(200, {});
  }

  bool claim_job(std::uint64_t *id, op_result_t *err) {
    auto &r = runner();
    std::thread stale;
    {
      std::lock_guard<std::mutex> lk(r.mu);
      if (r.job.running) {
        *err = result_of(409, "busy");
        return false;
      }
      stale = std::move(r.thr);
    }
    // 上一條已經跑完，detach 免得在 HTTPS 那條執行緒上 join
    if (stale.joinable()) {
      stale.detach();
    }
    std::lock_guard<std::mutex> lk(r.mu);
    if (r.job.running) {
      *err = result_of(409, "busy");
      return false;
    }
    const auto new_id = r.next_id++;
    if (r.next_id == 0) {
      r.next_id = 1;
    }
    r.job = job_slot_t {};
    r.job.id = new_id;
    r.job.state = "running";
    r.job.running = true;
    *id = new_id;
    return true;
  }

  op_result_t logged(const std::string &op, int root, bool has_dest, int droot, op_result_t r) {
    log_op(op, root, has_dest, droot, r.status == 200 ? "ok" : r.message);
    return r;
  }

  std::string child_name_of(const fs::path &node) {
    return to_utf8(node.filename());
  }

  // 父資料夾或目的資料夾：用 resolve 的結果（已經在分享根裡），再確認它是真的資料夾而不是連結。
  bool locate_dir(int index, const std::string &rel, fs::path *out, op_result_t *err) {
    std::string e;
    if (!resolve(index, rel, out, &e)) {
      *err = result_of(404, "not found");
      return false;
    }
    const auto k = classify(*out);
    if (k.type == kind_e::error) {
      *err = result_of(500, k.error.empty() ? std::string("failed: unknown error") : k.error);
      return false;
    }
    if (k.type != kind_e::dir) {
      *err = result_of(404, "not found");
      return false;
    }
    return true;
  }

  // 空字串＝不存在；"exists"＝已有同名；其他是 failed:
  std::string occupant(const fs::path &p) {
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    if (ec) {
      if (is_absent(ec)) {
        return {};
      }
      return failed_ec(ec);
    }
    if (fs::exists(st)) {
      return "exists";
    }
    return {};
  }

  bool root_writable(const root_t &root) {
  // 磁碟機根（整台 PC）不開放寫入：範圍太大，而且拔插會讓編號對到別的槽
  return config::nvhttp.media_write && !root.drive;
}

bool valid_name(const std::string &name) {
  return name_ok(name);
}

op_result_t make_directory(int root, const std::string &parent, const std::string &name) {
  if (!name_ok(name)) {
    return logged("mkdir", root, false, 0, result_of(400, "bad name"));
  }
  root_t folder;
  const auto gate = reject_unwritable(root, &folder);
  if (gate.status != 200) {
    return logged("mkdir", root, false, 0, gate);
  }
  if (job_running()) {
    return logged("mkdir", root, false, 0, result_of(409, "busy"));
  }
  fs::path dir;
  op_result_t err;
  if (!locate_dir(root, parent, &dir, &err)) {
    return logged("mkdir", root, false, 0, err);
  }
  const fs::path dest = dir / from_utf8(name);
  if (!within(folder.path, dest)) {
    return logged("mkdir", root, false, 0, result_of(404, "not found"));
  }
  const auto occ = occupant(dest);
  if (!occ.empty()) {
    const int code = occ == "exists" ? 409 : 500;
    return logged("mkdir", root, false, 0, result_of(code, occ));
  }
  auto made = make_dir_new(dest);
  if (!made.empty()) {
    // 只有「我們剛建成的東西其實是連結」才刪。其他失敗可能根本沒建出來，不能亂刪已存在的項目。
    if (made == "failed: created path is a link") {
      remove_leaf(dest);
    }
    const int code = made == "exists" ? 409 : 500;
    return logged("mkdir", root, false, 0, result_of(code, made));
  }
  return logged("mkdir", root, false, 0, result_of(200, {}));
}

op_result_t start_background(const std::string &op, int root, const std::string &path, int droot, const std::string &dpath, bool has_dest) {
  const bool is_delete = op == "delete" && !has_dest;
  const bool is_copy = op == "copy" && has_dest;
  const bool is_move = op == "move" && has_dest;
  if (!is_delete && !is_copy && !is_move) {
    return logged(op == "delete" || op == "copy" || op == "move" ? op : "unknown", root, has_dest, droot, result_of(400, "bad request"));
  }
  // 刪除或移動分享根自己會把整個根拿掉，沒有上層可以留在分享範圍裡
  if ((is_delete || is_move) && path.empty()) {
    return logged(op, root, has_dest, droot, result_of(400, "bad request"));
  }
  root_t src_root;
  const auto gate = reject_unwritable(root, &src_root);
  if (gate.status != 200) {
    return logged(op, root, has_dest, droot, gate);
  }
  root_t dst_root;
  if (has_dest) {
    const auto gate_dst = reject_unwritable(droot, &dst_root);
    if (gate_dst.status != 200) {
      return logged(op, root, true, droot, gate_dst);
    }
  }
  const auto located = locate_source(src_root, path);
  if (!located.ok) {
    return logged(op, root, has_dest, droot, located.err);
  }
  if ((is_delete || is_move) && located.node == src_root.path) {
    return logged(op, root, has_dest, droot, result_of(400, "bad request"));
  }
  fs::path dest_final;
  if (has_dest) {
    fs::path dest_dir;
    op_result_t err;
    if (!locate_dir(droot, dpath, &dest_dir, &err)) {
      return logged(op, root, true, droot, err);
    }
    const auto leaf = child_name_of(located.node);
    if (leaf.empty() || leaf == "." || leaf == "..") {
      return logged(op, root, true, droot, result_of(400, "bad name"));
    }
    dest_final = dest_dir / located.node.filename();
    if (!within(dst_root.path, dest_final)) {
      return logged(op, root, true, droot, result_of(404, "not found"));
    }
    if (!located.is_link) {
      const auto k = classify(located.node);
      if (k.type == kind_e::error) {
        return logged(op, root, true, droot, result_of(500, k.error.empty() ? std::string("failed: unknown error") : k.error));
      }
      if (k.type == kind_e::dir && into_itself(located.node, dest_dir)) {
        return logged(op, root, true, droot, result_of(409, "into itself"));
      }
    }
    const auto occ = occupant(dest_final);
    if (!occ.empty()) {
      const int code = occ == "exists" ? 409 : 500;
      return logged(op, root, true, droot, result_of(code, occ));
    }
  }
  if (job_running()) {
    return logged(op, root, has_dest, droot, result_of(409, "busy"));
  }
  std::uint64_t id = 0;
  op_result_t claim_err;
  if (!claim_job(&id, &claim_err)) {
    return logged(op, root, has_dest, droot, claim_err);
  }
  try {
    std::thread th([id, op, root, droot, has_dest, src = located.node, dest_final, link = located.is_link]() {
      perform_job(id, op, root, droot, has_dest, src, dest_final, link);
    });
    auto &run = runner();
    std::lock_guard<std::mutex> lk(run.mu);
    run.thr = std::move(th);
  } catch (const std::exception &ex) {
    const auto msg = failed_text(ex.what());
    finish_job(id, op, root, droot, has_dest, msg);
    return result_of(500, msg);
  } catch (...) {
    finish_job(id, op, root, droot, has_dest, "failed: unknown error");
    return result_of(500, "failed: unknown error");
  }
  return result_of(200, {}, id);
}

op_result_t background_status(std::uint64_t id, job_status_t *out) {
  auto &run = runner();
  std::lock_guard<std::mutex> lk(run.mu);
  if (run.job.id != id || run.job.state.empty()) {
    return result_of(404, "not found");
  }
  if (out != nullptr) {
    out->state = run.job.state;
    out->done = run.job.done;
    out->total = run.job.total;
    out->error = run.job.error;
  }
  return result_of(200, {});
}

op_result_t background_cancel(std::uint64_t id) {
  bool known = false;
  bool running = false;
  {
    auto &run = runner();
    std::lock_guard<std::mutex> lk(run.mu);
    if (run.job.id == id && !run.job.state.empty()) {
      known = true;
      running = run.job.running;
      if (running) {
        run.job.cancel = true;
      }
    }
  }
  if (!known) {
    BOOST_LOG(info) << "[VIPLE-MEDIA] op=cancel id="sv << id << " result=not found"sv;
    return result_of(404, "not found");
  }
  // 還在跑的那一行由工作自己收尾時寫（result=cancelled）。這裡只記「已經結束、沒有東西可取消」。
  if (!running) {
    BOOST_LOG(info) << "[VIPLE-MEDIA] op=cancel id="sv << id << " result=ok"sv;
  }
  return result_of(200, {});
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
    check("name-ok", valid_name("Movies") && valid_name("a..b") && valid_name("a b") && valid_name(std::string(255, 'a')) && valid_name("中文") &&
                        valid_name("com10") && valid_name("console"));
    check("name-empty", !valid_name("") && !valid_name(".") && !valid_name(".."));
    check("name-chars", !valid_name("a/b") && !valid_name("a\\b") && !valid_name("a:b") && !valid_name("a*b") && !valid_name("a?b") &&
                          !valid_name("a\"b") && !valid_name("a<b") && !valid_name("a>b") && !valid_name("a|b"));
    check("name-ctrl", !valid_name("a\n") && !valid_name(std::string("a\0b", 3)) && !valid_name(std::string("\xC2\x85", 2)));
    check("name-long", !valid_name(std::string(256, 'a')));
    check("name-utf8", !valid_name(std::string("\xC0\xAF", 2)) && !valid_name("a.") && !valid_name("a ") && !valid_name("con") &&
                         !valid_name("CON.txt") && !valid_name("nul") && !valid_name("com1") && !valid_name("lpt9"));
    check("into-self", into_itself(fs::path("/share/Movies"), fs::path("/share/Movies")) &&
                         into_itself(fs::path("/share/Movies"), fs::path("/share/Movies/Sub")) &&
                         !into_itself(fs::path("/share/Movies"), fs::path("/share")) &&
                         !into_itself(fs::path("/share/Movies"), fs::path("/share/Movies2")));
    {
      const bool saved = config::nvhttp.media_write;
      const root_t folder {0, "v", fs::path("v"), false};
      const root_t drive {1, "C", fs::path("C"), true};
      config::nvhttp.media_write = false;
      check("write-off", !root_writable(folder) && !root_writable(drive));
      config::nvhttp.media_write = true;
      check("write-on", root_writable(folder) && !root_writable(drive));
      config::nvhttp.media_write = saved;
    }
    return failures;
  }

}  // namespace media_share
