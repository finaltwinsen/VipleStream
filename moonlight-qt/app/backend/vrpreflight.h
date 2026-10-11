// VipleStream 2.0 §VR-PREFLIGHT — 開 VR 串流之前先量到主機的往返時間（`[VIPLE-VR-LAUNCHER] preflight: …`）。
//
// 為什麼要有：頭盔和主機經 VPN 連線時，VPN 閒置幾分鐘後常從直連掉到中繼。2026-10-11 使用者開了四次 VR、
// 有三次一開始就在中繼上：畫面每 10 秒只有 30～85 張（應該是 600 張）、動作到畫面約 1 秒、約 50 秒後子行程以
// 「connection terminated」結束，介面上沒有任何一句話說明原因。這裡在開子行程之前先量，太慢就明講、繼續量、
// 變快了自動開始；使用者隨時可以選「仍要開始」或取消。
//
// 這個檔只有判斷邏輯：header-only、只用標準函式庫（不依賴 Qt）；時間由呼叫端傳入，這裡不讀時鐘。
// GUI（backend/vrpreflightcheck.cpp）、命令列的 --vr-test-preflight 檢查、獨立測試
// （moonlight-qt/tests/vr_preflight/）都 include 這一個檔。
//
// 流程（Gate）：
//   begin → Checking：一次一次量，最多 kInitialSamples 次、取最小值。
//     ・有一次 ≤ kThresholdMs → 立刻開始。不等剩下的：最小值已經不會超過門檻，結果跟量滿三次一樣
//     ・每一次都量不到（拒絕連線、逾時）→ 照原本的方式開始（連不上的錯誤由串流那一段回報）
//     ・最小值 > kThresholdMs → Waiting
//   Waiting（介面顯示對話框）：每 kProbeIntervalMs 量一次，連續 kGoodStreakToStart 次 ≤ 門檻就自動開始；
//     中間有一次超過門檻或量不到，連續次數歸零。使用者可以隨時 startAnyway() 或 cancel()。
//
// 已知的限制：
//   - 量不到不等於連線好。往返超過 kSampleTimeoutMs 的路徑每一次都逾時，結果是「量不到 → 照常開始」。
//     中繼在閒置時是 95～130 ms，量得到；真的超過 1 秒的路徑這裡擋不住。
//   - 量的是 TCP 連線建立的時間（一次握手＝一次往返），串流走 UDP。兩者走同一條 VPN 通道；
//     VPN 以外、對 TCP 和 UDP 分開處理的網路不保證一樣。
//
// 執行緒：Gate、TestScript 都沒有鎖，只給一條執行緒用（GUI 執行緒）。

#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vrpreflight {

// 門檻（毫秒，含）。專案的計畫（docs/vr_architecture.md §7「WAN 閘門」）：往返 ≤ 25 ms 直接允許、25–60 ms
// 允許並提示、> 60 ms 時 PCVR 預設停用。實測（2026-10-11，頭盔與主機經 VPN）：直連 8～15 ms，VR 正常；
// 掉到中繼時閒置 95～130 ms、串流中 1～2 秒，不能玩。60 ms 落在兩群中間，兩邊都有餘裕。
// 目前是常數，還沒有對應的設定。
constexpr int kThresholdMs = 60;

constexpr int kInitialSamples = 3;        // 一開始最多量幾次（取最小值）
constexpr int kSampleTimeoutMs = 1000;    // 每一次最多等多久
constexpr int kProbeIntervalMs = 2000;    // Waiting 時多久量一次（從上一次開始量的時刻起算）
constexpr int kGoodStreakToStart = 3;     // 連續幾次不超過門檻就自動開始

// Waiting 的 log 量：對話框可能開很久（使用者放著等 VPN 回到直連）。前 kWaitLogAllProbes 次每次都記；
// 之後每 kWaitLogEvery 次記一次，另外「跟上一次不同類（夠快／太慢／量不到）」與「正在累計連續次數」照記。
constexpr int kWaitLogAllProbes = 30;     // 約一分鐘
constexpr int kWaitLogEvery = 15;         // 約 30 秒

// 一次量測的結果
struct Sample {
    bool ok = false;      // false＝量不到（拒絕連線、逾時、其他錯誤）
    int rttMs = -1;       // ok 時的往返時間（毫秒，四捨五入）
    std::string why;      // 量不到的原因（"timeout"、"refused"…），只給 log 用

    static Sample measured(int ms)
    {
        Sample s;
        s.ok = true;
        s.rttMs = ms < 0 ? 0 : ms;
        return s;
    }

    static Sample failed(const std::string& reason)
    {
        Sample s;
        s.why = reason;
        return s;
    }
};

// 門檻本身算夠快（計畫裡 25–60 ms 是「允許」）。判斷用的就是介面上顯示的那個整數毫秒，兩者不會對不上
inline bool isFastEnough(const Sample& s)
{
    return s.ok && s.rttMs <= kThresholdMs;
}

enum class Phase {
    Idle,       // 沒有在檢查
    Checking,   // 開頭那幾次還在量
    Waiting,    // 太慢：等使用者決定，同時繼續量
};

// 呼叫端接著要做的事
enum class Next {
    None,         // 不用做事（這個事件來的時候已經不在檢查中）
    ProbeNow,     // 立刻再量一次
    ProbeLater,   // Step::delayMs 之後再量一次
    Launch,       // 開始串流（夠快、量不到、連續夠快、使用者堅持，四種都是這個）
    Cancelled,    // 這一次不開了
};

struct Step {
    Next next = Next::None;
    int delayMs = 0;                  // next == ProbeLater 時有效
    std::vector<std::string> log;     // 要印的行（不含字首 "[VIPLE-VR-LAUNCHER] preflight: "）
};

namespace detail {

inline std::string format(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

inline std::string describe(const Sample& s)
{
    if (s.ok) {
        return format("%d ms", s.rttMs);
    }
    return format("no answer (%s)", s.why.empty() ? "unknown" : s.why.c_str());
}

}  // namespace detail

class Gate
{
public:
    Phase phase() const { return m_Phase; }
    bool busy() const { return m_Phase != Phase::Idle; }

    // 下面三個在檢查結束之後保留最後的值（對話框淡出時還在顯示），下一次 begin() 才歸零
    int lastRttMs() const { return (m_HaveLast && m_Last.ok) ? m_Last.rttMs : -1; }   // 最近一次；量不到或還沒量＝-1
    int measuredRttMs() const { return m_MeasuredRttMs; }                             // 進 Waiting 的依據（開頭那幾次的最小值）；沒進過＝-1
    int goodStreak() const { return m_GoodStreak; }                                   // Waiting 時目前連續幾次夠快

    // 開始一次檢查。已經在檢查中＝不做事（Next::None）
    Step begin(int64_t nowMs)
    {
        Step step;
        if (m_Phase != Phase::Idle) {
            return step;
        }
        *this = Gate();
        m_Phase = Phase::Checking;
        m_BeginMs = nowMs;
        m_ProbeStartMs = nowMs;
        step.next = Next::ProbeNow;
        return step;
    }

    // 每一次真的開始量之前呼叫（ProbeLater 的間隔從這裡起算）
    void probeStarted(int64_t nowMs) { m_ProbeStartMs = nowMs; }

    // 一次量測有結果了
    Step onSample(int64_t nowMs, const Sample& s)
    {
        Step step;
        if (m_Phase == Phase::Idle) {
            return step;
        }
        m_Last = s;
        m_HaveLast = true;

        if (m_Phase == Phase::Checking) {
            m_Initial.push_back(s);
            const int n = static_cast<int>(m_Initial.size());
            step.log.push_back(detail::format("sample %d/%d: %s", n, kInitialSamples, detail::describe(s).c_str()));

            if (isFastEnough(s)) {
                step.log.push_back(detail::format("launch: %d ms <= %d ms (sample %d of %d, %lld ms after the request)",
                                                  s.rttMs, kThresholdMs, n, kInitialSamples,
                                                  static_cast<long long>(nowMs - m_BeginMs)));
                m_Phase = Phase::Idle;
                step.next = Next::Launch;
                return step;
            }
            if (n < kInitialSamples) {
                step.next = Next::ProbeNow;
                return step;
            }

            int best = -1;
            std::string all;
            for (const Sample& i : m_Initial) {
                if (i.ok && (best < 0 || i.rttMs < best)) {
                    best = i.rttMs;
                }
                if (!all.empty()) {
                    all += ", ";
                }
                all += i.ok ? detail::format("%d", i.rttMs) : (i.why.empty() ? std::string("unknown") : i.why);
            }
            if (best < 0) {
                step.log.push_back(detail::format("unmeasured: no answer on %d samples (%s) - launching as before", n, all.c_str()));
                m_Phase = Phase::Idle;
                step.next = Next::Launch;
                return step;
            }

            m_Phase = Phase::Waiting;
            m_MeasuredRttMs = best;
            m_GoodStreak = 0;
            step.log.push_back(detail::format("wait: best of %d samples is %d ms > %d ms (%s) - asking the user, probing every %d s",
                                              n, best, kThresholdMs, all.c_str(), kProbeIntervalMs / 1000));
            step.next = Next::ProbeLater;
            step.delayMs = delayToNextProbe(nowMs);
            return step;
        }

        // Waiting
        m_WaitProbes++;
        const bool fast = isFastEnough(s);
        m_GoodStreak = fast ? m_GoodStreak + 1 : 0;

        const int cls = !s.ok ? 2 : (fast ? 0 : 1);
        const bool logIt = m_WaitProbes <= kWaitLogAllProbes || cls != m_PrevClass || fast ||
                           (m_WaitProbes % kWaitLogEvery) == 0;
        m_PrevClass = cls;
        if (logIt) {
            std::string line = detail::format("probe %d: %s", m_WaitProbes, detail::describe(s).c_str());
            if (fast) {
                line += detail::format(" (fast enough %d/%d)", m_GoodStreak, kGoodStreakToStart);
            }
            if (m_NotLogged > 0) {
                line += detail::format(" (%d similar probes not logged)", m_NotLogged);
                m_NotLogged = 0;
            }
            step.log.push_back(line);
        }
        else {
            m_NotLogged++;
        }

        if (m_GoodStreak >= kGoodStreakToStart) {
            step.log.push_back(detail::format("auto-start after %lld s: %d ms or less on %d probes in a row (last %d ms)",
                                              secondsSinceBegin(nowMs), kThresholdMs, kGoodStreakToStart, s.rttMs));
            m_Phase = Phase::Idle;
            step.next = Next::Launch;
            return step;
        }
        step.next = Next::ProbeLater;
        step.delayMs = delayToNextProbe(nowMs);
        return step;
    }

    // 使用者選「仍要開始」（Checking 時也可以：等於不量了）
    Step startAnyway(int64_t nowMs)
    {
        Step step;
        if (m_Phase == Phase::Idle) {
            return step;
        }
        step.log.push_back(detail::format("override: the user chose to start anyway after %lld s (%s)",
                                          secondsSinceBegin(nowMs), describeLast().c_str()));
        m_Phase = Phase::Idle;
        step.next = Next::Launch;
        return step;
    }

    // 這一次不開了。who＝誰取消的（"the user"、"mode changed"…），只給 log 用
    Step cancel(int64_t nowMs, const char* who)
    {
        Step step;
        if (m_Phase == Phase::Idle) {
            return step;
        }
        step.log.push_back(detail::format("cancel: %s after %lld s (%s)", (who != nullptr && *who != '\0') ? who : "unknown",
                                          secondsSinceBegin(nowMs), describeLast().c_str()));
        m_Phase = Phase::Idle;
        step.next = Next::Cancelled;
        return step;
    }

private:
    long long secondsSinceBegin(int64_t nowMs) const
    {
        const int64_t ms = nowMs > m_BeginMs ? nowMs - m_BeginMs : 0;
        return static_cast<long long>((ms + 500) / 1000);
    }

    int delayToNextProbe(int64_t nowMs) const
    {
        const int64_t sinceStart = nowMs > m_ProbeStartMs ? nowMs - m_ProbeStartMs : 0;
        return static_cast<int>(std::max<int64_t>(0, kProbeIntervalMs - sinceStart));
    }

    std::string describeLast() const
    {
        if (!m_HaveLast) {
            return "nothing measured yet";
        }
        return "last " + detail::describe(m_Last);
    }

    Phase m_Phase = Phase::Idle;
    int64_t m_BeginMs = 0;
    int64_t m_ProbeStartMs = 0;
    std::vector<Sample> m_Initial;    // Checking 時量到的
    Sample m_Last;
    bool m_HaveLast = false;
    int m_MeasuredRttMs = -1;
    int m_GoodStreak = 0;
    int m_WaitProbes = 0;             // Waiting 時量了幾次
    int m_PrevClass = -1;             // 上一次是哪一類（0 夠快、1 太慢、2 量不到）
    int m_NotLogged = 0;              // 上一行 log 之後省略了幾次
};

// dev：--vr-test-preflight <spec>。不連線，照腳本給量測結果：沒有慢的網路也能把對話框的每一條路走一遍。
// 只存在這個行程裡，不寫進設定；每一次開 VR 串流都從腳本的開頭再來。
//
//   spec ＝ <項>[,<項>…]
//   <項> ＝ <ms>[*<次數>]   量到 <ms> 毫秒（0–999）
//          x[*<次數>]      量不到（等同逾時）
//   最後一項用完之後一直重複。
//
//   例：150               一直 150 ms：對話框出現後不會自己開始（試「仍要開始」與「取消」）
//       150*8,30          前 8 次 150 ms（開頭 3 次＋對話框裡 5 次），之後都是 30 ms：對話框出現約 16 秒後自動開始
//       150*4,30*2,150,30 夠快兩次之後又慢一次：連續次數歸零，之後才累計到自動開始
//       x                 每次都量不到：照原本的方式直接開始
//       x*2,40            前兩次逾時、第三次 40 ms：約 2 秒後開始（中間會看到「正在檢查連線」）
class TestScript
{
public:
    static constexpr int kMaxItems = 64;
    static constexpr int kMaxCount = 100000;

    // 空字串＝關。格式不對回 false，*err 是可以直接給命令列的英文說明；設定維持「關」
    bool configure(const std::string& spec, std::string* err)
    {
        *this = TestScript();
        if (spec.empty()) {
            return true;
        }
        static const char* const kUsage =
            "expected a comma-separated list of <ms>[*<count>] or x[*<count>] "
            "(ms 0-999, x = no answer, count 1-100000); the last item repeats";

        std::vector<Item> items;
        size_t pos = 0;
        for (;;) {
            const size_t comma = spec.find(',', pos);
            const std::string token = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t star = token.find('*');
            const std::string value = token.substr(0, star);
            Item item;
            if (value == "x" || value == "X") {
                item.ok = false;
            }
            else {
                uint64_t ms = 0;
                // 往返一秒以上在實際量測裡就是逾時，用 x 表示
                if (!parseUint(value, kSampleTimeoutMs - 1, &ms)) {
                    return fail(err, kUsage);
                }
                item.ok = true;
                item.ms = static_cast<int>(ms);
            }
            if (star != std::string::npos) {
                uint64_t count = 0;
                if (!parseUint(token.substr(star + 1), kMaxCount, &count) || count == 0) {
                    return fail(err, kUsage);
                }
                item.count = static_cast<int>(count);
            }
            items.push_back(item);
            if (static_cast<int>(items.size()) > kMaxItems) {
                return fail(err, "too many items (at most 64)");
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }

        m_Items = items;
        m_Spec = spec;
        return true;
    }

    bool enabled() const { return !m_Items.empty(); }
    const std::string& spec() const { return m_Spec; }

    // 回到腳本的開頭
    void rewind()
    {
        m_Index = 0;
        m_Used = 0;
    }

    // 下一次量測的結果
    Sample next()
    {
        if (m_Items.empty()) {
            return Sample::failed("no script");
        }
        const Item& item = m_Items[m_Index];
        const Sample s = item.ok ? Sample::measured(item.ms) : Sample::failed("scripted");
        if (++m_Used >= item.count && m_Index + 1 < m_Items.size()) {
            m_Index++;
            m_Used = 0;
        }
        return s;
    }

private:
    struct Item {
        bool ok = false;
        int ms = 0;
        int count = 1;
    };

    static bool fail(std::string* err, const char* message)
    {
        if (err != nullptr) {
            *err = message;
        }
        return false;
    }

    // 只接受十進位數字（不接受正負號、空白、空字串），而且不超過 max
    static bool parseUint(const std::string& text, uint64_t max, uint64_t* out)
    {
        if (text.empty() || text.size() > 9) {
            return false;
        }
        uint64_t v = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        if (v > max) {
            return false;
        }
        *out = v;
        return true;
    }

    std::vector<Item> m_Items;
    std::string m_Spec;
    size_t m_Index = 0;
    int m_Used = 0;
};

// 腳本的結果隔多久才交給 Gate：量到的照往返時間、量不到的照逾時，介面上的節奏才跟真的量測一樣
inline int simulatedDurationMs(const Sample& s)
{
    return s.ok ? std::min(s.rttMs, kSampleTimeoutMs) : kSampleTimeoutMs;
}

}  // namespace vrpreflight
