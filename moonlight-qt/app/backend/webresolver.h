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
    // 可以換的畫質（只有影像的那一條；聲音不變）。同一個高度只留一個：https 直連、不要 AV1、SDR，同高度取 yt-dlp 排最好的
    struct VideoChoice {
        int height = 0;
        QString label;      // "1080p"、"2160p60"
        QString url;
        QString projection; // 同 Result::projection
    };
    // 字幕軌（json3 格式的網址；YouTube 的時間文字）
    struct SubtitleTrack {
        QString lang;       // "en"、"zh-Hant"、"en-orig"
        QString name;       // "English"、"Chinese (Traditional)"
        QString url;
        bool automatic = false;  // 自動產生（含自動翻譯）
    };

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
        // 影像那一條的投影（YouTube 的 projectionType，yt-dlp 寫在 format_note 最後）：equi＝等距柱狀、
        // equi_threed_top_bottom＝等距柱狀上下立體、mesh＝網格（等角立方體等）；平面影片或網站沒說＝空字串
        QString projection;
        // 換畫質、字幕（2026-11-02 使用者要求 VR 播放時也能用 YouTube 的畫質與字幕）
        QList<VideoChoice> videoChoices;   // 由高到低；空＝不能換（網站只給整條、或不是 YouTube 的格式）
        int videoChoice = -1;              // 目前用的是 videoChoices 的哪一個
        QList<SubtitleTrack> subtitles;    // 作者上傳的在前，自動產生的原文在後（不含自動翻譯）
    };

    explicit WebResolver(QObject* parent = nullptr);
    ~WebResolver() override;

    // yt-dlp 的位置（更新過的優先）；沒有回空字串
    static QString scriptPath();
    // 這個網址要不要先經過 yt-dlp：網址的路徑不是常見的影片檔／串流清單，也不是 VipleStream host 的分享檔案
    static bool needsResolve(const QString& url);
    // 阻塞：解析一個網址（timeoutMs 內沒結果就失敗）
    static Result resolve(const QString& pageUrl, int timeoutMs);
    // §REVIEW-1104：載入中按 B 要能打斷正在等的 yt-dlp（resolve 週期檢查這個旗標）
    static void setResolveCancel(bool cancel);

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
