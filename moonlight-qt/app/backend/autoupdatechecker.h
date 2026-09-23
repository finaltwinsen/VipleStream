#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class AutoUpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit AutoUpdateChecker(QObject *parent = nullptr);

    Q_INVOKABLE void start();

signals:
    void onUpdateAvailable(QString newVersion, QString url);

private slots:
    void handleUpdateCheckRequestFinished(QNetworkReply* reply);

private:
    // §UPDATE-HEAD：兩段式查詢——先 HEAD github.com 的 releases/latest（免 API 配額），
    // 失敗才退回 REST API（帶 ETag 條件請求）。
    enum class Phase { Idle, HeadLatest, ApiLatest };

    void sendHeadLatest();
    void sendApiLatest();
    void handleHeadReply(QNetworkReply* reply);
    void handleApiReply(QNetworkReply* reply);
    void finishNam();

    bool parseStringToVersionQuad(const QString& string, QVector<int>& version);
    int compareVersion(const QVector<int>& version1, const QVector<int>& version2);
    QString getPlatform();

    // 以 QSettings 快取的上次結果判斷（debounce / 304 / 失敗路徑共用）
    void evaluateCachedLatest(const char* reason);
    // 驗證 tag → 存快取 + 時間戳 → 比對；回 false 表示 tag 不像版號（不存）
    bool storeAndNotify(const QString& latestVersion, const QString& releasePageUrl, const char* source);
    void compareAndNotify(const QString& latestVersion, const QString& releasePageUrl, const char* source);

    QVector<int> m_CurrentVersionQuad;
    QNetworkAccessManager* m_Nam;
    Phase m_Phase;
    QString m_CachedEtag;
};
