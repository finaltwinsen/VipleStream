package com.limelight.binding.input.driver;

import android.annotation.SuppressLint;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.view.InputDevice;
import android.widget.Toast;

import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.nvstream.jni.MoonBridge;
import com.limelight.preferences.PreferenceConfiguration;

import java.io.File;
import java.util.ArrayList;

public class UsbDriverService extends Service implements UsbDriverListener {

    private static final String ACTION_USB_PERMISSION =
            "com.limelight.USB_PERMISSION";

    private UsbManager usbManager;
    private PreferenceConfiguration prefConfig;
    private boolean started;

    // §REVIEW-1104：保留主執行緒 Handler 用於管理延遲工作與非同步回呼
    private final Handler handler = new Handler(Looper.getMainLooper());
    // §REVIEW-1104：保護 controllers 與 steamControllers 清單結構修改的同步鎖
    private final Object controllerLock = new Object();

    private final UsbEventReceiver receiver = new UsbEventReceiver();
    private final UsbDriverBinder binder = new UsbDriverBinder();

    private final ArrayList<AbstractController> controllers = new ArrayList<>();

    // §SC-HID: Steam Controller passthrough drivers live in their own list
    // because they are not AbstractControllers — they forward raw HID reports
    // to the host instead of reporting parsed button state, so they must never
    // be announced to the UsbDriverListener (the host creates its own virtual
    // Steam Controller device on the other end).
    private final ArrayList<SteamControllerDriver> steamControllers = new ArrayList<>();

    private UsbDriverListener listener;
    private UsbDriverStateListener stateListener;
    private int nextDeviceId;

    @Override
    public void reportControllerState(int controllerId, int buttonFlags, float leftStickX, float leftStickY,
                                      float rightStickX, float rightStickY, float leftTrigger, float rightTrigger) {
        // Call through to the client's listener
        

        if (listener != null) {
            listener.reportControllerState(controllerId, buttonFlags, leftStickX, leftStickY, rightStickX, rightStickY, leftTrigger, rightTrigger);
        }
    }

    @Override
    public void deviceRemoved(final AbstractController controller) {
        // §REVIEW-1104：驅動執行緒只標記已停，實際移除與通知丟回主執行緒並同步清單鎖
        handler.post(new Runnable() {
            @Override
            public void run() {
        // Remove the the controller from our list (if not removed already)
                synchronized (controllerLock) {
                    controllers.remove(controller);
                }

        // Call through to the client's listener
        
                if (listener != null) {
                    listener.deviceRemoved(controller);
                }
            }
        });
    }

    /*

        }
    }

    @Override
    */
    @Override
    public void deviceAdded(AbstractController controller) {
        // Call through to the client's listener
        
        if (listener != null) {
            listener.deviceAdded(controller);
        }
    }

    public class UsbEventReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();

            // Initial attachment broadcast
            if (action.equals(UsbManager.ACTION_USB_DEVICE_ATTACHED)) {
                final UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);

                // shouldClaimDevice() looks at the kernel's enumerated input
                // devices to make its decision about whether to prompt to take
                // control of the device. The kernel bringing up the input stack
                // may race with this callback and cause us to prompt when the
                // kernel is capable of running the device. Let's post a delayed
                // message to process this state change to allow the kernel
                // some time to bring up the stack.
                // §REVIEW-1104：使用保留的 handler 排程延遲工作，並於工作執行前檢查 started
                handler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (!started) {
                            return;
                        }
                        // Continue the state machine
                        handleUsbDeviceState(device);
                    }
                }, 1000);
            }
            // Subsequent permission dialog completion intent
            else if (action.equals(ACTION_USB_PERMISSION)) {
                UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);

                // Permission dialog is now closed
                if (stateListener != null) {
                    stateListener.onUsbPermissionPromptCompleted();
                }

                // If we got this far, we've already found we're able to handle this device
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    handleUsbDeviceState(device);
                }
            }
        }
    }

    public class UsbDriverBinder extends Binder {
        public void setListener(UsbDriverListener listener) {
            UsbDriverService.this.listener = listener;

            // Report all controllerMap that already exist
            
            if (listener != null) {
                // §REVIEW-1104：遍歷 controllers 清單時取得 controllerLock 同步鎖
                synchronized (controllerLock) {
                    for (AbstractController controller : controllers) {
                        listener.deviceAdded(controller);
                    }
                }
            }
        }
        /*
                if (listener != null) {
            listener.deviceAdded(controller);
                }
            }
        }

        */

        public void setStateListener(UsbDriverStateListener stateListener) {
            UsbDriverService.this.stateListener = stateListener;
        }

        public void start() {
            UsbDriverService.this.start();
        }

        public void stop() {
            UsbDriverService.this.stop();
        }
    }

    private void handleUsbDeviceState(UsbDevice device) {
        // §REVIEW-1104：檢查 service 是否已停止，避免在 onDestroy 後繼續認領裝置
        if (!started) {
            return;
        }
        // Are we able to operate it?
        if (shouldClaimDevice(device, prefConfig.bindAllUsb)) {
            // Do we have permission yet?
            if (!usbManager.hasPermission(device)) {
                // Let's ask for permission
                try {
                    // Tell the state listener that we're about to display a permission dialog
                    if (stateListener != null) {
                        stateListener.onUsbPermissionPromptStarting();
                    }

                    int intentFlags = 0;
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        // This PendingIntent must be mutable to allow the framework to populate EXTRA_DEVICE and EXTRA_PERMISSION_GRANTED.
                        intentFlags |= PendingIntent.FLAG_MUTABLE;
                    }

                    // This function is not documented as throwing any exceptions (denying access
                    // is indicated by calling the PendingIntent with a false result). However,
                    // Samsung Knox has some policies which block this request, but rather than
                    // just returning a false result or returning 0 enumerated devices,
                    // they throw an undocumented SecurityException from this call, crashing
                    // the whole app. :(

                    // Use an explicit intent to activate our unexported broadcast receiver, as required on Android 14+
                    Intent i = new Intent(ACTION_USB_PERMISSION);
                    i.setPackage(getPackageName());

                    usbManager.requestPermission(device, PendingIntent.getBroadcast(UsbDriverService.this, 0, i, intentFlags));
                } catch (SecurityException e) {
                    Toast.makeText(this, this.getText(R.string.error_usb_prohibited), Toast.LENGTH_LONG).show();
                    if (stateListener != null) {
                        stateListener.onUsbPermissionPromptCompleted();
                    }
                }
                return;
            }

            // Open the device
            UsbDeviceConnection connection = usbManager.openDevice(device);
            if (connection == null) {
                LimeLog.warning("Unable to open USB device: "+device.getDeviceName());
                return;
            }

            // §REVIEW-1104：已打開連線但發現服務已停止，立刻關閉連線避免佔用介面
            if (!started) {
                connection.close();
                return;
            }


            // §SC-HID: Steam Controller gen-2 raw passthrough (outside the
            // AbstractController lifecycle, see steamControllers above)
            if (SteamControllerDriver.canClaimDevice(device)) {
                SteamControllerDriver scDriver = new SteamControllerDriver(device, connection,
                        new SteamControllerDriver.Listener() {
                            @Override
                            public void onStopped(final SteamControllerDriver driver) {
                                // §REVIEW-1104：驅動執行緒只標記已停，實際移除排回 service 主執行緒並同步清單鎖
                                handler.post(new Runnable() {
                                    @Override
                                    public void run() {
                                        synchronized (controllerLock) {
                                            steamControllers.remove(driver);
                                            // §REVIEW-1104：若仍有其他存活的 Steam Controller 驅動，將 feature request slot 重新掛載給剩餘驅動，否則確保已停驅動從 slot 清除
                                            if (!steamControllers.isEmpty()) {
                                                MoonBridge.setScHidFeatureRequestHandler(
                                                        steamControllers.get(steamControllers.size() - 1));
                                            } else {
                                                MoonBridge.clearScHidFeatureRequestHandler(driver);
                                            }
                                        }
                                    }
                                });
                            }
                        });
                /*
                // §REVIEW-1104
                                            steamControllers.remove(driver);
            
        
                                           });
                            }
                        });
                                       });
                            }
                        });
        
                            }




                
                */
                if (!scDriver.start()) {
                    connection.close();
                    return;
                }
                // §REVIEW-1104：加入 steamControllers 清單時同步清單鎖
                synchronized (controllerLock) {
                    steamControllers.add(scDriver);
                }
                return;
            }

            AbstractController controller;

            if (XboxOneController.canClaimDevice(device)) {
                controller = new XboxOneController(device, connection, nextDeviceId++, this);
            }
            else if (Xbox360Controller.canClaimDevice(device)) {
                controller = new Xbox360Controller(device, connection, nextDeviceId++, this);
            }
            else if (Xbox360WirelessDongle.canClaimDevice(device)) {
                controller = new Xbox360WirelessDongle(device, connection, nextDeviceId++, this);
            }
            else {
                // Unreachable
                return;
            }

            // Start the controller
            if (!controller.start()) {
                connection.close();
                return;
            }

            // Add this controller to the list
            // §REVIEW-1104：加入 controllers 清單時同步清單鎖
            synchronized (controllerLock) {
                controllers.add(controller);
            }
        }
    }

    public static boolean isRecognizedInputDevice(UsbDevice device) {
        // Determine if this VID and PID combo matches an existing input device
        // and defer to the built-in controller support in that case.
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice inputDev = InputDevice.getDevice(id);
            if (inputDev == null) {
                // Device was removed while looping
                continue;
            }

            if (inputDev.getVendorId() == device.getVendorId() &&
                    inputDev.getProductId() == device.getProductId()) {
                return true;
            }
        }

        return false;
    }

    public static boolean kernelSupportsXboxOne() {
        String kernelVersion = System.getProperty("os.version");
        LimeLog.info("Kernel Version: "+kernelVersion);

        if (kernelVersion == null) {
            // We'll assume this is some newer version of Android
            // that doesn't let you read the kernel version this way.
            return true;
        }
        else if (kernelVersion.startsWith("2.") || kernelVersion.startsWith("3.")) {
            // These are old kernels that definitely don't support Xbox One controllers properly
            return false;
        }
        else if (kernelVersion.startsWith("4.4.") || kernelVersion.startsWith("4.9.")) {
            // These aren't guaranteed to have backported kernel patches for proper Xbox One
            // support (though some devices will).
            return false;
        }
        else {
            // The next AOSP common kernel is 4.14 which has working Xbox One controller support
            return true;
        }
    }

    public static boolean kernelSupportsXbox360W() {
        // Check if this kernel is 4.2+ to see if the xpad driver sets Xbox 360 wireless LEDs
        // https://github.com/torvalds/linux/commit/75b7f05d2798ee3a1cc5bbdd54acd0e318a80396
        String kernelVersion = System.getProperty("os.version");
        if (kernelVersion != null) {
            if (kernelVersion.startsWith("2.") || kernelVersion.startsWith("3.") ||
                    kernelVersion.startsWith("4.0.") || kernelVersion.startsWith("4.1.")) {
                // Even if LED devices are present, the driver won't set the initial LED state.
                return false;
            }
        }

        // We know we have a kernel that should set Xbox 360 wireless LEDs, but we still don't
        // know if CONFIG_JOYSTICK_XPAD_LEDS was enabled during the kernel build. Unfortunately
        // it's not possible to detect this reliably due to Android's app sandboxing. Reading
        // /proc/config.gz and enumerating /sys/class/leds are both blocked by SELinux on any
        // relatively modern device. We will assume that CONFIG_JOYSTICK_XPAD_LEDS=y on these
        // kernels and users can override by using the settings option to claim all devices.
        return true;
    }

    public static boolean shouldClaimDevice(UsbDevice device, boolean claimAllAvailable) {
        return ((!kernelSupportsXboxOne() || !isRecognizedInputDevice(device) || claimAllAvailable) && XboxOneController.canClaimDevice(device)) ||
                ((!isRecognizedInputDevice(device) || claimAllAvailable) && Xbox360Controller.canClaimDevice(device)) ||
                // We must not call isRecognizedInputDevice() because wireless controllers don't share the same product ID as the dongle
                ((!kernelSupportsXbox360W() || claimAllAvailable) && Xbox360WirelessDongle.canClaimDevice(device)) ||
                // §SC-HID: always claim gen-2 Steam Controllers for raw passthrough,
                // even if the kernel exposes an InputDevice for them — the host needs
                // the raw vendor HID reports, which the Android input stack doesn't
                // deliver. Claiming detaches the kernel driver, so the InputDevice
                // goes away on its own (no extra suppression needed in ControllerHandler).
                SteamControllerDriver.canClaimDevice(device);
    }

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    private void start() {
        if (started || usbManager == null) {
            return;
        }

        started = true;

        // Register for USB attach broadcasts and permission completions
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(ACTION_USB_PERMISSION);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, filter, RECEIVER_NOT_EXPORTED);
        }
        else {
            registerReceiver(receiver, filter);
        }

        // Enumerate existing devices
        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            if (shouldClaimDevice(dev, prefConfig.bindAllUsb)) {
                // Start the process of claiming this device
                handleUsbDeviceState(dev);
            }
        }
    }

    private void stop() {
        if (!started) {
            return;
        }

        started = false;

        // §REVIEW-1104：停止服務時移除尚未執行的延遲工作與回呼
        handler.removeCallbacksAndMessages(null);

        // Stop the attachment receiver
        unregisterReceiver(receiver);

        // §REVIEW-1104：使用同一把鎖同步清空 controllers 與 steamControllers 清單並停止裝置
        synchronized (controllerLock) {
            // Stop all controllers
        while (controllers.size() > 0) {
            // Stop and remove the controller
            controllers.remove(0).stop();
        }

        // §SC-HID: stop all Steam Controller passthrough drivers. Remove before
        // stopping — the driver's onStopped callback also removes itself, which
        // would be a no-op here.
        while (steamControllers.size() > 0) {
            steamControllers.remove(0).stop();
        }
        }
    }

    @Override
    public void onCreate() {
        this.usbManager = (UsbManager) getSystemService(Context.USB_SERVICE);
        this.prefConfig = PreferenceConfiguration.readPreferences(this);
    }

    @Override
    public void onDestroy() {
        stop();

        // Remove listeners
        listener = null;
        stateListener = null;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    public interface UsbDriverStateListener {
        void onUsbPermissionPromptStarting();
        void onUsbPermissionPromptCompleted();
    }
}
