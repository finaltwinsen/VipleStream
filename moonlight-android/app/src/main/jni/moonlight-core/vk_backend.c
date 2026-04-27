// vk_backend.c — VipleStream §I.B.2a: real Vulkan resource lifecycle for
// the FRUC backend. Differs from vk_probe.c in that this owns long-lived
// VkInstance + VkDevice + VkQueue and exposes init/destroy via JNI so the
// Java-side VkBackend can carry the handle across stream sessions.
//
// B.2a contract: build the Vulkan instance, pick a physical device with a
// graphics queue, create a logical device with the two extensions Phase A2
// confirmed are present (VK_KHR_swapchain,
// VK_ANDROID_external_memory_android_hardware_buffer), grab one graphics
// queue, log everything, and stop. No swapchain (B.2b), no AHB import
// (B.2c). VkBackend.initialize() still returns null at the Java layer to
// keep the GLES fallback engaged until those steps land.
//
// We dlopen libvulkan.so instead of -lvulkan'ing for two reasons: (1) the
// app's minSdk is still 21 while libvulkan is only guaranteed on API 24+,
// (2) it lets a probe failure degrade cleanly to the GLES backend without
// the linker bailing on app startup.

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "VKBE-NAT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------- Vulkan ABI subset (we don't pull <vulkan/vulkan.h>) ----------

typedef enum { VK_SUCCESS = 0 } VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef uint32_t VkStructureType;
typedef uint32_t VkFlags;
typedef VkFlags VkQueueFlags;

#define VK_STRUCTURE_TYPE_APPLICATION_INFO        0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO    1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO 2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO      3

#define VK_API_VERSION_1_1 ((1u << 22) | (1u << 12))
#define VK_QUEUE_GRAPHICS_BIT 0x00000001

#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    const char*     pApplicationName;
    uint32_t        applicationVersion;
    const char*     pEngineName;
    uint32_t        engineVersion;
    uint32_t        apiVersion;
} VkApplicationInfo;

typedef struct {
    VkStructureType    sType;
    const void*        pNext;
    uint32_t           flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t           enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t           enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct { uint32_t width, height, depth; } VkExtent3D;

typedef struct {
    VkQueueFlags queueFlags;
    uint32_t     queueCount;
    uint32_t     timestampValidBits;
    VkExtent3D   minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        flags;
    uint32_t        queueFamilyIndex;
    uint32_t        queueCount;
    const float*    pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct {
    VkStructureType sType;
    const void*     pNext;
    uint32_t        flags;
    uint32_t        queueCreateInfoCount;
    const VkDeviceQueueCreateInfo* pQueueCreateInfos;
    uint32_t        enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t        enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
    const void*     pEnabledFeatures;  // VkPhysicalDeviceFeatures* — passing NULL
} VkDeviceCreateInfo;

typedef struct {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char     deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t  pipelineCacheUUID[16];
    uint8_t  _tail[1024];   // limits + sparse — we don't read past name
} VkPhysicalDeviceProperties;

typedef void* (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef void* (*PFN_vkGetDeviceProcAddr)  (VkDevice,   const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef void     (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef void     (*PFN_vkGetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t*, VkQueueFamilyProperties*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef void     (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void     (*PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue*);

// ---------- backend handle ----------

typedef struct vk_backend_s {
    void* libvulkan;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

    VkInstance instance;
    PFN_vkDestroyInstance vkDestroyInstance;

    VkPhysicalDevice physDevice;

    VkDevice device;
    PFN_vkDestroyDevice vkDestroyDevice;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;

    VkQueue graphicsQueue;
    uint32_t graphicsQueueFamily;
} vk_backend_t;

#define LOAD_INSTANCE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetInstanceProcAddr((be)->instance, #name))

// ---------- JNI: nativeInit ----------

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VkBackend_nativeInit(JNIEnv* env, jclass clazz)
{
    vk_backend_t* be = (vk_backend_t*)calloc(1, sizeof(*be));
    if (!be) { LOGE("calloc failed"); return 0; }
    be->graphicsQueueFamily = (uint32_t)-1;

    be->libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!be->libvulkan) {
        LOGE("dlopen(libvulkan.so) failed: %s", dlerror());
        goto fail;
    }

    be->vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
        dlsym(be->libvulkan, "vkGetInstanceProcAddr");
    if (!be->vkGetInstanceProcAddr) {
        LOGE("dlsym(vkGetInstanceProcAddr) failed");
        goto fail;
    }

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)be->vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { LOGE("getProc(vkCreateInstance) NULL"); goto fail; }

    VkApplicationInfo appInfo = {0};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VipleStreamFrucBackend";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName      = "VipleStream";
    appInfo.engineVersion    = 1;
    appInfo.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;

    if (vkCreateInstance(&ici, NULL, &be->instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed");
        goto fail;
    }
    LOGI("VkInstance created");

    be->vkDestroyInstance = LOAD_INSTANCE_PROC(be, vkDestroyInstance);
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        LOAD_INSTANCE_PROC(be, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkCreateDevice vkCreateDevice = LOAD_INSTANCE_PROC(be, vkCreateDevice);
    if (!be->vkDestroyInstance || !vkEnumeratePhysicalDevices ||
        !vkGetPhysicalDeviceProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties || !vkCreateDevice) {
        LOGE("instance proc loading incomplete");
        goto fail;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, NULL);
    if (deviceCount == 0) { LOGE("no physical devices"); goto fail; }
    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(deviceCount, sizeof(*devices));
    if (!devices) goto fail;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, devices);
    be->physDevice = devices[0];   // single GPU on Pixel-class; pick first
    free(devices);

    {
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(be->physDevice, &props);
        LOGI("VkPhysicalDevice picked: '%s' apiVersion=%u.%u.%u vendor=0x%x",
             props.deviceName,
             (props.apiVersion >> 22) & 0x7F,
             (props.apiVersion >> 12) & 0x3FF,
             props.apiVersion & 0xFFF, props.vendorID);
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, NULL);
    if (qfCount == 0) { LOGE("zero queue families"); goto fail; }
    VkQueueFamilyProperties* qf =
        (VkQueueFamilyProperties*)calloc(qfCount, sizeof(*qf));
    if (!qf) goto fail;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, qf);

    for (uint32_t i = 0; i < qfCount; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            be->graphicsQueueFamily = i;
            break;
        }
    }
    free(qf);
    if (be->graphicsQueueFamily == (uint32_t)-1) {
        LOGE("no graphics-capable queue family");
        goto fail;
    }
    LOGI("graphics queue family = %u", be->graphicsQueueFamily);

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = be->graphicsQueueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &qprio;

    // Phase A2 confirmed both extensions are exposed on Adreno 620 / LineageOS 22.1.
    // We enable swapchain now; AHB import header gets enabled at B.2c when we
    // actually import. (Enabling unused extensions is fine but noisy in logs.)
    const char* deviceExts[] = {
        "VK_KHR_swapchain",
    };

    VkDeviceCreateInfo dci = {0};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = (uint32_t)(sizeof(deviceExts) / sizeof(deviceExts[0]));
    dci.ppEnabledExtensionNames = deviceExts;

    if (vkCreateDevice(be->physDevice, &dci, NULL, &be->device) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        goto fail;
    }
    LOGI("VkDevice created");

    be->vkDestroyDevice = LOAD_INSTANCE_PROC(be, vkDestroyDevice);
    be->vkGetDeviceProcAddr = LOAD_INSTANCE_PROC(be, vkGetDeviceProcAddr);
    PFN_vkGetDeviceQueue vkGetDeviceQueue = LOAD_INSTANCE_PROC(be, vkGetDeviceQueue);
    if (!be->vkDestroyDevice || !be->vkGetDeviceProcAddr || !vkGetDeviceQueue) {
        LOGE("device-level proc loading incomplete");
        goto fail;
    }

    vkGetDeviceQueue(be->device, be->graphicsQueueFamily, 0, &be->graphicsQueue);
    LOGI("VkQueue acquired (graphics, family=%u)", be->graphicsQueueFamily);

    LOGI("B.2a init complete: instance + device + graphics queue ready");
    return (jlong)(uintptr_t)be;

fail:
    if (be) {
        if (be->device && be->vkDestroyDevice) be->vkDestroyDevice(be->device, NULL);
        if (be->instance && be->vkDestroyInstance) be->vkDestroyInstance(be->instance, NULL);
        if (be->libvulkan) dlclose(be->libvulkan);
        free(be);
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VkBackend_nativeDestroy(JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return;
    LOGI("destroying backend handle=%p", be);
    if (be->device && be->vkDestroyDevice) {
        be->vkDestroyDevice(be->device, NULL);
        be->device = NULL;
    }
    if (be->instance && be->vkDestroyInstance) {
        be->vkDestroyInstance(be->instance, NULL);
        be->instance = NULL;
    }
    if (be->libvulkan) {
        dlclose(be->libvulkan);
        be->libvulkan = NULL;
    }
    free(be);
}
