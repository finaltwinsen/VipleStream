/**
 * @file streaming/transfer/filetransferclient.h
 * @brief In-stream file transfer client（對應 server side VipleStream §N）。
 *
 * Lifecycle = streaming session 同步。Session 啟動後 new、teardown 前 stop()。
 * 內部以 QNetworkAccessManager 對 server `/transfer/*` HTTPS endpoints 做：
 *   1. 每 2 秒 GET /transfer/poll 抓命令
 *   2. 依命令類型走 LIST_DIR / DOWNLOAD / UPLOAD / CANCEL 流程
 *   3. 進度透過 IFFmpegRenderer-backed OSD overlay 顯示
 *
 * 平台差異：使用 QStandardPaths::DownloadLocation；Windows + Linux 都 OK。
 * 不動 moonlight-common-c，所有通訊都走 HTTPS sidechannel。
 */
#pragma once

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslError>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>

class FileTransferClient : public QObject {
    Q_OBJECT

public:
    explicit FileTransferClient(const QString &hostAddress,
                                quint16 httpsPort,
                                const QSslConfiguration &sslConfig,
                                const QSslCertificate &serverCert,
                                QObject *parent = nullptr);
    ~FileTransferClient() override;

    /**
     * 啟動 polling。多次呼叫無害（idempotent）。
     */
    void start();

    /**
     * 停止 polling 並中止 in-flight transfer。
     */
    void stop();

    /**
     * 由 hotkey 觸發。中止當前傳輸（若有），通知 server。
     */
    void cancelCurrent();

signals:
    /**
     * 進度 / 狀態變化，給 session.cpp 顯示 OverlayManager OSD 文字。
     * @param status 一行 user-facing 訊息（譬如 "Receiving foo.zip (25%)"）
     */
    void statusChanged(const QString &status);

private slots:
    void onWorkerStarted();          ///< QThread::started 後在 worker thread 跑
    void onWorkerStopping();         ///< 停止前清理（在 worker thread）
    void onPollTimer();
    void onPollFinished();
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);

private:
    enum class CommandType {
        ListDir,
        DownloadToClient,
        UploadFromClient,
        Cancel,
        Unknown,
    };

    struct PendingCommand {
        CommandType type = CommandType::Unknown;
        QString token;
        QString filename;
        quint64 size = 0;
        QString path;            ///< 任意 client 端路徑（empty = Downloads）
        bool isDirectory = false; ///< upload：是否是資料夾（client 端 zip 後再上傳）
    };

    QUrl buildUrl(const QString &path, const QString &query = QString()) const;
    QNetworkRequest buildRequest(const QUrl &url) const;

    void dispatch(const PendingCommand &cmd);
    void handleListDir(const PendingCommand &cmd);
    void handleDownloadToClient(const PendingCommand &cmd);
    void handleUploadFromClient(const PendingCommand &cmd);

    /**
     * Filename sanitize（單一檔名、無 path traversal 字元）。失敗回傳空。
     */
    static QString sanitizeFilename(const QString &raw);

    /**
     * Downloads dir resolver via QStandardPaths；建立必要時自動 mkdir。
     */
    static QString downloadsDir();

    /**
     * 把實際寫到 Downloads 之前先 collision-resolve。
     */
    static QString resolveCollision(const QString &dir, const QString &filename);

    /**
     * 發 POST /transfer/result 回 server（final ack 或 listing payload）。
     */
    void postResult(const QByteArray &json);

    /**
     * 上報 5% 步進進度（讓 server 端 progress endpoint 也能看到）。
     */
    void reportProgress(const QString &token,
                        const QString &direction,
                        quint64 bytesDone,
                        quint64 totalBytes);

    QString m_HostAddress;
    quint16 m_HttpsPort;
    QSslConfiguration m_SslConfig;
    QSslCertificate m_ServerCert;   ///< Pinned server cert (paired)
    /**
     * Session::exec() 在 stream 進行中跑 SDL main loop（SDL_WaitEventTimeout /
     * SDL_PollEvent），Qt event loop 完全被 starve。所以 FileTransferClient
     * 不能 live on main thread — 必須有自己的 QThread + event loop，QTimer
     * 才會 tick、QNAM 才會處理 reply。
     */
    QThread *m_Worker = nullptr;
    QNetworkAccessManager *m_Nam = nullptr;   ///< Heap-allocated 才能 moveToThread + lazy init
    QTimer *m_PollTimer = nullptr;
    QPointer<QNetworkReply> m_PollReply;
    QPointer<QNetworkReply> m_ActiveTransferReply;
    QString m_ActiveToken;
    QString m_ActiveDirection;       ///< "in" (download to client) 或 "out"
    quint64 m_ActiveTotalBytes = 0;
    quint64 m_ActiveBytesDone = 0;
    int m_LastProgressBucket = -1;   ///< 0-20 (每 5%)，用來抑制 toast 重複更新
    QFile *m_ActiveFile = nullptr;   ///< Download writer / upload reader 共用
    bool m_Started = false;
};
