package com.limelight.preferences;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.media.MediaCodecInfo;
import android.os.Build;
import android.os.Bundle;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.os.Handler;
import android.os.Vibrator;
import android.preference.CheckBoxPreference;
import android.preference.ListPreference;
import android.preference.Preference;
import android.preference.PreferenceCategory;
import android.preference.PreferenceFragment;
import android.preference.PreferenceManager;
import android.preference.PreferenceScreen;
import android.util.DisplayMetrics;
import android.util.Range;
import android.util.TypedValue;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;

import com.limelight.LimeLog;
import com.limelight.PcView;
import com.limelight.R;
import com.limelight.glasses.GlassesDiagnosticsActivity;
import com.limelight.binding.video.MediaCodecHelper;
import com.limelight.utils.Dialog;
import com.limelight.utils.LogDrop;
import com.limelight.utils.UiHelper;

import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.lang.reflect.Method;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

public class StreamSettings extends Activity {
    private PreferenceConfiguration previousPrefs;
    private int previousDisplayPixelCount;

    // HACK for Android 9
    static DisplayCutout displayCutoutP;

    void reloadSettings() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display.Mode mode = getWindowManager().getDefaultDisplay().getMode();
            previousDisplayPixelCount = mode.getPhysicalWidth() * mode.getPhysicalHeight();
        }
        getFragmentManager().beginTransaction().replace(
                R.id.stream_settings, new SettingsFragment()
        ).commitAllowingStateLoss();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        previousPrefs = PreferenceConfiguration.readPreferences(this);

        UiHelper.setLocale(this);

        setContentView(R.layout.activity_stream_settings);

        UiHelper.notifyNewRootView(this);
    }

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();

        // We have to use this hack on Android 9 because we don't have Display.getCutout()
        // which was added in Android 10.
        if (Build.VERSION.SDK_INT == Build.VERSION_CODES.P) {
            // Insets can be null when the activity is recreated on screen rotation
            // https://stackoverflow.com/questions/61241255/windowinsets-getdisplaycutout-is-null-everywhere-except-within-onattachedtowindo
            WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
            if (insets != null) {
                displayCutoutP = insets.getDisplayCutout();
            }
        }

        reloadSettings();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display.Mode mode = getWindowManager().getDefaultDisplay().getMode();

            // If the display's physical pixel count has changed, we consider that it's a new display
            // and we should reload our settings (which include display-dependent values).
            //
            // NB: We aren't using displayId here because that stays the same (DEFAULT_DISPLAY) when
            // switching between screens on a foldable device.
            if (mode.getPhysicalWidth() * mode.getPhysicalHeight() != previousDisplayPixelCount) {
                reloadSettings();
            }
        }
    }

    @Override
    // NOTE: This will NOT be called on Android 13+ with android:enableOnBackInvokedCallback="true"
    public void onBackPressed() {
        finish();

        // Language changes are handled via configuration changes in Android 13+,
        // so manual activity relaunching is no longer required.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            PreferenceConfiguration newPrefs = PreferenceConfiguration.readPreferences(this);
            if (!newPrefs.language.equals(previousPrefs.language)) {
                // Restart the PC view to apply UI changes
                Intent intent = new Intent(this, PcView.class);
                intent.setFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
                startActivity(intent, null);
            }
        }
    }

    public static class SettingsFragment extends PreferenceFragment {
        private int nativeResolutionStartIndex = Integer.MAX_VALUE;
        private boolean nativeFramerateShown = false;
        private volatile boolean logDropUploading;
        private android.app.Dialog logDropDialog;

        private void setValue(String preferenceKey, String value) {
            ListPreference pref = (ListPreference) findPreference(preferenceKey);

            pref.setValue(value);
        }

        private void appendPreferenceEntry(ListPreference pref, String newEntryName, String newEntryValue) {
            CharSequence[] newEntries = Arrays.copyOf(pref.getEntries(), pref.getEntries().length + 1);
            CharSequence[] newValues = Arrays.copyOf(pref.getEntryValues(), pref.getEntryValues().length + 1);

            // Add the new option
            newEntries[newEntries.length - 1] = newEntryName;
            newValues[newValues.length - 1] = newEntryValue;

            pref.setEntries(newEntries);
            pref.setEntryValues(newValues);
        }

        private void addNativeResolutionEntry(int nativeWidth, int nativeHeight, boolean insetsRemoved, boolean portrait) {
            ListPreference pref = (ListPreference) findPreference(PreferenceConfiguration.RESOLUTION_PREF_STRING);

            String newName;

            if (insetsRemoved) {
                newName = getResources().getString(R.string.resolution_prefix_native_fullscreen);
            }
            else {
                newName = getResources().getString(R.string.resolution_prefix_native);
            }

            if (PreferenceConfiguration.isSquarishScreen(nativeWidth, nativeHeight)) {
                if (portrait) {
                    newName += " " + getResources().getString(R.string.resolution_prefix_native_portrait);
                }
                else {
                    newName += " " + getResources().getString(R.string.resolution_prefix_native_landscape);
                }
            }

            newName += " ("+nativeWidth+"x"+nativeHeight+")";

            String newValue = nativeWidth+"x"+nativeHeight;

            // Check if the native resolution is already present
            for (CharSequence value : pref.getEntryValues()) {
                if (newValue.equals(value.toString())) {
                    // It is present in the default list, so don't add it again
                    return;
                }
            }

            if (pref.getEntryValues().length < nativeResolutionStartIndex) {
                nativeResolutionStartIndex = pref.getEntryValues().length;
            }
            appendPreferenceEntry(pref, newName, newValue);
        }

        private void addNativeResolutionEntries(int nativeWidth, int nativeHeight, boolean insetsRemoved) {
            if (PreferenceConfiguration.isSquarishScreen(nativeWidth, nativeHeight)) {
                addNativeResolutionEntry(nativeHeight, nativeWidth, insetsRemoved, true);
            }
            addNativeResolutionEntry(nativeWidth, nativeHeight, insetsRemoved, false);
        }

        private void addNativeFrameRateEntry(float framerate) {
            int frameRateRounded = Math.round(framerate);
            if (frameRateRounded == 0) {
                return;
            }

            ListPreference pref = (ListPreference) findPreference(PreferenceConfiguration.FPS_PREF_STRING);
            String fpsValue = Integer.toString(frameRateRounded);
            String fpsName = getResources().getString(R.string.resolution_prefix_native) +
                    " (" + fpsValue + " " + getResources().getString(R.string.fps_suffix_fps) + ")";

            // Check if the native frame rate is already present
            for (CharSequence value : pref.getEntryValues()) {
                if (fpsValue.equals(value.toString())) {
                    // It is present in the default list, so don't add it again
                    nativeFramerateShown = false;
                    return;
                }
            }

            appendPreferenceEntry(pref, fpsName, fpsValue);
            nativeFramerateShown = true;
        }

        private void removeValue(String preferenceKey, String value, Runnable onMatched) {
            int matchingCount = 0;

            ListPreference pref = (ListPreference) findPreference(preferenceKey);

            // Count the number of matching entries we'll be removing
            for (CharSequence seq : pref.getEntryValues()) {
                if (seq.toString().equalsIgnoreCase(value)) {
                    matchingCount++;
                }
            }

            // Create the new arrays
            CharSequence[] entries = new CharSequence[pref.getEntries().length-matchingCount];
            CharSequence[] entryValues = new CharSequence[pref.getEntryValues().length-matchingCount];
            int outIndex = 0;
            for (int i = 0; i < pref.getEntryValues().length; i++) {
                if (pref.getEntryValues()[i].toString().equalsIgnoreCase(value)) {
                    // Skip matching values
                    continue;
                }

                entries[outIndex] = pref.getEntries()[i];
                entryValues[outIndex] = pref.getEntryValues()[i];
                outIndex++;
            }

            if (pref.getValue().equalsIgnoreCase(value)) {
                onMatched.run();
            }

            // Update the preference with the new list
            pref.setEntries(entries);
            pref.setEntryValues(entryValues);
        }

        private void resetBitrateToDefault(SharedPreferences prefs, String res, String fps) {
            if (res == null) {
                res = prefs.getString(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.DEFAULT_RESOLUTION);
            }
            if (PreferenceConfiguration.RES_CUSTOM.equalsIgnoreCase(res)) {
                // §REVIEW-1104：選取 custom 自訂解析度時改讀 text_custom_resolution，解析失敗由 getDefaultBitrate 退回預設值
                res = prefs.getString(PreferenceConfiguration.CUSTOM_RESOLUTION_PREF_STRING, PreferenceConfiguration.DEFAULT_RESOLUTION);
            }
            if (fps == null) {
                fps = prefs.getString(PreferenceConfiguration.FPS_PREF_STRING, PreferenceConfiguration.DEFAULT_FPS);
            }

            prefs.edit()
                    .putInt(PreferenceConfiguration.BITRATE_PREF_STRING,
                            PreferenceConfiguration.getDefaultBitrate(res, fps))
                    .apply();
        }

        @Override
        public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle savedInstanceState) {
            View view = super.onCreateView(inflater, container, savedInstanceState);
            UiHelper.applyStatusBarPadding(view);
            return view;
        }

        @Override
        public void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);

            addPreferencesFromResource(R.xml.preferences);
            // §VITURE：XML intent 會寫死 applicationId，debug 後綴對不上，所以用明確 Intent 開診斷畫面。
            Preference glassesDiagnostics = findPreference("glasses_diagnostics");
            if (glassesDiagnostics != null) {
                glassesDiagnostics.setOnPreferenceClickListener(new Preference.OnPreferenceClickListener() {
                    @Override
                    public boolean onPreferenceClick(Preference preference) {
                        Activity activity = getActivity();
                        if (activity == null) {
                            return false;
                        }
                        activity.startActivity(new Intent(activity, GlassesDiagnosticsActivity.class));
                        return true;
                    }
                });
            }
            // Android TV 沒有這台裝置的螢幕可當觸控板，沒有 USB host 也接不了眼鏡。跟手把 USB 選項一樣直接拿掉。
            if (getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK)
                    || !getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_USB_HOST)) {
                PreferenceCategory basic =
                        (PreferenceCategory) findPreference("category_basic_settings");
                basic.removePreference(findPreference("list_external_display_mode"));
                basic.removePreference(findPreference("checkbox_external_display_dim"));
                basic.removePreference(findPreference("checkbox_glasses_timewarp"));
                basic.removePreference(findPreference("glasses_diagnostics"));
            }
            PreferenceScreen screen = getPreferenceScreen();

            // hide on-screen controls category on non touch screen devices
            if (!getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN)) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_onscreen_controls");
                screen.removePreference(category);
            }

            // Hide remote desktop mouse mode on pre-Oreo (which doesn't have pointer capture)
            // and NVIDIA SHIELD devices (which support raw mouse input in pointer capture mode)
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                    getActivity().getPackageManager().hasSystemFeature("com.nvidia.feature.shield")) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_input_settings");
                category.removePreference(findPreference("checkbox_absolute_mouse_mode"));
            }

            // Hide gamepad motion sensor option when running on OSes before Android 12.
            // Support for motion, LED, battery, and other extensions were introduced in S.
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_gamepad_settings");
                category.removePreference(findPreference("checkbox_gamepad_motion_sensors"));
            }

            // Hide gamepad motion sensor fallback option if the device has no gyro or accelerometer
            if (!getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_SENSOR_ACCELEROMETER) &&
                    !getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_SENSOR_GYROSCOPE)) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_gamepad_settings");
                category.removePreference(findPreference("checkbox_gamepad_motion_fallback"));
            }

            // Hide USB driver options on devices without USB host support
            if (!getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_USB_HOST)) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_gamepad_settings");
                category.removePreference(findPreference("checkbox_usb_bind_all"));
                category.removePreference(findPreference("checkbox_usb_driver"));
            }

            // Remove PiP mode on devices pre-Oreo, where the feature is not available (some low RAM devices),
            // and on Fire OS where it violates the Amazon App Store guidelines for some reason.
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                    !getActivity().getPackageManager().hasSystemFeature("android.software.picture_in_picture") ||
                    getActivity().getPackageManager().hasSystemFeature("com.amazon.software.fireos")) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_ui_settings");
                category.removePreference(findPreference("checkbox_enable_pip"));
            }

            // Fire TV apps are not allowed to use WebViews or browsers, so hide the Help category
            /*if (getActivity().getPackageManager().hasSystemFeature("amazon.hardware.fire_tv")) {
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_help");
                screen.removePreference(category);
            }*/
            PreferenceCategory category_gamepad_settings =
                    (PreferenceCategory) findPreference("category_gamepad_settings");
            // Remove the vibration options if the device can't vibrate
            if (!((Vibrator)getActivity().getSystemService(Context.VIBRATOR_SERVICE)).hasVibrator()) {
                category_gamepad_settings.removePreference(findPreference("checkbox_vibrate_fallback"));
                category_gamepad_settings.removePreference(findPreference("seekbar_vibrate_fallback_strength"));
                // The entire OSC category may have already been removed by the touchscreen check above
                PreferenceCategory category = (PreferenceCategory) findPreference("category_onscreen_controls");
                if (category != null) {
                    category.removePreference(findPreference("checkbox_vibrate_osc"));
                }
            }
            else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                    !((Vibrator)getActivity().getSystemService(Context.VIBRATOR_SERVICE)).hasAmplitudeControl() ) {
                // Remove the vibration strength selector of the device doesn't have amplitude control
                category_gamepad_settings.removePreference(findPreference("seekbar_vibrate_fallback_strength"));
            }

            Display display = getActivity().getWindowManager().getDefaultDisplay();
            float maxSupportedFps = display.getRefreshRate();

            // Hide non-supported resolution/FPS combinations
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                int maxSupportedResW = 0;

                // Add a native resolution with any insets included for users that don't want content
                // behind the notch of their display
                boolean hasInsets = false;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    DisplayCutout cutout;

                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                        // Use the much nicer Display.getCutout() API on Android 10+
                        cutout = display.getCutout();
                    }
                    else {
                        // Android 9 only
                        cutout = displayCutoutP;
                    }

                    if (cutout != null) {
                        int widthInsets = cutout.getSafeInsetLeft() + cutout.getSafeInsetRight();
                        int heightInsets = cutout.getSafeInsetBottom() + cutout.getSafeInsetTop();

                        if (widthInsets != 0 || heightInsets != 0) {
                            DisplayMetrics metrics = new DisplayMetrics();
                            display.getRealMetrics(metrics);

                            int width = Math.max(metrics.widthPixels - widthInsets, metrics.heightPixels - heightInsets);
                            int height = Math.min(metrics.widthPixels - widthInsets, metrics.heightPixels - heightInsets);

                            addNativeResolutionEntries(width, height, false);
                            hasInsets = true;
                        }
                    }
                }

                // Always allow resolutions that are smaller or equal to the active
                // display resolution because decoders can report total non-sense to us.
                // For example, a p201 device reports:
                // AVC Decoder: OMX.amlogic.avc.decoder.awesome
                // HEVC Decoder: OMX.amlogic.hevc.decoder.awesome
                // AVC supported width range: 64 - 384
                // HEVC supported width range: 64 - 544
                for (Display.Mode candidate : display.getSupportedModes()) {
                    // Some devices report their dimensions in the portrait orientation
                    // where height > width. Normalize these to the conventional width > height
                    // arrangement before we process them.

                    int width = Math.max(candidate.getPhysicalWidth(), candidate.getPhysicalHeight());
                    int height = Math.min(candidate.getPhysicalWidth(), candidate.getPhysicalHeight());

                    // Some TVs report strange values here, so let's avoid native resolutions on a TV
                    // unless they report greater than 4K resolutions.
                    if (!getActivity().getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEVISION) ||
                            (width > 3840 || height > 2160)) {
                        addNativeResolutionEntries(width, height, hasInsets);
                    }

                    if ((width >= 3840 || height >= 2160) && maxSupportedResW < 3840) {
                        maxSupportedResW = 3840;
                    }
                    else if ((width >= 2560 || height >= 1440) && maxSupportedResW < 2560) {
                        maxSupportedResW = 2560;
                    }
                    else if ((width >= 1920 || height >= 1080) && maxSupportedResW < 1920) {
                        maxSupportedResW = 1920;
                    }

                    if (candidate.getRefreshRate() > maxSupportedFps) {
                        maxSupportedFps = candidate.getRefreshRate();
                    }
                }

                // This must be called to do runtime initialization before calling functions that evaluate
                // decoder lists.
                MediaCodecHelper.initialize(getContext(), GlPreferences.readPreferences(getContext()).glRenderer);

                MediaCodecInfo avcDecoder = MediaCodecHelper.findProbableSafeDecoder("video/avc", -1);
                MediaCodecInfo hevcDecoder = MediaCodecHelper.findProbableSafeDecoder("video/hevc", -1);

                if (avcDecoder != null) {
                    try {
                    Range<Integer> avcWidthRange = avcDecoder.getCapabilitiesForType("video/avc").getVideoCapabilities().getSupportedWidths();

                    LimeLog.info("AVC supported width range: "+avcWidthRange.getLower()+" - "+avcWidthRange.getUpper());

                    // If 720p is not reported as supported, ignore all results from this API
                    if (avcWidthRange.contains(1280)) {
                        if (avcWidthRange.contains(3840) && maxSupportedResW < 3840) {
                            maxSupportedResW = 3840;
                        }
                        else if (avcWidthRange.contains(1920) && maxSupportedResW < 1920) {
                            maxSupportedResW = 1920;
                        }
                        else if (maxSupportedResW < 1280) {
                            maxSupportedResW = 1280;
                        }
                    } /*
                            maxSupportedResW = 1920;
                        }
                        else if (maxSupportedResW < 1280) {
                            maxSupportedResW = 1280;
                        }
                    }
                }

                */ } catch (Exception e) {
                    // §REVIEW-1104：部分解碼器呼叫 getCapabilitiesForType 會拋出 IllegalArgumentException，捕捉並跳過該解碼器
                    LimeLog.warning("Failed to get AVC decoder capabilities: " + e.getMessage());
                } }

                if (hevcDecoder != null) {
                    try {
                    Range<Integer> hevcWidthRange = hevcDecoder.getCapabilitiesForType("video/hevc").getVideoCapabilities().getSupportedWidths();

                    LimeLog.info("HEVC supported width range: "+hevcWidthRange.getLower()+" - "+hevcWidthRange.getUpper());

                    // If 720p is not reported as supported, ignore all results from this API
                    if (hevcWidthRange.contains(1280)) {
                        if (hevcWidthRange.contains(3840) && maxSupportedResW < 3840) {
                            maxSupportedResW = 3840;
                        }
                        else if (hevcWidthRange.contains(1920) && maxSupportedResW < 1920) {
                            maxSupportedResW = 1920;
                        }
                        else if (maxSupportedResW < 1280) {
                            maxSupportedResW = 1280;
                        }
                    } /*
                            maxSupportedResW = 1920;
                        }
                        else if (maxSupportedResW < 1280) {
                            maxSupportedResW = 1280;
                        }
                    }
                }

                */ } catch (Exception e) {
                    // §REVIEW-1104：部分解碼器呼叫 getCapabilitiesForType 會拋出 IllegalArgumentException，捕捉並跳過該解碼器
                    LimeLog.warning("Failed to get HEVC decoder capabilities: " + e.getMessage());
                } }

                LimeLog.info("Maximum resolution slot: "+maxSupportedResW);

                if (maxSupportedResW != 0) {
                    if (maxSupportedResW < 3840) {
                        // 4K is unsupported
                        removeValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_4K, new Runnable() {
                            @Override
                            public void run() {
                                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                                setValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_1440P);
                                resetBitrateToDefault(prefs, null, null);
                            }
                        });
                    }
                    if (maxSupportedResW < 2560) {
                        // 1440p is unsupported
                        removeValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_1440P, new Runnable() {
                            @Override
                            public void run() {
                                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                                setValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_1080P);
                                resetBitrateToDefault(prefs, null, null);
                            }
                        });
                    }
                    if (maxSupportedResW < 1920) {
                        // 1080p is unsupported
                        removeValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_1080P, new Runnable() {
                            @Override
                            public void run() {
                                SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                                setValue(PreferenceConfiguration.RESOLUTION_PREF_STRING, PreferenceConfiguration.RES_720P);
                                resetBitrateToDefault(prefs, null, null);
                            }
                        });
                    }
                    // Never remove 720p
                }
            }
            else {
                // We can get the true metrics via the getRealMetrics() function (unlike the lies
                // that getWidth() and getHeight() tell to us).
                DisplayMetrics metrics = new DisplayMetrics();
                display.getRealMetrics(metrics);
                int width = Math.max(metrics.widthPixels, metrics.heightPixels);
                int height = Math.min(metrics.widthPixels, metrics.heightPixels);
                addNativeResolutionEntries(width, height, false);
            }

            if (!PreferenceConfiguration.readPreferences(this.getActivity()).unlockFps) {
                // We give some extra room in case the FPS is rounded down
                if (maxSupportedFps < 118) {
                    removeValue(PreferenceConfiguration.FPS_PREF_STRING, "120", new Runnable() {
                        @Override
                        public void run() {
                            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                            setValue(PreferenceConfiguration.FPS_PREF_STRING, "90");
                            resetBitrateToDefault(prefs, null, null);
                        }
                    });
                }
                if (maxSupportedFps < 88) {
                    // 1080p is unsupported
                    removeValue(PreferenceConfiguration.FPS_PREF_STRING, "90", new Runnable() {
                        @Override
                        public void run() {
                            SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                            setValue(PreferenceConfiguration.FPS_PREF_STRING, "60");
                            resetBitrateToDefault(prefs, null, null);
                        }
                    });
                }
                // Never remove 30 FPS or 60 FPS
            }
            addNativeFrameRateEntry(maxSupportedFps);

            // Android L introduces the drop duplicate behavior of releaseOutputBuffer()
            // that the unlock FPS option relies on to not massively increase latency.
            findPreference(PreferenceConfiguration.UNLOCK_FPS_STRING).setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    // HACK: We need to let the preference change succeed before reinitializing to ensure
                    // it's reflected in the new layout.
                    final Handler h = new Handler();
                    h.postDelayed(new Runnable() {
                        @Override
                        public void run() {
                            // Ensure the activity is still open when this timeout expires
                            StreamSettings settingsActivity = (StreamSettings) SettingsFragment.this.getActivity();
                            if (settingsActivity != null) {
                                settingsActivity.reloadSettings();
                            }
                        }
                    }, 500);

                    // Allow the original preference change to take place
                    return true;
                }
            });

            // Remove HDR preference for devices below Nougat
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
                LimeLog.info("Excluding HDR toggle based on OS");
                PreferenceCategory category =
                        (PreferenceCategory) findPreference("category_advanced_settings");
                category.removePreference(findPreference("checkbox_enable_hdr"));
            }
            else {
                Display.HdrCapabilities hdrCaps = display.getHdrCapabilities();

                // We must now ensure our display is compatible with HDR10
                boolean foundHdr10 = false;
                if (hdrCaps != null) {
                    // getHdrCapabilities() returns null on Lenovo Lenovo Mirage Solo (vega), Android 8.0
                    for (int hdrType : hdrCaps.getSupportedHdrTypes()) {
                        if (hdrType == Display.HdrCapabilities.HDR_TYPE_HDR10) {
                            foundHdr10 = true;
                            break;
                        }
                    }
                }

                if (!foundHdr10) {
                    LimeLog.info("Excluding HDR toggle based on display capabilities");
                    PreferenceCategory category =
                            (PreferenceCategory) findPreference("category_advanced_settings");
                    category.removePreference(findPreference("checkbox_enable_hdr"));
                }
                else if (PreferenceConfiguration.isShieldAtvFirmwareWithBrokenHdr()) {
                    LimeLog.info("Disabling HDR toggle on old broken SHIELD TV firmware");
                    PreferenceCategory category =
                            (PreferenceCategory) findPreference("category_advanced_settings");
                    CheckBoxPreference hdrPref = (CheckBoxPreference) category.findPreference("checkbox_enable_hdr");
                    hdrPref.setEnabled(false);
                    hdrPref.setChecked(false);
                    hdrPref.setSummary("Update the firmware on your NVIDIA SHIELD Android TV to enable HDR");
                }
            }

            // Add a listener to the FPS and resolution preference
            // so the bitrate can be auto-adjusted
            findPreference(PreferenceConfiguration.RESOLUTION_PREF_STRING).setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                    String valueStr = (String) newValue;

                    // Detect if this value is the native resolution option
                    CharSequence[] values = ((ListPreference)preference).getEntryValues();
                    boolean isNativeRes = true;
                    for (int i = 0; i < values.length; i++) {
                        // Look for a match prior to the start of the native resolution entries
                        if (valueStr.equals(values[i].toString()) && i < nativeResolutionStartIndex) {
                            isNativeRes = false;
                            break;
                        }
                    }

                    // If this is native resolution, show the warning dialog
                    if (isNativeRes) {
                        Dialog.displayDialog(getActivity(),
                                getResources().getString(R.string.title_native_res_dialog),
                                getResources().getString(R.string.text_native_res_dialog),
                                false);
                    }

                    // Write the new bitrate value
                    resetBitrateToDefault(prefs, valueStr, null);

                    // Allow the original preference change to take place
                    return true;
                }
            });
            findPreference(PreferenceConfiguration.FPS_PREF_STRING).setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    SharedPreferences prefs = PreferenceManager.getDefaultSharedPreferences(SettingsFragment.this.getActivity());
                    String valueStr = (String) newValue;

                    // If this is native frame rate, show the warning dialog
                    CharSequence[] values = ((ListPreference)preference).getEntryValues();
                    if (nativeFramerateShown && values[values.length - 1].toString().equals(newValue.toString())) {
                        Dialog.displayDialog(getActivity(),
                                getResources().getString(R.string.title_native_fps_dialog),
                                getResources().getString(R.string.text_native_res_dialog),
                                false);
                    }

                    // Write the new bitrate value
                    resetBitrateToDefault(prefs, null, valueStr);

                    // Allow the original preference change to take place
                    return true;
                }
            });

            // 放在最後：上面的程式還會用 findPreference 找進階區的選項（例如 unlock FPS），先移除就找不到了
            setupAdvancedToggle();
            setupLogDrop();
        }

        // §SETTINGS-ADV：「顯示進階與實驗性選項」關著時，進階區收起這些測試、除錯與實驗性的選項。
        // 只是不顯示：值照樣存在 SharedPreferences、照樣套用。
        private static final String[] ADVANCED_ONLY_KEYS = {
                "checkbox_unlock_fps",
                "list_fruc_quality",
                "checkbox_fruc_backend_vulkan",
                "checkbox_full_range",
                "checkbox_enable_yuv444",
                "seekbar_packet_size",
                "list_mpquic_scheduler",
        };
        private final ArrayList<Preference> advancedOnlyPrefs = new ArrayList<>();

        private void setupAdvancedToggle() {
            final PreferenceCategory category =
                    (PreferenceCategory) findPreference("category_advanced_settings");
            CheckBoxPreference toggle = (CheckBoxPreference) findPreference("checkbox_show_advanced");
            if (category == null || toggle == null) {
                return;
            }

            // 收起再加回時照 XML 的順序排，不要排到最後面（XML 載入時每個選項都已經編好 order）
            category.setOrderingAsAdded(false);
            for (String key : ADVANCED_ONLY_KEYS) {
                Preference pref = category.findPreference(key);
                if (pref != null) {
                    advancedOnlyPrefs.add(pref);
                }
            }

            applyAdvancedVisibility(category, toggle.isChecked());
            toggle.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    applyAdvancedVisibility(category, (Boolean) newValue);
                    return true;
                }
            });
        }

        private void applyAdvancedVisibility(PreferenceCategory category, boolean show) {
            for (Preference pref : advancedOnlyPrefs) {
                boolean shown = category.findPreference(pref.getKey()) != null;
                if (show && !shown) {
                    category.addPreference(pref);
                }
                else if (!show && shown) {
                    category.removePreference(pref);
                }
            }
        }

        private void setupLogDrop() {
            PreferenceCategory category =
                    (PreferenceCategory) findPreference("category_advanced_settings");
            CheckBoxPreference toggle = (CheckBoxPreference) findPreference("checkbox_logdrop_auto");
            Preference uploadNow = findPreference("pref_logdrop_upload_now");
            if (category == null || toggle == null) {
                return;
            }
            if (!LogDrop.isConfigured()) {
                // §LOGDROP：還沒填收件端網址就不要出現，避免勾了卻沒有任何上傳。
                category.removePreference(toggle);
                if (uploadNow != null) {
                    category.removePreference(uploadNow);
                }
                return;
            }
            Activity activity = getActivity();
            if (activity == null) {
                return;
            }
            boolean allow = "yes".equals(LogDrop.consent(activity));
            toggle.setChecked(allow);
            applyLogDropSummary(toggle, activity);
            toggle.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    Activity host = getActivity();
                    if (host == null) {
                        return false;
                    }
                    boolean next = Boolean.TRUE.equals(newValue);
                    LogDrop.setConsent(host, next);
                    applyLogDropSummary((CheckBoxPreference) preference, host);
                    return true;
                }
            });
            if (uploadNow != null) {
                uploadNow.setOnPreferenceClickListener(new Preference.OnPreferenceClickListener() {
                    @Override
                    public boolean onPreferenceClick(Preference preference) {
                        startLogDropUpload();
                        return true;
                    }
                });
            }
        }

        @Override
        public void onDestroy() {
            dismissLogDropDialog();
            super.onDestroy();
        }

        private void startLogDropUpload() {
            if (logDropUploading || !canUpdateLogDropUi()) {
                return;
            }
            final Activity activity = getActivity();
            if (activity == null) {
                return;
            }
            if (!LogDrop.isConfigured()) {
                Toast.makeText(activity, R.string.logdrop_not_configured, Toast.LENGTH_LONG).show();
                return;
            }
            logDropUploading = true;
            setLogDropUploadEnabled(false);
            ProgressDialog progress = new ProgressDialog(activity);
            progress.setMessage(getString(R.string.logdrop_upload_progress));
            progress.setProgressStyle(ProgressDialog.STYLE_SPINNER);
            progress.setIndeterminate(true);
            progress.setCancelable(false);
            progress.setCanceledOnTouchOutside(false);
            if (!showLogDropDialog(progress)) {
                logDropUploading = false;
                setLogDropUploadEnabled(true);
                return;
            }
            // §LOGDROP：Quest 用 android-quest。眼鏡診斷頁維持自己的 android-glasses。
            final Context app = activity.getApplicationContext();
            final String source = LogDrop.sourceFor(app);
            final String report = LogDrop.deviceSummary();
            new Thread(new Runnable() {
                @Override
                public void run() {
                    LogDrop.Result result;
                    try {
                        result = LogDrop.upload(app, source, report, "", "manual");
                    } catch (RuntimeException error) {
                        result = new LogDrop.Result(false, "", "exception");
                    }
                    final LogDrop.Result done = result;
                    if (activity.isDestroyed() || activity.isFinishing()) {
                        logDropUploading = false;
                        return;
                    }
                    activity.runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            logDropUploading = false;
                            if (!canUpdateLogDropUi()) {
                                return;
                            }
                            setLogDropUploadEnabled(true);
                            CheckBoxPreference current = (CheckBoxPreference) findPreference("checkbox_logdrop_auto");
                            Activity host = getActivity();
                            if (current != null && host != null) {
                                applyLogDropSummary(current, host);
                            }
                            showLogDropUploadResult(done);
                        }
                    });
                }
            }, "viple-logdrop-upload").start();
        }

        private void showLogDropUploadResult(LogDrop.Result result) {
            if (!canUpdateLogDropUi()) {
                return;
            }
            if (result != null && result.ok && result.code != null && result.code.length() > 0) {
                showLogDropCode(result.code);
                return;
            }
            String error = result == null ? "" : result.error;
            String message;
            if ("rate_limit".equals(error) || "daily_limit".equals(error) || "install_limit".equals(error)) {
                message = getString(R.string.logdrop_upload_busy);
            } else if ("network".equals(error)) {
                message = getString(R.string.logdrop_upload_network);
            } else {
                if (error == null || error.length() == 0) {
                    error = "bad_response";
                }
                message = getString(R.string.logdrop_upload_failed, error);
            }
            Activity activity = getActivity();
            if (activity == null) {
                return;
            }
            showLogDropDialog(new AlertDialog.Builder(activity)
                    .setMessage(message)
                    .setPositiveButton(android.R.string.ok, null)
                    .create());
        }

        private void showLogDropCode(final String code) {
            Activity activity = getActivity();
            if (activity == null) {
                return;
            }
            LinearLayout box = new LinearLayout(activity);
            box.setOrientation(LinearLayout.VERTICAL);
            TextView codeView = new TextView(activity);
            codeView.setText(code);
            codeView.setGravity(Gravity.CENTER);
            codeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
            codeView.setTypeface(android.graphics.Typeface.MONOSPACE);
            codeView.setTextIsSelectable(true);
            int pad = (int) (16 * activity.getResources().getDisplayMetrics().density + 0.5f);
            codeView.setPadding(pad, pad, pad, pad / 2);
            TextView message = new TextView(activity);
            message.setText(R.string.logdrop_upload_code_message);
            message.setGravity(Gravity.CENTER);
            message.setPadding(pad, 0, pad, pad);
            box.addView(codeView, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            box.addView(message, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            final AlertDialog dialog = new AlertDialog.Builder(activity)
                    .setView(box)
                    .setNeutralButton(R.string.logdrop_upload_copy, new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int which) {
                            copyLogDropCode(code);
                        }
                    })
                    .setPositiveButton(android.R.string.ok, null)
                    .create();
            dialog.setOnShowListener(new DialogInterface.OnShowListener() {
                @Override
                public void onShow(DialogInterface d) {
                    Button copy = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
                    if (copy == null) {
                        return;
                    }
                    // §LOGDROP：預設按鈕會關掉對話框，代碼還沒看完就沒了。
                    copy.setOnClickListener(new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            copyLogDropCode(code);
                        }
                    });
                }
            });
            showLogDropDialog(dialog);
        }

        private void copyLogDropCode(String code) {
            Activity activity = getActivity();
            if (activity == null) {
                return;
            }
            ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
            if (clipboard == null) {
                Toast.makeText(activity, R.string.logdrop_upload_copy_failed, Toast.LENGTH_LONG).show();
                return;
            }
            clipboard.setPrimaryClip(ClipData.newPlainText("logdrop-code", code));
            Toast.makeText(activity, R.string.logdrop_upload_copied, Toast.LENGTH_SHORT).show();
        }

        private void setLogDropUploadEnabled(boolean enabled) {
            Preference uploadNow = findPreference("pref_logdrop_upload_now");
            if (uploadNow != null) {
                uploadNow.setEnabled(enabled);
            }
        }

        private boolean canUpdateLogDropUi() {
            Activity activity = getActivity();
            return isAdded() && activity != null && !activity.isFinishing() && !activity.isDestroyed();
        }

        private boolean showLogDropDialog(android.app.Dialog dialog) {
            if (!canUpdateLogDropUi()) {
                return false;
            }
            dismissLogDropDialog();
            try {
                dialog.show();
            } catch (WindowManager.BadTokenException error) {
                return false;
            }
            logDropDialog = dialog;
            return true;
        }

        private void dismissLogDropDialog() {
            android.app.Dialog dialog = logDropDialog;
            logDropDialog = null;
            if (dialog == null) {
                return;
            }
            try {
                if (dialog.isShowing()) {
                    dialog.dismiss();
                }
            } catch (RuntimeException ignored) {
            }
        }

        private void applyLogDropSummary(CheckBoxPreference toggle, Context context) {
            String id = LogDrop.installId(context);
            String code = LogDrop.lastSuccessCode(context);
            long at = LogDrop.lastSuccessAt(context);
            if (code.length() > 0 && at > 0L) {
                SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US);
                format.setTimeZone(TimeZone.getDefault());
                toggle.setSummary(getString(R.string.summary_logdrop_auto_with_last,
                        id, code, format.format(new Date(at))));
            } else {
                toggle.setSummary(getString(R.string.summary_logdrop_auto, id));
            }
        }
    }
}
