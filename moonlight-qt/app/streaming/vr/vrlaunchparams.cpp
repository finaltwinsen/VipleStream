#include "vrlaunchparams.h"

#include <Limelight.h>

#include <QStringList>

VrLaunchConfig::VrLaunchConfig()
{
    fovTan.fill(10000);  // tan 45° = 1.0
    caps = VIPLE_VR_CLIENT_CAP_RECOVERY_INTRA | VIPLE_VR_CLIENT_CAP_TRACK_THREAD;
    codecs = VIPLE_VR_CODEC_HEVC | VIPLE_VR_CODEC_H264;
    setIpd(ipd);
}

void VrLaunchConfig::setIpd(int ipdMmX100)
{
    ipd = ipdMmX100;
    const int half = ipdMmX100 / 2;
    // 左眼
    eyeToHead[0] = -half; eyeToHead[1] = 0; eyeToHead[2] = 0;
    eyeToHead[3] = 0; eyeToHead[4] = 0; eyeToHead[5] = 0; eyeToHead[6] = 10000;
    // 右眼
    eyeToHead[7] = half; eyeToHead[8] = 0; eyeToHead[9] = 0;
    eyeToHead[10] = 0; eyeToHead[11] = 0; eyeToHead[12] = 0; eyeToHead[13] = 10000;
}

static QString joinInts(const int* values, int count)
{
    QStringList parts;
    parts.reserve(count);
    for (int i = 0; i < count; i++) {
        parts.append(QString::number(values[i]));
    }
    return parts.join(',');
}

QString VrLaunchConfig::toQuery() const
{
    const qint64 period = periodNs > 0 ? periodNs
                                       : (refreshHz > 0 ? 1000000000LL / refreshHz : 0);
    return QStringLiteral("&vr=1") +
           "&vrEye=" + QString::number(eyeWidth) + "x" + QString::number(eyeHeight) +
           "&vrHz=" + QString::number(refreshHz) +
           "&vrPeriodNs=" + QString::number(period) +
           "&vrFov=" + joinInts(fovTan.data(), (int)fovTan.size()) +
           "&vrIpd=" + QString::number(ipd) +
           "&vrEyeToHead=" + joinInts(eyeToHead.data(), (int)eyeToHead.size()) +
           "&vrCaps=" + QString::number(caps, 16) +
           "&vrCodecs=" + QString::number(codecs) +
           "&vrCtrl=" + controller +
           "&vrOverscan=" + QString::number(overscan) +
           "&vrForce=" + QString::number(force ? 1 : 0);
}

VrSessionInfo VrSessionInfo::parse(const QString& text)
{
    VrSessionInfo info;
    const QStringList pairs = text.trimmed().split(';', Qt::SkipEmptyParts);
    for (const QString& pair : pairs) {
        const int eq = pair.indexOf('=');
        if (eq <= 0) {
            continue;
        }
        const QString key = pair.left(eq).trimmed();
        const QString value = pair.mid(eq + 1).trimmed();
        if (key == "proto") {
            info.proto = value.toInt();
        }
        else if (key == "packed") {
            const QStringList wh = value.split('x');
            if (wh.size() == 2) {
                info.packedWidth = wh[0].toInt();
                info.packedHeight = wh[1].toInt();
            }
        }
        else if (key == "hz") {
            info.refreshHz = value.toInt();
        }
        else if (key == "codec") {
            info.codec = value;
        }
        else if (key == "layout") {
            info.layout = value;
        }
        else if (key == "overscan") {
            info.overscan = value.toInt();
        }
        else if (key == "recovery") {
            info.recoveryIntra = (value == "intra");
        }
        else if (key == "irFrames") {
            info.irFrames = value.toInt();
        }
        else if (key == "transport") {
            info.transport = value;
        }
        else if (key == "universeId") {
            info.universeId = value;
        }
        else if (key == "session") {
            info.sessionGuid = value;
        }
        else if (key == "mode") {
            info.mode = value;
        }
        // 不認得的 key 直接略過（server 之後可以往後加欄位）
    }

    info.valid = info.proto >= VIPLE_VR_PROTO_VERSION &&
                 info.packedWidth > 0 && info.packedHeight > 0 &&
                 info.refreshHz > 0;
    return info;
}

QString VrSessionInfo::describe() const
{
    return QString("proto=%1 packed=%2x%3 hz=%4 codec=%5 layout=%6 overscan=%7 recovery=%8 "
                   "irFrames=%9 transport=%10 mode=%11 session=%12")
        .arg(proto).arg(packedWidth).arg(packedHeight).arg(refreshHz)
        .arg(codec, layout).arg(overscan)
        .arg(recoveryIntra ? "intra" : "idr").arg(irFrames)
        .arg(transport, mode, sessionGuid);
}
