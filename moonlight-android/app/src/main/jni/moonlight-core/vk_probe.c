// vk_probe.c — Phase A recon for VipleStream Android FRUC Vulkan path.
//
// Loads libvulkan.so via dlopen (no link-time dependency, safe on devices
// without Vulkan), enumerates physical devices + their extensions, and logs
// to logcat under the [VIPLE-VK-PROBE] tag. Specifically reports support for
// the two extensions Phase A needs to confirm before committing to the
// full Vulkan migration:
//
//   - VK_GOOGLE_display_timing
//       Optional. Adreno drivers historically have not shipped this — if
//       it's missing on Pixel 5 / Adreno 620, the entire Vulkan path's
//       latency story collapses (no precise frame scheduling), and we
//       go back to evaluating EGL_ANDROID_presentation_time only.
//
//   - VK_ANDROID_external_memory_android_hardware_buffer
//       Required for zero-copy MediaCodec → VkImage. Mandatory on Vulkan
//       1.1 / Android 10+ but worth confirming on this specific build.
//
// Throwaway-ish recon code: deletes its instance and returns immediately.

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "VIPLE-VK-PROBE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Minimal Vulkan ABI subset — we don't pull in <vulkan/vulkan.h> so that the
// build works without the Vulkan SDK in NDK include path.
typedef enum { VK_SUCCESS = 0 } VkResult;
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef uint32_t VkStructureType;
#define VK_STRUCTURE_TYPE_APPLICATION_INFO     0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_API_VERSION_1_1 ((1u << 22) | (1u << 12))
#define VK_MAX_EXTENSION_NAME_SIZE 256
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

typedef struct {
    char     extensionName[VK_MAX_EXTENSION_NAME_SIZE];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct {
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char     deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t  pipelineCacheUUID[16];
    // limits + sparse... we don't read past here, so don't bother declaring
    uint8_t  _tail[1024];
} VkPhysicalDeviceProperties;

typedef void* (*PFN_vkGetInstanceProcAddr)(VkInstance, const char*);
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef void     (*PFN_vkDestroyInstance)(VkInstance, const void*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void     (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
typedef VkResult (*PFN_vkEnumerateDeviceExtensionProperties)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*);

JNIEXPORT jboolean JNICALL
Java_com_limelight_binding_video_VkProbe_runProbeNative(JNIEnv* env, jclass clazz)
{
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        LOGE("dlopen(libvulkan.so) failed: %s", dlerror());
        return JNI_FALSE;
    }

    PFN_vkGetInstanceProcAddr getProc = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!getProc) {
        LOGE("dlsym(vkGetInstanceProcAddr) failed: %s", dlerror());
        dlclose(lib);
        return JNI_FALSE;
    }

    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)getProc(NULL, "vkCreateInstance");
    if (!vkCreateInstance) {
        LOGE("getProc(vkCreateInstance) returned NULL — Vulkan loader broken");
        dlclose(lib);
        return JNI_FALSE;
    }

    VkApplicationInfo appInfo = {0};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VipleStreamVkProbe";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName      = "VipleStream";
    appInfo.engineVersion    = 1;
    appInfo.apiVersion       = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;

    VkInstance instance = NULL;
    VkResult r = vkCreateInstance(&ici, NULL, &instance);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateInstance failed: VkResult=%d", (int)r);
        dlclose(lib);
        return JNI_FALSE;
    }

    PFN_vkDestroyInstance vkDestroyInstance =
        (PFN_vkDestroyInstance)getProc(instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)getProc(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)getProc(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)getProc(instance, "vkEnumerateDeviceExtensionProperties");

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
        !vkEnumerateDeviceExtensionProperties || !vkDestroyInstance) {
        LOGE("getProc returned NULL for one of the post-instance functions");
        if (vkDestroyInstance) vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return JNI_FALSE;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
    LOGI("physical device count = %u", deviceCount);
    if (deviceCount == 0) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return JNI_FALSE;
    }

    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(deviceCount, sizeof(*devices));
    if (!devices) {
        vkDestroyInstance(instance, NULL);
        dlclose(lib);
        return JNI_FALSE;
    }
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices);

    jboolean overallOk = JNI_TRUE;

    for (uint32_t i = 0; i < deviceCount; i++) {
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(devices[i], &props);
        LOGI("device[%u] name='%s' apiVersion=%u.%u.%u driverVersion=0x%x vendorID=0x%x",
             i, props.deviceName,
             (props.apiVersion >> 22) & 0x7F,
             (props.apiVersion >> 12) & 0x3FF,
             (props.apiVersion) & 0xFFF,
             props.driverVersion, props.vendorID);

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extCount, NULL);
        if (extCount == 0) {
            LOGW("device[%u] reports zero extensions — odd", i);
            continue;
        }
        VkExtensionProperties* exts =
            (VkExtensionProperties*)calloc(extCount, sizeof(*exts));
        if (!exts) continue;
        vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extCount, exts);

        int hasDisplayTiming = 0;
        int hasAhb           = 0;
        int hasExtSemFd      = 0;  // bonus: external semaphore fd needed for sync interop
        int hasYcbcrSampler  = 0;  // bonus: YUV sampler needed for AHB import path

        // Dump every extension at INFO so we have a full inventory in the log.
        for (uint32_t e = 0; e < extCount; e++) {
            LOGI("device[%u] ext[%u] %s spec=%u", i, e,
                 exts[e].extensionName, exts[e].specVersion);
            if (strcmp(exts[e].extensionName, "VK_GOOGLE_display_timing") == 0)
                hasDisplayTiming = 1;
            if (strcmp(exts[e].extensionName,
                       "VK_ANDROID_external_memory_android_hardware_buffer") == 0)
                hasAhb = 1;
            if (strcmp(exts[e].extensionName, "VK_KHR_external_semaphore_fd") == 0)
                hasExtSemFd = 1;
            if (strcmp(exts[e].extensionName,
                       "VK_KHR_sampler_ycbcr_conversion") == 0)
                hasYcbcrSampler = 1;
        }

        LOGI("=== Phase A2 verdict for device[%u] ===", i);
        LOGI("  VK_GOOGLE_display_timing                            : %s",
             hasDisplayTiming ? "YES (latency path viable)"
                              : "NO  (latency path needs EGL fallback)");
        LOGI("  VK_ANDROID_external_memory_android_hardware_buffer  : %s",
             hasAhb ? "YES (zero-copy MediaCodec import viable)"
                    : "NO  (would need staged copy via VkBuffer)");
        LOGI("  VK_KHR_external_semaphore_fd                        : %s",
             hasExtSemFd ? "YES" : "NO");
        LOGI("  VK_KHR_sampler_ycbcr_conversion                     : %s",
             hasYcbcrSampler ? "YES" : "NO");

        if (!hasDisplayTiming || !hasAhb) {
            overallOk = JNI_FALSE;
        }

        free(exts);
    }

    free(devices);
    vkDestroyInstance(instance, NULL);
    dlclose(lib);
    return overallOk;
}
