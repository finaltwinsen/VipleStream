#include "qxr_tracking.h"

#include <android/log.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

uint64_t monoNs(const timespec& ts)
{
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void addNs(timespec* ts, uint64_t ns)
{
    ts->tv_sec += static_cast<time_t>(ns / 1000000000ull);
    ns %= 1000000000ull;
    ts->tv_nsec += static_cast<long>(ns);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

void raiseTrackingPriority()
{
    // §QUEST-VR：要最高的非即時優先權。SCHED_FIFO／RR 會跟顯示搶，不用。
    // nice 放在排程策略之後設，避免改策略時把 nice 清掉。沒有權限就只記 log。
    const int policy = SCHED_OTHER;
    const int maxPriority = sched_get_priority_max(policy);
    sched_param param{};
    param.sched_priority = maxPriority < 0 ? 0 : maxPriority;
    const int rc = pthread_setschedparam(pthread_self(), policy, &param);
    if (rc != 0) {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-TRK] could not set tracking thread sched: pthread_setschedparam errno=%d",
                            rc);
    }
    const pid_t tid = static_cast<pid_t>(gettid());
    if (setpriority(PRIO_PROCESS, tid, -20) != 0) {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-TRK] could not raise tracking thread priority: setpriority errno=%d",
                            errno);
    }
}

}  // namespace

void QxrSampleHistory::reset()
{
    std::lock_guard<std::mutex> guard(lock);
    std::memset(entries, 0, sizeof(entries));
}

void QxrSampleHistory::record(uint32_t sampleId, uint64_t sampleTimeNs, const float quatXyzw[4])
{
    std::lock_guard<std::mutex> guard(lock);
    Entry& entry = entries[sampleId % kSize];
    entry.sampleId = sampleId;
    entry.sampleTimeNs = sampleTimeNs;
    if (quatXyzw != nullptr) {
        std::memcpy(entry.quat, quatXyzw, sizeof(entry.quat));
    }
    else {
        entry.quat[0] = 0.0f;
        entry.quat[1] = 0.0f;
        entry.quat[2] = 0.0f;
        entry.quat[3] = 1.0f;
    }
}

bool QxrSampleHistory::lookup(uint32_t sampleId, uint64_t* sampleTimeNs) const
{
    if (sampleId == 0 || sampleTimeNs == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock);
    const Entry& entry = entries[sampleId % kSize];
    if (entry.sampleId != sampleId) {
        return false;
    }
    *sampleTimeNs = entry.sampleTimeNs;
    return true;
}

bool QxrSampleHistory::lookupPose(uint32_t sampleId, float q[4]) const
{
    if (sampleId == 0 || q == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(lock);
    const Entry& entry = entries[sampleId % kSize];
    if (entry.sampleId != sampleId) {
        return false;
    }
    std::memcpy(q, entry.quat, sizeof(entry.quat));
    return true;
}

QxrTrackingSender::QxrTrackingSender(QxrPoseSource* sourceIn, QxrSampleHistory* historyIn)
    : source(sourceIn), history(historyIn)
{
}

QxrTrackingSender::~QxrTrackingSender()
{
    stop();
}

void QxrTrackingSender::setRateDivisor(int divisor)
{
    if (divisor < 1) {
        divisor = 1;
    }
    rateDivisor.store(divisor, std::memory_order_relaxed);
}

void QxrTrackingSender::start(int hz, const char* name)
{
    if (running.load(std::memory_order_acquire)) {
        return;
    }
    displayHz = hz > 0 ? hz : 72;
    sourceName = (name != nullptr && name[0] != '\0') ? name : "sine";
    if (history != nullptr) {
        history->reset();
    }
    running.store(true, std::memory_order_release);
    thread = std::thread(&QxrTrackingSender::run, this);
}

void QxrTrackingSender::stop()
{
    if (!thread.joinable()) {
        running.store(false, std::memory_order_release);
        return;
    }
    running.store(false, std::memory_order_release);
    thread.join();
}

void QxrTrackingSender::run()
{
    raiseTrackingPriority();

    const int rateHz = displayHz * VIPLE_VR_TRACKING_RATE_MUL;
    const uint64_t periodNs = 1000000000ull / static_cast<uint64_t>(rateHz);
    // 合成姿態沒有真的顯示時間，預測固定 3 個顯示週期，跟 Qt 模擬模式一樣。
    const uint32_t predictNs = static_cast<uint32_t>(3ull * 1000000000ull / static_cast<uint64_t>(displayHz));

    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-TRK] tracking sender started: %d Hz (display %d Hz x %d), motion=%s",
                        rateHz, displayHz, VIPLE_VR_TRACKING_RATE_MUL, sourceName);

    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    const uint64_t startNs = monoNs(next);
    uint64_t lastLogNs = startNs;
    uint32_t sampleId = 0;
    uint32_t sent = 0;
    uint32_t failed = 0;
    uint32_t late = 0;
    uint32_t totalSent = 0;
    uint32_t totalFailed = 0;
    uint32_t totalLate = 0;
    uint32_t tick = 0;

    while (running.load(std::memory_order_acquire)) {
        timespec nowTs{};
        clock_gettime(CLOCK_MONOTONIC, &nowTs);
        const uint64_t nowNs = monoNs(nowTs);

        int divisor = rateDivisor.load(std::memory_order_relaxed);
        if (divisor < 1) {
            divisor = 1;
        }
        // 跳過的拍不取樣、不占用 sampleId。divisor 1 時餘數恆為 0。
        const bool skipTick = (tick++ % static_cast<uint32_t>(divisor)) != 0;

        if (!skipTick && source != nullptr) {
            VIPLE_VR_TRACKING sample{};
            sample.predictNs = predictNs;
            const bool havePose = source->sample(&sample, nowNs / 1000ull);
            sample.version = VIPLE_VR_TRACKING_VERSION;
            sample.spaceEpoch = 0;
            // §VITURE：來源把樣本標成較舊時 sampleTimeNs 不是 0，要留著。合成姿態維持 0，這裡仍填現在。
            if (sample.sampleTimeNs == 0) {
                sample.sampleTimeNs = nowNs;
            }
            // §QUEST-PCVR：OpenXR 來源在 sample() 裡寫入實際的 predictNs。這裡再指定一次的話，
            // 上行會一直送上面那個「3 個顯示週期」。合成姿態沿用這次預填，它的樣本時間已經含這段預測。
            // IMU 只外插到取樣當下，sample() 會把 predictNs 改成 0，讓 host 自己預測。
            if (!havePose) {
                // 這一拍沒有姿態，不送、不占用 sampleId。
            }
            else {
                sample.sampleId = ++sampleId;
                if (history != nullptr) {
                    // §VITURE-WARP：記送出去的 HMD 旋轉。sample() 裡的預測已經寫進 rot，不能改記融合原值。
                    history->record(sample.sampleId, sample.sampleTimeNs,
                                    sample.pose[VIPLE_VR_POSE_HMD].rot);
                }
                if (LiSendVrTracking(&sample) == 0) {
                    sent++;
                    totalSent++;
                }
                else {
                    failed++;
                    totalFailed++;
                }
            }
        }

        if (nowNs - lastLogNs >= 10000000000ull) {
            const double secs = static_cast<double>(nowNs - lastLogNs) / 1e9;
            __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                                "[VIPLE-VR-TRK] 10s: sent=%u (%.1f/s) fail=%u late=%u lastId=%u",
                                sent, secs > 0.0 ? sent / secs : 0.0, failed, late, sampleId);
            sent = 0;
            failed = 0;
            late = 0;
            lastLogNs = nowNs;
        }

        // 絕對時間：下一拍＝上一拍的截止＋週期。落後超過一個週期就跳到現在之後，不補送舊樣本。
        addNs(&next, periodNs);
        timespec after{};
        clock_gettime(CLOCK_MONOTONIC, &after);
        if (monoNs(after) > monoNs(next) + periodNs) {
            late++;
            totalLate++;
            next = after;
            addNs(&next, periodNs);
        }
        while (running.load(std::memory_order_acquire)) {
            const int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
            if (rc != EINTR) {
                break;
            }
        }
    }

    timespec endTs{};
    clock_gettime(CLOCK_MONOTONIC, &endTs);
    const double elapsed = static_cast<double>(monoNs(endTs) - startNs) / 1e9;
    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-TRK] (final) sent=%u (%.1f/s over %.1f s) fail=%u late=%u lastId=%u",
                        totalSent, elapsed > 0.0 ? totalSent / elapsed : 0.0, elapsed,
                        totalFailed, totalLate, sampleId);
}
