/**
 * @file src/platform/linux/vr_stub.cpp
 * @brief VipleStream 2.0 §VR：非 Windows 平台的 VR stub。
 *
 * Linux PCVR 不在 2.0 範圍內（docs/vr_architecture.md 不變式 10）：VR 程式碼照常
 * 編譯，但這裡恆回報不支援，所以 /serverinfo 的 PCVR bit 固定為 0、/launch 帶
 * vr=1 一律回 VR_DISABLED，linux-server .deb 的建置與 KMS 擷取行為都不受影響。
 * macOS 的 cmake 也掛這個檔（macOS 目前不出貨，只是讓連結不缺符號）。
 *
 * M1b（S1-07／S1-19）：`vr::bridge` 的平台中立介面在這裡全部回「不支援」——不起執行緒、
 * 不建任何物件；tracking_wanted() 恆為 false，所以 0x5506 handler 不會走到 publish。
 * Windows 專用的 frame_source_t／frame_reader_t／fence_waiter_t 在 vr_bridge.h 以 _WIN32 包住，
 * 這裡不需要定義。用到 vripc_* 型別的 stub（session config、pacing、haptic／timing sink、publish_tracking）
 * 只在 x64 編譯：vr_ipc_abi.h 在其他架構會 #error，vr_bridge.h 以 VIPLE_VR_BRIDGE_HAS_ABI 把那些宣告拿掉
 * （aarch64 Linux、Apple Silicon 的 server 建置因此仍編得過）。
 */
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_platform.h"

namespace vr {
  bool platform_supported() {
    return false;
  }
}  // namespace vr

// M1b V5：Linux／macOS 沒有 SteamVR 編排；全部回報不可用（不變式 10）
namespace vr::platform {
  env_t probe_environment(bool) {
    env_t e;
    e.probed = true;
    return e;
  }

  env_t cached_environment() {
    return probe_environment(false);
  }

  bool pcvr_available(std::string *reason) {
    if (reason) {
      *reason = "platform unsupported";
    }
    return false;
  }

  void disable_pcvr_until_restart() {
  }

  bool console_user_present() {
    return false;
  }

  uint32_t console_user_rid() {
    return 0;
  }

  uint32_t vrserver_pid() {
    return 0;
  }

  std::wstring expected_driver_host_image() {
    return {};
  }

  bool steam_logged_in() {
    return false;
  }

  bool launch_steamvr() {
    return false;
  }

  bool launch_vr_app(const std::string &) {
    return false;
  }

  quit_e quit_steamvr(bool) {
    return quit_e::not_running;
  }

  run_result_t run_vrpathreg(const std::string &, const std::string &) {
    return {};
  }

  std::optional<std::vector<std::string>> registered_viplestream_drivers() {
    return std::nullopt;
  }

  deploy_result_t deploy_driver() {
    deploy_result_t r;
    r.reason = "unsupported";
    return r;
  }

  int remove_other_registrations(const std::string &) {
    return 0;
  }

  void prune_old_versions(const std::string &) {
  }

  guard_report_t guard_apply() {
    return {};
  }

  guard_report_t guard_restore() {
    guard_report_t g;
    g.result = guard_e::nothing_to_do;
    return g;
  }

  bool guard_pending() {
    return false;
  }

  std::string guard_keys_snapshot() {
    return {};
  }

  std::string steamvr_log_tail(const std::string &, size_t) {
    return {};
  }

  conflict_t detect_conflicts(bool, uint32_t) {
    return {};
  }

  conflict_t cached_conflicts() {
    return {};
  }

  std::set<std::string> vr_manifest_app_ids(bool) {
    return {};
  }
}  // namespace vr::platform

namespace vr::bridge {
  bool start() {
    return false;
  }

  void stop() {
  }

  status_t status() {
    status_t s;
    s.pipe_reason = "unsupported";
    return s;
  }

  counters_t counters() {
    return {};
  }

#if VIPLE_VR_BRIDGE_HAS_ABI
  bool set_session_config(const vripc_session_config_t &) {
    return false;
  }
#endif

  void clear_session_config() {
  }

  void set_armed(bool) {
  }

  void request_steamvr_quit() {
  }

#if VIPLE_VR_BRIDGE_HAS_ABI
  void set_pacing_ppm(int32_t) {
  }

  void set_pacing(const vripc_pacing_t &) {
  }
#endif

  void set_dev_mode(bool) {
  }

  bool send_dev_set_v2p(uint32_t) {
    return false;
  }

  void on_user_session_changed() {
  }

  bool allow_selftest_peer(std::uintptr_t) {
    return false;
  }

  void revoke_selftest_peer() {
  }

  void set_expected_driver_host_image(const std::wstring &) {
  }

  void request_teardown(uint64_t, uint32_t) {
  }

  void set_event_sink(event_cb) {
  }

#if VIPLE_VR_BRIDGE_HAS_ABI
  void set_haptic_sink(haptic_cb) {
  }

  void set_timing_sink(timing_cb) {
  }
#endif

  bool tracking_wanted() {
    return false;
  }

#if VIPLE_VR_BRIDGE_HAS_ABI
  void publish_tracking(const tracked_sample_t &) {
  }
#endif

  pipe_sd_report_t check_pipe_security(bool) {
    pipe_sd_report_t r;
    r.pipe_reason = "unsupported";
    return r;
  }

  consume_report_t selftest_consume(std::chrono::milliseconds, const std::function<bool()> &) {
    consume_report_t r;
    r.error = "unsupported";
    return r;
  }
}  // namespace vr::bridge
