// VipleStream §SF-PROBE（M2a）— `viplestream xr-probe`（CLI 包裝）。設計背景見 xrprobe.h。
//
// 一律編譯。流程：
//   1. beginProbe → 取 SfEnv::snapshot()（在改任何 XR_* 環境變數之前，記的是使用者原本的環境；
//      放在 JSON 的 "env"，三個 probe 同一個鍵名）
//   2. dev 覆寫：--xr-runtime-json／--loader-debug 在行程內設 XR_RUNTIME_JSON／XR_LOADER_DEBUG
//      （只影響這個行程；不是使用者設定，不變式 7）
//   3. 列出 runtime JSON 的探索結果（XrRuntimeJson::candidates／resolveActive）
//   4. 沒有 HAVE_OPENXR → rc 10；--session → rc 12；否則：
//      - 只有 host-config 找得到（Flatpak 的 XDG_CONFIG_HOME 看不到 host 的 ~/.config）而且
//        XR_RUNTIME_JSON 沒設 → 行程內設 XR_RUNTIME_JSON，記進 JSON
//      - xrProbeInstance()（streaming/xr/xrprobe_instance.cpp）
//      - rc 13（runtime 載不起來）→ 自己 dlopen runtime JSON 的 library_path，記 dlerror()
//   5. JSON 一律寫檔（沒有 OpenXR 的建置也寫：AppImage 上的環境與 runtime JSON 診斷一樣有用）

#include "xrprobe.h"

#include "probeutil.h"
#include "backend/sfenv.h"
#include "streaming/xr/xrruntimejson.h"

#ifdef HAVE_OPENXR
#include "streaming/xr/xrprobe_instance.h"
#endif

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include <dlfcn.h>
#endif

namespace {

const char* const kTag = "VIPLE-XR-PROBE";

QJsonObject candidateJson(const XrRuntimeJson::Candidate& c)
{
    QJsonObject o;
    o[QStringLiteral("path")] = c.path;
    o[QStringLiteral("source")] = c.source;
    o[QStringLiteral("exists")] = c.exists;
    o[QStringLiteral("readable")] = c.readable;
    // SteamVR 的 active_runtime.json 通常是 symlink；在沙箱內指到沒開放的目錄時就是懸空的，
    // exists=false 但 symlink 本身在——這正是 PoC-F 要分辨的情況。
    const QFileInfo fi(c.path);
    if (fi.isSymLink()) {
        o[QStringLiteral("symlinkTarget")] = fi.symLinkTarget();
        o[QStringLiteral("symlinkDangling")] = !c.exists;
    }
    return o;
}

QJsonObject resolvedJson(const XrRuntimeJson::Resolved& r)
{
    QJsonObject o;
    o[QStringLiteral("found")] = r.found;
    o[QStringLiteral("jsonPath")] = r.jsonPath;
    o[QStringLiteral("source")] = r.source;
    o[QStringLiteral("runtimeName")] = r.runtimeName;
    o[QStringLiteral("libraryPath")] = r.libraryPath;
    o[QStringLiteral("libraryExists")] = r.libraryExists;
    o[QStringLiteral("libraryIsBareName")] =
        !r.libraryPath.isEmpty() && !r.libraryPath.contains(QLatin1Char('/')) && !r.libraryPath.contains(QLatin1Char('\\'));
    o[QStringLiteral("loaderWouldFind")] = r.loaderWouldFind;
    if (!r.parseError.isEmpty()) {
        o[QStringLiteral("parseError")] = r.parseError;
    }
    return o;
}

// 列出候選與解析結果（stdout＋log），同時回傳 JSON。存在的候選與懸空 symlink 逐行列，
// 其餘只算數量（完整清單在 JSON）。
QJsonObject describeRuntimeJson(XrRuntimeJson::Resolved* resolvedOut)
{
    const QList<XrRuntimeJson::Candidate> list = XrRuntimeJson::candidates();
    const XrRuntimeJson::Resolved res = XrRuntimeJson::resolveActive();

    QJsonObject o;
    if (list.isEmpty()) {
        // 非 Linux：XrRuntimeJson 只實作 Linux 的探索（Windows 的 loader 讀登錄檔，M3a 再做）
        ProbeUtil::printLine(kTag, "runtime-json: discovery is Linux-only in this build");
        o[QStringLiteral("candidates")] = QJsonArray();
        o[QStringLiteral("resolved")] = resolvedJson(res);
        if (resolvedOut != nullptr) {
            *resolvedOut = res;
        }
        return o;
    }

    QJsonArray arr;
    int present = 0;
    for (const XrRuntimeJson::Candidate& c : list) {
        arr.append(candidateJson(c));
        const QFileInfo fi(c.path);
        if (c.exists || fi.isSymLink()) {
            present++;
            if (fi.isSymLink()) {
                ProbeUtil::printLine(kTag, "runtime-json candidate: %s source=%s exists=%d readable=%d symlink->%s%s",
                                     qUtf8Printable(c.path), qUtf8Printable(c.source), c.exists ? 1 : 0,
                                     c.readable ? 1 : 0, qUtf8Printable(fi.symLinkTarget()),
                                     c.exists ? "" : " (dangling)");
            }
            else {
                ProbeUtil::printLine(kTag, "runtime-json candidate: %s source=%s exists=1 readable=%d",
                                     qUtf8Printable(c.path), qUtf8Printable(c.source), c.readable ? 1 : 0);
            }
        }
    }
    ProbeUtil::printLine(kTag, "runtime-json: %d candidate path(s) checked, %d present",
                         static_cast<int>(list.size()), present);

    if (res.found) {
        ProbeUtil::printLine(kTag, "runtime-json: active=%s source=%s name=\"%s\" library=%s libExists=%d loaderWouldFind=%d%s%s",
                             qUtf8Printable(res.jsonPath), qUtf8Printable(res.source),
                             qUtf8Printable(res.runtimeName), qUtf8Printable(res.libraryPath),
                             res.libraryExists ? 1 : 0, res.loaderWouldFind ? 1 : 0,
                             res.parseError.isEmpty() ? "" : " parseError=",
                             qUtf8Printable(res.parseError));
    }
    else if (!res.jsonPath.isEmpty()) {
        // XR_RUNTIME_JSON 指定的檔案有問題
        ProbeUtil::printLine(kTag, "runtime-json: %s (%s): %s",
                             qUtf8Printable(res.jsonPath), qUtf8Printable(res.source),
                             qUtf8Printable(res.parseError));
    }
    else {
        ProbeUtil::printLine(kTag, "runtime-json: none found (no active_runtime json in any searched path)");
    }

    o[QStringLiteral("candidates")] = arr;
    o[QStringLiteral("resolved")] = resolvedJson(res);
    if (resolvedOut != nullptr) {
        *resolvedOut = res;
    }
    return o;
}

// 環境快照的重點印到 stdout（log 已由 main.cpp 的 SfEnv::logOnce 寫過；經 SSH 跑時要直接看得到）：
// 打包形態／OS 摘要，以及 Vulkan 裝置（runtime 指定的 GPU 要跟這份清單對照）。
void printEnvHighlights(const QJsonObject& snap)
{
    const QString summary = snap.value(QStringLiteral("summary")).toString();
    if (!summary.isEmpty()) {
        ProbeUtil::printLine(kTag, "env: %s", qUtf8Printable(summary));
    }
    const QJsonObject vk = snap.value(QStringLiteral("vulkan")).toObject();
    if (vk.isEmpty()) {
        return;
    }
    if (!vk.value(QStringLiteral("available")).toBool()) {
        ProbeUtil::printLine(kTag, "env: vulkan unavailable (%s)",
                             qUtf8Printable(vk.value(QStringLiteral("error")).toString()));
        return;
    }
    const QJsonArray devices = vk.value(QStringLiteral("devices")).toArray();
    for (int i = 0; i < devices.size(); i++) {
        const QJsonObject d = devices.at(i).toObject();
        ProbeUtil::printLine(kTag, "env: vulkan[%d] \"%s\" type=%s driver=%s %s api=%s",
                             i, qUtf8Printable(d.value(QStringLiteral("name")).toString()),
                             qUtf8Printable(d.value(QStringLiteral("type")).toString()),
                             qUtf8Printable(d.value(QStringLiteral("driverName")).toString()),
                             qUtf8Printable(d.value(QStringLiteral("driverInfo")).toString()),
                             qUtf8Printable(d.value(QStringLiteral("apiVersion")).toString()));
    }
}

#if defined(HAVE_OPENXR) && defined(Q_OS_LINUX)

// ELF 檔頭的 e_machine：arm64 的 Frame 上若 runtime 是 x86_64 的 .so（靠 FEX 跑的 SteamVR），
// dlopen 只會給一句含糊的錯誤，這裡直接說清楚。
QString elfMachine(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QByteArray h = f.read(20);
    if (h.size() < 20 || !h.startsWith("\x7f" "ELF")) {
        return QStringLiteral("not-elf");
    }
    const bool bigEndian = static_cast<unsigned char>(h.at(5)) == 2;
    const unsigned b0 = static_cast<unsigned char>(h.at(18));
    const unsigned b1 = static_cast<unsigned char>(h.at(19));
    const unsigned machine = bigEndian ? ((b0 << 8) | b1) : (b0 | (b1 << 8));
    const QString cls = static_cast<unsigned char>(h.at(4)) == 1 ? QStringLiteral("ELF32") : QStringLiteral("ELF64");
    QString name;
    switch (machine) {
    case 3:   name = QStringLiteral("i386"); break;
    case 40:  name = QStringLiteral("arm"); break;
    case 62:  name = QStringLiteral("x86_64"); break;
    case 183: name = QStringLiteral("aarch64"); break;
    case 243: name = QStringLiteral("riscv"); break;
    default:  name = QStringLiteral("e_machine=%1").arg(machine); break;
    }
    return name + QLatin1Char('/') + cls;
}

// xrCreateInstance 失敗後的補充診斷：照 loader 的方式 dlopen runtime 的 .so，取 dlerror()。
// 成功時不 dlclose：runtime 的解構子在這裡跑不安全，行程馬上 _Exit。
QJsonObject dlopenRuntime(const XrRuntimeJson::Resolved& res)
{
    QJsonObject o;
    o[QStringLiteral("library")] = res.libraryPath;
    if (res.libraryPath.isEmpty()) {
        o[QStringLiteral("attempted")] = false;
        return o;
    }
    const bool bareName = !res.libraryPath.contains(QLatin1Char('/'));
    if (!bareName) {
        o[QStringLiteral("exists")] = QFileInfo::exists(res.libraryPath);
        const QString machine = elfMachine(res.libraryPath);
        if (!machine.isEmpty()) {
            o[QStringLiteral("elfMachine")] = machine;
        }
    }
    o[QStringLiteral("attempted")] = true;

    dlerror();   // 清掉殘留的錯誤
    void* handle = dlopen(QFile::encodeName(res.libraryPath).constData(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* err = dlerror();
        const QString errStr = err != nullptr ? QString::fromLocal8Bit(err) : QStringLiteral("(no dlerror)");
        o[QStringLiteral("dlopen")] = false;
        o[QStringLiteral("dlerror")] = errStr;
        ProbeUtil::printLine(kTag, "runtime-lib: dlopen(%s) failed: %s%s%s",
                             qUtf8Printable(res.libraryPath), qUtf8Printable(errStr),
                             o.contains(QStringLiteral("elfMachine")) ? " elf=" : "",
                             qUtf8Printable(o.value(QStringLiteral("elfMachine")).toString()));
    }
    else {
        const bool negotiate = dlsym(handle, "xrNegotiateLoaderRuntimeInterface") != nullptr;
        o[QStringLiteral("dlopen")] = true;
        o[QStringLiteral("hasNegotiateEntry")] = negotiate;
        ProbeUtil::printLine(kTag, "runtime-lib: dlopen(%s) ok, xrNegotiateLoaderRuntimeInterface=%d "
                                   "(library loads; the failure is inside the runtime or its IPC)",
                             qUtf8Printable(res.libraryPath), negotiate ? 1 : 0);
    }
    return o;
}

#endif // HAVE_OPENXR && Q_OS_LINUX

} // namespace

int runXrProbe(const XrProbeOptions& options)
{
    ProbeUtil::beginProbe("xr-probe");

    const QString jsonPath = options.jsonPath.isEmpty()
                                 ? ProbeUtil::defaultJsonPath(QStringLiteral("xr-probe"))
                                 : options.jsonPath;

    QJsonObject root;
    root[QStringLiteral("probe")] = QStringLiteral("xr-probe");
    root[QStringLiteral("schema")] = 1;
    {
        QJsonObject opts;
        opts[QStringLiteral("session")] = options.session;
        opts[QStringLiteral("runtimeJson")] = options.runtimeJson;
        opts[QStringLiteral("loaderDebug")] = options.loaderDebug;
        root[QStringLiteral("options")] = opts;
    }
#ifdef HAVE_OPENXR
    root[QStringLiteral("openxrBuilt")] = true;
#else
    root[QStringLiteral("openxrBuilt")] = false;
#endif

    // 環境快照先取：下面會在行程內改 XR_* 環境變數。鍵名與 v4l2-probe、decode-bench 一致（"env"）。
    const QJsonObject snap = SfEnv::snapshot();
    root[QStringLiteral("env")] = snap;
    printEnvHighlights(snap);

    // ── dev 覆寫（行程內） ──
    QJsonObject envApplied;
    if (!options.runtimeJson.isEmpty()) {
        // 轉成絕對路徑：loader 對相對路徑是以 cwd 解析，記錄時容易看錯
        const QString abs = QFileInfo(options.runtimeJson).absoluteFilePath();
        qputenv("XR_RUNTIME_JSON", QFile::encodeName(abs));
        envApplied[QStringLiteral("XR_RUNTIME_JSON")] = abs;
        envApplied[QStringLiteral("XR_RUNTIME_JSON_reason")] = QStringLiteral("--xr-runtime-json");
        ProbeUtil::printLine(kTag, "override: XR_RUNTIME_JSON=%s (--xr-runtime-json, in-process only)",
                             qUtf8Printable(abs));
    }
    if (options.loaderDebug) {
        qputenv("XR_LOADER_DEBUG", "all");
        envApplied[QStringLiteral("XR_LOADER_DEBUG")] = QStringLiteral("all");
        ProbeUtil::printLine(kTag, "override: XR_LOADER_DEBUG=all (--loader-debug; loader output goes to stderr)");
    }

    int rc = ProbeUtil::kExitOk;

#ifndef HAVE_OPENXR
    ProbeUtil::printLine(kTag, "xr-probe: this build has no OpenXR (CONFIG+=openxr)");
    // 沒有 loader 也照樣列 runtime JSON 的探索結果（AppImage、Windows 上都有參考價值）
    root[QStringLiteral("runtimeJson")] = describeRuntimeJson(nullptr);
    rc = ProbeUtil::kExitNotBuilt;
#else
    if (options.session) {
        ProbeUtil::printLine(kTag, "xr-probe: --session (B stage: session, refresh rate, FOV, rect test) "
                                   "requires M3a (XrContext); run without --session for the A stage");
    }

    XrRuntimeJson::Resolved res;
    QJsonObject runtimeJson = describeRuntimeJson(&res);

    if (options.session) {
        rc = ProbeUtil::kExitNeedsLaterMilestone;
    }
    else {
        // Flatpak：loader 只看沙箱的 XDG_CONFIG_HOME（~/.var/app/<id>/config），看不到 host 的
        // ~/.config/openxr。只有 host-config 找得到（或更前面有 loader 會選中卻讀不到的檔案）時，
        // 在行程內指給 loader。使用者自己設了 XR_RUNTIME_JSON 就尊重它。
        if (res.found && !res.loaderWouldFind && qEnvironmentVariableIsEmpty("XR_RUNTIME_JSON")) {
            qputenv("XR_RUNTIME_JSON", QFile::encodeName(res.jsonPath));
            const QString reason = res.source == QLatin1String("host-config")
                                       ? QStringLiteral("host-config (sandbox XDG_CONFIG_HOME hides host ~/.config/openxr)")
                                       : QStringLiteral("an earlier candidate the loader would pick is unreadable");
            envApplied[QStringLiteral("XR_RUNTIME_JSON")] = res.jsonPath;
            envApplied[QStringLiteral("XR_RUNTIME_JSON_reason")] = reason;
            ProbeUtil::printLine(kTag, "runtime-json: set XR_RUNTIME_JSON=%s in-process (%s)",
                                 qUtf8Printable(res.jsonPath), qUtf8Printable(reason));
        }

        QJsonObject openxr;
        rc = xrProbeInstance(options, openxr);
        root[QStringLiteral("openxr")] = openxr;

        if (rc == ProbeUtil::kExitCapabilityAbsent) {
#if defined(Q_OS_LINUX)
            if (res.found && !res.libraryPath.isEmpty()) {
                runtimeJson[QStringLiteral("dlopen")] = dlopenRuntime(res);
            }
#endif
            ProbeUtil::printLine(kTag, "result: no usable OpenXR runtime (rc=13; runtime JSON diagnostics in the JSON report%s)",
                                 options.loaderDebug ? ", loader debug on stderr" : "; rerun with --loader-debug for loader detail");
        }
    }
    root[QStringLiteral("runtimeJson")] = runtimeJson;
#endif

    if (!envApplied.isEmpty()) {
        root[QStringLiteral("envApplied")] = envApplied;
    }
    root[QStringLiteral("rc")] = rc;

    QString writtenPath = jsonPath;
    if (!ProbeUtil::writeJson(jsonPath, root)) {
        writtenPath.clear();
        // JSON 寫不出來只蓋掉 rc=0：其他結束碼本身就是結論（stdout 也已經印了細節）
        if (rc == ProbeUtil::kExitOk) {
            rc = ProbeUtil::kExitJsonWriteFailed;
        }
    }
    return ProbeUtil::finish(rc, writtenPath);
}
