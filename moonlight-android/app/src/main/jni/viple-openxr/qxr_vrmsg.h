#pragma once

// §QUEST-VR：1 Hz CLIENT_TIMING，以及 0x5508 的 TLV 拆包。
// 模擬模式沒有 XR frame latch，不送 LATCH。Qt 的 vrEmulate 也進不了送 LATCH 的那個函式。

#include "qxr_latch.h"

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

    // VIPLE_VR_STATE_ERROR 只通知一次。回呼在 message sink 裡，不可同步拆連線或呼叫 VipleVrBridgeSet*。
    // 沒掛回呼時（模擬、IMU）這個 subtype 維持原本的 first-seen log，不結束任何東西。
    using ServerErrorFn = std::function<void(uint16_t code)>;
    void setServerErrorCallback(ServerErrorFn fn);

    // §QUEST-PCVR：只有投影輸出會掛。沒掛的時候 sendTiming 維持模擬那組 0，不要讓兩條路互相改。
    using TimingFillFn = std::function<bool(QxrClientTimingFill*)>;
    void setTimingFill(TimingFillFn fn);

    // §QUEST-RATE：0x5508 的 RATE_APPLIED 要交回投影的 frame thread。模擬沒掛這個回呼，拆包維持原樣。
    // 行程裡只有一個投影 session 會掛；回呼不可阻塞，也不可再呼叫 setRateHandler。
    using RateAppliedFn = std::function<void(int div, bool accepted, uint16_t seq)>;
    static void setRateHandler(RateAppliedFn fn);

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
    ServerErrorFn serverErrorFn;
    TimingFillFn timingFill;
    bool haveEventId[2] = {};
    uint32_t lastEventId[2] = {};
    uint32_t subtypeCount[256] = {};
    bool subtypeSeen[256] = {};
    bool serverErrorReported = false;
    uint32_t timingSent = 0;
    uint32_t timingFail = 0;
    uint32_t s2cEntries = 0;
    uint32_t hapticOk = 0;
    uint32_t hapticDup = 0;
    uint32_t hapticExpired = 0;
};
