#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID 1
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN 1
#endif
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif

#include "qxr_session.h"

#include "qxr_log.h"
#include "qxr_video.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <pthread.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kScreenDistanceM = 2.0f;
constexpr float kScreenWidthM = 2.4f;

std::mutex g_loaderMu;
std::atomic<bool> g_loaderReady{false};
// Loader 的單例會一直留著這顆 jobject。Activity 結束時刪掉，同一個行程下一次建 instance 就是懸空的 global ref。
// 刻意不 DeleteGlobalRef，跟這個 .so 一起活到行程結束。
jobject g_loaderContext = nullptr;

// 正數也是成功。XR_TIMEOUT_EXPIRED 是 1，XR_SESSION_LOSS_PENDING 是 3，不能用 != XR_SUCCESS 當失敗。
bool xrOk(XrResult result) {
    return XR_SUCCEEDED(result);
}

// loss：這次呼叫已經成功，但 session 即將失效。呼叫端要走完已 begin 的幀再停。
// discarded：這幀不要畫，但仍要 xrEndFrame，layer 數為 0。
void xrNote(XrResult result, bool* lossPending, bool* discarded) {
    if (discarded != nullptr && result == XR_FRAME_DISCARDED) {
        *discarded = true;
    }
    if (lossPending != nullptr && result == XR_SESSION_LOSS_PENDING && !*lossPending) {
        *lossPending = true;
        QXR_LOGW("FRAME", "XR_SESSION_LOSS_PENDING");
    }
}

bool fail(std::string* error, const char* msg) {
    QXR_LOGE("SESSION", "%s", msg);
    if (error) {
        *error = msg;
    }
    return false;
}

void logXr(bool frame, XrInstance instance, const char* fn, XrResult result) {
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    const char* extra = "";
    if (instance != XR_NULL_HANDLE && xrOk(xrResultToString(instance, result, text))) {
        extra = text;
    }
    if (frame) {
        QXR_LOGE("FRAME", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
    } else {
        QXR_LOGE("SESSION", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
    }
}

bool failXr(std::string* error, XrInstance instance, const char* fn, XrResult result) {
    logXr(false, instance, fn, result);
    if (error) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%s failed: XrResult=%d", fn, static_cast<int>(result));
        *error = buf;
    }
    return false;
}

bool failVk(std::string* error, const char* fn, VkResult result) {
    QXR_LOGE("SESSION", "%s failed: VkResult=%d", fn, static_cast<int>(result));
    if (error) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed: VkResult=%d", fn, static_cast<int>(result));
        *error = buf;
    }
    return false;
}

template <typename Fn>
bool loadProc(XrInstance instance, const char* name, Fn* out, std::string* error) {
    PFN_xrVoidFunction fn = nullptr;
    const XrResult result = xrGetInstanceProcAddr(instance, name, &fn);
    if (!xrOk(result) || fn == nullptr) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrGetInstanceProcAddr(%s) failed: XrResult=%d", name, static_cast<int>(result));
        return fail(error, buf);
    }
    *out = reinterpret_cast<Fn>(fn);
    return true;
}

uint32_t vkApiFromXr(XrVersion version) {
    uint32_t patch = static_cast<uint32_t>(XR_VERSION_PATCH(version));
    if (patch > 0xfffu) {
        patch = 0xfffu;
    }
    const uint32_t major = static_cast<uint32_t>(XR_VERSION_MAJOR(version));
    const uint32_t minor = static_cast<uint32_t>(XR_VERSION_MINOR(version));
#if defined(VK_MAKE_API_VERSION)
    return VK_MAKE_API_VERSION(0, major, minor, patch);
#else
    return VK_MAKE_VERSION(major, minor, patch);
#endif
}

const char* sessionStateName(XrSessionState state) {
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

XrVector3f rotateQuat(const XrQuaternionf& q, const XrVector3f& v) {
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

bool extensionListed(const std::vector<XrExtensionProperties>& props, const char* name) {
    for (const XrExtensionProperties& prop : props) {
        if (std::strcmp(prop.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool deviceExtListed(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const VkExtensionProperties& prop : props) {
        if (std::strcmp(prop.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

struct QxrSession::Impl {
    JavaVM* vm = nullptr;
    jobject activity = nullptr;
    int32_t streamWidth = 0;
    int32_t streamHeight = 0;
    int32_t preferredHz = 0;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionBegun = false;
    bool hasRefresh = false;
    bool created = false;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool fencePending = false;

    std::vector<VkImageView> views;
    int64_t swapchainFormat = 0;
    uint32_t swapWidth = 0;
    uint32_t swapHeight = 0;
    QxrVideo video;

    std::vector<float> rates;
    float currentHz = 0.f;
    uint32_t eyeW = 0;
    uint32_t eyeH = 0;

    bool placed = false;
    bool needPlace = true;
    // 第一次擺放時 position 還沒有效。位置第一次變成有效才再擺一次，之後不再跟著頭動。
    bool awaitingPosition = false;
    bool forcePlace = false;
    bool lossPending = false;
    bool loggedLocate = false;
    float yaw = 0.f;
    XrPosef screenPose{};
    float quadWidthM = kScreenWidthM;
    float quadHeightM = 1.35f;

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stopFlag{false};
    // join 不到自己。start 時 detach，stop 改看這兩個旗標等到迴圈結束。
    std::atomic<bool> workerSet{false};
    pthread_t worker{};
    int threadState = 0;
    std::function<void(int)> onEnded;
    bool endedNotified = false;
    std::atomic<bool> inRelease{false};

    uint64_t statLoops = 0;
    uint64_t statRenders = 0;
    uint64_t statLayers = 0;
    int64_t lastPeriodNs = 0;
    std::chrono::steady_clock::time_point statWindow{};
    int beginFails = 0;

    bool build(const QxrSessionCreateInfo& info, std::string* error);
    void teardown();
    int frameLoop();

private:
    bool createInstance(std::string* error);
    bool createSystem(std::string* error);
    bool createVulkan(std::string* error);
    bool createXrSession(std::string* error);
    bool createSwapchain(std::string* error);
    bool createGpuObjects(std::string* error);
    void tryBegin();
    bool poll(int* reason);
    void tryPlace(XrTime displayTime);
    bool drawFrame(const XrFrameState& frame, bool* sentLayer, int* fatalReason);
    bool recordAndSubmit(uint32_t index);
    void heartbeat();
    void logBlendModes();
};

jobject loaderApplicationContext(JavaVM* vm, jobject activity, std::string* error) {
    if (g_loaderContext != nullptr) {
        return g_loaderContext;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint envResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
            fail(error, "AttachCurrentThread failed while reading the application context");
            return nullptr;
        }
        attached = true;
    } else if (envResult != JNI_OK || env == nullptr) {
        fail(error, "GetEnv failed while reading the application context");
        return nullptr;
    }
    jobject global = nullptr;
    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass != nullptr) {
        jmethodID method = env->GetMethodID(activityClass, "getApplicationContext", "()Landroid/content/Context;");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            method = nullptr;
        }
        if (method != nullptr) {
            jobject local = env->CallObjectMethod(activity, method);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                local = nullptr;
            }
            if (local != nullptr) {
                global = env->NewGlobalRef(local);
                env->DeleteLocalRef(local);
            }
        }
        env->DeleteLocalRef(activityClass);
    } else if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (attached) {
        vm->DetachCurrentThread();
    }
    if (global == nullptr) {
        fail(error, "activity.getApplicationContext failed");
        return nullptr;
    }
    g_loaderContext = global;
    return global;
}

bool QxrSession::Impl::createInstance(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(g_loaderMu);
        if (!g_loaderReady.load(std::memory_order_relaxed)) {
            PFN_xrInitializeLoaderKHR init = nullptr;
            if (!loadProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &init, error)) {
                return false;
            }
            // Application context 活過單一 Activity。傳 Activity 本身的話，第一次 nativeDestroy 刪掉 global ref，
            // loader 裡留的就是懸空參照。這顆 ref 不釋放，原因寫在 g_loaderContext。
            jobject appContext = loaderApplicationContext(vm, activity, error);
            if (appContext == nullptr) {
                return false;
            }
            XrLoaderInitInfoAndroidKHR loaderInfo{};
            loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
            loaderInfo.applicationVM = vm;
            loaderInfo.applicationContext = appContext;
            const XrResult result = init(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
            if (!xrOk(result)) {
                return failXr(error, XR_NULL_HANDLE, "xrInitializeLoaderKHR", result);
            }
            g_loaderReady.store(true, std::memory_order_relaxed);
        } else {
            // 同一個行程的 loader 只能初始化一次，之後的 Activity 沿用第一次的 VM。
            QXR_LOGI("SESSION", "xrInitializeLoaderKHR already completed in this process");
        }
    }

    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (!xrOk(result)) {
        return failXr(error, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", result);
    }
    std::vector<XrExtensionProperties> props(count);
    for (XrExtensionProperties& prop : props) {
        prop.type = XR_TYPE_EXTENSION_PROPERTIES;
        prop.next = nullptr;
    }
    if (count > 0) {
        result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
        if (!xrOk(result)) {
            return failXr(error, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", result);
        }
        props.resize(count);
    }

    struct ExtWant {
        const char* name;
        bool required;
    };
    const ExtWant kExts[] = {
        {"XR_KHR_android_create_instance", true},
        {"XR_KHR_vulkan_enable2", true},
        {"XR_FB_display_refresh_rate", false},
        {"XR_KHR_composition_layer_cylinder", false},
        {"XR_EXT_local_floor", false},
        {"XR_KHR_convert_timespec_time", false},
        {"XR_META_touch_controller_plus", false},
    };
    std::vector<const char*> enabled;
    for (const ExtWant& ext : kExts) {
        if (extensionListed(props, ext.name)) {
            enabled.push_back(ext.name);
            continue;
        }
        if (ext.required) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "missing required instance extension %s", ext.name);
            return fail(error, buf);
        }
        QXR_LOGI("SESSION", "optional extension %s not present", ext.name);
    }
    hasRefresh = extensionListed(props, "XR_FB_display_refresh_rate");

    XrInstanceCreateInfoAndroidKHR androidInfo{};
    androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM = vm;
    // Loader 用的是 Application context。這裡要當次的 Activity，runtime 才對得到這次的視窗。
    androidInfo.applicationActivity = activity;

    XrInstanceCreateInfo ci{};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.next = &androidInfo;
    std::snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "VipleStream");
    std::snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "VipleStream");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.engineVersion = 1;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ci.enabledExtensionNames = enabled.empty() ? nullptr : enabled.data();

    const XrVersion kApiTry[] = {XR_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0)};
    result = XR_ERROR_RUNTIME_UNAVAILABLE;
    for (XrVersion version : kApiTry) {
        ci.applicationInfo.apiVersion = version;
        result = xrCreateInstance(&ci, &instance);
        QXR_LOGI("SESSION", "xrCreateInstance api %d.%d -> XrResult=%d",
                 static_cast<int>(XR_VERSION_MAJOR(version)), static_cast<int>(XR_VERSION_MINOR(version)),
                 static_cast<int>(result));
        if (result != XR_ERROR_API_VERSION_UNSUPPORTED) {
            break;
        }
        instance = XR_NULL_HANDLE;
    }
    if (!xrOk(result) || instance == XR_NULL_HANDLE) {
        instance = XR_NULL_HANDLE;
        return failXr(error, XR_NULL_HANDLE, "xrCreateInstance", result);
    }

    std::string enabledList;
    for (size_t i = 0; i < enabled.size(); ++i) {
        if (i) enabledList += ",";
        enabledList += enabled[i];
    }
    XrInstanceProperties ip{};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    result = xrGetInstanceProperties(instance, &ip);
    if (!xrOk(result)) {
        logXr(false, instance, "xrGetInstanceProperties", result);
    } else {
        QXR_LOGI("SESSION", "runtime \"%s\" %u.%u.%u enabled=[%s]", ip.runtimeName,
                 static_cast<unsigned>(XR_VERSION_MAJOR(ip.runtimeVersion)),
                 static_cast<unsigned>(XR_VERSION_MINOR(ip.runtimeVersion)),
                 static_cast<unsigned>(XR_VERSION_PATCH(ip.runtimeVersion)), enabledList.c_str());
    }
    return true;
}

bool QxrSession::Impl::createSystem(std::string* error) {
    XrSystemGetInfo gi{};
    gi.type = XR_TYPE_SYSTEM_GET_INFO;
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    constexpr int kRetryMs = 500;
    constexpr int kBudgetMs = 10000;
    const auto start = std::chrono::steady_clock::now();
    auto waitedMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    };
    int attempts = 1;
    XrResult result = xrGetSystem(instance, &gi, &systemId);
    while (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE && waitedMs() + kRetryMs <= kBudgetMs) {
        if (attempts == 1) {
            QXR_LOGW("SESSION", "xrGetSystem: HMD not ready, retry every %d ms up to %d ms", kRetryMs, kBudgetMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
        result = xrGetSystem(instance, &gi, &systemId);
        ++attempts;
    }
    if (!xrOk(result)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrGetSystem(HMD) failed: XrResult=%d attempts=%d", static_cast<int>(result), attempts);
        logXr(false, instance, "xrGetSystem", result);
        if (error) *error = buf;
        return false;
    }
    if (attempts > 1) {
        QXR_LOGI("SESSION", "xrGetSystem: HMD ready after %lld ms (%d attempts)", static_cast<long long>(waitedMs()), attempts);
    }

    XrSystemProperties sp{};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    result = xrGetSystemProperties(instance, systemId, &sp);
    if (!xrOk(result)) {
        logXr(false, instance, "xrGetSystemProperties", result);
    } else {
        QXR_LOGI("SESSION", "system \"%s\" vendor=0x%x layers=%u", sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);
    }

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrEnumerateViewConfigurationViews", result);
    }
    if (viewCount == 0) {
        return fail(error, "xrEnumerateViewConfigurationViews returned no views");
    }
    std::vector<XrViewConfigurationView> viewsConfig(viewCount);
    for (XrViewConfigurationView& view : viewsConfig) {
        view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        view.next = nullptr;
    }
    result = xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount,
                                               viewsConfig.data());
    if (!xrOk(result) || viewCount == 0) {
        return failXr(error, instance, "xrEnumerateViewConfigurationViews", result);
    }
    eyeW = viewsConfig[0].recommendedImageRectWidth;
    eyeH = viewsConfig[0].recommendedImageRectHeight;
    if (viewCount > 1 && (viewsConfig[1].recommendedImageRectWidth != eyeW || viewsConfig[1].recommendedImageRectHeight != eyeH)) {
        QXR_LOGW("SESSION", "eyes differ: %ux%u vs %ux%u", eyeW, eyeH, viewsConfig[1].recommendedImageRectWidth,
                 viewsConfig[1].recommendedImageRectHeight);
    }
    QXR_LOGI("SESSION", "recommended eye %ux%u", eyeW, eyeH);
    return true;
}

bool QxrSession::Impl::createVulkan(std::string* error) {
    PFN_xrGetVulkanGraphicsRequirements2KHR reqFn = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInst = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDev = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDev = nullptr;
    if (!loadProc(instance, "xrGetVulkanGraphicsRequirements2KHR", &reqFn, error) ||
        !loadProc(instance, "xrCreateVulkanInstanceKHR", &createInst, error) ||
        !loadProc(instance, "xrGetVulkanGraphicsDevice2KHR", &getDev, error) ||
        !loadProc(instance, "xrCreateVulkanDeviceKHR", &createDev, error)) {
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    XrResult result = reqFn(instance, systemId, &req);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrGetVulkanGraphicsRequirements2KHR", result);
    }
    const uint32_t minApi = vkApiFromXr(req.minApiVersionSupported);
    const uint32_t maxApi = vkApiFromXr(req.maxApiVersionSupported);
    uint32_t api = VK_API_VERSION_1_1;
    if (minApi > api) {
        api = minApi;
    }
    if (maxApi != 0 && api > maxApi) {
        api = maxApi;
    }
    uint32_t loaderApi = 0;
    if (vkEnumerateInstanceVersion(&loaderApi) != VK_SUCCESS || loaderApi == 0) {
        loaderApi = VK_API_VERSION_1_0;
    }
    if (api > loaderApi) {
        api = loaderApi;
    }
    if (api < VK_API_VERSION_1_1) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Vulkan API below 1.1 (min=0x%x max=0x%x loader=0x%x)", minApi, maxApi, loaderApi);
        return fail(error, buf);
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VipleStream";
    app.pEngineName = "VipleStream";
    app.apiVersion = api;
    VkInstanceCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vci.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xci{};
    xci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    xci.systemId = systemId;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xci.vulkanCreateInfo = &vci;
    VkResult vkResult = VK_SUCCESS;
    result = createInst(instance, &xci, &vkInstance, &vkResult);
    if (!xrOk(result) || vkResult != VK_SUCCESS || vkInstance == VK_NULL_HANDLE) {
        vkInstance = VK_NULL_HANDLE;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrCreateVulkanInstanceKHR failed: XrResult=%d VkResult=%d", static_cast<int>(result),
                      static_cast<int>(vkResult));
        logXr(false, instance, "xrCreateVulkanInstanceKHR", result);
        if (vkResult != VK_SUCCESS) {
            QXR_LOGE("SESSION", "xrCreateVulkanInstanceKHR vk: VkResult=%d", static_cast<int>(vkResult));
        }
        if (error) *error = buf;
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR dgi{};
    dgi.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    dgi.systemId = systemId;
    dgi.vulkanInstance = vkInstance;
    result = getDev(instance, &dgi, &physicalDevice);
    if (!xrOk(result) || physicalDevice == VK_NULL_HANDLE) {
        return failXr(error, instance, "xrGetVulkanGraphicsDevice2KHR", result);
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        return fail(error, "GPU Vulkan API is below 1.1");
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount > 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
    }
    bool foundQueue = false;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0) {
            queueFamily = i;
            foundQueue = true;
            break;
        }
    }
    if (!foundQueue) {
        return fail(error, "no graphics queue family");
    }

    uint32_t extCount = 0;
    vkResult = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkEnumerateDeviceExtensionProperties", vkResult);
    }
    std::vector<VkExtensionProperties> deviceExts(extCount);
    if (extCount > 0) {
        vkResult = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, deviceExts.data());
        if (vkResult != VK_SUCCESS) {
            return failVk(error, "vkEnumerateDeviceExtensionProperties", vkResult);
        }
        deviceExts.resize(extCount);
    }
    if (!deviceExtListed(deviceExts, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        return fail(error, "missing device extension VK_ANDROID_external_memory_android_hardware_buffer");
    }
    if (!deviceExtListed(deviceExts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        return fail(error, "missing device extension VK_EXT_queue_family_foreign");
    }
    // 1.1 已升成核心的不放進 ppEnabledExtensionNames，否則部分驅動會拒。
    const char* kCore11[] = {
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    };
    std::vector<const char*> enableExts;
    enableExts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    enableExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    if (api < VK_API_VERSION_1_1) {
        for (const char* name : kCore11) {
            if (!deviceExtListed(deviceExts, name)) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "missing device extension %s", name);
                return fail(error, buf);
            }
            enableExts.push_back(name);
        }
    } else {
        for (const char* name : kCore11) {
            if (!deviceExtListed(deviceExts, name)) {
                QXR_LOGI("SESSION", "%s not advertised; using Vulkan 1.1 core", name);
            }
        }
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures supportedYcbcr{};
    supportedYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceFeatures2 queryFeatures{};
    queryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    queryFeatures.pNext = &supportedYcbcr;
    vkGetPhysicalDeviceFeatures2(physicalDevice, &queryFeatures);
    if (supportedYcbcr.samplerYcbcrConversion != VK_TRUE) {
        return fail(error, "samplerYcbcrConversion is not supported");
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures enableYcbcr{};
    enableYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    enableYcbcr.samplerYcbcrConversion = VK_TRUE;
    VkPhysicalDeviceFeatures2 enableFeatures{};
    enableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enableFeatures.pNext = &enableYcbcr;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enableFeatures;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queueInfo;
    dci.enabledExtensionCount = static_cast<uint32_t>(enableExts.size());
    dci.ppEnabledExtensionNames = enableExts.data();
    // pEnabledFeatures 與 pNext 的 Features2 不能同時給，否則建立會被拒。
    dci.pEnabledFeatures = nullptr;

    XrVulkanDeviceCreateInfoKHR xdci{};
    xdci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    xdci.systemId = systemId;
    xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = physicalDevice;
    xdci.vulkanCreateInfo = &dci;
    vkResult = VK_SUCCESS;
    result = createDev(instance, &xdci, &device, &vkResult);
    // runtime 可能把 feature 鏈接到自己的 stack。這兩個結構只活在這次呼叫，返回後不再跟著 pNext 走。
    enableFeatures.pNext = &enableYcbcr;
    enableYcbcr.pNext = nullptr;
    if (!xrOk(result) || vkResult != VK_SUCCESS || device == VK_NULL_HANDLE) {
        device = VK_NULL_HANDLE;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "xrCreateVulkanDeviceKHR failed: XrResult=%d VkResult=%d", static_cast<int>(result),
                      static_cast<int>(vkResult));
        logXr(false, instance, "xrCreateVulkanDeviceKHR", result);
        if (vkResult != VK_SUCCESS) {
            QXR_LOGE("SESSION", "xrCreateVulkanDeviceKHR vk: VkResult=%d", static_cast<int>(vkResult));
        }
        if (error) *error = buf;
        return false;
    }

    queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        return fail(error, "vkGetDeviceQueue returned null");
    }
    QXR_LOGI("SESSION", "vulkan api=%u.%u queueFamily=%u gpu=\"%s\"", VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
             queueFamily, props.deviceName);
    return true;
}

bool QxrSession::Impl::createXrSession(std::string* error) {
    XrGraphicsBindingVulkan2KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
    binding.instance = vkInstance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &binding;
    sci.systemId = systemId;
    XrResult result = xrCreateSession(instance, &sci, &session);
    if (!xrOk(result) || session == XR_NULL_HANDLE) {
        session = XR_NULL_HANDLE;
        return failXr(error, instance, "xrCreateSession", result);
    }

    uint32_t spaceCount = 0;
    result = xrEnumerateReferenceSpaces(session, 0, &spaceCount, nullptr);
    if (!xrOk(result)) {
        logXr(false, instance, "xrEnumerateReferenceSpaces", result);
    } else if (spaceCount > 0) {
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        result = xrEnumerateReferenceSpaces(session, spaceCount, &spaceCount, spaces.data());
        if (xrOk(result)) {
            xrNote(result, &lossPending, nullptr);
            std::string list;
            for (uint32_t i = 0; i < spaceCount; ++i) {
                if (i) list += ",";
                char one[16];
                std::snprintf(one, sizeof(one), "%d", static_cast<int>(spaces[i]));
                list += one;
            }
            QXR_LOGI("SESSION", "reference spaces=[%s]", list.c_str());
        } else {
            logXr(false, instance, "xrEnumerateReferenceSpaces", result);
        }
    }

    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    result = xrCreateReferenceSpace(session, &spaceInfo, &localSpace);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrCreateReferenceSpace(LOCAL)", result);
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    result = xrCreateReferenceSpace(session, &spaceInfo, &viewSpace);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrCreateReferenceSpace(VIEW)", result);
    }

    if (!hasRefresh) {
        return true;
    }
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    if (!loadProc(instance, "xrEnumerateDisplayRefreshRatesFB", &enumRates, error) ||
        !loadProc(instance, "xrGetDisplayRefreshRateFB", &getRate, error) ||
        !loadProc(instance, "xrRequestDisplayRefreshRateFB", &requestRate, error)) {
        hasRefresh = false;
        if (error) error->clear();
        QXR_LOGW("SESSION", "display refresh procs missing; continuing without them");
        return true;
    }
    float hz = 0.0f;
    result = getRate(session, &hz);
    if (xrOk(result)) {
        currentHz = hz;
        xrNote(result, &lossPending, nullptr);
    } else {
        logXr(false, instance, "xrGetDisplayRefreshRateFB", result);
    }
    uint32_t rateCount = 0;
    result = enumRates(session, 0, &rateCount, nullptr);
    if (!xrOk(result)) {
        logXr(false, instance, "xrEnumerateDisplayRefreshRatesFB", result);
        return true;
    }
    if (rateCount > 0) {
        rates.assign(rateCount, 0.0f);
        result = enumRates(session, rateCount, &rateCount, rates.data());
        if (!xrOk(result)) {
            rates.clear();
            logXr(false, instance, "xrEnumerateDisplayRefreshRatesFB", result);
            return true;
        }
        rates.resize(rateCount);
    }
    std::string list;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i) list += ",";
        char one[32];
        std::snprintf(one, sizeof(one), "%.1f", static_cast<double>(rates[i]));
        list += one;
    }
    if (preferredHz > 0 && !rates.empty()) {
        float best = rates[0];
        const float want = static_cast<float>(preferredHz);
        for (float rate : rates) {
            const float dr = std::fabs(rate - want);
            const float db = std::fabs(best - want);
            if (dr < db || (dr == db && rate > best)) {
                best = rate;
            }
        }
        result = requestRate(session, best);
        if (!xrOk(result)) {
            logXr(false, instance, "xrRequestDisplayRefreshRateFB", result);
        } else {
            xrNote(result, &lossPending, nullptr);
        }
        QXR_LOGI("SESSION", "refresh rates=[%s] current=%.1f requested=%.1f (want %d) XrResult=%d", list.c_str(),
                 static_cast<double>(currentHz), static_cast<double>(best), preferredHz, static_cast<int>(result));
    } else {
        QXR_LOGI("SESSION", "refresh rates=[%s] current=%.1f (no request)", list.c_str(), static_cast<double>(currentHz));
    }
    return true;
}

bool QxrSession::Impl::createSwapchain(std::string* error) {
    uint32_t count = 0;
    XrResult result = xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    if (!xrOk(result)) {
        return failXr(error, instance, "xrEnumerateSwapchainFormats", result);
    }
    std::vector<int64_t> formats(count);
    if (count > 0) {
        result = xrEnumerateSwapchainFormats(session, count, &count, formats.data());
        if (!xrOk(result)) {
            return failXr(error, instance, "xrEnumerateSwapchainFormats", result);
        }
        formats.resize(count);
    }
    const int64_t kWant[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    for (int64_t want : kWant) {
        for (int64_t format : formats) {
            if (format == want) {
                swapchainFormat = want;
                break;
            }
        }
        if (swapchainFormat != 0) {
            break;
        }
    }
    if (swapchainFormat == 0) {
        std::string list;
        const size_t shown = formats.size() < 8 ? formats.size() : 8;
        for (size_t i = 0; i < shown; ++i) {
            if (i) list += ",";
            list += std::to_string(formats[i]);
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf), "swapchain has neither R8G8B8A8_SRGB nor UNORM (have %s)", list.c_str());
        return fail(error, buf);
    }

    XrSwapchainCreateInfo ci{};
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = swapchainFormat;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(streamWidth);
    ci.height = static_cast<uint32_t>(streamHeight);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    result = xrCreateSwapchain(session, &ci, &swapchain);
    if (!xrOk(result) || swapchain == XR_NULL_HANDLE) {
        swapchain = XR_NULL_HANDLE;
        return failXr(error, instance, "xrCreateSwapchain", result);
    }
    swapWidth = ci.width;
    swapHeight = ci.height;

    uint32_t imageCount = 0;
    result = xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr);
    if (!xrOk(result) || imageCount == 0) {
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount);
    for (XrSwapchainImageVulkan2KHR& image : images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        image.next = nullptr;
    }
    result = xrEnumerateSwapchainImages(swapchain, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (!xrOk(result) || imageCount == 0) {
        return failXr(error, instance, "xrEnumerateSwapchainImages", result);
    }
    views.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i].image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = static_cast<VkFormat>(swapchainFormat);
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult vkResult = vkCreateImageView(device, &viewInfo, nullptr, &view);
        if (vkResult != VK_SUCCESS || view == VK_NULL_HANDLE) {
            return failVk(error, "vkCreateImageView(swapchain)", vkResult);
        }
        views.push_back(view);
    }
    QXR_LOGI("SESSION", "swapchain %ux%u format=%d images=%u", swapWidth, swapHeight, static_cast<int>(swapchainFormat), imageCount);
    return true;
}

bool QxrSession::Impl::createGpuObjects(std::string* error) {
    QxrVideoCreateInfo videoInfo{};
    videoInfo.physicalDevice = physicalDevice;
    videoInfo.device = device;
    videoInfo.queueFamily = queueFamily;
    videoInfo.targetFormat = static_cast<VkFormat>(swapchainFormat);
    videoInfo.streamWidth = streamWidth;
    videoInfo.streamHeight = streamHeight;
    const char* videoError = nullptr;
    if (!video.create(videoInfo, &videoError)) {
        return fail(error, videoError != nullptr ? videoError : "QxrVideo::create failed");
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VkResult vkResult = vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkCreateCommandPool", vkResult);
    }
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = cmdPool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkResult = vkAllocateCommandBuffers(device, &alloc, &cmd);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkAllocateCommandBuffers", vkResult);
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkResult = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    if (vkResult != VK_SUCCESS) {
        return failVk(error, "vkCreateFence", vkResult);
    }
    return true;
}

bool QxrSession::Impl::build(const QxrSessionCreateInfo& info, std::string* error) {
    if (info.vm == nullptr || info.activity == nullptr) {
        return fail(error, "activity or JavaVM is null");
    }
    if (info.streamWidth <= 0 || info.streamHeight <= 0) {
        return fail(error, "stream size is empty");
    }
    vm = info.vm;
    activity = info.activity;
    streamWidth = info.streamWidth;
    streamHeight = info.streamHeight;
    preferredHz = info.preferredHz;
    screenPose.orientation.w = 1.0f;
    quadWidthM = kScreenWidthM;
    quadHeightM = kScreenWidthM * static_cast<float>(streamHeight) / static_cast<float>(streamWidth);
    placed = false;
    needPlace = true;
    awaitingPosition = false;
    forcePlace = false;
    lossPending = false;
    loggedLocate = false;
    yaw = 0.0f;
    endedNotified = false;
    fencePending = false;
    sessionBegun = false;
    sessionState = XR_SESSION_STATE_UNKNOWN;
    beginFails = 0;
    rates.clear();
    currentHz = 0.0f;
    eyeW = 0;
    eyeH = 0;
    swapchainFormat = 0;

    if (!createInstance(error) || !createSystem(error) || !createVulkan(error) || !createXrSession(error) || !createSwapchain(error) ||
        !createGpuObjects(error)) {
        return false;
    }
    created = true;
    if (error) {
        error->clear();
    }
    QXR_LOGI("SESSION", "created %dx%d quad %.2fx%.2f m", streamWidth, streamHeight, static_cast<double>(quadWidthM),
             static_cast<double>(quadHeightM));
    return true;
}

void QxrSession::Impl::teardown() {
    const bool noisy = instance != XR_NULL_HANDLE || device != VK_NULL_HANDLE || session != XR_NULL_HANDLE;
    if (noisy) {
        QXR_LOGI("SESSION", "teardown");
    }
    if (device != VK_NULL_HANDLE) {
        const VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) {
            QXR_LOGE("SESSION", "vkDeviceWaitIdle failed: VkResult=%d", static_cast<int>(result));
        }
    }
    video.destroy();
    for (VkImageView view : views) {
        if (view != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
    }
    views.clear();
    if (swapchain != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySwapchain(swapchain);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySwapchain", result);
        }
        swapchain = XR_NULL_HANDLE;
    }
    if (viewSpace != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySpace(viewSpace);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySpace(VIEW)", result);
        }
        viewSpace = XR_NULL_HANDLE;
    }
    if (localSpace != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySpace(localSpace);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySpace(LOCAL)", result);
        }
        localSpace = XR_NULL_HANDLE;
    }
    if (session != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySession(session);
        if (!xrOk(result)) {
            logXr(false, instance, "xrDestroySession", result);
        }
        session = XR_NULL_HANDLE;
    }
    sessionBegun = false;
    if (cmdPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, cmdPool, nullptr);
    }
    cmdPool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    if (fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    fence = VK_NULL_HANDLE;
    fencePending = false;
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    queue = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    if (vkInstance != VK_NULL_HANDLE) {
        vkDestroyInstance(vkInstance, nullptr);
        vkInstance = VK_NULL_HANDLE;
    }
    if (instance != XR_NULL_HANDLE) {
        const XrResult result = xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
        if (!xrOk(result)) {
            QXR_LOGE("SESSION", "xrDestroyInstance failed: XrResult=%d", static_cast<int>(result));
        }
    }
    created = false;
    {
        std::lock_guard<std::mutex> lock(mu);
        onEnded = nullptr;
    }
}

void QxrSession::Impl::tryBegin() {
    if (sessionBegun || session == XR_NULL_HANDLE) {
        return;
    }
    XrSessionBeginInfo info{};
    info.type = XR_TYPE_SESSION_BEGIN_INFO;
    info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const XrResult result = xrBeginSession(session, &info);
    if (!xrOk(result)) {
        ++beginFails;
        if (beginFails == 1 || (beginFails % 20) == 0) {
            logXr(true, instance, "xrBeginSession", result);
        }
        return;
    }
    xrNote(result, &lossPending, nullptr);
    beginFails = 0;
    sessionBegun = true;
    QXR_LOGI("SESSION", "xrBeginSession");
}

void QxrSession::Impl::logBlendModes() {
    uint32_t count = 0;
    XrResult result = xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (!xrOk(result)) {
        logXr(true, instance, "xrEnumerateEnvironmentBlendModes", result);
        return;
    }
    std::vector<XrEnvironmentBlendMode> modes(count);
    if (count > 0) {
        result = xrEnumerateEnvironmentBlendModes(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data());
        if (!xrOk(result)) {
            logXr(true, instance, "xrEnumerateEnvironmentBlendModes", result);
            return;
        }
        modes.resize(count);
    }
    bool opaque = false;
    for (XrEnvironmentBlendMode mode : modes) {
        if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
            opaque = true;
        }
    }
    if (!opaque) {
        QXR_LOGE("FRAME", "runtime did not list OPAQUE blend; xrEndFrame still requests it");
    }
}

bool QxrSession::Impl::poll(int* reason) {
    for (;;) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        const XrResult result = xrPollEvent(instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            return true;
        }
        if (!xrOk(result)) {
            logXr(true, instance, "xrPollEvent", result);
            if (result == XR_ERROR_INSTANCE_LOST) {
                *reason = kQxrEndInstanceLoss;
                return false;
            }
            if (result == XR_ERROR_SESSION_LOST) {
                *reason = kQxrEndLossPending;
                return false;
            }
            return true;
        }
        xrNote(result, &lossPending, nullptr);
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            QXR_LOGW("FRAME", "instance loss pending");
            *reason = kQxrEndInstanceLoss;
            return false;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (session != XR_NULL_HANDLE && changed->session != session) {
                break;
            }
            QXR_LOGI("SESSION", "state %s -> %s", sessionStateName(sessionState), sessionStateName(changed->state));
            sessionState = changed->state;
            if (changed->state == XR_SESSION_STATE_READY) {
                tryBegin();
            } else if (changed->state == XR_SESSION_STATE_STOPPING) {
                if (sessionBegun && session != XR_NULL_HANDLE) {
                    const XrResult endResult = xrEndSession(session);
                    if (!xrOk(endResult)) {
                        logXr(true, instance, "xrEndSession", endResult);
                    } else {
                        xrNote(endResult, &lossPending, nullptr);
                    }
                    sessionBegun = false;
                }
            } else if (changed->state == XR_SESSION_STATE_EXITING) {
                *reason = kQxrEndExiting;
                return false;
            } else if (changed->state == XR_SESSION_STATE_LOSS_PENDING) {
                *reason = kQxrEndLossPending;
                return false;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            auto* changed = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&event);
            currentHz = changed->toDisplayRefreshRate;
            QXR_LOGI("FRAME", "refresh %.1f -> %.1f Hz", static_cast<double>(changed->fromDisplayRefreshRate),
                     static_cast<double>(changed->toDisplayRefreshRate));
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            auto* changed = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
            QXR_LOGI("FRAME", "reference space %d changed poseValid=%d", static_cast<int>(changed->referenceSpaceType),
                     changed->poseValid ? 1 : 0);
            needPlace = true;
            forcePlace = true;
            loggedLocate = false;
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            QXR_LOGW("FRAME", "runtime event queue overflowed");
            break;
        default:
            break;
        }
        if (lossPending) {
            *reason = kQxrEndLossPending;
            return false;
        }
    }
}

void QxrSession::Impl::tryPlace(XrTime displayTime) {
    XrSpaceLocation loc{};
    loc.type = XR_TYPE_SPACE_LOCATION;
    const XrResult result = xrLocateSpace(viewSpace, localSpace, displayTime, &loc);
    if (!xrOk(result)) {
        if (!loggedLocate) {
            loggedLocate = true;
            logXr(true, instance, "xrLocateSpace", result);
        }
        return;
    }
    xrNote(result, &lossPending, nullptr);
    const bool orientationValid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    const bool positionValid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (!orientationValid) {
        if (!loggedLocate) {
            loggedLocate = true;
            QXR_LOGW("FRAME", "head orientation not valid yet flags=0x%x", static_cast<unsigned>(loc.locationFlags));
        }
        return;
    }
    // 已經用原點擺過、位置還沒回來：不要每幀跟著頭轉。空間基準變了才強制再擺一次。
    if (placed && awaitingPosition && !positionValid && !forcePlace) {
        return;
    }
    loggedLocate = false;
    const XrVector3f head = positionValid ? loc.pose.position : XrVector3f{0.0f, 0.0f, 0.0f};
    const XrVector3f forward = rotateQuat(loc.pose.orientation, {0.0f, 0.0f, -1.0f});
    const float horizontal = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (horizontal > 1.0e-3f) {
        yaw = std::atan2(-forward.x / horizontal, -forward.z / horizontal);
    }
    screenPose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    screenPose.position = {head.x - std::sin(yaw) * kScreenDistanceM, head.y, head.z - std::cos(yaw) * kScreenDistanceM};
    placed = true;
    forcePlace = false;
    if (positionValid) {
        awaitingPosition = false;
        needPlace = false;
    } else {
        // 原點、高度 0，螢幕中心仍在前方同樣的距離。位置第一次有效時再擺一次。
        awaitingPosition = true;
        needPlace = true;
    }
    QXR_LOGI("SESSION", "screen placed yaw=%.1f deg head=(%.2f, %.2f, %.2f) center=(%.2f, %.2f, %.2f) size=%.2fx%.2f m positionValid=%d",
             static_cast<double>(yaw * 180.0f / 3.14159265f), static_cast<double>(head.x),
             static_cast<double>(head.y), static_cast<double>(head.z),
             static_cast<double>(screenPose.position.x), static_cast<double>(screenPose.position.y),
             static_cast<double>(screenPose.position.z), static_cast<double>(quadWidthM), static_cast<double>(quadHeightM),
             positionValid ? 1 : 0);
}

bool QxrSession::Impl::recordAndSubmit(uint32_t index) {
    VkResult result = vkResetCommandBuffer(cmd, 0);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkResetCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(cmd, &begin);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkBeginCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    // render pass 的 initial／final layout 已是 COLOR_ATTACHMENT_OPTIMAL，跟 swapchain acquire／release 相同，這裡不再轉。
    const bool drew = video.renderTo(cmd, views[index], swapWidth, swapHeight, video.sourceUv());
    result = vkEndCommandBuffer(cmd);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkEndCommandBuffer failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    if (!drew) {
        QXR_LOGE("FRAME", "QxrVideo::renderTo failed");
        return false;
    }
    // 只有這條 frame thread 碰這條 VkQueue。xrBeginFrame／xrEndFrame／xrAcquire／xrRelease 跟 submit 在同一條執行緒上前後發生，沒有第二條執行緒。
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        QXR_LOGE("FRAME", "vkQueueSubmit failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    fencePending = true;
    return true;
}

bool QxrSession::Impl::drawFrame(const XrFrameState& frame, bool* sentLayer, int* fatalReason) {
    *sentLayer = false;
    if (needPlace) {
        tryPlace(frame.predictedDisplayTime);
    }
    if (fencePending) {
        // consumeLatest 可能拆掉上一幀還掛在指令裡的 VkImage，要等這張 fence 才算做完。
        VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            QXR_LOGE("FRAME", "vkWaitForFences failed: VkResult=%d", static_cast<int>(result));
            fencePending = false;
            *fatalReason = kQxrEndLossPending;
            return false;
        }
        result = vkResetFences(device, 1, &fence);
        if (result != VK_SUCCESS) {
            QXR_LOGE("FRAME", "vkResetFences failed: VkResult=%d", static_cast<int>(result));
            fencePending = false;
            *fatalReason = kQxrEndLossPending;
            return false;
        }
        fencePending = false;
    }
    video.consumeLatest();

    XrSwapchainImageAcquireInfo acquire{};
    acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    uint32_t index = 0;
    const XrResult acquireResult = xrAcquireSwapchainImage(swapchain, &acquire, &index);
    if (!xrOk(acquireResult)) {
        // 沒有 acquire 成功就不能 release，否則 runtime 回 XR_ERROR_CALL_ORDER_INVALID。
        logXr(true, instance, "xrAcquireSwapchainImage", acquireResult);
        if (acquireResult == XR_ERROR_SESSION_LOST) {
            *fatalReason = kQxrEndLossPending;
            return false;
        }
        if (acquireResult == XR_ERROR_INSTANCE_LOST) {
            *fatalReason = kQxrEndInstanceLoss;
            return false;
        }
        return true;
    }
    xrNote(acquireResult, &lossPending, nullptr);

    XrSwapchainImageWaitInfo wait{};
    wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    const XrResult waitResult = xrWaitSwapchainImage(swapchain, &wait);
    // XR_TIMEOUT_EXPIRED 是成功碼，但這張還沒就緒，不能畫。acquire 已成功就一定要 release。
    const bool imageReady = xrOk(waitResult) && waitResult != XR_TIMEOUT_EXPIRED;
    if (!xrOk(waitResult)) {
        logXr(true, instance, "xrWaitSwapchainImage", waitResult);
    } else {
        xrNote(waitResult, &lossPending, nullptr);
        if (waitResult == XR_TIMEOUT_EXPIRED) {
            QXR_LOGW("FRAME", "xrWaitSwapchainImage returned XR_TIMEOUT_EXPIRED");
        }
    }
    bool submitted = false;
    if (imageReady) {
        if (index >= views.size() || views[index] == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) {
            QXR_LOGE("FRAME", "swapchain image index %u out of range (count=%u)", index, static_cast<unsigned>(views.size()));
        } else {
            submitted = recordAndSubmit(index);
        }
    }

    XrSwapchainImageReleaseInfo release{};
    release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    const XrResult releaseResult = xrReleaseSwapchainImage(swapchain, &release);
    if (!xrOk(releaseResult)) {
        logXr(true, instance, "xrReleaseSwapchainImage", releaseResult);
        return true;
    }
    xrNote(releaseResult, &lossPending, nullptr);
    if (submitted && placed) {
        *sentLayer = true;
    }
    return true;
}

void QxrSession::Impl::heartbeat() {
    const auto now = std::chrono::steady_clock::now();
    video.pumpStats();
    if (now - statWindow < std::chrono::seconds(10)) {
        return;
    }
    const double periodMs = static_cast<double>(lastPeriodNs) / 1.0e6;
    QXR_LOGI("FRAME", "loops=%llu shouldRender=%llu layers=%llu period=%.2f ms hz=%.1f (10s)",
             static_cast<unsigned long long>(statLoops), static_cast<unsigned long long>(statRenders),
             static_cast<unsigned long long>(statLayers), periodMs, static_cast<double>(currentHz));
    statLoops = 0;
    statRenders = 0;
    statLayers = 0;
    statWindow = now;
}

int QxrSession::Impl::frameLoop() {
    statWindow = std::chrono::steady_clock::now();
    statLoops = 0;
    statRenders = 0;
    statLayers = 0;
    logBlendModes();
    while (true) {
        heartbeat();
        ++statLoops;
        if (stopFlag.load(std::memory_order_acquire)) {
            return 0;
        }
        int reason = 0;
        if (!poll(&reason)) {
            return reason;
        }
        if (lossPending) {
            return kQxrEndLossPending;
        }
        if (!sessionBegun) {
            if (sessionState == XR_SESSION_STATE_READY) {
                tryBegin();
            }
            if (lossPending) {
                return kQxrEndLossPending;
            }
            // 還沒 begin 就不會畫。排空 reader，解碼器才有輸出緩衝。這裡不匯入，也不碰快取。
            video.drainReader();
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopFlag.load(std::memory_order_acquire); });
            continue;
        }

        XrFrameWaitInfo wait{};
        wait.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState frame{};
        frame.type = XR_TYPE_FRAME_STATE;
        const XrResult waitResult = xrWaitFrame(session, &wait, &frame);
        if (!xrOk(waitResult)) {
            logXr(true, instance, "xrWaitFrame", waitResult);
            if (waitResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (waitResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
            video.drainReader();
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopFlag.load(std::memory_order_acquire); });
            continue;
        }
        xrNote(waitResult, &lossPending, nullptr);
        lastPeriodNs = frame.predictedDisplayPeriod;

        XrFrameBeginInfo begin{};
        begin.type = XR_TYPE_FRAME_BEGIN_INFO;
        const XrResult beginResult = xrBeginFrame(session, &begin);
        if (!xrOk(beginResult)) {
            logXr(true, instance, "xrBeginFrame", beginResult);
            video.drainReader();
            if (beginResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (beginResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
            // begin 失敗就不能 end。
            continue;
        }
        bool discarded = false;
        xrNote(beginResult, &lossPending, &discarded);
        if (discarded) {
            QXR_LOGW("FRAME", "xrBeginFrame returned XR_FRAME_DISCARDED");
        }

        bool sentLayer = false;
        int fatalReason = 0;
        bool fatal = false;
        if (!discarded && frame.shouldRender) {
            ++statRenders;
            if (!drawFrame(frame, &sentLayer, &fatalReason)) {
                fatal = true;
            }
        } else {
            // 不畫的時候也要把 reader 排到只剩最新一張。fence 可能還沒到，不能拆快取。
            video.drainReader();
        }
        if (sentLayer) {
            ++statLayers;
        }

        XrCompositionLayerQuad quad{};
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        const XrCompositionLayerBaseHeader* header = nullptr;
        if (sentLayer) {
            // 不設 XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT。清除色是不透明的，交給 runtime 直接覆蓋。
            quad.layerFlags = 0;
            quad.space = localSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = swapchain;
            quad.subImage.imageRect.offset = {0, 0};
            quad.subImage.imageRect.extent.width = static_cast<int32_t>(swapWidth);
            quad.subImage.imageRect.extent.height = static_cast<int32_t>(swapHeight);
            quad.subImage.imageArrayIndex = 0;
            quad.pose = screenPose;
            quad.size.width = quadWidthM;
            quad.size.height = quadHeightM;
            header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        }
        XrFrameEndInfo end{};
        end.type = XR_TYPE_FRAME_END_INFO;
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = header != nullptr ? 1 : 0;
        end.layers = header != nullptr ? &header : nullptr;
        const XrResult endResult = xrEndFrame(session, &end);
        if (!xrOk(endResult)) {
            logXr(true, instance, "xrEndFrame", endResult);
            if (endResult == XR_ERROR_SESSION_LOST) {
                return kQxrEndLossPending;
            }
            if (endResult == XR_ERROR_INSTANCE_LOST) {
                return kQxrEndInstanceLoss;
            }
        } else {
            xrNote(endResult, &lossPending, nullptr);
        }
        if (fatal) {
            return fatalReason;
        }
        if (lossPending) {
            return kQxrEndLossPending;
        }
    }
}

QxrSession::QxrSession() : impl(std::make_shared<Impl>()) {}

QxrSession::~QxrSession() {
    destroy();
}

bool QxrSession::create(const QxrSessionCreateInfo& info, std::string* error) {
    if (!impl) {
        impl = std::make_shared<Impl>();
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->threadState == 1) {
            return fail(error, "create while the frame thread is running");
        }
    }
    release();
    if (!impl->build(info, error)) {
        release();
        return false;
    }
    return true;
}

ANativeWindow* QxrSession::decoderWindow() {
    if (!impl) {
        return nullptr;
    }
    return impl->video.decoderWindow();
}

bool QxrSession::start(std::string* error) {
    if (!impl || !impl->created) {
        return fail(error, "start before create");
    }
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        if (impl->threadState == 1) {
            return fail(error, "frame thread already running");
        }
        impl->stopFlag.store(false, std::memory_order_release);
        impl->threadState = 1;
    }
    try {
        std::thread([s = impl]() { run(s); }).detach();
    } catch (const std::exception& ex) {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->threadState = 0;
        return fail(error, ex.what());
    }
    if (error) {
        error->clear();
    }
    return true;
}

void QxrSession::stop() {
    auto s = impl;
    if (!s) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s->mu);
        s->stopFlag.store(true, std::memory_order_release);
    }
    s->cv.notify_all();
    if (s->workerSet.load(std::memory_order_acquire) && pthread_equal(pthread_self(), s->worker)) {
        return;
    }
    std::unique_lock<std::mutex> lock(s->mu);
    s->cv.wait(lock, [&] { return s->threadState != 1; });
}

void QxrSession::destroy() {
    stop();
    release();
}

void QxrSession::release() {
    auto s = impl;
    if (!s) {
        return;
    }
    if (s->inRelease.exchange(true)) {
        return;
    }
    s->teardown();
    s->inRelease.store(false);
}

std::vector<float> QxrSession::refreshRates() const {
    if (!impl) {
        return {};
    }
    return impl->rates;
}

void QxrSession::recommendedEyeSize(uint32_t* w, uint32_t* h) const {
    if (w) {
        *w = impl ? impl->eyeW : 0;
    }
    if (h) {
        *h = impl ? impl->eyeH : 0;
    }
}

void QxrSession::setEndedCallback(std::function<void(int)> callback) {
    if (!impl) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl->mu);
    impl->onEnded = std::move(callback);
}

void QxrSession::run(const std::shared_ptr<Impl>& impl) {
    impl->worker = pthread_self();
    impl->workerSet.store(true, std::memory_order_release);
    pthread_setname_np(pthread_self(), "qxr-frame");
    int reason = 0;
    try {
        reason = impl->frameLoop();
    } catch (const std::exception& ex) {
        QXR_LOGE("FRAME", "frame thread exception: %s", ex.what());
        reason = kQxrEndLossPending;
    }
    std::function<void(int)> callback;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        callback = impl->onEnded;
    }
    if (reason != 0 && !impl->endedNotified) {
        impl->endedNotified = true;
        if (callback) {
            callback(reason);
        }
    }
    callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl->mu);
        impl->workerSet.store(false, std::memory_order_release);
        impl->threadState = 2;
    }
    impl->cv.notify_all();
}
