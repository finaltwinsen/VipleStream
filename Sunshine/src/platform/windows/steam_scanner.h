/**
 * @file src/platform/windows/steam_scanner.h
 * @brief Scan a local Steam installation for installed games + login users.
 *
 * Phase 2 of the Steam auto-import feature (see docs/TODO.md §H). Read-only
 * scan — never writes to Steam's files or registry. Output is a flat
 * `SteamScanResult` that the caller (process.cpp) converts into
 * `proc::ctx_t` entries to merge into the live apps list.
 *
 * Schema matches `scripts/scan_steam.ps1` (Phase 1 reference impl) so the
 * HTTP layer (Phase 3) can trivially serialise it.
 */
#pragma once

#ifdef _WIN32

  #include <chrono>
  #include <cstdint>
  #include <optional>
  #include <string>
  #include <vector>

namespace viple::steam {

  struct Profile {
    std::string steam_id3;          ///< 32-bit accountID (folder name under userdata/)
    std::string steam_id64;         ///< 64-bit SteamID (loginusers.vdf key)
    std::string account_name;       ///< Steam username used for -login
    std::string persona_name;       ///< Display name (shown in Steam friends)
    std::string avatar_path;        ///< Local path to avatar.png, "" if missing
    bool        remember_password;  ///< true if `-login <user>` can skip password
    bool        switchable;         ///< alias for remember_password (future-proof)
    bool        most_recent;        ///< most recently logged-in account
    int64_t     last_login;         ///< unix timestamp of last login
  };

  struct App {
    std::string              app_id;         ///< Steam AppID as decimal string
    std::string              name;           ///< human-readable title from appmanifest
    std::string              install_dir;    ///< absolute path on disk
    int64_t                  size_on_disk;   ///< bytes
    std::string              image_header;   ///< local path to header.jpg, "" if absent
    std::string              image_library;  ///< local path to library_600x900.jpg, "" if absent
    std::vector<std::string> owners;         ///< steam_id3 list — which profiles own this app
    std::string              launch_url;     ///< "steam://rungameid/<AppID>"

    // VipleStream H Phase 2.2: per-user playtime data aggregated across
    // all owners on this machine. Steam tracks per (user, app) in the
    // `UserLocalConfigStore.Software.Valve.Steam.apps.<AppID>` block of
    // each user's `localconfig.vdf` — we take max(LastPlayed) so
    // "recently played by anyone" wins for the sort, and sum(Playtime)
    // so a game played on two accounts counts as total use on the
    // machine. Both 0 means nobody has ever launched it (newly
    // installed / installed-but-never-run).
    //
    // Phase 3 will switch aggregation to per-selected-profile once the
    // client UI exposes a profile filter.
    int64_t                  last_played      = 0;  ///< unix timestamp, 0 = never
    int64_t                  playtime_minutes = 0;  ///< all-time total across all owners
  };

  struct Result {
    std::string          steam_root;
    bool                 steam_running = false;
    std::string          auto_login;      ///< HKCU AutoLoginUser (account_name)
    std::string          current_user;    ///< ActiveUser steam_id3 ("" if 0/absent)
    std::vector<Profile> profiles;
    std::vector<App>     apps;
    // for /applist + diagnostics:
    int                  library_count = 0;
    std::chrono::system_clock::time_point scanned_at;
  };

  /**
   * Perform a full scan. Expensive (filesystem + registry) — callers
   * should cache the result. Returns std::nullopt if Steam isn't
   * installed (no registry entry found) or the install is broken.
   */
  std::optional<Result> scan();

  /**
   * Cached scan entry point. First call does a scan; subsequent calls
   * within `ttl` return the cached result. Thread-safe.
   */
  std::optional<Result> scan_cached(std::chrono::seconds ttl = std::chrono::seconds {300});

  /**
   * Force the next `scan_cached` call to re-scan. Useful after we
   * detect that Steam's own config has changed (e.g. after a `/api/steam/switch`
   * in Phase 4 the ActiveUser changes, and the UI wants fresh data).
   */
  void invalidate_cache();

  /**
   * Read the currently-active Steam user (steam_id3) from the registry,
   * bypassing scan_cached()'s 5-min TTL. Cheap (one DWORD read per
   * HKEY_USERS subhive). Returns "" if Steam isn't logged in.
   *
   * Walks HKEY_USERS rather than HKEY_CURRENT_USER because Sunshine
   * runs via CreateProcessAsUserW WITHOUT LoadUserProfile — so the
   * spawned process's HKCU points at the .DEFAULT hive, not the
   * actual logged-in user's. The same trick has been used since
   * v1.2.117 in the /steamswitch polling loop; v1.2.118 hoisted it
   * here so /steamprofiles, the /steamswitch short-circuit, and any
   * future caller all hit one canonical implementation.
   */
  std::string read_active_user_id3();

}  // namespace viple::steam

#endif  // _WIN32
