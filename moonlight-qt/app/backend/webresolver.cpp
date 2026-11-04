// VipleStream 2.0 §VR-PLAYER（V4）— 見 webresolver.h。

#include "webresolver.h"

#include <SDL.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <atomic>

namespace {

const char* const kBundled = "/app/share/viplestream/yt-dlp";
const char* const kReleaseBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";

// §REVIEW-1104：載入中按 B 由別的執行緒設這個旗標，resolve() 的等待迴圈看到就 kill yt-dlp
std::atomic<bool> g_ResolveCancel{false};

QString updatedPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/yt-dlp");
}

// 頭盔能硬體解的：不要 AV1、不要 HDR、最高 2160p；影像＋音訊分開優先，沒有就整條的
const char* const kFormat = "bv*[vcodec!*=av01][dynamic_range=SDR][height<=2160]+ba/b[vcodec!*=av01][dynamic_range=SDR]/b";

// 格式的投影：yt-dlp 把 YouTube 的 projectionType 去掉 "RECTANGULAR" 轉小寫，接在 format_note 最後
// （"2160s60, equi"、"1440s60, mesh"）；平面影片的 RECTANGULAR 變成空的、不會出現
QString projectionOf(const QJsonObject& f)
{
    const QStringList parts = f.value(QStringLiteral("format_note")).toString().split(QLatin1Char(','));
    for (const QString& raw : parts) {
        const QString p = raw.trimmed().toLower();
        if (p.startsWith(QLatin1String("equi")) || p == QLatin1String("mesh")) {
            return p;
        }
    }
    return QString();
}

// 畫質清單：只有影像的格式，https 直連（不要 HLS／DASH 清單）、不要 AV1、SDR、最高 2160p。
// yt-dlp 的 formats 由差到好排，所以同一個高度後面的蓋前面的
QList<WebResolver::VideoChoice> videoChoicesOf(const QJsonObject& j)
{
    QMap<int, WebResolver::VideoChoice> byHeight;
    const QJsonArray formats = j.value(QStringLiteral("formats")).toArray();
    for (const QJsonValue& v : formats) {
        const QJsonObject f = v.toObject();
        const QString vcodec = f.value(QStringLiteral("vcodec")).toString();
        const int h = f.value(QStringLiteral("height")).toInt();
        if (vcodec.isEmpty() || vcodec == QLatin1String("none") || vcodec.startsWith(QLatin1String("av01")) ||
            f.value(QStringLiteral("acodec")).toString() != QLatin1String("none") ||
            f.value(QStringLiteral("protocol")).toString() != QLatin1String("https") ||
            f.value(QStringLiteral("dynamic_range")).toString(QStringLiteral("SDR")) != QLatin1String("SDR") || h <= 0 || h > 2160) {
            continue;
        }
        WebResolver::VideoChoice c;
        c.height = h;
        c.url = f.value(QStringLiteral("url")).toString();
        c.projection = projectionOf(f);
        // 標籤照 YouTube 的寫法（format_note 開頭的 "1080p60"、360° 片是 "2160s60"），沒有就用高度
        const QString note = f.value(QStringLiteral("format_note")).toString().split(QLatin1Char(',')).value(0).trimmed();
        static const QRegularExpression kNote(QStringLiteral("^(\\d+)[ps](\\d*)$"));
        const QRegularExpressionMatch m = kNote.match(note);
        c.label = m.hasMatch() ? m.captured(1) + QStringLiteral("p") + m.captured(2) : QStringLiteral("%1p").arg(h);
        if (!c.url.isEmpty()) {
            byHeight[h] = c;
        }
    }
    QList<WebResolver::VideoChoice> out;
    for (auto it = byHeight.cbegin(); it != byHeight.cend(); ++it) {
        out.prepend(it.value());  // 由高到低
    }
    return out;
}

// 字幕軌：作者上傳的全部＋自動產生的原文（"-orig"）。自動翻譯（網址帶 tlang=，YouTube 給 150 多種）不列：2026-11-02 實測
// 不經瀏覽器下載一律 HTTP 429（開發機與頭盔都是），列出來只會選了沒字
QList<WebResolver::SubtitleTrack> subtitlesOf(const QJsonObject& j)
{
    QList<WebResolver::SubtitleTrack> out;
    auto json3 = [](const QJsonArray& a) {
        for (const QJsonValue& v : a) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("ext")).toString() == QLatin1String("json3")) {
                return o;
            }
        }
        return QJsonObject();
    };
    const QJsonObject subs = j.value(QStringLiteral("subtitles")).toObject();
    for (auto it = subs.begin(); it != subs.end(); ++it) {
        if (it.key() == QLatin1String("live_chat")) {
            continue;
        }
        const QJsonObject o = json3(it.value().toArray());
        if (!o.isEmpty()) {
            out.append({it.key(), o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("url")).toString(), false});
        }
    }
    // 作者上傳的排序：台灣繁中、香港繁中、片子本身的語言、英文、簡中在前，其他照語言代碼
    const QString videoLang = j.value(QStringLiteral("language")).toString();
    auto rank = [&videoLang](const QString& lang) {
        if (lang == QLatin1String("zh-TW") || lang == QLatin1String("zh-Hant")) {
            return 0;
        }
        if (lang == QLatin1String("zh-HK")) {
            return 1;
        }
        if (!videoLang.isEmpty() && (lang == videoLang || lang.startsWith(videoLang + QLatin1Char('-')))) {
            return 2;
        }
        if (lang == QLatin1String("en") || lang.startsWith(QLatin1String("en-"))) {
            return 3;
        }
        if (lang.startsWith(QLatin1String("zh"))) {
            return 4;
        }
        return 5;
    };
    std::stable_sort(out.begin(), out.end(), [&rank](const WebResolver::SubtitleTrack& a, const WebResolver::SubtitleTrack& b) {
        return rank(a.lang) < rank(b.lang);
    });
    // 控制列是「點一下換下一個」：TED 這類片有 50 多種語言，全部輪一遍太久。留排在前面的 8 種
    while (out.size() > 8) {
        out.removeLast();
    }
    const QJsonObject autos = j.value(QStringLiteral("automatic_captions")).toObject();
    QStringList wanted;
    for (auto it = autos.begin(); it != autos.end(); ++it) {
        if (it.key().endsWith(QLatin1String("-orig"))) {
            wanted << it.key();
        }
    }
    for (const QString& k : wanted) {
        bool have = false;
        for (const WebResolver::SubtitleTrack& t : out) {
            have = have || (!t.automatic && (t.lang == k || t.lang.startsWith(k + QLatin1Char('-'))));
        }
        const QJsonObject o = json3(autos.value(k).toArray());
        if (!have && !o.isEmpty()) {
            out.append({k, o.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("url")).toString(), true});
        }
        if (out.size() >= 16) {
            break;
        }
    }
    return out;
}

QStringList baseArgs(const QString& script)
{
    return {script, QStringLiteral("--no-warnings"), QStringLiteral("--no-playlist"), QStringLiteral("--ignore-config"),
            QStringLiteral("--no-cache-dir")};
}

}  // namespace

WebResolver::WebResolver(QObject* parent) : QObject(parent) {}

WebResolver::~WebResolver()
{
    if (m_Search != nullptr) {
        m_Search->kill();
        m_Search->waitForFinished(2000);
    }
}

QString WebResolver::scriptPath()
{
    const QString up = updatedPath();
    if (QFileInfo(up).isFile() && QFileInfo(up).size() > 100000) {
        return up;
    }
    if (QFileInfo(QString::fromLatin1(kBundled)).isFile()) {
        return QString::fromLatin1(kBundled);
    }
    return QString();
}

bool WebResolver::needsResolve(const QString& url)
{
    const QUrl u(url);
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return false;
    }
    const QString path = u.path().toLower();
    if (path.startsWith(QLatin1String("/viple/media/"))) {
        return false;
    }
    static const char* const kDirect[] = {".mp4", ".m4v", ".mkv", ".webm", ".mov", ".ts", ".m3u8", ".mpd", ".avi", ".flv"};
    for (const char* e : kDirect) {
        if (path.endsWith(QLatin1String(e))) {
            return false;
        }
    }
    return true;
}

void WebResolver::setResolveCancel(bool cancel)
{
    g_ResolveCancel.store(cancel, std::memory_order_release);
}

WebResolver::Result WebResolver::resolve(const QString& pageUrl, int timeoutMs)
{
    Result r;
    const QString script = scriptPath();
    if (script.isEmpty()) {
        r.error = QStringLiteral("the web video helper (yt-dlp) is not installed");
        return r;
    }
    // §REVIEW-1104：原本一次等滿逾時才 kill，載入中按 B 取消不了；改成週期檢查旗標並殺掉 yt-dlp
    auto cancelled = []() {
        return g_ResolveCancel.load(std::memory_order_acquire);
    };
    if (cancelled()) {
        r.error = QStringLiteral("cancelled");
        return r;
    }
    QProcess p;
    QStringList args = baseArgs(script);
    args << QStringLiteral("-J") << QStringLiteral("-f") << QString::fromLatin1(kFormat) << QStringLiteral("--") << pageUrl;
    p.start(QStringLiteral("python3"), args);
    bool started = false;
    int startWaited = 0;
    while (startWaited < 5000 && !cancelled()) {
        const int step = std::min(200, 5000 - startWaited);
        if (p.waitForStarted(step)) {
            started = true;
            break;
        }
        if (p.state() == QProcess::NotRunning) {
            break;
        }
        startWaited += step;
    }
    if (!started) {
        if (p.state() != QProcess::NotRunning) {
            p.kill();
            p.waitForFinished(2000);
        }
        r.error = cancelled() ? QStringLiteral("cancelled")
                              : QStringLiteral("could not start python3 for yt-dlp");
        return r;
    }
    auto waitUntilDone = [&p, &cancelled](int budget) -> bool {
        int waited = 0;
        for (;;) {
            if (cancelled()) {
                return false;
            }
            const int step = budget < 0 ? 200 : std::min(200, budget - waited);
            if (step <= 0) {
                break;
            }
            if (p.waitForFinished(step) || p.state() == QProcess::NotRunning) {
                return !cancelled();
            }
            if (budget < 0) {
                continue;
            }
            waited += step;
            if (waited >= budget) {
                break;
            }
        }
        return p.state() == QProcess::NotRunning && !cancelled();
    };
    if (cancelled() || !waitUntilDone(timeoutMs)) {
        if (p.state() != QProcess::NotRunning) {
            p.kill();
            p.waitForFinished(2000);
        }
        r.error = cancelled() ? QStringLiteral("cancelled")
                              : QStringLiteral("the site did not answer in time");
        return r;
    }
    const QByteArray out = p.readAllStandardOutput();
    if (p.exitCode() != 0 || out.isEmpty()) {
        QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        const int nl = err.lastIndexOf(QLatin1Char('\n'));
        r.error = nl >= 0 ? err.mid(nl + 1) : err;
        if (r.error.isEmpty()) {
            r.error = QStringLiteral("yt-dlp failed (exit %1)").arg(p.exitCode());
        }
        return r;
    }
    const QJsonObject j = QJsonDocument::fromJson(out).object();
    r.title = j.value(QStringLiteral("title")).toString();
    r.durationSec = j.value(QStringLiteral("duration")).toDouble();
    r.isLive = j.value(QStringLiteral("is_live")).toBool();
    QJsonObject headers;
    const QJsonArray req = j.value(QStringLiteral("requested_formats")).toArray();
    if (req.size() >= 2) {
        // 影像＋音訊分開
        for (const QJsonValue& v : req) {
            const QJsonObject f = v.toObject();
            const bool video = f.value(QStringLiteral("vcodec")).toString() != QLatin1String("none");
            (video ? r.videoUrl : r.audioUrl) = f.value(QStringLiteral("url")).toString();
            if (video) {
                r.projection = projectionOf(f);
            }
            if (headers.isEmpty()) {
                headers = f.value(QStringLiteral("http_headers")).toObject();
            }
        }
    }
    else {
        r.videoUrl = j.value(QStringLiteral("url")).toString();
        headers = j.value(QStringLiteral("http_headers")).toObject();
        r.projection = projectionOf(j);
    }
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (it.key().compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0) {
            r.userAgent = it.value().toString();
        }
        else {
            r.headers += it.key() + QStringLiteral(": ") + it.value().toString() + QStringLiteral("\r\n");
        }
    }
    if (!r.audioUrl.isEmpty()) {
        // 影像、聲音分開的才能只換影像
        r.videoChoices = videoChoicesOf(j);
        for (int i = 0; i < r.videoChoices.size(); i++) {
            if (r.videoChoices[i].url == r.videoUrl) {
                r.videoChoice = i;
            }
        }
        if (r.videoChoice < 0) {
            r.videoChoices.clear();  // 選到的不在清單裡（例如 HLS）：不提供換畫質，免得換過去又換不回來
        }
    }
    r.subtitles = subtitlesOf(j);
    r.ok = !r.videoUrl.isEmpty();
    if (!r.ok) {
        r.error = QStringLiteral("no playable format");
    }
    return r;
}

QString WebResolver::version()
{
    if (!m_Version.isEmpty()) {
        return m_Version;
    }
    const QString script = scriptPath();
    if (script.isEmpty()) {
        return QString();
    }
    QProcess p;
    p.start(QStringLiteral("python3"), {script, QStringLiteral("--version")});
    if (p.waitForFinished(15000) && p.exitCode() == 0) {
        m_Version = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    }
    return m_Version;
}

void WebResolver::search(const QString& query, int n)
{
    const QString script = scriptPath();
    if (script.isEmpty()) {
        emit searchFinished({}, tr("The web video helper (yt-dlp) is not installed. Use Update to download it."));
        return;
    }
    if (m_Search != nullptr) {
        m_Search->kill();
        m_Search->deleteLater();
    }
    m_Search = new QProcess(this);
    QProcess* proc = m_Search;
    connect(proc, &QProcess::finished, this, [this, proc](int code, QProcess::ExitStatus) {
        if (proc != m_Search) {
            return;  // 舊的那一次（被新的搜尋取代）
        }
        m_Search = nullptr;
        proc->deleteLater();
        QVariantList results;
        const QJsonObject j = QJsonDocument::fromJson(proc->readAllStandardOutput()).object();
        for (const QJsonValue& v : j.value(QStringLiteral("entries")).toArray()) {
            const QJsonObject e = v.toObject();
            QString url = e.value(QStringLiteral("url")).toString();
            if (url.isEmpty()) {
                url = QStringLiteral("https://www.youtube.com/watch?v=") + e.value(QStringLiteral("id")).toString();
            }
            QVariantMap m;
            m[QStringLiteral("title")] = e.value(QStringLiteral("title")).toString();
            m[QStringLiteral("url")] = url;
            m[QStringLiteral("channel")] = e.value(QStringLiteral("channel")).toString();
            const int d = static_cast<int>(e.value(QStringLiteral("duration")).toDouble());
            m[QStringLiteral("duration")] = d > 0 ? (d >= 3600 ? QString::asprintf("%d:%02d:%02d", d / 3600, (d / 60) % 60, d % 60)
                                                             : QString::asprintf("%d:%02d", d / 60, d % 60))
                                                  : QString();
            results.append(m);
        }
        emit searchFinished(results, code == 0 ? QString() : tr("Search failed."));
    });
    QStringList args = baseArgs(script);
    args << QStringLiteral("--flat-playlist") << QStringLiteral("-J") << QStringLiteral("--")
         << QStringLiteral("ytsearch%1:%2").arg(n).arg(query);
    proc->start(QStringLiteral("python3"), args);
}

void WebResolver::update()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
    }
    // 先抓 SHA2-256SUMS，再抓 yt-dlp 本體，雜湊對了才換上
    QNetworkReply* sums = m_Nam->get(QNetworkRequest(QUrl(QString::fromLatin1(kReleaseBase) + QStringLiteral("SHA2-256SUMS"))));
    connect(sums, &QNetworkReply::finished, this, [this, sums]() {
        sums->deleteLater();
        QByteArray want;
        if (sums->error() == QNetworkReply::NoError) {
            for (const QByteArray& line : sums->readAll().split('\n')) {
                const QList<QByteArray> parts = line.simplified().split(' ');
                if (parts.size() == 2 && parts.at(1) == "yt-dlp") {
                    want = parts.at(0).toLower();
                }
            }
        }
        if (want.size() != 64) {
            emit updateFinished(false, tr("Could not get the checksum list."));
            return;
        }
        QNetworkReply* bin = m_Nam->get(QNetworkRequest(QUrl(QString::fromLatin1(kReleaseBase) + QStringLiteral("yt-dlp"))));
        connect(bin, &QNetworkReply::finished, this, [this, bin, want]() {
            bin->deleteLater();
            const QByteArray data = bin->readAll();
            if (bin->error() != QNetworkReply::NoError || data.size() < 100000) {
                emit updateFinished(false, tr("Download failed."));
                return;
            }
            if (QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != want) {
                emit updateFinished(false, tr("The download did not match its checksum."));
                return;
            }
            const QString path = updatedPath();
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path + QStringLiteral(".new"));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) {
                emit updateFinished(false, tr("Could not save the update."));
                return;
            }
            f.close();
            // §REVIEW-1104：先把舊檔挪開，確認 .new 換上成功再刪；失敗就搬回並留下 .new，不要回報成功
            const QString neu = path + QStringLiteral(".new");
            const QString bak = path + QStringLiteral(".bak");
            if (QFile::exists(bak) && !QFile::remove(bak)) {
                emit updateFinished(false, tr("Could not save the update."));
                return;
            }
            const bool hadOld = QFile::exists(path);
            if (hadOld && !QFile::rename(path, bak)) {
                emit updateFinished(false, tr("Could not save the update."));
                return;
            }
            if (!QFile::rename(neu, path)) {
                if (hadOld) {
                    QFile::rename(bak, path);
                }
                emit updateFinished(false, tr("Could not save the update."));
                return;
            }
            if (hadOld) {
                QFile::remove(bak);
            }
            m_Version.clear();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] yt-dlp updated (%lld bytes)", static_cast<long long>(data.size()));
            emit updateFinished(true, tr("Updated to %1.").arg(version()));
        });
    });
}
