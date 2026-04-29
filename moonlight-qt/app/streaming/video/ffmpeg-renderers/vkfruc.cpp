// VipleStream §J.3.e.2.i — VkFrucRenderer
// See vkfruc.h header + docs/J.3.e.2.i_vulkan_native_renderer.md.

#include "vkfruc.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstring>
#include <mutex>

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

VkFrucRenderer::VkFrucRenderer(int pass)
    : IFFmpegRenderer(RendererType::Vulkan)
    , m_Pass(pass)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=%d)", pass);
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
    // Order: device-owned objects → device → surface → instance.
    // i.3+ will insert pipelines / etc. here.
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
    appInfo.apiVersion = VK_API_VERSION_1_1;

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

    VkPhysicalDeviceSynchronization2Features sync2Feat = {};
    sync2Feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    sync2Feat.synchronization2 = VK_TRUE;

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeat = {};
    timelineFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    timelineFeat.timelineSemaphore = VK_TRUE;
    timelineFeat.pNext = &sync2Feat;

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeat = {};
    ycbcrFeat.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeat.samplerYcbcrConversion = VK_TRUE;
    ycbcrFeat.pNext = &timelineFeat;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &ycbcrFeat;
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
    if (!populateAvHwDeviceCtx(m_VideoFormat)) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.a initialize: instance/PD/device/swapchain"
                " + AVHWDeviceContext READY — i.3.b (sampler+descriptor) is next; "
                "still returning false to fall through to PlVkRenderer for streaming");

    // i.3.a still returns false (no graphics pipeline / renderFrame yet).
    m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
    return false;
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
    // device_features: leave default (all-VK_FALSE struct).  ffmpeg
    // checks specific feature bits (samplerYcbcrConversion etc.) but
    // those are pulled from the device at hwdevice_ctx_init via
    // vkGetPhysicalDeviceFeatures2; we don't need to set them here.

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

    // Queue family info — fill the new-style nb_qf table when libavutil
    // supports it; older libavutil uses the discrete index/count fields.
    uint32_t qfCount = 0;
    pfnGetQFP(m_PhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    pfnGetQFP(m_PhysicalDevice, &qfCount, qfs.data());
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    (void)videoFormat;
    for (uint32_t i = 0; i < qfCount; i++) {
        vkCtx->qf[i].idx        = i;
        vkCtx->qf[i].num        = qfs[i].queueCount;
        vkCtx->qf[i].flags      = (VkQueueFlagBits)qfs[i].queueFlags;
        vkCtx->qf[i].video_caps = (VkVideoCodecOperationFlagBitsKHR)0;  // ffmpeg re-probes
    }
    vkCtx->nb_qf = qfCount;
#else
    vkCtx->queue_family_index        = (int)m_QueueFamily;
    vkCtx->nb_graphics_queues        = 1;
    vkCtx->queue_family_tx_index     = (int)m_QueueFamily;
    vkCtx->nb_tx_queues              = 1;
    vkCtx->queue_family_comp_index   = (int)m_QueueFamily;
    vkCtx->nb_comp_queues            = 1;
    vkCtx->queue_family_decode_index = (int)m_DecodeQueueFamily;
    vkCtx->nb_decode_queues          = (int)m_DecodeQueueCount;
    (void)videoFormat;
#endif

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

void VkFrucRenderer::renderFrame(AVFrame* frame)
{
    (void)frame;
}

int VkFrucRenderer::getDecoderCapabilities()  { return 0; }
int VkFrucRenderer::getRendererAttributes()    { return 0; }

#endif // HAVE_LIBPLACEBO_VULKAN
