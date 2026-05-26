#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QJsonArray>

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

public:
    explicit Updater(QObject *parent = nullptr);
    ~Updater();

    // 啟動完整流程。version 用於命名 staging dir + log。
    Q_INVOKABLE void startUpdate(const QString& version);

    // 任何時間呼叫都安全 — 進度未過 readyToRestart 之前都能取消。
    Q_INVOKABLE void cancel();

    qint64 bytesReceived() const { return m_BytesReceived; }
    qint64 bytesTotal()    const { return m_BytesTotal; }
    QString status()       const { return m_Status; }

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
    bool selectAssetForPlatform(const QJsonArray& assets,
                                QString& outUrl, QString& outName);
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
};
