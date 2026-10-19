#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include "backend/computermanager.h"
#include "backend/webresolver.h"

// VipleStream 2.0 §VR-PLAYER：影片模式選片頁（gui/VideoView.qml）的資料來源。
//   - 頭盔本機：幾個起點資料夾（影片、下載、外接儲存裝置、家目錄）與資料夾內容（只列子資料夾與影片檔）；
//   - 最近播過：播放器（PlayerSession）結束時寫在 QSettings 的 player/history/<雜湊>（片名、來源、停在哪裡、長度、
//     使用者選過的畫面格式、最後播放時間）；
//   - 從檔名猜畫面格式（playerformat.h，選片頁顯示用；真正播放時播放器還會看容器的中繼資料）。
// 純讀取，不碰播放本身；開播交給 VrLauncher::launchPlayer()。
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

    // ── §VR-PLAYER（V3）：host 分享的影片（VipleStream Server 的 media_dirs，經 HTTPS、用配對憑證）──
    // [{index, name, online}]：配對過的主機（index 是 ComputerManager 裡的位置）
    Q_INVOKABLE QVariantList hosts(ComputerManager* cm) const;
    // [{index, name}]：那台主機分享的資料夾；主機沒開分享、不在線上或連不上時回空清單（原因看 lastError()）
    Q_INVOKABLE QVariantList hostRoots(ComputerManager* cm, int host);
    // [{name, path, isDir, detail, format}]：path 是相對於分享資料夾的路徑（用 / 分隔）
    Q_INVOKABLE QVariantList hostList(ComputerManager* cm, int host, int root, const QString& path);
    // 非同步版（選片頁用，讀取期間介面不會卡住）：root＝-1 列分享資料夾，否則列那個資料夾裡 path 的內容。
    // 結果經 hostBrowseFinished(requestId, entries, error)；entries 的格式同 hostRoots（root＝-1）或 hostList
    Q_INVOKABLE void hostBrowse(ComputerManager* cm, int host, int root, const QString& path, int requestId);
    // 播放用的網址（https://<主機>:<埠>/viple/media/file?root=…&path=…）
    Q_INVOKABLE QString hostFileUrl(ComputerManager* cm, int host, int root, const QString& path) const;
    Q_INVOKABLE QString lastError() const { return m_LastError; }

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

private:
    QString m_LastError;
    WebResolver m_Web;
};
