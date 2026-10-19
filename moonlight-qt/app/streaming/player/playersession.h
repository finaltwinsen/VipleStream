// VipleStream 2.0 §VR-PLAYER — 頭盔影片模式的一場播放：XrContext（β 虛擬螢幕，不經串流 Session）＋MediaPipeline＋控制列。
//
// 流程（全部在自己的工作執行緒，GUI 執行緒只收 finished）：
//   1. 帶起 XR（Options.player：一律 quad、暫停不算 stale、有控制列）；XR 起不來就結束。
//   2. 開影片（網址要連線，這段時間頭盔裡是 loading 螢幕）；判斷畫面格式（playerformat.h）。
//   3. 呈現迴圈：照影音時鐘與 XR 的顯示格點（playersync.h）把每一張解好的幀交給 XrVideo；處理控制。
//   4. 播完、按「離開」、XR session 結束（使用者從 SteamVR 選單離開）、或發生錯誤就收尾：記下播到哪裡、關 XR。
//
// 控制：控制器的選單鍵切換控制列；扳機點螢幕也是；控制列上的按鈕與時間軸用射線點；搖桿左右＝後退／前進 10 秒
// （按住每 0.5 秒再跳一次）、上下＝音量。播放中 5 秒沒動控制列就自動收起，暫停時一直顯示。
//
// log：[VIPLE-PLAYER]（開檔、格式、每 10 秒一行：位置、交出／丟掉／晚了、每張佔幾格的分布、時鐘誤差、緩衝、解碼）。

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "mediapipeline.h"
#include "playerformat.h"
#include "playerpanel.h"
#include "playersync.h"

class XrContext;

namespace player {

class PlayerSession : public QObject
{
    Q_OBJECT

public:
    struct Config {
        QString source;            // 本機路徑或網址
        QString audioSource;       // 另一條音訊網址（V4：網站的影像、音訊分開）
        QString title;             // 控制列上的名稱（空＝用中繼資料或檔名）
        QString format = QStringLiteral("auto");  // playerfmt 的名稱或 auto
        double startSec = 0.0;
        float volume = 1.0f;
        int screenPreset = 1;      // 0 小、1 中、2 大
        bool hwDecode = true;
        // dev
        int durationSec = 0;       // >0：播這麼多秒就結束（無人驗測）
        bool noXr = false;         // 不帶起 XR：用假的 120 Hz 格點跑整條管線（開發機／builder 驗測）
        QString testScript;        // "秒:指令,…"（見 .cpp 的 runTestScript）
        QString dumpPanelPath;     // 第一次畫控制列時存一張 PNG
        QString dumpFramePath;     // XrContext 的 --xr-dump-frame
        bool testPointer = false;  // XrContext 的合成射線
        int traceFrames = 0;       // >0：前 N 張交出的幀各印一行（時鐘、格點、鎖相）
        QString userAgent;
        QString httpHeaders;
        QString xrRuntimeJson;     // dev：只影響這個行程的 OpenXR runtime（同 stream 的 --xr-runtime-json）
    };

    explicit PlayerSession(const Config& config, QObject* parent = nullptr);
    ~PlayerSession() override;

    void start();
    // 任何執行緒：要求結束（GUI 被關、收到 SIGTERM）
    void requestStop();

signals:
    // exitCode：0 正常（播完、使用者離開）、2 XR 起不來、3 影片開不了或讀取失敗、4 XR session 中途結束
    void finished(int exitCode, const QString& error);

private:
    enum class Cmd { TogglePause, Seek, SeekRel, Volume, VolumeRel, Format, Screen, Recenter, Exit, TogglePanel, ShowPanel, DepthRel };
    struct Command {
        Cmd cmd;
        double value = 0.0;
    };

    void threadMain();
    int run(QString* error);
    void post(Cmd c, double v = 0.0);
    void handleCommands();
    void onPanelClick(float u, float v);
    void onStick(float x, float y);
    void updatePanel(bool force);
    void applyFormat();
    void applyScreen();
    void logWindow(uint64_t nowNs, bool final);
    void runTestScript(double elapsedSec);

    Config m_Config;
    std::thread m_Thread;
    std::atomic<bool> m_Stop{false};
    XrContext* m_Xr = nullptr;
    MediaPipeline m_Media;

    std::mutex m_CmdMutex;
    std::deque<Command> m_Cmds;

    // 只在播放器執行緒
    playerfmt::Format m_Format;
    playerfmt::Source m_FormatSource = playerfmt::Source::Default;
    bool m_PanelVisible = true;
    uint64_t m_PanelShownNs = 0;     // 最近一次互動（自動收起從這裡算）
    uint64_t m_PanelDrawnNs = 0;
    PlayerPanel::State m_PanelState;
    bool m_PanelDumped = false;
    bool m_Ended = false;
    std::atomic<int> m_Hover{PlayerPanel::None};
    std::atomic<float> m_HoverTimeline{-1.0f};
    std::atomic<uint64_t> m_LastInteractNs{0};
    // 搖桿：上一次觸發的時刻與方向（重複觸發用）
    int m_StickXDir = 0, m_StickYDir = 0;
    // 遠近（搖桿上下；-kDepthMax..kDepthMax，0＝預設）：平面螢幕每格遠 15%（張角不變），180°／360° 每格每眼往外 0.1°。每支片記住
    int m_Depth = 0;
    uint64_t m_StickXNs = 0, m_StickYNs = 0;
    // 測試腳本
    int m_TestStep = 0;
    uint64_t m_StartNs = 0;

    // 10 秒視窗統計
    uint64_t m_WinStartNs = 0;
    uint32_t m_Submitted = 0, m_Dropped = 0, m_Late = 0, m_Previews = 0;
    uint32_t m_SlotHist[8] = {};     // 相鄰兩張相差幾格：1..6、7＝更多、0＝同一格或倒退
    int64_t m_LastSlotAbs = INT64_MIN;  // 上一張交出的那一張排到的格點時刻（steady ns）
    float m_LeadMinMs = 1e9f;        // 交出時距離挑影像還有多久（最小）
    int m_Traced = 0;
};

}  // namespace player
