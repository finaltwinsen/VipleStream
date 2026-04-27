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
// AHB JNI helpers are API 26+. minSdk is 21, so the headers reject the
// calls unless the build sets __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
// (in Android.mk LOCAL_CFLAGS). At runtime VkBackend gates this whole
// path on debug.viplestream.vkprobe + Build.VERSION.SDK_INT >= 28, so
// we can never reach a too-old device.
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Pre-compiled SPIR-V shaders. Source under shaders/, regenerate via:
//   $NDK/shader-tools/.../glslc <name> -o <name>.spv
//   xxd -i <name>.spv | sed 's|^unsigned |static const unsigned |' > <name>.spv.h
#include "shaders/fullscreen.vert.spv.h"
#include "shaders/test_pattern.frag.spv.h"
#include "shaders/video_sample.frag.spv.h"

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

    // AHB import path (B.2c.3b)
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID;
    PFN_vkCreateImage                vkCreateImage;
    PFN_vkDestroyImage               vkDestroyImage;
    PFN_vkAllocateMemory             vkAllocateMemory;
    PFN_vkFreeMemory                 vkFreeMemory;
    PFN_vkBindImageMemory            vkBindImageMemory;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    int ahbImportLogged;

    // YCbCr sampler path (B.2c.3c.1) — lazily created on first AHB frame
    // because it depends on the externalFormat we read off the buffer.
    PFN_vkCreateSamplerYcbcrConversion  vkCreateSamplerYcbcrConversion;
    PFN_vkDestroySamplerYcbcrConversion vkDestroySamplerYcbcrConversion;
    PFN_vkCreateSampler                 vkCreateSampler;
    PFN_vkDestroySampler                vkDestroySampler;
    VkSamplerYcbcrConversion ycbcrConversion;
    VkSampler                ycbcrSampler;
    uint64_t                 ycbcrExternalFormat;   // externalFormat the sampler is bound to
    int                      ycbcrInitialized;

    // Graphics pipeline (B.2c.3c.2) — full-screen triangle, no vertex
    // buffer, test_pattern.frag draws a UV gradient. B.2c.3c.3 swaps
    // the frag for one that samples the imported AHB image and uses
    // a non-empty descriptor set with the YCbCr sampler binding.
    PFN_vkCreateShaderModule        vkCreateShaderModule;
    PFN_vkDestroyShaderModule       vkDestroyShaderModule;
    PFN_vkCreateRenderPass          vkCreateRenderPass;
    PFN_vkDestroyRenderPass         vkDestroyRenderPass;
    PFN_vkCreateImageView           vkCreateImageView;
    PFN_vkDestroyImageView          vkDestroyImageView;
    PFN_vkCreateFramebuffer         vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer        vkDestroyFramebuffer;
    PFN_vkCreatePipelineLayout      vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout     vkDestroyPipelineLayout;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreateGraphicsPipelines   vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline           vkDestroyPipeline;
    PFN_vkCmdBeginRenderPass        vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass          vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline           vkCmdBindPipeline;
    PFN_vkCmdDraw                   vkCmdDraw;
    VkRenderPass         renderPass;
    VkPipelineLayout     pipelineLayout;
    VkDescriptorSetLayout descLayout;
    VkPipeline           graphicsPipeline;
    VkShaderModule       vertShader;
    VkShaderModule       fragShader;
    VkImageView*         swapchainViews;     // size = swapchainImageCount
    VkFramebuffer*       framebuffers;       // size = swapchainImageCount
    int                  graphicsInitialized;

    // Descriptor pool / set for the video sampler (B.2c.3c.3). The
    // descriptor set's image binding is updated per frame to point at
    // whatever VkImageView we just imported from MediaCodec's AHB.
    PFN_vkCreateDescriptorPool      vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool     vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets    vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets      vkUpdateDescriptorSets;
    PFN_vkCmdBindDescriptorSets     vkCmdBindDescriptorSets;
    VkDescriptorPool descPool;
    VkDescriptorSet  descSet;

    // Per-frame render path (B.2c.3a)
    PFN_vkAcquireNextImageKHR    vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR        vkQueuePresentKHR;
    PFN_vkQueueSubmit            vkQueueSubmit;
    PFN_vkQueueWaitIdle          vkQueueWaitIdle;
    PFN_vkBeginCommandBuffer     vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer       vkEndCommandBuffer;
    PFN_vkResetCommandBuffer     vkResetCommandBuffer;
    PFN_vkCmdClearColorImage     vkCmdClearColorImage;
    PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier;
    PFN_vkCreateSemaphore        vkCreateSemaphore;
    PFN_vkDestroySemaphore       vkDestroySemaphore;
    PFN_vkCreateCommandPool      vkCreateCommandPool;
    PFN_vkDestroyCommandPool     vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    VkCommandPool   cmdPool;
    VkCommandBuffer cmdBuffer;
    VkSemaphore     acquireSem;
    VkSemaphore     renderDoneSem;
    int             renderInitialized;
    int             frameCounter;
} vk_backend_t;

#define LOAD_INSTANCE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetInstanceProcAddr((be)->instance, #name))

// Forward declarations — definitions live further down so each phase reads
// top-to-bottom, but nativeInit calls them before they appear in source.
static int  init_graphics_pipeline(struct vk_backend_s* be);
static void destroy_graphics_pipeline(struct vk_backend_s* be);
static int  load_graphics_procs(struct vk_backend_s* be);

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

    // VK_ANDROID_external_memory_android_hardware_buffer for B.2c.3b AHB import.
    // Phase A2 confirmed Adreno 620 advertises both extensions on Pixel 5.
    const char* deviceExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    };

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

// ---------- B.2c.3a: persistent per-frame render resources ----------

static int load_ahb_procs(vk_backend_t* be)
{
    be->vkGetAndroidHardwareBufferPropertiesANDROID =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)be->vkGetDeviceProcAddr(
            be->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    be->vkCreateImage     = (PFN_vkCreateImage)be->vkGetDeviceProcAddr(be->device, "vkCreateImage");
    be->vkDestroyImage    = (PFN_vkDestroyImage)be->vkGetDeviceProcAddr(be->device, "vkDestroyImage");
    be->vkAllocateMemory  = (PFN_vkAllocateMemory)be->vkGetDeviceProcAddr(be->device, "vkAllocateMemory");
    be->vkFreeMemory      = (PFN_vkFreeMemory)be->vkGetDeviceProcAddr(be->device, "vkFreeMemory");
    be->vkBindImageMemory = (PFN_vkBindImageMemory)be->vkGetDeviceProcAddr(be->device, "vkBindImageMemory");
    be->vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)be->vkGetInstanceProcAddr(
            be->instance, "vkGetPhysicalDeviceMemoryProperties");
    // YCbCr sampler procs (Vulkan 1.1 core)
    be->vkCreateSamplerYcbcrConversion =
        (PFN_vkCreateSamplerYcbcrConversion)be->vkGetDeviceProcAddr(be->device, "vkCreateSamplerYcbcrConversion");
    be->vkDestroySamplerYcbcrConversion =
        (PFN_vkDestroySamplerYcbcrConversion)be->vkGetDeviceProcAddr(be->device, "vkDestroySamplerYcbcrConversion");
    be->vkCreateSampler  = (PFN_vkCreateSampler)be->vkGetDeviceProcAddr(be->device, "vkCreateSampler");
    be->vkDestroySampler = (PFN_vkDestroySampler)be->vkGetDeviceProcAddr(be->device, "vkDestroySampler");

    if (!be->vkGetAndroidHardwareBufferPropertiesANDROID || !be->vkCreateImage ||
        !be->vkDestroyImage || !be->vkAllocateMemory || !be->vkFreeMemory ||
        !be->vkBindImageMemory || !be->vkGetPhysicalDeviceMemoryProperties ||
        !be->vkCreateSamplerYcbcrConversion || !be->vkDestroySamplerYcbcrConversion ||
        !be->vkCreateSampler || !be->vkDestroySampler) {
        LOGE("load_ahb_procs: missing entry points");
        return -1;
    }
    return 0;
}

// Lazy create the YCbCr conversion + immutable sampler the first time we
// see an AHB. The conversion baked the externalFormat / ycbcrModel / range
// from MediaCodec's actual output — they're stable for a session, so we
// create once and reuse for the lifetime of the backend.
static int ensure_ycbcr_sampler(vk_backend_t* be,
                                const VkAndroidHardwareBufferFormatPropertiesANDROID* fmt)
{
    if (be->ycbcrInitialized) {
        if (be->ycbcrExternalFormat != fmt->externalFormat) {
            LOGW("AHB externalFormat changed (was 0x%llx, now 0x%llx) — sampler is now stale",
                 (unsigned long long)be->ycbcrExternalFormat,
                 (unsigned long long)fmt->externalFormat);
        }
        return 0;
    }
    if (!be->vkCreateSamplerYcbcrConversion || !be->vkCreateSampler) {
        LOGE("ensure_ycbcr_sampler: procs not loaded");
        return -1;
    }

    VkExternalFormatANDROID extFmt = {
        .sType          = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        .externalFormat = fmt->externalFormat,
    };
    VkSamplerYcbcrConversionCreateInfo cci = {
        .sType         = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .pNext         = &extFmt,
        .format        = VK_FORMAT_UNDEFINED,
        .ycbcrModel    = fmt->suggestedYcbcrModel,
        .ycbcrRange    = fmt->suggestedYcbcrRange,
        .components    = fmt->samplerYcbcrConversionComponents,
        .xChromaOffset = fmt->suggestedXChromaOffset,
        .yChromaOffset = fmt->suggestedYChromaOffset,
        .chromaFilter  = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
    };
    VkResult r = be->vkCreateSamplerYcbcrConversion(be->device, &cci, NULL, &be->ycbcrConversion);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSamplerYcbcrConversion failed: %d", r);
        return -1;
    }

    VkSamplerYcbcrConversionInfo convInfo = {
        .sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = be->ycbcrConversion,
    };
    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext        = &convInfo,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .compareEnable    = VK_FALSE,
        .borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    r = be->vkCreateSampler(be->device, &sci, NULL, &be->ycbcrSampler);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSampler (with ycbcr conversion) failed: %d", r);
        be->vkDestroySamplerYcbcrConversion(be->device, be->ycbcrConversion, NULL);
        be->ycbcrConversion = VK_NULL_HANDLE;
        return -1;
    }

    be->ycbcrExternalFormat = fmt->externalFormat;
    be->ycbcrInitialized = 1;
    LOGI("YCbCr sampler ready: externalFormat=0x%llx model=%d range=%d "
         "components(r=%d g=%d b=%d a=%d) chromaXOff=%d chromaYOff=%d",
         (unsigned long long)fmt->externalFormat,
         fmt->suggestedYcbcrModel, fmt->suggestedYcbcrRange,
         fmt->samplerYcbcrConversionComponents.r,
         fmt->samplerYcbcrConversionComponents.g,
         fmt->samplerYcbcrConversionComponents.b,
         fmt->samplerYcbcrConversionComponents.a,
         fmt->suggestedXChromaOffset, fmt->suggestedYChromaOffset);
    return 0;
}

static void destroy_ycbcr_sampler(vk_backend_t* be)
{
    if (be->ycbcrSampler && be->vkDestroySampler) {
        be->vkDestroySampler(be->device, be->ycbcrSampler, NULL);
        be->ycbcrSampler = VK_NULL_HANDLE;
    }
    if (be->ycbcrConversion && be->vkDestroySamplerYcbcrConversion) {
        be->vkDestroySamplerYcbcrConversion(be->device, be->ycbcrConversion, NULL);
        be->ycbcrConversion = VK_NULL_HANDLE;
    }
    be->ycbcrInitialized = 0;
}

static int pick_memory_type(vk_backend_t* be, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties memProps;
    be->vkGetPhysicalDeviceMemoryProperties(be->physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (bits & (1u << i)) return (int)i;
    }
    return -1;
}

// Import an AHardwareBuffer into Vulkan resources. Two modes:
//   keepAlive=0 → import + destroy in-function (validation only, B.2c.3b)
//   keepAlive=1 → return image / mem / view through outImage / outMem /
//                 outView; caller is responsible for freeing after the
//                 GPU is done reading them
static int do_import_ahb(vk_backend_t* be, AHardwareBuffer* ahb, int keepAlive,
                         VkImage* outImage, VkDeviceMemory* outMem, VkImageView* outView,
                         uint32_t* outWidth, uint32_t* outHeight)
{
    if (!be->vkGetAndroidHardwareBufferPropertiesANDROID) return -2;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(ahb, &desc);

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &fmtProps,
    };
    VkResult r = be->vkGetAndroidHardwareBufferPropertiesANDROID(be->device, ahb, &props);
    if (r != VK_SUCCESS) {
        LOGE("vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", r);
        return -1;
    }

    if (!be->ahbImportLogged) {
        LOGI("AHB desc: %ux%u layers=%u format=0x%x usage=0x%llx",
             desc.width, desc.height, desc.layers, desc.format,
             (unsigned long long)desc.usage);
        LOGI("AHB props: allocSize=%llu memTypeBits=0x%x VkFormat=%d "
             "externalFormat=0x%llx samplerYcbcrModel=%d range=%d",
             (unsigned long long)props.allocationSize, props.memoryTypeBits,
             fmtProps.format, (unsigned long long)fmtProps.externalFormat,
             fmtProps.suggestedYcbcrModel, fmtProps.suggestedYcbcrRange);
    }

    // B.2c.3c.1: lazy-create the YCbCr sampler from the first frame's
    // externalFormat. Subsequent frames reuse it. Sampler binds during
    // B.2c.3c.3 once the graphics pipeline lands.
    if (fmtProps.externalFormat != 0) {
        ensure_ycbcr_sampler(be, &fmtProps);
    }

    // VkImage with external chain. Format VK_FORMAT_UNDEFINED + non-zero
    // externalFormat is the YUV-from-AHB pattern (ycbcr_conversion territory);
    // RGBA-style AHB returns a real VkFormat in fmtProps.format.
    VkExternalFormatANDROID extFmt = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        .externalFormat = fmtProps.externalFormat,
    };
    VkExternalMemoryImageCreateInfo extImgInfo = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = (fmtProps.externalFormat != 0) ? &extFmt : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extImgInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = (fmtProps.externalFormat != 0) ? VK_FORMAT_UNDEFINED : fmtProps.format,
        .extent = { desc.width, desc.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage importedImage = VK_NULL_HANDLE;
    r = be->vkCreateImage(be->device, &ici, NULL, &importedImage);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateImage (AHB) failed: %d", r);
        return -1;
    }

    int memTypeIdx = pick_memory_type(be, props.memoryTypeBits);
    if (memTypeIdx < 0) {
        LOGE("no compatible memory type for AHB import");
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = ahb,
    };
    VkMemoryDedicatedAllocateInfo dedAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &importInfo,
        .image = importedImage,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedAlloc,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = (uint32_t)memTypeIdx,
    };

    VkDeviceMemory importedMem = VK_NULL_HANDLE;
    r = be->vkAllocateMemory(be->device, &mai, NULL, &importedMem);
    if (r != VK_SUCCESS) {
        LOGE("vkAllocateMemory (AHB) failed: %d", r);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    r = be->vkBindImageMemory(be->device, importedImage, importedMem, 0);
    if (r != VK_SUCCESS) {
        LOGE("vkBindImageMemory (AHB) failed: %d", r);
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    if (!be->ahbImportLogged) {
        LOGI("AHB import OK: VkImage %p + VkDeviceMemory %p bound (memTypeIdx=%d)",
             importedImage, importedMem, memTypeIdx);
        be->ahbImportLogged = 1;
    }

    if (!keepAlive) {
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return 0;
    }

    // Caller wants the image alive for sampling. Build a view with the
    // SamplerYcbcrConversion chain so the descriptor-set update knows the
    // implicit YUV→RGB conversion to apply.
    if (!be->ycbcrInitialized) {
        LOGE("do_import_ahb keepAlive: ycbcr sampler not ready");
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }
    VkSamplerYcbcrConversionInfo viewYcbcr = {
        .sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = be->ycbcrConversion,
    };
    VkImageViewCreateInfo ivci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = &viewYcbcr,
        .image    = importedImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_UNDEFINED,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkImageView importedView = VK_NULL_HANDLE;
    r = be->vkCreateImageView(be->device, &ivci, NULL, &importedView);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateImageView (AHB) failed: %d", r);
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    if (outImage)  *outImage  = importedImage;
    if (outMem)    *outMem    = importedMem;
    if (outView)   *outView   = importedView;
    if (outWidth)  *outWidth  = desc.width;
    if (outHeight) *outHeight = desc.height;
    return 0;
}

// Backward-compat shim used by Java's nativeImportAhb (B.2c.3b validation).
static int try_import_ahb(vk_backend_t* be, AHardwareBuffer* ahb)
{
    return do_import_ahb(be, ahb, /*keepAlive=*/0, NULL, NULL, NULL, NULL, NULL);
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeImportAhb(JNIEnv* env, jclass clazz,
                                                            jlong handle, jobject jHwBuffer)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be || !jHwBuffer) return -1;
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, jHwBuffer);
    if (!ahb) {
        LOGE("AHardwareBuffer_fromHardwareBuffer returned NULL");
        return -1;
    }
    return (jint)try_import_ahb(be, ahb);
}

static int load_render_procs(vk_backend_t* be)
{
    be->vkAcquireNextImageKHR    = (PFN_vkAcquireNextImageKHR)be->vkGetDeviceProcAddr(be->device, "vkAcquireNextImageKHR");
    be->vkQueuePresentKHR        = (PFN_vkQueuePresentKHR)be->vkGetDeviceProcAddr(be->device, "vkQueuePresentKHR");
    be->vkQueueSubmit            = (PFN_vkQueueSubmit)be->vkGetDeviceProcAddr(be->device, "vkQueueSubmit");
    be->vkQueueWaitIdle          = (PFN_vkQueueWaitIdle)be->vkGetDeviceProcAddr(be->device, "vkQueueWaitIdle");
    be->vkBeginCommandBuffer     = (PFN_vkBeginCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkBeginCommandBuffer");
    be->vkEndCommandBuffer       = (PFN_vkEndCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkEndCommandBuffer");
    be->vkResetCommandBuffer     = (PFN_vkResetCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkResetCommandBuffer");
    be->vkCmdClearColorImage     = (PFN_vkCmdClearColorImage)be->vkGetDeviceProcAddr(be->device, "vkCmdClearColorImage");
    be->vkCmdPipelineBarrier     = (PFN_vkCmdPipelineBarrier)be->vkGetDeviceProcAddr(be->device, "vkCmdPipelineBarrier");
    be->vkCreateSemaphore        = (PFN_vkCreateSemaphore)be->vkGetDeviceProcAddr(be->device, "vkCreateSemaphore");
    be->vkDestroySemaphore       = (PFN_vkDestroySemaphore)be->vkGetDeviceProcAddr(be->device, "vkDestroySemaphore");
    be->vkCreateCommandPool      = (PFN_vkCreateCommandPool)be->vkGetDeviceProcAddr(be->device, "vkCreateCommandPool");
    be->vkDestroyCommandPool     = (PFN_vkDestroyCommandPool)be->vkGetDeviceProcAddr(be->device, "vkDestroyCommandPool");
    be->vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)be->vkGetDeviceProcAddr(be->device, "vkAllocateCommandBuffers");
    if (!be->vkAcquireNextImageKHR || !be->vkQueuePresentKHR || !be->vkQueueSubmit ||
        !be->vkQueueWaitIdle || !be->vkBeginCommandBuffer || !be->vkEndCommandBuffer ||
        !be->vkResetCommandBuffer || !be->vkCmdClearColorImage || !be->vkCmdPipelineBarrier ||
        !be->vkCreateSemaphore || !be->vkDestroySemaphore ||
        !be->vkCreateCommandPool || !be->vkDestroyCommandPool || !be->vkAllocateCommandBuffers) {
        LOGE("load_render_procs: missing entry points");
        return -1;
    }
    return 0;
}

static int init_render_resources(vk_backend_t* be)
{
    if (load_render_procs(be) != 0) return -1;

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (be->vkCreateSemaphore(be->device, &sci, NULL, &be->acquireSem) != VK_SUCCESS ||
        be->vkCreateSemaphore(be->device, &sci, NULL, &be->renderDoneSem) != VK_SUCCESS) {
        LOGE("init_render_resources: semaphore create failed");
        return -1;
    }

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = be->graphicsQueueFamily,
    };
    if (be->vkCreateCommandPool(be->device, &pci, NULL, &be->cmdPool) != VK_SUCCESS) {
        LOGE("init_render_resources: cmdPool create failed");
        return -1;
    }

    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = be->cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (be->vkAllocateCommandBuffers(be->device, &cai, &be->cmdBuffer) != VK_SUCCESS) {
        LOGE("init_render_resources: cmdBuffer alloc failed");
        return -1;
    }

    be->renderInitialized = 1;
    LOGI("render resources ready (cmdPool + 1 cmdBuffer + 2 semaphores)");
    return 0;
}

static void destroy_render_resources(vk_backend_t* be)
{
    if (!be->renderInitialized) return;
    if (be->vkQueueWaitIdle && be->graphicsQueue) be->vkQueueWaitIdle(be->graphicsQueue);
    if (be->cmdPool && be->vkDestroyCommandPool) {
        be->vkDestroyCommandPool(be->device, be->cmdPool, NULL);
        be->cmdPool = VK_NULL_HANDLE;
    }
    if (be->acquireSem && be->vkDestroySemaphore) {
        be->vkDestroySemaphore(be->device, be->acquireSem, NULL);
        be->acquireSem = VK_NULL_HANDLE;
    }
    if (be->renderDoneSem && be->vkDestroySemaphore) {
        be->vkDestroySemaphore(be->device, be->renderDoneSem, NULL);
        be->renderDoneSem = VK_NULL_HANDLE;
    }
    be->renderInitialized = 0;
}

// One-shot acquire/clear/present roundtrip used during init to validate the
// driver. Steady-state per-frame uses render_clear_frame() below.
//
// ---------- B.2c.1: one-shot acquire/clear/present sanity ----------

static int sanity_present(vk_backend_t* be)
{
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR =
        (PFN_vkAcquireNextImageKHR)be->vkGetDeviceProcAddr(be->device, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR vkQueuePresentKHR =
        (PFN_vkQueuePresentKHR)be->vkGetDeviceProcAddr(be->device, "vkQueuePresentKHR");
    PFN_vkQueueSubmit vkQueueSubmit =
        (PFN_vkQueueSubmit)be->vkGetDeviceProcAddr(be->device, "vkQueueSubmit");
    PFN_vkQueueWaitIdle vkQueueWaitIdle =
        (PFN_vkQueueWaitIdle)be->vkGetDeviceProcAddr(be->device, "vkQueueWaitIdle");
    PFN_vkCreateSemaphore vkCreateSemaphore =
        (PFN_vkCreateSemaphore)be->vkGetDeviceProcAddr(be->device, "vkCreateSemaphore");
    PFN_vkDestroySemaphore vkDestroySemaphore =
        (PFN_vkDestroySemaphore)be->vkGetDeviceProcAddr(be->device, "vkDestroySemaphore");
    PFN_vkCreateCommandPool vkCreateCommandPool =
        (PFN_vkCreateCommandPool)be->vkGetDeviceProcAddr(be->device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool vkDestroyCommandPool =
        (PFN_vkDestroyCommandPool)be->vkGetDeviceProcAddr(be->device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers =
        (PFN_vkAllocateCommandBuffers)be->vkGetDeviceProcAddr(be->device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer =
        (PFN_vkBeginCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer vkEndCommandBuffer =
        (PFN_vkEndCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkEndCommandBuffer");
    PFN_vkCmdClearColorImage vkCmdClearColorImage =
        (PFN_vkCmdClearColorImage)be->vkGetDeviceProcAddr(be->device, "vkCmdClearColorImage");
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier =
        (PFN_vkCmdPipelineBarrier)be->vkGetDeviceProcAddr(be->device, "vkCmdPipelineBarrier");

    if (!vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkQueueSubmit ||
        !vkQueueWaitIdle || !vkCreateSemaphore || !vkDestroySemaphore ||
        !vkCreateCommandPool || !vkDestroyCommandPool || !vkAllocateCommandBuffers ||
        !vkBeginCommandBuffer || !vkEndCommandBuffer ||
        !vkCmdClearColorImage || !vkCmdPipelineBarrier) {
        LOGE("sanity_present: device proc loading failed");
        return -1;
    }

    int rc = -1;
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(be->device, &sci, NULL, &acquireSem) != VK_SUCCESS ||
        vkCreateSemaphore(be->device, &sci, NULL, &renderDoneSem) != VK_SUCCESS) {
        LOGE("sanity_present: vkCreateSemaphore failed");
        goto cleanup;
    }

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = be->graphicsQueueFamily,
    };
    if (vkCreateCommandPool(be->device, &pci, NULL, &pool) != VK_SUCCESS) {
        LOGE("sanity_present: vkCreateCommandPool failed");
        goto cleanup;
    }

    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(be->device, &cai, &cb) != VK_SUCCESS) {
        LOGE("sanity_present: vkAllocateCommandBuffers failed");
        goto cleanup;
    }

    uint32_t imgIdx = 0;
    VkResult r = vkAcquireNextImageKHR(be->device, be->swapchain, 1000000000ULL,
                                       acquireSem, VK_NULL_HANDLE, &imgIdx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("sanity_present: vkAcquireNextImageKHR failed (VkResult=%d)", r);
        goto cleanup;
    }
    LOGI("sanity_present: acquired image index %u (VkResult=%d)", imgIdx, r);

    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bbi) != VK_SUCCESS) {
        LOGE("sanity_present: vkBeginCommandBuffer failed");
        goto cleanup;
    }

    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    // UNDEFINED → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier toDst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = be->swapchainImages[imgIdx],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &toDst);

    // Clear with VipleStream-ish dark green so it's distinguishable
    VkClearColorValue clearColor = { .float32 = { 0.0f, 0.18f, 0.0f, 1.0f } };
    vkCmdClearColorImage(cb, be->swapchainImages[imgIdx],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &range);

    // TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR
    VkImageMemoryBarrier toPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = be->swapchainImages[imgIdx],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &toPresent);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        LOGE("sanity_present: vkEndCommandBuffer failed");
        goto cleanup;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquireSem,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &renderDoneSem,
    };
    if (vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        LOGE("sanity_present: vkQueueSubmit failed");
        goto cleanup;
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderDoneSem,
        .swapchainCount = 1,
        .pSwapchains = &be->swapchain,
        .pImageIndices = &imgIdx,
    };
    r = vkQueuePresentKHR(be->graphicsQueue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("sanity_present: vkQueuePresentKHR failed (VkResult=%d)", r);
        goto cleanup;
    }
    LOGI("sanity_present: submitted + presented (VkResult=%d)", r);

    if (vkQueueWaitIdle(be->graphicsQueue) != VK_SUCCESS) {
        LOGE("sanity_present: vkQueueWaitIdle failed");
        goto cleanup;
    }
    LOGI("sanity_present: queue idle — full present roundtrip OK");
    rc = 0;

cleanup:
    if (pool) vkDestroyCommandPool(be->device, pool, NULL);
    if (acquireSem) vkDestroySemaphore(be->device, acquireSem, NULL);
    if (renderDoneSem) vkDestroySemaphore(be->device, renderDoneSem, NULL);
    return rc;
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

    // B.2c.1 sanity: drive one acquire/clear/present roundtrip to prove
    // the swapchain actually renders end-to-end on this driver before we
    // start trusting it with the MediaCodec pipeline.
    if (sanity_present(be) != 0) {
        LOGW("sanity_present failed — keeping init successful so we can dump diagnostics");
    }

    // B.2c.3a: persistent resources for steady-state per-frame rendering.
    if (init_render_resources(be) != 0) {
        LOGW("init_render_resources failed — per-frame nativeRenderClearFrame will return early");
    }

    // B.2c.3b: load AHB import procs. Don't gate init on this — if it
    // fails we just lose AHB import logging on Java-side import calls.
    if (load_ahb_procs(be) != 0) {
        LOGW("load_ahb_procs failed — nativeImportAhb will return -2 on every frame");
    }

    // Graphics pipeline is now LAZY (B.2c.3c.3): we wait for the first
    // AHB so its externalFormat can drive a VkSamplerYcbcrConversion
    // immutable sampler in the descriptor set layout. Until that lands
    // the per-frame render path falls back to the clear-only path.
    if (load_graphics_procs(be) != 0) {
        LOGW("load_graphics_procs failed — graphics pipeline will never lazy-init");
    }

    LOGI("B.2c.3c.3 init complete: instance + surface + device + queue + swapchain "
         "+ render + AHB (graphics pipeline lazy-init on first AHB frame)");
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

// ---------- B.2c.3c.2: graphics pipeline (test pattern fragment) ----------

#define LOAD_DEVICE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetDeviceProcAddr((be)->device, #name))

static int load_graphics_procs(vk_backend_t* be)
{
    be->vkCreateShaderModule        = LOAD_DEVICE_PROC(be, vkCreateShaderModule);
    be->vkDestroyShaderModule       = LOAD_DEVICE_PROC(be, vkDestroyShaderModule);
    be->vkCreateRenderPass          = LOAD_DEVICE_PROC(be, vkCreateRenderPass);
    be->vkDestroyRenderPass         = LOAD_DEVICE_PROC(be, vkDestroyRenderPass);
    be->vkCreateImageView           = LOAD_DEVICE_PROC(be, vkCreateImageView);
    be->vkDestroyImageView          = LOAD_DEVICE_PROC(be, vkDestroyImageView);
    be->vkCreateFramebuffer         = LOAD_DEVICE_PROC(be, vkCreateFramebuffer);
    be->vkDestroyFramebuffer        = LOAD_DEVICE_PROC(be, vkDestroyFramebuffer);
    be->vkCreatePipelineLayout      = LOAD_DEVICE_PROC(be, vkCreatePipelineLayout);
    be->vkDestroyPipelineLayout     = LOAD_DEVICE_PROC(be, vkDestroyPipelineLayout);
    be->vkCreateDescriptorSetLayout = LOAD_DEVICE_PROC(be, vkCreateDescriptorSetLayout);
    be->vkDestroyDescriptorSetLayout= LOAD_DEVICE_PROC(be, vkDestroyDescriptorSetLayout);
    be->vkCreateGraphicsPipelines   = LOAD_DEVICE_PROC(be, vkCreateGraphicsPipelines);
    be->vkDestroyPipeline           = LOAD_DEVICE_PROC(be, vkDestroyPipeline);
    be->vkCmdBeginRenderPass        = LOAD_DEVICE_PROC(be, vkCmdBeginRenderPass);
    be->vkCmdEndRenderPass          = LOAD_DEVICE_PROC(be, vkCmdEndRenderPass);
    be->vkCmdBindPipeline           = LOAD_DEVICE_PROC(be, vkCmdBindPipeline);
    be->vkCmdDraw                   = LOAD_DEVICE_PROC(be, vkCmdDraw);
    be->vkCreateDescriptorPool      = LOAD_DEVICE_PROC(be, vkCreateDescriptorPool);
    be->vkDestroyDescriptorPool     = LOAD_DEVICE_PROC(be, vkDestroyDescriptorPool);
    be->vkAllocateDescriptorSets    = LOAD_DEVICE_PROC(be, vkAllocateDescriptorSets);
    be->vkUpdateDescriptorSets      = LOAD_DEVICE_PROC(be, vkUpdateDescriptorSets);
    be->vkCmdBindDescriptorSets     = LOAD_DEVICE_PROC(be, vkCmdBindDescriptorSets);
    if (!be->vkCreateShaderModule || !be->vkDestroyShaderModule ||
        !be->vkCreateRenderPass || !be->vkDestroyRenderPass ||
        !be->vkCreateImageView || !be->vkDestroyImageView ||
        !be->vkCreateFramebuffer || !be->vkDestroyFramebuffer ||
        !be->vkCreatePipelineLayout || !be->vkDestroyPipelineLayout ||
        !be->vkCreateDescriptorSetLayout || !be->vkDestroyDescriptorSetLayout ||
        !be->vkCreateGraphicsPipelines || !be->vkDestroyPipeline ||
        !be->vkCmdBeginRenderPass || !be->vkCmdEndRenderPass ||
        !be->vkCmdBindPipeline || !be->vkCmdDraw ||
        !be->vkCreateDescriptorPool || !be->vkDestroyDescriptorPool ||
        !be->vkAllocateDescriptorSets || !be->vkUpdateDescriptorSets ||
        !be->vkCmdBindDescriptorSets) {
        LOGE("load_graphics_procs: missing entry points");
        return -1;
    }
    return 0;
}

static int init_graphics_pipeline(vk_backend_t* be)
{
    if (load_graphics_procs(be) != 0) return -1;

    // Render pass: 1 color attachment matching swapchain format. clear→
    // present_src so the framebuffer can hand off to vkQueuePresentKHR.
    VkAttachmentDescription colorAttach = {
        .format         = be->swapchainFormat,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference colorRef = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorRef,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &colorAttach,
        .subpassCount    = 1, .pSubpasses    = &subpass,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    if (be->vkCreateRenderPass(be->device, &rpci, NULL, &be->renderPass) != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed"); return -1;
    }

    // Image views + framebuffers per swapchain image
    be->swapchainViews = (VkImageView*)calloc(be->swapchainImageCount, sizeof(VkImageView));
    be->framebuffers   = (VkFramebuffer*)calloc(be->swapchainImageCount, sizeof(VkFramebuffer));
    if (!be->swapchainViews || !be->framebuffers) return -1;

    for (uint32_t i = 0; i < be->swapchainImageCount; i++) {
        VkImageViewCreateInfo ivci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = be->swapchainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = be->swapchainFormat,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (be->vkCreateImageView(be->device, &ivci, NULL, &be->swapchainViews[i]) != VK_SUCCESS) {
            LOGE("vkCreateImageView[%u] failed", i); return -1;
        }
        VkFramebufferCreateInfo fci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = be->renderPass,
            .attachmentCount = 1,
            .pAttachments    = &be->swapchainViews[i],
            .width           = be->swapchainExtent.width,
            .height          = be->swapchainExtent.height,
            .layers          = 1,
        };
        if (be->vkCreateFramebuffer(be->device, &fci, NULL, &be->framebuffers[i]) != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer[%u] failed", i); return -1;
        }
    }

    // Shader modules. Pick the frag based on whether the YCbCr sampler is
    // ready (= we'll have a real video texture to sample). Without it we
    // fall back to the UV-gradient test pattern.
    VkShaderModuleCreateInfo smciV = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fullscreen_vert_spv_len,
        .pCode    = (const uint32_t*)fullscreen_vert_spv,
    };
    VkShaderModuleCreateInfo smciF = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    if (be->ycbcrInitialized) {
        smciF.codeSize = video_sample_frag_spv_len;
        smciF.pCode    = (const uint32_t*)video_sample_frag_spv;
    } else {
        smciF.codeSize = test_pattern_frag_spv_len;
        smciF.pCode    = (const uint32_t*)test_pattern_frag_spv;
    }
    if (be->vkCreateShaderModule(be->device, &smciV, NULL, &be->vertShader) != VK_SUCCESS ||
        be->vkCreateShaderModule(be->device, &smciF, NULL, &be->fragShader) != VK_SUCCESS) {
        LOGE("vkCreateShaderModule failed"); return -1;
    }

    // Descriptor set layout: 1 binding for the YCbCr-converted video
    // sampler. Immutable sampler is REQUIRED for VkSamplerYcbcrConversion
    // — the conversion has to be known at pipeline-compile time. If
    // ycbcrSampler isn't ready yet we fall back to an empty layout
    // (test_pattern frag), expecting a later lazy re-init.
    VkDescriptorSetLayoutBinding videoBind = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = be->ycbcrInitialized ? &be->ycbcrSampler : NULL,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = be->ycbcrInitialized ? 1u : 0u,
        .pBindings    = be->ycbcrInitialized ? &videoBind : NULL,
    };
    if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL, &be->descLayout) != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed"); return -1;
    }

    // Descriptor pool + 1 descriptor set (only if we have a sampler binding)
    if (be->ycbcrInitialized) {
        VkDescriptorPoolSize poolSize = {
            .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
        };
        VkDescriptorPoolCreateInfo dpci = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = 1,
            .poolSizeCount = 1,
            .pPoolSizes    = &poolSize,
        };
        if (be->vkCreateDescriptorPool(be->device, &dpci, NULL, &be->descPool) != VK_SUCCESS) {
            LOGE("vkCreateDescriptorPool failed"); return -1;
        }
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = be->descPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &be->descLayout,
        };
        if (be->vkAllocateDescriptorSets(be->device, &dsai, &be->descSet) != VK_SUCCESS) {
            LOGE("vkAllocateDescriptorSets failed"); return -1;
        }
    }
    VkPipelineLayoutCreateInfo plci = {
        .sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts    = &be->descLayout,
    };
    if (be->vkCreatePipelineLayout(be->device, &plci, NULL, &be->pipelineLayout) != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed"); return -1;
    }

    // Graphics pipeline
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = be->vertShader, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = be->fragShader, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0,
        (float)be->swapchainExtent.width, (float)be->swapchainExtent.height,
        0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, be->swapchainExtent };
    VkPipelineViewportStateCreateInfo vpst = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode      = VK_POLYGON_MODE_FILL,
        .cullMode         = VK_CULL_MODE_NONE,
        .frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth        = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cbAtt = {
        .blendEnable    = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &cbAtt,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,        .pStages           = stages,
        .pVertexInputState   = &vi,      .pInputAssemblyState = &ia,
        .pViewportState      = &vpst,    .pRasterizationState = &rs,
        .pMultisampleState   = &ms,      .pColorBlendState  = &cb,
        .layout              = be->pipelineLayout,
        .renderPass          = be->renderPass,
        .subpass             = 0,
    };
    if (be->vkCreateGraphicsPipelines(be->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                       &be->graphicsPipeline) != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed"); return -1;
    }

    be->graphicsInitialized = 1;
    LOGI("graphics pipeline ready: renderPass + %u framebuffers + pipeline (frag=%s)",
         be->swapchainImageCount, be->ycbcrInitialized ? "video_sample" : "test_pattern");
    return 0;
}

static void destroy_graphics_pipeline(vk_backend_t* be)
{
    if (!be->graphicsInitialized) return;
    if (be->descPool)         be->vkDestroyDescriptorPool(be->device, be->descPool, NULL);
    be->descPool = VK_NULL_HANDLE;
    be->descSet  = VK_NULL_HANDLE;
    if (be->graphicsPipeline) be->vkDestroyPipeline(be->device, be->graphicsPipeline, NULL);
    if (be->pipelineLayout)   be->vkDestroyPipelineLayout(be->device, be->pipelineLayout, NULL);
    if (be->descLayout)       be->vkDestroyDescriptorSetLayout(be->device, be->descLayout, NULL);
    if (be->vertShader)       be->vkDestroyShaderModule(be->device, be->vertShader, NULL);
    if (be->fragShader)       be->vkDestroyShaderModule(be->device, be->fragShader, NULL);
    if (be->framebuffers) {
        for (uint32_t i = 0; i < be->swapchainImageCount; i++)
            if (be->framebuffers[i]) be->vkDestroyFramebuffer(be->device, be->framebuffers[i], NULL);
        free(be->framebuffers);
        be->framebuffers = NULL;
    }
    if (be->swapchainViews) {
        for (uint32_t i = 0; i < be->swapchainImageCount; i++)
            if (be->swapchainViews[i]) be->vkDestroyImageView(be->device, be->swapchainViews[i], NULL);
        free(be->swapchainViews);
        be->swapchainViews = NULL;
    }
    if (be->renderPass) be->vkDestroyRenderPass(be->device, be->renderPass, NULL);
    be->graphicsPipeline = VK_NULL_HANDLE;
    be->pipelineLayout   = VK_NULL_HANDLE;
    be->descLayout       = VK_NULL_HANDLE;
    be->vertShader       = VK_NULL_HANDLE;
    be->fragShader       = VK_NULL_HANDLE;
    be->renderPass       = VK_NULL_HANDLE;
    be->graphicsInitialized = 0;
}

// ---------- B.2c.3a: per-frame steady-state clear+present ----------

static int render_clear_frame(vk_backend_t* be)
{
    if (!be->renderInitialized) return -1;

    // Cycle the clear color so the screen visibly animates — useful for
    // proving the loop is firing per MediaCodec frame, and for screen-cap
    // visual diff against a black-screen failure mode.
    int t = be->frameCounter++;
    float r = ((t      ) & 0xFF) / 255.0f;
    float g = ((t >>  4) & 0xFF) / 255.0f;
    float b = ((t >>  2) & 0xFF) / 255.0f;
    VkClearColorValue clearColor = { .float32 = { r, g, b, 1.0f } };

    uint32_t imgIdx = 0;
    VkResult r1 = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                            be->acquireSem, VK_NULL_HANDLE, &imgIdx);
    if (r1 != VK_SUCCESS && r1 != VK_SUBOPTIMAL_KHR) {
        if (be->frameCounter <= 5 || be->frameCounter % 60 == 0)
            LOGW("vkAcquireNextImageKHR returned %d at frame %d", r1, be->frameCounter);
        return -1;
    }

    be->vkResetCommandBuffer(be->cmdBuffer, 0);
    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (be->vkBeginCommandBuffer(be->cmdBuffer, &bbi) != VK_SUCCESS) return -1;

    if (be->graphicsInitialized) {
        // §I.B.2c.3c.2 path: render pass + fullscreen triangle test pattern.
        // Render pass loadOp=CLEAR with the cycling color, then frag shader
        // overwrites with the UV gradient. Final layout PRESENT_SRC_KHR
        // is set by the render pass itself.
        VkClearValue cv = { .color = clearColor };
        VkRenderPassBeginInfo rpbi = {
            .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass   = be->renderPass,
            .framebuffer  = be->framebuffers[imgIdx],
            .renderArea   = { {0, 0}, be->swapchainExtent },
            .clearValueCount = 1,
            .pClearValues = &cv,
        };
        be->vkCmdBeginRenderPass(be->cmdBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              be->graphicsPipeline);
        be->vkCmdDraw(be->cmdBuffer, 3, 1, 0, 0);   // fullscreen triangle
        be->vkCmdEndRenderPass(be->cmdBuffer);
    } else {
        // Fallback: B.2c.3a clear-only path. Used if pipeline init failed.
        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        };
        VkImageMemoryBarrier toDst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = be->swapchainImages[imgIdx],
            .subresourceRange = range,
        };
        be->vkCmdPipelineBarrier(be->cmdBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &toDst);
        be->vkCmdClearColorImage(be->cmdBuffer, be->swapchainImages[imgIdx],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clearColor, 1, &range);
        VkImageMemoryBarrier toPresent = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = be->swapchainImages[imgIdx],
            .subresourceRange = range,
        };
        be->vkCmdPipelineBarrier(be->cmdBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 1, &toPresent);
    }

    if (be->vkEndCommandBuffer(be->cmdBuffer) != VK_SUCCESS) return -1;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->acquireSem,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &be->cmdBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &be->renderDoneSem,
    };
    if (be->vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return -1;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->renderDoneSem,
        .swapchainCount = 1,
        .pSwapchains = &be->swapchain,
        .pImageIndices = &imgIdx,
    };
    VkResult r2 = be->vkQueuePresentKHR(be->graphicsQueue, &pi);
    if (r2 != VK_SUCCESS && r2 != VK_SUBOPTIMAL_KHR) {
        LOGW("vkQueuePresentKHR returned %d at frame %d", r2, be->frameCounter);
        return -1;
    }

    // Simple sync — eat the V-sync block per frame. Phase D will do real
    // async with timeline semaphores / fence-based fast path.
    be->vkQueueWaitIdle(be->graphicsQueue);

    if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
        LOGI("render_clear_frame #%d (img=%u, color=%.2f,%.2f,%.2f)",
             be->frameCounter, imgIdx, r, g, b);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeRenderClearFrame(JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return -1;
    return (jint)render_clear_frame(be);
}

// Per-frame video render: import AHB → bind → draw → present → cleanup.
static int render_ahb_frame(vk_backend_t* be, AHardwareBuffer* ahb)
{
    // First-frame lazy init of pipeline once we know the externalFormat
    // from the AHB. ensure_ycbcr_sampler runs first (called by import).
    if (!be->graphicsInitialized) {
        VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
        };
        VkAndroidHardwareBufferPropertiesANDROID propsOnly = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
            .pNext = &fmt,
        };
        if (be->vkGetAndroidHardwareBufferPropertiesANDROID(be->device, ahb, &propsOnly) == VK_SUCCESS &&
            fmt.externalFormat != 0) {
            ensure_ycbcr_sampler(be, &fmt);
            if (init_graphics_pipeline(be) != 0) {
                LOGW("lazy graphics pipeline init failed; staying on clear-only path");
                return render_clear_frame(be);
            }
        } else {
            return render_clear_frame(be);
        }
    }

    // Import the AHB into VkImage+memory+view (kept alive for the GPU).
    VkImage   imgIn  = VK_NULL_HANDLE;
    VkDeviceMemory memIn = VK_NULL_HANDLE;
    VkImageView viewIn = VK_NULL_HANDLE;
    uint32_t srcW = 0, srcH = 0;
    if (do_import_ahb(be, ahb, /*keepAlive=*/1, &imgIn, &memIn, &viewIn, &srcW, &srcH) != 0) {
        return render_clear_frame(be);
    }

    // Update descriptor set: combined image sampler binding 0 → (sampler
    // is immutable in the layout; we only fill imageView + layout).
    VkDescriptorImageInfo dii = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = viewIn,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wds = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = be->descSet,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &dii,
    };
    be->vkUpdateDescriptorSets(be->device, 1, &wds, 0, NULL);

    uint32_t imgIdx = 0;
    VkResult r = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                           be->acquireSem, VK_NULL_HANDLE, &imgIdx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGW("acquire failed: %d", r);
        goto fail_cleanup_imported;
    }

    be->vkResetCommandBuffer(be->cmdBuffer, 0);
    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (be->vkBeginCommandBuffer(be->cmdBuffer, &bbi) != VK_SUCCESS) goto fail_cleanup_imported;

    // Imported image arrives in EXTERNAL queue family (AHB external memory).
    // Acquire ownership + transition to SHADER_READ_ONLY_OPTIMAL.
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    VkImageMemoryBarrier inAcquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = be->graphicsQueueFamily,
        .image = imgIn,
        .subresourceRange = range,
    };
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &inAcquire);

    VkClearValue cv = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass   = be->renderPass,
        .framebuffer  = be->framebuffers[imgIdx],
        .renderArea   = { {0, 0}, be->swapchainExtent },
        .clearValueCount = 1,
        .pClearValues = &cv,
    };
    be->vkCmdBeginRenderPass(be->cmdBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, be->graphicsPipeline);
    be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                be->pipelineLayout, 0, 1, &be->descSet, 0, NULL);
    be->vkCmdDraw(be->cmdBuffer, 3, 1, 0, 0);
    be->vkCmdEndRenderPass(be->cmdBuffer);

    if (be->vkEndCommandBuffer(be->cmdBuffer) != VK_SUCCESS) goto fail_cleanup_imported;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->acquireSem,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &be->cmdBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &be->renderDoneSem,
    };
    if (be->vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
        goto fail_cleanup_imported;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->renderDoneSem,
        .swapchainCount = 1,
        .pSwapchains = &be->swapchain,
        .pImageIndices = &imgIdx,
    };
    r = be->vkQueuePresentKHR(be->graphicsQueue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGW("present failed: %d", r);
    }

    // Wait queue idle so we can free the imported resources before next
    // frame. Simple but slow — Phase D will queue these for delayed free.
    be->vkQueueWaitIdle(be->graphicsQueue);

    be->frameCounter++;
    if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
        LOGI("render_ahb_frame #%d (img=%u, src=%ux%u)", be->frameCounter, imgIdx, srcW, srcH);
    }

fail_cleanup_imported:
    if (viewIn) be->vkDestroyImageView(be->device, viewIn, NULL);
    if (imgIn)  be->vkDestroyImage(be->device, imgIn, NULL);
    if (memIn)  be->vkFreeMemory(be->device, memIn, NULL);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeRenderFrame(
    JNIEnv* env, jclass clazz, jlong handle, jobject jHwBuffer)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return -1;
    if (!jHwBuffer) return (jint)render_clear_frame(be);
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, jHwBuffer);
    if (!ahb) return (jint)render_clear_frame(be);
    return (jint)render_ahb_frame(be, ahb);
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VkBackend_nativeDestroy(JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return;
    LOGI("destroying backend handle=%p (frames rendered=%d)", be, be->frameCounter);
    destroy_graphics_pipeline(be);
    destroy_ycbcr_sampler(be);
    destroy_render_resources(be);
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
