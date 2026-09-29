// tracking.h - VipleStream §VR：tracking 執行緒（設計 §E.2 tracking；S2-07 V4 = 基本版，只有 HMD）。
//
// 專用執行緒等 evtTrk（逾時 = config.stale_zero_vel_us），讀最新 slot（§B.3），組 DriverPose_t 呼叫
// TrackedDevicePoseUpdated（OpenVR 範例與 ALVR 都在自己的執行緒呼叫；持 ctx.pose_lock 共享，Deactivate 以獨佔同步）。
// stale 政策：> 2T 沒新樣本 → 重送上一個 pose、速度歸零；HMD > stale_oor_hmd_us（預設 1 s）才 OutOfRange
// （OutOfRange 會讓 SteamVR 顯示灰畫面，短暫上行抖動不該讓整個畫面變灰）；
// 只有 pipe 斷線或 disarm（standby）才 poseIsValid=false，deviceIsConnected 一律維持 true（drv-M-3）。
#pragma once

#include <atomic>
#include <cstdint>

#include <windows.h>

#include <openvr_driver.h>

#include "driver_context.h"

namespace vrdrv {

  class hmd_device_t;

  class tracking_t {
  public:
    explicit tracking_t(driver_ctx_t &ctx);
    ~tracking_t();
    tracking_t(const tracking_t &) = delete;
    tracking_t &operator=(const tracking_t &) = delete;

    bool start();
    // 回傳 false：逾時沒 join（執行緒還在跑，物件不可釋放）
    bool stop(uint32_t join_ms);

    // RunFrame 在建立 HMD 後設定（之後不變）；tracking 執行緒只在 hmd_index 有效時使用
    void set_hmd(hmd_device_t *hmd) {
      hmd_.store(hmd);
    }

  private:
    static unsigned __stdcall entry(void *self);
    void run();
    void report(const vr::DriverPose_t &p);

    driver_ctx_t &ctx_;
    std::atomic<hmd_device_t *> hmd_ {nullptr};
    HANDLE thread_ = nullptr;
    HANDLE stop_evt_ = nullptr;
  };

}  // namespace vrdrv
