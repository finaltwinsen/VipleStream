#include "updater.h"
#include "updatenotes.h"
#include "cli/commandlineparser.h"
#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
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

// §F9：--dev-update-url 只活在這次行程。空字串就是正式的 GitHub。
static bool devUpdateActive()
{
    return !GlobalCommandLineParser::devUpdateUrl().isEmpty();
}

Updater::Updater(QObject *parent)
    : QObject(parent),
      m_Nam(new QNetworkAccessManager(this)),
      m_CurrentReply(nullptr),
      m_DownloadFile(nullptr),
      m_BytesReceived(0),
      m_BytesTotal(0),
      m_Cancelled(false),
      m_DownloadWriteFailed(false),
      m_Platform(UpdateAssetRules::detectPlatform()),
      m_NotesReply(nullptr)
{
    m_Nam->setStrictTransportSecurityEnabled(true);
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    if (devUpdateActive()) {
        // §F9：測試用的 asset 可以是 http://127.0.0.1。開著 HSTS 會被改寫成 https。
        m_Nam->setStrictTransportSecurityEnabled(false);
        qDebug() << "[VIPLE-UPDATE] dev update url" << GlobalCommandLineParser::devUpdateUrl();
    }

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
            // §AUTO-UPDATE：Steam Frame（arm64）用安裝腳本裝的，頭盔上的 viplestream-update.timer 會在 app 關著時自動更新
            // （沙盒裡的 app 沒辦法替自己裝 bundle）；x64 Flatpak 是本機建的，照舊手動
            if (m_Platform.arch == Arch::Arm64) {
                return tr("On the Steam Frame, VipleStream updates itself in the background while it is closed "
                          "(when installed with the installer script). "
                          "To update right away, close VipleStream and run the new installer script from the release page.");
            }
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
        // §REVIEW-1119：abort() 會同步發出 finished，槽函式把 m_CurrentReply 清成空之後再 deleteLater 就會崩。
        // 和下面的 m_NotesReply 一樣，先斷開這個物件的連線。
        m_CurrentReply->disconnect(this);
        m_CurrentReply->abort();
        m_CurrentReply->deleteLater();
        m_CurrentReply = nullptr;
    }
    // §REVIEW-1111：更新摘要的請求也要中止。原本解構後它的 finished 還會進來，槽裡用的是已經解構的 Updater。
    if (m_NotesReply) {
        m_NotesReply->disconnect(this);
        m_NotesReply->abort();
        m_NotesReply->deleteLater();
        m_NotesReply = nullptr;
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

// §REVIEW-1119：錯誤與安裝路徑會進除錯日誌。家目錄前綴改成 ~，才不會把帳號名稱一起寫出去。
static QString redactHomePath(QString text)
{
    const QString home = QDir::homePath();
    if (home.size() <= 3) {
        return text;
    }
    text.replace(home, QStringLiteral("~"));
    const QString nativeHome = QDir::toNativeSeparators(home);
    if (nativeHome != home) {
        text.replace(nativeHome, QStringLiteral("~"));
    }
    return text;
}

void Updater::fail(const QString& errorMessage)
{
    qWarning() << "[VIPLE-UPDATE] failed:" << redactHomePath(errorMessage);
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
    // §REVIEW-1119：spawnHelperAndQuit 失敗（找不到 powershell、UAC 取消）時下載檔已經先關掉，
    // 這裡原本沒清暫存目錄，zip 和腳本會留在 %TEMP%。成功交給 helper 的那條不會進 fail()。
    if (!m_StageDir.isEmpty()) {
        QDir(m_StageDir).removeRecursively();
        m_StageDir.clear();
        m_DownloadPath.clear();
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
    m_DownloadWriteFailed = false;
    m_BytesReceived = 0;
    m_BytesTotal    = 0;
    emit progressChanged();

    setStatus(tr("Querying release assets…"));

    QString releaseApi = QString::fromLatin1(RELEASE_API_URL);
    if (devUpdateActive()) {
        releaseApi = GlobalCommandLineParser::devUpdateUrl() + QStringLiteral("/releases/latest");
        qDebug() << "[VIPLE-UPDATE] querying dev release" << releaseApi;
    }
    QUrl url(releaseApi);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "VipleStream-Qt-Updater/1.0");
    if (devUpdateActive()) {
        // 測試伺服器要自己回 200。跟著 3xx 會離開允許的網址。
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif
    request.setTransferTimeout(15000);

    m_CurrentReply = m_Nam->get(request);
    connect(m_CurrentReply, &QNetworkReply::readyRead, this, [this]() {
        // §REVIEW-1119：GitHub 的 release JSON 只有幾十 KB。太大就中止，避免整包讀進記憶體再解析。
        auto* reply = qobject_cast<QNetworkReply*>(sender());
        if (reply && reply->bytesAvailable() > (2 * 1024 * 1024)) {
            reply->abort();
        }
    });
    connect(m_CurrentReply, &QNetworkReply::finished,
            this, &Updater::handleAssetListFinished);
}

void Updater::fetchReleaseNotes(const QString& version)
{
    if (version.isEmpty()) {
        return;
    }
    if (version == m_NotesVersion && (m_NotesState == QLatin1String("ready") || m_NotesState == QLatin1String("loading"))) {
        return;
    }
    if (m_NotesReply) {
        m_NotesReply->abort();  // handleNotesFinished 會因為版號不同而丟掉
    }
    m_NotesVersion = version;
    m_NotesText.clear();
    m_NotesState = QStringLiteral("loading");
    emit releaseNotesChanged();

    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.github.com/repos/finaltwinsen/VipleStream/releases/tags/v%1").arg(version)));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "VipleStream-Qt-Updater/1.0");
    request.setTransferTimeout(15000);
    m_NotesReply = m_Nam->get(request);
    m_NotesReply->setProperty("notesVersion", version);
    connect(m_NotesReply, &QNetworkReply::readyRead, this, [this]() {
        // §REVIEW-1119：同資產清單，release notes 的 JSON 不該超過 2 MB。
        auto* reply = qobject_cast<QNetworkReply*>(sender());
        if (reply && reply->bytesAvailable() > (2 * 1024 * 1024)) {
            reply->abort();
        }
    });
    connect(m_NotesReply, &QNetworkReply::finished, this, &Updater::handleNotesFinished);
}

void Updater::handleNotesFinished()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) {
        return;
    }
    reply->deleteLater();
    if (reply == m_NotesReply) {
        m_NotesReply = nullptr;
    }
    if (reply->property("notesVersion").toString() != m_NotesVersion) {
        return;  // 已經改查別的版本
    }

    QString body;
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray raw = reply->readAll();
        if (raw.size() > (2 * 1024 * 1024)) {
            qWarning() << "[VIPLE-UPDATE] release notes for" << m_NotesVersion << "exceeded 2 MB";
        }
        else {
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            body = doc.object().value(QStringLiteral("body")).toString();
        }
    }
    else {
        qWarning() << "[VIPLE-UPDATE] release notes for" << m_NotesVersion << "failed:" << reply->errorString();
    }

    // 摘要依介面語系取雙語 release notes 的其中一段（規則在 updatenotes.h）
    m_NotesText = UpdateNotes::summarize(body, StreamingPreferences::get()->uiLocaleName());
    m_NotesState = m_NotesText.isEmpty() ? QStringLiteral("failed") : QStringLiteral("ready");
    qDebug() << "[VIPLE-UPDATE] release notes for" << m_NotesVersion << m_NotesState
             << "(" << body.size() << "->" << m_NotesText.size() << "chars)";
    emit releaseNotesChanged();
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
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (devUpdateActive() && httpStatus >= 300 && httpStatus < 400) {
        fail(tr("Failed to query release info: %1").arg(QStringLiteral("unexpected redirect")));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Failed to query release info: %1").arg(reply->errorString()));
        return;
    }

    QByteArray body = reply->readAll();
    if (body.size() > (2 * 1024 * 1024)) {
        fail(tr("Release JSON malformed: %1").arg(QStringLiteral("response too large")));
        return;
    }
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
    m_AssetSha256 = plan.assetSha256;
    if (plan.action != UpdateAssetRules::AssetAction::AutoInstall) {
        // §F9：只通知的 asset（arm64 Flatpak／zip 等）絕不自動下載安裝
        fail(tr("VipleStream %1 is available as %2, but it has to be installed manually. %3")
             .arg(m_Version, m_AssetName, manualUpdateHint()));
        return;
    }

    qDebug() << "[VIPLE-UPDATE] selected asset" << m_AssetName << "→" << m_AssetUrl
             << "sha256" << (m_AssetSha256.isEmpty() ? QStringLiteral("<none>") : m_AssetSha256);

    // §UPDATE-DIGEST（§REVIEW-1111 第二輪）：下載回來的檔案原本只核對大小就覆蓋安裝目錄（需要時還提權）。
    // 現在：(1) 只從自己 repo 的 release 下載；(2) release 一定要帶 sha256，下載完核對（host 的自我更新早就這樣做）。
    // §REVIEW-1119：字首比對擋不住 download/../../ 其他 repo。正規化後的路徑必須仍在自己的 releases 底下。
    {
        const QUrl assetUrl(m_AssetUrl);
        bool dotDotSegment = false;
        for (const QString& segment : assetUrl.path().split(QLatin1Char('/'))) {
            if (segment == QLatin1String("..")) {
                dotDotSegment = true;
                break;
            }
        }
        bool allowed = false;
        if (devUpdateActive()) {
            // §F9：測試伺服器不在 github.com。名單與 --dev-update-url 相同，sha256 一樣必填。
            allowed = GlobalCommandLineParser::isAllowedDevUpdateUrl(m_AssetUrl)
                    && !m_AssetUrl.contains(QLatin1Char('\\'))
                    && !dotDotSegment;
        }
        else {
            const QString normalizedPath = assetUrl.adjusted(QUrl::NormalizePathSegments).path();
            allowed = assetUrl.isValid() && assetUrl.scheme() == QLatin1String("https")
                    && assetUrl.host() == QLatin1String("github.com") && assetUrl.port(-1) != -1
                    && assetUrl.userInfo().isEmpty() && !m_AssetUrl.contains(QLatin1Char('\\'))
                    && !dotDotSegment
                    && normalizedPath.startsWith(QLatin1String("/finaltwinsen/VipleStream/releases/download/"));
        }
        if (!allowed) {
            fail(tr("Download failed: %1").arg(QStringLiteral("unexpected download address")));
            return;
        }
    }
    if (m_AssetSha256.isEmpty()) {
        fail(tr("Download failed: %1").arg(QStringLiteral("the release does not list a SHA-256 for %1").arg(m_AssetName)));
        return;
    }
    m_DownloadHash.reset();

    // 開啟 staging 檔。放暫存位置而非 install dir，避免半套狀態污染目前安裝；helper 接手後才搬到 install dir。
    // 用隨機名稱的目錄（QTemporaryDir：只有自己可讀寫）：原本是暫存根目錄下的固定檔名，Linux 的 /tmp 是共用的，
    // 別的使用者可以先放一個同名的檔案或符號連結。
    {
        QTemporaryDir stage(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                .filePath(QStringLiteral("viplestream-update-XXXXXX")));
        stage.setAutoRemove(false);
        if (!stage.isValid()) {
            fail(tr("Cannot open download file: %1").arg(stage.errorString()));
            return;
        }
        m_StageDir = stage.path();
    }
    m_DownloadPath = QDir(m_StageDir).filePath(m_AssetName);

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
    // §REVIEW-1119：資產查詢有 15 秒逾時，下載這條沒有。60 秒沒有任何位元組才中止，慢速但持續的下載不受影響。
    request.setTransferTimeout(60000);
    if (devUpdateActive()) {
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    }

    m_CurrentReply = m_Nam->get(request);
    connect(m_CurrentReply, &QNetworkReply::downloadProgress,
            this, &Updater::handleDownloadProgress);
    connect(m_CurrentReply, &QNetworkReply::readyRead, this, [this]() {
        if (m_DownloadFile && m_CurrentReply) {
            const QByteArray data = m_CurrentReply->readAll();
            m_DownloadHash.addData(data);
            // §REVIEW-1104：磁碟滿時 write 會少寫；先記下，收尾改走 fail()，不要把截斷的 zip 交給 helper
            // §REVIEW-1119：只設旗標的話檔案大小不再增加，1 GB 上限與閒置逾時都不會到，下載會一直收。
            // abort() 可能同步跑到 finished；這裡要立刻返回，不能再碰已經收掉的檔案。
            if (m_DownloadFile->write(data) != data.size()) {
                m_DownloadWriteFailed = true;
                m_CurrentReply->abort();
                return;
            }
            // §UPDATE-DIGEST：安裝檔不到 200 MB；超過 1 GB 就不是我們的檔案，別把磁碟寫滿
            if (m_DownloadFile->size() > (qint64(1) << 30)) {
                m_DownloadWriteFailed = true;
                m_CurrentReply->abort();
                return;
            }
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
        const QByteArray tail = reply->readAll();
        m_DownloadHash.addData(tail);
        // §REVIEW-1104：write／flush 失敗或位元組數對不上總長度，不能當成下載成功，否則會關程式去解截斷的 zip
        if (m_DownloadFile->write(tail) != tail.size() || !m_DownloadFile->flush()) {
            m_DownloadWriteFailed = true;
        }
        const qint64 written = m_DownloadFile->size();
        m_DownloadFile->close();
        if (m_BytesTotal > 0 && written != m_BytesTotal) {
            m_DownloadWriteFailed = true;
        }
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
    // §REVIEW-1119：寫入失敗時會 abort()，reply 的錯誤會變成 Operation canceled。先看旗標，磁碟已滿才不會被蓋掉。
    if (m_DownloadWriteFailed) {
        if (reply) {
            reply->deleteLater();
        }
        QFile::remove(m_DownloadPath);
        fail(tr("Could not save the downloaded update. The disk may be full."));
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
    if (reply && devUpdateActive()) {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus >= 300 && httpStatus < 400) {
            reply->deleteLater();
            QFile::remove(m_DownloadPath);
            fail(tr("Download failed: %1").arg(QStringLiteral("unexpected redirect")));
            return;
        }
    }
    if (reply) reply->deleteLater();

    // §UPDATE-DIGEST：和 release 回報的 sha256 不同就不安裝
    {
        const QString got = QString::fromLatin1(m_DownloadHash.result().toHex());
        if (got != m_AssetSha256) {
            qWarning() << "[VIPLE-UPDATE] sha256 mismatch: got" << got << "expected" << m_AssetSha256;
            QFile::remove(m_DownloadPath);
            fail(tr("Download failed: %1").arg(QStringLiteral("SHA-256 mismatch")));
            return;
        }
        qDebug() << "[VIPLE-UPDATE] sha256 verified" << got;
    }

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
    // §REVIEW-1111（第二輪）：隨機檔名、只在不存在時建立。原本是固定檔名：那裡如果已經有一個同名的連結，
    // 開檔會把連結指到的檔案清空。
    QTemporaryFile f(QDir(dir).filePath(QStringLiteral(".vs-write-test-XXXXXX")));
    return f.open();  // 解構時自己刪掉
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

    // Write helper PS1 to temp（和下載的檔案放同一個隨機名稱的暫存目錄）
    QString tempRoot   = m_StageDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : m_StageDir;
    QString helperPath = QDir(tempRoot).filePath(QStringLiteral("viplestream_update_%1.ps1").arg(m_Version));

    // PowerShell 跳過 ExecutionPolicy + 隱藏視窗的標準呼叫慣例
    // ($PSScriptRoot 在 -File 模式下可用，這裡用絕對路徑所以不需要)
    // §REVIEW-1104：目錄要提權時，覆寫仍由已提高權限的 PowerShell 做；重開 exe 改經未提權的 explorer（Shell.Application），避免新行程維持系統管理員。COM 失敗才退回 Start-Process，避免更新好了卻沒有行程再開
    QString script = QStringLiteral(
        "param([int]$AppPid,[string]$ZipPath,[string]$InstallDir,[string]$ExePath,[int]$DropAdmin,[string]$Sha256,[string]$Version)\n"
        "$ErrorActionPreference='Stop'\n"
        // §REVIEW-1104：舊程式最多等 90 秒（自己收尾最多 30 秒，VR 收尾更久）；還沒結束就不要覆蓋——
        // 檔案被鎖時 Expand-Archive 會解到一半中斷，留下新舊混雜、可能再也開不起來的安裝。改成放棄這次更新並提示
        "try { Wait-Process -Id $AppPid -Timeout 90 -ErrorAction Stop } catch {}\n"
        "if (Get-Process -Id $AppPid -ErrorAction SilentlyContinue) {\n"
        "  try { Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show('__FAIL_MSG__', 'VipleStream') | Out-Null } catch {}\n"
        "  exit 2\n"
        "}\n"
        "Start-Sleep -Milliseconds 800\n"
        // §REVIEW-1111：先解到暫存目錄，整包解得開才覆蓋安裝目錄。原本直接解進安裝目錄：下載到的 zip 壞掉、
        // 解到一半磁碟滿時，安裝目錄已經新舊混雜，而且腳本就此結束、不會再把程式打開。
        // 現在解壓失敗時安裝目錄沒有動過，照樣把（舊的）程式開回來。
        "$ok = $true\n"
        "$stage = Join-Path ([IO.Path]::GetTempPath()) ('viplestream_stage_' + $AppPid)\n"
        "try {\n"
        // §UPDATE-DIGEST：解壓前再核對一次。程式驗完到這裡（可能已提權）之間，暫存檔有可能被換掉。
        // 用 .NET 算，不用 Get-FileHash：模組路徑被別的 PowerShell 版本帶歪時，5.1 會找不到那個 cmdlet
        "  if ($Sha256) {\n"
        "    $fs = [IO.File]::OpenRead($ZipPath)\n"
        "    try { $hb = [Security.Cryptography.SHA256]::Create().ComputeHash($fs) } finally { $fs.Close() }\n"
        "    if (([BitConverter]::ToString($hb) -replace '-', '') -ne $Sha256) { throw 'sha256 mismatch' }\n"
        "  }\n"
        "  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }\n"
        // 用 .NET 解壓，不用 Expand-Archive：程式從 PowerShell 7 的終端機啟動時，繼承到的模組路徑會讓 5.1 載到
        // 7 的 Archive 模組而失敗（2026-11-12 實測）
        "  Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
        // §REVIEW-1118B：解壓前先看每一筆路徑。有些 .NET Framework 的 ExtractToDirectory 不擋項目名稱裡的 ..，
        // 提權後會在覆蓋安裝目錄之前把檔案寫到 stage 外面。GetFullPath 之後必須落在 stage 底下，否則丟例外，
        // 沿用下面的 catch（安裝目錄不動、把舊程式開回來）。
        "  $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)\n"
        "  try {\n"
        "    $stageFull = [IO.Path]::GetFullPath($stage)\n"
        "    $sep = [string]([IO.Path]::DirectorySeparatorChar)\n"
        "    $stagePrefix = $stageFull\n"
        "    if (-not $stagePrefix.EndsWith($sep)) { $stagePrefix += $sep }\n"
        "    foreach ($entry in $zip.Entries) {\n"
        "      $dest = [IO.Path]::GetFullPath([IO.Path]::Combine($stageFull, $entry.FullName))\n"
        "      if ($dest.Length -lt $stagePrefix.Length -or $dest.Substring(0, $stagePrefix.Length) -ne $stagePrefix) { throw ('zip entry is outside the stage directory: ' + $entry.FullName) }\n"
        "    }\n"
        "  } finally {\n"
        "    $zip.Dispose()\n"
        "  }\n"
        "  [IO.Compression.ZipFile]::ExtractToDirectory($ZipPath, $stage)\n"
        "  Copy-Item -Path (Join-Path $stage '*') -Destination $InstallDir -Recurse -Force\n"
        // §F9：檔案已經蓋上。沒提權寫不了 HKLM，失敗只記 log，不能把 $ok 改成 false（否則會把裝好的更新判成失敗）。
        "  try {\n"
        "    if ($Version) {\n"
        "      $uninst = 'HKLM:\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VipleStreamClient'\n"
        "      if (Test-Path -LiteralPath $uninst) {\n"
        "        Set-ItemProperty -LiteralPath $uninst -Name DisplayVersion -Value $Version\n"
        "      }\n"
        "    }\n"
        "  } catch {\n"
        "    Write-Host ('DisplayVersion not updated: ' + $_)\n"
        "    try {\n"
        "      $logDir = Join-Path $env:LOCALAPPDATA 'VipleStream'\n"
        "      New-Item -ItemType Directory -Force -Path $logDir | Out-Null\n"
        "      Add-Content -LiteralPath (Join-Path $logDir 'update.log') -Value ('DisplayVersion not updated: ' + $_)\n"
        "    } catch {}\n"
        "  }\n"
        "} catch {\n"
        "  Write-Host \"VipleStream updater: extract failed: $_\"\n"
        "  $ok = $false\n"
        "}\n"
        "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue\n"
        "Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue\n"
        "$sd = Split-Path -Parent $ZipPath\n"
        "if ((Split-Path -Leaf $sd) -like 'viplestream-update-*') { Remove-Item -LiteralPath $sd -Recurse -Force -ErrorAction SilentlyContinue }\n"
        "if ($DropAdmin -ne 0) {\n"
        "  try {\n"
        // §REVIEW-1119：New-Object -ComObject Shell.Application 是載進這個（已提權的）PowerShell 行程裡的 shell32，
        // 它的 ShellExecute 開出來的程式一樣是系統管理員權限。改成向桌面的 explorer 要它的 Shell 物件
        // （ShellWindows 由 explorer 行程提供；FindWindowSW 的 8＝SWC_DESKTOP、1＝SWFO_NEEDDISPATCH），
        // 由 explorer 以一般權限啟動。11-19 在開發機實測開出來的行程是 Medium。explorer 沒在跑時才退回 Start-Process。
        "    $sw = [Activator]::CreateInstance([Type]::GetTypeFromCLSID('9BA05972-F6A8-11CF-A442-00A0C90A8F39'))\n"
        "    $hw = 0\n"
        "    $desk = $sw.FindWindowSW(0, $null, 8, [ref]$hw, 1)\n"
        "    $desk.Document.Application.ShellExecute($ExePath, '', $InstallDir, 'open', 1)\n"
        "  } catch {\n"
        "    Start-Process -FilePath $ExePath\n"
        "  }\n"
        "} else {\n"
        "  Start-Process -FilePath $ExePath\n"
        "}\n"
        "if (-not $ok) { exit 1 }\n"
    );

    // §REVIEW-1104：放棄更新時的提示（PowerShell 單引號字串：' 要寫成 ''）
    QString failMsg = tr("The update was not installed because VipleStream did not exit in time. "
                         "Please close VipleStream and try updating again.");
    failMsg.replace(QStringLiteral("'"), QStringLiteral("''"));
    script.replace(QStringLiteral("__FAIL_MSG__"), failMsg);

    QFile sf(helperPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write helper script: %1").arg(sf.errorString()));
        return false;
    }
    // §REVIEW-1104：加 UTF-8 BOM，Windows PowerShell 5.1 才不會把翻譯後的提示文字當 ANSI 讀成亂碼
    // §REVIEW-1118：磁碟滿時 write 會少寫。原本不看回傳值，提權後跑的是半截腳本，程式也已經關了。
    const QByteArray scriptBytes = script.toUtf8();
    if (sf.write("\xEF\xBB\xBF", 3) != 3 || sf.write(scriptBytes) != scriptBytes.size() || !sf.flush()) {
        const QString err = sf.errorString();
        sf.close();
        QFile::remove(helperPath);
        fail(tr("Cannot write helper script: %1").arg(err));
        return false;
    }
    sf.close();

    bool needElevate = !isPathWritable(installDir);
    qDebug() << "[VIPLE-UPDATE] install dir" << redactHomePath(installDir)
             << "writable =" << !needElevate;

    // 構造 PowerShell argv：-NoProfile -ExecutionPolicy Bypass -File <script> -AppPid ... -ZipPath ...
    // §REVIEW-1119：連續 .arg() 會把路徑裡的 %2～%8 再換一次。八個參數一次代入就不會。
    // 路徑以單一反斜線結尾時（磁碟根目錄 D:\），CommandLineToArgvW 會把 \" 當成跳脫的引號。
    const auto nativeArg = [](QString path) {
        path = QDir::toNativeSeparators(path);
        if (path.endsWith(QLatin1Char('\\'))) {
            path.append(QLatin1Char('\\'));
        }
        return path;
    };
    QString args = QStringLiteral(
        "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        "-File \"%1\" -AppPid %2 -ZipPath \"%3\" -InstallDir \"%4\" -ExePath \"%5\" -DropAdmin %6 -Sha256 %7 -Version \"%8\""
    ).arg(nativeArg(helperPath),
          QString::number(pid),
          nativeArg(m_DownloadPath),
          nativeArg(installDir),
          nativeArg(exePath),
          QString::number(needElevate ? 1 : 0),
          m_AssetSha256,
          m_Version);

    // §REVIEW-1111（第二輪）：用系統目錄裡的 PowerShell 的完整路徑。只給檔名時會照 PATH 找，提權執行到的可能是別人放的同名程式。
    // §REVIEW-1119：找不到完整路徑就放棄。退回 "powershell.exe" 仍會被目錄或 PATH 劫持。
    QString psExe;
    {
        wchar_t sysDir[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            const QString full = QString::fromWCharArray(sysDir, int(n)) + QStringLiteral("\\WindowsPowerShell\\v1.0\\powershell.exe");
            if (QFileInfo::exists(full)) {
                psExe = full;
            }
        }
    }
    if (psExe.isEmpty()) {
        fail(tr("Failed to launch updater (UAC denied or PowerShell missing)."));
        return false;
    }
    if (!shellExecuteRunas(psExe, args, needElevate)) {
        // 使用者取消 UAC prompt 也會走這條
        fail(tr("Failed to launch updater (UAC denied or PowerShell missing)."));
        return false;
    }
    return true;

#elif defined(Q_OS_LINUX)
    if (m_Platform.packaging == UpdateAssetRules::Packaging::Deb) {
        // §CLIENT-DEB：下載檔在使用者可寫的暫存目錄。helper 會先複製再驗套件名與版號，
        // 所以不把 sha256 當參數傳進去（呼叫端可以連雜湊一起換掉）。
        // 安裝失敗不重開：更新模式 2 會一啟動就再叫 pkexec。下次使用者自己打開才再通知。
        const QString helperBin = QStringLiteral("/usr/libexec/viplestream-client/self-update");
        const QFileInfo helperInfo(helperBin);
        const QFile::Permissions helperPerms = helperInfo.permissions();
        if (helperInfo.isSymLink() || !helperInfo.isFile() || !helperInfo.isExecutable()
                || helperInfo.ownerId() != 0
                || (helperPerms & (QFileDevice::WriteGroup | QFileDevice::WriteOther))) {
            fail(tr("This install cannot update itself yet. Install the new .deb from the release page once, then try again."));
            return false;
        }
        const QFileInfo pkexec(QStringLiteral("/usr/bin/pkexec"));
        if (!pkexec.isExecutable()) {
            fail(tr("pkexec is not installed, so the update cannot be applied automatically."));
            return false;
        }

        const qint64 pid = QCoreApplication::applicationPid();
        const QString tempRoot = m_StageDir.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                : m_StageDir;
        const QString helperPath = QDir(tempRoot).filePath(QStringLiteral("viplestream_update_%1.sh").arg(m_Version));
        const QString script = QStringLiteral(
            "#!/bin/bash\n"
            "set -u\n"
            "APP_PID=\"$1\"\n"
            "DEB=\"$2\"\n"
            "HELPER=/usr/libexec/viplestream-client/self-update\n"
            "LOG_DIR=\"${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/viplestream\"\n"
            "umask 077\n"
            "mkdir -p \"$LOG_DIR\" 2>/dev/null || true\n"
            "LOG=\"$LOG_DIR/update.log\"\n"
            "log() { echo \"$(date '+%F %T') [VIPLE-UPDATE] $*\" >> \"$LOG\" 2>/dev/null || true; }\n"
            "note() {\n"
            "  log \"$1\"\n"
            "  if command -v notify-send >/dev/null 2>&1; then\n"
            "    notify-send \"VipleStream\" \"$1\" >/dev/null 2>&1 || true\n"
            "  fi\n"
            "}\n"
            "cleanup_download() {\n"
            "  rm -f -- \"$DEB\" \"$0\"\n"
            "  rmdir \"$(dirname -- \"$0\")\" 2>/dev/null || true\n"
            "}\n"
            "if [ -L \"$HELPER\" ] || [ ! -f \"$HELPER\" ] || [ ! -x \"$HELPER\" ]; then\n"
            "  note \"self-update helper is missing; update not installed\"\n"
            "  cleanup_download\n"
            "  exit 4\n"
            "fi\n"
            "owner=$(stat -c '%u' \"$HELPER\" 2>/dev/null || true)\n"
            "mode=$(stat -c '%a' \"$HELPER\" 2>/dev/null || true)\n"
            "case \"$mode\" in\n"
            "  ''|*[!0-7]*)\n"
            "    note \"self-update helper mode is unexpected; update not installed\"\n"
            "    cleanup_download\n"
            "    exit 4\n"
            "    ;;\n"
            "esac\n"
            "if [ \"$owner\" != 0 ] || [ $(( 8#$mode & 022 )) -ne 0 ]; then\n"
            "  note \"self-update helper is not root-owned or is writable; update not installed\"\n"
            "  cleanup_download\n"
            "  exit 4\n"
            "fi\n"
            "for i in $(seq 1 180); do\n"
            "  kill -0 \"$APP_PID\" 2>/dev/null || break\n"
            "  sleep 0.5\n"
            "done\n"
            "if kill -0 \"$APP_PID\" 2>/dev/null; then\n"
            "  note \"the app did not exit in time; update not installed\"\n"
            "  cleanup_download\n"
            "  exit 2\n"
            "fi\n"
            "log \"running pkexec self-update\"\n"
            "pkexec \"$HELPER\" \"$DEB\" >> \"$LOG\" 2>&1\n"
            "rc=$?\n"
            "if [ \"$rc\" -ne 0 ]; then\n"
            "  note \"deb install failed (rc $rc); the update notification returns the next time VipleStream starts\"\n"
            "  cleanup_download\n"
            "  exit 1\n"
            "fi\n"
            "log \"installed; restarting /usr/bin/viplestream\"\n"
            "cleanup_download\n"
            "nohup /usr/bin/viplestream >/dev/null 2>&1 &\n"
            "disown\n"
            "exit 0\n"
        );

        QFile sf(helperPath);
        if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            fail(tr("Cannot write helper script: %1").arg(sf.errorString()));
            return false;
        }
        const QByteArray scriptBytes = script.toUtf8();
        if (sf.write(scriptBytes) != scriptBytes.size() || !sf.flush()) {
            const QString err = sf.errorString();
            sf.close();
            QFile::remove(helperPath);
            fail(tr("Cannot write helper script: %1").arg(err));
            return false;
        }
        sf.close();
        QFile::setPermissions(helperPath,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
            QFileDevice::ReadGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::ExeOther);

        if (!QProcess::startDetached(QStringLiteral("/bin/bash"),
                                     QStringList{ helperPath, QString::number(pid), m_DownloadPath })) {
            fail(tr("Failed to launch updater helper script."));
            return false;
        }
        qDebug() << "[VIPLE-UPDATE] scheduling deb install via pkexec";
        return true;
    }

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

    QString tempRoot   = m_StageDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : m_StageDir;
    QString helperPath = QDir(tempRoot).filePath(QStringLiteral("viplestream_update_%1.sh").arg(m_Version));

    QString script = QStringLiteral(
        "#!/bin/bash\n"
        "APP_PID=\"$1\"\n"
        "NEW_APPIMAGE=\"$2\"\n"
        "CURRENT_APPIMAGE=\"$3\"\n"
        // §REVIEW-1111：和 Windows 一樣等 90 秒（VR 收尾可以超過 30 秒），還沒結束就放棄。原本 30 秒後照樣覆蓋並重開：
        // 舊行程還握著單一實例鎖，新的那個被擋掉就退出，使用者看到的是「更新後程式沒有再打開」。
        "for i in $(seq 1 180); do\n"
        "  kill -0 \"$APP_PID\" 2>/dev/null || break\n"
        "  sleep 0.5\n"
        "done\n"
        "if kill -0 \"$APP_PID\" 2>/dev/null; then\n"
        "  echo \"VipleStream updater: the app did not exit in time; update not installed\" >&2\n"
        "  exit 2\n"
        "fi\n"
        "chmod +x \"$NEW_APPIMAGE\" || exit 1\n"
        "mv -f \"$NEW_APPIMAGE\" \"$CURRENT_APPIMAGE\" || exit 1\n"
        "rm -f \"$0\"; rmdir \"$(dirname \"$0\")\" 2>/dev/null\n"
        "nohup \"$CURRENT_APPIMAGE\" >/dev/null 2>&1 &\n"
        "disown\n"
    );

    QFile sf(helperPath);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("Cannot write helper script: %1").arg(sf.errorString()));
        return false;
    }
    // §REVIEW-1118：同 Windows。寫不完整就不要交給 bash，否則更新會安靜失敗、程式也已經關了。
    const QByteArray scriptBytes = script.toUtf8();
    if (sf.write(scriptBytes) != scriptBytes.size() || !sf.flush()) {
        const QString err = sf.errorString();
        sf.close();
        QFile::remove(helperPath);
        fail(tr("Cannot write helper script: %1").arg(err));
        return false;
    }
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
