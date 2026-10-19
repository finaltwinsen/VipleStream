// VipleStream 2.0 §VR-LOGASYNC — 10 秒彙總行交給另一條執行緒印（純邏輯）。
//
// 每 10 秒印彙總的那一格，frame thread 在 xrEndFrame 之後要多做 0.6～1.1 ms（2026-10-18 頭盔：maybeLogStats 那五行中位數
// 約 640 µs），回到 xrWaitFrame 最晚，「等在途的幀」與延後挑影像都因此要避開那一格。量過大半花在 log 本身：每一行經過
// main.cpp 的 log callback 兩次 QRegularExpression 取代、再丟進 QThreadPool。這裡讓 frame thread 只組好字串放進佇列，
// 由一條自己的執行緒呼叫 sink（XrContext 給的是 SDL_LogInfo）。同一個 AsyncLines 進來的行照順序印。
// 2026-10-18 頭盔細分：改成非同步之後剩下的大頭是排序與組字串（10s video 約 200～250 µs、POSEERR 約 220～280 µs），
// 所以另有 postTask：frame thread 只把這個視窗的資料整包交出來，字串到了這條執行緒才算。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／SDL），做法同 xrguard.h：`xr-probe --selftest-latch` 會接著跑這裡的 selfTest()。
//
// 執行緒：post／postf 可以從任何執行緒呼叫；stop() 印完佇列裡剩下的、收掉執行緒才返回（XrContext::shutdown 在 frame thread
// 結束之後呼叫），之後再 post 會重新起一條（同一個 XrContext 可以再開一場）；stop() 進行中的 post 直接在呼叫端印（不丟）。
// 佇列滿了（sink 卡住）新的行丟掉、計數，stop() 時交給 sink 一行說丟了幾行。

#pragma once

#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xrlog {

class AsyncLines
{
public:
    using Sink = std::function<void(const std::string& line)>;

    explicit AsyncLines(Sink sink, size_t maxQueued = 64) : m_Sink(std::move(sink)), m_Max(maxQueued) {}
    ~AsyncLines() { stop(); }
    AsyncLines(const AsyncLines&) = delete;
    AsyncLines& operator=(const AsyncLines&) = delete;

    using Task = std::function<std::string()>;

    // 第一次 post 時才起執行緒（沒有印過任何一行的場次不多一條執行緒）
    void post(std::string line)
    {
        postTask([l = std::move(line)]() { return l; });
    }

    // 到了這條執行緒才呼叫 fn 算出那一行（fn 要自己擁有它用到的資料——呼叫端之後可能已經清掉或重用）。
    // 順序和 post 的行一起排；回傳空字串就不印
    void postTask(Task fn)
    {
        std::unique_lock<std::mutex> lk(m_Mutex);
        if (m_Stopped) {
            lk.unlock();
            runTask(fn);
            return;
        }
        if (m_Queue.size() >= m_Max) {
            m_Dropped++;
            return;
        }
        if (!m_Thread.joinable()) {
            m_Thread = std::thread([this] { run(); });
        }
        m_Queue.push_back(std::move(fn));
        lk.unlock();
        m_Cv.notify_one();
    }

#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    void postf(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) {
            va_end(ap2);
            return;
        }
        if (static_cast<size_t>(n) < sizeof(buf)) {
            va_end(ap2);
            post(std::string(buf, static_cast<size_t>(n)));
            return;
        }
        std::string s(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(&s[0], s.size(), fmt, ap2);
        va_end(ap2);
        s.resize(static_cast<size_t>(n));
        post(std::move(s));
    }

    // 印完佇列裡剩下的、收掉執行緒；可以重複呼叫，之後的 post 會重新起執行緒
    void stop()
    {
        std::unique_lock<std::mutex> lk(m_Mutex);
        if (m_Stopped) {
            return;
        }
        m_Stopped = true;
        lk.unlock();
        m_Cv.notify_one();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
        lk.lock();
        const uint64_t dropped = m_Dropped;
        m_Dropped = 0;
        lk.unlock();
        if (dropped > 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "[VIPLE-VR-LOGASYNC] dropped %llu line(s) (queue full)",
                          static_cast<unsigned long long>(dropped));
            m_Sink(buf);
        }
        lk.lock();
        m_Thread = std::thread();
        m_Stopped = false;
    }

    uint64_t dropped() const
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        return m_Dropped;
    }

private:
    void runTask(const Task& fn)
    {
        const std::string line = fn();
        if (!line.empty()) {
            m_Sink(line);
        }
    }

    void run()
    {
        std::unique_lock<std::mutex> lk(m_Mutex);
        while (true) {
            m_Cv.wait(lk, [this] { return m_Stopped || !m_Queue.empty(); });
            while (!m_Queue.empty()) {
                Task fn = std::move(m_Queue.front());
                m_Queue.pop_front();
                lk.unlock();
                runTask(fn);
                lk.lock();
            }
            if (m_Stopped) {
                return;
            }
        }
    }

    Sink m_Sink;
    const size_t m_Max;
    mutable std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<Task> m_Queue;
    std::thread m_Thread;
    uint64_t m_Dropped = 0;
    bool m_Stopped = false;
};

// 自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)（簽名同 xrlatch::selfTest）
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto caseDone = [&](const char* name, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        report(name, ok, detail);
    };
    {
        // 照順序印、stop() 印完才返回；超過 1024 字的行走另一條路也要完整；postTask 排在同一個佇列；stop 之後再 post 會重新起執行緒
        std::mutex m;
        std::string got;
        AsyncLines a([&](const std::string& s) {
            std::lock_guard<std::mutex> lk(m);
            got += s;
            got += '|';
        });
        for (int i = 0; i < 20; i++) {
            a.postf("L%d", i);
        }
        const std::string longLine(1500, 'x');
        a.postf("%s", longLine.c_str());
        // postTask：到了印的執行緒才算，排在同一個佇列裡；回傳空字串不印
        a.postTask([]() { return std::string("task"); });
        a.postTask([]() { return std::string(); });
        a.stop();
        a.post("after");
        a.stop();
        std::string want;
        for (int i = 0; i < 20; i++) {
            want += "L" + std::to_string(i) + "|";
        }
        want += longLine + "|task|after|";
        caseDone("logasync-order-flush", got == want, got.size() > 80 ? got.substr(0, 80) + "..." : got);
    }
    {
        // sink 卡住時佇列滿了就丟、計數，stop() 補一行說丟了幾行
        std::mutex gate;
        std::unique_lock<std::mutex> hold(gate);
        std::mutex m;
        std::string got;
        int calls = 0;
        AsyncLines a(
            [&](const std::string& s) {
                std::lock_guard<std::mutex> g(gate);  // 第一行就卡在這裡，直到下面放開
                std::lock_guard<std::mutex> lk(m);
                calls++;
                got = s;
            },
            4);
        for (int i = 0; i < 10; i++) {
            a.postf("L%d", i);
        }
        // 執行緒最多已經取走 1 行（卡在 sink 裡），佇列最多 4 行 → 丟 5 或 6 行
        const uint64_t dropped = a.dropped();
        hold.unlock();
        a.stop();
        char buf[160];
        std::snprintf(buf, sizeof(buf), "dropped=%llu calls=%d last=%s", static_cast<unsigned long long>(dropped), calls, got.c_str());
        const bool ok = (dropped == 5 || dropped == 6) && calls == static_cast<int>(10 - dropped) + 1 &&
                        got == "[VIPLE-VR-LOGASYNC] dropped " + std::to_string(dropped) + " line(s) (queue full)";
        caseDone("logasync-drop-full", ok, buf);
    }
    return failures;
}

}  // namespace xrlog
