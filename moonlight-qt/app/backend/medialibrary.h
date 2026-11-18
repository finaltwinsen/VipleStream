#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <atomic>

#include "backend/computermanager.h"
#include "backend/webresolver.h"

// VipleStream 2.0 §VR-PLAYER：影片模式選片頁（gui/VideoView.qml）的資料來源。
//   - 頭盔本機：幾個起點資料夾（影片、下載、外接儲存裝置、家目錄）與資料夾內容（只列子資料夾與影片檔）；
//   - 最近播過：播放器（PlayerSession）結束時寫在 QSettings 的 player/history/<雜湊>（片名、來源、停在哪裡、長度、
//     使用者選過的畫面格式、最後播放時間）；
//   - 從檔名猜畫面格式（playerformat.h，選片頁顯示用；真正播放時播放器還會看容器的中繼資料）。
// 開播交給 VrLauncher::launchPlayer()。清單本身不改檔案；§MEDIA-FILEOPS 的檔案管理是另外一條路。
class MediaLibrary : public QObject
{
    Q_OBJECT

public:
    explicit MediaLibrary(QObject* parent = nullptr);

    // [{name, path}]：存在的起點資料夾
    Q_INVOKABLE QVariantList roots() const;
    // [{name, path, isDir, detail, format}]：子資料夾在前、影片檔在後，各自照名稱排序；讀不到回空清單
    Q_INVOKABLE QVariantList listDir(const QString& path) const;
    // 上一層（已經是起點或根目錄時回空字串）
    Q_INVOKABLE QString parentDir(const QString& path) const;
    // [{title, source, position, duration, progress, format}]：最近播過的，新的在前，最多 30 筆
    Q_INVOKABLE QVariantList recent() const;
    Q_INVOKABLE void forget(const QString& source);
    // 從檔名（或網址路徑）猜的畫面格式名稱（playerfmt::name）；猜不出來回空字串
    Q_INVOKABLE QString guessFormat(const QString& source) const;
    // 看起來像不像能播的網址（http／https 開頭）
    Q_INVOKABLE bool isUrl(const QString& text) const;

    // ── §VR-PLAYER（V3）：host 上的影片（VipleStream Server 的 /viple/media/*，經 HTTPS、用配對憑證）──
    // §REVIEW-1104：主機改用 uuid 識別，避免主機排序變動後指到錯的電腦
    // [{uuid, name, online}]：配對過的主機
    Q_INVOKABLE QVariantList hosts(ComputerManager* cm) const;
    // 在背景執行緒讀（主機慢或不通時介面不會卡住）：root＝-1 列這台主機的根（磁碟機或分享資料夾），否則列那個根裡
    // path（相對路徑，用 / 分隔）的內容。結果經 hostBrowseFinished(requestId, entries, error)：
    //   root＝-1：[{index, name, drive, writable}]；否則 [{name, path, isDir, detail, format}]（子資料夾在前）
    // §MEDIA-FILEOPS：writable 來自 <Writable>，沒有這個欄位（舊 server）就是 false。
    Q_INVOKABLE void hostBrowse(ComputerManager* cm, const QString& host, int root, const QString& path, int requestId);
    // 播放用的網址（https://<主機>:<埠>/viple/media/file?root=…&path=…）
    Q_INVOKABLE QString hostFileUrl(ComputerManager* cm, const QString& host, int root, const QString& path) const;
    // §REVIEW-1118B：播放器只對「這個 App 自己連過的已配對電腦」出示配對用的用戶端憑證。
    // 網頁或使用者給的網址只要路徑長得像 /viple/media/file 就附憑證的話，任何網站都拿得到這張憑證。
    // 這裡記下已配對電腦的 https 端點（主機:埠，存在設定裡，播放子行程也讀得到）；播放器照這份清單決定。
    static void rememberCertEndpoint(const QUrl& httpsBase);
    static bool isCertEndpoint(const QUrl& url);
    // §VR-PLAYER-THUMB：主機分享檔案的縮圖 id（image://vthumb/<這個>，見 videothumbs.h）；主機找不到回空字串
    // stamp＝清單給的「大小_修改時間」（主機上同名檔案換掉之後換新縮圖）
    Q_INVOKABLE QString hostThumbId(ComputerManager* cm, const QString& host, int root, const QString& path, const QString& stamp) const;

    // ── §MEDIA-FILEOPS：本機與 host 分享資料夾的檔案管理（各自內部，不跨裝置）──
    // 同時只跑一個。忙的時候再呼叫，立刻以 opFinished("busy") 結束，不排隊。
    Q_PROPERTY(bool opBusy READ opBusy NOTIFY opBusyChanged)
    bool opBusy() const { return m_OpBusy; }
    // 這個資料夾在某個起點底下，而且檔案系統說可以寫
    Q_INVOKABLE bool localWritable(const QString& dir) const;
    // op＝mkdir（path＝父資料夾、name＝新名稱）／delete（path）／copy、move（path＝來源、destDir＝目的資料夾）。背景執行緒做。
    Q_INVOKABLE void localOp(const QString& op, const QString& path, const QString& destDir, const QString& name);
    // 打 /viple/media/op。delete／copy／move 受理後每 500 ms 問 /viple/media/opstatus，直到不是 running。
    Q_INVOKABLE void hostOp(ComputerManager* cm, const QString& host, const QString& op, int root, const QString& path, int droot,
                            const QString& dpath, const QString& name);
    // 本機設取消旗標；host 另外打 /viple/media/opcancel（工作自己收尾後狀態才變 cancelled）
    Q_INVOKABLE void cancelOp();

    // ── §VR-PLAYER（V4）：YouTube 等網站（WebResolver）──
    // 非同步：結果經 webSearchFinished(results, error)；results＝[{title, url, channel, duration}]
    Q_INVOKABLE void webSearch(const QString& query);
    // 非同步：下載最新的 yt-dlp；結果經 webUpdateFinished(ok, message)
    Q_INVOKABLE void webUpdate();
    // 目前的 yt-dlp 版本（空＝沒有）
    Q_INVOKABLE QString webVersion();

signals:
    void webSearchFinished(const QVariantList& results, const QString& error);
    void webUpdateFinished(bool ok, const QString& message);
    void hostBrowseFinished(int requestId, const QVariantList& entries, const QString& error);
    // §MEDIA-FILEOPS：都排回 GUI 執行緒才發。error 空字串＝成功，否則是與 host 協定相同的英文字串。
    void opProgress(qint64 done, qint64 total);
    void opFinished(const QString& error);
    void opBusyChanged();

private:
    WebResolver m_Web;
    // 只在 GUI 執行緒讀寫。背景執行緒不碰它，結束時用 invokeMethod 排回來清。
    bool m_OpBusy = false;
    // 下面幾個給背景執行緒與 cancelOp 共用。物件何時拆掉與 hostBrowse 的 detach 一樣：
    // 單例活到行程結束，執行緒只在還會 invokeMethod 的前提下碰 this，不另做一套生命週期。
    std::atomic<bool> m_Cancel{false};
    std::atomic<qint64> m_HostJob{0};
    std::atomic<quint64> m_OpGen{0};
    ComputerManager* m_OpCm = nullptr;
    QString m_OpHost;

    // 背景執行緒呼叫：只把訊號排回 GUI，不在這邊改 m_OpBusy。
    void postProgress(qint64 done, qint64 total);
    void finishOp(const QString& error);
};
