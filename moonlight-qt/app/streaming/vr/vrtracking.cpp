#include "vrtracking.h"

#include <SDL.h>

#include <chrono>
#include <cstring>

std::mutex VrSampleHistory::s_Lock;
VrSampleHistory::Entry VrSampleHistory::s_Entries[VrSampleHistory::kSize];

void VrSampleHistory::reset()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    memset(s_Entries, 0, sizeof(s_Entries));
}

void VrSampleHistory::record(uint32_t sampleId, uint64_t sampleTimeNs)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    Entry& e = s_Entries[sampleId % kSize];
    e.sampleId = sampleId;
    e.sampleTimeNs = sampleTimeNs;
}

bool VrSampleHistory::lookup(uint32_t sampleId, uint64_t* sampleTimeNs)
{
    if (sampleId == 0) {
        return false;  // 0 保留為「無」
    }
    std::lock_guard<std::mutex> lock(s_Lock);
    const Entry& e = s_Entries[sampleId % kSize];
    if (e.sampleId != sampleId) {
        return false;
    }
    *sampleTimeNs = e.sampleTimeNs;
    return true;
}

VrTrackingSender::~VrTrackingSender()
{
    stop();
}

namespace {
std::atomic<uint64_t> s_GridNs{0};
std::atomic<uint64_t> s_GridPeriodNs{0};
std::atomic<int64_t> s_PhaseNs{0};
}

void VrTrackingSender::setDisplayGrid(uint64_t displayNs, uint64_t periodNs)
{
    s_GridNs.store(displayNs, std::memory_order_relaxed);
    s_GridPeriodNs.store(periodNs, std::memory_order_relaxed);
}

void VrTrackingSender::setPhaseNs(int64_t phaseNs)
{
    s_PhaseNs.store(phaseNs, std::memory_order_relaxed);
}

uint64_t VrTrackingSender::nowNs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void VrTrackingSender::start(int displayHz, VrSyntheticMotion motion)
{
    if (m_Running.load()) {
        return;
    }
    m_DisplayHz = displayHz > 0 ? displayHz : 90;
    s_GridPeriodNs.store(0, std::memory_order_relaxed);  // 上一個 session 的節拍不沿用
    s_PhaseNs.store(0, std::memory_order_relaxed);
    m_Motion = motion;
    VrSampleHistory::reset();
    m_Running.store(true);
    m_Thread = std::thread(&VrTrackingSender::run, this);
}

void VrTrackingSender::stop()
{
    if (!m_Thread.joinable()) {
        return;
    }
    m_Running.store(false);
    m_Thread.join();
}

void VrTrackingSender::run()
{
    using clock = std::chrono::steady_clock;

    const int rateHz = m_DisplayHz * VIPLE_VR_TRACKING_RATE_MUL;
    const auto period = std::chrono::nanoseconds(1000000000LL / rateHz);
    // 合成 pose 沒有真的顯示時間，predictNs 固定用 3 個顯示週期（約等於 M1a 的管線延遲量級）
    const uint32_t predictNs = (uint32_t)(3LL * 1000000000LL / m_DisplayHz);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-POSE] tracking sender started: %d Hz (display %d Hz × %d), source=%s",
                rateHz, m_DisplayHz, VIPLE_VR_TRACKING_RATE_MUL,
                m_Source ? (m_SourceName ? m_SourceName : "xr") : vrSyntheticMotionName(m_Motion));

    const auto start = clock::now();
    auto next = start;
    auto lastLog = start;
    uint32_t sampleId = 0;
    uint32_t sent = 0, failed = 0, late = 0, noPose = 0;
    uint32_t totalSent = 0, totalFailed = 0, totalLate = 0, totalNoPose = 0;

    while (m_Running.load()) {
        VIPLE_VR_TRACKING sample;
        memset(&sample, 0, sizeof(sample));

        const auto now = clock::now();
        bool havePose = true;
        if (m_Source) {
            // M4a R1：真 XR。來源填 pose[HMD]、flags、predictNs；取樣時間仍是 client 單調時鐘（M1a 定義）
            sample.predictNs = predictNs;
            havePose = m_Source(&sample);
        }
        else {
            const double t = std::chrono::duration<double>(now - start).count();
            vrSyntheticFill(m_Motion, t, &sample);
            if (m_SyntheticHands) {
                vrSyntheticHands(t, &sample);
            }
            sample.predictNs = predictNs;
        }
        sample.version = VIPLE_VR_TRACKING_VERSION;
        sample.spaceEpoch = 0;
        sample.sampleTimeNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();

        if (!havePose) {
            noPose++;
            totalNoPose++;
        }
        else {
            sample.sampleId = ++sampleId;
            VrSampleHistory::record(sample.sampleId, sample.sampleTimeNs);
            if (LiSendVrTracking(&sample) == 0) {
                sent++;
                totalSent++;
            }
            else {
                failed++;
                totalFailed++;
            }
        }

        if (now - lastLog >= std::chrono::seconds(10)) {
            const double secs = std::chrono::duration<double>(now - lastLog).count();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-POSE] 10s: sent=%u (%.1f/s) fail=%u late=%u noPose=%u lastId=%u",
                        sent, sent / secs, failed, late, noPose, sampleId);
            sent = failed = late = noPose = 0;
            lastLog = now;
        }

        // 固定頻率：落後超過一個週期就直接跳到下一個格點，不補發（舊樣本沒有價值）
        next += period;
        // §VR-TRACK-LOCK：有顯示的節拍時，下一拍改成「這一拍之後至少半個間隔」的第一個格點
        // （格點＝顯示時刻 + phase + k·T）。顯示時刻每幀更新，格點跟著顯示的時鐘走，不再用自己的計時累加。
        const uint64_t gridPeriod = s_GridPeriodNs.load(std::memory_order_relaxed);
        if (gridPeriod >= 2000000ull) {
            const int64_t T = static_cast<int64_t>(gridPeriod) / VIPLE_VR_TRACKING_RATE_MUL;
            const int64_t base = static_cast<int64_t>(s_GridNs.load(std::memory_order_relaxed)) + s_PhaseNs.load(std::memory_order_relaxed);
            const int64_t nowTick = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            const int64_t d = nowTick + T / 2 - base;
            const int64_t n = d >= 0 ? (d + T - 1) / T : -((-d) / T);  // ceil(d / T)
            next = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(base + n * T)));
        }
        const auto after = clock::now();
        if (after > next + period) {
            late++;
            totalLate++;
            next = after + period;
        }
        std::this_thread::sleep_until(next);
    }

    const double totalSecs = std::chrono::duration<double>(clock::now() - start).count();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-POSE] (final) sent=%u (%.1f/s over %.1f s) fail=%u late=%u noPose=%u lastId=%u",
                totalSent, totalSecs > 0 ? totalSent / totalSecs : 0.0, totalSecs,
                totalFailed, totalLate, totalNoPose, sampleId);
}
