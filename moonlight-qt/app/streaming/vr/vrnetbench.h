// §VR-NETBENCH（2026-11-09）：設定頁「測試網路」。
//
// 流程（GUI 端 VrNetBenchRunner）：網路狀態 B（家用 Wi-Fi＋轉接器）→ C（只用轉接器，串流期間暫停家用 Wi-Fi）→ B，
// 每個狀態跑解析度階梯（每眼 1152、1440、1728、2016，144 Hz），每一階開一個子行程
//   viplestream stream --display-target pcvr --vr-eye WxW --vr-hz 144 --vr-netbench <結果檔> --vr-netbench-secs N -- <主機> "VR Network Test"
// host 播預錄的桌球片段（不啟動 SteamVR），子行程跑滿 N 秒自己結束並把指標寫成 JSON。一階撐不住 144 Hz 就不再往上。
// 網路狀態由頭盔上的 viplestream-netmode.service 切換（見 vrnetmode.h），使用者不必操作。
//
// 這個檔案：
//   NetBench::Config／Collector：子行程端。XrContext 每 10 秒把一個視窗的指標交給 Collector；Session 時間到就 writeResult()。
//   StepResult：一階的指標（同 scripts/vr/netbench_summary.py）。
//   compareLadders()：綜合指標——幀率 > 延遲 > 畫質，逐項比、差距太小算平手；全部平手保留家用 Wi-Fi。
//
// 11-09 量測依據（temp/netbench/B1～B4、C1～C2，家用 5 GHz＋轉接器）：同狀態重跑的缺額 0.00～0.34%（「撐住」用 0.5%）；
// 2304 是頭盔端上限（網路 0 丟失仍跳格），所以階梯只到 2016；MTP p50 每階約差 2～3 ms。
#pragma once

#include <QDate>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace NetBench {

    constexpr int kHz = 144;
    constexpr int kSkipWindows = 1;        // 開場第一個 10 秒視窗（連線、緩衝建立）不算（同 netbench_summary.py 的 15 秒暖身）
    constexpr int kDefaultStepSec = 75;    // 第一個視窗不算，之後約六個 10 秒視窗
    constexpr double kHoldDeficitPct = 0.5;
    constexpr double kHoldRep10 = 5.0;
    constexpr int kHoldRun = 3;
    constexpr double kTieMtpMs = 2.0;      // 延遲 p95 差不到 2 ms 算平手
    constexpr double kTieMbpsFrac = 0.10;  // 碼率差不到一成算平手
    inline const QVector<int>& ladderEyes()
    {
        static const QVector<int> eyes{1152, 1440, 1728, 2016};
        return eyes;
    }
    inline QString appName() { return QStringLiteral("VR Network Test"); }

    // ── 子行程端 ──
    struct Config {
        QString resultPath;  // 空＝不是網路測試
        int seconds = kDefaultStepSec;
        bool active() const { return !resultPath.isEmpty(); }
    };
    inline Config& config()
    {
        static Config c;
        return c;
    }

    struct Window {
        double tSec = 0;          // 視窗結束時刻（距第一個視窗開始）
        uint32_t newFrames = 0;
        uint32_t repeats = 0;
        uint32_t maxRun = 0;
        double mtp50 = 0, mtp95 = 0;  // ms；沒有樣本＝0
        int rateDiv = 1;
        uint64_t lateNet = 0, lateHeadset = 0;  // 整場累計（§VR-RATE-CAUSE）
        uint64_t bytes = 0;       // 這個視窗收到的影像位元組
        double secs = 10;
    };

    class Collector {
    public:
        static Collector& get()
        {
            static Collector c;
            return c;
        }
        bool enabled() const { return config().active(); }
        // 影像收齊的位元組（decoder 執行緒）
        void addBytes(uint32_t n)
        {
            if (enabled()) {
                m_Bytes.fetch_add(n, std::memory_order_relaxed);
            }
        }
        // XrContext（frame thread）每 10 秒一次
        void pushWindow(Window w, uint64_t nowUs)
        {
            if (!enabled()) {
                return;
            }
            std::lock_guard<std::mutex> lk(m_Lock);
            if (m_StartUs == 0) {
                m_StartUs = nowUs > static_cast<uint64_t>(w.secs * 1e6) ? nowUs - static_cast<uint64_t>(w.secs * 1e6) : 1;
            }
            w.tSec = static_cast<double>(nowUs - m_StartUs) / 1e6;
            const uint64_t b = m_Bytes.load(std::memory_order_relaxed);
            w.bytes = b - m_LastBytes;
            m_LastBytes = b;
            if (w.rateDiv >= 2) {
                m_SawHalf = true;
            }
            m_Windows.push_back(w);
        }
        std::vector<Window> windows() const
        {
            std::lock_guard<std::mutex> lk(m_Lock);
            return m_Windows;
        }
        bool sawHalf() const
        {
            std::lock_guard<std::mutex> lk(m_Lock);
            return m_SawHalf;
        }

    private:
        mutable std::mutex m_Lock;
        std::vector<Window> m_Windows;
        std::atomic<uint64_t> m_Bytes{0};
        uint64_t m_LastBytes = 0;
        uint64_t m_StartUs = 0;
        bool m_SawHalf = false;
    };

    // ── 一階的指標 ──
    struct StepResult {
        int eye = 0;
        bool ran = false;         // 有結果檔、而且至少一個有效視窗
        QString error;
        int windows = 0;
        double deficitPct = 100;  // 1 − 新影像數／應有數
        double rep10 = 0;         // 每 10 秒重複格數平均
        int maxRun = 0;
        bool half = false;
        double mtp50 = 0, mtp95 = 0;
        double recvMbps = 0;
        uint64_t lateNet = 0, lateHeadset = 0;
        bool held = false;            // 撐住 144 Hz
        bool headsetLimited = false;  // 撐不住，而且主要是頭盔端

        QJsonObject toJson() const
        {
            QJsonObject o;
            o[QStringLiteral("eye")] = eye;
            o[QStringLiteral("ran")] = ran;
            o[QStringLiteral("error")] = error;
            o[QStringLiteral("windows")] = windows;
            o[QStringLiteral("deficitPct")] = deficitPct;
            o[QStringLiteral("rep10")] = rep10;
            o[QStringLiteral("maxRun")] = maxRun;
            o[QStringLiteral("half")] = half;
            o[QStringLiteral("mtp50")] = mtp50;
            o[QStringLiteral("mtp95")] = mtp95;
            o[QStringLiteral("recvMbps")] = recvMbps;
            o[QStringLiteral("lateNet")] = static_cast<double>(lateNet);
            o[QStringLiteral("lateHeadset")] = static_cast<double>(lateHeadset);
            o[QStringLiteral("held")] = held;
            o[QStringLiteral("headsetLimited")] = headsetLimited;
            return o;
        }
        static StepResult fromJson(const QJsonObject& o)
        {
            StepResult r;
            r.eye = o.value(QStringLiteral("eye")).toInt();
            r.ran = o.value(QStringLiteral("ran")).toBool();
            r.error = o.value(QStringLiteral("error")).toString();
            r.windows = o.value(QStringLiteral("windows")).toInt();
            r.deficitPct = o.value(QStringLiteral("deficitPct")).toDouble(100);
            r.rep10 = o.value(QStringLiteral("rep10")).toDouble();
            r.maxRun = o.value(QStringLiteral("maxRun")).toInt();
            r.half = o.value(QStringLiteral("half")).toBool();
            r.mtp50 = o.value(QStringLiteral("mtp50")).toDouble();
            r.mtp95 = o.value(QStringLiteral("mtp95")).toDouble();
            r.recvMbps = o.value(QStringLiteral("recvMbps")).toDouble();
            r.lateNet = static_cast<uint64_t>(o.value(QStringLiteral("lateNet")).toDouble());
            r.lateHeadset = static_cast<uint64_t>(o.value(QStringLiteral("lateHeadset")).toDouble());
            r.held = o.value(QStringLiteral("held")).toBool();
            r.headsetLimited = o.value(QStringLiteral("headsetLimited")).toBool();
            return r;
        }
    };

    inline double median(std::vector<double> v)
    {
        if (v.empty()) {
            return 0;
        }
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
    }

    // 視窗 → 一階的指標（規則同 netbench_summary.py：第一個視窗不算；中途掉到半速＝撐不住）
    inline StepResult summarize(int eye, const std::vector<Window>& all, bool sawHalf)
    {
        StepResult r;
        r.eye = eye;
        std::vector<Window> ws;
        for (size_t i = kSkipWindows; i < all.size(); ++i) {
            ws.push_back(all[i]);
        }
        r.windows = static_cast<int>(ws.size());
        r.half = sawHalf;
        if (ws.empty()) {
            r.error = QStringLiteral("no measurement windows");
            return r;
        }
        r.ran = true;
        double expected = 0, got = 0, reps = 0, bytes = 0, secs = 0;
        std::vector<double> p50s, p95s, mbps;
        for (const Window& w : ws) {
            expected += w.secs * kHz;
            got += w.newFrames;
            reps += w.repeats;
            r.maxRun = (std::max)(r.maxRun, static_cast<int>(w.maxRun));
            if (w.mtp50 > 0) {
                p50s.push_back(w.mtp50);
                p95s.push_back(w.mtp95);
            }
            bytes += static_cast<double>(w.bytes);
            secs += w.secs;
            if (w.secs > 0) {
                mbps.push_back(static_cast<double>(w.bytes) * 8.0 / w.secs / 1e6);
            }
        }
        r.deficitPct = expected > 0 ? (std::max)(0.0, 1.0 - got / expected) * 100.0 : 100.0;
        r.rep10 = reps / static_cast<double>(ws.size());
        r.mtp50 = median(p50s);
        r.mtp95 = median(p95s);
        r.recvMbps = median(mbps);
        // lateNet／lateHeadset 是整場累計：扣掉暖身最後一個視窗的值
        const uint64_t net0 = all[kSkipWindows - 1].lateNet, hs0 = all[kSkipWindows - 1].lateHeadset;
        r.lateNet = ws.back().lateNet >= net0 ? ws.back().lateNet - net0 : 0;
        r.lateHeadset = ws.back().lateHeadset >= hs0 ? ws.back().lateHeadset - hs0 : 0;
        r.held = !r.half && r.deficitPct <= kHoldDeficitPct && r.rep10 <= kHoldRep10 && r.maxRun <= kHoldRun;
        r.headsetLimited = !r.held && r.lateHeadset > 0 && r.lateHeadset >= 2 * r.lateNet;
        return r;
    }

    inline bool writeResult(const QString& path, const StepResult& r)
    {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        f.write(QJsonDocument(r.toJson()).toJson(QJsonDocument::Indented));
        return f.commit();
    }

    inline StepResult readResult(const QString& path, int eye)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            StepResult r;
            r.eye = eye;
            r.error = QStringLiteral("no result file");
            return r;
        }
        StepResult r = StepResult::fromJson(QJsonDocument::fromJson(f.readAll()).object());
        r.eye = eye;
        return r;
    }

    // ── 一個網路狀態的階梯 ──
    struct Ladder {
        QString state;  // "B"、"C"
        std::vector<StepResult> steps;
        // 撐住 144 Hz 的最高每眼解析度（0＝連 1152 都撐不住）
        int topHeld() const
        {
            int top = 0;
            for (const StepResult& s : steps) {
                if (s.held) {
                    top = (std::max)(top, s.eye);
                }
            }
            return top;
        }
        const StepResult* at(int eye) const
        {
            for (const StepResult& s : steps) {
                if (s.eye == eye) {
                    return &s;
                }
            }
            return nullptr;
        }
        bool headsetCapped() const
        {
            return !steps.empty() && !steps.back().held && steps.back().headsetLimited;
        }
    };

    enum class Verdict { KeepHome, AdapterOnly };
    struct Decision {
        Verdict verdict = Verdict::KeepHome;
        QString basis;  // 英文 log 用：哪一項分出勝負
        int level = 0;  // 比較延遲／碼率時用的每眼解析度
    };

    // C 對一次 B：+1＝C 較好、-1＝B 較好、0＝平手。依序看：撐住的最高解析度 → 共同最高那階的延遲 p95 → 碼率；
    // 都撐不住時比 1152 的缺額（差 0.5 個百分點以上而且少兩成才算贏）
    inline int compareOne(const Ladder& b, const Ladder& c, QString* basis, int* level)
    {
        const int tb = b.topHeld(), tc = c.topHeld();
        if (tb != tc) {
            *basis = QStringLiteral("highest resolution at 144 Hz");
            *level = (std::min)(tb, tc);
            return tc > tb ? 1 : -1;
        }
        if (tb == 0) {
            const StepResult* sb = b.at(ladderEyes().front());
            const StepResult* sc = c.at(ladderEyes().front());
            *basis = QStringLiteral("missed frames at the lowest resolution");
            *level = ladderEyes().front();
            if (sb == nullptr || sc == nullptr || !sb->ran || !sc->ran) {
                return sc != nullptr && sc->ran ? 1 : (sb != nullptr && sb->ran ? -1 : 0);
            }
            const double d = sb->deficitPct - sc->deficitPct;
            if (d >= 0.5 && sc->deficitPct <= sb->deficitPct * 0.8) {
                return 1;
            }
            if (-d >= 0.5 && sb->deficitPct <= sc->deficitPct * 0.8) {
                return -1;
            }
            return 0;
        }
        *level = tb;
        const StepResult* sb = b.at(tb);
        const StepResult* sc = c.at(tb);
        if (sb->mtp95 > 0 && sc->mtp95 > 0 && std::abs(sb->mtp95 - sc->mtp95) >= kTieMtpMs) {
            *basis = QStringLiteral("latency p95");
            return sc->mtp95 < sb->mtp95 ? 1 : -1;
        }
        const double hi = (std::max)(sb->recvMbps, sc->recvMbps);
        if (hi > 0 && std::abs(sb->recvMbps - sc->recvMbps) >= kTieMbpsFrac * hi) {
            *basis = QStringLiteral("stable bitrate");
            return sc->recvMbps > sb->recvMbps ? 1 : -1;
        }
        *basis = QStringLiteral("tie");
        return 0;
    }

    // 綜合判斷：C 要同時贏過前後兩次 B 才選 C（兩次 B 不一致＝量測雜訊比差距大，保留家用 Wi-Fi，頭盔才有網路）
    inline Decision compareLadders(const Ladder& b1, const Ladder& c, const Ladder* b2)
    {
        Decision d;
        QString basis1, basis2;
        int lv1 = 0, lv2 = 0;
        const int r1 = compareOne(b1, c, &basis1, &lv1);
        const int r2 = b2 != nullptr ? compareOne(*b2, c, &basis2, &lv2) : r1;
        d.level = lv1;
        if (r1 > 0 && r2 > 0) {
            d.verdict = Verdict::AdapterOnly;
            d.basis = basis1;
        }
        else {
            d.verdict = Verdict::KeepHome;
            d.basis = (r1 < 0 || r2 < 0) ? (r1 < 0 ? basis1 : basis2) : (r1 == 0 && r2 == 0 ? QStringLiteral("tie") : QStringLiteral("not consistent across the two home Wi-Fi runs"));
        }
        return d;
    }

}  // namespace NetBench
