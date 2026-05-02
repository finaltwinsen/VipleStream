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

#include <atomic>
#include <map>
#include <vector>

// §J.3.e.2.i.8 Phase 1.1d — forward decl from nvvideoparser; full def lives in
// 3rdparty/nvvideoparser/include/vkvideo_parser/StdVideoPictureParametersSet.h
// and is only pulled in by vkfruc-decode.cpp (keeps vkfruc.cpp's #include set
// from ballooning).
class StdVideoPictureParametersSet;

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

    void notifyOverlayUpdated(Overlay::OverlayType type) override;

    // §J.3.e.2.i — FRUC status hooks 給 perf overlay 用 (ffmpeg.cpp:1075).
    // 沒 override 時 base class 全 return false，overlay 會顯示「未啟用」.
    // Pacer::renderFrame (pacer.cpp:348) 也用 lastFrameHadFRUCInterp 來
    // 累加 frucInterpolatedFrames，要正確才能算出補幀比例.
    bool isFRUCActive() const override;
    bool lastFrameHadFRUCInterp() const override;
    const char* getFRUCBackendName() const override;

    // §J.3.e.2.i.8 Phase 3a — VkFrucDecodeClient::DecodePicture needs to gate
    // on codec before reading the CodecSpecific union (hevc / av1 share storage).
    int  getVideoCodecMask() const { return m_VideoCodec; }

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

    // §J.3.e.2.i.7 HW path：actual extension list enabled at vkCreateDevice
    // (filtered wanted∩available).  populateAvHwDeviceCtx 把這個交給
    // FFmpeg's vkCtx->enabled_dev_extensions —— 要跟實際 enable 完全一致,
    // 否則 hwcontext_vulkan PFN dispatch table mis-init → NV driver NULL deref.
    std::vector<const char*> m_EnabledDevExts;

    // §J.3.e.2.i.8 — native VK_KHR_video_decode (skip FFmpeg).
    //
    // Phase 1.0 scaffold：createVideoSession() 建 VkVideoSessionKHR +
    // memory bindings（per-codec H.265 / H.264 / AV1）.  確認 API 在 NV
    // driver 走得通後，再進 Phase 1.1+ 接 NAL parsing + per-frame decode.
    //
    // 流程:
    //   1. vkCreateVideoSessionKHR (codec profile + maxCodedExtent + format)
    //   2. vkGetVideoSessionMemoryRequirementsKHR (多個 binding)
    //   3. vkAllocateMemory + vkBindVideoSessionMemoryKHR
    //   4. vkCreateVideoSessionParametersKHR (VPS/SPS/PPS, Phase 1.1 填)
    bool createVideoSession(int videoFormat);
    bool createVideoSessionParameters(int videoFormat);  // Phase 1.1: empty
    void destroyVideoSession();
    VkVideoSessionKHR           m_VideoSession       = VK_NULL_HANDLE;
    VkVideoSessionParametersKHR m_VideoSessionParams = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> m_VideoSessionMem;  // per binding
    int                         m_VideoCodec         = 0;  // VIDEO_FORMAT_MASK_H264/H265/AV1

    // §J.3.e.2.i.8 Phase 1.1d — H.265 VPS/SPS/PPS upload to VkVideoSessionParametersKHR.
    //
    // Parser hands us the active StdVideoPictureParametersSet* for the current
    // picture inside the DecodePicture callback (CodecSpecific.hevc.pStdVps/Sps/Pps).
    // We track the last-seen GetUpdateSequenceCount() per ID and only call
    // vkUpdateVideoSessionParametersKHR when we see a new ID, or a strictly
    // higher seq count for an existing ID (Sunshine resends on every IDR but
    // typically without changes, so this avoids spamming the driver).
    //
    // Defined in vkfruc-decode.cpp where StdVideoPictureParametersSet's full
    // header is already pulled in.
public:
    bool onH265PictureParametersFromParser(const StdVideoPictureParametersSet* vps,
                                           const StdVideoPictureParametersSet* sps,
                                           const StdVideoPictureParametersSet* pps);

    // §J.3.e.2.i.8 Phase 2 — H.264 SPS/PPS upload to VkVideoSessionParametersKHR.
    // Same incremental add-info pattern as H.265 but two param-set types instead
    // of three (no VPS).  Caches per-id GetUpdateSequenceCount() so repeat
    // callbacks are no-ops.
    bool onH264PictureParametersFromParser(const StdVideoPictureParametersSet* sps,
                                           const StdVideoPictureParametersSet* pps);

    // §J.3.e.2.i.8 Phase 3b.1 — AV1 sequence-header upload to VkVideoSessionParametersKHR.
    //
    // Vulkan AV1 session parameters are immutable (no VkVideoDecodeAV1SessionParametersAddInfoKHR
    // exists, unlike H.265/H.264).  When we see a NEW sequence header (parser bumps
    // GetUpdateSequenceCount), the strategy is destroy + recreate the
    // m_VideoSessionParams object with the real StdVideoAV1SequenceHeader, replacing
    // the zero-init placeholder that createVideoSessionParameters initially builds.
    // Cache the seq count to no-op repeated calls with the same header (parser
    // re-delivers the same pStdSps every picture).
    bool onAv1SequenceHeaderFromParser(const StdVideoPictureParametersSet* sps);

    // §J.3.e.2.i.8 Phase 1.3a — GPU bitstream buffer factory for the parser.
    // Allocates a host-visible+coherent VkBuffer with VIDEO_DECODE_SRC_BIT_KHR
    // usage and the codec profile chained in pNext.  Returned handles are
    // owned by the caller — must call destroyGpuBitstreamBuffer to free.
    // outMappedPtr points to the persistent mapping (no need to vkMap on
    // every Resize/CopyDataFrom).
    bool createGpuBitstreamBuffer(VkDeviceSize size,
                                  VkBuffer& outBuffer,
                                  VkDeviceMemory& outMemory,
                                  void*& outMappedPtr,
                                  VkDeviceSize& outActualSize);
    void destroyGpuBitstreamBuffer(VkBuffer buffer, VkDeviceMemory memory);

    // §J.3.e.2.i.8 Phase 1.3b — DPB image pool, lazy-allocated when the parser
    // BeginSequence callback delivers stream dimensions.  17 NV12 images
    // (DPB+DST+SAMPLED), profile chained for the active codec.  ensureDpbImagePool
    // is idempotent; first call provisions, subsequent calls no-op (unless we
    // teardown via destroyVideoSession which clears m_DpbReady).
    bool ensureDpbImagePool(int width, int height);
    void destroyDpbImagePool();
private:
    PFN_vkUpdateVideoSessionParametersKHR m_pfnUpdateVideoSessionParams = nullptr;
    // Vulkan spec requires updateSequenceCount to be exactly the current value
    // of the session params object's update count + 1 on each call — we track
    // it ourselves and bump after every successful call.
    uint32_t                m_H265SessionParamsSeq = 0;
    std::map<int, uint32_t> m_H265VpsSeqSeen;
    std::map<int, uint32_t> m_H265SpsSeqSeen;
    std::map<int, uint32_t> m_H265PpsSeqSeen;
    // §J.3.e.2.i.8 Phase 2 — H.264 SPS/PPS tracking, parallel structure.
    uint32_t                m_H264SessionParamsSeq = 0;
    std::map<int, uint32_t> m_H264SpsSeqSeen;
    std::map<int, uint32_t> m_H264PpsSeqSeen;

    // §J.3.e.2.i.8 Phase 3b.1 — AV1 sequence header tracking.  AV1 has no SPS ID
    // (single sequence header per stream); cache by GetUpdateSequenceCount() so
    // repeat callbacks are no-ops.  m_AV1SeqHdrApplied flips true once the
    // zero-init placeholder has been replaced by the parser-supplied real header.
    PFN_vkDestroyVideoSessionParametersKHR m_pfnDestroyVideoSessionParams = nullptr;
    PFN_vkCreateVideoSessionParametersKHR  m_pfnCreateVideoSessionParams  = nullptr;
    uint32_t m_AV1SeqHdrSeqSeen = 0;
    bool     m_AV1SeqHdrApplied = false;
    // §J.3.e.2.i.8 Phase 3d.4d — content-aware dedup snapshot.  Parser bumps
    // GetUpdateSequenceCount() on every Sunshine OBU_SEQUENCE_HEADER resend
    // (≈once per second) even when the std header content is identical, so
    // count-based dedup thrashes session params.  Compare full struct bytes.
    StdVideoAV1SequenceHeader m_AV1SeqHdrCached = {};

    // §J.3.e.2.i.8 Phase 1.3 — codec profile structs kept alive at member scope
    // so VkVideoProfileListInfoKHR (used by bitstream buffer + DPB image creation)
    // can chain into them.  Populated by createVideoSession() based on m_VideoCodec.
    // Only one of m_H264/H265/AV1 is actually used; the chosen one is wired into
    // m_VideoProfile.pNext.
    VkVideoProfileInfoKHR           m_VideoProfile      = {};
    VkVideoDecodeH264ProfileInfoKHR m_H264ProfileInfo   = {};
    VkVideoDecodeH265ProfileInfoKHR m_H265ProfileInfo   = {};
    VkVideoDecodeAV1ProfileInfoKHR  m_AV1ProfileInfo    = {};
    bool                            m_VideoProfileReady = false;

    // §J.3.e.2.i.8 Phase 1.3b — DPB image pool state.  m_DpbReady gates per-frame
    // decode submission until the pool is provisioned at the actual stream
    // resolution (rounded up to a 64-pixel multiple for HEVC CTU alignment).
    //
    // §J.3.e.2.i.8 Phase 1.3d.2.b — refactored from N independent VkImage to
    // ONE VkImage with arrayLayers=kPicPoolSize: NV HEVC profile doesn't
    // expose VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR, so all
    // DPB slots must alias the same underlying VkImage.  Each slot still has
    // a unique VkImageView with its own baseArrayLayer.
    bool           m_DpbReady       = false;
    int            m_DpbAlignedW    = 0;
    int            m_DpbAlignedH    = 0;
    VkImage        m_DpbSharedImage = VK_NULL_HANDLE;
    VkDeviceMemory m_DpbSharedMem   = VK_NULL_HANDLE;
    // §J.3.e.2.i.8 Phase 1.4 — newest decoded slot index, set by submitDecodeFrame
    // and consumed by renderFrameSw (atomic for cross-thread).  -1 = no native
    // decode result yet, fall back to ffmpeg staging upload.
    std::atomic<int> m_NewestDecodedSlot{-1};
    // §J.3.e.2.i.8 Phase 1.4 proper — fence of the most recent graphics-queue
    // submit that sampled m_SwUploadImage.  Decode submit waits on this before
    // recording its own m_SwUploadImage write so the sample (graphics) finishes
    // before the next overwrite (decode).  Atomic because set on render thread,
    // read on parser/decode thread.
    //
    // §J.3.e.2.i.8 Phase 1.5c — DEPRECATED.  Cross-thread fence access raced
    // with render-thread fence reset (validation v1.3.284:
    // VUID-vkResetFences-pFences-01123 + VUID-vkBeginCommandBuffer-cmdBuf-00049
    // chains into VK_ERROR_DEVICE_LOST after ~12 seconds in ONLY mode).  Replaced
    // by m_GfxTimelineSem below (timeline sems support concurrent multi-thread
    // wait without reset).  Field kept VK_NULL_HANDLE for now until migration
    // confirmed clean.
    std::atomic<VkFence> m_LastGraphicsFence{VK_NULL_HANDLE};

    // §J.3.e.2.i.8 Phase 1.5c — graphics→decode timeline semaphore.
    // Replaces the racey m_LastGraphicsFence pattern: graphics submit signals
    // m_GfxTimelineSem at monotonic value N, decode submit blocks via host
    // vkWaitSemaphores(N) before recording its m_SwUploadImage write.  Timeline
    // sems are designed for concurrent multi-thread waits without state races
    // (unlike VkFence which can't be safely reset while another thread is in
    // WaitForFences).  Mirrors m_TimelineSem (decode→graphics) but in the
    // opposite direction.
    VkSemaphore           m_GfxTimelineSem    = VK_NULL_HANDLE;
    std::atomic<uint64_t> m_GfxTimelineNext   {1};   // next value to allocate
    std::atomic<uint64_t> m_LastGraphicsValue {0};   // last value signaled by graphics
    // §J.3.e.2.i.8 Phase 1.3d.2.d — single 2D_ARRAY view for video-decode
    // (NV vk_video_samples pattern).  vkCmdDecodeVideoKHR uses this view
    // with VkVideoPictureResourceInfoKHR.baseArrayLayer = slot index.
    // Per-slot 2D views (in m_PicPool[].view) remain for Phase 1.4 graphics
    // sampling (each will get its own VkSamplerYcbcrConversion).
    VkImageView    m_DpbDecodeArrayView = VK_NULL_HANDLE;
    // Per-slot active state — flipped to true after the slot has been used as
    // pSetupReferenceSlot in a successful vkCmdDecodeVideoKHR submission.
    // Drives setupSlot.slotIndex in BeginCoding (must be -1 until activated).
    bool           m_DpbSlotActive[17] = {};

    // §J.3.e.2.i.8 Phase 1.3c — decode queue + cmd pool + per-frame sync.
    //
    // Single-frame serial pattern for first cut: record → submit → wait fence →
    // reset cmd buf → repeat.  m_DecodeDoneSem signals graphics queue when
    // decode finishes (Phase 1.4 will wait on this in the render-pass submit).
    // Phase 1.3+ may upgrade to N-deep ring for pipelining.
    bool createDecodeCommandResources();
    void destroyDecodeCommandResources();
    VkQueue         m_DecodeQueue       = VK_NULL_HANDLE;
    VkCommandPool   m_DecodeCmdPool     = VK_NULL_HANDLE;
    VkCommandBuffer m_DecodeCmdBuf      = VK_NULL_HANDLE;
    VkFence         m_DecodeFence       = VK_NULL_HANDLE;
    VkSemaphore     m_DecodeDoneSem     = VK_NULL_HANDLE;
    // §J.3.e.2.i.8 Phase 1.5b — cross-queue timeline semaphore.
    // Decode submit signals monotonic value N; graphics submit waits for N
    // before sampling m_SwUploadImage.  Replaces the racey CPU-side
    // WaitForFences(m_DecodeFence) attempt that hit VUID-vkResetFences-pFences-01123
    // (renderFrameSw and submitDecodeFrame both touched the same fence).
    VkSemaphore           m_TimelineSem      = VK_NULL_HANDLE;
    std::atomic<uint64_t> m_TimelineNext     {1};   // next value to allocate
    std::atomic<uint64_t> m_LastDecodeValue  {0};   // last value signaled by decode
    bool            m_DecodeCmdReady    = false;
    // True until the very first vkCmdControlVideoCodingKHR with RESET fires
    // (driver requires session reset before first decode submit).
    bool            m_DecodeNeedsReset  = true;
    uint64_t        m_DecodeSubmitCount = 0;
    uint64_t        m_DecodeSkipCount   = 0;

    // §J.3.e.2.i.8 Phase 1.3d — record + submit one decode frame.  Called from
    // VkFrucDecodeClient::DecodePicture callback.  Returns true if submitted,
    // false if skipped (P/B references not yet supported, or pre-conditions
    // unmet).  forward decl avoids dragging nvvideoparser headers into vkfruc.h.
    //
    // Phase 3b.2a — submitDecodeFrame is a thin codec dispatcher; the actual
    // recording lives in submitDecodeFrameH265 / submitDecodeFrameAv1.  Both
    // helpers assume the universal pre-checks (DPB ready, video session live,
    // PFNs loaded) already ran in the dispatcher.
    struct VkParserPictureData_;  // alias to avoid pulling header
public:
    bool submitDecodeFrame(struct VkParserPictureData* ppd);
private:
    bool submitDecodeFrameH265(struct VkParserPictureData* ppd);
    bool submitDecodeFrameH264(struct VkParserPictureData* ppd);
    bool submitDecodeFrameAv1 (struct VkParserPictureData* ppd);

    // Decode-time PFN cache (looked up lazily on first submit).
    // Uses sync2 barrier — VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR is only
    // defined on the sync2 path; legacy VkAccessFlagBits has no VIDEO_DECODE_*.
    struct {
        PFN_vkBeginCommandBuffer       BeginCommandBuffer;
        PFN_vkEndCommandBuffer         EndCommandBuffer;
        PFN_vkResetCommandBuffer       ResetCommandBuffer;
        PFN_vkCmdPipelineBarrier2      CmdPipelineBarrier2;
        PFN_vkCmdBeginVideoCodingKHR   CmdBeginVideoCodingKHR;
        PFN_vkCmdEndVideoCodingKHR     CmdEndVideoCodingKHR;
        PFN_vkCmdControlVideoCodingKHR CmdControlVideoCodingKHR;
        PFN_vkCmdDecodeVideoKHR        CmdDecodeVideoKHR;
        PFN_vkQueueSubmit              QueueSubmit;
        PFN_vkWaitForFences            WaitForFences;
        PFN_vkResetFences              ResetFences;
        // §J.3.e.2.i.8 Phase 1.5c — host-side timeline-sem wait for gfx→decode sync.
        PFN_vkWaitSemaphores           WaitSemaphores;
    } m_DecodeRtPfn = {};
    bool m_DecodeRtPfnReady = false;
    bool loadDecodeRtPfns();

    // Phase 1.2 — native NAL intercept hook (renderer.h interface).
    bool acceptsNativeDecode() const override;
    bool isNativelyDecodingCurrentCodec() const override;
    void submitNativeDecodeUnit(const uint8_t* data, size_t len) override;

    // Phase 1.1c — NvVideoParser instance + callback handler.
    // Pimpl 完整型別在 vkfruc-decode.cpp，這裡只 forward decl + 指標.
    struct NvParserPimpl;
    NvParserPimpl* m_NvParser = nullptr;
    bool createNvVideoParser();
    void destroyNvVideoParser();
    void feedNalToNvParser(const uint8_t* data, size_t len);
    // Phase 1.1b scaffold — NAL type detection (parser comes next session).
    // Counts per NAL type accumulated; logged periodically for diagnostics.
    struct NalCounts {
        uint64_t vps = 0;
        uint64_t sps = 0;
        uint64_t pps = 0;
        uint64_t idr_slice = 0;
        uint64_t trailing_slice = 0;
        uint64_t aud = 0;     // access unit delimiter (NAL=35)
        uint64_t prefix_sei = 0;  // (NAL=39)
        uint64_t other = 0;
        uint64_t total_packets = 0;
        uint64_t total_bytes = 0;
    } m_NalCounts;
    uint64_t m_LastNalLogUs = 0;

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
    // §J.3.e.2.i.8 Phase 1.5c — bumping to 4 made things worse (10s vs 40s
    // crash) because more in-flight slots = more sem reuse opportunities.
    // Reverted back to 2.  The proper fix for the renderDone-sem-reuse race
    // is per-swapchain-image sems, deferred.
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
        // §J.3.e.2.i.8 Phase 2.5 — image→buffer copy for FRUC native NV12 mirror.
        PFN_vkCmdCopyImageToBuffer   CmdCopyImageToBuffer;
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
    // §J.3.e.2.i.8 Phase 2.5 — per-slot gpu-only NV12 buffer that mirrors
    // m_SwUploadImage for FRUC compute consumption when native VK decode is
    // active.  We avoid teaching the FRUC NV12→RGB compute shader to read
    // sampled images by copying the decoded NV12 bytes via vkCmdCopyImageToBuffer
    // at the start of renderFrameSw's cmd buffer; FRUC binding 0 then points
    // to this buffer (via m_FrucNv12RgbDescSetNative[slot]) instead of
    // m_SwStagingBuffer.  Removes the source asymmetry that caused 3-4 Hz
    // blur/sharp flicker in v1.3.275.
    //
    // PER-SLOT (kFrucFramesInFlight = 2): each frame slot owns its own buffer
    // and matching descriptor set so consecutive frames don't WAW-race on a
    // shared buffer across submissions (no implicit memory barrier between
    // separate vkQueueSubmit calls on the graphics queue).  The slot fence
    // (m_SlotInFlightFence[slot]) waited at the start of renderFrameSw
    // ensures the previous use of slot N has completed before slot N reuses
    // m_SwFrucNv12Buf[N].
    //
    // v1.3.277 REVERTED to single buffer — per-slot caused reliable
    // VK_ERROR_DEVICE_LOST after ~1380 frames on NV 596.84 + RTX 3060.
    // v1.3.276 single-buffer ran 2940+ frames stable (with theoretical WAW
    // race manifesting as "occasional blur" but no crash).  Keeping
    // single-buffer trade-off until we can root-cause the per-slot crash
    // (likely descriptor set / buffer interaction with NV driver).
    VkBuffer         m_SwFrucNv12Buf      = VK_NULL_HANDLE;
    VkDeviceMemory   m_SwFrucNv12BufMem   = VK_NULL_HANDLE;

    // §J.3.e.2.i.4.2 / §J.3.e.2.i.5 — interp graphics pipeline + dual-present.
    // Second graphics pipeline displays m_FrucInterpRgbBuf via fragment
    // shader that reads planar fp32 RGB storage buffer.  Used as the
    // FIRST of two render passes per frame when VIPLE_VKFRUC_DUAL=1.
    //
    // Per renderFrameSw call (when dual mode):
    //   1. acquire imgA + imgB
    //   2. begin cmd: NV12 upload + compute chain → bufInterpRGB
    //   3. render pass on m_Framebuffers[imgA] using m_InterpPipeline +
    //      m_InterpDescSet (samples bufInterpRGB)
    //   4. render pass on m_Framebuffers[imgB] using m_GraphicsPipeline
    //      (existing, samples m_SwUploadImage NV12 via ycbcr sampler)
    //   5. end cmd, submit (waits both acquireSems, signals both renderDones)
    //   6. present imgA, present imgB
    bool             m_DualMode             = false;
    VkShaderModule   m_InterpFragShaderMod  = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_InterpDescSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_InterpPipelineLayout = VK_NULL_HANDLE;
    VkPipeline       m_InterpPipeline       = VK_NULL_HANDLE;
    VkDescriptorPool m_InterpDescPool       = VK_NULL_HANDLE;
    VkDescriptorSet  m_InterpDescSet        = VK_NULL_HANDLE;
    bool createInterpGraphicsPipeline();
    void destroyInterpGraphicsPipeline();

    // §J.3.e.2.i.4 — FRUC compute pipeline (port from PlVkRenderer
    // §J.3.e.2.h.c).  Gated on VIPLE_VKFRUC_FRUC=1.  Allocates prev/curr/
    // interp RGB buffers + MV buffers, builds 3 compute pipelines (ME,
    // Median, Warp), dispatches the chain per-frame after our SW upload.
    // i.4.1 added NV12→RGB feed (real bufRGB instead of zeros).
    // i.4.2 added interp display via m_InterpPipeline (above).
    bool createFrucComputeResources(int width, int height);
    void destroyFrucComputeResources();
    bool runFrucComputeChain(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                             bool useNativeSrc);

    bool     m_FrucMode      = false;
    bool     m_FrucReady     = false;
    bool     m_FrucDisabled  = false;
    bool     m_NcnnInited    = false;  // §J.3.e.2.i.6: tracks ncnn::create_gpu_instance call
    uint32_t m_FrucMvWidth   = 0;   // ceil(W / 8)
    uint32_t m_FrucMvHeight  = 0;
    uint64_t m_FrucFrameCount = 0;

    // §J.3.e.2.i.4.1 NV12 → planar fp32 RGB compute.  Reads raw NV12
    // bytes from m_SwStagingBuffer (Y at offset 0, UV at offset W*H),
    // writes to m_FrucCurrRgbBuf in [R-plane, G-plane, B-plane] layout
    // matching ME shader's expectation.
    VkShaderModule        m_FrucNv12RgbShaderMod = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_FrucNv12RgbDsl       = VK_NULL_HANDLE;
    VkPipelineLayout      m_FrucNv12RgbPipeLay   = VK_NULL_HANDLE;
    VkPipeline            m_FrucNv12RgbPipeline  = VK_NULL_HANDLE;
    VkDescriptorSet       m_FrucNv12RgbDescSet   = VK_NULL_HANDLE;
    // §J.3.e.2.i.8 Phase 2.5 — alternate descriptor set whose binding 0 points
    // at m_SwFrucNv12Buf (native-decoded NV12) instead of m_SwStagingBuffer
    // (FFmpeg memcpy NV12).  Bound by runFrucComputeChain when useNative=true.
    // (Reverted from per-slot in v1.3.278 — see m_SwFrucNv12Buf comment.)
    VkDescriptorSet       m_FrucNv12RgbDescSetNative = VK_NULL_HANDLE;

    // 3 compute pipelines (ME / Median / Warp).  Each has own DSL because
    // binding counts differ (ME=4, Median=2, Warp=4).
    VkShaderModule        m_FrucMeShaderMod  = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_FrucMeDsl        = VK_NULL_HANDLE;
    VkPipelineLayout      m_FrucMePipeLay    = VK_NULL_HANDLE;
    VkPipeline            m_FrucMePipeline   = VK_NULL_HANDLE;
    VkDescriptorSet       m_FrucMeDescSet    = VK_NULL_HANDLE;

    VkShaderModule        m_FrucMedianShaderMod = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_FrucMedianDsl       = VK_NULL_HANDLE;
    VkPipelineLayout      m_FrucMedianPipeLay   = VK_NULL_HANDLE;
    VkPipeline            m_FrucMedianPipeline  = VK_NULL_HANDLE;
    VkDescriptorSet       m_FrucMedianDescSet   = VK_NULL_HANDLE;

    VkShaderModule        m_FrucWarpShaderMod = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_FrucWarpDsl       = VK_NULL_HANDLE;
    VkPipelineLayout      m_FrucWarpPipeLay   = VK_NULL_HANDLE;
    VkPipeline            m_FrucWarpPipeline  = VK_NULL_HANDLE;
    VkDescriptorSet       m_FrucWarpDescSet   = VK_NULL_HANDLE;

    // Storage buffers (DEVICE_LOCAL).  All planar fp32 R/G/B.
    VkBuffer       m_FrucPrevRgbBuf    = VK_NULL_HANDLE;
    VkDeviceMemory m_FrucPrevRgbBufMem = VK_NULL_HANDLE;
    VkBuffer       m_FrucCurrRgbBuf    = VK_NULL_HANDLE;
    VkDeviceMemory m_FrucCurrRgbBufMem = VK_NULL_HANDLE;
    VkBuffer       m_FrucMvBuf         = VK_NULL_HANDLE;  // mvW*mvH*2 int
    VkDeviceMemory m_FrucMvBufMem      = VK_NULL_HANDLE;
    VkBuffer       m_FrucMvFilteredBuf = VK_NULL_HANDLE;
    VkDeviceMemory m_FrucMvFilteredMem = VK_NULL_HANDLE;
    VkBuffer       m_FrucPrevMvBuf     = VK_NULL_HANDLE;
    VkDeviceMemory m_FrucPrevMvMem     = VK_NULL_HANDLE;
    VkBuffer       m_FrucInterpRgbBuf  = VK_NULL_HANDLE;
    VkDeviceMemory m_FrucInterpRgbMem  = VK_NULL_HANDLE;

    VkDescriptorPool m_FrucDescPool = VK_NULL_HANDLE;

    // §J.3.e.2.i — overlay rendering (Ctrl+Alt+Shift+S 效能資訊).
    // moonlight-qt OverlayManager 給 SDL_Surface (RGBA8)，我們：
    //   1. notifyOverlayUpdated 拿 surface，alloc 配對 VkImage + staging
    //   2. memcpy surface pixels → staging buffer（host-visible coherent）
    //   3. mark m_OverlayUploadPending；renderFrameSw 拿到 cmdbuf 後
    //      cmdCopyBufferToImage staging → image，barrier to SHADER_READ
    //   4. 在 main video render pass 之後 bind overlay pipeline + descSet，
    //      draw 3 verts (fullscreen tri)，frag shader discard 在 rect 外的
    //      pixel + alpha blend 在 rect 內
    // Overlay rect 固定 top-left，按 surface 比例放置.
    bool createOverlayResources();
    void destroyOverlayResources();
    void drainOverlayStash();  // render-thread: process notifyOverlayUpdated stash, alloc/copy
    void uploadPendingOverlay(VkCommandBuffer cmd);  // call before render pass
    void drawOverlayInRenderPass(VkCommandBuffer cmd);  // call inside render pass
    static constexpr uint32_t kOverlayMax = 2;  // matches Overlay::OverlayMax
    int             m_OverlayWidth [kOverlayMax]    = {};
    int             m_OverlayHeight[kOverlayMax]    = {};
    int             m_OverlayPitch [kOverlayMax]    = {};
    bool            m_OverlayPending[kOverlayMax]   = {};  // staging has new data
    bool            m_OverlayHasContent[kOverlayMax]= {};  // image has valid content
    bool            m_OverlayLayoutInited[kOverlayMax] = {};
    VkImage         m_OverlayImage [kOverlayMax]    = {};
    VkDeviceMemory  m_OverlayMem   [kOverlayMax]    = {};
    VkImageView     m_OverlayView  [kOverlayMax]    = {};
    VkBuffer        m_OverlayStagingBuf[kOverlayMax]= {};
    VkDeviceMemory  m_OverlayStagingMem[kOverlayMax]= {};
    void*           m_OverlayStagingMapped[kOverlayMax] = {};
    size_t          m_OverlayStagingSize[kOverlayMax]   = {};
    VkDescriptorSet m_OverlayDescSet[kOverlayMax]   = {};
    VkSampler       m_OverlaySampler                = VK_NULL_HANDLE;
    VkShaderModule  m_OverlayFragShaderMod          = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_OverlayDescSetLayout    = VK_NULL_HANDLE;
    VkPipelineLayout      m_OverlayPipelineLayout   = VK_NULL_HANDLE;
    VkPipeline            m_OverlayPipeline         = VK_NULL_HANDLE;
    VkDescriptorPool      m_OverlayDescPool         = VK_NULL_HANDLE;

    // Cross-thread stash: notifyOverlayUpdated runs on任意 thread (見
    // overlaymanager.cpp:290 註解，跟 D3D11VARenderer 一樣），render thread
    // 在 renderFrameSw → drainOverlayStash() 拉資料。改用 SDL_AtomicLock
    // (spinlock) 取代 SDL_mutex —— D3D11VA 也是這個 pattern：render 拿不到
    // lock 就跳這幀，比 mutex block 安全（teardown 也不必先拆 mutex）。
    int              m_OverlayLock                  = 0;
    SDL_Surface*     m_OverlayStashedSurface[kOverlayMax] = {};
    bool             m_OverlayStashedDisable[kOverlayMax] = {};

    // §J.3.e.2.i.6 GPU timestamp queries — measures compute chain total
    // GPU time (NV12->RGB + ME + Median + Warp).  2 timestamps per ring
    // slot: chain_start (TOP_OF_PIPE before NV12RGB dispatch), chain_end
    // (BOTTOM_OF_PIPE after Warp barrier).  Read at start of next frame
    // for the SAME slot (after fence wait → GPU has finished prior pass).
    VkQueryPool m_FrucTimerPool   = VK_NULL_HANDLE;
    uint32_t    m_FrucTimerSlot   = 0;     // ring slot for queries
    bool        m_FrucTimerArmed[kFrucFramesInFlight] = {};  // whether slot's timestamps were written
    double      m_FrucTimerNsPerTick = 0.0;  // from VkPhysicalDeviceLimits.timestampPeriod
    // Aggregated GPU time (us) — accumulated each frame, logged in stats window
    double      m_FrucGpuUsAccum  = 0.0;
    int         m_FrucGpuUsCount  = 0;

    // Loaded PFNs (we don't have the libplacebo wrapper's lookup; use
    // SDL_Vulkan_GetVkGetInstanceProcAddr + raw vk*ProcAddr chain).
    PFN_vkGetInstanceProcAddr m_pfnGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance     m_pfnDestroyInstance     = nullptr;
    PFN_vkDestroyDevice       m_pfnDestroyDevice       = nullptr;
    PFN_vkDestroySurfaceKHR   m_pfnDestroySurfaceKHR   = nullptr;

    // §J.3.e.2.i.8 Phase 1.3d.2 debug — VK_EXT_debug_utils messenger to route
    // Vulkan validation layer messages through SDL_Log so they land in our
    // log file alongside renderer events.  Created when the extension is
    // exposed (typical when the loader has the validation layer present).
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
    static VkBool32 VKAPI_PTR debugMessengerCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);
};

#endif // HAVE_LIBPLACEBO_VULKAN
