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

bool VulkanVideoRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** /*options*/)
{
    // §J.3.b.2: same pattern as D3D11VARenderer — pass our pre-created
    // AVHWDeviceContext to ffmpeg.  The actual hw_frames_ctx is set up
    // later in prepareDecoderContextInGetFormat (called from get_format
    // callback once ffmpeg confirms the codec accepts AV_PIX_FMT_VULKAN).
    if (!m_HwDeviceCtx) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VK-VIDEO] prepareDecoderContext called before initialize succeeded");
        return false;
    }
    context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.2 prepareDecoderContext: hw_device_ctx attached to AVCodecContext");
    return true;
}

bool VulkanVideoRenderer::prepareDecoderContextInGetFormat(AVCodecContext* context,
                                                           AVPixelFormat pixelFormat)
{
    // §J.3.b.2: called from FFmpegVideoDecoder's get_format callback
    // once ffmpeg has decided AV_PIX_FMT_VULKAN is the chosen output
    // format.  We need to build an AVHWFramesContext describing the DPB
    // (decoded picture buffer) — ffmpeg will allocate VkImages from
    // this context and tag each AVFrame.data[0] = AVVkFrame.
    //
    // ffmpeg's avcodec_get_hw_frames_parameters does most of the work;
    // we just init the resulting context.  For Vulkan-native, the
    // hwctx-specific tweaks (BindFlags etc. on D3D11) don't apply —
    // ffmpeg's AVVulkanFramesContext defaults are sane.

    av_buffer_unref(&context->hw_frames_ctx);
    int err = avcodec_get_hw_frames_parameters(context, m_HwDeviceCtx,
                                               pixelFormat, &context->hw_frames_ctx);
    if (err < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, errBuf, sizeof(errBuf));
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VK-VIDEO] §J.3.b.2 avcodec_get_hw_frames_parameters failed: %d (%s)",
                     err, errBuf);
        return false;
    }

    auto* framesContext = (AVHWFramesContext*)context->hw_frames_ctx->data;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.2 hw_frames_ctx params: %dx%d sw_format=%d initial_pool=%d",
                framesContext->width, framesContext->height,
                (int)framesContext->sw_format, framesContext->initial_pool_size);

    // Mimic d3d11va: extra 3 frames in initial pool to absorb decoder
    // reordering buffer + present queue + free frame headroom.
    if (framesContext->initial_pool_size) {
        framesContext->initial_pool_size += 3;
    }

    err = av_hwframe_ctx_init(context->hw_frames_ctx);
    if (err < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, errBuf, sizeof(errBuf));
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VK-VIDEO] §J.3.b.2 av_hwframe_ctx_init failed: %d (%s)",
                     err, errBuf);
        av_buffer_unref(&context->hw_frames_ctx);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VK-VIDEO] §J.3.b.2 hw_frames_ctx init OK — DPB allocator ready");
    return true;
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
