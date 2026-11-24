package com.limelight.binding.input.capture;

import android.app.Activity;

import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.binding.input.evdev.EvdevListener;
import com.limelight.utils.QuestSupport;

public class InputCaptureManager {
    public static InputCaptureProvider getInputCaptureProvider(Activity activity, EvdevListener rootListener) {
        // §QUEST-2D：Touch 雷射是絕對指標。requestPointerCapture 之後系統可能改送相對事件，或乾脆不送。
        // 未在實機確認。連線後 Game 仍會 enableCapture，但這個 provider 不會去抓指標。
        // 非 Quest 不進這裡，下面仍是原本的 Android O+ native capture。
        if (QuestSupport.isQuest(activity)) {
            LimeLog.info("[VIPLE-QUEST-2D] using NullCaptureProvider (no requestPointerCapture)");
            return new NullCaptureProvider();
        }
        if (AndroidNativePointerCaptureProvider.isCaptureProviderSupported()) {
            LimeLog.info("Using Android O+ native mouse capture");
            return new AndroidNativePointerCaptureProvider(activity, activity.findViewById(R.id.surfaceView));
        }
        // VipleStream: root flavor removed — ROOT_BUILD branch simplified away.
        else if (ShieldCaptureProvider.isCaptureProviderSupported()) {
            LimeLog.info("Using NVIDIA mouse capture extension");
            return new ShieldCaptureProvider(activity);
        }
        else if (AndroidPointerIconCaptureProvider.isCaptureProviderSupported()) {
            // Android N's native capture can't capture over system UI elements
            // so we want to only use it if there's no other option.
            LimeLog.info("Using Android N+ pointer hiding");
            return new AndroidPointerIconCaptureProvider(activity, activity.findViewById(R.id.surfaceView));
        }
        else {
            LimeLog.info("Mouse capture not available");
            return new NullCaptureProvider();
        }
    }
}
