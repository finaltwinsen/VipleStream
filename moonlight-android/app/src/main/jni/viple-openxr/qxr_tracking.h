#pragma once

// §QUEST-VR：0x5506 上行。頻率、sampleId、環狀表跟 Qt vrtracking 對齊。
// 不搬 Tailscale 中繼降頻（Android 沒有那個訊號）；要降頻時呼叫 setRateDivisor。

#include "qxr_pose_source.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

class QxrSampleHistory {
public:
    static constexpr int kSize = 2048;

    void reset();
    // quatXyzw 是這一筆實際上行的 HMD 旋轉（xyzw，含預測）。null 時記單位旋轉。
    void record(uint32_t sampleId, uint64_t sampleTimeNs, const float quatXyzw[4]);
    // sampleId 0，或那一格已經被蓋掉，回 false。
    bool lookup(uint32_t sampleId, uint64_t* sampleTimeNs) const;
    // §VITURE-WARP：timewarp 的算圖基準必須是送出去的那一筆，不是融合器裡還沒預測的姿態。
    bool lookupPose(uint32_t sampleId, float q[4]) const;

private:
    struct Entry {
        uint32_t sampleId = 0;
        uint64_t sampleTimeNs = 0;
        float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    mutable std::mutex lock;
    Entry entries[kSize]{};
};

class QxrTrackingSender {
public:
    // source 與 history 不擁有。stop() 會 join，呼叫端要在放掉這兩個物件之前先 stop。
    QxrTrackingSender(QxrPoseSource* source, QxrSampleHistory* history);
    ~QxrTrackingSender();

    QxrTrackingSender(const QxrTrackingSender&) = delete;
    QxrTrackingSender& operator=(const QxrTrackingSender&) = delete;

    // 已在跑就忽略。displayHz <= 0 時用 72。
    void start(int displayHz, const char* sourceName);
    void stop();

    // 1＝每一拍都送。D>1 時每 D 拍送 1 筆；跳過的拍不取樣、不占用 sampleId。
    void setRateDivisor(int divisor);

private:
    void run();

    QxrPoseSource* source = nullptr;
    QxrSampleHistory* history = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<int> rateDivisor{1};
    int displayHz = 72;
    const char* sourceName = "sine";
};
