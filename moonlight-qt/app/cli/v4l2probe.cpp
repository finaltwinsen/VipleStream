// VipleStream §SF-PROBE（M2a）— `viplestream v4l2-probe`（CLI 包裝）。設計背景見 v4l2probe.h。
//
// 本檔只做選項解讀、彙整、結束碼與 JSON；ioctl 全在 streaming/video/v4l2/v4l2caps.cpp（日後
// L1 的 V4l2Decoder 共用）。所以這裡沒有平台條件編譯：非 Linux 建置由
// V4l2Caps::isSupported() 回 false，走結束碼 10。
//
// 判定：至少一個 m2m 裝置的 OUTPUT 有壓縮（coded）的視訊格式 → 0；否則印
// `no m2m decoder visible (…)` 並回 13。JPEG／MJPEG／DV／MPEG 容器這類非視訊的 compressed 格式
// 不算（role=non-video-decoder）。stateless decoder（Request API）與 vicodec 的 FWHT 也算數，
// 所以結束碼 0 不等於 app 能用：G-α 的替代路徑條件與 PoC-0 要看 summary 的
// hasStatefulVideoDecoder（H264/HEVC/AV1/VP9 的 stateful decoder，app 的 v4l2m2m 路徑只能用這種），
// stdout 另印一行 `stateful video decoder: yes|no`。
// header-test 失敗不改結束碼（decoder 存在與否才是主要能力），細節看摘要行與 JSON。
//
// JSON 一律寫檔（非 Linux 建置也寫：環境快照一樣有參考價值），SfEnv::snapshot() 放在 "env"。

#include "v4l2probe.h"

#include "probeutil.h"
#include "backend/sfenv.h"
#include "streaming/video/v4l2/v4l2caps.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace {

const char* const kTag = "VIPLE-V4L2-PROBE";

// --header-test：""（不做）、"all"、或以逗號分隔的 h264／hevc（h265、avc 當別名）
bool parseHeaderTest(const QString& raw, bool* enabled, QStringList* codecs, QString* error)
{
    *enabled = false;
    codecs->clear();
    const QString value = raw.trimmed().toLower();
    if (value.isEmpty()) {
        return true;
    }
    if (value == QLatin1String("all")) {
        *enabled = true;  // codecs 留空 = 該裝置支援、而且有測試幀的全部 codec
        return true;
    }
    const QStringList items = value.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& item : items) {
        const QString v = item.trimmed();
        QString key;
        if (v == QLatin1String("h264") || v == QLatin1String("avc")) {
            key = QStringLiteral("h264");
        }
        else if (v == QLatin1String("hevc") || v == QLatin1String("h265")) {
            key = QStringLiteral("hevc");
        }
        else {
            *error = QStringLiteral("'") + v + QStringLiteral("' (expected h264, hevc or all)");
            return false;
        }
        if (!codecs->contains(key)) {
            codecs->append(key);
        }
    }
    if (codecs->isEmpty()) {
        *error = QStringLiteral("'") + raw + QStringLiteral("' (expected h264, hevc or all)");
        return false;
    }
    *enabled = true;
    return true;
}

QString hex32(uint32_t v)
{
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(8, QLatin1Char('0'));
}

QJsonObject summaryJson(const V4l2Caps::DeviceSummary& d)
{
    QJsonObject o;
    o[QStringLiteral("path")] = d.path;
    if (!d.sysName.isEmpty()) {
        o[QStringLiteral("sysName")] = d.sysName;
    }
    o[QStringLiteral("openErrno")] = d.openErrno;
    if (d.openErrno == 0) {
        o[QStringLiteral("driver")] = d.driver;
        o[QStringLiteral("card")] = d.card;
        o[QStringLiteral("deviceCaps")] = hex32(d.deviceCaps);
        o[QStringLiteral("m2m")] = d.m2m;
        o[QStringLiteral("m2mMplane")] = d.m2mMplane;
    }
    return o;
}

} // namespace

int runV4l2Probe(const V4l2ProbeOptions& options)
{
    ProbeUtil::beginProbe("v4l2-probe");

    bool headerTest = false;
    QStringList headerCodecs;
    QString parseError;
    if (!parseHeaderTest(options.headerTest, &headerTest, &headerCodecs, &parseError)) {
        ProbeUtil::printLine(kTag, "invalid --header-test value %s", qUtf8Printable(parseError));
        return ProbeUtil::finish(ProbeUtil::kExitBadInput, QString());
    }

    const QString jsonPath = options.jsonPath.isEmpty()
                                 ? ProbeUtil::defaultJsonPath(QStringLiteral("v4l2-probe"))
                                 : options.jsonPath;

    QJsonObject root;
    root[QStringLiteral("probe")] = QStringLiteral("v4l2-probe");
    root[QStringLiteral("schema")] = 1;
    root[QStringLiteral("appVersion")] = QCoreApplication::applicationVersion();
    root[QStringLiteral("timeUtc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    {
        QJsonObject opts;
        opts[QStringLiteral("device")] = options.device;
        opts[QStringLiteral("headerTest")] = options.headerTest;
        opts[QStringLiteral("headerCodecs")] = QJsonArray::fromStringList(headerCodecs);
        opts[QStringLiteral("expbuf")] = options.expbuf;
        root[QStringLiteral("options")] = opts;
    }
    root[QStringLiteral("supported")] = V4l2Caps::isSupported();
    root[QStringLiteral("env")] = SfEnv::snapshot();

    int rc = ProbeUtil::kExitOk;

    if (!V4l2Caps::isSupported()) {
        ProbeUtil::printLine(kTag, "v4l2-probe: not available in this build (V4L2 needs Linux with <linux/videodev2.h>)");
        root[QStringLiteral("result")] = QStringLiteral("not-built");
        rc = ProbeUtil::kExitNotBuilt;
    }
    else {
        if (options.expbuf && !headerTest) {
            ProbeUtil::printLine(kTag, "note: --expbuf only takes effect together with --header-test; ignored");
        }

        // sysfs 視角：/dev 看不到節點、sysfs 卻有，代表節點沒有開放給這個行程（沙箱）
        const QList<V4l2Caps::DeviceSummary> sysfs = V4l2Caps::enumerateSysfsOnly();
        {
            QJsonArray arr;
            QStringList text;
            for (const V4l2Caps::DeviceSummary& d : sysfs) {
                QJsonObject o;
                o[QStringLiteral("path")] = d.path;
                o[QStringLiteral("name")] = d.sysName;
                arr.append(o);
                text << (d.path.mid(5) + QStringLiteral("=\"") + d.sysName + QLatin1Char('"'));
            }
            root[QStringLiteral("sysfs")] = arr;
            ProbeUtil::printLine(kTag, "sysfs: %d video node(s)%s%s", static_cast<int>(sysfs.size()),
                                 sysfs.isEmpty() ? "" : ": ", qUtf8Printable(text.join(QLatin1Char(' '))));
        }

        // /dev/media*：stateless decoder（Request API）要搭配 media controller 節點
        const QStringList mediaNodes = QDir(QStringLiteral("/dev"))
                                           .entryList(QStringList{QStringLiteral("media*")},
                                                      QDir::System | QDir::NoDotAndDotDot, QDir::Name);
        {
            QJsonArray arr;
            for (const QString& m : mediaNodes) {
                arr.append(QStringLiteral("/dev/") + m);
            }
            root[QStringLiteral("mediaNodes")] = arr;
        }

        QStringList paths;
        if (!options.device.isEmpty()) {
            paths << options.device;
            ProbeUtil::printLine(kTag, "device: %s (--device)", qUtf8Printable(options.device));
        }
        else {
            const QList<V4l2Caps::DeviceSummary> nodes = V4l2Caps::enumerate();
            QJsonArray arr;
            for (const V4l2Caps::DeviceSummary& d : nodes) {
                paths << d.path;
                arr.append(summaryJson(d));
            }
            root[QStringLiteral("nodes")] = arr;
            ProbeUtil::printLine(kTag, "nodes: %d /dev/video* node(s), %d /dev/media* node(s)%s",
                                 static_cast<int>(paths.size()), static_cast<int>(mediaNodes.size()),
                                 ProbeUtil::isInFlatpak() ? " (inside Flatpak)" : "");
        }

        V4l2Caps::ProbeOptions probeOptions;
        probeOptions.headerTest = headerTest;
        probeOptions.headerCodecs = headerCodecs;
        probeOptions.expbuf = options.expbuf;

        QJsonArray devices;
        QJsonArray decoders;
        QStringList decoderText;
        QStringList statefulVideoText;
        QStringList openFailed;
        int notV4l2 = 0;
        int notM2m = 0;
        int m2mNotDecoder = 0;
        int nonVideoDecoders = 0;
        for (const QString& path : paths) {
            const QJsonObject d = V4l2Caps::probeDevice(path, probeOptions);
            devices.append(d);

            const QJsonObject open = d[QStringLiteral("open")].toObject();
            const QJsonObject querycap = d[QStringLiteral("querycap")].toObject();
            if (!open[QStringLiteral("ok")].toBool()) {
                openFailed << (path + QLatin1Char(' ') + open[QStringLiteral("errnoName")].toString());
            }
            else if (d[QStringLiteral("role")].toString() == QLatin1String("not-v4l2")) {
                ++notV4l2;
            }
            else if (!d[QStringLiteral("m2m")].toBool()) {
                ++notM2m;
            }
            else if (!d[QStringLiteral("isDecoder")].toBool()) {
                if (d[QStringLiteral("role")].toString() == QLatin1String("non-video-decoder")) {
                    ++nonVideoDecoders;  // 例如 JPEG decoder：對 app 沒用，不算 decoder
                }
                else {
                    ++m2mNotDecoder;
                }
            }
            else {
                QJsonObject dec;
                dec[QStringLiteral("path")] = path;
                dec[QStringLiteral("driver")] = querycap[QStringLiteral("driver")];
                dec[QStringLiteral("card")] = querycap[QStringLiteral("card")];
                dec[QStringLiteral("kind")] = d[QStringLiteral("decoderKind")];
                dec[QStringLiteral("legacyCaps")] = d[QStringLiteral("m2mLegacyCaps")];
                dec[QStringLiteral("codedFormats")] = d[QStringLiteral("codedFormats")];
                dec[QStringLiteral("statefulVideoCodecs")] = d[QStringLiteral("statefulVideoCodecs")];
                if (d.contains(QStringLiteral("otherCodedFormats"))) {
                    dec[QStringLiteral("otherCodedFormats")] = d[QStringLiteral("otherCodedFormats")];
                }
                decoders.append(dec);

                QStringList coded;
                for (const QJsonValue& v : d[QStringLiteral("codedFormats")].toArray()) {
                    coded << v.toString();
                }
                QStringList statefulVideo;
                for (const QJsonValue& v : d[QStringLiteral("statefulVideoCodecs")].toArray()) {
                    statefulVideo << v.toString();
                }
                if (!statefulVideo.isEmpty()) {
                    statefulVideoText << (path + QStringLiteral("=[") + statefulVideo.join(QLatin1Char(',')) + QLatin1Char(']'));
                }
                decoderText << (path + QStringLiteral(" driver=") + querycap[QStringLiteral("driver")].toString() +
                                QStringLiteral(" kind=") + d[QStringLiteral("decoderKind")].toString() +
                                (d[QStringLiteral("m2mLegacyCaps")].toBool() ? QStringLiteral("(legacy-caps)") : QString()) +
                                QStringLiteral(" coded=[") + coded.join(QLatin1Char(',')) + QLatin1Char(']') +
                                QStringLiteral(" statefulVideo=[") + statefulVideo.join(QLatin1Char(',')) + QLatin1Char(']'));
            }
        }
        root[QStringLiteral("devices")] = devices;

        // app 的 v4l2m2m 路徑真正能用的：H264/HEVC/AV1/VP9 的 stateful decoder（G-α／PoC-0 看這一項）
        const bool hasStatefulVideoDecoder = !statefulVideoText.isEmpty();

        QJsonObject summary;
        summary[QStringLiteral("nodes")] = static_cast<int>(paths.size());
        summary[QStringLiteral("openFailed")] = QJsonArray::fromStringList(openFailed);
        summary[QStringLiteral("notV4l2")] = notV4l2;
        summary[QStringLiteral("notM2m")] = notM2m;
        summary[QStringLiteral("m2mNotDecoder")] = m2mNotDecoder;
        summary[QStringLiteral("nonVideoDecoders")] = nonVideoDecoders;
        summary[QStringLiteral("decoders")] = decoders;
        summary[QStringLiteral("hasStatefulVideoDecoder")] = hasStatefulVideoDecoder;

        if (!decoders.isEmpty()) {
            ProbeUtil::printLine(kTag, "m2m decoders: %d: %s", static_cast<int>(decoders.size()),
                                 qUtf8Printable(decoderText.join(QStringLiteral("; "))));
            if (hasStatefulVideoDecoder) {
                ProbeUtil::printLine(kTag, "stateful video decoder: yes (%s)",
                                     qUtf8Printable(statefulVideoText.join(QStringLiteral("; "))));
            }
            else {
                ProbeUtil::printLine(kTag, "stateful video decoder: no (no H264/HEVC/AV1/VP9 stateful decoder; "
                                           "the app's v4l2m2m path cannot use these decoders)");
            }
            summary[QStringLiteral("result")] = QStringLiteral("ok");
            rc = ProbeUtil::kExitOk;
        }
        else {
            QString reason;
            if (paths.isEmpty()) {
                if (sysfs.isEmpty()) {
                    reason = QStringLiteral("no /dev/video* nodes; sysfs lists none either");
                }
                else {
                    reason = QStringLiteral("no /dev/video* nodes, but sysfs lists ") + QString::number(sysfs.size()) +
                             QStringLiteral("; device nodes are not exposed to this process") +
                             (ProbeUtil::isInFlatpak() ? QStringLiteral(" (Flatpak needs --device=all)") : QString());
                }
            }
            else {
                QStringList bits;
                bits << (QString::number(paths.size()) + QStringLiteral(" node(s)"));
                if (!openFailed.isEmpty()) {
                    bits << (QStringLiteral("open failed: ") + openFailed.join(QStringLiteral(", ")));
                }
                if (notV4l2 > 0) {
                    bits << (QString::number(notV4l2) + QStringLiteral(" not V4L2"));
                }
                if (notM2m > 0) {
                    bits << (QString::number(notM2m) + QStringLiteral(" not m2m"));
                }
                if (m2mNotDecoder > 0) {
                    bits << (QString::number(m2mNotDecoder) +
                             QStringLiteral(" m2m without a compressed OUTPUT format (encoder/converter)"));
                }
                if (nonVideoDecoders > 0) {
                    bits << (QString::number(nonVideoDecoders) +
                             QStringLiteral(" m2m with only non-video coded formats (JPEG etc.)"));
                }
                reason = bits.join(QStringLiteral("; "));
            }
            ProbeUtil::printLine(kTag, "no m2m decoder visible (%s)", qUtf8Printable(reason));
            summary[QStringLiteral("result")] = QStringLiteral("no m2m decoder visible (") + reason + QLatin1Char(')');
            rc = ProbeUtil::kExitCapabilityAbsent;
        }
        root[QStringLiteral("summary")] = summary;
    }

    root[QStringLiteral("rc")] = rc;

    QString writtenPath = jsonPath;
    if (!ProbeUtil::writeJson(jsonPath, root)) {
        writtenPath.clear();
        // 和 xr-probe 相同：JSON 寫不出來只蓋掉 rc=0；其他結束碼本身就是結論（stdout 已印細節）
        if (rc == ProbeUtil::kExitOk) {
            rc = ProbeUtil::kExitJsonWriteFailed;
        }
    }
    return ProbeUtil::finish(rc, writtenPath);
}
