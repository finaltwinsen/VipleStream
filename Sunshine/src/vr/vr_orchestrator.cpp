/**
 * @file src/vr/vr_orchestrator.cpp
 * @brief VipleStream 2.0 §VR M1b V5：SteamVR 編排器（設計 §D）。
 */
// standard includes
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

// lib includes
#include <moonlight-common-c/src/VipleVr.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "vr_bridge.h"
#include "vr_orchestrator.h"
#include "vr_platform.h"
#if VIPLE_VR_BRIDGE_HAS_ABI
  #include "vr_ipc_abi.h"
#endif

using namespace std::literals;

namespace vr::orchestrator {
  namespace {
    using steady = std::chrono::steady_clock;

    struct job_t {
      negotiated_t neg;
      app_ref_t app;
      bool force = false;
    };

    std::mutex g_mtx;
    std::condition_variable g_cv;
    std::thread g_thread;
    bool g_running = false;
    bool g_quit = false;
    bool g_kick = false;
    bool g_stop_req = false;
    std::optional<job_t> g_pending;
    std::optional<launch_params_t> g_cur_params;

    std::atomic<uint8_t> g_state {0};
    std::atomic<uint8_t> g_start_from {0};  ///< start() 先把狀態設成 PRECHECK；轉換 log 要印真正的來源狀態
    std::atomic<uint8_t> g_stream_state {0};
    std::atomic<uint8_t> g_progress {0};
    std::atomic<uint16_t> g_code {0};
    std::atomic<uint64_t> g_seq {0};
    std::atomic<int64_t> g_error_until_ms {0};
    steady::time_point g_state_since = steady::now();

    /// 路徑比對鍵：斜線統一、ASCII 小寫、去尾端斜線
    std::string path_key(std::string s) {
      for (auto &c : s) {
        c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      while (!s.empty() && s.back() == '\\') {
        s.pop_back();
      }
      return s;
    }

    int64_t now_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now().time_since_epoch()).count();
    }

    state_e cur() {
      return static_cast<state_e>(g_state.load());
    }

    void publish(uint8_t stream_state, uint8_t progress, uint16_t code) {
      if (g_stream_state.load() != stream_state || g_progress.load() != progress || g_code.load() != code) {
        g_stream_state.store(stream_state);
        g_progress.store(progress);
        g_code.store(code);
        g_seq.fetch_add(1);
      }
    }

    uint8_t progress_of(state_e s) {
      switch (s) {
        case state_e::precheck:
          return 5;
        case state_e::deploy:
          return 15;
        case state_e::reg:
          return 25;
        case state_e::conflict:
          return 35;
        case state_e::quit_steamvr:
          return 40;
        case state_e::guard:
          return 45;
        case state_e::arm:
          return 50;
        case state_e::launch_steamvr:
          return 60;
        case state_e::wait_driver:
          return 70;
        case state_e::wait_hmd:
          return 85;
        case state_e::launch_app:
          return 95;
        case state_e::active:
          return 100;
        case state_e::driver_lost:
          return 60;
        default:
          return 0;
      }
    }

    void set_state(state_e to, uint16_t code = 0) {
      const auto from = cur();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - g_state_since).count();
      g_state_since = steady::now();
      g_state.store(static_cast<uint8_t>(to));
      // start() 已先把狀態設成 PRECHECK（讓 active() 立即為真），log 記 start() 之前的狀態
      BOOST_LOG(info) << "[VIPLE-VR-ORCH] state " << state_name(from == to ? static_cast<state_e>(g_start_from.load()) : from) << " -> " << state_name(to) << " ms=" << ms << (code ? std::format(" code={}", code) : ""s);
      // stream_state 對映（§D.2）：0–11 → ORCHESTRATING、12 → HMD_ACTIVE、13 → ORCHESTRATING+14、16 → ERROR
      const auto s = static_cast<uint8_t>(to);
      if (to == state_e::error) {
        publish(VIPLE_VR_STATE_ERROR, 0, code);
      } else if (to == state_e::active) {
        publish(VIPLE_VR_STATE_HMD_ACTIVE, 100, platform::cached_environment().openxr_other ? VIPLE_VR_STATE_CODE_OPENXR_RUNTIME_OTHER : VIPLE_VR_STATE_CODE_NONE);
      } else if (to == state_e::driver_lost) {
        publish(VIPLE_VR_STATE_ORCHESTRATING, progress_of(to), VIPLE_VR_STATE_CODE_DRIVER_LOST);
      } else if (s >= 1 && s <= 11) {
        publish(VIPLE_VR_STATE_ORCHESTRATING, progress_of(to), code);
      } else {
        publish(VIPLE_VR_STATE_IDLE, 0, 0);
      }
    }

    void step(const char *name, bool ok, const std::string &reason, steady::time_point t0) {
      BOOST_LOG(ok ? info : warning) << "[VIPLE-VR-ORCH] step=" << name << " result=" << (ok ? "ok" : "fail") << (reason.empty() ? ""s : " reason=" + reason)
                                     << " ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count();
    }

    bool stop_requested() {
      std::lock_guard lk(g_mtx);
      return g_stop_req || g_quit;
    }

    /// 在編排執行緒上等條件（每 50 ms 查一次）；stop 要求或逾時回 false
    template<class F>
    bool wait_for(std::chrono::milliseconds timeout, F &&cond) {
      const auto end = steady::now() + timeout;
      while (steady::now() < end) {
        if (cond()) {
          return true;
        }
        if (stop_requested()) {
          return false;
        }
        std::unique_lock lk(g_mtx);
        g_cv.wait_for(lk, 50ms);
      }
      return cond();
    }

    /// 失敗：ERROR(code)；disarm；STATE 保留 10 s
    void fail(uint16_t code, const std::string &why) {
      BOOST_LOG(error) << "[VIPLE-VR-ORCH] error code=" << code << " reason=" << why;
      bridge::set_armed(false);
      g_error_until_ms.store(now_ms() + 10000);
      set_state(state_e::error, code);
    }

#if VIPLE_VR_BRIDGE_HAS_ABI
    vripc_session_config_t make_config(const negotiated_t &neg, const platform::env_t &env) {
      const auto &p = neg.params;
      vripc_session_config_t c {};
      c.eye_width = static_cast<uint32_t>(p.eye_width);
      c.eye_height = static_cast<uint32_t>(p.eye_height);
      c.packed_width = c.eye_width * 2;
      c.packed_height = c.eye_height;
      c.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
      c.refresh_mhz = static_cast<uint32_t>(p.hz) * 1000u;
      c.period_ns = p.period_ns > 0 ? static_cast<uint64_t>(p.period_ns) : 1000000000ull / static_cast<uint64_t>(std::max(1, p.hz));
      for (int e = 0; e < 2; ++e) {
        // vrFov 是 tan×10000（元素可為負值）；ABI 是 OpenXR 慣例：left／down 為負
        c.fov_tan[e][0] = -std::abs(p.fov[e * 4 + 0]) / 10000.0f;
        c.fov_tan[e][1] = std::abs(p.fov[e * 4 + 1]) / 10000.0f;
        c.fov_tan[e][2] = std::abs(p.fov[e * 4 + 2]) / 10000.0f;
        c.fov_tan[e][3] = -std::abs(p.fov[e * 4 + 3]) / 10000.0f;
        for (int i = 0; i < 3; ++i) {
          c.eye_to_head[e][i] = p.eye_to_head[e * 7 + i] / 100000.0f;  // mm×100 → m
        }
        for (int i = 3; i < 7; ++i) {
          c.eye_to_head[e][i] = p.eye_to_head[e * 7 + i] / 10000.0f;
        }
      }
      c.ipd_m = p.ipd / 100000.0f;
      c.overscan_deg = p.overscan / 10.0f;
      c.controller_profile = 0;
      c.vr_caps = p.caps;
      c.universe_id = 0x5649504Cu;  // K24
      c.adapter_luid_low = env.adapter_luid_low;
      c.adapter_luid_high = env.adapter_luid_high;
      c.space_epoch = 0;  // client 的 0x5506 目前固定 spaceEpoch=0
      c.layout_epoch = 1;
      const uint32_t period_us = static_cast<uint32_t>(c.period_ns / 1000u);
      c.vsync_to_photons_us = period_us;
      c.hmd_extrap_cap_us = 50000;
      c.ctrl_extrap_cap_us = 50000;
      c.stale_oor_ctrl_us = 100000;
      c.stale_zero_vel_us = 2 * period_us;
      c.stale_oor_hmd_us = 1000000;
      return c;
    }
#endif

    /// §D.2 的狀態機（在編排執行緒上同步跑到 ACTIVE 結束或失敗）
    void run_session(const job_t &job) {
#if !VIPLE_VR_BRIDGE_HAS_ABI
      fail(VIPLE_VR_STATE_CODE_IPC_UNAVAILABLE, "no IPC ABI on this architecture");
      return;
#else
      const auto &neg = job.neg;
      auto t0 = steady::now();

      // 1 PRECHECK
      set_state(state_e::precheck);
      const auto env = platform::probe_environment(true);
      {
        const auto bs = bridge::status();
        uint16_t code = 0;
        std::string why;
        if (!env.steamvr_installed) {
          code = VIPLE_VR_STATE_CODE_STEAMVR_NOT_INSTALLED, why = "steamvr not installed";
        } else if (!env.user_present) {
          code = VIPLE_VR_STATE_CODE_NO_USER_SESSION, why = "no console user";
        } else if (!env.session_ok || bs.phase == bridge::phase_e::stopped || bs.phase == bridge::phase_e::no_pipe) {
          code = VIPLE_VR_STATE_CODE_IPC_UNAVAILABLE, why = "pipe=" + bs.pipe_reason;
        } else if (!env.adapter_rule_ok) {
          code = VIPLE_VR_STATE_CODE_ADAPTER_MISMATCH, why = env.adapter_reason;
        } else if (!env.install_dir_safe) {
          code = VIPLE_VR_STATE_CODE_DEPLOY_REFUSED, why = "install dir unsafe: " + env.unsafe_reason;
        }
        step("precheck", code == 0, why, t0);
        if (code) {
          return fail(code, why);
        }
      }

      // 2 DEPLOY
      set_state(state_e::deploy);
      t0 = steady::now();
      const auto dep = platform::deploy_driver();
      step("deploy", dep.ok, dep.ok ? std::format("ver={} reused={}", dep.version, dep.reused ? 1 : 0) : dep.reason + " level=" + dep.level, t0);
      if (!dep.ok) {
        platform::disable_pcvr_until_restart();
        return fail(VIPLE_VR_STATE_CODE_DEPLOY_REFUSED, dep.reason);
      }

      // 3 REGISTER
      set_state(state_e::reg);
      t0 = steady::now();
      {
        auto has = [&](const std::optional<std::vector<std::string>> &regs) {
          if (!regs) {
            return false;
          }
          for (const auto &r : *regs) {
            if (path_key(r) == path_key(dep.driver_dir)) {
              return true;
            }
          }
          return false;
        };
        if (!has(platform::registered_viplestream_drivers())) {
          const auto rr = platform::run_vrpathreg("adddriver", dep.driver_dir);
          BOOST_LOG(info) << "[VIPLE-VR-ORCH] vrpathreg adddriver started=" << rr.started << " timedOut=" << rr.timed_out << " rc=" << rr.exit_code;
        }
        const bool ok = has(platform::registered_viplestream_drivers());
        if (ok && !platform::vrserver_pid()) {
          platform::remove_other_registrations(dep.driver_dir);
          platform::prune_old_versions(dep.version);
        }
        step("register", ok, ok ? ""s : "not in openvrpaths after adddriver"s, t0);
        if (!ok) {
          return fail(VIPLE_VR_STATE_CODE_REGISTER_FAILED, "register");
        }
      }

      const auto cfg = make_config(neg, env);

      // 4 CONFLICT
      set_state(state_e::conflict);
      t0 = steady::now();
      bool need_launch = true;
      {
        const auto bs = bridge::status();
        const auto c = platform::detect_conflicts(bs.connected && !bs.peer_is_selftest, bs.other_hmd);
        const bool running = platform::vrserver_pid() != 0;
        bool restart = false;
        if (running) {
          const bool ours = bs.connected && !bs.peer_is_selftest && bs.other_hmd != VRIPC_OTHER_HMD_PRESENT && bs.degraded_code == 0 && !bs.abi_mismatch_seen;
          if (ours) {
            // K20：HMD 已 Activate 時 Hz／每眼尺寸／LUID 不可變
            const bool differs = bs.act_valid && bs.act_refresh_mhz != 0 &&
                                 (bs.act_refresh_mhz != cfg.refresh_mhz || bs.act_eye_w != cfg.eye_width || bs.act_eye_h != cfg.eye_height || bs.act_luid.low != cfg.adapter_luid_low || bs.act_luid.high != cfg.adapter_luid_high);
            if (differs) {
              if (c.vr_app_running && !job.force) {
                step("conflict", false, "restart-required", t0);
                return fail(VIPLE_VR_STATE_CODE_STEAMVR_RESTART_REQUIRED, "K20 parameters differ");
              }
              restart = true;
            } else if (!bs.hmd_presenting) {
              // V5 S0 實測：上一個 session 的 disarm（standby）之後 re-arm，vrcompositor 不會再 Present（45 s HMD_TIMEOUT，
              // 與 V4 未解 2 的 AcquireSync 逾時同源）。我們自己閒置的 SteamVR 直接重啟；有 VR app 在跑時不動它。
              if (c.vr_app_running && !job.force) {
                step("conflict", false, "restart-required (standby)", t0);
                return fail(VIPLE_VR_STATE_CODE_STEAMVR_RESTART_REQUIRED, "our HMD is in standby and a VR app is running");
              }
              restart = true;
            } else {
              need_launch = false;
            }
          } else {
            if (!job.force) {
              const uint16_t code = c.kind == "other-hmd" || c.kind == "vd-streamer" ? VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE :
                                    c.kind == "vr-app"                               ? VIPLE_VR_STATE_CODE_VR_APP_RUNNING :
                                                                                       VIPLE_VR_STATE_CODE_VRLINK_ACTIVE;
              step("conflict", false, "kind=" + (c.kind.empty() ? "vrlink"s : c.kind), t0);
              return fail(code, "steamvr running without our driver");
            }
            restart = true;
          }
        } else if (c.any && !job.force) {
          step("conflict", false, "kind=" + c.kind, t0);
          return fail(c.kind == "vr-app" ? VIPLE_VR_STATE_CODE_VR_APP_RUNNING : VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE, c.kind);
        }
        step("conflict", true, std::format("running={} restart={} kind={} force={}", running ? 1 : 0, restart ? 1 : 0, c.kind.empty() ? "none" : c.kind, job.force ? 1 : 0), t0);

        // 5 QUIT_STEAMVR
        if (restart) {
          set_state(state_e::quit_steamvr);
          t0 = steady::now();
          bool quit = false;
          if (bs.connected && bs.hmd_added && !bs.peer_is_selftest) {
            // VendorSpecificEvent(DriverRequestedQuit) 實測（V5 T3）沒讓 vrserver 結束：只給 5 s，之後走 vrmonitor
            bridge::request_steamvr_quit();
            quit = wait_for(5s, []() {
              return platform::vrserver_pid() == 0;
            });
          }
          if (!quit) {
            const auto q = platform::quit_steamvr(job.force && !platform::guard_pending());
            quit = q == platform::quit_e::exited || q == platform::quit_e::killed || q == platform::quit_e::not_running;
          }
          step("quit-steamvr", quit, "", t0);
          if (!quit) {
            return fail(VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE, "steamvr did not exit");
          }
          need_launch = true;
        }
      }

      // 6 GUARD（vrserver 保證不在跑）
      if (need_launch) {
        set_state(state_e::guard);
        t0 = steady::now();
        const int removed = platform::remove_other_registrations(dep.driver_dir);
        const auto regs = platform::registered_viplestream_drivers();
        if (!regs || regs->size() != 1) {
          step("register-finalize", false, std::format("count={} removed={}", regs ? (int) regs->size() : -1, removed), t0);
          return fail(VIPLE_VR_STATE_CODE_REGISTER_FAILED, "register-finalize");
        }
        const auto g = platform::guard_apply();
        if (g.user_disabled_driver) {
          step("guard", false, "driver disabled by user", t0);
          return fail(VIPLE_VR_STATE_CODE_DRIVER_DISABLED_BY_USER, "driver_viplestream.enable=false");
        }
        if (g.result != platform::guard_e::ok) {
          step("guard", false, g.detail, t0);
          return fail(g.result == platform::guard_e::no_user ? VIPLE_VR_STATE_CODE_NO_USER_SESSION : VIPLE_VR_STATE_CODE_GUARD_FAILED, g.detail);
        }
        step("guard", true, g.detail + (g.safe_mode_blocked ? " safeModeUnblocked=1" : ""), t0);
      }

      // 7 ARM（STEP0-3：合法 driver 宿主映像在 SteamVR 啟動前就設好，避免第一次握手被拒）
      set_state(state_e::arm);
      t0 = steady::now();
      bridge::set_expected_driver_host_image(platform::expected_driver_host_image());
      if (!bridge::set_session_config(cfg)) {
        step("arm", false, "session config rejected", t0);
        return fail(VIPLE_VR_STATE_CODE_IPC_UNAVAILABLE, "session config rejected");
      }
      bridge::set_armed(true);
      if (!need_launch) {
        const bool ready = wait_for(5s, []() {
          const auto s = bridge::status();
          return s.connected && s.ready && s.has_config && !s.peer_is_selftest;
        });
        need_launch = !ready;
      }
      step("arm", true, need_launch ? "launch" : "reuse", t0);

      const auto launch_t0 = steady::now();
      if (need_launch) {
        // 8 LAUNCH_STEAMVR
        set_state(state_e::launch_steamvr);
        t0 = steady::now();
        const auto regs = platform::registered_viplestream_drivers();
        if (!regs || regs->size() != 1) {
          return fail(VIPLE_VR_STATE_CODE_REGISTER_FAILED, "not exactly one registration before launch");
        }
        if (!platform::vrserver_pid()) {
          if (!platform::launch_steamvr()) {
            return fail(VIPLE_VR_STATE_CODE_STEAMVR_LAUNCH_FAILED, "open_url");
          }
          // Steam 沒登入時同一個 URL 會啟動 Steam；等 60 s 的 ActiveUser
          if (!platform::steam_logged_in() && !wait_for(60s, []() {
                return platform::steam_logged_in();
              })) {
            return fail(VIPLE_VR_STATE_CODE_STEAM_NOT_LOGGED_IN, "steam not logged in");
          }
          if (!wait_for(30s, []() {
                return platform::vrserver_pid() != 0;
              })) {
            step("launch-steamvr", false, "vrserver did not start", t0);
            return fail(VIPLE_VR_STATE_CODE_STEAMVR_LAUNCH_FAILED, "vrserver did not start in 30 s");
          }
        }
        // 實際啟動的 vrserver 映像（與推算的不同時更新；下一次握手生效）
        bridge::set_expected_driver_host_image(platform::expected_driver_host_image());
        step("launch-steamvr", true, std::format("vrserverPid={}", platform::vrserver_pid()), t0);

        // 9 WAIT_DRIVER
        set_state(state_e::wait_driver);
        t0 = steady::now();
        if (!wait_for(30s, []() {
              const auto s = bridge::status();
              return (s.connected && s.ready && !s.peer_is_selftest) || s.abi_mismatch_seen;
            })) {
          if (stop_requested()) {
            return;
          }
          step("wait-driver", false, "timeout", t0);
          const auto s = bridge::status();
          return fail(s.last_reject_reason == VRIPC_REJ_ADAPTER_MISMATCH ? VIPLE_VR_STATE_CODE_ADAPTER_MISMATCH : VIPLE_VR_STATE_CODE_HMD_TIMEOUT, "driver not ready in 30 s");
        }
        if (bridge::status().abi_mismatch_seen) {
          return fail(VIPLE_VR_STATE_CODE_ABI_MISMATCH_RESTART, "abi mismatch");
        }
        step("wait-driver", true, std::format("gen={}", bridge::status().generation), t0);
      }

      // 10 WAIT_HMD（自 LAUNCH 起累計 45 s）
      set_state(state_e::wait_hmd);
      t0 = steady::now();
      const auto left = 45s - std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - launch_t0);
      if (!wait_for(std::max<std::chrono::milliseconds>(left, 5s), []() {
            const auto s = bridge::status();
            return s.hmd_presenting || s.other_hmd == VRIPC_OTHER_HMD_PRESENT || s.degraded_code != 0;
          })) {
        if (stop_requested()) {
          return;
        }
        step("wait-hmd", false, "timeout", t0);
        return fail(VIPLE_VR_STATE_CODE_HMD_TIMEOUT, "no HMD_PRESENTING in 45 s");
      }
      {
        const auto s = bridge::status();
        if (s.other_hmd == VRIPC_OTHER_HMD_PRESENT) {
          return fail(VIPLE_VR_STATE_CODE_OTHER_HMD_ACTIVE, "device 0 is " + s.other_hmd_system);
        }
        if (s.degraded_code) {
          return fail(VIPLE_VR_STATE_CODE_DRIVER_DEGRADED, std::format("degraded=0x{:x}", s.degraded_code));
        }
      }
      step("wait-hmd", true, "", t0);

      // 11 LAUNCH_APP
      set_state(state_e::launch_app);
      t0 = steady::now();
      if (!job.app.vr_class && !job.app.vr_launch_url.empty()) {
        const bool ok = platform::launch_vr_app(job.app.vr_launch_url);
        step("launch-app", ok, ok ? ""s : "open_url"s, t0);  // 不致命
      }

      // 12 ACTIVE（13 DRIVER_LOST）
      set_state(state_e::active);
      int lost_tries = 0;
      while (!stop_requested()) {
        {
          std::unique_lock lk(g_mtx);
          g_cv.wait_for(lk, 200ms);
        }
        const auto s = bridge::status();
        if (s.degraded_code) {
          return fail(VIPLE_VR_STATE_CODE_DRIVER_DEGRADED, std::format("degraded=0x{:x}", s.degraded_code));
        }
        if (!s.connected || s.peer_is_selftest) {
          set_state(state_e::driver_lost);
          t0 = steady::now();
          const bool back = wait_for(10s, []() {
            const auto x = bridge::status();
            return x.connected && x.ready && !x.peer_is_selftest;
          });
          if (stop_requested()) {
            break;
          }
          if (!back) {
            if (platform::vrserver_pid() == 0 && lost_tries++ == 0 && platform::launch_steamvr() && wait_for(40s, []() {
                  const auto x = bridge::status();
                  return x.connected && x.ready && x.hmd_presenting;
                })) {
              step("driver-lost-relaunch", true, "", t0);
              set_state(state_e::active);
              continue;
            }
            step("driver-lost", false, "no reconnect", t0);
            return fail(VIPLE_VR_STATE_CODE_DRIVER_LOST, "driver lost");
          }
          step("driver-lost", true, "reconnected", t0);
          set_state(state_e::active);
        }
      }
#endif
    }

    /// 維護（IDLE／RESTORE_PENDING／ERROR 時）：環境、衝突快取、guard 還原
    void maintenance(steady::time_point &last_env) {
      if (steady::now() - last_env >= 60s) {
        last_env = steady::now();
        platform::probe_environment(true);
      }
      // STEP0-3：vrserver 在跑時 bridge 一律知道合法 driver 宿主映像（server 重啟後 driver 才連得回來，
      // 否則衝突偵測會把我們自己的 SteamVR 當成 vrlink）
      if (platform::vrserver_pid() != 0) {
        bridge::set_expected_driver_host_image(platform::expected_driver_host_image());
      }
      const auto bs = bridge::status();
      platform::detect_conflicts(bs.connected && !bs.peer_is_selftest, bs.other_hmd);
      const auto st = cur();
      if (st == state_e::error && now_ms() < g_error_until_ms.load()) {
        return;
      }
      if (platform::guard_pending() && platform::vrserver_pid() == 0) {
        const auto t0 = steady::now();
        const auto g = platform::guard_restore();
        if (g.result != platform::guard_e::nothing_to_do) {
          step("guard-restore", g.result == platform::guard_e::ok, g.detail, t0);
        }
      }
      if (st == state_e::error || (st == state_e::restore_pending && !platform::guard_pending())) {
        set_state(platform::guard_pending() ? state_e::restore_pending : state_e::idle);
      }
    }

    void thread_main() {
      platf::set_thread_name("vr_orchestrator");
      steady::time_point last_env {};
      // server 啟動時掃一遍：上次沒還原的 guard（§D.10）
      maintenance(last_env);
      for (;;) {
        std::optional<job_t> job;
        {
          std::unique_lock lk(g_mtx);
          g_cv.wait_for(lk, 2s, []() {
            return g_quit || g_pending.has_value() || g_kick;
          });
          if (g_quit) {
            break;
          }
          g_kick = false;
          if (g_pending) {
            job = std::move(g_pending);
            g_pending.reset();
            g_stop_req = false;
          }
        }
        if (job) {
          BOOST_LOG(info) << "[VIPLE-VR-ORCH] start app=\"" << job->app.name << "\" vrClass=" << job->app.vr_class << " eye=" << job->neg.params.eye_width << 'x' << job->neg.params.eye_height
                          << " hz=" << job->neg.params.hz << " force=" << job->force << " session=" << log_guid(job->neg.guid);
          run_session(*job);
          if (cur() != state_e::error) {
            // 14 STOPPING → 15 RESTORE_PENDING（先清 stop 要求，wait_for 才會真的等 HMD_STANDBY）
            {
              std::lock_guard lk(g_mtx);
              g_stop_req = false;
            }
            set_state(state_e::stopping);
            bridge::set_armed(false);
            const bool standby = wait_for(1s, []() {
              return !bridge::status().hmd_presenting;
            });
            BOOST_LOG(info) << "[VIPLE-VR-ORCH] step=disarm result=" << (standby ? "ok" : "fail") << " standby=" << standby;
            set_state(state_e::restore_pending);
          } else {
            std::lock_guard lk(g_mtx);
            g_stop_req = false;
          }
        }
        maintenance(last_env);
      }
      bridge::set_armed(false);
    }
  }  // namespace

  const char *state_name(state_e s) {
    switch (s) {
      case state_e::idle:
        return "IDLE";
      case state_e::precheck:
        return "PRECHECK";
      case state_e::deploy:
        return "DEPLOY";
      case state_e::reg:
        return "REGISTER";
      case state_e::conflict:
        return "CONFLICT";
      case state_e::quit_steamvr:
        return "QUIT_STEAMVR";
      case state_e::guard:
        return "GUARD";
      case state_e::arm:
        return "ARM";
      case state_e::launch_steamvr:
        return "LAUNCH_STEAMVR";
      case state_e::wait_driver:
        return "WAIT_DRIVER";
      case state_e::wait_hmd:
        return "WAIT_HMD";
      case state_e::launch_app:
        return "LAUNCH_APP";
      case state_e::active:
        return "ACTIVE";
      case state_e::driver_lost:
        return "DRIVER_LOST";
      case state_e::stopping:
        return "STOPPING";
      case state_e::restore_pending:
        return "RESTORE_PENDING";
      case state_e::error:
        return "ERROR";
    }
    return "?";
  }

  void init() {
    std::lock_guard lk(g_mtx);
    if (g_running || !::vr::platform_supported()) {
      return;
    }
    g_quit = false;
    g_running = true;
    g_thread = std::thread(thread_main);
  }

  void shutdown() {
    {
      std::lock_guard lk(g_mtx);
      if (!g_running) {
        return;
      }
      g_quit = true;
      g_stop_req = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable()) {
      g_thread.join();
    }
    std::lock_guard lk(g_mtx);
    g_running = false;
  }

  bool running() {
    std::lock_guard lk(g_mtx);
    return g_running;
  }

  bool start(const negotiated_t &neg, const app_ref_t &app, bool force) {
    {
      std::lock_guard lk(g_mtx);
      if (!g_running || g_pending) {
        return false;
      }
      const auto s = cur();
      if (s != state_e::idle && s != state_e::restore_pending && s != state_e::error) {
        return false;
      }
      g_pending = job_t {neg, app, force};
      g_cur_params = neg.params;
      g_stop_req = false;
    }
    // 從這裡起 active() 為 true（PRECHECK 由執行緒記轉換 log）
    g_error_until_ms.store(0);
    g_start_from.store(g_state.load());
    g_state.store(static_cast<uint8_t>(state_e::precheck));
    publish(VIPLE_VR_STATE_ORCHESTRATING, 0, 0);
    g_cv.notify_all();
    return true;
  }

  void stop(stop_reason_e reason) {
    const auto s = cur();
    if (s == state_e::idle || s == state_e::restore_pending) {
      return;
    }
    BOOST_LOG(info) << "[VIPLE-VR-ORCH] stop reason=" << (reason == stop_reason_e::user_ended ? "user-ended" : reason == stop_reason_e::terminated ? "terminated" :
                                                           reason == stop_reason_e::selftest   ? "selftest" :
                                                                                                 "shutdown")
                    << " state=" << state_name(s);
    {
      std::lock_guard lk(g_mtx);
      g_stop_req = true;
      g_pending.reset();
    }
    if (s == state_e::error) {
      g_error_until_ms.store(0);
    }
    g_cv.notify_all();
  }

  bool active() {
    const auto s = static_cast<uint8_t>(cur());
    if (s >= 1 && s <= 13) {
      return true;
    }
    return cur() == state_e::error && now_ms() < g_error_until_ms.load();
  }

  snapshot_t state() {
    snapshot_t s;
    s.state = cur();
    s.stream_state = g_stream_state.load();
    s.progress = g_progress.load();
    s.code = g_code.load();
    s.seq = g_seq.load();
    return s;
  }

  std::optional<launch_params_t> current_params() {
    std::lock_guard lk(g_mtx);
    return g_cur_params;
  }

  void kick() {
    {
      std::lock_guard lk(g_mtx);
      g_kick = true;
    }
    g_cv.notify_all();
  }

  bool wait_idle(std::chrono::milliseconds timeout) {
    const auto end = steady::now() + timeout;
    while (steady::now() < end) {
      if (cur() == state_e::idle) {
        return true;
      }
      kick();
      std::this_thread::sleep_for(250ms);
    }
    return cur() == state_e::idle;
  }
}  // namespace vr::orchestrator
