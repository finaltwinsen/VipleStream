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
//
// §VR-NETSEL-HOLD（2026-11-08 傍晚實測後）：
//   - 每次要暫停／接回家用 Wi-Fi 之前先通知 host（netSwitch），那幾秒的掉包不拿來降碼。接回家用 Wi-Fi 時單一無線電
//     要重新連線，熱點跟著掉約 20 秒；舊版 host 把它當塞車，一路降到 2 Mbps、90 秒才回到 60 Mbps。
//   - 開場暖身 20 → 40 秒：遊戲載入的卡頓一直延續到第 33 秒，B 的量測窗被它拖累（0.43% 裡大半是開場）。
//   - 只有不順時才測（使用者 11-08 決定）：暫停再接回家用 Wi-Fi 會讓頭盔的 Steam 登出、重新登入，登入後 steamwebhelper
//     幾乎每次崩潰（Steam 的 bug，SteamVR 介面重啟）。所以每場先只量 B（不切換、沒有副作用），缺額 < kTestThreshold
//     就保留家用 Wi-Fi、不記住（下次照樣先量 B）。選 C 的條件本來就要 B − C ≥ 0.5 個百分點，B 低於這個值不可能選 C。
//     B 不順時：這個地點 14 天內已經測過、選了 B，就不再切（不重複觸發介面重啟）；沒測過或要求重測才去量 C。
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

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <SDL.h>

#include <cstdio>
#include <functional>

class VrNetMode
{
public:
    // 設定頁的「VR 網路」（vrnetmode）：0＝自動測試並選擇（預設）、1＝一律保留家用 Wi-Fi、2＝一律只用轉接器
    enum Pref { kAuto = 0, kKeepHome = 1, kAdapterOnly = 2 };

    static constexpr uint32_t kWarmupMs = 40000;    // 開場先等串流穩定（遊戲載入的卡頓實測延續到 33 秒）
    static constexpr int kPauseHoldMs = 8000;       // §VR-NETSEL-HOLD：暫停家用 Wi-Fi 時 host 多久內不因掉包降碼
    static constexpr int kResumeHoldMs = 20000;     // 接回家用 Wi-Fi：重新連線約 12 秒，熱點掉包實測約 20 秒
    static constexpr uint32_t kWindowMs = 20000;    // 每個狀態量 20 秒
    static constexpr uint32_t kApplyTimeoutMs = 20000;
    static constexpr uint32_t kSettleMs = 6000;     // 切換後等連線重新穩定
    static constexpr int kRetestDays = 14;
    static constexpr double kTestThreshold = 0.005;  // B 的缺額到這麼多才值得暫停家用 Wi-Fi 去比 C

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
    /// §VR-NETSEL-HOLD：要切網路了（phase＝VIPLE_VR_NET_SWITCH_*，1＝暫停家用 Wi-Fi、2＝接回），請 host 在 holdMs 內別因掉包降碼
    std::function<void(int phase, int holdMs)> netSwitch;

    /// main loop 每圈呼叫；newFrames＝XR 顯示迴圈累計拿到新影像的格數
    void tick(uint32_t nowMs, uint64_t newFrames)
    {
        if (m_Dir.isEmpty() || m_State == State::Done || m_State == State::Disabled) {
            keepAlive(nowMs);
            return;
        }
        if (m_StartMs == 0) {
            m_StartMs = nowMs;
            // §REVIEW-1119：下次輪詢的初值是 0。SDL_GetTicks 超過約 24.8 天（高半段）時，
            // SDL_TICKS_PASSED(now, 0) 為假，暖身會一直進不去。
            m_NextPollMs = nowMs;
        }
        if (!SDL_TICKS_PASSED(nowMs, m_NextPollMs)) {
            return;
        }
        m_NextPollMs = nowMs + 250;
        // §REVIEW-1119：暖身這 40 秒還沒有人讀狀態檔（keepAlive 在還沒切到轉接器時也直接返回）。
        // 這是 SDL 主迴圈，每 250 ms 同步開檔會卡在遊戲載入最容易抖的那一段。
        if (m_State == State::Warmup && !SDL_TICKS_PASSED(nowMs, m_StartMs + kWarmupMs)) {
            return;
        }
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
            // §VR-NETBENCH（11-08 使用者決定）：遊戲中不再比較網路。「自動」只套用這個地點存下的選擇（之後由設定頁的
            // 「測試網路」量出來）；沒有存過就保留家用 Wi-Fi。舊的量測流程（MeasureB 之後）留著給測試功能沿用，這裡不再進入
            // §REVIEW-1111：存檔超過 14 天也照樣套用。原本過期就當成「沒存過」而留在家用 Wi-Fi——測出來比較好的選擇
            // 兩週後無聲地換回比較差的那個，而且遊戲中早就不會自己重測（重測只在設定頁的「測試網路」）。
            if (!m_Loc.isEmpty() && rememberedChoice(true) == QLatin1String("C")) {
                log(rememberedChoice().isEmpty() ? QStringLiteral("saved for this location: adapter only (older than the retest interval; run the network test again)")
                                                 : QStringLiteral("saved for this location: adapter only"));
                applyAdapterOnly(nowMs, true);
            }
            else {
                log(QStringLiteral("no saved choice for adapter-only here; keeping home Wi-Fi"));
            }
            m_State = State::Done;
            return;
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
            if (m_DeficitB < kTestThreshold && !m_Retest) {
                log("test: smooth with home Wi-Fi; not switching (adapter-only could not win by enough)");
                m_State = State::Done;
                return;
            }
            if (m_RememberedB && !m_Retest) {
                log("test: not smooth, but this location was already tested and home Wi-Fi won; not switching again");
                m_State = State::Done;
                return;
            }
            say(QCoreApplication::translate("VrNetMode", "Network test: trying adapter only for about 30 seconds (home Wi-Fi pauses, then comes back)"));
            m_Want = Want::AdapterOnly;
            notifySwitch(1, kPauseHoldMs);
            writeRequest(nowMs, true);
            // §REVIEW-1119：寫完就把下一次心跳推到 1 秒後。初值 0 會讓下一次輪詢立刻再寫一次。
            m_NextBeatMs = nowMs + 1000;
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
        // §REVIEW-1119：先讀狀態再寫心跳。交還的那一次若先寫 adapter-only，helper 可能在
        // 兩次寫入之間讀到，再暫停一次家用 Wi-Fi（熱點再掉一輪，而且這次沒先通知 host）。
        const Status st = readStatus();
        if (st.fresh && !st.apStation && SDL_TICKS_PASSED(nowMs, m_AppliedMs + 10000)) {
            log("the streaming adapter link is gone; reconnecting home Wi-Fi");
            say(QCoreApplication::translate("VrNetMode", "Streaming adapter lost - reconnecting the headset's home Wi-Fi"));
            backToNormal(nowMs);
            return;
        }
        // §REVIEW-1111：helper 會自己把家用 Wi-Fi 交還（連續暫停滿 6 小時、helper 重啟、暫停失敗）。這時轉接器還連著，
        // 上面那個判斷不會成立；原本這邊繼續每秒要求「只用轉接器」，helper 下一圈又把 Wi-Fi 暫停一次——兩次切換
        // 熱點都會掉十幾二十秒，卻沒有通知 host 暫停降碼（碼率會被一路砍到底、約 90 秒才回來）。
        // 看到 helper 已經交還：通知 host、這一場改回一般模式，不再要求暫停。
        if (st.fresh && st.applied == QLatin1String("adapter-only")) {
            m_SeenApplied = true;
        }
        else if (st.fresh && m_SeenApplied) {
            log("the helper handed home Wi-Fi back (time limit, restart or failure); staying on both links for this session");
            backToNormal(nowMs);
            return;
        }
        else if (st.fresh && !st.err.isEmpty() && SDL_TICKS_PASSED(nowMs, m_AppliedMs + 15000)) {
            log("the helper could not pause home Wi-Fi; staying on both links for this session");
            backToNormal(nowMs);
            return;
        }
        writeRequest(nowMs, true);
    }

    void applyAdapterOnly(uint32_t nowMs, bool announce)
    {
        m_Want = Want::AdapterOnly;
        m_AppliedMs = nowMs;
        m_SeenApplied = false;
        notifySwitch(1, kPauseHoldMs);
        writeRequest(nowMs, true);
        // §REVIEW-1119：這裡沒記下一次心跳。呼叫端隨即把狀態改成 Done，下一圈主迴圈會立刻再讀再寫一次。
        // 初值 0 在 SDL_GetTicks 過半時，心跳比較也會一直失敗，服務 5 秒後自己交還家用 Wi-Fi。
        m_NextBeatMs = nowMs + 1000;
        if (announce) {
            say(QCoreApplication::translate("VrNetMode", "Home Wi-Fi paused during VR for a smoother stream - it reconnects when you stop streaming"));
        }
    }

    void backToNormal(uint32_t nowMs)
    {
        if (m_Want == Want::AdapterOnly) {
            notifySwitch(2, kResumeHoldMs);
        }
        m_Want = Want::Normal;
        writeRequest(nowMs, false);
    }

    void notifySwitch(int phase, int holdMs)
    {
        if (netSwitch) {
            netSwitch(phase, holdMs);
        }
        log(QStringLiteral("asked the host not to cut the bitrate for %1 s while %2").arg(holdMs / 1000)
                .arg(phase == 1 ? QStringLiteral("home Wi-Fi pauses") : QStringLiteral("home Wi-Fi reconnects")));
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
        // §REVIEW-1111：這是每秒一次的心跳，而且在串流的主迴圈上（震動、輸入事件排在它後面）。
        // 目錄只建一次；QSaveFile 的 commit 會 fsync（快閃上常見 1～10 ms），心跳不需要落盤，
        // 改成寫暫存檔再 rename（一樣是整份換掉，helper 不會讀到寫一半的）。
        if (!m_DirReady) {
#if defined(Q_OS_UNIX) && defined(O_NOFOLLOW) && defined(O_DIRECTORY)
            // §REVIEW-1119：::chmod(路徑) 會跟著符號連結，把別的目錄改成 0700，失敗也照樣寫。
            // 不存在才 mkpath；open 對連結回 ELOOP 就不建立。fd 確認是自己的目錄再 fchmod。
            const QByteArray dirPath = QFile::encodeName(m_Dir);
            const int dflags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
            int dfd = ::open(dirPath.constData(), dflags);
            if (dfd < 0) {
                if (errno != ENOENT || !QDir().mkpath(m_Dir)) {
                    return;
                }
                dfd = ::open(dirPath.constData(), dflags);
                if (dfd < 0) {
                    return;
                }
            }
            struct stat dst;
            const bool ok = ::fstat(dfd, &dst) == 0 && S_ISDIR(dst.st_mode)
                && dst.st_uid == ::geteuid()
                && ((dst.st_mode & 0777) == 0700 || ::fchmod(dfd, 0700) == 0);
            ::close(dfd);
            if (!ok) {
                return;
            }
#else
            if (!QDir().mkpath(m_Dir)) {
                return;
            }
#endif
            m_DirReady = true;
        }
        QJsonObject o;
        o.insert(QStringLiteral("mode"), adapterOnly ? QStringLiteral("adapter-only") : QStringLiteral("normal"));
        o.insert(QStringLiteral("t"), static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
        const QString path = QDir(m_Dir).filePath(QStringLiteral("netmode-request.json"));
#ifdef Q_OS_UNIX
        // §REVIEW-1119：固定的 .tmp 若先被做成符號連結，Truncate 會跟著把目標截斷。
        // 寫入失敗仍 rename，磁碟滿時會把有效的請求檔蓋成空檔，helper 5 秒後交還
        // 家用 Wi-Fi，而且這次沒先通知 host 暫停降碼。每次用新名字、O_EXCL，不跟連結，寫完才換名。
        const QByteArray payload = QJsonDocument(o).toJson(QJsonDocument::Compact);
        int flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        static uint32_t seq = 0;
        const QByteArray tmpName = QFile::encodeName(
            path + QStringLiteral(".%1-%2.tmp").arg(++seq).arg(QDateTime::currentMSecsSinceEpoch()));
        const int fd = ::open(tmpName.constData(), flags, 0600);
        if (fd < 0) {
            return;
        }
        const char* wp = payload.constData();
        qint64 left = payload.size();
        bool ok = left > 0;
        while (left > 0) {
            const ssize_t n = ::write(fd, wp, static_cast<size_t>(left));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ok = false;
                break;
            }
            if (n == 0) {
                ok = false;
                break;
            }
            wp += n;
            left -= n;
        }
        if (::close(fd) != 0) {
            ok = false;
        }
        if (!ok || ::rename(tmpName.constData(), QFile::encodeName(path).constData()) != 0) {
            ::unlink(tmpName.constData());
        }
#else
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            f.commit();
        }
#endif
    }

    Status readStatus() const
    {
        Status s;
        const QString path = QDir(m_Dir).filePath(QStringLiteral("netmode-status.json"));
#ifdef Q_OS_UNIX
        // §REVIEW-1119：SDL 主迴圈上讀。FIFO 的 open 會堵住，超大檔的 readAll 會在主迴圈上配置。
        // 不跟符號連結；不是一般檔、或超過 8 KB（正常約一百多位元組）就當沒有狀態。
        int rflags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
        rflags |= O_NOFOLLOW;
#endif
        const int fd = ::open(QFile::encodeName(path).constData(), rflags);
        if (fd < 0) {
            return s;
        }
        struct stat stbuf;
        if (::fstat(fd, &stbuf) != 0 || !S_ISREG(stbuf.st_mode) || stbuf.st_size <= 0 || stbuf.st_size > 8192) {
            ::close(fd);
            return s;
        }
        QByteArray raw(static_cast<int>(stbuf.st_size), '\0');
        char* rp = raw.data();
        size_t got = 0;
        const size_t want = static_cast<size_t>(stbuf.st_size);
        while (got < want) {
            const ssize_t n = ::read(fd, rp + got, want - got);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (n == 0) {
                break;
            }
            got += static_cast<size_t>(n);
        }
        ::close(fd);
        // §REVIEW-1119：單次 read 遇 EINTR 或短讀會當成沒有服務。暖身結束只看這一次，
        // 整場會停用；keepAlive 也會沒看到交還、又寫一次 adapter-only。這裡讀滿為止。
        // 提前 EOF（比 fstat 短）仍放棄。helper 寫暫存檔再改名，不會截斷同一個 inode。
        if (got != want) {
            return s;
        }
#else
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return s;
        }
        const QByteArray raw = f.read(8193);
        if (raw.size() > 8192) {
            return s;
        }
#endif
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        const qint64 t = o.value(QStringLiteral("t")).toVariant().toLongLong();
        s.fresh = t > 0 && qAbs(QDateTime::currentSecsSinceEpoch() - t) <= 5;  // REVIEW-1111 (round 2): a timestamp in the future is not "fresh" either
        s.apStation = o.value(QStringLiteral("ap_station")).toBool();
        s.applied = o.value(QStringLiteral("applied")).toString();
        s.loc = o.value(QStringLiteral("loc")).toString();
        s.err = o.value(QStringLiteral("err")).toString();
        return s;
    }

    bool m_DirReady = false;  // writeRequest：目錄已經建好

    // §REVIEW-1118：狀態檔的 loc 若含 /、\ 或換行，QSettings 會把它切成子群組
    bool locKeySafe() const
    {
        return !m_Loc.contains(QLatin1Char('/')) && !m_Loc.contains(QLatin1Char('\\'))
            && !m_Loc.contains(QLatin1Char('\n')) && !m_Loc.contains(QLatin1Char('\r'));
    }

    QString rememberedChoice(bool ignoreAge = false) const
    {
        if (!locKeySafe()) {
            return QString();
        }
        QSettings settings;
        settings.beginGroup(QStringLiteral("vrnetsel"));
        const QString v = settings.value(m_Loc).toString();  // "C|2026-11-05"
        const QString choice = v.section('|', 0, 0);
        const QDate when = QDate::fromString(v.section('|', 1, 1), Qt::ISODate);
        if (!ignoreAge && (!when.isValid() || when.daysTo(QDate::currentDate()) > kRetestDays)) {
            return QString();  // 太久沒測
        }
        return choice;
    }

    void remember(const char* choice)
    {
        if (m_Loc.isEmpty() || !locKeySafe()) {
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
    bool m_SeenApplied = false;  // 這次要求「只用轉接器」之後，helper 回報過已套用
    uint32_t m_WinStartMs = 0;
    uint64_t m_WinStartFrames = 0;
    double m_DeficitB = 0.0;
    bool m_RememberedB = false;  // 這個地點 14 天內測過、選了 B
};
