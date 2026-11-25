#pragma once

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>

class QThread;

// 實作在 main.cpp。logger 的鎖與檔案在那裡，logdrop 不能自己猜。
QString vipleCurrentLogFilePath();
void vipleFlushLogFile();

// §LOGDROP：桌面 client 的匿名 log 上傳。QML 用 LogDrop 單例。
class LogDrop : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString consent READ consent NOTIFY consentChanged)
    Q_PROPERTY(QString installId READ installId CONSTANT)
    Q_PROPERTY(QString lastCode READ lastCode NOTIFY lastResultChanged)
    Q_PROPERTY(qint64 lastTime READ lastTime NOTIFY lastResultChanged)

public:
    static LogDrop* instance();

    QString consent() const;
    QString installId() const;
    QString lastCode() const;
    qint64 lastTime() const;

    Q_INVOKABLE void setConsent(bool allow);
    Q_INVOKABLE void uploadNow(const QString& note);

    // 呼叫端立刻返回。session-end 會再等 3 秒才讀檔。
    // 靜態：串流清理在別的執行緒，instance() 在那裡不會建立物件。
    static void autoUpload(const QString& trigger);

    // 快路徑（--help、--version、probe、fruc-offline）不要呼叫。
    static void prepareForRun();
    static void markCleanExit();

signals:
    void consentChanged();
    void lastResultChanged();
    void uploadFinished(bool ok, QString code, QString error);

private slots:
    // 給別的執行緒丟回 GUI。參數用值，Q_ARG 才帶得了。
    void beginAuto(QString trigger);

private:
    explicit LogDrop();

    void startWorker(const QString& trigger, const QString& note, int delayMs, bool previous, bool manual);
    void finishUpload(bool manual, bool ok, const QString& code, const QString& error);
    QString reserve(const QString& trigger);
    bool autoEnabled() const;
    void loadState();
    bool consumeStaleMarkers();
    void writeOwnMarker() const;
    void removeOwnMarker() const;
    void waitForUploads();

    class UploadThread;
    friend class UploadThread;

    mutable QMutex m_Mu;
    QString m_Consent;
    QString m_InstallId;
    QString m_LastCode;
    qint64 m_LastTime = 0;
    std::atomic<bool> m_Shutdown {false};
    std::atomic<int> m_ManualBusy {0};
    QMutex m_ThreadMu;
    QList<QThread*> m_Threads;
};
