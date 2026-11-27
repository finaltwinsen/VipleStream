#include "qxr_framemeta.h"

#include "viple_vr_bridge.h"

#include <atomic>
#include <cmath>

namespace {
std::atomic<float> g_frameAgeEmaMs{0.0f};
// §VITURE-WARP：算圖姿態的來源統計（記在 10 秒的 META log）
std::atomic<uint32_t> warpFromRenderRot{0};
std::atomic<uint32_t> warpFromSent{0};
std::atomic<uint32_t> warpRenderRotRejected{0};

// §LOGHYG-VITURE（2026-11-27）：renderRot 與上傳那筆的實際夾角。這是唯一能分辨「固定座標偏移
// （例如 180°，p50 幾乎等於 max）」與「姿態真的一直在動、只是比錯筆（p50 遠小於 max）」的訊號。
// 只有比對過（renderRot 與 sent 都在）的幀才記；10 秒統計時取走。
std::mutex g_warpAngleLock;
std::vector<float> g_warpAngleDeg;
constexpr size_t kWarpAngleCap = 8192;

void noteWarpAngleDeg(float deg)
{
    std::lock_guard<std::mutex> guard(g_warpAngleLock);
    if (g_warpAngleDeg.size() < kWarpAngleCap) {
        g_warpAngleDeg.push_back(deg);
    }
}

void takeWarpAnglesDeg(std::vector<float>& out)
{
    std::lock_guard<std::mutex> guard(g_warpAngleLock);
    out.swap(g_warpAngleDeg);
}

void noteFrameAge(float ageMs)
{
    if (!(ageMs > 0.0f) || ageMs > 500.0f) {
        return;
    }
    const float prev = g_frameAgeEmaMs.load(std::memory_order_relaxed);
    g_frameAgeEmaMs.store(prev <= 0.0f ? ageMs : prev * 0.95f + ageMs * 0.05f, std::memory_order_relaxed);
}
}  // namespace

float qxrFrameAgeEmaMs()
{
    return g_frameAgeEmaMs.load(std::memory_order_relaxed);
}

#include <android/log.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <time.h>

namespace {

constexpr uint64_t kLogIntervalNs = 10000000000ull;
constexpr size_t kMaxTotalAges = 200000;

uint64_t monoNsNow()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

QxrFrameMeta::QxrFrameMeta(QxrSampleHistory* historyIn)
    : history(historyIn)
{
    intervalAgesMs.reserve(2048);
}

QxrFrameMeta::~QxrFrameMeta()
{
    std::vector<float> ages;
    Interval stats;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (total.frames == 0) {
            return;
        }
        stats = total;
        ages.swap(totalAgesMs);
    }
    logLine("(final)", stats, ages);
}

void QxrFrameMeta::logLine(const char* prefix, const Interval& stats, std::vector<float>& ages) const
{
    float p50 = 0.0f;
    float p95 = 0.0f;
    if (!ages.empty()) {
        std::sort(ages.begin(), ages.end());
        p50 = ages[ages.size() / 2];
        p95 = ages[std::min(ages.size() - 1, static_cast<size_t>(ages.size() * 0.95))];
    }
    const double headerPct = stats.frames > 0 ? 100.0 * static_cast<double>(stats.withHeader) / stats.frames : 0.0;
    const double echoPct = stats.withHeader > 0
                               ? 100.0 * static_cast<double>(stats.echoKnown) / stats.withHeader
                               : 0.0;
    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-META] %s frames=%u vrHeader=%.1f%% echoKnown=%.1f%% (%u/%u) latency p50=%.1fms p95=%.1fms",
                        prefix, stats.frames, headerPct, echoPct, stats.echoKnown, stats.withHeader, p50, p95);
    const uint32_t fromRr = warpFromRenderRot.exchange(0, std::memory_order_relaxed);
    const uint32_t fromSent = warpFromSent.exchange(0, std::memory_order_relaxed);
    const uint32_t rejected = warpRenderRotRejected.exchange(0, std::memory_order_relaxed);
    std::vector<float> warpAngles;
    takeWarpAnglesDeg(warpAngles);
    char angleBuf[112] = "";
    if (!warpAngles.empty()) {
        std::sort(warpAngles.begin(), warpAngles.end());
        const float a50 = warpAngles[warpAngles.size() / 2];
        const float aMax = warpAngles.back();
        std::snprintf(angleBuf, sizeof(angleBuf), " angleDeg p50=%.1f max=%.1f n=%u",
                      a50, aMax, static_cast<unsigned>(warpAngles.size()));
    }
    if (fromRr + fromSent + rejected > 0) {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VITURE] warp pose source: renderRot=%u sent=%u renderRotRejected=%u%s",
                            fromRr, fromSent, rejected, angleBuf);
    }
}

void QxrFrameMeta::setHistory(QxrSampleHistory* historyIn)
{
    std::lock_guard<std::mutex> guard(lock);
    history = historyIn;
}

bool QxrFrameMeta::lookup(int64_t frameNumber, VIPLE_VR_FRAME_META* meta, uint64_t* sinkMonoNs) const
{
    if (meta == nullptr || frameNumber < 0) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock);
    const uint32_t filled = ringWrite < static_cast<uint32_t>(kRing) ? ringWrite : static_cast<uint32_t>(kRing);
    for (uint32_t i = 0; i < filled; i++) {
        const uint32_t idx = (ringWrite - 1 - i) % static_cast<uint32_t>(kRing);
        if (!ring[idx].occupied || ring[idx].frameNumber != frameNumber) {
            continue;
        }
        *meta = ring[idx].meta;
        if (sinkMonoNs != nullptr) {
            *sinkMonoNs = ring[idx].sinkMonoNs;
        }
        return true;
    }
    return false;
}

bool QxrFrameMeta::lookupRenderPose(int64_t ptsUs, float q[4]) const
{
    if (q == nullptr || ptsUs < 0) {
        return false;
    }
    // §VITURE-WARP：跟 MediaCodecDecoderRenderer 一樣用整數除法。PTS＝frameNumber×8000，截斷才對得回同一個幀號。
    const int64_t frameNumber = ptsUs / static_cast<int64_t>(VIPLE_VR_BRIDGE_PTS_STEP_US);
    uint32_t echo = 0;
    QxrSampleHistory* hist = nullptr;
    float rr[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool haveRr = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        hist = history;
        const uint32_t filled = warpWrite < kWarpRing ? warpWrite : kWarpRing;
        bool found = false;
        for (uint32_t i = 0; i < filled; i++) {
            const uint32_t idx = (warpWrite - 1 - i) % kWarpRing;
            if (!warpRing[idx].occupied || warpRing[idx].frameNumber != frameNumber) {
                continue;
            }
            if (warpRing[idx].hasRenderRot) {
                for (int k = 0; k < 4; k++) {
                    rr[k] = warpRing[idx].renderRot[k];
                }
                haveRr = true;
            }
            echo = warpRing[idx].echoSampleId;
            found = true;
            break;
        }
        if (!found) {
            return false;
        }
    }
    float sent[4];
    const bool haveSent = hist != nullptr && echo != 0 && hist->lookupPose(echo, sent);
    // renderRot 是 SteamVR 實際算圖的旋轉，比上傳那筆多了它自己的預測，優先用。但 host 若對姿態加了固定偏移
    // （重新置中、座標轉換），它跟手機的姿態不在同一座標系，校正會整片轉歪；跟上傳那筆差超過 15° 就不信它。
    if (haveRr) {
        bool trust = true;
        if (haveSent) {
            const float dot = std::fabs(rr[0] * sent[0] + rr[1] * sent[1] + rr[2] * sent[2] + rr[3] * sent[3]);
            // §LOGHYG-VITURE：把實際夾角記下來（q 與 −q 已由 fabs 視為同一個旋轉）。夾角本身
            // 不代表比較方式有錯——座標系／重新置中是真的偏移、或是比到不同時間的姿態，都會讓它變大。
            // 這裡只量測、不改判斷（p50≈max 且接近固定值＝固定偏移；分布很寬＝姿態在動）。
            if (std::isfinite(dot)) {
                const float clamped = std::min(dot, 1.0f);
                const float angleDeg = 2.0f * std::acos(clamped) * 57.29577951f;
                noteWarpAngleDeg(angleDeg);
            }
            trust = dot >= 0.9914f;  // cos(15°/2)
        }
        if (trust) {
            for (int k = 0; k < 4; k++) {
                q[k] = rr[k];
            }
            warpFromRenderRot.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        warpRenderRotRejected.fetch_add(1, std::memory_order_relaxed);
    }
    if (!haveSent) {
        return false;
    }
    for (int k = 0; k < 4; k++) {
        q[k] = sent[k];
    }
    warpFromSent.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void QxrFrameMeta::onFrame(int frameNumber, const VIPLE_VR_FRAME_META* meta)
{
    // 到達時刻跟追蹤環同一顆單調時鐘。bridge 給的 receiveTimeUs 起點不同，不能拿來相減。
    const uint64_t arriveNs = monoNsNow();

    std::vector<float> agesToLog;
    Interval toLog;
    bool doLog = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        interval.frames++;
        total.frames++;
        const bool header = meta != nullptr && meta->present != 0;
        if (header) {
            interval.withHeader++;
            total.withHeader++;
            uint64_t sampleNs = 0;
            if (history != nullptr && history->lookup(meta->echoSampleId, &sampleNs)) {
                interval.echoKnown++;
                total.echoKnown++;
                if (arriveNs >= sampleNs) {
                    const float ageMs = static_cast<float>(static_cast<double>(arriveNs - sampleNs) / 1e6);
                    intervalAgesMs.push_back(ageMs);
                    noteFrameAge(ageMs);
                    if (totalAgesMs.size() < kMaxTotalAges) {
                        totalAgesMs.push_back(ageMs);
                    }
                }
            }
        }
        // §QUEST-PCVR：回呼返回後 meta 就不能再讀。幀號是影像 PTS 除回來的那一個，projection 用它配姿態。
        {
            Slot& slot = ring[ringWrite % kRing];
            slot.frameNumber = frameNumber;
            slot.occupied = true;
            slot.sinkMonoNs = arriveNs;
            if (meta != nullptr) {
                slot.meta = *meta;
            }
            else {
                slot.meta = {};
            }
            ringWrite++;
        }
        // §VITURE-WARP：沒有 header 也佔一格，查的時候才知道這幀到過、只是沒有 echo。
        {
            WarpSlot& slot = warpRing[warpWrite % kWarpRing];
            slot.frameNumber = frameNumber;
            slot.echoSampleId = (meta != nullptr && meta->present != 0) ? meta->echoSampleId : 0;
            slot.hasRenderRot = meta != nullptr && meta->present != 0 && (meta->vrFlags & VIPLE_VR_FF_POSE_FALLBACK) == 0;
            if (slot.hasRenderRot) {
                for (int k = 0; k < 4; k++) {
                    slot.renderRot[k] = meta->renderRot[k];
                }
            }
            slot.occupied = true;
            warpWrite++;
        }
        if (lastLogNs == 0) {
            lastLogNs = arriveNs;
        }
        else if (arriveNs - lastLogNs >= kLogIntervalNs) {
            doLog = true;
            toLog = interval;
            agesToLog.swap(intervalAgesMs);
            interval = Interval();
            intervalAgesMs.clear();
            intervalAgesMs.reserve(2048);
            lastLogNs = arriveNs;
        }
    }
    if (doLog) {
        logLine("10s:", toLog, agesToLog);
    }
}
