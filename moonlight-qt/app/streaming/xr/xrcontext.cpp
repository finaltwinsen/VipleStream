// VipleStream 2.0 §VR M3a X1 — XrContext 實作。設計見 xrcontext.h。

#include "xrcontext.h"

#include <QJsonArray>
#include <QtGlobal>

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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

}  // namespace

// ── Vulkan 函式表（instance／device 建立後填入）──
struct XrContextVk {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
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
    PFN_vkResetFences ResetFences = nullptr;
};

static XrContextVk s_Vk;  // 函式指標與 instance／device 無關的部分；每次 createVulkan 重填

XrContext::XrContext() : XrContext(Options()) {}

XrContext::XrContext(const Options& options) : m_Options(options) {}

XrContext::~XrContext()
{
    shutdown();
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
    shutdown();  // 重入：清掉前一次

    QString err;
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = createInstance(&err) && createSystem(&err) && createVulkan(&err) &&
              createSession(&err) && createQuadSwapchain(&err) && createActions(&err) &&
              waitForReadyAndBegin(timeoutMs, &err);
    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] bring-up failed: %s", qUtf8Printable(err));
        destroyAll();
        if (error) *error = err;
        return false;
    }

    m_StopRequested.store(false);
    m_Lost.store(false);
    m_WarmupEndNs = steadyNowNs() + 2000000000ull;
    m_FrameThreadRunning.store(true, std::memory_order_release);
    m_FrameThread = std::thread(&XrContext::frameThreadMain, this);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] bring-up OK in %lld ms: session begun, frame thread started (quad %ux%u fmt=%lld, refresh=%.1f Hz)",
                static_cast<long long>(ms), m_Options.quadWidth, m_Options.quadHeight,
                static_cast<long long>(m_QuadFormat), static_cast<double>(m_RefreshHz));
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
    for (const char* opt : {kExtRefresh, kExtCylinder, kExtLocalFloor}) {
        if (has(opt)) {
            enable.push_back(opt);
        }
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

    // 1.2 時啟用 timelineSemaphore／hostQueryReset（X2 libplacebo 需要），有才開
    VkPhysicalDeviceVulkan12Features f12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceFeatures2 f2 = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan12Features want12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    const bool use12 = api >= VK_API_VERSION_1_2 && pdp.apiVersion >= VK_API_VERSION_1_2 && s_Vk.GetPhysicalDeviceFeatures2 != nullptr;
    if (use12) {
        f2.pNext = &f12;
        s_Vk.GetPhysicalDeviceFeatures2(m_VkPhysicalDevice, &f2);
        want12.timelineSemaphore = f12.timelineSemaphore;
        want12.hostQueryReset = f12.hostQueryReset;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = vkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    qci.queueFamilyIndex = m_QueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = vkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (use12) {
        dci.pNext = &want12;
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
    LOAD_DEV(ResetFences);
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
                "[VIPLE-XR] vulkan: gpu=\"%s\" api=%u.%u (runtime %u.%u-%u.%u) queueFamily=%u timeline=%d",
                pdp.deviceName, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                VK_API_VERSION_MAJOR(minApi), VK_API_VERSION_MINOR(minApi),
                VK_API_VERSION_MAJOR(maxApi), VK_API_VERSION_MINOR(maxApi),
                m_QueueFamily, use12 ? static_cast<int>(want12.timelineSemaphore) : 0);
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

    if (m_HasRefreshRate) {
        auto pfnRate = xrProc<PFN_xrGetDisplayRefreshRateFB>(m_Instance, "xrGetDisplayRefreshRateFB");
        if (pfnRate) {
            pfnRate(m_Session, &m_RefreshHz);
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
    return true;
}

bool XrContext::createActions(QString* error)
{
    XrActionSetCreateInfo asci = xrStruct<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    qstrncpy(asci.actionSetName, "viple_xr", sizeof(asci.actionSetName));
    qstrncpy(asci.localizedActionSetName, "VipleStream XR", sizeof(asci.localizedActionSetName));
    XrResult r = xrCreateActionSet(m_Instance, &asci, &m_ActionSet);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrCreateActionSet: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }
    XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    xrStringToPath(m_Instance, "/user/hand/left", &hands[0]);
    xrStringToPath(m_Instance, "/user/hand/right", &hands[1]);
    XrActionCreateInfo aci = xrStruct<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
    qstrncpy(aci.actionName, "select", sizeof(aci.actionName));
    qstrncpy(aci.localizedActionName, "Select", sizeof(aci.localizedActionName));
    aci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    r = xrCreateAction(m_ActionSet, &aci, &m_SelectAction);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrCreateAction: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }

    // simple_controller 是所有 runtime 都得接受的 profile；其他 profile 的綁定在 X4
    XrPath profile = XR_NULL_PATH;
    XrPath selL = XR_NULL_PATH, selR = XR_NULL_PATH;
    xrStringToPath(m_Instance, "/interaction_profiles/khr/simple_controller", &profile);
    xrStringToPath(m_Instance, "/user/hand/left/input/select/click", &selL);
    xrStringToPath(m_Instance, "/user/hand/right/input/select/click", &selR);
    XrActionSuggestedBinding sb[2] = {{m_SelectAction, selL}, {m_SelectAction, selR}};
    XrInteractionProfileSuggestedBinding isb = xrStruct<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
    isb.interactionProfile = profile;
    isb.countSuggestedBindings = 2;
    isb.suggestedBindings = sb;
    r = xrSuggestInteractionProfileBindings(m_Instance, &isb);
    if (XR_FAILED(r)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] xrSuggestInteractionProfileBindings(simple): %s",
                    qUtf8Printable(xrResultStr(m_Instance, r)));
    }

    XrSessionActionSetsAttachInfo ai = xrStruct<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    ai.countActionSets = 1;
    ai.actionSets = &m_ActionSet;
    r = xrAttachSessionActionSets(m_Session, &ai);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrAttachSessionActionSets: %1").arg(xrResultStr(m_Instance, r));
        return false;
    }
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

bool XrContext::renderQuad(uint32_t imageIndex)
{
    VkImage img = m_QuadImages[imageIndex];
    s_Vk.ResetCommandBuffer(m_Cmd, 0);
    VkCommandBufferBeginInfo bi = vkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s_Vk.BeginCommandBuffer(m_Cmd, &bi);

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // XR_KHR_vulkan_enable：acquire 後影像在 COLOR_ATTACHMENT_OPTIMAL，release 前也要回到這個 layout
    VkImageMemoryBarrier b = vkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = range;
    s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &b);

    VkClearColorValue gray;
    std::memset(&gray, 0, sizeof(gray));
    gray.float32[0] = gray.float32[1] = gray.float32[2] = 0.08f;  // loading 環境：深灰
    gray.float32[3] = 1.0f;
    s_Vk.CmdClearColorImage(m_Cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &gray, 1, &range);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    s_Vk.CmdPipelineBarrier(m_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &b);
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] 10s frames=%llu missed=%llu (%.2f%%) notRendered=%llu endFrameErr=%llu state=%s period=%.2f ms",
                static_cast<unsigned long long>(d.frames), static_cast<unsigned long long>(d.missed), d.missPercent(),
                static_cast<unsigned long long>(cur.notRendered - prev.notRendered),
                static_cast<unsigned long long>(cur.endFrameErrors - prev.endFrameErrors),
                sessionStateName(cur.currentState), static_cast<double>(m_LastPeriod) / 1e6);
}

void XrContext::frameThreadMain()
{
    const float widthM = 2.0f * m_Options.quadDistanceM *
                         std::tan(m_Options.quadFovDeg * 0.5f * 3.14159265f / 180.0f);
    const float heightM = widthM * static_cast<float>(m_Options.quadHeight) / static_cast<float>(m_Options.quadWidth);

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
        const int state = m_State.load();
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
        if (XR_FAILED(xrWaitFrame(m_Session, &wi, &fs))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        XrFrameBeginInfo bfi = xrStruct<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
        if (XR_FAILED(xrBeginFrame(m_Session, &bfi))) {
            continue;
        }

        const XrCompositionLayerBaseHeader* layers[1];
        uint32_t layerCount = 0;
        XrCompositionLayerQuad quad = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
        if (fs.shouldRender) {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = xrStruct<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
            XrSwapchainImageWaitInfo swi = xrStruct<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
            swi.timeout = 100000000;  // 100 ms
            XrSwapchainImageReleaseInfo ri = xrStruct<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_QuadSwapchain, &ai, &idx)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(m_QuadSwapchain, &swi))) {
                const bool drawn = idx < m_QuadImageCount && renderQuad(idx);
                xrReleaseSwapchainImage(m_QuadSwapchain, &ri);
                if (drawn) {
                    quad.layerFlags = 0;
                    quad.space = m_LocalSpace;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = m_QuadSwapchain;
                    quad.subImage.imageRect.offset = {0, 0};
                    quad.subImage.imageRect.extent = {static_cast<int32_t>(m_Options.quadWidth),
                                                      static_cast<int32_t>(m_Options.quadHeight)};
                    quad.subImage.imageArrayIndex = 0;
                    quad.pose.orientation.w = 1.0f;
                    quad.pose.position.z = -m_Options.quadDistanceM;
                    quad.size = {widthM, heightM};
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                }
            }

            // FOV／眼位：每秒 locate 一次即可（X1 只做量測）
            const uint64_t nowNs = steadyNowNs();
            if (nowNs - lastViewLocateNs > 1000000000ull) {
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
            }
        }

        XrFrameEndInfo ei = xrStruct<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = blend;
        ei.layerCount = layerCount;
        ei.layers = layerCount ? layers : nullptr;
        const XrResult er = xrEndFrame(m_Session, &ei);

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
                }
            }
            m_LastDisplayTime = pdt;
            m_LastPeriod = period;
        }
        maybeLogStats(steadyNowNs());
    }

    const Stats s = stats();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] frame thread exit: frames=%llu missed=%llu (%.2f%%) warmupMissed=%llu highest=%s state=%s lost=%d",
                static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.missed), s.missPercent(),
                static_cast<unsigned long long>(s.warmupMissed),
                sessionStateName(s.highestState), sessionStateName(s.currentState), m_Lost.load() ? 1 : 0);
    m_FrameThreadRunning.store(false, std::memory_order_release);
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

void XrContext::destroyAll()
{
    const bool hadAnything = m_Instance != XR_NULL_HANDLE;
    if (m_VkDevice != VK_NULL_HANDLE && s_Vk.DeviceWaitIdle) {
        std::lock_guard<std::mutex> lk(m_QueueMutex);
        s_Vk.DeviceWaitIdle(m_VkDevice);
    }
    if (m_QuadSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_QuadSwapchain);
        m_QuadSwapchain = XR_NULL_HANDLE;
    }
    delete[] m_QuadImages;
    m_QuadImages = nullptr;
    m_QuadImageCount = 0;
    if (m_SelectAction != XR_NULL_HANDLE) {
        xrDestroyAction(m_SelectAction);
        m_SelectAction = XR_NULL_HANDLE;
    }
    if (m_ActionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_ActionSet);
        m_ActionSet = XR_NULL_HANDLE;
    }
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
    if (m_VkDevice != VK_NULL_HANDLE) {
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
