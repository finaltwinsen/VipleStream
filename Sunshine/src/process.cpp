/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/token_functions.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"

#ifdef _WIN32
  #include "platform/windows/steam_scanner.h"
  // from_utf8() string conversion function
  #include "platform/windows/utf_utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>

  // RegOpenKeyExW / RegEnumKeyExW for the Steam-game-exit watchdog —
  // we read RunningAppID from HKEY_USERS\<active SID>\... because
  // Sunshine runs as LocalSystem and HKCU points to the SYSTEM hive
  // (no Steam keys there).
  #include <windows.h>
#endif

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  // Definition for the static atomic generation counter declared in
  // process.h — see comment there explaining why this is static.
  std::atomic<uint64_t> proc_t::_steam_watchdog_gen {0};

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
    std::vector<std::string> parts;
    try {
#ifdef _WIN32
      parts = boost::program_options::split_winmain(cmd);
#else
      parts = boost::program_options::split_unix(cmd);
#endif
    } catch (boost::escaped_list_error &err) {
      BOOST_LOG(error) << "Boost failed to parse command ["sv << cmd << "] because " << err.what();
      return boost::filesystem::path();
    }
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

#ifdef _WIN32
  // Read Steam's "what app is currently running" indicator from any
  // user hive. Steam writes RunningAppID (REG_DWORD) to
  // HKCU\Software\Valve\Steam — but Sunshine runs as a service under
  // LocalSystem so its own HKCU hive has no Steam keys. We have to
  // walk HKEY_USERS, look at each loaded user hive, and find the one
  // that has Software\Valve\Steam set up.
  //
  // Returns the AppID (0 if no game is running, or no Steam hive
  // found at all). Cheap enough to call every second — under normal
  // conditions there are 3-6 user hives loaded and each lookup is a
  // single registry read.
  uint32_t read_steam_running_app_id() {
    HKEY users_root = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, nullptr, 0, KEY_ENUMERATE_SUB_KEYS | KEY_READ, &users_root) != ERROR_SUCCESS) {
      return 0;
    }
    auto root_guard = util::fail_guard([&]() {
      RegCloseKey(users_root);
    });

    uint32_t running = 0;
    for (DWORD i = 0; running == 0; ++i) {
      wchar_t sid_name[256];
      DWORD   sid_len = 256;
      auto rc = RegEnumKeyExW(users_root, i, sid_name, &sid_len, nullptr, nullptr, nullptr, nullptr);
      if (rc != ERROR_SUCCESS) {
        break;  // ERROR_NO_MORE_ITEMS or other — done enumerating
      }
      // Skip the per-user "_Classes" sub-hives — they aren't user hives.
      if (wcsstr(sid_name, L"_Classes")) {
        continue;
      }

      std::wstring path = std::wstring(sid_name) + L"\\Software\\Valve\\Steam";
      HKEY steam_key = nullptr;
      if (RegOpenKeyExW(users_root, path.c_str(), 0, KEY_READ, &steam_key) != ERROR_SUCCESS) {
        continue;
      }

      DWORD value = 0;
      DWORD value_size = sizeof(value);
      DWORD value_type = 0;
      if (RegQueryValueExW(steam_key, L"RunningAppID", nullptr, &value_type,
                           reinterpret_cast<BYTE *>(&value), &value_size) == ERROR_SUCCESS &&
          value_type == REG_DWORD && value != 0) {
        running = value;
      }
      RegCloseKey(steam_key);
    }
    return running;
  }
#endif  // _WIN32

  // VipleStream H Phase 2.4: when the Steam game we launched exits,
  // tear down the streaming session instead of falling back to the
  // host desktop. Same teardown the /cancel HTTP endpoint does.
  //
  // Implementation notes:
  //   - The watchdog runs as a *detached* thread. We never join it.
  //     Cancellation is via the generation counter: each new launch
  //     bumps it, the watchdog checks at every poll iteration and
  //     bails on mismatch. This avoids deadlock when proc.terminate()
  //     itself is what's calling stop_steam_watchdog_().
  //   - Phase A (waiting for game to start) times out at 5 min — if
  //     RunningAppID never matches, the user probably cancelled the
  //     launch in Steam UI or owned-but-not-installed dialog.
  //   - Phase B (waiting for game to exit) has no timeout — games
  //     can run for hours.
  void proc_t::start_steam_watchdog_(uint32_t app_id) {
#ifdef _WIN32
    // Bump generation; any watchdog still running for the previous
    // launch will see the mismatch and exit at its next poll tick.
    uint64_t my_gen = ++_steam_watchdog_gen;

    // Capture `this` so we can read `_steam_watchdog_gen` directly.
    // `proc` is a global singleton with static lifetime so the
    // capture is safe even though the thread is detached and may
    // outlive any call frame.
    std::thread([this, app_id, my_gen]() {
      // 250 ms poll: balances "snappy detection of game exit" against
      // CPU cost (registry read is ~µs but we only need sub-second
      // detection). Shorter = less time the host desktop is visible
      // to the client between game-window-closes and stream-disconnect.
      const auto poll_interval = std::chrono::milliseconds(250);
      const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);

      BOOST_LOG(info) << "[steam-watchdog "sv << app_id << "] started, waiting for game to launch"sv;

      bool game_started = false;
      while (_steam_watchdog_gen.load() == my_gen) {
        uint32_t cur = read_steam_running_app_id();

        if (!game_started) {
          if (cur == app_id) {
            game_started = true;
            BOOST_LOG(info) << "[steam-watchdog "sv << app_id << "] game running"sv;
          } else if (std::chrono::steady_clock::now() > start_deadline) {
            BOOST_LOG(warning) << "[steam-watchdog "sv << app_id
                               << "] game never started in 5 min; giving up"sv;
            return;
          }
        } else {
          if (cur != app_id) {
            BOOST_LOG(info) << "[steam-watchdog "sv << app_id
                            << "] game exited (RunningAppID="sv << cur << "); ending stream"sv;
            break;
          }
        }
        std::this_thread::sleep_for(poll_interval);
      }

      // Re-check generation in case we exited the loop because of
      // supersession (not game exit).
      if (_steam_watchdog_gen.load() != my_gen) {
        BOOST_LOG(info) << "[steam-watchdog "sv << app_id << "] superseded, bailing"sv;
        return;
      }

      // Trigger session teardown on yet another detached thread so
      // that proc.terminate() (which calls stop_steam_watchdog_(),
      // which bumps our generation) can run cleanly without us still
      // sitting in the polling loop.
      //
      // GRACEFUL-DISCONNECT NOTE: do NOT call rtsp_stream::terminate_sessions()
      // here. That force-stops every RTSP session and the controlBroadcast
      // loop reaps them via enet_peer_disconnect_now() *before* it gets a
      // chance to send the SERVER_TERMINATED_CLOSED (0x80030023) control
      // packet — which is exactly the packet moonlight-common-c maps to
      // ML_ERROR_GRACEFUL_TERMINATION (suppresses the "Connection
      // terminated" error dialog on the client).
      //
      // Instead, just call proc.terminate(). That makes proc.running()
      // return 0; the controlBroadcast loop notices on its next ~150 ms
      // iteration, exits its main loop, and runs the dedicated
      // termination-broadcast block (stream.cpp around line 1306) which
      // does send the 0x80030023 packet to every still-connected session.
      // Sessions then unwind cleanly — no error dialog on the client.
      std::thread([]() {
        if (proc.running() > 0) {
          proc.terminate();
        }
        display_device::revert_configuration();
      }).detach();
    }).detach();
#else
    (void) app_id;
#endif
  }

  void proc_t::stop_steam_watchdog_() {
    // Bump generation; any in-flight watchdog will notice and bail.
    // We do not join — see comment in start_steam_watchdog_().
    ++_steam_watchdog_gen;
  }

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    // Ensure starting from a clean slate
    terminate();

    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }

    _app_id = app_id;
    _app = *iter;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    // Add Stream-specific environment variables
    _env["SUNSHINE_APP_ID"] = std::to_string(_app_id);
    _env["SUNSHINE_APP_NAME"] = _app.name;
    _env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
    _env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
    _env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
    _env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = utf_utils::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      terminate();
    });

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        // We don't want any prep commands failing launch of the desktop.
        // This is to prevent the issue where users reboot their PC and need to log in with Sunshine.
        // permission_denied is typically returned when the user impersonation fails, which can happen when user is not signed in yet.
        if (!(_app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait(ec);
      if (ec) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] wait failed with error code ["sv << ec << ']';
        return -1;
      }
      auto ret = child.exit_code();
      if (ret != 0) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] exited with code ["sv << ret << ']';
        return -1;
      }
    }

    for (auto &cmd : _app.detached) {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (_app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(_app.cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << _app.cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, _app.cmd, working_dir, _env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << _app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

    _app_launch_time = std::chrono::steady_clock::now();

#ifdef _WIN32
    // VipleStream H Phase 2.4: kick off the Steam game-exit watchdog
    // for auto-imported Steam apps, AFTER all other launch steps so a
    // failure path above doesn't leave a watchdog running.
    if (_app.source == "steam" && !_app.steam_app_id.empty()) {
      try {
        uint32_t aid = static_cast<uint32_t>(std::stoul(_app.steam_app_id));
        if (aid > 0) {
          start_steam_watchdog_(aid);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Bad steam_app_id ["sv << _app.steam_app_id
                           << "]: "sv << e.what()
                           << " — game-exit watchdog disabled for this launch"sv;
      }
    }
#endif

    fg.disable();

    return 0;
  }

  int proc_t::running() {
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && _process.native_exit_code() == 0 &&
               std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;
      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << ']';
      terminate();
    }

    return 0;
  }

  void proc_t::terminate() {
    std::error_code ec;
    placebo = false;
    // VipleStream H Phase 2.4: cancel any in-flight Steam game-exit
    // watchdog before tearing down the app — otherwise the watchdog
    // would race with us and call terminate() again.
    stop_steam_watchdog_();
    terminate_process_group(_process, _process_group, _app.exit_timeout);
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

    _pipe.reset();

    bool has_run = _app_id > 0;

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif

      display_device::revert_configuration();
    }

    _app_id = -1;
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    return _app.name;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  template<class It>
  It find_match(It begin, It end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = val_raw.data();
    auto val_raw_end = val_raw.data() + val_raw.size();
    auto dollar = std::find(pos, val_raw_end, '$');

    std::stringstream ss;

    while (dollar != val_raw_end) {
      auto next = dollar + 1;
      if (next != val_raw_end) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, val_raw_end);
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, val_raw_end, '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  /**
   * @brief Validates a path whether it is a valid PNG.
   * @param path The path to the PNG file.
   * @return true if the file has a valid PNG signature, false otherwise.
   */
  bool check_valid_png(const std::filesystem::path &path) {
    // PNG signature as defined in PNG specification
    // http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }

    std::array<unsigned char, 8> header;
    file.read(reinterpret_cast<char *>(header.data()), 8);

    if (file.gcount() != 8) {
      return false;
    }

    return header == PNG_SIGNATURE;
  }

  // VipleStream H Phase 2: Steam library covers are JPEG not PNG.
  // Validate via magic bytes: JPEG starts with FF D8 FF (SOI marker).
  bool check_valid_jpeg(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }
    std::array<unsigned char, 3> header;
    file.read(reinterpret_cast<char *>(header.data()), 3);
    if (file.gcount() != 3) {
      return false;
    }
    return header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF;
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // VipleStream H Phase 2: also accept .jpg / .jpeg — Steam's local
    // librarycache uses JPEG for header.jpg / library_600x900.jpg and
    // we auto-populate image_path to those. Upstream Sunshine only
    // accepted .png which silently rejected every Steam cover and
    // substituted the generic placeholder. The appasset HTTP handler
    // picks Content-Type at send time based on the actual extension.
    const bool is_png  = (image_extension == ".png");
    const bool is_jpeg = (image_extension == ".jpg" || image_extension == ".jpeg");
    if (!is_png && !is_jpeg) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    auto validate_magic = [&](const std::filesystem::path &p) -> bool {
      return is_png ? check_valid_png(p) : check_valid_jpeg(p);
    };

    // check if image is in assets directory
    if (auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path; std::filesystem::exists(full_image_path)) {
      if (!validate_magic(full_image_path)) {
        BOOST_LOG(warning) << "Invalid " << (is_png ? "PNG" : "JPEG") << " file at path ["sv << full_image_path << ']';
        return DEFAULT_APP_IMAGE_PATH;
      }
      return full_image_path.string();
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    if (std::error_code code; !std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    if (!validate_magic(app_image_path)) {
      BOOST_LOG(warning) << "Invalid " << (is_png ? "PNG" : "JPEG") << " file at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is valid; return path verbatim so appasset can serve the bytes
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {
    pt::ptree tree;

    try {
      pt::read_json(file_name, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      auto this_env = boost::this_process::environment();

      for (auto &[name, val] : env_vars) {
        this_env[name] = parse_env_val(this_env, val.get_value<std::string>());
      }

      std::set<std::string> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        ctx.elevated = elevated.value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(5)};

        auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

#ifdef _WIN32
      // VipleStream Phase 2 of H (Steam auto-import): augment in-memory
      // apps list with installed Steam games. 5-minute cache inside the
      // scanner — scan() is cheap enough that we could call it on every
      // parse() but users editing apps.json via Web UI trigger a refresh
      // and we don't want a 200ms stall on each edit.
      //
      // Merge rules (per docs/TODO.md §H):
      //   - only append Steam apps whose name doesn't clash with an
      //     already-loaded manual entry (user may have hand-crafted
      //     a "Counter-Strike 2" launcher with custom prep-cmds; keep
      //     theirs)
      //   - each Steam ctx_t is tagged `source = "steam"` +
      //     `steam_app_id` + `steam_owners` so the /applist endpoint
      //     (Phase 3) can expose provenance to clients
      //   - `cmd = steam://rungameid/<AppID>` lets Steam handle
      //     DRM / overlay / cloud save transparently
      //   - image_path prefers library_600x900 (cover art), falls
      //     back to header.jpg
      if (auto steam_result = viple::steam::scan_cached()) {
        // Build set of lowercased manual app names to detect collisions.
        std::set<std::string> manual_names;
        for (const auto &m : apps) {
          std::string lower = m.name;
          std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
          manual_names.insert(std::move(lower));
        }
        int steam_added = 0, steam_skipped = 0;
        for (const auto &sa : steam_result->apps) {
          std::string lower = sa.name;
          std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
          if (manual_names.count(lower)) {
            steam_skipped++;
            continue;
          }
          proc::ctx_t ctx;
          ctx.name         = sa.name;
          ctx.cmd          = sa.launch_url;
          ctx.image_path   = !sa.image_library.empty() ? sa.image_library : sa.image_header;
          ctx.source                 = "steam";
          ctx.steam_app_id           = sa.app_id;
          ctx.steam_owners           = sa.owners;
          ctx.steam_last_played      = sa.last_played;
          ctx.steam_playtime_minutes = sa.playtime_minutes;

          // VipleStream H Phase 2.3: cover the "launching game" desktop
          // leak. Sunshine launches via `steam://rungameid/<AppID>`,
          // which returns instantly (steam.exe URL handler forwards
          // the launch), so from Sunshine's POV the app is "up" but
          // the game is still 5-15 s from showing. During that window
          // we're streaming the bare Windows desktop to the client.
          //
          // Fix: inject `viple-splash.exe` as a detached command that
          // brings up a topmost full-virtual-screen black window,
          // then times out (default 10 s) or exits when it detects
          // a fullscreen foreground window. Runs in parallel with the
          // steam URL launch, so the splash appears essentially
          // instantly while steam is still warming up.
          //
          // Path: viple-splash.exe is shipped next to sunshine.exe by
          // build_sunshine.cmd. Look up via the running module dir;
          // fall back to PATH lookup which Windows handles automatically
          // when we pass a bare name.
  #ifdef _WIN32
          {
            wchar_t mod[MAX_PATH] = {};
            DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
            std::string splash;
            if (n > 0 && n < MAX_PATH) {
              std::filesystem::path p {mod};
              p.replace_filename("viple-splash.exe");
              if (std::filesystem::exists(p)) {
                splash = "\"" + p.string() + "\"";
              }
            }
            if (splash.empty()) {
              splash = "viple-splash.exe";
            }
            ctx.detached.emplace_back(splash + " --timeout 12");
          }
  #endif
          // Steam's URL handler returns quickly, so auto-detach ON keeps
          // Sunshine from treating "steam.exe quit 0 after launch" as
          // session end. wait_all=false means don't wait for child
          // processes — Steam launches the game in its own session.
          ctx.auto_detach  = true;
          ctx.wait_all     = false;
          ctx.elevated     = false;
          ctx.exit_timeout = std::chrono::seconds {5};
          auto ids_tuple = calculate_app_id(ctx.name, ctx.image_path, i++);
          if (ids.count(std::get<0>(ids_tuple)) == 0) {
            ctx.id = std::get<0>(ids_tuple);
          } else {
            ctx.id = std::get<1>(ids_tuple);
          }
          ids.insert(ctx.id);
          apps.emplace_back(std::move(ctx));
          steam_added++;
        }
        BOOST_LOG(info) << "[steam-import] merged " << steam_added
                        << " Steam apps (" << steam_skipped
                        << " skipped due to name collision with manual entries)";
      }
#endif

      return proc::proc_t {
        std::move(this_env),
        std::move(apps)
      };
    } catch (std::exception &e) {
      BOOST_LOG(error) << e.what();
    }

    return std::nullopt;
  }

  void refresh(const std::string &file_name) {
    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
