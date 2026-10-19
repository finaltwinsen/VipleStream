// VipleStream 2.0 §VR-PLAYER — 見 playersession.h。

#include "playersession.h"

#include "backend/identitymanager.h"
#include "backend/webresolver.h"
#include "streaming/xr/xrcontext.h"
#include "streaming/xr/xrvideo.h"

#include <Limelight.h>
#include <SDL.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavutil/frame.h>
}

namespace player {

namespace {

// 螢幕大小的三個預設：距離（公尺）與水平張角（度）。中＝坐在電影院中間
struct ScreenPreset {
    const char* label;
    float distanceM;
    float fovDeg;
};
const ScreenPreset kScreens[] = {{"Screen S", 2.0f, 50.0f}, {"Screen M", 3.0f, 70.0f}, {"Screen L", 4.0f, 95.0f}};

// 控制列：播放中這麼久沒有互動就收起
constexpr uint64_t kPanelAutoHideNs = 5000000000ull;
// 控制列重畫的間隔（時間在走）
constexpr uint64_t kPanelRedrawNs = 250000000ull;
// 搖桿：推過這個值才算；按住時每隔多久再觸發一次
constexpr float kStickOn = 0.7f;
constexpr uint64_t kStickSeekRepeatNs = 500000000ull;
constexpr uint64_t kStickVolRepeatNs = 250000000ull;
constexpr int kDepthMax = 10;
constexpr float kDepthDistStep = 1.15f;   // 平面螢幕：每格距離乘以這個（張角不變，看起來一樣大、只是比較遠）
constexpr float kDepthEyeStepDeg = 0.1f;  // 180°／360°：每格每一眼往外轉幾度（10 格＝兩眼共 2°，背景要稍微往外張，再多會不舒服）
// 沒有聲音的片：落後超過這麼多就把時鐘對齊到目前這一張（網路卡住之後不要一路丟幀追上去）
constexpr double kVideoOnlyRebaseSec = 0.25;

// 格式按鈕依序切換的清單
const char* const kFormatCycle[] = {"flat", "flat-sbs", "flat-tb", "180-sbs", "180-mono", "360-mono", "360-tb", "360-sbs", "360eac-mono"};

QString formatLabel(const playerfmt::Format& f)
{
    QString s;
    switch (f.projection) {
    case playerfmt::Projection::Flat: s = QStringLiteral("2D"); break;
    case playerfmt::Projection::Equirect180: s = QStringLiteral("180°"); break;
    case playerfmt::Projection::Equirect360: s = QStringLiteral("360°"); break;
    case playerfmt::Projection::Eac360: s = QStringLiteral("360° EAC"); break;
    }
    if (f.stereo == playerfmt::Stereo::LeftRight) {
        s = f.projection == playerfmt::Projection::Flat ? QStringLiteral("3D SBS") : s + QStringLiteral(" SBS");
    }
    else if (f.stereo == playerfmt::Stereo::TopBottom) {
        s = f.projection == playerfmt::Projection::Flat ? QStringLiteral("3D TB") : s + QStringLiteral(" TB");
    }
    return s;
}

// 依片源幀率選顯示更新率：每張佔整數格最順。25／50 fps 要 100 Hz（頭盔沒有就由 runtime 選最接近的）
float preferredHzFor(double fps)
{
    if (fps > 0.0 && (std::fabs(fps - 25.0) < 0.1 || std::fabs(fps - 50.0) < 0.1)) {
        return 100.0f;
    }
    return 120.0f;
}

QString historyKey(const QString& source)
{
    return QStringLiteral("player/history/") +
           QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

}  // namespace

PlayerSession::PlayerSession(const Config& config, QObject* parent) : QObject(parent), m_Config(config) {}

PlayerSession::~PlayerSession()
{
    requestStop();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

void PlayerSession::start()
{
    m_Thread = std::thread([this] { threadMain(); });
}

void PlayerSession::requestStop()
{
    m_Stop.store(true, std::memory_order_release);
    m_Media.abort();
}

void PlayerSession::threadMain()
{
    QString error;
    const int rc = run(&error);
    QMetaObject::invokeMethod(this, [this, rc, error]() { emit finished(rc, error); }, Qt::QueuedConnection);
}

void PlayerSession::post(Cmd c, double v)
{
    std::lock_guard<std::mutex> lk(m_CmdMutex);
    m_Cmds.push_back({c, v});
}

int PlayerSession::run(QString* error)
{
    m_StartNs = steadyNowNs();

    // ── 1. 開影片（先開：要照幀率選顯示更新率）──
    MediaOptions mo;
    mo.url = m_Config.source.toStdString();
    mo.audioUrl = m_Config.audioSource.toStdString();
    mo.hwDecode = m_Config.hwDecode;
    mo.volume = m_Config.volume;
    mo.userAgent = m_Config.userAgent.toStdString();
    mo.httpHeaders = m_Config.httpHeaders.toStdString();
    // §VR-PLAYER（V3）：host 分享的影片要用配對憑證連（server 只讓配對過的 client 進來）。FFmpeg 的 TLS 只吃檔案，
    // 寫到 app 自己的快取資料夾（只有自己能讀），結束時刪掉
    QString tlsCert, tlsKey;
    if (m_Config.source.contains(QLatin1String("/viple/media/file")) && m_Config.source.startsWith(QLatin1String("https://"))) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(dir);
        tlsCert = dir + QStringLiteral("/player-client-cert.pem");
        tlsKey = dir + QStringLiteral("/player-client-key.pem");
        auto writePrivate = [](const QString& path, const QByteArray& data) {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                return false;
            }
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            return f.write(data) == data.size();
        };
        if (!writePrivate(tlsCert, IdentityManager::get()->getCertificate()) || !writePrivate(tlsKey, IdentityManager::get()->getPrivateKey())) {
            QFile::remove(tlsCert);
            QFile::remove(tlsKey);
            *error = QStringLiteral("could not prepare the client certificate");
            return 3;
        }
        mo.tlsCertFile = tlsCert.toStdString();
        mo.tlsKeyFile = tlsKey.toStdString();
    }
    struct TlsCleanup {
        QString cert, key;
        ~TlsCleanup()
        {
            if (!cert.isEmpty()) {
                QFile::remove(cert);
                QFile::remove(key);
            }
        }
    } tlsCleanup{tlsCert, tlsKey};
    // 從上次停下的地方繼續（沒指定起點、而且上次沒播完）
    QSettings settings;
    const QString hkey = historyKey(m_Config.source);
    if (m_Config.startSec <= 0.0) {
        const double last = settings.value(hkey + QStringLiteral("/position"), 0.0).toDouble();
        const double dur = settings.value(hkey + QStringLiteral("/duration"), 0.0).toDouble();
        if (last > 10.0 && (dur <= 0.0 || last < dur - 15.0)) {
            mo.startSec = last;
        }
    }
    else {
        mo.startSec = m_Config.startSec;
    }
    // §VR-PLAYER（V4）：YouTube 等網站的頁面網址先經 yt-dlp 換成影像／音訊網址（頭盔裡這段時間還沒有畫面）
    QString webTitle;
    const bool web = WebResolver::needsResolve(m_Config.source);
    // YouTube 偶爾給出一開就 403 的影像網址（同一支片重新解析一次就好）：網站來源開失敗時重新解析、再開一次
    for (int attempt = 0;; attempt++) {
        if (web) {
            const uint64_t r0 = steadyNowNs();
            const WebResolver::Result wr = WebResolver::resolve(m_Config.source, 45000);
            if (!wr.ok) {
                *error = wr.error;
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] web resolve failed: %s", qPrintable(wr.error));
                return 3;
            }
            mo.url = wr.videoUrl.toStdString();
            mo.audioUrl = wr.audioUrl.toStdString();
            if (!wr.userAgent.isEmpty()) {
                mo.userAgent = wr.userAgent.toStdString();
            }
            if (!wr.headers.isEmpty()) {
                mo.httpHeaders = wr.headers.toStdString();
            }
            webTitle = wr.title;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] web resolved in %llu ms: %s%s%s",
                        static_cast<unsigned long long>((steadyNowNs() - r0) / 1000000ull), wr.audioUrl.isEmpty() ? "single stream" : "video+audio",
                        wr.isLive ? " (live)" : "", wr.title.isEmpty() ? "" : " - title known");
        }
        std::string err;
        if (m_Media.open(mo, &err)) {
            break;
        }
        m_Media.close();
        if (web && attempt == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] open failed (%s): resolving the page again", err.c_str());
            continue;
        }
        *error = QString::fromStdString(err);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] open failed: %s", err.c_str());
        return 3;
    }
    // XR 還要一兩秒才起來：先停住，起來之後才開始播（不然開頭那幾秒的幀全都晚了、聲音先跑）
    m_Media.setPaused(true);
    const MediaInfo& info = m_Media.info();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] opened in %llu ms: %s start=%.1f s",
                static_cast<unsigned long long>((steadyNowNs() - m_StartNs) / 1000000ull), m_Media.describe().c_str(),
                mo.startSec);

    // 遠近：這支片上次調的
    m_Depth = std::clamp(settings.value(hkey + QStringLiteral("/depth"), 0).toInt(), -kDepthMax, kDepthMax);

    // 畫面格式：使用者指定 > 上次在這支片選的 > 中繼資料 > 檔名 > 平面
    QString fmtText = m_Config.format;
    if (fmtText.isEmpty() || fmtText == QLatin1String("auto")) {
        fmtText = settings.value(hkey + QStringLiteral("/format"), QStringLiteral("auto")).toString();
    }
    // 檔名標記：本機檔用檔名、網址用路徑；網站的影片（V4）用片名（YouTube 的 DASH 串流不一定帶 360° 中繼資料，
    // 片名裡常有「360°」「VR180」）
    // （片名裡的 / 與 . 換成空白：判斷規則會把它們當成路徑與副檔名）
    // host 的分享檔案（V3）：真正的檔名在網址的 path 參數裡
    const QUrl srcUrl(m_Config.source);
    const QString sharedName = srcUrl.path() == QLatin1String("/viple/media/file")
                                   ? QFileInfo(QUrlQuery(srcUrl).queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded)).fileName()
                                   : QString();
    const QString fileName = !webTitle.isEmpty() ? QString(webTitle).replace(QLatin1Char('/'), QLatin1Char(' ')).replace(QLatin1Char('.'), QLatin1Char(' '))
                             : !sharedName.isEmpty() ? sharedName
                             : QUrl(m_Config.source).isLocalFile() || !m_Config.source.contains(QLatin1String("://"))
                                 ? QFileInfo(m_Config.source).fileName()
                                 : QUrl(m_Config.source).path();
    m_Format = playerfmt::decide(fmtText.toStdString(), info.hints, fileName.toStdString(), &m_FormatSource);
    // YouTube 的 360° 影片都是等角立方體（EAC），容器裡的投影標記 FFmpeg 不認得（mp4 是 ytmp、webm 是 mesh），只能從來源判斷
    const QString host = QUrl(m_Config.source).host().toLower();
    if (m_Format.projection == playerfmt::Projection::Equirect360 && m_FormatSource != playerfmt::Source::User &&
        (host.endsWith(QLatin1String("youtube.com")) || host == QLatin1String("youtu.be"))) {
        m_Format.projection = playerfmt::Projection::Eac360;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] format %s (from %s)", playerfmt::name(m_Format).c_str(),
                playerfmt::sourceName(m_FormatSource));
    m_PanelState.title = !m_Config.title.isEmpty() ? m_Config.title
                         : !webTitle.isEmpty()     ? webTitle
                         : !info.title.empty()     ? QString::fromStdString(info.title)
                                                   : QFileInfo(fileName).completeBaseName();
    m_PanelState.durationSec = info.durationSec;
    m_PanelState.seekable = info.seekable;

    // ── 2. XR ──
    const int preset = std::clamp(m_Config.screenPreset, 0, 2);
    if (!m_Config.noXr) {
        XrContext::Options xo;
        xo.player = true;
        xo.quadDistanceM = kScreens[preset].distanceM;
        xo.quadFovDeg = kScreens[preset].fovDeg;
        xo.preferredRefreshHz = preferredHzFor(info.fps);
        xo.testPointer = m_Config.testPointer;
        xo.dumpFramePath = m_Config.dumpFramePath;
        // 手把（使用者 2026-10-19 的配戴回饋：控制列收起來後手把完全沒反應）：扳機（沒指在控制列上）＝開關控制列、
        // A＝播放／暫停、B＝離開回選片頁、握把＝置中；搖桿左右＝後退／前進 10 秒、上下＝音量（onStick）
        xo.inputSink.playerButton = [this](XrInputSink::PlayerButton b) {
            m_LastInteractNs.store(steadyNowNs(), std::memory_order_relaxed);
            switch (b) {
            case XrInputSink::PlayerButton::Trigger:
                post(Cmd::TogglePanel);
                break;
            case XrInputSink::PlayerButton::A:
                post(Cmd::TogglePause);
                break;
            case XrInputSink::PlayerButton::B:
                post(Cmd::Exit);
                break;
            case XrInputSink::PlayerButton::Grip:
                post(Cmd::Recenter);
                break;
            }
        };
        xo.inputSink.menu = [this]() { post(Cmd::TogglePanel); };
        xo.inputSink.stick = [this](float x, float y) { onStick(x, y); };
        xo.inputSink.panel = [this](float u, float v) {
            float frac = -1.0f;
            const int a = u < 0.0f ? PlayerPanel::None : PlayerPanel::hitTest(u, v, &frac);
            m_Hover.store(a == PlayerPanel::Timeline ? PlayerPanel::None : a, std::memory_order_relaxed);
            m_HoverTimeline.store(a == PlayerPanel::Timeline ? frac : -1.0f, std::memory_order_relaxed);
            if (u >= 0.0f) {
                m_LastInteractNs.store(steadyNowNs(), std::memory_order_relaxed);
            }
        };
        xo.inputSink.panelClick = [this](float u, float v) { onPanelClick(u, v); };
        if (!m_Config.xrRuntimeJson.isEmpty()) {
            qputenv("XR_RUNTIME_JSON", QFile::encodeName(m_Config.xrRuntimeJson));
        }
        m_Xr = new XrContext(xo);
        QString xerr;
        if (!m_Xr->bringUp(15000, &xerr)) {
            *error = QStringLiteral("XR: ") + xerr;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] XR bring-up failed: %s", qPrintable(xerr));
            delete m_Xr;
            m_Xr = nullptr;
            m_Media.close();
            return 2;
        }
        if (m_Xr->video() == nullptr) {
            *error = QStringLiteral("XR video path unavailable");
            m_Xr->shutdown();
            delete m_Xr;
            m_Xr = nullptr;
            m_Media.close();
            return 2;
        }
    }
    applyFormat();
    applyScreen();  // 套用這支片記住的遠近（也設好控制列上的標籤）
    m_Media.setPaused(false);
    m_PanelShownNs = steadyNowNs();
    m_LastInteractNs.store(m_PanelShownNs);
    updatePanel(true);

    // ── 3. 呈現迴圈 ──
    playersync::Lock lock;
    int lastSerial = -1;
    bool previewPending = false;
    int rc = 0;
    m_WinStartNs = steadyNowNs();
    // --no-xr：假的 120 Hz 格點（挑影像在顯示前 27 ms）
    playersync::Grid fake;
    fake.periodNs = 8333333;
    fake.wakeNs = steadyNowNs();
    fake.displayNs = fake.wakeNs + 27000000;
    while (!m_Stop.load(std::memory_order_acquire)) {
        const uint64_t now = steadyNowNs();
        const double elapsed = static_cast<double>(now - m_StartNs) / 1e9;
        if (!m_Config.testScript.isEmpty()) {
            runTestScript(elapsed);
        }
        if (m_Config.durationSec > 0 && elapsed > m_Config.durationSec) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] dev: --duration %d s reached", m_Config.durationSec);
            break;
        }
        handleCommands();
        if (m_Stop.load()) {
            break;
        }
        if (m_Xr != nullptr && (!m_Xr->isRunning() || m_Xr->isLost())) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] XR session ended (%s)", m_Xr->isLost() ? "lost" : "exit");
            rc = m_Xr->isLost() ? 4 : 0;
            break;
        }
        const std::string fatal = m_Media.fatalError();
        if (!fatal.empty()) {
            *error = QString::fromStdString(fatal);
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] %s", fatal.c_str());
            rc = 3;
            break;
        }
        if (!m_Ended && m_Media.finished()) {
            m_Ended = true;
            m_PanelState.ended = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] reached the end (%.1f s)", m_Media.position());
            if (m_Xr == nullptr) {
                break;
            }
            m_PanelVisible = true;  // 播完：顯示控制列（可以倒回去看或離開）
            m_PanelShownNs = now;
        }
        if (now - m_WinStartNs >= 10000000000ull) {
            logWindow(now, false);
        }
        updatePanel(false);

        // 顯示格點
        playersync::Grid g;
        if (m_Xr != nullptr) {
            const XrContext::DisplayTiming dt = m_Xr->displayTiming();
            g.wakeNs = dt.wakeNs;
            g.displayNs = dt.displayNs;
            g.periodNs = dt.periodNs;
        }
        else {
            while (fake.wakeNs + fake.periodNs <= now) {
                fake.wakeNs += fake.periodNs;
                fake.displayNs += fake.periodNs;
            }
            g = fake;
        }
        if (!g.valid()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        double pts = 0.0, npts = 0.0;
        int serial = 0;
        bool haveNext = false;
        if (!m_Media.peekVideo(&pts, &serial, &haveNext, &npts)) {
            m_Media.waitVideo(4);
            continue;
        }
        if (serial != lastSerial) {
            lastSerial = serial;
            lock.reset();
            m_LastSlotAbs = INT64_MIN;
            previewPending = true;
        }
        if (m_Media.paused()) {
            // 暫停中 seek：把新位置的第一張顯示出來，之後停住
            if (previewPending) {
                previewPending = false;
                AVFrame* f = m_Media.takeVideo();
                if (m_Xr != nullptr && f != nullptr) {
                    m_Xr->video()->submit(f);
                }
                av_frame_free(&f);
                m_Previews++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        uint64_t td = 0, ntd = 0;
        if (!m_Media.mediaToSteady(pts, &td)) {
            // 時鐘還沒開始（開頭、seek 之後等聲音）或斷音緩衝中
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (haveNext && !m_Media.mediaToSteady(npts, &ntd)) {
            haveNext = false;
        }
        if (!m_Media.info().hasAudio && td + static_cast<uint64_t>(kVideoOnlyRebaseSec * 1e9) < now) {
            // 沒有聲音的片落後太多（網路卡住過）：時鐘對齊到這一張，不要一路丟幀
            m_Media.rebaseVideoClock(pts, now + 3 * g.periodNs + g.aheadNs());
            lock.reset();
            continue;
        }
        uint64_t waitUntil = 0;
        const playersync::Action a =
            playersync::decide(g, td, haveNext, ntd, now, &waitUntil, playersync::kRenderBudgetNs, &lock, pts, npts);
        if (a == playersync::Action::Wait) {
            const uint64_t w = waitUntil > now ? waitUntil - now : 0;
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(w, 4000000ull)));
            continue;
        }
        if (a == playersync::Action::Drop) {
            m_Media.dropVideo();
            m_Dropped++;
            continue;
        }
        const playersync::Plan p = playersync::plan(g, td, now, playersync::kRenderBudgetNs, &lock, pts);
        AVFrame* f = m_Media.takeVideo();
        if (f == nullptr) {
            continue;
        }
        if (m_Xr != nullptr) {
            m_Xr->video()->submit(f);
        }
        av_frame_free(&f);
        previewPending = false;
        if (m_Traced < m_Config.traceFrames) {
            // dev：這一張的影片時間、該被看見的時刻（相對現在）、排到的格（相對最近一次醒來的格點）、
            // td 和那一格差多少、醒來的時刻（相對上一張的醒來時刻）
            static uint64_t s_LastWake = 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PLAYER] trace pts=%.4f td-now=%.2f ms slot=%lld td-slot=%.2f ms wake+%.3f ms lead=%.2f ms%s",
                        pts, static_cast<double>(static_cast<int64_t>(td - now)) / 1e6, static_cast<long long>(p.slot),
                        static_cast<double>(static_cast<int64_t>(td - p.slotDisplayNs)) / 1e6,
                        s_LastWake != 0 ? static_cast<double>(static_cast<int64_t>(g.wakeNs - s_LastWake)) / 1e6 : 0.0,
                        static_cast<double>(static_cast<int64_t>(p.latchNs - now)) / 1e6, p.late ? " LATE" : "");
            s_LastWake = g.wakeNs;
            m_Traced++;
        }
        lock.update(p.slotDisplayNs, pts);
        m_Submitted++;
        if (p.late) {
            m_Late++;
        }
        m_LeadMinMs = std::min(m_LeadMinMs, static_cast<float>(static_cast<double>(static_cast<int64_t>(p.latchNs - now)) / 1e6));
        // 相鄰兩張相差幾格（節拍是否均勻）。直接用兩張的格點時刻相減再除以週期：格點本身帶著醒來時刻的微小抖動，
        // 拿絕對時間除以週期再取整數，相位剛好在半格附近時會在相鄰兩格之間翻（2026-10-19 頭盔：60 fps 實際全是 2 格，
        // 舊算法卻印出 1／3 交錯）
        const int64_t slotNs = static_cast<int64_t>(p.slotDisplayNs);
        if (m_LastSlotAbs != INT64_MIN) {
            const int64_t d = static_cast<int64_t>(std::floor(static_cast<double>(slotNs - m_LastSlotAbs) / static_cast<double>(g.periodNs) + 0.5));
            m_SlotHist[d <= 0 ? 0 : (d >= 7 ? 7 : d)]++;
        }
        m_LastSlotAbs = slotNs;
    }

    // ── 4. 收尾 ──
    logWindow(steadyNowNs(), true);
    const double pos = m_Media.position();
    settings.setValue(hkey + QStringLiteral("/source"), m_Config.source);
    settings.setValue(hkey + QStringLiteral("/title"), m_PanelState.title);
    settings.setValue(hkey + QStringLiteral("/position"), m_Ended ? 0.0 : pos);
    settings.setValue(hkey + QStringLiteral("/duration"), info.durationSec);
    settings.setValue(hkey + QStringLiteral("/lastPlayed"), QDateTime::currentSecsSinceEpoch());
    if (m_FormatSource == playerfmt::Source::User) {
        settings.setValue(hkey + QStringLiteral("/format"), QString::fromStdString(playerfmt::name(m_Format)));
    }
    if (m_Depth != 0) {
        settings.setValue(hkey + QStringLiteral("/depth"), m_Depth);
    }
    else {
        settings.remove(hkey + QStringLiteral("/depth"));
    }
    settings.sync();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] stopped at %.1f s (rc=%d)", pos, rc);
    m_Media.close();
    if (m_Xr != nullptr) {
        m_Xr->shutdown();
        delete m_Xr;
        m_Xr = nullptr;
    }
    return rc;
}

void PlayerSession::applyFormat()
{
    if (m_Xr != nullptr) {
        m_Xr->setPlayerLayout(static_cast<int>(m_Format.projection), static_cast<int>(m_Format.stereo), m_Media.info().frameAspect);
    }
    m_PanelState.formatLabel = formatLabel(m_Format);
}

void PlayerSession::applyScreen()
{
    const int i = std::clamp(m_Config.screenPreset, 0, 2);
    const float dist = kScreens[i].distanceM * std::pow(kDepthDistStep, static_cast<float>(m_Depth));
    const float eyeOffDeg = kDepthEyeStepDeg * static_cast<float>(m_Depth);
    if (m_Xr != nullptr) {
        m_Xr->setPlayerScreen(dist, kScreens[i].fovDeg);
        m_Xr->setPlayerEyeOffset(eyeOffDeg * 3.14159265f / 180.0f);
    }
    m_PanelState.screenLabel = QString::fromLatin1(kScreens[i].label);
    if (m_Depth != 0) {
        m_PanelState.screenLabel += QStringLiteral(" %1%2").arg(m_Depth > 0 ? QStringLiteral("+") : QString()).arg(m_Depth);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] screen %s depth %+d (%.2f m, %.0f deg, eye %+.1f deg)", kScreens[i].label,
                m_Depth, static_cast<double>(dist), static_cast<double>(kScreens[i].fovDeg), static_cast<double>(eyeOffDeg));
}

void PlayerSession::handleCommands()
{
    std::deque<Command> cmds;
    {
        std::lock_guard<std::mutex> lk(m_CmdMutex);
        cmds.swap(m_Cmds);
    }
    for (const Command& c : cmds) {
        const uint64_t now = steadyNowNs();
        switch (c.cmd) {
        case Cmd::TogglePause:
            m_Media.setPaused(!m_Media.paused());
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] %s at %.1f s", m_Media.paused() ? "pause" : "play",
                        m_Media.position());
            break;
        case Cmd::Seek:
        case Cmd::SeekRel: {
            const double target = c.cmd == Cmd::Seek ? c.value : m_Media.position() + c.value;
            m_Media.seek(target);
            m_Ended = false;
            m_PanelState.ended = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] seek to %.1f s", std::max(0.0, target));
            break;
        }
        case Cmd::Volume:
        case Cmd::VolumeRel: {
            const float v = c.cmd == Cmd::Volume ? static_cast<float>(c.value) : m_Media.volume() + static_cast<float>(c.value);
            m_Media.setVolume(std::round(std::clamp(v, 0.0f, 2.0f) * 20.0f) / 20.0f);
            break;
        }
        case Cmd::Format: {
            // value < 0：照清單換下一個；否則是 kFormatCycle 的索引
            int idx = 0;
            const std::string cur = playerfmt::name(m_Format);
            const int n = static_cast<int>(sizeof(kFormatCycle) / sizeof(kFormatCycle[0]));
            if (c.value < 0.0) {
                for (int i = 0; i < n; i++) {
                    if (cur == kFormatCycle[i]) {
                        idx = (i + 1) % n;
                    }
                }
            }
            else {
                idx = std::clamp(static_cast<int>(c.value), 0, n - 1);
            }
            playerfmt::parse(kFormatCycle[idx], &m_Format);
            m_FormatSource = playerfmt::Source::User;
            applyFormat();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] format -> %s (user)", playerfmt::name(m_Format).c_str());
            break;
        }
        case Cmd::DepthRel: {
            const int d = std::clamp(m_Depth + static_cast<int>(c.value), -kDepthMax, kDepthMax);
            if (d != m_Depth) {
                m_Depth = d;
                applyScreen();
            }
            m_PanelVisible = true;  // 控制列上的「Screen」顯示目前的格數
            break;
        }
        case Cmd::Screen:
            m_Config.screenPreset = c.value < 0.0 ? (m_Config.screenPreset + 1) % 3 : std::clamp(static_cast<int>(c.value), 0, 2);
            applyScreen();
            break;
        case Cmd::Recenter:
            if (m_Xr != nullptr) {
                m_Xr->requestRecenter();
            }
            break;
        case Cmd::Exit:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] exit requested");
            m_Stop.store(true);
            break;
        case Cmd::TogglePanel:
            m_PanelVisible = !m_PanelVisible;
            break;
        case Cmd::ShowPanel:
            m_PanelVisible = true;
            break;
        }
        if (c.cmd != Cmd::TogglePanel || m_PanelVisible) {
            m_PanelShownNs = now;
            m_LastInteractNs.store(now, std::memory_order_relaxed);
        }
        updatePanel(true);
    }
}

void PlayerSession::onPanelClick(float u, float v)
{
    float frac = 0.0f;
    const int a = PlayerPanel::hitTest(u, v, &frac);
    switch (a) {
    case PlayerPanel::Timeline:
        if (m_Media.info().durationSec > 0.0) {
            post(Cmd::Seek, frac * m_Media.info().durationSec);
        }
        break;
    case PlayerPanel::Back30: post(Cmd::SeekRel, -30.0); break;
    case PlayerPanel::Back10: post(Cmd::SeekRel, -10.0); break;
    case PlayerPanel::PlayPause: post(Cmd::TogglePause); break;
    case PlayerPanel::Fwd10: post(Cmd::SeekRel, 10.0); break;
    case PlayerPanel::Fwd30: post(Cmd::SeekRel, 30.0); break;
    case PlayerPanel::VolDown: post(Cmd::VolumeRel, -0.1); break;
    case PlayerPanel::VolUp: post(Cmd::VolumeRel, 0.1); break;
    case PlayerPanel::Format: post(Cmd::Format, -1.0); break;
    case PlayerPanel::Screen: post(Cmd::Screen, -1.0); break;
    case PlayerPanel::Recenter: post(Cmd::Recenter); break;
    case PlayerPanel::Exit: post(Cmd::Exit); break;
    default: post(Cmd::ShowPanel); break;
    }
}

void PlayerSession::onStick(float x, float y)
{
    // XR frame thread：只判斷方向與重複，指令交給播放器執行緒
    const uint64_t now = steadyNowNs();
    const int xd = x > kStickOn ? 1 : (x < -kStickOn ? -1 : 0);
    const int yd = y > kStickOn ? 1 : (y < -kStickOn ? -1 : 0);
    if (xd != 0 && (xd != m_StickXDir || now - m_StickXNs >= kStickSeekRepeatNs)) {
        m_StickXNs = now;
        post(Cmd::SeekRel, xd * 10.0);
    }
    m_StickXDir = xd;
    // 上下＝遠近（使用者 2026-10-19：有些片太近；音量留在控制列的按鈕）。往上推＝推遠
    if (yd != 0 && (yd != m_StickYDir || now - m_StickYNs >= kStickVolRepeatNs)) {
        m_StickYNs = now;
        post(Cmd::DepthRel, yd);
    }
    m_StickYDir = yd;
}

void PlayerSession::updatePanel(bool force)
{
    const uint64_t now = steadyNowNs();
    const uint64_t lastInteract = m_LastInteractNs.load(std::memory_order_relaxed);
    if (m_PanelVisible && !m_Media.paused() && !m_Ended && now - std::max(lastInteract, m_PanelShownNs) > kPanelAutoHideNs) {
        m_PanelVisible = false;
        force = true;
    }
    if (m_Media.paused() && !m_PanelVisible) {
        m_PanelVisible = true;
        force = true;
    }
    const int hover = m_Hover.load(std::memory_order_relaxed);
    const float hoverTl = m_HoverTimeline.load(std::memory_order_relaxed);
    if (hover != m_PanelState.hover || std::fabs(hoverTl - m_PanelState.hoverTimeline) > 0.002f) {
        force = true;
    }
    if (!force && (!m_PanelVisible || now - m_PanelDrawnNs < kPanelRedrawNs)) {
        return;
    }
    m_PanelDrawnNs = now;
    m_PanelState.positionSec = m_Media.position();
    m_PanelState.paused = m_Media.paused();
    m_PanelState.volume = m_Media.volume();
    uint64_t dummy = 0;
    m_PanelState.buffering = !m_PanelState.paused && !m_Ended && !m_Media.mediaToSteady(m_PanelState.positionSec, &dummy);
    m_PanelState.hover = hover;
    m_PanelState.hoverTimeline = hoverTl;
    const QImage img = m_PanelVisible ? PlayerPanel::render(m_PanelState) : QImage();
    if (!img.isNull() && !m_PanelDumped && !m_Config.dumpPanelPath.isEmpty()) {
        m_PanelDumped = true;
        const bool ok = img.save(m_Config.dumpPanelPath, "PNG");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] dev panel dump -> %s (%s)", qPrintable(m_Config.dumpPanelPath),
                    ok ? "ok" : "FAILED");
    }
    if (m_Xr != nullptr) {
        m_Xr->setPlayerPanel(img, m_PanelVisible);
    }
}

void PlayerSession::logWindow(uint64_t nowNs, bool final)
{
    const MediaPipeline::Stats s = m_Media.takeStats();
    const double winSec = static_cast<double>(nowNs - m_WinStartNs) / 1e9;
    XrContext::DisplayTiming dt;
    if (m_Xr != nullptr) {
        dt = m_Xr->displayTiming();
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PLAYER] %s pos=%.1f/%.1f %s submitted=%u (%.2f fps) dropped=%u late=%u preview=%u "
                "slots(0,1..6,7+)=%u,%u,%u,%u,%u,%u,%u,%u leadMin=%.1f ms | decoded=%u decErr=%u decMax=%u us seekDrop=%u | "
                "clkErr=%.1f ms underrun=%u buffered=%.1f s fq=%d rx=%.1f Mbps | hz=%.2f%s",
                final ? "final" : "10s", m_Media.position(), m_Media.info().durationSec,
                m_Media.paused() ? "paused" : (m_Ended ? "ended" : "playing"), m_Submitted,
                winSec > 0.0 ? m_Submitted / winSec : 0.0, m_Dropped, m_Late, m_Previews, m_SlotHist[0], m_SlotHist[1],
                m_SlotHist[2], m_SlotHist[3], m_SlotHist[4], m_SlotHist[5], m_SlotHist[6], m_SlotHist[7],
                m_LeadMinMs < 1e8f ? static_cast<double>(m_LeadMinMs) : 0.0, s.decoded, s.decodeErrors, s.decodeUsMax,
                s.seekDropped, static_cast<double>(s.clockErrMaxMs), s.audioUnderruns, s.bufferedSec, s.frameQueue,
                winSec > 0.0 ? static_cast<double>(s.demuxBytes) * 8.0 / winSec / 1e6 : 0.0,
                dt.periodNs > 0 ? 1e9 / static_cast<double>(dt.periodNs) : 0.0, dt.periodNs > 0 && !dt.exact ? " (approx)" : "");
    m_WinStartNs = nowNs;
    m_Submitted = m_Dropped = m_Late = m_Previews = 0;
    std::fill(std::begin(m_SlotHist), std::end(m_SlotHist), 0u);
    m_LeadMinMs = 1e9f;
}

void PlayerSession::runTestScript(double elapsedSec)
{
    // "秒:指令[=值],…"：pause、play、seek=<秒>、seekrel=<秒>、vol=<0..2>、fmt=<名稱>、screen=<0..2>、panel、
    // depthrel=<格數>（遠近）、click=<u>:<v>（點控制列）、recenter、exit。照時間順序執行，每個只執行一次
    const QStringList steps = m_Config.testScript.split(QLatin1Char(','), Qt::SkipEmptyParts);
    while (m_TestStep < steps.size()) {
        const QString step = steps.at(m_TestStep).trimmed();
        const int colon = step.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            m_TestStep++;
            continue;
        }
        if (step.left(colon).toDouble() > elapsedSec) {
            return;
        }
        m_TestStep++;
        const QString cmd = step.mid(colon + 1);
        const QString name = cmd.section(QLatin1Char('='), 0, 0);
        const QString val = cmd.section(QLatin1Char('='), 1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] dev test: %s", qPrintable(cmd));
        if (name == QLatin1String("pause") || name == QLatin1String("play")) {
            if (m_Media.paused() != (name == QLatin1String("pause"))) {
                post(Cmd::TogglePause);
            }
        }
        else if (name == QLatin1String("seek")) {
            post(Cmd::Seek, val.toDouble());
        }
        else if (name == QLatin1String("seekrel")) {
            post(Cmd::SeekRel, val.toDouble());
        }
        else if (name == QLatin1String("vol")) {
            post(Cmd::Volume, val.toDouble());
        }
        else if (name == QLatin1String("fmt")) {
            for (int i = 0; i < static_cast<int>(sizeof(kFormatCycle) / sizeof(kFormatCycle[0])); i++) {
                if (val == QLatin1String(kFormatCycle[i])) {
                    post(Cmd::Format, i);
                }
            }
        }
        else if (name == QLatin1String("screen")) {
            post(Cmd::Screen, val.toDouble());
        }
        else if (name == QLatin1String("panel")) {
            post(Cmd::TogglePanel);
        }
        else if (name == QLatin1String("depthrel")) {
            post(Cmd::DepthRel, val.toDouble());
        }
        else if (name == QLatin1String("click")) {
            onPanelClick(val.section(QLatin1Char(':'), 0, 0).toFloat(), val.section(QLatin1Char(':'), 1, 1).toFloat());
        }
        else if (name == QLatin1String("recenter")) {
            post(Cmd::Recenter);
        }
        else if (name == QLatin1String("exit")) {
            post(Cmd::Exit);
        }
    }
}

}  // namespace player
