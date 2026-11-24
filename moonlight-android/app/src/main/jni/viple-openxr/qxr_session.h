#pragma once

#include <jni.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct ANativeWindow;

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
