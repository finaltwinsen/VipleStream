#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

#include <functional>

class VrPreflightCheck;

// VipleStream 2.0 §VR-LAUNCHER（2026-10-04）：在 Steam Frame 上從收藏庫開 VipleStream 時，可以選「桌面模式」
// 或「VR 模式」。VR 模式選主機後列出 host 的 VR app（/applist?vr=1、只列 `<IsVr>`），選好後 GUI 隱藏自己，另開
//   viplestream stream --display-target pcvr [--takeover] -- <host 位址> <app>
// 子行程跑 VR 串流（走 CLI 實測過的無頭路徑：§VR-HEADLESS 不開平面視窗，Frame 不會疊上平面遊戲介面），
// 子行程結束後 GUI 再出現。子行程只帶這幾個參數：CLI 覆寫會在 session 開始時被寫回偏好設定，
// 不能讓 VR 的參數污染桌面模式的設定（PCVR 不用 MP-QUIC 改在 session.cpp 執行期處理）。
//
// 只在 Linux＋OpenXR 建置、跑在 gamescope 裡（Frame）時提供。Flatpak 沙盒若有 PID 隔離（bwrap 是
// PID 1），OpenXR 連 Frame 的 SteamVR 會出事（曾讓 SteamVR 重啟），VR 選項停用並說明原因。
//
// §VR-PREFLIGHT（2026-10-11）：launch() 不直接開子行程，先量到主機的往返時間（規則與常數在 vrpreflight.h）。
// 夠快或量不到→照原本的方式開；太慢→不開，preflightWaiting 變成 true，gui/VrPreflightDialog.qml 顯示量到的值
// 並讓使用者選「仍要開始」或取消；這段期間繼續量，連續夠快就自己開。檢查還沒有結果時 launch() 不接第二次。
class VrLauncher : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString blockedReason READ blockedReason CONSTANT)
    Q_PROPERTY(bool vrMode READ vrMode NOTIFY vrModeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    // 串流子行程在跑（VR 串流或 XR 桌面；不含影片模式）
    Q_PROPERTY(bool streamRunning READ streamRunning NOTIFY runningChanged)
    // §VR-SHELL-LAUNCHING：按下 VR／桌面串流之後、子行程的 XR session 接手頭盔之前（含往返時間檢查）。
    // 頭盔 VR 介面用它馬上蓋上「啟動中」，不然要等好幾秒才知道有沒有按到，還會重複點
    Q_PROPERTY(bool launching READ launching NOTIFY launchingChanged)
    Q_PROPERTY(QString launchingApp READ launchingApp NOTIFY launchingChanged)
    // §VR-PLAYER：這個建置有影片模式，而且在 Frame 上（available）
    Q_PROPERTY(bool playerAvailable READ playerAvailable CONSTANT)
    // §VR-SHELL：頭盔 VR 介面在跑（桌面模式也改開 XR 桌面子行程，不在這個程式裡開平面視窗串流）
    // 不能是 CONSTANT：XrShell 先建 QML、之後才 setChildHook()，CONSTANT 的綁定在載入時讀到 false 就再也不更新
    // （2026-10-22 頭盔實測：「正在啟動」遮罩因此從來沒出現）
    Q_PROPERTY(bool shellActive READ shellActive NOTIFY shellActiveChanged)
    // §VR-PLAYER：這個建置有內建瀏覽器（Qt WebEngine）
    Q_PROPERTY(bool browserAvailable READ browserAvailable CONSTANT)
    // §VR-PLAYER（dev，--dev-video）：見 commandlineparser.h。devVideoTab＝-1＝沒帶
    Q_PROPERTY(int devVideoTab READ devVideoTab CONSTANT)
    Q_PROPERTY(QString devVideoUrl READ devVideoUrl CONSTANT)
    Q_PROPERTY(int devVideoGrabSec READ devVideoGrabSec CONSTANT)
    Q_PROPERTY(QString devVideoGrabPath READ devVideoGrabPath CONSTANT)
    Q_PROPERTY(int devVideoPlaySec READ devVideoPlaySec CONSTANT)
    Q_PROPERTY(QString devVideoHost READ devVideoHost CONSTANT)
    Q_PROPERTY(QString devVideoDir READ devVideoDir CONSTANT)
    // dev（--dev-video hide=…）：VR 子行程啟動時主視窗怎麼讓開（now＝馬上藏、focused＝子行程的 XR 拿到焦點才藏、
    // never＝不藏、minimize＝最小化）。空＝now
    Q_PROPERTY(QString devLaunchHide READ devLaunchHide CONSTANT)

    // §VR-PREFLIGHT：給 VrPreflightDialog.qml 顯示用
    Q_PROPERTY(bool preflightChecking READ preflightChecking NOTIFY preflightChanged)   // 開頭那幾次還在量（通常不到半秒）
    Q_PROPERTY(bool preflightWaiting READ preflightWaiting NOTIFY preflightChanged)     // 太慢：等使用者決定，同時繼續量
    Q_PROPERTY(int preflightMeasuredMs READ preflightMeasuredMs NOTIFY preflightChanged)   // 判定太慢時的往返時間（ms）；沒有＝-1
    Q_PROPERTY(int preflightRttMs READ preflightRttMs NOTIFY preflightChanged)             // 最近一次量到的（ms）；量不到或還沒量＝-1
    Q_PROPERTY(int preflightGoodStreak READ preflightGoodStreak NOTIFY preflightChanged)   // 目前連續幾次夠快
    Q_PROPERTY(int preflightThresholdMs READ preflightThresholdMs CONSTANT)
    Q_PROPERTY(int preflightGoodNeeded READ preflightGoodNeeded CONSTANT)                  // 連續幾次夠快就自己開
    Q_PROPERTY(int preflightIntervalSec READ preflightIntervalSec CONSTANT)                // 等待時多久量一次

public:
    explicit VrLauncher(QObject* parent = nullptr);
    ~VrLauncher() override;

    // 這個環境可以提供模式選擇（Frame 上）
    bool available() const;
    bool streamRunning() const;
    bool launching() const { return m_Launching; }
    QString launchingApp() const { return m_LaunchingApp; }
    // §VR-SHELL：一般啟動時預設開頭盔 VR 介面（Frame 上、VR 沒有被 PID 沙盒停用）。main() 在建立 QML engine 之前呼叫
    static bool vrShellByDefault();

    // 非空＝VR 模式不能用（原因給使用者看）
    QString blockedReason() const;

    bool vrMode() const { return m_VrMode; }
    bool running() const;
    bool playerAvailable() const;
    bool shellActive() const;
    bool browserAvailable() const
    {
#ifdef HAVE_WEBENGINE
        return true;
#else
        return false;
#endif
    }

    bool preflightChecking() const;
    bool preflightWaiting() const;
    int preflightMeasuredMs() const;
    int preflightRttMs() const;
    int preflightGoodStreak() const;
    int preflightThresholdMs() const;
    int preflightGoodNeeded() const;
    int preflightIntervalSec() const;

    // dev（--vr-test-preflight）：往返時間檢查改用腳本給結果（格式見 vrpreflight.h 的 TestScript），不寫進設定。
    // 格式不對回 false、*error 是英文說明。main() 在建立 QML engine 之前呼叫
    static bool setTestPreflightScript(const QString& spec, QString* error = nullptr);
    // dev（--dev-video）：main() 在建立 QML engine 之前呼叫
    static void setDevVideo(const QString& spec);
    // §VR-SHELL：子行程的事件給頭盔 VR 介面。Starting＝開之前（同步）；HandBack＝子行程要結束 XR 了（stderr 的
    // "VIPLE-XR-STATE HANDBACK"，介面該開回它的 session）；Ended＝子行程結束了。設了鉤子時子行程帶 --xr-handback。
    // nullptr＝取消
    enum class ChildEvent { Starting, HandBack, Ended };
    static void setChildHook(std::function<void(ChildEvent)> hook);
    // §VR-SHELL：子行程在跑、或剛結束不到 3 秒（任何執行緒都可呼叫；main.cpp 的訊號處理用）。SteamVR 快速選單的
    // 「結束」會把 SIGINT 同時送給 VR 介面與子行程（頭盔 10-21：介面跟著結束、回到 SteamVR），這段期間介面不理會
    static bool childGuardActive();
    // §VR-SHELL（A2）：VR 介面在跑時，影片模式不開子行程，改交給這個 handler 在 VR 介面的 XR session 裡播。
    // handler 回 false＝沒接受；接受之後播完由 VR 介面呼叫 playerFinished()。nullptr＝取消（回到子行程）
    struct PlayRequest {
        QString source, format, title, audioSource;
    };
    static void setPlayerHandler(std::function<bool(const PlayRequest&)> handler);
    // §VR-SHELL：VR 介面裡的那一場播完（GUI 執行緒）。exitCode 同 viplestream play 的結束碼，error 是播放器給的說明
    static void playerFinished(int exitCode, const QString& error);
    int devVideoTab() const;
    QString devVideoUrl() const;
    int devVideoGrabSec() const;
    QString devVideoGrabPath() const;
    int devVideoPlaySec() const;
    QString devVideoHost() const;
    QString devVideoDir() const;
    QString devLaunchHide() const;

    // 切換模式；回傳模式是否真的改變（改變時 QML 再請 ComputerManager.setVrAppLists() 重抓清單）
    Q_INVOKABLE bool setVrMode(bool on);

    // 開 VR 串流：先做往返時間檢查（§VR-PREFLIGHT），通過了才開子行程。hostAddress 是子行程拿來找主機的位址，
    // probeHost／probePort 是量往返時間用的同一組位址與埠（三個都來自 AppModel.vrLaunchTarget()）；
    // takeover＝使用者已在 GUI 確認接管別的裝置的 session。
    // 回傳 false＝沒有接受（不是 VR 模式、串流正在跑、上一次的檢查還沒有結果、參數不完整）
    // §VR-SHELL：VR 介面裡的桌面模式：另開 `stream --display-target xr-desktop --xr-handback` 子行程（XR 虛擬螢幕、
    // 無平面視窗、和 VR 串流一樣換手）。不做往返時間檢查（桌面串流不像 VR 那麼怕延遲）。回傳 false＝沒有接受
    Q_INVOKABLE bool launchDesktop(const QString& hostAddress, const QString& appName, bool takeover);

    Q_INVOKABLE bool launch(const QString& hostAddress, const QString& probeHost, int probePort,
                            const QString& appName, bool takeover);

    // §VR-PLAYER（2026-10-19）：影片模式。另開子行程 `viplestream play [--vr-format F] [--title T] [--audio-url A] -- <來源>`，
    // 不需要主機、不做往返時間檢查、和 VR 模式的開關無關（在桌面模式也能用）。GUI 一樣先隱藏、子行程結束再出現。
    // 回傳 false＝沒有接受（已經有子行程在跑、來源空白、這個環境不能跑 XR）
    Q_INVOKABLE bool launchPlayer(const QString& source, const QString& format, const QString& title, const QString& audioSource);

    // §VR-PREFLIGHT：對話框的兩個按鈕。不在檢查中時不做事
    Q_INVOKABLE void preflightStartAnyway();
    Q_INVOKABLE void preflightCancel();

signals:
    void vrModeChanged();
    void launchingChanged();
    void shellActiveChanged();
    void runningChanged();
    void preflightChanged();
    // 子行程結束；error 非空＝異常結束（給 GUI 顯示）
    void finished(int exitCode, const QString& error);
    // 子行程的 XR session 第一次到 VISIBLE／FOCUSED（stderr 的 "VIPLE-XR-STATE <狀態>"）
    void childXrState(const QString& state);

private:
    void onStderr();
    void processEnded(int exitCode, QProcess::ExitStatus status);
    void preflightPassed();
    bool startProcess(const QString& hostAddress, const QString& appName, bool takeover);
    // 開子行程（VR 串流與影片模式共用）；what 只給 log
    bool startChild(const QStringList& args, const QString& what);
    // 影片模式的結束碼 → 給使用者看的錯誤（空＝正常結束）
    QString playerError(int exitCode, bool crashed) const;
    void inProcessPlayerEnded(int exitCode, const QString& error);
    // §VR-SHELL-LAUNCHING：why 只給 log
    void setLaunching(bool on, const QString& app, const char* why);

    bool m_VrMode = false;
    QProcess* m_Proc = nullptr;
    QByteArray m_StderrPartial;   // 還沒湊成一整行的 stderr
    QString m_LastError;          // 子行程最後一行給使用者看的錯誤（"Stream failed:"／"Stream error:" 等）
    bool m_ChildIsPlayer = false; // 目前（或剛結束）的子行程是影片模式（結束碼的意思不同）
    bool m_InProcessPlayer = false; // §VR-SHELL：影片模式正在 VR 介面的 session 裡播（沒有子行程）
    bool m_Launching = false;       // §VR-SHELL-LAUNCHING
    QString m_LaunchingApp;

    // §VR-PREFLIGHT：檢查通過之後要開的那一場
    VrPreflightCheck* m_Preflight = nullptr;
    QString m_PendingAddress;
    QString m_PendingApp;
    bool m_PendingTakeover = false;
};

// main.cpp 實作（§SINGLE-INST）：Linux 的單一實例鎖。VR 子行程是同一支程式，GUI 開它之前先放鎖、它結束後再
// 拿回，否則子行程一定被自己的 GUI 擋下（"already running"）。拿不回來（別的實例趁隙拿走）回 false。
void singleInstanceReleaseForChild();
bool singleInstanceReacquire();
