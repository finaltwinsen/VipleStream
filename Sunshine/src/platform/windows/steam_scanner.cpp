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

    std::optional<std::string> slurp_file(const fs::path &p) {
      std::ifstream f {p, std::ios::binary};
      if (!f) {
        return std::nullopt;
      }
      std::ostringstream ss;
      ss << f.rdbuf();
      return ss.str();
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
          std::wstring name {pe.szExeFile};
          std::transform(name.begin(), name.end(), name.begin(), ::towlower);
          if (name == L"steam.exe") {
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
        if (RegEnumKeyExW(users_root, i, sid, &sid_len, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
          break;
        }
        if (wcsstr(sid, L"_Classes")) continue;
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

        // Image lookup — Steam 2024+ uses per-AppID subfolders
        fs::path newHeader  = steam / "appcache" / "librarycache" / app_id / "header.jpg";
        fs::path newLibrary = steam / "appcache" / "librarycache" / app_id / "library_600x900.jpg";
        fs::path oldHeader  = steam / "appcache" / "librarycache" / (app_id + "_header.jpg");
        fs::path oldLibrary = steam / "appcache" / "librarycache" / (app_id + "_library_600x900.jpg");
        if (fs::exists(newHeader, ec)) {
          a.image_header = path_to_utf8(newHeader);
        } else if (fs::exists(oldHeader, ec)) {
          a.image_header = path_to_utf8(oldHeader);
        }
        if (fs::exists(newLibrary, ec)) {
          a.image_library = path_to_utf8(newLibrary);
        } else if (fs::exists(oldLibrary, ec)) {
          a.image_library = path_to_utf8(oldLibrary);
        }

        if (auto it = ownership.find(app_id); it != ownership.end()) {
          a.owners = it->second;
        }
        if (auto it = playstats.find(app_id); it != playstats.end()) {
          a.last_played      = it->second.last_played;
          a.playtime_minutes = it->second.playtime_minutes;
        }

        r.apps.emplace_back(std::move(a));
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
    std::chrono::steady_clock::time_point             g_cached_at {};

  }  // namespace

  std::optional<Result> scan_cached(std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lk {g_cache_mtx};
    auto now = std::chrono::steady_clock::now();
    if (g_cached && (now - g_cached_at) < ttl) {
      return g_cached;
    }
    g_cached    = scan();
    g_cached_at = now;
    return g_cached;
  }

  void invalidate_cache() {
    std::lock_guard<std::mutex> lk {g_cache_mtx};
    g_cached.reset();
  }

}  // namespace viple::steam

#endif  // _WIN32
