// VipleStream §SF-PROBE（M2a）— OpenXR active runtime JSON 的探索（實作）。設計背景見
// xrruntimejson.h。
//
// 對照來源：OpenXR-SDK release-1.1.63（f2448a87）
//   - src/loader/manifest_file.cpp：FindXDGConfigFile() 的順序是 $XDG_CONFIG_HOME（空時
//     $HOME/.config）→ $XDG_CONFIG_DIRS（空時 /etc/xdg）→ 編譯時的 SYSCONFDIR → EXTRASYSCONFDIR
//     （SYSCONFDIR 不是 /etc 時才有，值是 /etc）。每個目錄先找 active_runtime.<arch>.json，
//     再找 active_runtime.json；判斷只用「路徑存在」（stat），不管讀不讀得到。
//     XR_RUNTIME_JSON 有設（非空）時 loader 只用它，完全不走上面的搜尋。
//   - 同檔 RuntimeManifestFile::CreateIfValid()：library_path 含 '/' 或 '\' 而且是相對路徑時，
//     以「manifest 的 canonical 路徑（解開 symlink 之後）」所在目錄為基準；不含路徑分隔字元
//     的裸檔名交給動態連結器的搜尋路徑。
//   - src/common/platform_utils.hpp：XR_ARCH_ABI 的字串（下面 archAbi() 照抄同一串判斷）。
//
// Flatpak 的 loader 以 prefix=/app 建置，所以 SYSCONFDIR=/app/etc、EXTRASYSCONFDIR=/etc；
// 一般發行版套件 prefix=/usr，SYSCONFDIR 就是 /etc。

#include "xrruntimejson.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringList>
#endif

namespace XrRuntimeJson {

#if defined(Q_OS_LINUX)

namespace {

const char* const kSrcEnv = "XR_RUNTIME_JSON";
const char* const kSrcXdgHome = "XDG_CONFIG_HOME";
const char* const kSrcHost = "host-config";
const char* const kSrcXdgDirs = "XDG_CONFIG_DIRS";
const char* const kSrcEtc = "etc";

// runtime JSON 只有幾百 bytes；讀取上限只是防呆（誤指到大檔時不要整個讀進來）。
const qint64 kMaxJsonBytes = 1024 * 1024;

// 與 OpenXR loader 的 XR_ARCH_ABI 完全相同的判斷鏈（platform_utils.hpp，release-1.1.63）。
// 注意是 uname 風格而不是 Debian 風格：x86_64、aarch64，不是 amd64、arm64。
const char* archAbi()
{
#if defined(__x86_64__) && defined(__ILP32__)
    return "x32";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__) || defined(_X86_)
    return "i686";
#elif (defined(__aarch64__) && defined(__LP64__)) || defined(_M_ARM64)
    return "aarch64";
#elif (defined(__ARM_ARCH) && __ARM_ARCH >= 7 && (defined(__ARM_PCS_VFP) || defined(__ANDROID__))) || defined(_M_ARM)
    return "armv7a-vfp";
#elif defined(__ARM_ARCH_5TE__) || (defined(__ARM_ARCH) && __ARM_ARCH > 5)
    return "armv5te";
#elif defined(__mips64)
    return "mips64";
#elif defined(__mips)
    return "mips";
#elif defined(__powerpc64__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return "ppc64";
#elif defined(__powerpc__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return "ppc64el";
#elif defined(__s390x__) || defined(__zarch__)
    return "s390x";
#elif defined(__hppa__)
    return "hppa";
#elif defined(__alpha__)
    return "alpha";
#elif defined(__ia64__) || defined(_M_IA64)
    return "ia64";
#elif defined(__m68k__)
    return "m68k";
#elif defined(__riscv_xlen) && (__riscv_xlen == 64)
    return "riscv64";
#elif defined(__sparc__) && defined(__arch64__)
    return "sparc64";
#elif defined(__loongarch64)
    return "loongarch64";
#else
    // loader 在這種架構上直接 #error；這裡只是不列 decorated 名稱
    return nullptr;
#endif
}

bool isInFlatpak()
{
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"));
}

bool containsPath(const QList<Candidate>& list, const QString& path)
{
    for (const Candidate& c : list) {
        if (c.path == path) {
            return true;
        }
    }
    return false;
}

void addFile(QList<Candidate>& out, const QString& path, const char* source)
{
    if (path.isEmpty() || containsPath(out, path)) {
        // 同一個檔案只列一次，先出現的來源優先（例如 XDG_CONFIG_DIRS 重複列了 /etc/xdg）
        return;
    }
    Candidate c;
    c.path = path;
    c.source = QString::fromLatin1(source);
    // QFileInfo::exists 會跟隨 symlink：懸空的 symlink（SteamVR 的 active_runtime.json 指到
    // 沙箱外的 SteamVR 目錄）算不存在，與 loader 的 stat() 判斷一致。
    const QFileInfo fi(path);
    c.exists = fi.exists();
    if (c.exists && fi.isFile()) {
        // 實際開檔判斷，權限位元以外的限制（ACL、沙箱唯讀掛載、AppArmor）也算進去
        QFile f(path);
        c.readable = f.open(QIODevice::ReadOnly);
    }
    out.append(c);
}

// <configDir>/openxr/1/ 下的兩個檔名，順序同 loader：先 decorated 再 undecorated。
void addConfigDir(QList<Candidate>& out, const QString& configDir, const char* source)
{
    if (configDir.isEmpty()) {
        return;
    }
    const QString base = QDir::cleanPath(configDir) + QStringLiteral("/openxr/1/");
    if (const char* arch = archAbi()) {
        addFile(out, base + QStringLiteral("active_runtime.") + QString::fromLatin1(arch) + QStringLiteral(".json"), source);
    }
    addFile(out, base + QStringLiteral("active_runtime.json"), source);
}

void parseRuntimeJson(Resolved& res)
{
    QFile f(res.jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        res.parseError = QStringLiteral("open failed: ") + f.errorString();
        return;
    }
    const QByteArray data = f.read(kMaxJsonBytes);
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
    if (pe.error != QJsonParseError::NoError) {
        res.parseError = QStringLiteral("JSON parse error at offset %1: %2").arg(pe.offset).arg(pe.errorString());
        return;
    }
    if (!doc.isObject()) {
        res.parseError = QStringLiteral("top-level JSON is not an object");
        return;
    }
    const QJsonObject runtime = doc.object().value(QStringLiteral("runtime")).toObject();
    res.runtimeName = runtime.value(QStringLiteral("name")).toString();
    QString lib = runtime.value(QStringLiteral("library_path")).toString();
    if (lib.isEmpty()) {
        res.parseError = QStringLiteral("runtime.library_path missing or not a string");
        return;
    }

    if (lib.contains(QLatin1Char('/')) || lib.contains(QLatin1Char('\\'))) {
        if (QDir::isRelativePath(lib)) {
            // 同 loader：相對於「解開 symlink 之後」的 manifest 所在目錄。SteamVR 的
            // active_runtime.json 通常是指向 SteamVR 目錄內 steamxr_linux64.json 的 symlink，
            // 以 symlink 本身的目錄解析會得到錯的路徑。
            QString canonical = QFileInfo(res.jsonPath).canonicalFilePath();
            if (canonical.isEmpty()) {
                canonical = res.jsonPath;
            }
            lib = QDir::cleanPath(QFileInfo(canonical).absolutePath() + QLatin1Char('/') + lib);
        }
        res.libraryExists = QFileInfo::exists(lib);
    }
    else {
        // 裸檔名：由動態連結器的搜尋路徑決定（LD_LIBRARY_PATH、ld.so.cache），這裡無法
        // 確認，libraryExists 維持 false；xr-probe 失敗診斷時會實際 dlopen 看 dlerror()。
        res.libraryExists = false;
    }
    res.libraryPath = lib;
}

} // namespace

QList<Candidate> candidates()
{
    QList<Candidate> out;

    // 1. XR_RUNTIME_JSON：loader 看到非空值就只用它
    const QString envJson = qEnvironmentVariable("XR_RUNTIME_JSON");
    if (!envJson.isEmpty()) {
        addFile(out, envJson, kSrcEnv);
    }

    const QString home = qEnvironmentVariable("HOME");

    // 2. $XDG_CONFIG_HOME（空時 $HOME/.config，同 loader 的 GetXDGEnvHome）
    QString xdgConfigHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (xdgConfigHome.isEmpty() && !home.isEmpty()) {
        xdgConfigHome = home + QStringLiteral("/.config");
    }
    addConfigDir(out, xdgConfigHome, kSrcXdgHome);

    // 3. Flatpak 內：host 的設定目錄。flatpak 把 XDG_CONFIG_HOME 改到 ~/.var/app/<id>/config，
    //    host 原本的值放在 HOST_XDG_CONFIG_HOME（有設時）；沙箱內的 HOME 仍是真的家目錄，
    //    所以 $HOME/.config 也列（實際看不看得到取決於 finish-args 的 filesystem 權限）。
    //    host 的系統層設定（例如 Monado 套件的 /etc/xdg/openxr/1/active_runtime.json）只有在
    //    --filesystem=host-etc 時才看得到，掛在 /run/host/etc；排在使用者層之後。
    if (isInFlatpak()) {
        addConfigDir(out, qEnvironmentVariable("HOST_XDG_CONFIG_HOME"), kSrcHost);
        if (!home.isEmpty()) {
            addConfigDir(out, home + QStringLiteral("/.config"), kSrcHost);
        }
        addConfigDir(out, QStringLiteral("/run/host/etc/xdg"), kSrcHost);
        addConfigDir(out, QStringLiteral("/run/host/etc"), kSrcHost);
    }

    // 4. $XDG_CONFIG_DIRS（空時 /etc/xdg），以 ':' 分隔、略過空項目
    QString xdgConfigDirs = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (xdgConfigDirs.isEmpty()) {
        xdgConfigDirs = QStringLiteral("/etc/xdg");
    }
    const QStringList dirs = xdgConfigDirs.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& dir : dirs) {
        addConfigDir(out, dir, kSrcXdgDirs);
    }

    // 5. SYSCONFDIR 與 EXTRASYSCONFDIR：Flatpak 內 loader 的 SYSCONFDIR 是 /app/etc
    if (isInFlatpak()) {
        addConfigDir(out, QStringLiteral("/app/etc"), kSrcEtc);
    }
    addConfigDir(out, QStringLiteral("/etc"), kSrcEtc);

    return out;
}

Resolved resolveActive()
{
    Resolved res;
    const QList<Candidate> list = candidates();
    if (list.isEmpty()) {
        return res;
    }

    // XR_RUNTIME_JSON 有設時 loader 沒有退路：這裡也只看它，不往下找（否則會報出一個
    // loader 根本不會用的檔案）。
    const bool envOverride = list.first().source == QLatin1String(kSrcEnv);

    // loader 只用 stat() 判斷存在：若更前面有 loader 看得到、存在但讀不到的檔案，loader
    // 會選中它然後失敗，那麼後面這個可讀的候選 loader 自己是找不到的。
    bool shadowedByUnreadable = false;

    for (const Candidate& c : list) {
        if (envOverride && c.source != QLatin1String(kSrcEnv)) {
            break;
        }
        const bool loaderVisible = c.source != QLatin1String(kSrcHost);
        if (!c.exists) {
            continue;
        }
        if (!c.readable) {
            if (loaderVisible) {
                shadowedByUnreadable = true;
            }
            continue;
        }
        res.found = true;
        res.jsonPath = c.path;
        res.source = c.source;
        res.loaderWouldFind = loaderVisible && !shadowedByUnreadable;
        parseRuntimeJson(res);
        return res;
    }

    if (envOverride) {
        // 指定的檔案不存在或讀不到：照實回報那個路徑，呼叫端才看得出是覆寫本身有問題
        const Candidate& c = list.first();
        res.jsonPath = c.path;
        res.source = c.source;
        res.parseError = c.exists ? QStringLiteral("XR_RUNTIME_JSON file not readable")
                                  : QStringLiteral("XR_RUNTIME_JSON file does not exist");
    }
    return res;
}

#else // !Q_OS_LINUX

// 非 Linux：Windows 的 loader 讀登錄檔（HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime），
// 等 M3a 的 Windows OpenXR（S2）再實作。
QList<Candidate> candidates()
{
    return QList<Candidate>();
}

Resolved resolveActive()
{
    return Resolved();
}

#endif

} // namespace XrRuntimeJson
