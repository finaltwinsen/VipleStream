#include "qxr_vrmsg.h"

#include <android/log.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>

namespace {

constexpr uint64_t kTimingPeriodNs = 1000000000ull;
constexpr uint64_t kSummaryPeriodNs = 10000000000ull;
// stop() 要 join 這條執行緒。一次睡滿 1 秒會把結束路徑卡住，所以用短的絕對時間切片去追同一個截止點。
constexpr uint64_t kWakeSliceNs = 50000000ull;

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

const char* handName(uint8_t device)
{
    if (device == VIPLE_VR_POSE_LEFT) {
        return "left";
    }
    if (device == VIPLE_VR_POSE_RIGHT) {
        return "right";
    }
    return "other";
}

}  // namespace

QxrVrMsg::QxrVrMsg(int hz)
    : displayHz(hz > 0 ? hz : 72)
{
}

QxrVrMsg::~QxrVrMsg()
{
    stop();
    // sink 在解構前就卸掉了，這裡的計數不會再變。沒跑過也沒收過訊息就不要多印一行。
    bool any = startedOnce;
    if (!any) {
        std::lock_guard<std::mutex> guard(lock);
        any = timingSent != 0 || timingFail != 0 || s2cEntries != 0 || hapticOk != 0;
    }
    if (any) {
        logSummary("(final)");
    }
}

void QxrVrMsg::setHapticCallback(HapticFn fn)
{
    std::lock_guard<std::mutex> guard(lock);
    hapticFn = std::move(fn);
}

void QxrVrMsg::start()
{
    if (running.load(std::memory_order_acquire)) {
        return;
    }
    startedOnce = true;
    running.store(true, std::memory_order_release);
    thread = std::thread(&QxrVrMsg::run, this);
}

void QxrVrMsg::stop()
{
    if (!thread.joinable()) {
        running.store(false, std::memory_order_release);
        return;
    }
    running.store(false, std::memory_order_release);
    thread.join();
}

void QxrVrMsg::sendTiming()
{
    // §QUEST-VR：欄位用 VipleVr.h 的 VIPLE_VR_TLV_CLIENT_TIMING，不自己排版。
    // 模擬沒有 XR 量測。空窗的填法對齊 xrcontext.cpp：沒有樣本的分位數是 0，
    // pose lag 樣本不足時用 VIPLE_VR_POSE_LAG_NONE。displayPeriodNs 用跟 /launch 一樣的整除。
    VIPLE_VR_TLV_CLIENT_TIMING ct{};
    ct.framesPresented = 0;
    ct.xrMissed = 0;
    ct.metaMiss = 0;
    ct.displayPeriodNs = static_cast<uint32_t>(1000000000ull / static_cast<uint64_t>(displayHz));
    ct.decodeP50Us = 0;
    ct.decodeP95Us = 0;
    ct.renderP50Us = 0;
    ct.renderP95Us = 0;
    ct.slackP50Us = 0;
    ct.slackP05Us = 0;
    ct.mtpP50_100us = 0;
    ct.mtpP95_100us = 0;
    ct.poseLagP50_100us = static_cast<int16_t>(VIPLE_VR_POSE_LAG_NONE);
    ct.poseLagCount = 0;
    ct.bufLag1Total = 0;
    ct.bufLag0Total = 0;

    uint8_t tlv[2 + sizeof(ct)];
    tlv[0] = VIPLE_VR_C2S_CLIENT_TIMING;
    tlv[1] = static_cast<uint8_t>(sizeof(ct));
    std::memcpy(tlv + 2, &ct, sizeof(ct));
    const int rc = LiSendVrMessage(tlv, static_cast<int>(sizeof(tlv)), false);

    std::lock_guard<std::mutex> guard(lock);
    if (rc == 0) {
        timingSent++;
    }
    else {
        timingFail++;
    }
}

void QxrVrMsg::logSummary(const char* prefix)
{
    uint32_t sent = 0;
    uint32_t fail = 0;
    uint32_t entries = 0;
    uint32_t haptic = 0;
    uint32_t dup = 0;
    uint32_t counts[256];
    {
        std::lock_guard<std::mutex> guard(lock);
        sent = timingSent;
        fail = timingFail;
        entries = s2cEntries;
        haptic = hapticOk;
        dup = hapticDup;
        std::memcpy(counts, subtypeCount, sizeof(counts));
        if (prefix != nullptr && std::strcmp(prefix, "(final)") != 0) {
            timingSent = 0;
            timingFail = 0;
            s2cEntries = 0;
            hapticOk = 0;
            hapticDup = 0;
            std::memset(subtypeCount, 0, sizeof(subtypeCount));
        }
    }

    char subtypes[192];
    size_t used = 0;
    subtypes[0] = '\0';
    for (int i = 0; i < 256; i++) {
        if (counts[i] == 0) {
            continue;
        }
        char piece[32];
        const int wrote = std::snprintf(piece, sizeof(piece), "%s0x%02x=%u", used == 0 ? "" : " ", i, counts[i]);
        if (wrote < 0) {
            break;
        }
        if (used + static_cast<size_t>(wrote) >= sizeof(subtypes) - 4) {
            std::snprintf(subtypes + used, sizeof(subtypes) - used, "...");
            used = std::strlen(subtypes);
            break;
        }
        std::memcpy(subtypes + used, piece, static_cast<size_t>(wrote));
        used += static_cast<size_t>(wrote);
        subtypes[used] = '\0';
    }
    if (used == 0) {
        std::snprintf(subtypes, sizeof(subtypes), "none");
    }

    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-MSG] %s timingSent=%u timingFail=%u s2c=%u haptic=%u hapticDup=%u subtypes=%s",
                        prefix, sent, fail, entries, haptic, dup, subtypes);
}

void QxrVrMsg::run()
{
    // §QUEST-VR：LATCH 只在 XrContext::pcvrTimingTick 組包。Qt 的 vrEmulate 不呼叫 setupXrPcvr()，
    // 不會進那個函式。這裡沒有幀 latch，不送 VIPLE_VR_C2S_LATCH。
    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-MSG] client timing 1 Hz started; LATCH is not sent (no XR frame latch in emulation)");

    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t summaryNs = monoNs(next);
    sendTiming();
    addNs(&next, kTimingPeriodNs);

    while (running.load(std::memory_order_acquire)) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t nowNs = monoNs(now);
        if (nowNs >= summaryNs + kSummaryPeriodNs) {
            logSummary("10s:");
            summaryNs = nowNs;
        }
        if (nowNs >= monoNs(next)) {
            sendTiming();
            addNs(&next, kTimingPeriodNs);
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (monoNs(now) > monoNs(next)) {
                next = now;
                addNs(&next, kTimingPeriodNs);
            }
        }

        timespec wake{};
        clock_gettime(CLOCK_MONOTONIC, &wake);
        const uint64_t targetNs = monoNs(next);
        uint64_t wakeNs = monoNs(wake) + kWakeSliceNs;
        if (wakeNs > targetNs) {
            wakeNs = targetNs;
        }
        if (wakeNs <= monoNs(wake)) {
            continue;
        }
        timespec abs{};
        abs.tv_sec = static_cast<time_t>(wakeNs / 1000000000ull);
        abs.tv_nsec = static_cast<long>(wakeNs % 1000000000ull);
        while (running.load(std::memory_order_acquire)) {
            const int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, nullptr);
            if (rc != EINTR) {
                break;
            }
        }
    }
}

void QxrVrMsg::onMessage(const uint8_t* tlv, int length)
{
    if (tlv == nullptr || length <= 0) {
        return;
    }

    // 一則 0x5508 最長 251 B，震動脈衝塞不滿 8 筆。log 放到鎖外面，sink 才不會卡住 1 Hz 那條執行緒。
    VIPLE_VR_TLV_HAPTIC pending[8];
    int pendingCount = 0;
    struct FirstSeen {
        uint8_t type;
        uint8_t len;
    };
    FirstSeen firstSeen[8];
    int firstSeenCount = 0;
    bool truncated = false;
    unsigned truncatedType = 0;
    unsigned truncatedLen = 0;
    int truncatedRemaining = 0;
    HapticFn callback;
    {
        std::lock_guard<std::mutex> guard(lock);
        int off = 0;
        while (off + 2 <= length) {
            const uint8_t type = tlv[off];
            const uint8_t len = tlv[off + 1];
            if (off + 2 + len > length) {
                truncated = true;
                truncatedType = type;
                truncatedLen = len;
                truncatedRemaining = length - off - 2;
                break;
            }
            const uint8_t* payload = tlv + off + 2;
            s2cEntries++;
            subtypeCount[type]++;

            bool acceptedHaptic = false;
            if (type == VIPLE_VR_S2C_HAPTIC && len >= 20) {
                VIPLE_VR_TLV_HAPTIC haptic{};
                const size_t copyLen = static_cast<size_t>(len) < sizeof(haptic) ? static_cast<size_t>(len) : sizeof(haptic);
                std::memcpy(&haptic, payload, copyLen);
                const unsigned dev = static_cast<unsigned>(haptic.device) - VIPLE_VR_POSE_LEFT;
                if (dev < 2) {
                    bool duplicate = false;
                    if (haveEventId[dev]) {
                        // 左右手各自的 32-bit 序號，迴繞後仍能看出舊的重送。
                        if (static_cast<int32_t>(haptic.eventId - lastEventId[dev]) <= 0) {
                            duplicate = true;
                        }
                    }
                    if (duplicate) {
                        hapticDup++;
                    }
                    else {
                        lastEventId[dev] = haptic.eventId;
                        haveEventId[dev] = true;
                        hapticOk++;
                        if (pendingCount < 8) {
                            pending[pendingCount++] = haptic;
                        }
                    }
                    acceptedHaptic = true;
                }
            }
            if (!acceptedHaptic && !subtypeSeen[type] && firstSeenCount < 8) {
                subtypeSeen[type] = true;
                firstSeen[firstSeenCount].type = type;
                firstSeen[firstSeenCount].len = len;
                firstSeenCount++;
            }
            off += 2 + len;
        }
        callback = hapticFn;
    }

    if (truncated) {
        __android_log_print(ANDROID_LOG_WARN, "viple-openxr",
                            "[VIPLE-VR-MSG] truncated s2c TLV type=%u len=%u remaining=%d",
                            truncatedType, truncatedLen, truncatedRemaining);
    }
    for (int i = 0; i < firstSeenCount; i++) {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-MSG] first s2c subtype=0x%02x len=%u",
                            firstSeen[i].type, firstSeen[i].len);
    }
    for (int i = 0; i < pendingCount; i++) {
        const VIPLE_VR_TLV_HAPTIC& haptic = pending[i];
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-HAPTIC] hand=%s durationUs=%u freq=%.1f Hz amp=%.2f id=%u",
                            handName(haptic.device), haptic.durationUs,
                            static_cast<double>(haptic.frequencyHz),
                            static_cast<double>(haptic.amplitude), haptic.eventId);
        if (callback) {
            callback(haptic);
        }
    }
}
