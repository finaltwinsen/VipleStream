#include "qxr_vrmsg.h"

#include "qxr_log.h"

#include <android/log.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

void QxrVrMsg::setServerErrorCallback(ServerErrorFn fn)
{
    std::lock_guard<std::mutex> guard(lock);
    serverErrorFn = std::move(fn);
    // 新的一場要能再報一次。呼叫端在登記 sink 之前設，進行中的訊息才不會先鎖住又被清掉。
    serverErrorReported = false;
}

void QxrVrMsg::setTimingFill(TimingFillFn fn)
{
    std::lock_guard<std::mutex> guard(lock);
    timingFill = std::move(fn);
}

namespace {
std::mutex g_rateMu;
QxrVrMsg::RateAppliedFn g_rateFn;
}  // namespace

void QxrVrMsg::setRateHandler(RateAppliedFn fn)
{
    std::lock_guard<std::mutex> guard(g_rateMu);
    g_rateFn = std::move(fn);
}

void QxrVrMsg::start()
{
    if (running.load(std::memory_order_acquire)) {
        return;
    }
    startedOnce = true;
    // §PERF-R3（2026-11-26）：每次 start 都用新的 eventfd，上一次 stop 留下的計數不會讓 ppoll 一直立刻返回
    if (wakeFd < 0) {
        wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }
    running.store(true, std::memory_order_release);
    thread = std::thread(&QxrVrMsg::run, this);
}

void QxrVrMsg::stop()
{
    if (!thread.joinable()) {
        running.store(false, std::memory_order_release);
        if (wakeFd >= 0) {
            close(wakeFd);
            wakeFd = -1;
        }
        return;
    }
    running.store(false, std::memory_order_release);
    // §PERF-R3（2026-11-26）：叫醒正在 ppoll 的 run()，結束路徑不用等到下一秒
    if (wakeFd >= 0) {
        const uint64_t one = 1;
        (void)write(wakeFd, &one, sizeof(one));
    }
    thread.join();
    if (wakeFd >= 0) {
        close(wakeFd);
        wakeFd = -1;
    }
}

void QxrVrMsg::sendTiming()
{
    // §QUEST-VR：欄位用 VipleVr.h 的 VIPLE_VR_TLV_CLIENT_TIMING，不自己排版。
    // 模擬沒有 XR 量測。空窗的填法對齊 xrcontext.cpp：沒有樣本的分位數是 0，
    // pose lag 樣本不足時用 VIPLE_VR_POSE_LAG_NONE。displayPeriodNs 用跟 /launch 一樣的整除。
    TimingFillFn fillFn;
    {
        std::lock_guard<std::mutex> guard(lock);
        fillFn = timingFill;
    }
    QxrClientTimingFill measured{};
    const bool haveMeasured = static_cast<bool>(fillFn) && fillFn(&measured);

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
    if (haveMeasured) {
        // §QUEST-PCVR：姿態落後與緩衝計數這裡量不到，維持上面的 NONE／0。解碼時間沒有樣本時也不要寫 0 蓋掉「沒量到」。
        ct.framesPresented = measured.framesPresented;
        ct.xrMissed = measured.xrMissed;
        ct.metaMiss = measured.metaMiss;
        if (measured.displayPeriodNs != 0) {
            ct.displayPeriodNs = measured.displayPeriodNs;
        }
        if (measured.haveDecode) {
            ct.decodeP50Us = measured.decodeP50Us;
            ct.decodeP95Us = measured.decodeP95Us;
        }
        if (measured.haveRender) {
            ct.renderP50Us = measured.renderP50Us;
            ct.renderP95Us = measured.renderP95Us;
        }
        if (measured.haveSlack) {
            ct.slackP50Us = measured.slackP50Us;
            ct.slackP05Us = measured.slackP05Us;
        }
        if (measured.haveMtp) {
            ct.mtpP50_100us = measured.mtpP50_100us;
            ct.mtpP95_100us = measured.mtpP95_100us;
        }
    }

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
    uint32_t expired = 0;
    uint32_t counts[256];
    {
        std::lock_guard<std::mutex> guard(lock);
        sent = timingSent;
        fail = timingFail;
        entries = s2cEntries;
        haptic = hapticOk;
        dup = hapticDup;
        expired = hapticExpired;
        std::memcpy(counts, subtypeCount, sizeof(counts));
        if (prefix != nullptr && std::strcmp(prefix, "(final)") != 0) {
            timingSent = 0;
            timingFail = 0;
            s2cEntries = 0;
            hapticOk = 0;
            hapticDup = 0;
            hapticExpired = 0;
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
                        "[VIPLE-VR-MSG] %s timingSent=%u timingFail=%u s2c=%u haptic=%u hapticDup=%u hapticExpired=%u subtypes=%s",
                        prefix, sent, fail, entries, haptic, dup, expired, subtypes);
}

void QxrVrMsg::run()
{
    // §QUEST-VR：LATCH 只在 XrContext::pcvrTimingTick 組包。Qt 的 vrEmulate 不呼叫 setupXrPcvr()，
    // 不會進那個函式。模擬這裡沒有幀 latch，不送 VIPLE_VR_C2S_LATCH。投影輸出的 LATCH 由 frame thread 另送。
    bool pcvrTiming = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        pcvrTiming = static_cast<bool>(timingFill);
    }
    if (pcvrTiming) {
        QXR_LOGI("MSG", "client timing 1 Hz started");
    }
    else {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-MSG] client timing 1 Hz started; LATCH is not sent (no XR frame latch in emulation)");
    }

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
        // §PERF-R3（2026-11-26）：有 eventfd 時一次睡到截止點（每秒醒一次，原本每秒 20 次），
        // stop() 寫 eventfd 打斷。ppoll 的逾時是 CLOCK_MONOTONIC 的相對時間；EINTR 或提早醒都回到迴圈頂端重算。
        if (wakeFd >= 0) {
            const uint64_t wakeNowNs = monoNs(wake);
            if (targetNs <= wakeNowNs) {
                continue;
            }
            const uint64_t waitNs = targetNs - wakeNowNs;
            timespec rel{};
            rel.tv_sec = static_cast<time_t>(waitNs / 1000000000ull);
            rel.tv_nsec = static_cast<long>(waitNs % 1000000000ull);
            pollfd pfd{};
            pfd.fd = wakeFd;
            pfd.events = POLLIN;
            (void)ppoll(&pfd, 1, &rel, nullptr);
            continue;
        }
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

    RateAppliedFn rateFn;
    {
        std::lock_guard<std::mutex> guard(g_rateMu);
        rateFn = g_rateFn;
    }

    // 一則 0x5508 最長 251 B，震動脈衝塞不滿 8 筆。log 放到鎖外面，sink 才不會卡住 1 Hz 那條執行緒。
    VIPLE_VR_TLV_HAPTIC pending[8];
    int pendingCount = 0;
    VIPLE_VR_TLV_RATE_APPLIED ratePending[4];
    int rateCount = 0;
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
    ServerErrorFn errorFn;
    bool reportError = false;
    uint16_t reportCode = 0;
    int expiredNow = 0;
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
                        // timestampNs 是 client 的 CLOCK_MONOTONIC。0 表示 host 沒給，不能拿來判過期。
                        // 寬限 50 ms 要蓋過單程延遲，否則幾毫秒的短脈衝第一份就會被丟掉。
                        bool expired = false;
                        if (haptic.timestampNs != 0) {
                            timespec nowTs{};
                            clock_gettime(CLOCK_MONOTONIC, &nowTs);
                            const uint64_t nowNs = monoNs(nowTs);
                            if (nowNs > haptic.timestampNs) {
                                const uint64_t maxAgeNs =
                                    (static_cast<uint64_t>(haptic.durationUs) + 50000ull) * 1000ull;
                                expired = nowNs - haptic.timestampNs > maxAgeNs;
                            }
                        }
                        if (expired) {
                            hapticExpired++;
                            expiredNow++;
                        }
                        else {
                            hapticOk++;
                            if (pendingCount < 8) {
                                pending[pendingCount++] = haptic;
                            }
                        }
                    }
                    acceptedHaptic = true;
                }
            }
            bool handledState = false;
            if (type == VIPLE_VR_S2C_STATE && len >= sizeof(VIPLE_VR_TLV_STATE) && serverErrorFn) {
                VIPLE_VR_TLV_STATE st{};
                std::memcpy(&st, payload, sizeof(st));
                if (st.state == VIPLE_VR_STATE_ERROR && !serverErrorReported) {
                    serverErrorReported = true;
                    reportError = true;
                    reportCode = st.code;
                    handledState = true;
                }
            }
            bool handledRate = false;
            if (type == VIPLE_VR_S2C_RATE && len >= sizeof(VIPLE_VR_TLV_RATE_APPLIED) && rateFn) {
                if (rateCount < 4) {
                    std::memcpy(&ratePending[rateCount], payload, sizeof(ratePending[rateCount]));
                    rateCount++;
                }
                handledRate = true;
            }
            if (!handledRate && !acceptedHaptic && !handledState && !subtypeSeen[type] && firstSeenCount < 8) {
                subtypeSeen[type] = true;
                firstSeen[firstSeenCount].type = type;
                firstSeen[firstSeenCount].len = len;
                firstSeenCount++;
            }
            off += 2 + len;
        }
        callback = hapticFn;
        errorFn = serverErrorFn;
    }

    if (reportError) {
        __android_log_print(ANDROID_LOG_ERROR, "viple-openxr",
                            "[VIPLE-VR-MSG] server could not start VR (code=%u) - ending the stream",
                            static_cast<unsigned>(reportCode));
        // 回呼返回後這個 sink 才放掉讀鎖。裡面若同步 nativeStop，bridge 的寫鎖會互等。
        if (errorFn) {
            errorFn(reportCode);
        }
    }
    if (expiredNow > 0) {
        __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                            "[VIPLE-VR-HAPTIC] dropped %d expired pulse(s)", expiredNow);
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
    for (int i = 0; i < rateCount; i++) {
        if (rateFn) {
            rateFn(static_cast<int>(ratePending[i].div), ratePending[i].accepted != 0, ratePending[i].seq);
        }
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
