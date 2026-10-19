#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QNetworkAccessManager;
class QProcess;

// VipleStream 2.0 §VR-PLAYER（V4）：YouTube 等影音網站的網址 → 可以直接播的影像／音訊網址。
//
// 用 yt-dlp（Python zipapp，Flatpak 的 KDE runtime 有 python3）。來源依序：
//   1. 使用者在介面上更新過的版本（<app 資料夾>/yt-dlp，網站改版時按「更新」就好，不必重裝 app）；
//   2. app 內附的版本（/app/share/viplestream/yt-dlp，打包時固定版本與 sha256）。
// 選格式的規則照頭盔能硬體解的：不要 AV1（頭盔沒有 AV1 硬體解碼）、不要 HDR（10-bit）、最高 2160p；影像與
// 音訊分開兩條（網站的高解析度都是這樣給），播放器各開一個輸入。
//
// resolve() 會阻塞（yt-dlp 要幾秒），只在播放器的工作執行緒呼叫。搜尋與更新是非同步的（MediaLibrary 給介面用）。
class WebResolver : public QObject
{
    Q_OBJECT

public:
    struct Result {
        bool ok = false;
        QString error;
        QString title;
        QString videoUrl;
        QString audioUrl;   // 空＝影像那一條自己有聲音
        QString userAgent;
        QString headers;    // "Key: value\r\n…"（不含 User-Agent）
        double durationSec = 0.0;
        bool isLive = false;
    };

    explicit WebResolver(QObject* parent = nullptr);
    ~WebResolver() override;

    // yt-dlp 的位置（更新過的優先）；沒有回空字串
    static QString scriptPath();
    // 這個網址要不要先經過 yt-dlp：網址的路徑不是常見的影片檔／串流清單，也不是 VipleStream host 的分享檔案
    static bool needsResolve(const QString& url);
    // 阻塞：解析一個網址（timeoutMs 內沒結果就失敗）
    static Result resolve(const QString& pageUrl, int timeoutMs);

    // ── 非同步（GUI 執行緒）──
    // 搜尋 YouTube（前 n 筆）；結果經 searchFinished([{title, url, duration, channel}], error)
    void search(const QString& query, int n);
    // 從 yt-dlp 的 GitHub release 下載最新版（核對 SHA2-256SUMS）存到 app 資料夾；結果經 updateFinished(ok, message)
    void update();
    // 目前版本（同步跑一次 --version，有快取）；沒有 yt-dlp 回空字串
    QString version();

signals:
    void searchFinished(const QVariantList& results, const QString& error);
    void updateFinished(bool ok, const QString& message);

private:
    QProcess* m_Search = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;
    QString m_Version;
};
