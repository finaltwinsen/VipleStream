#pragma once

#include <jni.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ANativeWindow;

class QxrPoseSource;
class QxrFrameMeta;
class QxrSampleHistory;
struct QxrClientTimingFill;

// §QUEST-PCVR：nativeGetViewConfig 的 33 個 float，順序跟 Java 註解相同。還沒量到時 viewConfig 回 false。
// 27 以後是尾端追加：坐姿零點，以及有沒有 XrTime 換算。既有 0..26 的索引不變。
struct QxrViewConfig {
    float eyeW = 0.f;
    float eyeH = 0.f;
    float refreshHz = 0.f;
    float leftAngleLeft = 0.f;
    float leftAngleRight = 0.f;
    float leftAngleUp = 0.f;
    float leftAngleDown = 0.f;
    float leftPosX = 0.f;
    float leftPosY = 0.f;
    float leftPosZ = 0.f;
    float leftRotX = 0.f;
    float leftRotY = 0.f;
    float leftRotZ = 0.f;
    float leftRotW = 1.f;
    float rightAngleLeft = 0.f;
    float rightAngleRight = 0.f;
    float rightAngleUp = 0.f;
    float rightAngleDown = 0.f;
    float rightPosX = 0.f;
    float rightPosY = 0.f;
    float rightPosZ = 0.f;
    float rightRotX = 0.f;
    float rightRotY = 0.f;
    float rightRotZ = 0.f;
    float rightRotW = 1.f;
    float playAreaWidthM = 0.f;
    float playAreaDepthM = 0.f;
    // 1＝下面四個有效。0＝量不到，呼叫端不要送 vrSeated。
    float seatedValid = 0.f;
    float seatedX = 0.f;
    float seatedY = 0.f;
    float seatedZ = 0.f;
    float seatedYawRad = 0.f;
    // 1＝XR_KHR_convert_timespec_time 可用（上行可離開影格執行緒）。0＝frameloop。
    float timeConversion = 0.f;
};

// Java onSessionEnded 的 reason。7、8 是 XrSessionState 的數值；instance loss 沒有對應的 session state。
constexpr int kQxrEndLossPending = 7;
constexpr int kQxrEndExiting = 8;
constexpr int kQxrEndInstanceLoss = 100;

struct QxrSessionCreateInfo {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;  // global ref，由呼叫端持有，這裡不釋放
    int32_t streamWidth = 0;
    int32_t streamHeight = 0;    // quad swapchain 尺寸
    int32_t preferredHz = 0;     // 0＝不要求
};

class QxrSession {
public:
    QxrSession();
    ~QxrSession();

    QxrSession(const QxrSession&) = delete;
    QxrSession& operator=(const QxrSession&) = delete;

    bool create(const QxrSessionCreateInfo& info, std::string* error);
    // §QUEST-PCVR：要在 start 之前。action set 一旦 session begin 就不能再掛。沒呼叫時畫面接線維持原本的 quad。
    bool enablePcvr(std::string* error);
    // §QUEST-PCVR：frame thread 建好 projection swapchain 才回。packed 是串流雙眼並排的寬高。
    bool armPcvrOutput(int32_t packedW, int32_t packedH, std::string* error);
    void bindPcvrTiming(QxrFrameMeta* meta, QxrSampleHistory* history);
    void unbindPcvrTiming();
    bool copyPcvrTiming(QxrClientTimingFill* out);
    bool viewConfig(QxrViewConfig* out) const;
    QxrPoseSource* pcvrPoseSource() const;
    void queuePcvrHaptic(int hand, int64_t durationNs, float frequencyHz, float amplitude);
    bool pcvrTimeConversion() const;
    ANativeWindow* decoderWindow();
    bool start(std::string* error);
    void stop();
    void destroy();
    std::vector<float> refreshRates() const;
    void recommendedEyeSize(uint32_t* w, uint32_t* h) const;
    void setEndedCallback(std::function<void(int)> callback);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;

    static void run(const std::shared_ptr<Impl>& impl);
    void release();
};
