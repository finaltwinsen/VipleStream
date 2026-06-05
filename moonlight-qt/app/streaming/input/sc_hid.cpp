// §SC-HID: Steam Controller raw-HID passthrough (client side)

#include "sc_hid.h"
#include "SDL_compat.h"
#include <SDL_hidapi.h>

extern "C" {
#include <Limelight.h>
}

// Steam Controller USB identifiers
static constexpr unsigned short SC_VID = 0x28DE;
static constexpr unsigned short SC_PID_USB = 0x1102;  // wired direct

// HID feature report IDs for lizard-mode control (sent from host side via
// the host's virtual device driver, but the client also sends these once
// at startup to mute the physical controller's own lizard-mode output so
// we get clean raw gamepad reports instead of spurious kbd/mouse events).
static constexpr uint8_t SC_ID_CLEAR_DIGITAL_MAPPINGS = 0x81;

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

    m_dev = SDL_hid_open(SC_VID, SC_PID_USB, nullptr);
    if (!m_dev) {
        // No Steam Controller attached — that's fine
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] No Steam Controller found (0x28DE:0x1102)");
        return;
    }

    // Disable lizard mode on the physical controller so we only receive raw
    // gamepad input reports (not spurious keyboard/mouse HID events).
    uint8_t lizardOff[65] = {};  // report ID 0 prepended (HID API convention)
    lizardOff[1] = SC_ID_CLEAR_DIGITAL_MAPPINGS;
    SDL_hid_send_feature_report(m_dev, lizardOff, 65);

    SDL_hid_set_nonblocking(m_dev, 0);  // blocking reads in the thread

    m_running = true;
    m_thread = SDL_CreateThread(readThreadFunc, "SC-HID", this);
    if (!m_thread) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Failed to create read thread: %s", SDL_GetError());
        m_running = false;
        SDL_hid_close(m_dev);
        m_dev = nullptr;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Steam Controller passthrough started");
    }
}

void ScHidPassthrough::stop() {
    if (!m_running) return;
    m_running = false;

    // Unblock the read thread by closing the device; SDL_hid_read will return
    // an error and the thread will exit.
    if (m_dev) {
        SDL_hid_close(m_dev);
        m_dev = nullptr;
    }

    if (m_thread) {
        SDL_WaitThread(m_thread, nullptr);
        m_thread = nullptr;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Steam Controller passthrough stopped");
}

bool ScHidPassthrough::isActive() const {
    return m_running && m_dev != nullptr;
}

int SDLCALL ScHidPassthrough::readThreadFunc(void* ctx) {
    static_cast<ScHidPassthrough*>(ctx)->readLoop();
    return 0;
}

void ScHidPassthrough::readLoop() {
    uint8_t buf[64];

    while (m_running) {
        int n = SDL_hid_read(m_dev, buf, sizeof(buf));
        if (n < 0) {
            // Device was closed or error
            break;
        }
        if (n == 0) {
            // Timeout (shouldn't happen in blocking mode, but be safe)
            continue;
        }

        // Pad short reads to exactly 64 bytes
        if (n < SS_SC_HID_REPORT_MAX) {
            memset(buf + n, 0, SS_SC_HID_REPORT_MAX - n);
        }

        LiSendScHidInputReport(buf, 64);
    }
}
