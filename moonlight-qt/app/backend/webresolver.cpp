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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace {

const char* const kBundled = "/app/share/viplestream/yt-dlp";
const char* const kReleaseBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";

QString updatedPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/yt-dlp");
}

// 頭盔能硬體解的：不要 AV1、不要 HDR、最高 2160p；影像＋音訊分開優先，沒有就整條的
const char* const kFormat = "bv*[vcodec!*=av01][dynamic_range=SDR][height<=2160]+ba/b[vcodec!*=av01][dynamic_range=SDR]/b";

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

WebResolver::Result WebResolver::resolve(const QString& pageUrl, int timeoutMs)
{
    Result r;
    const QString script = scriptPath();
    if (script.isEmpty()) {
        r.error = QStringLiteral("the web video helper (yt-dlp) is not installed");
        return r;
    }
    QProcess p;
    QStringList args = baseArgs(script);
    args << QStringLiteral("-J") << QStringLiteral("-f") << QString::fromLatin1(kFormat) << QStringLiteral("--") << pageUrl;
    p.start(QStringLiteral("python3"), args);
    if (!p.waitForStarted(5000)) {
        r.error = QStringLiteral("could not start python3 for yt-dlp");
        return r;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        r.error = QStringLiteral("the site did not answer in time");
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
            if (headers.isEmpty()) {
                headers = f.value(QStringLiteral("http_headers")).toObject();
            }
        }
    }
    else {
        r.videoUrl = j.value(QStringLiteral("url")).toString();
        headers = j.value(QStringLiteral("http_headers")).toObject();
    }
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (it.key().compare(QLatin1String("User-Agent"), Qt::CaseInsensitive) == 0) {
            r.userAgent = it.value().toString();
        }
        else {
            r.headers += it.key() + QStringLiteral(": ") + it.value().toString() + QStringLiteral("\r\n");
        }
    }
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
            QFile::remove(path);
            QFile::rename(path + QStringLiteral(".new"), path);
            m_Version.clear();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] yt-dlp updated (%lld bytes)", static_cast<long long>(data.size()));
            emit updateFinished(true, tr("Updated to %1.").arg(version()));
        });
    });
}
