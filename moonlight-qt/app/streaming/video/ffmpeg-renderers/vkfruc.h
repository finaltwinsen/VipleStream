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

    // §J.3.e.2.i.3.e-SW — software-decode pixel format hooks.  When the
    // env-var VIPLE_VKFRUC_SW=1 is set, this renderer is reachable via the
    // software cascade in ffmpeg.cpp (TRY_PREFERRED_PIXEL_FORMAT).  Returns
    // AV_PIX_FMT_NV12 so FFmpeg's get_format() picks software NV12 output
    // (no hwaccel), which we upload to our VkDevice via staging buffer.
    AVPixelFormat getPreferredPixelFormat(int videoFormat) override;
    bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override;

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
    bool createRenderPassAndFramebuffers();
    void destroyRenderPassAndFramebuffers();
    bool createGraphicsPipeline();
    void destroyGraphicsPipeline();
    bool createInFlightRing();
    void destroyInFlightRing();
    bool createSwUploadResources(int width, int height);
    void destroySwUploadResources();
    void renderFrameSw(AVFrame* frame);
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

    // §J.3.e.2.i.3.a — feature structs MUST persist for FFmpeg's lifetime
    // because vkCtx->device_features.pNext points into them.  Allocating
    // on stack in createLogicalDevice → use-after-free when FFmpeg later
    // walks the chain (NV driver crashes on NULL deref of feature state).
    VkPhysicalDeviceSynchronization2Features          m_Sync2Feat   = {};
    VkPhysicalDeviceTimelineSemaphoreFeatures         m_TimelineFeat = {};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures    m_YcbcrFeat   = {};
    VkPhysicalDeviceFeatures2                         m_DevFeat2    = {};
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

    // §J.3.e.2.i.3.c — render pass + framebuffer per swapchain image
    // + graphics pipeline (vertex + fragment).  Fragment shader samples
    // NV12 plane via the immutable ycbcr sampler from i.3.b.
    VkRenderPass               m_RenderPass         = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> m_Framebuffers;
    VkShaderModule             m_VertShaderModule   = VK_NULL_HANDLE;
    VkShaderModule             m_FragShaderModule   = VK_NULL_HANDLE;
    VkPipeline                 m_GraphicsPipeline   = VK_NULL_HANDLE;

    // §J.3.e.2.i.3.d — per-slot in-flight ring.  Mirrors Android's
    // VK_FRAMES_IN_FLIGHT pattern in moonlight-android/app/src/main/jni/
    // moonlight-core/vk_backend.c (init_in_flight_ring): N slots, each
    // with 1 cmd buffer + 2 acquireSems + 2 renderDoneSems + 1 fence.
    //
    // Two of each semaphore so §J.3.e.2.i.5 (dual-present interp + real)
    // can use the [0] pair for the interp present and the [1] pair for
    // the real-frame present without cross-pass sync hazards.  i.3.e
    // single-present uses only the [0] pair.
    static constexpr uint32_t kFrucFramesInFlight = 2;
    VkCommandPool   m_CmdPool                = VK_NULL_HANDLE;
    VkCommandBuffer m_SlotCmdBuf[kFrucFramesInFlight]            = {};
    VkSemaphore     m_SlotAcquireSem[kFrucFramesInFlight][2]     = {};
    VkSemaphore     m_SlotRenderDoneSem[kFrucFramesInFlight][2]  = {};
    VkFence         m_SlotInFlightFence[kFrucFramesInFlight]     = {};
    uint32_t        m_CurrentSlot            = 0;
    bool            m_RingInitialized        = false;

    // §J.3.e.2.i.3.e — descriptor pool + per-slot descriptor sets that
    // reference the AVVkFrame.img[0] image view of the frame currently
    // being rendered in that slot.  We allocate one set per ring slot;
    // each renderFrame() updates the slot's set with the new image view.
    //
    // Per-slot pending image view: AVVkFrame.img[0]'s VkImageView is
    // created fresh each frame (different AVFrame each call).  We can't
    // destroy it immediately because GPU is still reading from it after
    // submit — instead, the slot's "pending view" is destroyed on the
    // *next* iteration of that slot, after vkWaitForFences confirms the
    // previous frame has finished.
    bool createDescriptorPool();
    void destroyDescriptorPool();
    VkDescriptorPool m_DescPool                                  = VK_NULL_HANDLE;
    VkDescriptorSet  m_SlotDescSet[kFrucFramesInFlight]          = {};
    VkImageView      m_SlotPendingView[kFrucFramesInFlight]      = {};

    // §J.3.e.2.i.3.e — render-time PFNs cached at init for the hot path.
    // renderFrame() runs at ~60-120 fps; resolving these every call via
    // vkGetDeviceProcAddr would burn cycles on PFN lookup.  Cached once
    // by initialize() after device creation, used by renderFrame().
    struct {
        PFN_vkAcquireNextImageKHR    AcquireNextImageKHR;
        PFN_vkQueuePresentKHR        QueuePresentKHR;
        PFN_vkQueueSubmit            QueueSubmit;
        PFN_vkBeginCommandBuffer     BeginCommandBuffer;
        PFN_vkEndCommandBuffer       EndCommandBuffer;
        PFN_vkResetCommandBuffer     ResetCommandBuffer;
        PFN_vkCmdPipelineBarrier     CmdPipelineBarrier;
        PFN_vkCmdBeginRenderPass     CmdBeginRenderPass;
        PFN_vkCmdEndRenderPass       CmdEndRenderPass;
        PFN_vkCmdBindPipeline        CmdBindPipeline;
        PFN_vkCmdBindDescriptorSets  CmdBindDescriptorSets;
        PFN_vkCmdDraw                CmdDraw;
        PFN_vkWaitForFences          WaitForFences;
        PFN_vkResetFences            ResetFences;
        PFN_vkUpdateDescriptorSets   UpdateDescriptorSets;
        PFN_vkCreateImageView        CreateImageView;
        PFN_vkDestroyImageView       DestroyImageView;
        // §J.3.e.2.i.3.e-SW additions (only used when m_SwMode):
        PFN_vkCmdCopyBufferToImage   CmdCopyBufferToImage;
    } m_RtPfn = {};
    bool loadRenderTimePfns();

    // §J.3.e.2.i.3+ — populated by later sub-phases:
    //   • Compute pipeline (motionest / median / warp) re-using §J.3.e.2.h.a shaders (i.4)
    //   • Dual-present second-pass cmd record + present (i.5)

    // §J.3.e.2.i.3.e-SW — software-decode upload path resources.
    // VIPLE_VKFRUC_SW=1 opts in.  Validates VkFrucRenderer's graphics +
    // swapchain pipeline in isolation from FFmpeg-Vulkan hwcontext (which
    // crashes on first frame; see docs/J.3.e.2.i_vulkan_native_renderer.md
    // known-broken section).
    bool             m_SwMode             = false;
    int              m_SwImageWidth       = 0;
    int              m_SwImageHeight      = 0;
    VkBuffer         m_SwStagingBuffer    = VK_NULL_HANDLE;
    VkDeviceMemory   m_SwStagingMem       = VK_NULL_HANDLE;
    void*            m_SwStagingMapped    = nullptr;
    size_t           m_SwStagingSize      = 0;
    VkImage          m_SwUploadImage      = VK_NULL_HANDLE;
    VkDeviceMemory   m_SwUploadImageMem   = VK_NULL_HANDLE;
    VkImageView      m_SwUploadView       = VK_NULL_HANDLE;
    bool             m_SwImageLayoutInited = false;  // true after first transition

    // Loaded PFNs (we don't have the libplacebo wrapper's lookup; use
    // SDL_Vulkan_GetVkGetInstanceProcAddr + raw vk*ProcAddr chain).
    PFN_vkGetInstanceProcAddr m_pfnGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance     m_pfnDestroyInstance     = nullptr;
    PFN_vkDestroyDevice       m_pfnDestroyDevice       = nullptr;
    PFN_vkDestroySurfaceKHR   m_pfnDestroySurfaceKHR   = nullptr;
};

#endif // HAVE_LIBPLACEBO_VULKAN
