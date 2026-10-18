// VipleStream 2.0 §VR-LATCH-FLIGHT — 「等不等在途的幀」的判斷（純邏輯）。
//
// frame thread 挑影像（latch）之前，如果有一張串流影像已經送進 GPU、只差零點幾毫秒就做完，多等它一下，這一格就顯示得到
// 它；不等的話這一格重複上一張、它晚一格才出現。這個檔只有「要不要等、最晚等到什麼時候」的判斷與它用到的幾個小狀態：
// 真正的等待在 XrContext::pcvrFlightWait，在途的標記在 XrVideo::renderNewFrame。
//
// header-only，只用標準函式庫（不依賴 Qt／OpenXR／libplacebo），做法同 xrlatchstats.h、xrphase.h：沒有 OpenXR 的建置也編
// 得到，`xr-probe --selftest-latch` 會接著跑這裡的 selfTest()。全部用整數（微秒）算，沒有浮點。
//
// 四個條件都成立才等。decide() 照下面的順序檢查，兩個同時不成立時記前面那一個（10 秒行的計數才對得上）：
//   F1 有一張在途：flightSeq == latestSeq + 1。影像 render thread 在這張影像的 GPU 工作都送出去之後設、發布之後清。
//   F2 空格閘門  ：latestSeq == shownSeq 而且 shownCount ≥ streamDiv——目前發布的那一張就是正在顯示的，而且已經顯示滿
//                  它該佔的格數（全速 1、半速 2），也就是這一格本來就要重複。半速＋補幀時「該顯示合成那一半」的那一格
//                  不等：等到的新影像會提早一格顯示，上一張的合成那一格就被跳過了。
//   F3 快做完了  ：(now − flightSubmitUs) + capUs ≥ gTypUs——照典型的 GPU 時間，上限之內做得完。已經超過典型值的
//                  （被頭盔合成器擋住的那一群）一律等；剛送出的不等，等也等不到。
//   F4 還有空間  ：deadline = min(now + capUs, wakeUs + spanMaxUs)，而且 deadline > now + kMinRoomUs。
// 另外一條（2026-10-11 延後量的量測加的）：這一格在 xrEndFrame 之後要做 10 秒彙總、印 log 的話不等（Why::Busy）。那一格
// 回到 xrWaitFrame 本來就最晚，再晚交幀會貼到 runtime 的放行點。它排在最後才看，所以 busy 數的是「本來會等、因為這一條
// 沒等」的格。

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace xrflight {

// F4：離截止時刻不到這麼多就不等（叫一次定時等待本身就要這個量級）
constexpr uint32_t kMinRoomUs = 20;
// F3 的上限：送出之後比典型值多過這麼久還沒做完，當成卡住、不等。GPU 卡死要 1 秒才判定，沒有這一條的話
// 那 1 秒裡每一格都會等到截止時刻
constexpr uint32_t kStuckUs = 16000;
// xrWaitFrame 擋住不到這麼久就返回＝上一格晚回、放行點已經過了，這一格的返回時刻本身就晚。截止時刻以返回時刻算，
// 在這種格等滿會超出 Z2 驗過的範圍（那幾場晚回是 0 格），所以不等（記在 noRoom）
constexpr uint32_t kMinWakeBlockUs = 150;
// 等不到這麼久就等到的：判斷點到挑影像之間本來就還有 0.1～0.15 ms，這種多半不等也趕得上這一格。
// 10 秒行另外數（quick），效果看 got − quick
constexpr uint32_t kQuickUs = 150;
// g＝一張影像的 GPU 工作「送出 → 做完」的時間。典型值的估計：起始值、上下限、每幾張更新一次
// （起始值是頭盔上量到的 1.53 ms；上下限與張數是設計值）
constexpr uint32_t kGpuTypInitUs = 1500;
constexpr uint32_t kGpuTypMinUs = 300, kGpuTypMaxUs = 5000;
constexpr size_t kGpuTypWindow = 64;

struct Input {
    bool active = false;          // 這一邊開著、而且防護沒有暫停
    bool busy = false;            // 這一格 xrEndFrame 之後要做 10 秒彙總、印 log（見 windowDue）
    uint64_t nowUs = 0;           // 現在（steady 時鐘，µs；下面的時刻都是同一個時鐘）
    uint64_t wakeUs = 0;          // 這一格 xrWaitFrame 返回的時刻
    uint32_t wakeBlockedUs = 0xffffffffu;  // 這一格 xrWaitFrame 擋住了多久（0xffffffff＝不知道，不看這一條）
    uint32_t capUs = 0;           // 設定的上限：最多等多久
    uint32_t spanMaxUs = 0;       // xrWaitFrame 返回之後最晚等到多久（0＝沒有量過可以等到多晚，一律沒有空間）
    uint64_t shownSeq = 0;        // frame thread 正在顯示的那一張
    uint32_t shownCount = 0;      // 它已經顯示了幾格
    uint32_t streamDiv = 1;       // 串流一張佔幾個顯示格（0 當成 1）
    uint64_t latestSeq = 0;       // 目前發布的那一張（還沒有發布過＝0）
    uint64_t flightSeq = 0;       // 已經送進 GPU、還沒發布的那一張發布後的序號（0＝沒有）
    uint64_t flightSubmitUs = 0;  // 它送出的時刻
    uint32_t gTypUs = kGpuTypInitUs;  // g 的典型值
};

// Wait＝等；其餘是不等的原因
enum class Why { Wait, Off, NoFlight, Gate, Far, NoRoom, Busy };

struct Decision {
    Why why = Why::Off;
    uint64_t deadlineUs = 0;  // why == Wait 才有意義：最晚等到這個時刻
};

inline const char* whyName(Why why)
{
    switch (why) {
    case Why::Wait: return "wait";
    case Why::Off: return "off";
    case Why::NoFlight: return "noFlight";
    case Why::Gate: return "gate";
    case Why::Far: return "far";
    case Why::NoRoom: return "noRoom";
    case Why::Busy: return "busy";
    }
    return "?";
}

// 判斷順序固定：Off → NoFlight（F1）→ Gate（F2）→ Far（F3，含卡住）→ NoRoom（F4，含沒擋住就返回的格）→ Busy → Wait
inline Decision decide(const Input& in)
{
    Decision d;
    if (!in.active) {
        d.why = Why::Off;
        return d;
    }
    // F1：在途的那一張發布之後就是「下一張」。flightSeq == latestSeq 是影像 render thread 剛發布、還沒清標記的那一瞬間；
    // 差兩張以上是兩邊讀到的時刻對不上，都當成沒有
    if (in.flightSeq == 0 || in.flightSeq != in.latestSeq + 1) {
        d.why = Why::NoFlight;
        return d;
    }
    // F2
    const uint32_t div = in.streamDiv == 0 ? 1u : in.streamDiv;
    if (in.latestSeq != in.shownSeq || in.shownCount < div) {
        d.why = Why::Gate;
        return d;
    }
    // F3：兩條執行緒各自取時鐘，送出的時刻可能比 now 晚一點——已經過的時間當成 0，不可以無號相減
    const uint64_t elapsedUs = in.nowUs > in.flightSubmitUs ? in.nowUs - in.flightSubmitUs : 0;
    if (elapsedUs + in.capUs < in.gTypUs || elapsedUs > static_cast<uint64_t>(in.gTypUs) + kStuckUs) {
        d.why = Why::Far;
        return d;
    }
    // F4
    const uint64_t deadlineUs = (std::min)(in.nowUs + in.capUs, in.wakeUs + in.spanMaxUs);
    if (in.wakeBlockedUs < kMinWakeBlockUs || deadlineUs <= in.nowUs + kMinRoomUs) {
        d.why = Why::NoRoom;
        return d;
    }
    if (in.busy) {
        d.why = Why::Busy;
        return d;
    }
    d.why = Why::Wait;
    d.deadlineUs = deadlineUs;
    return d;
}

// 一個每 len 滿一次的統計視窗（起點 start）在「now＋look」之前會不會到期；四個值同一個單位。frame thread 在等之前
// 用它預測「這一格會不會印 10 秒行」：印的那一格是第一個時刻 ≥ start＋len 的格，而那個時刻比等待點晚一點（等待本身、
// xrBeginFrame、xrEndFrame），所以往前看一段。寧可多跳過一兩格。start＝0＝還沒開始計時，也當成會
inline bool windowDue(uint64_t now, uint64_t start, uint64_t len, uint64_t look)
{
    return start == 0 || now + look >= start + len;
}

// frame thread 正在顯示哪一張、已經顯示了幾格（F2 用）。只有真的送出這張影像的那一格才呼叫 onLatched()：
// 沒送出影像層的格不加也不歸零——少算只會讓閘門不開（等於沒有這個功能），多算才會在不該等的格放行。
// 起始 (0, 0)：還沒有任何影像時 latestSeq 也是 0，但 0 格 < streamDiv，閘門不開。
struct Shown {
    uint64_t seq = 0;
    uint32_t count = 0;
    void onLatched(uint64_t latchedSeq)
    {
        if (latchedSeq != seq) {
            seq = latchedSeq;
            count = 1;
        }
        else if (count != 0xffffffffu) {
            count++;
        }
    }
};

// g 的典型值：每 kGpuTypWindow 張取一次中位數（被合成器擋住的那幾張不會把它拉走），和舊值做 1／4 的平滑。
// 中位數先夾在上下限之內，所以估計值也一直在上下限之內，一個全是離群值的視窗最多把它推四分之一。
// 只在影像 render thread 用；不配置記憶體
class GpuTyp
{
public:
    // 推一筆樣本（µs）。湊滿一個視窗就更新估計值並回 true
    bool push(uint32_t sampleUs)
    {
        m_Ring[m_N++] = sampleUs;
        if (m_N < kGpuTypWindow) {
            return false;
        }
        m_N = 0;
        uint32_t sorted[kGpuTypWindow];
        std::copy(m_Ring, m_Ring + kGpuTypWindow, sorted);
        std::nth_element(sorted, sorted + kGpuTypWindow / 2, sorted + kGpuTypWindow);
        const uint32_t median = (std::min)((std::max)(sorted[kGpuTypWindow / 2], kGpuTypMinUs), kGpuTypMaxUs);
        m_TypUs = (3u * m_TypUs + median + 2u) / 4u;
        return true;
    }
    uint32_t typUs() const { return m_TypUs; }

private:
    uint32_t m_Ring[kGpuTypWindow] = {};
    size_t m_N = 0;
    uint32_t m_TypUs = kGpuTypInitUs;
};

// 量測（2026-10-18）：逾時、或判成「太遠」（F3 的前半，照典型值上限內做不完）的那一格，在途的那一張後來什麼時候發布。
// 2026-10-17 的頭盔場次逾時佔等待的三到六成，只知道醒來準不準（toLate），不知道差多少才等得到：
//   missBy＝逾時的那一張發布的時刻 − 截止時刻（多半只差 0.1～0.3 ms 的話，放寬上限或改 F3 的估法就收得回來）；
//   farBy ＝太遠的那一張發布的時刻 −（判斷時刻＋上限）（≤ 0 印 0＝其實上限內就做完了，F3 估得太保守）。
// frame thread 記下一筆，之後每一格去 XrVideo 的發布紀錄找那一張；找不到又過了 kPendingGiveUpUs 就算 unk
// （發布紀錄只留 64 張，或那一張根本沒發布）
constexpr uint64_t kPendingGiveUpUs = 50000;

struct Pending {
    uint64_t seq = 0;    // 在途的那一張發布後的序號；0＝沒有
    uint64_t refUs = 0;  // 比較的時刻：逾時＝截止時刻；太遠＝判斷時刻＋上限
    uint64_t setUs = 0;  // 記下的時刻
    bool tooFar = false; // false＝逾時（missBy）、true＝太遠（farBy）
};

enum class Resolve { Keep, Done, Unknown };

// found＝發布紀錄裡找到這一張；pubUs＝它發布的時刻。Done 時 *outUs＝pubUs − refUs（早於 refUs 記 0）
inline Resolve resolvePending(const Pending& p, bool found, uint64_t pubUs, uint64_t nowUs, uint32_t* outUs)
{
    if (found) {
        const uint64_t d = pubUs > p.refUs ? pubUs - p.refUs : 0;
        *outUs = static_cast<uint32_t>((std::min<uint64_t>)(d, 0xffffffffu));
        return Resolve::Done;
    }
    const uint64_t age = nowUs > p.setUs ? nowUs - p.setUs : 0;
    return age >= kPendingGiveUpUs ? Resolve::Unknown : Resolve::Keep;
}

// frame thread 的 10 秒視窗：`[VIPLE-VR-LDIAG] 10s` 行尾 flight 那一段的計數，與每次等了多久
struct Window {
    uint32_t waited = 0;   // 等了幾格（＝got＋timeout）
    uint32_t got = 0;      //   其中等到新影像的。不等就會錯過這一格的是 got − quick
    uint32_t timeout = 0;  //   與等到截止時刻的
    uint32_t quick = 0;    // got 裡等不到 kQuickUs 就等到的（多半不等也趕得上）
    // 下面四個只數「有一張在途、卻因為那個條件沒有等」的格。「太遠」印出來的欄位叫 far，成員不可以取這個名字：
    // Windows 的標頭把 far／near 定義成空的巨集
    uint32_t gate = 0, tooFar = 0, noRoom = 0, busy = 0;
    std::vector<uint32_t> waitUs;  // 每次等了多久（µs）
    std::vector<uint32_t> lateUs;  // 逾時的那幾次：醒來比截止時刻晚多少（µs）——定時等待準不準
    // 量測（見 Pending）：逾時的那一張晚截止時刻多少才發布、太遠的那一張晚「判斷＋上限」多少才發布；unk＝沒對到
    std::vector<uint32_t> missUs, farByUs;
    uint32_t missUnk = 0, farUnk = 0;

    // 120 Hz 的 10 秒是 1200 格。先配好：noteWait() 在 xrBeginFrame 之前呼叫，那裡不可以配置記憶體
    void reserveWindow()
    {
        waitUs.reserve(2048);
        lateUs.reserve(2048);
        missUs.reserve(2048);
        farByUs.reserve(2048);
    }

    // Pending 有結果了（Done 帶 us；Unknown 只計數）。Keep 不該傳進來
    void notePending(const Pending& p, Resolve r, uint32_t us)
    {
        std::vector<uint32_t>& v = p.tooFar ? farByUs : missUs;
        if (r == Resolve::Done) {
            if (v.size() < v.capacity()) {
                v.push_back(us);
            }
        }
        else if (r == Resolve::Unknown) {
            (p.tooFar ? farUnk : missUnk)++;
        }
    }

    // 沒有等的那一格。Wait（由 noteWait 記）、Off、NoFlight 不計
    void note(Why why)
    {
        switch (why) {
        case Why::Gate: gate++; break;
        case Why::Far: tooFar++; break;
        case Why::NoRoom: noRoom++; break;
        case Why::Busy: busy++; break;
        default: break;
        }
    }

    // 等過的那一格：有沒有等到、等了多久、逾時的話醒來比截止時刻晚多少。預留的空間滿了就不記時間（計數照記）
    void noteWait(bool gotIt, uint32_t waitedUs, uint32_t lateByUs = 0)
    {
        waited++;
        if (gotIt) {
            got++;
            if (waitedUs < kQuickUs) {
                quick++;
            }
        }
        else {
            timeout++;
            if (lateUs.size() < lateUs.capacity()) {
                lateUs.push_back(lateByUs);
            }
        }
        if (waitUs.size() < waitUs.capacity()) {
            waitUs.push_back(waitedUs);
        }
    }

    void clear()
    {
        waited = got = timeout = quick = gate = tooFar = noRoom = busy = 0;
        missUnk = farUnk = 0;
        waitUs.clear();
        lateUs.clear();
        missUs.clear();
        farByUs.clear();
    }

    // 組出 10 秒行的那一段（不清空；呼叫端印完再 clear()）。effUs＝實際生效的上限（不作用時 0）；paused＝防護暫停中；
    // gTypUs＝frame thread 最近一次判斷時用的 g 典型值（0＝還沒判斷過）。
    // 到 guard= 為止是設計文件訂的欄位與順序；busy、gTyp、quick、toLate 是後來加的，照「新欄位加在後面」接在這一段的最後。
    // p50 的位置同 XrContext 的 pctOf（第 n/2 個），p90 是第 n*9/10 個；沒有樣本時印 0。會重排各個 vector。
    // missBy／farBy 是 2026-10-18 加的量測（見 Pending），n＝對到的筆數、unk＝沒對到的
    std::string tail(uint32_t effUs, bool paused, uint32_t gTypUs)
    {
        uint32_t p50 = 0, maxUs = 0, lateP50 = 0, lateMax = 0;
        auto pick = [](std::vector<uint32_t>& v, uint32_t* p, uint32_t* mx) {
            if (v.empty()) {
                return;
            }
            const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(v.size() / 2);
            std::nth_element(v.begin(), v.begin() + m, v.end());
            *p = v[static_cast<size_t>(m)];
            *mx = *std::max_element(v.begin(), v.end());
        };
        auto p90 = [](std::vector<uint32_t>& v) -> uint32_t {
            if (v.empty()) {
                return 0;
            }
            const std::ptrdiff_t m = static_cast<std::ptrdiff_t>(v.size() * 9 / 10);
            std::nth_element(v.begin(), v.begin() + m, v.end());
            return v[static_cast<size_t>(m)];
        };
        pick(waitUs, &p50, &maxUs);
        pick(lateUs, &lateP50, &lateMax);
        uint32_t missP50 = 0, missMax = 0, farP50 = 0, farMax = 0;
        const uint32_t missP90 = p90(missUs), farP90 = p90(farByUs);
        pick(missUs, &missP50, &missMax);
        pick(farByUs, &farP50, &farMax);
        const uint32_t hundredths = (effUs + 5u) / 10u;  // 毫秒、兩位小數（四捨五入），用整數組
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      " | flight=%u.%02u ms waited=%u got=%u timeout=%u gate=%u far=%u noRoom=%u wait p50/max=%u/%u us guard=%s"
                      " busy=%u gTyp=%u us quick=%u toLate p50/max=%u/%u us"
                      " missBy p50/p90/max=%u/%u/%u us n=%u unk=%u farBy p50/p90/max=%u/%u/%u us n=%u unk=%u",
                      hundredths / 100u, hundredths % 100u, waited, got, timeout, gate, tooFar, noRoom, p50, maxUs,
                      paused ? "paused" : "ok", busy, gTypUs, quick, lateP50, lateMax,
                      missP50, missP90, missMax, static_cast<unsigned>(missUs.size()), missUnk,
                      farP50, farP90, farMax, static_cast<unsigned>(farByUs.size()), farUnk);
        return buf;
    }
};

// 自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)（簽名同 xrlatch::selfTest）
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto caseDone = [&](const char* name, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        if (report) {
            report(name, ok, detail);
        }
    };
    char buf[320];
    // 四個條件都成立的一格：全速、xrWaitFrame 返回後 6.085 ms（延後 6.0 ms 實測的位置）、第 7 張已經顯示過一格、
    // 第 8 張送出 1.4 ms 了、上限 0.5 ms、最晚等到返回後 6.5 ms
    auto ready = []() {
        Input in;
        in.active = true;
        in.wakeUs = 1000000;
        in.nowUs = 1006085;
        in.capUs = 500;
        in.spanMaxUs = 6500;
        in.shownSeq = 7;
        in.shownCount = 1;
        in.streamDiv = 1;
        in.latestSeq = 7;
        in.flightSeq = 8;
        in.flightSubmitUs = in.nowUs - 1400;
        in.gTypUs = 1500;
        return in;
    };
    auto why = [](const Input& in) { return decide(in).why; };

    {
        // 基準：等，最晚等到返回後 6.5 ms（比 now＋0.5 ms 早）→ 實際能等 415 µs
        const Decision d = decide(ready());
        std::snprintf(buf, sizeof(buf), "%s, deadline wake+%llu us, room %llu us", whyName(d.why),
                      static_cast<unsigned long long>(d.deadlineUs - 1000000), static_cast<unsigned long long>(d.deadlineUs - 1006085));
        caseDone("flight-wait-baseline", d.why == Why::Wait && d.deadlineUs == 1006500, buf);
    }
    {
        // 這一邊關著（同場切換的另一邊、防護暫停中）：其餘都成立也不等
        Input in = ready();
        in.active = false;
        const Decision d = decide(in);
        caseDone("flight-off", d.why == Why::Off && d.deadlineUs == 0, whyName(d.why));
    }
    {
        // F1：沒有在途的（0）；剛發布、標記還沒清（flightSeq == latestSeq）；差兩張（兩邊讀到的時刻對不上）。
        // 0 永遠是「沒有」：就算 latestSeq 是最大值、加一繞回 0，也不可以當成對上了
        Input a = ready(), b = ready(), c = ready(), e = ready();
        a.flightSeq = 0;
        b.flightSeq = b.latestSeq;
        c.flightSeq = c.latestSeq + 2;
        e.latestSeq = e.shownSeq = 0xffffffffffffffffull;
        e.flightSeq = 0;
        std::snprintf(buf, sizeof(buf), "none -> %s, just published -> %s, two ahead -> %s, none at the last seq -> %s", whyName(why(a)),
                      whyName(why(b)), whyName(why(c)), whyName(why(e)));
        caseDone("flight-none-in-flight",
                 why(a) == Why::NoFlight && why(b) == Why::NoFlight && why(c) == Why::NoFlight && why(e) == Why::NoFlight, buf);
    }
    {
        // F2：最新的那一張（7）還沒顯示過（正在顯示的是 6）——這一格本來就會換成 7，不是重複格
        Input in = ready();
        in.shownSeq = 6;
        caseDone("flight-gate-latest-not-shown", why(in) == Why::Gate, whyName(why(in)));
    }
    {
        // F2：半速，第 7 張只顯示了一格——這一格是它的第二格（補幀開著時顯示合成那一半），不等
        Input in = ready();
        in.streamDiv = 2;
        in.shownCount = 1;
        caseDone("flight-gate-half-rate-b-slot", why(in) == Why::Gate, whyName(why(in)));
    }
    {
        // F2 通過：半速顯示滿兩格；全速顯示滿一格；streamDiv 是 0（還沒設）當成 1；顯示超過該佔的格數（已經在重複）。
        // streamDiv 是 0 不等於「不必顯示過」：一格都還沒顯示（0 格）照樣擋
        Input a = ready(), b = ready(), c = ready(), e = ready(), z = ready();
        a.streamDiv = 2;
        a.shownCount = 2;
        b.streamDiv = 1;
        b.shownCount = 1;
        c.streamDiv = 0;
        c.shownCount = 1;
        e.streamDiv = 2;
        e.shownCount = 5;
        z.streamDiv = 0;
        z.shownCount = 0;
        const bool ok = why(a) == Why::Wait && why(b) == Why::Wait && why(c) == Why::Wait && why(e) == Why::Wait && why(z) == Why::Gate;
        std::snprintf(buf, sizeof(buf), "half 2/2 -> %s, full 1/1 -> %s, div 0 1/0 -> %s, half 5/2 -> %s, div 0 0/0 -> %s", whyName(why(a)),
                      whyName(why(b)), whyName(why(c)), whyName(why(e)), whyName(why(z)));
        caseDone("flight-gate-open", ok, buf);
    }
    {
        // F3：剛送出 0.3 ms，典型還要 1.2 ms，等 0.5 ms 等不到
        Input in = ready();
        in.flightSubmitUs = in.nowUs - 300;
        caseDone("flight-far-just-submitted", why(in) == Why::Far, whyName(why(in)));
    }
    {
        // F3 通過：送出 1.0 ms（1000＋500 ≥ 1500，邊界含等號）；送出 2.5 ms（已經超過典型值：被合成器擋住的那一群）；
        // 差 1 µs 不到邊界就不過
        Input a = ready(), b = ready(), c = ready();
        a.flightSubmitUs = a.nowUs - 1000;
        b.flightSubmitUs = b.nowUs - 2500;
        c.flightSubmitUs = c.nowUs - 999;
        std::snprintf(buf, sizeof(buf), "1000 us -> %s, 2500 us -> %s, 999 us -> %s", whyName(why(a)), whyName(why(b)), whyName(why(c)));
        caseDone("flight-near-done-and-overdue", why(a) == Why::Wait && why(b) == Why::Wait && why(c) == Why::Far, buf);
    }
    {
        // 送出的時刻比 now 晚（兩條執行緒各自取時鐘）：已經過的時間當成 0。直接無號相減會得到一個接近 2^64 的數，
        // 再加上限又繞回來變成「上限 − 晚的量」，所以兩種錯法都要驗：
        //   晚 40 µs、典型值 1500：0＋500 < 1500 → Far
        //   晚 40 µs、典型值 480 ：0＋500 ≥ 480 → 等（相減再繞回來會是 460 < 480，誤判成 Far）
        //   晚 5 ms（比上限還多）、典型值 1500：0＋500 < 1500 → Far（相減之後是個天文數字，會誤判成等）
        Input a = ready(), b = ready(), c = ready();
        a.flightSubmitUs = a.nowUs + 40;
        b.flightSubmitUs = b.nowUs + 40;
        b.gTypUs = 480;
        c.flightSubmitUs = c.nowUs + 5000;
        std::snprintf(buf, sizeof(buf), "40 us late, typ 1500 -> %s; typ 480 -> %s; 5 ms late, typ 1500 -> %s", whyName(why(a)),
                      whyName(why(b)), whyName(why(c)));
        caseDone("flight-submit-after-now", why(a) == Why::Far && why(b) == Why::Wait && why(c) == Why::Far, buf);
    }
    {
        // F4：返回後 6.490 ms、最晚 6.5 ms → 只剩 10 µs，不等。剩 20 µs 也不等、21 µs 才等。
        // spanMax 是 0（這個更新率沒有量過可以等到多晚）一律沒有空間
        Input a = ready(), b = ready(), c = ready(), e = ready();
        a.wakeUs = 0;
        a.nowUs = 6490;
        a.flightSubmitUs = a.nowUs - 1400;
        b.nowUs = b.wakeUs + 6480;
        b.flightSubmitUs = b.nowUs - 1400;
        c.nowUs = c.wakeUs + 6479;
        c.flightSubmitUs = c.nowUs - 1400;
        e.spanMaxUs = 0;
        const Decision dc = decide(c);
        const bool ok = why(a) == Why::NoRoom && why(b) == Why::NoRoom && dc.why == Why::Wait &&
                        dc.deadlineUs == c.wakeUs + 6500 && why(e) == Why::NoRoom;
        std::snprintf(buf, sizeof(buf), "10 us left -> %s, 20 us -> %s, 21 us -> %s, span 0 -> %s", whyName(why(a)), whyName(why(b)),
                      whyName(dc.why), whyName(why(e)));
        caseDone("flight-no-room", ok, buf);
    }
    {
        // 截止時刻：返回後 6.060 ms、上限 0.5 ms。最晚 6.5 ms → 截止在 6500（被它夾住）；最晚 6.8 ms → 6560（照上限）
        Input a = ready(), b = ready();
        a.wakeUs = b.wakeUs = 0;
        a.nowUs = b.nowUs = 6060;
        a.flightSubmitUs = b.flightSubmitUs = 6060 - 1400;
        b.spanMaxUs = 6800;
        const Decision da = decide(a), db = decide(b);
        std::snprintf(buf, sizeof(buf), "span 6500 -> %s until %llu, span 6800 -> %s until %llu", whyName(da.why),
                      static_cast<unsigned long long>(da.deadlineUs), whyName(db.why), static_cast<unsigned long long>(db.deadlineUs));
        caseDone("flight-deadline", da.why == Why::Wait && da.deadlineUs == 6500 && db.why == Why::Wait && db.deadlineUs == 6560, buf);
    }
    {
        // 兩個條件同時不成立時記前面那一個：閘門不過又太遠 → Gate；沒有在途又閘門不過 → NoFlight；太遠又沒有空間 → Far
        Input a = ready(), b = ready(), c = ready();
        a.shownSeq = 6;
        a.flightSubmitUs = a.nowUs - 300;
        b.flightSeq = 0;
        b.shownSeq = 6;
        c.flightSubmitUs = c.nowUs - 300;
        c.spanMaxUs = 0;
        std::snprintf(buf, sizeof(buf), "gate+far -> %s, none+gate -> %s, far+noRoom -> %s", whyName(why(a)), whyName(why(b)),
                      whyName(why(c)));
        caseDone("flight-order", why(a) == Why::Gate && why(b) == Why::NoFlight && why(c) == Why::Far, buf);
    }
    {
        // 半速＋補幀逐格（N＝20；A 格＝新影像第一次顯示、B 格＝顯示合成那一半）：
        //   1. N−1 的 B 格，N 還沒送出                     → NoFlight
        //   1'. 同一格，N 早就發布了、標記也清了            → NoFlight；N 發布了、N+1 已經在途 → Gate（最新的還沒顯示過）
        //   2. N−1 的 B 格，N 早到、正在途中               → Gate（不等：等到的話 N−1 的合成那一格會被跳過）
        //   3. N 的 A 格，N 晚到、正在途中                 → Wait
        //   4. N 發布之後，補幀那一批在做（不設在途標記）   → NoFlight
        const uint64_t N = 20;
        Input base = ready();
        base.streamDiv = 2;
        base.shownSeq = N - 1;
        base.latestSeq = N - 1;
        Input r1 = base, r1b = base, r1c = base, r2 = base, r3 = base, r4 = base;
        r1.shownCount = 1;
        r1.flightSeq = 0;
        r1b.shownCount = 1;
        r1b.latestSeq = N;
        r1b.flightSeq = 0;
        r1c.shownCount = 1;
        r1c.latestSeq = N;
        r1c.flightSeq = N + 1;
        r2.shownCount = 1;
        r2.flightSeq = N;
        r3.shownCount = 2;
        r3.flightSeq = N;
        r4.shownSeq = N;
        r4.shownCount = 1;
        r4.latestSeq = N;
        r4.flightSeq = 0;
        const bool ok = why(r1) == Why::NoFlight && why(r1b) == Why::NoFlight && why(r1c) == Why::Gate && why(r2) == Why::Gate &&
                        why(r3) == Why::Wait && why(r4) == Why::NoFlight;
        std::snprintf(buf, sizeof(buf), "B/not sent -> %s, B/published -> %s, B/next in flight -> %s, B/early -> %s, A/late -> %s, synth batch -> %s",
                      whyName(why(r1)), whyName(why(r1b)), whyName(why(r1c)), whyName(why(r2)), whyName(why(r3)), whyName(why(r4)));
        caseDone("flight-half-rate-synth-table", ok, buf);
    }
    {
        // 這一格要印 10 秒行：本來會等的不等，記 Busy。它排在最後：沒有在途、閘門不過、太遠、沒有空間的照原本的原因記
        Input a = ready(), b = ready(), c = ready(), e = ready(), g = ready();
        a.busy = b.busy = c.busy = e.busy = g.busy = true;
        b.flightSeq = 0;
        c.shownSeq = 6;
        e.flightSubmitUs = e.nowUs - 300;
        g.spanMaxUs = 0;
        const Decision da = decide(a);
        const bool ok = da.why == Why::Busy && da.deadlineUs == 0 && why(b) == Why::NoFlight && why(c) == Why::Gate &&
                        why(e) == Why::Far && why(g) == Why::NoRoom;
        std::snprintf(buf, sizeof(buf), "would wait -> %s, none -> %s, gate -> %s, far -> %s, noRoom -> %s", whyName(da.why),
                      whyName(why(b)), whyName(why(c)), whyName(why(e)), whyName(why(g)));
        caseDone("flight-busy-log-frame", ok, buf);
    }
    {
        // 「這一格會不會印 10 秒行」的預測（單位 ns；120 Hz、往前看一個週期 8.333 ms）。視窗起點 5 s、長度 10 s：
        // 到期的時刻是 15 s。差一個週期又 1 ns 還不算、剛好一個週期就算、過了更算；起點 0（還沒開始計時）當成會
        const uint64_t start = 5000000000ull, len = 10000000000ull, look = 8333333ull;
        const bool before = windowDue(start + len - look - 1, start, len, look);
        const bool edge = windowDue(start + len - look, start, len, look);
        const bool after = windowDue(start + len + 1000, start, len, look);
        const bool fresh = windowDue(start + 1000, start, len, look);
        const bool unset = windowDue(123456, 0, len, look);
        // 每 8.333 ms 一格，掃過到期的那一段：被跳過的格數是 1（實際印的那一格）或 2（加上它前面那一格）
        int flagged = 0;
        for (uint64_t now = start + len - 5 * look + 12345; now < start + len; now += look) {
            flagged += windowDue(now, start, len, look) ? 1 : 0;
        }
        flagged++;  // 第一個不小於到期時刻的那一格一定算（它就是印的那一格）
        std::snprintf(buf, sizeof(buf), "before=%d edge=%d after=%d fresh=%d unset=%d, frames skipped per window=%d", before ? 1 : 0,
                      edge ? 1 : 0, after ? 1 : 0, fresh ? 1 : 0, unset ? 1 : 0, flagged);
        caseDone("flight-window-due", !before && edge && after && !fresh && unset && flagged >= 1 && flagged <= 2, buf);
    }
    {
        // 開場：還沒有任何發布（latestSeq 0、顯示紀錄 (0, 0)），第一張在途 → F1 成立，但 0 格 < 1 → Gate。
        // 第一張發布並顯示一格之後，第二張在途 → 等
        Shown shown;
        Input in = ready();
        in.latestSeq = 0;
        in.flightSeq = 1;
        in.shownSeq = shown.seq;
        in.shownCount = shown.count;
        const Why first = why(in);
        shown.onLatched(1);
        in.latestSeq = 1;
        in.flightSeq = 2;
        in.shownSeq = shown.seq;
        in.shownCount = shown.count;
        const Why second = why(in);
        std::snprintf(buf, sizeof(buf), "first image in flight -> %s, second -> %s", whyName(first), whyName(second));
        caseDone("flight-first-frames", first == Why::Gate && second == Why::Wait, buf);
    }
    {
        // 顯示紀錄跟著 frame thread 走一段半速＋補幀（串流影像 30、31、32…；每格先判斷、再記這一格顯示了哪一張）：
        //   格 0：顯示 30（真的）                                      → (30, 1)
        //   格 1：31 早到在途 → Gate；顯示 30（合成）                   → (30, 2)
        //   格 2：31 晚到在途 → Wait；等到了，顯示 31                   → (31, 1)
        //   格 3：這一格沒有送出影像層（例如姿態還沒有）→ 不記          → (31, 1)
        //   格 4：32 在途 → Gate（31 帳面上只顯示了一格）；顯示 31       → (31, 2)
        //   格 5：xrEndFrame 前 32 的第一次發布插進來，改顯示上半（synthGuard）——這一格挑的仍是 31，照帳面記 → (31, 3)
        //   格 6：最新的是 32、還沒顯示過；33 已經在途 → Gate；顯示 32   → (32, 1)
        //   格 7：33 在途 → Gate（這一格要顯示 32 合成的那一半）；顯示 32 → (32, 2)
        //   格 8：33 在途 → Wait
        // 全速另驗 relatch：格內原本挑 40，xrEndFrame 前改送 41——只照最後送出的那一張記 → (41, 1)，下一格 42 在途 → Wait
        Shown s;
        bool ok = s.seq == 0 && s.count == 0;
        auto at = [&](uint64_t latest, uint64_t flight) {
            Input in = ready();
            in.streamDiv = 2;
            in.shownSeq = s.seq;
            in.shownCount = s.count;
            in.latestSeq = latest;
            in.flightSeq = flight;
            return decide(in).why;
        };
        s.onLatched(30);
        ok = ok && s.seq == 30 && s.count == 1;
        ok = ok && at(30, 31) == Why::Gate;
        s.onLatched(30);
        ok = ok && s.count == 2;
        ok = ok && at(30, 31) == Why::Wait;
        s.onLatched(31);
        ok = ok && s.seq == 31 && s.count == 1;
        // 格 3 不呼叫 onLatched
        ok = ok && at(31, 32) == Why::Gate;
        s.onLatched(31);
        ok = ok && s.count == 2;
        s.onLatched(31);  // 格 5（synthGuard）
        ok = ok && s.seq == 31 && s.count == 3;
        ok = ok && at(32, 33) == Why::Gate;
        s.onLatched(32);
        ok = ok && s.seq == 32 && s.count == 1;
        ok = ok && at(32, 33) == Why::Gate;
        s.onLatched(32);
        ok = ok && at(32, 33) == Why::Wait;
        Shown full;
        full.onLatched(40);
        full.onLatched(41);  // relatch 之後的那一張
        Input in = ready();
        in.shownSeq = full.seq;
        in.shownCount = full.count;
        in.latestSeq = 41;
        in.flightSeq = 42;
        ok = ok && full.seq == 41 && full.count == 1 && why(in) == Why::Wait;
        // 計數頂到上限不會繞回 0
        Shown top;
        top.seq = 9;
        top.count = 0xffffffffu;
        top.onLatched(9);
        ok = ok && top.count == 0xffffffffu;
        std::snprintf(buf, sizeof(buf), "half-rate walk ends at (%llu, %u), relatch -> (%llu, %u)", static_cast<unsigned long long>(s.seq),
                      s.count, static_cast<unsigned long long>(full.seq), full.count);
        caseDone("flight-shown-tracker", ok, buf);
    }
    {
        // g 的典型值：起始 1500；前 63 筆不動；第 64 筆更新——中位數 1700 → (3×1500＋1700＋2)/4＝1550。
        // 有 20 筆被合成器擋住（2700）不影響中位數。再一個全是 1700 的視窗 → 1588。
        // 全是離群值的視窗先夾在上下限：全 60000 → 用 5000 算；全 0 → 用 300 算。一直這樣餵，估計值停在上下限旁邊
        // （整數除法，差 1～2 µs），不會出界
        GpuTyp g;
        bool ok = g.typUs() == 1500;
        bool early = false;
        for (int i = 0; i < 63; i++) {
            early = g.push(i % 3 == 0 && i < 60 ? 2700u : 1700u) || early;
        }
        ok = ok && !early && g.typUs() == 1500;
        const bool updated = g.push(1700);
        const uint32_t afterOne = g.typUs();
        ok = ok && updated && afterOne == 1550;
        for (int i = 0; i < 64; i++) {
            g.push(1700);
        }
        const uint32_t afterTwo = g.typUs();
        ok = ok && afterTwo == 1588;
        GpuTyp hi, lo;
        for (int w = 0; w < 40; w++) {
            for (int i = 0; i < 64; i++) {
                hi.push(60000);
                lo.push(0);
            }
            if (w == 0) {
                ok = ok && hi.typUs() == 2375 && lo.typUs() == 1200;  // (4500＋5000＋2)/4、(4500＋300＋2)/4
            }
        }
        ok = ok && hi.typUs() >= 4998 && hi.typUs() <= 5000 && lo.typUs() >= 300 && lo.typUs() <= 302;
        std::snprintf(buf, sizeof(buf), "1500 -> %u -> %u; all 60000 us -> %u, all 0 -> %u", afterOne, afterTwo, hi.typUs(), lo.typUs());
        caseDone("flight-gpu-typ-estimate", ok, buf);
    }
    {
        // 10 秒視窗的計數：等到 2 次、逾時 1 次；沒等的四種原因各記各的（2、1、3、4）；Wait／Off／NoFlight 不計。
        // clear() 之後全部歸零。
        // 預留的空間滿了之後計數照記、時間不記（不配置記憶體）
        Window w;
        w.reserveWindow();
        w.noteWait(true, 120);
        w.noteWait(true, 300);
        w.noteWait(false, 415);
        const Why notes[] = {Why::Gate, Why::Gate, Why::Far,  Why::NoRoom, Why::NoRoom, Why::NoRoom,  Why::Busy,
                             Why::Busy, Why::Busy, Why::Busy, Why::Wait,   Why::Off,    Why::NoFlight};
        for (const Why n : notes) {
            w.note(n);
        }
        bool ok = w.waited == 3 && w.got == 2 && w.timeout == 1 && w.gate == 2 && w.tooFar == 1 && w.noRoom == 3 && w.busy == 4 &&
                  w.waitUs.size() == 3;
        w.clear();
        ok = ok && w.waited == 0 && w.got == 0 && w.timeout == 0 && w.gate == 0 && w.tooFar == 0 && w.noRoom == 0 && w.busy == 0 &&
             w.waitUs.empty();
        const size_t cap = w.waitUs.capacity();
        for (size_t i = 0; i < cap + 10; i++) {
            w.noteWait(true, 100);
        }
        ok = ok && cap >= 2048 && w.waited == cap + 10 && w.waitUs.size() == cap && w.waitUs.capacity() == cap;
        std::snprintf(buf, sizeof(buf), "reserved %u, counted %u waits, kept %u durations", static_cast<unsigned>(cap), w.waited,
                      static_cast<unsigned>(w.waitUs.size()));
        caseDone("flight-window-counts", ok, buf);
    }
    {
        // 10 秒行的那一段。空的視窗：計數都是 0、時間印 0/0。有資料：p50 是第 n/2 個（5 筆 → 第 3 小的）、max 是最大的。
        // 上限印兩位小數的毫秒（0.5 ms → 0.50；333 µs → 0.33；不作用 → 0.00）；防護暫停中印 paused
        Window empty;
        const std::string a = empty.tail(500, false, 1500);
        const char* const wantA =
            " | flight=0.50 ms waited=0 got=0 timeout=0 gate=0 far=0 noRoom=0 wait p50/max=0/0 us guard=ok busy=0 gTyp=1500 us"
            " quick=0 toLate p50/max=0/0 us missBy p50/p90/max=0/0/0 us n=0 unk=0 farBy p50/p90/max=0/0/0 us n=0 unk=0";
        Window w;
        w.reserveWindow();
        const uint32_t waits[5] = {410, 90, 250, 415, 130};
        for (int i = 0; i < 5; i++) {
            w.noteWait(i != 3, waits[i], i == 3 ? 70u : 0u);
        }
        for (int i = 0; i < 7; i++) {
            w.note(Why::Gate);
        }
        w.note(Why::Far);
        w.note(Why::Far);
        for (int i = 0; i < 3; i++) {
            w.note(Why::NoRoom);
        }
        for (int i = 0; i < 4; i++) {
            w.note(Why::Busy);
        }
        const std::string b = w.tail(333, true, 1588);
        const char* const wantB =
            " | flight=0.33 ms waited=5 got=4 timeout=1 gate=7 far=2 noRoom=3 wait p50/max=250/415 us guard=paused busy=4 gTyp=1588 us"
            " quick=2 toLate p50/max=70/70 us missBy p50/p90/max=0/0/0 us n=0 unk=0 farBy p50/p90/max=0/0/0 us n=0 unk=0";
        const std::string c = empty.tail(0, false, 0), e = empty.tail(1000, false, 0);
        const std::string down = empty.tail(334, false, 0), up = empty.tail(335, false, 0);  // 四捨五入到 0.01 ms
        const bool ok = a == wantA && b == wantB && c.compare(0, 18, " | flight=0.00 ms ") == 0 &&
                        e.compare(0, 18, " | flight=1.00 ms ") == 0 && down.compare(0, 18, " | flight=0.33 ms ") == 0 &&
                        up.compare(0, 18, " | flight=0.34 ms ") == 0;
        caseDone("flight-window-tail", ok, a == wantA ? (b == wantB ? b : "got [" + b + "] want [" + wantB + "]") : "got [" + a + "] want [" + wantA + "]");
    }
    {
        // 卡住：送出之後已經過了典型值＋16 ms（GPU 卡死判定之前的那 1 秒）→ Far，不再每格等滿。邊界不含等號
        Input a = ready(), b = ready();
        a.flightSubmitUs = a.nowUs - (1500 + kStuckUs);
        b.flightSubmitUs = b.nowUs - (1500 + kStuckUs + 1);
        std::snprintf(buf, sizeof(buf), "typ+16000 us -> %s, typ+16001 us -> %s", whyName(why(a)), whyName(why(b)));
        caseDone("flight-stuck", why(a) == Why::Wait && why(b) == Why::Far, buf);
    }
    {
        // xrWaitFrame 幾乎沒擋住就返回（上一格晚回）：這一格的截止時刻本身就晚 → NoRoom。擋了 150 µs 照常等；
        // 不知道擋了多久（預設值）不看這一條
        Input a = ready(), b = ready(), c = ready();
        a.wakeBlockedUs = 149;
        b.wakeBlockedUs = 150;
        std::snprintf(buf, sizeof(buf), "blocked 149 us -> %s, 150 us -> %s, unknown -> %s", whyName(why(a)), whyName(why(b)),
                      whyName(why(c)));
        caseDone("flight-wake-not-blocked", why(a) == Why::NoRoom && why(b) == Why::Wait && why(c) == Why::Wait, buf);
    }
    {
        // quick 只數等到的、而且等不到 150 µs 的；逾時的醒來晚多少記在 toLate（p50 是第 n/2 個）
        Window w;
        w.reserveWindow();
        w.noteWait(true, 149);
        w.noteWait(true, 150);
        w.noteWait(false, 100, 30);
        w.noteWait(false, 415, 90);
        w.noteWait(false, 415, 60);
        const std::string t = w.tail(500, false, 1500);
        const bool ok = w.quick == 1 && w.got == 2 && w.timeout == 3 &&
                        t.find(" quick=1 toLate p50/max=60/90 us") != std::string::npos;
        caseDone("flight-window-quick-late", ok, t);
    }
    {
        // g 的典型值取的是中位數：64 個不同的值（1000、1010、…、1630，順序打亂）的第 32 小是 1320 →
        // (3×1500＋1320＋2)/4＝1455。取平均（1315）、最小、最大都會得到別的數字
        GpuTyp g;
        for (uint32_t i = 0; i < 64; i++) {
            g.push(1000u + 10u * ((i * 37u) % 64u));
        }
        std::snprintf(buf, sizeof(buf), "1500 -> %u", g.typUs());
        caseDone("flight-gpu-typ-median", g.typUs() == 1455, buf);
    }
    {
        // 量測：在途的那一張後來發布了 → Done、記「發布 − 比較時刻」（早於比較時刻記 0）；還沒找到、記下不到 50 ms → Keep；
        // 滿 50 ms → Unknown
        Pending p;
        p.seq = 9;
        p.refUs = 10000;
        p.setUs = 9800;
        uint32_t a = 7, b = 7, c = 7;
        const Resolve ra = resolvePending(p, true, 10230, 10300, &a);
        const Resolve rb = resolvePending(p, true, 9990, 10300, &b);
        const Resolve rc = resolvePending(p, false, 0, 9800 + kPendingGiveUpUs - 1, &c);
        const Resolve rd = resolvePending(p, false, 0, 9800 + kPendingGiveUpUs, &c);
        const Resolve re = resolvePending(p, false, 0, 9000, &c);  // 時鐘倒退（兩條執行緒各自取的）當成剛記下
        std::snprintf(buf, sizeof(buf), "found +230 -> %u (%d), found early -> %u (%d), 49.999 ms -> %d, 50 ms -> %d, back -> %d", a,
                      static_cast<int>(ra), b, static_cast<int>(rb), static_cast<int>(rc), static_cast<int>(rd), static_cast<int>(re));
        caseDone("flight-pending-resolve", ra == Resolve::Done && a == 230 && rb == Resolve::Done && b == 0 && rc == Resolve::Keep &&
                                               c == 7 && rd == Resolve::Unknown && re == Resolve::Keep,
                 buf);
    }
    {
        // missBy／farBy 各自分開記；p50＝第 n/2 個、p90＝第 n*9/10 個（10 筆 → 第 6、10 小的）；unk 只計數
        Window w;
        w.reserveWindow();
        Pending miss, far2;
        far2.tooFar = true;
        for (uint32_t i = 0; i < 10; i++) {
            w.notePending(miss, Resolve::Done, 100u * (10u - i));  // 1000, 900, …, 100
        }
        w.notePending(miss, Resolve::Unknown, 0);
        w.notePending(far2, Resolve::Done, 40);
        w.notePending(far2, Resolve::Unknown, 0);
        w.notePending(far2, Resolve::Unknown, 0);
        const std::string t = w.tail(500, false, 1500);
        const bool ok = t.find(" missBy p50/p90/max=600/1000/1000 us n=10 unk=1 farBy p50/p90/max=40/40/40 us n=1 unk=2") !=
                        std::string::npos;
        w.clear();
        const std::string t2 = w.tail(500, false, 1500);
        const bool cleared = t2.find(" missBy p50/p90/max=0/0/0 us n=0 unk=0 farBy p50/p90/max=0/0/0 us n=0 unk=0") != std::string::npos;
        caseDone("flight-window-missby", ok && cleared, t);
    }
    return failures;
}

}  // namespace xrflight
