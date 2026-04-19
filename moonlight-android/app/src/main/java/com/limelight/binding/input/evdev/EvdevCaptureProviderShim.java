package com.limelight.binding.input.evdev;


import android.app.Activity;

import com.limelight.binding.input.capture.InputCaptureProvider;

// VipleStream: root flavor removed → evdev capture is never supported.
// Kept as a stub so existing callers (InputCaptureManager) compile.
public class EvdevCaptureProviderShim {
    public static boolean isCaptureProviderSupported() {
        return false;
    }

    public static InputCaptureProvider createEvdevCaptureProvider(Activity activity, EvdevListener listener) {
        throw new UnsupportedOperationException("Evdev capture removed with root flavor");
    }
}
