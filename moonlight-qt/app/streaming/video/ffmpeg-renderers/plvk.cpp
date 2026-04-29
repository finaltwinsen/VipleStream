#include "plvk.h"

#include "streaming/session.h"
#include "streaming/streamutils.h"

// §J.3.e.1.d — handoff to ncnn via the new external VkDevice API.
#include <ncnn/gpu.h>

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <SDL_vulkan.h>

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

#include <vector>
#include <set>
#include <atomic>

#ifndef VK_KHR_video_decode_av1
#define VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME "VK_KHR_video_decode_av1"
#define VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR ((VkVideoCodecOperationFlagBitsKHR)0x00000004)
#endif

#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(60, 26, 100)
static const char *k_OptionalDeviceExtensions[] = {
    /* Misc or required by other extensions */
    //VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
    VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
    VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,

    /* Imports/exports */
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
#ifdef Q_OS_WIN32
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif

    /* Video encoding/decoding */
    VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
    VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
#if LIBAVCODEC_VERSION_MAJOR >= 61
    VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, // FFmpeg 7.0 uses the official Khronos AV1 extension
#else
    "VK_MESA_video_decode_av1", // FFmpeg 6.1 uses the Mesa AV1 extension
#endif
};
#endif

static void pl_log_cb(void*, enum pl_log_level level, const char *msg)
{
    switch (level) {
    case PL_LOG_FATAL:
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_ERR:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_WARN:
        if (strncmp(msg, "Masking `", 9) == 0) {
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_INFO:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_DEBUG:
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    }
}

void PlVkRenderer::lockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index)
{
    auto me = (PlVkRenderer*)dev_ctx->user_opaque;
    me->m_Vulkan->lock_queue(me->m_Vulkan, queue_family, index);
}

void PlVkRenderer::unlockQueue(struct AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index)
{
    auto me = (PlVkRenderer*)dev_ctx->user_opaque;
    me->m_Vulkan->unlock_queue(me->m_Vulkan, queue_family, index);
}

void PlVkRenderer::overlayUploadComplete(void* opaque)
{
    SDL_FreeSurface((SDL_Surface*)opaque);
}

PlVkRenderer::PlVkRenderer(bool hwaccel, IFFmpegRenderer *backendRenderer) :
    IFFmpegRenderer(RendererType::Vulkan),
    m_Backend(backendRenderer),
    m_HwAccelBackend(hwaccel)
{
    bool ok;

    pl_log_params logParams = pl_log_default_params;
    logParams.log_cb = pl_log_cb;
    logParams.log_level = (pl_log_level)qEnvironmentVariableIntValue("PLVK_LOG_LEVEL", &ok);
    if (!ok) {
#ifdef QT_DEBUG
        logParams.log_level = PL_LOG_DEBUG;
#else
        logParams.log_level = PL_LOG_WARN;
#endif
    }

    m_Log = pl_log_create(PL_API_VER, &logParams);
}

PlVkRenderer::~PlVkRenderer()
{
    // The render context must have been cleaned up by now
    SDL_assert(!m_HasPendingSwapchainFrame);

    // §J.3.e.1.d — ncnn::destroy_gpu_instance must run BEFORE
    // pl_vulkan_destroy: ncnn-allocated VkPipeline/VkBuffer ride
    // libplacebo's VkDevice; tearing down the device first makes the
    // subsequent vkDestroy* in ncnn fail.
    teardownNcnnExternalHandoff();

    // §J.3.e.2.a — same lifetime constraint: probe resources are owned
    // on m_Vulkan->device; destroy before pl_vulkan_destroy.
    destroyFrucProbeResources();

    if (m_Vulkan != nullptr) {
        for (int i = 0; i < (int)SDL_arraysize(m_Overlays); i++) {
            pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[i].overlay.tex);
            pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[i].stagingOverlay.tex);
        }

        for (int i = 0; i < (int)SDL_arraysize(m_Textures); i++) {
            pl_tex_destroy(m_Vulkan->gpu, &m_Textures[i]);
        }
    }

    pl_renderer_destroy(&m_Renderer);
    pl_swapchain_destroy(&m_Swapchain);
    pl_vulkan_destroy(&m_Vulkan);

    // This surface was created by SDL, so there's no libplacebo API to destroy it
    if (fn_vkDestroySurfaceKHR && m_VkSurface) {
        fn_vkDestroySurfaceKHR(m_PlVkInstance->instance, m_VkSurface, nullptr);
    }

    if (m_HwDeviceCtx != nullptr) {
        av_buffer_unref(&m_HwDeviceCtx);
    }

    pl_vk_inst_destroy(&m_PlVkInstance);

    // m_Log must always be the last object destroyed
    pl_log_destroy(&m_Log);
}

bool PlVkRenderer::chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired)
{
    uint32_t physicalDeviceCount = 0;
    fn_vkEnumeratePhysicalDevices(m_PlVkInstance->instance, &physicalDeviceCount, nullptr);
    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    fn_vkEnumeratePhysicalDevices(m_PlVkInstance->instance, &physicalDeviceCount, physicalDevices.data());

    std::set<uint32_t> devicesTried;
    VkPhysicalDeviceProperties deviceProps;

    if (physicalDeviceCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "No Vulkan devices found!");
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }

    // First, try the first device in the list to support device selection layers
    // that put the user's preferred GPU in the first slot.
    fn_vkGetPhysicalDeviceProperties(physicalDevices[0], &deviceProps);
    if (tryInitializeDevice(physicalDevices[0], &deviceProps, params, hdrOutputRequired)) {
        return true;
    }
    devicesTried.emplace(0);

    // Next, we'll try to match an integrated GPU, since we want to minimize
    // power consumption and inter-GPU copies.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (deviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
                return true;
            }
            devicesTried.emplace(i);
        }
    }

    // Next, we'll try to match a discrete GPU.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (deviceProps.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
                return true;
            }
            devicesTried.emplace(i);
        }
    }

    // Finally, we'll try matching any non-software device.
    for (uint32_t i = 0; i < physicalDeviceCount; i++) {
        // Skip devices we've already tried
        if (devicesTried.find(i) != devicesTried.end()) {
            continue;
        }

        VkPhysicalDeviceProperties deviceProps;
        fn_vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProps);
        if (tryInitializeDevice(physicalDevices[i], &deviceProps, params, hdrOutputRequired)) {
            return true;
        }
        devicesTried.emplace(i);
    }

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "No suitable %sVulkan devices found!",
                 hdrOutputRequired ? "HDR-capable " : "");
    return false;
}

bool PlVkRenderer::tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                                       PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired)
{
    // Check the Vulkan API version first to ensure it meets libplacebo's minimum
    if (deviceProps->apiVersion < PL_VK_MIN_VERSION) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not meet minimum Vulkan version",
                    deviceProps->deviceName);
        return false;
    }

#ifdef Q_OS_WIN32
    // Intel's Windows drivers seem to have interoperability issues as of FFmpeg 7.0.1
    // when using Vulkan Video decoding. Since they also expose HEVC REXT profiles using
    // D3D11VA, let's reject them here so we can select a different Vulkan device or
    // just allow D3D11VA to take over.
    if (m_HwAccelBackend && deviceProps->vendorID == 0x8086 && !qEnvironmentVariableIntValue("PLVK_ALLOW_INTEL")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Skipping Intel GPU for Vulkan Video due to broken drivers");
        return false;
    }
#endif

    // If we're acting as the decoder backend, we need a physical device with Vulkan video support
    if (m_HwAccelBackend) {
        const char* videoDecodeExtension;

        if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_H264) {
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME;
        }
        else if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_H265) {
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME;
        }
        else if (decoderParams->videoFormat & VIDEO_FORMAT_MASK_AV1) {
            // FFmpeg 6.1 implemented an early Mesa extension for Vulkan AV1 decoding.
            // FFmpeg 7.0 replaced that implementation with one based on the official extension.
#if LIBAVCODEC_VERSION_MAJOR >= 61
            videoDecodeExtension = VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME;
#else
            videoDecodeExtension = "VK_MESA_video_decode_av1";
#endif
        }
        else {
            SDL_assert(false);
            return false;
        }

        if (!isExtensionSupportedByPhysicalDevice(device, videoDecodeExtension)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vulkan device '%s' does not support %s",
                        deviceProps->deviceName,
                        videoDecodeExtension);
            return false;
        }
    }

    if (!isSurfacePresentationSupportedByPhysicalDevice(device)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not support presenting on window surface",
                    deviceProps->deviceName);
        return false;
    }

    if (hdrOutputRequired && !isColorSpaceSupportedByPhysicalDevice(device, VK_COLOR_SPACE_HDR10_ST2084_EXT)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' does not support HDR10 (ST.2084 PQ)",
                    deviceProps->deviceName);
        return false;
    }

    // Avoid software GPUs
    if (deviceProps->deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && qgetenv("PLVK_ALLOW_SOFTWARE") != "1") {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Vulkan device '%s' is a (probably slow) software renderer. Set PLVK_ALLOW_SOFTWARE=1 to allow using this device.",
                    deviceProps->deviceName);
        return false;
    }

    pl_vulkan_params vkParams = pl_vulkan_default_params;
    vkParams.instance = m_PlVkInstance->instance;
    vkParams.get_proc_addr = m_PlVkInstance->get_proc_addr;
    vkParams.surface = m_VkSurface;
    vkParams.device = device;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 26, 100)
    vkParams.opt_extensions = av_vk_get_optional_device_extensions(&vkParams.num_opt_extensions);
#else
    vkParams.opt_extensions = k_OptionalDeviceExtensions;
    vkParams.num_opt_extensions = SDL_arraysize(k_OptionalDeviceExtensions);
#endif
    vkParams.extra_queues = m_HwAccelBackend ? VK_QUEUE_FLAG_BITS_MAX_ENUM : 0;
    m_Vulkan = pl_vulkan_create(m_Log, &vkParams);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 26, 100)
    av_free((void*)vkParams.opt_extensions);
#endif
    if (m_Vulkan == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vulkan_create() failed for '%s'",
                     deviceProps->deviceName);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Vulkan rendering device chosen: %s",
                deviceProps->deviceName);
    return true;
}

bool PlVkRenderer::isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char *extensionName)
{
    uint32_t extensionCount = 0;
    fn_vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    fn_vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());

    for (const VkExtensionProperties& extension : extensions) {
        if (strcmp(extension.extensionName, extensionName) == 0) {
            return true;
        }
    }

    return false;
}

#define POPULATE_FUNCTION(name) \
    fn_##name = (PFN_##name)m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, #name); \
    if (fn_##name == nullptr) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, \
                     "Missing required Vulkan function: " #name); \
        return false; \
    }

bool PlVkRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    unsigned int instanceExtensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(params->window, &instanceExtensionCount, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Vulkan_GetInstanceExtensions() #1 failed: %s",
                     SDL_GetError());
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }

    std::vector<const char*> instanceExtensions(instanceExtensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(params->window, &instanceExtensionCount, instanceExtensions.data())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Vulkan_GetInstanceExtensions() #2 failed: %s",
                     SDL_GetError());
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }

    pl_vk_inst_params vkInstParams = pl_vk_inst_default_params;
    {
        vkInstParams.debug_extra = !!qEnvironmentVariableIntValue("PLVK_DEBUG_EXTRA");
        vkInstParams.debug = vkInstParams.debug_extra || !!qEnvironmentVariableIntValue("PLVK_DEBUG");
    }
    vkInstParams.get_proc_addr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    vkInstParams.extensions = instanceExtensions.data();
    vkInstParams.num_extensions = (int)instanceExtensions.size();
    m_PlVkInstance = pl_vk_inst_create(m_Log, &vkInstParams);
    if (m_PlVkInstance == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vk_inst_create() failed");
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }

    // Lookup all Vulkan functions we require
    POPULATE_FUNCTION(vkDestroySurfaceKHR);
    POPULATE_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties2);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfacePresentModesKHR);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR);
    POPULATE_FUNCTION(vkEnumeratePhysicalDevices);
    POPULATE_FUNCTION(vkGetPhysicalDeviceProperties);
    POPULATE_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR);
    POPULATE_FUNCTION(vkEnumerateDeviceExtensionProperties);

    if (!SDL_Vulkan_CreateSurface(params->window, m_PlVkInstance->instance, &m_VkSurface)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_Vulkan_CreateSurface() failed: %s",
                     SDL_GetError());
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }

    // Enumerate physical devices and choose one that is suitable for our needs.
    //
    // For HDR streaming, we try to find an HDR-capable Vulkan device first then
    // try another search without the HDR requirement if the first attempt fails.
    if (!chooseVulkanDevice(params, params->videoFormat & VIDEO_FORMAT_MASK_10BIT) &&
        (!(params->videoFormat & VIDEO_FORMAT_MASK_10BIT) || !chooseVulkanDevice(params, false))) {
        return false;
    }

    VkPresentModeKHR presentMode;
    if (params->enableVsync) {
        // FIFO mode improves frame pacing compared with Mailbox, especially for
        // platforms like X11 that lack a VSyncSource implementation for Pacer.
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    else {
        // We want immediate mode for V-Sync disabled if possible
        if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Using Immediate present mode with V-Sync disabled");
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Immediate present mode is not supported by the Vulkan driver. Latency may be higher than normal with V-Sync disabled.");

            // FIFO Relaxed can tear if the frame is running late
            if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using FIFO Relaxed present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
            }
            // Mailbox at least provides non-blocking behavior
            else if (isPresentModeSupportedByPhysicalDevice(m_Vulkan->phys_device, VK_PRESENT_MODE_MAILBOX_KHR)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using Mailbox present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            // FIFO is always supported
            else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "Using FIFO present mode with V-Sync disabled");
                presentMode = VK_PRESENT_MODE_FIFO_KHR;
            }
        }
    }

    pl_vulkan_swapchain_params vkSwapchainParams = {};
    vkSwapchainParams.surface = m_VkSurface;
    vkSwapchainParams.present_mode = presentMode;
    vkSwapchainParams.swapchain_depth = 1; // No queued frames
#if PL_API_VER >= 338
    vkSwapchainParams.disable_10bit_sdr = true; // Some drivers don't dither 10-bit SDR output correctly
#endif
    m_Swapchain = pl_vulkan_create_swapchain(m_Vulkan, &vkSwapchainParams);
    if (m_Swapchain == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_vulkan_create_swapchain() failed");
        return false;
    }

    m_Renderer = pl_renderer_create(m_Log, m_Vulkan->gpu);
    if (m_Renderer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_renderer_create() failed");
        return false;
    }

    // We only need an hwaccel device context if we're going to act as the backend renderer too
    if (m_HwAccelBackend) {
        m_HwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (m_HwDeviceCtx == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN) failed");
            return false;
        }

        auto hwDeviceContext = ((AVHWDeviceContext *)m_HwDeviceCtx->data);
        hwDeviceContext->user_opaque = this; // Used by lockQueue()/unlockQueue()

        auto vkDeviceContext = (AVVulkanDeviceContext*)((AVHWDeviceContext *)m_HwDeviceCtx->data)->hwctx;
        vkDeviceContext->get_proc_addr = m_PlVkInstance->get_proc_addr;
        vkDeviceContext->inst = m_PlVkInstance->instance;
        vkDeviceContext->phys_dev = m_Vulkan->phys_device;
        vkDeviceContext->act_dev = m_Vulkan->device;
        vkDeviceContext->device_features = *m_Vulkan->features;
        vkDeviceContext->enabled_inst_extensions = m_PlVkInstance->extensions;
        vkDeviceContext->nb_enabled_inst_extensions = m_PlVkInstance->num_extensions;
        vkDeviceContext->enabled_dev_extensions = m_Vulkan->extensions;
        vkDeviceContext->nb_enabled_dev_extensions = m_Vulkan->num_extensions;
#if LIBAVUTIL_VERSION_INT > AV_VERSION_INT(58, 9, 100)
        vkDeviceContext->lock_queue = lockQueue;
        vkDeviceContext->unlock_queue = unlockQueue;
#endif

        // Populate the device queues for decoding this video format
        populateQueues(params->videoFormat);

        int err = av_hwdevice_ctx_init(m_HwDeviceCtx);
        if (err < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "av_hwdevice_ctx_init() failed: %d",
                         err);
            return false;
        }
    }

    // §J.3.e.1.d — opt-in: hand libplacebo's VkInstance/PhysDev/Device to
    // ncnn so the future §J.3.e FRUC integration can run on the same VkDevice
    // as the decoder + renderer (no cross-device shared-memory dance).
    if (qEnvironmentVariableIntValue("VIPLE_PLVK_NCNN_HANDOFF") != 0) {
        if (!initializeNcnnExternalHandoff()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PLVK-NCNN] §J.3.e.1.d handoff failed — "
                        "PlVkRenderer continues without ncnn integration");
            // Non-fatal: PlVkRenderer still works, just no NCNN backend
            // available on this VkDevice for §J.3.e to wire up.
        }
    }

    return true;
}

bool PlVkRenderer::initializeNcnnExternalHandoff()
{
    if (m_NcnnExternalReady) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PLVK-NCNN] §J.3.e.1.d handoff: already initialised");
        return true;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PLVK-NCNN] §J.3.e.1.d handoff: starting "
                "(libplacebo VkInstance=%p, VkPhysDev=%p, VkDevice=%p)",
                (void*)m_PlVkInstance->instance,
                (void*)m_Vulkan->phys_device,
                (void*)m_Vulkan->device);

    // libplacebo's queue_compute / queue_graphics / queue_transfer expose
    // .index (queue family idx) and .count (queue count from VkDeviceQueueCreateInfo).
    int rc = ncnn::create_gpu_instance_external(
        m_PlVkInstance->instance,
        m_Vulkan->phys_device,
        m_Vulkan->device,
        m_Vulkan->queue_compute.index,  m_Vulkan->queue_compute.count,
        m_Vulkan->queue_graphics.index, m_Vulkan->queue_graphics.count,
        m_Vulkan->queue_transfer.index, m_Vulkan->queue_transfer.count);
    if (rc != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PLVK-NCNN] §J.3.e.1.d ncnn::create_gpu_instance_external failed rc=%d "
                    "(ncnn singleton may already be claimed by another caller)",
                    rc);
        return false;
    }

    // Verify the API contract: ncnn's internal d->device must be exactly the
    // VkDevice we handed in.  This is the load-bearing assertion of §J.3.e.1.
    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(0);
    if (!vkdev) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PLVK-NCNN] §J.3.e.1.d ncnn::get_gpu_device(0) returned nullptr");
        ncnn::destroy_gpu_instance();
        return false;
    }
    VkDevice ncnnDevice = vkdev->vkdevice();
    if (ncnnDevice != m_Vulkan->device) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PLVK-NCNN] §J.3.e.1.d VkDevice mismatch — "
                     "ncnn d->device=%p, libplacebo m_Vulkan->device=%p",
                     (void*)ncnnDevice, (void*)m_Vulkan->device);
        ncnn::destroy_gpu_instance();
        return false;
    }

    const ncnn::GpuInfo& gi = ncnn::get_gpu_info(0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PLVK-NCNN] §J.3.e.1.d handoff: PASS — "
                "ncnn d->device=%p == m_Vulkan->device=%p; "
                "GpuInfo.name='%s' subgroup=%u fp16-arith=%d "
                "queueC=%u/%u queueG=%u/%u queueT=%u/%u",
                (void*)ncnnDevice, (void*)m_Vulkan->device,
                gi.device_name(), gi.subgroup_size(),
                gi.support_fp16_arithmetic() ? 1 : 0,
                gi.compute_queue_family_index(), gi.compute_queue_count(),
                gi.graphics_queue_family_index(), gi.graphics_queue_count(),
                gi.transfer_queue_family_index(), gi.transfer_queue_count());

    m_NcnnExternalReady = true;
    return true;
}

void PlVkRenderer::teardownNcnnExternalHandoff()
{
    if (!m_NcnnExternalReady) return;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PLVK-NCNN] §J.3.e.1.d teardown: ncnn::destroy_gpu_instance "
                "(BEFORE pl_vulkan_destroy)");
    ncnn::destroy_gpu_instance();
    m_NcnnExternalReady = false;
}

// §J.3.e.2.a — layout transition + 1-pixel readback probe.
//
// Probe goal: prove cross-queue-family ownership transfer
// (decode_qf → compute_qf) and VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR →
// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL transition work on production
// drivers.  This is the load-bearing primitive for §J.3.e.2.b/c —
// the NV12→RGB compute shader needs the source image readable from a
// compute-queue command buffer.
//
// Sync model: ffmpeg's hwcontext_vulkan exposes per-frame timeline
// VkSemaphores on AVVkFrame.sem[i] / sem_value[i].  Consumers must wait on
// sem_value[i] before any submit, and signal sem_value[i]+1 after, then
// store the new value back so ffmpeg knows to wait when reusing the slot.
//
// Queue ownership: we use VK_QUEUE_FAMILY_IGNORED for both sides of the
// barrier — ffmpeg's hwcontext_vulkan creates AVVkFrames with sharing
// flags that allow consumption across queue families it knows about
// (compute / transfer / video).  If validation layers complain on a
// future driver, we'll revisit with explicit release-on-decode-queue +
// acquire-on-compute-queue barriers.

#include <vulkan/vulkan.h>

bool PlVkRenderer::initFrucProbeResources()
{
    if (m_FrucProbeInitialised || m_FrucProbeDisabled) return m_FrucProbeInitialised;
    if (!m_Vulkan || !m_PlVkInstance) {
        m_FrucProbeDisabled = true;
        return false;
    }

    // Dynamically load Vulkan device entries via libplacebo's instance proc addr.
    // (moonlight-qt doesn't link vulkan-1.lib directly.)
    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        // vkGetDeviceProcAddr is itself an instance-level entry; load via
        // libplacebo's get_proc_addr (= vkGetInstanceProcAddr).
        static PFN_vkGetDeviceProcAddr s_pfnGetDeviceProcAddr = nullptr;
        if (!s_pfnGetDeviceProcAddr) {
            s_pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
                m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetDeviceProcAddr");
        }
        return s_pfnGetDeviceProcAddr ? s_pfnGetDeviceProcAddr(m_Vulkan->device, name) : nullptr;
    };

    auto pfnCreateCommandPool = (PFN_vkCreateCommandPool)getDevProc("vkCreateCommandPool");
    auto pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)getDevProc("vkAllocateCommandBuffers");
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevProc("vkCreateBuffer");
    auto pfnGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)getDevProc("vkGetBufferMemoryRequirements");
    auto pfnAllocateMemory = (PFN_vkAllocateMemory)getDevProc("vkAllocateMemory");
    auto pfnBindBufferMemory = (PFN_vkBindBufferMemory)getDevProc("vkBindBufferMemory");
    auto pfnCreateFence = (PFN_vkCreateFence)getDevProc("vkCreateFence");

    auto pfnGetPhysProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetPhysicalDeviceMemoryProperties");

    if (!pfnCreateCommandPool || !pfnAllocateCommandBuffers || !pfnCreateBuffer
        || !pfnGetBufferMemoryRequirements || !pfnAllocateMemory || !pfnBindBufferMemory
        || !pfnCreateFence || !pfnGetPhysProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: missing Vulkan PFN");
        m_FrucProbeDisabled = true;
        return false;
    }

    // Command pool on compute queue family — we'll submit on libplacebo's
    // compute queue via lock_queue/unlock_queue.
    VkCommandPoolCreateInfo poolCi = {};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = m_Vulkan->queue_compute.index;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (pfnCreateCommandPool(m_Vulkan->device, &poolCi, nullptr, &pool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkCreateCommandPool failed");
        m_FrucProbeDisabled = true;
        return false;
    }
    m_FrucProbeCmdPool = pool;

    VkCommandBufferAllocateInfo cbAlloc = {};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = pool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (pfnAllocateCommandBuffers(m_Vulkan->device, &cbAlloc, &cb) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkAllocateCommandBuffers failed");
        m_FrucProbeDisabled = true;
        return false;
    }
    m_FrucProbeCmdBuf = cb;

    // Tiny host-visible buffer for 1-texel readback.
    VkBufferCreateInfo bufCi = {};
    bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCi.size = 16;  // 4 bytes for Y, padded
    bufCi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (pfnCreateBuffer(m_Vulkan->device, &bufCi, nullptr, &buf) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkCreateBuffer failed");
        m_FrucProbeDisabled = true;
        return false;
    }
    m_FrucProbeBuffer = buf;

    VkMemoryRequirements memReq = {};
    pfnGetBufferMemoryRequirements(m_Vulkan->device, buf, &memReq);

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPhysProps(m_Vulkan->phys_device, &memProps);

    uint32_t memTypeIdx = UINT32_MAX;
    const VkMemoryPropertyFlags wantFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & wantFlags) == wantFlags) {
            memTypeIdx = i;
            break;
        }
    }
    if (memTypeIdx == UINT32_MAX) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: no host-coherent memory type");
        m_FrucProbeDisabled = true;
        return false;
    }

    VkMemoryAllocateInfo memAlloc = {};
    memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAlloc.allocationSize = memReq.size;
    memAlloc.memoryTypeIndex = memTypeIdx;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (pfnAllocateMemory(m_Vulkan->device, &memAlloc, nullptr, &mem) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkAllocateMemory failed");
        m_FrucProbeDisabled = true;
        return false;
    }
    m_FrucProbeBufferMem = mem;

    if (pfnBindBufferMemory(m_Vulkan->device, buf, mem, 0) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkBindBufferMemory failed");
        m_FrucProbeDisabled = true;
        return false;
    }

    VkFenceCreateInfo fenceCi = {};
    fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (pfnCreateFence(m_Vulkan->device, &fenceCi, nullptr, &fence) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: vkCreateFence failed");
        m_FrucProbeDisabled = true;
        return false;
    }
    m_FrucProbeFence = fence;

    m_FrucProbeInitialised = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.a probe init: ready (compute_qf=%u)",
                m_Vulkan->queue_compute.index);
    return true;
}

void PlVkRenderer::destroyFrucProbeResources()
{
    if (!m_FrucProbeInitialised && !m_FrucProbeCmdPool && !m_FrucProbeBuffer
        && !m_FrucProbeBufferMem && !m_FrucProbeFence) return;
    if (!m_Vulkan || !m_PlVkInstance) return;

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        static PFN_vkGetDeviceProcAddr s_pfnGetDeviceProcAddr = nullptr;
        if (!s_pfnGetDeviceProcAddr) {
            s_pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
                m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetDeviceProcAddr");
        }
        return s_pfnGetDeviceProcAddr ? s_pfnGetDeviceProcAddr(m_Vulkan->device, name) : nullptr;
    };

    auto pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)getDevProc("vkDestroyCommandPool");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevProc("vkDestroyBuffer");
    auto pfnFreeMemory = (PFN_vkFreeMemory)getDevProc("vkFreeMemory");
    auto pfnDestroyFence = (PFN_vkDestroyFence)getDevProc("vkDestroyFence");

    if (m_FrucProbeFence && pfnDestroyFence) {
        pfnDestroyFence(m_Vulkan->device, (VkFence)m_FrucProbeFence, nullptr);
    }
    if (m_FrucProbeBuffer && pfnDestroyBuffer) {
        pfnDestroyBuffer(m_Vulkan->device, (VkBuffer)m_FrucProbeBuffer, nullptr);
    }
    if (m_FrucProbeBufferMem && pfnFreeMemory) {
        pfnFreeMemory(m_Vulkan->device, (VkDeviceMemory)m_FrucProbeBufferMem, nullptr);
    }
    if (m_FrucProbeCmdPool && pfnDestroyCommandPool) {
        // Frees command buffers allocated from the pool too.
        pfnDestroyCommandPool(m_Vulkan->device, (VkCommandPool)m_FrucProbeCmdPool, nullptr);
    }

    m_FrucProbeFence = nullptr;
    m_FrucProbeBuffer = nullptr;
    m_FrucProbeBufferMem = nullptr;
    m_FrucProbeCmdBuf = nullptr;
    m_FrucProbeCmdPool = nullptr;
    m_FrucProbeInitialised = false;
}

bool PlVkRenderer::runLayoutTransitionProbe(AVVkFrame* vkFrame, AVFrame* frame)
{
    if (m_FrucProbeDisabled) return false;
    if (!m_FrucProbeInitialised && !initFrucProbeResources()) return false;
    if (!vkFrame || !vkFrame->img[0]) return false;

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        static PFN_vkGetDeviceProcAddr s_pfnGetDeviceProcAddr = nullptr;
        if (!s_pfnGetDeviceProcAddr) {
            s_pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
                m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetDeviceProcAddr");
        }
        return s_pfnGetDeviceProcAddr ? s_pfnGetDeviceProcAddr(m_Vulkan->device, name) : nullptr;
    };

    static auto pfnBegin = (PFN_vkBeginCommandBuffer)getDevProc("vkBeginCommandBuffer");
    static auto pfnEnd = (PFN_vkEndCommandBuffer)getDevProc("vkEndCommandBuffer");
    static auto pfnReset = (PFN_vkResetCommandBuffer)getDevProc("vkResetCommandBuffer");
    static auto pfnBarrier = (PFN_vkCmdPipelineBarrier)getDevProc("vkCmdPipelineBarrier");
    static auto pfnCopyImgBuf = (PFN_vkCmdCopyImageToBuffer)getDevProc("vkCmdCopyImageToBuffer");
    static auto pfnGetQueue = (PFN_vkGetDeviceQueue)getDevProc("vkGetDeviceQueue");
    static auto pfnSubmit = (PFN_vkQueueSubmit)getDevProc("vkQueueSubmit");
    static auto pfnWaitFences = (PFN_vkWaitForFences)getDevProc("vkWaitForFences");
    static auto pfnResetFences = (PFN_vkResetFences)getDevProc("vkResetFences");
    static auto pfnMap = (PFN_vkMapMemory)getDevProc("vkMapMemory");
    static auto pfnUnmap = (PFN_vkUnmapMemory)getDevProc("vkUnmapMemory");

    if (!pfnBegin || !pfnEnd || !pfnReset || !pfnBarrier || !pfnCopyImgBuf
        || !pfnGetQueue || !pfnSubmit || !pfnWaitFences || !pfnResetFences
        || !pfnMap || !pfnUnmap) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe: missing PFN — disabling");
        m_FrucProbeDisabled = true;
        return false;
    }

    VkCommandBuffer cb = (VkCommandBuffer)m_FrucProbeCmdBuf;
    VkImage srcImg = (VkImage)vkFrame->img[0];
    VkImageLayout origLayout = vkFrame->layout[0];

    pfnReset(cb, 0);

    VkCommandBufferBeginInfo bbi = {};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBegin(cb, &bbi);

    // Barrier 1: origLayout (probably VIDEO_DECODE_DPB_KHR) → TRANSFER_SRC_OPTIMAL,
    // plane 0 only.  Use VK_QUEUE_FAMILY_IGNORED — trust the AVVkFrame timeline
    // semaphore for cross-queue sync.
    VkImageMemoryBarrier toSrcBarrier = {};
    toSrcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrcBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    toSrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrcBarrier.oldLayout = origLayout;
    toSrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrcBarrier.image = srcImg;
    toSrcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    toSrcBarrier.subresourceRange.baseMipLevel = 0;
    toSrcBarrier.subresourceRange.levelCount = 1;
    toSrcBarrier.subresourceRange.baseArrayLayer = 0;
    toSrcBarrier.subresourceRange.layerCount = 1;

    pfnBarrier(cb,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, 0, nullptr, 0, nullptr, 1, &toSrcBarrier);

    // Copy 1 texel (top-left luma sample) from plane 0 → host buffer.
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {1, 1, 1};
    pfnCopyImgBuf(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucProbeBuffer, 1, &region);

    // Barrier 2: restore plane 0 to origLayout for ffmpeg's next access.
    VkImageMemoryBarrier toOrigBarrier = toSrcBarrier;
    toOrigBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toOrigBarrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    toOrigBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toOrigBarrier.newLayout = origLayout;
    pfnBarrier(cb,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               0, 0, nullptr, 0, nullptr, 1, &toOrigBarrier);

    pfnEnd(cb);

    // Submit on compute queue (libplacebo lock).  Wait on AVVkFrame.sem[0]
    // timeline = sem_value[0]; signal sem_value[0]+1.  Also signal local fence
    // for synchronous host wait.
    VkQueue computeQueue = VK_NULL_HANDLE;
    pfnGetQueue(m_Vulkan->device, m_Vulkan->queue_compute.index, 0, &computeQueue);

    uint64_t waitVal = vkFrame->sem_value[0];
    uint64_t signalVal = waitVal + 1;
    VkSemaphore sem = vkFrame->sem[0];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    VkTimelineSemaphoreSubmitInfo tlInfo = {};
    tlInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tlInfo.waitSemaphoreValueCount = 1;
    tlInfo.pWaitSemaphoreValues = &waitVal;
    tlInfo.signalSemaphoreValueCount = 1;
    tlInfo.pSignalSemaphoreValues = &signalVal;

    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tlInfo;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sem;

    m_Vulkan->lock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);
    VkResult subRes = pfnSubmit(computeQueue, 1, &si, (VkFence)m_FrucProbeFence);
    m_Vulkan->unlock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);

    if (subRes != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe: vkQueueSubmit rc=%d — disabling",
                    (int)subRes);
        m_FrucProbeDisabled = true;
        return false;
    }
    // Bump the AVVkFrame timeline so subsequent ffmpeg / consumer waits
    // pick up our signal.
    vkFrame->sem_value[0] = signalVal;

    VkFence fence = (VkFence)m_FrucProbeFence;
    VkResult waitRes = pfnWaitFences(m_Vulkan->device, 1, &fence, VK_TRUE,
                                      /* timeout: 1 sec */ 1000ULL * 1000 * 1000);
    if (waitRes != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.a probe: vkWaitForFences rc=%d — disabling",
                    (int)waitRes);
        m_FrucProbeDisabled = true;
        return false;
    }
    pfnResetFences(m_Vulkan->device, 1, &fence);

    // Read one luma byte and log.
    void* mapped = nullptr;
    pfnMap(m_Vulkan->device, (VkDeviceMemory)m_FrucProbeBufferMem, 0, VK_WHOLE_SIZE, 0, &mapped);
    uint8_t y = mapped ? *(uint8_t*)mapped : 0;
    if (mapped) pfnUnmap(m_Vulkan->device, (VkDeviceMemory)m_FrucProbeBufferMem);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.a probe: PASS  size=%dx%d  origLayout=%d  "
                "topleft Y=%u (raw NV12 luma byte)",
                frame->width, frame->height, (int)origLayout, (unsigned)y);
    return true;
}

bool PlVkRenderer::prepareDecoderContext(AVCodecContext *context, AVDictionary **)
{
    if (m_HwAccelBackend) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Vulkan video decoding");

        context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Vulkan renderer");
    }

    return true;
}

bool PlVkRenderer::mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame)
{
    pl_avframe_params mapParams = {};
    mapParams.frame = frame;
    mapParams.tex = m_Textures;
    if (!pl_map_avframe_ex(m_Vulkan->gpu, mappedFrame, &mapParams)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_map_avframe_ex() failed");
        return false;
    }

    // libplacebo assumes a minimum luminance value of 0 means the actual value was unknown.
    // Since we assume the host values are correct, we use the PL_COLOR_HDR_BLACK constant to
    // indicate infinite contrast.
    //
    // NB: We also have to check that the AVFrame actually had metadata in the first place,
    // because libplacebo may infer metadata if the frame didn't have any.
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) && !mappedFrame->color.hdr.min_luma) {
        mappedFrame->color.hdr.min_luma = PL_COLOR_HDR_BLACK;
    }

    // HACK: AMF AV1 encoding on the host PC does not set full color range properly in the
    // bitstream data, so libplacebo incorrectly renders the content as limited range.
    //
    // As a workaround, set full range manually in the mapped frame ourselves.
    mappedFrame->repr.levels = PL_COLOR_LEVELS_FULL;

    return true;
}

bool PlVkRenderer::populateQueues(int videoFormat)
{
    auto vkDeviceContext = (AVVulkanDeviceContext*)((AVHWDeviceContext *)m_HwDeviceCtx->data)->hwctx;

    uint32_t queueFamilyCount = 0;
    fn_vkGetPhysicalDeviceQueueFamilyProperties2(m_Vulkan->phys_device, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties2> queueFamilies(queueFamilyCount);
    std::vector<VkQueueFamilyVideoPropertiesKHR> queueFamilyVideoProps(queueFamilyCount);
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        queueFamilyVideoProps[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
        queueFamilies[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        queueFamilies[i].pNext = &queueFamilyVideoProps[i];
    }

    fn_vkGetPhysicalDeviceQueueFamilyProperties2(m_Vulkan->phys_device, &queueFamilyCount, queueFamilies.data());

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
    Q_UNUSED(videoFormat);

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        vkDeviceContext->qf[i].idx = i;
        vkDeviceContext->qf[i].num = queueFamilies[i].queueFamilyProperties.queueCount;
        vkDeviceContext->qf[i].flags = (VkQueueFlagBits)queueFamilies[i].queueFamilyProperties.queueFlags;
        vkDeviceContext->qf[i].video_caps = (VkVideoCodecOperationFlagBitsKHR)queueFamilyVideoProps[i].videoCodecOperations;
    }
    vkDeviceContext->nb_qf = queueFamilyCount;
#else
    vkDeviceContext->queue_family_index = m_Vulkan->queue_graphics.index;
    vkDeviceContext->nb_graphics_queues = m_Vulkan->queue_graphics.count;
    vkDeviceContext->queue_family_tx_index = m_Vulkan->queue_transfer.index;
    vkDeviceContext->nb_tx_queues = m_Vulkan->queue_transfer.count;
    vkDeviceContext->queue_family_comp_index = m_Vulkan->queue_compute.index;
    vkDeviceContext->nb_comp_queues = m_Vulkan->queue_compute.count;

    // Select a video decode queue that is capable of decoding our chosen format
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            if (videoFormat & VIDEO_FORMAT_MASK_H264) {
                if (queueFamilyVideoProps[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
                    vkDeviceContext->queue_family_decode_index = i;
                    vkDeviceContext->nb_decode_queues = queueFamilies[i].queueFamilyProperties.queueCount;
                    break;
                }
            }
            else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
                if (queueFamilyVideoProps[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
                    vkDeviceContext->queue_family_decode_index = i;
                    vkDeviceContext->nb_decode_queues = queueFamilies[i].queueFamilyProperties.queueCount;
                    break;
                }
            }
            else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
#if LIBAVCODEC_VERSION_MAJOR >= 61
                // VK_KHR_video_decode_av1 added VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR to check for AV1
                // decoding support on this queue. Since FFmpeg 6.1 used the older Mesa-specific AV1 extension,
                // we'll just assume all video decode queues on this device support AV1 (since we checked that
                // the physical device supports it earlier.
                if (queueFamilyVideoProps[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR)
#endif
                {
                    vkDeviceContext->queue_family_decode_index = i;
                    vkDeviceContext->nb_decode_queues = queueFamilies[i].queueFamilyProperties.queueCount;
                    break;
                }
            }
            else {
                SDL_assert(false);
            }
        }
    }

    if (vkDeviceContext->queue_family_decode_index < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to find compatible video decode queue!");
        return false;
    }
#endif

    return true;
}

bool PlVkRenderer::isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode)
{
    uint32_t presentModeCount = 0;
    fn_vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_VkSurface, &presentModeCount, nullptr);

    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    fn_vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_VkSurface, &presentModeCount, presentModes.data());

    for (uint32_t i = 0; i < presentModeCount; i++) {
        if (presentModes[i] == presentMode) {
            return true;
        }
    }

    return false;
}

bool PlVkRenderer::isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace)
{
    uint32_t formatCount = 0;
    fn_vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_VkSurface, &formatCount, nullptr);

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    fn_vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_VkSurface, &formatCount, formats.data());

    for (uint32_t i = 0; i < formatCount; i++) {
        if (formats[i].colorSpace == colorSpace) {
            return true;
        }
    }

    return false;
}

bool PlVkRenderer::isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device)
{
    uint32_t queueFamilyCount = 0;
    fn_vkGetPhysicalDeviceQueueFamilyProperties2(device, &queueFamilyCount, nullptr);

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        VkBool32 supported = VK_FALSE;
        if (fn_vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_VkSurface, &supported) == VK_SUCCESS && supported == VK_TRUE) {
            return true;
        }
    }

    return false;
}

void PlVkRenderer::waitToRender()
{
    // Check if the GPU has failed before doing anything else
    if (pl_gpu_is_failed(m_Vulkan->gpu)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "GPU is in failed state. Recreating renderer.");
        SDL_Event event;
        event.type = SDL_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        return;
    }

#ifndef Q_OS_WIN32
    // With libplacebo's Vulkan backend, all swap_buffers does is wait for queued
    // presents to finish. This happens to be exactly what we want to do here, since
    // it lets us wait to select a queued frame for rendering until we know that we
    // can present without blocking in renderFrame().
    //
    // NB: This seems to cause performance problems with the Windows display stack
    // (particularly on Nvidia) so we will only do this for non-Windows platforms.
    pl_swapchain_swap_buffers(m_Swapchain);
#endif

    // Handle the swapchain being resized
    int vkDrawableW, vkDrawableH;
    SDL_Vulkan_GetDrawableSize(m_Window, &vkDrawableW, &vkDrawableH);
    if (!pl_swapchain_resize(m_Swapchain, &vkDrawableW, &vkDrawableH)) {
        // Swapchain (re)creation can fail if the window is occluded
        return;
    }

    // Get the next swapchain buffer for rendering. If this fails, renderFrame()
    // will try again.
    //
    // NB: After calling this successfully, we *MUST* call pl_swapchain_submit_frame(),
    // hence the implementation of cleanupRenderContext() which does just this in case
    // renderFrame() wasn't called after waitToRender().
    if (pl_swapchain_start_frame(m_Swapchain, &m_SwapchainFrame)) {
        m_HasPendingSwapchainFrame = true;
    }
}

void PlVkRenderer::cleanupRenderContext()
{
    // We have to submit a pending swapchain frame before shutting down
    // in order to release a mutex that pl_swapchain_start_frame() acquires.
    if (m_HasPendingSwapchainFrame) {
        pl_swapchain_submit_frame(m_Swapchain);
        m_HasPendingSwapchainFrame = false;
    }
}

void PlVkRenderer::renderFrame(AVFrame *frame)
{
    pl_frame mappedFrame, targetFrame;

    // §J.3.e.0 — AVVkFrame access probe.  When VIPLE_VK_FRUC_PROBE=1
    // env var is set, log the VkImage handles every 60 frames.  This
    // proves the hook point for FRUC integration: at this stage in
    // renderFrame, the AVFrame carries an AVVkFrame whose img[] array
    // points to fully-decoded VkImages on m_Vulkan->device.  §J.3.e.1+
    // will pass these handles to NCNN-Vulkan without any D3D11 bridge.
    if (frame->format == AV_PIX_FMT_VULKAN) {
        const char* probe = SDL_getenv("VIPLE_VK_FRUC_PROBE");
        if (probe && SDL_atoi(probe) != 0) {
            static std::atomic<uint64_t> s_FrameCount{0};
            uint64_t n = s_FrameCount.fetch_add(1, std::memory_order_relaxed);
            if ((n % 60) == 0) {
                auto* vkFrame = (AVVkFrame*)frame->data[0];
                if (vkFrame) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VK-FRUC] §J.3.e.0 frame#%llu AVVkFrame "
                                "img[0]=%p img[1]=%p layout[0]=%d layout[1]=%d "
                                "size=%dx%d sw_format=%d",
                                (unsigned long long)n,
                                (void*)vkFrame->img[0], (void*)vkFrame->img[1],
                                (int)vkFrame->layout[0], (int)vkFrame->layout[1],
                                frame->width, frame->height, (int)frame->format);
                }
            }
        }

        // §J.3.e.2.a — layout transition + 1-pixel readback probe.  Independent
        // of §J.3.e.0 (different env var).  Validates cross-queue-family ownership
        // transfer + VIDEO_DECODE_DPB → TRANSFER_SRC_OPTIMAL — primitive for
        // §J.3.e.2.b/c NV12→RGB compute shader.
        const char* probe2 = SDL_getenv("VIPLE_VK_FRUC_PROBE2");
        if (probe2 && SDL_atoi(probe2) != 0) {
            static std::atomic<uint64_t> s_Probe2FrameCount{0};
            uint64_t n = s_Probe2FrameCount.fetch_add(1, std::memory_order_relaxed);
            // First probe at frame 30 (give decoder time to stabilise),
            // then every 60 frames.
            if (n == 30 || (n > 30 && ((n - 30) % 60) == 0)) {
                auto* vkFrame = (AVVkFrame*)frame->data[0];
                if (vkFrame) {
                    runLayoutTransitionProbe(vkFrame, frame);
                }
            }
        }
    }

    // If waitToRender() failed to get the next swapchain frame, skip
    // rendering this frame. It probably means the window is occluded.
    if (!m_HasPendingSwapchainFrame) {
        return;
    }

    if (!mapAvFrameToPlacebo(frame, &mappedFrame)) {
        // This function logs internally
        return;
    }

    // Adjust the swapchain if the colorspace of incoming frames has changed
    if (!pl_color_space_equal(&mappedFrame.color, &m_LastColorspace)) {
        m_LastColorspace = mappedFrame.color;
        SDL_assert(pl_color_space_equal(&mappedFrame.color, &m_LastColorspace));
        pl_swapchain_colorspace_hint(m_Swapchain, &mappedFrame.color);
    }

    // Reserve enough space to avoid allocating under the overlay lock
    pl_overlay_part overlayParts[Overlay::OverlayMax] = {};
    std::vector<pl_tex> texturesToDestroy;
    std::vector<pl_overlay> overlays;
    texturesToDestroy.reserve(Overlay::OverlayMax);
    overlays.reserve(Overlay::OverlayMax);

    pl_frame_from_swapchain(&targetFrame, &m_SwapchainFrame);

    // We perform minimal processing under the overlay lock to avoid blocking threads updating the overlay
    SDL_AtomicLock(&m_OverlayLock);
    for (int i = 0; i < Overlay::OverlayMax; i++) {
        // If we have a staging overlay, we need to transfer ownership to us
        if (m_Overlays[i].hasStagingOverlay) {
            if (m_Overlays[i].hasOverlay) {
                texturesToDestroy.push_back(m_Overlays[i].overlay.tex);
            }

            // Copy the overlay fields from the staging area
            m_Overlays[i].overlay = m_Overlays[i].stagingOverlay;

            // We now own the staging overlay
            m_Overlays[i].hasStagingOverlay = false;
            SDL_zero(m_Overlays[i].stagingOverlay);
            m_Overlays[i].hasOverlay = true;
        }

        // If we have an overlay but it's been disabled, free the overlay texture
        if (m_Overlays[i].hasOverlay && !Session::get()->getOverlayManager().isOverlayEnabled((Overlay::OverlayType)i)) {
            texturesToDestroy.push_back(m_Overlays[i].overlay.tex);
            SDL_zero(m_Overlays[i].overlay);
            m_Overlays[i].hasOverlay = false;
        }

        // We have an overlay to draw
        if (m_Overlays[i].hasOverlay) {
            // Position the overlay
            overlayParts[i].src = { 0, 0, (float)m_Overlays[i].overlay.tex->params.w, (float)m_Overlays[i].overlay.tex->params.h };
            if (i == Overlay::OverlayStatusUpdate) {
                // Bottom Left
                overlayParts[i].dst.x0 = 0;
                overlayParts[i].dst.y0 = SDL_max(0, targetFrame.crop.y1 - overlayParts[i].src.y1);
            }
            else if (i == Overlay::OverlayDebug) {
                // Top left
                overlayParts[i].dst.x0 = 0;
                overlayParts[i].dst.y0 = 0;
            }
            overlayParts[i].dst.x1 = overlayParts[i].dst.x0 + overlayParts[i].src.x1;
            overlayParts[i].dst.y1 = overlayParts[i].dst.y0 + overlayParts[i].src.y1;

            m_Overlays[i].overlay.parts = &overlayParts[i];
            m_Overlays[i].overlay.num_parts = 1;

            overlays.push_back(m_Overlays[i].overlay);
        }
    }
    SDL_AtomicUnlock(&m_OverlayLock);

    SDL_Rect src;
    src.x = mappedFrame.crop.x0;
    src.y = mappedFrame.crop.y0;
    src.w = mappedFrame.crop.x1 - mappedFrame.crop.x0;
    src.h = mappedFrame.crop.y1 - mappedFrame.crop.y0;

    SDL_Rect dst;
    dst.x = targetFrame.crop.x0;
    dst.y = targetFrame.crop.y0;
    dst.w = targetFrame.crop.x1 - targetFrame.crop.x0;
    dst.h = targetFrame.crop.y1 - targetFrame.crop.y0;

    // Scale the video to the surface size while preserving the aspect ratio
    StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

    targetFrame.crop.x0 = dst.x;
    targetFrame.crop.y0 = dst.y;
    targetFrame.crop.x1 = dst.x + dst.w;
    targetFrame.crop.y1 = dst.y + dst.h;

    // Render the video image and overlays into the swapchain buffer
    targetFrame.num_overlays = (int)overlays.size();
    targetFrame.overlays = overlays.data();
    if (!pl_render_image(m_Renderer, &mappedFrame, &targetFrame, &pl_render_fast_params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_render_image() failed");
        // NB: We must fallthrough to call pl_swapchain_submit_frame()
    }

    // Submit the frame for display and swap buffers
    m_HasPendingSwapchainFrame = false;
    if (!pl_swapchain_submit_frame(m_Swapchain)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_swapchain_submit_frame() failed");

        // Recreate the renderer
        SDL_Event event;
        event.type = SDL_RENDER_DEVICE_RESET;
        SDL_PushEvent(&event);
        goto UnmapExit;
    }

#ifdef Q_OS_WIN32
    // On Windows, we swap buffers here instead of waitToRender()
    // to avoid some performance problems on Nvidia GPUs.
    pl_swapchain_swap_buffers(m_Swapchain);
#endif

UnmapExit:
    // Delete any textures that need to be destroyed
    for (pl_tex& texture : texturesToDestroy) {
        pl_tex_destroy(m_Vulkan->gpu, &texture);
    }

    pl_unmap_avframe(m_Vulkan->gpu, &mappedFrame);
}

bool PlVkRenderer::testRenderFrame(AVFrame *frame)
{
    // Test if the frame can be mapped to libplacebo
    pl_frame mappedFrame;
    if (!mapAvFrameToPlacebo(frame, &mappedFrame)) {
        return false;
    }

    pl_unmap_avframe(m_Vulkan->gpu, &mappedFrame);
    return true;
}

void PlVkRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
    if (newSurface == nullptr && Session::get()->getOverlayManager().isOverlayEnabled(type)) {
        // The overlay is enabled and there is no new surface. Leave the old texture alone.
        return;
    }

    SDL_AtomicLock(&m_OverlayLock);
    // We want to clear the staging overlay flag even if a staging overlay is still present,
    // since this ensures the render thread will not read from a partially initialized pl_tex
    // as we modify or recreate the staging overlay texture outside the overlay lock.
    m_Overlays[type].hasStagingOverlay = false;
    SDL_AtomicUnlock(&m_OverlayLock);

    // If there's no new staging overlay, free the old staging overlay texture.
    // NB: This is safe to do outside the overlay lock because we're guaranteed
    // to not have racing readers/writers if hasStagingOverlay is false.
    if (newSurface == nullptr) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        return;
    }

    // Find a compatible texture format
    SDL_assert(newSurface->format->format == SDL_PIXELFORMAT_ARGB8888);
    pl_fmt texFormat = pl_find_named_fmt(m_Vulkan->gpu, "bgra8");
    if (!texFormat) {
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_find_named_fmt(bgra8) failed");
        return;
    }

    // Create a new texture for this overlay if necessary, otherwise reuse the existing texture.
    // NB: We're guaranteed that the render thread won't be reading this concurrently because
    // we set hasStagingOverlay to false above.
    pl_tex_params texParams = {};
    texParams.w = newSurface->w;
    texParams.h = newSurface->h;
    texParams.format = texFormat;
    texParams.sampleable = true;
    texParams.host_writable = true;
    texParams.blit_src = !!(texFormat->caps & PL_FMT_CAP_BLITTABLE);
    texParams.debug_tag = PL_DEBUG_TAG;
    if (!pl_tex_recreate(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex, &texParams)) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_tex_recreate() failed");
        return;
    }

    // Upload the surface data to the new texture
    SDL_assert(!SDL_MUSTLOCK(newSurface));
    pl_tex_transfer_params xferParams = {};
    xferParams.tex = m_Overlays[type].stagingOverlay.tex;
    xferParams.row_pitch = (size_t)newSurface->pitch;
    xferParams.ptr = newSurface->pixels;
    xferParams.callback = overlayUploadComplete;
    xferParams.priv = newSurface;
    if (!pl_tex_upload(m_Vulkan->gpu, &xferParams)) {
        pl_tex_destroy(m_Vulkan->gpu, &m_Overlays[type].stagingOverlay.tex);
        SDL_zero(m_Overlays[type].stagingOverlay);
        SDL_FreeSurface(newSurface);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_tex_upload() failed");
        return;
    }

    // newSurface is now owned by the texture upload process. It will be freed in overlayUploadComplete()
    newSurface = nullptr;

    // Initialize the rest of the overlay params
    m_Overlays[type].stagingOverlay.mode = PL_OVERLAY_NORMAL;
    m_Overlays[type].stagingOverlay.coords = PL_OVERLAY_COORDS_DST_FRAME;
    m_Overlays[type].stagingOverlay.repr = pl_color_repr_rgb;
    m_Overlays[type].stagingOverlay.color = pl_color_space_srgb;

    // Make this staging overlay visible to the render thread
    SDL_AtomicLock(&m_OverlayLock);
    SDL_assert(!m_Overlays[type].hasStagingOverlay);
    m_Overlays[type].hasStagingOverlay = true;
    SDL_AtomicUnlock(&m_OverlayLock);
}

bool PlVkRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    // We can transparently handle size and display changes
    return !(info->stateChangeFlags & ~(WINDOW_STATE_CHANGE_SIZE | WINDOW_STATE_CHANGE_DISPLAY));
}

int PlVkRenderer::getRendererAttributes()
{
    // This renderer supports HDR (including tone mapping to SDR displays)
    return RENDERER_ATTRIBUTE_HDR_SUPPORT;
}

int PlVkRenderer::getDecoderColorspace()
{
    // We rely on libplacebo for color conversion, pick colorspace with the same primaries as sRGB
    return COLORSPACE_REC_709;
}

int PlVkRenderer::getDecoderColorRange()
{
    // Explicitly set the color range to full to fix raised black levels on OLED displays,
    // should also reduce banding artifacts in all situations
    return COLOR_RANGE_FULL;
}

int PlVkRenderer::getDecoderCapabilities()
{
    return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
           CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
}

bool PlVkRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat)
{
    if (m_HwAccelBackend) {
        return pixelFormat == AV_PIX_FMT_VULKAN;
    }
    else if (m_Backend) {
        return m_Backend->isPixelFormatSupported(videoFormat, pixelFormat);
    }
    else {
        if (pixelFormat == AV_PIX_FMT_VULKAN) {
            // Vulkan frames are always supported
            return true;
        }
        else if (videoFormat & VIDEO_FORMAT_MASK_YUV444) {
            if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
                switch (pixelFormat) {
                case AV_PIX_FMT_P410:
                case AV_PIX_FMT_YUV444P10:
                    return true;
                default:
                    return false;
                }
            }
            else {
                switch (pixelFormat) {
                case AV_PIX_FMT_NV24:
                case AV_PIX_FMT_NV42:
                case AV_PIX_FMT_YUV444P:
                case AV_PIX_FMT_YUVJ444P:
                    return true;
                default:
                    return false;
                }
            }
        }
        else if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
            switch (pixelFormat) {
            case AV_PIX_FMT_P010:
            case AV_PIX_FMT_YUV420P10:
                return true;
            default:
                return false;
            }
        }
        else {
            switch (pixelFormat) {
            case AV_PIX_FMT_NV12:
            case AV_PIX_FMT_NV21:
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                return true;
            default:
                return false;
            }
        }
    }
}

AVPixelFormat PlVkRenderer::getPreferredPixelFormat(int videoFormat)
{
    if (m_Backend) {
        return m_Backend->getPreferredPixelFormat(videoFormat);
    }
    else {
        return AV_PIX_FMT_VULKAN;
    }
}
