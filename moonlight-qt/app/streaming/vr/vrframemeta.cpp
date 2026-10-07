#include "vrframemeta.h"
#include "vrtracking.h"

#include <SDL.h>

#include <algorithm>

namespace {
constexpr uint32_t kLogIntervalMs = 10000;
constexpr size_t kMaxTotalAges = 200000;  // 約 37 分鐘 @90 fps；超過就不再收（只影響 final 百分位）
}

VrFrameMetaTracker::VrFrameMetaTracker(int injectDropEvery, int injectLossSec)
    : m_InjectDropEvery(injectDropEvery > 0 ? injectDropEvery : 0),
      m_InjectLossSec(injectLossSec > 0 ? injectLossSec : 0)
{
    m_LastLogMs = SDL_GetTicks();
    m_LastLossInjectMs = SDL_GetTicks();
    m_IntervalAgesMs.reserve(2048);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-FRAME] pts-keyed metadata pairing enabled (injectDropEvery=%d injectLossSec=%d)",
                m_InjectDropEvery, m_InjectLossSec);
}

VrFrameMetaTracker::~VrFrameMetaTracker()
{
    maybeLog(true);
}

void VrFrameMetaTracker::onSubmit(const DECODE_UNIT* du)
{
    Slot& slot = m_Slots[(uint32_t)du->frameNumber % m_Slots.size()];
    slot.frameNumber = (uint32_t)du->frameNumber;
    slot.used = true;
    slot.meta = du->vrMeta;
    m_LastSubmittedFrame = (uint32_t)du->frameNumber;
    m_Interval.submitted++;
    m_Total.submitted++;
    maybeInjectLoss(m_LastSubmittedFrame);
}

bool VrFrameMetaTracker::onDecoded(int64_t pts, int fifoFrameNumber, VIPLE_VR_FRAME_META* meta)
{
    m_Interval.decoded++;
    m_Total.decoded++;

    if (fifoFrameNumber >= 0 && (int64_t)fifoFrameNumber != pts) {
        m_Interval.fifoMismatch++;
        m_Total.fifoMismatch++;
    }

    Slot& slot = m_Slots[(uint64_t)pts % m_Slots.size()];
    if (pts < 0 || !slot.used || (int64_t)slot.frameNumber != pts) {
        m_Interval.metaMiss++;
        m_Total.metaMiss++;
        static uint32_t s_LastMissLogMs;
        const uint32_t now = SDL_GetTicks();
        if (s_LastMissLogMs == 0 || now - s_LastMissLogMs >= 1000) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-FRAME] meta-miss: pts=%lld slot=%u used=%d",
                        (long long)pts, slot.frameNumber, (int)slot.used);
            s_LastMissLogMs = now;
        }
        maybeLog(false);
        return false;
    }

    *meta = slot.meta;
    slot.used = false;  // 每個 frameNumber 只配對一次
    m_Interval.metaHit++;
    m_Total.metaHit++;

    if (!meta->present) {
        m_Interval.headerMissing++;
        m_Total.headerMissing++;
    }
    else {
        if (meta->vrFlags & VIPLE_VR_FF_ECHO_MATCHED) {
            m_Interval.echoMatched++;
            m_Total.echoMatched++;
        }
        if (meta->vrFlags & VIPLE_VR_FF_POSE_FALLBACK) {
            m_Interval.poseFallback++;
            m_Total.poseFallback++;
        }
        if (meta->vrFlags & VIPLE_VR_FF_REFRESH_DONE) {
            m_Interval.refreshDone++;
            m_Total.refreshDone++;
        }
        uint64_t sampleTimeNs;
        if (VrSampleHistory::lookup(meta->echoSampleId, &sampleTimeNs)) {
            const uint64_t nowNs = VrTrackingSender::nowNs();
            if (nowNs >= sampleTimeNs) {
                const float ageMs = (float)((nowNs - sampleTimeNs) / 1e6);
                m_IntervalAgesMs.push_back(ageMs);
                if (m_TotalAgesMs.size() < kMaxTotalAges) {
                    m_TotalAgesMs.push_back(ageMs);
                }
            }
            m_Interval.echoKnown++;
            m_Total.echoKnown++;
        }
    }

    maybeLog(false);
    return true;
}

bool VrFrameMetaTracker::shouldInjectDrop()
{
    if (m_InjectDropEvery <= 0) {
        return false;
    }
    if (++m_DecodedSinceDrop < (uint32_t)m_InjectDropEvery) {
        return false;
    }
    m_DecodedSinceDrop = 0;
    m_Interval.injectedDrops++;
    m_Total.injectedDrops++;
    return true;
}

void VrFrameMetaTracker::noteFifoSkipped(int count)
{
    m_Interval.fifoSkipped += (uint32_t)count;
    m_Total.fifoSkipped += (uint32_t)count;
}

void VrFrameMetaTracker::notePtsMissing()
{
    if (m_Total.ptsMissing++ == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-FRAME] decoder did not carry pts — falling back to FIFO pairing");
    }
    m_Interval.ptsMissing++;
}

void VrFrameMetaTracker::maybeInjectLoss(uint32_t frameNumber)
{
    if (m_InjectLossSec <= 0) {
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (now - m_LastLossInjectMs < (uint64_t)m_InjectLossSec * 1000) {
        return;
    }
    m_LastLossInjectMs = now;
    m_Interval.injectedLoss++;
    m_Total.injectedLoss++;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-LOSS] injected LOSS for frame %u (test)", frameNumber);
    LiReportVrLoss(frameNumber, frameNumber, VIPLE_VR_LOSS_DECODE_ERROR);
}

void VrFrameMetaTracker::logStats(const char* prefix, const Stats& s, std::vector<float>& ages)
{
    float p50 = 0, p95 = 0, pmax = 0;
    if (!ages.empty()) {
        std::sort(ages.begin(), ages.end());
        p50 = ages[ages.size() / 2];
        p95 = ages[std::min(ages.size() - 1, (size_t)(ages.size() * 0.95))];
        pmax = ages.back();
    }
    const uint32_t withHeader = s.metaHit - s.headerMissing;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-FRAME] %s decoded=%u submitted=%u hit=%u miss=%u noHeader=%u "
                "echoMatched=%u/%u (%.2f%%) echoKnown=%u fallback=%u refreshDone=%u "
                "fifoMismatch=%u fifoSkipped=%u injectedDrops=%u injectedLoss=%u ptsMissing=%u "
                "echoAge p50=%.1fms p95=%.1fms max=%.1fms",
                prefix, s.decoded, s.submitted, s.metaHit, s.metaMiss, s.headerMissing,
                s.echoMatched, withHeader, withHeader ? 100.0 * s.echoMatched / withHeader : 0.0,
                s.echoKnown, s.poseFallback, s.refreshDone,
                s.fifoMismatch, s.fifoSkipped, s.injectedDrops, s.injectedLoss, s.ptsMissing,
                p50, p95, pmax);
}

void VrFrameMetaTracker::maybeLog(bool final)
{
    const uint64_t now = SDL_GetTicks();
    if (final) {
        logStats("(final)", m_Total, m_TotalAgesMs);
        return;
    }
    if (now - m_LastLogMs < kLogIntervalMs) {
        return;
    }
    logStats("10s:", m_Interval, m_IntervalAgesMs);
    m_Interval = Stats();
    m_IntervalAgesMs.clear();
    m_LastLogMs = now;
}

// ── M4a R1：VrRenderMetaRing ──
std::mutex VrRenderMetaRing::s_Lock;
VrRenderMetaRing::Entry VrRenderMetaRing::s_Entries[VrRenderMetaRing::kSize];

void VrRenderMetaRing::reset()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    for (Entry& e : s_Entries) {
        e.used = false;
    }
}

void VrRenderMetaRing::publish(int64_t pts, const VIPLE_VR_FRAME_META& meta)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    Entry& e = s_Entries[slot(pts)];
    e.pts = pts;
    e.used = true;
    e.meta = meta;
}

bool VrRenderMetaRing::lookup(int64_t pts, VIPLE_VR_FRAME_META* meta)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    const Entry& e = s_Entries[slot(pts)];
    if (!e.used || e.pts != pts) {
        return false;
    }
    *meta = e.meta;
    return true;
}

// ── 2026-10-05：VrDecodeTiming ──
std::mutex VrDecodeTiming::s_Lock;
std::vector<uint32_t> VrDecodeTiming::s_Samples;

void VrDecodeTiming::reset()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    s_Samples.clear();
}

void VrDecodeTiming::record(uint64_t us)
{
    std::lock_guard<std::mutex> lock(s_Lock);
    if (s_Samples.size() < kCap) {
        s_Samples.push_back((uint32_t)std::min<uint64_t>(us, 0xffffffffu));
    }
}

bool VrDecodeTiming::take(uint32_t* p50Us, uint32_t* p95Us)
{
    std::vector<uint32_t> v;
    {
        std::lock_guard<std::mutex> lock(s_Lock);
        v.swap(s_Samples);
    }
    if (v.empty()) {
        return false;
    }
    auto pct = [&v](double p) {
        const size_t idx = std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5));
        std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t)idx, v.end());
        return v[idx];
    };
    *p50Us = pct(0.50);
    *p95Us = pct(0.95);
    return true;
}
