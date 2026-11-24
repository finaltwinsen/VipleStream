#pragma once

// §QUEST-VR：0x81 與追蹤環的配對。延遲用同一顆 CLOCK_MONOTONIC：
// 姿態送出前記下的時刻，到 frame sink 被叫到的時刻。不能跟 LiGetMicroseconds 相減。

#include "qxr_tracking.h"

#include <cstdint>
#include <mutex>
#include <vector>

class QxrFrameMeta {
public:
    // history 不擁有。null＝這條路徑還沒有追蹤環，echoKnown 維持 0。
    explicit QxrFrameMeta(QxrSampleHistory* history);

    ~QxrFrameMeta();

    QxrFrameMeta(const QxrFrameMeta&) = delete;
    QxrFrameMeta& operator=(const QxrFrameMeta&) = delete;

    // frame sink 呼叫。不可阻塞，不可呼叫 VipleVrBridgeSet*。
    void onFrame(int frameNumber, const VIPLE_VR_FRAME_META* meta);

private:
    struct Interval {
        uint32_t frames = 0;
        uint32_t withHeader = 0;
        uint32_t echoKnown = 0;
    };

    void logLine(const char* prefix, const Interval& stats, std::vector<float>& ages) const;

    QxrSampleHistory* history = nullptr;
    std::mutex lock;
    Interval interval;
    Interval total;
    std::vector<float> intervalAgesMs;
    std::vector<float> totalAgesMs;
    uint64_t lastLogNs = 0;
};
