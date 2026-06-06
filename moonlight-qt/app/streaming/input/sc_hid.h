#pragma once

// §SC-HID: Steam Controller raw-HID passthrough (client side)
// Manages a background thread that reads raw HID input reports from a locally
// connected gen-2 Steam Controller — selected by its vendor interface
// (VID 0x28DE, UsagePage 0xFF00 / Usage 0x01, report id 66), so USB-direct
// (0x1302), Puck (0x1304) and Bluetooth (0x1303) are all handled — and forwards
// each report to the host via LiSendScHidInputReport().

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

    // A Steam Controller (especially via the Puck receiver) exposes SEVERAL
    // vendor interfaces (idle slots + the active controller). We open them all
    // and poll each non-blocking, forwarding from whichever actually streams —
    // so direct/Puck/Bluetooth connections are all covered without guessing.
    static constexpr int MAX_SC_DEVS = 8;
    struct SDL_hid_device* m_devs[MAX_SC_DEVS] = {};
    int m_devCount = 0;
    struct SDL_Thread* m_thread = nullptr;
    volatile bool m_running = false;
};
