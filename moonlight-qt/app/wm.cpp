#include <QtGlobal>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QByteArray>
#include <QProcessEnvironment>

#include "utils.h"

#include "SDL_compat.h"

#ifdef HAS_X11
#include <X11/Xlib.h>
#endif

#ifdef HAS_WAYLAND
#include <wayland-client.h>
#endif

#ifdef HAVE_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#ifdef HAVE_EGL
#include <EGL/egl.h>

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#endif

#define VALUE_SET 0x01
#define VALUE_TRUE 0x02

bool WMUtils::isRunningX11()
{
#ifdef HAS_X11
    static SDL_atomic_t isRunningOnX11;

    // If the value is not set yet, populate it now.
    int val = SDL_AtomicGet(&isRunningOnX11);
    if (!(val & VALUE_SET)) {
        Display* display = XOpenDisplay(nullptr);
        if (display != nullptr) {
            XCloseDisplay(display);
        }

        // Populate the value to return and have for next time.
        // This can race with another thread populating the same data,
        // but that's no big deal.
        val = VALUE_SET | ((display != nullptr) ? VALUE_TRUE : 0);
        SDL_AtomicSet(&isRunningOnX11, val);
    }

    return !!(val & VALUE_TRUE);
#else
    return false;
#endif
}

bool WMUtils::isRunningNvidiaProprietaryDriverX11()
{
#ifdef HAVE_EGL
    static SDL_atomic_t isRunningOnNvidiaDriver;

    // If the value is not set yet, populate it now.
    int val = SDL_AtomicGet(&isRunningOnNvidiaDriver);
    if (!(val & VALUE_SET)) {
        bool nvidiaDriver = false;

        // Open the default X11 display. This is critical for accurate detection of the
        // Nvidia driver under XWayland because eglGetDisplay(EGL_DEFAULT_DISPLAY) will
        // return the Wayland display but Qt will use the X11 display.
        EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, EGL_DEFAULT_DISPLAY, nullptr);
        if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr)) {
            const char* vendorString = eglQueryString(display, EGL_VENDOR);
            nvidiaDriver = vendorString && strstr(vendorString, "NVIDIA") != NULL;
            eglTerminate(display);
        }

        // Populate the value to return and have for next time.
        // This can race with another thread populating the same data,
        // but that's no big deal.
        val = VALUE_SET | (nvidiaDriver ? VALUE_TRUE : 0);
        SDL_AtomicSet(&isRunningOnNvidiaDriver, val);
    }

    return !!(val & VALUE_TRUE);
#else
    return false;
#endif
}

bool WMUtils::supportsDesktopGLWithEGL()
{
#ifdef HAVE_EGL
    static SDL_atomic_t supportsDesktopGL;

    // If the value is not set yet, populate it now.
    int val = SDL_AtomicGet(&supportsDesktopGL);
    if (!(val & VALUE_SET)) {
        // Assume it does if we can't confirm
        bool desktopGL = true;

        // Prefer GBM as some drivers (pvr) use swrast/llvmpipe for X11/XWayland,
        // so we'll get a different (and incorrect) result if we query X11.
        EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY) {
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
        if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr)) {
            EGLint matchingConfigs = 0;
            EGLint const attribs[] =
            {
                EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_NONE
            };

            desktopGL = eglChooseConfig(display, attribs, nullptr, 0, &matchingConfigs) == EGL_TRUE &&
                        matchingConfigs > 0;
            eglTerminate(display);
        }

        // Populate the value to return and have for next time.
        // This can race with another thread populating the same data,
        // but that's no big deal.
        val = VALUE_SET | (desktopGL ? VALUE_TRUE : 0);
        SDL_AtomicSet(&supportsDesktopGL, val);
    }

    return !!(val & VALUE_TRUE);
#else
    // Assume it does if we can't check ourselves
    return true;
#endif
}

bool WMUtils::isRunningWayland()
{
#ifdef HAS_WAYLAND
    static SDL_atomic_t isRunningOnWayland;

    // If the value is not set yet, populate it now.
    int val = SDL_AtomicGet(&isRunningOnWayland);
    if (!(val & VALUE_SET)) {
        struct wl_display* display = nullptr;

        // We need to avoid the default fallback to wayland-0 that wl_display_connect()
        // will try for cases where we might be running from a TTY with a Wayland
        // compositor running in another VT that happens to use the wayland-0 name.
        if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
            !qEnvironmentVariableIsEmpty("WAYLAND_SOCKET") ||
            qgetenv("XDG_SESSION_TYPE") == "wayland") {

            // This looks like it might be a Wayland environment, so give it a shot
            display = wl_display_connect(nullptr);
            if (display != nullptr) {
                wl_display_disconnect(display);
            }
        }

        // Populate the value to return and have for next time.
        // This can race with another thread populating the same data,
        // but that's no big deal.
        val = VALUE_SET | ((display != nullptr) ? VALUE_TRUE : 0);
        SDL_AtomicSet(&isRunningOnWayland, val);
    }

    return !!(val & VALUE_TRUE);
#else
    return false;
#endif
}

bool WMUtils::isRunningWindowManager()
{
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    // Windows and macOS are always running a window manager
    return true;
#else
    // On Unix OSes, look for Wayland or X
    return WMUtils::isRunningWayland() || WMUtils::isRunningX11();
#endif
}

bool WMUtils::isRunningDesktopEnvironment()
{
    bool value;
    if (Utils::getEnvironmentVariableOverride("HAS_DESKTOP_ENVIRONMENT", &value)) {
        return value;
    }

#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    // Windows and macOS are always running a desktop environment
    return true;
#elif defined(EMBEDDED_BUILD)
    // Embedded systems don't run desktop environments
    return false;
#else
    // On non-embedded systems, assume we have a desktop environment
    // if we have a WM running.
    return isRunningWindowManager();
#endif
}

bool WMUtils::isGpuSlow()
{
    bool ret;

    if (!Utils::getEnvironmentVariableOverride("GL_IS_SLOW", &ret)) {
#if defined(GL_IS_SLOW)
        // 建置時明確指定 glslow / gpuslow，維持原語意
        ret = true;
#elif !defined(Q_PROCESSOR_X86) && !defined(Q_OS_DARWIN) && !defined(Q_OS_WIN)
        // We currently assume GPUs on non-x86 hardware are slow by default
        ret = true;
#ifdef Q_OS_LINUX
        // §F6 — Adreno（kernel DRM driver = msm，Mesa 走 freedreno / Turnip）
        // 效能足以跑 PlVk / EGL 合成，不套用「非 x86 一律慢」的預設。
        // 這個判定會連帶改變 C19 的 7 個呼叫點（ffmpeg.cpp 的 glIsSlow /
        // vulkanIsSlow、session.cpp 的 SDL_WINDOW_FULLSCREEN 與 matchVideo、
        // streamingpreferences.cpp 的推薦全螢幕模式）。拿不到 driver 名稱
        // 時 isDrmDriverMsm() 回 false，維持舊預設（慢）。
        if (isDrmDriverMsm()) {
            ret = false;
        }
#endif
#else
        ret = false;
#endif
    }

    return ret;
}

#ifdef Q_OS_LINUX
// §F6 — sysfs 後備：/sys/class/drm/<node>/device/driver 是指向 driver 目錄的
// symlink，取其目錄名稱。只看 renderD* 與 cardN（排除 cardN-<connector>）。
static QStringList probeDrmDriverNamesFromSysfs()
{
    QStringList names;
    QDir sysDrm("/sys/class/drm");
    const QStringList nodes = sysDrm.entryList(QStringList() << "renderD*" << "card*",
                                               QDir::Dirs | QDir::System | QDir::NoDotAndDotDot,
                                               QDir::Name);
    for (const QString& node : nodes) {
        if (node.contains('-')) {
            // cardN-<connector> 是 connector 節點，不是 GPU
            continue;
        }

        QFileInfo driverLink(sysDrm.filePath(node) + "/device/driver");
        if (!driverLink.exists()) {
            continue;
        }

        QString driverName = QFileInfo(driverLink.canonicalFilePath()).fileName();
        if (!driverName.isEmpty() && !names.contains(driverName)) {
            names.append(driverName);
        }
    }

    return names;
}

static QStringList probeDrmDriverNames()
{
    QStringList names;
    const char* source = "none";

#ifdef HAVE_DRM
    // 優先用 libdrm：render node 不牽涉 DRM master，drmGetVersion() 的 name
    // 就是 kernel DRM driver 名稱（Mesa loader 也是用它挑 driver）。
    {
        QDir dir("/dev/dri");
        const QStringList nodes = dir.entryList(QStringList("renderD*"),
                                                QDir::Files | QDir::System,
                                                QDir::Name);
        for (const QString& node : nodes) {
            QFile nodeFile(dir.filePath(node));
            if (!nodeFile.open(QFile::ReadOnly)) {
                continue;
            }

            drmVersionPtr version = drmGetVersion(nodeFile.handle());
            if (version != nullptr) {
                if (version->name != nullptr && version->name_len > 0) {
                    QString driverName = QString::fromLatin1(version->name, version->name_len);
                    if (!names.contains(driverName)) {
                        names.append(driverName);
                    }
                }
                drmFreeVersion(version);
            }
        }

        if (!names.isEmpty()) {
            source = "libdrm";
        }
    }
#endif

    if (names.isEmpty()) {
        names = probeDrmDriverNamesFromSysfs();
        if (!names.isEmpty()) {
            source = "sysfs";
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-LNXFE] DRM driver probe: [%s] (source=%s)",
                names.join(',').toUtf8().constData(),
                source);

    return names;
}
#endif

QStringList WMUtils::getDrmDriverNames()
{
#ifdef Q_OS_LINUX
    // C++11 magic static：多執行緒下只會探測一次
    static const QStringList s_Names = probeDrmDriverNames();
    return s_Names;
#else
    return QStringList();
#endif
}

bool WMUtils::isDrmDriverMsm()
{
#ifdef Q_OS_LINUX
    for (const QString& name : getDrmDriverNames()) {
        // libdrm 回報的是 "msm"；sysfs 後備可能看到 platform driver 名稱
        // （msm_dpu / msm_mdss / msm-drm），無顯示輸出的 GPU-only 裝置則是 adreno
        if (name == "msm" || name.startsWith("msm_") || name.startsWith("msm-") || name == "adreno") {
            return true;
        }
    }
#endif
    return false;
}

#if defined(HAVE_EGL) && defined(Q_OS_LINUX)
// EGL_MESA_query_driver：eglGetDisplayDriverName（舊版 eglext.h 可能沒有宣告）
typedef const char* (EGLAPIENTRYP VIPLE_PFNEGLGETDISPLAYDRIVERNAMEPROC)(EGLDisplay dpy);
#endif

bool WMUtils::isEglZink()
{
#if defined(HAVE_EGL) && defined(Q_OS_LINUX)
    static SDL_atomic_t isZink;

    // If the value is not set yet, populate it now.
    int val = SDL_AtomicGet(&isZink);
    if (!(val & VALUE_SET)) {
        bool zink = false;
        QByteArray vendorName;
        QByteArray driverName;

        // 只用 GBM platform + EGL_DEFAULT_DISPLAY（與 supportsDesktopGLWithEGL()
        // 相同）。刻意不退到 eglGetDisplay(EGL_DEFAULT_DISPLAY)：那個 display
        // 可能正被 Qt eglfs / SDL 使用，這裡的 eglTerminate() 會把它一起拆掉。
        EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, EGL_DEFAULT_DISPLAY, nullptr);
        if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr)) {
            const char* vendorString = eglQueryString(display, EGL_VENDOR);
            if (vendorString != nullptr) {
                vendorName = vendorString;
                if (vendorName.toLower().contains("zink")) {
                    zink = true;
                }
            }

            // Mesa 的 EGL_VENDOR 一律是 "Mesa Project"，真正載入哪個 driver
            // 要靠 EGL_MESA_query_driver（Zink 回報 "zink"）
            const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
            if (extensions != nullptr && strstr(extensions, "EGL_MESA_query_driver") != nullptr) {
                auto getDisplayDriverName = reinterpret_cast<VIPLE_PFNEGLGETDISPLAYDRIVERNAMEPROC>(
                    eglGetProcAddress("eglGetDisplayDriverName"));
                if (getDisplayDriverName != nullptr) {
                    const char* name = getDisplayDriverName(display);
                    if (name != nullptr) {
                        // eglTerminate() 之後字串就失效，先複製
                        driverName = name;
                        if (driverName.toLower() == "zink") {
                            zink = true;
                        }
                    }
                }
            }

            eglTerminate(display);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-LNXFE] EGL probe: vendor='%s' driver='%s' zink=%d",
                    vendorName.constData(),
                    driverName.constData(),
                    zink ? 1 : 0);

        // Populate the value to return and have for next time.
        // This can race with another thread populating the same data,
        // but that's no big deal.
        val = VALUE_SET | (zink ? VALUE_TRUE : 0);
        SDL_AtomicSet(&isZink, val);
    }

    return !!(val & VALUE_TRUE);
#else
    return false;
#endif
}

void Utils::logDevEnvOverride(const char* name)
{
    if (name == nullptr || qEnvironmentVariableIsEmpty(name)) {
        return;
    }

    // 刻意配置在 heap 上且永不釋放：避免行程結束時 static destructor 的
    // 銷毀順序問題（見 CLAUDE.md 的 v337 QRegularExpression 事故）。
    static QMutex* s_Lock = new QMutex();
    static QSet<QByteArray>* s_Logged = new QSet<QByteArray>();

    const QByteArray key(name);
    {
        QMutexLocker locker(s_Lock);
        if (s_Logged->contains(key)) {
            return;
        }
        s_Logged->insert(key);
    }

    const QByteArray value = qgetenv(name);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-DEVENV] dev-only override %s=%s",
                name,
                value.constData());
}

void Utils::logDevEnvOverrides()
{
    static const char* const k_ExactNames[] = {
        "PREFER_VULKAN",
        "GL_IS_SLOW",
        "VULKAN_IS_SLOW",
        "MATCH_DISPLAY_MODE_TO_VIDEO",
        "SEPARATE_TEST_DECODER",
        "VIPLE_USE_VK_DECODER",
        "VIPLE_VK_FRUC_GENERIC",
    };

    for (const char* name : k_ExactNames) {
        logDevEnvOverride(name);
    }

#ifdef Q_OS_WIN
    // Windows 的環境變數名稱不分大小寫
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif

    const QStringList keys = QProcessEnvironment::systemEnvironment().keys();
    for (const QString& key : keys) {
        if (key.startsWith(QLatin1String("VIPLE_VKFRUC_"), cs) ||
                key.endsWith(QLatin1String("_AVOPTIONS"), cs) ||
                key.endsWith(QLatin1String("_DECODER_HINT"), cs)) {
            const QByteArray keyBytes = key.toLocal8Bit();
            logDevEnvOverride(keyBytes.constData());
        }
    }
}

QString WMUtils::getDrmCardOverride()
{
#ifdef HAVE_DRM
    QDir dir("/dev/dri");
    QStringList cardList = dir.entryList(QStringList("card*"), QDir::Files | QDir::System);
    if (cardList.length() == 0) {
        return QString();
    }

    bool needsOverride = false;
    for (const QString& card : cardList) {
        QFile cardFd(dir.filePath(card));
        if (!cardFd.open(QFile::ReadOnly)) {
            continue;
        }

        auto resources = drmModeGetResources(cardFd.handle());
        if (resources == nullptr) {
            // If we find a card that doesn't have a display before a card that
            // has one, we'll need to override Qt's EGLFS config because they
            // don't properly handle cards without displays.
            needsOverride = true;
        }
        else {
            // We found a card with a display
            drmModeFreeResources(resources);
            if (needsOverride) {
                // Override the default card with this one
                return dir.filePath(card);
            }
            else {
                return QString();
            }
        }
    }
#endif

    return QString();
}
