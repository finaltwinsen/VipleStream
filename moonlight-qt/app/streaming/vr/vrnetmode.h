// §VR-NETSEL（2026-11-05）：Steam Frame 的網路狀態自動選擇。
//
// 頭盔只有一個無線電：家用 Wi-Fi（wlan0）和串流轉接器的熱點同時連著時要輪流用，熱點那條每隔一段就整段收不到
// （10-04 實測：家用 Wi-Fi 連著時每分鐘斷訊 9 次、46 Mbps；wlan0 交出管理後 0 次、171 Mbps）。這裡在 VR 串流中比較
// 「B：家用 Wi-Fi＋轉接器」與「C：只用轉接器（串流期間暫停家用 Wi-Fi）」，選比較順的那個並記住這個地點。
//
// 實際切換由頭盔上的小服務代辦（install-steamframe.sh 裝的 viplestream-netmode.service，在沙盒外）：
//   app 寫 $XDG_DATA_HOME/viplestream/netmode-request.json {"mode":"adapter-only"|"normal","t":<epoch 秒>}，每秒更新；
//   服務 5 秒沒看到更新、轉接器那條沒有連線、或 app 要 normal，就把 wlan0 交還 NetworkManager（自動接回家用 Wi-Fi）。
//   服務寫 netmode-status.json {"t","applied","ap_station","loc","wlan0","err"}。
// 沒有服務（舊的安裝腳本、非 Frame 平台）時什麼都不做。
//
// 評分：XR 顯示迴圈「拿到新影像」的格數（每秒）。兩個量測窗都在同一場、同樣設定下量，比的是相對差：
// 缺額＝1 − 新影像率／串流幀率。C 的缺額至少少 0.5 個百分點、而且至少少兩成才選 C（差不多時保留家用 Wi-Fi，頭盔才有網路）。
#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

#include <SDL.h>

#include <functional>

class VrNetMode
{
public:
    // 設定頁的「VR 網路」（vrnetmode）：0＝自動測試並選擇（預設）、1＝一律保留家用 Wi-Fi、2＝一律只用轉接器
    enum Pref { kAuto = 0, kKeepHome = 1, kAdapterOnly = 2 };

    static constexpr uint32_t kWarmupMs = 20000;    // 開場先等串流穩定
    static constexpr uint32_t kWindowMs = 20000;    // 每個狀態量 20 秒
    static constexpr uint32_t kApplyTimeoutMs = 20000;
    static constexpr uint32_t kSettleMs = 6000;     // 切換後等連線重新穩定
    static constexpr int kRetestDays = 14;

    VrNetMode(int pref, bool retest, int streamFps) : m_Pref(pref), m_Retest(retest), m_Fps(streamFps > 0 ? streamFps : 90)
    {
        QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
        if (dataHome.isEmpty()) {
            dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        }
        if (!dataHome.isEmpty()) {
            m_Dir = QDir(dataHome).filePath(QStringLiteral("viplestream"));
        }
    }

    ~VrNetMode()
    {
        // session 怎麼結束都一樣：只用轉接器的話交還家用 Wi-Fi（服務 5 秒沒看到心跳也會自己交還）
        if (m_Want == Want::AdapterOnly) {
            stop();
        }
    }
    VrNetMode(const VrNetMode&) = delete;
    VrNetMode& operator=(const VrNetMode&) = delete;

    std::function<void(const QString&)> toast;  // 頭盔裡的提示（main thread）

    /// main loop 每圈呼叫；newFrames＝XR 顯示迴圈累計拿到新影像的格數
    void tick(uint32_t nowMs, uint64_t newFrames)
    {
        if (m_Dir.isEmpty() || m_State == State::Done || m_State == State::Disabled) {
            keepAlive(nowMs);
            return;
        }
        if (m_StartMs == 0) {
            m_StartMs = nowMs;
        }
        if (!SDL_TICKS_PASSED(nowMs, m_NextPollMs)) {
            return;
        }
        m_NextPollMs = nowMs + 250;
        const Status st = readStatus();
        keepAlive(nowMs);

        switch (m_State) {
        case State::Warmup:
            if (!SDL_TICKS_PASSED(nowMs, m_StartMs + kWarmupMs)) {
                return;
            }
            if (!st.fresh) {
                log("no headset network helper (install with the new installer script); nothing to do");
                m_State = State::Disabled;
                return;
            }
            if (!st.apStation) {
                log("the streaming adapter is not connected; keeping home Wi-Fi");
                m_State = State::Disabled;
                return;
            }
            m_Loc = st.loc;
            if (m_Pref == kKeepHome) {
                log("setting: always keep home Wi-Fi");
                m_State = State::Disabled;
                return;
            }
            if (m_Pref == kAdapterOnly) {
                log("setting: adapter only");
                applyAdapterOnly(nowMs, true);
                m_State = State::Done;
                return;
            }
            if (!m_Retest && !m_Loc.isEmpty()) {
                const QString remembered = rememberedChoice();
                if (remembered == QLatin1String("C")) {
                    log(QStringLiteral("remembered for this location: adapter only"));
                    applyAdapterOnly(nowMs, true);
                    m_State = State::Done;
                    return;
                }
                if (remembered == QLatin1String("B")) {
                    log(QStringLiteral("remembered for this location: keep home Wi-Fi"));
                    m_State = State::Done;
                    return;
                }
            }
            say(QCoreApplication::translate("VrNetMode", "Network test: comparing the headset's connections (about 1 minute)"));
            log("test: measuring B (home Wi-Fi + adapter)");
            beginWindow(nowMs, newFrames);
            m_State = State::MeasureB;
            return;

        case State::MeasureB:
            if (!SDL_TICKS_PASSED(nowMs, m_WinStartMs + kWindowMs)) {
                return;
            }
            m_DeficitB = endWindow(nowMs, newFrames);
            log(QStringLiteral("test: B deficit %1%").arg(m_DeficitB * 100.0, 0, 'f', 2));
            m_Want = Want::AdapterOnly;
            writeRequest(nowMs, true);
            m_StateMs = nowMs;
            m_State = State::ApplyC;
            return;

        case State::ApplyC:
            if (st.applied == QLatin1String("adapter-only")) {
                log("test: home Wi-Fi paused; settling");
                m_StateMs = nowMs;
                m_State = State::SettleC;
            }
            else if (SDL_TICKS_PASSED(nowMs, m_StateMs + kApplyTimeoutMs)) {
                log(QStringLiteral("test: the helper could not pause home Wi-Fi (%1); keeping it").arg(st.err));
                backToNormal(nowMs);
                m_State = State::Done;
            }
            return;

        case State::SettleC:
            if (SDL_TICKS_PASSED(nowMs, m_StateMs + kSettleMs)) {
                log("test: measuring C (adapter only)");
                beginWindow(nowMs, newFrames);
                m_State = State::MeasureC;
            }
            return;

        case State::MeasureC: {
            if (!SDL_TICKS_PASSED(nowMs, m_WinStartMs + kWindowMs)) {
                return;
            }
            const double c = endWindow(nowMs, newFrames);
            const double b = m_DeficitB;
            const bool pickC = (b - c) >= 0.005 && c <= b * 0.8;
            log(QStringLiteral("test: C deficit %1% vs B %2% -> %3").arg(c * 100.0, 0, 'f', 2).arg(b * 100.0, 0, 'f', 2)
                    .arg(pickC ? "adapter only" : "keep home Wi-Fi"));
            remember(pickC ? "C" : "B");
            if (pickC) {
                say(QCoreApplication::translate("VrNetMode", "Network test: smoother with home Wi-Fi paused during VR (missed frames %1% -> %2%). "
                            "It reconnects when you stop streaming.")
                             .arg(b * 100.0, 0, 'f', 1).arg(c * 100.0, 0, 'f', 1));
            }
            else {
                backToNormal(nowMs);
                say(QCoreApplication::translate("VrNetMode", "Network test: keeping home Wi-Fi (missed frames %1% with it, %2% without)")
                             .arg(b * 100.0, 0, 'f', 1).arg(c * 100.0, 0, 'f', 1));
            }
            m_State = State::Done;
            return;
        }
        default:
            return;
        }
    }

    /// session 結束：交還家用 Wi-Fi
    void stop()
    {
        if (m_Want == Want::AdapterOnly) {
            log("session ended: giving home Wi-Fi back");
        }
        m_Want = Want::Normal;
        if (!m_Dir.isEmpty()) {
            writeRequest(SDL_GetTicks(), false);
        }
    }

    bool retestConsumed() const { return m_Retest && m_State != State::Warmup; }

private:
    enum class State { Warmup, MeasureB, ApplyC, SettleC, MeasureC, Done, Disabled };
    enum class Want { Normal, AdapterOnly };

    struct Status {
        bool fresh = false;
        bool apStation = false;
        QString applied;
        QString loc;
        QString err;
    };

    // 只用轉接器時每秒更新請求；轉接器那條斷了（服務那邊也會自己交還）就提示
    void keepAlive(uint32_t nowMs)
    {
        if (m_Want != Want::AdapterOnly || !SDL_TICKS_PASSED(nowMs, m_NextBeatMs)) {
            return;
        }
        m_NextBeatMs = nowMs + 1000;
        writeRequest(nowMs, true);
        const Status st = readStatus();
        if (st.fresh && !st.apStation && SDL_TICKS_PASSED(nowMs, m_AppliedMs + 10000)) {
            log("the streaming adapter link is gone; reconnecting home Wi-Fi");
            say(QCoreApplication::translate("VrNetMode", "Streaming adapter lost - reconnecting the headset's home Wi-Fi"));
            backToNormal(nowMs);
        }
    }

    void applyAdapterOnly(uint32_t nowMs, bool announce)
    {
        m_Want = Want::AdapterOnly;
        m_AppliedMs = nowMs;
        writeRequest(nowMs, true);
        if (announce) {
            say(QCoreApplication::translate("VrNetMode", "Home Wi-Fi paused during VR for a smoother stream - it reconnects when you stop streaming"));
        }
    }

    void backToNormal(uint32_t nowMs)
    {
        m_Want = Want::Normal;
        writeRequest(nowMs, false);
    }

    void beginWindow(uint32_t nowMs, uint64_t newFrames)
    {
        m_WinStartMs = nowMs;
        m_WinStartFrames = newFrames;
    }

    double endWindow(uint32_t nowMs, uint64_t newFrames) const
    {
        const double secs = (nowMs - m_WinStartMs) / 1000.0;
        if (secs <= 0.0) {
            return 0.0;
        }
        const double rate = static_cast<double>(newFrames - m_WinStartFrames) / secs;
        const double deficit = 1.0 - rate / m_Fps;
        return deficit < 0.0 ? 0.0 : deficit;
    }

    void writeRequest(uint32_t, bool adapterOnly)
    {
        if (!QDir().mkpath(m_Dir)) {
            return;
        }
        QJsonObject o;
        o.insert(QStringLiteral("mode"), adapterOnly ? QStringLiteral("adapter-only") : QStringLiteral("normal"));
        o.insert(QStringLiteral("t"), static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
        QSaveFile f(QDir(m_Dir).filePath(QStringLiteral("netmode-request.json")));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }

    Status readStatus() const
    {
        Status s;
        QFile f(QDir(m_Dir).filePath(QStringLiteral("netmode-status.json")));
        if (!f.open(QIODevice::ReadOnly)) {
            return s;
        }
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        const qint64 t = o.value(QStringLiteral("t")).toVariant().toLongLong();
        s.fresh = t > 0 && QDateTime::currentSecsSinceEpoch() - t <= 5;
        s.apStation = o.value(QStringLiteral("ap_station")).toBool();
        s.applied = o.value(QStringLiteral("applied")).toString();
        s.loc = o.value(QStringLiteral("loc")).toString();
        s.err = o.value(QStringLiteral("err")).toString();
        return s;
    }

    QString rememberedChoice() const
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("vrnetsel"));
        const QString v = settings.value(m_Loc).toString();  // "C|2026-11-05"
        const QString choice = v.section('|', 0, 0);
        const QDate when = QDate::fromString(v.section('|', 1, 1), Qt::ISODate);
        if (!when.isValid() || when.daysTo(QDate::currentDate()) > kRetestDays) {
            return QString();  // 太久沒測：重測
        }
        return choice;
    }

    void remember(const char* choice)
    {
        if (m_Loc.isEmpty()) {
            return;
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("vrnetsel"));
        settings.setValue(m_Loc, QStringLiteral("%1|%2").arg(QLatin1String(choice), QDate::currentDate().toString(Qt::ISODate)));
    }

    void say(const QString& text)
    {
        if (toast && !text.isEmpty()) {
            toast(text);
        }
    }

    static void log(const QString& msg)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-NETSEL] %s", qUtf8Printable(msg));
    }
    static void log(const char* msg)
    {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-NETSEL] %s", msg);
    }

    int m_Pref;
    bool m_Retest;
    double m_Fps;
    QString m_Dir;
    QString m_Loc;
    State m_State = State::Warmup;
    Want m_Want = Want::Normal;
    uint32_t m_StartMs = 0;
    uint32_t m_NextPollMs = 0;
    uint32_t m_NextBeatMs = 0;
    uint32_t m_StateMs = 0;
    uint32_t m_AppliedMs = 0;
    uint32_t m_WinStartMs = 0;
    uint64_t m_WinStartFrames = 0;
    double m_DeficitB = 0.0;
};
