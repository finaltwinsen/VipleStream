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
#include "pose_history.h"
#include "vr_math.h"

namespace vrdrv {

  constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

  // 0x5506 tracking flags（VipleVr.h 的 VIPLE_VR_TRK_*；driver 不 include common-c，只取這兩個值）
  constexpr uint8_t VIPLE_TRK_HMD_FLAG = 0x01;
  constexpr uint8_t VIPLE_TRK_PRESENCE_FLAG = 0x10;

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

    // ── pose_history（S2-06：SubmitLayer 的 mHmdPose ↔ 回報過的樣本、space-delta）──
    pose_history_t pose_hist;

    // 目前是否應該對 SteamVR 呈現「有效 HMD」（否則 poseIsValid=false、/proximity=false、不合成）
    bool active() const {
      return link_up.load() && armed.load() && !server_stale.load();
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
