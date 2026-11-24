package com.limelight.nvstream.vr;

/**
 * §QUEST-VR：/launch 與 /resume 回應裡的 &lt;VipleStreamVRSession&gt;。
 * 解析與 vrFlags 對齊 moonlight-qt 的 VrSessionInfo::parse 與 session.cpp。
 * 純 Java，不碰 Android API。
 */
public class VrSessionInfo {
    /** VipleVr.h VIPLE_VR_PROTO_VERSION */
    public static final int PROTO_VERSION = 1;

    /** VipleVr.h VIPLE_VR_SF_*，寫進 STREAM_CONFIGURATION.vrFlags。 */
    public static final int SF_ENABLED = 0x01;
    public static final int SF_RECOVERY_INTRA = 0x02;
    public static final int SF_MULTILINK = 0x04;
    public static final int SF_MULTILINK_SELFTEST = 0x08;
    public static final int SF_REPAIR_RTT = 0x10;

    public boolean valid = false;
    public int proto = 0;
    public int packedWidth = 0;
    public int packedHeight = 0;
    public int refreshHz = 0;
    public String codec = "";
    public String layout = "";
    public int overscan = 0;
    public int fovea = 100;
    public int foveaInsetU0 = 0;
    public int tileW = 0;
    public int tileH = 0;
    public int tileRenderW = 0;
    public int tileRenderH = 0;
    public boolean tilesInvalid = false;
    public boolean recoveryIntra = false;
    public boolean multilink = false;
    public boolean gazeFovea = false;
    public int irFrames = 0;
    public String transport = "";
    public String universeId = "";
    public String sessionGuid = "";
    public String mode = "";

    /**
     * 空字串或 null 得到 valid=false。不認得的 key 略過。
     * 整數換算對齊 QString::toInt()：失敗當 0。
     */
    public static VrSessionInfo parse(String text) {
        VrSessionInfo info = new VrSessionInfo();
        if (text == null) {
            return info;
        }
        String trimmed = text.trim();
        if (trimmed.isEmpty()) {
            return info;
        }
        // Qt::SkipEmptyParts 只拿掉空段，不拿掉只有空白的段。
        String[] pairs = trimmed.split(";", -1);
        for (String pair : pairs) {
            if (pair.isEmpty()) {
                continue;
            }
            int eq = pair.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            String key = pair.substring(0, eq).trim();
            String value = pair.substring(eq + 1).trim();
            if (key.equals("proto")) {
                info.proto = toInt(value);
            }
            else if (key.equals("packed")) {
                String[] wh = value.split("x", -1);
                if (wh.length == 2) {
                    info.packedWidth = toInt(wh[0]);
                    info.packedHeight = toInt(wh[1]);
                }
            }
            else if (key.equals("hz")) {
                info.refreshHz = toInt(value);
            }
            else if (key.equals("codec")) {
                info.codec = value;
            }
            else if (key.equals("layout")) {
                info.layout = value;
            }
            else if (key.equals("overscan")) {
                info.overscan = toInt(value);
            }
            else if (key.equals("fovea")) {
                int v = toInt(value);
                info.fovea = (v > 100 && v <= 250) ? v : 100;
            }
            else if (key.equals("recovery")) {
                info.recoveryIntra = value.equals("intra");
            }
            else if (key.equals("irFrames")) {
                info.irFrames = toInt(value);
            }
            else if (key.equals("transport")) {
                info.transport = value;
            }
            else if (key.equals("universeId")) {
                info.universeId = value;
            }
            else if (key.equals("session")) {
                info.sessionGuid = value;
            }
            else if (key.equals("mode")) {
                info.mode = value;
            }
            else if (key.equals("multilink")) {
                info.multilink = value.equals("1");
            }
            else if (key.equals("gazeFovea")) {
                info.gazeFovea = value.equals("1");
            }
            else if (key.equals("foveaInset")) {
                int v = toInt(value);
                info.foveaInsetU0 = (v >= 1000 && v <= 9000) ? v : 0;
            }
            else if (key.equals("foveaTiles")) {
                String[] wh = value.split("x", -1);
                if (wh.length == 2) {
                    info.tileW = toInt(wh[0]);
                    info.tileH = toInt(wh[1]);
                }
            }
            else if (key.equals("foveaRender")) {
                String[] wh = value.split("x", -1);
                if (wh.length == 2) {
                    info.tileRenderW = toInt(wh[0]);
                    info.tileRenderH = toInt(wh[1]);
                }
            }
        }

        // 分塊兩個 key 不成對或尺寸對不上就整段不認，避免照錯的畫面去解。
        if (info.tileW != 0 || info.tileH != 0 || info.tileRenderW != 0 || info.tileRenderH != 0) {
            boolean ok = info.tileW >= 64 && info.tileH >= 64
                    && info.tileRenderW > info.tileW && info.tileRenderH > info.tileH
                    && info.tileRenderW <= 8192 && info.tileRenderH <= 8192
                    && info.packedWidth == 2 * info.tileW
                    && info.packedHeight == 2 * info.tileH;
            if (!ok) {
                info.tileW = 0;
                info.tileH = 0;
                info.tileRenderW = 0;
                info.tileRenderH = 0;
                info.tilesInvalid = true;
            }
        }

        info.valid = info.proto >= PROTO_VERSION
                && info.packedWidth > 0 && info.packedHeight > 0
                && info.refreshHz > 0 && !info.tilesInvalid;
        return info;
    }

    /**
     * Qt 預設：vrLinkSelftest=false、vrRepairRtt=true
     * （streamingpreferences.h）。selftest 與 repair 都要 server 回了 multilink=1 才進旗標。
     */
    public int toVrFlags() {
        return toVrFlags(false, true);
    }

    /**
     * @param linkSelftest 對應 StreamingPreferences::vrLinkSelftest
     * @param repairRtt 對應 StreamingPreferences::effectiveRepairRtt()
     */
    public int toVrFlags(boolean linkSelftest, boolean repairRtt) {
        if (!valid) {
            return 0;
        }
        int flags = SF_ENABLED;
        if (recoveryIntra) {
            flags |= SF_RECOVERY_INTRA;
        }
        if (multilink) {
            flags |= SF_MULTILINK;
            if (linkSelftest) {
                flags |= SF_MULTILINK_SELFTEST;
            }
            if (repairRtt) {
                flags |= SF_REPAIR_RTT;
            }
        }
        return flags;
    }

    private static int toInt(String value) {
        if (value == null) {
            return 0;
        }
        String s = value.trim();
        if (s.isEmpty()) {
            return 0;
        }
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
