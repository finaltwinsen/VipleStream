#pragma once

#include <QQuickAsyncImageProvider>
#include <QSet>
#include <QString>

class QNetworkAccessManager;
class QQmlEngine;
class QThreadPool;

// §VR-PLAYER-THUMB（2026-10-23）：影片模式格狀檢視的縮圖，QML 用 image://vthumb/<id>。
// id 是 "<種類>/<內容>"，內容用 encodeId() 編碼（QML 端同一套：encodeURIComponent 後 '~'→"%7E"、'%'→'~'）：
//   local/<檔案路徑>               頭盔本機檔：FFmpeg 抽一張畫面（只有 HAVE_VR_PLAYER 的建置）
//   host/<uuid>/<root>/<相對路徑>   主機分享的檔案：向主機要 /viple/media/thumb（主機產生）
//   src/<播放來源>                  最近播過：本機路徑／主機的 /viple/media/file 網址／YouTube 網址
// 結果存成磁碟快取（JPEG，寬約 320）；拿不到的這次執行期間不再試。全部在 GUI 執行緒排程，解碼與連線在背景。
class VideoThumbProvider : public QQuickAsyncImageProvider
{
public:
    explicit VideoThumbProvider(QQmlEngine* engine);
    ~VideoThumbProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    static QString encodeId(const QString& s);
    static QString decodeId(const QString& s);

    // 以下都在 GUI 執行緒呼叫（VideoThumbResponse 用）
    QQmlEngine* engine() const { return m_Engine; }
    QNetworkAccessManager* network();
    QThreadPool* decodePool() const { return m_DecodePool; }
    QThreadPool* hostPool() const { return m_HostPool; }
    QString cachePath(const QString& key) const;
    bool knownMissing(const QString& key) const { return m_Missing.contains(key); }
    void markMissing(const QString& key) { m_Missing.insert(key); }
    void noteResult(bool ok, const char* kind);

private:
    QQmlEngine* m_Engine;
    QNetworkAccessManager* m_Network = nullptr;
    QThreadPool* m_DecodePool;
    QThreadPool* m_HostPool;
    QString m_CacheDir;
    QSet<QString> m_Missing;
    int m_StatOk = 0;
    int m_StatFail = 0;
};
