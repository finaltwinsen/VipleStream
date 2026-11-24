#pragma once

// §QUEST-PCVR：投影輸出的 LATCH（10 Hz）與 CLIENT_TIMING 用的 1 秒樣本。
// 模擬模式不建立這份填法，CLIENT_TIMING 維持原本的填 0。

#include <cstdint>
#include <mutex>
#include <vector>

class QxrSampleHistory;

struct QxrClientTimingFill {
    uint32_t framesPresented = 0;
    uint32_t xrMissed = 0;
    uint32_t metaMiss = 0;
    uint32_t displayPeriodNs = 0;
    uint16_t decodeP50Us = 0;
    uint16_t decodeP95Us = 0;
    uint16_t renderP50Us = 0;
    uint16_t renderP95Us = 0;
    int16_t slackP50Us = 0;
    int16_t slackP05Us = 0;
    uint16_t mtpP50_100us = 0;
    uint16_t mtpP95_100us = 0;
    bool haveDecode = false;
    bool haveRender = false;
    bool haveSlack = false;
    bool haveMtp = false;
};

class QxrLatch {
public:
    QxrLatch() = default;

    QxrLatch(const QxrLatch&) = delete;
    QxrLatch& operator=(const QxrLatch&) = delete;

    // history 不擁有。停掉上行時要先 unbind，frame thread 還可能在這一圈裡。
    void bind(QxrSampleHistory* history);
    void unbind();

    // 這一圈 xrWaitFrame 的預測顯示時刻。漏幀用相鄰兩次的差，跟 Frame 的 missed 同一條式子。
    void noteDisplay(int64_t predictedDisplayTime, int64_t periodNs);

    // 新的一張真的送進 projection。slack／decode 量不到就不進分位數，上一筆 LATCH 照送。
    void onNewFrame(uint32_t frameId, bool haveSlack, int32_t slackUs, bool haveRender, uint32_t renderUs,
                    bool hasMeta, uint32_t echoSampleId, uint64_t predictedDisplayClientNs, bool haveDecode,
                    uint32_t decodeUs);

    void noteMetaMiss();

    // frame thread 每圈呼叫。有樣本才每 100 ms 送一則。
    void tick(uint64_t nowNs, uint64_t periodNs);

    // 1 Hz 那條執行緒呼叫。把這一秒的樣本拿走。
    bool copyTiming(QxrClientTimingFill* out);

private:
    std::mutex mu;
    QxrSampleHistory* history = nullptr;
    bool enabled = false;
    bool used = false;

    bool haveLatch = false;
    uint32_t lastFrameId = 0;
    int32_t lastSlackUs = 0;
    uint64_t lastLatchSendNs = 0;

    uint32_t presentedTotal = 0;
    uint32_t metaMissTotal = 0;
    uint32_t xrMissed = 0;
    uint32_t latchFail = 0;
    int64_t lastDisplayTime = 0;
    uint32_t lastPeriodNs = 0;

    std::vector<int32_t> slack1s;
    std::vector<uint32_t> render1s;
    std::vector<uint32_t> mtp1s;
    std::vector<uint32_t> decode1s;
};
