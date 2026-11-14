#include "vrtracking.h"

#include "backend/tailscalepath.h"  // 中繼時降頻

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
    // §REVIEW-1111：一拍只有約 1.7～3.5 ms，原本是預設優先權——系統忙的時候晚醒 1 ms 以上就佔掉大半拍，
    // host 來不及拿到這一拍就用上一拍的姿勢。和音訊執行緒同一級（不用即時級，免得反過來擠到顯示）。
    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-POSE] could not raise the tracking thread priority: %s", SDL_GetError());
    }
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
    // §PERF-1113：量每一拍花在「取姿態」（m_Source：xrLocateSpace 頭＋雙手＋視線）與「送出」（LiSendVrTracking：加密＋
    // 每條連線一次 sendto）的時間，每 10 秒印平均與最大。只量不改行為——每秒 576 拍值不值得降，要先有這兩個數字。
    uint64_t srcNsSum = 0, sendNsSum = 0;
    uint32_t srcNsMax = 0, sendNsMax = 0, srcCount = 0, sendCount = 0;
    // §PERF-1113 A35 中繼降頻：session 位址那條 Tailscale 路徑走 DERP（TCP 轉送，往返動輒數百毫秒）而且 tracking 只有這一條路時，
    // 每個顯示週期只送一筆——每 1.7 ms 一筆最新姿態在這種延遲下沒有意義，只是佔掉中繼的上行。
    // 有第二條連線就不降：那條可能是區網，降了它也跟著變稀。封包格式不變，host 照舊拿最新一筆。
    bool thinning = false;
    uint32_t thinTick = 0, thinSkipped = 0;
    auto thinSince = start;

    while (m_Running.load()) {
        VIPLE_VR_TRACKING sample;
        memset(&sample, 0, sizeof(sample));

        const auto now = clock::now();
        const uint64_t sampleTimeNs = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        const bool thin = TailscalePath::onDerpRelay() && LiGetVrLinkCount() <= 1;
        if (thin != thinning) {
            thinning = thin;
            if (thin) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-POSE] relay thinning on: sending 1 of %d ticks (Tailscale path is on a DERP relay, single link)",
                            VIPLE_VR_TRACKING_RATE_MUL);
                thinSince = now;
            }
            else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-POSE] relay thinning off after %.1f s (%u ticks skipped)",
                            std::chrono::duration<double>(now - thinSince).count(), thinSkipped);
            }
            thinTick = 0;  // 切換的這一拍照送
            thinSkipped = 0;
        }
        // 跳過的拍子整拍不做：不取樣、不遞增 sampleId、不記進 VrSampleHistory（下面兩處的 skipTick 分支）
        const bool skipTick = thinning && (thinTick++ % VIPLE_VR_TRACKING_RATE_MUL) != 0;
        bool havePose = true;
        if (skipTick) {
            thinSkipped++;
        }
        else if (m_Source) {
            // M4a R1：真 XR。來源填 pose[HMD]、flags、predictNs；取樣時間仍是 client 單調時鐘（M1a 定義），
            // 先填好讓來源讀得到（--vr-synthetic-hmd 的合成姿態照「取樣時間＋predictNs」那一刻算）
            sample.predictNs = predictNs;
            sample.sampleTimeNs = sampleTimeNs;
            havePose = m_Source(&sample);
            {
                // §PERF-1113：取姿態花了多久（見上面的說明）
                const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - now).count();
                const uint32_t v = ns <= 0 ? 0u : (ns > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)ns);
                srcNsSum += v;
                srcCount++;
                if (v > srcNsMax) {
                    srcNsMax = v;
                }
            }
        }
        else {
            // §REVIEW-1104：合成姿態要對到 sampleTimeNs+predictNs，否則 host 不再補這一段，畫面固定落後約 3 個顯示週期
            const double t = std::chrono::duration<double>(now - start).count() + predictNs * 1e-9;
            vrSyntheticFill(m_Motion, t, &sample);
            if (m_SyntheticHands) {
                vrSyntheticHands(t, &sample);
            }
            vrSyntheticMenu(t, m_SyntheticMenuAtSec, &sample);
            vrSyntheticPadTest(t, m_SyntheticPadTestSec, &sample);
            sample.predictNs = predictNs;
        }
        sample.version = VIPLE_VR_TRACKING_VERSION;
        sample.spaceEpoch = 0;
        sample.sampleTimeNs = sampleTimeNs;

        if (skipTick) {
            // 中繼降頻：這一拍不送（也不算進 noPose）
        }
        else if (!havePose) {
            noPose++;
            totalNoPose++;
        }
        else {
            sample.sampleId = ++sampleId;
            VrSampleHistory::record(sample.sampleId, sample.sampleTimeNs);
            const auto sendStart = clock::now();  // §PERF-1113：送出花了多久
            if (LiSendVrTracking(&sample) == 0) {
                sent++;
                totalSent++;
            }
            else {
                failed++;
                totalFailed++;
            }
            {
                const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - sendStart).count();
                const uint32_t v = ns <= 0 ? 0u : (ns > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)ns);
                sendNsSum += v;
                sendCount++;
                if (v > sendNsMax) {
                    sendNsMax = v;
                }
            }
        }

        if (now - lastLog >= std::chrono::seconds(10)) {
            const double secs = std::chrono::duration<double>(now - lastLog).count();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-POSE] 10s: sent=%u (%.1f/s) fail=%u late=%u noPose=%u lastId=%u",
                        sent, sent / secs, failed, late, noPose, sampleId);
            // §PERF-1113：另外印一行（原本那一行的欄位不動）。src＝取姿態、send＝送出，單位微秒；src 的 n＝0 代表用的是合成姿態
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-POSE] cost 10s: src avg=%.1f max=%.1f us (n=%u) send avg=%.1f max=%.1f us (n=%u)",
                        srcCount ? srcNsSum / 1000.0 / srcCount : 0.0, srcNsMax / 1000.0, srcCount,
                        sendCount ? sendNsSum / 1000.0 / sendCount : 0.0, sendNsMax / 1000.0, sendCount);
            srcNsSum = sendNsSum = 0;
            srcNsMax = sendNsMax = srcCount = sendCount = 0;
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
