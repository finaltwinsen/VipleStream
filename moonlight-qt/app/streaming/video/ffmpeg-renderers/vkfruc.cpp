// VipleStream §J.3.e.2.i — VkFrucRenderer scaffold (i.1)
// See vkfruc.h header for full design rationale.

#include "vkfruc.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include "path.h"

VkFrucRenderer::VkFrucRenderer(int pass)
    : IFFmpegRenderer(RendererType::Vulkan)
    , m_Pass(pass)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.1 ctor (pass=%d)", pass);
}

VkFrucRenderer::~VkFrucRenderer()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.1 dtor");
    // §J.3.e.2.i.2+ — destruction order will mirror Android's
    // vk_backend.c: graphics pipeline → swapchain → device → instance.
}

bool VkFrucRenderer::initialize(PDECODER_PARAMETERS params)
{
    (void)params;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.1 initialize: scaffold-only, "
                "returning false (cascade falls through to PlVkRenderer)");
    // i.1 deliberately fails so existing PlVkRenderer keeps handling
    // streaming.  i.2 will replace with real instance/device/swapchain
    // bring-up.
    m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
    return false;
}

bool VkFrucRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    (void)context;
    (void)options;
    return false;
}

void VkFrucRenderer::renderFrame(AVFrame* frame)
{
    (void)frame;
    // i.1 stub — no rendering.  Should never be called because
    // initialize() returns false.
}

int VkFrucRenderer::getDecoderCapabilities()
{
    // Mirror PlVkRenderer's capabilities once i.2-i.6 are done.
    // For now return 0 so cascade doesn't pick us preferentially.
    return 0;
}

int VkFrucRenderer::getRendererAttributes()
{
    // i.1 stub — no special attributes
    return 0;
}

#endif // HAVE_LIBPLACEBO_VULKAN
