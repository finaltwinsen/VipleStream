#pragma once

// §QUEST-PCVR：Quest 頭與手的姿態來源。sample() 在上行執行緒，onFrameTiming() 在影格執行緒。

#include "qxr_pose_source.h"

#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <mutex>

struct timespec;

typedef XrResult(XRAPI_PTR* QxrConvertTimespecFn)(XrInstance instance, const struct timespec* timespecTime, XrTime* time);

class QxrXrControllers;

class QxrXrPoseSource final : public QxrPoseSource {
public:
    // stage：追蹤空間是 STAGE，遊玩區才查得到。timeExtensionEnabled 只拿來寫退路的 log。
    QxrXrPoseSource(XrInstance instance, XrSession session, XrSpace track, XrSpace view,
                    QxrXrControllers* controllers, QxrConvertTimespecFn convert, bool stage,
                    bool timeExtensionEnabled);
    ~QxrXrPoseSource() override = default;

    bool sample(VIPLE_VR_TRACKING* out, uint64_t nowUs) override;

    // 影格執行緒在 xrBeginSession 之後設 true，xrEndSession 之前設 false。
    // 上行執行緒靠這個避開對已結束 session 的 xrLocateSpace。
    void setSessionRunning(bool running);

    // 平滑「predictedDisplayTime − 現在」。換算不到時先用兩個顯示週期；sample() 看到結果 <= 0 才改用 20 ms。
    void onFrameTiming(XrTime predictedDisplayTime, int64_t predictedDisplayPeriodNs);
    void refreshPlayArea();

    void copyPlayArea(float* widthM, float* depthM) const;
    bool hasTimeConversion() const { return convert_ != nullptr; }

private:
    struct HeadSample {
        bool valid = false;
        float pos[3] = {};
        float rot[4] = {0.f, 0.f, 0.f, 1.f};
        float linVel[3] = {};
        float angVel[3] = {};
        uint32_t predictNs = 0;
    };

    bool nowXrTime(XrTime* out);
    bool locateHead(XrTime t, HeadSample* out);
    void writeHead(VIPLE_VR_TRACKING* out, const HeadSample& head) const;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace track_ = XR_NULL_HANDLE;
    XrSpace view_ = XR_NULL_HANDLE;
    QxrXrControllers* controllers_ = nullptr;
    QxrConvertTimespecFn convert_ = nullptr;
    bool stage_ = false;

    mutable std::mutex mu_;
    HeadSample cache_;
    float playWidthM_ = 0.f;
    float playDepthM_ = 0.f;
    // 預設 false：pose source 在 xrBeginSession 之前就建好，那時還不能 locate。
    std::atomic<bool> sessionRunning_{false};
    std::atomic<int64_t> predictAheadNs_{0};
    std::atomic<uint32_t> convertFails_{0};
    std::atomic<uint32_t> locateFails_{0};
    std::atomic<uint32_t> headInvalid_{0};
};
