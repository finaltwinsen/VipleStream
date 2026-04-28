// VipleStream §J.3.b — Vulkan-native video decoder skeleton.
// See vulkanvideo.h for design rationale.

#ifdef _WIN32

#include "vulkanvideo.h"

#include <SDL.h>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
}

VulkanVideoRenderer::VulkanVideoRenderer()
    : IFFmpegRenderer(RendererType::Vulkan)
{
}

VulkanVideoRenderer::~VulkanVideoRenderer()
{
    if (m_HwDeviceCtx) {
        av_buffer_unref(&m_HwDeviceCtx);
    }
}

bool VulkanVideoRenderer::isFeasibleOnThisSystem()
{
    // §J.3.b.1: capability check is currently delegated to NcnnFRUC's
    // probe (see ncnnfruc.cpp §J.3.a / §J.3.b.0 blocks).  When this
    // class becomes the cascade entry point in §J.3.c, we'll move the
    // probe here as a static one-shot evaluator with a cached result.
    //
    // For now: assume feasible iff Windows + ffmpeg supports
    // AV_HWDEVICE_TYPE_VULKAN (compile-time check via header presence,
    // which app.pro guarantees via libavutil/hwcontext_vulkan.h).
    return true;
}

bool VulkanVideoRenderer::initialize(PDECODER_PARAMETERS params)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.1 initialize: %dx%d @ %d fps videoFormat=0x%x",
                params->width, params->height, params->frameRate, params->videoFormat);

    m_DecoderParams = *params;

    // §J.3.b.1: create ffmpeg AVHWDeviceContext for Vulkan.
    //
    // av_hwdevice_ctx_create with type=AV_HWDEVICE_TYPE_VULKAN and
    // device=NULL tells ffmpeg to pick a default Vulkan device.  ffmpeg
    // 8.x's hwcontext_vulkan handles VkInstance + VkDevice creation +
    // requires Vulkan 1.3 + auto-enables video extensions when available.
    //
    // Future §J.3.c will pass an existing VkDevice (shared with ncnn)
    // via AVVulkanDeviceContext fields, but for the skeleton we let
    // ffmpeg manage everything.
    AVBufferRef* hwDeviceCtx = nullptr;
    int rc = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VULKAN,
                                    /*device=*/nullptr, /*opts=*/nullptr, 0);
    if (rc < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, errBuf, sizeof(errBuf));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-VIDEO] §J.3.b.1 av_hwdevice_ctx_create(VULKAN) failed: %d (%s)",
                    rc, errBuf);
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        return false;
    }
    m_HwDeviceCtx = hwDeviceCtx;

    // Log what ffmpeg picked.
    auto* devCtx = (AVHWDeviceContext*)m_HwDeviceCtx->data;
    auto* vkCtx  = (AVVulkanDeviceContext*)devCtx->hwctx;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.1 SUCCESS — AVHWDeviceContext (Vulkan) created: "
                "VkInstance=%p VkPhysicalDevice=%p VkDevice=%p",
                (void*)vkCtx->inst, (void*)vkCtx->phys_dev, (void*)vkCtx->act_dev);

    // Walk queue families for diagnostics.  AVVulkanDeviceContext
    // exposes nb_qf + qf[] arrays of AVVulkanDeviceQueueFamily, each
    // tagged with VkQueueFlagBits + video_caps.
    for (int i = 0; i < vkCtx->nb_qf; ++i) {
        const auto& qf = vkCtx->qf[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-VIDEO] §J.3.b.1 qf[%d]: idx=%d num=%d flags=0x%x video_caps=0x%x",
                    i, qf.idx, qf.num, (unsigned)qf.flags, (unsigned)qf.video_caps);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.1 skeleton initialised — NOT wired into cascade. "
                "§J.3.b.2 will pass hw_device_ctx to AVCodecContext.");
    return true;
}

bool VulkanVideoRenderer::prepareDecoderContext(AVCodecContext* /*context*/, AVDictionary** /*options*/)
{
    // §J.3.b.2 will set context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx)
    // and install a get_format callback that picks AV_PIX_FMT_VULKAN.
    // For the skeleton, return false — we're not yet hooked into cascade.
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] prepareDecoderContext called — §J.3.b.1 skeleton, no impl yet");
    return false;
}

void VulkanVideoRenderer::renderFrame(AVFrame* /*frame*/)
{
    // §J.3.b.3 will receive AVFrame { data[0] = AVVkFrame } and either
    // (a) forward the VkImage to NCNN-Vulkan FRUC,
    // (b) blit to a Vulkan swapchain via VK_KHR_swapchain, or
    // (c) readback to CPU + present via legacy SDL path (debug only).
    //
    // For the skeleton this is unreachable — we don't get registered
    // in the cascade.
    uint64_t n = m_FrameCount.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-VIDEO] renderFrame called — §J.3.b.1 skeleton, no impl yet");
    }
}

IFFmpegRenderer::InitFailureReason VulkanVideoRenderer::getInitFailureReason()
{
    return m_InitFailureReason;
}

#endif // _WIN32
