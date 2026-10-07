/**
 * @file src/vr/vr_platform.h
 * @brief VipleStream 2.0 §VR：平台層的 VR 支援介面（docs/vr_architecture.md §3.11、M1b 設計 §S1-13、§D）。
 *
 * 實作依平台分開：
 *   - Windows：src/platform/windows/vr_platform_win.cpp（SteamVR 探索、使用者身分操作、部署、settings guard）
 *   - Linux／macOS：src/platform/linux/vr_stub.cpp（一律回報不支援）
 * 不支援的平台上 /serverinfo 的 PCVR bit 恆為 0，/launch 帶 vr=1 一律回 VR_DISABLED
 * （不變式 10）。
 *
 * 讀使用者可寫檔案（openvrpaths、steamvr.vrsettings、vrserver.txt、appmanifest、vrmanifest）的函式只能在
 * 編排器的背景執行緒（或非 HTTP 的初始化路徑）呼叫；HTTP 執行緒只讀快取（`cached_environment()`、
 * `pcvr_available()`、`cached_conflicts()`）——任何本機使用者都能對 Steam 樹的檔案設 oplock 讓開檔阻塞（sec-m5）。
 * 寫使用者可寫檔案（steamvr.vrsettings）一律在主控台使用者的模擬身分下進行（不變式 9）；
 * vrpathreg 以使用者 limited token 執行。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vr {
  /**
   * @brief 這個平台能不能跑 VR session（M1a stub 與 M1b pcvr 的共同前提）。
   * @return Windows 為 true；其他平台為 false。
   */
  bool platform_supported();

  namespace platform {
    /// 探測到的環境（§S1-13）。字串一律 UTF-8；路徑只給編排器內部用，寫 log 時只印尾端。
    struct env_t {
      bool probed = false;  ///< 至少完成一次 probe_environment()
      bool steamvr_installed = false;
      std::string steam_root;  ///< Steam 安裝目錄
      std::string steamvr_runtime;  ///< SteamVR 安裝目錄（openvrpaths 的 runtime[0] 優先）
      std::string buildid;  ///< appmanifest_250820.acf 的 buildid
      bool install_dir_safe = false;  ///< VipleStream 安裝目錄沒有一般使用者可寫（不變式 6）
      std::string unsafe_reason;
      bool user_present = false;  ///< 有主控台使用者（可以取得 limited token）
      bool session_ok = false;  ///< 自身 session == 主控台 session 且以 SYSTEM 執行
      bool adapter_rule_ok = false;  ///< K22：恰好一張可用的硬體 adapter，或 state.json 指定的那張在候選內
      std::string adapter_reason;
      uint32_t adapter_luid_low = 0;  ///< adapter_rule_ok 時的 LUID（session config 用）
      int32_t adapter_luid_high = 0;
      bool openxr_other = false;  ///< ActiveRuntime 不在 SteamVR runtime 之下（只警告，K14）
      std::string driver_version;  ///< 本次 zip 帶來的 driver 版本目錄名（viplestream.version，例 2.0.0-h6dc45e0d）
    };

    /**
     * @brief 重新探測環境（會讀使用者可寫檔案；只在背景執行緒呼叫）。結果同時更新快取。
     * @param force_refresh false 且快取未滿 60 s 時直接回快取。
     */
    env_t probe_environment(bool force_refresh);

    /// 最近一次 probe_environment() 的結果（任何執行緒）
    env_t cached_environment();

    /**
     * @brief `vr_pcvr=enabled` 時 serverinfo PCVR bit 與 /launch 的前提（只讀快取）。
     * @param reason 不可用時寫入原因（VR_DISABLED 說明用：steamvr not installed｜install dir unsafe｜
     *        pipe unavailable｜session mismatch｜adapter mismatch｜no console user｜not probed yet｜disabled until restart）
     */
    bool pcvr_available(std::string *reason = nullptr);

    /// vr_pcvr 在執行中改成 disabled：到 server 重啟之前 pcvr_available() 一律回 false（ops-m15）
    void disable_pcvr_until_restart();

    /// 有沒有主控台使用者（可取得 limited token；不讀檔）
    bool console_user_present();

    /// 主控台使用者 SID 的 RID（只供 log）；沒有使用者回 0
    uint32_t console_user_rid();

    /// 執行中的 vrserver（主控台 session）；沒有回 0
    uint32_t vrserver_pid();

    /// vrserver 已執行多久（毫秒，至少 1）；沒在跑或查不到回 0
    uint32_t vrserver_uptime_ms();

    /**
     * @brief §QUIT-SETTLE：vrserver 起來未滿 20 s 時先等到滿，再讓呼叫端結束 SteamVR；回實際等了幾毫秒。
     *
     * 2026-10-02 host 實測：SteamVR 還在啟動期（各 helper 還在連線、載入 binding）就結束 vrmonitor，
     * vrserver 會走「Lost master process → Quitting all immediately」，偶發在自己的 IPC 連線物件上
     * use-after-free 當掉（0xC0000005，堆疊沒有 driver 的 frame）；當掉時 uptime 很短，下一次啟動
     * SteamVR 就進 safe mode、擋掉我們的 driver。沒在跑或查不到啟動時間時不等。
     */
    uint32_t wait_steamvr_settled();

    /// 「合法 driver 宿主」映像：執行中的 vrserver，否則 `<runtime>\bin\win64\vrserver.exe`（沒有回空；直接交給 bridge）
    std::wstring expected_driver_host_image();

    /// Steam 已登入（HKU 的 ActiveUser ≠ 0；code 17 用）
    bool steam_logged_in();

    /// §STEAM-LOGIN（2026-10-04）：主控台使用者的 Steam 記住了帳號（HKU 的 `AutoLoginUser` 非空），啟動後會自己登入。
    /// 沒記住時 Steam 會停在登入畫面（例：為了換別的帳號登出後沒登回來），編排器不必白等 60 s
    bool steam_auto_login_configured();

    /// 主控台 session 裡有 steam.exe 在跑
    bool steam_running();

    /// 以使用者身分啟動 Steam（`-silent`：不開主視窗；有記住帳號就會自己登入）
    bool start_steam_silent();

    /// 以使用者身分開 `steam://rungameid/250820`（§D.8）
    bool launch_steamvr();

    /// 以使用者身分開 `steam://launch/<id>/VR`（失敗退 `steam://rungameid/<id>`）
    bool launch_vr_app(const std::string &url);

    enum class quit_e {
      not_running,  ///< 本來就沒在跑
      exited,  ///< vrserver 在時限內正常結束
      killed,  ///< allow_kill：強制結束 vrcompositor／vrserver
      failed,  ///< 時限內沒結束
    };

    /**
     * @brief §D.8 關閉鏈的第 2、3 步（第 1 步 `bridge::request_steamvr_quit()` 由編排器視情況先做）：
     *        結束主控台 session 內使用者的 vrmonitor → 等 vrserver 結束 ≤ 15 s；allow_kill 時再強制結束。
     */
    quit_e quit_steamvr(bool allow_kill);

    /// 以使用者 limited token 執行 vrpathreg（verb＝adddriver／removedriver），等 ≤ 15 s；成功與否以之後讀 openvrpaths 判定
    struct run_result_t {
      bool started = false;
      bool timed_out = false;
      int exit_code = -1;
    };

    run_result_t run_vrpathreg(const std::string &verb, const std::string &path_utf8);

    /// openvrpaths.vrpath 的 external_drivers 裡 manifest name == "viplestream" 的路徑（UTF-8）；讀不到回 nullopt
    std::optional<std::vector<std::string>> registered_viplestream_drivers();

    /// §D.11 部署（DEPLOY 狀態）：來源 `<install>\steamvr\viplestream` → `<install>\config\steamvr\<ver>\viplestream`
    struct deploy_result_t {
      bool ok = false;
      bool reused = false;  ///< 目的版本目錄已存在且 hash 全部相符
      std::string reason;  ///< owner｜dacl｜reparse｜path｜volume｜hash｜version｜stale-in-use｜source｜io
      std::string level;  ///< 出問題的那一層（尾端路徑）
      std::string version;  ///< 版本目錄名
      std::string driver_dir;  ///< 目的 viplestream 目錄（UTF-8，給 vrpathreg）
    };

    deploy_result_t deploy_driver();

    /// §D.9 REGISTER_FINALIZE：移除所有不是 keep 的 viplestream 註冊（vrserver 不在跑時才呼叫）；回傳移除數
    int remove_other_registrations(const std::string &keep_dir_utf8);

    /// 刪除 `<install>\config\steamvr\` 下不是 keep 的舊版本目錄（DLL 被載入時刪不掉就略過）
    void prune_old_versions(const std::string &keep_version);

    // ── D8b settings guard（§D.6）───────────────────────────────────────────
    enum class guard_e {
      ok,
      vrserver_running,  ///< vrserver 在跑：不寫也不還原（它結束時會把記憶體中的設定寫回）
      no_user,
      failed,  ///< 讀、解析或寫入失敗（GUARD_FAILED）；解析失敗絕不覆蓋
      nothing_to_do,
    };

    struct guard_report_t {
      guard_e result = guard_e::failed;
      std::string detail;  ///< log 用（鍵名、原因；不含檔案內容）
      bool user_disabled_driver = false;  ///< driver_viplestream.enable == false（DRIVER_DISABLED_BY_USER）
      bool safe_mode_blocked = false;  ///< driver_viplestream.blocked_by_safe_mode == true
    };

    /// 套用 guard（先寫 marker 再改 vrsettings）；已套用時冪等
    guard_report_t guard_apply();

    /// 依 marker 逐鍵還原（目前值 == 寫入值才還原）；沒有 marker 回 nothing_to_do
    guard_report_t guard_restore();

    /// 有沒有未還原的 guard marker
    bool guard_pending();

    /// guard 相關鍵的目前值（T3 比對用；只在背景執行緒呼叫）：`steamvr.forcedDriver=… driver_vrlink.enable=… …`
    std::string guard_keys_snapshot();

    /// steamvr.vrsettings 目前有 `driver_viplestream.blocked_by_safe_mode=true`（SteamVR 啟動時因上次當機擋掉我們的 driver）
    bool safe_mode_blocked();

    /// `<steam_root>\logs\<file>` 的尾端 cap 位元組（使用者身分讀；selftest 診斷用，不記內容到 log）
    std::string steamvr_log_tail(const std::string &file, size_t cap);

    // ── 衝突偵測（§D.5）─────────────────────────────────────────────────────
    struct conflict_t {
      bool any = false;
      std::string kind;  ///< vrlink｜other-hmd｜vr-app｜vd-streamer（any 為 true 時）
      bool steamvr_running = false;
      bool vr_app_running = false;
      bool openxr_other = false;
      /// §VR-REARM：SteamVR 在跑、HMD 是我們的 driver（已連線、沒有別的 HMD）。這時執行中的 VR 遊戲多半是上一個
      /// VR session 留下的，/launch 不擋（交給編排器：沿用、重新 arm，不行才回 restart-required）
      bool our_driver = false;
      /// Steam 的 RunningAppID（0＝沒有）。遊戲結束後常變成 250820（SteamVR 本身，也在 vrmanifest 裡）
      uint32_t running_app_id = 0;
    };

    /// SteamVR 本身的 Steam app id
    constexpr uint32_t k_steamvr_app_id = 250820;

    /// 重新判定（會讀使用者可寫的 vrserver.txt；只在背景執行緒呼叫）。driver_connected／other_hmd 取自 bridge
    conflict_t detect_conflicts(bool driver_connected, uint32_t other_hmd);

    /// 最近一次 detect_conflicts() 的結果（HTTP 執行緒用）
    conflict_t cached_conflicts();

    // ── app 模型（§D.12）────────────────────────────────────────────────────
    /// `<steam_root>\config\steamapps.vrmanifest` 裡 url 符合 `steam://launch/<id>/VR` 或 `…/<id>/openxr` 的
    /// Steam app：id → 啟動選項（"VR"／"openxr"；快取，refresh 時重讀）。新的 OpenXR 遊戲（例：Pixel Dungeon VR）
    /// 登記的是 `/openxr`，只認 `/VR` 會把它們當成一般 app，/launch 回「not a VR app」。
    std::map<std::string, std::string> vr_manifest_app_ids(bool refresh);
  }  // namespace platform
}  // namespace vr
