/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #define TRAY_ICON WEB_DIR "images/sunshine.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
    #define TRAY_ICON SUNSHINE_TRAY_PREFIX "-tray"
    #define TRAY_ICON_PLAYING SUNSHINE_TRAY_PREFIX "-playing"
    #define TRAY_ICON_PAUSING SUNSHINE_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SUNSHINE_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-sunshine-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked-16.png"
    #include <CoreFoundation/CoreFoundation.h>
    #include <dispatch/dispatch.h>
    #include <unordered_map>
  #endif

  // standard includes
  #include <atomic>
  #include <chrono>
  #include <csignal>
  #include <format>
  #include <string>
  #include <thread>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "confighttp.h"
  #include "display_device.h"
  #include "file_transfer.h"
  #include "fs_picker.h"
  #include "logging.h"
  #include "platform/common.h"
  #include "process.h"
  #include "src/entry_handler.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item) {
  }

  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item) {
  }

  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item) {
  }

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    platf::restart();
  }

  // VipleStream §N — file transfer tray callbacks ────────────────────────────
  //
  // 兩個 menu item 在 stream 未啟動時 disabled（由 update_tray_playing/stopped
  // 同步切換）。Callback 進入時再做一次 race-safe 確認。
  //
  // 「Send to client」流程：
  //   1. 確認 active stream + manager 沒 busy
  //   2. 開 native file picker，使用者選一個檔
  //   3. file_transfer::manager::queue_send_to_client → 命令塞入 client queue
  //   4. Client 下次 poll 拿到命令、回頭 GET /transfer/blob?token=… 拉檔
  //
  // 「Receive from client」流程：
  //   1. 確認 active stream
  //   2. 把 list_dir 命令塞給 client → client 回 listing → manager 緩存
  //   3. 開瀏覽器 http://localhost:<port>/transfer 給使用者挑檔（web UI）
  //   4. Web UI 點 Pull → confighttp 接到後再 queue upload_from_client 命令
  /**
   * VipleStream §N — forward declared 給 tray xfer callbacks 用。實作在
   * `tray` 變數宣告之後（line ~212）才合法 reference `tray`。
   */
  static void show_tray_balloon(const char *title, const char *text);

  void tray_xfer_send_cb([[maybe_unused]] struct tray_menu *item) {
    auto owner = proc::proc.running_owner_uuid();
    if (proc::proc.running() <= 0 || owner.empty()) {
      BOOST_LOG(info) << "[VIPLE-XFER] tray Send: no active stream / no owner uuid";
      show_tray_balloon("File transfer", "No active stream. Start streaming first.");
      return;
    }
    if (file_transfer::manager::instance().busy()) {
      BOOST_LOG(info) << "[VIPLE-XFER] tray Send: another transfer in progress";
      show_tray_balloon("File transfer", "Another transfer in progress. Wait for it to finish.");
      return;
    }
    fs_picker::open_options opts;
    opts.title = "VipleStream — Select file to send to client";
    auto chosen = fs_picker::pick_open_file(opts);
    if (!chosen) {
      BOOST_LOG(info) << "[VIPLE-XFER] tray Send: user cancelled";
      return;
    }
    auto token = file_transfer::manager::instance().queue_send_to_client(owner, *chosen);
    if (token.empty()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] tray Send: queue failed for " << chosen->string();
      return;
    }
    BOOST_LOG(info) << "[VIPLE-XFER] tray Send: queued token=" << token
                    << " owner=" << owner << " file=" << chosen->string();
  }

  void tray_xfer_recv_cb([[maybe_unused]] struct tray_menu *item) {
    auto owner = proc::proc.running_owner_uuid();
    if (proc::proc.running() <= 0 || owner.empty()) {
      BOOST_LOG(info) << "[VIPLE-XFER] tray Receive: no active stream / no owner uuid";
      show_tray_balloon("File transfer", "No active stream. Start streaming first.");
      return;
    }
    if (file_transfer::manager::instance().busy()) {
      BOOST_LOG(info) << "[VIPLE-XFER] tray Receive: another transfer in progress";
      show_tray_balloon("File transfer", "Another transfer in progress. Wait for it to finish.");
      return;
    }
    // 1. 先排 list_dir：client 下次 poll 會把 Downloads listing 回到 /transfer/result
    file_transfer::manager::instance().queue_list_dir(owner);
    // 2. 開瀏覽器到 confighttp 內部 /transfer 頁，使用者挑檔後 confighttp 排
    //    upload_from_client 命令
    BOOST_LOG(info) << "[VIPLE-XFER] tray Receive: opening transfer picker UI for owner="
                    << owner;
    launch_ui("/transfer"s);
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  // Tray menu - use named static arrays (compound literals are not standard C++)
  static struct tray_menu donate_submenu[] = {
    {.text = "GitHub Sponsors", .cb = tray_donate_github_cb},
    {.text = "Patreon", .cb = tray_donate_patreon_cb},
    {.text = "PayPal", .cb = tray_donate_paypal_cb},
    {.text = nullptr}
  };

  // VipleStream §N — index 持有的 menu item 位置（後面 update_tray_*
  // 用來切換 disabled）。如果 menu 順序變動，**記得**同步更新這兩個 index。
  static constexpr int k_xfer_send_idx = 2;
  static constexpr int k_xfer_recv_idx = 3;

  static struct tray_menu tray_menu_items[] = {
    // todo - use boost/locale to translate menu strings
    {.text = "Open VipleStream", .cb = tray_open_ui_cb},
    {.text = "-"},
    // VipleStream §N — 兩項都預設 disabled，update_tray_playing 啟用、
    // update_tray_stopped 關回 disabled。
    {.text = "Send file to client...",     .disabled = 1, .cb = tray_xfer_send_cb},
    {.text = "Receive file from client...", .disabled = 1, .cb = tray_xfer_recv_cb},
    {.text = "-"},
    {.text = "Donate", .submenu = donate_submenu},
    {.text = "-"},
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
    {.text = "Reset Display Device Config", .cb = tray_reset_display_device_config_cb},
  #endif
    {.text = "Restart", .cb = tray_restart_cb},
    {.text = "Quit", .cb = tray_quit_cb},
    {.text = nullptr}
  };

  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu = tray_menu_items,
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  // VipleStream §N — show_tray_balloon 實作，forward declared 在 line ~106。
  static void show_tray_balloon(const char *title, const char *text) {
    if (!tray_initialized) return;
    tray.notification_title = title;
    tray.notification_text = text;
    tray.notification_icon = TRAY_ICON;
    tray.notification_cb = nullptr;
    tray_update(&tray);
  }

  const char *GetResourcePath(const char *relativePath) {
  #ifdef __APPLE__
    if (!relativePath || !*relativePath) {
      return nullptr;
    }

    // Simple cache ensures our string pointers live forever
    static std::unordered_map<std::string, std::string> g_cache;
    auto search = g_cache.find(relativePath);
    if (search != g_cache.end()) {
      return search->second.c_str();
    }

    // If we're running from an .app bundle, get the internal Resources dir
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) {
      return relativePath;
    }

    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!resourcesURL) {
      return relativePath;
    }

    char resourcesPath[PATH_MAX];
    bool ok = CFURLGetFileSystemRepresentation(
      resourcesURL,
      true,
      reinterpret_cast<UInt8 *>(resourcesPath),
      sizeof(resourcesPath)
    );
    CFRelease(resourcesURL);
    if (!ok) {
      return relativePath;
    }

    std::string full;
    if (relativePath && relativePath[0] == '/') {
      full = relativePath;
    } else {
      full = std::string(resourcesPath) + "/" + relativePath;
    }

    BOOST_LOG(debug) << "System Tray: using " << full << " for icon path";

    auto [it, inserted] = g_cache.emplace(relativePath, std::move(full));
    return it->second.c_str();
  #else
    return relativePath;
  #endif
  }

  int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }
  #endif

  #ifdef __APPLE__
    // if these icon paths are relative, resolve to internal .app Resources path
    tray.allIconPaths[0] = GetResourcePath(TRAY_ICON);
    tray.allIconPaths[1] = GetResourcePath(TRAY_ICON_LOCKED);
    tray.allIconPaths[2] = GetResourcePath(TRAY_ICON_PLAYING);
    tray.allIconPaths[3] = GetResourcePath(TRAY_ICON_PAUSING);

    tray.icon = tray.allIconPaths[0];
  #endif

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      BOOST_LOG(error) << "System tray is not initialized"sv;
      return 1;
    }

    // Block until an event is processed or tray_quit() is called
    return tray_loop(1);
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    // VipleStream §N — stream 開始時啟用檔案傳輸 menu items
    tray_menu_items[k_xfer_send_idx].disabled = 0;
    tray_menu_items[k_xfer_recv_idx].disabled = 0;

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "Stream Started";

    static std::string msg = std::format("Streaming started for {}", app_name);
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);

    static std::string msg = std::format("Streaming paused for {}", app_name);
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = "Stream Paused";
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    // VipleStream §N — stream 結束時 disable 檔案傳輸 menu items + 中止 in-flight
    tray_menu_items[k_xfer_send_idx].disabled = 1;
    tray_menu_items[k_xfer_recv_idx].disabled = 1;
    file_transfer::manager::instance().abort_all("stream_stopped");

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);

    static std::string msg = std::format("Application {} successfully stopped", app_name);
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg.c_str();
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    tray.icon = TRAY_ICON;
    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/pin");
    };
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    platf::set_thread_name("system_tray");
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      return;
    }

    // Main tray event loop
    while (process_tray_events() == 0);

    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    try {
      auto tray_thread = std::thread(tray_thread_worker);

      // The tray thread doesn't require strong lifetime management.
      // It will exit asynchronously when tray_exit() is called.
      tray_thread.detach();

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

}  // namespace system_tray
#endif
