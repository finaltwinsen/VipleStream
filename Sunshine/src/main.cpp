/**
 * @file src/main.cpp
 * @brief Definitions for the main entry point for VipleStream-Server (upstream: Sunshine).
 */
// standard includes
#include <codecvt>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

// local includes
#include "confighttp.h"
#include "display_device.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "main.h"
#include "nvhttp.h"
#include "process.h"
#include "system_tray.h"
#include "self_update.h"
#include "relay.h"
#include "stun.h"
#include "upnp.h"
#include "video.h"
#include "vr/vr_selftest.h"

#ifdef _WIN32
  #include "platform/windows/config_acl.h"
  #include "platform/windows/vr_admin_pipe.h"
#endif

extern "C" {
#include "rswrapper.h"
}

using namespace std::literals;

std::map<int, std::function<void()>> signal_handlers;

void on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template<class FN>
void on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
}

std::map<std::string_view, std::function<int(const char *name, int argc, char **argv)>> cmd_to_func {
  {"creds"sv, [](const char *name, int argc, char **argv) {
     return args::creds(name, argc, argv);
   }},
  {"help"sv, [](const char *name, int argc, char **argv) {
     return args::help(name);
   }},
  {"version"sv, [](const char *name, int argc, char **argv) {
     return args::version();
   }},
  // VipleStream §SELF-UPDATE — CLI：--check-update / --self-update [--force] [package]
  {"check-update"sv, [](const char *name, int argc, char **argv) {
     return self_update::cli_check_update(argc, argv);
   }},
  {"self-update"sv, [](const char *name, int argc, char **argv) {
     return self_update::cli_self_update(argc, argv);
   }},
#ifdef _WIN32
  {"restore-nvprefs-undo"sv, [](const char *name, int argc, char **argv) {
     return args::restore_nvprefs_undo();
   }},
  // VipleStream 2.0 §VR（M1b S1-18、§F.5）：經 admin pipe 請執行中的 SYSTEM server 代勞；只有提升的管理員能連
  {"vr-selftest"sv, [](const char *name, int argc, char **argv) {
     return platf::vr_admin::cli_vr_selftest(argc, argv);
   }},
  {"vr-status"sv, [](const char *name, int argc, char **argv) {
     return platf::vr_admin::cli_vr_status(argc, argv);
   }},
  {"vr-abort"sv, [](const char *name, int argc, char **argv) {
     return platf::vr_admin::cli_vr_abort(argc, argv);
   }},
  {"steamvr-driver"sv, [](const char *name, int argc, char **argv) {
     return platf::vr_admin::cli_steamvr_driver(argc, argv);
   }},
#else
  // VipleStream 2.0 §VR（S1-19）：非 Windows 沒有 SteamVR driver 與 VR IPC
  {"vr-selftest"sv, [](const char *name, int argc, char **argv) {
     BOOST_LOG(error) << "--vr-selftest is not supported on this platform"sv;
     return vr::selftest::rc_unsupported;
   }},
  {"vr-status"sv, [](const char *name, int argc, char **argv) {
     BOOST_LOG(error) << "--vr-status is not supported on this platform"sv;
     return vr::selftest::rc_unsupported;
   }},
  {"vr-abort"sv, [](const char *name, int argc, char **argv) {
     BOOST_LOG(error) << "--vr-abort is not supported on this platform"sv;
     return vr::selftest::rc_unsupported;
   }},
  {"steamvr-driver"sv, [](const char *name, int argc, char **argv) {
     BOOST_LOG(error) << "--steamvr-driver is not supported on this platform"sv;
     return vr::selftest::rc_unsupported;
   }},
#endif
};

#ifdef _WIN32
LRESULT CALLBACK SessionMonitorWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_ENDSESSION:
      {
        // Terminate ourselves with a blocking exit call
        std::cout << "Received WM_ENDSESSION"sv << std::endl;
        lifetime::exit_sunshine(0, false);
        return 0;
      }
    default:
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
  if (type == CTRL_CLOSE_EVENT) {
    BOOST_LOG(info) << "Console closed handler called";
    lifetime::exit_sunshine(0, false);
  }
  return FALSE;
}
#endif

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
constexpr bool tray_is_enabled = true;
#else
constexpr bool tray_is_enabled = false;
#endif

void mainThreadLoop(const std::shared_ptr<safe::event_t<bool>> &shutdown_event) {
  bool run_loop = false;

  // Conditions that would require the main thread event loop
#ifndef _WIN32
  run_loop = tray_is_enabled && config::sunshine.system_tray;  // On Windows, tray runs in separate thread, so no main loop needed for tray
#endif

  if (!run_loop) {
    BOOST_LOG(info) << "No main thread features enabled, skipping event loop"sv;
    // Wait for shutdown
    shutdown_event->view();
    return;
  }

  // Main thread event loop
  BOOST_LOG(info) << "Starting main loop"sv;
  while (system_tray::process_tray_events() == 0);
  BOOST_LOG(info) << "Main loop has exited"sv;
}

int main(int argc, char *argv[]) {
#ifdef __APPLE__
  // Bundle assets are referenced relative to the executable
  // (e.g. ../Resources/assets), so anchor cwd to Contents/MacOS.
  {
    char executable[2048];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size) == 0) {
      std::error_code ec;
      auto exec_dir = std::filesystem::weakly_canonical(std::filesystem::path {executable}, ec).parent_path();
      if (!ec) {
        std::filesystem::current_path(exec_dir, ec);
      }
      if (ec) {
        std::cerr << "Failed to set working directory to executable path: " << ec.message() << '\n';
      }
    }
  }
#endif

  lifetime::argv = argv;

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Avoid searching the PATH in case a user has configured their system insecurely
  // by placing a user-writable directory in the system-wide PATH variable.
  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);

  setlocale(LC_ALL, "C");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  // parse config file
  if (config::parse(argc, argv)) {
    return 0;
  }

  // VipleStream §S1-02（K25）：CLI 模式（任何 --<command>，含未知指令）改寫同目錄的 sunshine-cli.log。
  // logging::init() 是截斷式開檔；以前從 SSH 跑一次 --version 就會清掉 service 正在寫的 sunshine.log。
  // stdout 照舊。tray 的自我更新在 service 行程內執行，不經過這裡。
  std::string log_path = config::sunshine.log_file;
  if (!config::sunshine.cmd.name.empty()) {
    log_path = (std::filesystem::path {config::sunshine.log_file}.parent_path() / "sunshine-cli.log").string();
  }

#ifdef _WIN32
  // VipleStream [VIPLE-SEC]：以 SYSTEM（或 config 已被收緊時的提升管理員）執行時，先以 protected SD
  // 建立／收緊 log 檔，logging::init() 的截斷式開檔會沿用這個 SD，沒有「先繼承 Users:RX」的窗口。
  platf::config_acl::prepare_log_file(platf::appdata(), log_path);
#endif

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, log_path);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VERSION << " commit: " << PROJECT_VERSION_COMMIT;

  // Log publisher metadata
  log_publisher_data();

  // Log modified_config_settings
  // VipleStream §S1-02（K25）：modified_config_settings 寫入時就只存遮蔽後的值；這裡再套一次 loggable_value
  // 當第二道防線（它對佔位字串是冪等的）。這是 config dump 進 sunshine.log 的唯一一處。
  for (auto &[name, val] : config::modified_config_settings) {
    BOOST_LOG(info) << "config: '"sv << name << "' = "sv << config::loggable_value(name, val);
  }
  config::modified_config_settings.clear();

  // VipleStream §CFG.defer: 補播設定解析期間（logging::init() 之前）累積的警告。
  // 不這樣做的話，這些訊息只會出現在 console，永遠不會進 sunshine.log 或 Web UI 的 log 檢視。
  // 位置必須在下面的 cmd.name 分支之前：像 `--creds` 這種跑完就 return 的路徑也要先寫出警告。
  for (const auto &message : config::deferred_config_warnings) {
    BOOST_LOG(warning) << message;
  }
  config::deferred_config_warnings.clear();

  if (!config::sunshine.cmd.name.empty()) {
    auto fn = cmd_to_func.find(config::sunshine.cmd.name);
    if (fn == std::end(cmd_to_func)) {
      BOOST_LOG(fatal) << "Unknown command: "sv << config::sunshine.cmd.name;

      BOOST_LOG(info) << "Possible commands:"sv;
      for (auto &[key, _] : cmd_to_func) {
        BOOST_LOG(info) << '\t' << key;
      }

      return 7;
    }

    return fn->second(argv[0], config::sunshine.cmd.argc, config::sunshine.cmd.argv);
  }

#ifdef _WIN32
  // VipleStream [VIPLE-SEC]：server 模式、以 SYSTEM 執行時，把 config 目錄內的機密檔案（sunshine.conf、
  // sunshine*.conf* 備份、sunshine_state.json（或 config 頂層的 credentials_file／file_state）、
  // sunshine*.log*、viplestream-svc.log、credentials\ 下的檔案）收成只有 SYSTEM 與 Administrators
  // 可存取；冪等，只動檔案不動目錄。非 SYSTEM（console 模式）直接返回。寫一行 [VIPLE-SEC] config-acl。
  {
    const std::filesystem::path secret_files[] {config::sunshine.credentials_file, config::nvhttp.file_state};
    platf::config_acl::tighten_config_dir(platf::appdata(), secret_files);
  }
  // conf 收緊後，一般使用者的 `--shortcut` 讀不到 conf 裡的 port；另外發佈一份不含機密的 webui_port 給它讀
  platf::config_acl::publish_webui_port(platf::appdata(), config::sunshine.port);
#endif

  // Adding guard here first as it also performs recovery after crash,
  // otherwise people could theoretically end up without display output.
  // It also should be destroyed before forced shutdown to expedite the cleanup.
  auto display_device_deinit_guard = display_device::init(platf::appdata() / "display_device.state", config::video);
  if (!display_device_deinit_guard) {
    BOOST_LOG(error) << "Display device session failed to initialize"sv;
  }

#ifdef _WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate VipleStream-Server.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();

  std::thread session_monitor_thread([&]() {
    platf::set_thread_name("session_monitor");
    session_monitor_join_thread_promise.set_value_at_thread_exit();

    WNDCLASSA wnd_class {};
    wnd_class.lpszClassName = "SunshineSessionMonitorClass";
    wnd_class.lpfnWndProc = SessionMonitorWindowProc;
    if (!RegisterClassA(&wnd_class)) {
      session_monitor_hwnd_promise.set_value(nullptr);
      BOOST_LOG(error) << "Failed to register session monitor window class"sv << std::endl;
      return;
    }

    auto wnd = CreateWindowExA(
      0,
      wnd_class.lpszClassName,
      "VipleStream-Server Session Monitor Window",
      0,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    session_monitor_hwnd_promise.set_value(wnd);

    if (!wnd) {
      BOOST_LOG(error) << "Failed to create session monitor window"sv << std::endl;
      return;
    }

    ShowWindow(wnd, SW_HIDE);

    // Run the message loop for our window
    MSG msg {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  });

  auto session_monitor_join_thread_guard = util::fail_guard([&]() {
    if (session_monitor_hwnd_future.wait_for(1s) == std::future_status::ready) {
      if (HWND session_monitor_hwnd = session_monitor_hwnd_future.get()) {
        PostMessage(session_monitor_hwnd, WM_CLOSE, 0, 0);
      }

      if (session_monitor_join_thread_future.wait_for(1s) == std::future_status::ready) {
        session_monitor_thread.join();
        return;
      } else {
        BOOST_LOG(warning) << "session_monitor_join_thread_future reached timeout";
      }
    } else {
      BOOST_LOG(warning) << "session_monitor_hwnd_future reached timeout";
    }

    session_monitor_thread.detach();
  });

#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet VipleStream-Server's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);
    self_update::shutdown();  // §SELF-UPDATE：取消 curl、join 背景執行緒
    system_tray::end_tray();

    display_device_deinit_guard = nullptr;
  });

  on_signal(SIGTERM, [&force_shutdown, &display_device_deinit_guard, shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet VipleStream-Server's still running: Forcing shutdown"sv;
      logging::log_flush();
      lifetime::debug_trap();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    // Break out of the main loop
    shutdown_event->raise(true);
    self_update::shutdown();  // §SELF-UPDATE：取消 curl、join 背景執行緒
    system_tray::end_tray();

    display_device_deinit_guard = nullptr;
  });

#ifdef _WIN32
  // Terminate gracefully on Windows when console window is closed
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
#endif

  proc::refresh(config::stream.file_apps);

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto proc_deinit_guard = proc::init();
  if (!proc_deinit_guard) {
    BOOST_LOG(error) << "Proc failed to initialize"sv;
  }

  reed_solomon_init();
  auto input_deinit_guard = input::init();

  if (input::probe_gamepads()) {
    BOOST_LOG(warning) << "No gamepad input is available"sv;
  }

  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
  }

  if (http::init()) {
    BOOST_LOG(fatal) << "HTTP interface failed to initialize"sv;

#ifdef _WIN32
    BOOST_LOG(fatal) << "To relaunch VipleStream-Server successfully, use the shortcut in the Start Menu. Do not run the server executable manually."sv;
    std::this_thread::sleep_for(10s);
#endif

    return -1;
  }

  std::unique_ptr<platf::deinit_t> mDNS;
  auto sync_mDNS = std::async(std::launch::async, [&mDNS]() {
    mDNS = platf::publish::start();
  });

  std::unique_ptr<platf::deinit_t> upnp_unmap;
  auto sync_upnp = std::async(std::launch::async, [&upnp_unmap]() {
    upnp_unmap = upnp::start();
  });

  // VipleStream: STUN NAT traversal prober
  std::unique_ptr<platf::deinit_t> stun_prober;
  auto sync_stun = std::async(std::launch::async, [&stun_prober]() {
    stun_prober = stun::start();
  });

  // VipleStream: Relay signaling client (optional, depends on config)
  std::unique_ptr<platf::deinit_t> relay_client;
  auto sync_relay = std::async(std::launch::async, [&relay_client]() {
    relay_client = relay::start();
  });

  // FIXME: Temporary workaround: Simple-Web_server needs to be updated or replaced
  if (shutdown_event->peek()) {
    return lifetime::desired_exit_code;
  }

#ifdef _WIN32
  // VipleStream 2.0 §VR（M1b S1-07／S1-18）：VR pipe（bridge，K3：一啟動就建、DACL 先只有 SY）與只給提升管理員的
  // admin pipe（--vr-selftest 等 CLI 的通道）。不變式 5：vr_pcvr=disabled 時完全不啟動，VR 程式碼閒置。
  std::unique_ptr<platf::deinit_t> vr_services;
  if (config::vr.pcvr != config::vr_t::pcvr_e::disabled) {
    vr_services = platf::vr_admin::start_services();
  }
#endif

  std::thread httpThread {nvhttp::start};
  std::thread configThread {confighttp::start};
  std::thread rtspThread {rtsp_stream::start};

#ifdef _WIN32
  // If we're using the default port and GameStream is enabled, warn the user
  if (config::sunshine.port == 47989 && is_gamestream_enabled()) {
    BOOST_LOG(fatal) << "GameStream is still enabled in GeForce Experience! This *will* cause streaming problems with VipleStream-Server!"sv;
    BOOST_LOG(fatal) << "Disable GameStream on the SHIELD tab in GeForce Experience or change the Port setting on the Advanced tab in the VipleStream-Server Web UI."sv;
  }
#endif

  if (tray_is_enabled && config::sunshine.system_tray) {
    BOOST_LOG(info) << "Starting system tray"sv;
#ifdef _WIN32
    // TODO: Windows has a weird bug where when running as a service and on the first Windows boot,
    // the tray icon would not appear even though VipleStream-Server is running correctly otherwise.
    // Restarting the service would allow the icon to appear normally.
    // For now we will keep the Windows tray icon on a separate thread.
    // Ideally, we would run the system tray on the main thread for all platforms.
    system_tray::init_tray_threaded();
#else
    system_tray::init_tray();
#endif
  }

  mainThreadLoop(shutdown_event);

#ifdef _WIN32
  // 先中止進行中的 selftest（它持有 VR 預約、可能開著 vr_probe），再停 admin pipe 與 bridge（BYE(SERVER_SHUTDOWN)）
  vr_services.reset();
#endif

  httpThread.join();
  configThread.join();
  rtspThread.join();

  task_pool.stop();
  task_pool.join();

#ifdef _WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  return lifetime::desired_exit_code;
}
