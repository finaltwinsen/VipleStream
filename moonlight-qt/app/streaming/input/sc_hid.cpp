// §SC-HID: Steam Controller raw-HID passthrough (client side)

#include "sc_hid.h"
#include "SDL_compat.h"
#include <SDL_hidapi.h>
#include <cstring>   // memset / memcpy

extern "C" {
#include <Limelight.h>
}

// Fixed wire report size (SS_SC_HID_REPORT_MAX in moonlight-common-c's internal
// Input.h, which is not on the app include path). LiSendScHidInputReport always
// sends exactly this many bytes.
static constexpr int SC_HID_WIRE_BYTES = 64;

// Valve vendor id. The gen-2 (2025) Steam Controller exposes its gamepad data
// on a vendor-defined HID interface (UsagePage 0xFF00 / Usage 0x01) carrying
// HID report id 66 (0x42). We select the device by that usage rather than by a
// hard-coded product id, so all transports are handled by one code path:
//   0x1302 = USB-direct, 0x1303 = Bluetooth LE, 0x1304 = Puck (wireless).
static constexpr unsigned short SC_VID        = 0x28DE;
static constexpr unsigned short SC_USAGE_PAGE = 0xFF00;
static constexpr unsigned short SC_USAGE      = 0x0001;

ScHidPassthrough::ScHidPassthrough() = default;

ScHidPassthrough::~ScHidPassthrough() {
    stop();
}

void ScHidPassthrough::start() {
    if (m_running) return;

    if (SDL_hid_init() < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] SDL_hid_init failed: %s", SDL_GetError());
        return;
    }

    // Find the Steam Controller's vendor gamepad interface across ANY transport
    // (USB-direct / Puck / Bluetooth) by enumerating VID 0x28DE and matching the
    // vendor usage, rather than a fixed product id.
    SDL_hid_device_info* devs = SDL_hid_enumerate(SC_VID, 0x0);
    m_devCount = 0;
    for (SDL_hid_device_info* d = devs; d != nullptr && m_devCount < MAX_SC_DEVS; d = d->next) {
        if (d->usage_page == SC_USAGE_PAGE && d->usage == SC_USAGE && d->path != nullptr) {
            SDL_hid_device* h = SDL_hid_open_path(d->path, 0 /* shared */);
            if (h != nullptr) {
                SDL_hid_set_nonblocking(h, 1);  // non-blocking: we poll all handles
                m_devs[m_devCount++] = h;
            }
        }
    }
    if (devs != nullptr) {
        SDL_hid_free_enumeration(devs);
    }

    if (m_devCount == 0) {
        // No Steam Controller attached — that's fine
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] No Steam Controller vendor interface found (0x28DE FF00/01)");
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Opened %d Steam Controller vendor interface(s)", m_devCount);

    // The gen-2 Steam Controller needs NO enable command -- it streams its
    // reports by default. The earlier difficulty was reading the wrong (idle
    // Puck slot) interface; opening them all and polling each (above/below)
    // solves it. CAVEAT: if Steam Input is running locally it can claim or
    // reconfigure the controller (changing which report it streams), which
    // interferes with passthrough -- the local machine should not be running
    // Steam on this controller. See project memory project-sc-hid-passthrough.

    m_running = true;
    m_thread = SDL_CreateThread(readThreadFunc, "SC-HID", this);
    if (!m_thread) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Failed to create read thread: %s", SDL_GetError());
        m_running = false;
        for (int i = 0; i < m_devCount; i++) {
            SDL_hid_close(m_devs[i]);
            m_devs[i] = nullptr;
        }
        m_devCount = 0;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Steam Controller passthrough started");
    }
}

void ScHidPassthrough::stop() {
    if (!m_running) return;
    m_running = false;

    // Reads are non-blocking, so the thread exits on its own once m_running is
    // cleared. Join first, then close the handles it was polling.
    if (m_thread) {
        SDL_WaitThread(m_thread, nullptr);
        m_thread = nullptr;
    }

    for (int i = 0; i < m_devCount; i++) {
        if (m_devs[i]) {
            SDL_hid_close(m_devs[i]);
            m_devs[i] = nullptr;
        }
    }
    m_devCount = 0;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Steam Controller passthrough stopped");
}

bool ScHidPassthrough::isActive() const {
    return m_running && m_devCount > 0;
}

int SDLCALL ScHidPassthrough::readThreadFunc(void* ctx) {
    static_cast<ScHidPassthrough*>(ctx)->readLoop();
    return 0;
}

void ScHidPassthrough::forwardFeatureRequest(uint8_t reportId, uint8_t reportType) {
    if (m_devCount == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] forwardFeatureRequest: no device open");
        return;
    }

    uint8_t buf[SC_HID_WIRE_BYTES];

    for (int i = 0; i < m_devCount; i++) {
        memset(buf, 0, sizeof(buf));
        buf[0] = reportId;

        int n = -1;
        if (reportType == 1) {
            // GET_FEATURE: read feature report from real SC
            n = SDL_hid_get_feature_report(m_devs[i], buf, sizeof(buf));
        } else {
            // output report (e.g. haptics): nothing to proxy back
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] forwardFeatureRequest: output type ignored (id=0x%02x)", reportId);
            return;
        }

        if (n > 0) {
            if (n < SC_HID_WIRE_BYTES) {
                memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
            }
            // buf[0] == reportId (non-0x45) → server routes to VipleSCHidSetFeature
            LiSendScHidInputReport(buf, SC_HID_WIRE_BYTES);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Feature report 0x%02x forwarded (%d bytes)", reportId, n);
            return;  // first successful response is enough
        }
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] forwardFeatureRequest: GET_FEATURE 0x%02x failed on all %d device(s)",
        reportId, m_devCount);
}

void ScHidPassthrough::readLoop() {
    uint8_t buf[64];

    while (m_running) {
        bool gotAny = false;

        // Poll every opened vendor interface; only the active controller's
        // interface actually delivers reports (the others stay silent).
        for (int i = 0; i < m_devCount; i++) {
            int n = SDL_hid_read(m_devs[i], buf, sizeof(buf));
            if (n <= 0) {
                continue;  // 0 = no data (non-blocking); <0 = error on this handle
            }
            gotAny = true;

            // Pad short reads to exactly the fixed wire report size.
            if (n < SC_HID_WIRE_BYTES) {
                memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
            }
            LiSendScHidInputReport(buf, SC_HID_WIRE_BYTES);
        }

        if (!gotAny) {
            SDL_Delay(1);  // nothing pending on any interface -- yield briefly
        }
    }
}
