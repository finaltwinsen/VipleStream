#pragma once

// §F9（VipleStream 2.0 M0 前置）：client updater 依「作業系統＋架構＋打包型態」
// 精確選 release asset。
//
// 舊規則（updater.cpp 舊版 selectAssetForPlatform）的問題：
//   - Windows：「含 Client、.zip 結尾、不含 linux」→ 會選到 -debug.zip（PDB 包），
//     或未來的 Windows arm64 zip；選到哪個取決於 GitHub 回傳 asset 的順序。
//   - Linux：「.AppImage 結尾且含 linux」→ arm64 client 會下載 x64 AppImage。
//
// 新規則：一律以完整檔名錨定比對（前綴、版號、架構、副檔名），並把
// 「這個 asset 是不是給本平台的」和「本平台能不能自己安裝」分開判斷：
//
//   平台（建置架構）       接受的檔名                                        動作
//   Windows x64            VipleStream-Client-X.Y.Z.zip                      自動安裝
//   Linux x86_64           VipleStream-Client-X.Y.Z-linux-x64.AppImage       從 AppImage 執行才自動安裝，
//                                                                            否則只通知
//   Linux arm64            VipleStream-Client-X.Y.Z-linux-arm64.flatpak      只通知（使用者自己裝）
//                          VipleStream-Client-X.Y.Z-linux-arm64.zip          只通知
//   其他架構／其他 OS                                                         不選
//
// 架構看的是「這支 binary 的建置架構」（QSysInfo::buildCpuArchitecture），
// 因為 asset 是要拿來覆蓋自己這支 binary 的：x64 build 在 arm64 上模擬執行
// （Windows on ARM、box64／FEX）時，該換上的仍是 x64 的 zip／AppImage，換成
// arm64 版反而會壞掉安裝目錄。currentCpuArchitecture 只拿來記 log、標示模擬。
//
// 另外，檔名裡的版號必須等於「這次 GET /releases/latest 實際抓到的 release
// 的 tag_name（去掉 v/V）」——不是 UI 啟動時看到、傳給 startUpdate 的版號，
// 兩者可能不同（程式常駐期間又發了新版；或 API 有 max-age 快取，比 HEAD 302
// 慢）。版號對齊擋的是 release 混進舊版 asset（v1.3.310 事故：release 叫
// 1.3.310、Client zip 是 1.3.308）：裝下去之後 AutoUpdateChecker 會一直說
// 「有新版」→ 無限更新迴圈；寧可不選，讓使用者走 release 頁面。抓到的 tag
// 若不比目前版號新（API 快取落後），planRelease 回 NotNewer，不去比 asset。
//
// 這支 header 只依賴 QtCore、沒有 Q_OBJECT、平台資訊與 release JSON 由呼叫端
// 傳入，所以離線測試可以直接 include、模擬各平台＋各種 release JSON 跑決策，
// 不必連網也不必建整個 client。平台偵測（detectPlatform）只讀 QSysInfo、
// /.flatpak-info 與 AppImage runtime 注入的 APPIMAGE——都是「偵測執行環境」，
// 不是設定開關。

#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <QRegularExpression>
#include <QSysInfo>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QVector>

namespace UpdateAssetRules {

enum class Os { Windows, Linux, Other };
enum class Arch { X64, Arm64, Other };
enum class Packaging { Plain, AppImage, Flatpak };

// 整個平台的更新方式（和這次 release 裡有哪些 asset 無關）
enum class InstallMode { AutoInstall, NotifyOnly, Unsupported };

// 單一 asset 對本平台的意義
enum class AssetAction { Skip, AutoInstall, NotifyOnly };

struct Platform {
    Os os = Os::Other;
    Arch arch = Arch::Other;       // 建置架構（asset 要覆蓋的就是這支 binary）
    Packaging packaging = Packaging::Plain;
    QString currentArch;  // QSysInfo 原始字串，只用於 log
    QString buildArch;
    bool emulated = false;         // 執行架構≠建置架構；只用於 log，不影響選擇
};

inline Arch normalizeArch(const QString& a)
{
    const QString s = a.trimmed().toLower();
    if (s == QLatin1String("x86_64") || s == QLatin1String("amd64") || s == QLatin1String("x64")) {
        return Arch::X64;
    }
    if (s == QLatin1String("arm64") || s == QLatin1String("aarch64")) {
        return Arch::Arm64;
    }
    return Arch::Other;
}

// arch 取建置架構：asset 是要覆蓋「自己這支 binary」的（見檔頭）。執行架構和
// 建置架構不一致（模擬器底下跑）只標 emulated 供 log，不因此判成不支援——
// x64 build 模擬跑在 arm64 上時，換上 x64 zip／AppImage 才是對的，舊規則也是
// 這樣做；arm64 build 則一定只會選到 arm64 asset，不會拿到 x64 的。
inline Platform makePlatform(Os os, const QString& currentArch, const QString& buildArch,
                             Packaging packaging)
{
    Platform p;
    p.os = os;
    p.currentArch = currentArch;
    p.buildArch = buildArch;
    p.packaging = packaging;
    p.arch = normalizeArch(buildArch);
    p.emulated = normalizeArch(currentArch) != p.arch;
    return p;
}

inline Platform detectPlatform()
{
#if defined(Q_OS_WIN)
    const Os os = Os::Windows;
#elif defined(Q_OS_LINUX)
    const Os os = Os::Linux;
#else
    const Os os = Os::Other;
#endif
    Packaging packaging = Packaging::Plain;
#if defined(Q_OS_LINUX)
    // Flatpak 沙箱一定有 /.flatpak-info（flatpak 官方的判斷方式）；
    // AppImage runtime 會注入 APPIMAGE＝.AppImage 本體路徑（spawnHelperAndQuit 也靠它）。
    if (QFileInfo::exists(QStringLiteral("/.flatpak-info"))) {
        packaging = Packaging::Flatpak;
    }
    else if (qEnvironmentVariableIsSet("APPIMAGE")) {
        packaging = Packaging::AppImage;
    }
#endif
    return makePlatform(os, QSysInfo::currentCpuArchitecture(),
                        QSysInfo::buildCpuArchitecture(), packaging);
}

inline InstallMode installMode(const Platform& p)
{
    if (p.os == Os::Windows && p.arch == Arch::X64) {
        return InstallMode::AutoInstall;
    }
    if (p.os == Os::Linux && p.arch == Arch::X64) {
        // 只有從 AppImage 執行時才有「自己那個檔案」可以換掉
        return p.packaging == Packaging::AppImage ? InstallMode::AutoInstall
                                                  : InstallMode::NotifyOnly;
    }
    if (p.os == Os::Linux && p.arch == Arch::Arm64) {
        return InstallMode::NotifyOnly;
    }
    return InstallMode::Unsupported;
}

// 判斷單一 asset 檔名。expectedVersion＝實際抓到的 release 的 tag（去掉 v/V），
// 由 planRelease 傳入；為空時不檢查版號（只給測試／log 用）。
// reason（可為 nullptr）填入跳過原因，供 log。
inline AssetAction classifyAsset(const QString& name, const Platform& p,
                                 const QString& expectedVersion, QString* reason = nullptr)
{
    // 精確比對：大小寫敏感、整串錨定
    static const QRegularExpression winX64(
        QStringLiteral("^VipleStream-Client-(\\d+\\.\\d+\\.\\d+)\\.zip$"));
    static const QRegularExpression linuxX64(
        QStringLiteral("^VipleStream-Client-(\\d+\\.\\d+\\.\\d+)-linux-x64\\.AppImage$"));
    static const QRegularExpression linuxArm64(
        QStringLiteral("^VipleStream-Client-(\\d+\\.\\d+\\.\\d+)-linux-arm64\\.(flatpak|zip)$"));

    const QRegularExpression* re = nullptr;
    AssetAction action = AssetAction::Skip;
    if (p.os == Os::Windows && p.arch == Arch::X64) {
        re = &winX64;
        action = AssetAction::AutoInstall;
    }
    else if (p.os == Os::Linux && p.arch == Arch::X64) {
        re = &linuxX64;
        action = p.packaging == Packaging::AppImage ? AssetAction::AutoInstall
                                                    : AssetAction::NotifyOnly;
    }
    else if (p.os == Os::Linux && p.arch == Arch::Arm64) {
        re = &linuxArm64;
        action = AssetAction::NotifyOnly;
    }
    else {
        if (reason) *reason = QStringLiteral("platform not supported");
        return AssetAction::Skip;
    }

    const QRegularExpressionMatch m = re->match(name);
    if (!m.hasMatch()) {
        if (reason) *reason = QStringLiteral("name does not match this platform");
        return AssetAction::Skip;
    }
    if (!expectedVersion.isEmpty() && m.captured(1) != expectedVersion) {
        if (reason) {
            *reason = QStringLiteral("version %1 != release %2").arg(m.captured(1), expectedVersion);
        }
        return AssetAction::Skip;
    }
    return action;
}

// 從 asset 名單挑一個。回傳 index（-1 表示沒有），outAction 填動作。
// 優先序：自動安裝 > 符合打包型態的只通知（Flatpak 優先 .flatpak，其餘優先 .zip）
// > 其他只通知；同級取第一個出現的。
inline int selectAsset(const QStringList& names, const Platform& p,
                       const QString& expectedVersion, AssetAction* outAction)
{
    int bestIndex = -1;
    int bestRank = 0;
    AssetAction bestAction = AssetAction::Skip;
    for (int i = 0; i < names.size(); i++) {
        const AssetAction a = classifyAsset(names.at(i), p, expectedVersion);
        int rank = 0;
        if (a == AssetAction::AutoInstall) {
            rank = 3;
        }
        else if (a == AssetAction::NotifyOnly) {
            const bool preferFlatpak = (p.packaging == Packaging::Flatpak);
            const bool isFlatpak = names.at(i).endsWith(QLatin1String(".flatpak"));
            rank = (isFlatpak == preferFlatpak) ? 2 : 1;
        }
        if (rank > bestRank) {
            bestRank = rank;
            bestIndex = i;
            bestAction = a;
        }
    }
    if (outAction) *outAction = bestAction;
    return bestIndex;
}

// ── 版號工具：語意與 AutoUpdateChecker::parseStringToVersionQuad／compareVersion
//    一致（只接受「數字.數字[.數字…]」、缺的位數視為 0），兩邊判斷「比較新」
//    的標準才不會分岔。

// 去掉 GitHub tag 開頭的 v/V（v1.5.276 → 1.5.276）
inline QString versionFromTag(const QString& tag)
{
    QString v = tag.trimmed();
    if (v.startsWith(QLatin1Char('v')) || v.startsWith(QLatin1Char('V'))) {
        v = v.mid(1);
    }
    return v;
}

inline bool parseVersion(const QString& s, QVector<int>* out)
{
    out->clear();
    const QStringList parts = s.split(QLatin1Char('.'));
    if (parts.size() < 2) {
        return false;
    }
    for (const QString& c : parts) {
        bool ok = false;
        const int v = c.toInt(&ok);
        if (!ok || v < 0) {
            out->clear();
            return false;
        }
        out->append(v);
    }
    return true;
}

// a<b → -1、a==b → 0、a>b → 1
inline int compareVersions(const QVector<int>& a, const QVector<int>& b)
{
    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; i++) {
        const int av = i < a.size() ? a.at(i) : 0;
        const int bv = i < b.size() ? b.at(i) : 0;
        if (av != bv) {
            return av < bv ? -1 : 1;
        }
    }
    return 0;
}

// ── 整個 release 的決策：Updater 收到 GET /releases/latest 的 JSON 之後只呼叫這個。
//    放在這裡（而不是 Updater 裡）是為了讓離線測試直接餵 JSON，連
//    「expectedVersion 從哪裡來」也一起驗到。

enum class PlanResult {
    Ok,          // 選到 asset（action 為 AutoInstall 或 NotifyOnly）
    MissingTag,  // JSON 沒有 tag_name
    BadTag,      // tag 不是版號格式
    NotNewer,    // 抓到的 release 不比目前版號新（API 快取落後、release 被撤等）
    NoAssets,    // release 沒掛任何可下載的 asset
    NoMatch,     // 有 asset，但沒有本平台、且版號等於 tag 的那一個
};

struct ReleasePlan {
    PlanResult result = PlanResult::NoMatch;
    QString tagName;          // 原始 tag_name
    QString releaseVersion;   // tag 去掉 v/V；asset 版號必須等於它
    AssetAction action = AssetAction::Skip;
    QString assetName;
    QString assetUrl;
    QStringList assetNames;   // 全部 asset 名稱，給 log
    QStringList skippedClient;  // 被跳過的 Client asset：「名稱 — 原因」，給 log
};

// currentVersion＝執行中的版號（main.cpp 以 VERSION_STR 設給
// QCoreApplication::applicationVersion）。解析不了時不擋 NotNewer，只靠
// 「asset 版號＝tag」這一道。
inline ReleasePlan planRelease(const QJsonObject& release, const Platform& p,
                               const QString& currentVersion)
{
    ReleasePlan plan;
    plan.tagName = release.value(QLatin1String("tag_name")).toString();
    if (plan.tagName.trimmed().isEmpty()) {
        plan.result = PlanResult::MissingTag;
        return plan;
    }
    plan.releaseVersion = versionFromTag(plan.tagName);

    QVector<int> rel;
    if (!parseVersion(plan.releaseVersion, &rel)) {
        plan.result = PlanResult::BadTag;
        return plan;
    }
    QVector<int> cur;
    if (parseVersion(currentVersion, &cur) && compareVersions(rel, cur) <= 0) {
        plan.result = PlanResult::NotNewer;
        return plan;
    }

    QStringList urls;
    const QJsonArray assets = release.value(QLatin1String("assets")).toArray();
    for (const QJsonValue& v : assets) {
        const QJsonObject obj = v.toObject();
        const QString name = obj.value(QLatin1String("name")).toString();
        const QString dl   = obj.value(QLatin1String("browser_download_url")).toString();
        if (name.isEmpty() || dl.isEmpty()) continue;
        plan.assetNames.append(name);
        urls.append(dl);
    }
    if (plan.assetNames.isEmpty()) {
        plan.result = PlanResult::NoAssets;
        return plan;
    }

    // 只收集 Client asset 的跳過原因（Server／Android 一定跳過，記了是雜訊）
    for (const QString& name : plan.assetNames) {
        if (!name.startsWith(QLatin1String("VipleStream-Client-"))) continue;
        QString reason;
        if (classifyAsset(name, p, plan.releaseVersion, &reason) == AssetAction::Skip) {
            plan.skippedClient.append(name + QStringLiteral(" — ") + reason);
        }
    }

    const int idx = selectAsset(plan.assetNames, p, plan.releaseVersion, &plan.action);
    if (idx < 0) {
        plan.result = PlanResult::NoMatch;
        return plan;
    }
    plan.assetName = plan.assetNames.at(idx);
    plan.assetUrl  = urls.at(idx);
    plan.result    = PlanResult::Ok;
    return plan;
}

inline const char* toString(Os v)
{
    switch (v) {
    case Os::Windows: return "windows";
    case Os::Linux:   return "linux";
    default:          return "other";
    }
}

inline const char* toString(Arch v)
{
    switch (v) {
    case Arch::X64:   return "x64";
    case Arch::Arm64: return "arm64";
    default:          return "other";
    }
}

inline const char* toString(Packaging v)
{
    switch (v) {
    case Packaging::AppImage: return "appimage";
    case Packaging::Flatpak:  return "flatpak";
    default:                  return "plain";
    }
}

inline const char* toString(InstallMode v)
{
    switch (v) {
    case InstallMode::AutoInstall: return "auto-install";
    case InstallMode::NotifyOnly:  return "notify-only";
    default:                       return "unsupported";
    }
}

inline const char* toString(AssetAction v)
{
    switch (v) {
    case AssetAction::AutoInstall: return "auto-install";
    case AssetAction::NotifyOnly:  return "notify-only";
    default:                       return "skip";
    }
}

inline const char* toString(PlanResult v)
{
    switch (v) {
    case PlanResult::Ok:         return "ok";
    case PlanResult::MissingTag: return "missing-tag";
    case PlanResult::BadTag:     return "bad-tag";
    case PlanResult::NotNewer:   return "not-newer";
    case PlanResult::NoAssets:   return "no-assets";
    default:                     return "no-match";
    }
}

// log 用的一行平台描述，例：linux/arm64 (cpu arm64, build arm64, flatpak)；
// 模擬執行時多一個 emulated，例：windows/x64 (cpu arm64, build x86_64, plain, emulated)
inline QString describe(const Platform& p)
{
    return QStringLiteral("%1/%2 (cpu %3, build %4, %5%6)")
        .arg(QLatin1String(toString(p.os)), QLatin1String(toString(p.arch)),
             p.currentArch, p.buildArch, QLatin1String(toString(p.packaging)),
             p.emulated ? QStringLiteral(", emulated") : QString());
}

} // namespace UpdateAssetRules
