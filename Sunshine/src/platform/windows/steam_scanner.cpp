/**
 * @file src/platform/windows/steam_scanner.cpp
 * @brief Implementation — see steam_scanner.h + scripts/scan_steam.ps1.
 */

#ifdef _WIN32

  #include "steam_scanner.h"
  #include "vdf_parser.h"

  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <cstring>
  #include <cwctype>
  #include <initializer_list>
  #include <filesystem>
  #include <fstream>
  #include <mutex>
  #include <sstream>
  #include <string>
  #include <unordered_map>
  #include <vector>

  #include <windows.h>
  #include <tlhelp32.h>

  #include <boost/log/trivial.hpp>

namespace viple::steam {

  namespace fs = std::filesystem;

  // ── Helpers ─────────────────────────────────────────────────────────────

  namespace {

    /**
     * Read UTF-16 REG_SZ from HKCU / HKLM. Returns empty string on miss.
     * Supports long paths dynamically if buffer exceeds MAX_PATH.
     */
    std::wstring read_reg_sz(HKEY root, const wchar_t *subkey, const wchar_t *value) {
      HKEY hk {};
      if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return {};
      }
      wchar_t buf[MAX_PATH] = {};
      DWORD   sz   = sizeof(buf);
      DWORD   type = 0;
      LSTATUS st   = RegQueryValueExW(hk, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz);
      if (st == ERROR_MORE_DATA && sz > sizeof(buf)) {
        std::vector<wchar_t> dyn_buf(sz / sizeof(wchar_t) + 1, L'\0');
        DWORD dyn_sz = static_cast<DWORD>(dyn_buf.size() * sizeof(wchar_t));
        st = RegQueryValueExW(hk, value, nullptr, &type, reinterpret_cast<LPBYTE>(dyn_buf.data()), &dyn_sz);
        RegCloseKey(hk);
        if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
          return {};
        }
        size_t len = dyn_sz ? (dyn_sz / sizeof(wchar_t)) : 0;
        while (len > 0 && dyn_buf[len - 1] == L'\0') {
          len--;
        }
        return std::wstring {dyn_buf.data(), len};
      }
      RegCloseKey(hk);
      if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
      }
      // sz includes trailing NUL in bytes
      size_t len = sz ? (sz / sizeof(wchar_t)) : 0;
      while (len > 0 && buf[len - 1] == L'\0') {
        len--;
      }
      return std::wstring {buf, len};
    }

    uint32_t read_reg_dword(HKEY root, const wchar_t *subkey, const wchar_t *value) {
      HKEY hk {};
      if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
        return 0;
      }
      DWORD   v    = 0;
      DWORD   sz   = sizeof(v);
      DWORD   type = 0;
      LSTATUS st   = RegQueryValueExW(hk, value, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &sz);
      RegCloseKey(hk);
      if (st != ERROR_SUCCESS || type != REG_DWORD) {
        return 0;
      }
      return static_cast<uint32_t>(v);
    }

    std::string wstr_to_utf8(const std::wstring &w) {
      if (w.empty()) {
        return {};
      }
      int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
      std::string s(n, '\0');
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
      return s;
    }

    /**
     * C++20's fs::path::u8string() returns std::u8string which is a
     * distinct type from std::string — not interchangeable since
     * C++20. For our HTTP/log consumers we want plain UTF-8 in
     * std::string; round-trip through wstring gives us the proper
     * encoding conversion on Windows without the u8string type
     * mismatch headache.
     */
    std::string path_to_utf8(const fs::path &p) {
      return wstr_to_utf8(p.wstring());
    }

    /**
     * §STEAM-ART：在 Steam 的 librarycache 找某個 app 的圖。
     *
     * Steam 的快取格式一直在變，同一台機器上三種並存（11-03 host 實測 55 款裡約 20 款只在新格式）：
     *   1. 舊版：librarycache/<appid>_library_600x900.jpg
     *   2. 2024：librarycache/<appid>/library_600x900.jpg
     *   3. 2025 起：librarycache/<appid>/<sha1>/library_600x900.jpg（每張圖各自一個雜湊子資料夾，圖更新就換資料夾）
     * 檔名也有變體：直式封面另有 library_capsule.jpg（同為 300x450），橫幅有 header.jpg／library_header.jpg，
     * 有語系版本時是 <base>_<語系>.jpg（例如 library_600x900_tchinese.jpg），而且可能只有語系版。
     *
     * 依 bases 的順序找，每個 base 先找完全同名、再找語系版；同一個名字出現在多處時取最新的檔案。
     * 子資料夾只往下看一層。找不到回傳空字串。
     */
    std::string find_librarycache_image(const fs::path &lc, const std::string &app_id, std::initializer_list<const char *> bases) {
      std::error_code ec;
      std::vector<fs::path> files;
      auto collect = [&](const fs::path &dir) {
        for (fs::directory_iterator it {dir, ec}, end; !ec && it != end; it.increment(ec)) {
          std::error_code ec2;
          if (it->is_regular_file(ec2)) {
            files.push_back(it->path());
          } else if (dir == lc / app_id && it->is_directory(ec2)) {
            for (fs::directory_iterator sub {it->path(), ec2}, send; !ec2 && sub != send; sub.increment(ec2)) {
              std::error_code ec3;
              if (sub->is_regular_file(ec3)) {
                files.push_back(sub->path());
              }
            }
          }
        }
        ec.clear();
      };
      collect(lc / app_id);

      auto lower = [](std::wstring w) {
        for (auto &c : w) {
          c = (wchar_t) std::towlower(c);
        }
        return w;
      };

      for (const char *base_c : bases) {
        const std::wstring base(base_c, base_c + std::strlen(base_c));
        // 舊版平面格式
        const fs::path flat = lc / (app_id + "_" + base_c + ".jpg");
        for (int pass = 0; pass < 2; ++pass) {
          fs::path best;
          fs::file_time_type best_time {};
          for (const auto &f : files) {
            const auto name = lower(f.filename().wstring());
            const bool exact = name == base + L".jpg";
            const bool variant = name.size() > base.size() + 5 && name.compare(0, base.size() + 1, base + L"_") == 0 &&
                                 name.compare(name.size() - 4, 4, L".jpg") == 0;
            if (pass == 0 ? !exact : !variant) {
              continue;
            }
            std::error_code ec2;
            const auto t = fs::last_write_time(f, ec2);
            if (best.empty() || (!ec2 && t > best_time)) {
              best = f;
              best_time = ec2 ? fs::file_time_type {} : t;
            }
          }
          if (!best.empty()) {
            return path_to_utf8(best);
          }
          if (pass == 0 && fs::exists(flat, ec)) {
            return path_to_utf8(flat);
          }
        }
      }
      return {};
    }

    std::optional<std::string> slurp_file(const fs::path &p) {
      std::ifstream f {p, std::ios::binary | std::ios::ate};
      if (!f) {
        return std::nullopt;
      }
      const auto size = f.tellg();
      if (size <= 0) {
        return std::string {};
      }
      // 防禦性檢查：VDF / ACF 設定檔一般小於 128 MB，防止畸形超大檔案耗盡記憶體 (CWE-400)
      if (size > 128 * 1024 * 1024) {
        return std::nullopt;
      }
      std::string s(static_cast<std::size_t>(size), '\0');
      f.seekg(0, std::ios::beg);
      if (!f.read(s.data(), size)) {
        return std::nullopt;
      }
      return s;
    }

    /**
     * Convert SteamID64 to AccountID (SteamID3 bottom 32 bits).
     * SteamID64 = 76561197960265728 (individual universe base) + AccountID.
     * Returns the input back if parse fails.
     */
    std::string steamid64_to_id3(const std::string &id64) {
      try {
        uint64_t n     = std::stoull(id64);
        uint64_t base  = 76561197960265728ULL;
        if (n > base) {
          return std::to_string(n - base);
        }
      } catch (...) {}
      return id64;
    }

    std::string steamid3_to_id64(const std::string &id3) {
      try {
        uint64_t n = std::stoull(id3);
        return std::to_string(n + 76561197960265728ULL);
      } catch (...) {}
      return id3;
    }

    bool is_steam_running() {
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snap == INVALID_HANDLE_VALUE) {
        return false;
      }
      PROCESSENTRY32W pe {};
      pe.dwSize = sizeof(pe);
      bool found = false;
      if (Process32FirstW(snap, &pe)) {
        do {
          if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
            found = true;
            break;
          }
        } while (Process32NextW(snap, &pe));
      }
      CloseHandle(snap);
      return found;
    }

    /**
     * Locate Steam installation. Checks HKCU then HKLM (both WOW6432Node
     * and native, for 32/64-bit hosts). Normalises trailing backslashes.
     */
    std::string find_steam_root() {
      struct Candidate {
        HKEY           hive;
        const wchar_t *subkey;
        const wchar_t *value;
      };
      const Candidate cands[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"InstallPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"},
      };
      for (const auto &c : cands) {
        auto w = read_reg_sz(c.hive, c.subkey, c.value);
        if (!w.empty()) {
          // Canonicalise to use OS-native path separators so later fs::path
          // concatenations stay consistent. The value often uses forward
          // slashes ("c:/program files (x86)/steam") which work on Windows
          // but look inconsistent when logged.
          for (auto &ch : w) {
            if (ch == L'/') {
              ch = L'\\';
            }
          }
          while (!w.empty() && w.back() == L'\\') {
            w.pop_back();
          }
          return wstr_to_utf8(w);
        }
      }
      return {};
    }

    /**
     * Walk HKEY_USERS subhives looking for any one that has
     * Software\Valve\Steam\ActiveProcess\ActiveUser set to a non-zero
     * DWORD. Used in place of HKEY_CURRENT_USER reads because Sunshine
     * runs via CreateProcessAsUserW without LoadUserProfile — so the
     * spawned worker's HKCU points at .DEFAULT (no Valve\Steam keys),
     * not the actual logged-in user's hive. Caused the H.4 dropdown
     * to permanently show "current_user = empty" through v1.2.117.
     *
     * Skips _Classes subkeys (those are per-user file association
     * hives, never contain Valve\Steam). Returns the FIRST non-zero
     * ActiveUser found, which on a typical single-user host is the
     * console user's value. Returns "" if no hive has it set.
     */
    std::string walk_hkey_users_for_active_user() {
      HKEY users_root = nullptr;
      if (RegOpenKeyExW(HKEY_USERS, nullptr, 0,
                        KEY_ENUMERATE_SUB_KEYS | KEY_READ, &users_root) != ERROR_SUCCESS) {
        return {};
      }
      std::string result;
      for (DWORD i = 0;; ++i) {
        wchar_t sid[256];
        DWORD sid_len = 256;
        auto enum_rc = RegEnumKeyExW(users_root, i, sid, &sid_len, nullptr, nullptr,
                                     nullptr, nullptr);
        if (enum_rc == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (enum_rc != ERROR_SUCCESS) {
          continue;
        }
        if (wcsstr(sid, L"_Classes") || _wcsicmp(sid, L".DEFAULT") == 0) continue;
        std::wstring path = std::wstring(sid) + L"\\Software\\Valve\\Steam\\ActiveProcess";
        HKEY ap = nullptr;
        if (RegOpenKeyExW(users_root, path.c_str(), 0, KEY_READ, &ap) != ERROR_SUCCESS) {
          continue;
        }
        DWORD value = 0, value_size = sizeof(value), value_type = 0;
        auto rc = RegQueryValueExW(ap, L"ActiveUser", nullptr, &value_type,
                                   reinterpret_cast<BYTE *>(&value), &value_size);
        RegCloseKey(ap);
        if (rc == ERROR_SUCCESS && value_type == REG_DWORD && value != 0) {
          result = std::to_string(value);
          break;
        }
      }
      RegCloseKey(users_root);
      return result;
    }

    std::string read_current_user_id3() {
      // Was HKCU read until v1.2.118 — see walk_hkey_users_for_active_user()
      // doc comment for why HKCU is the wrong hive in our spawn context.
      return walk_hkey_users_for_active_user();
    }

    std::string read_auto_login() {
      // FIXME(v1.2.118): same HKCU-vs-.DEFAULT problem as
      // read_current_user_id3 above — would need a HKEY_USERS walk to
      // be correct. Currently unused (H.4 doesn't read auto_login),
      // so left as-is; revisit if a future feature needs it.
      return wstr_to_utf8(read_reg_sz(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"AutoLoginUser"));
    }

  }  // namespace

  // Public re-export. Defined outside the anonymous namespace so callers
  // in other TUs (nvhttp.cpp's /steamprofiles + /steamswitch handlers)
  // can use the same implementation without duplicating the HKEY_USERS
  // walk. See header for rationale.
  std::string read_active_user_id3() {
    return walk_hkey_users_for_active_user();
  }

  // ── Main scan ───────────────────────────────────────────────────────────

  std::optional<Result> scan() {
    Result r;
    r.scanned_at    = std::chrono::system_clock::now();
    r.steam_root    = find_steam_root();
    if (r.steam_root.empty()) {
      BOOST_LOG_TRIVIAL(info) << "[steam-scan] Steam not installed (no registry entry)";
      return std::nullopt;
    }
    r.steam_running = is_steam_running();
    r.auto_login    = read_auto_login();
    r.current_user  = read_current_user_id3();

    // Steam root was read as UTF-8 via wstr_to_utf8. To turn that back
    // into an fs::path that Windows APIs consume correctly, round-trip
    // through wstring.
    auto utf8_to_wstr = [](const std::string &s) {
      if (s.empty()) {
        return std::wstring {};
      }
      int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
      std::wstring w(n, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
      return w;
    };
    const fs::path steam = fs::path {utf8_to_wstr(r.steam_root)};

    // ── Parse loginusers.vdf → profiles list (from accounts that were ever logged in) ──
    std::unordered_map<std::string /* id3 */, size_t /* index in r.profiles */> profile_idx;
    if (auto txt = slurp_file(steam / "config" / "loginusers.vdf")) {
      if (auto root = vdf::parse(*txt)) {
        if (const auto *users = root->child("users"); users && users->is_map()) {
          for (const auto &[id64, node] : users->as_map()) {
            if (!node.is_map()) {
              continue;
            }
            Profile p;
            p.steam_id64        = id64;
            p.steam_id3         = steamid64_to_id3(id64);
            p.account_name      = node.leaf_or("AccountName");
            p.persona_name      = node.leaf_or("PersonaName");
            p.remember_password = (node.leaf_or("RememberPassword") == "1");
            p.switchable        = p.remember_password;
            p.most_recent       = (node.leaf_or("MostRecent") == "1");
            try {
              p.last_login = std::stoll(node.leaf_or("Timestamp", "0"));
            } catch (...) {
              p.last_login = 0;
            }
            // Avatar cache uses SteamID64.png
            fs::path avatar = steam / "config" / "avatarcache" / (id64 + ".png");
            std::error_code ec;
            if (fs::exists(avatar, ec)) {
              p.avatar_path = path_to_utf8(avatar);
            }
            profile_idx[p.steam_id3] = r.profiles.size();
            r.profiles.emplace_back(std::move(p));
          }
        }
      }
    }

    // ── Add userdata/*/ profiles that have NO loginusers entry (rare — user
    //    deleted their "remember password" but still has per-user cache) ──
    std::error_code ec;
    fs::path userdata = steam / "userdata";
    if (fs::exists(userdata, ec)) {
      try {
        for (auto &de : fs::directory_iterator {userdata, ec}) {
          if (!de.is_directory()) {
            continue;
          }
          auto id3 = path_to_utf8(de.path().filename());
          if (id3 == "0" || id3 == "anonymous") {
            continue;
          }
          if (profile_idx.count(id3)) {
            continue;
          }
          Profile p;
          p.steam_id3     = id3;
          p.steam_id64    = steamid3_to_id64(id3);
          p.persona_name  = "(unknown #" + id3 + ")";
          p.switchable    = false;
          profile_idx[id3] = r.profiles.size();
          r.profiles.emplace_back(std::move(p));
        }
      } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[steam-scan] error enumerating userdata: " << e.what();
      }
    }

    // Resolve current_user's persona_name (handy for HTTP response)
    // — but keep the id3 as the stable identifier.
    (void) r.current_user;

    // ── Parse libraryfolders.vdf → list of library roots ──
    std::vector<fs::path> library_roots;
    if (auto txt = slurp_file(steam / "steamapps" / "libraryfolders.vdf")) {
      if (auto root = vdf::parse(*txt)) {
        if (const auto *libs = root->child("libraryfolders"); libs && libs->is_map()) {
          for (const auto &[_, node] : libs->as_map()) {
            if (!node.is_map()) {
              continue;
            }
            auto path = node.leaf_or("path");
            if (!path.empty()) {
              // Steam stores paths with forward slashes or escaped
              // backslashes — std::filesystem handles both but we
              // normalise for consistent logging.
              for (auto &c : path) {
                if (c == '/') {
                  c = '\\';
                }
              }
              while (!path.empty() && path.back() == '\\') {
                path.pop_back();
              }
              library_roots.emplace_back(fs::path {utf8_to_wstr(path)});
            }
          }
        }
      }
    }
    if (library_roots.empty()) {
      library_roots.emplace_back(steam);
    }
    r.library_count = static_cast<int>(library_roots.size());

    // ── Per-user entitlements + playtime: build maps AppID → [owners] and
    //    AppID → (max last_played, sum playtime_minutes) ──
    std::unordered_map<std::string /* app_id */, std::vector<std::string> /* owner id3 */> ownership;
    struct PlayStats {
      int64_t last_played      = 0;
      int64_t playtime_minutes = 0;
    };
    std::unordered_map<std::string /* app_id */, PlayStats> playstats;
    for (const auto &p : r.profiles) {
      fs::path lc = steam / "userdata" / p.steam_id3 / "config" / "localconfig.vdf";
      if (!fs::exists(lc, ec)) {
        continue;
      }
      auto txt = slurp_file(lc);
      if (!txt) {
        continue;
      }
      auto root = vdf::parse(*txt);
      if (!root) {
        continue;
      }
      // Path: UserLocalConfigStore > Software > Valve > Steam > apps > <AppID>
      const auto *apps_node = root->path({"UserLocalConfigStore", "Software", "Valve", "Steam", "apps"});
      if (!apps_node || !apps_node->is_map()) {
        continue;
      }
      for (const auto &[app_id, app_node] : apps_node->as_map()) {
        ownership[app_id].emplace_back(p.steam_id3);

        // Playtime + last_played are optional — older apps / never-run
        // entries don't have them. Both fields are plain decimal
        // integers stored as VDF strings; stoll's try/catch guard
        // keeps any weird Steam format from crashing the scan.
        if (app_node.is_map()) {
          auto &ps = playstats[app_id];
          try {
            int64_t lp = std::stoll(app_node.leaf_or("LastPlayed", "0"));
            ps.last_played = std::max(ps.last_played, lp);
          } catch (...) {}
          try {
            // "Playtime" key has been used historically; Steam also writes
            // "Playtime2wks" for 2-week trailing stats. We want cumulative.
            int64_t pt = std::stoll(app_node.leaf_or("Playtime", "0"));
            ps.playtime_minutes += pt;
          } catch (...) {}
        }
      }
    }

    // ── Iterate each library's appmanifest_*.acf ──
    for (const auto &lib : library_roots) {
      fs::path sa = lib / "steamapps";
      if (!fs::exists(sa, ec)) {
        continue;
      }
      try {
        for (auto &de : fs::directory_iterator {sa, ec}) {
          if (!de.is_regular_file()) {
            continue;
          }
          auto fname = path_to_utf8(de.path().filename());
          if (fname.size() < 18 || fname.rfind("appmanifest_", 0) != 0 ||
              fname.substr(fname.size() - 4) != ".acf") {
            continue;
          }
          auto txt = slurp_file(de.path());
          if (!txt) {
            continue;
          }
          auto root = vdf::parse(*txt);
          if (!root) {
            continue;
          }
          const auto *st = root->child("AppState");
          if (!st || !st->is_map()) {
            continue;
          }
          auto app_id = st->leaf_or("appid");
          if (app_id.empty()) {
            continue;
          }
          // StateFlags: bit 2 (= 4) means "fully installed". Filter the
          // rest (queued / downloading / validating / corrupt).
          int state = 0;
          try {
            state = std::stoi(st->leaf_or("StateFlags", "0"));
          } catch (...) {}
          if ((state & 4) == 0) {
            continue;
          }

          App a;
          a.app_id      = app_id;
          a.name        = st->leaf_or("name");
          auto dir      = st->leaf_or("installdir");
          a.install_dir = path_to_utf8(lib / "steamapps" / "common" / fs::path {utf8_to_wstr(dir)});
          try {
            a.size_on_disk = std::stoll(st->leaf_or("SizeOnDisk", "0"));
          } catch (...) {}
          a.launch_url = "steam://rungameid/" + app_id;

          // §STEAM-ART：三種快取格式＋檔名變體都要找（原本只找 <appid>/ 與 <appid>_ 兩種固定檔名，
          // 新版 Steam 放在雜湊子資料夾或只有 library_capsule／語系版的遊戲就沒有封面）。
          const fs::path lc = steam / "appcache" / "librarycache";
          a.image_library = find_librarycache_image(lc, app_id, {"library_600x900", "library_capsule"});
          a.image_header  = find_librarycache_image(lc, app_id, {"header", "library_header"});

          if (auto it = ownership.find(app_id); it != ownership.end()) {
            a.owners = it->second;
          }
          if (auto it = playstats.find(app_id); it != playstats.end()) {
            a.last_played      = it->second.last_played;
            a.playtime_minutes = it->second.playtime_minutes;
          }

          r.apps.emplace_back(std::move(a));
        }
      } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[steam-scan] error enumerating steamapps in " << path_to_utf8(lib) << ": " << e.what();
      }
    }

    // Sort apps by name for stable output (Moonlight-side filtering doesn't
    // need it sorted but it makes logs + eventual HTTP responses readable).
    std::sort(r.apps.begin(), r.apps.end(), [](const App &a, const App &b) {
      return a.name < b.name;
    });

    BOOST_LOG_TRIVIAL(info) << "[steam-scan] " << r.apps.size()
                            << " installed apps, " << r.profiles.size() << " profiles, "
                            << r.library_count << " libraries";
    return r;
  }

  // ── Caching layer ───────────────────────────────────────────────────────

  namespace {

    std::mutex                                        g_cache_mtx;
    std::optional<Result>                             g_cached;
    bool                                              g_cache_valid = false;
    std::chrono::steady_clock::time_point             g_cached_at {};

  }  // namespace

  std::optional<Result> scan_cached(std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lk {g_cache_mtx};
    auto now = std::chrono::steady_clock::now();
    if (g_cache_valid && (now - g_cached_at) < ttl) {
      return g_cached;
    }
    g_cached      = scan();
    g_cached_at   = now;
    g_cache_valid = true;
    return g_cached;
  }

  void invalidate_cache() {
    std::lock_guard<std::mutex> lk {g_cache_mtx};
    g_cached.reset();
    g_cache_valid = false;
  }

}  // namespace viple::steam

#endif  // _WIN32
