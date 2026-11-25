#pragma once

// §VITURE：眼鏡 IMU 融合後的姿態。sample() 在上行執行緒，push() 在 USB 執行緒。
// 兩條執行緒只共用最新一筆，鎖的時間就是複製那幾個 float。

#include "qxr_pose_source.h"

#include <cstdint>
#include <mutex>

class QxrImuPoseSource final : public QxrPoseSource {
public:
    void push(float qw, float qx, float qy, float qz, float wx, float wy, float wz, int64_t sourceTimeNs);

    bool sample(VIPLE_VR_TRACKING* out, uint64_t nowUs) override;

    // §VITURE-WARP：timewarp 要的是「眼鏡即將亮起」的頭，不是上行那筆已經往前預測的姿態
    // （那筆在 sample history 裡）。從最新融合姿態，用平滑角速度外插到 now + displayAheadNs。
    bool currentPose(float q[4], int64_t displayAheadNs);

    // §VITURE-TRIGGER：手機畫面上的扳機鈕。眼鏡沒有實體手把，右手改跟著頭的方向，
    // 看著選項按下去就是點選（跟 Quest 不拿手把時的操作一樣）。
    void setRightController(uint32_t buttons, float trigger);

private:
    void logSkip(uint64_t nowUs);

    std::mutex mu;
    bool has = false;
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float w[3] = {0.0f, 0.0f, 0.0f};
    float wSmooth[3] = {0.0f, 0.0f, 0.0f};  // §VITURE-PREDICT：往前預測用的平滑角速度
    uint64_t lastPredictLogNs = 0;
    int64_t sourceNs = 0;
    uint64_t lastLogNs = 0;
    uint32_t rightButtons = 0;
    uint16_t rightTrigger = 0;
};
