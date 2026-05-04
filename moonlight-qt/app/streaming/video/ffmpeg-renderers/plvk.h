#pragma once

#include "renderer.h"

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <atomic>
#include <thread>

// §J.3.e.2.e2 — RIFE forward integration on the external ncnn instance.
// ncnn::VkMat / ncnn::Net are needed as members.  ncnn is Windows-only
// (prebuilt under libs/windows/ncnn/); §K.1 Linux build needs full
// VIPLESTREAM_HAVE_NCNN gating across plvk.cpp's ~149 ncnn references.
#include <ncnn/mat.h>
#include <memory>

namespace ncnn {
class Net;
}

class PlVkRenderer : public IFFmpegRenderer {
public:
    PlVkRenderer(bool hwaccel = false, IFFmpegRenderer *backendRenderer = nullptr);
    virtual ~PlVkRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    // §J.3.f — allocate AVHWFramesContext at get_format() time so ffmpeg's
    // *_vulkan hwaccels (h264_vulkan / hevc_vulkan / av1_vulkan) can finish
    // init.  Without this override, hwaccel init logs "A hardware frames or
    // device context is required" and falls back to SW decode.
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
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

    // §J.3.e.2.d — reverse converter: planar fp32 RGB buffer → RGBA8 VkImage.
    // The RGBA8 VkImage is created with USAGE_STORAGE (compute-shader writable)
    // + USAGE_SAMPLED (for libplacebo pl_vulkan_wrap → pl_tex → pl_render_image)
    // + USAGE_TRANSFER_SRC (for §J.3.e.2.d readback verification).  Layout
    // GENERAL (no transitions needed between dispatch and pl_tex sampling).
    // Resources allocated alongside §J.3.e.2.c init; per-frame dispatch fires
    // inside runNv12RgbProbe right after the forward dispatch when
    // VIPLE_VK_FRUC_PROBE4=1 is set.  Verifies RGBA8 readback matches scaled
    // §J.3.e.2.c2 GPU output.
    bool initFrucRgbImgResources();
    void destroyFrucRgbImgResources();
    bool runRgbImgReversePass(VkCommandBuffer cb, uint32_t width, uint32_t height);

    // §J.3.e.2.e1b — render-path override.  When VIPLE_VK_FRUC_OUTPUT_OVERRIDE=1
    // is set, every frame: (1) wait host-side on m_FrucOverrideHoldSem timeline
    // (signaled by libplacebo when previous frame's pl_render_image finished
    // with our pl_tex), (2) record forward+reverse compute dispatch on a
    // dedicated cmd buffer, (3) submit on compute queue with AVVkFrame.sem
    // wait/signal + local fence, (4) host-wait local fence (image now in
    // GENERAL with new content), (5) pl_vulkan_release_ex hands image to
    // libplacebo for sampling.  Caller (renderFrame) then runs pl_render_image
    // with our pl_tex as source plane and finally pl_vulkan_hold_ex to take
    // the image back when libplacebo signals the timeline.
    bool runFrucOverridePass(struct AVVkFrame* vkFrame, AVFrame* frame);

    // §J.3.e.2.e2a — load RIFE model on the external ncnn instance + allocate
    // ncnn::VkMat resources for prev/curr/timestep frames.  Gated on
    // VIPLE_VK_FRUC_RIFE=1; lazy-initialised on first override frame.  Uses
    // ncnn::get_gpu_device(0) which under §J.3.e.1.d external mode returns
    // libplacebo's VkDevice — no cross-device sync needed.  VkMats are
    // allocated via the device's blob_allocator (process-lifetime ncnn pool).
    bool initRifeModel(uint32_t width, uint32_t height);
    void destroyRifeModel();

    // §J.3.e.2.e2 (Path B) — same role as runFrucOverridePass but injects
    // RIFE forward between the NV12→bufRGB forward shader and the
    // bufRGB→VkImage reverse shader.  Three-phase per-frame submit:
    //   Phase A (our cmd buf): forward dispatch + vkCmdCopyBuffer bufRGB →
    //                          m_RifeRgbInHostBuf (HOST_VISIBLE staging)
    //   Phase B (CPU):         if has prev: wrap input staging as ncnn::Mat,
    //                          ex.extract(prev, curr, timestep) → CPU out_mat,
    //                          memcpy out_mat → m_RifeRgbOutHostBuf, clone
    //                          curr→prev.  If first frame: clone curr→prev,
    //                          phaseCSrcBuf = m_RifeRgbInHostBuf (pass-through).
    //   Phase C (our cmd buf): vkCmdCopyBuffer phaseCSrcBuf → bufRGB +
    //                          reverse shader (bufRGB → m_FrucRgbImgImage)
    // Each phase host-fenced for now (3 syncs/frame; §J.3.e.2.f will replace
    // with timeline sem chain for async pipelining).
    bool runFrucOverridePassWithRife(struct AVVkFrame* vkFrame, AVFrame* frame);

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
    void*    m_FrucNv12RgbDescPool = nullptr;  // VkDescriptorPool
    void*    m_FrucNv12RgbDescSet  = nullptr;  // VkDescriptorSet
    uint64_t m_FrucNv12RgbFrameCount = 0;

    // §J.3.e.2.d — reverse converter (planar fp32 RGB buffer → RGBA8 VkImage).
    bool     m_FrucRgbImgReady     = false;
    bool     m_FrucRgbImgDisabled  = false;
    void*    m_FrucRgbImgVkShader  = nullptr;  // VkShaderModule
    void*    m_FrucRgbImgVkDsl     = nullptr;  // VkDescriptorSetLayout (1 SSBO + 1 storage image)
    void*    m_FrucRgbImgVkPipeLay = nullptr;  // VkPipelineLayout
    void*    m_FrucRgbImgVkPipeline= nullptr;  // VkPipeline (compute)
    // §J.3.e.2.g.C — ring of 2 VkImages.  v1.3.111 benchmark identified
    // the single-image + pl_vulkan_hold_ex/release_ex timeline pattern
    // as the systematic frame#2 stall root cause.  Replacing with a
    // 2-deep ring (matches typical swapchain depth) lets libplacebo
    // sample ring[N] while we write ring[(N+1) % 2], no cross-frame
    // timeline sem needed — relies on pl_swapchain_start_frame's
    // implicit wait on the previous swap to gate ring slot reuse.
    static constexpr uint32_t FRUC_RGB_IMG_RING = 2;
    void*    m_FrucRgbImgImage    [FRUC_RGB_IMG_RING] = {};  // VkImage[2] (RGBA8)
    void*    m_FrucRgbImgImageMem [FRUC_RGB_IMG_RING] = {};
    void*    m_FrucRgbImgImageView[FRUC_RGB_IMG_RING] = {};
    void*    m_FrucRgbImgDescSet  [FRUC_RGB_IMG_RING] = {};  // VkDescriptorSet[2]
    void*    m_FrucRgbImgPlTex    [FRUC_RGB_IMG_RING] = {};  // pl_tex[2] (long-lived wrap)
    uint32_t m_FrucRgbImgRingIdx  = 0;  // monotonic frame counter, slot = idx % RING
    void*    m_FrucRgbImgDescPool  = nullptr;  // VkDescriptorPool (sized for RING sets)
    void*    m_FrucRgbImgHostBuf   = nullptr;  // VkBuffer (4 bytes RGBA8 readback, single)
    void*    m_FrucRgbImgHostBufMem= nullptr;  // VkDeviceMemory

    // §J.3.e.2.e1b — render-path override: hold-timeline semaphore + monotonic
    // value for ping-ponging image ownership between us (compute writes) and
    // libplacebo (sampling for pl_render_image).  Dedicated cmd buffer + fence
    // separate from §J.3.e.2.c2's probe path so override and probe can co-exist.
    bool     m_FrucOverrideReady    = false;
    void*    m_FrucOverrideHoldSem  = nullptr;  // VkSemaphore (timeline)
    uint64_t m_FrucOverrideHoldVal  = 0;
    uint64_t m_FrucOverrideFrameCount = 0;
    void*    m_FrucOverrideCmdPool   = nullptr;  // VkCommandPool (shared by Phase A + C)
    void*    m_FrucOverrideCmdBuf    = nullptr;  // VkCommandBuffer (Phase A submits on this)
    void*    m_FrucOverrideFence     = nullptr;  // VkFence (Phase A host-waits on this)
    // §J.3.e.2.g — Phase C uses its own cmd buf + fence to remove the
    // reset/reuse-within-frame race that v1.3.107 benchmark hit (§J.3.e.2.f
    // findings: same cmd buf + fence reset between Phase A and Phase C
    // collided with libplacebo swapchain present sync, frame#2 fence wait
    // timed out 100% systematically).  Cmd pool is shared (allocations only).
    void*    m_FrucOverridePhaseCCmdBuf = nullptr;  // VkCommandBuffer (Phase C submits on this)
    void*    m_FrucOverridePhaseCFence  = nullptr;  // VkFence (Phase C host-waits on this)

    // §J.3.e.2.e2 (Path B) — RIFE model + CPU ncnn::Mat + HOST_VISIBLE staging.
    //
    // Why CPU Mat (Path B) rather than VkMat (Path A): feeding ex.input(VkMat)
    // on an external-VkDevice ncnn singleton hits an MSVC virtual-inheritance
    // ABI bug in Convolution_vulkan's internal Padding sub-layer dispatch
    // (verified via per-layer trace in §J.3.e.2.e2e — Conv_16's
    // padding->forward(VkMat,...) virtual dispatch fails to reach
    // Padding_vulkan::forward and eventually hangs/crashes).  See
    // docs/J.3.e.2_ncnn_renderframe_integration.md §J.3.e.X.
    //
    // Path B sidesteps the external-VkDevice ABI rabbit hole entirely:
    //   • m_RifeNet runs on ncnn's INTERNAL vkdev (default
    //     ncnn::create_gpu_instance, same path NcnnFRUC uses successfully).
    //     PlVkRenderer drops the §J.3.e.1.d external handoff for RIFE.
    //   • Phase A copies bufRGB (DEVICE_LOCAL) → m_RifeRgbInHostBuf
    //     (HOST_VISIBLE) so we can wrap the CPU side as a non-owning
    //     ncnn::Mat for ex.input("in1", curr_mat).
    //   • ex.extract returns a CPU ncnn::Mat (out_mat); we memcpy its data
    //     into m_RifeRgbOutHostBuf (HOST_VISIBLE) and let Phase C copy that
    //     back to bufRGB before the RGB→VkImage compute.
    //   • Cost: one PCIe round-trip per frame (W*H*3*sizeof(float) each way).
    //     Budget at 720p ≈ 10.5 MB × 2 = 21 MB ≈ 1-2 ms on PCIe 4.0.
    bool     m_RifeReady    = false;
    bool     m_RifeDisabled = false;
    std::unique_ptr<ncnn::Net> m_RifeNet;
    ncnn::Mat m_RifePrevMat;       // W×H×3 fp32, persists across frames
    ncnn::Mat m_RifeTimestepMat;   // W×H×1 fp32 const 0.5 (matches RIFE 4.25-lite spec)
    bool      m_RifeHasPrevFrame = false;
    void*    m_RifeRgbInHostBuf      = nullptr;  // VkBuffer (W*H*3*4 bytes, HOST_VISIBLE)
    void*    m_RifeRgbInHostBufMem   = nullptr;  // VkDeviceMemory
    void*    m_RifeRgbInHostMapped   = nullptr;  // mapped pointer (lifetime = init→destroy)
    void*    m_RifeRgbOutHostBuf     = nullptr;  // VkBuffer (W*H*3*4 bytes, HOST_VISIBLE)
    void*    m_RifeRgbOutHostBufMem  = nullptr;  // VkDeviceMemory
    void*    m_RifeRgbOutHostMapped  = nullptr;  // mapped pointer
    // §J.3.e.2.e2 (Path B) — async pipeline-cache warm-up.  ncnn lazily
    // compiles SPIR-V on first ex.extract; for RIFE 4.25-lite that's ~4s
    // at 720p which (a) overflows moonlight's session-establish window if
    // done in initRifeModel synchronously, (b) freezes the render thread if
    // done on frame#1.  Solution: launch a detached warm-up thread at init
    // time; until it completes, Phase B treats every frame as "first frame
    // pass-through" (no RIFE forward, just rotate prev = curr).  Once the
    // flag flips true, real RIFE midpoint interpolation kicks in.
    std::atomic<bool> m_RifeWarmupComplete{false};
    std::thread       m_RifeWarmupThread;

    // §J.3.e.2.h — Generic FRUC port (Vulkan native).  Resources
    // allocated lazily on first override frame when VIPLE_VK_FRUC_GENERIC=1
    // is set.  All buffers DEVICE_LOCAL (no host staging round-trip);
    // dispatch chain runs on libplacebo's compute queue.
    bool     m_FrucGenericReady    = false;
    bool     m_FrucGenericDisabled = false;
    uint32_t m_FrucGenericMvWidth  = 0;  // ceil(W / 8)
    uint32_t m_FrucGenericMvHeight = 0;
    uint64_t m_FrucGenericFrameCount = 0;

    // 3 compute pipelines + their layouts.  Each shader has its own
    // descriptor set layout (different binding count) and push constant
    // range, so we can't share a single pipeline layout.
    void*    m_FrucMeShaderMod   = nullptr;  // VkShaderModule
    void*    m_FrucMeDsl         = nullptr;  // VkDescriptorSetLayout (4 bindings)
    void*    m_FrucMePipeLay     = nullptr;  // VkPipelineLayout
    void*    m_FrucMePipeline    = nullptr;  // VkPipeline
    void*    m_FrucMeDescSet     = nullptr;  // VkDescriptorSet (bound once at init)

    void*    m_FrucMedianShaderMod = nullptr;
    void*    m_FrucMedianDsl       = nullptr;
    void*    m_FrucMedianPipeLay   = nullptr;
    void*    m_FrucMedianPipeline  = nullptr;
    void*    m_FrucMedianDescSet   = nullptr;

    void*    m_FrucWarpShaderMod = nullptr;
    void*    m_FrucWarpDsl       = nullptr;
    void*    m_FrucWarpPipeLay   = nullptr;
    void*    m_FrucWarpPipeline  = nullptr;
    void*    m_FrucWarpDescSet   = nullptr;

    // Storage buffers (DEVICE_LOCAL).  prevRGB/currRGB share format with
    // §J.3.e.2.c bufRGB (planar fp32 R/G/B, W*H*3*sizeof(float) bytes).
    void*    m_FrucPrevRgbBuf     = nullptr;  // VkBuffer
    void*    m_FrucPrevRgbBufMem  = nullptr;
    // currRGB aliases m_FrucNv12RgbBufRGB — the §J.3.e.2.c forward
    // shader's output buffer.  No separate alloc.

    void*    m_FrucMvBuf          = nullptr;  // VkBuffer (mvW*mvH*2*sizeof(int))
    void*    m_FrucMvBufMem       = nullptr;
    void*    m_FrucMvFilteredBuf  = nullptr;
    void*    m_FrucMvFilteredMem  = nullptr;
    void*    m_FrucPrevMvBuf      = nullptr;  // for next frame's temporal predictor
    void*    m_FrucPrevMvMem      = nullptr;
    void*    m_FrucInterpRgbBuf   = nullptr;  // VkBuffer (W*H*3*sizeof(float)) — warp output
    void*    m_FrucInterpRgbMem   = nullptr;

    // Descriptor pool shared across the 3 sets.
    void*    m_FrucDescPool   = nullptr;  // VkDescriptorPool

    // Dedicated cmd buf + fence for the compute chain.  Reused per frame.
    void*    m_FrucCmdPool    = nullptr;  // VkCommandPool
    void*    m_FrucCmdBuf     = nullptr;  // VkCommandBuffer
    void*    m_FrucFence      = nullptr;  // VkFence

    bool initFrucGenericResources(uint32_t width, uint32_t height);
    void destroyFrucGenericResources();
    // Records + submits the 3-stage compute dispatch (ME → median →
    // warp), then dispatches §J.3.e.2.d's reverse converter writing
    // m_FrucInterpRgbBuf → m_FrucRgbImgImage so libplacebo can wrap
    // the same VkImage that §J.3.e.2.e1 already exposes.  Caller must
    // ensure m_FrucNv12RgbBufRGB has been written this frame
    // (§J.3.e.2.c forward).
    bool runFrucGenericComputePass(uint32_t width, uint32_t height);
};
