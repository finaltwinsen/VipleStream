#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

// VipleStream 2.0 §VR-LAUNCHER（2026-10-04）：在 Steam Frame 上從收藏庫開 VipleStream 時，可以選「桌面模式」
// 或「VR 模式」。VR 模式選主機後列出 host 的 VR app（/applist?vr=1、只列 `<IsVr>`），選好後 GUI 隱藏自己，另開
//   viplestream stream --display-target pcvr [--takeover] -- <host 位址> <app>
// 子行程跑 VR 串流（走 CLI 實測過的無頭路徑：§VR-HEADLESS 不開平面視窗，Frame 不會疊上平面遊戲介面），
// 子行程結束後 GUI 再出現。子行程只帶這幾個參數：CLI 覆寫會在 session 開始時被寫回偏好設定，
// 不能讓 VR 的參數污染桌面模式的設定（PCVR 不用 MP-QUIC 改在 session.cpp 執行期處理）。
//
// 只在 Linux＋OpenXR 建置、跑在 gamescope 裡（Frame）時提供。Flatpak 沙盒若有 PID 隔離（bwrap 是
// PID 1），OpenXR 連 Frame 的 SteamVR 會出事（曾讓 SteamVR 重啟），VR 選項停用並說明原因。
class VrLauncher : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString blockedReason READ blockedReason CONSTANT)
    Q_PROPERTY(bool vrMode READ vrMode NOTIFY vrModeChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    explicit VrLauncher(QObject* parent = nullptr);
    ~VrLauncher() override;

    // 這個環境可以提供模式選擇（Frame 上）
    bool available() const;

    // 非空＝VR 模式不能用（原因給使用者看）
    QString blockedReason() const;

    bool vrMode() const { return m_VrMode; }
    bool running() const;

    // 切換模式；回傳模式是否真的改變（改變時 QML 再請 ComputerManager.setVrAppLists() 重抓清單）
    Q_INVOKABLE bool setVrMode(bool on);

    // 以子行程開 VR 串流。hostAddress 用主機目前的位址（AppModel.vrLaunchAddress()）；takeover＝使用者已在
    // GUI 確認接管別的裝置的 session
    Q_INVOKABLE bool launch(const QString& hostAddress, const QString& appName, bool takeover);

signals:
    void vrModeChanged();
    void runningChanged();
    // 子行程結束；error 非空＝異常結束（給 GUI 顯示）
    void finished(int exitCode, const QString& error);

private:
    void onStderr();
    void processEnded(int exitCode, QProcess::ExitStatus status);

    bool m_VrMode = false;
    QProcess* m_Proc = nullptr;
    QByteArray m_StderrPartial;   // 還沒湊成一整行的 stderr
    QString m_LastError;          // 子行程最後一行給使用者看的錯誤（"Stream failed:"／"Stream error:" 等）
};

// main.cpp 實作（§SINGLE-INST）：Linux 的單一實例鎖。VR 子行程是同一支程式，GUI 開它之前先放鎖、它結束後再
// 拿回，否則子行程一定被自己的 GUI 擋下（"already running"）。拿不回來（別的實例趁隙拿走）回 false。
void singleInstanceReleaseForChild();
bool singleInstanceReacquire();
