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

private:
    void logSkip(uint64_t nowUs);

    std::mutex mu;
    bool has = false;
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float w[3] = {0.0f, 0.0f, 0.0f};
    int64_t sourceNs = 0;
    uint64_t lastLogNs = 0;
};
