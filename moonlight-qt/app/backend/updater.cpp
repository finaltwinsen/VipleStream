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

// GitHub 對未認證 IP 限 60 req/hr；只在 startUpdate()（使用者點更新）內
// 主動觸發一次。AutoUpdateChecker（§UPDATE-HEAD）平常走 github.com 網頁端的
// HEAD/302，不吃這個配額，只有備援路徑才會打同一個 API。
static const char* RELEASE_API_URL =
    "https://api.github.com/repos/finaltwinsen/VipleStream/releases/latest";

Updater::Updater(QObject *parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_CurrentReply(nullptr),
      m_DownloadFile(nullptr),
      m_BytesReceived(0),
      m_BytesTotal(0),
      m_Cancelled(false),
      m_Platform(UpdateAssetRules::detectPlatform())
{
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    qDebug() << "[VIPLE-UPDATE] platform" << UpdateAssetRules::describe(m_Platform)
             << "install mode =" << UpdateAssetRules::toString(UpdateAssetRules::installMode(m_Platform));
    if (m_Platform.emulated) {
        // §F9：模擬執行時 asset 仍依建置架構選（要覆蓋的是這支 binary）
        qDebug() << "[VIPLE-UPDATE] running under emulation — cpu" << m_Platform.currentArch
                 << "build" << m_Platform.buildArch << "; assets follow the build architecture";
    }
}

bool Updater::autoInstallSupported() const
{
    return UpdateAssetRules::installMode(m_Platform) == UpdateAssetRules::InstallMode::AutoInstall;
}

QString Updater::manualUpdateHint() const
{
    // §F9：不能自動安裝的平台，UpdateDialog 顯示這段說明＋「Open release page」
    using namespace UpdateAssetRules;
    switch (installMode(m_Platform)) {
    case InstallMode::AutoInstall:
        return QString();
    case InstallMode::NotifyOnly:
        if (m_Platform.packaging == Packaging::Flatpak) {
            // α 的 arm64 Flatpak 是 release 上的單檔 bundle、x64 Flatpak 是本機建的，
            // 兩者預設都沒有 remote，flatpak update 更新不到——先講 bundle 的裝法；
            // P1 發佈方式確定有 remote 之後再調整（見 §F9／F17 文件同步待辦）。
            return tr("This copy of VipleStream is installed as a Flatpak. "
                      "Download the new .flatpak from the release page and install it with "
                      "\"flatpak install --bundle <file>\". "
                      "If you installed it from a Flatpak repository, run \"flatpak update\" instead.");
        }
        if (m_Platform.arch == Arch::X64) {
            return tr("VipleStream is not running from an AppImage, so it cannot replace itself. "
                      "Download the new version from the release page.");
        }
        return tr("Automatic install is not available for Linux arm64 yet. "
                  "Download the new version from the release page.");
    case InstallMode::Unsupported:
    default:
        return tr("Automatic install is not available on this platform (%1). "
                  "Download the new version from the release page.")
            .arg(QStringLiteral("%1/%2").arg(m_Platform.currentArch, m_Platform.buildArch));
    }
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
    if (!autoInstallSupported()) {
        // §F9：UpdateDialog 在這些平台只顯示通知、不會呼叫 startUpdate；
        // 這裡是防線——連 API 都不打，更不下載。
        qDebug() << "[VIPLE-UPDATE] startUpdate refused — install mode"
                 << UpdateAssetRules::toString(UpdateAssetRules::installMode(m_Platform));
        fail(manualUpdateHint());
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

UpdateAssetRules::ReleasePlan Updater::selectAssetForPlatform(const QJsonObject& release)
{
    // Asset 命名規則（v1.5.276 範例；3.0.0 起六件）：
    //   VipleStream-Client-1.5.276.zip                     ← Windows x64（自動安裝）
    //   VipleStream-Client-1.5.276-debug.zip               ← 略過（PDB 包）
    //   VipleStream-Client-1.5.276-linux-x64.AppImage      ← Linux x64（自動安裝）
    //   VipleStream-Client-X.Y.Z-linux-arm64.flatpak/.zip  ← Linux arm64（只通知）
    //   VipleStream-Server-... / VipleStream-Android-...   ← 略過
    // §F9：決策全部在 updateassetrules.h 的 planRelease（精確錨定＋建置架構＋
    // 版號）。asset 版號比對的是這份 JSON 的 tag_name，不是 startUpdate 收到的
    // m_Version——UI 的版號是啟動時（或快取）查到的，可能已經過時。
    using namespace UpdateAssetRules;

    const ReleasePlan plan =
        planRelease(release, m_Platform, QCoreApplication::applicationVersion());

    for (const QString& s : plan.skippedClient) {
        qDebug() << "[VIPLE-UPDATE] skip asset" << s;
    }
    if (plan.result == PlanResult::Ok) {
        qDebug() << "[VIPLE-UPDATE] matched asset" << plan.assetName
                 << "action =" << toString(plan.action) << "release tag" << plan.tagName;
    }
    else {
        qWarning() << "[VIPLE-UPDATE] no asset for" << describe(m_Platform)
                   << "— plan" << toString(plan.result) << "release tag" << plan.tagName
                   << "current" << QCoreApplication::applicationVersion()
                   << "among" << plan.assetNames;
    }
    return plan;
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

    using UpdateAssetRules::PlanResult;
    const UpdateAssetRules::ReleasePlan plan = selectAssetForPlatform(doc.object());

    // §F9：實際要裝的版號以這次抓到的 release 為準。和 UI 傳進來的不同時
    // （常駐期間又發了新版、API 快取落後等）記一行；tag 有效且比目前新時改用
    // 抓到的 tag，helper 腳本檔名與狀態文字才會和真正下載的檔案一致。
    if (!plan.releaseVersion.isEmpty() && plan.releaseVersion != m_Version) {
        qDebug() << "[VIPLE-UPDATE] fetched release" << plan.tagName
                 << "differs from the version offered in the UI" << m_Version;
    }
    const bool tagIsNewer = plan.result == PlanResult::Ok ||
                            plan.result == PlanResult::NoAssets ||
                            plan.result == PlanResult::NoMatch;
    if (tagIsNewer) {
        m_Version = plan.releaseVersion;
    }

    switch (plan.result) {
    case PlanResult::Ok:
        break;
    case PlanResult::MissingTag:
        fail(tr("Release info from GitHub has no tag."));
        return;
    case PlanResult::BadTag:
        fail(tr("The latest release tag \"%1\" is not a version number.").arg(plan.tagName));
        return;
    case PlanResult::NotNewer:
        // API 回應有 max-age 快取，剛發佈時可能比 HEAD 302（AutoUpdateChecker）
        // 慢；也可能 release 被撤回。這不是「平台沒有安裝檔」，別那樣講。
        fail(tr("GitHub's release information is not up to date yet: it still reports %1, "
                "which is not newer than this version (%2). Please try again in a few minutes.")
             .arg(plan.releaseVersion, QCoreApplication::applicationVersion()));
        return;
    case PlanResult::NoAssets:
        fail(tr("Release %1 has no files attached yet. Please try again in a few minutes.")
             .arg(plan.releaseVersion));
        return;
    case PlanResult::NoMatch:
    default:
        // §F9：本平台在這次 release 沒有可安裝的 asset（還沒上傳、版號對不上、
        // 或本架構根本沒出）——不猜、不下載別的，交給 release 頁面。
        fail(tr("Release %1 has no installable package for this platform (%2).")
             .arg(plan.releaseVersion, UpdateAssetRules::describe(m_Platform)));
        return;
    }

    m_AssetName = plan.assetName;
    m_AssetUrl  = plan.assetUrl;
    if (plan.action != UpdateAssetRules::AssetAction::AutoInstall) {
        // §F9：只通知的 asset（arm64 Flatpak／zip 等）絕不自動下載安裝
        fail(tr("VipleStream %1 is available as %2, but it has to be installed manually. %3")
             .arg(m_Version, m_AssetName, manualUpdateHint()));
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
