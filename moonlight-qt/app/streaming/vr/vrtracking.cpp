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
                "[VIPLE-VR-POSE] tracking sender started: %d Hz (display %d Hz × %d), motion=%s",
                rateHz, m_DisplayHz, VIPLE_VR_TRACKING_RATE_MUL, vrSyntheticMotionName(m_Motion));

    const auto start = clock::now();
    auto next = start;
    auto lastLog = start;
    uint32_t sampleId = 0;
    uint32_t sent = 0, failed = 0, late = 0;
    uint32_t totalSent = 0, totalFailed = 0, totalLate = 0;

    while (m_Running.load()) {
        VIPLE_VR_TRACKING sample;
        memset(&sample, 0, sizeof(sample));

        const auto now = clock::now();
        const double t = std::chrono::duration<double>(now - start).count();
        vrSyntheticFill(m_Motion, t, &sample);
        sample.version = VIPLE_VR_TRACKING_VERSION;
        sample.spaceEpoch = 0;
        sample.sampleId = ++sampleId;
        sample.sampleTimeNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        sample.predictNs = predictNs;

        VrSampleHistory::record(sample.sampleId, sample.sampleTimeNs);
        if (LiSendVrTracking(&sample) == 0) {
            sent++;
            totalSent++;
        }
        else {
            failed++;
            totalFailed++;
        }

        if (now - lastLog >= std::chrono::seconds(10)) {
            const double secs = std::chrono::duration<double>(now - lastLog).count();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-POSE] 10s: sent=%u (%.1f/s) fail=%u late=%u lastId=%u",
                        sent, sent / secs, failed, late, sampleId);
            sent = failed = late = 0;
            lastLog = now;
        }

        // 固定頻率：落後超過一個週期就直接跳到下一個格點，不補發（舊樣本沒有價值）
        next += period;
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
                "[VIPLE-VR-POSE] (final) sent=%u (%.1f/s over %.1f s) fail=%u late=%u lastId=%u",
                totalSent, totalSecs > 0 ? totalSent / totalSecs : 0.0, totalSecs,
                totalFailed, totalLate, sampleId);
}
