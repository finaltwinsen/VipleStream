/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

// standard includes
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>

// lib includes
#include <boost/process/v1.hpp>

// local includes
#include "config.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#define DEFAULT_APP_IMAGE_PATH SUNSHINE_ASSETS_DIR "/box.png"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

  typedef config::prep_cmd_t cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    bool elevated;
    bool auto_detach;
    bool wait_all;
    std::chrono::seconds exit_timeout;

    // VipleStream: provenance tags for auto-discovered apps (Steam etc).
    // `source` is the origin of this entry:
    //   ""            (or missing) — declared in apps.json manually
    //   "steam"       — emitted by viple::steam::scan_cached() every 5 min
    // Future: "epic" / "xbox" / "gog". Manual entries are never
    // touched by the auto-import layer.
    std::string source;
    std::string steam_app_id;
    // steam_owners is the list of SteamID3 strings that have this app in
    // their localconfig.vdf entitlement. Used client-side (Phase 3) to
    // filter the apps list by profile.
    std::vector<std::string> steam_owners;
    // VipleStream H Phase 2.2: aggregated playtime stats across all owners.
    // Client uses these to offer Recently-Played / Most-Played sort modes.
    int64_t steam_last_played      = 0;
    int64_t steam_playtime_minutes = 0;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _app_id(0),
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    int execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session,
                std::string owner_uuid = {}, std::string owner_name = {});

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    const std::vector<ctx_t> &get_apps() const;
    std::vector<ctx_t> &get_apps();
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    void terminate();

    // VipleStream §M.1 — multi-user ownership.
    //
    // running_owner_uuid()/_name() identify which paired device launched the
    // currently-running app; empty when nothing is running or when the launch
    // predates the ownership-tracking patch.
    //
    // detach() ends the streaming-session bookkeeping for the running app
    // WITHOUT calling group.terminate() / undo_cmds / display revert.  Used
    // for "soft handover" on /launch?takeover=1 and friendly /cancel by a
    // non-owner: B's Steam game keeps running on the host (Steam runs outside
    // Sunshine's Job Object via the URL handler anyway, see proc_t::running()
    // placebo path) while the new caller takes the streaming session.
    //
    // is_running_steam_source() tells the endpoint guards whether the current
    // app was auto-imported from Steam, in which case detach is the safe
    // takeover path; manual apps may have prep_cmds whose undo step we must
    // run, so they fall back to terminate().
    // §M.1.f.2 (2026-05-14) — return by value snapshot under _owner_mutex,
    // so cross-thread reads (HTTP handlers / idle watchdog) can't tear on a
    // concurrent clear() in another thread.  Callers all `auto owner = ...`
    // so no ABI change.
    std::string running_owner_uuid() const;
    std::string running_owner_name() const;
    bool is_running_steam_source() const;
    void detach();

    /**
     * §M.1.f defensive fix (2026-05-14) — clear `_owner_uuid` / `_owner_name`
     * without touching the running process or app state.  Hook point for the
     * RTSP TEARDOWN handler when `rtsp_stream::session_count()` drops to 0
     * (i.e. all paired-client RTSP streams have ended).  Catches the case
     * where moonlight-qt client crashes / loses network without sending the
     * graceful /cancel — without this, ownership stays stuck on the host
     * and the next /launch from any device gets 503 `Server in use by
     * another paired device`.
     *
     * Why not terminate()/detach()?  Those tear down the running app, which
     * is too aggressive — the app may legitimately still be running (Steam
     * game continuing in background, manual app etc).  This only releases
     * the ownership lock so a fresh /launch can take over without manual
     * Web UI Force Disconnect.
     */
    void clear_owner_uuid();

    /**
     * §M.1.f.2 idle reconcile (2026-05-14) — refresh activity timestamp
     * for the running owner.  Called from request handlers in nvhttp.cpp
     * / confighttp.cpp; pass the SSL-cert uuid of whoever is making the
     * request.  No-op unless caller_uuid matches _owner_uuid.
     *
     * Companion to the idle watchdog spawned by execute(): if the owner
     * stops doing anything for kIdleTimeout AND no RTSP session is
     * alive, watchdog calls clear_owner_uuid() to recover from
     * abnormal disconnects (client crash / WiFi drop / power off) that
     * never sent /cancel.  Thread-safe.
     */
    void touch_activity(const std::string &caller_uuid);

    /**
     * §M.1.f.2 — current idle duration (now - _last_activity) in
     * seconds.  Returns -1 if no owner.  Exposed for /api/current_session
     * Web UI display.  Thread-safe.
     */
    int64_t idle_seconds() const;

    // Wall-clock time the current app was launched, in seconds since epoch.
    // Returns 0 if no app is running.  Used by /api/current_session.
    int64_t running_started_unix_s() const;

  private:
    int _app_id;

    // VipleStream §M.1 — paired-device that owns the currently-running app.
    std::string _owner_uuid;
    std::string _owner_name;

    boost::process::v1::environment _env;
    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;
    // VipleStream §M.1 — wall-clock launch time for Web UI session dashboard
    // (steady_clock is monotonic, can't be converted to a calendar timestamp).
    std::chrono::system_clock::time_point _app_launch_wall;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;

    // VipleStream H Phase 2.4: Steam game-exit watchdog.
    //
    // Steam launches via URL handler (`steam://rungameid/<id>`); the
    // URL handler exits in <1s but the actual game runs minutes-to-
    // hours. Sunshine's auto_detach heuristic flips the app to
    // placebo mode and `running()` then returns `_app_id` forever,
    // so the streaming session never ends when the game closes and
    // the client falls back to seeing the host desktop.
    //
    // Workaround: kick off a detached watchdog thread on Steam-app
    // launch. It polls `RunningAppID` under HKEY_USERS &lt;SID&gt;
    // Software Valve Steam (any user, since Sunshine itself runs
    // as LocalSystem). When that DWORD goes from our app id to 0
    // (or any other value), it triggers the same teardown as the
    // /cancel HTTP handler: terminate sessions, terminate proc,
    // revert display config.
    //
    // The generation counter lets us cancel a running watchdog
    // without joining its thread (which would deadlock when the
    // watchdog itself initiated proc.terminate()).  Each new launch
    // bumps it; the watchdog checks each iteration and bails if the
    // value changed.
    //
    // Static because std::atomic isn't move-constructible and
    // proc_t needs default move ops (parse() returns std::optional
    // and gets assigned to the singleton).  Functionally identical
    // since `proc` is a singleton anyway.
    static std::atomic<uint64_t> _steam_watchdog_gen;
    void start_steam_watchdog_(uint32_t app_id);
    void stop_steam_watchdog_();

    // §M.1.f.2 idle reconcile (2026-05-14) — owner activity bookkeeping
    // + auto-release watchdog.  See process.h doc-comments on
    // touch_activity() / idle_seconds() and process.cpp impl.
    //
    // _owner_mutex protects _owner_uuid / _owner_name / _last_activity
    // for cross-thread access between HTTP request handlers, RTSP
    // teardown, the idle watchdog itself, and execute() / detach() /
    // terminate() called from main.
    //
    // Static for the same reason _steam_watchdog_gen is: std::mutex
    // isn't move-constructible and proc_t needs default move ops
    // (parse() returns std::optional<proc_t> and gets assigned to the
    // singleton via std::move).  Functionally identical since `proc` is
    // a singleton — only one proc_t exists program-wide.
    //
    // _idle_watchdog_gen mirrors the _steam_watchdog_gen pattern: each
    // new execute() / clear_owner_uuid() / terminate() / detach() bumps
    // it, so any pre-existing watchdog thread checks the gen and self-
    // exits without joining (joining from clear_owner_uuid() would
    // deadlock when the watchdog itself initiated the clear).
    static std::mutex _owner_mutex;
    std::chrono::steady_clock::time_point _last_activity {};
    static std::atomic<uint64_t> _idle_watchdog_gen;
    void start_idle_watchdog_();
    void stop_idle_watchdog_();
  };

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  bool check_valid_png(const std::filesystem::path &path);
  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;
}  // namespace proc
