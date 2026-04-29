#include "plvk.h"

#include "streaming/session.h"
#include "streaming/streamutils.h"

// §J.3.e.1.d — handoff to ncnn via the new external VkDevice API.
#include <ncnn/gpu.h>
// §J.3.e.2.c — pipeline + shader compilation for NV12 → planar fp32 RGB.
#include <ncnn/pipeline.h>
#include <ncnn/option.h>

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

    // §J.3.e.2.d — reverse pipeline references §J.3.e.2.c bufRGB via descriptor
    // set; tear down BEFORE §J.3.e.2.c cleanup destroys bufRGB.
    destroyFrucRgbImgResources();
    // §J.3.e.2.c — VkPipeline / DSL / VkShaderModule live on m_Vulkan->device.
    // Tear down before pl_vulkan_destroy.
    destroyFrucNv12RgbResources();

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

    // Tiny host-visible buffer for plane-0 (Y, 1 byte) + plane-1 (UV pair, 2 bytes)
    // readback.  Layout: [Y at offset 0][UV pair at offset 16, 16-byte aligned].
    VkBufferCreateInfo bufCi = {};
    bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCi.size = 32;  // §J.3.e.2.b: enlarged from 16 → 32 to fit dual-plane samples.
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

    // Barrier 1: origLayout (probably VIDEO_DECODE_DPB_KHR) → TRANSFER_SRC_OPTIMAL.
    // §J.3.e.2.b: cover BOTH plane 0 (Y) and plane 1 (UV) so we can copy from
    // each below.  NV12 multi-planar VkImage exposes plane aspects separately;
    // each plane needs its own barrier (subresourceRange.aspectMask is bitwise).
    // Use VK_QUEUE_FAMILY_IGNORED — trust the AVVkFrame timeline semaphore for
    // cross-queue sync.
    auto buildBarrier = [&](VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                             VkAccessFlags srcA, VkAccessFlags dstA) -> VkImageMemoryBarrier {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = srcImg;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
        return b;
    };

    VkImageMemoryBarrier toSrcBarriers[2] = {
        buildBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, origLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        buildBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, origLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
    };

    pfnBarrier(cb,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, 0, nullptr, 0, nullptr, 2, toSrcBarriers);

    // Copy 1 luma byte (plane 0) from CENTER of frame → buffer offset 0.
    // Copy 1 UV pair (plane 1, R8G8) from chroma center (W/2, H/2 in chroma
    // coordinates = W/4, H/4 in luma coords for 4:2:0) → buffer offset 16.
    // §J.3.e.2.b: dual-plane access lets us do BT.709 on host below.
    const int32_t cx = frame->width  / 2;
    const int32_t cy = frame->height / 2;
    const int32_t ccx = cx / 2;  // chroma plane is half-res for NV12 (4:2:0)
    const int32_t ccy = cy / 2;

    VkBufferImageCopy regions[2] = {};
    // plane 0 Y, R8 format
    regions[0].bufferOffset = 0;
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regions[0].imageSubresource.layerCount = 1;
    regions[0].imageOffset = {cx, cy, 0};
    regions[0].imageExtent = {1, 1, 1};
    // plane 1 UV, R8G8 format (2 bytes per texel)
    regions[1].bufferOffset = 16;
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regions[1].imageSubresource.layerCount = 1;
    regions[1].imageOffset = {ccx, ccy, 0};
    regions[1].imageExtent = {1, 1, 1};

    pfnCopyImgBuf(cb, srcImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucProbeBuffer, 2, regions);

    // Barrier 2: restore both planes to origLayout for ffmpeg's next access.
    VkImageMemoryBarrier toOrigBarriers[2] = {
        buildBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, origLayout,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
        buildBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, origLayout,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
    };
    pfnBarrier(cb,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               0, 0, nullptr, 0, nullptr, 2, toOrigBarriers);

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

    // Read one luma byte (plane 0, offset 0) + one UV pair (plane 1, offset 16),
    // do BT.709 limited-range conversion on host, log Y/U/V/R/G/B together.
    // §J.3.e.2.b: this proves dual-plane access works AND validates that the
    // BT.709 conversion math we'll bake into §J.3.e.2.c's compute shader gives
    // sensible results on real decoded frames.
    void* mapped = nullptr;
    pfnMap(m_Vulkan->device, (VkDeviceMemory)m_FrucProbeBufferMem, 0, VK_WHOLE_SIZE, 0, &mapped);
    uint8_t y_raw = 0, cb_raw = 0, cr_raw = 0;
    if (mapped) {
        const uint8_t* p = (const uint8_t*)mapped;
        y_raw  = p[0];
        cb_raw = p[16];
        cr_raw = p[17];
        pfnUnmap(m_Vulkan->device, (VkDeviceMemory)m_FrucProbeBufferMem);
    }

    // BT.709 limited-range YUV (Y∈[16,235], UV∈[16,240]) → linear sRGB [0,1].
    // Same matrix that §J.3.e.2.c GLSL shader will use, so we have a cross-check.
    auto u8tof = [](uint8_t v) { return (float)v / 255.0f; };
    float Y_n  = (u8tof(y_raw)  - 16.0f / 255.0f) * (255.0f / 219.0f);
    float Cb_n = (u8tof(cb_raw) - 128.0f / 255.0f) * (255.0f / 224.0f);
    float Cr_n = (u8tof(cr_raw) - 128.0f / 255.0f) * (255.0f / 224.0f);
    float r = Y_n + 1.5748f * Cr_n;
    float g = Y_n - 0.1873f * Cb_n - 0.4681f * Cr_n;
    float b = Y_n + 1.8556f * Cb_n;
    auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    r = clamp01(r); g = clamp01(g); b = clamp01(b);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.b probe: PASS  size=%dx%d  origLayout=%d  "
                "center NV12 raw Y=%u Cb=%u Cr=%u → BT.709 linear RGB (%.3f, %.3f, %.3f)",
                frame->width, frame->height, (int)origLayout,
                (unsigned)y_raw, (unsigned)cb_raw, (unsigned)cr_raw,
                r, g, b);
    return true;
}

// §J.3.e.2.d — reverse converter: planar fp32 RGB buffer → RGBA8 VkImage.
// Reads bufRGB (output from §J.3.e.2.c shader, layout R-plane / G-plane /
// B-plane each W*H floats), writes RGBA8 image with alpha=1.0.  Storage-image
// output binding (rgba8 format inferred from the layout qualifier).  GENERAL
// layout for the image so we don't need transitions between dispatch and
// libplacebo sampling (§J.3.e.2.e wraps this image as pl_tex).
static const char* kRgbImgShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) readonly  buffer RGB_in { float data[]; } rgbIn;
layout(binding = 1, rgba8) writeonly uniform image2D outImg;
layout(push_constant) uniform Params { int w; int h; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;
    int idx = y * p.w + x;
    int planeSize = p.w * p.h;
    float r = clamp(rgbIn.data[idx + 0 * planeSize], 0.0, 1.0);
    float g = clamp(rgbIn.data[idx + 1 * planeSize], 0.0, 1.0);
    float b = clamp(rgbIn.data[idx + 2 * planeSize], 0.0, 1.0);
    imageStore(outImg, ivec2(x, y), vec4(r, g, b, 1.0));
}
)GLSL";

// §J.3.e.2.c — NV12 → planar fp32 RGB compute shader.  Reads NV12 plane-0
// (Y) and plane-1 (UV interleaved at half-res) from raw byte storage buffers,
// applies BT.709 limited-range YCbCr → linear sRGB conversion (same matrix as
// §J.3.e.2.b CPU sanity check), writes planar fp32 RGB to output storage
// buffer in (R-plane, G-plane, B-plane) layout — the format ncnn::VkMat
// expects for c=3 mats fed to RIFE.
//
// Caller pipeline (§J.3.e.2.c2 will wire it up):
//   1. vkCmdCopyImageToBuffer NV12 plane-0 → bufY (W*H bytes)
//   2. vkCmdCopyImageToBuffer NV12 plane-1 → bufUV (W*H/2 bytes)
//   3. buffer barrier (TRANSFER_WRITE → SHADER_READ)
//   4. vkCmdDispatch(ceilDiv(W,8), ceilDiv(H,8), 1) of THIS shader
//   5. result lands in bufRGB (W*H*3 floats, planar)
static const char* kNv12RgbShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) readonly  buffer Y_in   { uint  data[]; } yIn;
layout(binding = 1) readonly  buffer UV_in  { uint  data[]; } uvIn;
layout(binding = 2) writeonly buffer RGB_out{ float data[]; } rgbOut;
layout(push_constant) uniform Params { int w; int h; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;

    int yIdx   = y * p.w + x;
    uint yWord = yIn.data[yIdx >> 2];
    uint yByte = (yWord >> ((yIdx & 3) * 8)) & 0xFFu;
    float Y_raw = float(yByte) * (1.0 / 255.0);

    int chromaX  = x >> 1;
    int chromaY  = y >> 1;
    int uvByteI  = (chromaY * (p.w >> 1) + chromaX) * 2;
    uint uvWord0 = uvIn.data[uvByteI >> 2];
    uint uvWord1 = uvIn.data[(uvByteI + 1) >> 2];
    uint cbByte  = (uvWord0 >> ((uvByteI       & 3) * 8)) & 0xFFu;
    uint crByte  = (uvWord1 >> (((uvByteI + 1) & 3) * 8)) & 0xFFu;
    float Cb_raw = float(cbByte) * (1.0 / 255.0);
    float Cr_raw = float(crByte) * (1.0 / 255.0);

    float Y_n  = (Y_raw  - 16.0  / 255.0) * (255.0 / 219.0);
    float Cb_n = (Cb_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float Cr_n = (Cr_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float r = clamp(Y_n + 1.5748   * Cr_n, 0.0, 1.0);
    float g = clamp(Y_n - 0.1873   * Cb_n - 0.4681 * Cr_n, 0.0, 1.0);
    float b = clamp(Y_n + 1.8556   * Cb_n, 0.0, 1.0);

    int outIdx    = y * p.w + x;
    int planeSize = p.w * p.h;
    rgbOut.data[outIdx + 0 * planeSize] = r;
    rgbOut.data[outIdx + 1 * planeSize] = g;
    rgbOut.data[outIdx + 2 * planeSize] = b;
}
)GLSL";

bool PlVkRenderer::initFrucNv12RgbResources(uint32_t width, uint32_t height)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: enter (W=%u H=%u ready=%d disabled=%d ncnn=%d)",
                width, height, (int)m_FrucNv12RgbReady, (int)m_FrucNv12RgbDisabled,
                (int)m_NcnnExternalReady);
    if (m_FrucNv12RgbReady || m_FrucNv12RgbDisabled) return m_FrucNv12RgbReady;
    if (!m_NcnnExternalReady) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: ncnn external mode not ready "
                    "(VIPLE_PLVK_NCNN_HANDOFF=1 must be set first)");
        m_FrucNv12RgbDisabled = true;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: A1 — calling ncnn::get_gpu_device(0)");
    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(0);
    if (!vkdev) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: ncnn::get_gpu_device(0) returned null");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: A2 — vkdev=%p, vkdevice=%p",
                (void*)vkdev, (void*)vkdev->vkdevice());

    ncnn::Option opt;
    opt.use_vulkan_compute  = true;
    opt.use_fp16_packed     = false;
    opt.use_fp16_storage    = false;
    opt.use_fp16_arithmetic = false;
    opt.use_int8_storage    = false;
    opt.use_int8_arithmetic = false;
    opt.use_packing_layout  = false;
    opt.use_shader_pack8    = false;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: A3 — calling ncnn::compile_spirv_module");
    std::vector<uint32_t> spirv;
    int rc = ncnn::compile_spirv_module(kNv12RgbShaderGlsl, opt, spirv);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: A4 — compile_spirv_module rc=%d spv_size=%zu",
                rc, spirv.size());
    if (rc != 0 || spirv.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: compile_spirv_module failed rc=%d size=%zu",
                    rc, spirv.size());
        m_FrucNv12RgbDisabled = true;
        return false;
    }

    // Build VkPipeline directly via raw Vulkan — ncnn::Pipeline::create()
    // crashes inside SPIR-V reflection when the shader doesn't follow
    // ncnn-Mat binding semantics (verified empirically with this 3-binding
    // shader at v1.3.75).  We have the SPIR-V bytecode; the rest is plain
    // Vulkan: shader module → descriptor set layout → pipeline layout →
    // compute pipeline.

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: A5 — raw Vulkan pipeline build");

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)getDevProc("vkCreateShaderModule");
    auto pfnCreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)getDevProc("vkCreateDescriptorSetLayout");
    auto pfnCreatePipelineLayout = (PFN_vkCreatePipelineLayout)getDevProc("vkCreatePipelineLayout");
    auto pfnCreateComputePipelines = (PFN_vkCreateComputePipelines)getDevProc("vkCreateComputePipelines");
    if (!pfnCreateShaderModule || !pfnCreateDescriptorSetLayout || !pfnCreatePipelineLayout
        || !pfnCreateComputePipelines) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: raw Vulkan PFN load failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }

    VkShaderModuleCreateInfo smCi = {};
    smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCi.codeSize = spirv.size() * sizeof(uint32_t);
    smCi.pCode = spirv.data();
    VkShaderModule shaderMod = VK_NULL_HANDLE;
    if (pfnCreateShaderModule(m_Vulkan->device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: vkCreateShaderModule failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbVkShader = shaderMod;

    VkDescriptorSetLayoutBinding dslBindings[3] = {};
    for (int i = 0; i < 3; i++) {
        dslBindings[i].binding = (uint32_t)i;
        dslBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dslBindings[i].descriptorCount = 1;
        dslBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 3;
    dslCi.pBindings = dslBindings;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (pfnCreateDescriptorSetLayout(m_Vulkan->device, &dslCi, nullptr, &dsl) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: vkCreateDescriptorSetLayout failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbVkDsl = dsl;

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(int) * 2;  // w, h

    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &dsl;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges = &pcRange;
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    if (pfnCreatePipelineLayout(m_Vulkan->device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: vkCreatePipelineLayout failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbVkPipeLay = pipeLay;

    VkComputePipelineCreateInfo cpCi = {};
    cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCi.stage.module = shaderMod;
    cpCi.stage.pName = "main";
    cpCi.layout = pipeLay;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (pfnCreateComputePipelines(m_Vulkan->device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipe)
        != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c init: vkCreateComputePipelines failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbVkPipeline = pipe;
    m_FrucNv12RgbWidth = width;
    m_FrucNv12RgbHeight = height;

    // §J.3.e.2.c2 — allocate dispatch resources: 3 device-local storage buffers
    // (bufY, bufUV, bufRGB), 1 host-visible readback buffer, descriptor pool +
    // descriptor set bound once, command pool + buffer + fence.
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevProc("vkCreateBuffer");
    auto pfnGetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)getDevProc("vkGetBufferMemoryRequirements");
    auto pfnAllocateMemory = (PFN_vkAllocateMemory)getDevProc("vkAllocateMemory");
    auto pfnBindBufferMemory = (PFN_vkBindBufferMemory)getDevProc("vkBindBufferMemory");
    auto pfnCreateDescriptorPool = (PFN_vkCreateDescriptorPool)getDevProc("vkCreateDescriptorPool");
    auto pfnAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)getDevProc("vkAllocateDescriptorSets");
    auto pfnUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)getDevProc("vkUpdateDescriptorSets");
    auto pfnCreateCommandPool = (PFN_vkCreateCommandPool)getDevProc("vkCreateCommandPool");
    auto pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)getDevProc("vkAllocateCommandBuffers");
    auto pfnCreateFence = (PFN_vkCreateFence)getDevProc("vkCreateFence");
    auto pfnGetPhysProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufferMemoryRequirements || !pfnAllocateMemory
        || !pfnBindBufferMemory || !pfnCreateDescriptorPool || !pfnAllocateDescriptorSets
        || !pfnUpdateDescriptorSets || !pfnCreateCommandPool || !pfnAllocateCommandBuffers
        || !pfnCreateFence || !pfnGetPhysProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: missing dispatch PFNs");
        m_FrucNv12RgbDisabled = true;
        return false;
    }

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPhysProps(m_Vulkan->phys_device, &memProps);

    auto findMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags wantFlags) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & wantFlags) == wantFlags) {
                return i;
            }
        }
        return UINT32_MAX;
    };

    auto createBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                             VkMemoryPropertyFlags memFlags,
                             VkBuffer& outBuf, VkDeviceMemory& outMem) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Vulkan->device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq = {};
        pfnGetBufferMemoryRequirements(m_Vulkan->device, outBuf, &memReq);
        uint32_t mti = findMemType(memReq.memoryTypeBits, memFlags);
        if (mti == UINT32_MAX) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = mti;
        if (pfnAllocateMemory(m_Vulkan->device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        if (pfnBindBufferMemory(m_Vulkan->device, outBuf, outMem, 0) != VK_SUCCESS) return false;
        return true;
    };

    const VkDeviceSize sizeY   = (VkDeviceSize)width * height;
    const VkDeviceSize sizeUV  = (VkDeviceSize)width * height / 2;
    const VkDeviceSize sizeRGB = (VkDeviceSize)width * height * 3 * sizeof(float);
    VkBuffer bufY = VK_NULL_HANDLE, bufUV = VK_NULL_HANDLE, bufRGB = VK_NULL_HANDLE, hostBuf = VK_NULL_HANDLE;
    VkDeviceMemory bufYMem = VK_NULL_HANDLE, bufUVMem = VK_NULL_HANDLE, bufRGBMem = VK_NULL_HANDLE, hostBufMem = VK_NULL_HANDLE;

    if (!createBuffer(sizeY, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufY, bufYMem)
        || !createBuffer(sizeUV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufUV, bufUVMem)
        || !createBuffer(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufRGB, bufRGBMem)
        || !createBuffer(/*3 floats*/ 12, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          hostBuf, hostBufMem)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: buffer/memory allocation failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbBufY = bufY;        m_FrucNv12RgbBufYMem = bufYMem;
    m_FrucNv12RgbBufUV = bufUV;      m_FrucNv12RgbBufUVMem = bufUVMem;
    m_FrucNv12RgbBufRGB = bufRGB;    m_FrucNv12RgbBufRGBMem = bufRGBMem;
    m_FrucNv12RgbHostBuf = hostBuf;  m_FrucNv12RgbHostBufMem = hostBufMem;

    // Descriptor pool + set, bind 3 buffers once.
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSize.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 1;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &poolSize;
    VkDescriptorPool dPool = VK_NULL_HANDLE;
    if (pfnCreateDescriptorPool(m_Vulkan->device, &dpCi, nullptr, &dPool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: vkCreateDescriptorPool failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbDescPool = dPool;

    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = dPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &dsl;
    VkDescriptorSet dSet = VK_NULL_HANDLE;
    if (pfnAllocateDescriptorSets(m_Vulkan->device, &dsAlloc, &dSet) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: vkAllocateDescriptorSets failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbDescSet = dSet;

    VkDescriptorBufferInfo dbi[3] = {};
    dbi[0].buffer = bufY;   dbi[0].offset = 0; dbi[0].range = VK_WHOLE_SIZE;
    dbi[1].buffer = bufUV;  dbi[1].offset = 0; dbi[1].range = VK_WHOLE_SIZE;
    dbi[2].buffer = bufRGB; dbi[2].offset = 0; dbi[2].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[3] = {};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dSet;
        writes[i].dstBinding = (uint32_t)i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &dbi[i];
    }
    pfnUpdateDescriptorSets(m_Vulkan->device, 3, writes, 0, nullptr);

    // Command pool on compute queue family + cmd buffer + fence.
    VkCommandPoolCreateInfo cpCi2 = {};
    cpCi2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpCi2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpCi2.queueFamilyIndex = m_Vulkan->queue_compute.index;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    if (pfnCreateCommandPool(m_Vulkan->device, &cpCi2, nullptr, &cmdPool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: vkCreateCommandPool failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbCmdPool = cmdPool;

    VkCommandBufferAllocateInfo cbAlloc = {};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool = cmdPool;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (pfnAllocateCommandBuffers(m_Vulkan->device, &cbAlloc, &cb) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: vkAllocateCommandBuffers failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbCmdBuf = cb;

    VkFenceCreateInfo fenceCi2 = {};
    fenceCi2.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (pfnCreateFence(m_Vulkan->device, &fenceCi2, nullptr, &fence) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 init: vkCreateFence failed");
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    m_FrucNv12RgbFence = fence;

    m_FrucNv12RgbReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c init: shader compiled (spv=%zu bytes), "
                "raw VkPipeline=%p (DSL=%p PL=%p ShaderMod=%p), target %ux%u, "
                "bufY/UV/RGB sizes %llu/%llu/%llu bytes",
                spirv.size() * sizeof(uint32_t),
                (void*)pipe, (void*)dsl, (void*)pipeLay, (void*)shaderMod, width, height,
                (unsigned long long)sizeY, (unsigned long long)sizeUV, (unsigned long long)sizeRGB);
    return true;
}

void PlVkRenderer::destroyFrucNv12RgbResources()
{
    if (!m_PlVkInstance || !m_Vulkan) return;
    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    auto pfnDestroyPipeline = (PFN_vkDestroyPipeline)getDevProc("vkDestroyPipeline");
    auto pfnDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)getDevProc("vkDestroyPipelineLayout");
    auto pfnDestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout)getDevProc("vkDestroyDescriptorSetLayout");
    auto pfnDestroyShaderModule = (PFN_vkDestroyShaderModule)getDevProc("vkDestroyShaderModule");
    auto pfnDestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)getDevProc("vkDestroyDescriptorPool");
    auto pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)getDevProc("vkDestroyCommandPool");
    auto pfnDestroyFence = (PFN_vkDestroyFence)getDevProc("vkDestroyFence");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevProc("vkDestroyBuffer");
    auto pfnFreeMemory = (PFN_vkFreeMemory)getDevProc("vkFreeMemory");
    auto pfnDeviceWait = (PFN_vkDeviceWaitIdle)getDevProc("vkDeviceWaitIdle");

    // Wait for any pending dispatch on the device before destroying its resources.
    if (pfnDeviceWait) pfnDeviceWait(m_Vulkan->device);

    if (m_FrucNv12RgbFence && pfnDestroyFence) {
        pfnDestroyFence(m_Vulkan->device, (VkFence)m_FrucNv12RgbFence, nullptr);
    }
    if (m_FrucNv12RgbCmdPool && pfnDestroyCommandPool) {
        // Frees the command buffer too.
        pfnDestroyCommandPool(m_Vulkan->device, (VkCommandPool)m_FrucNv12RgbCmdPool, nullptr);
    }
    if (m_FrucNv12RgbDescPool && pfnDestroyDescriptorPool) {
        // Frees the descriptor set too.
        pfnDestroyDescriptorPool(m_Vulkan->device, (VkDescriptorPool)m_FrucNv12RgbDescPool, nullptr);
    }
    auto destroyBuf = [&](void*& buf, void*& mem) {
        if (buf && pfnDestroyBuffer) pfnDestroyBuffer(m_Vulkan->device, (VkBuffer)buf, nullptr);
        if (mem && pfnFreeMemory)    pfnFreeMemory(m_Vulkan->device, (VkDeviceMemory)mem, nullptr);
        buf = nullptr; mem = nullptr;
    };
    destroyBuf(m_FrucNv12RgbBufY,    m_FrucNv12RgbBufYMem);
    destroyBuf(m_FrucNv12RgbBufUV,   m_FrucNv12RgbBufUVMem);
    destroyBuf(m_FrucNv12RgbBufRGB,  m_FrucNv12RgbBufRGBMem);
    destroyBuf(m_FrucNv12RgbHostBuf, m_FrucNv12RgbHostBufMem);

    if (m_FrucNv12RgbVkPipeline && pfnDestroyPipeline) {
        pfnDestroyPipeline(m_Vulkan->device, (VkPipeline)m_FrucNv12RgbVkPipeline, nullptr);
    }
    if (m_FrucNv12RgbVkPipeLay && pfnDestroyPipelineLayout) {
        pfnDestroyPipelineLayout(m_Vulkan->device, (VkPipelineLayout)m_FrucNv12RgbVkPipeLay, nullptr);
    }
    if (m_FrucNv12RgbVkDsl && pfnDestroyDescriptorSetLayout) {
        pfnDestroyDescriptorSetLayout(m_Vulkan->device, (VkDescriptorSetLayout)m_FrucNv12RgbVkDsl, nullptr);
    }
    if (m_FrucNv12RgbVkShader && pfnDestroyShaderModule) {
        pfnDestroyShaderModule(m_Vulkan->device, (VkShaderModule)m_FrucNv12RgbVkShader, nullptr);
    }
    m_FrucNv12RgbFence = nullptr;
    m_FrucNv12RgbCmdPool = nullptr;
    m_FrucNv12RgbCmdBuf = nullptr;
    m_FrucNv12RgbDescPool = nullptr;
    m_FrucNv12RgbDescSet = nullptr;
    m_FrucNv12RgbVkPipeline = nullptr;
    m_FrucNv12RgbVkPipeLay  = nullptr;
    m_FrucNv12RgbVkDsl      = nullptr;
    m_FrucNv12RgbVkShader   = nullptr;
    m_FrucNv12RgbReady = false;
    m_FrucNv12RgbWidth = 0;
    m_FrucNv12RgbHeight = 0;
}

bool PlVkRenderer::initFrucRgbImgResources()
{
    if (m_FrucRgbImgReady || m_FrucRgbImgDisabled) return m_FrucRgbImgReady;
    if (!m_FrucNv12RgbReady) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.d init: §J.3.e.2.c (forward path) must be ready first");
        m_FrucRgbImgDisabled = true;
        return false;
    }

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };

    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)getDevProc("vkCreateShaderModule");
    auto pfnCreateDSL = (PFN_vkCreateDescriptorSetLayout)getDevProc("vkCreateDescriptorSetLayout");
    auto pfnCreatePL = (PFN_vkCreatePipelineLayout)getDevProc("vkCreatePipelineLayout");
    auto pfnCreatePipe = (PFN_vkCreateComputePipelines)getDevProc("vkCreateComputePipelines");
    auto pfnCreateImage = (PFN_vkCreateImage)getDevProc("vkCreateImage");
    auto pfnGetImageMemReq = (PFN_vkGetImageMemoryRequirements)getDevProc("vkGetImageMemoryRequirements");
    auto pfnAllocMem = (PFN_vkAllocateMemory)getDevProc("vkAllocateMemory");
    auto pfnBindImgMem = (PFN_vkBindImageMemory)getDevProc("vkBindImageMemory");
    auto pfnCreateImageView = (PFN_vkCreateImageView)getDevProc("vkCreateImageView");
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevProc("vkCreateBuffer");
    auto pfnGetBufMemReq = (PFN_vkGetBufferMemoryRequirements)getDevProc("vkGetBufferMemoryRequirements");
    auto pfnBindBufMem = (PFN_vkBindBufferMemory)getDevProc("vkBindBufferMemory");
    auto pfnCreateDPool = (PFN_vkCreateDescriptorPool)getDevProc("vkCreateDescriptorPool");
    auto pfnAllocDS = (PFN_vkAllocateDescriptorSets)getDevProc("vkAllocateDescriptorSets");
    auto pfnUpdateDS = (PFN_vkUpdateDescriptorSets)getDevProc("vkUpdateDescriptorSets");
    auto pfnGetPhysProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, "vkGetPhysicalDeviceMemoryProperties");

    if (!pfnCreateShaderModule || !pfnCreateDSL || !pfnCreatePL || !pfnCreatePipe
        || !pfnCreateImage || !pfnGetImageMemReq || !pfnAllocMem || !pfnBindImgMem
        || !pfnCreateImageView || !pfnCreateBuffer || !pfnGetBufMemReq || !pfnBindBufMem
        || !pfnCreateDPool || !pfnAllocDS || !pfnUpdateDS || !pfnGetPhysProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.d init: missing PFN");
        m_FrucRgbImgDisabled = true;
        return false;
    }

    // 1. Compile reverse shader
    ncnn::Option opt;
    opt.use_vulkan_compute = true;
    opt.use_fp16_packed = false; opt.use_fp16_storage = false; opt.use_fp16_arithmetic = false;
    opt.use_int8_storage = false; opt.use_int8_arithmetic = false;
    opt.use_packing_layout = false; opt.use_shader_pack8 = false;
    std::vector<uint32_t> spirv;
    int rc = ncnn::compile_spirv_module(kRgbImgShaderGlsl, opt, spirv);
    if (rc != 0 || spirv.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.d init: compile_spirv_module rc=%d size=%zu", rc, spirv.size());
        m_FrucRgbImgDisabled = true;
        return false;
    }

    VkShaderModuleCreateInfo smCi = {};
    smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smCi.codeSize = spirv.size() * sizeof(uint32_t);
    smCi.pCode = spirv.data();
    VkShaderModule shaderMod = VK_NULL_HANDLE;
    if (pfnCreateShaderModule(m_Vulkan->device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgVkShader = shaderMod;

    // 2. Descriptor set layout: binding 0 = SSBO (rgbIn), binding 1 = STORAGE_IMAGE (outImg)
    VkDescriptorSetLayoutBinding dslB[2] = {};
    dslB[0].binding = 0; dslB[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dslB[0].descriptorCount = 1; dslB[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dslB[1].binding = 1; dslB[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    dslB[1].descriptorCount = 1; dslB[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 2; dslCi.pBindings = dslB;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    if (pfnCreateDSL(m_Vulkan->device, &dslCi, nullptr, &dsl) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgVkDsl = dsl;

    // 3. Pipeline layout (push constants W, H)
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size = sizeof(int) * 2;
    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1; plCi.pSetLayouts = &dsl;
    plCi.pushConstantRangeCount = 1; plCi.pPushConstantRanges = &pcRange;
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    if (pfnCreatePL(m_Vulkan->device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgVkPipeLay = pipeLay;

    // 4. Compute pipeline
    VkComputePipelineCreateInfo cpCi = {};
    cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpCi.stage.module = shaderMod;
    cpCi.stage.pName = "main";
    cpCi.layout = pipeLay;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (pfnCreatePipe(m_Vulkan->device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipe) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgVkPipeline = pipe;

    // 5. Dest VkImage (RGBA8 UNORM, USAGE STORAGE | TRANSFER_SRC | SAMPLED for libplacebo wrap)
    VkImageCreateInfo iCi = {};
    iCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iCi.imageType = VK_IMAGE_TYPE_2D;
    iCi.format = VK_FORMAT_R8G8B8A8_UNORM;
    iCi.extent = {m_FrucNv12RgbWidth, m_FrucNv12RgbHeight, 1};
    iCi.mipLevels = 1; iCi.arrayLayers = 1;
    iCi.samples = VK_SAMPLE_COUNT_1_BIT;
    iCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    iCi.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
              | VK_IMAGE_USAGE_SAMPLED_BIT;
    iCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    iCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    if (pfnCreateImage(m_Vulkan->device, &iCi, nullptr, &img) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgImage = img;

    VkPhysicalDeviceMemoryProperties mp = {};
    pfnGetPhysProps(m_Vulkan->phys_device, &mp);

    VkMemoryRequirements imgReq = {};
    pfnGetImageMemReq(m_Vulkan->device, img, &imgReq);
    uint32_t imgMti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((imgReq.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            imgMti = i; break;
        }
    }
    if (imgMti == UINT32_MAX) { m_FrucRgbImgDisabled = true; return false; }

    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = imgReq.size;
    mai.memoryTypeIndex = imgMti;
    VkDeviceMemory imgMem = VK_NULL_HANDLE;
    if (pfnAllocMem(m_Vulkan->device, &mai, nullptr, &imgMem) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgImageMem = imgMem;
    if (pfnBindImgMem(m_Vulkan->device, img, imgMem, 0) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }

    // 6. Image view (RGBA8 COLOR aspect)
    VkImageViewCreateInfo ivCi = {};
    ivCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivCi.image = img;
    ivCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivCi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivCi.subresourceRange.levelCount = 1;
    ivCi.subresourceRange.layerCount = 1;
    VkImageView iv = VK_NULL_HANDLE;
    if (pfnCreateImageView(m_Vulkan->device, &ivCi, nullptr, &iv) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgImageView = iv;

    // 7. 4-byte readback host buffer (RGBA8 center pixel)
    VkBufferCreateInfo hbCi = {};
    hbCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    hbCi.size = 4;
    hbCi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    hbCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer hostBuf = VK_NULL_HANDLE;
    if (pfnCreateBuffer(m_Vulkan->device, &hbCi, nullptr, &hostBuf) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgHostBuf = hostBuf;

    VkMemoryRequirements hbReq = {};
    pfnGetBufMemReq(m_Vulkan->device, hostBuf, &hbReq);
    uint32_t hbMti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((hbReq.memoryTypeBits & (1u << i))
            && (mp.memoryTypes[i].propertyFlags
                & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            hbMti = i; break;
        }
    }
    if (hbMti == UINT32_MAX) { m_FrucRgbImgDisabled = true; return false; }

    VkMemoryAllocateInfo hmai = {};
    hmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    hmai.allocationSize = hbReq.size;
    hmai.memoryTypeIndex = hbMti;
    VkDeviceMemory hbMem = VK_NULL_HANDLE;
    if (pfnAllocMem(m_Vulkan->device, &hmai, nullptr, &hbMem) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgHostBufMem = hbMem;
    if (pfnBindBufMem(m_Vulkan->device, hostBuf, hbMem, 0) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }

    // 8. Descriptor pool + set, bind RGB SSBO + storage image once.
    VkDescriptorPoolSize ps[2] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[0].descriptorCount = 1;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ps[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 1;
    dpCi.poolSizeCount = 2; dpCi.pPoolSizes = ps;
    VkDescriptorPool dPool = VK_NULL_HANDLE;
    if (pfnCreateDPool(m_Vulkan->device, &dpCi, nullptr, &dPool) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgDescPool = dPool;

    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = dPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet dSet = VK_NULL_HANDLE;
    if (pfnAllocDS(m_Vulkan->device, &dsai, &dSet) != VK_SUCCESS) {
        m_FrucRgbImgDisabled = true; return false;
    }
    m_FrucRgbImgDescSet = dSet;

    VkDescriptorBufferInfo dbi = {};
    dbi.buffer = (VkBuffer)m_FrucNv12RgbBufRGB; dbi.offset = 0; dbi.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo dii = {};
    dii.imageView = iv; dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = dSet; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &dbi;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = dSet; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &dii;
    pfnUpdateDS(m_Vulkan->device, 2, w, 0, nullptr);

    m_FrucRgbImgReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.d init: reverse pipeline ready (spv=%zu bytes), "
                "VkImage=%p (RGBA8_UNORM %ux%u, GENERAL layout, STORAGE|SAMPLED|TRANSFER_SRC)",
                spirv.size() * sizeof(uint32_t), (void*)img, m_FrucNv12RgbWidth, m_FrucNv12RgbHeight);

    // §J.3.e.2.e1a — wrap the VkImage as a pl_tex so libplacebo can sample it.
    // pl_vulkan_wrap doesn't take ownership of the VkImage; pl_tex_destroy
    // releases the wrapper without touching the underlying image.  The image
    // starts in "held by user" state — we own it until pl_vulkan_release_ex
    // hands it to libplacebo (will be wired in §J.3.e.2.e1b).
    pl_vulkan_wrap_params wrapParams = {};
    wrapParams.image  = (VkImage)m_FrucRgbImgImage;
    wrapParams.width  = (int)m_FrucNv12RgbWidth;
    wrapParams.height = (int)m_FrucNv12RgbHeight;
    wrapParams.format = VK_FORMAT_R8G8B8A8_UNORM;
    wrapParams.usage  = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      | VK_IMAGE_USAGE_SAMPLED_BIT;
    wrapParams.debug_tag = PL_DEBUG_TAG;
    pl_tex wrappedTex = pl_vulkan_wrap(m_Vulkan->gpu, &wrapParams);
    if (!wrappedTex) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1a: pl_vulkan_wrap failed — "
                    "libplacebo couldn't map our RGBA8 VkImage to a pl_fmt; "
                    "render-path override (§J.3.e.2.e1b) won't be available");
        // Non-fatal: §J.3.e.2.c/d probes still work, but §J.3.e.2.e1b override
        // can't proceed.  Leave m_FrucRgbImgPlTex null.
    } else {
        // pl_tex is `const struct pl_tex_t *` so an explicit const-cast is
        // needed to stash it in a void* slot.  We won't write through it.
        m_FrucRgbImgPlTex = const_cast<void*>(static_cast<const void*>(wrappedTex));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1a: pl_vulkan_wrap → pl_tex=%p "
                    "(ready for §J.3.e.2.e1b render path override)",
                    (const void*)wrappedTex);

        // §J.3.e.2.e1b — set up dedicated override resources: timeline
        // semaphore + cmd pool + cmd buf + fence.
        auto pfnCreateSem    = (PFN_vkCreateSemaphore)getDevProc("vkCreateSemaphore");
        auto pfnCreatePool   = (PFN_vkCreateCommandPool)getDevProc("vkCreateCommandPool");
        auto pfnAllocCB      = (PFN_vkAllocateCommandBuffers)getDevProc("vkAllocateCommandBuffers");
        auto pfnCreateFence2 = (PFN_vkCreateFence)getDevProc("vkCreateFence");
        if (pfnCreateSem && pfnCreatePool && pfnAllocCB && pfnCreateFence2) {
            VkSemaphoreTypeCreateInfo tci = {};
            tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            tci.initialValue = 0;
            VkSemaphoreCreateInfo sci = {};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            sci.pNext = &tci;
            VkSemaphore holdSem = VK_NULL_HANDLE;
            VkCommandPool ovPool = VK_NULL_HANDLE;
            VkCommandBuffer ovCb = VK_NULL_HANDLE;
            VkFence ovFence = VK_NULL_HANDLE;

            VkCommandPoolCreateInfo poolCi = {};
            poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolCi.queueFamilyIndex = m_Vulkan->queue_compute.index;

            VkCommandBufferAllocateInfo cbAi = {};
            cbAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbAi.commandBufferCount = 1;

            VkFenceCreateInfo fci = {};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

            if (pfnCreateSem(m_Vulkan->device, &sci, nullptr, &holdSem) == VK_SUCCESS
                && pfnCreatePool(m_Vulkan->device, &poolCi, nullptr, &ovPool) == VK_SUCCESS) {
                cbAi.commandPool = ovPool;
                if (pfnAllocCB(m_Vulkan->device, &cbAi, &ovCb) == VK_SUCCESS
                    && pfnCreateFence2(m_Vulkan->device, &fci, nullptr, &ovFence) == VK_SUCCESS) {
                    m_FrucOverrideHoldSem = holdSem;
                    m_FrucOverrideCmdPool = ovPool;
                    m_FrucOverrideCmdBuf  = ovCb;
                    m_FrucOverrideFence   = ovFence;
                    m_FrucOverrideHoldVal = 0;
                    m_FrucOverrideFrameCount = 0;
                    m_FrucOverrideReady = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VK-FRUC] §J.3.e.2.e1b: override resources ready "
                                "(timeline sem=%p, cmd pool=%p, cmd buf=%p, fence=%p)",
                                (void*)holdSem, (void*)ovPool, (void*)ovCb, (void*)ovFence);
                }
            }
            if (!m_FrucOverrideReady) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VK-FRUC] §J.3.e.2.e1b: override resource alloc failed");
            }
        }
    }
    return true;
}

void PlVkRenderer::destroyFrucRgbImgResources()
{
    if (!m_PlVkInstance || !m_Vulkan) return;
    auto getDevProcEarly = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    auto pfnWaitDev = (PFN_vkDeviceWaitIdle)getDevProcEarly("vkDeviceWaitIdle");
    if (pfnWaitDev) pfnWaitDev(m_Vulkan->device);

    // §J.3.e.2.e1b — drop override timeline sem + cmd pool + fence FIRST,
    // before pl_tex_destroy or VkImage destroy (pool destruction frees cmd
    // buffers; fence/sem must outlive any pending submit, which the wait
    // above guaranteed has finished).
    auto pfnDestroySem = (PFN_vkDestroySemaphore)getDevProcEarly("vkDestroySemaphore");
    auto pfnDestroyPool = (PFN_vkDestroyCommandPool)getDevProcEarly("vkDestroyCommandPool");
    auto pfnDestroyFence = (PFN_vkDestroyFence)getDevProcEarly("vkDestroyFence");
    if (m_FrucOverrideFence && pfnDestroyFence)
        pfnDestroyFence(m_Vulkan->device, (VkFence)m_FrucOverrideFence, nullptr);
    if (m_FrucOverrideCmdPool && pfnDestroyPool)
        pfnDestroyPool(m_Vulkan->device, (VkCommandPool)m_FrucOverrideCmdPool, nullptr);
    if (m_FrucOverrideHoldSem && pfnDestroySem)
        pfnDestroySem(m_Vulkan->device, (VkSemaphore)m_FrucOverrideHoldSem, nullptr);
    m_FrucOverrideFence = nullptr;
    m_FrucOverrideCmdPool = nullptr;
    m_FrucOverrideCmdBuf = nullptr;
    m_FrucOverrideHoldSem = nullptr;
    m_FrucOverrideHoldVal = 0;
    m_FrucOverrideFrameCount = 0;
    m_FrucOverrideReady = false;

    // §J.3.e.2.e1a — drop the libplacebo wrapper.  pl_tex_destroy doesn't free
    // the underlying VkImage, but holds an internal ref that must be released
    // before vkDestroyImage runs further down.
    if (m_FrucRgbImgPlTex) {
        pl_tex tex = (pl_tex)m_FrucRgbImgPlTex;
        pl_tex_destroy(m_Vulkan->gpu, &tex);
        m_FrucRgbImgPlTex = nullptr;
    }
    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    auto pfnDestroyPipe = (PFN_vkDestroyPipeline)getDevProc("vkDestroyPipeline");
    auto pfnDestroyPL = (PFN_vkDestroyPipelineLayout)getDevProc("vkDestroyPipelineLayout");
    auto pfnDestroyDSL = (PFN_vkDestroyDescriptorSetLayout)getDevProc("vkDestroyDescriptorSetLayout");
    auto pfnDestroySM = (PFN_vkDestroyShaderModule)getDevProc("vkDestroyShaderModule");
    auto pfnDestroyDPool = (PFN_vkDestroyDescriptorPool)getDevProc("vkDestroyDescriptorPool");
    auto pfnDestroyImg = (PFN_vkDestroyImage)getDevProc("vkDestroyImage");
    auto pfnDestroyIV = (PFN_vkDestroyImageView)getDevProc("vkDestroyImageView");
    auto pfnDestroyBuf = (PFN_vkDestroyBuffer)getDevProc("vkDestroyBuffer");
    auto pfnFreeMem = (PFN_vkFreeMemory)getDevProc("vkFreeMemory");

    if (m_FrucRgbImgDescPool && pfnDestroyDPool)
        pfnDestroyDPool(m_Vulkan->device, (VkDescriptorPool)m_FrucRgbImgDescPool, nullptr);
    if (m_FrucRgbImgImageView && pfnDestroyIV)
        pfnDestroyIV(m_Vulkan->device, (VkImageView)m_FrucRgbImgImageView, nullptr);
    if (m_FrucRgbImgImage && pfnDestroyImg)
        pfnDestroyImg(m_Vulkan->device, (VkImage)m_FrucRgbImgImage, nullptr);
    if (m_FrucRgbImgImageMem && pfnFreeMem)
        pfnFreeMem(m_Vulkan->device, (VkDeviceMemory)m_FrucRgbImgImageMem, nullptr);
    if (m_FrucRgbImgHostBuf && pfnDestroyBuf)
        pfnDestroyBuf(m_Vulkan->device, (VkBuffer)m_FrucRgbImgHostBuf, nullptr);
    if (m_FrucRgbImgHostBufMem && pfnFreeMem)
        pfnFreeMem(m_Vulkan->device, (VkDeviceMemory)m_FrucRgbImgHostBufMem, nullptr);
    if (m_FrucRgbImgVkPipeline && pfnDestroyPipe)
        pfnDestroyPipe(m_Vulkan->device, (VkPipeline)m_FrucRgbImgVkPipeline, nullptr);
    if (m_FrucRgbImgVkPipeLay && pfnDestroyPL)
        pfnDestroyPL(m_Vulkan->device, (VkPipelineLayout)m_FrucRgbImgVkPipeLay, nullptr);
    if (m_FrucRgbImgVkDsl && pfnDestroyDSL)
        pfnDestroyDSL(m_Vulkan->device, (VkDescriptorSetLayout)m_FrucRgbImgVkDsl, nullptr);
    if (m_FrucRgbImgVkShader && pfnDestroySM)
        pfnDestroySM(m_Vulkan->device, (VkShaderModule)m_FrucRgbImgVkShader, nullptr);
    m_FrucRgbImgDescPool = nullptr;     m_FrucRgbImgDescSet = nullptr;
    m_FrucRgbImgImageView = nullptr;    m_FrucRgbImgImage = nullptr;    m_FrucRgbImgImageMem = nullptr;
    m_FrucRgbImgHostBuf = nullptr;      m_FrucRgbImgHostBufMem = nullptr;
    m_FrucRgbImgVkPipeline = nullptr;   m_FrucRgbImgVkPipeLay = nullptr;
    m_FrucRgbImgVkDsl = nullptr;        m_FrucRgbImgVkShader = nullptr;
    m_FrucRgbImgReady = false;
}

bool PlVkRenderer::runRgbImgReversePass(VkCommandBuffer cb, uint32_t width, uint32_t height)
{
    if (!m_FrucRgbImgReady || !cb) return false;

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    static auto pfnBarrier   = (PFN_vkCmdPipelineBarrier)getDevProc("vkCmdPipelineBarrier");
    static auto pfnBindPipe  = (PFN_vkCmdBindPipeline)getDevProc("vkCmdBindPipeline");
    static auto pfnBindDS    = (PFN_vkCmdBindDescriptorSets)getDevProc("vkCmdBindDescriptorSets");
    static auto pfnPushC     = (PFN_vkCmdPushConstants)getDevProc("vkCmdPushConstants");
    static auto pfnDispatch  = (PFN_vkCmdDispatch)getDevProc("vkCmdDispatch");
    if (!pfnBarrier || !pfnBindPipe || !pfnBindDS || !pfnPushC || !pfnDispatch) return false;

    // First-use: image is in UNDEFINED.  Subsequent calls: image is in GENERAL
    // (we leave it there).  Use a self-targeting barrier with oldLayout=GENERAL
    // (initial UNDEFINED is a one-shot transitioned via the same barrier on
    // first dispatch; storage image accepts UNDEFINED→GENERAL transition).
    static bool s_FirstUseTracked = false;
    VkImageMemoryBarrier toGen = {};
    toGen.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGen.srcAccessMask = s_FirstUseTracked ? VK_ACCESS_SHADER_READ_BIT : 0;
    toGen.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toGen.oldLayout = s_FirstUseTracked ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGen.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGen.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGen.image = (VkImage)m_FrucRgbImgImage;
    toGen.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toGen.subresourceRange.levelCount = 1;
    toGen.subresourceRange.layerCount = 1;
    pfnBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               0, 0, nullptr, 0, nullptr, 1, &toGen);
    s_FirstUseTracked = true;

    pfnBindPipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)m_FrucRgbImgVkPipeline);
    VkDescriptorSet dSet = (VkDescriptorSet)m_FrucRgbImgDescSet;
    pfnBindDS(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
              (VkPipelineLayout)m_FrucRgbImgVkPipeLay, 0, 1, &dSet, 0, nullptr);
    int pushVals[2] = {(int)width, (int)height};
    pfnPushC(cb, (VkPipelineLayout)m_FrucRgbImgVkPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
             0, sizeof(pushVals), pushVals);
    const uint32_t gx = (width + 7) / 8;
    const uint32_t gy = (height + 7) / 8;
    pfnDispatch(cb, gx, gy, 1);

    return true;
}

// §J.3.e.2.e1b — runFrucOverridePass: every-frame dispatch for override mode.
// Same pipeline as runNv12RgbProbe (forward + reverse) but no readback, dedicated
// cmd buffer/fence (separate from probe path), waits on hold timeline before
// dispatch (host-side vkWaitSemaphores), submits with AVVkFrame.sem chain.
// On success, image is in GENERAL with new content; caller does pl_vulkan_release_ex
// then pl_render_image with our pl_tex, then pl_vulkan_hold_ex with the
// timeline sem at incremented value.
bool PlVkRenderer::runFrucOverridePass(AVVkFrame* vkFrame, AVFrame* frame)
{
    if (!m_FrucOverrideReady || !m_FrucNv12RgbReady || !m_FrucRgbImgReady) return false;
    if (!vkFrame || !vkFrame->img[0]) return false;

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    static auto pfnBegin       = (PFN_vkBeginCommandBuffer)getDevProc("vkBeginCommandBuffer");
    static auto pfnEnd         = (PFN_vkEndCommandBuffer)getDevProc("vkEndCommandBuffer");
    static auto pfnReset       = (PFN_vkResetCommandBuffer)getDevProc("vkResetCommandBuffer");
    static auto pfnBarrier     = (PFN_vkCmdPipelineBarrier)getDevProc("vkCmdPipelineBarrier");
    static auto pfnCopyImgBuf  = (PFN_vkCmdCopyImageToBuffer)getDevProc("vkCmdCopyImageToBuffer");
    static auto pfnBindPipe    = (PFN_vkCmdBindPipeline)getDevProc("vkCmdBindPipeline");
    static auto pfnBindDS      = (PFN_vkCmdBindDescriptorSets)getDevProc("vkCmdBindDescriptorSets");
    static auto pfnPushC       = (PFN_vkCmdPushConstants)getDevProc("vkCmdPushConstants");
    static auto pfnDispatch    = (PFN_vkCmdDispatch)getDevProc("vkCmdDispatch");
    static auto pfnGetQueue    = (PFN_vkGetDeviceQueue)getDevProc("vkGetDeviceQueue");
    static auto pfnSubmit      = (PFN_vkQueueSubmit)getDevProc("vkQueueSubmit");
    static auto pfnWaitFences  = (PFN_vkWaitForFences)getDevProc("vkWaitForFences");
    static auto pfnResetFences = (PFN_vkResetFences)getDevProc("vkResetFences");
    static auto pfnWaitSems    = (PFN_vkWaitSemaphores)getDevProc("vkWaitSemaphores");
    if (!pfnBegin || !pfnEnd || !pfnReset || !pfnBarrier || !pfnCopyImgBuf
        || !pfnBindPipe || !pfnBindDS || !pfnPushC || !pfnDispatch || !pfnGetQueue
        || !pfnSubmit || !pfnWaitFences || !pfnResetFences || !pfnWaitSems) {
        return false;
    }

    // Step A: host-wait on hold timeline at current value (== libplacebo's prev
    // frame done with our pl_tex).  First frame: m_FrucOverrideHoldVal=0,
    // timeline init=0, returns immediately.  After ++ at end of frame N, frame
    // N+1 waits at value N (signaled by libplacebo when it finished frame N's
    // pl_render_image).
    if (m_FrucOverrideHoldVal > 0) {
        VkSemaphore sem = (VkSemaphore)m_FrucOverrideHoldSem;
        uint64_t waitVal = m_FrucOverrideHoldVal;
        VkSemaphoreWaitInfo wi = {};
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &waitVal;
        // 100ms timeout.  If libplacebo hasn't finished prev frame within
        // 100ms something's wrong — disable override to avoid runaway.
        VkResult wr = pfnWaitSems(m_Vulkan->device, &wi, 100ULL * 1000 * 1000);
        if (wr != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VK-FRUC] §J.3.e.2.e1b: hold timeline wait rc=%d (val=%llu)",
                        (int)wr, (unsigned long long)waitVal);
            return false;
        }
    }

    // Step B: record cmd buffer (forward + reverse, no readback).
    VkCommandBuffer cb = (VkCommandBuffer)m_FrucOverrideCmdBuf;
    VkImage         img = (VkImage)vkFrame->img[0];
    VkImageLayout   orig = vkFrame->layout[0];
    const uint32_t  W = m_FrucNv12RgbWidth;
    const uint32_t  H = m_FrucNv12RgbHeight;

    pfnReset(cb, 0);
    VkCommandBufferBeginInfo bbi = {};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBegin(cb, &bbi);

    auto buildImgBarrier = [&](VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                                VkAccessFlags srcA, VkAccessFlags dstA) -> VkImageMemoryBarrier {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        b.oldLayout = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        return b;
    };

    VkImageMemoryBarrier toSrc[2] = {
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, orig, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, orig, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
    };
    pfnBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, 0, nullptr, 0, nullptr, 2, toSrc);

    VkBufferImageCopy regY = {};
    regY.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regY.imageSubresource.layerCount = 1;
    regY.imageExtent = {W, H, 1};
    pfnCopyImgBuf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucNv12RgbBufY, 1, &regY);
    VkBufferImageCopy regUV = {};
    regUV.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regUV.imageSubresource.layerCount = 1;
    regUV.imageExtent = {W / 2, H / 2, 1};
    pfnCopyImgBuf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucNv12RgbBufUV, 1, &regUV);

    VkBufferMemoryBarrier bbar1[2] = {};
    bbar1[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bbar1[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bbar1[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bbar1[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bbar1[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bbar1[0].buffer = (VkBuffer)m_FrucNv12RgbBufY;
    bbar1[0].size = VK_WHOLE_SIZE;
    bbar1[1] = bbar1[0];
    bbar1[1].buffer = (VkBuffer)m_FrucNv12RgbBufUV;
    pfnBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               0, 0, nullptr, 2, bbar1, 0, nullptr);

    // Forward dispatch (NV12 → bufRGB)
    pfnBindPipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)m_FrucNv12RgbVkPipeline);
    VkDescriptorSet fwdDS = (VkDescriptorSet)m_FrucNv12RgbDescSet;
    pfnBindDS(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
              (VkPipelineLayout)m_FrucNv12RgbVkPipeLay, 0, 1, &fwdDS, 0, nullptr);
    int pushVals[2] = {(int)W, (int)H};
    pfnPushC(cb, (VkPipelineLayout)m_FrucNv12RgbVkPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
             0, sizeof(pushVals), pushVals);
    const uint32_t gx = (W + 7) / 8;
    const uint32_t gy = (H + 7) / 8;
    pfnDispatch(cb, gx, gy, 1);

    // Buffer barrier (forward shader write → reverse shader read)
    VkBufferMemoryBarrier bbar2 = {};
    bbar2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bbar2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bbar2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bbar2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bbar2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bbar2.buffer = (VkBuffer)m_FrucNv12RgbBufRGB;
    bbar2.size = VK_WHOLE_SIZE;
    pfnBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               0, 0, nullptr, 1, &bbar2, 0, nullptr);

    // Reverse dispatch (bufRGB → m_FrucRgbImgImage in GENERAL)
    if (!runRgbImgReversePass(cb, W, H)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1b: runRgbImgReversePass returned false");
        return false;
    }

    // Restore plane layouts for ffmpeg.
    VkImageMemoryBarrier toOrig[2] = {
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, orig,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, orig,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
    };
    pfnBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               0, 0, nullptr, 0, nullptr, 2, toOrig);

    pfnEnd(cb);

    // Step C: submit on compute queue with AVVkFrame.sem timeline wait/signal
    // + local fence for host wait.
    VkQueue computeQueue = VK_NULL_HANDLE;
    pfnGetQueue(m_Vulkan->device, m_Vulkan->queue_compute.index, 0, &computeQueue);

    uint64_t waitVal   = vkFrame->sem_value[0];
    uint64_t signalVal = waitVal + 1;
    VkSemaphore avSem  = vkFrame->sem[0];
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
    si.pWaitSemaphores = &avSem;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &avSem;

    m_Vulkan->lock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);
    VkResult subRes = pfnSubmit(computeQueue, 1, &si, (VkFence)m_FrucOverrideFence);
    m_Vulkan->unlock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);
    if (subRes != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1b: vkQueueSubmit rc=%d", (int)subRes);
        return false;
    }
    vkFrame->sem_value[0] = signalVal;

    // Step D: host-wait on local fence (image now in GENERAL with new content).
    VkFence fence = (VkFence)m_FrucOverrideFence;
    VkResult wr = pfnWaitFences(m_Vulkan->device, 1, &fence, VK_TRUE, 1000ULL * 1000 * 1000);
    if (wr != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1b: vkWaitForFences rc=%d", (int)wr);
        return false;
    }
    pfnResetFences(m_Vulkan->device, 1, &fence);

    ++m_FrucOverrideFrameCount;
    if (m_FrucOverrideFrameCount == 1 || (m_FrucOverrideFrameCount % 60) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.e1b: dispatch frame#%llu OK (image in GENERAL, "
                    "ready for pl_vulkan_release_ex)",
                    (unsigned long long)m_FrucOverrideFrameCount);
    }
    return true;
}

bool PlVkRenderer::runNv12RgbProbe(AVVkFrame* vkFrame, AVFrame* frame)
{
    if (m_FrucNv12RgbDisabled) return false;
    if (!m_FrucNv12RgbReady) {
        if (!initFrucNv12RgbResources((uint32_t)frame->width, (uint32_t)frame->height)) {
            return false;
        }
    }
    if (!vkFrame || !vkFrame->img[0]) return false;

    // §J.3.e.2.c2 — per-frame dispatch:
    //   1. plane-0 / plane-1 layout transition origLayout → TRANSFER_SRC_OPTIMAL
    //   2. vkCmdCopyImageToBuffer × 2 (Y plane → bufY, UV plane → bufUV)
    //   3. buffer barrier (TRANSFER_WRITE → SHADER_READ on bufY/bufUV)
    //   4. dispatch NV12→RGB compute (writes bufRGB)
    //   5. buffer barrier (SHADER_WRITE → TRANSFER_READ on bufRGB)
    //   6. vkCmdCopyBuffer (3 plane center-pixel floats → host buf)
    //   7. plane-0 / plane-1 layout transition back to origLayout
    //   8. submit on compute queue with AVVkFrame.sem timeline wait/signal
    //   9. wait fence, map host buf, log RGB & compare to §J.3.e.2.b CPU result.

    auto getDevProc = [&](const char* name) -> PFN_vkVoidFunction {
        return m_PlVkInstance->get_proc_addr(m_PlVkInstance->instance, name);
    };
    static auto pfnBegin       = (PFN_vkBeginCommandBuffer)getDevProc("vkBeginCommandBuffer");
    static auto pfnEnd         = (PFN_vkEndCommandBuffer)getDevProc("vkEndCommandBuffer");
    static auto pfnReset       = (PFN_vkResetCommandBuffer)getDevProc("vkResetCommandBuffer");
    static auto pfnBarrier     = (PFN_vkCmdPipelineBarrier)getDevProc("vkCmdPipelineBarrier");
    static auto pfnCopyImgBuf  = (PFN_vkCmdCopyImageToBuffer)getDevProc("vkCmdCopyImageToBuffer");
    static auto pfnCopyBuf     = (PFN_vkCmdCopyBuffer)getDevProc("vkCmdCopyBuffer");
    static auto pfnBindPipe    = (PFN_vkCmdBindPipeline)getDevProc("vkCmdBindPipeline");
    static auto pfnBindDS      = (PFN_vkCmdBindDescriptorSets)getDevProc("vkCmdBindDescriptorSets");
    static auto pfnPushC       = (PFN_vkCmdPushConstants)getDevProc("vkCmdPushConstants");
    static auto pfnDispatch    = (PFN_vkCmdDispatch)getDevProc("vkCmdDispatch");
    static auto pfnGetQueue    = (PFN_vkGetDeviceQueue)getDevProc("vkGetDeviceQueue");
    static auto pfnSubmit      = (PFN_vkQueueSubmit)getDevProc("vkQueueSubmit");
    static auto pfnWaitFences  = (PFN_vkWaitForFences)getDevProc("vkWaitForFences");
    static auto pfnResetFences = (PFN_vkResetFences)getDevProc("vkResetFences");
    static auto pfnMap         = (PFN_vkMapMemory)getDevProc("vkMapMemory");
    static auto pfnUnmap       = (PFN_vkUnmapMemory)getDevProc("vkUnmapMemory");
    if (!pfnBegin || !pfnEnd || !pfnReset || !pfnBarrier || !pfnCopyImgBuf || !pfnCopyBuf
        || !pfnBindPipe || !pfnBindDS || !pfnPushC || !pfnDispatch || !pfnGetQueue
        || !pfnSubmit || !pfnWaitFences || !pfnResetFences || !pfnMap || !pfnUnmap) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 probe: missing PFN");
        m_FrucNv12RgbDisabled = true;
        return false;
    }

    VkCommandBuffer cb   = (VkCommandBuffer)m_FrucNv12RgbCmdBuf;
    VkImage         img  = (VkImage)vkFrame->img[0];
    VkImageLayout   orig = vkFrame->layout[0];
    const uint32_t  W    = m_FrucNv12RgbWidth;
    const uint32_t  H    = m_FrucNv12RgbHeight;

    pfnReset(cb, 0);
    VkCommandBufferBeginInfo bbi = {};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBegin(cb, &bbi);

    auto buildImgBarrier = [&](VkImageAspectFlags aspect, VkImageLayout oldL, VkImageLayout newL,
                                VkAccessFlags srcA, VkAccessFlags dstA) -> VkImageMemoryBarrier {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        return b;
    };

    // Step 1: orig → TRANSFER_SRC for both planes.
    VkImageMemoryBarrier toSrc[2] = {
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, orig, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, orig, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
    };
    pfnBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, 0, nullptr, 0, nullptr, 2, toSrc);

    // Step 2: vkCmdCopyImageToBuffer × 2.  Plane 0 is W×H (R8 / 1 byte each).
    // Plane 1 is (W/2)×(H/2) of R8G8 (2 bytes each → W*H/2 bytes total).
    VkBufferImageCopy regY = {};
    regY.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
    regY.imageSubresource.layerCount = 1;
    regY.imageExtent = {W, H, 1};
    pfnCopyImgBuf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucNv12RgbBufY, 1, &regY);

    VkBufferImageCopy regUV = {};
    regUV.imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
    regUV.imageSubresource.layerCount = 1;
    regUV.imageExtent = {W / 2, H / 2, 1};
    pfnCopyImgBuf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  (VkBuffer)m_FrucNv12RgbBufUV, 1, &regUV);

    // Step 3: buffer barrier (TRANSFER_WRITE → SHADER_READ on bufY/bufUV).
    VkBufferMemoryBarrier bufBar1[2] = {};
    bufBar1[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufBar1[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufBar1[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bufBar1[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufBar1[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufBar1[0].buffer = (VkBuffer)m_FrucNv12RgbBufY;
    bufBar1[0].size = VK_WHOLE_SIZE;
    bufBar1[1] = bufBar1[0];
    bufBar1[1].buffer = (VkBuffer)m_FrucNv12RgbBufUV;
    pfnBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               0, 0, nullptr, 2, bufBar1, 0, nullptr);

    // Step 4: dispatch NV12→RGB compute shader.
    pfnBindPipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)m_FrucNv12RgbVkPipeline);
    VkDescriptorSet dSet = (VkDescriptorSet)m_FrucNv12RgbDescSet;
    pfnBindDS(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
              (VkPipelineLayout)m_FrucNv12RgbVkPipeLay,
              0, 1, &dSet, 0, nullptr);
    int pushVals[2] = {(int)W, (int)H};
    pfnPushC(cb, (VkPipelineLayout)m_FrucNv12RgbVkPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
             0, sizeof(pushVals), pushVals);
    const uint32_t gx = (W + 7) / 8;
    const uint32_t gy = (H + 7) / 8;
    pfnDispatch(cb, gx, gy, 1);

    // Step 5: buffer barrier (SHADER_WRITE → TRANSFER_READ + SHADER_READ on bufRGB).
    // dstAccessMask covers both consumers: §J.3.e.2.c2 vkCmdCopyBuffer host readback
    // (TRANSFER_READ) AND §J.3.e.2.d reverse pass (SHADER_READ).  dstStageMask
    // also widened to TRANSFER | COMPUTE_SHADER.
    VkBufferMemoryBarrier bufBar2 = {};
    bufBar2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufBar2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bufBar2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    bufBar2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufBar2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufBar2.buffer = (VkBuffer)m_FrucNv12RgbBufRGB;
    bufBar2.size = VK_WHOLE_SIZE;
    pfnBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               0, 0, nullptr, 1, &bufBar2, 0, nullptr);

    // Step 6: copy 3 plane center-pixel floats to host buffer.  Center pixel
    // index = (H/2) * W + (W/2).  Layout: planar [R-plane][G-plane][B-plane],
    // each plane W*H floats.  byteOffset_R = idx*4, byteOffset_G = idx*4 + W*H*4,
    // byteOffset_B = idx*4 + 2*W*H*4.
    const VkDeviceSize centerIdx = (VkDeviceSize)(H / 2) * W + (W / 2);
    const VkDeviceSize planeBytes = (VkDeviceSize)W * H * sizeof(float);
    VkBufferCopy bcRegions[3] = {};
    bcRegions[0].srcOffset = centerIdx * sizeof(float);
    bcRegions[0].dstOffset = 0;
    bcRegions[0].size = sizeof(float);
    bcRegions[1].srcOffset = centerIdx * sizeof(float) + planeBytes;
    bcRegions[1].dstOffset = sizeof(float);
    bcRegions[1].size = sizeof(float);
    bcRegions[2].srcOffset = centerIdx * sizeof(float) + 2 * planeBytes;
    bcRegions[2].dstOffset = 2 * sizeof(float);
    bcRegions[2].size = sizeof(float);
    pfnCopyBuf(cb, (VkBuffer)m_FrucNv12RgbBufRGB, (VkBuffer)m_FrucNv12RgbHostBuf,
               3, bcRegions);

    // §J.3.e.2.d — reverse pass + center pixel RGBA8 readback (gated PROBE4=1).
    // Reads bufRGB (already SHADER_READ-visible from Step 5 barrier), writes
    // RGBA8 storage image, then copies center texel to m_FrucRgbImgHostBuf.
    bool ranReverse = false;
    const char* probe4 = SDL_getenv("VIPLE_VK_FRUC_PROBE4");
    if (probe4 && SDL_atoi(probe4) != 0) {
        if (!m_FrucRgbImgReady && !m_FrucRgbImgDisabled) {
            initFrucRgbImgResources();
        }
        if (m_FrucRgbImgReady) {
            if (runRgbImgReversePass(cb, W, H)) {
                // Image now has fresh content in GENERAL layout.  Transition to
                // TRANSFER_SRC for readback, copy 1 RGBA8 texel, transition back.
                VkImageMemoryBarrier toSrc = {};
                toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toSrc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toSrc.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toSrc.image = (VkImage)m_FrucRgbImgImage;
                toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toSrc.subresourceRange.levelCount = 1;
                toSrc.subresourceRange.layerCount = 1;
                pfnBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toSrc);

                VkBufferImageCopy regCenter = {};
                regCenter.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                regCenter.imageSubresource.layerCount = 1;
                regCenter.imageOffset = {(int32_t)(W / 2), (int32_t)(H / 2), 0};
                regCenter.imageExtent = {1, 1, 1};
                pfnCopyImgBuf(cb, (VkImage)m_FrucRgbImgImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              (VkBuffer)m_FrucRgbImgHostBuf, 1, &regCenter);

                VkImageMemoryBarrier toGen = toSrc;
                toGen.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                toGen.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toGen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                toGen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                pfnBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toGen);

                ranReverse = true;
            }
        }
    }

    // Step 7: restore plane layouts.
    VkImageMemoryBarrier toOrig[2] = {
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, orig,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
        buildImgBarrier(VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, orig,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT),
    };
    pfnBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               0, 0, nullptr, 0, nullptr, 2, toOrig);

    pfnEnd(cb);

    // Step 8: submit on compute queue with AVVkFrame.sem timeline wait/signal.
    VkQueue computeQueue = VK_NULL_HANDLE;
    pfnGetQueue(m_Vulkan->device, m_Vulkan->queue_compute.index, 0, &computeQueue);

    uint64_t waitVal   = vkFrame->sem_value[0];
    uint64_t signalVal = waitVal + 1;
    VkSemaphore sem    = vkFrame->sem[0];
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

    auto t0 = std::chrono::high_resolution_clock::now();

    m_Vulkan->lock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);
    VkResult subRes = pfnSubmit(computeQueue, 1, &si, (VkFence)m_FrucNv12RgbFence);
    m_Vulkan->unlock_queue(m_Vulkan, m_Vulkan->queue_compute.index, 0);

    if (subRes != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 probe: vkQueueSubmit rc=%d", (int)subRes);
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    vkFrame->sem_value[0] = signalVal;

    // Step 9: wait fence, map host buf, log + compare.
    VkFence fence = (VkFence)m_FrucNv12RgbFence;
    VkResult wr = pfnWaitFences(m_Vulkan->device, 1, &fence, VK_TRUE, /*1s*/ 1000ULL * 1000 * 1000);
    if (wr != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.c2 probe: vkWaitForFences rc=%d", (int)wr);
        m_FrucNv12RgbDisabled = true;
        return false;
    }
    pfnResetFences(m_Vulkan->device, 1, &fence);

    auto t1 = std::chrono::high_resolution_clock::now();
    double dispatchMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    void* mapped = nullptr;
    pfnMap(m_Vulkan->device, (VkDeviceMemory)m_FrucNv12RgbHostBufMem, 0, VK_WHOLE_SIZE, 0, &mapped);
    float r = 0, g = 0, b = 0;
    if (mapped) {
        const float* p = (const float*)mapped;
        r = p[0]; g = p[1]; b = p[2];
        pfnUnmap(m_Vulkan->device, (VkDeviceMemory)m_FrucNv12RgbHostBufMem);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-FRUC] §J.3.e.2.c2 probe: PASS  %ux%u dispatch+readback=%.2fms  "
                "GPU center RGB (%.3f, %.3f, %.3f) — compare to §J.3.e.2.b CPU result",
                W, H, dispatchMs, r, g, b);

    if (ranReverse) {
        // Map §J.3.e.2.d host buf, log RGBA8, compare to scaled (*255) §J.3.e.2.c2 RGB.
        void* mapped2 = nullptr;
        pfnMap(m_Vulkan->device, (VkDeviceMemory)m_FrucRgbImgHostBufMem, 0, VK_WHOLE_SIZE, 0, &mapped2);
        uint8_t r8 = 0, g8 = 0, b8 = 0, a8 = 0;
        if (mapped2) {
            const uint8_t* q = (const uint8_t*)mapped2;
            r8 = q[0]; g8 = q[1]; b8 = q[2]; a8 = q[3];
            pfnUnmap(m_Vulkan->device, (VkDeviceMemory)m_FrucRgbImgHostBufMem);
        }
        // Expected RGBA8 from forward output (*255): ideally close to (r*255, g*255, b*255, 255).
        const int er = (int)(r * 255.0f + 0.5f), eg = (int)(g * 255.0f + 0.5f), eb = (int)(b * 255.0f + 0.5f);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-FRUC] §J.3.e.2.d probe: PASS  RGBA8 from VkImage (%u, %u, %u, %u) "
                    "vs expected (%d, %d, %d, 255) — delta(R/G/B)=(%d, %d, %d)",
                    (unsigned)r8, (unsigned)g8, (unsigned)b8, (unsigned)a8,
                    er, eg, eb,
                    (int)r8 - er, (int)g8 - eg, (int)b8 - eb);
    }
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

        // §J.3.e.2.c — NV12→RGB compute pipeline + per-frame dispatch probe.
        // Gated on VIPLE_VK_FRUC_PROBE3=1 + VIPLE_PLVK_NCNN_HANDOFF=1.
        // First fire at frame 30 (init + first dispatch), then every 60 frames
        // after.  Per-instance counter so test-decode + reconnect cycles each
        // get a fresh window.
        const char* probe3 = SDL_getenv("VIPLE_VK_FRUC_PROBE3");
        if (probe3 && SDL_atoi(probe3) != 0 && m_NcnnExternalReady) {
            ++m_FrucNv12RgbFrameCount;
            uint64_t n = m_FrucNv12RgbFrameCount;
            if (n == 30 || (n > 30 && ((n - 30) % 60) == 0)) {
                auto* vkFrame = (AVVkFrame*)frame->data[0];
                if (vkFrame) {
                    runNv12RgbProbe(vkFrame, frame);
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

    // §J.3.e.2.e1b — when VIPLE_VK_FRUC_OUTPUT_OVERRIDE=1 and our pl_tex /
    // override resources are ready, run forward+reverse compute every frame
    // and use our wrapped RGBA8 VkImage as the pl_render_image source.  This
    // proves the end-to-end pipeline (NV12→RGB→VkImage→swapchain) without
    // RIFE yet — §J.3.e.2.e2 will insert the RIFE forward in between.
    pl_frame ourFrame = {};
    bool useOverride = false;
    pl_tex heldTex = nullptr;
    const char* ovEnv = SDL_getenv("VIPLE_VK_FRUC_OUTPUT_OVERRIDE");
    if (ovEnv && SDL_atoi(ovEnv) != 0
        && m_NcnnExternalReady
        && frame->format == AV_PIX_FMT_VULKAN) {
        // Lazy-init §J.3.e.2.c forward + §J.3.e.2.d reverse + §J.3.e.2.e1
        // wrap/override resources (normally gated behind probe env vars).
        if (!m_FrucNv12RgbReady && !m_FrucNv12RgbDisabled) {
            initFrucNv12RgbResources((uint32_t)frame->width, (uint32_t)frame->height);
        }
        if (m_FrucNv12RgbReady && !m_FrucRgbImgReady && !m_FrucRgbImgDisabled) {
            initFrucRgbImgResources();
        }
        auto* vkFrame = (AVVkFrame*)frame->data[0];
        if (vkFrame && m_FrucOverrideReady && m_FrucRgbImgPlTex
            && runFrucOverridePass(vkFrame, frame)) {
            // Image now in GENERAL with new content.  Hand to libplacebo via
            // pl_vulkan_release_ex (sem omitted because we host-fenced above).
            heldTex = (pl_tex)m_FrucRgbImgPlTex;
            pl_vulkan_release_params relP = {};
            relP.tex = heldTex;
            relP.layout = VK_IMAGE_LAYOUT_GENERAL;
            relP.qf = VK_QUEUE_FAMILY_IGNORED;
            // semaphore left zero-initialized (optional per pl_vulkan_release_params)
            pl_vulkan_release_ex(m_Vulkan->gpu, &relP);

            // Build ourFrame from mappedFrame template, then override planes
            // to point at our single RGBA8 pl_tex.  Keep mappedFrame's color/
            // repr metadata so libplacebo's tone-mapping/dither stays sane —
            // but force repr.sys=RGB and repr.levels=FULL since our shader did
            // limited→full + YCbCr→RGB matrix.
            ourFrame = mappedFrame;
            ourFrame.num_planes = 1;
            ourFrame.planes[0] = {};
            ourFrame.planes[0].texture = heldTex;
            ourFrame.planes[0].components = 4;
            ourFrame.planes[0].component_mapping[0] = 0; // R
            ourFrame.planes[0].component_mapping[1] = 1; // G
            ourFrame.planes[0].component_mapping[2] = 2; // B
            ourFrame.planes[0].component_mapping[3] = 3; // A
            ourFrame.repr.sys = PL_COLOR_SYSTEM_RGB;
            ourFrame.repr.levels = PL_COLOR_LEVELS_FULL;
            useOverride = true;
        }
    }

    const pl_frame* renderSrc = useOverride ? &ourFrame : &mappedFrame;
    if (!pl_render_image(m_Renderer, renderSrc, &targetFrame, &pl_render_fast_params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "pl_render_image() failed");
        // NB: We must fallthrough to call pl_swapchain_submit_frame()
    }

    // §J.3.e.2.e1b — if we used the override path, take ownership back via
    // pl_vulkan_hold_ex with the timeline sem at the next value.  libplacebo
    // signals this sem when its sampling/render submit completes; runFrucOverridePass
    // will host-wait at that value before next frame's dispatch.
    if (useOverride && heldTex) {
        ++m_FrucOverrideHoldVal;
        pl_vulkan_hold_params holdP = {};
        holdP.tex = heldTex;
        holdP.layout = VK_IMAGE_LAYOUT_GENERAL;
        holdP.qf = VK_QUEUE_FAMILY_IGNORED;
        holdP.semaphore.sem = (VkSemaphore)m_FrucOverrideHoldSem;
        holdP.semaphore.value = m_FrucOverrideHoldVal;
        pl_vulkan_hold_ex(m_Vulkan->gpu, &holdP);
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
