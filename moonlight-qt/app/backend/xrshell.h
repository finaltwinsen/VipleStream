#pragma once

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <mutex>

class QOffscreenSurface;
class QOpenGLContext;
class QQmlEngine;
class QQuickItem;
class QQuickRenderControl;
class QQuickWindow;
class XrContext;
namespace player {
class PlayerSession;
}

// VipleStream 2.0 §VR-SHELL（2026-10-20）：頭盔上的全 VR 介面。
//
// 原本在 Steam Frame 上主畫面是平面視窗，SteamVR 把它當「平面遊戲」放在劇院畫面裡；開始播影片或 VR 串流時要換成另一個
// XR 程式，SteamVR 每次都跳出 Steam 介面、黑一下（使用者 2026-10-20：「速度慢體感差」）。VR 介面讓程式從一開始就是 XR 程式：
//   - 同一份 QML（gui/VrShell.qml 外殼＋AppContent.qml 內容）用 QQuickRenderControl 畫在離屏的 QQuickWindow（OpenGL），
//     讀回成圖片交給 XrContext 的介面面板（Options.shellWidth／Height，位置與大小同影像螢幕）；不開任何平面視窗。
//   - 控制器射線（XrInput 的一般射線滑鼠）轉成 Qt 的滑鼠事件：指到＝移動（hover）、扳機＝左鍵、握把＝右鍵、搖桿上下＝捲動；
//     B＝Esc（返回上一頁／關對話框）；虛擬鍵盤的按鍵轉成 Qt 按鍵事件；文字欄位拿到焦點時自動開鍵盤。
//   - XR session 結束（使用者從 SteamVR 離開）＝程式結束。
//
// 階段：A1 介面本身；A2 影片模式改在這個 session 裡播（VrLauncher::setPlayerHandler；載入時面板還是介面，第一張影像
// 要顯示時才換成影片，播完換回來；這段期間控制器輸入轉給播放器）。
// VR 串流仍是子行程（那條路的延遲調校都在 Session／XrContext 的 PCVR 模式裡），用「換手」避免中間沒有 XR 程式：
//   介面不先關 session；子行程的 session 一起來，SteamVR 就把介面的結束掉（`transition_app_quitting`，順便收起儀表板）；
//   子行程結束前印 HANDBACK 並等最多 6 秒（XrContext::setHandBackOnShutdown），介面先開回 session，SteamVR 再把子行程的
//   結束掉。中間如果沒有任何 XR 程式，SteamVR 會打開儀表板（`NoMoreSceneAppTransition`）——就是使用者說的「跳出介面」。
// 桌面模式的串流在同一個程式裡、用 Session 自己的 XrContext：開始前先關掉介面的 session、結束後再開回來
// （Session::setXrOwnerHook；同一個程式不能同時有兩個 session）。A3 再看要不要搬進這個 session。
//
// Frame 上一般啟動預設就是這個介面（VrLauncher::vrShellByDefault()）；XR 帶不起來（SteamVR 沒在跑等）就退回平面視窗。
// --vr-shell "<xr|noxr|off>[,dump=<秒>:<png>][,test=<腳本>]"：off＝強制平面視窗；noxr＝不帶起 XR，只離屏畫（開發機驗介面
// 與事件）；腳本見 runTest()。log：[VIPLE-SHELL]。
class XrShell : public QObject
{
    Q_OBJECT

public:
    struct Config {
        bool off = false;       // --vr-shell off：用平面視窗
        bool noXr = false;
        int dumpSec = 0;        // >0：這麼多秒後存一張 PNG
        QString dumpPath;
        QString testScript;     // "秒:指令[:參數]…;…"
        // dev（childplayer）：影片模式也用子行程（驗換手；一般在這個 session 裡播）
        bool childPlayer = false;
    };

    // dev 旗標的字串（見上）；格式不對回 false
    static bool parseSpec(const QString& spec, Config* out, QString* error);

    XrShell(QQmlEngine* engine, const Config& config, QObject* parent = nullptr);
    ~XrShell() override;

    // 建離屏畫面、載入 VrShell.qml、帶起 XR。失敗回 false（*error 是英文說明），呼叫端結束程式
    bool start(QString* error);

    // 程式要結束了（app.exec() 返回之後）：停掉播放器與鉤子，但 XR session 留著
    void prepareExit();
    // 最後一步：先送幾百毫秒的空畫面（XrContext::setBlank）再關 XR session，之後呼叫端走正常收尾（main.cpp）。
    // 當初（2026-10-20）是為了 Steam 的 steamwebhelper 在我們結束時當掉重啟而加的；2026-11-02 對照實驗確定那是
    // Steam 的 bug（任何遊戲結束都會，steam-for-linux #13690），跟我們怎麼結束無關。空畫面留著：關 session 前不會停在最後一張
    void shutdownXrForExit();

    // §PERF-1113：XR session 的狀態（GUI 執行緒呼叫）。rendered＝runtime 要我們畫（VISIBLE／FOCUSED），focused＝FOCUSED。
    // 目前沒有人呼叫（XrContext 還沒有把 session 狀態往外傳的回呼），兩個都維持預設的 true＝行為同以往。
    // 接上之後：rendered=false 時介面不重畫（回來時補畫一張）；focused 轉給 VrLauncher.shellFocused，
    // VrShell.qml 在不是 FOCUSED 超過一段時間後停主機輪詢
    void setXrSessionState(bool rendered, bool focused);

    // 面板的像素尺寸
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 800;

private:
    void scheduleRender(bool sync);
    void renderNow();
    // §PERF-1113：真的畫一張＋讀回＋交給 XR（不看閘控；renderNow() 與補畫用）。沒畫成回 false
    bool renderFrame();
    // §PERF-1113：介面的畫面現在有沒有人看得到／影片是否已經蓋住介面（見 .cpp 的 scheduleRender）
    bool renderBlocked() const;
    bool videoCoversShell() const;
    bool bringUpXr(QString* error);
    void shutdownXr();
    // 桌面模式的串流要用 XR：wantsXr＝true 先關掉自己的 session，false 再開回來
    void lendXr(bool wantsXr);
    // VR 串流子行程的換手（見 .cpp）
    void onChild(int event);
    // XR session 被 runtime 結束（GUI 執行緒）
    void onXrEnded(int reason);
    bool reopenXr(const char* why);

    // 射線事件（GUI 執行緒）
    void pointerMoved(float u, float v);
    void mouseButton(Qt::MouseButton button, bool pressed);
    void wheel(int amount);
    void key(int vk, bool down, uint8_t mods);
    void sendKeyPress(int qtKey, const QString& text, Qt::KeyboardModifiers mods, bool down);
    void back();
    void focusChanged();
    bool startPlayer(const QString& source, const QString& format, const QString& title, const QString& audioSource);

    void runTest();

    QQmlEngine* m_Engine;
    Config m_Config;
    QOpenGLContext* m_Gl = nullptr;
    QOffscreenSurface* m_Surface = nullptr;
    QQuickRenderControl* m_Rc = nullptr;
    QQuickWindow* m_Win = nullptr;
    QQuickItem* m_Root = nullptr;
    unsigned int m_Tex = 0;   // GL 貼圖（Qt Quick 畫進去）
    unsigned int m_Fbo = 0;   // 讀回用
    QTimer m_RenderTimer;
    bool m_NeedSync = true;
    QImage m_Last;            // 最後一張（dump 用）
    uint64_t m_Frames = 0;
    uint64_t m_RenderNs = 0;  // 10 秒視窗：畫＋讀回的時間
    uint64_t m_WinStartNs = 0;
    uint64_t m_LastRenderNs = 0;
    // §PERF-1113：沒有 XR session（交給串流）期間有畫面變更被略過；session 回來時要先補畫一張再顯示
    bool m_SkippedWhileHidden = false;
    // §PERF-1113：見 setXrSessionState()；沒有人設＝true
    bool m_XrSessionRendered = true;

    XrContext* m_Xr = nullptr;
    bool m_XrSuspended = false;   // 自己的 XR session 關著（交給子行程或桌面模式的串流）
    bool m_ChildRunning = false;  // VR 串流子行程在跑（runtime 結束我們的 session＝它接手了，不是使用者離開）
    QPointF m_Pointer{-1, -1};
    Qt::MouseButtons m_Buttons = Qt::NoButton;
    // 按下扳機時手會晃：按住期間離按下的位置不到 kClickSlopPx 的移動不送出，放開才是乾淨的點擊（清單在 Flickable 裡，
    // 晃超過 Qt 的拖動門檻就變成拖動、點不進去）；超過一次之後照常送（拖動、捲動）
    QPointF m_PressPos{-1, -1};
    bool m_PressSlopExceeded = false;
    bool m_TextFocus = false;
    QPointer<QQuickItem> m_LastScrollTarget;  // §VR-SHELL-SCROLL：只給 log（換了捲動對象才印）

    // §VR-SHELL（A2）：在這個 session 裡播的那一場。m_Route 是 XR frame thread 轉輸入用的（m_RouteMutex）
    player::PlayerSession* m_Player = nullptr;
    // §REVIEW-1104：載入中按 B 取消時，結束碼不要顯示成播放失敗
    bool m_PlayerCancel = false;
    std::mutex m_RouteMutex;
    player::PlayerSession* m_Route = nullptr;

    QTimer m_TestTimer;
    qint64 m_StartMs = 0;
    int m_TestStep = 0;
};
