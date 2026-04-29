// VipleStream §J.3.e.2.i — VkFrucRenderer
//
// Native Vulkan FRUC renderer ported from
// `moonlight-android/app/src/main/jni/moonlight-core/vk_backend.c`.
// Bypasses libplacebo entirely: owns its own VkSwapchainKHR, calls
// vkAcquireNextImageKHR / vkQueuePresentKHR directly, single
// vkQueueSubmit + 2× vkQueuePresentKHR for dual-present (interp + real).
//
// See docs/J.3.e.2.i_vulkan_native_renderer.md for the full design.

#pragma once

#include "renderer.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <vector>

class VkFrucRenderer : public IFFmpegRenderer {
public:
    VkFrucRenderer(int pass);
    ~VkFrucRenderer() override;

    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    void renderFrame(AVFrame* frame) override;

    InitFailureReason getInitFailureReason() override { return m_InitFailureReason; }
    int getDecoderCapabilities() override;
    int getRendererAttributes() override;
    int getDecoderColorspace() override { return COLORSPACE_REC_709; }

    void notifyOverlayUpdated(Overlay::OverlayType) override {}

private:
    // §J.3.e.2.i.2 sub-helpers — port from Android vk_backend.c
    bool createInstanceAndSurface(SDL_Window* window);
    bool pickPhysicalDeviceAndQueue();
    bool createLogicalDevice();
    bool createSwapchain();
    void destroySwapchain();
    bool createYcbcrSamplerAndLayouts();
    void destroyYcbcrSamplerAndLayouts();
    void teardown();

    int m_Pass;
    InitFailureReason m_InitFailureReason = InitFailureReason::Unknown;

    SDL_Window* m_Window = nullptr;

    // §J.3.e.2.i.2 — Vulkan handles owned by VkFrucRenderer (NOT shared
    // with PlVkRenderer's libplacebo handles — they're alternatives).
    VkInstance       m_Instance       = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice         m_Device         = VK_NULL_HANDLE;
    VkSurfaceKHR     m_Surface        = VK_NULL_HANDLE;

    // Single graphics+present queue family (PC NVIDIA / AMD typically
    // expose a "universal" queue family; Android's same-family pattern
    // ports cleanly).  Compute work also runs on this queue — we don't
    // need async compute for the FRUC pipeline (motion est ~0.7ms is
    // already under frame budget).
    uint32_t m_QueueFamily       = UINT32_MAX;  // graphics + present (universal)
    uint32_t m_DecodeQueueFamily = UINT32_MAX;  // VK_QUEUE_VIDEO_DECODE_BIT_KHR (separate on NV)
    uint32_t m_DecodeQueueCount  = 0;
    VkQueue  m_GraphicsQueue     = VK_NULL_HANDLE;

    // §J.3.e.2.i.3.a — ffmpeg AVHWDeviceContext bridges our VkDevice
    // to ffmpeg's Vulkan video decoder so AVVkFrame.img[0] gets created
    // on the same device our graphics pipeline samples from.
    AVBufferRef* m_HwDeviceCtx = nullptr;
    int          m_VideoFormat = 0;
    bool populateAvHwDeviceCtx(int videoFormat);
    static void  lockQueueStub(struct AVHWDeviceContext* ctx, uint32_t qf, uint32_t idx);
    static void  unlockQueueStub(struct AVHWDeviceContext* ctx, uint32_t qf, uint32_t idx);

    // §J.3.e.2.i.2.b — swapchain
    VkSwapchainKHR             m_Swapchain       = VK_NULL_HANDLE;
    VkFormat                   m_SwapchainFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR            m_SwapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D                 m_SwapchainExtent = { 0, 0 };
    VkPresentModeKHR           m_SwapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>       m_SwapchainImages;
    std::vector<VkImageView>   m_SwapchainViews;

    // §J.3.e.2.i.3.b — VkSamplerYcbcrConversion + sampler + layouts.
    // The sampler must be IMMUTABLE on the descriptor binding so that
    // GLSL fragment shader sees a single combined image+sampler with
    // YCbCr→RGB conversion baked in (ports Android's ensure_ycbcr_sampler).
    VkSamplerYcbcrConversion m_YcbcrConversion = VK_NULL_HANDLE;
    VkSampler                m_YcbcrSampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout    m_DescSetLayout   = VK_NULL_HANDLE;
    VkPipelineLayout         m_GraphicsPipelineLayout = VK_NULL_HANDLE;

    // §J.3.e.2.i.3+ — populated by later sub-phases:
    //   • m_Swapchain + image array + image views
    //   • m_RenderPass / m_GraphicsPipeline / m_DescPool / etc.
    //   • Compute pipeline (motionest / median / warp) re-using §J.3.e.2.h.a shaders
    //   • Per-slot in-flight ring (acquire+renderDone sems × 2 for dual-present)

    // Loaded PFNs (we don't have the libplacebo wrapper's lookup; use
    // SDL_Vulkan_GetVkGetInstanceProcAddr + raw vk*ProcAddr chain).
    PFN_vkGetInstanceProcAddr m_pfnGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance     m_pfnDestroyInstance     = nullptr;
    PFN_vkDestroyDevice       m_pfnDestroyDevice       = nullptr;
    PFN_vkDestroySurfaceKHR   m_pfnDestroySurfaceKHR   = nullptr;
};

#endif // HAVE_LIBPLACEBO_VULKAN
