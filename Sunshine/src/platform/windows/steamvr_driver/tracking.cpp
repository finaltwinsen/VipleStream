// tracking.cpp - 見 tracking.h（設計 §E.2 tracking、§F.3）。
#include <openvr_driver.h>

#include "tracking.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <process.h>
#include <vector>

#include "controller_device.h"
#include "driver_log.h"
#include "hmd_device.h"
#include "pose_policy.h"
#include "seh_guard.h"

namespace vrdrv {

  namespace {
    int64_t now_qpc() {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    bool finite_pose(const vripc_pose_t &p) {
      for (float v : p.pos) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.rot) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.lin_vel) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.ang_vel) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      return true;
    }

    vr::DriverPose_t base_pose() {
      vr::DriverPose_t p {};
      p.qWorldFromDriverRotation.w = 1.0;
      p.qDriverFromHeadRotation.w = 1.0;
      p.qRotation.w = 1.0;
      p.deviceIsConnected = true;
      return p;
    }

    /// 10 秒統計的百分位數（180 Hz × 10 s ≈ 1800 筆；超過容量的不收）
    struct pct_buf_t {
      static constexpr size_t k_cap = 4096;
      std::vector<int32_t> v;

      pct_buf_t() {
        v.reserve(k_cap);
      }

      void add(int64_t x) {
        if (v.size() < k_cap) {
          v.push_back((int32_t) std::clamp<int64_t>(x, INT32_MIN, INT32_MAX));
        }
      }

      int32_t pct(double p) {
        if (v.empty()) {
          return 0;
        }
        const size_t idx = std::min(v.size() - 1, (size_t) (p * (double) (v.size() - 1) + 0.5));
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t) idx, v.end());
        return v[idx];
      }
    };
  }  // namespace

  tracking_t::tracking_t(driver_ctx_t &ctx):
      ctx_(ctx) {
  }

  tracking_t::~tracking_t() {
    if (thread_) {
      CloseHandle(thread_);
    }
    if (stop_evt_) {
      CloseHandle(stop_evt_);
    }
  }

  bool tracking_t::start() {
    stop_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_evt_) {
      return false;
    }
    thread_ = (HANDLE) _beginthreadex(nullptr, 0, &tracking_t::entry, this, 0, nullptr);
    return thread_ != nullptr;
  }

  bool tracking_t::stop(uint32_t join_ms) {
    if (!thread_) {
      return true;
    }
    SetEvent(stop_evt_);
    if (WaitForSingleObject(thread_, join_ms) != WAIT_OBJECT_0) {
      VRDRV_LOG_WARN("tracking stop-timeout ms=%u", join_ms);
      return false;
    }
    CloseHandle(thread_);
    thread_ = nullptr;
    return true;
  }

  unsigned __stdcall tracking_t::entry(void *self) {
    static_cast<tracking_t *>(self)->run();
    return 0;
  }

  void tracking_t::report(const vr::DriverPose_t &p) {
    AcquireSRWLockShared(&ctx_.pose_lock);
    const uint32_t idx = ctx_.hmd_index.load();
    if (idx != k_invalid_index) {
      vr::VRServerDriverHost()->TrackedDevicePoseUpdated(idx, p, sizeof(vr::DriverPose_t));
    }
    ReleaseSRWLockShared(&ctx_.pose_lock);
    if (auto *h = hmd_.load()) {
      h->store_pose(p);
    }
  }

  void tracking_t::invalidate_controllers() {
    for (auto &c : ctrl_) {
      if (auto *d = c.load()) {
        d->invalidate();
      }
    }
  }

  void tracking_t::run() {
    uint64_t cur_gen = 0;
    uint64_t last_idx = UINT64_MAX;
    bool have = false;
    vripc_tracking_slot_t last {};
    int64_t last_new_qpc = 0;
    int64_t last_send_qpc = 0;
    bool stale = false;
    bool oor = false;
    bool invalid_sent = false;
    // 10 秒統計
    int64_t t0 = now_qpc();
    uint64_t n_new = 0, n_stale_sends = 0, n_stale = 0, n_oor = 0, n_invalid = 0, n_torn = 0;
    int64_t offset_min_us = INT64_MAX, offset_max_us = INT64_MIN;
    // 2026-10-05：HMD／控制器 offset 分布、新樣本到達間隔（含 > 2T 次數）、§VR-STALE-HOLD 次數
    pct_buf_t hmd_off_us, ctrl_off_us;
    int64_t arrival_max_us = 0;
    uint64_t n_arrival_gt2t = 0, n_hold = 0, n_hold_expired = 0;
    bool in_hold = false;
    const int64_t f = ctx_.qpf;

    for (;;) {
      if (degraded()) {
        break;
      }
      auto gen = ctx_.ipc ? ctx_.ipc->current() : generation_ptr {};
      const bool usable = gen && !gen->idle() && gen->evt_trk();
      if (!usable) {
        if (!invalid_sent && ctx_.hmd_index.load() != k_invalid_index) {
          // pipe 斷線：poseIsValid=false、OutOfRange，deviceIsConnected 維持 true
          bool ok = guarded("tracking.invalid", [&]() {
            vr::DriverPose_t p = base_pose();
            p.result = vr::TrackingResult_Running_OutOfRange;
            p.poseIsValid = false;
            report(p);
            invalidate_controllers();
          });
          if (!ok) {
            break;
          }
          invalid_sent = true;
          ++n_invalid;
          ctx_.presence.store(false);
          push_timing(ctx_, VRIPC_TE_STALE, 0, 3, 0);
        }
        have = false;
        if (WaitForSingleObject(stop_evt_, 10) == WAIT_OBJECT_0) {
          break;
        }
        continue;
      }
      if (gen->id() != cur_gen) {
        cur_gen = gen->id();
        last_idx = UINT64_MAX;
        have = false;
        stale = oor = false;
      }
      const vripc_session_config_t &cfg = gen->config();
      uint32_t zero_vel_us = cfg.stale_zero_vel_us != 0 ? cfg.stale_zero_vel_us : 22222;
      const DWORD wait_ms = zero_vel_us / 1000 > 0 ? zero_vel_us / 1000 : 1;
      HANDLE hs[2] = {stop_evt_, gen->evt_trk()};
      const DWORD w = WaitForMultipleObjects(2, hs, FALSE, wait_ms);
      if (w == WAIT_OBJECT_0) {
        break;
      }
      const int64_t now = now_qpc();

      bool ok = guarded("tracking", [&]() {
        if (!ctx_.active()) {
          // disarm／server heartbeat 逾時：standby
          if (!invalid_sent && ctx_.hmd_index.load() != k_invalid_index) {
            vr::DriverPose_t p = base_pose();
            p.result = vr::TrackingResult_Running_OutOfRange;
            p.poseIsValid = false;
            report(p);
            invalidate_controllers();
            invalid_sent = true;
            ++n_invalid;
            push_timing(ctx_, VRIPC_TE_STALE, 0, 3, 0);
          }
          ctx_.presence.store(false);
          return;
        }
        vripc_tracking_slot_t s {};
        uint64_t idx = 0;
        bool torn = false;
        const bool got = gen->read_latest_tracking(s, idx, &torn);
        if (torn) {
          ++n_torn;
        }
        const bool fresh = got && idx != last_idx && (s.flags & VIPLE_TRK_HMD_FLAG) && finite_pose(s.pose[0]);
        if (fresh) {
          if (have && last_new_qpc != 0) {
            // 新樣本到達間隔（driver 端；含 server 轉送）。> 2T 就是會觸發 stale 規則的空窗
            const int64_t gap_us = (now - last_new_qpc) * 1000000 / f;
            arrival_max_us = gap_us > arrival_max_us ? gap_us : arrival_max_us;
            if (gap_us > (int64_t) zero_vel_us) {
              ++n_arrival_gt2t;
            }
            if (stale || oor) {
              // gap_ms：距上一次 stale 重送；total_ms：整段空窗（上一張新樣本到這一張）
              VRDRV_LOG_INFO("tracking recovered gap_ms=%.1f total_ms=%.1f hold=%d", (double) (now - last_send_qpc) * 1000.0 / (double) f,
                             (double) gap_us / 1000.0, in_hold ? 1 : 0);
            }
          }
          last = s;
          last_idx = idx;
          have = true;
          last_new_qpc = now;
          stale = oor = false;
          in_hold = false;
        }
        if (!have) {
          return;
        }
        const int64_t age_us = (now - last_new_qpc) * 1000000 / f;
        const bool now_stale = age_us > (int64_t) zero_vel_us;
        const uint32_t oor_us = cfg.stale_oor_hmd_us != 0 ? cfg.stale_oor_hmd_us : 1000000;
        const bool now_oor = age_us > (int64_t) oor_us;
        if (!fresh && !now_stale) {
          return;  // 同一筆樣本已送過、還沒 stale：不重送
        }
        const uint32_t ctrl_oor_us = cfg.stale_oor_ctrl_us != 0 ? cfg.stale_oor_ctrl_us : 100000;
        // 速度與 poseTimeOffset 的規則見 pose_policy.h：
        //   §VR-CTRL-OFFSET（pose_flags bit0）：舊版控制器的 poseTimeOffset 一直是 0，SteamVR 把已經預測過的拍子姿態
        //     當成「現在」的姿態再外插一次（拍子超前約 25～31 ms）；開啟後與 HMD 用同一個目標時間。
        //   §VR-STALE-HOLD（pose_flags bit1）：2T < age ≤ stale_oor_ctrl_us（預設 100 ms）的短空窗保留速度與 offset，
        //     舊規則（速度與 offset 歸零）會讓 HMD 與拍子先往回跳、新樣本到了再往前跳。
        pose_policy_in_t pin;
        pin.age_us = age_us;
        pin.zero_vel_us = zero_vel_us;
        pin.hold_max_us = ctrl_oor_us;
        pin.ctrl_offset = (cfg.pose_flags & VRIPC_POSE_F_CTRL_OFFSET) != 0;
        pin.stale_hold = (cfg.pose_flags & VRIPC_POSE_F_STALE_HOLD) != 0;
        pin.raw_off_s = (double) (last.target_server_qpc - now) / (double) f;
        pin.hmd_cap_us = cfg.hmd_extrap_cap_us;
        pin.ctrl_cap_us = cfg.ctrl_extrap_cap_us;
        const pose_policy_t pol = decide_pose_policy(pin);
        const bool hold = pol.hold;
        const bool zero_vel = pol.zero_vel;
        const double ang_scale = pol.ang_scale;
        const double off = pol.hmd_off_s;
        const double ctrl_off = pol.ctrl_off_s;
        if (hold && !in_hold) {
          in_hold = true;
          ++n_hold;
        } else if (!hold && in_hold && now_stale) {
          in_hold = false;
          ++n_hold_expired;  // 空窗超過 hold 上限：回到舊規則
        }

        // S2-09：控制器跟 HMD 同一個節奏（新樣本或 stale 重送）；> stale_oor_ctrl_us（預設 100 ms）OutOfRange
        {
          const uint8_t hand_flag[2] = {0x02, 0x04};  // VIPLE_VR_TRK_LEFT／RIGHT
          for (int h = 0; h < 2; ++h) {
            if (auto *d = ctrl_[h].load()) {
              d->update(last.pose[1 + h], last.input[h], (last.flags & hand_flag[h]) != 0, age_us, ctrl_oor_us, zero_vel, ctrl_off, ang_scale,
                        (cfg.pose_flags & VRIPC_POSE_F_ANGVEL_LOCAL) != 0);
            }
          }
        }
        const vripc_pose_t &hp = last.pose[0];
        vr::DriverPose_t p = base_pose();
        p.vecPosition[0] = hp.pos[0];
        p.vecPosition[1] = hp.pos[1];
        p.vecPosition[2] = hp.pos[2];
        math::quat_t q = math::normalize(math::quat_t {hp.rot[0], hp.rot[1], hp.rot[2], hp.rot[3]});
        p.qRotation.x = q.x;
        p.qRotation.y = q.y;
        p.qRotation.z = q.z;
        p.qRotation.w = q.w;
        // H:546：SteamVR 的預測只靠我們給的速度；stale 期間速度歸零 = 關掉 SteamVR 外插（hold 時保留）
        if (!zero_vel) {
          math::vec3_t w {hp.ang_vel[0] * ang_scale, hp.ang_vel[1] * ang_scale, hp.ang_vel[2] * ang_scale};
          if ((cfg.pose_flags & VRIPC_POSE_F_ANGVEL_LOCAL) != 0) {
            // §VR-ANGVEL-LOCAL：SteamVR 以 q·exp(ω t) 外插（ω 是機體座標）；client 給的是世界座標 → ω_local = q⁻¹ ω q。
            // 舊版頭一傾斜（或拍子任何姿態）就繞錯的軸外插；pose_history 仍記世界座標（它以 exp(ω t)·q 外插，結果相同）
            w = math::rotate(math::conj(q), w);
          }
          for (int i = 0; i < 3; ++i) {
            p.vecVelocity[i] = hp.lin_vel[i];
          }
          p.vecAngularVelocity[0] = w.x;
          p.vecAngularVelocity[1] = w.y;
          p.vecAngularVelocity[2] = w.z;
        }
        p.poseTimeOffset = off;
        p.result = now_oor ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Running_OK;
        p.poseIsValid = true;
        report(p);
        invalid_sent = false;
        last_send_qpc = now;
        ctx_.presence.store((last.flags & VIPLE_TRK_PRESENCE_FLAG) != 0);
        if (fresh || hold) {
          // hold 重送也記進 pose_history：SteamVR 這段時間 render 的姿態是從重送的速度（角速度已衰減）外插出來的
          ph_sample_t hs;
          hs.sample_id = last.sample_id;
          hs.pose.rot = q;
          hs.pose.pos = {hp.pos[0], hp.pos[1], hp.pos[2]};
          hs.lin_vel = {hp.lin_vel[0], hp.lin_vel[1], hp.lin_vel[2]};
          hs.ang_vel = {hp.ang_vel[0] * ang_scale, hp.ang_vel[1] * ang_scale, hp.ang_vel[2] * ang_scale};
          hs.reported_qpc = now;
          hs.offset_s = off;
          ctx_.pose_hist.record(hs);
        }
        if (fresh) {
          ++n_new;
          const int64_t off_us = (int64_t) (off * 1e6);
          offset_min_us = off_us < offset_min_us ? off_us : offset_min_us;
          offset_max_us = off_us > offset_max_us ? off_us : offset_max_us;
          hmd_off_us.add(off_us);
          if ((cfg.pose_flags & VRIPC_POSE_F_CTRL_OFFSET) != 0) {
            ctrl_off_us.add((int64_t) (ctrl_off * 1e6));
          }
          push_timing(ctx_, VRIPC_TE_POSE_UPDATED, last.sample_id, last.sample_id, off_us);
        } else {
          ++n_stale_sends;
          if (!stale) {
            stale = true;
            ++n_stale;
            ctx_.ipc->status().stale_pose_count.fetch_add(1, std::memory_order_relaxed);
            push_timing(ctx_, VRIPC_TE_STALE, last.sample_id, 1, age_us);
          }
          if (now_oor && !oor) {
            oor = true;
            ++n_oor;
            ctx_.ipc->status().outofrange_count.fetch_add(1, std::memory_order_relaxed);
            push_timing(ctx_, VRIPC_TE_STALE, last.sample_id, 2, age_us);
            VRDRV_LOG_WARN("tracking hmd out-of-range age_ms=%lld", (long long) (age_us / 1000));
          }
        }
      });
      if (!ok) {
        break;
      }
      if (now - t0 >= 10 * f) {
        // 前段欄位順序不變（docs/log_tags.md）；2026-10-05 起尾端加 offset 分布、到達間隔、hold 與 pose_flags
        VRDRV_LOG_INFO("tracking 10s: new=%llu staleSends=%llu stale=%llu oor=%llu invalid=%llu torn=%llu offsetUs min=%lld max=%lld"
                       " p50=%d ctrlOffUs p50=%d p95=%d arrivalMaxMs=%.1f gt2T=%llu hold=%llu holdExpired=%llu poseFlags=0x%x",
                       (unsigned long long) n_new, (unsigned long long) n_stale_sends, (unsigned long long) n_stale, (unsigned long long) n_oor,
                       (unsigned long long) n_invalid, (unsigned long long) n_torn, (long long) (n_new ? offset_min_us : 0), (long long) (n_new ? offset_max_us : 0),
                       hmd_off_us.pct(0.50), ctrl_off_us.pct(0.50), ctrl_off_us.pct(0.95), (double) arrival_max_us / 1000.0,
                       (unsigned long long) n_arrival_gt2t, (unsigned long long) n_hold, (unsigned long long) n_hold_expired, cfg.pose_flags);
        t0 = now;
        n_new = n_stale_sends = n_stale = n_oor = n_invalid = n_torn = 0;
        offset_min_us = INT64_MAX;
        offset_max_us = INT64_MIN;
        hmd_off_us.v.clear();
        ctrl_off_us.v.clear();
        arrival_max_us = 0;
        n_arrival_gt2t = n_hold = n_hold_expired = 0;
      }
    }
  }

}  // namespace vrdrv
