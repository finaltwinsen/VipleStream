#include "updater.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

// GitHub 對未認證 IP 限 60 req/hr；只在 startUpdate() 內主動觸發一次，
// 與 AutoUpdateChecker 的 24h cache 各自獨立，但兩者不會在同一秒同 IP
// 超過該限額。
static const char* RELEASE_API_URL =
    "https://api.github.com/repos/finaltwinsen/VipleStream/releases/latest";

Updater::Updater(QObject *parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_CurrentReply(nullptr),
      m_DownloadFile(nullptr),
      m_BytesReceived(0),
      m_BytesTotal(0),
      m_Cancelled(false)
{
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

Updater::~Updater()
{
    if (m_CurrentReply) {
        m_CurrentReply->abort();
        m_CurrentReply->deleteLater();
    }
    if (m_DownloadFile) {
        m_DownloadFile->close();
        delete m_DownloadFile;
    }
}

void Updater::setStatus(const QString& s)
{
    if (m_Status != s) {
        m_Status = s;
        emit statusChanged();
    }
}

void Updater::fail(const QString& errorMessage)
{
    qWarning() << "[VIPLE-UPDATE] failed:" << errorMessage;
    if (m_CurrentReply) {
        m_CurrentReply->abort();
        m_CurrentReply->deleteLater();
        m_CurrentReply = nullptr;
    }
    if (m_DownloadFile) {
        m_DownloadFile->close();
        m_DownloadFile->remove();
        delete m_DownloadFile;
        m_DownloadFile = nullptr;
    }
    setStatus(QString());
    emit updateFailed(errorMessage);
}

void Updater::startUpdate(const QString& version)
{
    if (m_CurrentReply) {
        qDebug() << "[VIPLE-UPDATE] startUpdate ignored — already in progress";
        return;
    }

    m_Version       = version;
    m_Cancelled     = false;
    m_BytesReceived = 0;
    m_BytesTotal    = 0;
    emit progressChanged();

    setStatus(tr("Querying release assets…"));

    QUrl url(RELEASE_API_URL);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "VipleStream-Qt-Updater/1.0");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    m_CurrentReply = m_Nam->get(request);
    connect(m_CurrentReply, &QNetworkReply::finished,
            this, &Updater::handleAssetListFinished);
}

void Updater::cancel()
{
    if (!m_CurrentReply) {
        return;
    }
    m_Cancelled = true;
    m_CurrentReply->abort();
    // handleDownloadFinished / handleAssetListFinished 會看到 OperationCanceledError
    // 並走 fail() 清理；不要在這裡 emit updateFailed，避免重複信號。
}

bool Updater::selectAssetForPlatform(const QJsonArray& assets,
                                     QString& outUrl, QString& outName)
{
    // Asset 命名規則（v1.5.108 範例）：
    //   VipleStream-Client-1.5.108.zip                     ← Windows x64
    //   VipleStream-Client-1.5.108-linux-x64.AppImage      ← Linux x64
    //   VipleStream-Server-...                              ← 略過（server）
    //   VipleStream-Android-...apk                          ← 略過（android）
    for (const QJsonValue& v : assets) {
        QJsonObject obj = v.toObject();
        QString name = obj.value("name").toString();
        QString dl   = obj.value("browser_download_url").toString();
        if (name.isEmpty() || dl.isEmpty()) continue;
        if (!name.contains(QStringLiteral("Client"), Qt::CaseInsensitive)) continue;

#if defined(Q_OS_WIN)
        if (name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) &&
            !name.contains(QStringLiteral("linux"), Qt::CaseInsensitive)) {
            outUrl = dl; outName = name; return true;
        }
#elif defined(Q_OS_LINUX)
        if (name.endsWith(QStringLiteral(".AppImage"), Qt::CaseInsensitive) &&
            name.contains(QStringLiteral("linux"), Qt::CaseInsensitive)) {
            outUrl = dl; outName = name; return true;
        }
#else
        Q_UNUSED(dl);
#endif
    }
    return false;
}

void Updater::handleAssetListFinished()
{
    QNetworkReply* reply = m_CurrentReply;
    m_CurrentReply = nullptr;
    reply->deleteLater();

    if (m_Cancelled) {
        fail(tr("Update cancelled."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Failed to query release info: %1").arg(reply->errorString()));
        return;
    }

    QByteArray body = reply->readAll();
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (doc.isNull() || !doc.isObject()) {
        fail(tr("Release JSON malformed: %1").arg(pe.errorString()));
        return;
    }
    QJsonObject release = doc.object();
    QJsonArray assets = release.value("assets").toArray();
    if (assets.isEmpty()) {
        fail(tr("No assets attached to release."));
        return;
    }

    if (!selectAssetForPlatform(assets, m_AssetUrl, m_AssetName)) {
        fail(tr("No matching asset for this platform in release %1.").arg(m_Version));
        return;
    }

    qDebug() << "[VIPLE-UPDATE] selected asset" << m_AssetName << "→" << m_AssetUrl;

    // 開啟 staging 檔。放 TempLocation 而非 install dir，避免半套狀態
    // 污染目前安裝；helper 接手後才搬到 install dir。
    QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(tempRoot);
    m_DownloadPath = QDir(tempRoot).filePath(m_AssetName);

    m_DownloadFile = new QFile(m_DownloadPath);
    if (!m_DownloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot open download file: %1").arg(m_DownloadFile->errorString()));
        return;
    }

    setStatus(tr("Downloading %1…").arg(m_AssetName));

    QNetworkRequest request{QUrl(m_AssetUrl)};
    request.setRawHeader("User-Agent", "VipleStream-Qt-Updater/1.0");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    m_CurrentReply = m_Nam->get(request);
    connect(m_CurrentReply, &QNetworkReply::downloadProgress,
            this, &Updater::handleDownloadProgress);
    connect(m_CurrentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_DownloadFile && m_CurrentReply) {
            m_DownloadFile->write(m_CurrentReply->readAll());
        }
    });
    connect(m_CurrentReply, &QNetworkReply::finished,
            this, &Updater::handleDownloadFinished);
}

void Updater::handleDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    m_BytesReceived = bytesReceived;
    m_BytesTotal    = bytesTotal;
    emit progressChanged();
}

void Updater::handleDownloadFinished()
{
    QNetworkReply* reply = m_CurrentReply;
    m_CurrentReply = nullptr;

    // Flush 殘留 buffer（readyRead 之後 finished 之前可能還有 bytes）
    if (m_DownloadFile && reply) {
        m_DownloadFile->write(reply->readAll());
        m_DownloadFile->flush();
        m_DownloadFile->close();
    }

    if (m_Cancelled) {
        if (reply) reply->deleteLater();
        if (m_DownloadFile) {
            m_DownloadFile->remove();
            delete m_DownloadFile;
            m_DownloadFile = nullptr;
        }
        fail(tr("Update cancelled."));
        return;
    }
    if (reply && reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        reply->deleteLater();
        if (m_DownloadFile) {
            m_DownloadFile->remove();
            delete m_DownloadFile;
            m_DownloadFile = nullptr;
        }
        fail(tr("Download failed: %1").arg(err));
        return;
    }
    if (reply) reply->deleteLater();

    if (m_DownloadFile) {
        delete m_DownloadFile;
        m_DownloadFile = nullptr;
    }

    setStatus(tr("Preparing installer…"));
    if (!spawnHelperAndQuit()) {
        // spawnHelperAndQuit() 內已呼叫 fail()
        return;
    }

    setStatus(tr("Restarting to apply update…"));
    emit readyToRestart();
}

#ifdef Q_OS_WIN
// Windows-only：寫 PowerShell helper、ShellExecuteEx 啟動（需要時帶 UAC）
//
// Helper 職責：
//   1. Wait-Process 等本 PID 結束（檔案鎖釋放）
//   2. Expand-Archive 把 zip 直接覆蓋到 install dir
//   3. 刪 zip
//   4. Start-Process 啟動新版 exe
//
// 為什麼用 PowerShell 而非小型 helper exe：
//   - 不污染 build pipeline（無新 binary target）
//   - PS 5.1 內建 Expand-Archive；Windows 10+ 都有
//   - Start-Process / Wait-Process 簡潔，錯誤訊息可以 echo 出來
static bool isPathWritable(const QString& dir)
{
    QString probe = QDir(dir).filePath(QStringLiteral(".vs-write-test"));
    QFile f(probe);
    bool ok = f.open(QIODevice::WriteOnly);
    if (ok) {
        f.close();
        QFile::remove(probe);
    }
    return ok;
}

static bool shellExecuteRunas(const QString& exe, const QString& args, bool elevate)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = elevate ? L"runas" : L"open";
    QString exeNative  = QDir::toNativeSeparators(exe);
    QString argsNative = args;
    sei.lpFile        = reinterpret_cast<LPCWSTR>(exeNative.utf16());
    sei.lpParameters  = reinterpret_cast<LPCWSTR>(argsNative.utf16());
    sei.nShow         = SW_HIDE;
    sei.fMask         = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        qWarning() << "[VIPLE-UPDATE] ShellExecuteExW failed, GetLastError ="
                   << GetLastError();
        return false;
    }
    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }
    return true;
}
#endif

bool Updater::spawnHelperAndQuit()
{
#if defined(Q_OS_WIN)
    QString installDir = QCoreApplication::applicationDirPath();
    QString exePath    = QCoreApplication::applicationFilePath();
    qint64  pid        = QCoreApplication::applicationPid();

    // Write helper PS1 to temp
    QString tempRoot   = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString helperPath = QDir(tempRoot).filePath(QStringLiteral("viplestream_update_%1.ps1").arg(m_Version));

    // PowerShell 跳過 ExecutionPolicy + 隱藏視窗的標準呼叫慣例
    // ($PSScriptRoot 在 -File 模式下可用，這裡用絕對路徑所以不需要)
    QString script = QStringLiteral(
        "param([int]$AppPid,[string]$ZipPath,[string]$InstallDir,[string]$ExePath)\n"
        "$ErrorActionPreference='Stop'\n"
        "try { Wait-Process -Id $AppPid -Timeout 30 -ErrorAction Stop } catch {}\n"
        "Start-Sleep -Milliseconds 800\n"
        "try {\n"
        "  Expand-Archive -LiteralPath $ZipPath -DestinationPath $InstallDir -Force\n"
        "} catch {\n"
        "  Write-Host \"VipleStream updater: extract failed: $_\"\n"
        "  exit 1\n"
        "}\n"
        "Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue\n"
        "Start-Process -FilePath $ExePath\n"
    );

    QFile sf(helperPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write helper script: %1").arg(sf.errorString()));
        return false;
    }
    sf.write(script.toUtf8());
    sf.close();

    bool needElevate = !isPathWritable(installDir);
    qDebug() << "[VIPLE-UPDATE] install dir" << installDir
             << "writable =" << !needElevate;

    // 構造 PowerShell argv：-NoProfile -ExecutionPolicy Bypass -File <script> -AppPid ... -ZipPath ...
    QString args = QStringLiteral(
        "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        "-File \"%1\" -AppPid %2 -ZipPath \"%3\" -InstallDir \"%4\" -ExePath \"%5\""
    ).arg(QDir::toNativeSeparators(helperPath))
     .arg(pid)
     .arg(QDir::toNativeSeparators(m_DownloadPath))
     .arg(QDir::toNativeSeparators(installDir))
     .arg(QDir::toNativeSeparators(exePath));

    if (!shellExecuteRunas(QStringLiteral("powershell.exe"), args, needElevate)) {
        // 使用者取消 UAC prompt 也會走這條
        fail(tr("Failed to launch updater (UAC denied or PowerShell missing)."));
        return false;
    }
    return true;

#elif defined(Q_OS_LINUX)
    // AppImage 路徑必須從 APPIMAGE 環境變數取得 — applicationFilePath()
    // 在 AppImage 內會回傳 mount 點下的內部 binary 路徑，不是 .AppImage 本身。
    QByteArray appImage = qgetenv("APPIMAGE");
    if (appImage.isEmpty()) {
        fail(tr("Not running from AppImage — APPIMAGE env not set. "
                "Please reinstall manually."));
        return false;
    }
    QString currentAppImage = QString::fromLocal8Bit(appImage);
    qint64  pid             = QCoreApplication::applicationPid();

    // AppImage 通常在 ~/Applications/ 或 ~/Downloads/，user-writable；
    // 若在 /opt 等 system 路徑就 fail。
    if (!QFileInfo(QFileInfo(currentAppImage).absolutePath()).isWritable()) {
        fail(tr("AppImage directory not writable: %1. Move VipleStream to a "
                "user-writable location (e.g. ~/Applications/) and retry.")
             .arg(QFileInfo(currentAppImage).absolutePath()));
        return false;
    }

    QString tempRoot   = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QString helperPath = QDir(tempRoot).filePath(QStringLiteral("viplestream_update_%1.sh").arg(m_Version));

    QString script = QStringLiteral(
        "#!/bin/bash\n"
        "APP_PID=\"$1\"\n"
        "NEW_APPIMAGE=\"$2\"\n"
        "CURRENT_APPIMAGE=\"$3\"\n"
        "for i in $(seq 1 60); do\n"
        "  kill -0 \"$APP_PID\" 2>/dev/null || break\n"
        "  sleep 0.5\n"
        "done\n"
        "chmod +x \"$NEW_APPIMAGE\" || exit 1\n"
        "mv -f \"$NEW_APPIMAGE\" \"$CURRENT_APPIMAGE\" || exit 1\n"
        "nohup \"$CURRENT_APPIMAGE\" >/dev/null 2>&1 &\n"
        "disown\n"
    );

    QFile sf(helperPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write helper script: %1").arg(sf.errorString()));
        return false;
    }
    sf.write(script.toUtf8());
    sf.close();
    QFile::setPermissions(helperPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::ExeOther);

    QStringList args{ QString::number(pid), m_DownloadPath, currentAppImage };
    if (!QProcess::startDetached(QStringLiteral("/bin/bash"),
                                 QStringList{ helperPath } + args)) {
        fail(tr("Failed to launch updater helper script."));
        return false;
    }
    return true;

#else
    fail(tr("Automatic update not supported on this platform."));
    return false;
#endif
}
