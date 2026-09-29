// tracking.cpp - 見 tracking.h（設計 §E.2 tracking、§F.3）。
#include <openvr_driver.h>

#include "tracking.h"

#include <cmath>
#include <process.h>

#include "controller_device.h"
#include "driver_log.h"
#include "hmd_device.h"
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
          last = s;
          last_idx = idx;
          have = true;
          last_new_qpc = now;
          if (stale || oor) {
            VRDRV_LOG_INFO("tracking recovered gap_ms=%.1f", (double) (now - last_send_qpc) * 1000.0 / (double) f);
          }
          stale = oor = false;
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
        // S2-09：控制器跟 HMD 同一個節奏（新樣本或 stale 重送）；> stale_oor_ctrl_us（預設 100 ms）OutOfRange
        {
          const uint32_t ctrl_oor_us = cfg.stale_oor_ctrl_us != 0 ? cfg.stale_oor_ctrl_us : 100000;
          const uint8_t hand_flag[2] = {0x02, 0x04};  // VIPLE_VR_TRK_LEFT／RIGHT
          for (int h = 0; h < 2; ++h) {
            if (auto *d = ctrl_[h].load()) {
              d->update(last.pose[1 + h], last.input[h], (last.flags & hand_flag[h]) != 0, age_us, ctrl_oor_us, now_stale);
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
        // H:546：SteamVR 的預測只靠我們給的速度；stale 期間速度歸零 = 關掉 SteamVR 外插
        if (!now_stale) {
          for (int i = 0; i < 3; ++i) {
            p.vecVelocity[i] = hp.lin_vel[i];
            p.vecAngularVelocity[i] = hp.ang_vel[i];
          }
        }
        // poseTimeOffset = clamp((target − now)/freq, −0.05, cap)
        const double cap = (double) (cfg.hmd_extrap_cap_us != 0 ? cfg.hmd_extrap_cap_us : 50000) / 1e6;
        double off = (double) (last.target_server_qpc - now) / (double) f;
        if (!std::isfinite(off) || off < -0.05) {
          off = -0.05;
        }
        if (off > cap) {
          off = cap;
        }
        if (now_stale) {
          off = 0.0;
        }
        p.poseTimeOffset = off;
        p.result = now_oor ? vr::TrackingResult_Running_OutOfRange : vr::TrackingResult_Running_OK;
        p.poseIsValid = true;
        report(p);
        invalid_sent = false;
        last_send_qpc = now;
        ctx_.presence.store((last.flags & VIPLE_TRK_PRESENCE_FLAG) != 0);
        if (fresh) {
          ++n_new;
          ph_sample_t hs;
          hs.sample_id = last.sample_id;
          hs.pose.rot = q;
          hs.pose.pos = {hp.pos[0], hp.pos[1], hp.pos[2]};
          hs.lin_vel = {hp.lin_vel[0], hp.lin_vel[1], hp.lin_vel[2]};
          hs.ang_vel = {hp.ang_vel[0], hp.ang_vel[1], hp.ang_vel[2]};
          hs.reported_qpc = now;
          hs.offset_s = off;
          ctx_.pose_hist.record(hs);
          const int64_t off_us = (int64_t) (off * 1e6);
          offset_min_us = off_us < offset_min_us ? off_us : offset_min_us;
          offset_max_us = off_us > offset_max_us ? off_us : offset_max_us;
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
        VRDRV_LOG_INFO("tracking 10s: new=%llu staleSends=%llu stale=%llu oor=%llu invalid=%llu torn=%llu offsetUs min=%lld max=%lld",
                       (unsigned long long) n_new, (unsigned long long) n_stale_sends, (unsigned long long) n_stale, (unsigned long long) n_oor,
                       (unsigned long long) n_invalid, (unsigned long long) n_torn, (long long) (n_new ? offset_min_us : 0), (long long) (n_new ? offset_max_us : 0));
        t0 = now;
        n_new = n_stale_sends = n_stale = n_oor = n_invalid = n_torn = 0;
        offset_min_us = INT64_MAX;
        offset_max_us = INT64_MIN;
      }
    }
  }

}  // namespace vrdrv
