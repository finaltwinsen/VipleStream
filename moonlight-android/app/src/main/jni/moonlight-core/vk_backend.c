// vk_backend.c — VipleStream §I.B.2a + B.2b: real Vulkan resource lifecycle
// for the FRUC backend.
//
// B.2a (v1.2.139): instance + physical device + logical device + graphics queue.
// B.2b (this rev): instance enables VK_KHR_surface + VK_KHR_android_surface,
//                  Java Surface → ANativeWindow, VkSurfaceKHR creation,
//                  format/presentMode probing, VkSwapchainKHR creation,
//                  swapchain image enumeration, all torn down before
//                  initialize() returns null so GLES still owns the
//                  displaySurface for the actual streaming session.
//
// Why dlopen instead of -lvulkan: app's minSdk is 21 while libvulkan only
// landed at API 24. dlopen lets the rest of the app keep loading on older
// devices and degrade cleanly. Real headers from <vulkan/vulkan.h> +
// <vulkan/vulkan_android.h> are pulled in for type definitions only — we
// never link any vk* symbol directly.

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define TAG "VKBE-NAT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------- backend handle ----------

typedef struct vk_backend_s {
    void* libvulkan;

    // Loader entry points
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   vkGetDeviceProcAddr;

    // Instance + lifecycle
    VkInstance instance;
    PFN_vkDestroyInstance vkDestroyInstance;

    // Surface
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR;
    PFN_vkDestroySurfaceKHR       vkDestroySurfaceKHR;
    ANativeWindow* nativeWindow;
    VkSurfaceKHR   surface;

    // Physical / logical device
    VkPhysicalDevice physDevice;
    VkDevice         device;
    PFN_vkDestroyDevice vkDestroyDevice;

    // Queue
    VkQueue  graphicsQueue;
    uint32_t graphicsQueueFamily;

    // Swapchain
    PFN_vkCreateSwapchainKHR    vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR   vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    VkSwapchainKHR swapchain;
    VkFormat       swapchainFormat;
    VkExtent2D     swapchainExtent;
    uint32_t       swapchainImageCount;
    VkImage*       swapchainImages;
} vk_backend_t;

#define LOAD_INSTANCE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetInstanceProcAddr((be)->instance, #name))

// ---------- helpers ----------

static int create_instance(vk_backend_t* be)
{
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)be->vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { LOGE("getProc(vkCreateInstance) NULL"); return -1; }

    VkApplicationInfo appInfo = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VipleStreamFrucBackend",
        .applicationVersion = 1,
        .pEngineName      = "VipleStream",
        .engineVersion    = 1,
        .apiVersion       = VK_API_VERSION_1_1,
    };

    // VK_KHR_surface + VK_KHR_android_surface are mandatory on every
    // conformant Android Vulkan driver (per vendor compliance), so we
    // demand them rather than probe.
    const char* instExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &appInfo,
        .enabledExtensionCount   = (uint32_t)(sizeof(instExts) / sizeof(instExts[0])),
        .ppEnabledExtensionNames = instExts,
    };

    if (vkCreateInstance(&ici, NULL, &be->instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed (likely missing instance extensions)");
        return -1;
    }
    LOGI("VkInstance created (with VK_KHR_surface + VK_KHR_android_surface)");

    be->vkDestroyInstance         = LOAD_INSTANCE_PROC(be, vkDestroyInstance);
    be->vkCreateAndroidSurfaceKHR = LOAD_INSTANCE_PROC(be, vkCreateAndroidSurfaceKHR);
    be->vkDestroySurfaceKHR       = LOAD_INSTANCE_PROC(be, vkDestroySurfaceKHR);

    if (!be->vkDestroyInstance || !be->vkCreateAndroidSurfaceKHR ||
        !be->vkDestroySurfaceKHR) {
        LOGE("instance proc table incomplete");
        return -1;
    }
    return 0;
}

static int pick_physical_device_and_queue(vk_backend_t* be)
{
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        LOAD_INSTANCE_PROC(be, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceSupportKHR);

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceSurfaceSupportKHR) {
        LOGE("physical-device proc loading failed");
        return -1;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, NULL);
    if (deviceCount == 0) { LOGE("no physical devices"); return -1; }

    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(deviceCount, sizeof(*devices));
    if (!devices) return -1;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, devices);
    be->physDevice = devices[0];   // Pixel-class is single-GPU
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(be->physDevice, &props);
    LOGI("VkPhysicalDevice picked: '%s' apiVersion=%u.%u.%u vendor=0x%x",
         props.deviceName,
         (props.apiVersion >> 22) & 0x7F,
         (props.apiVersion >> 12) & 0x3FF,
         props.apiVersion & 0xFFF, props.vendorID);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, NULL);
    if (qfCount == 0) { LOGE("zero queue families"); return -1; }
    VkQueueFamilyProperties* qf =
        (VkQueueFamilyProperties*)calloc(qfCount, sizeof(*qf));
    if (!qf) return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, qf);

    // Pick a queue family with both graphics and surface present support.
    // VkSurfaceKHR must already exist; this is called after surface creation.
    be->graphicsQueueFamily = (uint32_t)-1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(be->physDevice, i, be->surface, &supported);
        if (supported) {
            be->graphicsQueueFamily = i;
            break;
        }
    }
    free(qf);
    if (be->graphicsQueueFamily == (uint32_t)-1) {
        LOGE("no queue family with graphics + surface present support");
        return -1;
    }
    LOGI("graphics+present queue family = %u", be->graphicsQueueFamily);
    return 0;
}

static int create_device(vk_backend_t* be)
{
    PFN_vkCreateDevice vkCreateDevice = LOAD_INSTANCE_PROC(be, vkCreateDevice);
    if (!vkCreateDevice) { LOGE("getProc(vkCreateDevice) NULL"); return -1; }

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = be->graphicsQueueFamily,
        .queueCount       = 1,
        .pQueuePriorities = &qprio,
    };

    const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = (uint32_t)(sizeof(deviceExts) / sizeof(deviceExts[0])),
        .ppEnabledExtensionNames = deviceExts,
    };

    if (vkCreateDevice(be->physDevice, &dci, NULL, &be->device) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        return -1;
    }
    LOGI("VkDevice created");

    be->vkDestroyDevice     = LOAD_INSTANCE_PROC(be, vkDestroyDevice);
    be->vkGetDeviceProcAddr = LOAD_INSTANCE_PROC(be, vkGetDeviceProcAddr);
    PFN_vkGetDeviceQueue vkGetDeviceQueue = LOAD_INSTANCE_PROC(be, vkGetDeviceQueue);
    if (!be->vkDestroyDevice || !be->vkGetDeviceProcAddr || !vkGetDeviceQueue) {
        LOGE("device-level proc loading failed");
        return -1;
    }

    vkGetDeviceQueue(be->device, be->graphicsQueueFamily, 0, &be->graphicsQueue);
    LOGI("VkQueue acquired (graphics, family=%u)", be->graphicsQueueFamily);

    // Swapchain function pointers (device-level, but loadable via instance proc)
    be->vkCreateSwapchainKHR    = LOAD_INSTANCE_PROC(be, vkCreateSwapchainKHR);
    be->vkDestroySwapchainKHR   = LOAD_INSTANCE_PROC(be, vkDestroySwapchainKHR);
    be->vkGetSwapchainImagesKHR = LOAD_INSTANCE_PROC(be, vkGetSwapchainImagesKHR);
    if (!be->vkCreateSwapchainKHR || !be->vkDestroySwapchainKHR ||
        !be->vkGetSwapchainImagesKHR) {
        LOGE("swapchain proc loading failed");
        return -1;
    }
    return 0;
}

static int create_surface(vk_backend_t* be, JNIEnv* env, jobject jSurface)
{
    if (!jSurface) { LOGE("Java Surface is null"); return -1; }
    be->nativeWindow = ANativeWindow_fromSurface(env, jSurface);
    if (!be->nativeWindow) {
        LOGE("ANativeWindow_fromSurface returned NULL");
        return -1;
    }

    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = be->nativeWindow,
    };
    if (be->vkCreateAndroidSurfaceKHR(be->instance, &sci, NULL, &be->surface)
        != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed");
        return -1;
    }
    LOGI("VkSurfaceKHR created on ANativeWindow=%p", be->nativeWindow);
    return 0;
}

static int create_swapchain(vk_backend_t* be)
{
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceFormatsKHR);
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfacePresentModesKHR);

    if (!vkGetPhysicalDeviceSurfaceCapabilitiesKHR ||
        !vkGetPhysicalDeviceSurfaceFormatsKHR ||
        !vkGetPhysicalDeviceSurfacePresentModesKHR) {
        LOGE("surface query proc loading failed");
        return -1;
    }

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(be->physDevice, be->surface, &caps)
        != VK_SUCCESS) {
        LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return -1;
    }
    LOGI("surface caps: minImage=%u maxImage=%u currentExtent=%ux%u "
         "supportedTransforms=0x%x supportedCompositeAlpha=0x%x",
         caps.minImageCount, caps.maxImageCount,
         caps.currentExtent.width, caps.currentExtent.height,
         caps.supportedTransforms, caps.supportedCompositeAlpha);

    // Pick format. Android-side surface usually advertises BGRA8 UNORM as
    // first option; we accept either RGBA8 or BGRA8 in UNORM/sRGB form.
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(be->physDevice, be->surface, &fmtCount, NULL);
    if (fmtCount == 0) { LOGE("no surface formats"); return -1; }
    VkSurfaceFormatKHR* fmts =
        (VkSurfaceFormatKHR*)calloc(fmtCount, sizeof(*fmts));
    if (!fmts) return -1;
    vkGetPhysicalDeviceSurfaceFormatsKHR(be->physDevice, be->surface, &fmtCount, fmts);

    VkSurfaceFormatKHR chosen = fmts[0];   // first is always valid per spec
    for (uint32_t i = 0; i < fmtCount; i++) {
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = fmts[i];
            break;
        }
    }
    LOGI("picked surface format=%d colorSpace=%d (out of %u)",
         chosen.format, chosen.colorSpace, fmtCount);
    free(fmts);

    // Pick FIFO (always available, V-sync gated). Phase D will swap to
    // MAILBOX or IMMEDIATE if measurements warrant.
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    {
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(be->physDevice, be->surface, &pmCount, NULL);
        if (pmCount > 0) {
            VkPresentModeKHR* pms = (VkPresentModeKHR*)calloc(pmCount, sizeof(*pms));
            if (pms) {
                vkGetPhysicalDeviceSurfacePresentModesKHR(be->physDevice, be->surface,
                                                          &pmCount, pms);
                LOGI("present modes available (%u):", pmCount);
                for (uint32_t i = 0; i < pmCount; i++) LOGI("  [%u] = %d", i, pms[i]);
                free(pms);
            }
        }
    }
    LOGI("picked present mode = FIFO (%d)", present);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        // Special value — surface decides at swapchain creation. Use a
        // sane default; swapchain images get their own extent reported on
        // creation anyway.
        extent.width  = 1920;
        extent.height = 1080;
    }

    VkSwapchainCreateInfoKHR sci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = be->surface,
        .minImageCount    = imageCount,
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = present,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE,
    };

    if (be->vkCreateSwapchainKHR(be->device, &sci, NULL, &be->swapchain)
        != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed");
        return -1;
    }
    be->swapchainFormat = chosen.format;
    be->swapchainExtent = extent;
    LOGI("VkSwapchainKHR created (extent=%ux%u, requested=%u images)",
         extent.width, extent.height, imageCount);

    be->vkGetSwapchainImagesKHR(be->device, be->swapchain,
                                &be->swapchainImageCount, NULL);
    if (be->swapchainImageCount == 0) {
        LOGE("swapchain reported 0 images");
        return -1;
    }
    be->swapchainImages = (VkImage*)calloc(be->swapchainImageCount, sizeof(VkImage));
    if (!be->swapchainImages) return -1;
    be->vkGetSwapchainImagesKHR(be->device, be->swapchain,
                                &be->swapchainImageCount, be->swapchainImages);
    LOGI("swapchain image count actual = %u", be->swapchainImageCount);

    return 0;
}

// ---------- JNI: nativeInit / nativeDestroy ----------

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VkBackend_nativeInit(JNIEnv* env, jclass clazz, jobject jSurface)
{
    vk_backend_t* be = (vk_backend_t*)calloc(1, sizeof(*be));
    if (!be) { LOGE("calloc failed"); return 0; }
    be->graphicsQueueFamily = (uint32_t)-1;

    be->libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!be->libvulkan) { LOGE("dlopen libvulkan.so failed: %s", dlerror()); goto fail; }
    be->vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(be->libvulkan, "vkGetInstanceProcAddr");
    if (!be->vkGetInstanceProcAddr) { LOGE("dlsym vkGetInstanceProcAddr failed"); goto fail; }

    if (create_instance(be) != 0) goto fail;
    if (create_surface(be, env, jSurface) != 0) goto fail;
    if (pick_physical_device_and_queue(be) != 0) goto fail;
    if (create_device(be) != 0) goto fail;
    if (create_swapchain(be) != 0) goto fail;

    LOGI("B.2b init complete: instance + surface + device + queue + swapchain ready");
    return (jlong)(uintptr_t)be;

fail:
    if (be) {
        if (be->swapchainImages) free(be->swapchainImages);
        if (be->swapchain && be->vkDestroySwapchainKHR)
            be->vkDestroySwapchainKHR(be->device, be->swapchain, NULL);
        if (be->surface && be->vkDestroySurfaceKHR)
            be->vkDestroySurfaceKHR(be->instance, be->surface, NULL);
        if (be->nativeWindow) ANativeWindow_release(be->nativeWindow);
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
    if (be->swapchainImages) {
        free(be->swapchainImages);
        be->swapchainImages = NULL;
    }
    if (be->swapchain && be->vkDestroySwapchainKHR) {
        be->vkDestroySwapchainKHR(be->device, be->swapchain, NULL);
        be->swapchain = VK_NULL_HANDLE;
    }
    if (be->surface && be->vkDestroySurfaceKHR) {
        be->vkDestroySurfaceKHR(be->instance, be->surface, NULL);
        be->surface = VK_NULL_HANDLE;
    }
    if (be->nativeWindow) {
        ANativeWindow_release(be->nativeWindow);
        be->nativeWindow = NULL;
    }
    if (be->device && be->vkDestroyDevice) {
        be->vkDestroyDevice(be->device, NULL);
        be->device = VK_NULL_HANDLE;
    }
    if (be->instance && be->vkDestroyInstance) {
        be->vkDestroyInstance(be->instance, NULL);
        be->instance = VK_NULL_HANDLE;
    }
    if (be->libvulkan) {
        dlclose(be->libvulkan);
        be->libvulkan = NULL;
    }
    free(be);
}
