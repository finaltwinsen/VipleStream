#pragma once

#include <SDL_hidapi.h>   // SDL_hid_device + SDLCALL (via begin_code.h)
#include <SDL_thread.h>   // SDL_Thread + SDL_ThreadFunction

// §SC-HID: Steam Controller raw-HID passthrough (client side)
// Manages a background thread that reads raw HID input reports from a locally
// connected gen-2 Steam Controller — selected by its vendor interface
// (VID 0x28DE, UsagePage 0xFF00 / Usage 0x01, report id 0x45), so USB-direct
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

    // §SC-HID Phase 2C: transparent feature proxy. The host relays a Steam
    // feature op against the real SC:
    //   op==2 (SET): SDL_hid_send_feature_report(query) then read back
    //   op==1 (GET): SDL_hid_get_feature_report(reportId)
    // The real SC's response is returned to the host via LiSendScHidFeatureReport
    // tagged with the same seq.
    void forwardFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                               const uint8_t* query, uint8_t queryLen);

private:
    static int SDLCALL readThreadFunc(void* ctx);
    void readLoop();

    // A Steam Controller (especially via the Puck receiver) exposes SEVERAL
    // vendor interfaces (idle slots + the active controller). We open them all
    // and poll each non-blocking, forwarding from whichever actually streams —
    // so direct/Puck/Bluetooth connections are all covered without guessing.
    static constexpr int MAX_SC_DEVS = 8;
    SDL_hid_device* m_devs[MAX_SC_DEVS] = {};
    int m_devCount = 0;
    SDL_Thread* m_thread = nullptr;
    volatile bool m_running = false;
};
