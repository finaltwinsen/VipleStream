// driver_context.h - VipleStream §VR：driver 各元件共用的狀態（driver_main 擁有，Init 建、Cleanup 放）。
//
// 執行緒：
//  - ipc 執行緒（ipc_client callback）只改這裡的 atomic 旗標；SteamVR host API 只在 RunFrame／裝置回呼裡呼叫（§B.6）。
//  - tracking 執行緒呼叫 TrackedDevicePoseUpdated（持 pose_lock 共享；Deactivate 以獨佔取得後把 hmd_index 設成無效）。
//  - vrserver 的 compositor 執行緒呼叫 direct_mode（自己的 present 鎖）。
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

#include "ipc_client.h"
#include "vr_math.h"

namespace vrdrv {

  constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

  // 0x5506 tracking flags（VipleVr.h 的 VIPLE_VR_TRK_*；driver 不 include common-c，只取這兩個值）
  constexpr uint8_t VIPLE_TRK_HMD_FLAG = 0x01;
  constexpr uint8_t VIPLE_TRK_PRESENCE_FLAG = 0x10;

  // 回報給 SteamVR 的 HMD 樣本（echo 比對用；V5 的 pose_history 取代它）
  struct reported_sample_t {
    uint32_t sample_id = 0;
    math::quat_t rot;
    int64_t reported_qpc = 0;
  };

  struct driver_ctx_t {
    ipc_client_t *ipc = nullptr;
    int64_t qpf = 0;

    // ── ipc 執行緒 → RunFrame／tracking／direct_mode ──
    std::atomic<bool> armed {false};  // 目前 generation 的 header.armed 或最近的 ARM／DISARM
    std::atomic<bool> link_up {false};  // 有 generation（MAPPED 之後、TEARDOWN 之前）
    std::atomic<bool> server_stale {false};  // server heartbeat > 1 s
    std::atomic<bool> quit_pending {false};  // STATE REQUEST_QUIT
    std::atomic<uint32_t> v2p_pending {0};  // STATE DEV_SET_V2P 的 µs（0 = 沒有）
    std::atomic<uint32_t> link_events {0};  // on_mapped／on_teardown 次數（RunFrame 偵測變化用）

    // ── HMD ──
    std::atomic<uint32_t> hmd_index {k_invalid_index};  // Activate 後有效；Deactivate 設回無效
    std::atomic<bool> hmd_added {false};
    std::atomic<bool> presenting {false};  // 已送 HMD_PRESENTING（standby 時清掉）
    std::atomic<bool> presence {false};  // 最新樣本帶 PRESENCE（RunFrame 更新 /proximity）
    SRWLOCK pose_lock = SRWLOCK_INIT;

    // ── echo（SubmitLayer 的 mHmdPose ↔ 回報過的樣本）──
    static constexpr uint32_t k_echo_slots = 128;
    SRWLOCK echo_lock = SRWLOCK_INIT;
    reported_sample_t echo[k_echo_slots] {};
    uint32_t echo_head = 0;

    // 目前是否應該對 SteamVR 呈現「有效 HMD」（否則 poseIsValid=false、/proximity=false、不合成）
    bool active() const {
      return link_up.load() && armed.load() && !server_stale.load();
    }

    void remember_sample(uint32_t id, const math::quat_t &q, int64_t qpc) {
      AcquireSRWLockExclusive(&echo_lock);
      echo[echo_head % k_echo_slots] = reported_sample_t {id, q, qpc};
      ++echo_head;
      ReleaseSRWLockExclusive(&echo_lock);
    }

    // 最近 100 ms 內角距離最小的樣本；回傳角度（度），找不到回負值
    double find_echo(const math::quat_t &q, int64_t now, uint32_t &id_out) {
      double best = -1.0;
      AcquireSRWLockShared(&echo_lock);
      const uint32_t n = echo_head < k_echo_slots ? echo_head : k_echo_slots;
      for (uint32_t i = 0; i < n; ++i) {
        const auto &s = echo[i];
        if (s.sample_id == 0 || now - s.reported_qpc > qpf / 10) {
          continue;
        }
        const double a = math::angle_deg(q, s.rot);
        // 同角度時取較新的樣本
        if (best < 0.0 || a < best || (a == best && (int32_t) (s.sample_id - id_out) > 0)) {
          best = a;
          id_out = s.sample_id;
        }
      }
      ReleaseSRWLockShared(&echo_lock);
      return best;
    }
  };

  // timing ring：有 generation 才寫（失敗只計數，不影響熱路徑）
  inline void push_timing(driver_ctx_t &ctx, uint16_t event, uint32_t frame_id, int64_t arg0, int64_t arg1) {
    if (!ctx.ipc) {
      return;
    }
    auto gen = ctx.ipc->current();
    if (!gen) {
      return;
    }
    vripc_timing_rec_t r {};
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    r.qpc = v.QuadPart;
    r.frame_id = frame_id;
    r.event = event;
    r.arg0 = arg0;
    r.arg1 = arg1;
    gen->push_timing(r);
  }

}  // namespace vrdrv
