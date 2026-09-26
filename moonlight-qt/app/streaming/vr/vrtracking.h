/**
 * @file streaming/vr/vrtracking.h
 * @brief VipleStream 2.0 §VR：tracking 上行（0x5506）送出執行緒與樣本時間紀錄。
 *
 * M1a 只有合成 pose（--vr-emulate）；M4a 會把樣本來源換成 XrTrackingSource。
 * 頻率 = 顯示 Hz × VIPLE_VR_TRACKING_RATE_MUL，每筆經 LiSendVrTracking 送出
 * （ENet VR channel UNSEQUENCED，ENet 不通時 common-c 自動改走 QUIC）。
 *
 * 生命週期：LiStartConnection 成功後 start()，LiStopConnection 之前 stop()
 * （Session 的 DeferredSessionCleanupTask 涵蓋所有結束路徑）。
 */
#pragma once

#include "vrsynthetic.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

// sampleId → client 取樣時間。decoder 執行緒用它把 0x81 的 echoSampleId 換回取樣
// 時間，算「取樣 → 解出這一幀」的延遲（M1a 的 echo age；M4a 的 MTP 也從這裡起算）。
class VrSampleHistory {
public:
    static void reset();
    static void record(uint32_t sampleId, uint64_t sampleTimeNs);
    // 找不到（太舊被覆蓋，或從沒送過）回 false
    static bool lookup(uint32_t sampleId, uint64_t* sampleTimeNs);

private:
    static constexpr int kSize = 2048;  // 2×Hz 下約 11 秒
    struct Entry {
        uint32_t sampleId;
        uint64_t sampleTimeNs;
    };
    static std::mutex s_Lock;
    static Entry s_Entries[kSize];
};

class VrTrackingSender {
public:
    VrTrackingSender() = default;
    ~VrTrackingSender();

    VrTrackingSender(const VrTrackingSender&) = delete;
    VrTrackingSender& operator=(const VrTrackingSender&) = delete;

    // 重複呼叫無害；已在跑時忽略
    void start(int displayHz, VrSyntheticMotion motion);
    // join 送出執行緒並印 (final) 統計；重複呼叫無害
    void stop();

    // client 單調時鐘（奈秒）。tracking 的 sampleTimeNs 與 echo age 都用它
    static uint64_t nowNs();

private:
    void run();

    std::thread m_Thread;
    std::atomic<bool> m_Running { false };
    int m_DisplayHz = 0;
    VrSyntheticMotion m_Motion = VrSyntheticMotion::Sine;
};
