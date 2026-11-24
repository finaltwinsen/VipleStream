#pragma once

#include <mutex>

struct ANativeWindow;

// §QXR-VIDEO-TEST：手機上自建 Vulkan device，不經過 OpenXR，把解碼畫面送進 QxrVideo。
class QxrVideoTest {
public:
    QxrVideoTest() = default;
    ~QxrVideoTest();

    QxrVideoTest(const QxrVideoTest&) = delete;
    QxrVideoTest& operator=(const QxrVideoTest&) = delete;

    // srgb 在建立時選定目標格式。解碼輸出的 Surface 不能中途換，所以不在跑起來之後重建。
    bool create(int streamW, int streamH, const char* outDir, bool srgb);
    // 多加一個 reference，呼叫端要 ANativeWindow_release。toSurface 另持有回傳 Surface 的引用，這份不能留在那裡。
    ANativeWindow* decoderWindow() const;
    void start();
    void stop();
    void destroy();

private:
    struct Impl;
    Impl* impl = nullptr;
    // §QXR-VIDEO-TEST：stop 與 destroy 可重入，跟 render 執行緒以外的 JNI 呼叫互斥。
    mutable std::mutex life;
};
