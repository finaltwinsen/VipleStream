#include "qxr_framemeta.h"

#include <android/log.h>

#include <algorithm>
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
}

void QxrFrameMeta::onFrame(int frameNumber, const VIPLE_VR_FRAME_META* meta)
{
    (void)frameNumber;
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
                    if (totalAgesMs.size() < kMaxTotalAges) {
                        totalAgesMs.push_back(ageMs);
                    }
                }
            }
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
