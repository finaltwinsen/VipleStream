#pragma once

#include <QString>
#include <QStringList>

#define THROW_BAD_ALLOC_IF_NULL(x) \
    if ((x) == nullptr) throw std::bad_alloc()

namespace WMUtils {
    bool isRunningX11();
    bool isRunningNvidiaProprietaryDriverX11();
    bool supportsDesktopGLWithEGL();
    bool isRunningWayland();
    bool isRunningWindowManager();
    bool isRunningDesktopEnvironment();
    QString getDrmCardOverride();
    bool isGpuSlow();

    // §F6 — Linux renderer 決策用的探測（結果在行程內快取）。
    // 非 Linux 平台一律回空清單 / false，Windows 與 macOS 行為不受影響。
    //
    // getDrmDriverNames()：/dev/dri/renderD* 的 kernel DRM driver 名稱
    //   （libdrm drmGetVersion；拿不到時退到 /sys/class/drm 的 driver symlink）。
    // isDrmDriverMsm()：任一 DRM 裝置是 msm（Adreno，Mesa 走 freedreno/Turnip）。
    // isEglZink()：EGL（GBM platform）的 vendor 或 EGL_MESA_query_driver 回報
    //   的 driver 是 Zink（GL 跑在 Vulkan 之上）。
    QStringList getDrmDriverNames();
    bool isDrmDriverMsm();
    bool isEglZink();
}

namespace Utils {
    // §F6 — 環境變數覆寫一律是 dev-only（功能設定不得用環境變數）。
    // 有設定（非空）時印一次 `[VIPLE-DEVENV] dev-only override <NAME>=<value>`，
    // 同一個名稱整個行程只印一次；不改變覆寫本身的效果。
    void logDevEnvOverride(const char* name);

    // §F6 — 掃描目前行程的環境變數，把 dev-only 名單內的覆寫各印一次：
    // 固定名稱（PREFER_VULKAN、GL_IS_SLOW、VULKAN_IS_SLOW、
    // MATCH_DISPLAY_MODE_TO_VIDEO、SEPARATE_TEST_DECODER、VIPLE_USE_VK_DECODER、
    // VIPLE_VK_FRUC_GENERIC）、前綴 VIPLE_VKFRUC_*、後綴 *_AVOPTIONS 與
    // *_DECODER_HINT。VIPLE_VKFRUC_* 的讀取點散在 vkfruc.cpp 數十處，
    // 所以集中在 decoder 初始化時掃一次，而不是逐一插樁。
    void logDevEnvOverrides();

    template <typename T>
    bool getEnvironmentVariableOverride(const char* name, T* value) {
        bool ok;
        *value = (T)qEnvironmentVariableIntValue(name, &ok);
        // §F6 — 經由這個 helper 讀的覆寫全部標成 dev-only
        logDevEnvOverride(name);
        return ok;
    }
}
