#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QQuickAsyncImageProvider>
#include <QSet>
#include <QString>

#include <atomic>
#include <memory>

class QNetworkAccessManager;
class QQmlEngine;
class QThreadPool;
class QQuickImageResponse;

// §VR-PLAYER-THUMB（2026-10-23）：影片模式格狀檢視的縮圖，QML 用 image://vthumb/<id>。
// id 是 "<種類>/<內容>"，內容用 encodeId() 編碼（QML 端同一套：encodeURIComponent 後 '~'→"%7E"、'%'→'~'）：
//   local/<檔案路徑>                       頭盔本機檔：FFmpeg 抽一張畫面（只有 HAVE_VR_PLAYER 的建置）
//   host/<uuid>/<root>/<戳記>/<相對路徑>    主機分享的檔案：向主機要 /viple/media/thumb（主機產生）；戳記＝大小_修改時間
//   src/<播放來源>                          最近播過：本機路徑／主機的 /viple/media/file 網址／YouTube 網址
// 結果存成磁碟快取（JPEG，寬約 320）。
//
// 執行緒（code review 2026-10-23 之後）：provider 的狀態只在 GUI 執行緒讀寫。同一個快取鍵同時只有一個工作（m_Pending，
// 等它的回應都掛在同一筆）；等待的回應全都取消或被刪掉時設取消旗標，工作在做重的事之前會看。背景工作不拿 NvComputer*
// （排程時就在 GUI 執行緒把位址、埠、憑證複製下來）；結果排回 GUI 執行緒，provider 已經不在（m_Alive＝false）就丟掉。
// 只有確定的結果（404、壞的影像、解不出畫面）才記成「這次執行不再試」；逾時、連線錯誤下次還會再試。
class VideoThumbProvider : public QQuickAsyncImageProvider
{
public:
    explicit VideoThumbProvider(QQmlEngine* engine);
    ~VideoThumbProvider() override;

    QQuickImageResponse* requestImageResponse(const QString& id, const QSize& requestedSize) override;

    static QString encodeId(const QString& s);
    static QString decodeId(const QString& s);

    // 以下都在 GUI 執行緒呼叫
    QQmlEngine* engine() const { return m_Engine; }
    QNetworkAccessManager* network();
    QThreadPool* decodePool() const { return m_DecodePool; }
    QThreadPool* hostPool() const { return m_HostPool; }
    std::shared_ptr<bool> alive() const { return m_Alive; }
    QString cachePath(const QString& key) const;
    bool knownMissing(const QString& key) const { return m_Missing.contains(key); }

    // 這個鍵已經有工作在跑：把 r 掛上去，回 true（呼叫端不必再開工作）
    bool joinPending(const QString& key, QQuickImageResponse* r);
    // 開一個新工作：回傳它的取消旗標
    std::shared_ptr<std::atomic<bool>> beginPending(const QString& key, QQuickImageResponse* r);
    // r 不等了（取消或刪掉）；沒人等了就設取消旗標
    void withdraw(const QString& key, QQuickImageResponse* r);
    // 工作結束（GUI 執行緒）：存快取或記下拿不到（definitive 時），交給所有還在等的回應
    void complete(const QString& key, const QImage& img, const QString& err, bool definitive, const char* kind);

private:
    struct Pending {
        QList<QPointer<QQuickImageResponse>> waiters;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    QQmlEngine* m_Engine;
    QNetworkAccessManager* m_Network = nullptr;
    QThreadPool* m_DecodePool;
    QThreadPool* m_HostPool;
    QString m_CacheDir;
    QSet<QString> m_Missing;
    QHash<QString, Pending> m_Pending;
    std::shared_ptr<bool> m_Alive;
    int m_StatOk = 0;
    int m_StatFail = 0;
};
