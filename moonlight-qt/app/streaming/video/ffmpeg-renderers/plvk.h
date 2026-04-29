#pragma once

#include "renderer.h"

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

class PlVkRenderer : public IFFmpegRenderer {
public:
    PlVkRenderer(bool hwaccel = false, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual void waitToRender() override;
    virtual void cleanupRenderContext() override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int getDecoderCapabilities() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;
    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) override;

private:
    static void lockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void unlockQueue(AVHWDeviceContext *dev_ctx, uint32_t queue_family, uint32_t index);
    static void overlayUploadComplete(void* opaque);

    bool mapAvFrameToPlacebo(const AVFrame *frame, pl_frame* mappedFrame);
    bool populateQueues(int videoFormat);
    bool chooseVulkanDevice(PDECODER_PARAMETERS params, bool hdrOutputRequired);
    bool tryInitializeDevice(VkPhysicalDevice device, VkPhysicalDeviceProperties* deviceProps,
                             PDECODER_PARAMETERS decoderParams, bool hdrOutputRequired);
    bool isExtensionSupportedByPhysicalDevice(VkPhysicalDevice device, const char* extensionName);
    bool isPresentModeSupportedByPhysicalDevice(VkPhysicalDevice device, VkPresentModeKHR presentMode);
    bool isColorSpaceSupportedByPhysicalDevice(VkPhysicalDevice device, VkColorSpaceKHR colorSpace);
    bool isSurfacePresentationSupportedByPhysicalDevice(VkPhysicalDevice device);

    // §J.3.e.1.d — opt-in handoff of libplacebo's VkInstance/PhysDev/Device
    // triple to ncnn via the new external API.  Gated on
    // VIPLE_PLVK_NCNN_HANDOFF=1.  When successful, ncnn lives on the same
    // VkDevice as PlVkRenderer's libplacebo, ready for §J.3.e.2 to plug
    // AVVkFrame.img[0] (NV12) directly into ncnn::VkMat without crossing
    // VkDevices.  Lifetime: must be torn down (ncnn::destroy_gpu_instance)
    // BEFORE pl_vulkan_destroy, since ncnn-allocated VkPipeline/VkBuffer
    // ride the libplacebo VkDevice.
    bool initializeNcnnExternalHandoff();
    void teardownNcnnExternalHandoff();

    // §J.3.e.2.a — layout transition + 1-pixel readback probe for AVVkFrame.img[0].
    // Validates cross-queue-family ownership transfer (decode → compute) and
    // VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR → VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
    // transition.  Gated on VIPLE_VK_FRUC_PROBE2=1.  Ran every 60 frames; if any
    // step fails the per-instance probe is permanently disabled.  Resources are
    // allocated lazily on first call and freed in dtor.
    bool initFrucProbeResources();
    void destroyFrucProbeResources();
    bool runLayoutTransitionProbe(struct AVVkFrame* vkFrame, AVFrame* frame);

    // §J.3.e.2.c — full-frame NV12 → planar fp32 RGB compute pipeline.
    // Pipeline: vkCmdCopyImageToBuffer (NV12 plane-0 → bufY, plane-1 → bufUV) →
    // compute shader (BT.709 limited-range YCbCr → linear sRGB) → planar fp32
    // RGB output buffer.  Storage-buffer-only shader so we reuse ncnn's
    // compile_spirv_module + ncnn::Pipeline machinery (samplers would need a
    // different compilation path).  Gated on VIPLE_VK_FRUC_PROBE3=1 — runs every
    // 60 frames, dispatches W*H workgroups, reads back a 12-byte center-pixel
    // window for correctness check vs §J.3.e.2.b's CPU BT.709 result.
    bool initFrucNv12RgbResources(uint32_t width, uint32_t height);
    void destroyFrucNv12RgbResources();
    bool runNv12RgbProbe(struct AVVkFrame* vkFrame, AVFrame* frame);

    // The backend renderer if we're frontend-only
    IFFmpegRenderer* m_Backend;
    bool m_HwAccelBackend;

    // SDL state
    SDL_Window* m_Window = nullptr;

    // The libplacebo rendering state
    pl_log m_Log = nullptr;
    pl_vk_inst m_PlVkInstance = nullptr;
    VkSurfaceKHR m_VkSurface = VK_NULL_HANDLE;
    pl_vulkan m_Vulkan = nullptr;
    pl_swapchain m_Swapchain = nullptr;
    pl_renderer m_Renderer = nullptr;
    pl_tex m_Textures[PL_MAX_PLANES] = {};
    pl_color_space m_LastColorspace = {};

    // Pending swapchain state shared between waitToRender(), renderFrame(), and cleanupRenderContext()
    pl_swapchain_frame m_SwapchainFrame = {};
    bool m_HasPendingSwapchainFrame = false;

    // Overlay state
    SDL_SpinLock m_OverlayLock = 0;
    struct {
        // The staging overlay state is copied here under the overlay lock in the render thread.
        //
        // These values can be safely read by the render thread outside of the overlay lock,
        // but the copy from stagingOverlay to overlay must only happen under the overlay
        // lock when hasStagingOverlay is true.
        bool hasOverlay;
        pl_overlay overlay;

        // This state is written by the overlay update thread
        //
        // NB: hasStagingOverlay may be false even if there is a staging overlay texture present,
        // because this is how the overlay update path indicates that the overlay is not currently
        // safe for the render thread to read.
        //
        // It is safe for the overlay update thread to write to stagingOverlay outside of the lock,
        // as long as hasStagingOverlay is false.
        bool hasStagingOverlay;
        pl_overlay stagingOverlay;
    } m_Overlays[Overlay::OverlayMax] = {};

    // Device context used for hwaccel decoders
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Vulkan functions we call directly
    PFN_vkDestroySurfaceKHR fn_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 fn_vkGetPhysicalDeviceQueueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fn_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fn_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkEnumeratePhysicalDevices fn_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fn_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fn_vkEnumerateDeviceExtensionProperties = nullptr;

    // §J.3.e.1.d — true while ncnn singleton is initialised in external
    // mode against this PlVkRenderer's libplacebo VkDevice.  Set after a
    // successful initializeNcnnExternalHandoff(); cleared by
    // teardownNcnnExternalHandoff() (must run before pl_vulkan_destroy).
    bool m_NcnnExternalReady = false;

    // §J.3.e.2.a — frame-recurrent layout transition probe state.
    // Resources lazily allocated on first probe call; held until dtor.
    // Stored as opaque so the header doesn't drag in <vulkan/vulkan.h>.
    bool  m_FrucProbeInitialised = false;
    bool  m_FrucProbeDisabled    = false;
    void* m_FrucProbeCmdPool     = nullptr;  // VkCommandPool
    void* m_FrucProbeCmdBuf      = nullptr;  // VkCommandBuffer
    void* m_FrucProbeBuffer      = nullptr;  // VkBuffer (host-visible readback)
    void* m_FrucProbeBufferMem   = nullptr;  // VkDeviceMemory
    void* m_FrucProbeFence       = nullptr;  // VkFence

    // §J.3.e.2.c — NV12 → planar fp32 RGB compute pipeline state.
    // Held as opaque pointers; types live in the ncnn:: / Vk:: spaces hidden
    // behind the .cpp.  The ncnn::Pipeline rides the same VkDevice as the
    // §J.3.e.1.d external handoff (m_Vulkan->device).
    bool     m_FrucNv12RgbReady    = false;
    bool     m_FrucNv12RgbDisabled = false;
    uint32_t m_FrucNv12RgbWidth    = 0;
    uint32_t m_FrucNv12RgbHeight   = 0;
    // §J.3.e.2.c1 raw VkPipeline path (ncnn::Pipeline::create crashes on
    // shaders with non-ncnn-Mat binding semantics — see git log for details).
    void*    m_FrucNv12RgbVkShader   = nullptr;  // VkShaderModule
    void*    m_FrucNv12RgbVkDsl      = nullptr;  // VkDescriptorSetLayout
    void*    m_FrucNv12RgbVkPipeLay  = nullptr;  // VkPipelineLayout
    void*    m_FrucNv12RgbVkPipeline = nullptr;  // VkPipeline (compute)
    void*    m_FrucNv12RgbBufY     = nullptr;  // VkBuffer (W*H bytes)
    void*    m_FrucNv12RgbBufYMem  = nullptr;  // VkDeviceMemory
    void*    m_FrucNv12RgbBufUV    = nullptr;  // VkBuffer (W*H/2 bytes)
    void*    m_FrucNv12RgbBufUVMem = nullptr;  // VkDeviceMemory
    void*    m_FrucNv12RgbBufRGB   = nullptr;  // VkBuffer (W*H*3*sizeof(float))
    void*    m_FrucNv12RgbBufRGBMem= nullptr;  // VkDeviceMemory
    void*    m_FrucNv12RgbHostBuf  = nullptr;  // VkBuffer (12 bytes host-visible readback)
    void*    m_FrucNv12RgbHostBufMem= nullptr; // VkDeviceMemory
    void*    m_FrucNv12RgbCmdPool  = nullptr;  // VkCommandPool
    void*    m_FrucNv12RgbCmdBuf   = nullptr;  // VkCommandBuffer
    void*    m_FrucNv12RgbFence    = nullptr;  // VkFence
    uint64_t m_FrucNv12RgbFrameCount = 0;
};
