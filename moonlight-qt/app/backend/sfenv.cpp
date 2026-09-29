// VipleStream §SF-ENV（M2a）— Steam Frame 執行環境摘要（實作）。設計背景與成本規則見 sfenv.h。
//
// 三個出口共用同一份「便宜資訊」收集（collectCheap）：
//   logOnce()     → 兩行 `[VIPLE-SF-ENV]`（啟動時；探測動作在 QCoreApplication 派發時）
//   summaryLine() → 快取的精簡版＋SDL 當下的 video driver／app id（session 開場）
//   snapshot()    → 重新收集一次，再加 Vulkan 裝置與 V4L2 QUERYCAP（只有探測動作）
// 非 Linux：logOnce／summaryLine 什麼都不做，snapshot 只有平台欄位。

#include "sfenv.h"

#include "backend/updateassetrules.h"
#include "streaming/video/v4l2/v4l2caps.h"
#include "streaming/xr/xrruntimejson.h"
#include "utils.h"

#include "SDL_compat.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QSysInfo>
#include <QtGlobal>

#include <atomic>
#include <cstdint>
#include <vector>

#if defined(Q_OS_LINUX)
#include <dlfcn.h>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
// VkPhysicalDeviceDriverProperties 是 1.2 核心結構；更舊的 header 就不列 Vulkan 裝置
#if defined(VK_API_VERSION_1_2)
#define VIPLE_SFENV_HAVE_VULKAN 1
#endif
#endif
#endif

namespace {

using UpdateAssetRules::Packaging;

QString packagingName(Packaging p)
{
    switch (p) {
    case Packaging::Flatpak:
        return QStringLiteral("flatpak");
    case Packaging::AppImage:
        return QStringLiteral("appimage");
    case Packaging::Plain:
    default:
        return QStringLiteral("plain");
    }
}

QString osName(UpdateAssetRules::Os os)
{
    switch (os) {
    case UpdateAssetRules::Os::Windows:
        return QStringLiteral("windows");
    case UpdateAssetRules::Os::Linux:
        return QStringLiteral("linux");
    case UpdateAssetRules::Os::Other:
    default:
        return QStringLiteral("other");
    }
}

// 所有平台都有的欄位（snapshot 的 "platform"）
QJsonObject platformJson(const UpdateAssetRules::Platform& p)
{
    QJsonObject o;
    o[QStringLiteral("os")] = osName(p.os);
    o[QStringLiteral("buildArch")] = p.buildArch;
    o[QStringLiteral("currentArch")] = p.currentArch;
    o[QStringLiteral("emulated")] = p.emulated;
    o[QStringLiteral("packaging")] = packagingName(p.packaging);
    o[QStringLiteral("kernelType")] = QSysInfo::kernelType();
    o[QStringLiteral("kernelVersion")] = QSysInfo::kernelVersion();
    o[QStringLiteral("productType")] = QSysInfo::productType();
    o[QStringLiteral("productVersion")] = QSysInfo::productVersion();
    o[QStringLiteral("prettyProductName")] = QSysInfo::prettyProductName();
    return o;
}

#if defined(Q_OS_LINUX)

// ── 原始環境變數（captureOriginalEnv） ──
// main() 最前面寫入一次（單執行緒），之後只讀。
struct OriginalEnv {
    bool captured = false;
    bool qpaSet = false;
    QByteArray qpa;
    bool sdlSet = false;
    QByteArray sdl;
};

OriginalEnv& originalEnv()
{
    static OriginalEnv s_Env;
    return s_Env;
}

// log 用：unset／""（有設但空）／原值。只用在白名單內的變數。
QString envForLog(const char* name)
{
    if (!qEnvironmentVariableIsSet(name)) {
        return QStringLiteral("unset");
    }
    const QString v = qEnvironmentVariable(name);
    return v.isEmpty() ? QStringLiteral("\"\"") : v;
}

QJsonValue envForJson(const char* name)
{
    if (!qEnvironmentVariableIsSet(name)) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonValue(qEnvironmentVariable(name));
}

// QT_QPA_PLATFORM／SDL_VIDEODRIVER：原始值與目前值不同時印成 `orig->now`。
QString origAndNowForLog(const char* name, bool origSet, const QByteArray& orig)
{
    const QString now = envForLog(name);
    if (!originalEnv().captured) {
        // main.cpp 沒呼叫 captureOriginalEnv（不該發生）：只能記目前值
        return now + QStringLiteral("(orig=?)");
    }
    const QString o = !origSet ? QStringLiteral("unset")
                               : (orig.isEmpty() ? QStringLiteral("\"\"") : QString::fromLocal8Bit(orig));
    return o == now ? now : o + QStringLiteral("->") + now;
}

QJsonObject origAndNowForJson(const char* name, bool origSet, const QByteArray& orig)
{
    QJsonObject o;
    if (originalEnv().captured) {
        o[QStringLiteral("original")] = origSet ? QJsonValue(QString::fromLocal8Bit(orig)) : QJsonValue(QJsonValue::Null);
    }
    o[QStringLiteral("current")] = envForJson(name);
    return o;
}

// 過長的欄位（Flatpak filesystems 清單等）在 log 行內截斷；完整內容在 snapshot。
QString clip(const QString& s, int max = 240)
{
    if (s.size() <= max) {
        return s;
    }
    return s.left(max) + QStringLiteral("...");
}

// ── 小型檔案解析 ──

// /.flatpak-info 是 GKeyFile 格式：[Section]、key=value、# 註解。值原樣保留（分號清單不拆）。
using IniSection = QMap<QString, QString>;
using IniFile = QMap<QString, IniSection>;

IniFile readIni(const QString& path)
{
    IniFile ini;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return ini;
    }
    QString section;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine(8192)).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || section.isEmpty()) {
            continue;
        }
        ini[section][line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
    }
    return ini;
}

// os-release：KEY=value，值可能用單或雙引號包住。
QMap<QString, QString> readOsRelease(const QString& path)
{
    QMap<QString, QString> m;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return m;
    }
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine(4096)).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            continue;
        }
        QString v = line.mid(eq + 1).trimmed();
        if (v.size() >= 2 &&
            ((v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) ||
             (v.startsWith(QLatin1Char('\'')) && v.endsWith(QLatin1Char('\''))))) {
            v = v.mid(1, v.size() - 2);
            v.replace(QStringLiteral("\\\""), QStringLiteral("\""));
            v.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        }
        m[line.left(eq).trimmed()] = v;
    }
    return m;
}

// log 用的一段：ID/VERSION_ID[,variant=…][,build=…]；讀不到時回 "?"
QString osBrief(const QMap<QString, QString>& m)
{
    if (m.isEmpty()) {
        return QStringLiteral("?");
    }
    QString s = m.value(QStringLiteral("ID"), QStringLiteral("?")) + QLatin1Char('/') +
                m.value(QStringLiteral("VERSION_ID"), QStringLiteral("-"));
    const QString variant = m.value(QStringLiteral("VARIANT_ID"));
    if (!variant.isEmpty()) {
        s += QStringLiteral(",variant=") + variant;
    }
    const QString build = m.value(QStringLiteral("BUILD_ID"));
    if (!build.isEmpty()) {
        s += QStringLiteral(",build=") + build;
    }
    return s;
}

QJsonObject osReleaseJson(const QString& source, const QMap<QString, QString>& m)
{
    static const char* const kKeys[] = {
        "ID", "ID_LIKE", "VERSION_ID", "VERSION_CODENAME", "VARIANT_ID",
        "BUILD_ID", "IMAGE_ID", "IMAGE_VERSION", "PRETTY_NAME",
    };
    QJsonObject o;
    o[QStringLiteral("source")] = source;
    QJsonObject fields;
    for (const char* k : kKeys) {
        const QString key = QString::fromLatin1(k);
        if (m.contains(key)) {
            fields[key] = m.value(key);
        }
    }
    o[QStringLiteral("fields")] = fields;
    return o;
}

// ── 便宜資訊 ──

struct GamescopeInfo {
    bool detected = false;
    QString envDisplay;      // GAMESCOPE_WAYLAND_DISPLAY
    QString socketPath;      // $XDG_RUNTIME_DIR/gamescope-0
    bool socketExists = false;
};

struct CheapInfo {
    UpdateAssetRules::Platform platform;
    bool flatpak = false;
    IniFile flatpakInfo;
    QString osSource;                      // 本機（或 Flatpak 內 host）的 os-release 來源路徑
    QMap<QString, QString> os;
    QMap<QString, QString> runtimeOs;      // 只有 Flatpak：runtime 的 /etc/os-release
    GamescopeInfo gamescope;
    QStringList steamEnvNames;             // 只記名稱，不記值（可能含 token）
    QList<V4l2Caps::DeviceSummary> videoSysfs;
    QStringList drm;
    XrRuntimeJson::Resolved xr;
};

CheapInfo collectCheap()
{
    CheapInfo c;
    c.platform = UpdateAssetRules::detectPlatform();
    c.flatpak = c.platform.packaging == Packaging::Flatpak;

    if (c.flatpak) {
        c.flatpakInfo = readIni(QStringLiteral("/.flatpak-info"));
        // Flatpak 內 /etc/os-release 是 runtime 的；host 的在 /run/host/os-release
        c.osSource = QStringLiteral("/run/host/os-release");
        c.os = readOsRelease(c.osSource);
        c.runtimeOs = readOsRelease(QStringLiteral("/etc/os-release"));
    }
    else {
        c.osSource = QStringLiteral("/etc/os-release");
        c.os = readOsRelease(c.osSource);
        if (c.os.isEmpty()) {
            c.osSource = QStringLiteral("/usr/lib/os-release");
            c.os = readOsRelease(c.osSource);
        }
    }

    // gamescope：GAMESCOPE_WAYLAND_DISPLAY 有值，或 $XDG_RUNTIME_DIR/gamescope-0 存在
    // （Flatpak 內要有 --filesystem=xdg-run/gamescope-0 才看得到）。F14 閘門同款判斷。
    c.gamescope.envDisplay = qEnvironmentVariable("GAMESCOPE_WAYLAND_DISPLAY");
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDir.isEmpty()) {
        c.gamescope.socketPath = runtimeDir + QStringLiteral("/gamescope-0");
        c.gamescope.socketExists = QFileInfo::exists(c.gamescope.socketPath);
    }
    c.gamescope.detected = !c.gamescope.envDisplay.isEmpty() || c.gamescope.socketExists;

    // STEAM_*、SteamAppId、SteamGameId…：只收名稱
    const QStringList keys = QProcessEnvironment::systemEnvironment().keys();
    for (const QString& k : keys) {
        if (k.startsWith(QStringLiteral("STEAM"), Qt::CaseInsensitive)) {
            c.steamEnvNames.append(k);
        }
    }
    c.steamEnvNames.sort();

    c.videoSysfs = V4l2Caps::enumerateSysfsOnly();
    c.drm = WMUtils::getDrmDriverNames();   // 有快取；第一次會印 [VIPLE-LNXFE] DRM driver probe
    c.xr = XrRuntimeJson::resolveActive();  // 只找檔案、讀 JSON，不載入 loader
    return c;
}

// logOnce 與 summaryLine 共用：行程內只收集一次（magic static，執行緒安全）
const CheapInfo& cheapInfo()
{
    static const CheapInfo s_Info = collectCheap();
    return s_Info;
}

QString flatpakValue(const CheapInfo& c, const char* section, const char* key)
{
    return c.flatpakInfo.value(QString::fromLatin1(section)).value(QString::fromLatin1(key));
}

QString archForLog(const UpdateAssetRules::Platform& p)
{
    QString s = p.buildArch + QLatin1Char('/') + p.currentArch;
    if (p.emulated) {
        s += QStringLiteral("(emulated)");
    }
    return s;
}

QString gamescopeForLog(const GamescopeInfo& g)
{
    if (!g.detected) {
        return QStringLiteral("0");
    }
    QStringList how;
    if (!g.envDisplay.isEmpty()) {
        how << (QStringLiteral("env=") + g.envDisplay);
    }
    if (g.socketExists) {
        how << QStringLiteral("gamescope-0");
    }
    return QStringLiteral("1(") + how.join(QLatin1Char(',')) + QLatin1Char(')');
}

QString videoForLog(const QList<V4l2Caps::DeviceSummary>& devs)
{
    QStringList parts;
    for (const V4l2Caps::DeviceSummary& d : devs) {
        QString node = QFileInfo(d.path).fileName();
        if (node.isEmpty()) {
            node = d.path;
        }
        parts << (d.sysName.isEmpty() ? node : node + QLatin1Char(':') + d.sysName);
        if (parts.size() >= 16) {
            parts << QStringLiteral("...");
            break;
        }
    }
    return QLatin1Char('[') + parts.join(QLatin1Char(',')) + QLatin1Char(']');
}

QString xrForLog(const XrRuntimeJson::Resolved& r)
{
    if (!r.found) {
        return QStringLiteral("none");
    }
    return r.jsonPath + QLatin1Char('(') + r.source +
           (r.loaderWouldFind ? QStringLiteral(",loader=1)") : QStringLiteral(",loader=0)"));
}

// 第一行：打包形態、架構、作業系統、kernel、Flatpak 權限
QString logLine1(const CheapInfo& c)
{
    QString s = QStringLiteral("pkg=") + packagingName(c.platform.packaging);
    if (c.flatpak) {
        s += QStringLiteral(" id=") + flatpakValue(c, "Application", "name");
        s += QStringLiteral(" runtime=") + flatpakValue(c, "Application", "runtime");
        s += QStringLiteral(" flatpak=") + flatpakValue(c, "Instance", "flatpak-version");
        const QString commit = flatpakValue(c, "Instance", "app-commit");
        if (!commit.isEmpty()) {
            s += QStringLiteral(" commit=") + commit.left(12);
        }
    }
    s += QStringLiteral(" app=") + QCoreApplication::applicationVersion();
    s += QStringLiteral(" arch=") + archForLog(c.platform);
    if (c.flatpak) {
        s += QStringLiteral(" host-os=") + osBrief(c.os);
        s += QStringLiteral(" runtime-os=") + osBrief(c.runtimeOs);
    }
    else {
        s += QStringLiteral(" os=") + osBrief(c.os);
    }
    s += QStringLiteral(" kernel=") + QSysInfo::kernelVersion();
    if (c.flatpak) {
        s += QStringLiteral(" devices=") + clip(flatpakValue(c, "Context", "devices"), 80);
        s += QStringLiteral(" fs=") + clip(flatpakValue(c, "Context", "filesystems"));
    }
    return s;
}

// 第二行：顯示環境、裝置、OpenXR
QString logLine2(const CheapInfo& c)
{
    const OriginalEnv& orig = originalEnv();
    QString s = QStringLiteral("session=") + envForLog("XDG_SESSION_TYPE");
    s += QStringLiteral(" desktop=") + envForLog("XDG_CURRENT_DESKTOP");
    s += QStringLiteral(" wayland=") + envForLog("WAYLAND_DISPLAY");
    s += QStringLiteral(" display=") + envForLog("DISPLAY");
    s += QStringLiteral(" qpa=") + origAndNowForLog("QT_QPA_PLATFORM", orig.qpaSet, orig.qpa);
    s += QStringLiteral(" sdl=") + origAndNowForLog("SDL_VIDEODRIVER", orig.sdlSet, orig.sdl);
    s += QStringLiteral(" gamescope=") + gamescopeForLog(c.gamescope);
    s += QStringLiteral(" steam-env=") + QString::number(c.steamEnvNames.size());
    s += QStringLiteral(" video=") + videoForLog(c.videoSysfs);
    s += QStringLiteral(" drm=[") + c.drm.join(QLatin1Char(',')) + QLatin1Char(']');
    s += QStringLiteral(" xr-env=") + envForLog("XR_RUNTIME_JSON");
    s += QStringLiteral(" xr-json=") + xrForLog(c.xr);
    return s;
}

// summaryLine 的快取部分（session 開場用，比兩行版精簡）
QString buildSummaryStatic()
{
    const CheapInfo& c = cheapInfo();
    QString s = QStringLiteral("pkg=") + packagingName(c.platform.packaging);
    s += QStringLiteral(" arch=") + archForLog(c.platform);
    s += QStringLiteral(" os=") + osBrief(c.os);
    s += QStringLiteral(" kernel=") + QSysInfo::kernelVersion();
    s += QStringLiteral(" gamescope=") + (c.gamescope.detected ? QStringLiteral("1") : QStringLiteral("0"));
    s += QStringLiteral(" video=") + QString::number(c.videoSysfs.size());
    s += QStringLiteral(" drm=[") + c.drm.join(QLatin1Char(',')) + QLatin1Char(']');
    s += QStringLiteral(" xr=") + (c.xr.found ? c.xr.source : QStringLiteral("none"));
    return s;
}

// ── snapshot 專用（貴的部分） ──

QJsonObject flatpakJson(const CheapInfo& c)
{
    QJsonObject o;
    o[QStringLiteral("present")] = c.flatpak;
    if (!c.flatpak) {
        return o;
    }
    // [Instance] 只取白名單：instance-path、app-path 之類含家目錄或系統路徑，沒有判讀價值
    static const char* const kInstanceKeys[] = {
        "arch", "branch", "flatpak-version", "app-commit", "runtime-commit",
        "app-extensions", "runtime-extensions", "devel",
    };
    const IniSection app = c.flatpakInfo.value(QStringLiteral("Application"));
    const IniSection inst = c.flatpakInfo.value(QStringLiteral("Instance"));
    const IniSection ctx = c.flatpakInfo.value(QStringLiteral("Context"));

    QJsonObject appJson;
    appJson[QStringLiteral("name")] = app.value(QStringLiteral("name"));
    appJson[QStringLiteral("runtime")] = app.value(QStringLiteral("runtime"));
    o[QStringLiteral("application")] = appJson;

    QJsonObject instJson;
    for (const char* k : kInstanceKeys) {
        const QString key = QString::fromLatin1(k);
        if (inst.contains(key)) {
            instJson[key] = inst.value(key);
        }
    }
    o[QStringLiteral("instance")] = instJson;

    // [Context] 全列：核對 finish-args（--device=all、xdg-run/gamescope-0、xdg-config/openxr:ro…）有沒有生效
    QJsonObject ctxJson;
    for (auto it = ctx.constBegin(); it != ctx.constEnd(); ++it) {
        ctxJson[it.key()] = it.value();
    }
    o[QStringLiteral("context")] = ctxJson;
    o[QStringLiteral("flatpakId")] = envForJson("FLATPAK_ID");
    return o;
}

QJsonObject envJson()
{
    static const char* const kPlain[] = {
        "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "WAYLAND_DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY",
        "DISPLAY", "XR_RUNTIME_JSON",
    };
    const OriginalEnv& orig = originalEnv();
    QJsonObject o;
    for (const char* k : kPlain) {
        o[QString::fromLatin1(k)] = envForJson(k);
    }
    o[QStringLiteral("QT_QPA_PLATFORM")] = origAndNowForJson("QT_QPA_PLATFORM", orig.qpaSet, orig.qpa);
    o[QStringLiteral("SDL_VIDEODRIVER")] = origAndNowForJson("SDL_VIDEODRIVER", orig.sdlSet, orig.sdl);
    return o;
}

QJsonObject gamescopeJson(const GamescopeInfo& g)
{
    QJsonObject o;
    o[QStringLiteral("detected")] = g.detected;
    o[QStringLiteral("envDisplay")] = g.envDisplay;
    o[QStringLiteral("socketPath")] = g.socketPath;
    o[QStringLiteral("socketExists")] = g.socketExists;
    return o;
}

QString hex32(uint32_t v)
{
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(8, QLatin1Char('0'));
}

QJsonObject v4l2Json()
{
    QJsonObject o;
    o[QStringLiteral("supported")] = V4l2Caps::isSupported();
    QJsonArray devs;
    const QList<V4l2Caps::DeviceSummary> list = V4l2Caps::enumerate();
    for (const V4l2Caps::DeviceSummary& d : list) {
        QJsonObject j;
        j[QStringLiteral("path")] = d.path;
        j[QStringLiteral("sysName")] = d.sysName;
        j[QStringLiteral("openErrno")] = d.openErrno;
        if (d.openErrno == 0) {
            j[QStringLiteral("driver")] = d.driver;
            j[QStringLiteral("card")] = d.card;
            j[QStringLiteral("busInfo")] = d.busInfo;
            j[QStringLiteral("deviceCaps")] = hex32(d.deviceCaps);
            j[QStringLiteral("m2m")] = d.m2m;
            j[QStringLiteral("m2mMplane")] = d.m2mMplane;
        }
        devs.append(j);
    }
    o[QStringLiteral("devices")] = devs;
    return o;
}

QJsonObject xrJson(const XrRuntimeJson::Resolved& r)
{
    QJsonObject o;
    o[QStringLiteral("found")] = r.found;
    o[QStringLiteral("jsonPath")] = r.jsonPath;
    o[QStringLiteral("source")] = r.source;
    o[QStringLiteral("runtimeName")] = r.runtimeName;
    o[QStringLiteral("libraryPath")] = r.libraryPath;
    o[QStringLiteral("libraryExists")] = r.libraryExists;
    o[QStringLiteral("loaderWouldFind")] = r.loaderWouldFind;
    if (!r.parseError.isEmpty()) {
        o[QStringLiteral("parseError")] = r.parseError;
    }
    return o;
}

#if defined(VIPLE_SFENV_HAVE_VULKAN)

QString vkVersionString(uint32_t v)
{
    // 不依賴 VK_API_VERSION_MAJOR 等巨集（舊 header 沒有）：variant 3 bit、major 7、minor 10、patch 12
    return QStringLiteral("%1.%2.%3").arg((v >> 22) & 0x7Fu).arg((v >> 12) & 0x3FFu).arg(v & 0xFFFu);
}

QString vkDeviceTypeName(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return QStringLiteral("integrated");
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return QStringLiteral("discrete");
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return QStringLiteral("virtual");
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return QStringLiteral("cpu");
    default:
        return QStringLiteral("other");
    }
}

// Vulkan 裝置清單的合併（去重）。
//
// 同一個 physical device 可能被 loader 列出不只一次：builder 上的 x86_64 Flatpak 實測出現兩筆
// 一模一樣的 "AMD Radeon Vega 10 Graphics (RADV RAVEN)"（推測是沙箱內兩份 ICD manifest 指到同一個
// driver）。不合併的話，讀 JSON 或 xr-probe 輸出的人會以為有兩張 GPU。合併鍵：
//   - 裝置 ≥ 1.1、拿得到 VkPhysicalDeviceIDProperties，而且 deviceUUID 不是全 0：
//     deviceUUID＋driverUUID。同一張卡上的不同 driver（例如 RADV 與 AMDVLK）driverUUID 不同，
//     仍分開列；同一個 driver 的不同 Mesa 版本 driverUUID 相同，會合併。
//   - 否則退回 vendorID／deviceID／driverName／name。這條路分不出兩張同型號、同 driver 的卡
//     （會合併成 instances=2）；只有 1.0 的 loader 或裝置才會走到。
// 每筆記 "instances"（合併了幾次列舉），頂層記 "enumerated"（loader 原始筆數）與
// "duplicatesMerged"。被合併的那幾筆若有任何欄位和第一筆不同（例如 driverInfo 的 Mesa 版本），
// 整筆放進 "differingInstances"，不丟資訊。
// UUID 本身不寫進 JSON：部分 driver（例如 NVIDIA）的 deviceUUID 是每張卡唯一的硬體識別碼，
// 而這份 JSON 會被節錄進文件。
struct VkDeviceEntry {
    QString key;
    QJsonObject props;
    int instances;
    QJsonArray differing;
};

// 照 ffmpeg.cpp hasVulkanVideoDecodeQueue() 的作法：執行期 dlopen libvulkan.so.1（不連 -lvulkan），
// 建一個最小 instance 列 physical device；api ≥ 1.1 的裝置串 VkPhysicalDeviceIDProperties 取合併鍵，
// api ≥ 1.2 的裝置另外串 VkPhysicalDeviceDriverProperties 取 driverName／driverInfo（Turnip、RADV 的
// 版本字串在這裡）。
QJsonObject vulkanJson()
{
    QJsonObject o;
    o[QStringLiteral("loader")] = QStringLiteral("libvulkan.so.1");

    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        const char* e = dlerror();
        o[QStringLiteral("available")] = false;
        o[QStringLiteral("error")] = e != nullptr ? QString::fromLocal8Bit(e) : QStringLiteral("dlopen failed");
        return o;
    }
    // 不 dlclose：之後 renderer／OpenXR runtime 也會用到，而且部分 ICD 不喜歡被卸載

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (gipa == nullptr) {
        o[QStringLiteral("available")] = false;
        o[QStringLiteral("error")] = QStringLiteral("dlsym(vkGetInstanceProcAddr) failed");
        return o;
    }

    uint32_t instanceVersion = VK_API_VERSION_1_0;
    auto enumVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumVersion != nullptr && enumVersion(&instanceVersion) != VK_SUCCESS) {
        instanceVersion = VK_API_VERSION_1_0;
    }
    o[QStringLiteral("instanceVersion")] = vkVersionString(instanceVersion);

    // 1.0 loader 收到更高的 apiVersion 會回 VK_ERROR_INCOMPATIBLE_DRIVER，所以依 loader 能力選
    uint32_t apiVersion = VK_API_VERSION_1_0;
    if (instanceVersion >= VK_API_VERSION_1_2) {
        apiVersion = VK_API_VERSION_1_2;
    }
    else if (instanceVersion >= VK_API_VERSION_1_1) {
        apiVersion = VK_API_VERSION_1_1;
    }

    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        o[QStringLiteral("available")] = false;
        o[QStringLiteral("error")] = QStringLiteral("vkCreateInstance not found");
        return o;
    }

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "VipleStream sf-env";
    ai.pEngineName = "VipleStream";
    ai.apiVersion = apiVersion;
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;

    VkInstance inst = VK_NULL_HANDLE;
    const VkResult vr = createInstance(&ici, nullptr, &inst);
    if (vr != VK_SUCCESS || inst == VK_NULL_HANDLE) {
        o[QStringLiteral("available")] = false;
        o[QStringLiteral("error")] = QStringLiteral("vkCreateInstance failed: VkResult %1").arg(static_cast<int>(vr));
        return o;
    }
    o[QStringLiteral("available")] = true;

    auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    PFN_vkGetPhysicalDeviceProperties2 getProps2 = nullptr;
    if (apiVersion >= VK_API_VERSION_1_1) {
        getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(inst, "vkGetPhysicalDeviceProperties2"));
    }

    std::vector<VkDeviceEntry> entries;   // 依第一次出現的順序；合併規則見 VkDeviceEntry 上方的註解
    uint32_t enumerated = 0;
    if (enumDevices != nullptr && getProps != nullptr) {
        uint32_t count = 0;
        if (enumDevices(inst, &count, nullptr) == VK_SUCCESS && count > 0) {
            std::vector<VkPhysicalDevice> pds(count);
            if (enumDevices(inst, &count, pds.data()) >= VK_SUCCESS) {   // VK_INCOMPLETE 也收
                pds.resize(count);
                enumerated = count;
                for (VkPhysicalDevice pd : pds) {
                    VkPhysicalDeviceProperties p = {};
                    getProps(pd, &p);
                    QJsonObject d;
                    d[QStringLiteral("name")] = QString::fromUtf8(p.deviceName);
                    d[QStringLiteral("type")] = vkDeviceTypeName(p.deviceType);
                    d[QStringLiteral("vendorId")] = hex32(p.vendorID);
                    d[QStringLiteral("deviceId")] = hex32(p.deviceID);
                    d[QStringLiteral("apiVersion")] = vkVersionString(p.apiVersion);
                    d[QStringLiteral("driverVersionRaw")] = hex32(p.driverVersion);

                    // 1.1 結構（ID properties）要裝置 ≥ 1.1（getProps2 非空已代表 instance ≥ 1.1）；
                    // 1.2 結構只能在「instance 與裝置的有效版本」都 ≥ 1.2 時串
                    QString uuidKey;
                    if (getProps2 != nullptr && p.apiVersion >= VK_API_VERSION_1_1) {
                        const bool wantDriver = apiVersion >= VK_API_VERSION_1_2 && p.apiVersion >= VK_API_VERSION_1_2;
                        VkPhysicalDeviceDriverProperties drv = {};
                        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
                        VkPhysicalDeviceIDProperties idp = {};
                        idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
                        idp.pNext = wantDriver ? &drv : nullptr;
                        VkPhysicalDeviceProperties2 p2 = {};
                        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                        p2.pNext = &idp;
                        getProps2(pd, &p2);
                        if (wantDriver) {
                            d[QStringLiteral("driverId")] = static_cast<int>(drv.driverID);
                            d[QStringLiteral("driverName")] = QString::fromUtf8(drv.driverName);
                            d[QStringLiteral("driverInfo")] = QString::fromUtf8(drv.driverInfo);
                        }
                        // 全 0 的 deviceUUID 代表 driver 沒填，不能拿來分辨裝置 → 退回屬性鍵
                        bool uuidValid = false;
                        for (uint8_t b : idp.deviceUUID) {
                            if (b != 0) {
                                uuidValid = true;
                                break;
                            }
                        }
                        if (uuidValid) {
                            uuidKey = QString::fromLatin1(
                                          QByteArray(reinterpret_cast<const char*>(idp.deviceUUID),
                                                     static_cast<int>(VK_UUID_SIZE)).toHex()) +
                                      QLatin1Char('/') +
                                      QString::fromLatin1(
                                          QByteArray(reinterpret_cast<const char*>(idp.driverUUID),
                                                     static_cast<int>(VK_UUID_SIZE)).toHex());
                        }
                    }

                    QString key;
                    if (!uuidKey.isEmpty()) {
                        key = QStringLiteral("uuid:") + uuidKey;
                        d[QStringLiteral("dedupBy")] = QStringLiteral("uuid");
                    }
                    else {
                        // 換行當分隔字元：裝置名稱與 driver 名稱裡不會出現
                        key = QStringLiteral("props:") + hex32(p.vendorID) + QLatin1Char('\n') + hex32(p.deviceID) +
                              QLatin1Char('\n') + d.value(QStringLiteral("driverName")).toString() +
                              QLatin1Char('\n') + d.value(QStringLiteral("name")).toString();
                        d[QStringLiteral("dedupBy")] = QStringLiteral("properties");
                    }

                    VkDeviceEntry* same = nullptr;
                    for (VkDeviceEntry& e : entries) {
                        if (e.key == key) {
                            same = &e;
                            break;
                        }
                    }
                    if (same == nullptr) {
                        entries.push_back(VkDeviceEntry{key, d, 1, QJsonArray()});
                    }
                    else {
                        same->instances++;
                        if (d != same->props) {
                            same->differing.append(d);
                        }
                    }
                }
            }
        }
    }

    QJsonArray devices;
    for (const VkDeviceEntry& e : entries) {
        QJsonObject d = e.props;
        d[QStringLiteral("instances")] = e.instances;
        if (!e.differing.isEmpty()) {
            d[QStringLiteral("differingInstances")] = e.differing;
        }
        devices.append(d);
    }
    o[QStringLiteral("devices")] = devices;
    o[QStringLiteral("enumerated")] = static_cast<int>(enumerated);
    o[QStringLiteral("duplicatesMerged")] = static_cast<int>(enumerated) - static_cast<int>(entries.size());

    if (destroyInstance != nullptr) {
        destroyInstance(inst, nullptr);
    }
    return o;
}

#endif // VIPLE_SFENV_HAVE_VULKAN

#endif // Q_OS_LINUX

} // namespace

namespace SfEnv {

bool isSteamFrame()
{
#if defined(Q_OS_LINUX)
    const CheapInfo& c = cheapInfo();
    return c.os.value(QStringLiteral("ID")) == QStringLiteral("steamos") &&
           c.os.value(QStringLiteral("VARIANT_ID")) == QStringLiteral("vr");
#else
    return false;
#endif
}

void captureOriginalEnv()
{
#if defined(Q_OS_LINUX)
    OriginalEnv& e = originalEnv();
    if (e.captured) {
        return;
    }
    e.qpaSet = qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    e.qpa = qgetenv("QT_QPA_PLATFORM");
    e.sdlSet = qEnvironmentVariableIsSet("SDL_VIDEODRIVER");
    e.sdl = qgetenv("SDL_VIDEODRIVER");
    e.captured = true;
#endif
}

void logOnce()
{
#if defined(Q_OS_LINUX)
    static std::atomic<bool> s_Logged{false};
    if (s_Logged.exchange(true)) {
        return;
    }
    const CheapInfo& c = cheapInfo();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SF-ENV] %s", logLine1(c).toUtf8().constData());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-SF-ENV] %s", logLine2(c).toUtf8().constData());
#endif
}

QString summaryLine()
{
#if defined(Q_OS_LINUX)
    static const QString s_Static = buildSummaryStatic();
    // SDL 的部分每次現取（R3）：session 開場時 SDL video 已由 main.cpp 初始化，
    // 看得到實際採用的 driver 與 SDL3 的 app id（sdl2-compat 下 WMCLASS env 不一定生效）。
    const char* driver = SDL_GetCurrentVideoDriver();
    const char* appId = SDL_GetHint("SDL_APP_ID");
    return s_Static +
           QStringLiteral(" sdl-driver=") + QString::fromUtf8(driver != nullptr ? driver : "none") +
           QStringLiteral(" sdl-app-id=") + QString::fromUtf8(appId != nullptr ? appId : "unset");
#else
    return QString();
#endif
}

QJsonObject snapshot()
{
    QJsonObject o;
    o[QStringLiteral("schema")] = 1;
    o[QStringLiteral("appVersion")] = QCoreApplication::applicationVersion();
    const UpdateAssetRules::Platform platform = UpdateAssetRules::detectPlatform();
    o[QStringLiteral("platform")] = platformJson(platform);

#if defined(Q_OS_LINUX)
    // 重新收集（不用 logOnce 的快取）：探測動作可能在啟動後才改了環境，這裡要看當下
    const CheapInfo c = collectCheap();
    o[QStringLiteral("flatpak")] = flatpakJson(c);
    o[QStringLiteral("osRelease")] = osReleaseJson(c.osSource, c.os);
    if (c.flatpak) {
        o[QStringLiteral("runtimeOsRelease")] = osReleaseJson(QStringLiteral("/etc/os-release"), c.runtimeOs);
    }
    o[QStringLiteral("env")] = envJson();
    o[QStringLiteral("steamEnvNames")] = QJsonArray::fromStringList(c.steamEnvNames);
    o[QStringLiteral("gamescope")] = gamescopeJson(c.gamescope);
    o[QStringLiteral("drm")] = QJsonArray::fromStringList(c.drm);
    o[QStringLiteral("v4l2")] = v4l2Json();
#if defined(VIPLE_SFENV_HAVE_VULKAN)
    o[QStringLiteral("vulkan")] = vulkanJson();
#else
    QJsonObject vk;
    vk[QStringLiteral("available")] = false;
    vk[QStringLiteral("error")] = QStringLiteral("built without Vulkan 1.2 headers");
    o[QStringLiteral("vulkan")] = vk;
#endif
    o[QStringLiteral("openxr")] = xrJson(c.xr);
    o[QStringLiteral("summary")] = logLine1(c) + QLatin1Char(' ') + logLine2(c);
#endif
    return o;
}

} // namespace SfEnv
