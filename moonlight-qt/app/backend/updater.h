#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include "updateassetrules.h"

// VipleStream 自動更新器 — 從 GitHub Releases 抓 asset、下載、寫 helper
// script 由外部 process 接手 swap + restart。流程：
//   1. queryAssetList()  → GitHub /releases/latest，挑符合平台的 asset
//   2. downloadAsset()    → 串流寫入 staging 檔，progressChanged 推進
//   3. spawn<Platform>Helper() → PS1 (Win) / sh (Linux) 等本 PID 退出後
//      解壓 / 覆蓋 + 啟動新版
//   4. emit readyToRestart() → QML 端呼叫 Qt.quit()
//
// Windows 寫不到 install dir（Program Files）時自動 elevate 用 UAC；
// portable 解壓位置自動跳過 elevation。
class Updater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY progressChanged)
    Q_PROPERTY(qint64 bytesTotal    READ bytesTotal    NOTIFY progressChanged)
    Q_PROPERTY(QString status       READ status        NOTIFY statusChanged)
    // §F9：本平台能不能自動下載安裝（Windows x64、從 AppImage 執行的 Linux x64）。
    // false 時 UpdateDialog 只通知新版＋顯示 manualUpdateHint，不下載。
    Q_PROPERTY(bool autoInstallSupported READ autoInstallSupported CONSTANT)
    Q_PROPERTY(QString manualUpdateHint  READ manualUpdateHint     CONSTANT)

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater();

    // 啟動完整流程。version＝UI 顯示的新版號，只當初值與 log 用；§F9：實際
    // 安裝的版號以 GET /releases/latest 抓到的 tag_name 為準（兩者可能不同），
    // 抓到後會覆寫 m_Version，helper 腳本檔名跟著用。
    Q_INVOKABLE void startUpdate(const QString& version);

    // 任何時間呼叫都安全 — 進度未過 readyToRestart 之前都能取消。
    Q_INVOKABLE void cancel();

    qint64 bytesReceived() const { return m_BytesReceived; }
    qint64 bytesTotal()    const { return m_BytesTotal; }
    QString status()       const { return m_Status; }
    bool autoInstallSupported() const;
    QString manualUpdateHint() const;

signals:
    void progressChanged();
    void statusChanged();
    void updateFailed(QString errorMessage);
    void readyToRestart();

private slots:
    void handleAssetListFinished();
    void handleDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void handleDownloadFinished();

private:
    void setStatus(const QString& s);
    void fail(const QString& errorMessage);
    // §F9：把 /releases/latest 的 JSON 交給 UpdateAssetRules::planRelease 決策
    // （版號取自 JSON 的 tag_name），並記 [VIPLE-UPDATE] log
    UpdateAssetRules::ReleasePlan selectAssetForPlatform(const QJsonObject& release);
    bool spawnHelperAndQuit();

    QNetworkAccessManager* m_Nam;
    QNetworkReply*         m_CurrentReply;
    QFile*                 m_DownloadFile;
    QString                m_Version;
    QString                m_AssetUrl;
    QString                m_AssetName;
    QString                m_DownloadPath;
    qint64                 m_BytesReceived;
    qint64                 m_BytesTotal;
    QString                m_Status;
    bool                   m_Cancelled;
    UpdateAssetRules::Platform m_Platform;  // §F9：建構時偵測一次
};
