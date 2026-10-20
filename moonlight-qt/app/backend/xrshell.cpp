#include "xrshell.h"

#include "vrlauncher.h"

#include "streaming/session.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <thread>

#include "SDL_compat.h"

#ifdef HAVE_OPENXR
#include <Limelight.h>

#include "streaming/xr/xrcontext.h"
#endif
#ifdef HAVE_VR_PLAYER
#include "streaming/player/playersession.h"
#endif

namespace {

// 結束前送空畫面多久（shutdownXrForExit）
constexpr int kBlankBeforeExitMs = 600;
// 按住時的晃動容許（面板像素；1280 像素寬約 62°，40 像素約 2°）
constexpr qreal kClickSlopPx = 40.0;
// 介面重畫的最小間隔（最多約 30 張／秒）
constexpr uint64_t kMinFrameNs = 33000000ull;
// 介面面板的距離與水平張角（影片模式結束後換回來）
constexpr float kShellDistanceM = 1.5f;
constexpr float kShellFovDeg = 62.0f;

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 虛擬鍵盤送的是 Win32 VK（xrkeyboard.h）；介面要的是 Qt 的按鍵與文字（US 版面，同鍵盤上印的字）
Qt::Key vkToQt(int vk, bool shift, QString* text)
{
    text->clear();
    if (vk >= 0x41 && vk <= 0x5A) {
        const QChar c = QChar(vk);
        *text = shift ? QString(c) : QString(c.toLower());
        return static_cast<Qt::Key>(Qt::Key_A + (vk - 0x41));
    }
    if (vk >= 0x30 && vk <= 0x39) {
        static const char kShifted[] = ")!@#$%^&*(";
        *text = shift ? QString(QChar(kShifted[vk - 0x30])) : QString(QChar(vk));
        return static_cast<Qt::Key>(Qt::Key_0 + (vk - 0x30));
    }
    struct Oem {
        int vk;
        char plain, shifted;
    };
    static const Oem kOem[] = {{0xBA, ';', ':'}, {0xBB, '=', '+'}, {0xBC, ',', '<'}, {0xBD, '-', '_'}, {0xBE, '.', '>'},
                               {0xBF, '/', '?'}, {0xC0, '`', '~'}, {0xDB, '[', '{'}, {0xDC, '\\', '|'}, {0xDD, ']', '}'},
                               {0xDE, '\'', '"'}};
    for (const Oem& o : kOem) {
        if (o.vk == vk) {
            const char c = shift ? o.shifted : o.plain;
            *text = QString(QChar(c));
            // Qt::Key 的可列印字元就是它的 Latin-1 碼（大寫）
            return static_cast<Qt::Key>(QChar(c).toUpper().unicode());
        }
    }
    switch (vk) {
    case 0x08: return Qt::Key_Backspace;
    case 0x09: *text = QStringLiteral("\t"); return Qt::Key_Tab;
    case 0x0D: *text = QStringLiteral("\r"); return Qt::Key_Return;
    case 0x1B: return Qt::Key_Escape;
    case 0x20: *text = QStringLiteral(" "); return Qt::Key_Space;
    case 0x25: return Qt::Key_Left;
    case 0x26: return Qt::Key_Up;
    case 0x27: return Qt::Key_Right;
    case 0x28: return Qt::Key_Down;
    case 0x2E: return Qt::Key_Delete;
    default: return Qt::Key_unknown;
    }
}

}  // namespace

bool XrShell::parseSpec(const QString& spec, Config* out, QString* error)
{
    *out = Config();
    const QStringList parts = spec.split(QLatin1Char(','));
    const QString mode = parts.value(0).trimmed();
    if (mode == QLatin1String("noxr")) {
        out->noXr = true;
    }
    else if (mode == QLatin1String("off")) {
        out->off = true;
    }
    else if (mode != QLatin1String("xr")) {
        *error = QStringLiteral("the first item must be xr, noxr or off");
        return false;
    }
    for (const QString& p : parts.mid(1)) {
        const QString k = p.section(QLatin1Char('='), 0, 0), v = p.section(QLatin1Char('='), 1);
        if (k == QLatin1String("dump")) {
            out->dumpSec = v.section(QLatin1Char(':'), 0, 0).toInt();
            out->dumpPath = v.section(QLatin1Char(':'), 1);
            if (out->dumpSec <= 0 || out->dumpPath.isEmpty()) {
                *error = QStringLiteral("dump=<sec>:<png>");
                return false;
            }
        }
        else if (k == QLatin1String("test")) {
            out->testScript = v;
        }
        else if (k == QLatin1String("childplayer")) {
            out->childPlayer = true;
        }
        else {
            *error = QStringLiteral("unknown item: ") + p;
            return false;
        }
    }
    return true;
}

XrShell::XrShell(QQmlEngine* engine, const Config& config, QObject* parent)
    : QObject(parent),
      m_Engine(engine),
      m_Config(config)
{
    m_RenderTimer.setSingleShot(true);
    connect(&m_RenderTimer, &QTimer::timeout, this, &XrShell::renderNow);
}

void XrShell::prepareExit()
{
    m_RenderTimer.stop();
    m_TestTimer.stop();
    VrLauncher::setChildHook(nullptr);
    VrLauncher::setPlayerHandler(nullptr);
    Session::setXrOwnerHook(nullptr);
#ifdef HAVE_VR_PLAYER
    if (m_Player != nullptr) {
        {
            std::lock_guard<std::mutex> lk(m_RouteMutex);
            m_Route = nullptr;
        }
        delete m_Player;  // 要求結束並等它的執行緒（它在用 m_Xr，要先於 shutdownXr）
        m_Player = nullptr;
    }
#endif
}

void XrShell::shutdownXrForExit()
{
#ifdef HAVE_OPENXR
    if (m_Xr != nullptr && m_Xr->isRunning()) {
        m_Xr->setBlank(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(kBlankBeforeExitMs));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] exit: blanked for %d ms, closing the XR session", kBlankBeforeExitMs);
    }
#endif
    shutdownXr();
}

XrShell::~XrShell()
{
    prepareExit();
    // Qt Quick 的東西要在 GL context 還在的時候拆（render control 要釋放場景圖的 GL 資源）。XR 最後才關（見 shutdownXrForExit）
    if (m_Gl != nullptr && m_Surface != nullptr) {
        m_Gl->makeCurrent(m_Surface);
    }
    delete m_Root;
    m_Root = nullptr;
    delete m_Rc;  // 先於視窗（Qt 文件的順序：render control 釋放場景圖，再刪視窗）
    m_Rc = nullptr;
    delete m_Win;
    m_Win = nullptr;
    if (m_Gl != nullptr && m_Surface != nullptr && m_Gl->makeCurrent(m_Surface)) {
        QOpenGLExtraFunctions* f = m_Gl->extraFunctions();
        if (m_Fbo != 0) {
            f->glDeleteFramebuffers(1, &m_Fbo);
        }
        if (m_Tex != 0) {
            f->glDeleteTextures(1, &m_Tex);
        }
        m_Gl->doneCurrent();
    }
    delete m_Gl;
    delete m_Surface;
    shutdownXr();
}

bool XrShell::start(QString* error)
{
    m_StartMs = QDateTime::currentMSecsSinceEpoch();

    // ── 離屏的 OpenGL 與 Qt Quick ──
    m_Gl = new QOpenGLContext();
    if (QOpenGLContext::globalShareContext() != nullptr) {
        m_Gl->setShareContext(QOpenGLContext::globalShareContext());  // Qt WebEngine（內建瀏覽器）要共用
    }
    if (!m_Gl->create()) {
        *error = QStringLiteral("could not create an OpenGL context");
        return false;
    }
    m_Surface = new QOffscreenSurface();
    m_Surface->setFormat(m_Gl->format());
    m_Surface->create();
    if (!m_Gl->makeCurrent(m_Surface)) {
        *error = QStringLiteral("could not make the OpenGL context current");
        return false;
    }

    m_Rc = new QQuickRenderControl();
    m_Win = new QQuickWindow(m_Rc);
    m_Win->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_Gl));
    m_Win->setGeometry(0, 0, kWidth, kHeight);
    if (!m_Rc->initialize()) {
        *error = QStringLiteral("QQuickRenderControl::initialize failed");
        return false;
    }

    QOpenGLExtraFunctions* f = m_Gl->extraFunctions();
    f->glGenTextures(1, &m_Tex);
    f->glBindTexture(GL_TEXTURE_2D, m_Tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glGenFramebuffers(1, &m_Fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_Tex, 0);
    const GLenum fbStatus = f->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_Gl->defaultFramebufferObject());
    if (fbStatus != GL_FRAMEBUFFER_COMPLETE) {
        *error = QStringLiteral("readback framebuffer incomplete (0x%1)").arg(fbStatus, 0, 16);
        return false;
    }
    m_Win->setRenderTarget(QQuickRenderTarget::fromOpenGLTexture(m_Tex, QSize(kWidth, kHeight)));

    connect(m_Rc, &QQuickRenderControl::renderRequested, this, [this]() { scheduleRender(false); });
    connect(m_Rc, &QQuickRenderControl::sceneChanged, this, [this]() { scheduleRender(true); });
    connect(m_Win, &QQuickWindow::activeFocusItemChanged, this, &XrShell::focusChanged);

    // ── 介面：先掛上視窗再完成建立（Component.onCompleted 裡開的對話框要找得到視窗的 overlay）──
    QQmlComponent comp(m_Engine, QUrl(QStringLiteral("qrc:/gui/VrShell.qml")));
    if (comp.isError()) {
        *error = comp.errorString();
        return false;
    }
    QObject* obj = comp.beginCreate(m_Engine->rootContext());
    m_Root = qobject_cast<QQuickItem*>(obj);
    if (m_Root == nullptr) {
        delete obj;
        *error = QStringLiteral("VrShell.qml did not create an Item: ") + comp.errorString();
        return false;
    }
    m_Root->setParentItem(m_Win->contentItem());
    // QQmlApplicationEngine 只在 load() 時把 Qt.quit()／Qt.exit() 接到程式結束；這裡是自己建的元件，要自己接
    // （頭盔 10-20：結束對話框按 Yes 沒反應，log「QQmlEngine::quit() emitted, but no receivers」）
    connect(m_Engine, &QQmlEngine::quit, QCoreApplication::instance(), &QCoreApplication::quit, Qt::QueuedConnection);
    connect(m_Engine, &QQmlEngine::exit, QCoreApplication::instance(), &QCoreApplication::exit, Qt::QueuedConnection);
    comp.completeCreate();
    m_Win->contentItem()->setSize(QSizeF(kWidth, kHeight));
    // 離屏視窗不會被視窗系統啟用；給它焦點，鍵盤事件才會送到有焦點的元件
    QFocusEvent focusIn(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(m_Win, &focusIn);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] UI loaded (%dx%d, GL %d.%d%s)", kWidth, kHeight,
                m_Gl->format().majorVersion(), m_Gl->format().minorVersion(), m_Gl->isOpenGLES() ? " ES" : "");

    // ── XR ──
    if (!m_Config.noXr && !bringUpXr(error)) {
        return false;
    }
    // VR 串流還是子行程。它要自己的 XR session，開之前先把這邊的關掉
    VrLauncher::setChildHook([this](VrLauncher::ChildEvent e) { onChild(static_cast<int>(e)); });
    // 桌面模式的串流在這個程式裡，但用它自己的 XrContext（第一次要 XR 時在連線執行緒呼叫，這時主執行緒照常跑事件迴圈）
    Session::setXrOwnerHook([this](bool wantsXr) {
        if (QThread::currentThread() == thread()) {
            lendXr(wantsXr);
        }
        else {
            QMetaObject::invokeMethod(this, [this, wantsXr]() { lendXr(wantsXr); }, Qt::BlockingQueuedConnection);
        }
    });
#ifdef HAVE_VR_PLAYER
    // A2：影片模式在這個 session 裡播
    if (!m_Config.noXr && !m_Config.childPlayer) {
        VrLauncher::setPlayerHandler([this](const VrLauncher::PlayRequest& r) {
            return startPlayer(r.source, r.format, r.title, r.audioSource);
        });
    }
#endif

    m_WinStartNs = steadyNowNs();
    scheduleRender(true);

    if (!m_Config.testScript.isEmpty() || m_Config.dumpSec > 0) {
        connect(&m_TestTimer, &QTimer::timeout, this, &XrShell::runTest);
        m_TestTimer.start(100);
    }
    return true;
}

bool XrShell::bringUpXr(QString* error)
{
#ifdef HAVE_OPENXR
    XrContext::Options xo;
    xo.player = true;  // 一律 quad、有控制列（A2 的影片模式直接用同一個 session）
    xo.quadDistanceM = kShellDistanceM;
    xo.quadFovDeg = kShellFovDeg;
    xo.shellWidth = kWidth;
    xo.shellHeight = kHeight;
    // 事件在 XR frame thread 產生，轉到 GUI 執行緒再送進介面
    xo.inputSink.pointer = [this](float u, float v) {
        QMetaObject::invokeMethod(this, [this, u, v]() { pointerMoved(u, v); }, Qt::QueuedConnection);
    };
    xo.inputSink.button = [this](int button, bool pressed) {
        const Qt::MouseButton b = button == BUTTON_LEFT ? Qt::LeftButton : Qt::RightButton;
        QMetaObject::invokeMethod(this, [this, b, pressed]() { mouseButton(b, pressed); }, Qt::QueuedConnection);
    };
    xo.inputSink.scroll = [this](int amount) {
        QMetaObject::invokeMethod(this, [this, amount]() { wheel(amount); }, Qt::QueuedConnection);
    };
    xo.inputSink.key = [this](int vk, bool down, uint8_t mods) {
        QMetaObject::invokeMethod(this, [this, vk, down, mods]() { key(vk, down, mods); }, Qt::QueuedConnection);
    };
    xo.inputSink.back = [this]() { QMetaObject::invokeMethod(this, [this]() { back(); }, Qt::QueuedConnection); };
#ifdef HAVE_VR_PLAYER
    // A2：螢幕換成影片之後，按鍵、搖桿與控制列交給播放器（同 viplestream play 的配置）；之前（載入中）照一般射線滑鼠操作介面
    xo.inputSink.playerActive = [this]() {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        return m_Route != nullptr && m_Route->videoShown();
    };
    xo.inputSink.playerButton = [this](XrInputSink::PlayerButton b) {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        if (m_Route != nullptr) {
            m_Route->inputButton(static_cast<int>(b));
        }
    };
    xo.inputSink.menu = [this]() {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        if (m_Route != nullptr) {
            m_Route->inputMenu();
        }
    };
    xo.inputSink.stick = [this](float x, float y) {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        if (m_Route != nullptr) {
            m_Route->inputStick(x, y);
        }
    };
    xo.inputSink.panel = [this](float u, float v) {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        if (m_Route != nullptr) {
            m_Route->inputPanel(u, v);
        }
    };
    xo.inputSink.panelClick = [this](float u, float v) {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        if (m_Route != nullptr) {
            m_Route->inputPanelClick(u, v);
        }
    };
#else
    xo.inputSink.playerActive = []() { return false; };
#endif
    xo.onEnded = [this](int reason) {
        QMetaObject::invokeMethod(this, [this, reason]() { onXrEnded(reason); }, Qt::QueuedConnection);
    };
    m_Xr = new XrContext(xo);
    const uint64_t t0 = steadyNowNs();
    if (!m_Xr->bringUp(15000, error)) {
        delete m_Xr;
        m_Xr = nullptr;
        return false;
    }
    m_Xr->setShellActive(true);
    m_NeedSync = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] XR up in %llu ms",
                static_cast<unsigned long long>((steadyNowNs() - t0) / 1000000ull));
    // 新的 session 要重新上傳目前的畫面
    if (!m_Last.isNull()) {
        m_Xr->setShellImage(m_Last.convertToFormat(m_Xr->shellImageFormat()));
    }
    return true;
#else
    *error = QStringLiteral("this build has no OpenXR");
    return false;
#endif
}

void XrShell::shutdownXr()
{
#ifdef HAVE_OPENXR
    if (m_Xr != nullptr) {
        m_Xr->shutdown();
        delete m_Xr;
        m_Xr = nullptr;
    }
#endif
}

void XrShell::lendXr(bool wantsXr)
{
    if (m_Config.noXr) {
        return;
    }
    if (wantsXr && !m_XrSuspended) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] desktop stream starting: closing the shell XR session");
        m_XrSuspended = true;
        shutdownXr();
    }
    else if (!wantsXr && m_XrSuspended) {
        reopenXr("desktop stream ended");
    }
}

bool XrShell::reopenXr(const char* why)
{
    if (!m_XrSuspended) {
        return true;
    }
    m_XrSuspended = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] %s: reopening the shell XR session", why);
    QString err;
    if (!bringUpXr(&err)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] XR bring-up failed: %s; quitting", qPrintable(err));
        QCoreApplication::quit();
        return false;
    }
    return true;
}

void XrShell::onChild(int event)
{
    if (m_Config.noXr) {
        return;
    }
    switch (static_cast<VrLauncher::ChildEvent>(event)) {
    case VrLauncher::ChildEvent::Starting:
        // 不關 session：子行程的 session 起來時 SteamVR 會把這邊的結束掉（onXrEnded 當成交出去）
        m_ChildRunning = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] VR stream process starting: keeping the shell XR session until it takes over");
        break;
    case VrLauncher::ChildEvent::HandBack:
        // 子行程要結束了、還拿著頭盔：先開回這邊的 session，SteamVR 再把它的結束掉
        reopenXr("the VR stream process is handing the headset back");
        break;
    case VrLauncher::ChildEvent::Ended:
        m_ChildRunning = false;
        reopenXr("the VR stream process ended");  // 沒交還就結束（出錯、被關掉）時才會真的開
        break;
    }
}

void XrShell::onXrEnded(int reason)
{
#ifdef HAVE_OPENXR
    if (m_ChildRunning && reason == XrContext::kEndedExit) {
        // VR 串流子行程的 session 接手了
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] the VR stream process took over the headset");
        m_XrSuspended = true;
        shutdownXr();
        return;
    }
    // 使用者從 SteamVR 離開（或 runtime 出事）：整個程式結束
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] XR session ended (%s); quitting",
                reason == XrContext::kEndedLoss ? "lost" : "exit");
#else
    Q_UNUSED(reason)
#endif
    QCoreApplication::quit();
}

bool XrShell::startPlayer(const QString& source, const QString& format, const QString& title, const QString& audioSource)
{
#ifdef HAVE_VR_PLAYER
    if (m_Xr == nullptr || m_Player != nullptr) {
        return false;
    }
    player::PlayerSession::Config pc;
    pc.source = source;
    pc.audioSource = audioSource;
    pc.title = title;
    pc.format = format.isEmpty() ? QStringLiteral("auto") : format;
    pc.sharedXr = m_Xr;
    m_Player = new player::PlayerSession(pc, this);
    connect(m_Player, &player::PlayerSession::finished, this, [this](int rc, const QString& error) {
        {
            std::lock_guard<std::mutex> lk(m_RouteMutex);
            m_Route = nullptr;
        }
        delete m_Player;  // 執行緒已經跑完 run()，這裡只是 join
        m_Player = nullptr;
        // 播放器已經把螢幕換回介面；距離與大小換回介面的
        if (m_Xr != nullptr) {
            m_Xr->setPlayerScreen(kShellDistanceM, kShellFovDeg);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] video player finished (rc=%d)", rc);
        VrLauncher::playerFinished(rc, error);
        scheduleRender(true);
    });
    {
        std::lock_guard<std::mutex> lk(m_RouteMutex);
        m_Route = m_Player;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] starting the video player in this XR session");
    m_Player->start();
    return true;
#else
    Q_UNUSED(source)
    Q_UNUSED(format)
    Q_UNUSED(title)
    Q_UNUSED(audioSource)
    return false;
#endif
}

void XrShell::scheduleRender(bool sync)
{
    m_NeedSync = m_NeedSync || sync;
    if (!m_RenderTimer.isActive()) {
        // 合併同一輪事件裡的多次變更；兩張之間至少隔 kMinFrameNs（介面上有動畫時一直在重畫，每張都要讀回、在 XR frame
        // thread 上傳：開發機量到動畫中每秒 72 張）。指標是 XR 自己畫的圖層，不受這個限制
        const uint64_t now = steadyNowNs();
        const uint64_t since = now - m_LastRenderNs;
        const uint64_t waitNs = since >= kMinFrameNs ? 0 : kMinFrameNs - since;
        m_RenderTimer.start(static_cast<int>(std::max<uint64_t>(waitNs / 1000000ull, 4)));
    }
}

void XrShell::renderNow()
{
    if (m_Rc == nullptr || !m_Gl->makeCurrent(m_Surface)) {
        return;
    }
    const uint64_t t0 = steadyNowNs();
    m_LastRenderNs = t0;
    m_Rc->polishItems();
    m_Rc->beginFrame();
    m_Rc->sync();
    m_Rc->render();
    m_Rc->endFrame();
    m_NeedSync = false;

    // 讀回：GL 的列由下往上，翻成 QImage 的由上往下。介面整片不透明（VrShell.qml 的底色），直接當成不預乘的 RGBA
    QOpenGLExtraFunctions* f = m_Gl->extraFunctions();
    QImage img(kWidth, kHeight, QImage::Format_RGBA8888);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_Fbo);
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_Gl->defaultFramebufferObject());
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    img.flip(Qt::Vertical);
#else
    img = img.mirrored(false, true);
#endif
    m_Last = img;
#ifdef HAVE_OPENXR
    if (m_Xr != nullptr) {
        m_Xr->setShellImage(img.convertToFormat(m_Xr->shellImageFormat()));
    }
#endif
    const uint64_t t1 = steadyNowNs();
    m_Frames++;
    m_RenderNs += t1 - t0;
    if (t1 - m_WinStartNs >= 10000000000ull) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] 10s: %llu frames, render+readback avg %.1f ms",
                    static_cast<unsigned long long>(m_Frames),
                    m_Frames > 0 ? static_cast<double>(m_RenderNs) / 1e6 / static_cast<double>(m_Frames) : 0.0);
        m_Frames = 0;
        m_RenderNs = 0;
        m_WinStartNs = t1;
    }
}

void XrShell::pointerMoved(float u, float v)
{
    const QPointF p(u * kWidth, v * kHeight);
    if (m_Buttons != Qt::NoButton && !m_PressSlopExceeded) {
        const QPointF d = p - m_PressPos;
        if (d.x() * d.x() + d.y() * d.y() < kClickSlopPx * kClickSlopPx) {
            return;  // 按住時的晃動：游標留在按下的位置（m_Pointer 不變，放開也在那裡）
        }
        m_PressSlopExceeded = true;
    }
    m_Pointer = p;
    QMouseEvent ev(QEvent::MouseMove, m_Pointer, m_Pointer, Qt::NoButton, m_Buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(m_Win, &ev);
}

void XrShell::mouseButton(Qt::MouseButton button, bool pressed)
{
    if (m_Pointer.x() < 0) {
        return;
    }
    if (pressed == ((m_Buttons & button) != 0)) {
        return;
    }
    if (pressed && m_Buttons == Qt::NoButton) {
        m_PressPos = m_Pointer;
        m_PressSlopExceeded = false;
    }
    m_Buttons = pressed ? (m_Buttons | button) : (m_Buttons & ~button);
    QMouseEvent ev(pressed ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease, m_Pointer, m_Pointer, button, m_Buttons,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(m_Win, &ev);
}

void XrShell::wheel(int amount)
{
    if (m_Pointer.x() < 0) {
        return;
    }
    QWheelEvent ev(m_Pointer, m_Pointer, QPoint(), QPoint(0, amount), m_Buttons, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(m_Win, &ev);
}

void XrShell::sendKeyPress(int qtKey, const QString& text, Qt::KeyboardModifiers mods, bool down)
{
    QKeyEvent ev(down ? QEvent::KeyPress : QEvent::KeyRelease, qtKey, mods, text);
    QCoreApplication::sendEvent(m_Win, &ev);
}

void XrShell::key(int vk, bool down, uint8_t mods)
{
    // 修飾鍵本身（黏滯鍵送出的左側 Shift／Ctrl／Alt／Win）不必送：mods 已經帶著狀態
    if (vk == 0x5B || (vk >= 0xA0 && vk <= 0xA5) || (vk >= 0x10 && vk <= 0x12)) {
        return;
    }
    const bool shift = (mods & 0x01) != 0;
    Qt::KeyboardModifiers qm = Qt::NoModifier;
    if (shift) {
        qm |= Qt::ShiftModifier;
    }
    if (mods & 0x02) {
        qm |= Qt::ControlModifier;
    }
    if (mods & 0x04) {
        qm |= Qt::AltModifier;
    }
    if (mods & 0x08) {
        qm |= Qt::MetaModifier;
    }
    QString text;
    const Qt::Key k = vkToQt(vk, shift, &text);
    if (k == Qt::Key_unknown) {
        return;
    }
    if (qm & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        text.clear();  // Ctrl+A 等是快速鍵，不是輸入文字
    }
    sendKeyPress(k, text, qm, down);
}

void XrShell::back()
{
    // 同平面介面的 Esc：關對話框、回上一頁（StackView 的 Keys.onEscapePressed）
    sendKeyPress(Qt::Key_Escape, QString(), Qt::NoModifier, true);
    sendKeyPress(Qt::Key_Escape, QString(), Qt::NoModifier, false);
}

void XrShell::focusChanged()
{
    QQuickItem* item = m_Win->activeFocusItem();
    const bool text = item != nullptr && (item->inherits("QQuickTextInput") || item->inherits("QQuickTextEdit"));
    if (text == m_TextFocus) {
        return;
    }
    m_TextFocus = text;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] text field %s", text ? "focused: opening the keyboard" : "unfocused");
#ifdef HAVE_OPENXR
    if (m_Xr != nullptr) {
        m_Xr->requestKeyboard(text);
    }
#endif
}

void XrShell::runTest()
{
    // dev 腳本："秒:指令[:參數…];…"，秒數從 start() 起算，依序執行。指令：
    //   move:x:y  click:x:y  rclick:x:y（面板像素座標）、wheel:量（120＝一格，正＝往上）、type:文字、key:vk（十六進位）、
    //   back、pbutton:n（影片模式的手把按鍵）、dump:png、quit
    const double elapsed = static_cast<double>(QDateTime::currentMSecsSinceEpoch() - m_StartMs) / 1000.0;
    if (m_Config.dumpSec > 0 && elapsed >= m_Config.dumpSec) {
        const bool ok = m_Last.save(m_Config.dumpPath, "PNG");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] dev dump -> %s (%s)", qPrintable(m_Config.dumpPath),
                    ok ? "ok" : "FAILED");
        m_Config.dumpSec = 0;
    }
    const QStringList steps = m_Config.testScript.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    while (m_TestStep < steps.size()) {
        const QStringList a = steps[m_TestStep].split(QLatin1Char(':'));
        if (elapsed < a.value(0).toDouble()) {
            break;
        }
        m_TestStep++;
        const QString cmd = a.value(1);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] dev step %d: %s", m_TestStep, qPrintable(steps[m_TestStep - 1]));
        if (cmd == QLatin1String("move") || cmd == QLatin1String("click") || cmd == QLatin1String("rclick")) {
            pointerMoved(a.value(2).toFloat() / kWidth, a.value(3).toFloat() / kHeight);
            if (cmd != QLatin1String("move")) {
                const Qt::MouseButton b = cmd == QLatin1String("click") ? Qt::LeftButton : Qt::RightButton;
                mouseButton(b, true);
                mouseButton(b, false);
            }
        }
        else if (cmd == QLatin1String("wheel")) {
            wheel(a.value(2).toInt());
        }
        else if (cmd == QLatin1String("type")) {
            for (const QChar c : a.mid(2).join(QLatin1Char(':'))) {
                sendKeyPress(c.isLetter() ? (Qt::Key_A + (c.toUpper().unicode() - 'A')) : c.unicode(), QString(c), Qt::NoModifier, true);
                sendKeyPress(c.isLetter() ? (Qt::Key_A + (c.toUpper().unicode() - 'A')) : c.unicode(), QString(c), Qt::NoModifier, false);
            }
        }
        else if (cmd == QLatin1String("key")) {
            const int vk = a.value(2).toInt(nullptr, 16);
            key(vk, true, 0);
            key(vk, false, 0);
        }
        else if (cmd == QLatin1String("back")) {
            back();
        }
#ifdef HAVE_VR_PLAYER
        else if (cmd == QLatin1String("pbutton")) {
            // 影片模式的手把按鍵（XrInputSink::PlayerButton：0 扳機、1 A、2 B、3 握把），同 XR frame thread 轉進去的路徑
            std::lock_guard<std::mutex> lk(m_RouteMutex);
            if (m_Route != nullptr) {
                m_Route->inputButton(a.value(2).toInt());
            }
        }
#endif
        else if (cmd == QLatin1String("dump")) {
            renderNow();
            const bool ok = m_Last.save(a.mid(2).join(QLatin1Char(':')), "PNG");
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SHELL] dev dump %s", ok ? "ok" : "FAILED");
        }
        else if (cmd == QLatin1String("quit")) {
            QCoreApplication::quit();
        }
    }
}
