#include "dxvsyncsource.h"

// Useful references:
// https://bugs.chromium.org/p/chromium/issues/detail?id=467617
// https://chromium.googlesource.com/chromium/src.git/+/c564f2fe339b2b2abb0c8773c90c83215670ea71/gpu/ipc/service/gpu_vsync_provider_win.cc

DxVsyncSource::DxVsyncSource(Pacer* pacer) :
    m_Pacer(pacer),
    m_Gdi32Handle(nullptr),
    m_LastMonitor(nullptr)
{
    SDL_zero(m_WaitForVblankEventParams);
}

DxVsyncSource::~DxVsyncSource()
{
    if (m_WaitForVblankEventParams.hAdapter != 0) {
        D3DKMT_CLOSEADAPTER closeAdapterParams = {};
        closeAdapterParams.hAdapter = m_WaitForVblankEventParams.hAdapter;
        m_D3DKMTCloseAdapter(&closeAdapterParams);
    }

    if (m_Gdi32Handle != nullptr) {
        FreeLibrary(m_Gdi32Handle);
    }
}

bool DxVsyncSource::initialize(SDL_Window* window, int)
{
    m_Gdi32Handle = LoadLibraryA("gdi32.dll");
    if (m_Gdi32Handle == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to load gdi32.dll: %d",
                     GetLastError());
        return false;
    }

    m_D3DKMTOpenAdapterFromHdc = (PFND3DKMTOPENADAPTERFROMHDC)GetProcAddress(m_Gdi32Handle, "D3DKMTOpenAdapterFromHdc");
    m_D3DKMTCloseAdapter = (PFND3DKMTCLOSEADAPTER)GetProcAddress(m_Gdi32Handle, "D3DKMTCloseAdapter");
    m_D3DKMTWaitForVerticalBlankEvent = (PFND3DKMTWAITFORVERTICALBLANKEVENT)GetProcAddress(m_Gdi32Handle, "D3DKMTWaitForVerticalBlankEvent");

    if (m_D3DKMTOpenAdapterFromHdc == nullptr ||
            m_D3DKMTCloseAdapter == nullptr ||
            m_D3DKMTWaitForVerticalBlankEvent == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Missing required function in gdi32.dll");
        return false;
    }

    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);

    if (!SDL_GetWindowWMInfo(window, &info)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_GetWindowWMInfo() failed: %s",
                     SDL_GetError());
        return false;
    }

    // Pacer should only create us on Win32
    SDL_assert(info.subsystem == SDL_SYSWM_WINDOWS);

    m_Window = info.info.win.window;

    return true;
}

bool DxVsyncSource::isAsync()
{
    // We wait in the context of the Pacer thread
    return false;
}

bool DxVsyncSource::waitForVsyncOnce()
{
    NTSTATUS status;

    // If the monitor has changed from last time, open the new adapter
    HMONITOR currentMonitor = MonitorFromWindow(m_Window, MONITOR_DEFAULTTONEAREST);
    if (currentMonitor != m_LastMonitor) {
        MONITORINFOEXA monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoA(currentMonitor, &monitorInfo)) {
            if (m_FailStreak == 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "GetMonitorInfo() failed: %d",
                         GetLastError());
            return false;
        }

        // §REVIEW-1104：dmDriverExtra 沒初始化，EnumDisplaySettingsA 可能失敗或寫過結構結尾
        DEVMODEA monitorMode = {};
        monitorMode.dmSize = sizeof(monitorMode);
        if (!EnumDisplaySettingsA(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &monitorMode)) {
            if (m_FailStreak == 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "EnumDisplaySettings() failed: %d",
                         GetLastError());
            return false;
        }

        if (monitorMode.dmDisplayFrequency > 1) {
            m_LastHz = static_cast<int>(monitorMode.dmDisplayFrequency);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Monitor changed: %s %d Hz",
                    monitorInfo.szDevice,
                    monitorMode.dmDisplayFrequency);

        // Close the old adapter
        if (m_WaitForVblankEventParams.hAdapter != 0) {
            D3DKMT_CLOSEADAPTER closeAdapterParams = {};
            closeAdapterParams.hAdapter = m_WaitForVblankEventParams.hAdapter;
            m_D3DKMTCloseAdapter(&closeAdapterParams);
            // §REVIEW-1104：Close 後清掉 hAdapter 與 m_LastMonitor，開新 adapter 失敗時下一圈才會整段重開，也不會再關一次
            m_WaitForVblankEventParams.hAdapter = 0;
            m_LastMonitor = nullptr;
        }

        D3DKMT_OPENADAPTERFROMHDC openAdapterParams = {};
        openAdapterParams.hDc = CreateDCA(nullptr, monitorInfo.szDevice, nullptr, nullptr);
        if (!openAdapterParams.hDc) {
            if (m_FailStreak == 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CreateDC() failed: %d",
                         GetLastError());
            return false;
        }

        // Open the new adapter
        status = m_D3DKMTOpenAdapterFromHdc(&openAdapterParams);
        DeleteDC(openAdapterParams.hDc);

        if (status != STATUS_SUCCESS) {
            if (m_FailStreak == 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "D3DKMTOpenAdapterFromHdc() failed: %x",
                         status);
            return false;
        }

        m_WaitForVblankEventParams.hAdapter = openAdapterParams.hAdapter;
        m_WaitForVblankEventParams.hDevice = 0;
        m_WaitForVblankEventParams.VidPnSourceId = openAdapterParams.VidPnSourceId;

        m_LastMonitor = currentMonitor;
    }

    status = m_D3DKMTWaitForVerticalBlankEvent(&m_WaitForVblankEventParams);
    if (status != STATUS_SUCCESS) {
        if (m_FailStreak == 0) SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "D3DKMTWaitForVerticalBlankEvent() failed: %x",
                     status);
        return false;
    }
    return true;
}

// §REVIEW-1104（使用者決策 B）：等 vblank 的呼叫立刻失敗時（螢幕關閉／省電、遠端桌面、驅動重置、切換螢幕），
// 原本直接 return，vsync 執行緒就變成無等待空轉：一顆核心吃滿、畫面盡快亂交、log 每圈一行。
// 改成失敗時睡一個畫面間隔（照最後已知的螢幕 Hz）再照常交幀；log 只在連續失敗的第一次印，恢復時印一行
void DxVsyncSource::waitForVsync()
{
    if (waitForVsyncOnce()) {
        if (m_FailStreak > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Vertical blank wait recovered after %d failure(s)",
                        m_FailStreak);
            m_FailStreak = 0;
        }
        return;
    }
    m_FailStreak++;
    SDL_Delay(static_cast<Uint32>(1000 / (m_LastHz > 0 ? m_LastHz : 60)));
}
