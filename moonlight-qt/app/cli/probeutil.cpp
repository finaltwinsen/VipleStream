// VipleStream §SF-PROBE（M2a）— 探測動作（xr-probe、v4l2-probe、decode-bench）共用小工具。
// 介面與輸出分層見 probeutil.h。
//
// 這裡只能依賴 QtCore 與 SDL_Log：三個探測動作在 main.cpp 早期（log 就緒後、QGuiApplication
// 之前）以 QCoreApplication 派發，SDL 也沒有初始化（SDL_Log 不需要）。

#include "probeutil.h"

#include "path.h"
#include "backend/updateassetrules.h"
#include "SDL_compat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSysInfo>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

// beginProbe() 記下的動作名稱（呼叫端傳的是字串常值）；finish() 的收尾 log 行用。
const char* s_Action = nullptr;

// 動作 → log tag（docs/log_tags.md）。decode-bench 的慣例是 `[VIPLE-V4L2] bench: …`，
// 所以多一個接在 tag 後面的子前綴。
struct ActionTag {
    const char* action;
    const char* tag;
    const char* subPrefix;
};

const ActionTag k_ActionTags[] = {
    {"xr-probe",     "VIPLE-XR-PROBE",   ""},
    {"v4l2-probe",   "VIPLE-V4L2-PROBE", ""},
    {"decode-bench", "VIPLE-V4L2",       "bench: "},
};

const ActionTag& tagForAction(const char* action)
{
    // 保底：未知動作（不應發生）掛在 SF-ENV 底下，至少 grep 得到。
    static const ActionTag k_Unknown = {"", "VIPLE-SF-ENV", "probe: "};
    if (action != nullptr) {
        for (const ActionTag& entry : k_ActionTags) {
            if (strcmp(entry.action, action) == 0) {
                return entry;
            }
        }
    }
    return k_Unknown;
}

// errno → 可讀字串。MSVC 的 strerror 會觸發 C4996，改用 strerror_s；
// 不用 qt_error_string：它在 Windows 會把數值當 Win32 錯誤碼解讀。
QString errnoString(int err)
{
#ifdef _MSC_VER
    char buf[256];
    if (strerror_s(buf, sizeof(buf), err) == 0) {
        return QString::fromLocal8Bit(buf);
    }
    return QString("errno %1").arg(err);
#else
    return QString::fromLocal8Bit(std::strerror(err));
#endif
}

// vsnprintf 成 QByteArray（一般一行放得進 stack buffer，放不下才配置）。
QByteArray formatV(const char* fmt, va_list ap)
{
    char stackBuf[1024];
    va_list apCopy;
    va_copy(apCopy, ap);
    const int needed = vsnprintf(stackBuf, sizeof(stackBuf), fmt, apCopy);
    va_end(apCopy);
    if (needed < 0) {
        return QByteArray("(printLine: format error)");
    }
    if (needed < (int)sizeof(stackBuf)) {
        return QByteArray(stackBuf, needed);
    }
    QByteArray buf(needed + 1, '\0');
    vsnprintf(buf.data(), (size_t)buf.size(), fmt, ap);
    buf.resize(needed);
    return buf;
}

// 沙箱內的 app id（給提示字串用）：flatpak 會在沙箱內設 FLATPAK_ID；沒有就讀 /.flatpak-info。
QString flatpakAppId()
{
    QString id = qEnvironmentVariable("FLATPAK_ID");
    if (!id.isEmpty()) {
        return id;
    }
    QFile info(QStringLiteral("/.flatpak-info"));
    if (info.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool inApplication = false;
        while (!info.atEnd()) {
            const QString line = QString::fromUtf8(info.readLine()).trimmed();
            if (line.startsWith('[')) {
                inApplication = line == QLatin1String("[Application]");
            }
            else if (inApplication && line.startsWith(QLatin1String("name="))) {
                return line.mid(5);
            }
        }
    }
    return QStringLiteral("<app-id>");
}

// 一行平台描述，例：linux-arm64 flatpak、winnt-x86_64 plain、linux-x86_64 plain (running on arm64)。
QString platformString()
{
    const UpdateAssetRules::Platform p = UpdateAssetRules::detectPlatform();
    QString s = QStringLiteral("%1-%2 %3").arg(QSysInfo::kernelType(), p.buildArch,
                                               QLatin1String(UpdateAssetRules::toString(p.packaging)));
    if (p.emulated) {
        s += QStringLiteral(" (running on %1)").arg(p.currentArch);
    }
    return s;
}

} // namespace

namespace ProbeUtil {

void beginProbe(const char* action)
{
    s_Action = action;

    // stdout 被 pipe／導向檔案時預設是全緩衝：driver 在 ioctl 或 xrCreateInstance 裡當掉，
    // 已經印出的行會跟著緩衝一起消失。Linux 改行緩衝；MSVC 的 _IOLBF 等同全緩衝，而且
    // size=0 會觸發 invalid parameter handler（直接終止行程），Windows 一律用不緩衝。
    // 呼叫前 stdout 還沒有任何輸出（main.cpp 的早期派發路徑），setvbuf 合法。
#ifdef Q_OS_WIN32
    setvbuf(stdout, nullptr, _IONBF, 0);
#else
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif

    const ActionTag& t = tagForAction(action);
    const QByteArray version = QCoreApplication::applicationVersion().toUtf8();
    const QByteArray platform = platformString().toUtf8();

    fprintf(stdout, "%s: VipleStream %s %s\n", action ? action : "probe", version.constData(), platform.constData());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[%s] %s%s: VipleStream %s %s pid=%lld",
                t.tag, t.subPrefix, action ? action : "probe",
                version.constData(), platform.constData(),
                (long long)QCoreApplication::applicationPid());
}

QString defaultJsonPath(const QString& action)
{
    return QDir(Path::getLogDir()).filePath(QStringLiteral("probe-%1-%2-%3.json")
                                                .arg(action)
                                                .arg(QDateTime::currentMSecsSinceEpoch())
                                                .arg(QCoreApplication::applicationPid()));
}

bool writeJson(const QString& path, const QJsonObject& obj)
{
    if (path.isEmpty()) {
        fprintf(stderr, "json: no output path\n");
        return false;
    }

    const QString nativePath = QDir::toNativeSeparators(path);
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dirPath)) {
        const int err = errno;
        fprintf(stderr, "json: cannot create directory %s: %s\n",
                qPrintable(QDir::toNativeSeparators(dirPath)),
                qPrintable(isInFlatpak() ? sandboxPathHint(dirPath, err) : QStringLiteral("mkpath failed")));
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QString reason = file.errorString();
#ifdef Q_OS_UNIX
        // QFile 的錯誤字串不含 errno 數值；沙箱提示要 errno，用 access(2) 重新取得一個可靠的值
        // （目錄不可見 = ENOENT、不可寫 = EACCES／EROFS）。
        if (isInFlatpak()) {
            const QByteArray dirBytes = QFile::encodeName(dirPath);
            const int err = ::access(dirBytes.constData(), W_OK) != 0 ? errno : EACCES;
            reason = sandboxPathHint(path, err);
        }
#endif
        fprintf(stderr, "json: cannot write %s: %s\n", qPrintable(nativePath), qPrintable(reason));
        return false;
    }

    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size() || !file.flush()) {
        fprintf(stderr, "json: write to %s failed: %s\n", qPrintable(nativePath), qPrintable(file.errorString()));
        file.close();
        return false;
    }
    file.close();
    return true;
}

void printLine(const char* tag, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    QByteArray line = formatV(fmt, ap);
    va_end(ap);

    // 一行一個事實：呼叫端多帶的結尾換行拿掉，換行由這裡統一加
    while (line.endsWith('\n') || line.endsWith('\r')) {
        line.chop(1);
    }

    fwrite(line.constData(), 1, (size_t)line.size(), stdout);
    fputc('\n', stdout);

    if (tag != nullptr && tag[0] != '\0') {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[%s] %s", tag, line.constData());
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", line.constData());
    }
}

bool isInFlatpak()
{
#ifdef Q_OS_LINUX
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"));
#else
    return false;
#endif
}

QString sandboxPathHint(const QString& path, int err)
{
    QString msg = errnoString(err);
    bool sandboxRelated = err == ENOENT || err == EACCES;
#ifdef EROFS
    sandboxRelated = sandboxRelated || err == EROFS;
#endif
    if (isInFlatpak() && sandboxRelated) {
        const QString appId = flatpakAppId();
        const QString dir = QFileInfo(path).absolutePath();
        msg += QStringLiteral(" -- inside the Flatpak sandbox only ~/.var/app/%1 and the paths granted by "
                              "finish-args are visible; rerun with `flatpak run --filesystem=%2:ro %1 ...` "
                              "(drop :ro for an output path) or copy the file under ~/.var/app/%1/")
                   .arg(appId, dir);
    }
    return msg;
}

int finish(int rc, const QString& jsonPath)
{
    if (!jsonPath.isEmpty()) {
        fprintf(stderr, "json: %s\n", qPrintable(QDir::toNativeSeparators(jsonPath)));
    }

    // log 收尾一行，Day-1 收 log 時不必對照 stderr 就知道每次跑的結論
    const ActionTag& t = tagForAction(s_Action);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[%s] %s%s: done rc=%d json=%s",
                t.tag, t.subPrefix, s_Action ? s_Action : "probe", rc,
                jsonPath.isEmpty() ? "(none)" : qPrintable(jsonPath));

    fflush(stdout);
    fflush(stderr);
    return rc;
}

} // namespace ProbeUtil
