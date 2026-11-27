#include "logdrop.h"

#include "logredactor.h"
#include "path.h"
#include "sfenv.h"
#include "version_string.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QSysInfo>
#include <QEvent>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <array>
#include <chrono>
#include <thread>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

// §LOGDROP：網址空著就整段關掉。目前與 Android、server 同一支收件端。
const char kEndpoint[] = "https://script.google.com/macros/s/AKfycbzaTG_jii7H8485RNs9YRigKwT29F4RnA8wsLFQDGRPdiRVkt2dVMHiea56GNcJehu75w/exec";
const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr int kInstallIdLength = 8;
constexpr qint64 kTriggerGapMs = 10LL * 60LL * 1000LL;
constexpr int kDailyLimit = 8;
constexpr qint64 kLogTailBytes = 4LL * 1024LL * 1024LL;
// §LOGDROP-1127：串流結束只送最後 512 KB（crash 與手動上傳維持 4 MB）。
constexpr qint64 kSessionEndTailBytes = 512LL * 1024LL;
// §LOGDROP-1127：摘要要的解析度／編碼／要求幀率在開場，只多讀檔頭。
constexpr qint64 kSummaryHeadBytes = 256LL * 1024LL;
constexpr int kNoteLimit = 2000;

std::atomic<LogDrop*> g_Instance {nullptr};

struct UploadResult {
    bool ok = false;
    QString code;
    QString error;
};

UploadResult failure(const QString& error)
{
    UploadResult result;
    result.error = error;
    return result;
}

QString clipText(const QString& value, int max)
{
    if (value.size() <= max) {
        return value;
    }
    int end = max;
    // §LOGDROP：裁在代理對中間，收件端的 JSON 會壞掉。
    if (end > 0 && value.at(end - 1).isHighSurrogate()) {
        --end;
    }
    return value.left(end);
}

QString safeToken(const QString& value)
{
    QString out;
    for (int i = 0; i < value.size() && out.size() < 32; ++i) {
        const ushort u = value.at(i).unicode();
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '-') {
            out.append(value.at(i));
        }
    }
    return out;
}

QString localDay(qint64 nowMs)
{
    return QDateTime::fromMSecsSinceEpoch(nowMs).toString(QStringLiteral("yyyy-MM-dd"));
}

bool isInstallId(const QString& value)
{
    if (value.size() != kInstallIdLength) {
        return false;
    }
    const QString alphabet = QString::fromLatin1(kAlphabet);
    for (int i = 0; i < value.size(); ++i) {
        if (!alphabet.contains(value.at(i))) {
            return false;
        }
    }
    return true;
}

QString newInstallId()
{
    QString out;
    out.reserve(kInstallIdLength);
    const int alphabetLength = int(sizeof(kAlphabet) - 1);
    for (int i = 0; i < kInstallIdLength; ++i) {
        const int index = QRandomGenerator::system()->bounded(alphabetLength);
        out.append(QLatin1Char(kAlphabet[index]));
    }
    return out;
}

QString platformSource()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("qt-windows");
#elif defined(Q_OS_LINUX)
    // §LOGDROP：Frame 與一般 Linux 是同一份程式，用執行期的 os-release 分開，不要靠編譯巨集。
    if (SfEnv::isSteamFrame()) {
        return QStringLiteral("qt-steamframe");
    }
    return QStringLiteral("qt-linux");
#elif defined(Q_OS_DARWIN)
    return QStringLiteral("qt-macos");
#else
    return QStringLiteral("qt-other");
#endif
}

bool processAlive(qint64 pid)
{
    if (pid <= 0) {
        return false;
    }
#if defined(Q_OS_WIN)
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

QString ownMarkerName()
{
    return QStringLiteral("logdrop_running.") + QString::number(QCoreApplication::applicationPid());
}

quint32 gzipCrc32(const QByteArray& data)
{
    static const auto table = []() {
        std::array<quint32, 256> built {};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            built[i] = c;
        }
        return built;
    }();
    quint32 c = 0xFFFFFFFFu;
    const auto* bytes = reinterpret_cast<const uchar*>(data.constData());
    for (int i = 0; i < data.size(); ++i) {
        c = table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void appendLe32(QByteArray& out, quint32 value)
{
    out.append(char(value & 0xFF));
    out.append(char((value >> 8) & 0xFF));
    out.append(char((value >> 16) & 0xFF));
    out.append(char((value >> 24) & 0xFF));
}

// §LOGDROP：qCompress 是 zlib 包裝加 4 byte 長度，不是 gzip。收件端只解 gzip。
// 專案沒有 zlib.h，所以拿掉 zlib 標頭與 Adler32，補上 gzip 標頭與 CRC32。
QByteArray gzipEncode(const QByteArray& plain, bool* ok)
{
    *ok = false;
    if (plain.isEmpty()) {
        static const char kEmpty[] = {
            '\x1f', '\x8b', '\x08', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\xff',
            '\x03', '\x00',
            '\x00', '\x00', '\x00', '\x00',
            '\x00', '\x00', '\x00', '\x00'
        };
        *ok = true;
        return QByteArray(kEmpty, int(sizeof(kEmpty)));
    }
    const QByteArray wrapped = qCompress(plain);
    if (wrapped.size() < 10) {
        return QByteArray();
    }
    const auto* bytes = reinterpret_cast<const uchar*>(wrapped.constData());
    const quint32 declared = (quint32(bytes[0]) << 24) | (quint32(bytes[1]) << 16)
            | (quint32(bytes[2]) << 8) | quint32(bytes[3]);
    if (declared != quint32(plain.size())) {
        return QByteArray();
    }
    const uchar cmf = bytes[4];
    const uchar flg = bytes[5];
    if ((cmf & 0x0F) != 8 || (flg & 0x20) != 0) {
        return QByteArray();
    }
    if (((int(cmf) << 8) | int(flg)) % 31 != 0) {
        return QByteArray();
    }
    const int deflateLen = wrapped.size() - 4 - 2 - 4;
    if (deflateLen <= 0) {
        return QByteArray();
    }
    QByteArray gzip;
    gzip.reserve(10 + deflateLen + 8);
    gzip.append('\x1f');
    gzip.append('\x8b');
    gzip.append('\x08');
    gzip.append('\x00');
    gzip.append('\x00');
    gzip.append('\x00');
    gzip.append('\x00');
    gzip.append('\x00');
    gzip.append('\x00');
    gzip.append('\xff');
    gzip.append(wrapped.constData() + 6, deflateLen);
    appendLe32(gzip, gzipCrc32(plain));
    appendLe32(gzip, quint32(plain.size()));
    *ok = true;
    return gzip;
}

void logBad(int status, const QString& raw)
{
    QString body = raw;
    if (body.size() > 200) {
        body.truncate(200);
    }
    body.replace(QLatin1Char('\n'), QLatin1Char(' '));
    body.replace(QLatin1Char('\r'), QLatin1Char(' '));
    qWarning().nospace() << "[VIPLE-LOGDROP] bad_response http=" << status << " body=" << body;
}

UploadResult incomplete(bool hasStatus, int status, const QString& raw)
{
    if (!hasStatus || status < 200 || status >= 300) {
        if (!hasStatus || status == 0) {
            return failure(QStringLiteral("network"));
        }
        qWarning().nospace() << "[VIPLE-LOGDROP] http_" << status;
        return failure(QStringLiteral("http_") + QString::number(status));
    }
    logBad(status, raw);
    return failure(QStringLiteral("bad_response"));
}

UploadResult interpret(bool hasStatus, int status, const QString& raw)
{
    const QString body = raw.trimmed();
    if (body.isEmpty()) {
        return incomplete(hasStatus, status, raw);
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        logBad(hasStatus ? status : 0, raw);
        return failure(QStringLiteral("bad_response"));
    }
    const QJsonObject json = document.object();
    const QJsonValue okValue = json.value(QStringLiteral("ok"));
    if (!okValue.isBool()) {
        return incomplete(hasStatus, status, raw);
    }
    if (okValue.toBool()) {
        const QJsonValue codeValue = json.value(QStringLiteral("code"));
        if (codeValue.isUndefined() || codeValue.isNull()) {
            return incomplete(hasStatus, status, raw);
        }
        const QString code = clipText(codeValue.toString(), 64);
        if (code.isEmpty()) {
            return incomplete(hasStatus, status, raw);
        }
        UploadResult result;
        result.ok = true;
        result.code = code;
        return result;
    }
    const QJsonValue errorValue = json.value(QStringLiteral("error"));
    if (errorValue.isUndefined() || errorValue.isNull()) {
        return incomplete(hasStatus, status, raw);
    }
    const QString error = clipText(errorValue.toString(), 200);
    if (error.isEmpty()) {
        return incomplete(hasStatus, status, raw);
    }
    return failure(error);
}

UploadResult postBody(const QByteArray& body)
{
    QNetworkAccessManager nam;
    // §LOGDROP：doPost 先 302。只設在 request 上時，Qt 6 的 manager 預設可能不跟。跟了之後 302／303 會改成 GET。
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest request{QUrl(QString::fromLatin1(kEndpoint))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(120000);
    QNetworkReply* reply = nam.post(request, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const QVariant statusValue = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const bool hasStatus = statusValue.isValid();
    const int status = statusValue.toInt();
    const QByteArray raw = reply->readAll();
    const int networkError = int(reply->error());
    const bool transportFailed = reply->error() != QNetworkReply::NoError;
    delete reply;
    if (transportFailed && !hasStatus) {
        qWarning().nospace() << "[VIPLE-LOGDROP] network error=" << networkError;
        return failure(QStringLiteral("network"));
    }
    return interpret(hasStatus, status, QString::fromUtf8(raw));
}

QString previousLogPath()
{
    // §LOGDROP：QDir::Time 哪個新哪個舊跟版本有關。crash 要修改時間最新、且不是這次行程的那份。
    const QString current = QFileInfo(vipleCurrentLogFilePath()).absoluteFilePath();
    const QFileInfoList files = QDir(Path::getLogDir()).entryInfoList(
            QStringList(QStringLiteral("VipleStream-*.log")),
            QDir::Files | QDir::NoSymLinks);
    QString bestPath;
    QDateTime bestTime;
    for (const QFileInfo& info : files) {
        if (!current.isEmpty() && info.absoluteFilePath() == current) {
            continue;
        }
        const QDateTime modified = info.lastModified();
        if (bestPath.isEmpty() || modified > bestTime) {
            bestPath = info.absoluteFilePath();
            bestTime = modified;
        }
    }
    return bestPath;
}

QString readTail(const QString& path, bool previous, qint64 maxBytes)
{
    if (path.isEmpty()) {
        return previous
                ? QStringLiteral("[VIPLE-LOGDROP] previous log unavailable\n")
                : QStringLiteral("[VIPLE-LOGDROP] log unavailable\n");
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return previous
                ? QStringLiteral("[VIPLE-LOGDROP] previous log unavailable\n")
                : QStringLiteral("[VIPLE-LOGDROP] log unavailable\n");
    }
    const qint64 size = file.size();
    bool truncated = false;
    if (size > maxBytes) {
        if (!file.seek(size - maxBytes)) {
            return QStringLiteral("[VIPLE-LOGDROP] log unavailable\n");
        }
        truncated = true;
    }
    const QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes);
    // §LOGDROP：切在擷取上限邊界可能切到半個字或半行，從下一個換行才是完整的一行。
    if (truncated) {
        const int newline = text.indexOf(QLatin1Char('\n'));
        if (newline >= 0 && newline + 1 < text.size()) {
            text = text.mid(newline + 1);
        }
    }
    return text;
}

QString readHead(const QString& path, qint64 maxBytes)
{
    if (path.isEmpty()) {
        return QString();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QString text = QString::fromUtf8(file.read(maxBytes));
    const int newline = text.lastIndexOf(QLatin1Char('\n'));
    if (newline >= 0 && newline + 1 < text.size()) {
        text.truncate(newline + 1);
    }
    return text;
}

QString clippedToken(const QString& value, const QString& allowed, int max)
{
    QString out;
    for (int i = 0; i < value.size() && out.size() < max; ++i) {
        if (allowed.contains(value.at(i))) {
            out.append(value.at(i));
        }
    }
    return out;
}

// §LOGDROP-1127：`# summary:` 只用 log 裡固定格式的數字與代碼，不搬自由文字；欄位找不到就省略。
// head 給開場的解析度／編碼／要求幀率，body 給最近的取樣；只有 body 的 fps 樣本會計入平均，
// 兩段重疊時才不會把同一個樣本算兩次。
static void scanSummary(const QString& text, bool countSamples,
                        QString& res, QString& requestedFps, QString& codec, QString& audio, QString& loss,
                        double& fpsSum, int& fpsCount)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        if (line.contains(QLatin1String("[VIPLE-SESSION]"))) {
            const QString fpsMarker = QStringLiteral("fps=");
            int p = line.indexOf(fpsMarker);
            if (p >= 0) {
                int e = p + fpsMarker.size();
                while (e < line.size() && (line.at(e).isDigit() || line.at(e) == QLatin1Char('.'))) {
                    ++e;
                }
                requestedFps = clippedToken(line.mid(p + fpsMarker.size(), e - p - fpsMarker.size()),
                                            QStringLiteral("0123456789."), 8);
            }
            const QString codecMarker = QStringLiteral("codecs=0x");
            p = line.indexOf(codecMarker);
            if (p >= 0) {
                int e = p + codecMarker.size();
                while (e < line.size() && line.at(e).isLetterOrNumber()) {
                    ++e;
                }
                bool ok = false;
                const uint mask = line.mid(p + codecMarker.size(), e - p - codecMarker.size()).toUInt(&ok, 16);
                if (ok) {
                    // 遮罩定義與 MoonBridge.VIDEO_FORMAT_MASK_* 相同。
                    if ((mask & 0x00F00000u) != 0) {
                        codec = QStringLiteral("pyrowave");
                    }
                    else if ((mask & 0x000Fu) != 0) {
                        codec = QStringLiteral("h264");
                    }
                    else if ((mask & 0x0F00u) != 0) {
                        codec = QStringLiteral("h265");
                    }
                    else if ((mask & 0xF000u) != 0) {
                        codec = QStringLiteral("av1");
                    }
                }
            }
        }
        const QString resMarker = QStringLiteral("desktop resolution: ");
        int p = line.indexOf(resMarker);
        if (p >= 0) {
            int e = p + resMarker.size();
            int separators = 0;
            while (e < line.size()) {
                const QChar c = line.at(e);
                if (c == QLatin1Char('x')) {
                    if (separators >= 1) {
                        break;
                    }
                    ++separators;
                }
                else if (!c.isDigit()) {
                    break;
                }
                ++e;
            }
            res = clippedToken(line.mid(p + resMarker.size(), e - p - resMarker.size()),
                               QStringLiteral("0123456789x"), 16);
        }
        if (countSamples && line.contains(QLatin1String("[VIPLE-2D-PERF]"))) {
            const QString fpsMarker = QStringLiteral("fps=");
            int q = line.indexOf(fpsMarker);
            if (q >= 0) {
                int e = q + fpsMarker.size();
                while (e < line.size() && (line.at(e).isDigit() || line.at(e) == QLatin1Char('.'))) {
                    ++e;
                }
                bool ok = false;
                const double value = line.mid(q + fpsMarker.size(), e - q - fpsMarker.size()).toDouble(&ok);
                if (ok && value > 0.0) {
                    fpsSum += value;
                    ++fpsCount;
                }
            }
        }
        const QString audioMarker = QStringLiteral("server audio status ");
        p = line.indexOf(audioMarker);
        if (p >= 0) {
            p += audioMarker.size();
            if (p < line.size()) {
                const QChar status = line.at(p);
                if (status == QLatin1Char('0') || status == QLatin1Char('2')) {
                    audio = QStringLiteral("ok");
                }
                else if (status == QLatin1Char('1')) {
                    audio = QStringLiteral("stalled");
                }
                else if (status == QLatin1Char('3')) {
                    audio = QStringLiteral("none");
                }
            }
        }
        const QString lossMarker = QStringLiteral("loss=");
        p = line.indexOf(lossMarker);
        if (p >= 0) {
            int e = p + lossMarker.size();
            while (e < line.size() && (line.at(e).isDigit() || line.at(e) == QLatin1Char('.'))) {
                ++e;
            }
            if (e > p + lossMarker.size() && e < line.size() && line.at(e) == QLatin1Char('%')) {
                loss = clippedToken(line.mid(p + lossMarker.size(), e - p - lossMarker.size()),
                                    QStringLiteral("0123456789."), 8)
                        + QLatin1Char('%');
            }
        }
    }
}

QString summarizeLog(const QString& head, const QString& body, bool sessionEnd)
{
    QString res;
    QString requestedFps;
    QString codec;
    QString audio;
    QString loss;
    double fpsSum = 0.0;
    int fpsCount = 0;
    scanSummary(head, false, res, requestedFps, codec, audio, loss, fpsSum, fpsCount);
    scanSummary(body, true, res, requestedFps, codec, audio, loss, fpsSum, fpsCount);

    QStringList parts;
    if (sessionEnd) {
        // 串流結束一定有一場（client 端的 log 沒有 server 的 CLIENT CONNECTED 可以數）。
        parts.append(QStringLiteral("sessions=1"));
    }
    if (!res.isEmpty()) {
        QString value = QStringLiteral("res=") + res;
        if (!requestedFps.isEmpty()) {
            value += QLatin1Char('@') + requestedFps;
        }
        parts.append(value);
    }
    if (!codec.isEmpty()) {
        parts.append(QStringLiteral("codec=") + codec);
    }
    if (fpsCount > 0) {
        parts.append(QStringLiteral("avgFps=") + QString::number(fpsSum / fpsCount, 'f', 1));
    }
    if (!loss.isEmpty()) {
        parts.append(QStringLiteral("loss=") + loss);
    }
    if (!audio.isEmpty()) {
        parts.append(QStringLiteral("audio=") + audio);
    }
    return parts.join(QLatin1Char(' '));
}

// §LOGDROP-1127：當機處理常式把 `.dmp` 檔名寫進 log（只有檔名、沒有使用者路徑）。下次上傳從上一輪的
// log 抓出來，不另外存狀態檔。
QString crashDumpName(const QString& text)
{
    const QString marker = QStringLiteral("Minidump written to:");
    const int p = text.lastIndexOf(marker);
    if (p < 0) {
        return QString();
    }
    const int begin = p + marker.size();
    int end = text.indexOf(QLatin1Char('\n'), begin);
    if (end < 0) {
        end = text.size();
    }
    QString name = QFileInfo(text.mid(begin, end - begin).trimmed()).fileName();
    if (name.size() < 5 || name.size() > 64 || !name.endsWith(QLatin1String(".dmp"))) {
        return QString();
    }
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (!c.isLetterOrNumber() && c != QLatin1Char('.') && c != QLatin1Char('_') && c != QLatin1Char('-')) {
            return QString();
        }
    }
    return name;
}

QStringList savedComputerNames()
{
    // §LOGDROP：ComputerManager 是 QML 單例，上傳執行緒拿不到。已存名稱在 QSettings 的 hosts 陣列。
    QStringList names;
    QSettings settings;
    int count = settings.beginReadArray(QStringLiteral("hostsbackup"));
    if (count == 0) {
        settings.endArray();
        count = settings.beginReadArray(QStringLiteral("hosts"));
    }
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString name = settings.value(QStringLiteral("hostname")).toString();
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    settings.endArray();
    return names;
}

QStringList collectSecrets()
{
    QStringList secrets = savedComputerNames();
    const QString host = QSysInfo::machineHostName();
    if (!host.isEmpty()) {
        secrets.append(host);
    }
    const QString userName = qEnvironmentVariable("USERNAME");
    const QString user = qEnvironmentVariable("USER");
    if (!userName.isEmpty()) {
        secrets.append(userName);
    }
    if (!user.isEmpty() && user.compare(userName, Qt::CaseInsensitive) != 0) {
        secrets.append(user);
    }
    // §REVIEW-1127 qt_ui #13：設定檔目錄的名稱可能和 USERNAME 不同（改過帳號名稱、Microsoft 帳號）
    const QString homeName = QDir::home().dirName();
    if (!homeName.isEmpty() && homeName.compare(userName, Qt::CaseInsensitive) != 0 &&
            homeName.compare(user, Qt::CaseInsensitive) != 0) {
        secrets.append(homeName);
    }
    return secrets;
}

} // namespace

class LogDrop::UploadThread : public QThread
{
public:
    UploadThread(LogDrop* owner, QString trigger, QString note, QString source, QString version,
                 QString device, QString installId, QStringList secrets, int delayMs, bool previous, bool manual)
        : m_Owner(owner),
          m_Trigger(std::move(trigger)),
          m_Note(std::move(note)),
          m_Source(std::move(source)),
          m_Version(std::move(version)),
          m_Device(std::move(device)),
          m_InstallId(std::move(installId)),
          m_Secrets(std::move(secrets)),
          m_DelayMs(delayMs),
          m_Previous(previous),
          m_Manual(manual)
    {
    }

    void run() override
    {
        if (m_DelayMs > 0) {
            const int steps = m_DelayMs / 100;
            for (int i = 0; i < steps; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!m_Manual) {
                QMutexLocker lock(&m_Owner->m_Mu);
                if (m_Owner->m_Consent != QLatin1String("yes")) {
                    qInfo().nospace() << "[VIPLE-LOGDROP] auto trigger=" << safeToken(m_Trigger) << " skipped disabled";
                    return;
                }
            }
        }

        // §LOGDROP：先把已入列的行寫進檔，再讀。crash 要的是上一個行程的檔，不是這次的。
        vipleFlushLogFile();
        const QString path = m_Previous ? previousLogPath() : vipleCurrentLogFilePath();
        // §LOGDROP-1127：串流結束只送最後 512 KB；crash 與手動上傳維持原本的 4 MB。
        const qint64 tailBytes = m_Trigger == QLatin1String("session-end") ? kSessionEndTailBytes : kLogTailBytes;
        const QString text = readTail(path, m_Previous, tailBytes);
        QString prefix;
        const QString summary = summarizeLog(readHead(path, kSummaryHeadBytes), text,
                                             m_Trigger == QLatin1String("session-end"));
        if (!summary.isEmpty()) {
            prefix += QStringLiteral("# summary: ") + summary + QLatin1Char('\n');
        }
        if (m_Trigger == QLatin1String("crash")) {
            // §LOGDROP-1127：crash 觸發只代表「上次沒正常結束」。上一輪 log 有 Minidump 行才是例外，
            // 否則寫 unclean-exit，才不會把停服務誤標成當掉。
            const QString dump = crashDumpName(text);
            prefix += dump.isEmpty()
                    ? QStringLiteral("# exit: unclean-exit\n")
                    : QStringLiteral("# exit: exception dump=") + dump + QLatin1Char('\n');
        }
        const QString redacted = LogRedactor::redact(prefix + text, m_Secrets);
        const QByteArray utf8 = redacted.toUtf8();
        bool gzipOk = false;
        const QByteArray gzip = gzipEncode(utf8, &gzipOk);
        QString encoding;
        QString payload;
        if (gzipOk) {
            encoding = QStringLiteral("gzip-base64");
            payload = QString::fromLatin1(gzip.toBase64());
        }
        else {
            qWarning("[VIPLE-LOGDROP] gzip unavailable; sending text");
            encoding = QStringLiteral("text");
            payload = redacted;
        }

        QJsonObject json;
        json.insert(QStringLiteral("v"), 1);
        json.insert(QStringLiteral("source"), m_Source);
        json.insert(QStringLiteral("app_version"), m_Version);
        json.insert(QStringLiteral("device"), m_Device);
        json.insert(QStringLiteral("install_id"), m_InstallId);
        json.insert(QStringLiteral("trigger"), m_Trigger);
        json.insert(QStringLiteral("note"), clipText(m_Note, kNoteLimit));
        json.insert(QStringLiteral("encoding"), encoding);
        json.insert(QStringLiteral("payload"), payload);
        const QByteArray body = QJsonDocument(json).toJson(QJsonDocument::Compact);
        const UploadResult result = postBody(body);
        if (result.ok) {
            qInfo().nospace() << "[VIPLE-LOGDROP] trigger=" << safeToken(m_Trigger) << " ok code=" << result.code;
        }
        else {
            qWarning().nospace() << "[VIPLE-LOGDROP] trigger=" << safeToken(m_Trigger) << " error=" << result.error;
        }
        m_Owner->finishUpload(m_Manual, result.ok, result.code, result.error);
    }

private:
    LogDrop* m_Owner;
    QString m_Trigger;
    QString m_Note;
    QString m_Source;
    QString m_Version;
    QString m_Device;
    QString m_InstallId;
    QStringList m_Secrets;
    int m_DelayMs = 0;
    bool m_Previous = false;
    bool m_Manual = false;
};

LogDrop* LogDrop::instance()
{
    if (LogDrop* existing = g_Instance.load(std::memory_order_acquire)) {
        return existing;
    }
    if (QCoreApplication::instance() == nullptr
            || QThread::currentThread() != QCoreApplication::instance()->thread()) {
        return nullptr;
    }
    auto* created = new LogDrop();
    g_Instance.store(created, std::memory_order_release);
    return created;
}

LogDrop::LogDrop()
{
    loadState();
}

void LogDrop::loadState()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("logdrop"));
    const QString consent = settings.value(QStringLiteral("consent")).toString();
    if (consent == QLatin1String("yes") || consent == QLatin1String("no")) {
        m_Consent = consent;
    }
    const QString existing = settings.value(QStringLiteral("installId")).toString();
    if (isInstallId(existing)) {
        m_InstallId = existing;
    }
    else {
        m_InstallId = newInstallId();
        settings.setValue(QStringLiteral("installId"), m_InstallId);
        settings.sync();
        qInfo().nospace() << "[VIPLE-LOGDROP] created install id " << m_InstallId;
    }
    m_LastCode = settings.value(QStringLiteral("lastCode")).toString();
    m_LastTime = settings.value(QStringLiteral("lastTime")).toLongLong();
    settings.endGroup();
}

QString LogDrop::consent() const
{
    QMutexLocker lock(&m_Mu);
    return m_Consent;
}

QString LogDrop::installId() const
{
    return m_InstallId;
}

QString LogDrop::lastCode() const
{
    QMutexLocker lock(&m_Mu);
    return m_LastCode;
}

qint64 LogDrop::lastTime() const
{
    QMutexLocker lock(&m_Mu);
    return m_LastTime;
}

void LogDrop::setConsent(bool allow)
{
    const QString value = allow ? QStringLiteral("yes") : QStringLiteral("no");
    {
        QMutexLocker lock(&m_Mu);
        // §LOGDROP：只改記憶體的話，行程被殺掉會再問一次。
        m_Consent = value;
        QSettings settings;
        settings.beginGroup(QStringLiteral("logdrop"));
        settings.setValue(QStringLiteral("consent"), value);
        settings.sync();
        settings.endGroup();
    }
    emit consentChanged();
}

bool LogDrop::autoEnabled() const
{
    if (kEndpoint[0] == '\0') {
        return false;
    }
    QMutexLocker lock(&m_Mu);
    return m_Consent == QLatin1String("yes");
}

QString LogDrop::reserve(const QString& trigger)
{
    QMutexLocker lock(&m_Mu);
    QSettings settings;
    settings.beginGroup(QStringLiteral("logdrop"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString token = safeToken(trigger);
    const QString key = token.isEmpty() ? QStringLiteral("_") : token;
    settings.beginGroup(QStringLiteral("trigger"));
    const qint64 last = settings.value(key).toLongLong();
    settings.endGroup();
    if (last > 0 && now >= last && now - last < kTriggerGapMs) {
        settings.endGroup();
        return QStringLiteral("interval");
    }
    const QString day = localDay(now);
    const int count = settings.value(QStringLiteral("day")).toString() == day
            ? settings.value(QStringLiteral("dayCount")).toInt()
            : 0;
    if (count >= kDailyLimit) {
        settings.endGroup();
        return QStringLiteral("daily");
    }
    // §LOGDROP：網路失敗若把次數退回，背景會一直重送。決定送出時就扣。
    settings.beginGroup(QStringLiteral("trigger"));
    settings.setValue(key, now);
    settings.endGroup();
    settings.setValue(QStringLiteral("day"), day);
    settings.setValue(QStringLiteral("dayCount"), count + 1);
    settings.sync();
    settings.endGroup();
    return QString();
}

void LogDrop::startWorker(const QString& trigger, const QString& note, int delayMs, bool previous, bool manual)
{
    const QString source = platformSource();
    const QString version = clipText(QString::fromLatin1(VERSION_STR), 64);
    // §LOGDROP-1127：補上 OS build（Windows 是 10.0.x、Linux 是核心版號）。GPU 與驅動在 client 端拿不到，省略。
    QString device = QSysInfo::prettyProductName() + QStringLiteral(" / ") + QSysInfo::currentCpuArchitecture();
    const QString kernel = QSysInfo::kernelVersion();
    if (!kernel.isEmpty()) {
        device += QStringLiteral(" / kernel ") + kernel;
    }
    device = clipText(device, 200);
    QString install;
    QStringList secrets;
    {
        QMutexLocker lock(&m_Mu);
        install = m_InstallId;
        secrets = collectSecrets();
    }
    auto* thread = new UploadThread(this, trigger, note, source, version, device, install, secrets, delayMs, previous, manual);
    {
        QMutexLocker lock(&m_ThreadMu);
        m_Threads.append(thread);
    }
    thread->start();
}

void LogDrop::finishUpload(bool manual, bool ok, const QString& code, const QString& error)
{
    if (ok) {
        QMutexLocker lock(&m_Mu);
        m_LastCode = code;
        m_LastTime = QDateTime::currentMSecsSinceEpoch();
        QSettings settings;
        settings.beginGroup(QStringLiteral("logdrop"));
        settings.setValue(QStringLiteral("lastCode"), m_LastCode);
        settings.setValue(QStringLiteral("lastTime"), m_LastTime);
        settings.sync();
        settings.endGroup();
    }
    if (manual) {
        m_ManualBusy.store(0);
        emit uploadFinished(ok, ok ? code : QString(), ok ? QString() : error);
    }
    if (ok) {
        emit lastResultChanged();
    }
}

void LogDrop::uploadNow(const QString& note)
{
    if (m_ManualBusy.load()) {
        emit uploadFinished(false, QString(), QStringLiteral("busy"));
        return;
    }
    if (kEndpoint[0] == '\0') {
        emit uploadFinished(false, QString(), QStringLiteral("disabled"));
        return;
    }
    m_ManualBusy.store(1);
    startWorker(QStringLiteral("manual"), note, 0, false, true);
}

void LogDrop::beginAuto(QString trigger)
{
    if (m_Shutdown.load()) {
        return;
    }
    if (!autoEnabled()) {
        return;
    }
    const QString block = reserve(trigger);
    if (!block.isEmpty()) {
        // §LOGDROP：同一個狀況短時間結束兩次、或當天已經送滿，都不要再打收件端。
        qInfo().nospace() << "[VIPLE-LOGDROP] auto trigger=" << safeToken(trigger) << " skipped " << block;
        return;
    }
    const bool previous = trigger == QLatin1String("crash");
    const int delayMs = trigger == QLatin1String("session-end") ? 3000 : 0;
    startWorker(trigger, QString(), delayMs, previous, false);
}

void LogDrop::autoUpload(const QString& trigger)
{
    LogDrop* self = g_Instance.load(std::memory_order_acquire);
    if (self == nullptr) {
        qWarning("[VIPLE-LOGDROP] auto upload skipped: not ready");
        return;
    }
    if (QThread::currentThread() == self->thread()) {
        self->beginAuto(trigger);
        return;
    }
    QMetaObject::invokeMethod(self, "beginAuto", Qt::QueuedConnection, Q_ARG(QString, trigger));
}

bool LogDrop::consumeStaleMarkers()
{
    // §LOGDROP：VR 介面和串流子行程會同時活著。單一檔名會被對方當成當掉，或在對方還活著時被刪掉。
    QDir dir(Path::getLogDir());
    const QStringList names = dir.entryList(
            QStringList() << QStringLiteral("logdrop_running") << QStringLiteral("logdrop_running.*"),
            QDir::Files | QDir::NoSymLinks);
    const qint64 selfPid = QCoreApplication::applicationPid();
    const QString prefix = QStringLiteral("logdrop_running.");
    bool unclean = false;
    for (const QString& name : names) {
        bool stale = false;
        if (name == QLatin1String("logdrop_running")) {
            stale = true;
        }
        else if (name.startsWith(prefix)) {
            bool ok = false;
            const qint64 pid = name.mid(prefix.size()).toLongLong(&ok);
            if (!ok || pid <= 0) {
                stale = true;
            }
            else if (pid != selfPid) {
                stale = !processAlive(pid);
            }
        }
        if (!stale) {
            continue;
        }
        unclean = true;
        if (!QFile::remove(dir.filePath(name))) {
            qWarning("[VIPLE-LOGDROP] could not remove running marker");
        }
    }
    return unclean;
}

void LogDrop::writeOwnMarker() const
{
    QFile file(QDir(Path::getLogDir()).filePath(ownMarkerName()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("[VIPLE-LOGDROP] could not write running marker");
        return;
    }
    file.write("1\n");
}

void LogDrop::removeOwnMarker() const
{
    const QString path = QDir(Path::getLogDir()).filePath(ownMarkerName());
    if (QFile::exists(path) && !QFile::remove(path)) {
        qWarning("[VIPLE-LOGDROP] could not remove running marker");
    }
}

void LogDrop::waitForUploads()
{
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        QList<QThread*> alive;
        QList<QThread*> done;
        {
            QMutexLocker lock(&m_ThreadMu);
            for (QThread* thread : m_Threads) {
                if (thread->isFinished()) {
                    done.append(thread);
                }
                else {
                    alive.append(thread);
                }
            }
            for (QThread* thread : done) {
                m_Threads.removeAll(thread);
            }
        }
        for (QThread* thread : done) {
            delete thread;
        }
        if (alive.isEmpty()) {
            return;
        }
        // 視窗已經關了，使用者看不到這個行程；網路不通時不要掛在背景兩分鐘。
        if (timer.elapsed() > 20000) {
            qWarning("[VIPLE-LOGDROP] upload still running at exit");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void LogDrop::prepareForRun()
{
    LogDrop* self = instance();
    if (self == nullptr) {
        return;
    }
    const bool unclean = self->consumeStaleMarkers();
    self->writeOwnMarker();
    if (!unclean) {
        return;
    }
    qInfo("[VIPLE-LOGDROP] previous exit was unclean; uploading in 30s");
    QTimer::singleShot(30000, self, [self]() {
        if (self->m_Shutdown.load()) {
            return;
        }
        self->beginAuto(QStringLiteral("crash"));
    });
}

void LogDrop::markCleanExit()
{
    LogDrop* self = g_Instance.load(std::memory_order_acquire);
    if (self == nullptr) {
        return;
    }
    // §LOGDROP：串流清理執行緒用 queued 呼叫排進來的 session-end，排在 quit() 前面也可能還沒被處理，
    // 事件迴圈就結束了（命令列串流結束就直接離開）。先把這些呼叫送掉，再關門等它送完。
    // 關視窗結束時，事件迴圈可能比串流清理（全域執行緒池上的 DeferredSessionCleanupTask）先結束，
    // 清理到最後才會呼叫 autoUpload。先等清理跑完（有上限），再把排進來的呼叫送掉。
    QThreadPool::globalInstance()->waitForDone(15000);
    QCoreApplication::sendPostedEvents(self, QEvent::MetaCall);
    self->m_Shutdown.store(true);
    self->removeOwnMarker();
    self->waitForUploads();
}
