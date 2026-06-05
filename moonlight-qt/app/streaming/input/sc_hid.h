#pragma once

// §SC-HID: Steam Controller raw-HID passthrough (client side)
// Manages a background thread that reads 64-byte HID input reports from a
// locally connected Steam Controller (USB wired, VID 0x28DE / PID 0x1102)
// and forwards each report to the host via LiSendScHidInputReport().

class ScHidPassthrough {
public:
    ScHidPassthrough();
    ~ScHidPassthrough();

    // Call once after LiStartConnection() succeeds.
    // Scans for a connected Steam Controller and starts the read thread.
    // Safe to call even if no Steam Controller is present (no-op).
    void start();

    // Call once before LiStopConnection(). Stops the read thread and
    // closes the HID device.
    void stop();

    bool isActive() const;

private:
    static int SDLCALL readThreadFunc(void* ctx);
    void readLoop();

    struct SDL_hid_device* m_dev = nullptr;
    struct SDL_Thread* m_thread = nullptr;
    volatile bool m_running = false;
};
