#include "qxr_latch.h"

#include "qxr_log.h"
#include "qxr_tracking.h"

#include <Limelight.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint64_t kLatchIntervalNs = 100000000ull;
constexpr size_t kSampleCap = 4096;

template <typename T>
T pctOf(std::vector<T> values, double p)
{
    if (values.empty()) {
        return T{};
    }
    const size_t idx = std::min(values.size() - 1, static_cast<size_t>(p * static_cast<double>(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx), values.end());
    return values[idx];
}

uint16_t sat16u(uint64_t value)
{
    return static_cast<uint16_t>(std::min<uint64_t>(value, 0xffffu));
}

int16_t sat16s(int64_t value)
{
    return static_cast<int16_t>(std::max<int64_t>(-32768, std::min<int64_t>(value, 32767)));
}

template <typename T>
void pushCap(std::vector<T>* values, T sample)
{
    if (values->size() >= kSampleCap) {
        values->erase(values->begin());
    }
    values->push_back(sample);
}

}  // namespace

void QxrLatch::bind(QxrSampleHistory* historyIn)
{
    std::lock_guard<std::mutex> guard(mu);
    history = historyIn;
    enabled = true;
    used = true;
    haveLatch = false;
    lastFrameId = 0;
    lastSlackUs = 0;
    lastLatchSendNs = 0;
    presentedTotal = 0;
    metaMissTotal = 0;
    xrMissed = 0;
    latchFail = 0;
    lastDisplayTime = 0;
    lastPeriodNs = 0;
    slack1s.clear();
    render1s.clear();
    mtp1s.clear();
    decode1s.clear();
}

void QxrLatch::unbind()
{
    std::lock_guard<std::mutex> guard(mu);
    // 物件還在，1 Hz 還可能來抄最後一筆。只是不再收新樣本、也不再送 LATCH。
    enabled = false;
    history = nullptr;
}

void QxrLatch::noteDisplay(int64_t predictedDisplayTime, int64_t periodNs)
{
    std::lock_guard<std::mutex> guard(mu);
    if (!enabled) {
        return;
    }
    if (periodNs > 0) {
        const uint64_t capped = static_cast<uint64_t>(periodNs);
        lastPeriodNs = capped > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(capped);
    }
    if (lastDisplayTime != 0 && periodNs > 0 && predictedDisplayTime > lastDisplayTime) {
        const uint64_t delta = static_cast<uint64_t>(predictedDisplayTime - lastDisplayTime);
        const uint64_t period = static_cast<uint64_t>(periodNs);
        // 跟 xrcontext.cpp 3776–3784 同一條：相差超過 1.5 個週期就當成中間漏了幾格。
        if (delta > period + period / 2) {
            const uint64_t lost = (delta + period / 2) / period - 1;
            const uint64_t sum = static_cast<uint64_t>(xrMissed) + lost;
            xrMissed = sum > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(sum);
        }
    }
    lastDisplayTime = predictedDisplayTime;
}

void QxrLatch::noteMetaMiss()
{
    std::lock_guard<std::mutex> guard(mu);
    if (!enabled) {
        return;
    }
    if (metaMissTotal != 0xffffffffu) {
        metaMissTotal++;
    }
}

void QxrLatch::onNewFrame(uint32_t frameId, bool haveSlack, int32_t slackUs, bool haveRender, uint32_t renderUs,
                          bool hasMeta, uint32_t echoSampleId, uint64_t predictedDisplayClientNs, bool haveDecode,
                          uint32_t decodeUs)
{
    // 補包剛放行的那幾張，到達時刻不是 host 的節拍。LATCH 樣本留上一筆，統計仍記。
    const bool repairQuiet = LiGetVrRepairQuietMs() < 60u;
    QxrSampleHistory* historyCopy = nullptr;
    {
        std::lock_guard<std::mutex> guard(mu);
        if (!enabled) {
            return;
        }
        if (presentedTotal != 0xffffffffu) {
            presentedTotal++;
        }
        if (haveRender) {
            pushCap(&render1s, renderUs);
        }
        if (haveDecode) {
            pushCap(&decode1s, decodeUs);
        }
        if (haveSlack) {
            pushCap(&slack1s, slackUs);
            if (!repairQuiet) {
                lastSlackUs = slackUs;
                lastFrameId = frameId;
                haveLatch = true;
            }
        }
        historyCopy = history;
    }

    uint64_t sampleNs = 0;
    const bool matched = hasMeta && historyCopy != nullptr && historyCopy->lookup(echoSampleId, &sampleNs) &&
                         predictedDisplayClientNs > sampleNs;
    if (!matched) {
        return;
    }
    const uint64_t mtpUs = (predictedDisplayClientNs - sampleNs) / 1000ull;
    // 超過 2 秒當成樣本對不上，跟 Frame 一樣不列入。
    if (mtpUs >= 2000000ull || mtpUs > 0xffffffffu) {
        return;
    }
    std::lock_guard<std::mutex> guard(mu);
    if (!enabled) {
        return;
    }
    pushCap(&mtp1s, static_cast<uint32_t>(mtpUs));
}

void QxrLatch::tick(uint64_t nowNs, uint64_t periodNs)
{
    uint32_t frameId = 0;
    int32_t slackUs = 0;
    uint32_t period = 0;
    bool send = false;
    {
        std::lock_guard<std::mutex> guard(mu);
        if (!enabled || !haveLatch) {
            return;
        }
        // 截止時間是固定間距。落後超過一個間隔就對齊到現在，幀間隔抖動才不會把 10 Hz 拖成 8 Hz。
        if (lastLatchSendNs != 0 && nowNs < lastLatchSendNs) {
            return;
        }
        if (lastLatchSendNs == 0 || nowNs - lastLatchSendNs >= kLatchIntervalNs) {
            lastLatchSendNs = nowNs + kLatchIntervalNs;
        }
        else {
            lastLatchSendNs += kLatchIntervalNs;
        }
        frameId = lastFrameId;
        slackUs = lastSlackUs;
        const uint64_t capped = std::min<uint64_t>(periodNs, 0xffffffffu);
        period = capped != 0 ? static_cast<uint32_t>(capped) : lastPeriodNs;
        send = true;
    }
    if (!send) {
        return;
    }

    VIPLE_VR_TLV_LATCH latch{};
    latch.frameId = frameId;
    latch.slackUs = slackUs;
    latch.displayPeriodNs = period;
    uint8_t tlv[2 + sizeof(latch)];
    tlv[0] = VIPLE_VR_C2S_LATCH;
    tlv[1] = static_cast<uint8_t>(sizeof(latch));
    std::memcpy(tlv + 2, &latch, sizeof(latch));
    const int rc = LiSendVrMessage(tlv, static_cast<int>(sizeof(tlv)), false);
    if (rc != 0) {
        uint32_t fails = 0;
        {
            std::lock_guard<std::mutex> guard(mu);
            if (latchFail != 0xffffffffu) {
                latchFail++;
            }
            fails = latchFail;
        }
        if (fails == 1 || (fails % 100) == 0) {
            QXR_LOGW("LATCH", "LiSendVrMessage failed rc=%d frame=%u fails=%u", rc, frameId, fails);
        }
    }
}

bool QxrLatch::copyTiming(QxrClientTimingFill* out)
{
    if (out == nullptr) {
        return false;
    }
    std::vector<int32_t> slack;
    std::vector<uint32_t> render;
    std::vector<uint32_t> mtp;
    std::vector<uint32_t> decode;
    {
        std::lock_guard<std::mutex> guard(mu);
        if (!used) {
            return false;
        }
        out->framesPresented = presentedTotal;
        out->xrMissed = xrMissed;
        out->metaMiss = metaMissTotal;
        out->displayPeriodNs = lastPeriodNs;
        slack.swap(slack1s);
        render.swap(render1s);
        mtp.swap(mtp1s);
        decode.swap(decode1s);
    }
    out->haveDecode = !decode.empty();
    out->haveRender = !render.empty();
    out->haveSlack = !slack.empty();
    out->haveMtp = !mtp.empty();
    if (out->haveDecode) {
        out->decodeP50Us = sat16u(pctOf(decode, 0.50));
        out->decodeP95Us = sat16u(pctOf(decode, 0.95));
    }
    if (out->haveRender) {
        out->renderP50Us = sat16u(pctOf(render, 0.50));
        out->renderP95Us = sat16u(pctOf(render, 0.95));
    }
    if (out->haveSlack) {
        out->slackP50Us = sat16s(pctOf(slack, 0.50));
        out->slackP05Us = sat16s(pctOf(slack, 0.05));
    }
    if (out->haveMtp) {
        out->mtpP50_100us = sat16u(pctOf(mtp, 0.50) / 100u);
        out->mtpP95_100us = sat16u(pctOf(mtp, 0.95) / 100u);
    }
    return true;
}
