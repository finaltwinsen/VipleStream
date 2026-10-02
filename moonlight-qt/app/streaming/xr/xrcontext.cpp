// VipleStream 2.0 §VR M3a X1 — XrContext 實作。設計見 xrcontext.h。

#include "xrcontext.h"
#include "xrdesktopscreen.h"
#include <Limelight.h>  // M4a R2：VIPLE_VR_TRACKING（sampleControllers）
#include "streaming/vr/vrtracking.h"  // M4a R3：VrSampleHistory（MTP）

#ifdef HAVE_XR_VIDEO
#include "xrvideo.h"
#include <libplacebo/vulkan.h>
#endif

#include <QGuiApplication>
#include <QJsonArray>
#include <QtGlobal>

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <vector>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

const char* const kExtVulkan2 = "XR_KHR_vulkan_enable2";
const char* const kExtVulkan1 = "XR_KHR_vulkan_enable";
const char* const kExtRefresh = "XR_FB_display_refresh_rate";
const char* const kExtCylinder = "XR_KHR_composition_layer_cylinder";
const char* const kExtLocalFloor = "XR_EXT_local_floor";
// X4：Steam Frame 控制器（1.1.59 registry 未收錄；SteamVR 2.17.10 提供）
const char* const kExtFrameController = "XR_VALVE_frame_controller_interaction";
// M4a R1：XrTime ↔ client 單調時鐘（tracking thread 模式）
const char* const kExtTimespec = "XR_KHR_convert_timespec_time";
const char* const kExtQpc = "XR_KHR_win32_convert_performance_counter_time";
// XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR（1.1 核心／XR_EXT_local_floor 同值）；舊標頭沒有這個列舉值
const XrReferenceSpaceType kRefSpaceLocalFloor = static_cast<XrReferenceSpaceType>(1000426000);
// 自訂函式指標型別：避免為了 XR_USE_TIMESPEC／XR_USE_PLATFORM_WIN32 在標頭拉進 <time.h>／<windows.h>
typedef XrResult(XRAPI_PTR* PfnConvTimespec)(XrInstance, const struct timespec*, XrTime*);
typedef XrResult(XRAPI_PTR* PfnConvQpc)(XrInstance, const void* /* LARGE_INTEGER* */, XrTime*);

template <typename T>
T xrStruct(XrStructureType type)
{
    T s;
    std::memset(&s, 0, sizeof(s));
    s.type = type;
    return s;
}

template <typename T>
T vkStruct(VkStructureType type)
{
    T s;
    std::memset(&s, 0, sizeof(s));
    s.sType = type;
    return s;
}

template <typename PFN>
PFN xrProc(XrInstance instance, const char* name)
{
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) {
        return nullptr;
    }
    return reinterpret_cast<PFN>(fn);
}

QString xrResultStr(XrInstance instance, XrResult r)
{
    if (instance != XR_NULL_HANDLE) {
        char buf[XR_MAX_RESULT_STRING_SIZE] = {};
        if (XR_SUCCEEDED(xrResultToString(instance, r, buf))) {
            return QString::fromLatin1(buf);
        }
    }
    return QStringLiteral("XrResult(%1)").arg(static_cast<int>(r));
}

uint32_t vkApiFromXr(XrVersion v)
{
    const uint32_t major = static_cast<uint32_t>(XR_VERSION_MAJOR(v));
    const uint32_t minor = static_cast<uint32_t>(XR_VERSION_MINOR(v));
    return VK_MAKE_API_VERSION(0, major, minor, 0);
}

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

double radToDeg(float r)
{
    return static_cast<double>(r) * 180.0 / 3.14159265358979323846;
}

// §VR M3a X2：Vulkan feature 結構除了 sType／pNext 表頭之外全是 VkBool32。依 sType 對齊後逐欄位
// 合併「want（libplacebo 的 required／recommended）∩ supported」到 enable，不必逐一列舉欄位名。
struct FeatureSlot {
    VkStructureType type;
    VkBaseOutStructure* enable;
    const VkBaseOutStructure* supported;
    size_t size;
};

void mergeFeatureChain(const VkBaseInStructure* want, FeatureSlot* featSlots, int featSlotCount, int* missingRequired)
{
    // pNext 對齊到 8：x64 上表頭是 16 bytes（不是 4+8）
    const size_t header = offsetof(VkBaseOutStructure, pNext) + sizeof(void*);
    for (const VkBaseInStructure* w = want; w != nullptr; w = w->pNext) {
        for (int i = 0; i < featSlotCount; i++) {
            if (featSlots[i].type != w->sType) {
                continue;
            }
            // VkPhysicalDeviceFeatures2 的 features 成員也全是 VkBool32，同樣處理
            const size_t count = (featSlots[i].size - header) / sizeof(VkBool32);
            auto wantB = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(w) + header);
            auto supB = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(featSlots[i].supported) + header);
            auto enB = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(featSlots[i].enable) + header);
            for (size_t f = 0; f < count; f++) {
                if (wantB[f]) {
                    if (supB[f]) {
                        enB[f] = VK_TRUE;
                    }
                    else if (missingRequired) {
                        (*missingRequired)++;
                    }
                }
            }
        }
    }
}

}  // namespace

// ── Vulkan 函式表（instance／device 建立後填入）──
struct XrContextVk {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;  // M4a 收尾：有上限的 GPU 等待（1.2 core 或 KHR）
    PFN_vkResetFences ResetFences = nullptr;
    // 虛擬鍵盤貼圖上傳（staging buffer）
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
};

static XrContextVk s_Vk;  // 函式指標與 instance／device 無關的部分；每次 createVulkan 重填

XrContext::XrContext() : XrContext(Options()) {}

XrContext::XrContext(const Options& options)
    : m_Options(options), m_Screen(new XrDesktopScreen(options.quadDistanceM, options.quadFovDeg))
{
}

void XrContext::requestRecenter()
{
    m_Screen->requestRecenter();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] recenter requested");
}

void XrContext::requestKeyboardToggle()
{
    if (m_Input != nullptr) {
        m_Input->requestKeyboardToggle();
    }
}

XrContext::~XrContext()
{
    shutdown();
    delete m_Screen;
    m_Screen = nullptr;
}

PFN_vkGetInstanceProcAddr XrContext::loadVulkanLoader(QString* error)
{
#if defined(Q_OS_WIN)
    HMODULE lib = ::GetModuleHandleW(L"vulkan-1.dll");
    if (lib == nullptr) {
        lib = ::LoadLibraryW(L"vulkan-1.dll");
    }
    if (lib == nullptr) {
        if (error) *error = QStringLiteral("LoadLibrary(vulkan-1.dll) failed (%1)").arg(::GetLastError());
        return nullptr;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
    // 不 dlclose：runtime 建的 VkInstance 與它自己的 Vulkan 用途都還靠這份 loader
    static void* s_Lib = nullptr;
    if (s_Lib == nullptr) {
        s_Lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (s_Lib == nullptr) {
            const char* e = dlerror();
            if (error) *error = QStringLiteral("dlopen(libvulkan.so.1): ") + QString::fromLocal8Bit(e ? e : "failed");
            return nullptr;
        }
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(s_Lib, "vkGetInstanceProcAddr"));
#endif
    if (gipa == nullptr && error) {
        *error = QStringLiteral("vkGetInstanceProcAddr not found in the Vulkan loader");
    }
    return gipa;
}

const char* XrContext::sessionStateName(int state)
{
    switch (state) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

// ── bringUp ──

bool XrContext::bringUp(int timeoutMs, QString* error)
{
    if (m_Options.testFailBringUp) {
        // X5（dev）：--xr-test-fail bringup／loss3 的重建嘗試
        const QString err = QStringLiteral("test injection (--xr-test-fail)");
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] bring-up failed: %s", qUtf8Printable(err));
        if (error) *error = err;
        return false;
    }
    shutdown();  // 重入：清掉前一次

    QString err;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = createInstance(&err) && createSystem(&err) && createVulkan(&err);
#ifdef HAVE_XR_VIDEO
    if (ok && m_Options.enableVideo) {
        // X2：影像路徑失敗不影響 session（XrRenderer 會看到 video()==nullptr 而退回平面）
        XrVideo::Config vc;
        vc.dumpPath = m_Options.dumpFramePath;
        vc.dumpAfterFrames = m_Options.dumpAfterFrames;
        vc.testStallMs = m_Options.testStallMs;
        vc.requireMeta = m_Options.pcvr;  // M4a R4：PCVR 不發布無 render pose 的影像
        m_Video = new XrVideo(this, vc);
        QString verr;
        if (!m_Video->init(&verr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video path unavailable: %s", qUtf8Printable(verr));
            m_Video->destroy();
            delete m_Video;
            m_Video = nullptr;
        }
    }
#endif
    ok = ok && createSession(&err) && createQuadSwapchain(&err) && createActions(&err) &&
         waitForReadyAndBegin(timeoutMs, &err);
    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] bring-up failed: %s", qUtf8Printable(err));
        destroyAll();
        if (error) *error = err;
        return false;
    }

#ifdef HAVE_XR_VIDEO
    if (m_Video != nullptr) {
        m_Video->setSession(m_Session);  // X3：影像 render thread 用
    }
#endif
    m_StopRequested.store(false);
    m_Lost.store(false);
    m_WarmupEndNs = steadyNowNs() + 2000000000ull;
    m_BringUpNs = steadyNowNs();
    m_TestRecenterDone = false;
    m_TestFailFired = false;
    m_SimExit.store(false);
    m_MissEventsLogged = 0;
    m_QuadReady = false;
    m_StatusReady = false;
    m_PointerReady = false;
    m_StaleState = 0;
    m_LoggedLayerKind = false;
    m_FadeReady = false;
    m_LoggedProjection = false;
    m_PcvrStale = 0;
    m_PredictAheadNs = 0;
    {
        std::lock_guard<std::mutex> lk(m_HmdMutex);
        m_Hmd = HmdSample();
    }
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_HaveEyeToHead = false;
        m_StablePeriodFrames = 0;
        m_PeriodSettled = false;
        m_LateRefreshTries = 0;
        m_ProjFrames = 0;
        m_ProjNoMeta = 0;
    }
    m_FrameThreadRunning.store(true, std::memory_order_release);
    m_FrameThread = std::thread(&XrContext::frameThreadMain, this);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // 虛擬鍵盤貼圖：背景執行緒先畫無修飾鍵版本（暖字型，不阻塞 bring-up 與 XR thread）。
    // QPainter 畫字需要 QGuiApplication：xr-probe 在 QCoreApplication 下派發（M2a），沒有它就不畫鍵盤，
    // 否則 Qt Fatal「Must construct a QGuiApplication before accessing QFontDatabase」→ SIGABRT
    // （M4a R4 發現：xr-probe --session 自鍵盤 commit 起一律 rc=134）。
    const bool haveGuiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
    if (!haveGuiApp && !m_Options.pcvr && m_Input != nullptr && m_KbSwapchain != XR_NULL_HANDLE) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] keyboard disabled (no QGuiApplication, e.g. xr-probe)");
    }
    if (haveGuiApp && !m_Options.pcvr && m_Input != nullptr && m_KbSwapchain != XR_NULL_HANDLE && !m_KbRenderThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_KbRenderMutex);
            m_KbRenderCancel = false;
            m_KbRenderRequest = 0;
        }
        m_KbReadyMask.store(0);
        m_KbRenderThread = std::thread(&XrContext::keyboardRenderThreadMain, this);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] bring-up OK in %lld ms: session begun, frame thread started (quad %ux%u fmt=%lld, refresh=%.1f Hz%s%s%s)",
                static_cast<long long>(ms), m_Options.quadWidth, m_Options.quadHeight,
                static_cast<long long>(m_QuadFormat), static_cast<double>(m_RefreshHz),
                m_Options.pcvr ? ", pcvr space=" : "", m_Options.pcvr ? qUtf8Printable(m_TrackSpaceName) : "",
                m_Options.pcvr ? (m_TimeConv != nullptr ? " tracking=thread" : " tracking=frameloop") : "");
    return true;
}

bool XrContext::createInstance(QString* error)
{
    uint32_t count = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrEnumerateInstanceExtensionProperties: %1 (no OpenXR runtime?)").arg(xrResultStr(XR_NULL_HANDLE, r));
        return false;
    }
    std::vector<XrExtensionProperties> props(count, xrStruct<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrEnumerateInstanceExtensionProperties: %1").arg(xrResultStr(XR_NULL_HANDLE, r));
        return false;
    }
    auto has = [&](const char* name) {
        return std::any_of(props.begin(), props.end(), [&](const XrExtensionProperties& p) {
            return std::strcmp(p.extensionName, name) == 0;
        });
    };

    std::vector<const char*> enable;
    if (!has(kExtVulkan2)) {
        // TODO(M3a)：XR_KHR_vulkan_enable（舊式）備援。設計允許，但 Frame 與 S1／S2 都有 enable2。
        *error = QStringLiteral("runtime lacks %1 (vulkan_enable fallback=%2, not implemented yet)")
                     .arg(QLatin1String(kExtVulkan2)).arg(has(kExtVulkan1) ? 1 : 0);
        return false;
    }
    enable.push_back(kExtVulkan2);
    for (const char* opt : {kExtRefresh, kExtCylinder, kExtLocalFloor, kExtFrameController}) {
        if (has(opt)) {
            enable.push_back(opt);
        }
    }
    // M4a R1：tracking thread 模式要把 client 單調時鐘換成 XrTime
#if defined(Q_OS_WIN)
    const char* const timeExt = kExtQpc;
#else
    const char* const timeExt = kExtTimespec;
#endif
    const bool wantTime = m_Options.pcvr && has(timeExt);
    if (wantTime) {
        enable.push_back(timeExt);
    }

    XrInstanceCreateInfo ci = xrStruct<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    qstrncpy(ci.applicationInfo.applicationName, m_Options.applicationName, sizeof(ci.applicationInfo.applicationName));
    qstrncpy(ci.applicationInfo.engineName, "VipleStream", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.applicationVersion = 2;
    ci.applicationInfo.engineVersion = 2;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.data();

    // API 1.1 → 1.0（SteamVR 2.17 只支援 1.0；loader 會在 stderr 印一次 1.1 被拒）
#if defined(XR_API_VERSION_1_1)
    const XrVersion apiTry[] = {XR_API_VERSION_1_1, XR_API_VERSION_1_0};
#elif defined(XR_API_VERSION_1_0)
    const XrVersion apiTry[] = {XR_API_VERSION_1_0};
#else
    const XrVersion apiTry[] = {XR_MAKE_VERSION(1, 0, XR_VERSION_PATCH(XR_CURRENT_API_VERSION))};
#endif
    r = XR_ERROR_RUNTIME_UNAVAILABLE;
    for (XrVersion v : apiTry) {
        ci.applicationInfo.apiVersion = v;
        r = xrCreateInstance(&ci, &m_Instance);
        if (r != XR_ERROR_API_VERSION_UNSUPPORTED) {
            m_ApiVersion = v;
            break;
        }
    }
    if (XR_FAILED(r) || m_Instance == XR_NULL_HANDLE) {
        m_Instance = XR_NULL_HANDLE;
        *error = QStringLiteral("xrCreateInstance: %1").arg(xrResultStr(XR_NULL_HANDLE, r));
        return false;
    }

    m_EnabledExtensions.clear();
    for (const char* e : enable) {
        m_EnabledExtensions << QString::fromLatin1(e);
    }
    m_HasRefreshRate = has(kExtRefresh);
    m_TimeConv = nullptr;
    m_TimeConvQpc = false;
    if (wantTime) {
#if defined(Q_OS_WIN)
        m_TimeConv = reinterpret_cast<void*>(xrProc<PfnConvQpc>(m_Instance, "xrConvertWin32PerformanceCounterToTimeKHR"));
        m_TimeConvQpc = true;
#else
        m_TimeConv = reinterpret_cast<void*>(xrProc<PfnConvTimespec>(m_Instance, "xrConvertTimespecTimeToTimeKHR"));
#endif
    }

    XrInstanceProperties ip = xrStruct<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    xrGetInstanceProperties(m_Instance, &ip);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] instance: runtime=\"%s\" %u.%u.%u api=%u.%u enabled=[%s]",
                ip.runtimeName,
                static_cast<unsigned>(XR_VERSION_MAJOR(ip.runtimeVersion)),
                static_cast<unsigned>(XR_VERSION_MINOR(ip.runtimeVersion)),
                static_cast<unsigned>(XR_VERSION_PATCH(ip.runtimeVersion)),
                static_cast<unsigned>(XR_VERSION_MAJOR(m_ApiVersion)),
                static_cast<unsigned>(XR_VERSION_MINOR(m_ApiVersion)),
                qUtf8Printable(m_EnabledExtensions.join(QLatin1Char(','))));
    return true;
}

bool XrContext::createSystem(QString* error)
{
    XrSystemGetInfo gi = xrStruct<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(m_Instance, &gi, &m_SystemId);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrGetSystem(HMD): %1").arg(xrResultStr(m_Instance, r));
        return false;
    }
    XrSystemProperties sp = xrStruct<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    xrGetSystemProperties(m_Instance, m_SystemId, &sp);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] system: \"%s\" vendor=0x%x layers=%u",
                sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);
    return true;
}

bool XrContext::createVulkan(QString* error)
{
    auto pfnReq = xrProc<PFN_xrGetVulkanGraphicsRequirements2KHR>(m_Instance, "xrGetVulkanGraphicsRequirements2KHR");
    auto pfnCreateInst = xrProc<PFN_xrCreateVulkanInstanceKHR>(m_Instance, "xrCreateVulkanInstanceKHR");
    auto pfnGetDev = xrProc<PFN_xrGetVulkanGraphicsDevice2KHR>(m_Instance, "xrGetVulkanGraphicsDevice2KHR");
    auto pfnCreateDev = xrProc<PFN_xrCreateVulkanDeviceKHR>(m_Instance, "xrCreateVulkanDeviceKHR");
    if (!pfnReq || !pfnCreateInst || !pfnGetDev || !pfnCreateDev) {
        *error = QStringLiteral("vulkan_enable2 entry points missing");
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR req = xrStruct<XrGraphicsRequirementsVulkan2KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    XrResult r = pfnReq(m_Instance, m_SystemId, &req);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrGetVulkanGraphicsRequirements2KHR: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }

    m_Gipa = loadVulkanLoader(error);
    if (m_Gipa == nullptr) {
        return false;
    }

    // Vulkan 1.2（libplacebo 需要 timeline semaphore 等 1.2 功能），受 runtime 上限約束
    uint32_t api = VK_API_VERSION_1_2;
    const uint32_t maxApi = vkApiFromXr(req.maxApiVersionSupported);
    const uint32_t minApi = vkApiFromXr(req.minApiVersionSupported);
    if (maxApi != 0 && api > maxApi) api = maxApi;
    if (api < minApi) api = minApi;
    auto pfnEnumVer = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(m_Gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    uint32_t loaderApi = VK_API_VERSION_1_0;
    if (pfnEnumVer) pfnEnumVer(&loaderApi);
    if (api > loaderApi) api = loaderApi;

    VkApplicationInfo ai = vkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    ai.pApplicationName = m_Options.applicationName;
    ai.pEngineName = "VipleStream";
    ai.apiVersion = api;
    VkInstanceCreateInfo vci = vkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    vci.pApplicationInfo = &ai;

    XrVulkanInstanceCreateInfoKHR xci = xrStruct<XrVulkanInstanceCreateInfoKHR>(XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    xci.systemId = m_SystemId;
    xci.pfnGetInstanceProcAddr = m_Gipa;
    xci.vulkanCreateInfo = &vci;
    VkResult vr = VK_SUCCESS;
    r = pfnCreateInst(m_Instance, &xci, &m_VkInstance, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) {
        m_VkInstance = VK_NULL_HANDLE;
        *error = QStringLiteral("xrCreateVulkanInstanceKHR: %1 vk=%2").arg(xrResultStr(m_Instance, r)).arg(static_cast<int>(vr));
        return false;
    }
    m_VkApiVersion = api;

    s_Vk = XrContextVk();
#define LOAD_INST(name) s_Vk.name = reinterpret_cast<PFN_vk##name>(m_Gipa(m_VkInstance, "vk" #name))
    LOAD_INST(GetDeviceProcAddr);
    LOAD_INST(DestroyInstance);
    LOAD_INST(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INST(GetPhysicalDeviceProperties);
    LOAD_INST(GetPhysicalDeviceFeatures2);
    LOAD_INST(EnumerateDeviceExtensionProperties);
    LOAD_INST(GetPhysicalDeviceMemoryProperties);
#undef LOAD_INST

    XrVulkanGraphicsDeviceGetInfoKHR dgi = xrStruct<XrVulkanGraphicsDeviceGetInfoKHR>(XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    dgi.systemId = m_SystemId;
    dgi.vulkanInstance = m_VkInstance;
    r = pfnGetDev(m_Instance, &dgi, &m_VkPhysicalDevice);
    if (XR_FAILED(r) || m_VkPhysicalDevice == VK_NULL_HANDLE) {
        *error = QStringLiteral("xrGetVulkanGraphicsDevice2KHR: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }

    uint32_t qfCount = 0;
    s_Vk.GetPhysicalDeviceQueueFamilyProperties(m_VkPhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    s_Vk.GetPhysicalDeviceQueueFamilyProperties(m_VkPhysicalDevice, &qfCount, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_QueueFamily = i;
            found = true;
            break;
        }
    }
    if (!found) {
        *error = QStringLiteral("no graphics queue family on the runtime's GPU");
        return false;
    }

    VkPhysicalDeviceProperties pdp;
    std::memset(&pdp, 0, sizeof(pdp));
    s_Vk.GetPhysicalDeviceProperties(m_VkPhysicalDevice, &pdp);

    // X2：device 擴充與 features 依 libplacebo（pl_vulkan_import）需求啟用——required 必須全開，
    // recommended 有支援就開；外部記憶體擴充給 Linux／Frame 的 DRM_PRIME 匯入。只在 Vulkan 1.2
    // 以上掛 Vulkan11／12 feature 結構（libplacebo 最低 1.2，不足時 XrVideo 會失敗並退回平面）。
    const bool use12 = api >= VK_API_VERSION_1_2 && pdp.apiVersion >= VK_API_VERSION_1_2 && s_Vk.GetPhysicalDeviceFeatures2 != nullptr;
    VkPhysicalDeviceFeatures2 sup2 = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan11Features sup11 = vkStruct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features sup12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    m_EnFeat2 = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    m_EnF11 = vkStruct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    m_EnF12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    m_HaveFeatureChain = false;
    int missingRequired = 0;
    if (use12) {
        sup2.pNext = &sup11;
        sup11.pNext = &sup12;
        s_Vk.GetPhysicalDeviceFeatures2(m_VkPhysicalDevice, &sup2);
        m_EnFeat2.pNext = &m_EnF11;
        m_EnF11.pNext = &m_EnF12;
        FeatureSlot featSlots[] = {
            {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, reinterpret_cast<VkBaseOutStructure*>(&m_EnFeat2),
             reinterpret_cast<const VkBaseOutStructure*>(&sup2), sizeof(VkPhysicalDeviceFeatures2)},
            {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, reinterpret_cast<VkBaseOutStructure*>(&m_EnF11),
             reinterpret_cast<const VkBaseOutStructure*>(&sup11), sizeof(VkPhysicalDeviceVulkan11Features)},
            {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, reinterpret_cast<VkBaseOutStructure*>(&m_EnF12),
             reinterpret_cast<const VkBaseOutStructure*>(&sup12), sizeof(VkPhysicalDeviceVulkan12Features)},
        };
#ifdef HAVE_XR_VIDEO
        mergeFeatureChain(reinterpret_cast<const VkBaseInStructure*>(&pl_vulkan_required_features), featSlots, 3, &missingRequired);
        mergeFeatureChain(reinterpret_cast<const VkBaseInStructure*>(&pl_vulkan_recommended_features), featSlots, 3, nullptr);
#endif
        // X1 已開的兩項（沒有 libplacebo 的建置也保留）
        if (sup12.timelineSemaphore) m_EnF12.timelineSemaphore = VK_TRUE;
        if (sup12.hostQueryReset) m_EnF12.hostQueryReset = VK_TRUE;
        m_HaveFeatureChain = true;
    }

    // device 擴充：固定清單 ∩ 裝置支援（刻意不照抄 pl_vulkan_recommended_extensions——其中有些依賴
    // swapchain／video 擴充，沒一起開會讓 device 建立失敗）
    m_DevExts.clear();
    {
        static const char* const kWantDevExts[] = {
            "VK_KHR_push_descriptor",
            "VK_EXT_external_memory_host",
            "VK_KHR_external_memory_fd",
            "VK_EXT_external_memory_dma_buf",
            "VK_EXT_image_drm_format_modifier",
            "VK_KHR_external_semaphore_fd",
            "VK_EXT_physical_device_drm",
            "VK_KHR_image_format_list",
        };
        uint32_t extCount = 0;
        std::vector<VkExtensionProperties> avail;
        if (s_Vk.EnumerateDeviceExtensionProperties != nullptr &&
            s_Vk.EnumerateDeviceExtensionProperties(m_VkPhysicalDevice, nullptr, &extCount, nullptr) == VK_SUCCESS) {
            avail.resize(extCount);
            s_Vk.EnumerateDeviceExtensionProperties(m_VkPhysicalDevice, nullptr, &extCount, avail.data());
        }
        for (const char* want : kWantDevExts) {
            for (const VkExtensionProperties& e : avail) {
                if (std::strcmp(e.extensionName, want) == 0) {
                    m_DevExts.push_back(want);
                    break;
                }
            }
        }
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = vkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    qci.queueFamilyIndex = m_QueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = vkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(m_DevExts.size());
    dci.ppEnabledExtensionNames = m_DevExts.empty() ? nullptr : m_DevExts.data();
    if (m_HaveFeatureChain) {
        dci.pNext = &m_EnFeat2;  // features 走 pNext（pEnabledFeatures 必須為 null）
    }

    XrVulkanDeviceCreateInfoKHR xdci = xrStruct<XrVulkanDeviceCreateInfoKHR>(XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR);
    xdci.systemId = m_SystemId;
    xdci.pfnGetInstanceProcAddr = m_Gipa;
    xdci.vulkanPhysicalDevice = m_VkPhysicalDevice;
    xdci.vulkanCreateInfo = &dci;
    r = pfnCreateDev(m_Instance, &xdci, &m_VkDevice, &vr);
    if (XR_FAILED(r) || vr != VK_SUCCESS) {
        m_VkDevice = VK_NULL_HANDLE;
        *error = QStringLiteral("xrCreateVulkanDeviceKHR: %1 vk=%2").arg(xrResultStr(m_Instance, r)).arg(static_cast<int>(vr));
        return false;
    }

#define LOAD_DEV(name) s_Vk.name = reinterpret_cast<PFN_vk##name>(s_Vk.GetDeviceProcAddr(m_VkDevice, "vk" #name))
    LOAD_DEV(DestroyDevice);
    LOAD_DEV(GetDeviceQueue);
    LOAD_DEV(DeviceWaitIdle);
    LOAD_DEV(CreateCommandPool);
    LOAD_DEV(DestroyCommandPool);
    LOAD_DEV(AllocateCommandBuffers);
    LOAD_DEV(BeginCommandBuffer);
    LOAD_DEV(EndCommandBuffer);
    LOAD_DEV(ResetCommandBuffer);
    LOAD_DEV(CmdPipelineBarrier);
    LOAD_DEV(CmdClearColorImage);
    LOAD_DEV(QueueSubmit);
    LOAD_DEV(CreateFence);
    LOAD_DEV(DestroyFence);
    LOAD_DEV(WaitForFences);
    s_Vk.WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(s_Vk.GetDeviceProcAddr(m_VkDevice, "vkWaitSemaphores"));
    if (s_Vk.WaitSemaphores == nullptr) {
        s_Vk.WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(s_Vk.GetDeviceProcAddr(m_VkDevice, "vkWaitSemaphoresKHR"));
    }
    LOAD_DEV(ResetFences);
    LOAD_DEV(CreateBuffer);
    LOAD_DEV(DestroyBuffer);
    LOAD_DEV(GetBufferMemoryRequirements);
    LOAD_DEV(AllocateMemory);
    LOAD_DEV(FreeMemory);
    LOAD_DEV(BindBufferMemory);
    LOAD_DEV(MapMemory);
    LOAD_DEV(UnmapMemory);
    LOAD_DEV(CmdCopyBufferToImage);
#undef LOAD_DEV

    s_Vk.GetDeviceQueue(m_VkDevice, m_QueueFamily, 0, &m_Queue);

    VkCommandPoolCreateInfo pci = vkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_QueueFamily;
    if (s_Vk.CreateCommandPool(m_VkDevice, &pci, nullptr, &m_CmdPool) != VK_SUCCESS) {
        *error = QStringLiteral("vkCreateCommandPool failed");
        return false;
    }
    VkCommandBufferAllocateInfo cai = vkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    cai.commandPool = m_CmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (s_Vk.AllocateCommandBuffers(m_VkDevice, &cai, &m_Cmd) != VK_SUCCESS) {
        *error = QStringLiteral("vkAllocateCommandBuffers failed");
        return false;
    }
    VkFenceCreateInfo fci = vkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    if (s_Vk.CreateFence(m_VkDevice, &fci, nullptr, &m_Fence) != VK_SUCCESS) {
        *error = QStringLiteral("vkCreateFence failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] vulkan: gpu=\"%s\" api=%u.%u (runtime %u.%u-%u.%u) queueFamily=%u timeline=%d devExts=%d missingRequiredFeatures=%d",
                pdp.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                VK_API_VERSION_MAJOR(minApi), VK_API_VERSION_MINOR(minApi),
                VK_API_VERSION_MAJOR(maxApi), VK_API_VERSION_MINOR(maxApi),
                m_QueueFamily, m_HaveFeatureChain ? static_cast<int>(m_EnF12.timelineSemaphore) : 0,
                static_cast<int>(m_DevExts.size()), missingRequired);
    return true;
}

bool XrContext::createSession(QString* error)
{
    XrGraphicsBindingVulkan2KHR binding = xrStruct<XrGraphicsBindingVulkan2KHR>(XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR);
    binding.instance = m_VkInstance;
    binding.physicalDevice = m_VkPhysicalDevice;
    binding.device = m_VkDevice;
    binding.queueFamilyIndex = m_QueueFamily;
    binding.queueIndex = 0;

    XrSessionCreateInfo sci = xrStruct<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    sci.next = &binding;
    sci.systemId = m_SystemId;
    XrResult r = xrCreateSession(m_Instance, &sci, &m_Session);
    if (XR_FAILED(r)) {
        m_Session = XR_NULL_HANDLE;
        *error = QStringLiteral("xrCreateSession: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }

    uint32_t n = 0;
    xrEnumerateReferenceSpaces(m_Session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces(m_Session, n, &n, spaces.data());
    m_ReferenceSpaces.clear();
    for (XrReferenceSpaceType t : spaces) {
        switch (t) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: m_ReferenceSpaces << QStringLiteral("VIEW"); break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL: m_ReferenceSpaces << QStringLiteral("LOCAL"); break;
        case XR_REFERENCE_SPACE_TYPE_STAGE: m_ReferenceSpaces << QStringLiteral("STAGE"); break;
        case kRefSpaceLocalFloor: m_ReferenceSpaces << QStringLiteral("LOCAL_FLOOR"); break;
        default: m_ReferenceSpaces << QStringLiteral("0x%1").arg(static_cast<int>(t), 0, 16); break;
        }
    }

    // X1 一律 LOCAL（STAGE／LOCAL_FLOOR 的優先順序在 X3／M4a 決定，這裡只記錄支援清單）
    XrReferenceSpaceCreateInfo rci = xrStruct<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(m_Session, &rci, &m_LocalSpace);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrCreateReferenceSpace(LOCAL): %1").arg(xrResultStr(m_Instance, r));
        return false;
    }
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(m_Session, &rci, &m_ViewSpace))) {
        m_ViewSpace = XR_NULL_HANDLE;  // head-locked quad 用；X1 不強制
    }

    // M4a R1：PCVR 的追蹤／投影參考空間 STAGE → LOCAL_FLOOR → LOCAL（設計「參考空間」）
    m_TrackSpaceName.clear();
    if (m_Options.pcvr) {
        if (m_ViewSpace == XR_NULL_HANDLE) {
            *error = QStringLiteral("PCVR needs the VIEW reference space");
            return false;
        }
        const bool haveStage = std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end();
        const bool haveFloor = std::find(spaces.begin(), spaces.end(), kRefSpaceLocalFloor) != spaces.end();
        struct Cand { XrReferenceSpaceType t; const char* name; bool ok; };
        const Cand cands[] = {{XR_REFERENCE_SPACE_TYPE_STAGE, "STAGE", haveStage},
                              {kRefSpaceLocalFloor, "LOCAL_FLOOR", haveFloor},
                              {XR_REFERENCE_SPACE_TYPE_LOCAL, "LOCAL", true}};
        for (const Cand& c : cands) {
            if (!c.ok) {
                continue;
            }
            rci.referenceSpaceType = c.t;
            if (XR_SUCCEEDED(xrCreateReferenceSpace(m_Session, &rci, &m_TrackSpace))) {
                m_TrackSpaceName = QString::fromLatin1(c.name);
                break;
            }
            m_TrackSpace = XR_NULL_HANDLE;
        }
        if (m_TrackSpace == XR_NULL_HANDLE) {
            *error = QStringLiteral("PCVR: no usable tracking reference space");
            return false;
        }
        uint32_t vc = 0;
        xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
        if (vc >= 2) {
            std::vector<XrViewConfigurationView> v(vc, xrStruct<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
            if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                              vc, &vc, v.data()))) {
                m_RecW = v[0].recommendedImageRectWidth;
                m_RecH = v[0].recommendedImageRectHeight;
            }
        }
    }

    m_RequestedHz = 0.0f;
    if (m_HasRefreshRate) {
        auto pfnRate = xrProc<PFN_xrGetDisplayRefreshRateFB>(m_Instance, "xrGetDisplayRefreshRateFB");
        if (pfnRate) {
            float hz = 0.0f;
            pfnRate(m_Session, &hz);
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            m_RefreshHz = hz;
        }
        // M4a 收尾（Frame 實測）：xrGetDisplayRefreshRateFB 回 120、實際 predictedDisplayPeriod 卻是 13.89 ms
        // （72 Hz），/launch 照 120 要、server 白做工。這裡明確要求一個可用值（最接近 preferredRefreshHz），
        // waitViews 再以量到的週期把關。
        auto pfnEnum = xrProc<PFN_xrEnumerateDisplayRefreshRatesFB>(m_Instance, "xrEnumerateDisplayRefreshRatesFB");
        auto pfnReq = xrProc<PFN_xrRequestDisplayRefreshRateFB>(m_Instance, "xrRequestDisplayRefreshRateFB");
        uint32_t n = 0;
        if (m_Options.preferredRefreshHz > 0.0f && pfnEnum && pfnReq && XR_SUCCEEDED(pfnEnum(m_Session, 0, &n, nullptr)) &&
            n > 0) {
            std::vector<float> rates(n, 0.0f);
            if (XR_SUCCEEDED(pfnEnum(m_Session, n, &n, rates.data())) && n > 0) {
                rates.resize(n);
                const float want = m_Options.preferredRefreshHz;
                float best = rates[0];
                QStringList names;
                for (float r : rates) {
                    names << QString::number(static_cast<double>(r), 'f', 1);
                    const float dr = std::fabs(r - want), db = std::fabs(best - want);
                    if (dr < db || (dr == db && r > best)) {
                        best = r;
                    }
                }
                const XrResult rr = pfnReq(m_Session, best);
                if (XR_SUCCEEDED(rr)) {
                    m_RequestedHz = best;
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-XR] display refresh rates=[%s] current=%.1f Hz -> requested %.1f Hz (want %.1f): %s",
                            qUtf8Printable(names.join(QLatin1Char(','))), static_cast<double>(m_RefreshHz),
                            static_cast<double>(best), static_cast<double>(want), qUtf8Printable(xrResultStr(m_Instance, rr)));
            }
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] session created: spaces=[%s] refresh=%.1f Hz",
                qUtf8Printable(m_ReferenceSpaces.join(QLatin1Char(','))), static_cast<double>(m_RefreshHz));
    return true;
}

bool XrContext::createQuadSwapchain(QString* error)
{
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(m_Session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(m_Session, n, &n, formats.data());
    const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    m_QuadFormat = 0;
    for (int64_t want : preferred) {
        if (std::find(formats.begin(), formats.end(), want) != formats.end()) {
            m_QuadFormat = want;
            break;
        }
    }
    if (m_QuadFormat == 0) {
        if (formats.empty()) {
            *error = QStringLiteral("runtime offers no swapchain formats");
            return false;
        }
        m_QuadFormat = formats[0];
    }

    XrSwapchainCreateInfo ci = xrStruct<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = m_QuadFormat;
    ci.sampleCount = 1;
    ci.width = m_Options.quadWidth;
    ci.height = m_Options.quadHeight;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(m_Session, &ci, &m_QuadSwapchain);
    if (XR_FAILED(r)) {
        m_QuadSwapchain = XR_NULL_HANDLE;
        *error = QStringLiteral("xrCreateSwapchain(quad %1x%2 fmt=%3): %4")
                     .arg(ci.width).arg(ci.height).arg(m_QuadFormat).arg(xrResultStr(m_Instance, r));
        return false;
    }
    xrEnumerateSwapchainImages(m_QuadSwapchain, 0, &m_QuadImageCount, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(m_QuadImageCount,
                                                 xrStruct<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
    r = xrEnumerateSwapchainImages(m_QuadSwapchain, m_QuadImageCount, &m_QuadImageCount,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    if (XR_FAILED(r) || m_QuadImageCount == 0) {
        *error = QStringLiteral("xrEnumerateSwapchainImages: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }
    m_QuadImages = new VkImage[m_QuadImageCount];
    for (uint32_t i = 0; i < m_QuadImageCount; i++) {
        m_QuadImages[i] = imgs[i].image;
    }

    // X3：stale 狀態 quad（小色條）。建不起來不影響 session，只是沒有 stale 提示
    ci.width = kStatusW;
    ci.height = kStatusH;
    r = xrCreateSwapchain(m_Session, &ci, &m_StatusSwapchain);
    if (XR_SUCCEEDED(r)) {
        xrEnumerateSwapchainImages(m_StatusSwapchain, 0, &m_StatusImageCount, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> simgs(m_StatusImageCount,
                                                      xrStruct<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
        r = xrEnumerateSwapchainImages(m_StatusSwapchain, m_StatusImageCount, &m_StatusImageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(simgs.data()));
        if (XR_SUCCEEDED(r) && m_StatusImageCount > 0) {
            m_StatusImages = new VkImage[m_StatusImageCount];
            for (uint32_t i = 0; i < m_StatusImageCount; i++) {
                m_StatusImages[i] = simgs[i].image;
            }
        }
        else {
            xrDestroySwapchain(m_StatusSwapchain);
            m_StatusSwapchain = XR_NULL_HANDLE;
            m_StatusImageCount = 0;
        }
    }
    else {
        m_StatusSwapchain = XR_NULL_HANDLE;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] status quad swapchain unavailable: %s",
                    xrResultStr(m_Instance, r).toUtf8().constData());
    }

    // X4：指標 quad（白色小方塊）。建不起來只是沒有指標，輸入照常
    ci.width = kPointerPx;
    ci.height = kPointerPx;
    r = xrCreateSwapchain(m_Session, &ci, &m_PointerSwapchain);
    if (XR_SUCCEEDED(r)) {
        xrEnumerateSwapchainImages(m_PointerSwapchain, 0, &m_PointerImageCount, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> pimgs(m_PointerImageCount,
                                                      xrStruct<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
        r = xrEnumerateSwapchainImages(m_PointerSwapchain, m_PointerImageCount, &m_PointerImageCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(pimgs.data()));
        if (XR_SUCCEEDED(r) && m_PointerImageCount > 0) {
            m_PointerImages = new VkImage[m_PointerImageCount];
            for (uint32_t i = 0; i < m_PointerImageCount; i++) {
                m_PointerImages[i] = pimgs[i].image;
            }
        }
        else {
            xrDestroySwapchain(m_PointerSwapchain);
            m_PointerSwapchain = XR_NULL_HANDLE;
            m_PointerImageCount = 0;
        }
    }
    else {
        m_PointerSwapchain = XR_NULL_HANDLE;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] pointer quad swapchain unavailable: %s",
                    xrResultStr(m_Instance, r).toUtf8().constData());
    }

    // 虛擬鍵盤：貼圖（尺寸同 XrKeyboard）＋hover／按下高亮。建不起來只是沒有鍵盤，其他照常
    {
        const XrKeyboard kb;
        createSolidSwapchain(static_cast<uint32_t>(kb.width()), static_cast<uint32_t>(kb.height()), &m_KbSwapchain,
                             &m_KbImages, &m_KbImageCount, "keyboard");
        createSolidSwapchain(8, 8, &m_KbHoverSwapchain, &m_KbHoverImages, &m_KbHoverImageCount, "keyboard hover");
        createSolidSwapchain(8, 8, &m_KbPressSwapchain, &m_KbPressImages, &m_KbPressImageCount, "keyboard press");
    }
    // M4a R1：PCVR stale 淡出（head-locked 半透明黑，預乘 alpha）
    if (m_Options.pcvr) {
        createSolidSwapchain(8, 8, &m_FadeSwapchain, &m_FadeImages, &m_FadeImageCount, "pcvr fade");
    }
    return true;
}

bool XrContext::createSolidSwapchain(uint32_t w, uint32_t h, XrSwapchain* sc, VkImage** images, uint32_t* count,
                                     const char* what)
{
    XrSwapchainCreateInfo ci = xrStruct<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = m_QuadFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(m_Session, &ci, sc);
    if (XR_FAILED(r)) {
        *sc = XR_NULL_HANDLE;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] %s swapchain unavailable: %s", what,
                    xrResultStr(m_Instance, r).toUtf8().constData());
        return false;
    }
    xrEnumerateSwapchainImages(*sc, 0, count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(*count, xrStruct<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
    r = xrEnumerateSwapchainImages(*sc, *count, count, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    if (XR_FAILED(r) || *count == 0) {
        xrDestroySwapchain(*sc);
        *sc = XR_NULL_HANDLE;
        *count = 0;
        return false;
    }
    *images = new VkImage[*count];
    for (uint32_t i = 0; i < *count; i++) {
        (*images)[i] = imgs[i].image;
    }
    return true;
}

bool XrContext::createActions(QString* error)
{
    if (m_Options.pcvr) {
        // M4a R2：PCVR 的控制器（grip pose、按鍵、haptic）→ 0x5506
        m_VrCtl = new XrVrControllers(m_Options.testVrInput);
        if (!m_VrCtl->create(m_Instance, m_Session, m_EnabledExtensions, error)) {
            return false;
        }
        m_ActionSet = m_VrCtl->actionSet();
        return true;
    }
    // X4：action set、actions、各 profile 綁定、attach 與 aim space 全交給 XrInput
    m_Input = new XrInput(m_Options.inputSink, m_Options.testPointer, m_Options.testKeyboardText);
    if (!m_Input->create(m_Instance, m_Session, m_EnabledExtensions, error)) {
        return false;
    }
    m_ActionSet = m_Input->actionSet();
    return true;
}

bool XrContext::waitForReadyAndBegin(int timeoutMs, QString* error)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!m_SessionBegun.load()) {
        if (!pollEvents()) {
            *error = QStringLiteral("session lost before READY (state=%1)").arg(QLatin1String(sessionStateName(m_State.load())));
            return false;
        }
        if (m_SessionBegun.load()) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            *error = QStringLiteral("timed out after %1 ms waiting for READY (state=%2)")
                         .arg(timeoutMs).arg(QLatin1String(sessionStateName(m_State.load())));
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// ── 事件與狀態機（bringUp 期間在呼叫端執行緒，之後只在 frame thread）──

bool XrContext::pollEvents()
{
    for (;;) {
        XrEventDataBuffer ev = xrStruct<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        XrResult r = xrPollEvent(m_Instance, &ev);
        if (r != XR_SUCCESS) {
            break;  // XR_EVENT_UNAVAILABLE 或錯誤
        }
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] instance loss pending");
            m_Lost.store(true);
            return false;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            handleStateChange(sc->state);
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] runtime event queue overflowed");
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            auto* rc = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] display refresh rate changed %.1f -> %.1f Hz",
                        static_cast<double>(rc->fromDisplayRefreshRate), static_cast<double>(rc->toDisplayRefreshRate));
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            m_RefreshHz = rc->toDisplayRefreshRate;
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            if (m_Input != nullptr) {
                m_Input->onProfileChanged();
            }
            if (m_VrCtl != nullptr) {
                m_VrCtl->onProfileChanged();
            }
            break;
        default:
            break;
        }
    }
    const int s = m_State.load();
    return !m_Lost.load() && s != XR_SESSION_STATE_EXITING && s != XR_SESSION_STATE_LOSS_PENDING;
}

void XrContext::handleStateChange(XrSessionState state)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] session state %s -> %s",
                sessionStateName(m_State.load()), sessionStateName(state));
    m_State.store(state);
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_Stats.currentState = state;
        if (state <= XR_SESSION_STATE_FOCUSED && state > m_Stats.highestState) {
            m_Stats.highestState = state;
        }
    }
    switch (state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi = xrStruct<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XrResult r = xrBeginSession(m_Session, &bi);
        if (XR_SUCCEEDED(r)) {
            m_SessionBegun.store(true);
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] xrBeginSession: %s",
                        qUtf8Printable(xrResultStr(m_Instance, r)));
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        if (m_SessionBegun.load()) {
            xrEndSession(m_Session);
            m_SessionBegun.store(false);
        }
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
        m_Lost.store(true);
        break;
    default:
        break;
    }
}

// ── frame thread ──

bool XrContext::clearImage(VkImage img, float r, float g, float b, float a)
{
    s_Vk.ResetCommandBuffer(m_Cmd, 0);
    VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s_Vk.BeginCommandBuffer(m_Cmd, &bi);

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // XR_KHR_vulkan_enable：acquire 後影像在 COLOR_ATTACHMENT_OPTIMAL，release 前也要回到這個 layout
    VkImageMemoryBarrier bar = vkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img;
    bar.subresourceRange = range;
    s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &bar);

    VkClearColorValue gray;
    std::memset(&gray, 0, sizeof(gray));
    gray.float32[0] = r;
    gray.float32[1] = g;
    gray.float32[2] = b;
    gray.float32[3] = a;
    s_Vk.CmdClearColorImage(m_Cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &gray, 1, &range);

    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &bar);
    s_Vk.EndCommandBuffer(m_Cmd);

    VkSubmitInfo si = vkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m_Cmd;
    VkResult vr;
    {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        vr = s_Vk.QueueSubmit(m_Queue, 1, &si, m_Fence);
    }
    if (vr != VK_SUCCESS) {
        return false;
    }
    s_Vk.WaitForFences(m_VkDevice, 1, &m_Fence, VK_TRUE, 1000000000ull);
    s_Vk.ResetFences(m_VkDevice, 1, &m_Fence);
    return true;
}

bool XrContext::ensureSolidQuad(XrSwapchain sc, VkImage* images, uint32_t count, bool* ready,
                                float r, float g, float b, float a)
{
    if (*ready) {
        return true;  // runtime 沿用最後 release 的影像
    }
    if (sc == XR_NULL_HANDLE || images == nullptr) {
        return false;
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = xrStruct<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    XrSwapchainImageWaitInfo swi = xrStruct<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    swi.timeout = 100000000;  // 100 ms
    XrSwapchainImageReleaseInfo ri = xrStruct<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    XrResult ar;
    {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        ar = xrAcquireSwapchainImage(sc, &ai, &idx);
    }
    if (XR_FAILED(ar) || XR_FAILED(xrWaitSwapchainImage(sc, &swi))) {
        return false;
    }
    const bool drawn = idx < count && clearImage(images[idx], r, g, b, a);
    {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        xrReleaseSwapchainImage(sc, &ri);
    }
    *ready = drawn;
    return drawn;
}

void XrContext::keyboardRenderThreadMain()
{
    const XrKeyboard kb;
    bool first = true;
    for (;;) {
        int m = -1;
        {
            std::unique_lock<std::mutex> lk(m_KbRenderMutex);
            m_KbRenderCv.wait(lk, [this]() { return m_KbRenderCancel || m_KbRenderRequest >= 0; });
            if (m_KbRenderCancel) {
                return;
            }
            m = m_KbRenderRequest;
            m_KbRenderRequest = -1;
        }
        if (m_KbReadyMask.load(std::memory_order_acquire) & (1u << m)) {
            continue;
        }
        const uint64_t t0 = steadyNowNs();
        m_KbCache[m] = kb.render(static_cast<uint8_t>(m));
        m_KbReadyMask.fetch_or(1u << m, std::memory_order_release);
        if (first) {
            first = false;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] keyboard texture ready in %.0f ms (first render warms up fonts)",
                        static_cast<double>(steadyNowNs() - t0) / 1e6);
        }
    }
}

bool XrContext::uploadImage(XrSwapchain sc, VkImage* images, uint32_t count, const QImage& src)
{
    const bool bgra = m_QuadFormat == VK_FORMAT_B8G8R8A8_SRGB || m_QuadFormat == VK_FORMAT_B8G8R8A8_UNORM;
    const bool rgba = m_QuadFormat == VK_FORMAT_R8G8B8A8_SRGB || m_QuadFormat == VK_FORMAT_R8G8B8A8_UNORM;
    if (sc == XR_NULL_HANDLE || images == nullptr || (!bgra && !rgba) || s_Vk.CreateBuffer == nullptr) {
        return false;
    }
    // Format_ARGB32 在記憶體裡是 B,G,R,A（little-endian）；UNORM 格式會被 runtime 當線性值（稍亮），SRGB 正確
    const QImage img = src.convertToFormat(bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    const uint32_t w = static_cast<uint32_t>(img.width());
    const uint32_t h = static_cast<uint32_t>(img.height());
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    if (m_StagingSize < size) {
        if (m_Staging != VK_NULL_HANDLE) {
            s_Vk.UnmapMemory(m_VkDevice, m_StagingMem);
            s_Vk.DestroyBuffer(m_VkDevice, m_Staging, nullptr);
            s_Vk.FreeMemory(m_VkDevice, m_StagingMem, nullptr);
            m_Staging = VK_NULL_HANDLE;
            m_StagingMem = VK_NULL_HANDLE;
            m_StagingPtr = nullptr;
            m_StagingSize = 0;
        }
        VkBufferCreateInfo bci = vkStruct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (s_Vk.CreateBuffer(m_VkDevice, &bci, nullptr, &m_Staging) != VK_SUCCESS) {
            m_Staging = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req = {};
        s_Vk.GetBufferMemoryRequirements(m_VkDevice, m_Staging, &req);
        VkPhysicalDeviceMemoryProperties mp = {};
        s_Vk.GetPhysicalDeviceMemoryProperties(m_VkPhysicalDevice, &mp);
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
                type = t;
                break;
            }
        }
        VkMemoryAllocateInfo mai = vkStruct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        if (type == UINT32_MAX || s_Vk.AllocateMemory(m_VkDevice, &mai, nullptr, &m_StagingMem) != VK_SUCCESS ||
            s_Vk.BindBufferMemory(m_VkDevice, m_Staging, m_StagingMem, 0) != VK_SUCCESS ||
            s_Vk.MapMemory(m_VkDevice, m_StagingMem, 0, VK_WHOLE_SIZE, 0, &m_StagingPtr) != VK_SUCCESS) {
            if (m_StagingMem != VK_NULL_HANDLE) {
                s_Vk.FreeMemory(m_VkDevice, m_StagingMem, nullptr);
            }
            s_Vk.DestroyBuffer(m_VkDevice, m_Staging, nullptr);
            m_Staging = VK_NULL_HANDLE;
            m_StagingMem = VK_NULL_HANDLE;
            m_StagingPtr = nullptr;
            return false;
        }
        m_StagingSize = size;
    }
    for (uint32_t y = 0; y < h; y++) {
        std::memcpy(static_cast<uint8_t*>(m_StagingPtr) + static_cast<size_t>(y) * w * 4, img.constScanLine(static_cast<int>(y)),
                    static_cast<size_t>(w) * 4);
    }

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = xrStruct<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    XrSwapchainImageWaitInfo swi = xrStruct<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    swi.timeout = 100000000;  // 100 ms
    XrSwapchainImageReleaseInfo ri = xrStruct<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    XrResult ar;
    {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        ar = xrAcquireSwapchainImage(sc, &ai, &idx);
    }
    if (XR_FAILED(ar) || XR_FAILED(xrWaitSwapchainImage(sc, &swi))) {
        return false;
    }
    bool ok = idx < count;
    if (ok) {
        const VkImage dst = images[idx];
        s_Vk.ResetCommandBuffer(m_Cmd, 0);
        VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        s_Vk.BeginCommandBuffer(m_Cmd, &bi);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier bar = vkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = dst;
        bar.subresourceRange = range;
        s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        s_Vk.CmdCopyBufferToImage(m_Cmd, m_Staging, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                0, 0, nullptr, 0, nullptr, 1, &bar);
        s_Vk.EndCommandBuffer(m_Cmd);
        VkSubmitInfo si = vkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_Cmd;
        VkResult vr;
        {
            std::lock_guard<std::mutex> lk(m_QueueMutex);
            vr = s_Vk.QueueSubmit(m_Queue, 1, &si, m_Fence);
        }
        ok = vr == VK_SUCCESS;
        if (ok) {
            s_Vk.WaitForFences(m_VkDevice, 1, &m_Fence, VK_TRUE, 1000000000ull);
            s_Vk.ResetFences(m_VkDevice, 1, &m_Fence);
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        xrReleaseSwapchainImage(sc, &ri);
    }
    return ok;
}

void XrContext::maybeLogStats(uint64_t nowNs)
{
    if (m_StatsWindowStartNs == 0) {
        m_StatsWindowStartNs = nowNs;
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_StatsAtWindowStart = m_Stats;
        return;
    }
    if (nowNs - m_StatsWindowStartNs < 10000000000ull) {
        return;
    }
    Stats cur, prev;
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        cur = m_Stats;
        prev = m_StatsAtWindowStart;
        m_StatsAtWindowStart = m_Stats;
    }
    m_StatsWindowStartNs = nowNs;
    Stats d;
    d.frames = cur.frames - prev.frames;
    d.missed = cur.missed - prev.missed;
    double cpuP50 = 0.0, cpuP95 = 0.0;
    if (!m_XrCpuUs.empty()) {
        std::vector<uint32_t> v;
        v.swap(m_XrCpuUs);
        std::sort(v.begin(), v.end());
        cpuP50 = v[v.size() / 2] / 1000.0;
        cpuP95 = v[(std::min)(v.size() - 1, (v.size() * 95) / 100)] / 1000.0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] 10s frames=%llu missed=%llu (%.2f%%) notRendered=%llu endFrameErr=%llu state=%s period=%.2f ms "
                "xrThread cpu p50=%.2f p95=%.2f ms stale=%d",
                static_cast<unsigned long long>(d.frames), static_cast<unsigned long long>(d.missed), d.missPercent(),
                static_cast<unsigned long long>(cur.notRendered - prev.notRendered),
                static_cast<unsigned long long>(cur.endFrameErrors - prev.endFrameErrors),
                sessionStateName(cur.currentState), static_cast<double>(m_LastPeriod) / 1e6,
                cpuP50, cpuP95, m_StaleState);
#ifdef HAVE_XR_VIDEO
    if (m_Video != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] 10s video %s", qUtf8Printable(m_Video->takeStatsLine()));
    }
#endif
    if (m_Options.pcvr) {
        uint64_t proj = 0, noMeta = 0;
        {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            proj = m_ProjFrames;
            noMeta = m_ProjNoMeta;
            m_ProjFrames = 0;
            m_ProjNoMeta = 0;
        }
        HmdSample h;
        {
            std::lock_guard<std::mutex> lk(m_HmdMutex);
            h = m_Hmd;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] 10s pcvr projection=%llu noMeta=%llu state=%d space=%s tracking=%s predictAhead=%.1f ms "
                    "hmd=(%.3f, %.3f, %.3f | %.3f, %.3f, %.3f, %.3f)",
                    static_cast<unsigned long long>(proj), static_cast<unsigned long long>(noMeta), m_PcvrStale,
                    qUtf8Printable(m_TrackSpaceName), m_TimeConv != nullptr ? "thread" : "frameloop",
                    static_cast<double>(m_PredictAheadNs) / 1e6, static_cast<double>(h.pos[0]),
                    static_cast<double>(h.pos[1]), static_cast<double>(h.pos[2]), static_cast<double>(h.rot[0]),
                    static_cast<double>(h.rot[1]), static_cast<double>(h.rot[2]), static_cast<double>(h.rot[3]));
    }
    if (!m_Options.pcvr && m_Input != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] 10s %s", qUtf8Printable(m_Input->takeStatsLine()));
    }
    if (m_VrCtl != nullptr) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] 10s %s", qUtf8Printable(m_VrCtl->takeStatsLine()));
    }
}

void XrContext::frameThreadMain()
{
    XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    {
        uint32_t n = 0;
        xrEnumerateEnvironmentBlendModes(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        if (n > 0) {
            std::vector<XrEnvironmentBlendMode> modes(n);
            xrEnumerateEnvironmentBlendModes(m_Instance, m_SystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
            if (std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == modes.end()) {
                blend = modes[0];
            }
        }
    }

    bool exitRequested = false;
    uint64_t lastViewLocateNs = 0;
    while (true) {
        if (m_StopRequested.load()) {
            break;
        }
        if (!pollEvents()) {
            break;  // EXITING／LOSS_PENDING／instance loss
        }
        // X5（dev）：--xr-test-fail loss／loss3／exit
        if (m_Options.testFailAfterSec > 0 && !m_TestFailFired &&
            steadyNowNs() - m_BringUpNs > static_cast<uint64_t>(m_Options.testFailAfterSec) * 1000000000ull) {
            m_TestFailFired = true;
            if (m_Options.testFailKind == kEndedLoss) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] test: simulating LOSS_PENDING (--xr-test-fail)");
                m_Lost.store(true);
                break;
            }
            if (m_Options.testFailKind == kTestGpuWedge) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] test: simulating a GPU wait timeout (--xr-test-fail gpuwedge)");
                markGpuWedged("test injection (--xr-test-fail gpuwedge)");
                m_Lost.store(true);
                break;
            }
            if (m_Options.testFailKind == kEndedExit && m_SessionBegun.load()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] test: simulating a runtime-initiated exit (--xr-test-fail)");
                m_SimExit.store(true);
                xrRequestExitSession(m_Session);
            }
        }
        const int state = m_State.load();
        if (m_SimExit.load() && !m_SessionBegun.load() &&
            (state == XR_SESSION_STATE_IDLE || state == XR_SESSION_STATE_EXITING)) {
            break;
        }
        if (exitRequested && !m_SessionBegun.load() &&
            (state == XR_SESSION_STATE_IDLE || state == XR_SESSION_STATE_EXITING)) {
            break;
        }
        if (!exitRequested && m_ExitRequested.load()) {
            exitRequested = true;
            if (m_SessionBegun.load()) {
                xrRequestExitSession(m_Session);
            }
        }
        if (!m_SessionBegun.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // SteamVR 要 app 每幀 sync actions 才會給輸入焦點（FOCUSED）；未 FOCUSED 時回
        // XR_SESSION_NOT_FOCUSED（成功碼），不影響 frame loop
        if (m_ActionSet != XR_NULL_HANDLE) {
            XrActiveActionSet active = {m_ActionSet, XR_NULL_PATH};
            XrActionsSyncInfo syi = xrStruct<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
            syi.countActiveActionSets = 1;
            syi.activeActionSets = &active;
            xrSyncActions(m_Session, &syi);
        }

        XrFrameState fs = xrStruct<XrFrameState>(XR_TYPE_FRAME_STATE);
        XrFrameWaitInfo wi = xrStruct<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
        const uint64_t tw0 = steadyNowNs();
        const XrResult wr = xrWaitFrame(m_Session, &wi, &fs);
        m_DiagWaitNs = steadyNowNs() - tw0;
        if (wr == XR_ERROR_SESSION_LOST || wr == XR_ERROR_INSTANCE_LOST) {
            // X5：runtime 已失效（例如 SteamVR 崩潰）——與 LOSS_PENDING 同樣處理
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] xrWaitFrame: %s - treating as session loss",
                        qUtf8Printable(xrResultStr(m_Instance, wr)));
            m_Lost.store(true);
            break;
        }
        if (XR_FAILED(wr)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (m_LostByGpu.load(std::memory_order_acquire)) {
            // M4a 收尾：影像 render thread 的 GPU 等待逾時——runtime 多半已死或卡住，同 session loss 處理
            m_Lost.store(true);
            break;
        }
        if (m_Options.pcvr) {
            pcvrAfterWaitFrame(fs.predictedDisplayTime, static_cast<uint64_t>(fs.predictedDisplayPeriod));
            if (m_VrCtl != nullptr) {
                // M4a R2：按鍵快照＋frameloop 的 grip pose（xrSyncActions 已在本幀 xrWaitFrame 前做過）
                m_VrCtl->updateFrame(fs.predictedDisplayTime, m_State.load() == XR_SESSION_STATE_FOCUSED, m_TrackSpace);
                m_VrCtl->applyPendingHaptics();
            }
        }
        XrFrameBeginInfo bfi = xrStruct<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
        XrResult br;
        {
            const uint64_t tl0 = steadyNowNs();
            std::lock_guard<std::mutex> lk(m_QueueMutex);  // runtime 可能在 xrBeginFrame 用 queue
            const uint64_t tl1 = steadyNowNs();
            br = xrBeginFrame(m_Session, &bfi);
            m_DiagLockNs = tl1 - tl0;
            m_DiagBeginNs = steadyNowNs() - tl1;
        }
        if (XR_FAILED(br)) {
            continue;
        }

        const uint64_t cpuT0 = steadyNowNs();

        // X3：擺放。第一次 FOCUSED 依頭部水平朝向擺到正前方；recenter 要求（熱鍵／dev）下一幀重擺
        if (fs.shouldRender) {
            if (m_Options.testRecenterSec > 0 && !m_TestRecenterDone &&
                cpuT0 - m_BringUpNs > static_cast<uint64_t>(m_Options.testRecenterSec) * 1000000000ull) {
                m_TestRecenterDone = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev test recenter (--xr-test-recenter-sec %d)",
                            m_Options.testRecenterSec);
                m_Screen->requestRecenter();
            }
            const XrDesktopScreen::Reason why = m_Screen->takePendingPlacement(m_State.load() == XR_SESSION_STATE_FOCUSED);
            if (why != XrDesktopScreen::Reason::None) {
                XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
                const XrResult lr = xrLocateSpace(m_ViewSpace, m_LocalSpace, fs.predictedDisplayTime, &loc);
                if (XR_SUCCEEDED(lr) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                    m_Screen->place(loc.pose);
                    const XrPosef qp = m_Screen->quadPose();
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-XR] placement (%s): yaw=%.1f deg head=(%.2f, %.2f, %.2f) screen=(%.2f, %.2f, %.2f) layer=%s",
                                why == XrDesktopScreen::Reason::Initial ? "initial" : "recenter",
                                static_cast<double>(m_Screen->yawRad() * 180.0f / 3.14159265f),
                                static_cast<double>(loc.pose.position.x), static_cast<double>(loc.pose.position.y),
                                static_cast<double>(loc.pose.position.z), static_cast<double>(qp.position.x),
                                static_cast<double>(qp.position.y), static_cast<double>(qp.position.z),
                                m_EnabledExtensions.contains(QLatin1String(kExtCylinder)) ? "cylinder" : "quad");
                }
                else if (why == XrDesktopScreen::Reason::Recenter) {
                    m_Screen->requestRecenter();  // head pose 暫時無效：下一幀再試
                }
            }
        }

        const XrCompositionLayerBaseHeader* layers[8];
        uint32_t layerCount = 0;
        // M4a R1（PCVR）：projection＋淡出 quad（結構體要活到 xrEndFrame）
        XrCompositionLayerProjection proj = xrStruct<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
        XrCompositionLayerProjectionView projViews[2] = {
            xrStruct<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW),
            xrStruct<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)};
        XrCompositionLayerQuad fade = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        XrCompositionLayerQuad quad = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        XrCompositionLayerCylinderKHR cyl = xrStruct<XrCompositionLayerCylinderKHR>(XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR);
        XrCompositionLayerQuad status = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        const bool useCylinder = m_EnabledExtensions.contains(QLatin1String(kExtCylinder));
        const float screenW = m_Screen->quadWidthM();
        bool videoLayer = false;
        bool loadingLayer = false;
        float screenAspect = static_cast<float>(m_Options.quadWidth) / static_cast<float>(m_Options.quadHeight);
        uint64_t videoGen = 0;
#ifdef HAVE_XR_VIDEO
        // X3：影像由 XrVideo 的 render thread 畫，這裡只引用最後 release 的影像（沒有新幀時就是
        // 自有的最後一幀複本）。> 1 s 沒更新疊狀態條；> 5 s 改回 loading quad。
        XrVideo::Current vcur;
        const bool haveVideo = fs.shouldRender && m_Video != nullptr && m_Video->current(&vcur);
        int stale = 0;
        if (haveVideo) {
            const uint64_t nowUs = cpuT0 / 1000ull;
            const uint64_t ageMs = nowUs > vcur.lastDrawnUs ? (nowUs - vcur.lastDrawnUs) / 1000ull : 0;
            stale = ageMs < 1000 ? 1 : (ageMs < 5000 ? 2 : 3);
        }
        if (m_Options.pcvr && fs.shouldRender) {
            // M4a R1：projection。pose＝0x81 帶回的 renderPose∘eyeToHead；>100 ms 沒新幀疊半透明黑、
            // >250 ms 改 loading quad（下面 β 的 loading 分支，因為 videoLayer 維持 false）
            uint64_t ageMs = 0;
            if (haveVideo) {
                const uint64_t nowUs = cpuT0 / 1000ull;
                ageMs = nowUs > vcur.lastDrawnUs ? (nowUs - vcur.lastDrawnUs) / 1000ull : 0;
            }
            int ps = 0;
            if (haveVideo && vcur.hasMeta) {
                ps = ageMs < 100 ? 1 : (ageMs < 250 ? 2 : 3);
            }
            else if (haveVideo) {
                std::lock_guard<std::mutex> lk(m_StatsMutex);
                m_ProjNoMeta++;
                m_PcvrTiming.metaMissTotal++;
            }
            if (ps != m_PcvrStale) {
                static const char* const kPcvrName[] = {"no-video", "live", "fade", "loading"};
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] pcvr video state %s -> %s (age %llu ms)",
                            kPcvrName[m_PcvrStale], kPcvrName[ps], static_cast<unsigned long long>(ageMs));
                m_PcvrStale = ps;
            }
            if (ps == 1 || ps == 2) {
                if (buildProjection(vcur.subImage.swapchain, vcur.subImage.imageRect, vcur.meta.renderRot,
                                    vcur.meta.renderPos, vcur.meta.echoSampleId, &proj, projViews)) {
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
                    videoLayer = true;
                    videoGen = vcur.generation;
                    pcvrTimingOnLatch(vcur.seq, vcur.lastDrawnUs, vcur.renderUs, vcur.hasMeta,
                                      vcur.meta.echoSampleId, cpuT0);
                    std::lock_guard<std::mutex> lk(m_StatsMutex);
                    m_ProjFrames++;
                }
            }
            if (videoLayer && ps == 2 &&
                ensureSolidQuad(m_FadeSwapchain, m_FadeImages, m_FadeImageCount, &m_FadeReady, 0.0f, 0.0f, 0.0f, 0.6f)) {
                fade.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                fade.space = m_ViewSpace;
                fade.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                fade.subImage.swapchain = m_FadeSwapchain;
                fade.subImage.imageRect.offset = {0, 0};
                fade.subImage.imageRect.extent = {8, 8};
                fade.subImage.imageArrayIndex = 0;
                fade.pose.orientation.w = 1.0f;
                fade.pose.position = {0.0f, 0.0f, -0.3f};
                fade.size = {3.0f, 3.0f};  // 0.3 m 前方 3 m 寬：蓋住整個視野
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fade);
            }
        }
        if (m_Options.pcvr) {
            pcvrTimingTick(cpuT0, static_cast<uint64_t>(fs.predictedDisplayPeriod));  // M4a R3
        }
        if (!m_Options.pcvr && fs.shouldRender && stale != m_StaleState) {
            static const char* const kStaleName[] = {"no-video", "live", "stale", "lost"};
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video state %s -> %s",
                        kStaleName[m_StaleState], kStaleName[stale]);
            m_StaleState = stale;
        }
        if (!m_Options.pcvr && haveVideo && stale != 3) {
            const float screenH = (useCylinder ? m_Screen->cylinderRadiusM() * m_Screen->cylinderAngleRad() : screenW) /
                                  vcur.aspect;
            if (useCylinder) {
                cyl.layerFlags = 0;
                cyl.space = m_LocalSpace;
                cyl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                cyl.subImage = vcur.subImage;
                cyl.pose = m_Screen->cylinderPose();
                cyl.radius = m_Screen->cylinderRadiusM();
                cyl.centralAngle = m_Screen->cylinderAngleRad();
                cyl.aspectRatio = vcur.aspect;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cyl);
            }
            else {
                quad.layerFlags = 0;
                quad.space = m_LocalSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage = vcur.subImage;
                quad.pose = m_Screen->quadPose();
                quad.size = {screenW, screenW / vcur.aspect};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            }
            if (!m_LoggedLayerKind) {
                m_LoggedLayerKind = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video layer: %s %.2f x %.2f m at %.2f m",
                            useCylinder ? "cylinder" : "quad",
                            static_cast<double>(useCylinder ? m_Screen->cylinderRadiusM() * m_Screen->cylinderAngleRad() : screenW),
                            static_cast<double>(screenH), static_cast<double>(m_Options.quadDistanceM));
            }
            videoLayer = true;
            videoGen = vcur.generation;
            screenAspect = vcur.aspect;
            if (stale == 2 &&
                ensureSolidQuad(m_StatusSwapchain, m_StatusImages, m_StatusImageCount, &m_StatusReady, 0.85f, 0.45f, 0.05f)) {
                const float statusW = screenW * 0.3f;
                const float statusH = statusW * static_cast<float>(kStatusH) / static_cast<float>(kStatusW);
                status.layerFlags = 0;
                status.space = m_LocalSpace;
                status.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                status.subImage.swapchain = m_StatusSwapchain;
                status.subImage.imageRect.offset = {0, 0};
                status.subImage.imageRect.extent = {static_cast<int32_t>(kStatusW), static_cast<int32_t>(kStatusH)};
                status.subImage.imageArrayIndex = 0;
                status.pose = m_Screen->statusPose(screenH, statusH);
                status.size = {statusW, statusH};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&status);
            }
        }
#endif
        if (fs.shouldRender && !videoLayer &&
            ensureSolidQuad(m_QuadSwapchain, m_QuadImages, m_QuadImageCount, &m_QuadReady, 0.08f, 0.08f, 0.08f)) {
            // loading 環境：深灰 quad（只清一次色）
            quad.layerFlags = 0;
            quad.space = m_LocalSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = m_QuadSwapchain;
            quad.subImage.imageRect.offset = {0, 0};
            quad.subImage.imageRect.extent = {static_cast<int32_t>(m_Options.quadWidth),
                                              static_cast<int32_t>(m_Options.quadHeight)};
            quad.subImage.imageArrayIndex = 0;
            quad.pose = m_Screen->quadPose();
            quad.size = {screenW, screenW * static_cast<float>(m_Options.quadHeight) / static_cast<float>(m_Options.quadWidth)};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
            loadingLayer = true;
        }

        // 虛擬鍵盤：影像螢幕下方 6 cm、往使用者拉近 30 cm、後仰 30°（像筆電鍵盤），寬為螢幕 62%
        XrCompositionLayerQuad kbLayer = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        XrCompositionLayerQuad kbHi = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        bool kbShown = false;
        XrPosef kbPose = {};
        float kbW = 0.0f, kbH = 0.0f;
        if (!m_Options.pcvr && m_Input != nullptr && fs.shouldRender && m_Input->keyboardOpen() && m_Screen->placed() &&
            (videoLayer || loadingLayer) && m_KbSwapchain != XR_NULL_HANDLE) {
            const XrKeyboard& kb = m_Input->keyboard();
            m_Input->takeKeyboardDirty();
            const int wantMods = m_Input->stickyModifiers() & 0x0F;
            const bool ready = (m_KbReadyMask.load(std::memory_order_acquire) & (1u << wantMods)) != 0;
            if (!ready) {
                std::lock_guard<std::mutex> lk(m_KbRenderMutex);
                if (m_KbRenderRequest != wantMods) {
                    m_KbRenderRequest = wantMods;
                    m_KbRenderCv.notify_one();
                }
            }
            if (ready && (wantMods != m_KbUploadedMods || !m_KbUploaded)) {
                const QImage& img = m_KbCache[wantMods];
                m_KbUploaded = uploadImage(m_KbSwapchain, m_KbImages, m_KbImageCount, img);
                m_KbUploadedMods = m_KbUploaded ? wantMods : -1;
                if (!m_KbUploaded && !m_KbUnsupportedLogged) {
                    m_KbUnsupportedLogged = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] keyboard texture upload failed (format %lld)",
                                static_cast<long long>(m_QuadFormat));
                }
                if (m_KbUploaded && !m_KbDumped && !m_Options.dumpKeyboardPath.isEmpty()) {
                    m_KbDumped = true;
                    const QImage out = img.width() > 1280 || img.height() > 1280
                                           ? img.scaled(QSize(1280, 1280), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                           : img;
                    const bool saved = out.save(m_Options.dumpKeyboardPath, "PNG");
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev keyboard dump %dx%d -> %s (%s)", out.width(),
                                out.height(), qUtf8Printable(m_Options.dumpKeyboardPath), saved ? "ok" : "FAILED");
                }
            }
            if (m_KbUploaded) {
                const XrPosef sp = m_Screen->quadPose();
                const float scrH = (useCylinder && videoLayer ? m_Screen->cylinderRadiusM() * m_Screen->cylinderAngleRad() : screenW) /
                                   screenAspect;
                kbW = screenW * 0.62f;
                kbH = kbW / kb.aspect();
                const float tilt = -30.0f * 3.14159265f / 180.0f;
                const XrQuaternionf qx = {std::sin(tilt * 0.5f), 0.0f, 0.0f, std::cos(tilt * 0.5f)};
                const XrQuaternionf& q = sp.orientation;
                const XrQuaternionf qt = {q.w * qx.x + q.x * qx.w + q.y * qx.z - q.z * qx.y,
                                          q.w * qx.y - q.x * qx.z + q.y * qx.w + q.z * qx.x,
                                          q.w * qx.z + q.x * qx.y - q.y * qx.x + q.z * qx.w,
                                          q.w * qx.w - q.x * qx.x - q.y * qx.y - q.z * qx.z};
                const XrRay::Vec3 top = XrRay::rotate(q, {0.0f, -scrH * 0.5f - 0.06f, 0.30f});
                const XrRay::Vec3 down = XrRay::rotate(qt, {0.0f, -kbH * 0.5f, 0.0f});
                kbPose.orientation = qt;
                kbPose.position = {sp.position.x + top.x + down.x, sp.position.y + top.y + down.y, sp.position.z + top.z + down.z};
                kbLayer.layerFlags = 0;
                kbLayer.space = m_LocalSpace;
                kbLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                kbLayer.subImage.swapchain = m_KbSwapchain;
                kbLayer.subImage.imageRect.offset = {0, 0};
                kbLayer.subImage.imageRect.extent = {kb.width(), kb.height()};
                kbLayer.subImage.imageArrayIndex = 0;
                kbLayer.pose = kbPose;
                kbLayer.size = {kbW, kbH};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&kbLayer);
                kbShown = true;
            }
        }

        // X4：射線滑鼠。螢幕（影像或 loading quad）在畫面上才求交；失去 FOCUSED 時 XrInput 送全部放開
        XrCompositionLayerQuad pointer = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        if (!m_Options.pcvr && m_Input != nullptr && fs.shouldRender) {
            XrInput::Screen scr;
            scr.kbValid = kbShown;
            scr.kbPose = kbPose;
            scr.kbWidthM = kbW;
            scr.kbHeightM = kbH;
            scr.valid = m_Screen->placed() && (videoLayer || loadingLayer);
            scr.cylinder = videoLayer && useCylinder;
            scr.pose = scr.cylinder ? m_Screen->cylinderPose() : m_Screen->quadPose();
            scr.widthM = screenW;
            scr.heightM = screenW / screenAspect;
            scr.radiusM = m_Screen->cylinderRadiusM();
            scr.angleRad = m_Screen->cylinderAngleRad();
            scr.aspect = screenAspect;
            m_Input->update(fs.predictedDisplayTime, m_LocalSpace, m_ViewSpace, scr,
                            m_State.load() == XR_SESSION_STATE_FOCUSED, steadyNowNs());
            // 鍵盤高亮：射線指著的鍵（按住時換色），貼在鍵盤面前 2 mm
            bool hiPressed = false;
            const int hover = kbShown ? m_Input->keyboardHover(&hiPressed) : -1;
            if (hover >= 0 && layerCount < 7) {
                XrSwapchain hsc = hiPressed ? m_KbPressSwapchain : m_KbHoverSwapchain;
                const bool ready = hiPressed
                                       ? ensureSolidQuad(m_KbPressSwapchain, m_KbPressImages, m_KbPressImageCount, &m_KbPressReady,
                                                         0.10f * 0.55f, 0.45f * 0.55f, 1.0f * 0.55f, 0.55f)
                                       : ensureSolidQuad(m_KbHoverSwapchain, m_KbHoverImages, m_KbHoverImageCount, &m_KbHoverReady,
                                                         0.30f, 0.30f, 0.30f, 0.30f);
                if (ready && hsc != XR_NULL_HANDLE) {
                    float cu = 0.0f, cv = 0.0f, du = 0.0f, dv = 0.0f;
                    m_Input->keyboard().keyUv(hover, &cu, &cv, &du, &dv);
                    const XrRay::Vec3 c = XrRay::quadPoint(kbPose, kbW, kbH, cu, cv);
                    const XrRay::Vec3 nrm = XrRay::rotate(kbPose.orientation, {0.0f, 0.0f, 0.002f});
                    kbHi.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    kbHi.space = m_LocalSpace;
                    kbHi.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    kbHi.subImage.swapchain = hsc;
                    kbHi.subImage.imageRect.offset = {0, 0};
                    kbHi.subImage.imageRect.extent = {8, 8};
                    kbHi.subImage.imageArrayIndex = 0;
                    kbHi.pose.orientation = kbPose.orientation;
                    kbHi.pose.position = {c.x + nrm.x, c.y + nrm.y, c.z + nrm.z};
                    kbHi.size = {du * kbW, dv * kbH};
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&kbHi);
                }
            }
            XrPosef pp;
            if (m_Input->pointerPose(&pp) && layerCount < 8 &&
                ensureSolidQuad(m_PointerSwapchain, m_PointerImages, m_PointerImageCount, &m_PointerReady, 1.0f, 1.0f, 1.0f)) {
                const float size = screenW * 0.008f;  // 1080p 螢幕上約 16 px
                pointer.layerFlags = 0;
                pointer.space = m_LocalSpace;
                pointer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                pointer.subImage.swapchain = m_PointerSwapchain;
                pointer.subImage.imageRect.offset = {0, 0};
                pointer.subImage.imageRect.extent = {static_cast<int32_t>(kPointerPx), static_cast<int32_t>(kPointerPx)};
                pointer.subImage.imageArrayIndex = 0;
                pointer.pose = pp;
                pointer.size = {size, size};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pointer);
            }
        }
        if (fs.shouldRender) {
            // FOV／眼位：每秒 locate 一次即可（X1 只做量測）
            const uint64_t nowNs = steadyNowNs();
            if (nowNs - lastViewLocateNs > 1000000000ull || (m_Options.pcvr && lastViewLocateNs == 0)) {
                lastViewLocateNs = nowNs;
                XrViewLocateInfo li = xrStruct<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
                li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                li.displayTime = fs.predictedDisplayTime;
                li.space = m_LocalSpace;
                XrViewState vs = xrStruct<XrViewState>(XR_TYPE_VIEW_STATE);
                XrView views[2] = {xrStruct<XrView>(XR_TYPE_VIEW), xrStruct<XrView>(XR_TYPE_VIEW)};
                uint32_t vc = 0;
                if (XR_SUCCEEDED(xrLocateViews(m_Session, &li, &vs, 2, &vc, views)) && vc == 2) {
                    std::lock_guard<std::mutex> lk(m_StatsMutex);
                    for (int e = 0; e < 2; e++) {
                        m_Fov[e] = views[e].fov;
                        m_EyePose[e] = views[e].pose;
                    }
                    m_HaveViews = true;
                }
                if (m_Options.pcvr && m_ViewSpace != XR_NULL_HANDLE) {
                    // M4a R1：eyeToHead（每眼相對頭）＝以 VIEW space locate
                    li.space = m_ViewSpace;
                    XrView hv[2] = {xrStruct<XrView>(XR_TYPE_VIEW), xrStruct<XrView>(XR_TYPE_VIEW)};
                    uint32_t hc = 0;
                    XrViewState hs = xrStruct<XrViewState>(XR_TYPE_VIEW_STATE);
                    if (XR_SUCCEEDED(xrLocateViews(m_Session, &li, &hs, 2, &hc, hv)) && hc == 2 &&
                        (hs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (hs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                        bool first = false;
                        {
                            std::lock_guard<std::mutex> lk(m_StatsMutex);
                            for (int e = 0; e < 2; e++) {
                                m_EyeToHead[e] = hv[e].pose;
                                m_Fov[e] = hv[e].fov;
                            }
                            first = !m_HaveEyeToHead;
                            m_HaveEyeToHead = true;
                        }
                        if (first) {
                            m_ViewsCv.notify_all();
                        }
                    }
                }
            }
        }

        XrFrameEndInfo ei = xrStruct<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = blend;
        ei.layerCount = layerCount;
        ei.layers = layerCount ? layers : nullptr;
        XrResult er;
        {
            const uint64_t tl0 = steadyNowNs();
            std::lock_guard<std::mutex> lk(m_QueueMutex);  // runtime 可能在 xrEndFrame 用 queue
            const uint64_t tl1 = steadyNowNs();
            er = xrEndFrame(m_Session, &ei);
            m_DiagLockNs += tl1 - tl0;
            m_DiagEndNs = steadyNowNs() - tl1;
        }
#ifdef HAVE_XR_VIDEO
        if (videoLayer && XR_SUCCEEDED(er) && m_Video != nullptr) {
            m_Video->frameEnded(videoGen);
        }
#endif
        {
            const uint64_t cpuUs = (steadyNowNs() - cpuT0) / 1000ull;
            m_XrCpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(cpuUs, 0xffffffffu)));
        }

        double lateRefreshMeasuredHz = 0.0;  // > 0：鎖外呼叫 requestRefreshLate()
        {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            if (XR_SUCCEEDED(er)) {
                m_Stats.frames++;
            }
            else {
                m_Stats.endFrameErrors++;
            }
            if (!fs.shouldRender) {
                m_Stats.notRendered++;
            }
            const uint64_t pdt = static_cast<uint64_t>(fs.predictedDisplayTime);
            const uint64_t period = static_cast<uint64_t>(fs.predictedDisplayPeriod);
            if (m_LastDisplayTime != 0 && period > 0 && pdt > m_LastDisplayTime) {
                const uint64_t delta = pdt - m_LastDisplayTime;
                if (delta > period + period / 2) {
                    const uint64_t lost = (delta + period / 2) / period - 1;
                    if (steadyNowNs() < m_WarmupEndNs) {
                        m_Stats.warmupMissed += lost;
                    }
                    else {
                        m_Stats.missed += lost;
                    }
                    // X3 診斷：開場前 30 次漏幀逐次記錄（時間點＋影像狀態），定位開場卡頓來源
                    if (m_MissEventsLogged < 30) {
                        m_MissEventsLogged++;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-XR] missed %llu frame(s) at +%llu ms (video=%d prevCpu=%.2f waitFrame=%.1f lock=%.1f begin=%.1f end=%.1f ms)",
                                    static_cast<unsigned long long>(lost),
                                    static_cast<unsigned long long>((steadyNowNs() - m_BringUpNs) / 1000000ull),
                                    m_StaleState, m_XrCpuUs.empty() ? 0.0 : m_XrCpuUs.back() / 1000.0,
                                    m_DiagWaitNs / 1e6, m_DiagLockNs / 1e6, m_DiagBeginNs / 1e6, m_DiagEndNs / 1e6);
                    }
                }
            }
            m_LastDisplayTime = pdt;
            // M4a 收尾：週期穩定度（waitViews 用）。穩定 20 幀且符合要求（±3%）就算定下；有要求但一直不符合時，
            // 穩定 60 幀（72～90 Hz 約 0.7～0.8 s）後接受量到的週期（waitViews 會 log 警告）。
            // 門檻由 10／45 幀放寬：Frame 上 app 變成 scene app 約 0.1 s 後 SteamVR 才把顯示器切到該 app 的
            // 更新率，太早定下會把切換前的週期當真。
            const uint64_t pd = period > m_LastPeriod ? period - m_LastPeriod : m_LastPeriod - period;
            m_StablePeriodFrames = (period > 0 && m_LastPeriod > 0 && pd <= m_LastPeriod / 100) ? m_StablePeriodFrames + 1 : 0;
            m_LastPeriod = period;
            if (!m_PeriodSettled && period > 0) {
                const double periodHz = 1e9 / static_cast<double>(period);
                const bool matches = m_RequestedHz <= 0.0f ||
                                     std::fabs(periodHz - m_RequestedHz) <= 0.03 * static_cast<double>(m_RequestedHz);
                // §XR-REFRESH-LATE：週期穩定在「不是想要的更新率」→ 鎖外重新列舉並再要求一次（最多 2 次）
                const double want = static_cast<double>(m_Options.preferredRefreshHz);
                if (m_StablePeriodFrames == 20 && m_HasRefreshRate && want > 0.0 && m_LateRefreshTries < 2 &&
                    std::fabs(periodHz - want) > 0.03 * want) {
                    lateRefreshMeasuredHz = periodHz;
                }
                else if ((m_StablePeriodFrames >= 20 && matches) || m_StablePeriodFrames >= 60) {
                    m_PeriodSettled = true;
                    m_ViewsCv.notify_all();
                }
            }
        }
        if (lateRefreshMeasuredHz > 0.0) {
            requestRefreshLate(lateRefreshMeasuredHz);
        }
        maybeLogStats(steadyNowNs());
    }

    const Stats s = stats();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] frame thread exit: frames=%llu missed=%llu (%.2f%%) warmupMissed=%llu highest=%s state=%s lost=%d",
                static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.missed), s.missPercent(),
                static_cast<unsigned long long>(s.warmupMissed),
                sessionStateName(s.highestState), sessionStateName(s.currentState), m_Lost.load() ? 1 : 0);

    // X5：不是 shutdown() 要求的結束 → 通知 Session（β：重建或退回平面）
    if (!m_ExitRequested.load() && !m_StopRequested.load()) {
        int reason = 0;
        if (m_Lost.load()) {
            reason = kEndedLoss;
        }
        else if (m_SimExit.load() || m_State.load() == XR_SESSION_STATE_EXITING) {
            reason = kEndedExit;
        }
        if (reason != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] session ended by the runtime: %s",
                        reason == kEndedLoss ? "loss" : "exit");
            if (m_Options.onEnded) {
                m_Options.onEnded(reason);
            }
        }
    }
    m_FrameThreadRunning.store(false, std::memory_order_release);
}

// ── M4a R1（PCVR）──

namespace {
inline XrQuaternionf quatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline XrVector3f quatRotate(const XrQuaternionf& q, const XrVector3f& v)
{
    // v' = v + 2w(q×v) + 2(q×(q×v))
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}
}  // namespace

bool XrContext::nowXrTime(XrTime* out) const
{
    if (m_TimeConv == nullptr || m_Instance == XR_NULL_HANDLE) {
        return false;
    }
#if defined(Q_OS_WIN)
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    return XR_SUCCEEDED(reinterpret_cast<PfnConvQpc>(m_TimeConv)(m_Instance, &qpc, out));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return XR_SUCCEEDED(reinterpret_cast<PfnConvTimespec>(m_TimeConv)(m_Instance, &ts, out));
#endif
}

namespace {
template <typename T>
T pctOf(std::vector<T> v, double p)
{
    if (v.empty()) {
        return T{};
    }
    const size_t idx = (std::min)(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
    return v[idx];
}
uint16_t sat16u(uint64_t v) { return static_cast<uint16_t>((std::min<uint64_t>)(v, 0xffff)); }
int16_t sat16s(int64_t v) { return static_cast<int16_t>((std::max<int64_t>)(-32768, (std::min<int64_t>)(v, 32767))); }
}  // namespace

// M4a R3：frame thread，新的一張串流影像第一次被 latch 進 projection layer 時呼叫。
// slack＝latch 時刻 − 影像就緒時刻（正值＝幀比 latch 早到）；MTP_content＝本幀預測顯示時間 −
// echoSampleId 的 sampleTime（兩者都是 client steady 時鐘）。
void XrContext::pcvrTimingOnLatch(uint64_t seq, uint64_t lastDrawnUs, uint32_t renderUs, bool hasMeta,
                                  uint32_t echoSampleId, uint64_t latchNs)
{
    auto& t = m_PcvrTiming;
    if (seq == 0 || seq == t.lastLatchedSeq) {
        return;  // 同一張影像重送（沒有新幀）不算
    }
    t.lastLatchedSeq = seq;
    t.presentedTotal++;
    const int64_t slackUs = static_cast<int64_t>(latchNs / 1000ull) - static_cast<int64_t>(lastDrawnUs);
    t.lastSlackUs = static_cast<int32_t>((std::max<int64_t>)(INT32_MIN, (std::min<int64_t>)(slackUs, INT32_MAX)));
    t.lastFrameId = static_cast<uint32_t>(seq);
    t.haveLatch = true;
    t.slack1s.push_back(t.lastSlackUs);
    t.render1s.push_back(renderUs);
    if (hasMeta) {
        uint64_t sampleNs = 0;
        if (VrSampleHistory::lookup(echoSampleId, &sampleNs) && t.predictedDisplayClientNs > sampleNs) {
            const uint64_t mtpUs = (t.predictedDisplayClientNs - sampleNs) / 1000ull;
            if (mtpUs < 2000000ull) {  // > 2 s 視為樣本對不上（不列入）
                t.mtp1sUs.push_back(static_cast<uint32_t>(mtpUs));
                t.mtp10sUs.push_back(static_cast<uint32_t>(mtpUs));
            }
        }
        else {
            t.mtpNoSample++;
        }
    }
}

// M4a R3：frame thread 每幀呼叫。10 Hz 送 LATCH、1 Hz 送 CLIENT_TIMING、10 s 印 [VIPLE-VR-MTP10]。
// LiSendVrMessage 在沒有 VR session 時回 -1（例如 bring-up 後、/launch 前），直接忽略。
void XrContext::pcvrTimingTick(uint64_t nowNs, uint64_t periodNs)
{
    auto& t = m_PcvrTiming;
    if (t.mtpWindowStartNs == 0) {
        t.mtpWindowStartNs = nowNs;
        t.lastTimingSendNs = nowNs;
    }

    // 以固定截止時間排程（下一次＝上一次截止＋間隔），frame thread 每幀呼叫一次時不會因為幀間隔
    // 抖動而把 10 Hz 拖成 8 Hz；落後超過一個間隔就重新對齊到現在。
    auto due = [nowNs](uint64_t& next, uint64_t interval) {
        if (nowNs < next) {
            return false;
        }
        next = (nowNs - next >= interval) ? nowNs + interval : next + interval;
        return true;
    };
    if (t.haveLatch && due(t.lastLatchSendNs, 100000000ull)) {
        uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_LATCH)];
        VIPLE_VR_TLV_LATCH l{};
        l.frameId = t.lastFrameId;
        l.slackUs = t.lastSlackUs;
        l.displayPeriodNs = static_cast<uint32_t>((std::min<uint64_t>)(periodNs, 0xffffffffu));
        tlv[0] = VIPLE_VR_C2S_LATCH;
        tlv[1] = sizeof(l);
        memcpy(tlv + 2, &l, sizeof(l));
        if (LiSendVrMessage(tlv, sizeof(tlv), false) == 0) {
            t.latchSent++;
        }
    }

    if (due(t.lastTimingSendNs, 1000000000ull)) {
        VIPLE_VR_TLV_CLIENT_TIMING ct{};
        ct.framesPresented = t.presentedTotal;
        {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            ct.xrMissed = static_cast<uint32_t>((std::min<uint64_t>)(m_Stats.missed, 0xffffffffu));
        }
        ct.metaMiss = t.metaMissTotal;
        ct.displayPeriodNs = static_cast<uint32_t>((std::min<uint64_t>)(periodNs, 0xffffffffu));
        ct.decodeP50Us = 0;  // XrContext 拿不到 decoder 延遲（TODO：由 Session 提供）
        ct.decodeP95Us = 0;
        ct.renderP50Us = sat16u(pctOf(t.render1s, 0.50));
        ct.renderP95Us = sat16u(pctOf(t.render1s, 0.95));
        ct.slackP50Us = sat16s(pctOf(t.slack1s, 0.50));
        ct.slackP05Us = sat16s(pctOf(t.slack1s, 0.05));
        ct.mtpP50_100us = sat16u(pctOf(t.mtp1sUs, 0.50) / 100u);
        ct.mtpP95_100us = sat16u(pctOf(t.mtp1sUs, 0.95) / 100u);
        uint8_t tlv[2 + sizeof(VIPLE_VR_TLV_CLIENT_TIMING)];
        tlv[0] = VIPLE_VR_C2S_CLIENT_TIMING;
        tlv[1] = sizeof(ct);
        memcpy(tlv + 2, &ct, sizeof(ct));
        if (LiSendVrMessage(tlv, sizeof(tlv), false) == 0) {
            t.timingSent++;
        }
        t.slack1s.clear();
        t.render1s.clear();
        t.mtp1sUs.clear();
    }

    if (nowNs - t.mtpWindowStartNs >= 10000000000ull) {
        const double secs = static_cast<double>(nowNs - t.mtpWindowStartNs) / 1e9;
        t.mtpWindowStartNs = nowNs;
        const size_t n = t.mtp10sUs.size();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-MTP10] n=%zu p50=%.1f p95=%.1f p99=%.1f ms noSample=%u | latch sent=%u (%.1f/s) lastSlack=%d us | timing sent=%u%s",
                    n, pctOf(t.mtp10sUs, 0.50) / 1000.0, pctOf(t.mtp10sUs, 0.95) / 1000.0,
                    pctOf(t.mtp10sUs, 0.99) / 1000.0, t.mtpNoSample, t.latchSent,
                    secs > 0 ? t.latchSent / secs : 0.0, t.lastSlackUs, t.timingSent,
                    m_TimeConv != nullptr ? "" : " (mtp approx: no time conversion ext)");
        t.mtp10sUs.clear();
        t.mtpNoSample = 0;
        t.latchSent = 0;
        t.timingSent = 0;
    }
}

// frame thread：xrWaitFrame 之後。更新「預測顯示時間 − 現在」的估計，並 locate 一次 HMD（frameloop 模式樣本）
void XrContext::pcvrAfterWaitFrame(XrTime predictedDisplayTime, uint64_t periodNs)
{
    const uint64_t steadyNow = steadyNowNs();
    XrTime xrNow = 0;
    int64_t ahead = static_cast<int64_t>(2 * periodNs);  // 沒有時間換算：約兩個顯示週期
    if (nowXrTime(&xrNow) && predictedDisplayTime > xrNow) {
        ahead = static_cast<int64_t>(predictedDisplayTime - xrNow);
    }
    // M4a R3：預測顯示時間換成 client steady 時鐘（MTP 用）。沒有時間換算擴充時 ahead 是估計值（≈2 週期）
    m_PcvrTiming.predictedDisplayClientNs = steadyNow + static_cast<uint64_t>(ahead);
    m_PredictAheadNs = m_PredictAheadNs == 0 ? ahead : (m_PredictAheadNs * 7 + ahead) / 8;
    if (m_TrackSpace == XR_NULL_HANDLE || m_ViewSpace == XR_NULL_HANDLE) {
        return;
    }
    XrSpaceVelocity vel = xrStruct<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    loc.next = &vel;
    if (XR_FAILED(xrLocateSpace(m_ViewSpace, m_TrackSpace, predictedDisplayTime, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        return;
    }
    HmdSample s;
    s.valid = true;
    s.pos[0] = loc.pose.position.x;
    s.pos[1] = loc.pose.position.y;
    s.pos[2] = loc.pose.position.z;
    s.rot[0] = loc.pose.orientation.x;
    s.rot[1] = loc.pose.orientation.y;
    s.rot[2] = loc.pose.orientation.z;
    s.rot[3] = loc.pose.orientation.w;
    if (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) {
        s.linVel[0] = vel.linearVelocity.x;
        s.linVel[1] = vel.linearVelocity.y;
        s.linVel[2] = vel.linearVelocity.z;
    }
    if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
        s.angVel[0] = vel.angularVelocity.x;
        s.angVel[1] = vel.angularVelocity.y;
        s.angVel[2] = vel.angularVelocity.z;
    }
    s.predictNs = static_cast<uint64_t>(std::max<int64_t>(ahead, 0));
    std::lock_guard<std::mutex> lk(m_HmdMutex);
    m_Hmd = s;
}

bool XrContext::sampleHmd(float pos[3], float rot[4], float linVel[3], float angVel[3], uint32_t* predictNs)
{
    XrTime xrNow = 0;
    if (m_SessionBegun.load() && m_TrackSpace != XR_NULL_HANDLE && m_ViewSpace != XR_NULL_HANDLE && nowXrTime(&xrNow)) {
        // thread 模式：當下 locate「現在＋預測提前量」
        const int64_t ahead = m_PredictAheadNs > 0 ? m_PredictAheadNs : 20000000;
        XrSpaceVelocity vel = xrStruct<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
        XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        loc.next = &vel;
        if (XR_SUCCEEDED(xrLocateSpace(m_ViewSpace, m_TrackSpace, xrNow + ahead, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            pos[0] = loc.pose.position.x;
            pos[1] = loc.pose.position.y;
            pos[2] = loc.pose.position.z;
            rot[0] = loc.pose.orientation.x;
            rot[1] = loc.pose.orientation.y;
            rot[2] = loc.pose.orientation.z;
            rot[3] = loc.pose.orientation.w;
            const bool lv = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
            const bool av = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
            linVel[0] = lv ? vel.linearVelocity.x : 0.0f;
            linVel[1] = lv ? vel.linearVelocity.y : 0.0f;
            linVel[2] = lv ? vel.linearVelocity.z : 0.0f;
            angVel[0] = av ? vel.angularVelocity.x : 0.0f;
            angVel[1] = av ? vel.angularVelocity.y : 0.0f;
            angVel[2] = av ? vel.angularVelocity.z : 0.0f;
            *predictNs = static_cast<uint32_t>(std::min<int64_t>(ahead, 0xffffffffll));
            return true;
        }
    }
    std::lock_guard<std::mutex> lk(m_HmdMutex);
    if (!m_Hmd.valid) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        pos[i] = m_Hmd.pos[i];
        linVel[i] = m_Hmd.linVel[i];
        angVel[i] = m_Hmd.angVel[i];
    }
    for (int i = 0; i < 4; i++) {
        rot[i] = m_Hmd.rot[i];
    }
    *predictNs = static_cast<uint32_t>(std::min<uint64_t>(m_Hmd.predictNs, 0xffffffffull));
    return true;
}

void XrContext::sampleControllers(VIPLE_VR_TRACKING* sample)
{
    if (m_VrCtl == nullptr || !m_SessionBegun.load()) {
        return;
    }
    XrTime xrTime = 0;
    XrTime xrNow = 0;
    if (m_TimeConv != nullptr && nowXrTime(&xrNow)) {
        xrTime = xrNow + (m_PredictAheadNs > 0 ? m_PredictAheadNs : 20000000);
    }
    m_VrCtl->fill(sample, m_TrackSpace, xrTime);
}

void XrContext::queueHaptic(uint8_t device, uint32_t durationUs, float frequencyHz, float amplitude)
{
    if (m_VrCtl != nullptr) {
        m_VrCtl->queueHaptic(device, durationUs, frequencyHz, amplitude);
    }
}

// §XR-REFRESH-LATE（2026-10-02，Frame 實測）：xrCreateSession 當下 Frame 的 SteamVR 只列舉得到「目前」的更新率
// （[120.0]），而 app 變成 scene app 之後它會依「每個 app 的更新率設定」把顯示器切到別的值（沒設定過的 app
// 是 72 Hz）。所以 session 跑起來、週期穩定後若不是想要的更新率，就在這裡重新列舉並再要求一次；runtime
// 不理會時最後仍以量到的週期為準（waitViews）。只在 frame thread 呼叫。
void XrContext::requestRefreshLate(double measuredHz)
{
    ++m_LateRefreshTries;
    auto pfnEnum = xrProc<PFN_xrEnumerateDisplayRefreshRatesFB>(m_Instance, "xrEnumerateDisplayRefreshRatesFB");
    auto pfnReq = xrProc<PFN_xrRequestDisplayRefreshRateFB>(m_Instance, "xrRequestDisplayRefreshRateFB");
    const float want = m_Options.preferredRefreshHz;
    if (!pfnEnum || !pfnReq || want <= 0.0f) {
        m_LateRefreshTries = 2;
        return;
    }
    std::vector<float> rates;
    uint32_t n = 0;
    if (XR_SUCCEEDED(pfnEnum(m_Session, 0, &n, nullptr)) && n > 0) {
        rates.assign(n, 0.0f);
        if (XR_FAILED(pfnEnum(m_Session, n, &n, rates.data()))) {
            n = 0;
        }
        rates.resize(n);
    }
    // 候選＝列舉到的值＋目前量到的值；取最接近 want 的（一樣近取高的）
    float best = static_cast<float>(measuredHz);
    QStringList names;
    for (float r : rates) {
        names << QString::number(static_cast<double>(r), 'f', 1);
        const float dr = std::fabs(r - want), db = std::fabs(best - want);
        if (dr < db || (dr == db && r > best)) {
            best = r;
        }
    }
    QString outcome;
    if (std::fabs(static_cast<double>(best) - measuredHz) <= 0.03 * measuredHz) {
        m_LateRefreshTries = 2;  // 沒有比現況更接近的值，不再試
        outcome = QStringLiteral("keeping the measured rate (nothing closer)");
    }
    else {
        const XrResult rr = pfnReq(m_Session, best);
        if (XR_SUCCEEDED(rr)) {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            m_RequestedHz = best;
            m_StablePeriodFrames = 0;
        }
        outcome = QStringLiteral("requested %1 Hz: %2").arg(static_cast<double>(best), 0, 'f', 1).arg(xrResultStr(m_Instance, rr));
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] display refresh (running, try %d) rates=[%s] measured=%.1f Hz want=%.1f -> %s",
                m_LateRefreshTries, qUtf8Printable(names.join(QLatin1Char(','))), measuredHz, static_cast<double>(want),
                qUtf8Printable(outcome));
}

float XrContext::waitRefreshHz(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_StatsMutex);
    m_ViewsCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] {
        return m_PeriodSettled || !m_FrameThreadRunning.load();
    });
    const float periodHz = m_LastPeriod > 0 ? 1e9f / static_cast<float>(m_LastPeriod) : 0.0f;
    if (periodHz > 0.0f && m_RefreshHz > 0.0f && std::fabs(periodHz - m_RefreshHz) > 0.03f * periodHz) {
        return periodHz;
    }
    return m_RefreshHz > 0.0f ? m_RefreshHz : periodHz;
}

bool XrContext::waitViews(ViewInfo* out, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_StatsMutex);
    m_ViewsCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] {
        return (m_HaveEyeToHead && m_PeriodSettled) || !m_FrameThreadRunning.load();
    });
    if (!m_HaveEyeToHead) {
        return false;
    }
    if (!m_PeriodSettled) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] predictedDisplayPeriod did not settle within %d ms (last %.2f ms) - using it anyway",
                    timeoutMs, static_cast<double>(m_LastPeriod) / 1e6);
    }
    out->valid = true;
    for (int e = 0; e < 2; e++) {
        out->fov[e] = m_Fov[e];
        out->eyeToHead[e] = m_EyeToHead[e];
    }
    out->periodNs = m_LastPeriod;
    const float periodHz = m_LastPeriod > 0 ? 1e9f / static_cast<float>(m_LastPeriod) : 0.0f;
    out->refreshHz = m_RefreshHz > 0.0f ? m_RefreshHz : periodHz;
    // M4a 收尾：runtime 回報的更新率與實際的幀週期不一致（Frame：回 120、實際 72）→ 以量到的週期為準
    if (periodHz > 0.0f && m_RefreshHz > 0.0f && std::fabs(periodHz - m_RefreshHz) > 0.03f * periodHz) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] refresh mismatch: runtime reports %.1f Hz (requested %.1f) but predictedDisplayPeriod=%.2f ms "
                    "(%.1f Hz) - using the measured period",
                    static_cast<double>(m_RefreshHz), static_cast<double>(m_RequestedHz),
                    static_cast<double>(m_LastPeriod) / 1e6, static_cast<double>(periodHz));
        out->refreshHz = periodHz;
    }
    out->recommendedWidth = m_RecW;
    out->recommendedHeight = m_RecH;
    out->trackingSpace = m_TrackSpaceName;
    out->timeConversion = m_TimeConv != nullptr;
    return true;
}

// frame thread：VR session 的 SBS 影像（2W×H）→ 兩個 projection view（左右各半）。
// view pose＝renderPose（0x81，追蹤參考空間）∘ eyeToHead（量測值）；fov 用量測（＝/launch 協商）值。
bool XrContext::buildProjection(XrSwapchain swapchain, const XrRect2Di& rect, const float renderRot[4],
                                const float renderPos[3], uint32_t echoSampleId, XrCompositionLayerProjection* proj,
                                XrCompositionLayerProjectionView views[2])
{
    XrPosef eye[2];
    XrFovf fov[2];
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        if (!m_HaveEyeToHead) {
            return false;
        }
        for (int e = 0; e < 2; e++) {
            eye[e] = m_EyeToHead[e];
            fov[e] = m_Fov[e];
        }
    }
    const int32_t w = rect.extent.width;
    const int32_t h = rect.extent.height;
    if (w < 2 || h < 1) {
        return false;
    }
    XrPosef head;
    head.orientation = {renderRot[0], renderRot[1], renderRot[2], renderRot[3]};
    head.position = {renderPos[0], renderPos[1], renderPos[2]};
    for (int e = 0; e < 2; e++) {
        const XrVector3f off = quatRotate(head.orientation, eye[e].position);
        views[e].pose.orientation = quatMul(head.orientation, eye[e].orientation);
        views[e].pose.position = {head.position.x + off.x, head.position.y + off.y, head.position.z + off.z};
        views[e].fov = fov[e];
        views[e].subImage.swapchain = swapchain;
        views[e].subImage.imageArrayIndex = 0;
        views[e].subImage.imageRect.offset = {rect.offset.x + (e == 0 ? 0 : w / 2), rect.offset.y};
        views[e].subImage.imageRect.extent = {w / 2, h};
    }
    proj->layerFlags = 0;
    proj->space = m_TrackSpace;
    proj->viewCount = 2;
    proj->views = views;
    if (!m_LoggedProjection) {
        m_LoggedProjection = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] pcvr projection: %dx%d per eye from %dx%d SBS, space=%s, first render pose "
                    "(%.3f, %.3f, %.3f | %.3f, %.3f, %.3f, %.3f) echo=%u",
                    w / 2, h, w, h, qUtf8Printable(m_TrackSpaceName), static_cast<double>(head.position.x),
                    static_cast<double>(head.position.y), static_cast<double>(head.position.z),
                    static_cast<double>(head.orientation.x), static_cast<double>(head.orientation.y),
                    static_cast<double>(head.orientation.z), static_cast<double>(head.orientation.w),
                    echoSampleId);
    }
    return true;
}

XrContext::Stats XrContext::stats() const
{
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    return m_Stats;
}

QJsonObject XrContext::describe() const
{
    QJsonObject o;
    o[QStringLiteral("apiVersion")] = QStringLiteral("%1.%2")
                                          .arg(static_cast<unsigned>(XR_VERSION_MAJOR(m_ApiVersion)))
                                          .arg(static_cast<unsigned>(XR_VERSION_MINOR(m_ApiVersion)));
    o[QStringLiteral("enabledExtensions")] = QJsonArray::fromStringList(m_EnabledExtensions);
    o[QStringLiteral("referenceSpaces")] = QJsonArray::fromStringList(m_ReferenceSpaces);
    o[QStringLiteral("refreshRateHz")] = static_cast<double>(m_RefreshHz);
    o[QStringLiteral("quadSwapchainFormat")] = static_cast<double>(m_QuadFormat);
    o[QStringLiteral("quadSwapchainImages")] = static_cast<int>(m_QuadImageCount);
    o[QStringLiteral("vulkanApi")] = QStringLiteral("%1.%2").arg(VK_API_VERSION_MAJOR(m_VkApiVersion)).arg(VK_API_VERSION_MINOR(m_VkApiVersion));

    std::lock_guard<std::mutex> lk(m_StatsMutex);
    o[QStringLiteral("frames")] = static_cast<double>(m_Stats.frames);
    o[QStringLiteral("missed")] = static_cast<double>(m_Stats.missed);
    o[QStringLiteral("warmupMissed")] = static_cast<double>(m_Stats.warmupMissed);
    o[QStringLiteral("missPercent")] = m_Stats.missPercent();
    o[QStringLiteral("notRendered")] = static_cast<double>(m_Stats.notRendered);
    o[QStringLiteral("endFrameErrors")] = static_cast<double>(m_Stats.endFrameErrors);
    o[QStringLiteral("highestState")] = QLatin1String(sessionStateName(m_Stats.highestState));
    o[QStringLiteral("predictedPeriodMs")] = static_cast<double>(m_LastPeriod) / 1e6;
    if (m_HaveViews) {
        QJsonArray eyes;
        for (int e = 0; e < 2; e++) {
            QJsonObject eye;
            eye[QStringLiteral("angleLeftDeg")] = radToDeg(m_Fov[e].angleLeft);
            eye[QStringLiteral("angleRightDeg")] = radToDeg(m_Fov[e].angleRight);
            eye[QStringLiteral("angleUpDeg")] = radToDeg(m_Fov[e].angleUp);
            eye[QStringLiteral("angleDownDeg")] = radToDeg(m_Fov[e].angleDown);
            eye[QStringLiteral("posX")] = static_cast<double>(m_EyePose[e].position.x);
            eyes.append(eye);
        }
        o[QStringLiteral("views")] = eyes;
        o[QStringLiteral("ipdM")] = static_cast<double>(m_EyePose[1].position.x - m_EyePose[0].position.x);
    }
    return o;
}

// ── shutdown ──

void XrContext::shutdown()
{
    if (m_FrameThread.joinable()) {
        // frame thread 自己呼叫 xrRequestExitSession 並跑完 STOPPING→xrEndSession→IDLE／EXITING
        m_ExitRequested.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (m_FrameThreadRunning.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (m_FrameThreadRunning.load(std::memory_order_acquire)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] session did not reach STOPPING within 3 s; forcing frame thread stop");
        }
        m_StopRequested.store(true);
        m_FrameThread.join();
    }
    destroyAll();
    m_ExitRequested.store(false);
    m_StopRequested.store(false);
}

bool XrContext::waitSemaphoreBounded(VkSemaphore sem, uint64_t value, uint64_t timeoutNs)
{
    if (m_VkDevice == VK_NULL_HANDLE || sem == VK_NULL_HANDLE || s_Vk.WaitSemaphores == nullptr) {
        return false;
    }
    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &value;
    return s_Vk.WaitSemaphores(m_VkDevice, &wi, timeoutNs) == VK_SUCCESS;
}

void XrContext::markGpuWedged(const char* why)
{
    if (!m_GpuWedged.exchange(true, std::memory_order_acq_rel)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] GPU wait timed out (%s) - treating the XR context as lost",
                     why);
        // 讓 frame thread 走既有的 loss 流程（β 重建／退平面，PCVR 送 /cancel 結束）
        m_LostByGpu.store(true, std::memory_order_release);
    }
}

void XrContext::destroyAll()
{
    const bool hadAnything = m_Instance != XR_NULL_HANDLE;
    if (m_KbRenderThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_KbRenderMutex);
            m_KbRenderCancel = true;
        }
        m_KbRenderCv.notify_one();
        m_KbRenderThread.join();
    }
    m_KbReadyMask.store(0);
    m_KbUploadedMods = -1;
    for (QImage& img : m_KbCache) {
        img = QImage();
    }
    // M4a 收尾：原本 vkDeviceWaitIdle 沒有上限——runtime 在串流中死掉、GPU 工作等不到結果時會永遠卡住。
    // 改成送一個空 batch 帶 fence、最多等 2 s；等不到就標 wedged，下面跳過所有無限期等待並洩漏資源。
    if (m_VkDevice != VK_NULL_HANDLE && m_Queue != VK_NULL_HANDLE && !gpuWedged() &&
        s_Vk.CreateFence && s_Vk.QueueSubmit && s_Vk.WaitForFences) {
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence idle = VK_NULL_HANDLE;
        if (s_Vk.CreateFence(m_VkDevice, &fci, nullptr, &idle) == VK_SUCCESS) {
            VkResult sr;
            {
                std::lock_guard<std::mutex> lk(m_QueueMutex);
                sr = s_Vk.QueueSubmit(m_Queue, 0, nullptr, idle);
            }
            const VkResult wr = sr == VK_SUCCESS ? s_Vk.WaitForFences(m_VkDevice, 1, &idle, VK_TRUE, 2000000000ull) : sr;
            if (wr == VK_SUCCESS) {
                s_Vk.DestroyFence(m_VkDevice, idle, nullptr);
            }
            else {
                markGpuWedged(wr == VK_TIMEOUT ? "queue did not go idle within 2 s at teardown" : "queue idle wait failed at teardown");
                // fence 仍可能被 GPU 參照：刻意不 destroy
            }
        }
    }
#ifdef HAVE_XR_VIDEO
    // X2：影像 swapchain 與 libplacebo 物件要在 session／VkDevice 之前釋放
    if (m_Video != nullptr) {
        m_Video->destroy(gpuWedged());
        delete m_Video;
        m_Video = nullptr;
    }
#endif
    if (m_QuadSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_QuadSwapchain);
        m_QuadSwapchain = XR_NULL_HANDLE;
    }
    if (m_StatusSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_StatusSwapchain);
        m_StatusSwapchain = XR_NULL_HANDLE;
    }
    delete[] m_StatusImages;
    m_StatusImages = nullptr;
    m_StatusImageCount = 0;
    delete[] m_QuadImages;
    m_QuadImages = nullptr;
    m_QuadImageCount = 0;
    for (auto* s : {&m_KbSwapchain, &m_KbHoverSwapchain, &m_KbPressSwapchain}) {
        if (*s != XR_NULL_HANDLE) {
            xrDestroySwapchain(*s);
            *s = XR_NULL_HANDLE;
        }
    }
    for (auto* imgs : {&m_KbImages, &m_KbHoverImages, &m_KbPressImages}) {
        delete[] *imgs;
        *imgs = nullptr;
    }
    m_KbImageCount = m_KbHoverImageCount = m_KbPressImageCount = 0;
    m_KbUploaded = m_KbHoverReady = m_KbPressReady = false;
    if (m_Staging != VK_NULL_HANDLE && m_VkDevice != VK_NULL_HANDLE) {
        s_Vk.UnmapMemory(m_VkDevice, m_StagingMem);
        s_Vk.DestroyBuffer(m_VkDevice, m_Staging, nullptr);
        s_Vk.FreeMemory(m_VkDevice, m_StagingMem, nullptr);
    }
    m_Staging = VK_NULL_HANDLE;
    m_StagingMem = VK_NULL_HANDLE;
    m_StagingPtr = nullptr;
    m_StagingSize = 0;
    if (m_PointerSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_PointerSwapchain);
        m_PointerSwapchain = XR_NULL_HANDLE;
    }
    delete[] m_PointerImages;
    m_PointerImages = nullptr;
    m_PointerImageCount = 0;
    m_PointerReady = false;
    if (m_Input != nullptr) {
        m_Input->destroy();  // aim space／actions／action set，要在 session 之前
        delete m_Input;
        m_Input = nullptr;
    }
    if (m_VrCtl != nullptr) {
        m_VrCtl->destroy();  // grip space／actions／action set，要在 session 之前
        delete m_VrCtl;
        m_VrCtl = nullptr;
    }
    m_ActionSet = XR_NULL_HANDLE;
    if (m_FadeSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_FadeSwapchain);
        m_FadeSwapchain = XR_NULL_HANDLE;
    }
    delete[] m_FadeImages;
    m_FadeImages = nullptr;
    m_FadeImageCount = 0;
    m_FadeReady = false;
    if (m_TrackSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_TrackSpace);
        m_TrackSpace = XR_NULL_HANDLE;
    }
    {
        std::lock_guard<std::mutex> lk(m_HmdMutex);
        m_Hmd = HmdSample();
    }
    m_TimeConv = nullptr;
    if (m_ViewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_ViewSpace);
        m_ViewSpace = XR_NULL_HANDLE;
    }
    if (m_LocalSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_LocalSpace);
        m_LocalSpace = XR_NULL_HANDLE;
    }
    if (m_Session != XR_NULL_HANDLE) {
        if (m_SessionBegun.load()) {
            xrEndSession(m_Session);  // 強制停止時：狀態未必合法，失敗也沒關係，destroy 一律允許
            m_SessionBegun.store(false);
        }
        xrDestroySession(m_Session);
        m_Session = XR_NULL_HANDLE;
    }
    if (m_VkDevice != VK_NULL_HANDLE && gpuWedged()) {
        // GPU 工作卡住：vkDestroyDevice 本身也會等 GPU，刻意洩漏 device、command pool 與 fence
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] GPU wedged - leaking the Vulkan device instead of waiting for it");
    }
    else if (m_VkDevice != VK_NULL_HANDLE) {
        if (m_Fence != VK_NULL_HANDLE && s_Vk.DestroyFence) s_Vk.DestroyFence(m_VkDevice, m_Fence, nullptr);
        if (m_CmdPool != VK_NULL_HANDLE && s_Vk.DestroyCommandPool) s_Vk.DestroyCommandPool(m_VkDevice, m_CmdPool, nullptr);
        if (s_Vk.DestroyDevice) s_Vk.DestroyDevice(m_VkDevice, nullptr);
    }
    m_Fence = VK_NULL_HANDLE;
    m_CmdPool = VK_NULL_HANDLE;
    m_Cmd = VK_NULL_HANDLE;
    m_VkDevice = VK_NULL_HANDLE;
    m_Queue = VK_NULL_HANDLE;
    if (m_VkInstance != VK_NULL_HANDLE && s_Vk.DestroyInstance) {
        s_Vk.DestroyInstance(m_VkInstance, nullptr);
    }
    m_VkInstance = VK_NULL_HANDLE;
    m_VkPhysicalDevice = VK_NULL_HANDLE;
    if (m_Instance != XR_NULL_HANDLE) {
        xrDestroyInstance(m_Instance);
        m_Instance = XR_NULL_HANDLE;
    }
    m_SystemId = XR_NULL_SYSTEM_ID;
    m_State.store(0);
    if (hadAnything) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] destroyed");
    }
}
