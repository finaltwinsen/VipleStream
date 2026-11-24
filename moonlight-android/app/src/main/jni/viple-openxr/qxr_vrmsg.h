#pragma once

// §QUEST-VR：1 Hz CLIENT_TIMING，以及 0x5508 的 TLV 拆包。
// 模擬模式沒有 XR frame latch，不送 LATCH。Qt 的 vrEmulate 也進不了送 LATCH 的那個函式。

#include <Limelight.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

class QxrVrMsg {
public:
    using HapticFn = std::function<void(const VIPLE_VR_TLV_HAPTIC&)>;

    explicit QxrVrMsg(int displayHz);
    ~QxrVrMsg();

    QxrVrMsg(const QxrVrMsg&) = delete;
    QxrVrMsg& operator=(const QxrVrMsg&) = delete;

    // 要在登記 message sink 之前設好。回呼不可阻塞，也不可呼叫 VipleVrBridgeSet*。
    void setHapticCallback(HapticFn fn);

    void start();
    void stop();

    // message sink 呼叫。一包裡可以有多筆 TLV。
    void onMessage(const uint8_t* tlv, int length);

private:
    void run();
    void sendTiming();
    void logSummary(const char* prefix);

    int displayHz = 72;
    std::thread thread;
    std::atomic<bool> running{false};
    bool startedOnce = false;

    std::mutex lock;
    HapticFn hapticFn;
    bool haveEventId[2] = {};
    uint32_t lastEventId[2] = {};
    uint32_t subtypeCount[256] = {};
    bool subtypeSeen[256] = {};
    uint32_t timingSent = 0;
    uint32_t timingFail = 0;
    uint32_t s2cEntries = 0;
    uint32_t hapticOk = 0;
    uint32_t hapticDup = 0;
};
