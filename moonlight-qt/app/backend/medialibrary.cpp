// VipleStream 2.0 §VR-PLAYER — 見 medialibrary.h。

#include "medialibrary.h"
#include "videothumbs.h"

#include "streaming/player/playerformat.h"
#include "backend/computermanager.h"
#include "backend/nvhttp.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <algorithm>
#include <thread>

namespace {

const QStringList& videoSuffixes()
{
    static const QStringList k = {QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mkv"), QStringLiteral("webm"),
                                  QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("ts"),  QStringLiteral("m2ts"),
                                  QStringLiteral("mts"), QStringLiteral("flv"), QStringLiteral("mpg"), QStringLiteral("mpeg")};
    return k;
}

QString sizeText(qint64 bytes)
{
    if (bytes >= (1ll << 30)) {
        return QString::number(bytes / double(1ll << 30), 'f', 1) + QStringLiteral(" GB");
    }
    return QString::number(bytes / double(1ll << 20), 'f', 0) + QStringLiteral(" MB");
}

QString timeText(double sec)
{
    const int t = static_cast<int>(sec < 0 ? 0 : sec);
    const int h = t / 3600, m = (t / 60) % 60, s = t % 60;
    return h > 0 ? QString::asprintf("%d:%02d:%02d", h, m, s) : QString::asprintf("%d:%02d", m, s);
}

}  // namespace

MediaLibrary::MediaLibrary(QObject* parent) : QObject(parent)
{
    connect(&m_Web, &WebResolver::searchFinished, this, &MediaLibrary::webSearchFinished);
    connect(&m_Web, &WebResolver::updateFinished, this, &MediaLibrary::webUpdateFinished);
}

void MediaLibrary::webSearch(const QString& query)
{
    m_Web.search(query.trimmed(), 20);
}

void MediaLibrary::webUpdate()
{
    m_Web.update();
}

QString MediaLibrary::webVersion()
{
    return m_Web.version();
}

QVariantList MediaLibrary::roots() const
{
    QVariantList out;
    auto add = [&](const QString& name, const QString& path) {
        if (!path.isEmpty() && QFileInfo(path).isDir()) {
            QVariantMap m;
            m[QStringLiteral("name")] = name;
            m[QStringLiteral("path")] = QDir(path).absolutePath();
            out.append(m);
        }
    };
    add(tr("Videos"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    add(tr("Downloads"), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    // 外接儲存裝置（SteamOS 掛在 /run/media/<使用者>/<標籤>，有時直接在 /run/media/<標籤>）
    const QDir media(QStringLiteral("/run/media"));
    for (const QFileInfo& fi : media.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fi.fileName() == qEnvironmentVariable("USER")) {
            for (const QFileInfo& d : QDir(fi.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                add(d.fileName(), d.absoluteFilePath());
            }
        }
        else {
            add(fi.fileName(), fi.absoluteFilePath());
        }
    }
    add(tr("Home"), QDir::homePath());
    return out;
}

QVariantList MediaLibrary::listDir(const QString& path) const
{
    QVariantList dirs, files;
    const QDir dir(path);
    if (!dir.exists()) {
        return {};
    }
    for (const QFileInfo& fi : dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                                                 QDir::Name | QDir::IgnoreCase | QDir::DirsFirst)) {
        if (fi.isHidden()) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("name")] = fi.fileName();
        m[QStringLiteral("path")] = fi.absoluteFilePath();
        if (fi.isDir()) {
            m[QStringLiteral("isDir")] = true;
            m[QStringLiteral("detail")] = QString();
            m[QStringLiteral("format")] = QString();
            dirs.append(m);
        }
        else if (videoSuffixes().contains(fi.suffix().toLower())) {
            m[QStringLiteral("isDir")] = false;
            m[QStringLiteral("detail")] = sizeText(fi.size());
            m[QStringLiteral("format")] = guessFormat(fi.fileName());
            files.append(m);
        }
    }
    return dirs + files;
}

QString MediaLibrary::parentDir(const QString& path) const
{
    QDir d(path);
    if (!d.cdUp()) {
        return QString();
    }
    return d.absolutePath() == QDir(path).absolutePath() ? QString() : d.absolutePath();
}

QVariantList MediaLibrary::recent() const
{
    QSettings s;
    s.beginGroup(QStringLiteral("player/history"));
    struct Item {
        qint64 when;
        QVariantMap m;
    };
    std::vector<Item> items;
    for (const QString& key : s.childGroups()) {
        s.beginGroup(key);
        const QString source = s.value(QStringLiteral("source")).toString();
        if (!source.isEmpty()) {
            QVariantMap m;
            const double pos = s.value(QStringLiteral("position"), 0.0).toDouble();
            const double dur = s.value(QStringLiteral("duration"), 0.0).toDouble();
            m[QStringLiteral("title")] = s.value(QStringLiteral("title"), source).toString();
            m[QStringLiteral("source")] = source;
            m[QStringLiteral("position")] = pos;
            m[QStringLiteral("duration")] = dur;
            m[QStringLiteral("format")] = s.value(QStringLiteral("format")).toString();
            m[QStringLiteral("progress")] = dur > 0.0 ? timeText(pos) + QStringLiteral(" / ") + timeText(dur) : timeText(pos);
            items.push_back({s.value(QStringLiteral("lastPlayed"), 0).toLongLong(), m});
        }
        s.endGroup();
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.when > b.when; });
    QVariantList out;
    for (size_t i = 0; i < items.size() && i < 30; i++) {
        out.append(items[i].m);
    }
    return out;
}

void MediaLibrary::forget(const QString& source)
{
    QSettings s;
    s.beginGroup(QStringLiteral("player/history"));
    for (const QString& key : s.childGroups()) {
        if (s.value(key + QStringLiteral("/source")).toString() == source) {
            s.remove(key);
        }
    }
}

QString MediaLibrary::guessFormat(const QString& source) const
{
    const QString name = isUrl(source) ? QUrl(source).path() : QFileInfo(source).fileName();
    playerfmt::Format f;
    if (!playerfmt::fromFileName(name.toStdString(), &f)) {
        return QString();
    }
    return QString::fromStdString(playerfmt::name(f));
}

namespace {

NvComputer* hostAt(ComputerManager* cm, int host)
{
    if (cm == nullptr) {
        return nullptr;
    }
    const QVector<NvComputer*> list = cm->getComputers();
    return host >= 0 && host < list.size() ? list.at(host) : nullptr;
}

// 不在線上回 false（不連）
bool hostOnline(NvComputer* c)
{
    QReadLocker lock(&c->lock);
    return c->state == NvComputer::CS_ONLINE && c->pairState == NvComputer::PS_PAIRED && !c->activeAddress.isNull();
}

}  // namespace

QVariantList MediaLibrary::hosts(ComputerManager* cm) const
{
    QVariantList out;
    if (cm == nullptr) {
        return out;
    }
    const QVector<NvComputer*> list = cm->getComputers();
    for (int i = 0; i < list.size(); i++) {
        NvComputer* c = list.at(i);
        QReadLocker lock(&c->lock);
        if (c->pairState != NvComputer::PS_PAIRED) {
            continue;
        }
        QVariantMap m;
        m[QStringLiteral("index")] = i;
        m[QStringLiteral("name")] = c->name;
        m[QStringLiteral("online")] = c->state == NvComputer::CS_ONLINE;
        out.append(m);
    }
    return out;
}

void MediaLibrary::hostBrowse(ComputerManager* cm, int host, int root, const QString& path, int requestId)
{
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr || !hostOnline(c)) {
        emit hostBrowseFinished(requestId, {}, tr("The PC is not online."));
        return;
    }
    // 連線與讀 XML 在另一條執行緒（主機慢或不通時最多等幾秒，不能讓 GUI 卡住）；結果排回 GUI 執行緒發出
    std::thread([this, c, root, path, requestId]() {
        QVariantList entries;
        QString error;
        try {
            NvHTTP http(c);
            if (root < 0) {
                const QString xml = http.openConnectionToString(http.httpsBaseUrl(), QStringLiteral("viple/media/roots"), QString(), 4000);
                NvHTTP::verifyResponseStatus(xml);
                QXmlStreamReader r(xml);
                QVariantMap cur;
                while (!r.atEnd()) {
                    r.readNext();
                    if (r.isStartElement() && r.name() == QLatin1String("Index")) {
                        cur[QStringLiteral("index")] = r.readElementText().toInt();
                    }
                    else if (r.isStartElement() && r.name() == QLatin1String("Name")) {
                        cur[QStringLiteral("name")] = r.readElementText();
                    }
                    else if (r.isStartElement() && r.name() == QLatin1String("Kind")) {
                        cur[QStringLiteral("drive")] = r.readElementText() == QLatin1String("drive");
                    }
                    else if (r.isEndElement() && r.name() == QLatin1String("Folder")) {
                        entries.append(cur);
                        cur.clear();
                    }
                }
                if (entries.isEmpty()) {
                    // 新版 server 沒設 media_dirs 時是整台 PC；空的＝關掉了分享（media_share = disabled）或 server 太舊
                    error = tr("This PC doesn't share its videos. Update VipleStream Server on that PC, and check that "
                               "media_share isn't disabled in its configuration.");
                }
            }
            else {
                const QString args = QStringLiteral("root=%1&path=%2").arg(root).arg(QString::fromLatin1(QUrl::toPercentEncoding(path)));
                const QString xml = http.openConnectionToString(http.httpsBaseUrl(), QStringLiteral("viple/media/list"), args, 6000);
                NvHTTP::verifyResponseStatus(xml);
                QVariantList dirs, files;
                QXmlStreamReader r(xml);
                QVariantMap cur;
                while (!r.atEnd()) {
                    r.readNext();
                    if (r.isStartElement()) {
                        if (r.name() == QLatin1String("Name")) {
                            cur[QStringLiteral("name")] = r.readElementText();
                        }
                        else if (r.name() == QLatin1String("Dir")) {
                            cur[QStringLiteral("isDir")] = r.readElementText() == QLatin1String("1");
                        }
                        else if (r.name() == QLatin1String("Size")) {
                            cur[QStringLiteral("size")] = r.readElementText().toLongLong();
                        }
                        else if (r.name() == QLatin1String("Mtime")) {
                            cur[QStringLiteral("mtime")] = r.readElementText().toLongLong();
                        }
                    }
                    else if (r.isEndElement() && r.name() == QLatin1String("Entry")) {
                        const QString name = cur.value(QStringLiteral("name")).toString();
                        const bool isDir = cur.value(QStringLiteral("isDir")).toBool();
                        QVariantMap m;
                        m[QStringLiteral("name")] = name;
                        m[QStringLiteral("path")] = path.isEmpty() ? name : path + QLatin1Char('/') + name;
                        m[QStringLiteral("isDir")] = isDir;
                        m[QStringLiteral("detail")] = isDir ? QString() : sizeText(cur.value(QStringLiteral("size")).toLongLong());
                        m[QStringLiteral("format")] = isDir ? QString() : guessFormat(name);
                        // §VR-PLAYER-THUMB：縮圖快取鍵的戳記（主機上同名檔案換掉時換新縮圖）
                        m[QStringLiteral("stamp")] = QStringLiteral("%1_%2").arg(cur.value(QStringLiteral("size")).toLongLong())
                                                         .arg(cur.value(QStringLiteral("mtime")).toLongLong());
                        (isDir ? dirs : files).append(m);
                        cur.clear();
                    }
                }
                entries = dirs + files;
            }
        }
        catch (const std::exception&) {
            error = root < 0 ? tr("Could not reach the PC. Make sure VipleStream Server is running.")
                             : tr("Could not read this folder from the PC.");
        }
        QMetaObject::invokeMethod(this, [this, requestId, entries, error]() { emit hostBrowseFinished(requestId, entries, error); },
                                  Qt::QueuedConnection);
    }).detach();
}

QString MediaLibrary::hostFileUrl(ComputerManager* cm, int host, int root, const QString& path) const
{
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr) {
        return QString();
    }
    NvHTTP http(c);
    QUrl url = http.httpsBaseUrl();
    url.setPath(QStringLiteral("/viple/media/file"));
    url.setQuery(QStringLiteral("root=%1&path=%2").arg(root).arg(QString::fromLatin1(QUrl::toPercentEncoding(path))),
                 QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded);
}

QString MediaLibrary::hostThumbId(ComputerManager* cm, int host, int root, const QString& path, const QString& stamp) const
{
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr) {
        return QString();
    }
    QString uuid;
    {
        QReadLocker lock(&c->lock);
        uuid = c->uuid;
    }
    // 清單的索引會變（主機上下線、排序），id 用 uuid
    return QStringLiteral("host/%1/%2/%3/%4").arg(uuid).arg(root).arg(VideoThumbProvider::encodeId(stamp)).arg(VideoThumbProvider::encodeId(path));
}

bool MediaLibrary::isUrl(const QString& text) const
{
    const QString t = text.trimmed();
    return t.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || t.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}
