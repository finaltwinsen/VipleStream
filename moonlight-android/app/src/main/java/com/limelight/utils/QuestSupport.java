package com.limelight.utils;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

import com.limelight.LimeLog;

/**
 * §QUEST-2D：判斷是不是 Meta Quest（Horizon OS），好把 2D 面板專屬行為收在單一分支。
 * 製造商字串 {@code Oculus}／{@code Meta}（不分大小寫），或 {@code oculus.hardware.standalone_vr}。
 * {@code android.hardware.vr.headtracking} 是 AOSP 的 Daydream feature，非 Quest 也會宣告，
 * 拿來判斷會關掉滑鼠鎖定、自動橫向與子母畫面，所以只記進 log。
 * 製造商字串若和實機不符，側載後看 log {@code [VIPLE-QUEST-2D] detect} 的 manufacturer 欄位。
 */
public final class QuestSupport {
    private static final String FEATURE_STANDALONE_VR = "oculus.hardware.standalone_vr";
    private static final String FEATURE_HEADTRACKING = "android.hardware.vr.headtracking";

    private static volatile Boolean cached;

    private QuestSupport() {
    }

    public static boolean isQuest(Context context) {
        Boolean local = cached;
        if (local != null) {
            return local;
        }
        synchronized (QuestSupport.class) {
            if (cached != null) {
                return cached;
            }
            boolean quest = detect(context);
            // 沒有 Context 又還不能靠 feature 判斷時不快取，避免第一次 null 把結果鎖成 false
            if (context != null || quest) {
                cached = quest;
            }
            return quest;
        }
    }

    private static boolean detect(Context context) {
        String manufacturer = Build.MANUFACTURER;
        boolean manufacturerMatch = manufacturer != null
                && ("oculus".equalsIgnoreCase(manufacturer) || "meta".equalsIgnoreCase(manufacturer));
        boolean standaloneVr = false;
        boolean headtracking = false;
        if (context != null) {
            PackageManager packageManager = context.getPackageManager();
            if (packageManager != null) {
                standaloneVr = packageManager.hasSystemFeature(FEATURE_STANDALONE_VR);
                headtracking = packageManager.hasSystemFeature(FEATURE_HEADTRACKING);
            }
        }
        boolean quest = manufacturerMatch || standaloneVr;
        // 所有裝置只打一次，用來核對實機製造商字串；非 Quest 之後不會再進 Quest 分支
        LimeLog.info("[VIPLE-QUEST-2D] detect manufacturer=" + manufacturer
                + " manufacturerMatch=" + manufacturerMatch
                + " standaloneVr=" + standaloneVr
                + " headtracking=" + headtracking
                + " result=" + quest);
        if (headtracking && !quest) {
            // 實機上若這行出現，代表有 headtracking feature 但沒被當成 Quest。
            LimeLog.info("[VIPLE-QUEST-2D] android.hardware.vr.headtracking is present but ignored");
        }
        return quest;
    }
}
