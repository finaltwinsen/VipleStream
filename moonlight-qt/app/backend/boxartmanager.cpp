#include "boxartmanager.h"
#include "../path.h"

#include <QImageReader>
#include <QImageWriter>

// §REVIEW-1104：uniqueid 來自 serverinfo，只允許 hex 與連字號，否則會被當成路徑走出 boxart
static bool isSafeBoxArtUuid(const QString& uuid)
{
    if (uuid.isEmpty()) {
        return false;
    }
    for (int i = 0; i < uuid.size(); i++) {
        const QChar ch = uuid.at(i);
        if (ch == QLatin1Char('-')) {
            continue;
        }
        const ushort u = ch.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F'))) {
            return false;
        }
    }
    return true;
}

// §REVIEW-1104：cleanPath 後必須是 boxart 的直接子目錄；canonical 若解析到外面（symlink、..）就拒絕
static bool isInsideBoxArtRoot(const QDir& root, const QDir& child)
{
    const QString rootPath = QDir::cleanPath(root.absolutePath());
    const QString childPath = QDir::cleanPath(child.absolutePath());
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const QString prefix = rootPath + QLatin1Char('/');
    if (rootPath.isEmpty() || !childPath.startsWith(prefix, cs)) {
        return false;
    }
    const QString rest = childPath.mid(prefix.size());
    if (rest.isEmpty() || rest.contains(QLatin1Char('/')) || rest.contains(QLatin1Char('\\'))) {
        return false;
    }

    QString rootCanon = QDir::cleanPath(root.canonicalPath());
    QString childCanon = QDir::cleanPath(child.canonicalPath());
    if (rootCanon.startsWith(QLatin1String("//?/"))) {
        rootCanon = rootCanon.mid(4);
    }
    if (childCanon.startsWith(QLatin1String("//?/"))) {
        childCanon = childCanon.mid(4);
    }
    if (!rootCanon.isEmpty() && !childCanon.isEmpty()
            && !childCanon.startsWith(rootCanon + QLatin1Char('/'), cs)) {
        return false;
    }
    return true;
}

BoxArtManager::BoxArtManager(QObject *parent) :
    QObject(parent),
    m_BoxArtDir(Path::getBoxArtCacheDir()),
    m_ThreadPool(this)
{
    // 4 is a good balance between fast loading for large
    // app grids and not crushing GFE with tons of requests
    // and causing UI jank from constantly stalling to decode
    // new images.
    m_ThreadPool.setMaxThreadCount(4);
    if (!m_BoxArtDir.exists()) {
        m_BoxArtDir.mkpath(".");
    }
}

QString
BoxArtManager::getFilePathForBoxArt(NvComputer* computer, int appId)
{
    QDir dir = m_BoxArtDir;

    // §REVIEW-1104：uuid 不合法或解析後不在 boxart 根目錄下，就不要建立或寫入封面
    if (!isSafeBoxArtUuid(computer->uuid)) {
        return QString();
    }

    // Create the cache directory if it did not already exist
    if (!dir.exists(computer->uuid)) {
        dir.mkdir(computer->uuid);
    }

    // Change to this computer's box art cache folder
    if (!dir.cd(computer->uuid) || !isInsideBoxArtRoot(m_BoxArtDir, dir)) {
        return QString();
    }

    // Try to open the cached file
    return dir.filePath(QString::number(appId) + ".png");
}

class NetworkBoxArtLoadTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    NetworkBoxArtLoadTask(BoxArtManager* boxArtManager, NvComputer* computer, NvApp& app)
        : m_Bam(boxArtManager),
          m_Computer(computer),
          m_App(app)
    {
        connect(this, &NetworkBoxArtLoadTask::boxArtFetchCompleted,
                boxArtManager, &BoxArtManager::handleBoxArtLoadComplete);
    }

signals:
    void boxArtFetchCompleted(NvComputer* computer, NvApp app, QUrl image);

private:
    void run()
    {
        QUrl image = m_Bam->loadBoxArtFromNetwork(m_Computer, m_App.id);
        if (image.isEmpty()) {
            // Give it another shot if it fails once
            image = m_Bam->loadBoxArtFromNetwork(m_Computer, m_App.id);
        }
        emit boxArtFetchCompleted(m_Computer, m_App, image);
    }

    BoxArtManager* m_Bam;
    NvComputer* m_Computer;
    NvApp m_App;
};

QUrl BoxArtManager::loadBoxArt(NvComputer* computer, NvApp& app)
{
    // Try to open the cached file if it exists and contains data
    QFile cacheFile(getFilePathForBoxArt(computer, app.id));
    if (cacheFile.exists() && cacheFile.size() > 0) {
        return QUrl::fromLocalFile(cacheFile.fileName());
    }

    // If we get here, we need to fetch asynchronously.
    // Kick off a worker on our thread pool to do just that.
    NetworkBoxArtLoadTask* netLoadTask = new NetworkBoxArtLoadTask(this, computer, app);
    m_ThreadPool.start(netLoadTask);

    // Return the placeholder then we can notify the caller
    // later when the real image is ready.
    return QUrl("qrc:/res/no_app_image.png");
}

void BoxArtManager::deleteBoxArt(NvComputer* computer)
{
    // §REVIEW-1104：uuid 含 .. 或絕對路徑時，刪主機會把 boxart 以外的目錄整棵刪掉
    if (!isSafeBoxArtUuid(computer->uuid)) {
        return;
    }

    QDir dir(Path::getBoxArtCacheDir());
    const QDir root = dir;

    // Delete everything in this computer's box art directory
    if (dir.cd(computer->uuid) && isInsideBoxArtRoot(root, dir)) {
        dir.removeRecursively();
    }
}

void BoxArtManager::handleBoxArtLoadComplete(NvComputer* computer, NvApp app, QUrl image)
{
    if (!image.isEmpty()) {
        emit boxArtLoadComplete(computer, app, image);
    }
}

QUrl BoxArtManager::loadBoxArtFromNetwork(NvComputer* computer, int appId)
{
    NvHTTP http(computer);

    QString cachePath = getFilePathForBoxArt(computer, appId);
    QImage image;
    try {
        image = http.getBoxArt(appId);
    } catch (...) {}

    // Cache the box art on disk if it loaded
    if (!image.isNull()) {
        if (image.save(cachePath)) {
            return QUrl::fromLocalFile(cachePath);
        }
        else {
            // A failed save() may leave a zero byte file. Make sure that's removed.
            QFile(cachePath).remove();
        }
    }

    return QUrl();
}

#include "boxartmanager.moc"
