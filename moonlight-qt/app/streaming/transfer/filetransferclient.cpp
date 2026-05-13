/**
 * @file streaming/transfer/filetransferclient.cpp
 * @brief In-stream file transfer client 實作。對應 server VipleStream §N。
 */

#include "filetransferclient.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

Q_LOGGING_CATEGORY(lcXfer, "viple.xfer")

namespace {
constexpr int k_PollIntervalMs = 2000;
constexpr int k_PollTimeoutMs = 15000;       // 從 5s 提高 — 連線握手 + 偶發網路延遲不算錯
constexpr int k_TransferTimeoutMs = 0;       // 無 timeout — 大檔可能很久
constexpr qint64 k_ChunkBytes = 64 * 1024;
}

FileTransferClient::FileTransferClient(const QString &hostAddress,
                                       quint16 httpsPort,
                                       const QSslConfiguration &sslConfig,
                                       const QSslCertificate &serverCert,
                                       QObject *parent)
    : QObject(parent)
    , m_HostAddress(hostAddress)
    , m_HttpsPort(httpsPort)
    , m_SslConfig(sslConfig)
    , m_ServerCert(serverCert)
{
    // 注意：m_Nam / m_PollTimer 都不在這裡建構。Stream 期間 SDL 主迴圈會
    // starve Qt event loop，必須讓這些物件 live on dedicated worker thread。
    // 真正建構在 start() → m_Worker thread 啟動後的 onWorkerStarted() 內。
}

void FileTransferClient::onWorkerStarted()
{
    // 此函式跑在 worker thread。從這刻起，QNAM / QTimer 才有事件迴圈可用。
    qCInfo(lcXfer) << "[VIPLE-XFER] worker thread started, initializing Nam + timer";
    m_Nam = new QNetworkAccessManager();
    m_PollTimer = new QTimer();
    m_PollTimer->setInterval(k_PollIntervalMs);
    m_PollTimer->setSingleShot(false);
    connect(m_PollTimer, &QTimer::timeout, this, &FileTransferClient::onPollTimer);
    connect(m_Nam, &QNetworkAccessManager::sslErrors,
            this, &FileTransferClient::onSslErrors);
    m_PollTimer->start();
    // 第一次 poll 不等 2s，立刻試一次
    QTimer::singleShot(200, this, &FileTransferClient::onPollTimer);
}

void FileTransferClient::onWorkerStopping()
{
    qCInfo(lcXfer) << "[VIPLE-XFER] worker thread stopping, cleaning up";
    if (m_PollTimer) {
        m_PollTimer->stop();
        m_PollTimer->deleteLater();
        m_PollTimer = nullptr;
    }
    // 注意：abort() 會「同步」emit finished signal，觸發 onPollFinished →
    // 把 m_PollReply QPointer clear 掉。如果我們先 abort 再 .clear()，外層
    // 的後續 `m_PollReply->deleteLater()` 會 null-deref。所以**先取出本地
    // pointer + clear QPointer**，再用本地 pointer 安全 abort/delete。
    if (m_PollReply) {
        auto *p = m_PollReply.data();
        m_PollReply.clear();
        if (p) {
            p->abort();
            p->deleteLater();
        }
    }
    if (m_ActiveTransferReply) {
        auto *p = m_ActiveTransferReply.data();
        m_ActiveTransferReply.clear();
        if (p) {
            p->abort();
            p->deleteLater();
        }
    }
    if (m_ActiveFile) {
        m_ActiveFile->close();
        delete m_ActiveFile;
        m_ActiveFile = nullptr;
    }
    if (m_Nam) {
        m_Nam->deleteLater();
        m_Nam = nullptr;
    }
}

void FileTransferClient::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    qCInfo(lcXfer) << "[VIPLE-XFER] sslErrors fired, count=" << errors.size()
                   << "pinnedCertNull=" << m_ServerCert.isNull();
    if (m_ServerCert.isNull()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] SSL errors but no pinned cert to verify against";
        return;
    }
    bool ignoreAll = true;
    for (const QSslError &err : errors) {
        qCInfo(lcXfer) << "[VIPLE-XFER] sslError:" << err.error() << err.errorString()
                       << "matchesPinned=" << (err.certificate() == m_ServerCert);
        if (err.certificate() != m_ServerCert) {
            qCWarning(lcXfer) << "[VIPLE-XFER] SSL error cert mismatch (rejecting):" << err.errorString();
            ignoreAll = false;
            break;
        }
    }
    if (ignoreAll) {
        reply->ignoreSslErrors(errors);
    }
}

FileTransferClient::~FileTransferClient()
{
    stop();
}

void FileTransferClient::start()
{
    if (m_Started) return;
    m_Started = true;
    qCInfo(lcXfer) << "[VIPLE-XFER] FileTransferClient start — host" << m_HostAddress << "port" << m_HttpsPort;
    m_Worker = new QThread();
    moveToThread(m_Worker);
    connect(m_Worker, &QThread::started, this, &FileTransferClient::onWorkerStarted);
    connect(m_Worker, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Worker->start();
}

void FileTransferClient::stop()
{
    if (!m_Started) return;
    m_Started = false;
    if (!m_Worker) {
        qCInfo(lcXfer) << "[VIPLE-XFER] FileTransferClient stop (no worker)";
        return;
    }
    // 先丟一個 cleanup invocation 到 worker thread（仍有 event loop），等執行完
    // 再 quit() event loop、wait join。BlockingQueuedConnection 確保
    // cleanup 完成才返回。
    QMetaObject::invokeMethod(this, "onWorkerStopping", Qt::BlockingQueuedConnection);
    m_Worker->quit();
    m_Worker->wait(3000);
    m_Worker = nullptr;
    qCInfo(lcXfer) << "[VIPLE-XFER] FileTransferClient stop";
}

void FileTransferClient::cancelCurrent()
{
    if (m_ActiveToken.isEmpty()) {
        emit statusChanged(tr("File transfer: nothing to cancel"));
        return;
    }
    qCInfo(lcXfer) << "[VIPLE-XFER] cancel requested token=" << m_ActiveToken;
    if (m_ActiveTransferReply) {
        m_ActiveTransferReply->abort();
    }
    QJsonObject obj;
    obj["kind"] = QStringLiteral("canceled");
    obj["token"] = m_ActiveToken;
    postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    emit statusChanged(tr("File transfer cancelled"));
    m_ActiveToken.clear();
    if (m_ActiveFile) {
        QString tmpPath = m_ActiveFile->fileName();
        m_ActiveFile->close();
        delete m_ActiveFile;
        m_ActiveFile = nullptr;
        QFile::remove(tmpPath);  // 半寫入的暫存清掉
    }
    m_ActiveTotalBytes = 0;
    m_ActiveBytesDone = 0;
    m_LastProgressBucket = -1;
}

QUrl FileTransferClient::buildUrl(const QString &path, const QString &query) const
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(m_HostAddress);
    url.setPort(m_HttpsPort);
    url.setPath(path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    return url;
}

QNetworkRequest FileTransferClient::buildRequest(const QUrl &url) const
{
    QNetworkRequest req(url);
    req.setSslConfiguration(m_SslConfig);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
    // 註：NvHTTP 那邊用 ConnectionCacheExpiryTimeoutSecondsAttribute=0 是因為
    // GFE 不喜歡 persistent connections。我們對的是 Sunshine（這個 fork 自己），
    // persistent connection 沒問題。**不要**設這個 attribute — 之前實測在 Qt
    // 6.10 + Sunshine 反而讓第一次 poll 直接 hang 15s 被 watchdog 砍掉。
    return req;
}

void FileTransferClient::onPollTimer()
{
    if (!m_Started || !m_Nam) return;
    if (m_PollReply) return;            // 上一次 poll 還在跑，跳過
    if (m_ActiveTransferReply) return;  // 有 transfer 進行中，先別 poll 新命令

    QUrl url = buildUrl(QStringLiteral("/transfer/poll"));
    qCInfo(lcXfer) << "[VIPLE-XFER] poll firing url=" << url.toString();
    QNetworkRequest req = buildRequest(url);
    m_PollReply = m_Nam->get(req);
    connect(m_PollReply.data(), &QNetworkReply::finished,
            this, &FileTransferClient::onPollFinished);
    QTimer::singleShot(k_PollTimeoutMs, m_PollReply.data(), [reply = m_PollReply]() {
        if (reply && !reply->isFinished()) {
            reply->abort();
        }
    });
}

void FileTransferClient::onPollFinished()
{
    if (!m_PollReply) return;
    QNetworkReply *reply = m_PollReply.data();
    m_PollReply.clear();
    auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });

    if (reply->error() != QNetworkReply::NoError) {
        // Bump 到 warning 讓 default log level 看得到（diagnosing 失敗用）
        qCWarning(lcXfer) << "[VIPLE-XFER] poll error" << reply->error() << reply->errorString();
        return;
    }
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 204) {
        return;  // no command
    }
    if (status != 200) {
        qCWarning(lcXfer) << "[VIPLE-XFER] poll non-200" << status;
        return;
    }
    QByteArray body = reply->readAll();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] poll bad JSON" << err.errorString() << body;
        return;
    }
    QJsonObject obj = doc.object();
    PendingCommand cmd;
    cmd.token = obj.value("token").toString();
    cmd.filename = obj.value("filename").toString();
    cmd.size = static_cast<quint64>(obj.value("size").toDouble());
    cmd.path = obj.value("path").toString();
    cmd.isDirectory = obj.value("is_directory").toBool();
    QString typeStr = obj.value("type").toString();
    if (typeStr == "list_dir") cmd.type = CommandType::ListDir;
    else if (typeStr == "download_to_client") cmd.type = CommandType::DownloadToClient;
    else if (typeStr == "upload_from_client") cmd.type = CommandType::UploadFromClient;
    else if (typeStr == "cancel") cmd.type = CommandType::Cancel;
    else cmd.type = CommandType::Unknown;

    qCInfo(lcXfer) << "[VIPLE-XFER] poll got cmd type=" << typeStr
                   << "token=" << cmd.token
                   << "filename=" << cmd.filename
                   << "size=" << cmd.size;
    dispatch(cmd);
}

void FileTransferClient::dispatch(const PendingCommand &cmd)
{
    switch (cmd.type) {
    case CommandType::ListDir:
        handleListDir(cmd);
        break;
    case CommandType::DownloadToClient:
        handleDownloadToClient(cmd);
        break;
    case CommandType::UploadFromClient:
        handleUploadFromClient(cmd);
        break;
    case CommandType::Cancel:
        if (!m_ActiveToken.isEmpty() && m_ActiveToken == cmd.token) {
            cancelCurrent();
        }
        break;
    default:
        qCWarning(lcXfer) << "[VIPLE-XFER] dispatch unknown command type";
        break;
    }
}

void FileTransferClient::handleListDir(const PendingCommand &cmd)
{
    // 空字串 → 預設 Downloads；否則用 client 端絕對路徑（信任 web UI 使用者）
    QString dir = cmd.path.isEmpty() ? downloadsDir() : cmd.path;
    QJsonArray entries;
    QDir d(dir);
    QString resolvedPath = d.absolutePath();
    if (d.exists()) {
        QFileInfoList list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &fi : list) {
            QJsonObject e;
            e["name"] = fi.fileName();
            e["is_directory"] = fi.isDir();
            e["size"] = fi.isDir() ? 0.0 : static_cast<double>(fi.size());
            e["mtime"] = fi.lastModified().toMSecsSinceEpoch();
            entries.append(e);
        }
    } else {
        qCWarning(lcXfer) << "[VIPLE-XFER] list_dir path does not exist:" << dir;
    }
    QJsonObject obj;
    obj["kind"] = QStringLiteral("listing");
    obj["query_id"] = cmd.token;
    obj["path"] = resolvedPath;
    obj["entries"] = entries;
    postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qCInfo(lcXfer) << "[VIPLE-XFER] sent listing path=" << resolvedPath
                   << "entries=" << entries.size();
}

void FileTransferClient::handleDownloadToClient(const PendingCommand &cmd)
{
    if (!m_ActiveToken.isEmpty()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] download requested but transfer in progress";
        return;
    }
    QString safeName = sanitizeFilename(cmd.filename);
    if (safeName.isEmpty()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] download: rejected filename" << cmd.filename;
        return;
    }
    QString dir = downloadsDir();
    QString target = resolveCollision(dir, safeName);
    m_ActiveFile = new QFile(target);
    if (!m_ActiveFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcXfer) << "[VIPLE-XFER] open write failed" << target;
        delete m_ActiveFile;
        m_ActiveFile = nullptr;
        return;
    }
    m_ActiveToken = cmd.token;
    m_ActiveDirection = QStringLiteral("in");
    m_ActiveTotalBytes = cmd.size;
    m_ActiveBytesDone = 0;
    m_LastProgressBucket = -1;

    QUrl url = buildUrl(QStringLiteral("/transfer/blob"),
                        QStringLiteral("token=") + cmd.token);
    QNetworkRequest req = buildRequest(url);
    m_ActiveTransferReply = m_Nam->get(req);

    emit statusChanged(tr("Receiving file: %1 (0%)").arg(safeName));
    qCInfo(lcXfer) << "[VIPLE-XFER] download started token=" << cmd.token
                   << "target=" << target << "size=" << cmd.size;

    QPointer<FileTransferClient> self(this);
    QString filenameCopy = safeName;
    connect(m_ActiveTransferReply.data(), &QNetworkReply::readyRead, this, [self]() {
        if (!self || !self->m_ActiveFile || !self->m_ActiveTransferReply) return;
        QByteArray chunk = self->m_ActiveTransferReply->readAll();
        if (chunk.isEmpty()) return;
        self->m_ActiveFile->write(chunk);
        self->m_ActiveBytesDone += static_cast<quint64>(chunk.size());
        int bucket = (self->m_ActiveTotalBytes > 0)
                         ? static_cast<int>(self->m_ActiveBytesDone * 20 / self->m_ActiveTotalBytes)
                         : -1;
        if (bucket != self->m_LastProgressBucket && bucket >= 0) {
            self->m_LastProgressBucket = bucket;
            int pct = bucket * 5;
            emit self->statusChanged(tr("Receiving file: %1 (%2%)")
                                         .arg(QFileInfo(self->m_ActiveFile->fileName()).fileName())
                                         .arg(pct));
            self->reportProgress(self->m_ActiveToken, QStringLiteral("in"),
                                 self->m_ActiveBytesDone, self->m_ActiveTotalBytes);
        }
    });

    connect(m_ActiveTransferReply.data(), &QNetworkReply::finished, this, [self, filenameCopy]() {
        if (!self) return;
        QNetworkReply *r = self->m_ActiveTransferReply.data();
        self->m_ActiveTransferReply.clear();
        if (!r) return;
        bool ok = (r->error() == QNetworkReply::NoError);
        if (self->m_ActiveFile) {
            // flush 剩餘 readyRead 沒被觸發到的尾段
            QByteArray tail = r->readAll();
            if (!tail.isEmpty()) self->m_ActiveFile->write(tail);
            self->m_ActiveFile->flush();
            self->m_ActiveFile->close();
            QString finalPath = self->m_ActiveFile->fileName();
            delete self->m_ActiveFile;
            self->m_ActiveFile = nullptr;
            if (!ok) {
                QFile::remove(finalPath);
            } else {
                emit self->statusChanged(tr("Received: %1 → Downloads").arg(filenameCopy));
            }
        }
        QJsonObject obj;
        obj["kind"] = ok ? QStringLiteral("done") : QStringLiteral("failed");
        obj["token"] = self->m_ActiveToken;
        if (!ok) obj["error"] = r->errorString();
        self->postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        qCInfo(lcXfer) << "[VIPLE-XFER] download finished token=" << self->m_ActiveToken
                       << "ok=" << ok;
        self->m_ActiveToken.clear();
        self->m_ActiveTotalBytes = 0;
        self->m_ActiveBytesDone = 0;
        self->m_LastProgressBucket = -1;
        r->deleteLater();
        // 大檔長 GET 結束之後 QNAM 內部 socket pool state 可能不乾淨 —
        // 下一輪 poll 會卡到 watchdog timeout。強制 flush 連線池後再讓下一次
        // 請求建新 TCP / TLS handshake，避免重用半關閉的連線。
        if (self->m_Nam) self->m_Nam->clearAccessCache();
    });
}

void FileTransferClient::handleUploadFromClient(const PendingCommand &cmd)
{
    if (!m_ActiveToken.isEmpty()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] upload requested but transfer in progress";
        return;
    }
    QString safeName = sanitizeFilename(cmd.filename);
    if (safeName.isEmpty()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] upload: rejected filename" << cmd.filename;
        return;
    }
    QString baseDir = cmd.path.isEmpty() ? downloadsDir() : cmd.path;
    QString src = QDir(baseDir).filePath(safeName);
    QFileInfo srcInfo(src);
    if (!srcInfo.exists()) {
        qCWarning(lcXfer) << "[VIPLE-XFER] upload: missing src" << src;
        QJsonObject obj;
        obj["kind"] = QStringLiteral("failed");
        obj["token"] = cmd.token;
        obj["error"] = QStringLiteral("src_missing");
        postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        return;
    }

    // 資料夾 → 先 zip 到 temp，然後上傳 zip。完成後刪除 temp。
    QString uploadPath = src;
    bool zipTempCreated = false;
    if (cmd.isDirectory || srcInfo.isDir()) {
        QString tempZip = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                              .filePath("VipleXfer-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".zip");
        emit statusChanged(tr("Zipping folder: %1…").arg(safeName));
        qCInfo(lcXfer) << "[VIPLE-XFER] zipping folder src=" << src << "to" << tempZip;
        QProcess zipProc;
#ifdef Q_OS_WIN
        // PowerShell Compress-Archive — 通用、不依賴外部安裝
        QStringList args;
        args << "-NoLogo" << "-NonInteractive" << "-Command"
             << QString("Compress-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                    .arg(QString(src).replace("'", "''"), QString(tempZip).replace("'", "''"));
        zipProc.start("powershell.exe", args);
#else
        // Linux：用 zip 命令（zip -r OUT IN）
        QStringList args;
        args << "-r" << tempZip << safeName;
        zipProc.setWorkingDirectory(baseDir);
        zipProc.start("zip", args);
#endif
        if (!zipProc.waitForStarted(5000)) {
            qCWarning(lcXfer) << "[VIPLE-XFER] zip process failed to start";
            QJsonObject obj;
            obj["kind"] = QStringLiteral("failed");
            obj["token"] = cmd.token;
            obj["error"] = QStringLiteral("zip_start_failed");
            postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            return;
        }
        zipProc.waitForFinished(-1);
        if (zipProc.exitStatus() != QProcess::NormalExit || zipProc.exitCode() != 0) {
            qCWarning(lcXfer) << "[VIPLE-XFER] zip failed exitCode=" << zipProc.exitCode()
                              << "stderr=" << zipProc.readAllStandardError();
            QFile::remove(tempZip);
            QJsonObject obj;
            obj["kind"] = QStringLiteral("failed");
            obj["token"] = cmd.token;
            obj["error"] = QStringLiteral("zip_failed");
            postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
            return;
        }
        uploadPath = tempZip;
        zipTempCreated = true;
        QFileInfo zipInfo(tempZip);
        qCInfo(lcXfer) << "[VIPLE-XFER] zip done, size=" << zipInfo.size();
    }

    QFileInfo uploadInfo(uploadPath);
    m_ActiveFile = new QFile(uploadPath);
    if (!m_ActiveFile->open(QIODevice::ReadOnly)) {
        qCWarning(lcXfer) << "[VIPLE-XFER] upload: open read failed" << uploadPath;
        delete m_ActiveFile;
        m_ActiveFile = nullptr;
        if (zipTempCreated) QFile::remove(uploadPath);
        return;
    }
    m_ActiveToken = cmd.token;
    m_ActiveDirection = QStringLiteral("out");
    m_ActiveTotalBytes = static_cast<quint64>(uploadInfo.size());
    m_ActiveBytesDone = 0;
    m_LastProgressBucket = -1;

    QUrl url = buildUrl(QStringLiteral("/transfer/blob"),
                        QStringLiteral("token=") + cmd.token);
    QNetworkRequest req = buildRequest(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setHeader(QNetworkRequest::ContentLengthHeader, uploadInfo.size());

    // QNetworkAccessManager::post(QIODevice*) 會自動以 streaming 方式發送
    m_ActiveTransferReply = m_Nam->post(req, m_ActiveFile);

    emit statusChanged(tr("Sending: %1 (0%)").arg(safeName));
    qCInfo(lcXfer) << "[VIPLE-XFER] upload started token=" << cmd.token
                   << "uploadPath=" << uploadPath << "size=" << uploadInfo.size()
                   << "isDir=" << cmd.isDirectory;

    QPointer<FileTransferClient> self(this);
    QString filenameCopy = safeName;
    QString tempZipForCleanup = zipTempCreated ? uploadPath : QString();
    connect(m_ActiveTransferReply.data(), &QNetworkReply::uploadProgress,
            this, [self, filenameCopy](qint64 sent, qint64 total) {
                if (!self || total <= 0) return;
                self->m_ActiveBytesDone = static_cast<quint64>(sent);
                int bucket = static_cast<int>(sent * 20 / total);
                if (bucket != self->m_LastProgressBucket && bucket >= 0) {
                    self->m_LastProgressBucket = bucket;
                    int pct = bucket * 5;
                    emit self->statusChanged(tr("Sending: %1 (%2%)").arg(filenameCopy).arg(pct));
                    self->reportProgress(self->m_ActiveToken, QStringLiteral("out"),
                                         self->m_ActiveBytesDone,
                                         static_cast<quint64>(total));
                }
            });

    connect(m_ActiveTransferReply.data(), &QNetworkReply::finished, this, [self, filenameCopy, tempZipForCleanup]() {
        if (!self) return;
        QNetworkReply *r = self->m_ActiveTransferReply.data();
        self->m_ActiveTransferReply.clear();
        if (!r) return;
        bool ok = (r->error() == QNetworkReply::NoError);
        if (self->m_ActiveFile) {
            self->m_ActiveFile->close();
            delete self->m_ActiveFile;
            self->m_ActiveFile = nullptr;
        }
        // 清掉 zip temp（如果有的話）
        if (!tempZipForCleanup.isEmpty()) {
            QFile::remove(tempZipForCleanup);
        }
        if (ok) {
            emit self->statusChanged(tr("Sent: %1").arg(filenameCopy));
        } else {
            emit self->statusChanged(tr("Send failed: %1 (%2)").arg(filenameCopy, r->errorString()));
        }
        qCInfo(lcXfer) << "[VIPLE-XFER] upload finished token=" << self->m_ActiveToken
                       << "ok=" << ok;
        self->m_ActiveToken.clear();
        self->m_ActiveTotalBytes = 0;
        self->m_ActiveBytesDone = 0;
        self->m_LastProgressBucket = -1;
        r->deleteLater();
        // 同 download：長 POST 結束後 flush 連線池，避免重用半關閉的 socket
        if (self->m_Nam) self->m_Nam->clearAccessCache();
    });
}

void FileTransferClient::postResult(const QByteArray &json)
{
    QNetworkRequest req = buildRequest(buildUrl(QStringLiteral("/transfer/result")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_Nam->post(req, json);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void FileTransferClient::reportProgress(const QString &token,
                                        const QString &direction,
                                        quint64 bytesDone,
                                        quint64 totalBytes)
{
    // 進度也透過 /transfer/result 通知（kind=progress），server side 進度查
    // 詢一律用 /transfer/progress endpoint（query token），所以這裡不一定要發
    // 但為了讓 server log 有 5% 步進紀錄、且 web UI 可以即時追，發出去。
    QJsonObject obj;
    obj["kind"] = QStringLiteral("progress");
    obj["token"] = token;
    obj["direction"] = direction;
    obj["bytes_done"] = static_cast<double>(bytesDone);
    obj["total_bytes"] = static_cast<double>(totalBytes);
    postResult(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString FileTransferClient::sanitizeFilename(const QString &raw)
{
    if (raw.isEmpty() || raw.size() > 255) return {};
    if (raw == "." || raw == "..") return {};
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar ch : raw) {
        if (ch.unicode() < 0x20) return {};
        if (forbidden.contains(ch)) return {};
    }
    return raw;
}

QString FileTransferClient::downloadsDir()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/Downloads";
    }
    QDir().mkpath(dir);
    return dir;
}

QString FileTransferClient::resolveCollision(const QString &dir, const QString &filename)
{
    QString candidate = QDir(dir).filePath(filename);
    if (!QFile::exists(candidate)) return candidate;
    QFileInfo fi(candidate);
    QString stem = fi.completeBaseName();
    QString suffix = fi.suffix();
    QString sep = suffix.isEmpty() ? QString() : QStringLiteral(".");
    for (int i = 1; i < 1000; ++i) {
        QString c = QDir(dir).filePath(stem + QStringLiteral("-") + QString::number(i) + sep + suffix);
        if (!QFile::exists(c)) return c;
    }
    return QDir(dir).filePath(stem + QStringLiteral("-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + sep + suffix);
}
