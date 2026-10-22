#include "sdlgamepadkeynavigation.h"

#include <QKeyEvent>
#include <QWheelEvent>
#include <QGuiApplication>
#include <QStringList>
#include <QWindow>
#include <QtMath>

#include "settings/mappingmanager.h"
#include "backend/hidprobe.h"   // §HID-PROBE

// §HID-PROBE：enable() 因 SDL_InitSubSystem 失敗而放棄時記一筆，讓 getConnectedGamepads()
// 的「未 enable 就呼叫」斷言只對真正的程式錯誤觸發（審查建議）。本類別是 QML 單例。
static bool s_InitFailed = false;

// §VR-SHELL-SCROLL：頭盔 VR 介面自己用 XR 搖桿捲了頁面的時刻（SDL_GetTicks）。同一支搖桿也可能被 SDL 當成手把讀到，
// 這段時間內這裡不再發 scrollRequested，免得兩邊一起捲（速度變兩倍、方向打架）
static Uint32 s_ExternalScrollTicks = 0;
static constexpr Uint32 EXTERNAL_SCROLL_HOLD_MS = 300;

void SdlGamepadKeyNavigation::noteExternalScroll()
{
    s_ExternalScrollTicks = SDL_GetTicks();
}

#define AXIS_NAVIGATION_REPEAT_DELAY 150

// 左搖桿推過這個值才算一次上下左右。原本是 30000（滿量程 32767 的 92%），要幾乎直直推到底才會觸發；
// 24000 約是 73%，斜一點或沒推到底也算。（沒有實測過哪一支手把到不了 30000，這是放寬、不是對症。）
#define AXIS_NAVIGATION_THRESHOLD 24000

// §UI-SCROLL（2026-10-11）：設定頁（Tab 導覽模式）下，右搖桿上下＝捲動那一頁，LB／RB＝一次捲大半頁。
// 原本手把只能靠「焦點一項一項往下移」讓頁面跟著捲：設定頁有幾十個選項，VR 那一區排在左欄最底下，
// 用頭盔的控制器很難走到。這裡只算出要捲多少（單位＝一個可見高度）、發 scrollRequested，由那一頁自己捲。
// 不合成滑鼠滾輪事件：那要看視窗裡滑鼠座標底下是什麼，也要看 Qt 怎麼處理合成事件的時戳，直接捲比較可靠。
// 死區要明顯大於搖桿靜止時可能出現的值（XInput 建議的右搖桿死區是 8689）：接著的任何一支手把只要靜止值超過死區，
// 設定頁就會一直收到捲動要求
#define SCROLL_AXIS_DEADZONE 12000         // 右搖桿超過這個值才開始捲（約 37%）
// 推到底的速度。2026-10-22 使用者：影片模式搖桿捲頁太慢 → 2.0 改 4.0，幅度曲線從平方改 1.5 次方（推七成約 1.5 頁／秒，
// 原本約 0.5），捲動模式／Tab 導覽模式下輪詢也從 100 ms 改 33 ms（一次跳的距離小、看起來是連續捲）
#define SCROLL_MAX_PAGES_PER_SEC 4.0
#define SCROLL_POLL_MS 33
#define SCROLL_BUTTON_PAGES 0.8            // LB／RB 一次捲多少

SdlGamepadKeyNavigation::SdlGamepadKeyNavigation(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_Enabled(false),
      m_UiNavMode(false),
      m_ScrollMode(false),
      m_FirstPoll(false),
      m_HasFocus(false),
      m_LastAxisNavigationEventTime(0),
      m_LastScrollEventTime(0),
      m_PollLogged(false),
      m_InputLogged(false),
      m_ScrollLogged(false)
{
    m_PollingTimer = new QTimer(this);
    connect(m_PollingTimer, &QTimer::timeout, this, &SdlGamepadKeyNavigation::onPollingTimerFired);
}

SdlGamepadKeyNavigation::~SdlGamepadKeyNavigation()
{
    disable();
}

void SdlGamepadKeyNavigation::enable()
{
    if (m_Enabled) {
        return;
    }

    // §HID-PROBE：有 HID 裝置卡死時，下面的 SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)
    // 會在 hidapi 列舉（HidD_GetProductString）永久阻塞主執行緒。GUI 路徑在這之前已先
    // 經過 getUnmappedGamepads；CLI stream/quit 路徑（runConfigChecks=false）則最先到這裡。
    // 每次從 UI 回來重新啟用前先 refresh（正常機器 ~ms）：涵蓋「串流中才卡死」與
    // 「使用者已重插」兩種狀態改變，重插後不必重啟程式。
    // m_Enabled 維持 false → disable() / updateTimerState() / getConnectedGamepads() 都是安全的 no-op。
    HidProbe::refresh();
    if (HidProbe::anyUnresponsive()) {
        static bool s_Logged = false;
        if (!s_Logged) {
            s_Logged = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-HID] SdlGamepadKeyNavigation::enable skipped: unresponsive HID device(s) %s - "
                        "gamepad UI navigation disabled; unplug and replug the device",
                        HidProbe::describeUnresponsive().toUtf8().constData());
        }
        return;
    }

    // We have to initialize and uninitialize this in enable()/disable()
    // because we need to get out of the way of the Session class. If it
    // doesn't get to reinitialize the GC subsystem, it won't get initial
    // arrival events. Additionally, there's a race condition between
    // our QML objects being destroyed and SDL being deinitialized that
    // this solves too.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
        s_InitFailed = true;   // §HID-PROBE
        return;
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Drop all pending gamepad add events. SDL will generate these for us
    // on first init of the GC subsystem. We can't depend on them due to
    // overlapping lifetimes of SdlGamepadKeyNavigation instances, so we
    // will attach ourselves.
    //
    // NB: We use SDL_JoystickUpdate() instead of SDL_PumpEvents() because
    // the latter can do a bit more work that we want (like handling video
    // events that we intentionally do not want to process yet).
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);

    // Open all currently attached game controllers
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i);
            if (gc != nullptr) {
                m_Gamepads.append(gc);
            }
        }
    }

    // §UI-SCROLL：介面收不到手把輸入時，先從這幾行確認手把有沒有被看到、是哪幾支、輸入有沒有進來
    // （每次 enable() 各印一行：這一行、開始輪詢、第一個輸入、第一次捲動）
    {
        QStringList names;
        for (auto gc : std::as_const(m_Gamepads)) {
            const char* name = SDL_GameControllerName(gc);
            names.append(QString::fromUtf8(name != nullptr ? name : "(unnamed)"));
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-UI-NAV] enabled: %d gamepad(s)%s%s",
                    (int)m_Gamepads.size(), names.isEmpty() ? "" : ": ",
                    names.join(", ").toUtf8().constData());
    }
    m_PollLogged = false;
    m_InputLogged = false;
    m_ScrollLogged = false;

    m_Enabled = true;

    // Start the polling timer if the window is focused
    updateTimerState();
}

void SdlGamepadKeyNavigation::disable()
{
    if (!m_Enabled) {
        return;
    }

    m_Enabled = false;
    updateTimerState();
    Q_ASSERT(!m_PollingTimer->isActive());

    while (!m_Gamepads.isEmpty()) {
        SDL_GameControllerClose(m_Gamepads[0]);
        m_Gamepads.removeAt(0);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void SdlGamepadKeyNavigation::notifyWindowFocus(bool hasFocus)
{
    m_HasFocus = hasFocus;
    updateTimerState();
}

void SdlGamepadKeyNavigation::onPollingTimerFired()
{
    SDL_Event event;

    // Update joystick state without pumping other events (see enable() comment)
    SDL_JoystickUpdate();

    // Discard any pending button events on the first poll to avoid picking up
    // stale input data from the stream session (like the quit combo).
    if (m_FirstPoll) {
        SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
        SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
        m_FirstPoll = false;
    }

    // Peep events rather than polling to avoid calling SDL_PumpEvents()
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1) {
        switch (event.type) {
        case SDL_QUIT:
            // SDL may send us a quit event since we initialize
            // the video subsystem on startup. If we get one,
            // forward it on for Qt to take care of.
            QCoreApplication::instance()->quit();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            QEvent::Type type =
                    event.type == SDL_CONTROLLERBUTTONDOWN ?
                        QEvent::Type::KeyPress : QEvent::Type::KeyRelease;

            if (!m_InputLogged) {
                m_InputLogged = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-UI-NAV] first gamepad input: button %d (uiNav=%d)",
                            (int)event.cbutton.button, m_UiNavMode ? 1 : 0);
            }

            // Swap face buttons if needed
            if (m_Prefs->swapFaceButtons) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_X;
                    break;
                }
            }

            switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (m_UiNavMode) {
                    // Back-tab
                    sendKey(type, Qt::Key_Tab, Qt::ShiftModifier);
                }
                else {
                    sendKey(type, Qt::Key_Up);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (m_UiNavMode) {
                    sendKey(type, Qt::Key_Tab);
                }
                else {
                    sendKey(type, Qt::Key_Down);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                sendKey(type, Qt::Key_Left);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                sendKey(type, Qt::Key_Right);
                break;
            case SDL_CONTROLLER_BUTTON_A:
                if (m_UiNavMode) {
                    sendKey(type, Qt::Key_Space);
                }
                else {
                    sendKey(type, Qt::Key_Return);
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                sendKey(type, Qt::Key_Escape);
                break;
            case SDL_CONTROLLER_BUTTON_X:
                sendKey(type, Qt::Key_Menu);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
            case SDL_CONTROLLER_BUTTON_START:
                // HACK: We use this keycode to inform main.qml
                // to show the settings when Key_Menu is handled
                // by the control in focus.
                sendKey(type, Qt::Key_Hangup);
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                // §UI-SCROLL：LB 往上、RB 往下捲大半頁（只在按下那一刻、只在 Tab 導覽模式）
                if ((m_UiNavMode || m_ScrollMode) && type == QEvent::Type::KeyPress) {
                    emit scrollRequested(event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ?
                                             SCROLL_BUTTON_PAGES : -SCROLL_BUTTON_PAGES);
                }
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
            SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which);
            if (gc != nullptr) {
                // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
                // gamepad in rare cases, because SDL doesn't fixup the device index in
                // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
                // before we've processed the add event.
                if (!m_Gamepads.contains(gc)) {
                    m_Gamepads.append(gc);
                    const char* name = SDL_GameControllerName(gc);
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-UI-NAV] gamepad added: %s (now %d)",
                                name != nullptr ? name : "(unnamed)", (int)m_Gamepads.size());
                }
                else {
                    // We already have this game controller open
                    SDL_GameControllerClose(gc);
                }
            }
            break;
        }
    }

    // §UI-SCROLL：右搖桿的捲動每次輪詢只算一次——取所有手把裡推得最深的那一支。逐支各算各發的話，
    // 第二支會因為共用上一次的時刻而算出 0，還會多發一個方向不明的 0。
    int scrollAxis = 0;

    // Handle analog sticks by polling
    for (auto gc : std::as_const(m_Gamepads)) {
        short leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        short leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (m_ScrollMode) {
            // 捲動模式：左搖桿的上下也是捲動（下面和右搖桿一起取推得最深的）；左右照舊送方向鍵
            if (qAbs((int) leftY) > qAbs(scrollAxis)) {
                scrollAxis = leftY;
            }
            leftY = 0;
        }
        if (SDL_GetTicks() - m_LastAxisNavigationEventTime < AXIS_NAVIGATION_REPEAT_DELAY) {
            // Do nothing
        }
        else if (leftY < -AXIS_NAVIGATION_THRESHOLD) {
            if (m_UiNavMode) {
                // Back-tab
                sendKey(QEvent::Type::KeyPress, Qt::Key_Tab, Qt::ShiftModifier);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Tab, Qt::ShiftModifier);
            }
            else {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Up);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Up);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftY > AXIS_NAVIGATION_THRESHOLD) {
            if (m_UiNavMode) {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Tab);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Tab);
            }
            else {
                sendKey(QEvent::Type::KeyPress, Qt::Key_Down);
                sendKey(QEvent::Type::KeyRelease, Qt::Key_Down);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX < -AXIS_NAVIGATION_THRESHOLD) {
            sendKey(QEvent::Type::KeyPress, Qt::Key_Left);
            sendKey(QEvent::Type::KeyRelease, Qt::Key_Left);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX > AXIS_NAVIGATION_THRESHOLD) {
            sendKey(QEvent::Type::KeyPress, Qt::Key_Right);
            sendKey(QEvent::Type::KeyRelease, Qt::Key_Right);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }

        const int rightY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
        if (qAbs(rightY) > qAbs(scrollAxis)) {
            scrollAxis = rightY;
        }
    }

    // §UI-SCROLL：右搖桿上下捲動（只在 Tab 導覽模式）。SDL 的搖桿往下是正值＝往下捲。
    const int scrollMag = qAbs(scrollAxis);
    if ((m_UiNavMode || m_ScrollMode) && scrollMag > SCROLL_AXIS_DEADZONE) {
        // 距離用實際經過的時間算（輪詢名義上 SCROLL_POLL_MS，計時器不準）；剛開始推的那一次算一個輪詢間隔
        const Uint32 now = SDL_GetTicks();
        const Uint32 sinceLast = now - m_LastScrollEventTime;
        const double dt = (sinceLast > 250 ? SCROLL_POLL_MS : sinceLast) / 1000.0;
        const double f = qMin(1.0, (scrollMag - SCROLL_AXIS_DEADZONE) / (32767.0 - SCROLL_AXIS_DEADZONE));
        const double pages = SCROLL_MAX_PAGES_PER_SEC * f * qSqrt(f) * dt;
        m_LastScrollEventTime = now;
        if (s_ExternalScrollTicks != 0 && now - s_ExternalScrollTicks < EXTERNAL_SCROLL_HOLD_MS) {
            return;  // §VR-SHELL-SCROLL：頭盔 VR 介面剛用 XR 搖桿捲過
        }

        if (!m_ScrollLogged) {
            m_ScrollLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UI-NAV] right stick scroll in use (axis=%d)", scrollAxis);
        }

        if (pages > 0.0) {
            emit scrollRequested(scrollAxis > 0 ? pages : -pages);
        }
    }
}

void SdlGamepadKeyNavigation::sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    QGuiApplication* app = static_cast<QGuiApplication*>(QGuiApplication::instance());
    QWindow* focusWindow = app->focusWindow();
    if (focusWindow != nullptr) {
        QKeyEvent keyPressEvent(type, key, modifiers);
        app->sendEvent(focusWindow, &keyPressEvent);
    }
}

void SdlGamepadKeyNavigation::updateTimerState()
{
    if (m_PollingTimer->isActive() && (!m_HasFocus || !m_Enabled)) {
        m_PollingTimer->stop();
    }
    else if (!m_PollingTimer->isActive() && m_HasFocus && m_Enabled) {
        // Flush events on the first poll
        m_FirstPoll = true;

        if (!m_PollLogged) {
            m_PollLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-UI-NAV] polling started (window has focus)");
        }

        // Poll every 100 ms for a new joystick event.
        // VIPLE §perf — was 50 ms (20 Hz).  UI navigation only needs ~10 Hz to
        // feel responsive; halving the poll rate cuts SDL_GameControllerUpdate
        // calls without observable latency to a human pressing menu buttons.
        // Streaming-time gamepad input goes through a separate higher-rate path.
        m_PollingTimer->start(pollIntervalMs());
    }
}

int SdlGamepadKeyNavigation::pollIntervalMs() const
{
    // 捲動要連續，捲動模式／Tab 導覽模式輪詢快一點；其他時候維持 100 ms
    return (m_ScrollMode || m_UiNavMode) ? SCROLL_POLL_MS : 100;
}

void SdlGamepadKeyNavigation::setUiNavMode(bool uiNavMode)
{
    m_UiNavMode = uiNavMode;
    m_PollingTimer->setInterval(pollIntervalMs());
}

void SdlGamepadKeyNavigation::setScrollMode(bool scrollMode)
{
    if (scrollMode != m_ScrollMode) {
        if (scrollMode) {
            qGuiApp->installEventFilter(this);
            m_DiagWindowStart = SDL_GetTicks();
            m_DiagWheel = m_DiagKey = m_DiagButton = 0;
            m_DiagWheelLogged = m_DiagKeyLogged = false;
        }
        else {
            qGuiApp->removeEventFilter(this);
        }
    }
    m_ScrollMode = scrollMode;
    m_PollingTimer->setInterval(pollIntervalMs());
    m_ScrollLogged = false;  // 每次開啟後第一次捲動各印一行
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-UI-NAV] scroll mode %s (%d gamepad(s))", scrollMode ? "on" : "off",
                (int) m_Gamepads.size());
}

bool SdlGamepadKeyNavigation::eventFilter(QObject* obj, QEvent* event)
{
    // 只看送到視窗本身的（QQuickWindow 會再轉送給項目，那些不重複算）
    if (obj->isWindowType()) {
        switch (event->type()) {
        case QEvent::Wheel: {
            m_DiagWheel++;
            if (!m_DiagWheelLogged) {
                m_DiagWheelLogged = true;
                auto* we = static_cast<QWheelEvent*>(event);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-UI-NAV] first wheel event: angle=(%d,%d) pixel=(%d,%d) phase=%d at (%.0f,%.0f)",
                            we->angleDelta().x(), we->angleDelta().y(), we->pixelDelta().x(), we->pixelDelta().y(),
                            (int) we->phase(), we->position().x(), we->position().y());
            }
            break;
        }
        case QEvent::KeyPress: {
            m_DiagKey++;
            if (!m_DiagKeyLogged) {
                m_DiagKeyLogged = true;
                // 只記第一個鍵的代碼（選片頁沒有密碼欄位；網址列打的字不記）
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-UI-NAV] first key event: key=0x%x", static_cast<QKeyEvent*>(event)->key());
            }
            break;
        }
        case QEvent::MouseButtonPress:
            m_DiagButton++;
            break;
        default:
            break;
        }
        const Uint32 now = SDL_GetTicks();
        if (now - m_DiagWindowStart >= 10000) {
            if (m_DiagWheel + m_DiagKey + m_DiagButton > 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-UI-NAV] 10s input: wheel=%d keys=%d clicks=%d", m_DiagWheel, m_DiagKey,
                            m_DiagButton);
            }
            m_DiagWindowStart = now;
            m_DiagWheel = m_DiagKey = m_DiagButton = 0;
        }
    }
    return QObject::eventFilter(obj, event);
}

int SdlGamepadKeyNavigation::getConnectedGamepads()
{
    // §HID-PROBE：enable() 被閘控（或 SDL init 失敗）時 m_Enabled 仍為 false、joystick 子系統
    // 沒開，直接回 0（AppView/PcView/SettingsView 的 StackView.onActivated 都會呼叫）。
    // 正常路徑維持原本的 Q_ASSERT 語意：未 enable 就呼叫仍是 bug。
    if (!m_Enabled) {
        Q_ASSERT(HidProbe::anyUnresponsive() || s_InitFailed);
        return 0;
    }

    int count = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            count++;
        }
    }

    return count;
}
