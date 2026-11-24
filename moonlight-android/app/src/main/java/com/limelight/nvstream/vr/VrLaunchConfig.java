package com.limelight.nvstream.vr;

/**
 * §QUEST-VR：/launch 的 VR 參數。欄位與換算對齊
 * moonlight-qt/app/streaming/vr/vrlaunchparams.h、vrlaunchparams.cpp。
 * 純 Java，給之後的 JVM 測試用，不碰 Android API。
 */
public class VrLaunchConfig {
    /** VipleVr.h VIPLE_VR_CLIENT_CAP_* */
    public static final int CAP_RECOVERY_INTRA = 0x01;
    public static final int CAP_TRACK_THREAD = 0x02;
    public static final int CAP_GAZE = 0x04;
    public static final int CAP_SKELETON = 0x08;
    public static final int CAP_MULTILINK = 0x10;
    public static final int CAP_GAZE_FOVEA = 0x20;
    public static final int CAP_FOVEA_INSET = 0x40;
    public static final int CAP_FOVEA_TILES = 0x80;

    /** VipleVr.h VIPLE_VR_CODEC_* */
    public static final int CODEC_H264 = 0x01;
    public static final int CODEC_HEVC = 0x02;
    public static final int CODEC_AV1 = 0x04;
    public static final int CODEC_PYROWAVE = 0x08;

    public int eyeWidth = 1728;
    public int eyeHeight = 1728;
    public int refreshHz = 90;
    /** 0：由 refreshHz 用整數除法推算。 */
    public long periodNs = 0;
    /** tan×10000：左眼 left,right,up,down，右眼同順序。 */
    public int[] fovTan = new int[8];
    /** mm×100 */
    public int ipd = 6300;
    /** 每眼 pos(mm×100)×3＋quat x,y,z,w(×10000)×4，左眼在前。 */
    public int[] eyeToHead = new int[14];
    public int caps = 0;
    public int codecs = 0;
    public String controller = "touch";
    /** deg×10 */
    public int overscan = 0;
    /** 正前方像素密度是均勻取樣的幾 %。100＝不帶 vrFovea。 */
    public int fovea = 100;
    /** 0～3 才帶 vrFoveaTiles。-1＝不帶。 */
    public int foveaTilesReq = -1;
    /** 頭盔顯示 Hz。0 或等於 refreshHz＝不帶 vrDispHz。 */
    public int displayHz = 0;
    public boolean force = false;

    public int playAreaWidthMm = 0;
    public int playAreaDepthMm = 0;
    public boolean hasSeated = false;
    public int[] seatedPosMm = new int[3];
    /** 繞 +Y 的 yaw，毫度。 */
    public int seatedYawMdeg = 0;

    public VrLaunchConfig() {
        for (int i = 0; i < fovTan.length; i++) {
            fovTan[i] = 10000;
        }
        // §QUEST-VR：MULTILINK 對應 vrlaunchparams.cpp 在 VIPLE_MPQUIC 有定義時才或上的那一位。
        // 這個 Android 樹的 Android.mk 在有 picoquic 預編譯庫時會定義 VIPLE_MPQUIC。
        // 沒有那個庫的建置，呼叫端要把 CAP_MULTILINK 清掉，不然 vrCaps 會和該建置的 Qt 預設不同。
        caps = CAP_RECOVERY_INTRA | CAP_TRACK_THREAD | CAP_MULTILINK;
        codecs = CODEC_HEVC | CODEC_H264;
        setIpd(ipd);
    }

    /** 以 IPD 重算兩眼的水平位置，旋轉維持單位四元數。 */
    public void setIpd(int ipdMmX100) {
        ipd = ipdMmX100;
        int half = ipdMmX100 / 2;
        eyeToHead[0] = -half;
        eyeToHead[1] = 0;
        eyeToHead[2] = 0;
        eyeToHead[3] = 0;
        eyeToHead[4] = 0;
        eyeToHead[5] = 0;
        eyeToHead[6] = 10000;
        eyeToHead[7] = half;
        eyeToHead[8] = 0;
        eyeToHead[9] = 0;
        eyeToHead[10] = 0;
        eyeToHead[11] = 0;
        eyeToHead[12] = 0;
        eyeToHead[13] = 10000;
    }

    /**
     * 接在 /launch query 尾端的字串，開頭含 '&'。
     * 參數順序與選填條件對齊 VrLaunchConfig::toQuery()。
     */
    public String toQuery() {
        long period = periodNs > 0 ? periodNs : (refreshHz > 0 ? 1000000000L / refreshHz : 0);
        StringBuilder q = new StringBuilder();
        q.append("&vr=1");
        q.append("&vrEye=").append(eyeWidth).append('x').append(eyeHeight);
        q.append("&vrHz=").append(refreshHz);
        q.append("&vrPeriodNs=").append(period);
        q.append("&vrFov=").append(joinInts(fovTan));
        q.append("&vrIpd=").append(ipd);
        q.append("&vrEyeToHead=").append(joinInts(eyeToHead));
        q.append("&vrCaps=").append(Integer.toHexString(caps));
        q.append("&vrCodecs=").append(codecs);
        // controller 為 null 時不印出 "null" 這個字，Qt 的 QString 不會是 null。
        q.append("&vrCtrl=").append(controller == null ? "" : controller);
        q.append("&vrOverscan=").append(overscan);
        q.append("&vrForce=").append(force ? 1 : 0);
        if (fovea > 100) {
            q.append("&vrFovea=").append(fovea);
        }
        if (foveaTilesReq >= 0) {
            q.append("&vrFoveaTiles=").append(foveaTilesReq);
        }
        if (displayHz > 0 && displayHz != refreshHz) {
            q.append("&vrDispHz=").append(displayHz);
        }
        if (playAreaWidthMm > 0 && playAreaDepthMm > 0) {
            q.append("&vrPlayArea=").append(playAreaWidthMm).append('x').append(playAreaDepthMm);
        }
        if (hasSeated) {
            q.append("&vrSeated=")
                    .append(seatedPosMm[0]).append(',')
                    .append(seatedPosMm[1]).append(',')
                    .append(seatedPosMm[2]).append(',')
                    .append(seatedYawMdeg);
        }
        return q.toString();
    }

    private static String joinInts(int[] values) {
        if (values == null || values.length == 0) {
            return "";
        }
        StringBuilder parts = new StringBuilder();
        for (int i = 0; i < values.length; i++) {
            if (i > 0) {
                parts.append(',');
            }
            parts.append(values[i]);
        }
        return parts.toString();
    }
}
