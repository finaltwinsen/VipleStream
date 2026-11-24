package com.limelight.vr;

import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.nvstream.vr.VrLaunchConfig;
import com.limelight.preferences.PreferenceConfiguration;

/**
 * §QUEST-PCVR：與 {@code Game.java} 這幾處是同一套規則，修改時兩邊要一起改。
 * {@code applySharedVrPrefs} 關掉 HDR、補幀，並把強制 AV1／PyroWave 改回 AUTO；
 * 串流組態裡 {@code vrEmulate} 為真時拿掉的 codec 遮罩；
 * {@code vrDefaultBitrateKbps}；
 * 寫進 {@code VrLaunchConfig.codecs} 的 H.264／HEVC 位元。
 * 不改 {@code Game.java}，那邊仍是原本的複本。
 */
public final class VrStreamDefaults {
    private VrStreamDefaults() {
    }

    public static void applySessionLimits(PreferenceConfiguration pref) {
        pref.enableHdr = false;
        pref.enableFruc = false;
        if (pref.videoFormat == PreferenceConfiguration.FormatOption.FORCE_AV1
                || pref.videoFormat == PreferenceConfiguration.FormatOption.FORCE_PYROWAVE) {
            pref.videoFormat = PreferenceConfiguration.FormatOption.AUTO;
        }
    }

    /** {@code width}、{@code height} 是打包後的串流尺寸（2×單眼寬 × 單眼高），跟 {@code Game.vrDefaultBitrateKbps} 的入參一樣。 */
    public static int defaultBitrateKbps(int width, int height, int fps) {
        double pixRate = (double) width * height * fps;
        double ref = 3456.0 * 1728.0 * 90.0;
        int vrDefault = (int) Math.round(150000.0 * pixRate / ref);
        if (vrDefault < 10000) {
            vrDefault = 10000;
        } else if (vrDefault > 200000) {
            vrDefault = 200000;
        }
        return vrDefault;
    }

    public static int retainVrCodecs(int supportedVideoFormats, boolean hevcSupported,
                                      PreferenceConfiguration.FormatOption videoFormat) {
        int vrStrip = MoonBridge.VIDEO_FORMAT_MASK_AV1
                | MoonBridge.VIDEO_FORMAT_MASK_10BIT
                | MoonBridge.VIDEO_FORMAT_MASK_YUV444
                | MoonBridge.VIDEO_FORMAT_MASK_PYROWAVE;
        if (!hevcSupported || videoFormat == PreferenceConfiguration.FormatOption.FORCE_H264) {
            vrStrip |= MoonBridge.VIDEO_FORMAT_MASK_H265;
        }
        return supportedVideoFormats & ~vrStrip;
    }

    public static int launchCodecMask(int supportedVideoFormats) {
        int codecs = VrLaunchConfig.CODEC_H264;
        if ((supportedVideoFormats & MoonBridge.VIDEO_FORMAT_H265) != 0) {
            codecs |= VrLaunchConfig.CODEC_HEVC;
        }
        return codecs;
    }
}
