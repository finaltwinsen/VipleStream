// VipleStream 2.0 §VR-PLAYER — 見 medialibrary.h。

#include "medialibrary.h"
#include "videothumbs.h"

#include "streaming/player/playerformat.h"
#include "backend/computermanager.h"
#include "backend/nvhttp.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QVariantMap>
#include <QXmlStreamReader>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
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

// §REVIEW-1104：主機排序索引會在新增或改名時變動，改以 uuid 查找主機
NvComputer* hostAt(ComputerManager* cm, const QString& host)
{
    if (cm == nullptr) {
        return nullptr;
    }
    const QVector<NvComputer*> list = cm->getComputers();
    for (NvComputer* c : list) {
        QReadLocker lock(&c->lock);
        if (c->uuid == host) {
            return c;
        }
    }
    return nullptr;
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
        // §REVIEW-1104：回傳 uuid 供 QML 保存與查詢，避免排序索引失效
        m[QStringLiteral("uuid")] = c->uuid;
        m[QStringLiteral("name")] = c->name;
        m[QStringLiteral("online")] = c->state == NvComputer::CS_ONLINE;
        out.append(m);
    }
    return out;
}

// §REVIEW-1104：host 參數改收 uuid
void MediaLibrary::hostBrowse(ComputerManager* cm, const QString& host, int root, const QString& path, int requestId)
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
                // §REVIEW-1118B：host 接受了配對憑證。先記下端點，「最近播放」裡這台主機的舊網址才播得起來
                rememberCertEndpoint(http.httpsBaseUrl());
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
                    // §MEDIA-FILEOPS：舊 server 沒有 <Writable>。缺欄位不能當成可寫，否則頭盔會對唯讀分享送出刪除。
                    else if (r.isStartElement() && r.name() == QLatin1String("Writable")) {
                        cur[QStringLiteral("writable")] = r.readElementText().trimmed() == QLatin1String("1");
                    }
                    else if (r.isEndElement() && r.name() == QLatin1String("Folder")) {
                        if (!cur.contains(QStringLiteral("writable"))) {
                            cur[QStringLiteral("writable")] = false;
                        }
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

namespace {
const QString kCertEndpointsKey = QStringLiteral("player/certendpoints");
const int kCertEndpointsMax = 32;

QString certEndpointOf(const QUrl& url)
{
    if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0 || url.host().isEmpty()) {
        return QString();
    }
    return url.host().toLower() + QLatin1Char(':') + QString::number(url.port(443));
}
}

void MediaLibrary::rememberCertEndpoint(const QUrl& httpsBase)
{
    const QString ep = certEndpointOf(httpsBase);
    if (ep.isEmpty()) {
        return;
    }
    // 瀏覽執行緒與 GUI 執行緒都會進來；已經記過的端點不重寫設定檔
    static std::mutex lock;
    static QStringList known;
    static bool loaded = false;
    std::lock_guard<std::mutex> lk(lock);
    QSettings settings;
    if (!loaded) {
        known = settings.value(kCertEndpointsKey).toStringList();
        loaded = true;
    }
    if (!known.isEmpty() && known.last() == ep) {
        return;
    }
    known.removeAll(ep);
    known.append(ep);
    while (known.size() > kCertEndpointsMax) {
        known.removeFirst();
    }
    settings.setValue(kCertEndpointsKey, known);
}

bool MediaLibrary::isCertEndpoint(const QUrl& url)
{
    const QString ep = certEndpointOf(url);
    if (ep.isEmpty() || url.path() != QLatin1String("/viple/media/file")) {
        return false;
    }
    return QSettings().value(kCertEndpointsKey).toStringList().contains(ep);
}

// §REVIEW-1104：host 參數改收 uuid
QString MediaLibrary::hostFileUrl(ComputerManager* cm, const QString& host, int root, const QString& path) const
{
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr) {
        return QString();
    }
    NvHTTP http(c);
    QUrl url = http.httpsBaseUrl();
    if (c->pairState == NvComputer::PS_PAIRED) {
        rememberCertEndpoint(url);  // §REVIEW-1118B
    }
    url.setPath(QStringLiteral("/viple/media/file"));
    url.setQuery(QStringLiteral("root=%1&path=%2").arg(root).arg(QString::fromLatin1(QUrl::toPercentEncoding(path))),
                 QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded);
}

// §REVIEW-1104：host 參數改收 uuid
QString MediaLibrary::hostThumbId(ComputerManager* cm, const QString& host, int root, const QString& path, const QString& stamp) const
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

// ── §MEDIA-FILEOPS ──────────────────────────────────────────────────────────
// 本機與 host 的失敗都用同一組英文字串，介面才對得上翻譯。背景執行緒只做檔案或 HTTPS，
// 訊號一律 invokeMethod 回 GUI（與 hostBrowseFinished 相同）。this 的壽命也沿用那條：
// detach 的執行緒不另做看守，避免一條新的拆掉路徑跟既有瀏覽執行緒不一致。

namespace {

constexpr qint64 kCopyChunk = 4 * 1024 * 1024;
constexpr int kMaxDepth = 128;
constexpr int kOpTimeoutMs = 30000;
constexpr int kStatusTimeoutMs = 8000;
constexpr int kCancelTimeoutMs = 4000;
constexpr int kStatusFails = 3;

using ProgressFn = std::function<void(qint64, qint64, bool)>;

QString protocolError(const QString& message)
{
    const QString m = message.trimmed();
    if (m == QLatin1String("read-only") || m == QLatin1String("not found") || m == QLatin1String("bad name")
        || m == QLatin1String("bad request") || m == QLatin1String("exists") || m == QLatin1String("into itself")
        || m == QLatin1String("busy") || m == QLatin1String("no space") || m == QLatin1String("cancelled")) {
        return m;
    }
    if (m.startsWith(QLatin1String("failed:"))) {
        return m;
    }
    if (m.isEmpty()) {
        return QStringLiteral("failed: unknown");
    }
    return QStringLiteral("failed: ") + m;
}

// QFile::errorString() 會本地化，也常把整條路徑拼進去。協定要英文、而且不把使用者路徑送去顯示。
QString ioFailed(QFileDevice::FileError err)
{
    switch (err) {
    case QFileDevice::ReadError:
        return QStringLiteral("failed: read error");
    case QFileDevice::WriteError:
        return QStringLiteral("failed: write error");
    case QFileDevice::FatalError:
        return QStringLiteral("failed: fatal error");
    case QFileDevice::ResourceError:
        return QStringLiteral("failed: resource error");
    case QFileDevice::OpenError:
        return QStringLiteral("failed: open error");
    case QFileDevice::AbortError:
        return QStringLiteral("failed: aborted");
    case QFileDevice::TimeOutError:
        return QStringLiteral("failed: timeout");
    case QFileDevice::RemoveError:
        return QStringLiteral("failed: remove error");
    case QFileDevice::RenameError:
        return QStringLiteral("failed: rename error");
    case QFileDevice::PositionError:
        return QStringLiteral("failed: position error");
    case QFileDevice::ResizeError:
        return QStringLiteral("failed: resize error");
    case QFileDevice::PermissionsError:
        return QStringLiteral("failed: permission denied");
    case QFileDevice::CopyError:
        return QStringLiteral("failed: copy error");
    case QFileDevice::UnspecifiedError:
        return QStringLiteral("failed: unspecified error");
    case QFileDevice::NoError:
        break;
    }
    return QStringLiteral("failed: unknown");
}

// QNetworkReply::errorString() 會把整條網址（含 path）拼進去。錯誤只留英文類別，不把路徑送到畫面上。
QString networkFailed(QNetworkReply::NetworkError err)
{
    switch (err) {
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("failed: timeout");
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("failed: host not found");
    case QNetworkReply::ConnectionRefusedError:
        return QStringLiteral("failed: connection refused");
    default:
        return QStringLiteral("failed: network error");
    }
}

bool samePath(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

Qt::CaseSensitivity pathCase()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

// root 本身也算「在起點底下」。用補上分隔字元的前綴，避免 /Videos 對上 /Videos2。
bool pathInside(const QString& rootIn, const QString& pathIn)
{
    const QString root = QDir::cleanPath(rootIn);
    const QString path = QDir::cleanPath(pathIn);
    if (root.isEmpty() || path.isEmpty()) {
        return false;
    }
    if (samePath(root, path)) {
        return true;
    }
    QString prefix = root;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
    return path.startsWith(prefix, pathCase());
}

struct RootRef {
    QString absolute;
    QString canonical;
};

QList<RootRef> rootRefsOf(const MediaLibrary* lib)
{
    QList<RootRef> out;
    const QVariantList list = lib->roots();
    for (const QVariant& v : list) {
        const QString abs = QDir::cleanPath(v.toMap().value(QStringLiteral("path")).toString());
        if (abs.isEmpty()) {
            continue;
        }
        RootRef r;
        r.absolute = abs;
        const QString canon = QFileInfo(abs).canonicalFilePath();
        r.canonical = canon.isEmpty() ? abs : QDir::cleanPath(canon);
        out.append(r);
    }
    return out;
}

bool underAny(const QList<RootRef>& roots, const QString& path)
{
    for (const RootRef& r : roots) {
        if (pathInside(r.canonical, path) || pathInside(r.absolute, path)) {
            return true;
        }
    }
    return false;
}

// 起點本身（含「Home」與巢狀的「Videos」）不能當刪除或移動的目標。比對絕對路徑與正規化路徑，
// 符號連結起點的連結路徑與目標路徑都算。
bool isRootPath(const QList<RootRef>& roots, const QString& path)
{
    const QString p = QDir::cleanPath(path);
    if (p.isEmpty()) {
        return false;
    }
    for (const RootRef& r : roots) {
        if (samePath(p, r.absolute) || samePath(p, r.canonical)) {
            return true;
        }
    }
    return false;
}

// mkdir 的名稱，以及複製時要建立的每一個名稱。與 host 的 bad name 同一套，避免兩邊行為岔開。
QString checkName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return QStringLiteral("bad name");
    }
    if (name.toUtf8().size() > 255) {
        return QStringLiteral("bad name");
    }
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7F || c.category() == QChar::Other_Control) {
            return QStringLiteral("bad name");
        }
        switch (u) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            return QStringLiteral("bad name");
        default:
            break;
        }
    }
    return QString();
}

bool entryExists(const QString& path)
{
    const QFileInfo fi(path);
    // exists() 會跟著連結走，斷掉的連結要另外看 isSymLink，否則會蓋掉它。
    return fi.exists() || fi.isSymLink();
}

QFileInfoList childEntries(const QString& path)
{
    // 含隱藏檔與非影片。不要加 NoSymLinks：連結要被看見，但是呼叫端不准跟著走進去。
    return QDir(path).entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

struct LocalTarget {
    QString error;
    QString path;
    QString canonical;
    bool isSymLink = false;
    bool isDir = false;
    bool isRoot = false;
};

// 最後一段若是符號連結，正規化只做到父資料夾。連結的目標可能在起點外面，刪的是連結本身，不能跟著走。
LocalTarget resolveTarget(const QString& userPath, const QList<RootRef>& roots)
{
    LocalTarget t;
    if (userPath.isEmpty() || userPath.contains(QChar(0))) {
        t.error = QStringLiteral("not found");
        return t;
    }
    const QFileInfo fi(QDir::cleanPath(userPath));
    if (fi.isSymLink()) {
        const QString parentCanon = QFileInfo(fi.absolutePath()).canonicalFilePath();
        const QString name = fi.fileName();
        if (parentCanon.isEmpty() || !underAny(roots, parentCanon) || name.isEmpty() || name == QLatin1String(".")
            || name == QLatin1String("..") || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            t.error = QStringLiteral("not found");
            return t;
        }
        t.path = QDir::cleanPath(QDir(parentCanon).filePath(name));
        t.canonical = t.path;
        t.isSymLink = true;
        t.isRoot = isRootPath(roots, t.path) || isRootPath(roots, QDir::cleanPath(fi.absoluteFilePath()));
        return t;
    }
    const QString canon = fi.canonicalFilePath();
    if (canon.isEmpty() || !underAny(roots, canon)) {
        t.error = QStringLiteral("not found");
        return t;
    }
    t.path = QDir::cleanPath(canon);
    t.canonical = t.path;
    t.isDir = QFileInfo(t.path).isDir();
    t.isRoot = isRootPath(roots, t.canonical) || isRootPath(roots, QDir::cleanPath(fi.absoluteFilePath()));
    return t;
}

// 目的資料夾要真正落地在某個起點裡。連結只接受「目標也在起點內」的那一種，指到外面的直接拒絕。
QString resolveDir(const QString& userPath, const QList<RootRef>& roots, QString* error)
{
    if (userPath.isEmpty() || userPath.contains(QChar(0))) {
        *error = QStringLiteral("not found");
        return QString();
    }
    const QFileInfo fi(QDir::cleanPath(userPath));
    const QString canon = fi.canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir() || !underAny(roots, canon)) {
        *error = QStringLiteral("not found");
        return QString();
    }
    if (!QFileInfo(canon).isWritable()) {
        *error = QStringLiteral("read-only");
        return QString();
    }
    return QDir::cleanPath(canon);
}

bool removeTree(const QString& path, std::atomic<bool>* cancel, qint64* done, qint64 total, const ProgressFn& progress, QString* error,
                int depth)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        *error = QStringLiteral("cancelled");
        return false;
    }
    if (depth > kMaxDepth) {
        *error = QStringLiteral("failed: folder is too deep");
        return false;
    }
    const QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink()) {
        return true;
    }
    // 連結（含 Windows junction）只移掉連結，不能 rmdir 進目標。
    if (fi.isSymLink() || !fi.isDir()) {
        bool ok = QFile::remove(path);
        if (!ok && fi.isSymLink()) {
            ok = QDir(fi.absolutePath()).rmdir(fi.fileName());
        }
        if (!ok) {
            *error = QStringLiteral("failed: remove error");
            return false;
        }
        if (done != nullptr) {
            *done += 1;
            if (progress) {
                progress(*done, total, false);
            }
        }
        return true;
    }
    const QFileInfoList kids = childEntries(path);
    for (const QFileInfo& kid : kids) {
        if (!removeTree(kid.absoluteFilePath(), cancel, done, total, progress, error, depth + 1)) {
            return false;
        }
    }
    if (!QDir(fi.absolutePath()).rmdir(fi.fileName())) {
        *error = QStringLiteral("failed: remove error");
        return false;
    }
    if (done != nullptr) {
        *done += 1;
        if (progress) {
            progress(*done, total, false);
        }
    }
    return true;
}

qint64 countTree(const QString& path, std::atomic<bool>* cancel, QString* error, int depth)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        *error = QStringLiteral("cancelled");
        return -1;
    }
    if (depth > kMaxDepth) {
        *error = QStringLiteral("failed: folder is too deep");
        return -1;
    }
    const QFileInfo fi(path);
    if (fi.isSymLink()) {
        return 1;
    }
    if (!fi.exists()) {
        *error = QStringLiteral("not found");
        return -1;
    }
    if (!fi.isDir()) {
        return 1;
    }
    qint64 n = 1;
    const qint64 kMax = std::numeric_limits<qint64>::max();
    const QFileInfoList kids = childEntries(path);
    for (const QFileInfo& kid : kids) {
        const qint64 c = countTree(kid.absoluteFilePath(), cancel, error, depth + 1);
        if (c < 0) {
            return -1;
        }
        if (c > kMax - n) {
            *error = QStringLiteral("failed: folder is too large");
            return -1;
        }
        n += c;
    }
    return n;
}

qint64 sumSize(const QString& path, std::atomic<bool>* cancel, QString* error, int depth)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
        *error = QStringLiteral("cancelled");
        return -1;
    }
    if (depth > kMaxDepth) {
        *error = QStringLiteral("failed: folder is too deep");
        return -1;
    }
    const QFileInfo fi(path);
    if (fi.isSymLink()) {
        return 0;
    }
    if (!fi.exists()) {
        *error = QStringLiteral("not found");
        return -1;
    }
    if (fi.isDir()) {
        qint64 n = 0;
        const qint64 kMax = std::numeric_limits<qint64>::max();
        const QFileInfoList kids = childEntries(path);
        for (const QFileInfo& kid : kids) {
            const qint64 c = sumSize(kid.absoluteFilePath(), cancel, error, depth + 1);
            if (c < 0) {
                return -1;
            }
            if (c > kMax - n) {
                *error = QStringLiteral("failed: folder is too large");
                return -1;
            }
            n += c;
        }
        return n;
    }
    const qint64 sz = fi.size();
    if (sz < 0) {
        *error = QStringLiteral("failed: read error");
        return -1;
    }
    return sz;
}

QString ensureSpace(const QString& destDir, qint64 need)
{
    // 看不準可用空間就拒絕。估計錯了還去複製，可能寫到一半才爆，清不乾淨的風險比較大。
    const QStorageInfo storage(destDir);
    if (!storage.isValid() || !storage.isReady()) {
        return QStringLiteral("failed: cannot determine free space");
    }
    if (storage.isReadOnly()) {
        return QStringLiteral("read-only");
    }
    const qint64 avail = storage.bytesAvailable();
    if (avail < 0) {
        return QStringLiteral("failed: cannot determine free space");
    }
    if (need > avail) {
        return QStringLiteral("no space");
    }
    return QString();
}

QString copyTree(const QString& src, const QString& dst, const QList<RootRef>& roots, std::atomic<bool>* cancel, qint64* done, qint64 total,
                 const ProgressFn& progress, bool* createdTop, int depth)
{
    if (cancel->load(std::memory_order_relaxed)) {
        return QStringLiteral("cancelled");
    }
    if (depth > kMaxDepth) {
        return QStringLiteral("failed: folder is too deep");
    }
    const QFileInfo fi(src);
    if (fi.isSymLink()) {
        return QString();
    }
    if (!fi.exists()) {
        return QStringLiteral("not found");
    }
    const QString dstClean = QDir::cleanPath(dst);
    if (!underAny(roots, dstClean)) {
        return QStringLiteral("not found");
    }
    const QString nameErr = checkName(QFileInfo(dstClean).fileName());
    if (!nameErr.isEmpty()) {
        return nameErr;
    }
    if (entryExists(dstClean)) {
        return QStringLiteral("exists");
    }

    if (fi.isDir()) {
        if (!QDir(QFileInfo(dstClean).absolutePath()).mkdir(QFileInfo(dstClean).fileName())) {
            if (entryExists(dstClean)) {
                return QStringLiteral("exists");
            }
            return QStringLiteral("failed: cannot create folder");
        }
        if (createdTop != nullptr) {
            *createdTop = true;
        }
        const QFileInfoList kids = childEntries(src);
        for (const QFileInfo& kid : kids) {
            const QString sub = QDir(dstClean).filePath(kid.fileName());
            const QString err = copyTree(kid.absoluteFilePath(), sub, roots, cancel, done, total, progress, nullptr, depth + 1);
            if (!err.isEmpty()) {
                return err;
            }
        }
        return QString();
    }

    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        return ioFailed(in.error() == QFileDevice::NoError ? QFileDevice::OpenError : in.error());
    }
    QFile out(dstClean);
    // NewOnly：目的已存在就失敗，不覆蓋。上面先查過一次，這裡擋住檢查之後才出現的同名項目。
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (entryExists(dstClean)) {
            return QStringLiteral("exists");
        }
        return ioFailed(out.error() == QFileDevice::NoError ? QFileDevice::OpenError : out.error());
    }
    if (createdTop != nullptr) {
        *createdTop = true;
    }
    const qint64 kMax = std::numeric_limits<qint64>::max();
    while (!in.atEnd()) {
        if (cancel->load(std::memory_order_relaxed)) {
            return QStringLiteral("cancelled");
        }
        const QByteArray buf = in.read(kCopyChunk);
        if (buf.isEmpty()) {
            if (in.error() != QFileDevice::NoError) {
                return ioFailed(in.error());
            }
            break;
        }
        const qint64 got = buf.size();
        qint64 off = 0;
        while (off < got) {
            const qint64 n = out.write(buf.constData() + off, got - off);
            if (n <= 0) {
                return ioFailed(out.error() == QFileDevice::NoError ? QFileDevice::WriteError : out.error());
            }
            off += n;
        }
        if (got > kMax - *done) {
            return QStringLiteral("failed: folder is too large");
        }
        *done += got;
        if (progress) {
            progress(*done, total, false);
        }
    }
    if (!out.flush()) {
        return ioFailed(out.error() == QFileDevice::NoError ? QFileDevice::WriteError : out.error());
    }
    out.close();
    in.close();
    return QString();
}

QString cleanupCreated(const QString& created, const QList<RootRef>& roots)
{
    if (created.isEmpty()) {
        return QString();
    }
    // 清的是這次建立的目的。路徑不在起點裡就寧願留下，也不要刪到分享範圍外面。
    if (!underAny(roots, created) || isRootPath(roots, created)) {
        return QStringLiteral("failed: cleanup failed");
    }
    QString ignore;
    if (!removeTree(created, nullptr, nullptr, 0, ProgressFn(), &ignore, 0)) {
        return QStringLiteral("failed: cleanup failed");
    }
    return QString();
}

bool knownOp(const QString& op)
{
    return op == QLatin1String("mkdir") || op == QLatin1String("delete") || op == QLatin1String("copy") || op == QLatin1String("move");
}

QString runLocal(MediaLibrary* lib, std::atomic<bool>* cancel, const QString& op, const QString& path, const QString& destDir,
                 const QString& name, const ProgressFn& progress)
{
    const QList<RootRef> roots = rootRefsOf(lib);
    if (roots.isEmpty()) {
        return QStringLiteral("not found");
    }
    if (cancel->load(std::memory_order_relaxed)) {
        return QStringLiteral("cancelled");
    }

    if (op == QLatin1String("mkdir")) {
        const QString nameErr = checkName(name);
        if (!nameErr.isEmpty()) {
            return nameErr;
        }
        QString dirErr;
        const QString parent = resolveDir(path, roots, &dirErr);
        if (parent.isEmpty()) {
            return dirErr;
        }
        const QString dst = QDir::cleanPath(QDir(parent).filePath(name));
        if (!underAny(roots, dst) || isRootPath(roots, dst)) {
            return QStringLiteral("not found");
        }
        if (entryExists(dst)) {
            return QStringLiteral("exists");
        }
        if (!QDir(parent).mkdir(name)) {
            if (entryExists(dst)) {
                return QStringLiteral("exists");
            }
            return QStringLiteral("failed: cannot create folder");
        }
        progress(1, 1, true);
        return QString();
    }

    const LocalTarget src = resolveTarget(path, roots);
    if (!src.error.isEmpty()) {
        return src.error;
    }
    if ((op == QLatin1String("delete") || op == QLatin1String("move")) && src.isRoot) {
        return QStringLiteral("bad request");
    }

    if (op == QLatin1String("delete")) {
        const QFileInfo parent(QFileInfo(src.path).absolutePath());
        if (!parent.exists()) {
            return QStringLiteral("not found");
        }
        if (!parent.isWritable()) {
            return QStringLiteral("read-only");
        }
        QString countErr;
        const qint64 total = countTree(src.path, cancel, &countErr, 0);
        if (total < 0) {
            return countErr;
        }
        progress(0, total, true);
        qint64 done = 0;
        QString removeErr;
        if (!removeTree(src.path, cancel, &done, total, progress, &removeErr, 0)) {
            return removeErr;
        }
        progress(total, total, true);
        return QString();
    }

    if (op != QLatin1String("copy") && op != QLatin1String("move")) {
        return QStringLiteral("bad request");
    }

    QString dirErr;
    const QString dest = resolveDir(destDir, roots, &dirErr);
    if (dest.isEmpty()) {
        return dirErr;
    }
    const QString srcName = QFileInfo(src.path).fileName();
    const QString nameErr = checkName(srcName);
    if (!nameErr.isEmpty()) {
        return nameErr;
    }
    const QString dst = QDir::cleanPath(QDir(dest).filePath(srcName));
    if (!underAny(roots, dst) || isRootPath(roots, dst)) {
        return QStringLiteral("not found");
    }
    if (entryExists(dst)) {
        return QStringLiteral("exists");
    }
    if (!src.isSymLink && src.isDir && pathInside(src.canonical, dest)) {
        return QStringLiteral("into itself");
    }

    if (src.isSymLink) {
        // 複製略過連結。使用者直接選了一個連結時不能報成功卻什麼都沒產生。
        // 跨磁碟區的移動若改走「複製再刪」，複製會略過、刪除卻會把來源連結清掉。
        if (op == QLatin1String("copy")) {
            return QStringLiteral("failed: symbolic link");
        }
        if (!QFile::rename(src.path, dst)) {
            return QStringLiteral("failed: cannot move symbolic link");
        }
        progress(1, 1, true);
        return QString();
    }

    if (op == QLatin1String("move")) {
        const bool renamed = src.isDir ? QDir().rename(src.path, dst) : QFile::rename(src.path, dst);
        if (renamed) {
            progress(1, 1, true);
            return QString();
        }
        if (cancel->load(std::memory_order_relaxed)) {
            return QStringLiteral("cancelled");
        }
    }

    QString sizeErr;
    const qint64 total = sumSize(src.path, cancel, &sizeErr, 0);
    if (total < 0) {
        return sizeErr;
    }
    const QString spaceErr = ensureSpace(dest, total);
    if (!spaceErr.isEmpty()) {
        return spaceErr;
    }
    progress(0, total, true);
    qint64 done = 0;
    bool created = false;
    const QString copyErr = copyTree(src.path, dst, roots, cancel, &done, total, progress, &created, 0);
    if (!copyErr.isEmpty()) {
        if (created) {
            const QString cleaned = cleanupCreated(dst, roots);
            if (!cleaned.isEmpty()) {
                return cleaned;
            }
        }
        return copyErr;
    }
    if (op == QLatin1String("move") && cancel->load(std::memory_order_relaxed)) {
        const QString cleaned = cleanupCreated(created ? dst : QString(), roots);
        return cleaned.isEmpty() ? QStringLiteral("cancelled") : cleaned;
    }
    if (op == QLatin1String("move")) {
        // 複製已經成功才刪來源。刪到一半不能反悔，所以這段不再看取消旗標。
        QString removeErr;
        if (!removeTree(src.path, nullptr, nullptr, 0, ProgressFn(), &removeErr, 0)) {
            return removeErr.isEmpty() ? QStringLiteral("failed: remove error") : removeErr;
        }
    }
    progress(total > 0 ? total : 1, total > 0 ? total : 1, true);
    return QString();
}

QString percentEncode(const QString& s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QString hostOpQuery(const QString& op, int root, const QString& path, int droot, const QString& dpath, const QString& name)
{
    if (op == QLatin1String("mkdir")) {
        return QStringLiteral("op=mkdir&root=") + QString::number(root) + QStringLiteral("&path=") + percentEncode(path)
            + QStringLiteral("&name=") + percentEncode(name);
    }
    if (op == QLatin1String("delete")) {
        return QStringLiteral("op=delete&root=") + QString::number(root) + QStringLiteral("&path=") + percentEncode(path);
    }
    return QStringLiteral("op=") + op + QStringLiteral("&root=") + QString::number(root) + QStringLiteral("&path=") + percentEncode(path)
        + QStringLiteral("&droot=") + QString::number(droot) + QStringLiteral("&dpath=") + percentEncode(dpath);
}

void requestHostCancel(ComputerManager* cm, const QString& host, qint64 job)
{
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr || !hostOnline(c)) {
        return;
    }
    try {
        // 與 hostBrowse 一樣在這條執行緒上建 NvHTTP。NVLL_ERROR：平常不要把帶路徑的網址打進 log。
        NvHTTP http(c);
        const QString args = QStringLiteral("id=") + QString::number(job);
        const QString xml = http.openConnectionToString(http.httpsBaseUrl(), QStringLiteral("viple/media/opcancel"), args, kCancelTimeoutMs,
                                                        NvHTTP::NVLL_ERROR);
        NvHTTP::verifyResponseStatus(xml);
    } catch (const std::exception&) {
    }
}

struct HostStatus {
    QString state;
    qint64 done = 0;
    qint64 total = 0;
    QString error;
};

HostStatus parseHostStatus(const QString& xml)
{
    HostStatus s;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement()) {
            continue;
        }
        if (r.name() == QLatin1String("State")) {
            s.state = r.readElementText().trimmed();
        }
        else if (r.name() == QLatin1String("Done")) {
            s.done = r.readElementText().trimmed().toLongLong();
        }
        else if (r.name() == QLatin1String("Total")) {
            s.total = r.readElementText().trimmed().toLongLong();
        }
        else if (r.name() == QLatin1String("Error")) {
            s.error = r.readElementText();
        }
    }
    return s;
}

QString statusToError(const HostStatus& s)
{
    if (s.state == QLatin1String("done")) {
        return QString();
    }
    if (s.state == QLatin1String("cancelled")) {
        return s.error.trimmed().isEmpty() ? QStringLiteral("cancelled") : protocolError(s.error);
    }
    if (s.state == QLatin1String("failed")) {
        return protocolError(s.error);
    }
    return QStringLiteral("failed: bad status");
}

}  // namespace

void MediaLibrary::postProgress(qint64 done, qint64 total)
{
    QMetaObject::invokeMethod(this, [this, done, total]() { emit opProgress(done, total); }, Qt::QueuedConnection);
}

void MediaLibrary::finishOp(const QString& error)
{
    QMetaObject::invokeMethod(
        this,
        [this, error]() {
            m_OpBusy = false;
            m_OpCm = nullptr;
            m_OpHost.clear();
            m_HostJob.store(0, std::memory_order_release);
            emit opBusyChanged();
            emit opFinished(error);
        },
        Qt::QueuedConnection);
}

bool MediaLibrary::localWritable(const QString& dir) const
{
    if (dir.isEmpty() || dir.contains(QChar(0))) {
        return false;
    }
    const QString canon = QFileInfo(QDir::cleanPath(dir)).canonicalFilePath();
    if (canon.isEmpty()) {
        return false;
    }
    const QFileInfo fi(canon);
    // 規格就是這兩件事：在起點底下，而且這個資料夾檔案系統說可以寫。
    if (!fi.isDir() || !fi.isWritable()) {
        return false;
    }
    return underAny(rootRefsOf(this), canon);
}

void MediaLibrary::localOp(const QString& op, const QString& path, const QString& destDir, const QString& name)
{
    if (m_OpBusy) {
        emit opFinished(QStringLiteral("busy"));
        return;
    }
    if (!knownOp(op)) {
        emit opFinished(QStringLiteral("bad request"));
        return;
    }
    m_Cancel.store(false, std::memory_order_release);
    m_HostJob.store(0, std::memory_order_release);
    m_OpCm = nullptr;
    m_OpHost.clear();
    m_OpGen.fetch_add(1, std::memory_order_acq_rel);
    m_OpBusy = true;
    emit opBusyChanged();

    std::thread([this, op, path, destDir, name]() {
        QString error;
        try {
            qint64 lastMs = -1000;
            const ProgressFn progress = [this, &lastMs](qint64 done, qint64 total, bool force) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (!force && now - lastMs < 200) {
                    return;
                }
                lastMs = now;
                postProgress(done, total);
            };
            error = runLocal(this, &m_Cancel, op, path, destDir, name, progress);
        } catch (const std::exception& e) {
            error = protocolError(QString::fromUtf8(e.what()));
        } catch (...) {
            error = QStringLiteral("failed: unknown");
        }
        finishOp(error);
    }).detach();
}

void MediaLibrary::hostOp(ComputerManager* cm, const QString& host, const QString& op, int root, const QString& path, int droot,
                          const QString& dpath, const QString& name)
{
    if (m_OpBusy) {
        emit opFinished(QStringLiteral("busy"));
        return;
    }
    if (!knownOp(op)) {
        emit opFinished(QStringLiteral("bad request"));
        return;
    }
    NvComputer* c = hostAt(cm, host);
    if (c == nullptr || !hostOnline(c)) {
        emit opFinished(QStringLiteral("failed: The PC is not online."));
        return;
    }
    m_Cancel.store(false, std::memory_order_release);
    m_HostJob.store(0, std::memory_order_release);
    m_OpCm = cm;
    m_OpHost = host;
    const quint64 gen = m_OpGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_OpBusy = true;
    emit opBusyChanged();

    std::thread([this, cm, host, op, root, path, droot, dpath, name, gen]() {
        QString error;
        try {
            NvComputer* computer = hostAt(cm, host);
            if (computer == nullptr || !hostOnline(computer)) {
                error = QStringLiteral("failed: The PC is not online.");
            }
            else {
                NvHTTP http(computer);
                const QString args = hostOpQuery(op, root, path, droot, dpath, name);
                const QString xml = http.openConnectionToString(http.httpsBaseUrl(), QStringLiteral("viple/media/op"), args, kOpTimeoutMs,
                                                                NvHTTP::NVLL_ERROR);
                NvHTTP::verifyResponseStatus(xml);
                if (op == QLatin1String("mkdir")) {
                    error = QString();
                }
                else {
                    bool jobOk = false;
                    const qint64 job = NvHTTP::getXmlString(xml, QStringLiteral("Job")).trimmed().toLongLong(&jobOk);
                    if (!jobOk || job <= 0) {
                        error = QStringLiteral("failed: bad job");
                    }
                    else {
                        m_HostJob.store(job, std::memory_order_release);
                        bool cancelSent = false;
                        auto sendCancel = [&]() {
                            if (cancelSent || !m_Cancel.load(std::memory_order_acquire)) {
                                return;
                            }
                            if (m_OpGen.load(std::memory_order_acquire) != gen) {
                                return;
                            }
                            requestHostCancel(cm, host, job);
                            cancelSent = true;
                        };
                        sendCancel();
                        int failures = 0;
                        qint64 lastMs = -1000;
                        while (error.isEmpty()) {
                            sendCancel();
                            try {
                                const QString statusXml = http.openConnectionToString(
                                    http.httpsBaseUrl(), QStringLiteral("viple/media/opstatus"), QStringLiteral("id=") + QString::number(job),
                                    kStatusTimeoutMs, NvHTTP::NVLL_ERROR);
                                NvHTTP::verifyResponseStatus(statusXml);
                                failures = 0;
                                const HostStatus st = parseHostStatus(statusXml);
                                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                                if (st.state != QLatin1String("running") || now - lastMs >= 200) {
                                    lastMs = now;
                                    postProgress(st.done, st.total);
                                }
                                if (st.state != QLatin1String("running")) {
                                    error = statusToError(st);
                                    break;
                                }
                            } catch (const GfeHttpResponseException& e) {
                                error = protocolError(QString::fromUtf8(e.getStatusMessage()));
                                break;
                            } catch (const QtNetworkReplyException& e) {
                                if (++failures >= kStatusFails) {
                                    error = networkFailed(e.getError());
                                    break;
                                }
                            } catch (const std::exception& e) {
                                if (++failures >= kStatusFails) {
                                    error = protocolError(QString::fromUtf8(e.what()));
                                    break;
                                }
                            }
                            const qint64 until = QDateTime::currentMSecsSinceEpoch() + 500;
                            while (QDateTime::currentMSecsSinceEpoch() < until) {
                                if (m_Cancel.load(std::memory_order_acquire) && !cancelSent) {
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }
                        }
                    }
                }
            }
        } catch (const GfeHttpResponseException& e) {
            error = protocolError(QString::fromUtf8(e.getStatusMessage()));
        } catch (const QtNetworkReplyException& e) {
            error = networkFailed(e.getError());
        } catch (const std::exception& e) {
            error = protocolError(QString::fromUtf8(e.what()));
        } catch (...) {
            error = QStringLiteral("failed: unknown");
        }
        finishOp(error);
    }).detach();
}

void MediaLibrary::cancelOp()
{
    m_Cancel.store(true, std::memory_order_release);
    const qint64 job = m_HostJob.load(std::memory_order_acquire);
    if (job <= 0 || m_OpCm == nullptr || m_OpHost.isEmpty()) {
        return;
    }
    const quint64 gen = m_OpGen.load(std::memory_order_acquire);
    ComputerManager* cm = m_OpCm;
    const QString host = m_OpHost;
    // 工作執行緒可能正卡在 opstatus。取消要另外一條連線才打得出去，不能等下一次輪詢。
    std::thread([this, cm, host, job, gen]() {
        if (m_OpGen.load(std::memory_order_acquire) != gen) {
            return;
        }
        if (m_HostJob.load(std::memory_order_acquire) != job) {
            return;
        }
        requestHostCancel(cm, host, job);
    }).detach();
}
