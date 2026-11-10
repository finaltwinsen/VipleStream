#include "vrlaunchparams.h"

#include <Limelight.h>

#include <QStringList>

VrLaunchConfig::VrLaunchConfig()
{
    fovTan.fill(10000);  // tan 45° = 1.0
    caps = VIPLE_VR_CLIENT_CAP_RECOVERY_INTRA | VIPLE_VR_CLIENT_CAP_TRACK_THREAD;
#ifdef VIPLE_MPQUIC
    // §VR-MULTILINK：common-c 的多連線靠 PlatformNetIf 列舉網卡，只有 VIPLE_MPQUIC 建置有
    caps |= VIPLE_VR_CLIENT_CAP_MULTILINK;
#endif
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
    QString q = QStringLiteral("&vr=1") +
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
           "&vrForce=" + QString::number(force ? 1 : 0) +
           (fovea > 100 ? "&vrFovea=" + QString::number(fovea) : QString()) +
           // §VR-FOVEA-TILES：有指定大小時才帶（server 的 format_launch_params 同一個位置）
           (foveaTilesReq >= 0 ? "&vrFoveaTiles=" + QString::number(foveaTilesReq) : QString()) +
           // §VR-HALF-RATE：和 vrHz 不同才帶（其他情況字串和以前一樣；舊 server 不認得的 key 會略過）
           (displayHz > 0 && displayHz != refreshHz ? "&vrDispHz=" + QString::number(displayHz) : QString());

    // §VR-PLAYAREA：遊玩區已知時加 &vrPlayArea=<寬mm>x<深mm>
    if (playAreaWidthMm > 0 && playAreaDepthMm > 0) {
        q += "&vrPlayArea=" + QString::number(playAreaWidthMm) + "x" + QString::number(playAreaDepthMm);
    }
    // §VR-PLAYAREA：坐姿零點已知時加 &vrSeated=<x_mm>,<y_mm>,<z_mm>,<yaw 毫度>
    if (hasSeated) {
        q += "&vrSeated=" + QString::number(seatedPosMm[0]) + "," +
                            QString::number(seatedPosMm[1]) + "," +
                            QString::number(seatedPosMm[2]) + "," +
                            QString::number(seatedYawMdeg);
    }
    return q;
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
        else if (key == "fovea") {
            const int v = value.toInt();
            info.fovea = (v > 100 && v <= 250) ? v : 100;
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
        else if (key == "multilink") {
            info.multilink = (value == "1");
        }
        else if (key == "gazeFovea") {
            info.gazeFovea = (value == "1");
        }
        else if (key == "foveaInset") {
            const int v = value.toInt();
            info.foveaInsetU0 = (v >= 1000 && v <= 9000) ? v : 0;
        }
        else if (key == "foveaTiles") {
            const QStringList wh = value.split('x');
            if (wh.size() == 2) {
                info.tileW = wh[0].toInt();
                info.tileH = wh[1].toInt();
            }
        }
        else if (key == "foveaRender") {
            const QStringList wh = value.split('x');
            if (wh.size() == 2) {
                info.tileRenderW = wh[0].toInt();
                info.tileRenderH = wh[1].toInt();
            }
        }
        // 不認得的 key 直接略過（server 之後可以往後加欄位）
    }

    // §VR-FOVEA-TILES：兩個 key 要成對、尺寸關係要對（packed＝2×每塊、算圖比一塊大、位置放得進 u16），不然當成沒有分塊。
    // server 只在我們宣告 VIPLE_VR_CLIENT_CAP_FOVEA_TILES 時才會回，回了卻對不上＝畫面會錯，寧可整個不認
    if (info.tileW != 0 || info.tileH != 0 || info.tileRenderW != 0 || info.tileRenderH != 0) {
        const bool ok = info.tileW >= 64 && info.tileH >= 64 && info.tileRenderW > info.tileW && info.tileRenderH > info.tileH &&
                        info.tileRenderW <= 65535 && info.tileRenderH <= 65535 &&
                        info.packedWidth == 2 * info.tileW && info.packedHeight == 2 * info.tileH;
        if (!ok) {
            info.tileW = info.tileH = info.tileRenderW = info.tileRenderH = 0;
            info.tilesInvalid = true;
        }
    }

    info.valid = info.proto >= VIPLE_VR_PROTO_VERSION &&
                 info.packedWidth > 0 && info.packedHeight > 0 &&
                 info.refreshHz > 0 && !info.tilesInvalid;
    return info;
}

QString VrSessionInfo::describe() const
{
    return QString("proto=%1 packed=%2x%3 hz=%4 codec=%5 layout=%6 overscan=%7 recovery=%8 "
                   "irFrames=%9 transport=%10 mode=%11 session=%12%13%14%15%16")
        .arg(proto).arg(packedWidth).arg(packedHeight).arg(refreshHz)
        .arg(codec, layout).arg(overscan)
        .arg(recoveryIntra ? "intra" : "idr").arg(irFrames)
        .arg(transport, mode, sessionGuid)
        .arg(multilink ? " multilink=1" : "")
        .arg(fovea > 100 ? QString(" fovea=%1%2").arg(fovea).arg(foveaInsetU0 > 0 ? QString(" foveaInset=%1").arg(foveaInsetU0) : QString())
                         : QString())
        .arg(gazeFovea ? " gazeFovea=1" : "")
        .arg(tileW > 0 ? QString(" foveaTiles=%1x%2 foveaRender=%3x%4").arg(tileW).arg(tileH).arg(tileRenderW).arg(tileRenderH)
                       : QString());
}
