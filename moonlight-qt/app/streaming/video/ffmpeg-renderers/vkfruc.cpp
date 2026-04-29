// VipleStream §J.3.e.2.i — VkFrucRenderer
// See vkfruc.h header + docs/J.3.e.2.i_vulkan_native_renderer.md.

#include "vkfruc.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

// §J.3.e.2.i.3.a — FFmpeg 6.1 (LIBAVCODEC_VERSION_MAJOR=60) used a
// MESA AV1 ext name; 7.0 (61+) uses official KHR.  AVVulkanDeviceContext
// also bumped its queue-family struct shape between the two.  We
// follow PlVkRenderer's compile-time gating.
#ifndef VK_KHR_video_decode_av1
#define VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME "VK_KHR_video_decode_av1"
#endif

// Shared lock for ffmpeg vkQueueSubmit serialisation (per AVVulkanDeviceContext
// contract).  ffmpeg may submit on multiple threads; our renderFrame is
// single-threaded, but ffmpeg can submit decode work concurrently.
static std::mutex s_VkFrucQueueLock;

// §J.3.e.2.i.6 — process-wide ref count for ncnn::create_gpu_instance.
// Multiple VkFrucRenderer instances (test probes + real) each call create
// in createFrucComputeResources; we must call destroy ONLY when the last
// instance tears down — otherwise teardown order vs ncnn's static dtors
// causes SIGSEGV on process exit (observed in v1.3.151 dual-present test).
// PlVkRenderer pattern (plvk.cpp:158): ncnn::destroy_gpu_instance() must
// run BEFORE the renderer's VkDevice is destroyed.
static std::atomic<int> s_NcnnRefCount(0);

VkFrucRenderer::VkFrucRenderer(int pass)
    : IFFmpegRenderer(RendererType::Vulkan)
    , m_Pass(pass)
{
    // §J.3.e.2.i.3.e-SW — opt into software-decode upload path when env
    // var is set.  Bypasses FFmpeg-Vulkan hwcontext entirely (which is
    // currently broken; see docs/J.3.e.2.i_vulkan_native_renderer.md).
    m_SwMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_SW") != 0;
    m_FrucMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_FRUC") != 0;
    m_DualMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_DUAL") != 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=%d, swMode=%d, frucMode=%d, dualMode=%d)",
                pass, m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0);
}

// §J.3.e.2.i.3.e-SW — declare NV12 as preferred so FFmpeg get_format()
// picks software NV12 output when our renderer is selected via the
// software cascade.  In Vulkan-hwaccel mode (m_SwMode=false), we don't
// participate in the software cascade — return base default (will be
// ignored anyway because Vulkan path goes through createHwAccelRenderer).
AVPixelFormat VkFrucRenderer::getPreferredPixelFormat(int videoFormat)
{
    if (m_SwMode) {
        // FFmpeg software h264 / hevc / av1 decoders default to YUV420P
        // (3 planes: Y + U + V).  We accept both YUV420P and NV12 in
        // renderFrameSw() and re-pack into our NV12 multi-plane VkImage.
        return AV_PIX_FMT_YUV420P;
    }
    return IFFmpegRenderer::getPreferredPixelFormat(videoFormat);
}

bool VkFrucRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat)
{
    if (m_SwMode) {
        return pixelFormat == AV_PIX_FMT_YUV420P || pixelFormat == AV_PIX_FMT_NV12;
    }
    return IFFmpegRenderer::isPixelFormatSupported(videoFormat, pixelFormat);
}

VkFrucRenderer::~VkFrucRenderer()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 dtor");
    teardown();
    if (m_HwDeviceCtx != nullptr) {
        av_buffer_unref(&m_HwDeviceCtx);
        m_HwDeviceCtx = nullptr;
    }
}

// AVVulkanDeviceContext lock_queue / unlock_queue callbacks.  ffmpeg
// uses these to serialise vkQueueSubmit calls on shared queues.  We
// only have one graphics queue family so a single mutex suffices.
void VkFrucRenderer::lockQueueStub(struct AVHWDeviceContext*, uint32_t, uint32_t)
{
    s_VkFrucQueueLock.lock();
}
void VkFrucRenderer::unlockQueueStub(struct AVHWDeviceContext*, uint32_t, uint32_t)
{
    s_VkFrucQueueLock.unlock();
}

void VkFrucRenderer::teardown()
{
    // §J.3.e.2.i.3.e — drain GPU first.  Pending submits may still hold
    // image views / descriptor sets / cmd buffers; if we destroy them
    // mid-flight the driver will explode (or silently corrupt).
    if (m_Device != VK_NULL_HANDLE && m_pfnGetInstanceProcAddr) {
        auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        if (pfnGetDevPa) {
            auto pfnDeviceWaitIdle = (PFN_vkDeviceWaitIdle)pfnGetDevPa(
                m_Device, "vkDeviceWaitIdle");
            if (pfnDeviceWaitIdle) {
                pfnDeviceWaitIdle(m_Device);
            }
        }
    }

    // Order: device-owned objects (descriptor pool → in-flight ring →
    // pipelines → render-pass → layouts → sampler) → swapchain → device
    // → surface → instance.  Pipelines reference render pass + descriptor
    // set layout, so they go first; sampler-conversion holds the
    // immutable sampler used in descriptor set layout, so layouts go
    // before the sampler.
    destroyInterpGraphicsPipeline(); // §J.3.e.2.i.4.2
    destroyFrucComputeResources(); // §J.3.e.2.i.4
    destroySwUploadResources();   // §J.3.e.2.i.3.e-SW
    destroyDescriptorPool();
    destroyInFlightRing();
    destroyGraphicsPipeline();
    destroyRenderPassAndFramebuffers();
    destroyYcbcrSamplerAndLayouts();
    destroySwapchain();
    if (m_Device != VK_NULL_HANDLE && m_pfnDestroyDevice) {
        m_pfnDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_Surface != VK_NULL_HANDLE && m_pfnDestroySurfaceKHR) {
        m_pfnDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE && m_pfnDestroyInstance) {
        m_pfnDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::createInstanceAndSurface(SDL_Window* window)
{
    m_pfnGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!m_pfnGetInstanceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetVkGetInstanceProcAddr failed: %s",
                     SDL_GetError());
        return false;
    }

    auto pfnCreateInstance =
        (PFN_vkCreateInstance)m_pfnGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!pfnCreateInstance) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 getProc(vkCreateInstance) NULL");
        return false;
    }

    unsigned int extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #1: %s",
                     SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(extCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, exts.data())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #2: %s",
                     SDL_GetError());
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VipleStreamFruc";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "VipleStream";
    appInfo.engineVersion = 1;
    // §J.3.e.2.i.3.a CRITICAL — FFmpeg's hwcontext_vulkan.h documents
    // `VkInstance inst` requires "at least version 1.3" (libavutil 60+).
    // 1.1 here causes the NV driver dispatch table to NOT populate
    // 1.3-core PFNs (synchronization2, dynamic_rendering, etc.) that
    // FFmpeg's decoder calls at submitDecodeUnit time → NULL deref at
    // offset 0xF0 in nvoglv64 (verified via cdb on minidump
    // VipleStream-1777464748.dmp; root cause of v1.3.123-133 crashes).
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();

    VkResult rc = pfnCreateInstance(&ici, nullptr, &m_Instance);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateInstance rc=%d", (int)rc);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkInstance created (exts=%u)",
                (unsigned)exts.size());

    m_pfnDestroyInstance = (PFN_vkDestroyInstance)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroyInstance");
    m_pfnDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroySurfaceKHR");
    if (!m_pfnDestroyInstance || !m_pfnDestroySurfaceKHR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 instance proc table incomplete");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, m_Instance, &m_Surface)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_CreateSurface: %s",
                     SDL_GetError());
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkSurfaceKHR created");
    return true;
}

bool VkFrucRenderer::pickPhysicalDeviceAndQueue()
{
    auto pfnEnumPDs = (PFN_vkEnumeratePhysicalDevices)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumeratePhysicalDevices");
    auto pfnGetPDProps = (PFN_vkGetPhysicalDeviceProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceProperties");
    auto pfnGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnGetSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    auto pfnEnumDevExts = (PFN_vkEnumerateDeviceExtensionProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumerateDeviceExtensionProperties");
    (void)pfnEnumDevExts;
    if (!pfnEnumPDs || !pfnGetPDProps || !pfnGetQFP || !pfnGetSurfSupport) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD proc PFN load failed");
        return false;
    }

    uint32_t pdCount = 0;
    pfnEnumPDs(m_Instance, &pdCount, nullptr);
    if (pdCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    pfnEnumPDs(m_Instance, &pdCount, pds.data());

    // Pick first device with a queue family that supports both graphics
    // and present on m_Surface.  Prefer DISCRETE_GPU; fall back to whatever
    // works.  PC NVIDIA / AMD / Intel typically have a "universal" queue
    // family — same pattern as Android's pick_physical_device_and_queue.
    int discreteIdx = -1, anyIdx = -1;
    uint32_t discreteQF = UINT32_MAX, anyQF = UINT32_MAX;
    for (uint32_t i = 0; i < pdCount; i++) {
        VkPhysicalDeviceProperties pdp = {};
        pfnGetPDProps(pds[i], &pdp);

        uint32_t qfCount = 0;
        pfnGetQFP(pds[i], &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        pfnGetQFP(pds[i], &qfCount, qfs.data());

        for (uint32_t qf = 0; qf < qfCount; qf++) {
            VkBool32 surfaceOk = VK_FALSE;
            pfnGetSurfSupport(pds[i], qf, m_Surface, &surfaceOk);
            const bool gfxOk = (qfs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (!surfaceOk || !gfxOk) continue;

            if (anyIdx < 0) { anyIdx = (int)i; anyQF = qf; }
            if (pdp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && discreteIdx < 0) {
                discreteIdx = (int)i; discreteQF = qf;
            }
            break;  // one queue family per device is enough for our pick
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD[%u] '%s' type=%d apiVer=%u",
                    i, pdp.deviceName, (int)pdp.deviceType, pdp.apiVersion);
    }

    if (discreteIdx >= 0) {
        m_PhysicalDevice = pds[discreteIdx];
        m_QueueFamily    = discreteQF;
    } else if (anyIdx >= 0) {
        m_PhysicalDevice = pds[anyIdx];
        m_QueueFamily    = anyQF;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no PD with graphics+present queue");
        return false;
    }

    VkPhysicalDeviceProperties chosen = {};
    pfnGetPDProps(m_PhysicalDevice, &chosen);

    // §J.3.e.2.i.3.a — also locate a queue family with VIDEO_DECODE_BIT_KHR
    // so ffmpeg can decode H264/H265/AV1 into AVVkFrame on our device.
    // NV typically exposes this on QF=3 (separate from graphics QF=0).
    uint32_t qfCount = 0;
    pfnGetQFP(m_PhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    pfnGetQFP(m_PhysicalDevice, &qfCount, qfs.data());
    for (uint32_t qf = 0; qf < qfCount; qf++) {
        if (qfs[qf].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            m_DecodeQueueFamily = qf;
            m_DecodeQueueCount  = qfs[qf].queueCount;
            break;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 picked '%s' (QF gfx=%u, decode=%u%s) %s",
                chosen.deviceName, m_QueueFamily,
                m_DecodeQueueFamily,
                m_DecodeQueueFamily == UINT32_MAX ? " — NO VIDEO DECODE QF!" : "",
                discreteIdx >= 0 ? "(DISCRETE_GPU)" : "(integrated/other)");
    return true;
}

bool VkFrucRenderer::createLogicalDevice()
{
    auto pfnCreateDevice = (PFN_vkCreateDevice)m_pfnGetInstanceProcAddr(
        m_Instance, "vkCreateDevice");
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnCreateDevice || !pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc PFN load failed");
        return false;
    }

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    {
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_QueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        qcis.push_back(qci);
    }
    if (m_DecodeQueueFamily != UINT32_MAX && m_DecodeQueueFamily != m_QueueFamily) {
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_DecodeQueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        qcis.push_back(qci);
    }

    // i.3.a extension list: swapchain + ycbcr (i.2.b) + video decode
    // (so ffmpeg can build AVVkFrame on our device).  Mirrors
    // PlVkRenderer's k_RequiredDeviceExtensions video-decode block.
    std::vector<const char*> devExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
#if LIBAVCODEC_VERSION_MAJOR >= 61
        VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
#endif
        // ffmpeg's vk hwcontext also asks for synchronization2 +
        // timeline semaphore.  Both are core in Vulkan 1.3 / available
        // as 1.2 ext.  Add explicitly for safety.
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };

    // §J.3.e.2.i.3.a — feature chain stored in members so it persists for
    // FFmpeg's lifetime (vkCtx->device_features.pNext walks this chain
    // long after createLogicalDevice returns).  Mirrors what libplacebo
    // does in PlVkRenderer: query EVERYTHING the device supports via
    // vkGetPhysicalDeviceFeatures2 and enable it all when creating the
    // device.  FFmpeg's hwcontext_vulkan internal code paths assume the
    // device has full feature set (shaderImageGatherExtended,
    // fragmentStoresAndAtomics, etc.); enabling only a minimal subset
    // causes NV driver NULL deref in submitDecodeUnit (v1.3.123-135).
    m_Sync2Feat = {};
    m_Sync2Feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;

    m_TimelineFeat = {};
    m_TimelineFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    m_TimelineFeat.pNext = &m_Sync2Feat;

    m_YcbcrFeat = {};
    m_YcbcrFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    m_YcbcrFeat.pNext = &m_TimelineFeat;

    m_DevFeat2 = {};
    m_DevFeat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    m_DevFeat2.pNext = &m_YcbcrFeat;

    // Query the physical device for all supported features — populate
    // m_DevFeat2 + chain in-place.  Then we enable everything the device
    // says it supports.
    auto pfnGetPhysFeat2 = (PFN_vkGetPhysicalDeviceFeatures2)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceFeatures2");
    if (pfnGetPhysFeat2) {
        pfnGetPhysFeat2(m_PhysicalDevice, &m_DevFeat2);
    } else {
        // Fallback: hardcode the minimum set we know we need.
        m_Sync2Feat.synchronization2         = VK_TRUE;
        m_TimelineFeat.timelineSemaphore     = VK_TRUE;
        m_YcbcrFeat.samplerYcbcrConversion   = VK_TRUE;
    }

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &m_DevFeat2;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    VkResult rc = pfnCreateDevice(m_PhysicalDevice, &dci, nullptr, &m_Device);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateDevice rc=%d", (int)rc);
        return false;
    }

    m_pfnDestroyDevice = (PFN_vkDestroyDevice)pfnGetDeviceProcAddr(m_Device, "vkDestroyDevice");
    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)pfnGetDeviceProcAddr(m_Device, "vkGetDeviceQueue");
    if (!m_pfnDestroyDevice || !pfnGetDeviceQueue) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc table incomplete");
        return false;
    }
    pfnGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_GraphicsQueue);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkDevice created (%s, QF=%u, queue=%p)",
                "KHR_swapchain + KHR_sampler_ycbcr_conversion",
                m_QueueFamily, (void*)m_GraphicsQueue);
    return true;
}

bool VkFrucRenderer::createSwapchain()
{
    auto pfnGetSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto pfnGetSurfFmts = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto pfnGetSurfModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnGetSurfCaps || !pfnGetSurfFmts || !pfnGetSurfModes || !pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b PFN load (instance) failed");
        return false;
    }
    auto pfnCreateSwapchain = (PFN_vkCreateSwapchainKHR)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSwapchainKHR");
    auto pfnGetSwapImgs = (PFN_vkGetSwapchainImagesKHR)pfnGetDeviceProcAddr(
        m_Device, "vkGetSwapchainImagesKHR");
    auto pfnCreateImgView = (PFN_vkCreateImageView)pfnGetDeviceProcAddr(
        m_Device, "vkCreateImageView");
    if (!pfnCreateSwapchain || !pfnGetSwapImgs || !pfnCreateImgView) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b PFN load (device) failed");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps = {};
    if (pfnGetSurfCaps(m_PhysicalDevice, m_Surface, &caps) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return false;
    }

    // Image count: minImageCount + 1 for triple buffering, capped by
    // maxImageCount (0 = unbounded per spec).
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Extent: prefer surface's currentExtent; fall back to SDL's window
    // size if currentExtent is { 0xFFFFFFFF, 0xFFFFFFFF } (some drivers
    // signal "renderer chooses" with that value).
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(m_Window, &w, &h);
        extent.width  = (uint32_t)w;
        extent.height = (uint32_t)h;
    }
    m_SwapchainExtent = extent;

    // Format: pick BGRA8 SRGB / UNORM if available, else first.
    uint32_t fmtCount = 0;
    pfnGetSurfFmts(m_PhysicalDevice, m_Surface, &fmtCount, nullptr);
    if (fmtCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b no supported surface formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    pfnGetSurfFmts(m_PhysicalDevice, m_Surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f; break;
        }
    }
    m_SwapchainFormat     = chosen.format;
    m_SwapchainColorSpace = chosen.colorSpace;

    // Present mode: IMMEDIATE for --no-vsync, MAILBOX as fallback,
    // FIFO guaranteed by spec.
    uint32_t modeCount = 0;
    pfnGetSurfModes(m_PhysicalDevice, m_Surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    pfnGetSurfModes(m_PhysicalDevice, m_Surface, &modeCount, modes.data());
    VkPresentModeKHR pmode = VK_PRESENT_MODE_FIFO_KHR;
    bool wantImmediate = !qEnvironmentVariableIntValue("VIPLE_VK_FRUC_VSYNC");
    if (wantImmediate) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { pmode = m; break; }
            if (m == VK_PRESENT_MODE_MAILBOX_KHR && pmode == VK_PRESENT_MODE_FIFO_KHR) {
                pmode = m;  // mailbox as 2nd choice
            }
        }
    }
    m_SwapchainPresentMode = pmode;

    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = m_Surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.queueFamilyIndexCount = 0;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = pmode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    if (pfnCreateSwapchain(m_Device, &sci, nullptr, &m_Swapchain) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t imgCnt = 0;
    pfnGetSwapImgs(m_Device, m_Swapchain, &imgCnt, nullptr);
    m_SwapchainImages.resize(imgCnt);
    pfnGetSwapImgs(m_Device, m_Swapchain, &imgCnt, m_SwapchainImages.data());

    m_SwapchainViews.resize(imgCnt, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imgCnt; i++) {
        VkImageViewCreateInfo ivCi = {};
        ivCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivCi.image = m_SwapchainImages[i];
        ivCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCi.format = chosen.format;
        ivCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCi.subresourceRange.levelCount = 1;
        ivCi.subresourceRange.layerCount = 1;
        if (pfnCreateImgView(m_Device, &ivCi, nullptr, &m_SwapchainViews[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkCreateImageView failed (i=%u)", i);
            return false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2.b VkSwapchainKHR created "
                "(format=%d colorSpace=%d extent=%ux%u images=%u presentMode=%d)",
                (int)chosen.format, (int)chosen.colorSpace,
                extent.width, extent.height, imgCnt, (int)pmode);
    return true;
}

void VkFrucRenderer::destroySwapchain()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyImgView = (PFN_vkDestroyImageView)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyImageView");
    auto pfnDestroySwapchain = (PFN_vkDestroySwapchainKHR)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySwapchainKHR");
    for (auto v : m_SwapchainViews) {
        if (v != VK_NULL_HANDLE && pfnDestroyImgView)
            pfnDestroyImgView(m_Device, v, nullptr);
    }
    m_SwapchainViews.clear();
    m_SwapchainImages.clear();
    if (m_Swapchain != VK_NULL_HANDLE && pfnDestroySwapchain) {
        pfnDestroySwapchain(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    if (!createInstanceAndSurface(m_Window)) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!pickPhysicalDeviceAndQueue()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createLogicalDevice()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createSwapchain()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    m_VideoFormat = params->videoFormat;
    // §J.3.e.2.i.3.e-SW — skip AVHWDeviceContext + AVVulkanDeviceContext
    // setup when in software-upload mode; we don't bridge ffmpeg to our
    // VkDevice in that path (frames come in CPU memory as AV_PIX_FMT_NV12).
    if (!m_SwMode) {
        if (!populateAvHwDeviceCtx(m_VideoFormat)) {
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            teardown();
            return false;
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW skipping AVHWDeviceContext "
                    "(software upload mode active)");
    }
    if (!createYcbcrSamplerAndLayouts()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createRenderPassAndFramebuffers()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createGraphicsPipeline()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createInFlightRing()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createDescriptorPool()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!loadRenderTimePfns()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }

    // §J.3.e.2.i.3.e-SW — allocate upload buffer + image when in software
    // mode.  Size from params.  We use the source resolution; if the
    // stream switches resolution mid-session we'd need to recreate, but
    // moonlight streams stay at requested resolution for the session.
    if (m_SwMode) {
        if (!createSwUploadResources(params->width, params->height)) {
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            teardown();
            return false;
        }
    }

    // §J.3.e.2.i.4 — FRUC compute pipeline (motion estimate + median filter
    // + warp).  Independent of SW vs HW path; runs on storage buffers in
    // either mode.  Only allocate when m_FrucMode is set (env-var gated).
    if (m_FrucMode) {
        if (!createFrucComputeResources(params->width, params->height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4 init failed — disabling FRUC for "
                        "this session (renderer keeps running without compute chain)");
            // Don't fail init; just disable FRUC for this session.
            m_FrucMode = false;
            m_DualMode = false;
        }
    }

    // §J.3.e.2.i.4.2 / i.5 — interp graphics pipeline (depends on FRUC
    // compute being ready, since it samples m_FrucInterpRgbBuf).
    if (m_DualMode && m_FrucMode) {
        if (!createInterpGraphicsPipeline()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 interp graphics pipeline init "
                        "failed — disabling dual-present");
            m_DualMode = false;
        }
    } else if (m_DualMode && !m_FrucMode) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 VIPLE_VKFRUC_DUAL=1 requires "
                    "VIPLE_VKFRUC_FRUC=1 — disabling dual-present");
        m_DualMode = false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e initialize SUCCESS: instance/PD/device/"
                "swapchain + %s + ycbcr-sampler+layouts + render pass + graphics pipeline"
                " + in-flight ring + descriptor pool + render-time PFNs%s"
                " — renderFrame is now live",
                m_SwMode ? "SW upload buffer/image (no AVHWDeviceContext)"
                         : "AVHWDeviceContext",
                m_SwMode ? " (SW mode: AV_PIX_FMT_NV12 from CPU memory)"
                         : " (HW mode: AV_PIX_FMT_VULKAN)");

    return true;
}

// §J.3.e.2.i.3.c — pre-compiled SPIR-V for fullscreen-triangle vertex
// shader + NV12 ycbcr-sampler fragment shader.  Sources live in
// vkfruc-shaders/vkfruc.{vert,frag}; build_shaders.cmd compiles them via
// glslc (Android NDK / Vulkan SDK) into .spv + .spv.h.
//
// Pattern matches moonlight-android (.spv.h files are checked-in so end
// users do NOT need to run build_shaders.cmd).  ncnn::compile_spirv_module
// can't be reused here — it's hardcoded to compute stage; pre-compile
// avoids vendoring glslang into the renderer.
#include "vkfruc-shaders/vkfruc.vert.spv.h"
#include "vkfruc-shaders/vkfruc.frag.spv.h"
#include "vkfruc-shaders/vkfruc_interp.frag.spv.h"

bool VkFrucRenderer::createRenderPassAndFramebuffers()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateRenderPass = (PFN_vkCreateRenderPass)pfnGetDeviceProcAddr(
        m_Device, "vkCreateRenderPass");
    auto pfnCreateFB = (PFN_vkCreateFramebuffer)pfnGetDeviceProcAddr(
        m_Device, "vkCreateFramebuffer");
    if (!pfnCreateRenderPass || !pfnCreateFB) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c PFN load (renderpass) failed");
        return false;
    }

    VkAttachmentDescription colorAttach = {};
    colorAttach.format = m_SwapchainFormat;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // Subpass dependency: external → subpass 0, ensuring the swapchain
    // image acquire has happened before color attachment write.
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCi = {};
    rpCi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCi.attachmentCount = 1;
    rpCi.pAttachments = &colorAttach;
    rpCi.subpassCount = 1;
    rpCi.pSubpasses = &subpass;
    rpCi.dependencyCount = 1;
    rpCi.pDependencies = &dep;

    if (pfnCreateRenderPass(m_Device, &rpCi, nullptr, &m_RenderPass) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateRenderPass failed");
        return false;
    }

    m_Framebuffers.resize(m_SwapchainViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_SwapchainViews.size(); i++) {
        VkFramebufferCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = m_RenderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &m_SwapchainViews[i];
        fci.width = m_SwapchainExtent.width;
        fci.height = m_SwapchainExtent.height;
        fci.layers = 1;
        if (pfnCreateFB(m_Device, &fci, nullptr, &m_Framebuffers[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateFramebuffer #%zu failed", i);
            return false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.c render pass + %zu framebuffers ready",
                m_Framebuffers.size());
    return true;
}

void VkFrucRenderer::destroyRenderPassAndFramebuffers()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyFB = (PFN_vkDestroyFramebuffer)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyFramebuffer");
    auto pfnDestroyRP = (PFN_vkDestroyRenderPass)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyRenderPass");
    for (auto fb : m_Framebuffers) {
        if (fb && pfnDestroyFB) pfnDestroyFB(m_Device, fb, nullptr);
    }
    m_Framebuffers.clear();
    if (m_RenderPass && pfnDestroyRP) {
        pfnDestroyRP(m_Device, m_RenderPass, nullptr);
        m_RenderPass = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::createGraphicsPipeline()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkCreateShaderModule");
    auto pfnCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)pfnGetDeviceProcAddr(
        m_Device, "vkCreateGraphicsPipelines");
    auto pfnDestroyShaderModule = (PFN_vkDestroyShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyShaderModule");
    if (!pfnCreateShaderModule || !pfnCreateGraphicsPipelines || !pfnDestroyShaderModule) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c PFN load (pipeline) failed");
        return false;
    }

    // §J.3.e.2.i.3.c — load pre-compiled SPIR-V.  xxd emits unsigned char
    // arrays; SPIR-V is little-endian uint32 and the byte layout in the
    // .spv file matches little-endian uint32 directly, so reinterpret_cast
    // is fine on every supported target.  The _len constant is the size
    // in BYTES (must be multiple of 4 — guaranteed by glslc).
    auto buildShader = [&](const char* tag,
                           const unsigned char* spv,
                           unsigned int spvLen,
                           VkShaderModule& outMod) -> bool {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spvLen;
        smCi.pCode = reinterpret_cast<const uint32_t*>(spv);
        VkResult vr = pfnCreateShaderModule(m_Device, &smCi, nullptr, &outMod);
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateShaderModule(%s) "
                         "failed (%d)", tag, (int)vr);
            return false;
        }
        return true;
    };

    if (!buildShader("vert", vkfruc_vert_spv, vkfruc_vert_spv_len, m_VertShaderModule)) {
        return false;
    }
    if (!buildShader("frag", vkfruc_frag_spv, vkfruc_frag_spv_len, m_FragShaderModule)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertShaderModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_FragShaderModule;
    stages[1].pName  = "main";

    // No vertex buffer — fullscreen triangle uses gl_VertexIndex only.
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)m_SwapchainExtent.width;
    viewport.height   = (float)m_SwapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = m_SwapchainExtent;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports    = &viewport;
    vp.scissorCount  = 1;
    vp.pScissors     = &scissor;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;          // 3-vertex cover triangle, no culling
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount  = 1;
    cb.pAttachments     = &cba;

    // Dynamic state: viewport + scissor so swapchain resize doesn't force
    // pipeline recreate.  i.3.c-first-ship will use static viewport above
    // (resize handled by destroying & re-creating swapchain + pipeline);
    // i.6+ may switch to fully dynamic.  For now keep it simple — declare
    // no dynamic state.

    VkGraphicsPipelineCreateInfo gpCi = {};
    gpCi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCi.stageCount          = 2;
    gpCi.pStages             = stages;
    gpCi.pVertexInputState   = &vi;
    gpCi.pInputAssemblyState = &ia;
    gpCi.pViewportState      = &vp;
    gpCi.pRasterizationState = &rs;
    gpCi.pMultisampleState   = &ms;
    gpCi.pColorBlendState    = &cb;
    gpCi.layout              = m_GraphicsPipelineLayout;
    gpCi.renderPass          = m_RenderPass;
    gpCi.subpass             = 0;

    VkResult vr = pfnCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE,
                                             1, &gpCi, nullptr,
                                             &m_GraphicsPipeline);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateGraphicsPipelines "
                     "failed (%d)", (int)vr);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.c graphics pipeline ready "
                "(vert=%u B, frag=%u B SPIR-V)",
                (unsigned)vkfruc_vert_spv_len,
                (unsigned)vkfruc_frag_spv_len);
    return true;
}

void VkFrucRenderer::destroyGraphicsPipeline()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyShader = (PFN_vkDestroyShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyShaderModule");
    auto pfnDestroyPipeline = (PFN_vkDestroyPipeline)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyPipeline");
    if (m_GraphicsPipeline && pfnDestroyPipeline) {
        pfnDestroyPipeline(m_Device, m_GraphicsPipeline, nullptr);
        m_GraphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_FragShaderModule && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_FragShaderModule, nullptr);
        m_FragShaderModule = VK_NULL_HANDLE;
    }
    if (m_VertShaderModule && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_VertShaderModule, nullptr);
        m_VertShaderModule = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.4.2 / §J.3.e.2.i.5 — second graphics pipeline that displays
// m_FrucInterpRgbBuf (planar fp32 RGB storage buffer) via a fragment
// shader that reads the buffer directly.  Reuses the same vert shader as
// the real-frame pipeline.
//
// Different from m_GraphicsPipeline because:
//   • Different DSL: storage buffer (binding 0) instead of combined image
//     sampler with immutable ycbcr conversion
//   • Different push constant range (16 bytes: srcW + srcH)
//   • Different fragment shader (vkfruc_interp.frag)
bool VkFrucRenderer::createInterpGraphicsPipeline()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkCreateShaderModule");
    auto pfnCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)pfnGetDeviceProcAddr(
        m_Device, "vkCreateGraphicsPipelines");
    auto pfnCreateDsl = (PFN_vkCreateDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePL = (PFN_vkCreatePipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreatePipelineLayout");
    auto pfnCreateDescPool = (PFN_vkCreateDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDescSets = (PFN_vkAllocateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets = (PFN_vkUpdateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkUpdateDescriptorSets");
    if (!pfnCreateShaderModule || !pfnCreateGraphicsPipelines || !pfnCreateDsl
        || !pfnCreatePL || !pfnCreateDescPool || !pfnAllocDescSets || !pfnUpdateDescSets) {
        return false;
    }

    // ---- Create interp frag shader module ----
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = vkfruc_interp_frag_spv_len;
        smCi.pCode = reinterpret_cast<const uint32_t*>(vkfruc_interp_frag_spv);
        if (pfnCreateShaderModule(m_Device, &smCi, nullptr, &m_InterpFragShaderMod) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 vkCreateShaderModule(interp frag) failed");
            return false;
        }
    }

    // ---- DSL: 1 storage buffer at binding 0 (interpRGB) ----
    VkDescriptorSetLayoutBinding dslB = {};
    dslB.binding = 0;
    dslB.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dslB.descriptorCount = 1;
    dslB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 1;
    dslCi.pBindings = &dslB;
    if (pfnCreateDsl(m_Device, &dslCi, nullptr, &m_InterpDescSetLayout) != VK_SUCCESS) return false;

    // ---- Pipeline layout with 16-byte push const (srcW, srcH) ----
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = 16;
    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &m_InterpDescSetLayout;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges = &pcRange;
    if (pfnCreatePL(m_Device, &plCi, nullptr, &m_InterpPipelineLayout) != VK_SUCCESS) return false;

    // ---- Graphics pipeline (reuses vert shader from m_GraphicsPipeline) ----
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertShaderModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_InterpFragShaderMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {};
    viewport.width    = (float)m_SwapchainExtent.width;
    viewport.height   = (float)m_SwapchainExtent.height;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = { {0,0}, m_SwapchainExtent };
    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports    = &viewport;
    vp.scissorCount  = 1;
    vp.pScissors     = &scissor;
    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount  = 1;
    cb.pAttachments     = &cba;

    VkGraphicsPipelineCreateInfo gpCi = {};
    gpCi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCi.stageCount          = 2;
    gpCi.pStages             = stages;
    gpCi.pVertexInputState   = &vi;
    gpCi.pInputAssemblyState = &ia;
    gpCi.pViewportState      = &vp;
    gpCi.pRasterizationState = &rs;
    gpCi.pMultisampleState   = &ms;
    gpCi.pColorBlendState    = &cb;
    gpCi.layout              = m_InterpPipelineLayout;
    gpCi.renderPass          = m_RenderPass;  // shared render pass
    gpCi.subpass             = 0;
    if (pfnCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &gpCi, nullptr,
                                    &m_InterpPipeline) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 vkCreateGraphicsPipelines(interp) failed");
        return false;
    }

    // ---- Descriptor pool + set bound to interpRGB ----
    VkDescriptorPoolSize ps = {};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 1;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &ps;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_InterpDescPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo asi = {};
    asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    asi.descriptorPool = m_InterpDescPool;
    asi.descriptorSetCount = 1;
    asi.pSetLayouts = &m_InterpDescSetLayout;
    if (pfnAllocDescSets(m_Device, &asi, &m_InterpDescSet) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bi = {};
    bi.buffer = m_FrucInterpRgbBuf;   // bound by FRUC compute init
    bi.offset = 0;
    bi.range  = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wds = {};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = m_InterpDescSet;
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds.pBufferInfo     = &bi;
    pfnUpdateDescSets(m_Device, 1, &wds, 0, nullptr);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 interp pipeline ready (frag=%u B SPIR-V)",
                (unsigned)vkfruc_interp_frag_spv_len);
    return true;
}

void VkFrucRenderer::destroyInterpGraphicsPipeline()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyShader   = (PFN_vkDestroyShaderModule)getDevPa(m_Device, "vkDestroyShaderModule");
    auto pfnDestroyPipeline = (PFN_vkDestroyPipeline)getDevPa(m_Device, "vkDestroyPipeline");
    auto pfnDestroyPL       = (PFN_vkDestroyPipelineLayout)getDevPa(m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl      = (PFN_vkDestroyDescriptorSetLayout)getDevPa(m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroyDescPool = (PFN_vkDestroyDescriptorPool)getDevPa(m_Device, "vkDestroyDescriptorPool");
    if (m_InterpDescPool && pfnDestroyDescPool) {
        pfnDestroyDescPool(m_Device, m_InterpDescPool, nullptr);
        m_InterpDescPool = VK_NULL_HANDLE;
        m_InterpDescSet  = VK_NULL_HANDLE;
    }
    if (m_InterpPipeline && pfnDestroyPipeline) {
        pfnDestroyPipeline(m_Device, m_InterpPipeline, nullptr);
        m_InterpPipeline = VK_NULL_HANDLE;
    }
    if (m_InterpPipelineLayout && pfnDestroyPL) {
        pfnDestroyPL(m_Device, m_InterpPipelineLayout, nullptr);
        m_InterpPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_InterpDescSetLayout && pfnDestroyDsl) {
        pfnDestroyDsl(m_Device, m_InterpDescSetLayout, nullptr);
        m_InterpDescSetLayout = VK_NULL_HANDLE;
    }
    if (m_InterpFragShaderMod && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_InterpFragShaderMod, nullptr);
        m_InterpFragShaderMod = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.3.d — per-slot in-flight ring.  Mirrors Android's
// init_in_flight_ring (vk_backend.c:2791): one VkCommandPool with
// RESET_COMMAND_BUFFER_BIT, kFrucFramesInFlight cmd buffers allocated
// from it, and per-slot semaphore pairs + a signaled-initial fence.
//
// The fence is created VK_FENCE_CREATE_SIGNALED_BIT so the very first
// renderFrame() can vkWaitForFences without blocking (slot 0 has no
// prior submit to wait on).
bool VkFrucRenderer::createInFlightRing()
{
    if (m_RingInitialized) return true;

    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateCmdPool = (PFN_vkCreateCommandPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateCommandPool");
    auto pfnAllocCmdBufs = (PFN_vkAllocateCommandBuffers)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateCommandBuffers");
    auto pfnCreateSem = (PFN_vkCreateSemaphore)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSemaphore");
    auto pfnCreateFence = (PFN_vkCreateFence)pfnGetDeviceProcAddr(
        m_Device, "vkCreateFence");
    if (!pfnCreateCmdPool || !pfnAllocCmdBufs || !pfnCreateSem || !pfnCreateFence) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d PFN load (ring) failed");
        return false;
    }

    VkCommandPoolCreateInfo pci = {};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_QueueFamily;
    if (pfnCreateCmdPool(m_Device, &pci, nullptr, &m_CmdPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = m_CmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFrucFramesInFlight;
    if (pfnAllocCmdBufs(m_Device, &cbai, m_SlotCmdBuf) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkAllocateCommandBuffers failed");
        return false;
    }

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        for (int p = 0; p < 2; p++) {
            if (pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotAcquireSem[i][p]) != VK_SUCCESS ||
                pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotRenderDoneSem[i][p]) != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateSemaphore[%u][%d] failed",
                             i, p);
                return false;
            }
        }
        if (pfnCreateFence(m_Device, &fci, nullptr,
                           &m_SlotInFlightFence[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateFence[%u] failed", i);
            return false;
        }
    }

    m_CurrentSlot = 0;
    m_RingInitialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.d in-flight ring ready: %u slots × "
                "(1 cmdbuf + 2 acquireSem + 2 renderDoneSem + 1 fence)",
                (unsigned)kFrucFramesInFlight);
    return true;
}

void VkFrucRenderer::destroyInFlightRing()
{
    if (!m_RingInitialized || m_Device == VK_NULL_HANDLE) {
        // Even if !initialized, drop the cmd pool if it leaked half-way through
        // createInFlightRing failure.
    }

    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroySem = (PFN_vkDestroySemaphore)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySemaphore");
    auto pfnDestroyFence = (PFN_vkDestroyFence)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyFence");
    auto pfnDestroyCmdPool = (PFN_vkDestroyCommandPool)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyCommandPool");

    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        if (m_SlotInFlightFence[i] && pfnDestroyFence) {
            pfnDestroyFence(m_Device, m_SlotInFlightFence[i], nullptr);
            m_SlotInFlightFence[i] = VK_NULL_HANDLE;
        }
        for (int p = 0; p < 2; p++) {
            if (m_SlotAcquireSem[i][p] && pfnDestroySem) {
                pfnDestroySem(m_Device, m_SlotAcquireSem[i][p], nullptr);
                m_SlotAcquireSem[i][p] = VK_NULL_HANDLE;
            }
            if (m_SlotRenderDoneSem[i][p] && pfnDestroySem) {
                pfnDestroySem(m_Device, m_SlotRenderDoneSem[i][p], nullptr);
                m_SlotRenderDoneSem[i][p] = VK_NULL_HANDLE;
            }
        }
        // Cmd buffers freed implicitly when cmd pool is destroyed below.
        m_SlotCmdBuf[i] = VK_NULL_HANDLE;
    }
    if (m_CmdPool && pfnDestroyCmdPool) {
        pfnDestroyCmdPool(m_Device, m_CmdPool, nullptr);
        m_CmdPool = VK_NULL_HANDLE;
    }
    m_RingInitialized = false;
}

// §J.3.e.2.i.3.b — VkSamplerYcbcrConversion + VkSampler + descriptor
// set layout + pipeline layout.  Maps NV12 (G8_B8R8_2PLANE_420_UNORM)
// to a single combined image sampler binding the fragment shader sees
// as `sampler2D` returning RGB (driver-side YCbCr→RGB conversion).
//
// Algorithm choice: BT.709 narrow range (matches Sunshine / standard
// streaming output).  10-bit (P010 → G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16)
// deferred to §J.3.e.2.j HDR.
bool VkFrucRenderer::createYcbcrSamplerAndLayouts()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateYcbcrConv = (PFN_vkCreateSamplerYcbcrConversion)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSamplerYcbcrConversion");
    auto pfnCreateSampler = (PFN_vkCreateSampler)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSampler");
    auto pfnCreateDsl = (PFN_vkCreateDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePL = (PFN_vkCreatePipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreatePipelineLayout");
    if (!pfnCreateYcbcrConv || !pfnCreateSampler || !pfnCreateDsl || !pfnCreatePL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b PFN load failed");
        return false;
    }

    VkSamplerYcbcrConversionCreateInfo yci = {};
    yci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    yci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
    yci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    yci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    yci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    yci.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    yci.chromaFilter = VK_FILTER_LINEAR;
    yci.forceExplicitReconstruction = VK_FALSE;
    if (pfnCreateYcbcrConv(m_Device, &yci, nullptr, &m_YcbcrConversion) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateSamplerYcbcrConversion failed");
        return false;
    }

    VkSamplerYcbcrConversionInfo ycbInfo = {};
    ycbInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbInfo.conversion = m_YcbcrConversion;

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.pNext = &ycbInfo;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxAnisotropy = 1.0f;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (pfnCreateSampler(m_Device, &sci, nullptr, &m_YcbcrSampler) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateSampler failed");
        return false;
    }

    // Descriptor set layout: 1 combined image sampler with IMMUTABLE
    // sampler (required when using ycbcr conversion — driver bakes
    // conversion into the descriptor).
    VkDescriptorSetLayoutBinding dslBinding = {};
    dslBinding.binding = 0;
    dslBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslBinding.descriptorCount = 1;
    dslBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dslBinding.pImmutableSamplers = &m_YcbcrSampler;

    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 1;
    dslCi.pBindings = &dslBinding;
    if (pfnCreateDsl(m_Device, &dslCi, nullptr, &m_DescSetLayout) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &m_DescSetLayout;
    if (pfnCreatePL(m_Device, &plCi, nullptr, &m_GraphicsPipelineLayout) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreatePipelineLayout failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.b ycbcr sampler+layouts ready "
                "(format=NV12, model=BT709, range=narrow, magFilter=linear)");
    return true;
}

void VkFrucRenderer::destroyYcbcrSamplerAndLayouts()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyPL = (PFN_vkDestroyPipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl = (PFN_vkDestroyDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroySampler = (PFN_vkDestroySampler)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySampler");
    auto pfnDestroyYcbcrConv = (PFN_vkDestroySamplerYcbcrConversion)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySamplerYcbcrConversion");
    if (m_GraphicsPipelineLayout && pfnDestroyPL) {
        pfnDestroyPL(m_Device, m_GraphicsPipelineLayout, nullptr);
        m_GraphicsPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_DescSetLayout && pfnDestroyDsl) {
        pfnDestroyDsl(m_Device, m_DescSetLayout, nullptr);
        m_DescSetLayout = VK_NULL_HANDLE;
    }
    if (m_YcbcrSampler && pfnDestroySampler) {
        pfnDestroySampler(m_Device, m_YcbcrSampler, nullptr);
        m_YcbcrSampler = VK_NULL_HANDLE;
    }
    if (m_YcbcrConversion && pfnDestroyYcbcrConv) {
        pfnDestroyYcbcrConv(m_Device, m_YcbcrConversion, nullptr);
        m_YcbcrConversion = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.3.a — populate AVVulkanDeviceContext with our handles so
// ffmpeg's Vulkan video decoder builds AVVkFrame on OUR VkDevice.
// Mirrors PlVkRenderer::populateQueues + the AVHWDeviceContext setup
// block in PlVkRenderer::completeInitialization.
bool VkFrucRenderer::populateAvHwDeviceCtx(int videoFormat)
{
    auto pfnGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!pfnGetQFP) return false;

    m_HwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (m_HwDeviceCtx == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.a av_hwdevice_ctx_alloc(VULKAN) failed");
        return false;
    }
    auto hwDeviceContext = (AVHWDeviceContext*)m_HwDeviceCtx->data;
    hwDeviceContext->user_opaque = this;

    auto vkCtx = (AVVulkanDeviceContext*)hwDeviceContext->hwctx;
    vkCtx->get_proc_addr = m_pfnGetInstanceProcAddr;
    vkCtx->inst          = m_Instance;
    vkCtx->phys_dev      = m_PhysicalDevice;
    vkCtx->act_dev       = m_Device;

    // §J.3.e.2.i.3.a CRITICAL — device_features must reflect EXACTLY what
    // we enabled at vkCreateDevice (ffmpeg/NV driver dereferences feature
    // state from this struct's pNext chain to find timeline_semaphore /
    // synchronization2 / sampler_ycbcr_conversion state objects).  If any
    // are missing or NULL, NV's nvoglv64 internal state at offset 0xF0
    // is NULL → crash on decoder thread (v1.3.123-132 root cause; see
    // doc/J.3.e.2.i_vulkan_native_renderer.md).
    //
    // m_DevFeat2 / m_YcbcrFeat / m_TimelineFeat / m_Sync2Feat are
    // PERSISTENT MEMBERS (not stack-allocated).  Copying device_features
    // is a struct copy of VkPhysicalDeviceFeatures2 BUT pNext is a raw
    // pointer that survives the copy and points back into our member
    // chain — exactly what FFmpeg needs.
    vkCtx->device_features = m_DevFeat2;

    // Extension list: same minimal set we built device with.  ffmpeg
    // uses this to filter what it can request.
    static const char* kInstExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef Q_OS_WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };
    static const char* kDevExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
#if LIBAVCODEC_VERSION_MAJOR >= 61
        VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
#endif
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };
    vkCtx->enabled_inst_extensions    = kInstExts;
    vkCtx->nb_enabled_inst_extensions = (int)(sizeof(kInstExts) / sizeof(kInstExts[0]));
    vkCtx->enabled_dev_extensions     = kDevExts;
    vkCtx->nb_enabled_dev_extensions  = (int)(sizeof(kDevExts) / sizeof(kDevExts[0]));

#if LIBAVUTIL_VERSION_INT > AV_VERSION_INT(58, 9, 100)
    vkCtx->lock_queue   = lockQueueStub;
    vkCtx->unlock_queue = unlockQueueStub;
#endif

    // Queue family info — libavutil 60 (FFmpeg 8 master) STILL has
    // FF_API_VULKAN_FIXED_QUEUES=1, so the deprecated old-style discrete
    // fields (queue_family_*_index, nb_*_queues) are still part of the
    // struct AND still accessed by FFmpeg's internal code paths.  Fill
    // BOTH old AND new style to avoid zero-init values being misused.
    // (v1.3.123-134 crash root cause: only new-style qf[] was filled,
    // old fields zero → FFmpeg internal queue lookup uses queue family 0
    // for decode-only ops → NV driver NULL deref via uninitialised
    // dispatch state).
    uint32_t qfCount = 0;
    pfnGetQFP(m_PhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    pfnGetQFP(m_PhysicalDevice, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        vkCtx->qf[i].idx        = i;
        vkCtx->qf[i].num        = qfs[i].queueCount;
        vkCtx->qf[i].flags      = (VkQueueFlagBits)qfs[i].queueFlags;
        vkCtx->qf[i].video_caps = (VkVideoCodecOperationFlagBitsKHR)0;  // ffmpeg re-probes
    }
    vkCtx->nb_qf = qfCount;
#if FF_API_VULKAN_FIXED_QUEUES
    // Mirror our actual queue layout into the deprecated fields.
    vkCtx->queue_family_index        = (int)m_QueueFamily;
    vkCtx->nb_graphics_queues        = 1;
    vkCtx->queue_family_tx_index     = (int)m_QueueFamily;
    vkCtx->nb_tx_queues              = 1;
    vkCtx->queue_family_comp_index   = (int)m_QueueFamily;
    vkCtx->nb_comp_queues            = 1;
    vkCtx->queue_family_encode_index = -1;
    vkCtx->nb_encode_queues          = 0;
    vkCtx->queue_family_decode_index = (int)m_DecodeQueueFamily;
    vkCtx->nb_decode_queues          = (int)m_DecodeQueueCount;
#endif
    (void)videoFormat;

    int err = av_hwdevice_ctx_init(m_HwDeviceCtx);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.a av_hwdevice_ctx_init failed: %d", err);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.a AVHWDeviceContext init OK "
                "(QF gfx=%u decode=%u, %u queue families wired)",
                m_QueueFamily, m_DecodeQueueFamily, qfCount);
    return true;
}

bool VkFrucRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    (void)options;
    // §J.3.e.2.i.3.e-SW — software-upload mode: no hwaccel binding, ffmpeg
    // decodes in CPU into AV_PIX_FMT_NV12 frames that we upload via our
    // staging buffer.
    if (m_SwMode) {
        (void)context;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW prepareDecoderContext OK "
                    "(software decode path; no hw_device_ctx binding)");
        return true;
    }
    if (m_HwDeviceCtx == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.a prepareDecoderContext called "
                    "without m_HwDeviceCtx ready");
        return false;
    }
    context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.a prepareDecoderContext OK — ffmpeg "
                "will decode into AVVkFrame on our VkDevice");
    return true;
}

// §J.3.e.2.i.3.e — descriptor pool sized for kFrucFramesInFlight
// COMBINED_IMAGE_SAMPLER bindings (one per ring slot).  We pre-allocate
// the descriptor sets at init and re-update each frame to reference the
// new AVVkFrame's image view.
bool VkFrucRenderer::createDescriptorPool()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateDP = (PFN_vkCreateDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDS = (PFN_vkAllocateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateDescriptorSets");
    if (!pfnCreateDP || !pfnAllocDS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e descriptor PFN load failed");
        return false;
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kFrucFramesInFlight;

    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets       = kFrucFramesInFlight;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes    = &poolSize;
    if (pfnCreateDP(m_Device, &dpCi, nullptr, &m_DescPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetLayout layouts[kFrucFramesInFlight];
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        layouts[i] = m_DescSetLayout;
    }
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_DescPool;
    dsai.descriptorSetCount = kFrucFramesInFlight;
    dsai.pSetLayouts        = layouts;
    if (pfnAllocDS(m_Device, &dsai, m_SlotDescSet) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e vkAllocateDescriptorSets failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e descriptor pool + %u sets ready",
                (unsigned)kFrucFramesInFlight);
    return true;
}

void VkFrucRenderer::destroyDescriptorPool()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyDP = (PFN_vkDestroyDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyDescriptorPool");
    auto pfnDestroyView = (PFN_vkDestroyImageView)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyImageView");

    // Free any pending image views (per-slot views from the last in-flight
    // frame).  Caller has already drained the queue via vkDeviceWaitIdle
    // (in teardown sequence) so these are safe to destroy.
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        if (m_SlotPendingView[i] && pfnDestroyView) {
            pfnDestroyView(m_Device, m_SlotPendingView[i], nullptr);
            m_SlotPendingView[i] = VK_NULL_HANDLE;
        }
        m_SlotDescSet[i] = VK_NULL_HANDLE;  // freed implicitly with pool
    }
    if (m_DescPool && pfnDestroyDP) {
        pfnDestroyDP(m_Device, m_DescPool, nullptr);
        m_DescPool = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.3.e — cache hot-path PFNs to avoid resolving via
// vkGetDeviceProcAddr on every renderFrame call (~120 fps in worst case).
bool VkFrucRenderer::loadRenderTimePfns()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnGetDeviceProcAddr) return false;

#define LOAD_RT(name)                                                          \
    m_RtPfn.name = (PFN_vk##name)pfnGetDeviceProcAddr(m_Device, "vk" #name);   \
    if (!m_RtPfn.name) {                                                       \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,                             \
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e PFN miss: vk" #name);     \
        return false;                                                          \
    }

    LOAD_RT(AcquireNextImageKHR);
    LOAD_RT(QueuePresentKHR);
    LOAD_RT(QueueSubmit);
    LOAD_RT(BeginCommandBuffer);
    LOAD_RT(EndCommandBuffer);
    LOAD_RT(ResetCommandBuffer);
    LOAD_RT(CmdPipelineBarrier);
    LOAD_RT(CmdBeginRenderPass);
    LOAD_RT(CmdEndRenderPass);
    LOAD_RT(CmdBindPipeline);
    LOAD_RT(CmdBindDescriptorSets);
    LOAD_RT(CmdDraw);
    LOAD_RT(WaitForFences);
    LOAD_RT(ResetFences);
    LOAD_RT(UpdateDescriptorSets);
    LOAD_RT(CreateImageView);
    LOAD_RT(DestroyImageView);
    LOAD_RT(CmdCopyBufferToImage);

#undef LOAD_RT
    return true;
}

// §J.3.e.2.i.3.e-SW — staging buffer + multi-plane NV12 VkImage for the
// software-decode upload path.  Allocates ONCE for the source resolution
// and reuses the image+buffer per-frame (Y/UV memcpy + cmdCopyBufferToImage).
bool VkFrucRenderer::createSwUploadResources(int width, int height)
{
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem    = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem  = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnMapMem      = (PFN_vkMapMemory)getDevPa(m_Device, "vkMapMemory");
    auto pfnCreateImage = (PFN_vkCreateImage)getDevPa(m_Device, "vkCreateImage");
    auto pfnGetImgMemReq = (PFN_vkGetImageMemoryRequirements)getDevPa(m_Device, "vkGetImageMemoryRequirements");
    auto pfnBindImgMem  = (PFN_vkBindImageMemory)getDevPa(m_Device, "vkBindImageMemory");
    auto pfnGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem ||
        !pfnMapMem || !pfnCreateImage || !pfnGetImgMemReq || !pfnBindImgMem ||
        !pfnGetMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW PFN load failed");
        return false;
    }

    m_SwImageWidth  = width;
    m_SwImageHeight = height;
    // NV12: Y plane = w×h bytes, UV plane = (w×h)/2 bytes (interleaved at half height)
    m_SwStagingSize = (size_t)width * height * 3 / 2;

    VkPhysicalDeviceMemoryProperties memProps;
    pfnGetMemProps(m_PhysicalDevice, &memProps);
    auto findMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags wanted) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return (int)i;
            }
        }
        return -1;
    };

    // Staging buffer: host-visible coherent.  STORAGE_BUFFER usage added
    // (in addition to TRANSFER_SRC) so §J.3.e.2.i.4.1 NV12→RGB compute can
    // sample raw NV12 bytes via storage buffer descriptor with Y at offset
    // 0 and UV at offset W*H — avoids an extra image→buffer copy step.
    {
        VkBufferCreateInfo bci = {};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = m_SwStagingSize;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &m_SwStagingBuffer) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateBuffer failed");
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, m_SwStagingBuffer, &mr);
        int memTypeIdx = findMemType(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memTypeIdx < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW no host-visible+coherent memory type");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)memTypeIdx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_SwStagingMem) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkAllocateMemory(staging) failed");
            return false;
        }
        if (pfnBindBufMem(m_Device, m_SwStagingBuffer, m_SwStagingMem, 0) != VK_SUCCESS ||
            pfnMapMem(m_Device, m_SwStagingMem, 0, VK_WHOLE_SIZE, 0, &m_SwStagingMapped) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW staging bind/map failed");
            return false;
        }
    }

    // Upload image: NV12 multi-plane, sampled + transfer-dst
    {
        VkImageCreateInfo ici = {};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ici.extent.width  = (uint32_t)width;
        ici.extent.height = (uint32_t)height;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (pfnCreateImage(m_Device, &ici, nullptr, &m_SwUploadImage) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateImage failed");
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetImgMemReq(m_Device, m_SwUploadImage, &mr);
        int memTypeIdx = findMemType(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memTypeIdx < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW no device-local memory type");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)memTypeIdx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_SwUploadImageMem) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkAllocateMemory(image) failed");
            return false;
        }
        if (pfnBindImgMem(m_Device, m_SwUploadImage, m_SwUploadImageMem, 0) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkBindImageMemory failed");
            return false;
        }
    }

    // Image view with our YCbCr conversion baked in (matches descriptor
    // set layout's immutable sampler).  Built once, reused for all frames.
    {
        auto pfnCreateView = (PFN_vkCreateImageView)getDevPa(m_Device, "vkCreateImageView");
        VkSamplerYcbcrConversionInfo convInfo = {};
        convInfo.sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        convInfo.conversion = m_YcbcrConversion;

        VkImageViewCreateInfo vci = {};
        vci.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.pNext      = &convInfo;
        vci.image      = m_SwUploadImage;
        vci.viewType   = VK_IMAGE_VIEW_TYPE_2D;
        vci.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.layerCount     = 1;
        if (pfnCreateView(m_Device, &vci, nullptr, &m_SwUploadView) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateImageView failed");
            return false;
        }
    }

    // Descriptor sets — both ring slots' descriptor sets point at the
    // same upload image view (we don't need per-slot views since the
    // image is single-buffered and reused).  Update once.
    {
        VkDescriptorImageInfo dii = {};
        dii.imageView   = m_SwUploadView;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds[kFrucFramesInFlight] = {};
        for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
            wds[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet          = m_SlotDescSet[i];
            wds[i].dstBinding      = 0;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds[i].pImageInfo      = &dii;
        }
        m_RtPfn.UpdateDescriptorSets(m_Device, kFrucFramesInFlight, wds, 0, nullptr);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW upload resources ready: "
                "%dx%d NV12, staging=%zu B, image=DEVICE_LOCAL",
                width, height, m_SwStagingSize);
    return true;
}

void VkFrucRenderer::destroySwUploadResources()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyView   = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnDestroyImage  = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem       = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnUnmapMem      = (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory");

    if (m_SwUploadView && pfnDestroyView) {
        pfnDestroyView(m_Device, m_SwUploadView, nullptr);
        m_SwUploadView = VK_NULL_HANDLE;
    }
    if (m_SwUploadImage && pfnDestroyImage) {
        pfnDestroyImage(m_Device, m_SwUploadImage, nullptr);
        m_SwUploadImage = VK_NULL_HANDLE;
    }
    if (m_SwUploadImageMem && pfnFreeMem) {
        pfnFreeMem(m_Device, m_SwUploadImageMem, nullptr);
        m_SwUploadImageMem = VK_NULL_HANDLE;
    }
    if (m_SwStagingMapped && pfnUnmapMem) {
        pfnUnmapMem(m_Device, m_SwStagingMem);
        m_SwStagingMapped = nullptr;
    }
    if (m_SwStagingBuffer && pfnDestroyBuffer) {
        pfnDestroyBuffer(m_Device, m_SwStagingBuffer, nullptr);
        m_SwStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_SwStagingMem && pfnFreeMem) {
        pfnFreeMem(m_Device, m_SwStagingMem, nullptr);
        m_SwStagingMem = VK_NULL_HANDLE;
    }
}

// =====================================================================
// §J.3.e.2.i.4 — FRUC compute pipeline integration
//
// Port of PlVkRenderer::initFrucGenericResources / runFrucGenericComputePass
// (plvk.cpp:3604-4180).  Builds 3 compute pipelines (motionest / mv_median /
// warp), allocates planar fp32 RGB buffers + MV buffers, runs the chain
// every frame after our SW upload.
//
// i.4 first ship — placeholder bufRGB pair (zeros), no NV12→RGB feed yet,
// no interp display.  Validates compute pipeline integration in our
// VkFrucRenderer.  Future expansions:
//   • i.4.1 — add NV12→RGB compute feed from m_SwUploadImage
//   • i.4.2 — display m_FrucInterpRgbBuf via dual-present (with §J.3.e.2.i.5)
//
// Shader sources (kFrucMotionEstShaderGlsl etc.) are defined in plvk.cpp;
// extern-declared here so we can compile them without copy/paste.
// =====================================================================

// Forward declarations — defined in plvk.cpp (external linkage; not
// `static`).  Use C++ linkage to match the definitions there.
extern const char* kFrucMotionEstShaderGlsl;
extern const char* kFrucMvMedianShaderGlsl;
extern const char* kFrucWarpShaderGlsl;

// §J.3.e.2.i.4.1 — NV12 → planar fp32 RGB compute shader.  Reuses the
// algorithm from PlVkRenderer's §J.3.e.2.c kNv12RgbShaderGlsl (BT.709
// limited-range YCbCr → linear sRGB), but with a SINGLE input storage
// buffer (m_SwStagingBuffer): binding 0 reads the whole staging buffer
// as uint array; we compute Y plane offsets directly (offset 0..W*H) and
// UV plane offsets (W*H..W*H*3/2).
//
// PlVkRenderer keeps that shader `static` — can't extern.  Inline copy
// here, simplified to single-buffer.
static const char* kVkFrucNv12RgbShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Single buffer holding NV12: Y plane (W*H bytes) + UV plane (W*H/2 bytes,
// interleaved at half-resolution).  Read as uint32 array (4 bytes/elem).
layout(binding = 0) readonly  buffer NV12_in { uint  data[]; } nv12;
layout(binding = 1) writeonly buffer RGB_out { float data[]; } rgbOut;
layout(push_constant) uniform Params { int w; int h; int uvByteOffset; int _pad; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;

    // Y at byte offset (y * w + x), within [0, w*h)
    int yByteIdx = y * p.w + x;
    uint yWord   = nv12.data[yByteIdx >> 2];
    uint yByte   = (yWord >> ((yByteIdx & 3) * 8)) & 0xFFu;
    float Y_raw  = float(yByte) * (1.0 / 255.0);

    // UV plane starts at byte offset uvByteOffset (= w*h)
    int chromaX = x >> 1;
    int chromaY = y >> 1;
    int uvByteI = p.uvByteOffset + (chromaY * (p.w >> 1) + chromaX) * 2;
    uint uvWord0 = nv12.data[uvByteI >> 2];
    uint uvWord1 = nv12.data[(uvByteI + 1) >> 2];
    uint cbByte  = (uvWord0 >> ((uvByteI       & 3) * 8)) & 0xFFu;
    uint crByte  = (uvWord1 >> (((uvByteI + 1) & 3) * 8)) & 0xFFu;
    float Cb_raw = float(cbByte) * (1.0 / 255.0);
    float Cr_raw = float(crByte) * (1.0 / 255.0);

    // BT.709 limited-range YCbCr → linear sRGB
    float Y_n  = (Y_raw  - 16.0  / 255.0) * (255.0 / 219.0);
    float Cb_n = (Cb_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float Cr_n = (Cr_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float r = clamp(Y_n + 1.5748 * Cr_n,                       0.0, 1.0);
    float g = clamp(Y_n - 0.1873 * Cb_n - 0.4681 * Cr_n,       0.0, 1.0);
    float b = clamp(Y_n + 1.8556 * Cb_n,                       0.0, 1.0);

    int outIdx    = y * p.w + x;
    int planeSize = p.w * p.h;
    rgbOut.data[outIdx + 0 * planeSize] = r;
    rgbOut.data[outIdx + 1 * planeSize] = g;
    rgbOut.data[outIdx + 2 * planeSize] = b;
}
)GLSL";

#include <ncnn/gpu.h>  // for ncnn::compile_spirv_module + ncnn::Option

bool VkFrucRenderer::createFrucComputeResources(int width, int height)
{
    if (m_FrucReady || m_FrucDisabled) return m_FrucReady;

    const uint32_t BLOCK_SIZE = 8;
    const uint32_t mvW = ((uint32_t)width  + BLOCK_SIZE - 1) / BLOCK_SIZE;
    const uint32_t mvH = ((uint32_t)height + BLOCK_SIZE - 1) / BLOCK_SIZE;
    m_FrucMvWidth  = mvW;
    m_FrucMvHeight = mvH;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: enter (W=%d H=%d block=%u mv=%ux%u)",
                width, height, BLOCK_SIZE, mvW, mvH);

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)getDevPa(m_Device, "vkCreateShaderModule");
    auto pfnCreateDsl          = (PFN_vkCreateDescriptorSetLayout)getDevPa(m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePipeLay      = (PFN_vkCreatePipelineLayout)getDevPa(m_Device, "vkCreatePipelineLayout");
    auto pfnCreateComputePipes = (PFN_vkCreateComputePipelines)getDevPa(m_Device, "vkCreateComputePipelines");
    auto pfnCreateBuffer       = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq       = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem           = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem         = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnCreateDescPool     = (PFN_vkCreateDescriptorPool)getDevPa(m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDescSets      = (PFN_vkAllocateDescriptorSets)getDevPa(m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets     = (PFN_vkUpdateDescriptorSets)getDevPa(m_Device, "vkUpdateDescriptorSets");
    auto pfnGetPdMemProps      = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateShaderModule || !pfnCreateDsl || !pfnCreatePipeLay || !pfnCreateComputePipes
        || !pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem
        || !pfnCreateDescPool || !pfnAllocDescSets || !pfnUpdateDescSets || !pfnGetPdMemProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 PFN load failed");
        m_FrucDisabled = true;
        return false;
    }

    // §J.3.e.2.i.4 — ncnn::compile_spirv_module needs ncnn's Vulkan context
    // initialised.  PlVkRenderer does this via create_gpu_instance_external
    // (sharing libplacebo's VkDevice).  We don't have libplacebo here; use
    // the plain create_gpu_instance() which creates ncnn's own context.
    // Idempotent: returns 0 if already initialised.
    int ncnnInit = ncnn::create_gpu_instance();
    if (ncnnInit == 0) {
        // Successfully created or already created (idempotent).  Track
        // refcount so destroyFrucComputeResources knows to call destroy
        // when the last renderer instance tears down.
        m_NcnnInited = true;
        s_NcnnRefCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 ncnn::create_gpu_instance failed rc=%d "
                    "(may already be claimed; compile may still work)", ncnnInit);
    }

    auto buildPipeline = [&](const char* tag, const char* glsl, int numBindings,
                              uint32_t pcSize,
                              VkShaderModule& outMod, VkDescriptorSetLayout& outDsl,
                              VkPipelineLayout& outPL, VkPipeline& outPipe) -> bool {
        std::vector<uint32_t> spirv;
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(glsl, opt, spirv) != 0 || spirv.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4 %s: compile_spirv_module failed", tag);
            return false;
        }
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(m_Device, &smCi, nullptr, &outMod) != VK_SUCCESS) return false;
        std::vector<VkDescriptorSetLayoutBinding> dslB(numBindings);
        for (int i = 0; i < numBindings; i++) {
            dslB[i].binding = (uint32_t)i;
            dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            dslB[i].descriptorCount = 1;
            dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = (uint32_t)numBindings;
        dslCi.pBindings = dslB.data();
        if (pfnCreateDsl(m_Device, &dslCi, nullptr, &outDsl) != VK_SUCCESS) return false;
        VkPushConstantRange pcRange = {};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.size = pcSize;
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &outDsl;
        plCi.pushConstantRangeCount = pcSize > 0 ? 1 : 0;
        plCi.pPushConstantRanges = pcSize > 0 ? &pcRange : nullptr;
        if (pfnCreatePipeLay(m_Device, &plCi, nullptr, &outPL) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = outMod;
        cpCi.stage.pName = "main";
        cpCi.layout = outPL;
        if (pfnCreateComputePipes(m_Device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &outPipe) != VK_SUCCESS) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 %s: pipeline built (spv=%zu B, %d bind, pc=%u B)",
                    tag, spirv.size() * sizeof(uint32_t), numBindings, pcSize);
        return true;
    };

    // §J.3.e.2.i.4.1 — NV12 → RGB compute pipeline (2 bindings, 16-byte
    // push const = w/h/uvByteOffset/_pad).
    if (!buildPipeline("NV12->RGB", kVkFrucNv12RgbShaderGlsl, 2, 16,
                        m_FrucNv12RgbShaderMod, m_FrucNv12RgbDsl,
                        m_FrucNv12RgbPipeLay, m_FrucNv12RgbPipeline)) {
        m_FrucDisabled = true; return false;
    }

    if (!buildPipeline("ME", kFrucMotionEstShaderGlsl, 4, 24,
                        m_FrucMeShaderMod, m_FrucMeDsl, m_FrucMePipeLay, m_FrucMePipeline)) {
        m_FrucDisabled = true; return false;
    }
    if (!buildPipeline("Median", kFrucMvMedianShaderGlsl, 2, 16,
                        m_FrucMedianShaderMod, m_FrucMedianDsl, m_FrucMedianPipeLay, m_FrucMedianPipeline)) {
        m_FrucDisabled = true; return false;
    }
    if (!buildPipeline("Warp", kFrucWarpShaderGlsl, 4, 24,
                        m_FrucWarpShaderMod, m_FrucWarpDsl, m_FrucWarpPipeLay, m_FrucWarpPipeline)) {
        m_FrucDisabled = true; return false;
    }

    // === Allocate buffers (DEVICE_LOCAL) ===
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(m_PhysicalDevice, &memProps);
    auto pickMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags want) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want)
                return (int)i;
        }
        return -1;
    };
    auto allocBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer& outBuf, VkDeviceMemory& outMem) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq = {};
        pfnGetBufMemReq(m_Device, outBuf, &memReq);
        int mti = pickMemType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mti < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = (uint32_t)mti;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        return pfnBindBufMem(m_Device, outBuf, outMem, 0) == VK_SUCCESS;
    };
    const VkDeviceSize sizeRGB = (VkDeviceSize)width * height * 3 * sizeof(float);
    const VkDeviceSize sizeMV  = (VkDeviceSize)mvW * mvH * 2 * sizeof(int);

    if (!allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  m_FrucPrevRgbBuf, m_FrucPrevRgbBufMem)
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucCurrRgbBuf, m_FrucCurrRgbBufMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvBuf, m_FrucMvBufMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucPrevMvBuf, m_FrucPrevMvMem)
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     m_FrucInterpRgbBuf, m_FrucInterpRgbMem)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: buffer alloc failed");
        m_FrucDisabled = true;
        return false;
    }

    // === Descriptor pool: 4 sets × (2+4+2+4) = 12 storage-buffer descriptors ===
    // (i.4.1 added NV12→RGB with 2 bindings → 1 more set, 2 more descriptors)
    VkDescriptorPoolSize pSize = {};
    pSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pSize.descriptorCount = 12;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 4;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &pSize;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_FrucDescPool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: vkCreateDescriptorPool failed");
        m_FrucDisabled = true; return false;
    }

    auto allocAndUpdateSet = [&](VkDescriptorSetLayout dsl,
                                  std::vector<VkBuffer> bufs,
                                  VkDescriptorSet& outDs) -> bool {
        VkDescriptorSetAllocateInfo asi = {};
        asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        asi.descriptorPool = m_FrucDescPool;
        asi.descriptorSetCount = 1;
        asi.pSetLayouts = &dsl;
        if (pfnAllocDescSets(m_Device, &asi, &outDs) != VK_SUCCESS) return false;
        std::vector<VkDescriptorBufferInfo> bi(bufs.size());
        std::vector<VkWriteDescriptorSet> wr(bufs.size());
        for (size_t i = 0; i < bufs.size(); i++) {
            bi[i].buffer = bufs[i];
            bi[i].offset = 0;
            bi[i].range = VK_WHOLE_SIZE;
            wr[i] = {};
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = outDs;
            wr[i].dstBinding = (uint32_t)i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[i].pBufferInfo = &bi[i];
        }
        pfnUpdateDescSets(m_Device, (uint32_t)wr.size(), wr.data(), 0, nullptr);
        return true;
    };

    // §J.3.e.2.i.4.1 NV12→RGB descriptor: binding 0 = staging buffer
    // (entire NV12), binding 1 = currRGB output buffer.
    if (!allocAndUpdateSet(m_FrucNv12RgbDsl,
                           { m_SwStagingBuffer, m_FrucCurrRgbBuf },
                           m_FrucNv12RgbDescSet)
        || !allocAndUpdateSet(m_FrucMeDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucPrevMvBuf, m_FrucMvBuf },
                              m_FrucMeDescSet)
        || !allocAndUpdateSet(m_FrucMedianDsl,
                              { m_FrucMvBuf, m_FrucMvFilteredBuf },
                              m_FrucMedianDescSet)
        || !allocAndUpdateSet(m_FrucWarpDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvFilteredBuf, m_FrucInterpRgbBuf },
                              m_FrucWarpDescSet)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: descriptor set alloc/update failed");
        m_FrucDisabled = true; return false;
    }

    // §J.3.e.2.i.6 GPU timestamp query pool + timestampPeriod cache.
    // Optional — failure here doesn't block FRUC compute, just disables timing.
    {
        auto pfnCreateQueryPool = (PFN_vkCreateQueryPool)getDevPa(m_Device, "vkCreateQueryPool");
        auto pfnGetPhysProps    = (PFN_vkGetPhysicalDeviceProperties)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetPhysicalDeviceProperties");
        if (pfnCreateQueryPool && pfnGetPhysProps) {
            VkPhysicalDeviceProperties props = {};
            pfnGetPhysProps(m_PhysicalDevice, &props);
            m_FrucTimerNsPerTick = (double)props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qpCi = {};
            qpCi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpCi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpCi.queryCount = 2 * kFrucFramesInFlight;  // 2 timestamps per slot
            if (pfnCreateQueryPool(m_Device, &qpCi, nullptr, &m_FrucTimerPool) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.6 timestamp pool create failed (non-fatal)");
                m_FrucTimerPool = VK_NULL_HANDLE;
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.6 timestamp pool ready "
                            "(period=%.2f ns/tick, %u queries)",
                            m_FrucTimerNsPerTick, qpCi.queryCount);
            }
        }
    }

    m_FrucReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: PASS — 3 pipelines + 6 buffers + 3 descSets ready "
                "(sizeRGB=%llu, sizeMV=%llu)",
                (unsigned long long)sizeRGB, (unsigned long long)sizeMV);
    return true;
}

void VkFrucRenderer::destroyFrucComputeResources()
{
    if (!m_FrucReady && !m_FrucMePipeline) return;
    if (m_Device == VK_NULL_HANDLE) return;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyPipe     = (PFN_vkDestroyPipeline)getDevPa(m_Device, "vkDestroyPipeline");
    auto pfnDestroyPL       = (PFN_vkDestroyPipelineLayout)getDevPa(m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl      = (PFN_vkDestroyDescriptorSetLayout)getDevPa(m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroyShader   = (PFN_vkDestroyShaderModule)getDevPa(m_Device, "vkDestroyShaderModule");
    auto pfnDestroyBuf      = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem         = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnDestroyDescPool = (PFN_vkDestroyDescriptorPool)getDevPa(m_Device, "vkDestroyDescriptorPool");

#define DESTROY_PIPE(p, l, d, s)                                          \
    if (p && pfnDestroyPipe)   { pfnDestroyPipe(m_Device,   p, nullptr); p = VK_NULL_HANDLE; } \
    if (l && pfnDestroyPL)     { pfnDestroyPL(m_Device,     l, nullptr); l = VK_NULL_HANDLE; } \
    if (d && pfnDestroyDsl)    { pfnDestroyDsl(m_Device,    d, nullptr); d = VK_NULL_HANDLE; } \
    if (s && pfnDestroyShader) { pfnDestroyShader(m_Device, s, nullptr); s = VK_NULL_HANDLE; }
    DESTROY_PIPE(m_FrucNv12RgbPipeline, m_FrucNv12RgbPipeLay, m_FrucNv12RgbDsl, m_FrucNv12RgbShaderMod)
    DESTROY_PIPE(m_FrucMePipeline,      m_FrucMePipeLay,      m_FrucMeDsl,      m_FrucMeShaderMod)
    DESTROY_PIPE(m_FrucMedianPipeline,  m_FrucMedianPipeLay,  m_FrucMedianDsl,  m_FrucMedianShaderMod)
    DESTROY_PIPE(m_FrucWarpPipeline,    m_FrucWarpPipeLay,    m_FrucWarpDsl,    m_FrucWarpShaderMod)
#undef DESTROY_PIPE

#define DESTROY_BUF(b, m)                                          \
    if (b && pfnDestroyBuf) { pfnDestroyBuf(m_Device, b, nullptr); b = VK_NULL_HANDLE; } \
    if (m && pfnFreeMem)    { pfnFreeMem(m_Device,    m, nullptr); m = VK_NULL_HANDLE; }
    DESTROY_BUF(m_FrucPrevRgbBuf,    m_FrucPrevRgbBufMem)
    DESTROY_BUF(m_FrucCurrRgbBuf,    m_FrucCurrRgbBufMem)
    DESTROY_BUF(m_FrucMvBuf,         m_FrucMvBufMem)
    DESTROY_BUF(m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
    DESTROY_BUF(m_FrucPrevMvBuf,     m_FrucPrevMvMem)
    DESTROY_BUF(m_FrucInterpRgbBuf,  m_FrucInterpRgbMem)
#undef DESTROY_BUF

    if (m_FrucDescPool && pfnDestroyDescPool) {
        pfnDestroyDescPool(m_Device, m_FrucDescPool, nullptr);
        m_FrucDescPool = VK_NULL_HANDLE;
    }

    // §J.3.e.2.i.6 timestamp pool
    auto pfnDestroyQueryPool = (PFN_vkDestroyQueryPool)getDevPa(m_Device, "vkDestroyQueryPool");
    if (m_FrucTimerPool && pfnDestroyQueryPool) {
        pfnDestroyQueryPool(m_Device, m_FrucTimerPool, nullptr);
        m_FrucTimerPool = VK_NULL_HANDLE;
    }

    // §J.3.e.2.i.6 teardown crash fix — when the last ref drops, call
    // ncnn::destroy_gpu_instance() so ncnn's internal Vulkan resources
    // are released BEFORE process exit (their static dtors otherwise hit
    // stale state that we already destroyed → SIGSEGV).  Use fetch_sub:
    // returns OLD value, so old==1 means we're the last one releasing.
    if (m_NcnnInited) {
        if (s_NcnnRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.6 last ref — calling "
                        "ncnn::destroy_gpu_instance()");
            ncnn::destroy_gpu_instance();
        }
        m_NcnnInited = false;
    }

    m_FrucReady = false;
}

// §J.3.e.2.i.4 — record FRUC compute chain into the existing renderFrame
// command buffer (we don't use a separate compute queue/cmdpool — runs on
// our universal graphics queue with explicit pipeline barriers).
//
// Push constant layouts (must match the GLSL shader expectations from
// PlVkRenderer):
//   ME     (24 bytes): vec2 invSize / int mvW / int mvH / int blockSize
//                       — but actually: int srcW,srcH,mvW,mvH,blockSize,frameNum
//   Median (16 bytes): int mvW,mvH,radius,reserved
//   Warp   (24 bytes): int srcW,srcH,mvW,mvH,blockSize,frameNum
bool VkFrucRenderer::runFrucComputeChain(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if (!m_FrucReady) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdBindPipeline    = (PFN_vkCmdBindPipeline)getDevPa(m_Device, "vkCmdBindPipeline");
    auto pfnCmdBindDescSets    = (PFN_vkCmdBindDescriptorSets)getDevPa(m_Device, "vkCmdBindDescriptorSets");
    auto pfnCmdPushConst       = (PFN_vkCmdPushConstants)getDevPa(m_Device, "vkCmdPushConstants");
    auto pfnCmdDispatch        = (PFN_vkCmdDispatch)getDevPa(m_Device, "vkCmdDispatch");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    auto pfnCmdCopyBuffer      = (PFN_vkCmdCopyBuffer)getDevPa(m_Device, "vkCmdCopyBuffer");
    auto pfnCmdResetQueryPool  = (PFN_vkCmdResetQueryPool)getDevPa(m_Device, "vkCmdResetQueryPool");
    auto pfnCmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp)getDevPa(m_Device, "vkCmdWriteTimestamp");
    auto pfnGetQueryPoolResults = (PFN_vkGetQueryPoolResults)getDevPa(m_Device, "vkGetQueryPoolResults");
    if (!pfnCmdBindPipeline || !pfnCmdBindDescSets || !pfnCmdPushConst
        || !pfnCmdDispatch || !pfnCmdPipelineBarrier || !pfnCmdCopyBuffer) return false;

    // §J.3.e.2.i.6 — read PREVIOUS pass's timestamps for THIS slot (fence
    // wait at start of renderFrameSw guarantees GPU finished).  Skip on
    // first iteration when not yet armed.
    const uint32_t timerSlot = m_FrucTimerSlot;
    const uint32_t timerBase = timerSlot * 2;
    if (m_FrucTimerPool && pfnGetQueryPoolResults && pfnCmdResetQueryPool
        && pfnCmdWriteTimestamp && m_FrucTimerArmed[timerSlot]) {
        uint64_t ts[2] = {};
        VkResult qr = pfnGetQueryPoolResults(m_Device, m_FrucTimerPool,
            timerBase, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (qr == VK_SUCCESS && ts[1] >= ts[0]) {
            uint64_t deltaTicks = ts[1] - ts[0];
            double deltaUs = (double)deltaTicks * m_FrucTimerNsPerTick / 1000.0;
            m_FrucGpuUsAccum += deltaUs;
            m_FrucGpuUsCount++;
        }
    }
    // Reset queries for this slot before re-using.
    if (m_FrucTimerPool && pfnCmdResetQueryPool) {
        pfnCmdResetQueryPool(cmd, m_FrucTimerPool, timerBase, 2);
    }
    m_FrucTimerSlot = (m_FrucTimerSlot + 1) % kFrucFramesInFlight;

    const uint32_t mvW = m_FrucMvWidth;
    const uint32_t mvH = m_FrucMvHeight;
    const uint32_t BLOCK_SIZE = 8;
    const uint32_t MEDIAN_RADIUS = 1;
    const uint32_t frameNum = (uint32_t)(m_FrucFrameCount++);

    auto bufBarrier = [&](VkBuffer b,
                          VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage,
                          VkAccessFlags srcAcc, VkAccessFlags dstAcc) {
        VkBufferMemoryBarrier bmb = {};
        bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask = srcAcc;
        bmb.dstAccessMask = dstAcc;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = b;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };
    auto computeBufBarrier = [&](VkBuffer b) {
        bufBarrier(b,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    // §J.3.e.2.i.6 — write chain_start timestamp BEFORE first dispatch.
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 0);
    }

    // ---- Stage 0 (i.4.1): NV12 → planar fp32 RGB ----
    // Reads m_SwStagingBuffer (NV12 packed: Y plane + UV plane), writes to
    // m_FrucCurrRgbBuf.  Staging buffer contents were memcpy'd from CPU
    // earlier in renderFrameSw — host coherent so already visible to GPU.
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeLay,
                       0, 1, &m_FrucNv12RgbDescSet, 0, nullptr);
    {
        struct { int w, h, uvByteOffset, _pad; } pcN = {
            (int)width, (int)height, (int)(width * height), 0
        };
        pfnCmdPushConst(cmd, m_FrucNv12RgbPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcN), &pcN);
    }
    pfnCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    computeBufBarrier(m_FrucCurrRgbBuf);

    // ---- Stage 1: motion estimation ----
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeLay,
                       0, 1, &m_FrucMeDescSet, 0, nullptr);
    {
        struct { int srcW, srcH, mvW, mvH, blockSize, frameNum; } pcME = {
            (int)width, (int)height, (int)mvW, (int)mvH, (int)BLOCK_SIZE, (int)frameNum
        };
        pfnCmdPushConst(cmd, m_FrucMePipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcME), &pcME);
    }
    pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
    computeBufBarrier(m_FrucMvBuf);

    // ---- Stage 2: MV median filter ----
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeLay,
                       0, 1, &m_FrucMedianDescSet, 0, nullptr);
    {
        struct { int mvW, mvH, radius, _pad; } pcMed = {
            (int)mvW, (int)mvH, (int)MEDIAN_RADIUS, 0
        };
        pfnCmdPushConst(cmd, m_FrucMedianPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcMed), &pcMed);
    }
    pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
    computeBufBarrier(m_FrucMvFilteredBuf);

    // ---- Stage 3: warp ----
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeLay,
                       0, 1, &m_FrucWarpDescSet, 0, nullptr);
    {
        struct { int srcW, srcH, mvW, mvH, blockSize, frameNum; } pcWarp = {
            (int)width, (int)height, (int)mvW, (int)mvH, (int)BLOCK_SIZE, (int)frameNum
        };
        pfnCmdPushConst(cmd, m_FrucWarpPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcWarp), &pcWarp);
    }
    pfnCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
    computeBufBarrier(m_FrucInterpRgbBuf);

    // ---- Stage 4 (i.4.1): currRGB → prevRGB for next frame's ME ----
    // ME shader reads (prevRGB, currRGB) — we want prevRGB to be the
    // PREVIOUS frame's RGB on next call.  Copy curr→prev at end of chain.
    // (Alternative: ping-pong descriptors, but cmdCopyBuffer is simpler.)
    bufBarrier(m_FrucCurrRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bufBarrier(m_FrucPrevRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy cpyRegion = {};
    cpyRegion.srcOffset = 0;
    cpyRegion.dstOffset = 0;
    cpyRegion.size      = (VkDeviceSize)width * height * 3 * sizeof(float);
    pfnCmdCopyBuffer(cmd, m_FrucCurrRgbBuf, m_FrucPrevRgbBuf, 1, &cpyRegion);
    // Make prev visible to next frame's compute reads (via implicit chain
    // — next frame's NV12→RGB writes currRGB, ME reads both).
    bufBarrier(m_FrucPrevRgbBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // §J.3.e.2.i.6 — write chain_end timestamp AFTER last barrier.
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 1);
        m_FrucTimerArmed[timerSlot] = true;
    }

    return true;
}

// §J.3.e.2.i.3.e — full renderFrame impl.
//
// Per-frame steps (single-present; dual-present added in i.5):
//   1. Slot rotation + vkWaitForFences (drain prior frame in this slot)
//   2. Destroy slot's pending image view (from the prior frame in this slot)
//   3. vkAcquireNextImageKHR with slot's acquireSem[0]
//   4. Lock AVVkFrame, build VkImageView with VkSamplerYcbcrConversionInfo
//   5. Update slot's descriptor set with the new view
//   6. Record cmd buffer:
//        a. QFOT acquire + layout transition for AVVkFrame.img[0]
//        b. vkCmdBeginRenderPass on framebuffer[imgIdx]
//        c. Bind pipeline + descriptor set
//        d. vkCmdDraw(3, 1, 0, 0)
//        e. vkCmdEndRenderPass
//   7. vkQueueSubmit:
//        - waitSems = [acquireSem[0], AVVkFrame.sem[0]@sem_value[0]]
//        - signalSems = [renderDoneSem[0], AVVkFrame.sem[0]@sem_value[0]+1]
//        - fence = inFlightFence (CPU drains here next iteration)
//   8. Update AVVkFrame.layout/access/queue_family/sem_value
//   9. Unlock AVVkFrame
//  10. vkQueuePresentKHR waiting on renderDoneSem[0]
void VkFrucRenderer::renderFrame(AVFrame* frame)
{
    // §J.3.e.2.i.3.e-SW dispatch: software upload path validates i.3
    // graphics pipeline in isolation from FFmpeg-Vulkan hwcontext.
    if (m_SwMode) {
        renderFrameSw(frame);
        return;
    }

    static std::atomic<uint64_t> s_FrameCount{0};
    uint64_t fnum = s_FrameCount.fetch_add(1, std::memory_order_relaxed);
    bool firstFrame = (fnum < 3);   // log first 3 frames for bisect coverage
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e renderFrame#%llu ENTRY frame=%p data[0]=%p "
                    "hw_frames_ctx=%p", (unsigned long long)fnum, (void*)frame,
                    frame ? frame->data[0] : nullptr,
                    frame ? (void*)frame->hw_frames_ctx : nullptr);
    }

    // §J.3.e.2.i.3.e DIAGNOSTIC: VIPLE_VKFRUC_DIAG_EMPTY=1 returns
    // immediately — pure ABI smoke test for IFFmpegRenderer interface.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_DIAG_EMPTY") != 0) {
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_EMPTY: empty return",
                                     (unsigned long long)fnum);
        return;
    }

    // §J.3.e.2.i.3.e DIAGNOSTIC: when VIPLE_VKFRUC_DIAG_NOAVVKFRAME=1, skip
    // ALL AVVkFrame interaction (no lock_frame, no image view, no descriptor
    // update, no sem wait/signal, no state mutation) and just clear-and-present
    // the swapchain.  Isolates whether the v1.3.123-130 crash-after-frame#0 is
    // in the AVVkFrame interaction or in the cmd record/submit/present cycle
    // itself.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_DIAG_NOAVVKFRAME") != 0) {
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_NOAVVKFRAME: clear-only path",
                                     (unsigned long long)fnum);

        uint32_t slot = m_CurrentSlot;
        m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;
        m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot], VK_TRUE, UINT64_MAX);
        m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);

        uint32_t imgIdx = 0;
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdx);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) return;

        VkCommandBuffer cmd = m_SlotCmdBuf[slot];
        m_RtPfn.ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

        VkClearValue clearVal = {};
        clearVal.color.float32[0] = 0.5f;  // mid-grey so we can see it's working
        clearVal.color.float32[1] = 0.0f;
        clearVal.color.float32[2] = 0.5f;
        clearVal.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rpbi = {};
        rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass           = m_RenderPass;
        rpbi.framebuffer          = m_Framebuffers[imgIdx];
        rpbi.renderArea.offset    = { 0, 0 };
        rpbi.renderArea.extent    = m_SwapchainExtent;
        rpbi.clearValueCount      = 1;
        rpbi.pClearValues         = &clearVal;
        m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        // No bind/draw — clear-only.
        m_RtPfn.CmdEndRenderPass(cmd);
        m_RtPfn.EndCommandBuffer(cmd);

        VkPipelineStageFlags waitMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_SlotAcquireSem[slot][0];
        si.pWaitDstStageMask    = &waitMask;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_SlotRenderDoneSem[slot][0];
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }

        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_Swapchain;
        pi.pImageIndices      = &imgIdx;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
        }
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_NOAVVKFRAME OK",
                                     (unsigned long long)fnum);
        return;  // bypass real path
    }

    if (frame == nullptr || frame->data[0] == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] renderFrame: null frame/data");
        return;
    }
    if (frame->hw_frames_ctx == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] renderFrame: null hw_frames_ctx — non-hwaccel frame");
        return;
    }

    AVVkFrame* vkf = (AVVkFrame*)frame->data[0];
    AVHWFramesContext* fc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
    AVVulkanFramesContext* vkfc = (AVVulkanFramesContext*)fc->hwctx;
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkf=%p img[0]=%p img[1]=%p layout[0]=%d "
                    "queue_family[0]=%u sem[0]=%p sem_value[0]=%llu fc=%p vkfc=%p "
                    "lock_frame=%p", (void*)vkf,
                    vkf ? (void*)vkf->img[0] : nullptr,
                    vkf ? (void*)vkf->img[1] : nullptr,
                    vkf ? (int)vkf->layout[0] : -1,
                    vkf ? (unsigned)vkf->queue_family[0] : 0,
                    vkf ? (void*)vkf->sem[0] : nullptr,
                    vkf ? (unsigned long long)vkf->sem_value[0] : 0,
                    (void*)fc, (void*)vkfc,
                    vkfc ? (void*)vkfc->lock_frame : nullptr);
    }

    // ---- 1. Slot rotation + fence wait ----
    uint32_t slot = m_CurrentSlot;
    m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;

    m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot],
                          VK_TRUE, UINT64_MAX);
    m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);

    // ---- 2. Destroy slot's pending image view from prior frame ----
    if (m_SlotPendingView[slot] != VK_NULL_HANDLE) {
        m_RtPfn.DestroyImageView(m_Device, m_SlotPendingView[slot], nullptr);
        m_SlotPendingView[slot] = VK_NULL_HANDLE;
    }

    // ---- 3. Acquire next swapchain image ----
    uint32_t imgIdx = 0;
    VkResult vr = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                              m_SlotAcquireSem[slot][0],
                                              VK_NULL_HANDLE, &imgIdx);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkAcquireNextImageKHR failed (%d)", (int)vr);
        // Re-sign the fence so next iteration doesn't deadlock; resize / recreate
        // is i.6 work — for now just bail.
        m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
        return;
    }

    // ---- 4. Lock AVVkFrame, build image view ----
    // §J.3.e.2.i.3.e: lock_frame is documented as required when mutating
    // AVVkFrame metadata; in practice FFmpeg-vulkan always sets it via its
    // default mutex helper but defensive null-check is cheap insurance.
    if (vkfc->lock_frame) vkfc->lock_frame(fc, vkf);
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 after lock_frame OK — building image view");
    }

    VkSamplerYcbcrConversionInfo convInfo = {};
    convInfo.sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    convInfo.conversion = m_YcbcrConversion;

    VkImageViewCreateInfo viewCi = {};
    viewCi.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCi.pNext      = &convInfo;
    viewCi.image      = vkf->img[0];
    viewCi.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    viewCi.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    viewCi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCi.subresourceRange.baseMipLevel   = 0;
    viewCi.subresourceRange.levelCount     = 1;
    viewCi.subresourceRange.baseArrayLayer = 0;
    viewCi.subresourceRange.layerCount     = 1;

    VkImageView frameView = VK_NULL_HANDLE;
    VkResult viewVr = m_RtPfn.CreateImageView(m_Device, &viewCi, nullptr, &frameView);
    if (viewVr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkCreateImageView for AVVkFrame failed (%d)", (int)viewVr);
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
        return;
    }
    m_SlotPendingView[slot] = frameView;
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 image view created OK (%p)", (void*)frameView);
    }

    // ---- 5. Update slot's descriptor set with the new view ----
    VkDescriptorImageInfo dii = {};
    dii.sampler     = VK_NULL_HANDLE;  // immutable, baked into layout
    dii.imageView   = frameView;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = m_SlotDescSet[slot];
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    m_RtPfn.UpdateDescriptorSets(m_Device, 1, &wds, 0, nullptr);

    // ---- 6. Record cmd buffer ----
    VkCommandBuffer cmd = m_SlotCmdBuf[slot];
    m_RtPfn.ResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

    //  6a. Pipeline barrier — layout transition only.  Use
    //      VK_QUEUE_FAMILY_IGNORED on both sides (PlVkRenderer pattern):
    //      AVVkFrame.sem timeline semaphore wait in vkQueueSubmit below
    //      provides cross-queue-family execution + memory dependency, so
    //      explicit QFOT release/acquire is unnecessary (and would hang
    //      without a matching release-side barrier on the decoder queue,
    //      which FFmpeg-vulkan does not always issue).
    VkImageMemoryBarrier acquireBar = {};
    acquireBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquireBar.srcAccessMask       = 0;
    acquireBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    acquireBar.oldLayout           = vkf->layout[0];
    acquireBar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    acquireBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBar.image               = vkf->img[0];
    acquireBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    acquireBar.subresourceRange.baseMipLevel   = 0;
    acquireBar.subresourceRange.levelCount     = 1;
    acquireBar.subresourceRange.baseArrayLayer = 0;
    acquireBar.subresourceRange.layerCount     = 1;
    m_RtPfn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &acquireBar);
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 cmd record: barrier issued (oldLayout=%d) "
                    "imgIdx=%u fb=%p extent=%ux%u pipeline=%p layout=%p descSet=%p",
                    (int)vkf->layout[0], imgIdx,
                    (void*)m_Framebuffers[imgIdx],
                    m_SwapchainExtent.width, m_SwapchainExtent.height,
                    (void*)m_GraphicsPipeline, (void*)m_GraphicsPipelineLayout,
                    (void*)m_SlotDescSet[slot]);
    }

    //  6b. Begin render pass — clear to opaque black.
    VkClearValue clearVal = {};
    clearVal.color.float32[0] = 0.0f;
    clearVal.color.float32[1] = 0.0f;
    clearVal.color.float32[2] = 0.0f;
    clearVal.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass           = m_RenderPass;
    rpbi.framebuffer          = m_Framebuffers[imgIdx];
    rpbi.renderArea.offset    = { 0, 0 };
    rpbi.renderArea.extent    = m_SwapchainExtent;
    rpbi.clearValueCount      = 1;
    rpbi.pClearValues         = &clearVal;
    m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBeginRenderPass OK");

    //  6c. Bind pipeline + descriptor set, draw 3 vertices (fullscreen tri).
    m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBindPipeline OK");
    m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_GraphicsPipelineLayout, 0,
                                  1, &m_SlotDescSet[slot], 0, nullptr);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBindDescriptorSets OK");
    m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdDraw OK");

    m_RtPfn.CmdEndRenderPass(cmd);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdEndRenderPass OK");

    //  6d. Transition AVVkFrame.img[0] to VK_IMAGE_LAYOUT_GENERAL so
    //      FFmpeg's decoder can re-use it as a reference frame.  We
    //      can't transition back to VIDEO_DECODE_DPB_KHR here because
    //      that layout is restricted to queues with VK_QUEUE_VIDEO_DECODE_BIT
    //      (spec).  GENERAL is universally compatible — FFmpeg's next
    //      decode does its own barrier GENERAL → DECODE_DPB on its
    //      decode queue (valid).  We update vkf->layout[0] = GENERAL
    //      below so FFmpeg knows the image's current layout when it
    //      issues that next barrier.
    VkImageMemoryBarrier releaseBar = {};
    releaseBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    releaseBar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    releaseBar.dstAccessMask       = 0;
    releaseBar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    releaseBar.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    releaseBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    releaseBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    releaseBar.image               = vkf->img[0];
    releaseBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    releaseBar.subresourceRange.baseMipLevel   = 0;
    releaseBar.subresourceRange.levelCount     = 1;
    releaseBar.subresourceRange.baseArrayLayer = 0;
    releaseBar.subresourceRange.layerCount     = 1;
    m_RtPfn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &releaseBar);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 release barrier issued (to GENERAL)");

    VkResult endVr = m_RtPfn.EndCommandBuffer(cmd);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 EndCommandBuffer returned %d", (int)endVr);

    // ---- 7. Submit ----
    // Wait on (a) swapchain acquire (color attachment write must wait), and
    // (b) AVVkFrame's timeline semaphore at the value FFmpeg signaled when
    // decode finished (fragment shader read must wait).
    VkSemaphore     waitSems[2]   = { m_SlotAcquireSem[slot][0], vkf->sem[0] };
    VkPipelineStageFlags waitMasks[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    };
    uint64_t        waitVals[2]   = { 0, vkf->sem_value[0] };

    // Signal (a) renderDoneSem (consumed by present), and (b) AVVkFrame
    // timeline at sem_value[0]+1 so FFmpeg knows we're done with the frame.
    VkSemaphore signalSems[2]     = { m_SlotRenderDoneSem[slot][0], vkf->sem[0] };
    uint64_t    signalVals[2]     = { 0, vkf->sem_value[0] + 1 };

    VkTimelineSemaphoreSubmitInfo tssi = {};
    tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tssi.waitSemaphoreValueCount   = 2;
    tssi.pWaitSemaphoreValues      = waitVals;
    tssi.signalSemaphoreValueCount = 2;
    tssi.pSignalSemaphoreValues    = signalVals;

    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = &tssi;
    si.waitSemaphoreCount   = 2;
    si.pWaitSemaphores      = waitSems;
    si.pWaitDstStageMask    = waitMasks;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores    = signalSems;

    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 about to QueueSubmit (queue=%p fence=%p sem[wait]=%p+%llu sem[signal]=%p+%llu)",
                                (void*)m_GraphicsQueue, (void*)m_SlotInFlightFence[slot],
                                (void*)vkf->sem[0], (unsigned long long)waitVals[1],
                                (void*)vkf->sem[0], (unsigned long long)signalVals[1]);
    {
        std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
        vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
    }
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkQueueSubmit failed (%d)", (int)vr);
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
        return;
    }
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkQueueSubmit OK — first GPU work in flight");
    }

    // ---- 8. Update AVVkFrame state ----
    // §J.3.e.2.i.3.e: tell FFmpeg the image's new state so it can issue
    // a correct barrier (GENERAL → DECODE_DPB) on its decode queue when
    // it re-uses this image as a reference frame.
    vkf->access[0]     = (VkAccessFlagBits)0;            // matches dstAccess of releaseBar (some FFmpeg builds type access[] as VkAccessFlagBits not uint32_t)
    vkf->layout[0]     = VK_IMAGE_LAYOUT_GENERAL;        // matches newLayout of releaseBar
    // queue_family[0] kept as IGNORED (no QFOT was performed)
    vkf->sem_value[0] += 1;

    // ---- 9. Unlock AVVkFrame ----
    if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);

    // ---- 10. Present ----
    VkPresentInfoKHR pi = {};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_Swapchain;
    pi.pImageIndices      = &imgIdx;

    {
        std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
        vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
    }
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] vkQueuePresentKHR returned %d", (int)vr);
        // Don't bail — outer caller may handle resize; defer to i.6.
    }
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkQueuePresentKHR OK — first frame complete");
    }
}

// §J.3.e.2.i.3.e-SW — software-upload renderFrame.  Dispatched from
// renderFrame() when m_SwMode is set.  Frame is AV_PIX_FMT_NV12 in CPU
// memory: data[0]=Y plane (linesize[0] stride), data[1]=UV plane
// (linesize[1] stride).  We:
//   1. memcpy Y + UV planes into the persistent staging buffer
//   2. Acquire next swapchain image (binary acquire sem)
//   3. Wait this slot's fence, reset
//   4. Record cmd buffer:
//        a. barrier upload image → TRANSFER_DST_OPTIMAL (or UNDEFINED→DST first frame)
//        b. vkCmdCopyBufferToImage staging → image (one region per plane)
//        c. barrier upload image → SHADER_READ_ONLY_OPTIMAL
//        d. begin renderpass on framebuffer[imgIdx]
//        e. bind pipeline + descriptor (already pointing at upload image view)
//        f. cmdDraw(3,1,0,0)
//        g. end renderpass
//   5. submit (wait acquireSem, signal renderDoneSem + fence) + present
void VkFrucRenderer::renderFrameSw(AVFrame* frame)
{
    static std::atomic<uint64_t> s_FrameCountSw{0};
    uint64_t fnum = s_FrameCountSw.fetch_add(1, std::memory_order_relaxed);
    bool firstFrame = (fnum < 3);

    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-SW] frame#%llu ENTRY format=%d w=%d h=%d "
                    "data[0]=%p data[1]=%p linesize[0]=%d linesize[1]=%d",
                    (unsigned long long)fnum, (int)frame->format,
                    frame->width, frame->height,
                    (void*)frame->data[0], (void*)frame->data[1],
                    frame->linesize[0], frame->linesize[1]);
    }

    if (frame == nullptr || frame->data[0] == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-SW] null frame/data[0]");
        return;
    }
    if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_NV12) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-SW] unexpected pixfmt %d (want YUV420P=%d or NV12=%d)",
                    frame->format, (int)AV_PIX_FMT_YUV420P, (int)AV_PIX_FMT_NV12);
        return;
    }
    if (frame->width != m_SwImageWidth || frame->height != m_SwImageHeight) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-SW] resolution mismatch %dx%d vs allocated %dx%d",
                    frame->width, frame->height, m_SwImageWidth, m_SwImageHeight);
        return;
    }

    // ---- 1. memcpy/repack Y + UV into staging (NV12 layout) ----
    const int W = m_SwImageWidth;
    const int H = m_SwImageHeight;
    uint8_t* dst = (uint8_t*)m_SwStagingMapped;
    // Y plane: same layout for both YUV420P and NV12, just stride-fix copy.
    for (int y = 0; y < H; y++) {
        memcpy(dst + y * W, frame->data[0] + y * frame->linesize[0], W);
    }
    // UV plane:
    //   NV12 input → already interleaved, plain memcpy each row (W bytes, H/2 rows)
    //   YUV420P input → 3 planes (Y, U, V); interleave U+V to get NV12 UV layout
    uint8_t* uvDst = dst + W * H;
    if (frame->format == AV_PIX_FMT_NV12) {
        if (frame->data[1] == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] NV12 frame missing data[1]");
            return;
        }
        for (int y = 0; y < H / 2; y++) {
            memcpy(uvDst + y * W, frame->data[1] + y * frame->linesize[1], W);
        }
    } else {
        // YUV420P: data[1]=U plane (W/2 × H/2), data[2]=V plane (W/2 × H/2)
        if (frame->data[1] == nullptr || frame->data[2] == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] YUV420P frame missing U/V plane");
            return;
        }
        for (int y = 0; y < H / 2; y++) {
            const uint8_t* uRow = frame->data[1] + y * frame->linesize[1];
            const uint8_t* vRow = frame->data[2] + y * frame->linesize[2];
            uint8_t* dstRow = uvDst + y * W;
            for (int x = 0; x < W / 2; x++) {
                dstRow[2 * x + 0] = uRow[x];  // U
                dstRow[2 * x + 1] = vRow[x];  // V
            }
        }
    }

    // ---- 2/3. Slot rotation, fence wait/reset, swapchain acquire ----
    uint32_t slot = m_CurrentSlot;
    m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;
    m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot], VK_TRUE, UINT64_MAX);
    m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);

    // Acquire 1 (real) image for single mode, or 2 (interp + real) for dual.
    uint32_t imgIdxA = 0;  // interp slot (only used in dual mode)
    uint32_t imgIdxB = 0;  // real frame slot (always)
    if (m_DualMode) {
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdxA);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: acquire interp imgA failed (%d)", (int)vrA);
            return;
        }
        VkResult vrB = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][1],
                                                    VK_NULL_HANDLE, &imgIdxB);
        if (vrB != VK_SUCCESS && vrB != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: acquire real imgB failed (%d)", (int)vrB);
            return;
        }
    } else {
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdxB);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] vkAcquireNextImageKHR failed (%d)", (int)vrA);
            return;
        }
    }
    uint32_t imgIdx = imgIdxB;  // legacy alias for the existing single-render code below

    // ---- 4. Record cmd buffer ----
    VkCommandBuffer cmd = m_SlotCmdBuf[slot];
    m_RtPfn.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

    // 4a. Barrier upload image → TRANSFER_DST_OPTIMAL (oldLayout depends
    // on whether this is the first frame).
    VkImageLayout oldImgLayout = m_SwImageLayoutInited
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier toDstBar = {};
    toDstBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDstBar.srcAccessMask       = m_SwImageLayoutInited ? VK_ACCESS_SHADER_READ_BIT : (VkAccessFlags)0;
    toDstBar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDstBar.oldLayout           = oldImgLayout;
    toDstBar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDstBar.image               = m_SwUploadImage;
    toDstBar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDstBar.subresourceRange.levelCount = 1;
    toDstBar.subresourceRange.layerCount = 1;
    m_RtPfn.CmdPipelineBarrier(cmd,
        m_SwImageLayoutInited ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                              : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toDstBar);

    // 4b. Copy staging → image (two regions, one per plane).
    VkBufferImageCopy regions[2] = {};
    // Y plane → PLANE_0
    regions[0].bufferOffset      = 0;
    regions[0].bufferRowLength   = (uint32_t)W;
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions[0].imageSubresource.layerCount = 1;
    regions[0].imageExtent       = { (uint32_t)W, (uint32_t)H, 1 };
    // UV plane → PLANE_1 (half-resolution, R8G8 format)
    regions[1].bufferOffset      = (VkDeviceSize)W * H;
    regions[1].bufferRowLength   = (uint32_t)W / 2;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions[1].imageSubresource.layerCount = 1;
    regions[1].imageExtent       = { (uint32_t)W / 2, (uint32_t)H / 2, 1 };
    m_RtPfn.CmdCopyBufferToImage(cmd, m_SwStagingBuffer, m_SwUploadImage,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  2, regions);

    // 4c. Barrier upload image → SHADER_READ_ONLY_OPTIMAL.
    VkImageMemoryBarrier toShaderBar = toDstBar;  // copy template
    toShaderBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderBar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderBar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    m_RtPfn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderBar);

    // §J.3.e.2.i.4 — FRUC compute chain (ME → median → warp).  Records
    // dispatches into the same cmd buffer as our graphics rendering; the
    // GPU executes them in order.  Outputs to m_FrucInterpRgbBuf which is
    // not yet displayed (i.4.2 will add dual-present); for now we just
    // verify the chain runs without crash.
    if (m_FrucMode && m_FrucReady) {
        runFrucComputeChain(cmd, (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight);
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu FRUC compute chain dispatched (%ux%u "
                        "block=8 mv=%ux%u)",
                        (unsigned long long)fnum,
                        (unsigned)m_SwImageWidth, (unsigned)m_SwImageHeight,
                        (unsigned)m_FrucMvWidth, (unsigned)m_FrucMvHeight);
        }
    }

    VkClearValue clearVal = {};
    clearVal.color.float32[3] = 1.0f;

    // §J.3.e.2.i.4.2 dual-present: first render pass writes interp via
    // m_InterpPipeline (samples bufInterpRGB) into framebuffer[imgIdxA].
    if (m_DualMode) {
        // Need pfnCmdPushConstants for interp shader; load via m_RtPfn isn't
        // there for push-const, so resolve here.
        auto getDevPa2 = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdPushConst = (PFN_vkCmdPushConstants)getDevPa2(m_Device, "vkCmdPushConstants");

        VkRenderPassBeginInfo rpbiA = {};
        rpbiA.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbiA.renderPass           = m_RenderPass;
        rpbiA.framebuffer          = m_Framebuffers[imgIdxA];
        rpbiA.renderArea.extent    = m_SwapchainExtent;
        rpbiA.clearValueCount      = 1;
        rpbiA.pClearValues         = &clearVal;
        m_RtPfn.CmdBeginRenderPass(cmd, &rpbiA, VK_SUBPASS_CONTENTS_INLINE);
        m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_InterpPipelineLayout, 0,
                                      1, &m_InterpDescSet, 0, nullptr);
        struct { int srcW, srcH, _pad0, _pad1; } pcInterp = {
            (int)m_SwImageWidth, (int)m_SwImageHeight, 0, 0
        };
        if (pfnCmdPushConst) {
            pfnCmdPushConst(cmd, m_InterpPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(pcInterp), &pcInterp);
        }
        m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
        m_RtPfn.CmdEndRenderPass(cmd);
    }

    // Real-frame render pass (always runs) — samples m_SwUploadImage NV12
    // via ycbcr conversion sampler.  In dual mode goes to imgIdxB; in
    // single mode goes to imgIdx (== imgIdxB).
    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass           = m_RenderPass;
    rpbi.framebuffer          = m_Framebuffers[imgIdx];
    rpbi.renderArea.extent    = m_SwapchainExtent;
    rpbi.clearValueCount      = 1;
    rpbi.pClearValues         = &clearVal;
    m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
    m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_GraphicsPipelineLayout, 0,
                                  1, &m_SlotDescSet[slot], 0, nullptr);
    m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
    m_RtPfn.CmdEndRenderPass(cmd);
    m_RtPfn.EndCommandBuffer(cmd);
    m_SwImageLayoutInited = true;  // image is now in SHADER_READ_ONLY for next frame's barrier

    // ---- 5. Submit + present ----
    VkResult vr;
    if (m_DualMode) {
        // Dual: wait both acquire sems (pass 0 = interp, pass 1 = real),
        // signal both renderDone sems.  Both render passes ran in order
        // in the same cmd buf; GPU executes them sequentially before
        // signaling.  The two presents below display both images.
        VkSemaphore waitSems[2]   = { m_SlotAcquireSem[slot][0], m_SlotAcquireSem[slot][1] };
        VkSemaphore signalSems[2] = { m_SlotRenderDoneSem[slot][0], m_SlotRenderDoneSem[slot][1] };
        VkPipelineStageFlags waitMasks[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        };
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 2;
        si.pWaitSemaphores      = waitSems;
        si.pWaitDstStageMask    = waitMasks;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 2;
        si.pSignalSemaphores    = signalSems;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: vkQueueSubmit failed (%d)", (int)vr);
            return;
        }

        // Present interp (imgIdxA) first, real (imgIdxB) second.
        VkPresentInfoKHR piA = {};
        piA.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piA.waitSemaphoreCount = 1;
        piA.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
        piA.swapchainCount     = 1;
        piA.pSwapchains        = &m_Swapchain;
        piA.pImageIndices      = &imgIdxA;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piA);
        }
        VkPresentInfoKHR piB = {};
        piB.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piB.waitSemaphoreCount = 1;
        piB.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][1];
        piB.swapchainCount     = 1;
        piB.pSwapchains        = &m_Swapchain;
        piB.pImageIndices      = &imgIdxB;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            VkResult vrB = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piB);
            if (vrB != VK_SUCCESS && vrB != VK_SUBOPTIMAL_KHR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-SW] dual: present(real) returned %d", (int)vrB);
            }
        }
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu DUAL OK — interp imgA=%u + real imgB=%u "
                        "presented", (unsigned long long)fnum, imgIdxA, imgIdxB);
        }
    } else {
        // Single-present (legacy SW path, m_DualMode off)
        VkPipelineStageFlags waitMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_SlotAcquireSem[slot][0];
        si.pWaitDstStageMask    = &waitMask;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_SlotRenderDoneSem[slot][0];
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] vkQueueSubmit failed (%d)", (int)vr);
            return;
        }

        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_Swapchain;
        pi.pImageIndices      = &imgIdx;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] vkQueuePresentKHR returned %d", (int)vr);
        }
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu OK — upload+render+present complete",
                        (unsigned long long)fnum);
        }
    }

    // §J.3.e.2.i.6 — periodic [VIPLE-VKFRUC-Stats] benchmark logging.
    // Mirrors D3D11+GenericFRUC's [VIPLE-PRESENT-Stats] format so we can
    // compare apples-to-apples.  Tracks frame-to-frame intervals (= present
    // pacing).  In dual mode, reports both real and "effective" (real +
    // interp counted as separate display events).  Emit every ~5 sec.
    {
        using namespace std::chrono;
        static thread_local steady_clock::time_point s_LastPresent{};
        static thread_local steady_clock::time_point s_StatsBucketStart{};
        static thread_local std::vector<double> s_FrameMsRing;  // intervals in current 5s bucket
        static thread_local uint64_t s_CumulReal   = 0;
        static thread_local uint64_t s_CumulInterp = 0;

        auto now = steady_clock::now();
        if (s_LastPresent.time_since_epoch().count() != 0) {
            double dtMs = duration_cast<duration<double, std::milli>>(now - s_LastPresent).count();
            s_FrameMsRing.push_back(dtMs);
        } else {
            s_StatsBucketStart = now;
        }
        s_LastPresent = now;
        s_CumulReal++;
        if (m_DualMode) s_CumulInterp++;

        // Emit every ~5 seconds of wall time in the current bucket.
        double bucketSec = duration_cast<duration<double>>(now - s_StatsBucketStart).count();
        if (bucketSec >= 5.0 && !s_FrameMsRing.empty()) {
            // Compute percentiles by sorted copy (small N=~150 at 30fps).
            std::vector<double> sorted = s_FrameMsRing;
            std::sort(sorted.begin(), sorted.end());
            auto pct = [&](double q) -> double {
                size_t idx = (size_t)((sorted.size() - 1) * q + 0.5);
                if (idx >= sorted.size()) idx = sorted.size() - 1;
                return sorted[idx];
            };
            double sum = 0;
            for (double v : sorted) sum += v;
            double mean = sum / sorted.size();
            double fps = sorted.size() / bucketSec;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] %s n=%zu fps=%.2f ft_mean=%.2fms "
                        "p50=%.2f p95=%.2f p99=%.2f p99.9=%.2f (window %.1fs)",
                        m_DualMode ? "dual-present" : "single-present",
                        sorted.size(), fps, mean,
                        pct(0.50), pct(0.95), pct(0.99), pct(0.999),
                        bucketSec);
            // §J.3.e.2.i.6 — GPU compute chain timing (NV12->RGB + ME +
            // Median + Warp), averaged over the window.
            double gpuMeanUs = (m_FrucGpuUsCount > 0)
                                ? (m_FrucGpuUsAccum / m_FrucGpuUsCount) : 0.0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] cumul real=%llu interp=%llu "
                        "compute_gpu_mean=%.3fms (n=%d) "
                        "(swMode=%d frucMode=%d dualMode=%d)",
                        (unsigned long long)s_CumulReal,
                        (unsigned long long)s_CumulInterp,
                        gpuMeanUs / 1000.0,
                        m_FrucGpuUsCount,
                        m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0);
            m_FrucGpuUsAccum = 0.0;
            m_FrucGpuUsCount = 0;

            s_FrameMsRing.clear();
            s_StatsBucketStart = now;
        }
    }
}

int VkFrucRenderer::getDecoderCapabilities()  { return 0; }
int VkFrucRenderer::getRendererAttributes()    { return 0; }

#endif // HAVE_LIBPLACEBO_VULKAN
