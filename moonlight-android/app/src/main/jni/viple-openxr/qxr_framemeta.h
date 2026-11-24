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

    // §QUEST-PCVR：上行的環比 session 晚建立，停掉時也先摘掉。onFrame 跟這裡同一把鎖，避免讀到半套的指標。
    void setHistory(QxrSampleHistory* historyIn);

    // §QUEST-PCVR：影像幀號對 0x81。sink 返回後 meta 指標就失效，所以 onFrame 先複製。
    // 回 false＝這張還沒進環（遺失或解碼比封包早）。present 仍可能是 0，呼叫端要再看。
    bool lookup(int64_t frameNumber, VIPLE_VR_FRAME_META* meta, uint64_t* sinkMonoNs) const;

private:
    struct Interval {
        uint32_t frames = 0;
        uint32_t withHeader = 0;
        uint32_t echoKnown = 0;
    };

    // 120 Hz 約 4 秒。解碼通常晚幾個幀號才出來，用不到更長的。
    static constexpr int kRing = 512;
    struct Slot {
        int64_t frameNumber = -1;
        bool occupied = false;
        uint64_t sinkMonoNs = 0;
        VIPLE_VR_FRAME_META meta{};
    };

    void logLine(const char* prefix, const Interval& stats, std::vector<float>& ages) const;

    QxrSampleHistory* history = nullptr;
    Slot ring[kRing]{};
    uint32_t ringWrite = 0;
    // §QUEST-PCVR：lookup 給 frame thread 用，簽章是 const；鎖仍要能在 const 裡拿。
    mutable std::mutex lock;
    Interval interval;
    Interval total;
    std::vector<float> intervalAgesMs;
    std::vector<float> totalAgesMs;
    uint64_t lastLogNs = 0;
};
