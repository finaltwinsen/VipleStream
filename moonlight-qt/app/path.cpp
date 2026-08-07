#include "path.h"

#include <QtDebug>
#include <QDir>
#include <QMutex>
#include <QSet>
#include <QStandardPaths>
#include <QSettings>
#include <QCoreApplication>

namespace {
// getDataFilePath 會被 render thread 高頻呼叫（shader / model 檔等），
// 「Found X at Y」每個檔名只印首次解析結果；跨執行緒共用 set 需鎖
void logFoundOnce(const QString& fileName, const QString& path)
{
    static QMutex s_Mutex;
    static QSet<QString> s_LoggedFiles;

    QMutexLocker locker(&s_Mutex);
    if (s_LoggedFiles.contains(fileName)) {
        return;
    }
    s_LoggedFiles.insert(fileName);
    qInfo() << "Found" << fileName << "at" << path;
}
}

QString Path::s_CacheDir;
QString Path::s_LogDir;
QString Path::s_BoxArtCacheDir;
QString Path::s_QmlCacheDir;

QString Path::getLogDir()
{
    Q_ASSERT(!s_LogDir.isEmpty());
    return s_LogDir;
}

QString Path::getBoxArtCacheDir()
{
    Q_ASSERT(!s_BoxArtCacheDir.isEmpty());
    return s_BoxArtCacheDir;
}

QString Path::getQmlCacheDir()
{
    Q_ASSERT(!s_QmlCacheDir.isEmpty());
    return s_QmlCacheDir;
}

QByteArray Path::readDataFile(QString fileName)
{
    QFile dataFile(getDataFilePath(fileName));
    if (!dataFile.open(QIODevice::ReadOnly)) {
        return {};
    }
    return dataFile.readAll();
}

void Path::writeCacheFile(QString fileName, QByteArray data)
{
    QDir cacheDir(s_CacheDir);

    // Create the cache path if it does not exist
    if (!cacheDir.exists()) {
        cacheDir.mkpath(".");
    }

    QFile dataFile(cacheDir.absoluteFilePath(fileName));
    if (dataFile.open(QIODevice::WriteOnly)) {
        dataFile.write(data);
    }
}

void Path::deleteCacheFile(QString fileName)
{
    QFile dataFile(QDir(s_CacheDir).absoluteFilePath(fileName));
    dataFile.remove();
}

QFileInfo Path::getCacheFileInfo(QString fileName)
{
    return QFileInfo(QDir(s_CacheDir), fileName);
}

QString Path::getDataFilePath(QString fileName)
{
    QString candidatePath;

    // Check the cache location first (used by Path::writeDataFile())
    candidatePath = QDir(s_CacheDir).absoluteFilePath(fileName);
    if (QFile::exists(candidatePath)) {
        logFoundOnce(fileName, candidatePath);
        return candidatePath;
    }

    // Try the directory of our app installation before the current directory —
    // 安裝目錄的檔案（shader 等）優先於工作目錄，避免從別的 CWD 啟動時
    // 撿到 stale 副本；portable 模式 CWD == 安裝目錄，行為不變
    candidatePath = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(fileName);
    if (QFile::exists(candidatePath)) {
        logFoundOnce(fileName, candidatePath);
        return candidatePath;
    }

    // Check the current directory
    candidatePath = QDir(QDir::currentPath()).absoluteFilePath(fileName);
    if (QFile::exists(candidatePath)) {
        logFoundOnce(fileName, candidatePath);
        return candidatePath;
    }

    // Now check the data directories (for Linux, in particular)
    candidatePath = QStandardPaths::locate(QStandardPaths::AppDataLocation, fileName);
    if (!candidatePath.isEmpty() && QFile::exists(candidatePath)) {
        logFoundOnce(fileName, candidatePath);
        return candidatePath;
    }

    // VipleStream v1.4.143 — AppImage portable layout: 解 <bin>/../share/<AppName>/
    // 對 /usr/share/<AppName>/.  AppImage 跑時 applicationDirPath = $APPDIR/usr/bin,
    // 但 AppRun 沒 export XDG_DATA_DIRS, 所以 AppDataLocation 不會 hit
    // $APPDIR/usr/share/<AppName>.  這條 candidate 補, 之後 installed deb 也
    // 走 /usr/bin/../share/<AppName>/ = /usr/share/<AppName>/ 同條 path.
    // 主要 use case: 補幀 RIFE-β 的 rife-v4.25-lite/flownet.{param,bin} model 檔.
    candidatePath = QDir(QCoreApplication::applicationDirPath()
                         + "/../share/"
                         + QCoreApplication::applicationName())
                        .absoluteFilePath(fileName);
    if (QFile::exists(candidatePath)) {
        logFoundOnce(fileName, candidatePath);
        return candidatePath;
    }

    // Return the QRC embedded copy
    candidatePath = ":/data/" + fileName;
    logFoundOnce(fileName, candidatePath);
    return QString(candidatePath);
}

void Path::initialize(bool portable)
{
    if (portable) {
        s_LogDir = QDir::currentPath();
        s_BoxArtCacheDir = QDir::currentPath() + "/boxart";
        s_QmlCacheDir = QDir::currentPath() + "/qmlcache";

        // In order for the If-Modified-Since logic to work in MappingFetcher,
        // the cache directory must be different than the current directory.
        s_CacheDir = QDir::currentPath() + "/cache";
    }
    else {
#ifdef Q_OS_DARWIN
        // On macOS, $TMPDIR is some random folder under /var/folders/ that nobody can
        // easily find, so use the system's global tmp directory instead.
        s_LogDir = "/tmp";
#elif defined(Q_OS_LINUX)
        // §LOG-PERSIST (2026-07-23)：Linux 桌面的 QDir::tempPath()=/tmp 是
        // tmpfs，重開機即蒸發——07-21 AMD 機 300ms 延遲事故就因為 log 隨
        // 重開機消失而無從屍檢。改寫進 CacheLocation 下的 logs/
        // （~/.cache/VipleStream/VipleStream/logs），搭配 main.cpp 啟動期
        // 輪替（保留最新 10 份）。CacheLocation 需要 org/app name 已設定，
        // Path::initialize 本來就在 setApplicationName 之後被呼叫（見
        // CLAUDE.md 的 Qt 初始化順序），這裡不引入新的順序需求。
        // mkpath 失敗（唯讀 HOME 等異常環境）就退回 /tmp，行為同舊版。
        s_LogDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/logs";
        if (!QDir().mkpath(s_LogDir)) {
            s_LogDir = QDir::tempPath();
        }
#else
        s_LogDir = QDir::tempPath();
#endif
        s_CacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        s_BoxArtCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/boxart";
        s_QmlCacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/qmlcache";
    }
}
