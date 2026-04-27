// vk_backend.c — VipleStream §I.B.2a + B.2b: real Vulkan resource lifecycle
// for the FRUC backend.
//
// B.2a (v1.2.139): instance + physical device + logical device + graphics queue.
// B.2b (this rev): instance enables VK_KHR_surface + VK_KHR_android_surface,
//                  Java Surface → ANativeWindow, VkSurfaceKHR creation,
//                  format/presentMode probing, VkSwapchainKHR creation,
//                  swapchain image enumeration, all torn down before
//                  initialize() returns null so GLES still owns the
//                  displaySurface for the actual streaming session.
//
// Why dlopen instead of -lvulkan: app's minSdk is 21 while libvulkan only
// landed at API 24. dlopen lets the rest of the app keep loading on older
// devices and degrade cleanly. Real headers from <vulkan/vulkan.h> +
// <vulkan/vulkan_android.h> are pulled in for type definitions only — we
// never link any vk* symbol directly.

#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
// AHB JNI helpers are API 26+. minSdk is 21, so the headers reject the
// calls unless the build sets __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
// (in Android.mk LOCAL_CFLAGS). At runtime VkBackend gates this whole
// path on debug.viplestream.vkprobe + Build.VERSION.SDK_INT >= 28, so
// we can never reach a too-old device.
#include <android/hardware_buffer.h>
#include <android/hardware_buffer_jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Pre-compiled SPIR-V shaders. Source under shaders/, regenerate via:
//   $NDK/shader-tools/.../glslc <name> -o <name>.spv
//   xxd -i <name>.spv | sed 's|^unsigned |static const unsigned |' > <name>.spv.h
#include "shaders/fullscreen.vert.spv.h"
#include "shaders/test_pattern.frag.spv.h"
#include "shaders/video_sample.frag.spv.h"
// §I.C.3 compute pipelines.
#include "shaders/ycbcr_to_rgba.comp.spv.h"
#include "shaders/mv_median.comp.spv.h"
// §I.C.5.a quality variants: Q1 is the default; Q0=Quality, Q2=Performance.
#include "shaders/motionest_compute_q0.comp.spv.h"
#include "shaders/motionest_compute_q1.comp.spv.h"
#include "shaders/motionest_compute_q2.comp.spv.h"
#include "shaders/warp_compute_q0.comp.spv.h"
#include "shaders/warp_compute_q1.comp.spv.h"
#include "shaders/warp_compute_q2.comp.spv.h"

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

#define TAG "VKBE-NAT"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---------- §I.C.3 FRUC compute pipeline indices ----------

#define FRUC_IMG_CURR     0  // currFrameRgba (rgba8, video size)
#define FRUC_IMG_PREV     1  // prevFrameRgba (rgba8, video size)
#define FRUC_IMG_MV       2  // motionField (r32i, mv size)
#define FRUC_IMG_PREV_MV  3  // prevMotionField
#define FRUC_IMG_FILT_MV  4  // filteredMotionField
#define FRUC_IMG_INTERP   5  // interpFrame (rgba8, not sampled in §I.C.3)
#define FRUC_NUM_IMAGES   6

#define FRUC_PIPE_YCBCR     0  // ycbcr_to_rgba.comp
#define FRUC_PIPE_ME        1  // motionest_compute_q1.comp (§I.C.3 hardcodes Q1)
#define FRUC_PIPE_MV_MEDIAN 2  // mv_median.comp
#define FRUC_PIPE_WARP      3  // warp_compute_q1.comp
#define FRUC_NUM_PIPELINES  4

// §I.D async compute — in-flight ring depth. 2 slots = double buffering.
// Per-slot resources let frame N+1 begin CPU-side work (record, AHB
// import, descriptor update) while frame N's GPU work is still running.
// Increase to 3 for triple-buffered if profiling shows CPU-bound.
#define VK_FRAMES_IN_FLIGHT 2

// ---------- backend handle ----------

typedef struct vk_backend_s {
    void* libvulkan;

    // Loader entry points
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr   vkGetDeviceProcAddr;

    // Instance + lifecycle
    VkInstance instance;
    PFN_vkDestroyInstance vkDestroyInstance;

    // Surface
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR;
    PFN_vkDestroySurfaceKHR       vkDestroySurfaceKHR;
    ANativeWindow* nativeWindow;
    VkSurfaceKHR   surface;

    // Physical / logical device
    VkPhysicalDevice physDevice;
    VkDevice         device;
    PFN_vkDestroyDevice vkDestroyDevice;

    // Queue
    VkQueue  graphicsQueue;
    uint32_t graphicsQueueFamily;

    // Swapchain
    PFN_vkCreateSwapchainKHR    vkCreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR   vkDestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
    VkSwapchainKHR swapchain;
    VkFormat       swapchainFormat;
    VkExtent2D     swapchainExtent;
    uint32_t       swapchainImageCount;
    VkImage*       swapchainImages;

    // AHB import path (B.2c.3b)
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID vkGetAndroidHardwareBufferPropertiesANDROID;
    PFN_vkCreateImage                vkCreateImage;
    PFN_vkDestroyImage               vkDestroyImage;
    PFN_vkAllocateMemory             vkAllocateMemory;
    PFN_vkFreeMemory                 vkFreeMemory;
    PFN_vkBindImageMemory            vkBindImageMemory;
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
    int ahbImportLogged;

    // YCbCr sampler path (B.2c.3c.1) — lazily created on first AHB frame
    // because it depends on the externalFormat we read off the buffer.
    PFN_vkCreateSamplerYcbcrConversion  vkCreateSamplerYcbcrConversion;
    PFN_vkDestroySamplerYcbcrConversion vkDestroySamplerYcbcrConversion;
    PFN_vkCreateSampler                 vkCreateSampler;
    PFN_vkDestroySampler                vkDestroySampler;
    VkSamplerYcbcrConversion ycbcrConversion;
    VkSampler                ycbcrSampler;
    uint64_t                 ycbcrExternalFormat;   // externalFormat the sampler is bound to
    int                      ycbcrInitialized;

    // Graphics pipeline (B.2c.3c.2) — full-screen triangle, no vertex
    // buffer, test_pattern.frag draws a UV gradient. B.2c.3c.3 swaps
    // the frag for one that samples the imported AHB image and uses
    // a non-empty descriptor set with the YCbCr sampler binding.
    PFN_vkCreateShaderModule        vkCreateShaderModule;
    PFN_vkDestroyShaderModule       vkDestroyShaderModule;
    PFN_vkCreateRenderPass          vkCreateRenderPass;
    PFN_vkDestroyRenderPass         vkDestroyRenderPass;
    PFN_vkCreateImageView           vkCreateImageView;
    PFN_vkDestroyImageView          vkDestroyImageView;
    PFN_vkCreateFramebuffer         vkCreateFramebuffer;
    PFN_vkDestroyFramebuffer        vkDestroyFramebuffer;
    PFN_vkCreatePipelineLayout      vkCreatePipelineLayout;
    PFN_vkDestroyPipelineLayout     vkDestroyPipelineLayout;
    PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
    PFN_vkCreateGraphicsPipelines   vkCreateGraphicsPipelines;
    PFN_vkDestroyPipeline           vkDestroyPipeline;
    PFN_vkCmdBeginRenderPass        vkCmdBeginRenderPass;
    PFN_vkCmdEndRenderPass          vkCmdEndRenderPass;
    PFN_vkCmdBindPipeline           vkCmdBindPipeline;
    PFN_vkCmdDraw                   vkCmdDraw;
    PFN_vkCmdPushConstants          vkCmdPushConstants;
    VkRenderPass         renderPass;
    VkPipelineLayout     pipelineLayout;
    VkDescriptorSetLayout descLayout;
    VkPipeline           graphicsPipeline;
    VkShaderModule       vertShader;
    VkShaderModule       fragShader;
    VkImageView*         swapchainViews;     // size = swapchainImageCount
    VkFramebuffer*       framebuffers;       // size = swapchainImageCount
    int                  graphicsInitialized;

    // Descriptor pool / set for the video sampler (B.2c.3c.3). The
    // descriptor set's image binding is updated per frame to point at
    // whatever VkImageView we just imported from MediaCodec's AHB.
    PFN_vkCreateDescriptorPool      vkCreateDescriptorPool;
    PFN_vkDestroyDescriptorPool     vkDestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets    vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets      vkUpdateDescriptorSets;
    PFN_vkCmdBindDescriptorSets     vkCmdBindDescriptorSets;
    VkDescriptorPool descPool;
    VkDescriptorSet  descSet;

    // Logical video dims (passed from Java; matches MediaCodec output res
    // and ImageReader's defaultBufferSize). The AHB allocated by the OEM
    // pads height up to a multiple of 16 (e.g. 1080 → 1088). uvScale push
    // constant in video_sample.frag uses videoH/ahbH to crop the padding.
    int videoWidth;
    int videoHeight;
    int ahbPaddedWidth;
    int ahbPaddedHeight;

    // Per-frame render path (B.2c.3a)
    PFN_vkAcquireNextImageKHR    vkAcquireNextImageKHR;
    PFN_vkQueuePresentKHR        vkQueuePresentKHR;
    PFN_vkQueueSubmit            vkQueueSubmit;
    PFN_vkQueueWaitIdle          vkQueueWaitIdle;
    PFN_vkBeginCommandBuffer     vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer       vkEndCommandBuffer;
    PFN_vkResetCommandBuffer     vkResetCommandBuffer;
    PFN_vkCmdClearColorImage     vkCmdClearColorImage;
    PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier;
    PFN_vkCreateSemaphore        vkCreateSemaphore;
    PFN_vkDestroySemaphore       vkDestroySemaphore;
    PFN_vkCreateCommandPool      vkCreateCommandPool;
    PFN_vkDestroyCommandPool     vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
    VkCommandPool   cmdPool;
    VkCommandBuffer cmdBuffer;
    VkSemaphore     acquireSem;
    VkSemaphore     renderDoneSem;
    int             renderInitialized;
    int             frameCounter;

    // §I.C.3 FRUC compute resources. Built lazily after the YCbCr sampler
    // is ready (parallel to the graphics pipeline). C.3.a sets them up;
    // C.3.b wires per-frame dispatch.
    PFN_vkCreateComputePipelines    vkCreateComputePipelines;
    PFN_vkCmdDispatch               vkCmdDispatch;
    PFN_vkCmdCopyImage              vkCmdCopyImage;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;

    VkImage        fImage[FRUC_NUM_IMAGES];
    VkDeviceMemory fImageMem[FRUC_NUM_IMAGES];
    VkImageView    fImageView[FRUC_NUM_IMAGES];

    VkShaderModule        fShader[FRUC_NUM_PIPELINES];
    VkDescriptorSetLayout fDescLayout[FRUC_NUM_PIPELINES];
    VkPipelineLayout      fPipeLayout[FRUC_NUM_PIPELINES];
    VkPipeline            fPipeline[FRUC_NUM_PIPELINES];
    VkDescriptorPool      fDescPool;
    VkDescriptorSet       fDescSet[FRUC_NUM_PIPELINES];
    VkSampler             fLinearSampler;  // CLAMP_TO_EDGE LINEAR for warp's sampleMV
    int                   fInitialized;

    // §I.C.4.a: second graphics pipeline that samples interpFrame (rgba8, no
    // ycbcr conversion) via fLinearSampler — produced by warp_q1, currently
    // not displayed. Reuses fullscreen.vert + video_sample.frag (same SPIR-V),
    // only the descriptor layout differs because of the sampler swap. Bound
    // in place of the existing graphicsPipeline once fInitialized.
    VkDescriptorSetLayout fInterpDescLayout;
    VkPipelineLayout      fInterpPipeLayout;
    VkPipeline            fInterpPipeline;
    VkDescriptorSet       fInterpDescSet;

    // §I.C.4.b dual present counter: # of synthesized in-between frames
    // (1 per render_ahb_frame invocation when fInitialized — pass 1 is
    // the interp, pass 2 is the real). Read back from Java via
    // nativeGetInterpolatedCount for the perf overlay.
    int                   fInterpolatedCount;

    // §I.C.5.a quality preset wiring: 3 ME shaders + 3 warp shaders +
    // 6 pipelines, all sharing the same desc set layout / pipeline layout
    // (per docs/vulkan_fruc_port.md §4 共用 layout). Q1 is the default;
    // fPipeline[FRUC_PIPE_ME] / fPipeline[FRUC_PIPE_WARP] above are the
    // Q1 modules — kept for back-compat / log clarity.
    // Java's setQualityLevel calls down via nativeSetQualityLevel.
    VkShaderModule        fMeShaderQ[3];     // [0]=Q0, [1]=Q1 (alias of fShader[ME]), [2]=Q2
    VkShaderModule        fWarpShaderQ[3];
    VkPipeline            fMePipelineQ[3];
    VkPipeline            fWarpPipelineQ[3];
    int                   fQualityLevel;     // 0/1/2, default 1 (Balanced)

    // §I.C.6 VK_GOOGLE_display_timing — interp frame PTS scheduling.
    // Probed at create_device; enabled iff present (Pixel 5 / Adreno 620
    // confirmed support in §I.A.A2 v1.2.134). PASS 1 / PASS 2 of dual
    // present each carry a VkPresentTimeGOOGLE to hint the driver where
    // to slot interp vs real on the display timeline.
    int                                       fDisplayTimingSupported;
    PFN_vkGetRefreshCycleDurationGOOGLE       vkGetRefreshCycleDurationGOOGLE;
    PFN_vkGetPastPresentationTimingGOOGLE     vkGetPastPresentationTimingGOOGLE;
    uint64_t                                  fRefreshDurationNs;
    uint32_t                                  fPresentId;          // monotonic, +1 per present

    // §I.D in-flight ring — double-buffered cmdbuf/sems/fence per slot
    // so frame N+1 CPU-side work (record + AHB import) can overlap
    // frame N's GPU work. Replaces the single per-frame cmdBuffer +
    // vkQueueWaitIdle dance that was throttling input from 60→45 FPS
    // (see §I.C.5.b baseline). Active only when fInitialized (dual
    // present path); fall-back single-present path still uses the old
    // cmdBuffer + waitIdle.
    PFN_vkCreateFence    vkCreateFence;
    PFN_vkDestroyFence   vkDestroyFence;
    PFN_vkWaitForFences  vkWaitForFences;
    PFN_vkResetFences    vkResetFences;
    VkCommandBuffer      fSlotCmdBuf[VK_FRAMES_IN_FLIGHT];
    VkSemaphore          fSlotAcquireSem[VK_FRAMES_IN_FLIGHT][2];     // [pass1, pass2]
    VkSemaphore          fSlotRenderDoneSem[VK_FRAMES_IN_FLIGHT][2];
    VkFence              fSlotInFlightFence[VK_FRAMES_IN_FLIGHT];
    // Pending AHB import per slot — freed at start of next slot reuse
    // (after fence signal proves GPU done with them).
    VkImage              fSlotPendingImg[VK_FRAMES_IN_FLIGHT];
    VkDeviceMemory       fSlotPendingMem[VK_FRAMES_IN_FLIGHT];
    VkImageView          fSlotPendingView[VK_FRAMES_IN_FLIGHT];
    int                  fSlotHasPending[VK_FRAMES_IN_FLIGHT];
    uint32_t             fCurrentSlot;
    int                  fRingInitialized;

    // §I.D.b smart-mode dual present — when input fps ≈ display refresh
    // (e.g. 60 FPS input on 60 Hz panel), dual present (interp + real)
    // throttles input from 60→45 because of FIFO vsync gating. Detect
    // this and degrade to single present (real only, skip interp) so
    // input rate matches display rate. When input < display (e.g. 30
    // in 60Hz, 45 in 90Hz), keep dual present so FRUC actually adds
    // frames. Decision is per-frame; measurement is a 1s sliding window.
    uint64_t             fInputWindowStartNs;
    int                  fInputWindowFrames;
    float                fInputFpsRecent;     // 0 = not yet measured
    int                  fLastSingleMode;     // 0 = dual, 1 = single (for log spam suppression)

    // §I.D.c — Pixel 5 / Adreno 620: vkGetRefreshCycleDurationGOOGLE
    // caches the swapchain-creation-time panel rate (60 Hz) and never
    // updates after the OS hot-switches the panel mode (e.g. via
    // ANativeWindow_setFrameRateWithChangeStrategy(90, ALWAYS)). When
    // the hint is accepted, override the driver-reported refresh with
    // the hinted rate for smart-mode decisions; falls back to driver
    // query when no hint was accepted.
    float                fHintedRefreshHz;    // 0 = no hint accepted, use driver query

    // §I.E.a HDR recon — capability flags. Set during init; pure
    // observability for now (recon phase, no behavioural change).
    // §I.E.b/c will gate actual HDR pipeline changes on these.
    int                  fHdrColorspaceExt;   // VK_EXT_swapchain_colorspace (instance)
    int                  fHdrMetadataExt;     // VK_EXT_hdr_metadata (device)
    int                  fHdrCapableSurface;  // surface advertises HDR10_ST2084_EXT colorspace
} vk_backend_t;

#define LOAD_INSTANCE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetInstanceProcAddr((be)->instance, #name))

// Forward declarations — definitions live further down so each phase reads
// top-to-bottom, but nativeInit calls them before they appear in source.
static int  init_graphics_pipeline(struct vk_backend_s* be);
static void destroy_graphics_pipeline(struct vk_backend_s* be);
static int  load_graphics_procs(struct vk_backend_s* be);
static int  init_compute_pipelines(struct vk_backend_s* be);
static void destroy_compute_pipelines(struct vk_backend_s* be);
static int  dispatch_fruc(struct vk_backend_s* be, VkImageView ahbView);
static int  init_in_flight_ring(struct vk_backend_s* be);
static void destroy_in_flight_ring(struct vk_backend_s* be);

// ---------- helpers ----------

static int create_instance(vk_backend_t* be)
{
    PFN_vkCreateInstance vkCreateInstance =
        (PFN_vkCreateInstance)be->vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { LOGE("getProc(vkCreateInstance) NULL"); return -1; }

    VkApplicationInfo appInfo = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VipleStreamFrucBackend",
        .applicationVersion = 1,
        .pEngineName      = "VipleStream",
        .engineVersion    = 1,
        .apiVersion       = VK_API_VERSION_1_1,
    };

    // VK_KHR_surface + VK_KHR_android_surface are mandatory on every
    // conformant Android Vulkan driver (per vendor compliance), so we
    // demand them rather than probe.
    // §I.E.a recon — probe VK_EXT_swapchain_colorspace (instance ext)
    // for HDR10/BT2020 support. Conditionally enable.
    PFN_vkEnumerateInstanceExtensionProperties vkEnumInstExts =
        (PFN_vkEnumerateInstanceExtensionProperties)be->vkGetInstanceProcAddr(
            NULL, "vkEnumerateInstanceExtensionProperties");
    be->fHdrColorspaceExt = 0;
    if (vkEnumInstExts) {
        uint32_t cnt = 0;
        vkEnumInstExts(NULL, &cnt, NULL);
        if (cnt > 0) {
            VkExtensionProperties* p = (VkExtensionProperties*)calloc(cnt, sizeof(*p));
            if (p) {
                vkEnumInstExts(NULL, &cnt, p);
                for (uint32_t i = 0; i < cnt; i++) {
                    if (strcmp(p[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
                        be->fHdrColorspaceExt = 1;
                        break;
                    }
                }
                free(p);
            }
        }
    }
    LOGI("VK_EXT_swapchain_colorspace %s",
         be->fHdrColorspaceExt ? "available — will enable" : "not available");

    const char* instExts[3];
    instExts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    instExts[1] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    uint32_t instExtCount = 2;
    if (be->fHdrColorspaceExt) {
        instExts[instExtCount++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
    }

    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &appInfo,
        .enabledExtensionCount   = instExtCount,
        .ppEnabledExtensionNames = instExts,
    };

    if (vkCreateInstance(&ici, NULL, &be->instance) != VK_SUCCESS) {
        LOGE("vkCreateInstance failed (likely missing instance extensions)");
        return -1;
    }
    LOGI("VkInstance created (with VK_KHR_surface + VK_KHR_android_surface%s)",
         be->fHdrColorspaceExt ? " + VK_EXT_swapchain_colorspace" : "");

    be->vkDestroyInstance         = LOAD_INSTANCE_PROC(be, vkDestroyInstance);
    be->vkCreateAndroidSurfaceKHR = LOAD_INSTANCE_PROC(be, vkCreateAndroidSurfaceKHR);
    be->vkDestroySurfaceKHR       = LOAD_INSTANCE_PROC(be, vkDestroySurfaceKHR);

    if (!be->vkDestroyInstance || !be->vkCreateAndroidSurfaceKHR ||
        !be->vkDestroySurfaceKHR) {
        LOGE("instance proc table incomplete");
        return -1;
    }
    return 0;
}

static int pick_physical_device_and_queue(vk_backend_t* be)
{
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
        LOAD_INSTANCE_PROC(be, vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceQueueFamilyProperties);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceSupportKHR);

    if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
        !vkGetPhysicalDeviceQueueFamilyProperties ||
        !vkGetPhysicalDeviceSurfaceSupportKHR) {
        LOGE("physical-device proc loading failed");
        return -1;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, NULL);
    if (deviceCount == 0) { LOGE("no physical devices"); return -1; }

    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(deviceCount, sizeof(*devices));
    if (!devices) return -1;
    vkEnumeratePhysicalDevices(be->instance, &deviceCount, devices);
    be->physDevice = devices[0];   // Pixel-class is single-GPU
    free(devices);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(be->physDevice, &props);
    LOGI("VkPhysicalDevice picked: '%s' apiVersion=%u.%u.%u vendor=0x%x",
         props.deviceName,
         (props.apiVersion >> 22) & 0x7F,
         (props.apiVersion >> 12) & 0x3FF,
         props.apiVersion & 0xFFF, props.vendorID);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, NULL);
    if (qfCount == 0) { LOGE("zero queue families"); return -1; }
    VkQueueFamilyProperties* qf =
        (VkQueueFamilyProperties*)calloc(qfCount, sizeof(*qf));
    if (!qf) return -1;
    vkGetPhysicalDeviceQueueFamilyProperties(be->physDevice, &qfCount, qf);

    // Pick a queue family with both graphics and surface present support.
    // VkSurfaceKHR must already exist; this is called after surface creation.
    be->graphicsQueueFamily = (uint32_t)-1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(be->physDevice, i, be->surface, &supported);
        if (supported) {
            be->graphicsQueueFamily = i;
            break;
        }
    }
    free(qf);
    if (be->graphicsQueueFamily == (uint32_t)-1) {
        LOGE("no queue family with graphics + surface present support");
        return -1;
    }
    LOGI("graphics+present queue family = %u", be->graphicsQueueFamily);
    return 0;
}

static int create_device(vk_backend_t* be)
{
    PFN_vkCreateDevice vkCreateDevice = LOAD_INSTANCE_PROC(be, vkCreateDevice);
    PFN_vkEnumerateDeviceExtensionProperties vkEnumDevExts =
        LOAD_INSTANCE_PROC(be, vkEnumerateDeviceExtensionProperties);
    if (!vkCreateDevice || !vkEnumDevExts) {
        LOGE("getProc(vkCreateDevice / vkEnumerateDeviceExtensionProperties) NULL");
        return -1;
    }

    // §I.C.6 — probe VK_GOOGLE_display_timing. Confirmed available on
    // Pixel 5 / Adreno 620 in §I.A.A2 (v1.2.134), but stay defensive in
    // case different drivers / emulators don't ship it.
    // §I.E.a — probe VK_EXT_hdr_metadata in same enumeration pass.
    be->fDisplayTimingSupported = 0;
    be->fHdrMetadataExt = 0;
    {
        uint32_t extCount = 0;
        vkEnumDevExts(be->physDevice, NULL, &extCount, NULL);
        if (extCount > 0) {
            VkExtensionProperties* extProps =
                (VkExtensionProperties*)calloc(extCount, sizeof(*extProps));
            if (extProps) {
                vkEnumDevExts(be->physDevice, NULL, &extCount, extProps);
                for (uint32_t i = 0; i < extCount; i++) {
                    if (strcmp(extProps[i].extensionName,
                               VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0) {
                        be->fDisplayTimingSupported = 1;
                    } else if (strcmp(extProps[i].extensionName,
                               VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0) {
                        be->fHdrMetadataExt = 1;
                    }
                }
                free(extProps);
            }
        }
    }
    LOGI("VK_GOOGLE_display_timing %s",
         be->fDisplayTimingSupported ? "available — will enable" : "not available");
    LOGI("VK_EXT_hdr_metadata %s",
         be->fHdrMetadataExt ? "available — will enable (§I.E recon)" : "not available");

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = be->graphicsQueueFamily,
        .queueCount       = 1,
        .pQueuePriorities = &qprio,
    };

    // VK_ANDROID_external_memory_android_hardware_buffer for B.2c.3b AHB import.
    // Phase A2 confirmed Adreno 620 advertises both extensions on Pixel 5.
    // VK_GOOGLE_display_timing added conditionally for §I.C.6.
    // VK_EXT_hdr_metadata added conditionally for §I.E (recon level).
    const char* deviceExts[4];
    deviceExts[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    deviceExts[1] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    uint32_t deviceExtCount = 2;
    if (be->fDisplayTimingSupported) {
        deviceExts[deviceExtCount++] = VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME;
    }
    if (be->fHdrMetadataExt) {
        deviceExts[deviceExtCount++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }

    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = deviceExtCount,
        .ppEnabledExtensionNames = deviceExts,
    };

    if (vkCreateDevice(be->physDevice, &dci, NULL, &be->device) != VK_SUCCESS) {
        LOGE("vkCreateDevice failed");
        return -1;
    }
    LOGI("VkDevice created");

    be->vkDestroyDevice     = LOAD_INSTANCE_PROC(be, vkDestroyDevice);
    be->vkGetDeviceProcAddr = LOAD_INSTANCE_PROC(be, vkGetDeviceProcAddr);
    PFN_vkGetDeviceQueue vkGetDeviceQueue = LOAD_INSTANCE_PROC(be, vkGetDeviceQueue);
    if (!be->vkDestroyDevice || !be->vkGetDeviceProcAddr || !vkGetDeviceQueue) {
        LOGE("device-level proc loading failed");
        return -1;
    }

    vkGetDeviceQueue(be->device, be->graphicsQueueFamily, 0, &be->graphicsQueue);
    LOGI("VkQueue acquired (graphics, family=%u)", be->graphicsQueueFamily);

    // §I.C.6 — load VK_GOOGLE_display_timing entry points when extension
    // is enabled. PFN miss => disable feature gracefully (refresh-cycle
    // query won't fire, present timing chain won't attach).
    if (be->fDisplayTimingSupported) {
        be->vkGetRefreshCycleDurationGOOGLE = (PFN_vkGetRefreshCycleDurationGOOGLE)
            be->vkGetDeviceProcAddr(be->device, "vkGetRefreshCycleDurationGOOGLE");
        be->vkGetPastPresentationTimingGOOGLE = (PFN_vkGetPastPresentationTimingGOOGLE)
            be->vkGetDeviceProcAddr(be->device, "vkGetPastPresentationTimingGOOGLE");
        if (!be->vkGetRefreshCycleDurationGOOGLE || !be->vkGetPastPresentationTimingGOOGLE) {
            LOGW("display_timing PFNs missing despite extension enabled — disabling feature");
            be->fDisplayTimingSupported = 0;
        }
    }

    // Swapchain function pointers (device-level, but loadable via instance proc)
    be->vkCreateSwapchainKHR    = LOAD_INSTANCE_PROC(be, vkCreateSwapchainKHR);
    be->vkDestroySwapchainKHR   = LOAD_INSTANCE_PROC(be, vkDestroySwapchainKHR);
    be->vkGetSwapchainImagesKHR = LOAD_INSTANCE_PROC(be, vkGetSwapchainImagesKHR);
    if (!be->vkCreateSwapchainKHR || !be->vkDestroySwapchainKHR ||
        !be->vkGetSwapchainImagesKHR) {
        LOGE("swapchain proc loading failed");
        return -1;
    }
    return 0;
}

static int create_surface(vk_backend_t* be, JNIEnv* env, jobject jSurface)
{
    if (!jSurface) { LOGE("Java Surface is null"); return -1; }
    be->nativeWindow = ANativeWindow_fromSurface(env, jSurface);
    if (!be->nativeWindow) {
        LOGE("ANativeWindow_fromSurface returned NULL");
        return -1;
    }

    VkAndroidSurfaceCreateInfoKHR sci = {
        .sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = be->nativeWindow,
    };
    if (be->vkCreateAndroidSurfaceKHR(be->instance, &sci, NULL, &be->surface)
        != VK_SUCCESS) {
        LOGE("vkCreateAndroidSurfaceKHR failed");
        return -1;
    }
    LOGI("VkSurfaceKHR created on ANativeWindow=%p", be->nativeWindow);

    // §I.D.c — request 90 Hz panel mode. Pixel 5 has a 90 Hz LCD but
    // defaults to 60 Hz unless the active app explicitly hints higher.
    // FIXED_SOURCE compatibility tells the compositor "I produce frames
    // at this exact rate — do not interpolate or average". Combined with
    // §I.D.b smart-mode, this lets a 45 FPS input stream trigger dual
    // present (45 < 90 × 0.92) and reach 90 FPS displayed (FRUC's
    // designed sweet spot).
    //
    // Hint-only API; if user hasn't enabled "smooth display" or another
    // window pins lower rate, panel stays at 60 Hz. Verify via
    // [VK-DISPLAY-TIMING] refresh cycle log in create_swapchain.
    //
    // Symbol introduced in API 30 (Android 11). Our minSdk is 21 so the
    // direct call is weakly linked → SIGSEGV at runtime if device < 30.
    // Use dlsym to gracefully no-op on old devices. FIXED_SOURCE = 1
    // per ANW header.
    // Prefer the API 31 setFrameRateWithChangeStrategy with
    // CHANGE_FRAME_RATE_ALWAYS so the compositor *forces* the mode
    // switch even if it's not seamless. The default API 30
    // setFrameRate uses ONLY_IF_SEAMLESS which on Pixel 5 LineageOS
    // 22.1 leaves the panel pinned at 60 Hz despite accepting the
    // 90 Hz hint (observed in v1.2.163: rc=0 accepted but
    // mActiveSfDisplayMode stayed at fps=60).
    {
        typedef int32_t (*PFN_ANW_setFrameRateStrat)(ANativeWindow*, float, int8_t, int8_t);
        typedef int32_t (*PFN_ANW_setFrameRate)(ANativeWindow*, float, int8_t);
        static PFN_ANW_setFrameRateStrat s_anw_setFrameRateStrat = NULL;
        static PFN_ANW_setFrameRate      s_anw_setFrameRate      = NULL;
        static int                       s_loaded                = 0;
        if (!s_loaded) {
            void* libandroid = dlopen("libandroid.so", RTLD_NOW);
            if (libandroid) {
                s_anw_setFrameRateStrat = (PFN_ANW_setFrameRateStrat)
                    dlsym(libandroid, "ANativeWindow_setFrameRateWithChangeStrategy");
                s_anw_setFrameRate = (PFN_ANW_setFrameRate)
                    dlsym(libandroid, "ANativeWindow_setFrameRate");
            }
            s_loaded = 1;
        }
        // Use Java-provided max refresh rate (already in fHintedRefreshHz
        // from nativeInit). Falls back to 90 only if Java query failed.
        float hintRate = (be->fHintedRefreshHz > 0.0f) ? be->fHintedRefreshHz : 90.0f;
        if (s_anw_setFrameRateStrat) {
            // FIXED_SOURCE=1, CHANGE_FRAME_RATE_ALWAYS=1
            int32_t fr = s_anw_setFrameRateStrat(be->nativeWindow, hintRate, 1, 1);
            LOGI("ANativeWindow_setFrameRateWithChangeStrategy(%.1f, FIXED_SOURCE, ALWAYS) rc=%d %s",
                 (double)hintRate, fr, fr == 0 ? "(hint accepted)" : "(rejected)");
            // fHintedRefreshHz already set from Java; don't overwrite even on
            // hint reject (Java's Display query is more authoritative than
            // our setFrameRate request status).
        } else if (s_anw_setFrameRate) {
            int32_t fr = s_anw_setFrameRate(be->nativeWindow, hintRate, 1);
            LOGI("ANativeWindow_setFrameRate(%.1f, FIXED_SOURCE) rc=%d %s (no ChangeStrategy variant)",
                 (double)hintRate, fr, fr == 0 ? "(hint accepted)" : "(rejected)");
        } else {
            LOGI("ANativeWindow_setFrameRate symbol unavailable (need API 30+) — staying at panel default rate");
        }
    }
    return 0;
}

static int create_swapchain(vk_backend_t* be)
{
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfaceFormatsKHR);
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR =
        LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceSurfacePresentModesKHR);

    if (!vkGetPhysicalDeviceSurfaceCapabilitiesKHR ||
        !vkGetPhysicalDeviceSurfaceFormatsKHR ||
        !vkGetPhysicalDeviceSurfacePresentModesKHR) {
        LOGE("surface query proc loading failed");
        return -1;
    }

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(be->physDevice, be->surface, &caps)
        != VK_SUCCESS) {
        LOGE("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return -1;
    }
    LOGI("surface caps: minImage=%u maxImage=%u currentExtent=%ux%u "
         "supportedTransforms=0x%x supportedCompositeAlpha=0x%x",
         caps.minImageCount, caps.maxImageCount,
         caps.currentExtent.width, caps.currentExtent.height,
         caps.supportedTransforms, caps.supportedCompositeAlpha);

    // Pick format. Android-side surface usually advertises BGRA8 UNORM as
    // first option; we accept either RGBA8 or BGRA8 in UNORM/sRGB form.
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(be->physDevice, be->surface, &fmtCount, NULL);
    if (fmtCount == 0) { LOGE("no surface formats"); return -1; }
    VkSurfaceFormatKHR* fmts =
        (VkSurfaceFormatKHR*)calloc(fmtCount, sizeof(*fmts));
    if (!fmts) return -1;
    vkGetPhysicalDeviceSurfaceFormatsKHR(be->physDevice, be->surface, &fmtCount, fmts);

    VkSurfaceFormatKHR chosen = fmts[0];   // first is always valid per spec
    for (uint32_t i = 0; i < fmtCount; i++) {
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = fmts[i];
            break;
        }
    }

    // §I.E.a recon — log all surface formats + look for HDR10 colorspace.
    // We don't switch to HDR yet (still picking SDR R8G8B8A8_UNORM); just
    // record what's available so §I.E.b can negotiate HDR10 + 10-bit
    // format pair. HDR10_ST2084_EXT colorspace value = 1000104008 (per
    // VK_EXT_swapchain_colorspace ext).
    be->fHdrCapableSurface = 0;
    for (uint32_t i = 0; i < fmtCount; i++) {
        const char* csName = "?";
        switch (fmts[i].colorSpace) {
            case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:        csName = "sRGB_NONLINEAR";      break;
            case 1000104001 /*DISPLAY_P3_NONLINEAR_EXT*/:  csName = "DISPLAY_P3_NONLINEAR"; break;
            case 1000104002 /*EXTENDED_SRGB_LINEAR_EXT*/:  csName = "EXTENDED_SRGB_LINEAR"; break;
            case 1000104003 /*DISPLAY_P3_LINEAR_EXT*/:     csName = "DISPLAY_P3_LINEAR";   break;
            case 1000104004 /*DCI_P3_NONLINEAR_EXT*/:      csName = "DCI_P3_NONLINEAR";    break;
            case 1000104005 /*BT709_LINEAR_EXT*/:          csName = "BT709_LINEAR";        break;
            case 1000104006 /*BT709_NONLINEAR_EXT*/:       csName = "BT709_NONLINEAR";     break;
            case 1000104007 /*BT2020_LINEAR_EXT*/:         csName = "BT2020_LINEAR";       break;
            case 1000104008 /*HDR10_ST2084_EXT*/:          csName = "HDR10_ST2084 ✨";     break;
            case 1000104009 /*DOLBYVISION_EXT*/:           csName = "DOLBYVISION";         break;
            case 1000104010 /*HDR10_HLG_EXT*/:             csName = "HDR10_HLG";           break;
            case 1000104011 /*ADOBERGB_LINEAR_EXT*/:       csName = "ADOBERGB_LINEAR";     break;
            case 1000104012 /*ADOBERGB_NONLINEAR_EXT*/:    csName = "ADOBERGB_NONLINEAR";  break;
            case 1000104013 /*PASS_THROUGH_EXT*/:          csName = "PASS_THROUGH";        break;
            case 1000104014 /*EXTENDED_SRGB_NONLINEAR_EXT*/: csName = "EXTENDED_SRGB_NONLINEAR"; break;
            case 1000213000 /*DISPLAY_NATIVE_AMD*/:        csName = "DISPLAY_NATIVE_AMD";  break;
            default: break;
        }
        LOGI("[VK-HDR-RECON] surface[%u]: format=%d colorSpace=%d (%s)",
             i, fmts[i].format, fmts[i].colorSpace, csName);
        if (fmts[i].colorSpace == 1000104008 /* HDR10_ST2084 */) {
            be->fHdrCapableSurface = 1;
        }
    }
    LOGI("[VK-HDR-RECON] HDR10_ST2084 colorspace %s on this surface",
         be->fHdrCapableSurface ? "AVAILABLE" : "not advertised");
    LOGI("picked surface format=%d colorSpace=%d (out of %u)",
         chosen.format, chosen.colorSpace, fmtCount);
    free(fmts);

    // §I.E.a recon — probe 10-bit format support for HDR pipeline.
    // Three candidate formats covering the typical HDR data flow:
    //   A2B10G10R10_UNORM_PACK32 = 64   — 10-bit RGB swapchain image (HDR10 display)
    //   R16G16B16A16_SFLOAT      = 97   — half-float RGB for compute / interp frame
    //   G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 = 1000156005 — YCbCr P010 / P016 (decoded video)
    // Look for SAMPLED_IMAGE_BIT (0x1), STORAGE_IMAGE_BIT (0x2),
    // COLOR_ATTACHMENT_BIT (0x80), MIDPOINT_CHROMA_SAMPLES_BIT (0x20000)
    // in the optimalTilingFeatures bitmask.
    {
        PFN_vkGetPhysicalDeviceFormatProperties vkGetFmtProps =
            LOAD_INSTANCE_PROC(be, vkGetPhysicalDeviceFormatProperties);
        if (vkGetFmtProps) {
            struct { VkFormat f; const char* name; } probes[] = {
                { VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32" },
                { VK_FORMAT_R16G16B16A16_SFLOAT,      "R16G16B16A16_SFLOAT" },
                { (VkFormat)1000156005,               "G10X6_B10X6R10X6_2PLANE_420 (P010)" },
            };
            for (size_t i = 0; i < sizeof(probes)/sizeof(probes[0]); i++) {
                VkFormatProperties fp = {0};
                vkGetFmtProps(be->physDevice, probes[i].f, &fp);
                LOGI("[VK-HDR-RECON] format %s (%d): optimalTiling=0x%x linearTiling=0x%x buffer=0x%x %s",
                     probes[i].name, probes[i].f,
                     fp.optimalTilingFeatures, fp.linearTilingFeatures, fp.bufferFeatures,
                     (fp.optimalTilingFeatures & 0x1) ? "[SAMPLED✓]" : "[no SAMPLED]");
            }
        }
    }

    // Pick FIFO (always available, V-sync gated). Phase D will swap to
    // MAILBOX or IMMEDIATE if measurements warrant.
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    {
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(be->physDevice, be->surface, &pmCount, NULL);
        if (pmCount > 0) {
            VkPresentModeKHR* pms = (VkPresentModeKHR*)calloc(pmCount, sizeof(*pms));
            if (pms) {
                vkGetPhysicalDeviceSurfacePresentModesKHR(be->physDevice, be->surface,
                                                          &pmCount, pms);
                LOGI("present modes available (%u):", pmCount);
                for (uint32_t i = 0; i < pmCount; i++) LOGI("  [%u] = %d", i, pms[i]);
                free(pms);
            }
        }
    }
    LOGI("picked present mode = FIFO (%d)", present);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        // Special value — surface decides at swapchain creation. Use a
        // sane default; swapchain images get their own extent reported on
        // creation anyway.
        extent.width  = 1920;
        extent.height = 1080;
    }

    VkSwapchainCreateInfoKHR sci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = be->surface,
        .minImageCount    = imageCount,
        .imageFormat      = chosen.format,
        .imageColorSpace  = chosen.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = present,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE,
    };

    if (be->vkCreateSwapchainKHR(be->device, &sci, NULL, &be->swapchain)
        != VK_SUCCESS) {
        LOGE("vkCreateSwapchainKHR failed");
        return -1;
    }
    be->swapchainFormat = chosen.format;
    be->swapchainExtent = extent;
    LOGI("VkSwapchainKHR created (extent=%ux%u, requested=%u images)",
         extent.width, extent.height, imageCount);

    be->vkGetSwapchainImagesKHR(be->device, be->swapchain,
                                &be->swapchainImageCount, NULL);
    if (be->swapchainImageCount == 0) {
        LOGE("swapchain reported 0 images");
        return -1;
    }
    be->swapchainImages = (VkImage*)calloc(be->swapchainImageCount, sizeof(VkImage));
    if (!be->swapchainImages) return -1;
    be->vkGetSwapchainImagesKHR(be->device, be->swapchain,
                                &be->swapchainImageCount, be->swapchainImages);
    LOGI("swapchain image count actual = %u", be->swapchainImageCount);

    // §I.C.6 — query display refresh cycle duration once swapchain is up.
    // Cached as fRefreshDurationNs; used to space PASS 1 / PASS 2 PTS in
    // dual present (interp at +0, real at +refreshDuration/2 → driver
    // ideally rasters them on consecutive vsyncs).
    if (be->fDisplayTimingSupported && be->vkGetRefreshCycleDurationGOOGLE) {
        VkRefreshCycleDurationGOOGLE rcd = { 0 };
        VkResult rc = be->vkGetRefreshCycleDurationGOOGLE(be->device, be->swapchain, &rcd);
        if (rc == VK_SUCCESS && rcd.refreshDuration > 0) {
            be->fRefreshDurationNs = rcd.refreshDuration;
            double hz = 1e9 / (double)rcd.refreshDuration;
            LOGI("[VK-DISPLAY-TIMING] refresh cycle = %llu ns (~%.2f Hz)",
                 (unsigned long long)rcd.refreshDuration, hz);
        } else {
            LOGW("[VK-DISPLAY-TIMING] vkGetRefreshCycleDurationGOOGLE failed (rc=%d, dur=%llu) — disabling",
                 rc, (unsigned long long)rcd.refreshDuration);
            be->fDisplayTimingSupported = 0;
        }
    }

    return 0;
}

// ---------- B.2c.3a: persistent per-frame render resources ----------

static int load_ahb_procs(vk_backend_t* be)
{
    be->vkGetAndroidHardwareBufferPropertiesANDROID =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)be->vkGetDeviceProcAddr(
            be->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    be->vkCreateImage     = (PFN_vkCreateImage)be->vkGetDeviceProcAddr(be->device, "vkCreateImage");
    be->vkDestroyImage    = (PFN_vkDestroyImage)be->vkGetDeviceProcAddr(be->device, "vkDestroyImage");
    be->vkAllocateMemory  = (PFN_vkAllocateMemory)be->vkGetDeviceProcAddr(be->device, "vkAllocateMemory");
    be->vkFreeMemory      = (PFN_vkFreeMemory)be->vkGetDeviceProcAddr(be->device, "vkFreeMemory");
    be->vkBindImageMemory = (PFN_vkBindImageMemory)be->vkGetDeviceProcAddr(be->device, "vkBindImageMemory");
    be->vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)be->vkGetInstanceProcAddr(
            be->instance, "vkGetPhysicalDeviceMemoryProperties");
    // YCbCr sampler procs (Vulkan 1.1 core)
    be->vkCreateSamplerYcbcrConversion =
        (PFN_vkCreateSamplerYcbcrConversion)be->vkGetDeviceProcAddr(be->device, "vkCreateSamplerYcbcrConversion");
    be->vkDestroySamplerYcbcrConversion =
        (PFN_vkDestroySamplerYcbcrConversion)be->vkGetDeviceProcAddr(be->device, "vkDestroySamplerYcbcrConversion");
    be->vkCreateSampler  = (PFN_vkCreateSampler)be->vkGetDeviceProcAddr(be->device, "vkCreateSampler");
    be->vkDestroySampler = (PFN_vkDestroySampler)be->vkGetDeviceProcAddr(be->device, "vkDestroySampler");

    if (!be->vkGetAndroidHardwareBufferPropertiesANDROID || !be->vkCreateImage ||
        !be->vkDestroyImage || !be->vkAllocateMemory || !be->vkFreeMemory ||
        !be->vkBindImageMemory || !be->vkGetPhysicalDeviceMemoryProperties ||
        !be->vkCreateSamplerYcbcrConversion || !be->vkDestroySamplerYcbcrConversion ||
        !be->vkCreateSampler || !be->vkDestroySampler) {
        LOGE("load_ahb_procs: missing entry points");
        return -1;
    }
    return 0;
}

// Lazy create the YCbCr conversion + immutable sampler the first time we
// see an AHB. The conversion baked the externalFormat / ycbcrModel / range
// from MediaCodec's actual output — they're stable for a session, so we
// create once and reuse for the lifetime of the backend.
static int ensure_ycbcr_sampler(vk_backend_t* be,
                                const VkAndroidHardwareBufferFormatPropertiesANDROID* fmt)
{
    if (be->ycbcrInitialized) {
        if (be->ycbcrExternalFormat != fmt->externalFormat) {
            LOGW("AHB externalFormat changed (was 0x%llx, now 0x%llx) — sampler is now stale",
                 (unsigned long long)be->ycbcrExternalFormat,
                 (unsigned long long)fmt->externalFormat);
        }
        return 0;
    }
    if (!be->vkCreateSamplerYcbcrConversion || !be->vkCreateSampler) {
        LOGE("ensure_ycbcr_sampler: procs not loaded");
        return -1;
    }

    VkExternalFormatANDROID extFmt = {
        .sType          = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        .externalFormat = fmt->externalFormat,
    };
    VkSamplerYcbcrConversionCreateInfo cci = {
        .sType         = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .pNext         = &extFmt,
        .format        = VK_FORMAT_UNDEFINED,
        .ycbcrModel    = fmt->suggestedYcbcrModel,
        .ycbcrRange    = fmt->suggestedYcbcrRange,
        .components    = fmt->samplerYcbcrConversionComponents,
        .xChromaOffset = fmt->suggestedXChromaOffset,
        .yChromaOffset = fmt->suggestedYChromaOffset,
        .chromaFilter  = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
    };
    VkResult r = be->vkCreateSamplerYcbcrConversion(be->device, &cci, NULL, &be->ycbcrConversion);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSamplerYcbcrConversion failed: %d", r);
        return -1;
    }

    VkSamplerYcbcrConversionInfo convInfo = {
        .sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = be->ycbcrConversion,
    };
    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext        = &convInfo,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .compareEnable    = VK_FALSE,
        .borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    r = be->vkCreateSampler(be->device, &sci, NULL, &be->ycbcrSampler);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateSampler (with ycbcr conversion) failed: %d", r);
        be->vkDestroySamplerYcbcrConversion(be->device, be->ycbcrConversion, NULL);
        be->ycbcrConversion = VK_NULL_HANDLE;
        return -1;
    }

    be->ycbcrExternalFormat = fmt->externalFormat;
    be->ycbcrInitialized = 1;
    LOGI("YCbCr sampler ready: externalFormat=0x%llx model=%d range=%d "
         "components(r=%d g=%d b=%d a=%d) chromaXOff=%d chromaYOff=%d",
         (unsigned long long)fmt->externalFormat,
         fmt->suggestedYcbcrModel, fmt->suggestedYcbcrRange,
         fmt->samplerYcbcrConversionComponents.r,
         fmt->samplerYcbcrConversionComponents.g,
         fmt->samplerYcbcrConversionComponents.b,
         fmt->samplerYcbcrConversionComponents.a,
         fmt->suggestedXChromaOffset, fmt->suggestedYChromaOffset);
    return 0;
}

static void destroy_ycbcr_sampler(vk_backend_t* be)
{
    if (be->ycbcrSampler && be->vkDestroySampler) {
        be->vkDestroySampler(be->device, be->ycbcrSampler, NULL);
        be->ycbcrSampler = VK_NULL_HANDLE;
    }
    if (be->ycbcrConversion && be->vkDestroySamplerYcbcrConversion) {
        be->vkDestroySamplerYcbcrConversion(be->device, be->ycbcrConversion, NULL);
        be->ycbcrConversion = VK_NULL_HANDLE;
    }
    be->ycbcrInitialized = 0;
}

static int pick_memory_type(vk_backend_t* be, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties memProps;
    be->vkGetPhysicalDeviceMemoryProperties(be->physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (bits & (1u << i)) return (int)i;
    }
    return -1;
}

// Import an AHardwareBuffer into Vulkan resources. Two modes:
//   keepAlive=0 → import + destroy in-function (validation only, B.2c.3b)
//   keepAlive=1 → return image / mem / view through outImage / outMem /
//                 outView; caller is responsible for freeing after the
//                 GPU is done reading them
static int do_import_ahb(vk_backend_t* be, AHardwareBuffer* ahb, int keepAlive,
                         VkImage* outImage, VkDeviceMemory* outMem, VkImageView* outView,
                         uint32_t* outWidth, uint32_t* outHeight)
{
    if (!be->vkGetAndroidHardwareBufferPropertiesANDROID) return -2;

    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(ahb, &desc);

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
    };
    VkAndroidHardwareBufferPropertiesANDROID props = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
        .pNext = &fmtProps,
    };
    VkResult r = be->vkGetAndroidHardwareBufferPropertiesANDROID(be->device, ahb, &props);
    if (r != VK_SUCCESS) {
        LOGE("vkGetAndroidHardwareBufferPropertiesANDROID failed: %d", r);
        return -1;
    }

    if (!be->ahbImportLogged) {
        LOGI("AHB desc: %ux%u layers=%u format=0x%x usage=0x%llx",
             desc.width, desc.height, desc.layers, desc.format,
             (unsigned long long)desc.usage);
        LOGI("AHB props: allocSize=%llu memTypeBits=0x%x VkFormat=%d "
             "externalFormat=0x%llx samplerYcbcrModel=%d range=%d",
             (unsigned long long)props.allocationSize, props.memoryTypeBits,
             fmtProps.format, (unsigned long long)fmtProps.externalFormat,
             fmtProps.suggestedYcbcrModel, fmtProps.suggestedYcbcrRange);
    }

    // B.2c.3c.1: lazy-create the YCbCr sampler from the first frame's
    // externalFormat. Subsequent frames reuse it. Sampler binds during
    // B.2c.3c.3 once the graphics pipeline lands.
    if (fmtProps.externalFormat != 0) {
        ensure_ycbcr_sampler(be, &fmtProps);
    }

    // VkImage with external chain. Format VK_FORMAT_UNDEFINED + non-zero
    // externalFormat is the YUV-from-AHB pattern (ycbcr_conversion territory);
    // RGBA-style AHB returns a real VkFormat in fmtProps.format.
    VkExternalFormatANDROID extFmt = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID,
        .externalFormat = fmtProps.externalFormat,
    };
    VkExternalMemoryImageCreateInfo extImgInfo = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = (fmtProps.externalFormat != 0) ? &extFmt : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &extImgInfo,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = (fmtProps.externalFormat != 0) ? VK_FORMAT_UNDEFINED : fmtProps.format,
        .extent = { desc.width, desc.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage importedImage = VK_NULL_HANDLE;
    r = be->vkCreateImage(be->device, &ici, NULL, &importedImage);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateImage (AHB) failed: %d", r);
        return -1;
    }

    int memTypeIdx = pick_memory_type(be, props.memoryTypeBits);
    if (memTypeIdx < 0) {
        LOGE("no compatible memory type for AHB import");
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    VkImportAndroidHardwareBufferInfoANDROID importInfo = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = ahb,
    };
    VkMemoryDedicatedAllocateInfo dedAlloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &importInfo,
        .image = importedImage,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedAlloc,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = (uint32_t)memTypeIdx,
    };

    VkDeviceMemory importedMem = VK_NULL_HANDLE;
    r = be->vkAllocateMemory(be->device, &mai, NULL, &importedMem);
    if (r != VK_SUCCESS) {
        LOGE("vkAllocateMemory (AHB) failed: %d", r);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    r = be->vkBindImageMemory(be->device, importedImage, importedMem, 0);
    if (r != VK_SUCCESS) {
        LOGE("vkBindImageMemory (AHB) failed: %d", r);
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    if (!be->ahbImportLogged) {
        LOGI("AHB import OK: VkImage %p + VkDeviceMemory %p bound (memTypeIdx=%d)",
             importedImage, importedMem, memTypeIdx);
        be->ahbImportLogged = 1;
    }

    if (!keepAlive) {
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return 0;
    }

    // Caller wants the image alive for sampling. Build a view with the
    // SamplerYcbcrConversion chain so the descriptor-set update knows the
    // implicit YUV→RGB conversion to apply.
    if (!be->ycbcrInitialized) {
        LOGE("do_import_ahb keepAlive: ycbcr sampler not ready");
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }
    VkSamplerYcbcrConversionInfo viewYcbcr = {
        .sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = be->ycbcrConversion,
    };
    VkImageViewCreateInfo ivci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = &viewYcbcr,
        .image    = importedImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_UNDEFINED,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    VkImageView importedView = VK_NULL_HANDLE;
    r = be->vkCreateImageView(be->device, &ivci, NULL, &importedView);
    if (r != VK_SUCCESS) {
        LOGE("vkCreateImageView (AHB) failed: %d", r);
        be->vkFreeMemory(be->device, importedMem, NULL);
        be->vkDestroyImage(be->device, importedImage, NULL);
        return -1;
    }

    if (outImage)  *outImage  = importedImage;
    if (outMem)    *outMem    = importedMem;
    if (outView)   *outView   = importedView;
    if (outWidth)  *outWidth  = desc.width;
    if (outHeight) *outHeight = desc.height;
    // Cache AHB padded dims so the per-frame push constant in
    // render_ahb_frame can compute uvScale = videoSize / ahbPaddedSize.
    be->ahbPaddedWidth  = (int)desc.width;
    be->ahbPaddedHeight = (int)desc.height;
    return 0;
}

// Backward-compat shim used by Java's nativeImportAhb (B.2c.3b validation).
static int try_import_ahb(vk_backend_t* be, AHardwareBuffer* ahb)
{
    return do_import_ahb(be, ahb, /*keepAlive=*/0, NULL, NULL, NULL, NULL, NULL);
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeImportAhb(JNIEnv* env, jclass clazz,
                                                            jlong handle, jobject jHwBuffer)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be || !jHwBuffer) return -1;
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, jHwBuffer);
    if (!ahb) {
        LOGE("AHardwareBuffer_fromHardwareBuffer returned NULL");
        return -1;
    }
    return (jint)try_import_ahb(be, ahb);
}

static int load_render_procs(vk_backend_t* be)
{
    be->vkAcquireNextImageKHR    = (PFN_vkAcquireNextImageKHR)be->vkGetDeviceProcAddr(be->device, "vkAcquireNextImageKHR");
    be->vkQueuePresentKHR        = (PFN_vkQueuePresentKHR)be->vkGetDeviceProcAddr(be->device, "vkQueuePresentKHR");
    be->vkQueueSubmit            = (PFN_vkQueueSubmit)be->vkGetDeviceProcAddr(be->device, "vkQueueSubmit");
    be->vkQueueWaitIdle          = (PFN_vkQueueWaitIdle)be->vkGetDeviceProcAddr(be->device, "vkQueueWaitIdle");
    be->vkBeginCommandBuffer     = (PFN_vkBeginCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkBeginCommandBuffer");
    be->vkEndCommandBuffer       = (PFN_vkEndCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkEndCommandBuffer");
    be->vkResetCommandBuffer     = (PFN_vkResetCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkResetCommandBuffer");
    be->vkCmdClearColorImage     = (PFN_vkCmdClearColorImage)be->vkGetDeviceProcAddr(be->device, "vkCmdClearColorImage");
    be->vkCmdPipelineBarrier     = (PFN_vkCmdPipelineBarrier)be->vkGetDeviceProcAddr(be->device, "vkCmdPipelineBarrier");
    be->vkCreateSemaphore        = (PFN_vkCreateSemaphore)be->vkGetDeviceProcAddr(be->device, "vkCreateSemaphore");
    be->vkDestroySemaphore       = (PFN_vkDestroySemaphore)be->vkGetDeviceProcAddr(be->device, "vkDestroySemaphore");
    be->vkCreateCommandPool      = (PFN_vkCreateCommandPool)be->vkGetDeviceProcAddr(be->device, "vkCreateCommandPool");
    be->vkDestroyCommandPool     = (PFN_vkDestroyCommandPool)be->vkGetDeviceProcAddr(be->device, "vkDestroyCommandPool");
    be->vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)be->vkGetDeviceProcAddr(be->device, "vkAllocateCommandBuffers");
    if (!be->vkAcquireNextImageKHR || !be->vkQueuePresentKHR || !be->vkQueueSubmit ||
        !be->vkQueueWaitIdle || !be->vkBeginCommandBuffer || !be->vkEndCommandBuffer ||
        !be->vkResetCommandBuffer || !be->vkCmdClearColorImage || !be->vkCmdPipelineBarrier ||
        !be->vkCreateSemaphore || !be->vkDestroySemaphore ||
        !be->vkCreateCommandPool || !be->vkDestroyCommandPool || !be->vkAllocateCommandBuffers) {
        LOGE("load_render_procs: missing entry points");
        return -1;
    }
    return 0;
}

static int init_render_resources(vk_backend_t* be)
{
    if (load_render_procs(be) != 0) return -1;

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (be->vkCreateSemaphore(be->device, &sci, NULL, &be->acquireSem) != VK_SUCCESS ||
        be->vkCreateSemaphore(be->device, &sci, NULL, &be->renderDoneSem) != VK_SUCCESS) {
        LOGE("init_render_resources: semaphore create failed");
        return -1;
    }

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = be->graphicsQueueFamily,
    };
    if (be->vkCreateCommandPool(be->device, &pci, NULL, &be->cmdPool) != VK_SUCCESS) {
        LOGE("init_render_resources: cmdPool create failed");
        return -1;
    }

    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = be->cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (be->vkAllocateCommandBuffers(be->device, &cai, &be->cmdBuffer) != VK_SUCCESS) {
        LOGE("init_render_resources: cmdBuffer alloc failed");
        return -1;
    }

    be->renderInitialized = 1;
    LOGI("render resources ready (cmdPool + 1 cmdBuffer + 2 semaphores)");
    return 0;
}

static void destroy_render_resources(vk_backend_t* be)
{
    if (!be->renderInitialized) return;
    if (be->vkQueueWaitIdle && be->graphicsQueue) be->vkQueueWaitIdle(be->graphicsQueue);
    if (be->cmdPool && be->vkDestroyCommandPool) {
        be->vkDestroyCommandPool(be->device, be->cmdPool, NULL);
        be->cmdPool = VK_NULL_HANDLE;
    }
    if (be->acquireSem && be->vkDestroySemaphore) {
        be->vkDestroySemaphore(be->device, be->acquireSem, NULL);
        be->acquireSem = VK_NULL_HANDLE;
    }
    if (be->renderDoneSem && be->vkDestroySemaphore) {
        be->vkDestroySemaphore(be->device, be->renderDoneSem, NULL);
        be->renderDoneSem = VK_NULL_HANDLE;
    }
    be->renderInitialized = 0;
}

// One-shot acquire/clear/present roundtrip used during init to validate the
// driver. Steady-state per-frame uses render_clear_frame() below.
//
// ---------- B.2c.1: one-shot acquire/clear/present sanity ----------

static int sanity_present(vk_backend_t* be)
{
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR =
        (PFN_vkAcquireNextImageKHR)be->vkGetDeviceProcAddr(be->device, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR vkQueuePresentKHR =
        (PFN_vkQueuePresentKHR)be->vkGetDeviceProcAddr(be->device, "vkQueuePresentKHR");
    PFN_vkQueueSubmit vkQueueSubmit =
        (PFN_vkQueueSubmit)be->vkGetDeviceProcAddr(be->device, "vkQueueSubmit");
    PFN_vkQueueWaitIdle vkQueueWaitIdle =
        (PFN_vkQueueWaitIdle)be->vkGetDeviceProcAddr(be->device, "vkQueueWaitIdle");
    PFN_vkCreateSemaphore vkCreateSemaphore =
        (PFN_vkCreateSemaphore)be->vkGetDeviceProcAddr(be->device, "vkCreateSemaphore");
    PFN_vkDestroySemaphore vkDestroySemaphore =
        (PFN_vkDestroySemaphore)be->vkGetDeviceProcAddr(be->device, "vkDestroySemaphore");
    PFN_vkCreateCommandPool vkCreateCommandPool =
        (PFN_vkCreateCommandPool)be->vkGetDeviceProcAddr(be->device, "vkCreateCommandPool");
    PFN_vkDestroyCommandPool vkDestroyCommandPool =
        (PFN_vkDestroyCommandPool)be->vkGetDeviceProcAddr(be->device, "vkDestroyCommandPool");
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers =
        (PFN_vkAllocateCommandBuffers)be->vkGetDeviceProcAddr(be->device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer =
        (PFN_vkBeginCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer vkEndCommandBuffer =
        (PFN_vkEndCommandBuffer)be->vkGetDeviceProcAddr(be->device, "vkEndCommandBuffer");
    PFN_vkCmdClearColorImage vkCmdClearColorImage =
        (PFN_vkCmdClearColorImage)be->vkGetDeviceProcAddr(be->device, "vkCmdClearColorImage");
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier =
        (PFN_vkCmdPipelineBarrier)be->vkGetDeviceProcAddr(be->device, "vkCmdPipelineBarrier");

    if (!vkAcquireNextImageKHR || !vkQueuePresentKHR || !vkQueueSubmit ||
        !vkQueueWaitIdle || !vkCreateSemaphore || !vkDestroySemaphore ||
        !vkCreateCommandPool || !vkDestroyCommandPool || !vkAllocateCommandBuffers ||
        !vkBeginCommandBuffer || !vkEndCommandBuffer ||
        !vkCmdClearColorImage || !vkCmdPipelineBarrier) {
        LOGE("sanity_present: device proc loading failed");
        return -1;
    }

    int rc = -1;
    VkSemaphore acquireSem = VK_NULL_HANDLE;
    VkSemaphore renderDoneSem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(be->device, &sci, NULL, &acquireSem) != VK_SUCCESS ||
        vkCreateSemaphore(be->device, &sci, NULL, &renderDoneSem) != VK_SUCCESS) {
        LOGE("sanity_present: vkCreateSemaphore failed");
        goto cleanup;
    }

    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = be->graphicsQueueFamily,
    };
    if (vkCreateCommandPool(be->device, &pci, NULL, &pool) != VK_SUCCESS) {
        LOGE("sanity_present: vkCreateCommandPool failed");
        goto cleanup;
    }

    VkCommandBufferAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(be->device, &cai, &cb) != VK_SUCCESS) {
        LOGE("sanity_present: vkAllocateCommandBuffers failed");
        goto cleanup;
    }

    uint32_t imgIdx = 0;
    VkResult r = vkAcquireNextImageKHR(be->device, be->swapchain, 1000000000ULL,
                                       acquireSem, VK_NULL_HANDLE, &imgIdx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("sanity_present: vkAcquireNextImageKHR failed (VkResult=%d)", r);
        goto cleanup;
    }
    LOGI("sanity_present: acquired image index %u (VkResult=%d)", imgIdx, r);

    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(cb, &bbi) != VK_SUCCESS) {
        LOGE("sanity_present: vkBeginCommandBuffer failed");
        goto cleanup;
    }

    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };

    // UNDEFINED → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier toDst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = be->swapchainImages[imgIdx],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &toDst);

    // Clear with VipleStream-ish dark green so it's distinguishable
    VkClearColorValue clearColor = { .float32 = { 0.0f, 0.18f, 0.0f, 1.0f } };
    vkCmdClearColorImage(cb, be->swapchainImages[imgIdx],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clearColor, 1, &range);

    // TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR
    VkImageMemoryBarrier toPresent = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = be->swapchainImages[imgIdx],
        .subresourceRange = range,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &toPresent);

    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        LOGE("sanity_present: vkEndCommandBuffer failed");
        goto cleanup;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &acquireSem,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &renderDoneSem,
    };
    if (vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        LOGE("sanity_present: vkQueueSubmit failed");
        goto cleanup;
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &renderDoneSem,
        .swapchainCount = 1,
        .pSwapchains = &be->swapchain,
        .pImageIndices = &imgIdx,
    };
    r = vkQueuePresentKHR(be->graphicsQueue, &pi);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("sanity_present: vkQueuePresentKHR failed (VkResult=%d)", r);
        goto cleanup;
    }
    LOGI("sanity_present: submitted + presented (VkResult=%d)", r);

    if (vkQueueWaitIdle(be->graphicsQueue) != VK_SUCCESS) {
        LOGE("sanity_present: vkQueueWaitIdle failed");
        goto cleanup;
    }
    LOGI("sanity_present: queue idle — full present roundtrip OK");
    rc = 0;

cleanup:
    if (pool) vkDestroyCommandPool(be->device, pool, NULL);
    if (acquireSem) vkDestroySemaphore(be->device, acquireSem, NULL);
    if (renderDoneSem) vkDestroySemaphore(be->device, renderDoneSem, NULL);
    return rc;
}

// ---------- JNI: nativeInit / nativeDestroy ----------

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_video_VkBackend_nativeInit(JNIEnv* env, jclass clazz, jobject jSurface,
                                                       jint videoWidth, jint videoHeight,
                                                       jfloat maxRefreshHz)
{
    vk_backend_t* be = (vk_backend_t*)calloc(1, sizeof(*be));
    if (!be) { LOGE("calloc failed"); return 0; }
    be->graphicsQueueFamily = (uint32_t)-1;
    be->videoWidth  = (int)videoWidth;
    be->videoHeight = (int)videoHeight;
    // §I.D.c v2: Java caller probed Display.getSupportedModes()'s max
    // refresh rate. Used both as setFrameRate hint target AND smart-mode
    // displayHz reference. Hardcoded 90 was Pixel-5-only.
    if (maxRefreshHz > 0.0f) {
        be->fHintedRefreshHz = (float)maxRefreshHz;
    }
    LOGI("nativeInit: video logical dims = %dx%d, device max refresh = %.1f Hz",
         be->videoWidth, be->videoHeight, (double)maxRefreshHz);

    be->libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!be->libvulkan) { LOGE("dlopen libvulkan.so failed: %s", dlerror()); goto fail; }
    be->vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(be->libvulkan, "vkGetInstanceProcAddr");
    if (!be->vkGetInstanceProcAddr) { LOGE("dlsym vkGetInstanceProcAddr failed"); goto fail; }

    if (create_instance(be) != 0) goto fail;
    if (create_surface(be, env, jSurface) != 0) goto fail;
    if (pick_physical_device_and_queue(be) != 0) goto fail;
    if (create_device(be) != 0) goto fail;
    if (create_swapchain(be) != 0) goto fail;

    // B.2c.1 sanity: drive one acquire/clear/present roundtrip to prove
    // the swapchain actually renders end-to-end on this driver before we
    // start trusting it with the MediaCodec pipeline.
    if (sanity_present(be) != 0) {
        LOGW("sanity_present failed — keeping init successful so we can dump diagnostics");
    }

    // B.2c.3a: persistent resources for steady-state per-frame rendering.
    if (init_render_resources(be) != 0) {
        LOGW("init_render_resources failed — per-frame nativeRenderClearFrame will return early");
    }

    // B.2c.3b: load AHB import procs. Don't gate init on this — if it
    // fails we just lose AHB import logging on Java-side import calls.
    if (load_ahb_procs(be) != 0) {
        LOGW("load_ahb_procs failed — nativeImportAhb will return -2 on every frame");
    }

    // Graphics pipeline is now LAZY (B.2c.3c.3): we wait for the first
    // AHB so its externalFormat can drive a VkSamplerYcbcrConversion
    // immutable sampler in the descriptor set layout. Until that lands
    // the per-frame render path falls back to the clear-only path.
    if (load_graphics_procs(be) != 0) {
        LOGW("load_graphics_procs failed — graphics pipeline will never lazy-init");
    }

    LOGI("B.2c.3c.3 init complete: instance + surface + device + queue + swapchain "
         "+ render + AHB (graphics pipeline lazy-init on first AHB frame)");
    return (jlong)(uintptr_t)be;

fail:
    if (be) {
        if (be->swapchainImages) free(be->swapchainImages);
        if (be->swapchain && be->vkDestroySwapchainKHR)
            be->vkDestroySwapchainKHR(be->device, be->swapchain, NULL);
        if (be->surface && be->vkDestroySurfaceKHR)
            be->vkDestroySurfaceKHR(be->instance, be->surface, NULL);
        if (be->nativeWindow) ANativeWindow_release(be->nativeWindow);
        if (be->device && be->vkDestroyDevice) be->vkDestroyDevice(be->device, NULL);
        if (be->instance && be->vkDestroyInstance) be->vkDestroyInstance(be->instance, NULL);
        if (be->libvulkan) dlclose(be->libvulkan);
        free(be);
    }
    return 0;
}

// ---------- B.2c.3c.2: graphics pipeline (test pattern fragment) ----------

#define LOAD_DEVICE_PROC(be, name) \
    ((PFN_##name)(be)->vkGetDeviceProcAddr((be)->device, #name))

static int load_graphics_procs(vk_backend_t* be)
{
    be->vkCreateShaderModule        = LOAD_DEVICE_PROC(be, vkCreateShaderModule);
    be->vkDestroyShaderModule       = LOAD_DEVICE_PROC(be, vkDestroyShaderModule);
    be->vkCreateRenderPass          = LOAD_DEVICE_PROC(be, vkCreateRenderPass);
    be->vkDestroyRenderPass         = LOAD_DEVICE_PROC(be, vkDestroyRenderPass);
    be->vkCreateImageView           = LOAD_DEVICE_PROC(be, vkCreateImageView);
    be->vkDestroyImageView          = LOAD_DEVICE_PROC(be, vkDestroyImageView);
    be->vkCreateFramebuffer         = LOAD_DEVICE_PROC(be, vkCreateFramebuffer);
    be->vkDestroyFramebuffer        = LOAD_DEVICE_PROC(be, vkDestroyFramebuffer);
    be->vkCreatePipelineLayout      = LOAD_DEVICE_PROC(be, vkCreatePipelineLayout);
    be->vkDestroyPipelineLayout     = LOAD_DEVICE_PROC(be, vkDestroyPipelineLayout);
    be->vkCreateDescriptorSetLayout = LOAD_DEVICE_PROC(be, vkCreateDescriptorSetLayout);
    be->vkDestroyDescriptorSetLayout= LOAD_DEVICE_PROC(be, vkDestroyDescriptorSetLayout);
    be->vkCreateGraphicsPipelines   = LOAD_DEVICE_PROC(be, vkCreateGraphicsPipelines);
    be->vkDestroyPipeline           = LOAD_DEVICE_PROC(be, vkDestroyPipeline);
    be->vkCmdBeginRenderPass        = LOAD_DEVICE_PROC(be, vkCmdBeginRenderPass);
    be->vkCmdEndRenderPass          = LOAD_DEVICE_PROC(be, vkCmdEndRenderPass);
    be->vkCmdBindPipeline           = LOAD_DEVICE_PROC(be, vkCmdBindPipeline);
    be->vkCmdDraw                   = LOAD_DEVICE_PROC(be, vkCmdDraw);
    be->vkCmdPushConstants          = LOAD_DEVICE_PROC(be, vkCmdPushConstants);
    be->vkCreateDescriptorPool      = LOAD_DEVICE_PROC(be, vkCreateDescriptorPool);
    be->vkDestroyDescriptorPool     = LOAD_DEVICE_PROC(be, vkDestroyDescriptorPool);
    be->vkAllocateDescriptorSets    = LOAD_DEVICE_PROC(be, vkAllocateDescriptorSets);
    be->vkUpdateDescriptorSets      = LOAD_DEVICE_PROC(be, vkUpdateDescriptorSets);
    be->vkCmdBindDescriptorSets     = LOAD_DEVICE_PROC(be, vkCmdBindDescriptorSets);
    if (!be->vkCreateShaderModule || !be->vkDestroyShaderModule ||
        !be->vkCreateRenderPass || !be->vkDestroyRenderPass ||
        !be->vkCreateImageView || !be->vkDestroyImageView ||
        !be->vkCreateFramebuffer || !be->vkDestroyFramebuffer ||
        !be->vkCreatePipelineLayout || !be->vkDestroyPipelineLayout ||
        !be->vkCreateDescriptorSetLayout || !be->vkDestroyDescriptorSetLayout ||
        !be->vkCreateGraphicsPipelines || !be->vkDestroyPipeline ||
        !be->vkCmdBeginRenderPass || !be->vkCmdEndRenderPass ||
        !be->vkCmdBindPipeline || !be->vkCmdDraw || !be->vkCmdPushConstants ||
        !be->vkCreateDescriptorPool || !be->vkDestroyDescriptorPool ||
        !be->vkAllocateDescriptorSets || !be->vkUpdateDescriptorSets ||
        !be->vkCmdBindDescriptorSets) {
        LOGE("load_graphics_procs: missing entry points");
        return -1;
    }
    return 0;
}

static int init_graphics_pipeline(vk_backend_t* be)
{
    if (load_graphics_procs(be) != 0) return -1;

    // Render pass: 1 color attachment matching swapchain format. clear→
    // present_src so the framebuffer can hand off to vkQueuePresentKHR.
    VkAttachmentDescription colorAttach = {
        .format         = be->swapchainFormat,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference colorRef = {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &colorRef,
    };
    VkSubpassDependency dep = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &colorAttach,
        .subpassCount    = 1, .pSubpasses    = &subpass,
        .dependencyCount = 1, .pDependencies = &dep,
    };
    if (be->vkCreateRenderPass(be->device, &rpci, NULL, &be->renderPass) != VK_SUCCESS) {
        LOGE("vkCreateRenderPass failed"); return -1;
    }

    // Image views + framebuffers per swapchain image
    be->swapchainViews = (VkImageView*)calloc(be->swapchainImageCount, sizeof(VkImageView));
    be->framebuffers   = (VkFramebuffer*)calloc(be->swapchainImageCount, sizeof(VkFramebuffer));
    if (!be->swapchainViews || !be->framebuffers) return -1;

    for (uint32_t i = 0; i < be->swapchainImageCount; i++) {
        VkImageViewCreateInfo ivci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = be->swapchainImages[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = be->swapchainFormat,
            .components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0, .levelCount = 1,
                .baseArrayLayer = 0, .layerCount = 1,
            },
        };
        if (be->vkCreateImageView(be->device, &ivci, NULL, &be->swapchainViews[i]) != VK_SUCCESS) {
            LOGE("vkCreateImageView[%u] failed", i); return -1;
        }
        VkFramebufferCreateInfo fci = {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass      = be->renderPass,
            .attachmentCount = 1,
            .pAttachments    = &be->swapchainViews[i],
            .width           = be->swapchainExtent.width,
            .height          = be->swapchainExtent.height,
            .layers          = 1,
        };
        if (be->vkCreateFramebuffer(be->device, &fci, NULL, &be->framebuffers[i]) != VK_SUCCESS) {
            LOGE("vkCreateFramebuffer[%u] failed", i); return -1;
        }
    }

    // Shader modules. Pick the frag based on whether the YCbCr sampler is
    // ready (= we'll have a real video texture to sample). Without it we
    // fall back to the UV-gradient test pattern.
    VkShaderModuleCreateInfo smciV = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = fullscreen_vert_spv_len,
        .pCode    = (const uint32_t*)fullscreen_vert_spv,
    };
    VkShaderModuleCreateInfo smciF = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    if (be->ycbcrInitialized) {
        smciF.codeSize = video_sample_frag_spv_len;
        smciF.pCode    = (const uint32_t*)video_sample_frag_spv;
    } else {
        smciF.codeSize = test_pattern_frag_spv_len;
        smciF.pCode    = (const uint32_t*)test_pattern_frag_spv;
    }
    if (be->vkCreateShaderModule(be->device, &smciV, NULL, &be->vertShader) != VK_SUCCESS ||
        be->vkCreateShaderModule(be->device, &smciF, NULL, &be->fragShader) != VK_SUCCESS) {
        LOGE("vkCreateShaderModule failed"); return -1;
    }

    // Descriptor set layout: 1 binding for the YCbCr-converted video
    // sampler. Immutable sampler is REQUIRED for VkSamplerYcbcrConversion
    // — the conversion has to be known at pipeline-compile time. If
    // ycbcrSampler isn't ready yet we fall back to an empty layout
    // (test_pattern frag), expecting a later lazy re-init.
    VkDescriptorSetLayoutBinding videoBind = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = be->ycbcrInitialized ? &be->ycbcrSampler : NULL,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = be->ycbcrInitialized ? 1u : 0u,
        .pBindings    = be->ycbcrInitialized ? &videoBind : NULL,
    };
    if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL, &be->descLayout) != VK_SUCCESS) {
        LOGE("vkCreateDescriptorSetLayout failed"); return -1;
    }

    // Descriptor pool + 1 descriptor set (only if we have a sampler binding)
    if (be->ycbcrInitialized) {
        VkDescriptorPoolSize poolSize = {
            .type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
        };
        VkDescriptorPoolCreateInfo dpci = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = 1,
            .poolSizeCount = 1,
            .pPoolSizes    = &poolSize,
        };
        if (be->vkCreateDescriptorPool(be->device, &dpci, NULL, &be->descPool) != VK_SUCCESS) {
            LOGE("vkCreateDescriptorPool failed"); return -1;
        }
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = be->descPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &be->descLayout,
        };
        if (be->vkAllocateDescriptorSets(be->device, &dsai, &be->descSet) != VK_SUCCESS) {
            LOGE("vkAllocateDescriptorSets failed"); return -1;
        }
    }
    // Push constant range for video_sample.frag's uvScale (vec2 = 8 bytes).
    // Used to crop AHB macroblock padding (e.g. 1080→1088 = scale .y 0.9926)
    // and Y-flip the AHB layout to match swapchain origin convention.
    VkPushConstantRange pcRange = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = 8,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 1,
        .pSetLayouts            = &be->descLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &pcRange,
    };
    if (be->vkCreatePipelineLayout(be->device, &plci, NULL, &be->pipelineLayout) != VK_SUCCESS) {
        LOGE("vkCreatePipelineLayout failed"); return -1;
    }

    // Graphics pipeline
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = be->vertShader, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = be->fragShader, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0,
        (float)be->swapchainExtent.width, (float)be->swapchainExtent.height,
        0.0f, 1.0f };
    VkRect2D sc = { {0, 0}, be->swapchainExtent };
    VkPipelineViewportStateCreateInfo vpst = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp,
        .scissorCount  = 1, .pScissors  = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode      = VK_POLYGON_MODE_FILL,
        .cullMode         = VK_CULL_MODE_NONE,
        .frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth        = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cbAtt = {
        .blendEnable    = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &cbAtt,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,        .pStages           = stages,
        .pVertexInputState   = &vi,      .pInputAssemblyState = &ia,
        .pViewportState      = &vpst,    .pRasterizationState = &rs,
        .pMultisampleState   = &ms,      .pColorBlendState  = &cb,
        .layout              = be->pipelineLayout,
        .renderPass          = be->renderPass,
        .subpass             = 0,
    };
    if (be->vkCreateGraphicsPipelines(be->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                       &be->graphicsPipeline) != VK_SUCCESS) {
        LOGE("vkCreateGraphicsPipelines failed"); return -1;
    }

    be->graphicsInitialized = 1;
    LOGI("graphics pipeline ready: renderPass + %u framebuffers + pipeline (frag=%s)",
         be->swapchainImageCount, be->ycbcrInitialized ? "video_sample" : "test_pattern");
    return 0;
}

static void destroy_graphics_pipeline(vk_backend_t* be)
{
    if (!be->graphicsInitialized) return;
    if (be->descPool)         be->vkDestroyDescriptorPool(be->device, be->descPool, NULL);
    be->descPool = VK_NULL_HANDLE;
    be->descSet  = VK_NULL_HANDLE;
    if (be->graphicsPipeline) be->vkDestroyPipeline(be->device, be->graphicsPipeline, NULL);
    if (be->pipelineLayout)   be->vkDestroyPipelineLayout(be->device, be->pipelineLayout, NULL);
    if (be->descLayout)       be->vkDestroyDescriptorSetLayout(be->device, be->descLayout, NULL);
    if (be->vertShader)       be->vkDestroyShaderModule(be->device, be->vertShader, NULL);
    if (be->fragShader)       be->vkDestroyShaderModule(be->device, be->fragShader, NULL);
    if (be->framebuffers) {
        for (uint32_t i = 0; i < be->swapchainImageCount; i++)
            if (be->framebuffers[i]) be->vkDestroyFramebuffer(be->device, be->framebuffers[i], NULL);
        free(be->framebuffers);
        be->framebuffers = NULL;
    }
    if (be->swapchainViews) {
        for (uint32_t i = 0; i < be->swapchainImageCount; i++)
            if (be->swapchainViews[i]) be->vkDestroyImageView(be->device, be->swapchainViews[i], NULL);
        free(be->swapchainViews);
        be->swapchainViews = NULL;
    }
    if (be->renderPass) be->vkDestroyRenderPass(be->device, be->renderPass, NULL);
    be->graphicsPipeline = VK_NULL_HANDLE;
    be->pipelineLayout   = VK_NULL_HANDLE;
    be->descLayout       = VK_NULL_HANDLE;
    be->vertShader       = VK_NULL_HANDLE;
    be->fragShader       = VK_NULL_HANDLE;
    be->renderPass       = VK_NULL_HANDLE;
    be->graphicsInitialized = 0;
}

// ============================================================
// §I.C.3.a — FRUC compute scaffolding (no dispatch yet)
// ============================================================
//
// Allocates 6 storage images (currFrameRgba / prevFrameRgba / motionField /
// prevMotionField / filteredMotionField / interpFrame), builds 4 compute
// pipelines (ycbcr_to_rgba / motionest_q1 / mv_median / warp_q1) plus
// matching descriptor sets. Per-frame dispatch is wired in §I.C.3.b;
// this phase only proves the layout/binding story compiles + the
// pipelines can be created on Adreno 620.
//
// §I.C.5 will wire Q0/Q2 variants of motionest/warp; §I.C.4 hooks
// dispatch results into the swapchain.

static int load_compute_procs(vk_backend_t* be)
{
    be->vkCreateComputePipelines     = LOAD_DEVICE_PROC(be, vkCreateComputePipelines);
    be->vkCmdDispatch                = LOAD_DEVICE_PROC(be, vkCmdDispatch);
    be->vkCmdCopyImage               = LOAD_DEVICE_PROC(be, vkCmdCopyImage);
    be->vkGetImageMemoryRequirements = LOAD_DEVICE_PROC(be, vkGetImageMemoryRequirements);
    if (!be->vkCreateComputePipelines || !be->vkCmdDispatch ||
        !be->vkCmdCopyImage || !be->vkGetImageMemoryRequirements) {
        LOGE("[VKBE-COMPUTE] load_compute_procs: missing entry points");
        return -1;
    }
    return 0;
}

// Allocate a 2D storage+sampled image of given format/extent. Memory is
// DEVICE_LOCAL. View is full-mip-level/full-layer COLOR_BIT.
static int allocate_storage_image(vk_backend_t* be,
                                   VkFormat format, uint32_t w, uint32_t h,
                                   VkImage* outImage, VkDeviceMemory* outMem,
                                   VkImageView* outView)
{
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = format,
        .extent        = { w, h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (be->vkCreateImage(be->device, &ici, NULL, outImage) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkCreateImage(%dx%d fmt=%d) failed", w, h, format);
        return -1;
    }

    VkMemoryRequirements mreq;
    be->vkGetImageMemoryRequirements(be->device, *outImage, &mreq);

    int memType = pick_memory_type(be, mreq.memoryTypeBits);
    if (memType < 0) {
        LOGE("[VKBE-COMPUTE] no compatible memory type for fmt=%d (mask=0x%x)",
             format, mreq.memoryTypeBits);
        return -1;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = mreq.size,
        .memoryTypeIndex = (uint32_t)memType,
    };
    if (be->vkAllocateMemory(be->device, &mai, NULL, outMem) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkAllocateMemory(size=%llu) failed",
             (unsigned long long)mreq.size);
        return -1;
    }
    if (be->vkBindImageMemory(be->device, *outImage, *outMem, 0) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkBindImageMemory failed");
        return -1;
    }

    VkImageViewCreateInfo ivci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = *outImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format,
        .components = {
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        },
    };
    if (be->vkCreateImageView(be->device, &ivci, NULL, outView) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkCreateImageView failed");
        return -1;
    }
    return 0;
}

static int init_compute_pipelines(vk_backend_t* be)
{
    if (be->fInitialized) return 0;
    if (!be->ycbcrInitialized) {
        LOGW("[VKBE-COMPUTE] init called before ycbcr sampler is ready; skipping");
        return -1;
    }
    if (load_compute_procs(be) != 0) return -1;

    // Block size constant — must match the .comp shaders + GLES path.
    const uint32_t BLOCK_SIZE = 64;
    uint32_t W   = (uint32_t)be->videoWidth;
    uint32_t H   = (uint32_t)be->videoHeight;
    uint32_t mvW = (W + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint32_t mvH = (H + BLOCK_SIZE - 1) / BLOCK_SIZE;

    // 1. Storage images. (currFrameRgba / prevFrameRgba / interpFrame are
    //    rgba8 at video size; the 3 motion fields are r32i at mv size.)
    if (allocate_storage_image(be, VK_FORMAT_R8G8B8A8_UNORM, W, H,
                                &be->fImage[FRUC_IMG_CURR], &be->fImageMem[FRUC_IMG_CURR], &be->fImageView[FRUC_IMG_CURR]) != 0)
        return -1;
    if (allocate_storage_image(be, VK_FORMAT_R8G8B8A8_UNORM, W, H,
                                &be->fImage[FRUC_IMG_PREV], &be->fImageMem[FRUC_IMG_PREV], &be->fImageView[FRUC_IMG_PREV]) != 0)
        return -1;
    if (allocate_storage_image(be, VK_FORMAT_R32_SINT, mvW, mvH,
                                &be->fImage[FRUC_IMG_MV], &be->fImageMem[FRUC_IMG_MV], &be->fImageView[FRUC_IMG_MV]) != 0)
        return -1;
    if (allocate_storage_image(be, VK_FORMAT_R32_SINT, mvW, mvH,
                                &be->fImage[FRUC_IMG_PREV_MV], &be->fImageMem[FRUC_IMG_PREV_MV], &be->fImageView[FRUC_IMG_PREV_MV]) != 0)
        return -1;
    if (allocate_storage_image(be, VK_FORMAT_R32_SINT, mvW, mvH,
                                &be->fImage[FRUC_IMG_FILT_MV], &be->fImageMem[FRUC_IMG_FILT_MV], &be->fImageView[FRUC_IMG_FILT_MV]) != 0)
        return -1;
    if (allocate_storage_image(be, VK_FORMAT_R8G8B8A8_UNORM, W, H,
                                &be->fImage[FRUC_IMG_INTERP], &be->fImageMem[FRUC_IMG_INTERP], &be->fImageView[FRUC_IMG_INTERP]) != 0)
        return -1;

    // 2. Linear sampler for the rgba8 / r32i storage-image bindings on the
    //    ME / mv_median / warp pipelines. CLAMP_TO_EDGE so warp's
    //    sampleMV bilinear at the frame border doesn't pull in garbage.
    VkSamplerCreateInfo sci = {
        .sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter     = VK_FILTER_LINEAR,
        .minFilter     = VK_FILTER_LINEAR,
        .mipmapMode    = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (be->vkCreateSampler(be->device, &sci, NULL, &be->fLinearSampler) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkCreateSampler(linear) failed");
        return -1;
    }

    // 3. Shader modules (4 pipelines × 1 module each).
    struct { const void* code; size_t size; const char* name; } shaders[FRUC_NUM_PIPELINES] = {
        [FRUC_PIPE_YCBCR]     = { ycbcr_to_rgba_comp_spv,        ycbcr_to_rgba_comp_spv_len,        "ycbcr_to_rgba" },
        [FRUC_PIPE_ME]        = { motionest_compute_q1_comp_spv, motionest_compute_q1_comp_spv_len, "motionest_q1" },
        [FRUC_PIPE_MV_MEDIAN] = { mv_median_comp_spv,            mv_median_comp_spv_len,            "mv_median" },
        [FRUC_PIPE_WARP]      = { warp_compute_q1_comp_spv,      warp_compute_q1_comp_spv_len,      "warp_q1" },
    };
    for (int i = 0; i < FRUC_NUM_PIPELINES; i++) {
        VkShaderModuleCreateInfo smci = {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shaders[i].size,
            .pCode    = (const uint32_t*)shaders[i].code,
        };
        if (be->vkCreateShaderModule(be->device, &smci, NULL, &be->fShader[i]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] vkCreateShaderModule(%s) failed", shaders[i].name);
            return -1;
        }
    }

    // 4. Descriptor set layouts (per docs/vulkan_fruc_port.md §4).
    //    Sampler bindings use immutable samplers — ycbcr binding takes the
    //    YCbCr conversion sampler (mandatory for the conversion to work),
    //    everything else takes fLinearSampler.

    // 4a. ycbcr_to_rgba: combined image sampler (binding 0, ycbcr) + storage image (binding 1)
    {
        VkDescriptorSetLayoutBinding b[2] = {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->ycbcrSampler },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = b,
        };
        if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL,
                                             &be->fDescLayout[FRUC_PIPE_YCBCR]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] descset_layout[ycbcr] failed");
            return -1;
        }
    }

    // 4b. motionest: 3 combined image samplers (prev/curr/prevMV) + 1 storage image (mv)
    {
        VkDescriptorSetLayoutBinding b[4] = {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4, .pBindings = b,
        };
        if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL,
                                             &be->fDescLayout[FRUC_PIPE_ME]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] descset_layout[me] failed");
            return -1;
        }
    }

    // 4c. mv_median: 1 isampler + 1 storage image
    {
        VkDescriptorSetLayoutBinding b[2] = {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = b,
        };
        if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL,
                                             &be->fDescLayout[FRUC_PIPE_MV_MEDIAN]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] descset_layout[mv_median] failed");
            return -1;
        }
    }

    // 4d. warp: 3 combined image samplers + 1 storage image (interp)
    {
        VkDescriptorSetLayoutBinding b[4] = {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
              .pImmutableSamplers = &be->fLinearSampler },
            { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4, .pBindings = b,
        };
        if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL,
                                             &be->fDescLayout[FRUC_PIPE_WARP]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] descset_layout[warp] failed");
            return -1;
        }
    }

    // 5. Pipeline layouts. Push constant block ≤ 16 bytes (well under
    //    Vulkan 1.0's 128-byte minimum guarantee).
    static const uint32_t PC_SIZE[FRUC_NUM_PIPELINES] = {
        [FRUC_PIPE_YCBCR]     = 8,   // uvec2 dims
        [FRUC_PIPE_ME]        = 12,  // uvec3 (frameW, frameH, blockSize)
        [FRUC_PIPE_MV_MEDIAN] = 8,   // uvec2 (mvW, mvH)
        [FRUC_PIPE_WARP]      = 16,  // uvec3 + float blendFactor
    };
    for (int i = 0; i < FRUC_NUM_PIPELINES; i++) {
        VkPushConstantRange pcRange = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0,
            .size       = PC_SIZE[i],
        };
        VkPipelineLayoutCreateInfo plci = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &be->fDescLayout[i],
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcRange,
        };
        if (be->vkCreatePipelineLayout(be->device, &plci, NULL, &be->fPipeLayout[i]) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] vkCreatePipelineLayout[%d] failed", i);
            return -1;
        }
    }

    // 6. Compute pipelines (one vkCreateComputePipelines call for all 4).
    VkComputePipelineCreateInfo cpci[FRUC_NUM_PIPELINES];
    memset(cpci, 0, sizeof(cpci));
    for (int i = 0; i < FRUC_NUM_PIPELINES; i++) {
        cpci[i].sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci[i].stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci[i].stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci[i].stage.module = be->fShader[i];
        cpci[i].stage.pName  = "main";
        cpci[i].layout       = be->fPipeLayout[i];
    }
    if (be->vkCreateComputePipelines(be->device, VK_NULL_HANDLE,
                                      FRUC_NUM_PIPELINES, cpci, NULL,
                                      be->fPipeline) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkCreateComputePipelines failed");
        return -1;
    }

    // 6.5. §I.C.5.a quality-preset variants: build extra Q0 / Q2 modules
    //      and pipelines for ME and warp. Q1 entries alias the already-
    //      created shader/pipeline so dispatch_fruc can index uniformly.
    //      Layouts (descset + pipeline) are shared across Q0/Q1/Q2 by
    //      docs/vulkan_fruc_port.md §4 design.
    {
        struct { const void* code; size_t size; } variants[3][2] = {
            // [quality][0=me, 1=warp]
            { { motionest_compute_q0_comp_spv, motionest_compute_q0_comp_spv_len },
              { warp_compute_q0_comp_spv,      warp_compute_q0_comp_spv_len      } },
            { { motionest_compute_q1_comp_spv, motionest_compute_q1_comp_spv_len },
              { warp_compute_q1_comp_spv,      warp_compute_q1_comp_spv_len      } },
            { { motionest_compute_q2_comp_spv, motionest_compute_q2_comp_spv_len },
              { warp_compute_q2_comp_spv,      warp_compute_q2_comp_spv_len      } },
        };

        for (int q = 0; q < 3; q++) {
            if (q == 1) {
                // Q1 already built — alias to the original me/warp shader/pipeline.
                be->fMeShaderQ[1]    = be->fShader[FRUC_PIPE_ME];
                be->fWarpShaderQ[1]  = be->fShader[FRUC_PIPE_WARP];
                be->fMePipelineQ[1]  = be->fPipeline[FRUC_PIPE_ME];
                be->fWarpPipelineQ[1] = be->fPipeline[FRUC_PIPE_WARP];
                continue;
            }
            VkShaderModuleCreateInfo smciMe = {
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = variants[q][0].size,
                .pCode    = (const uint32_t*)variants[q][0].code,
            };
            if (be->vkCreateShaderModule(be->device, &smciMe, NULL, &be->fMeShaderQ[q]) != VK_SUCCESS) {
                LOGE("[VKBE-COMPUTE] vkCreateShaderModule(me_q%d) failed", q);
                return -1;
            }
            VkShaderModuleCreateInfo smciWarp = {
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = variants[q][1].size,
                .pCode    = (const uint32_t*)variants[q][1].code,
            };
            if (be->vkCreateShaderModule(be->device, &smciWarp, NULL, &be->fWarpShaderQ[q]) != VK_SUCCESS) {
                LOGE("[VKBE-COMPUTE] vkCreateShaderModule(warp_q%d) failed", q);
                return -1;
            }
        }

        // Build Q0 + Q2 pipelines (4 extra) using shared ME / warp layouts.
        VkComputePipelineCreateInfo cpciExtra[4];
        memset(cpciExtra, 0, sizeof(cpciExtra));
        // [0]=me_q0, [1]=warp_q0, [2]=me_q2, [3]=warp_q2
        const VkShaderModule modules[4] = {
            be->fMeShaderQ[0],   be->fWarpShaderQ[0],
            be->fMeShaderQ[2],   be->fWarpShaderQ[2],
        };
        const VkPipelineLayout layouts[4] = {
            be->fPipeLayout[FRUC_PIPE_ME],   be->fPipeLayout[FRUC_PIPE_WARP],
            be->fPipeLayout[FRUC_PIPE_ME],   be->fPipeLayout[FRUC_PIPE_WARP],
        };
        for (int i = 0; i < 4; i++) {
            cpciExtra[i].sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpciExtra[i].stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpciExtra[i].stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            cpciExtra[i].stage.module = modules[i];
            cpciExtra[i].stage.pName  = "main";
            cpciExtra[i].layout       = layouts[i];
        }
        VkPipeline outPipes[4];
        if (be->vkCreateComputePipelines(be->device, VK_NULL_HANDLE,
                                          4, cpciExtra, NULL, outPipes) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] vkCreateComputePipelines(Q0+Q2) failed");
            return -1;
        }
        be->fMePipelineQ[0]   = outPipes[0];
        be->fWarpPipelineQ[0] = outPipes[1];
        be->fMePipelineQ[2]   = outPipes[2];
        be->fWarpPipelineQ[2] = outPipes[3];
        be->fQualityLevel = 1;  // default Balanced
    }

    // 7. Descriptor pool + 4 compute sets + 1 interp graphics set (§I.C.4.a).
    //    Compute: ycbcr(1+1) + me(3+1) + mv_median(1+1) + warp(3+1) = 8 sampler + 4 storage.
    //    Interp graphics: 1 sampler.
    //    Total: 9 sampler + 4 storage, 5 sets. Pad sampler to 10 for slack.
    VkDescriptorPoolSize poolSizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 10 },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           .descriptorCount = 4 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets       = FRUC_NUM_PIPELINES + 1,
        .poolSizeCount = 2,
        .pPoolSizes    = poolSizes,
    };
    if (be->vkCreateDescriptorPool(be->device, &dpci, NULL, &be->fDescPool) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkCreateDescriptorPool failed");
        return -1;
    }
    VkDescriptorSetAllocateInfo dsai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = be->fDescPool,
        .descriptorSetCount = FRUC_NUM_PIPELINES,
        .pSetLayouts        = be->fDescLayout,
    };
    if (be->vkAllocateDescriptorSets(be->device, &dsai, be->fDescSet) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] vkAllocateDescriptorSets failed");
        return -1;
    }

    // 8. Static descriptor writes. Everything except ycbcr binding 0
    //    (the per-frame AHB view) is bound here once and reused. Layout
    //    convention: ALL fImages stay in VK_IMAGE_LAYOUT_GENERAL — same
    //    layout works for storage write, sampled read, and transfer
    //    src/dst, so dispatch_fruc only needs memory barriers, not
    //    layout transitions.
    VkDescriptorImageInfo iiCurr   = { .imageView = be->fImageView[FRUC_IMG_CURR],     .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo iiPrev   = { .imageView = be->fImageView[FRUC_IMG_PREV],     .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo iiMv     = { .imageView = be->fImageView[FRUC_IMG_MV],       .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo iiPrevMv = { .imageView = be->fImageView[FRUC_IMG_PREV_MV],  .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo iiFiltMv = { .imageView = be->fImageView[FRUC_IMG_FILT_MV],  .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo iiInterp = { .imageView = be->fImageView[FRUC_IMG_INTERP],   .imageLayout = VK_IMAGE_LAYOUT_GENERAL };

#define FRUC_W(setIdx, bind, type, info)                              \
    {                                                                  \
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,     \
        .dstSet          = be->fDescSet[setIdx],                       \
        .dstBinding      = (bind),                                     \
        .descriptorCount = 1,                                          \
        .descriptorType  = (type),                                     \
        .pImageInfo      = (info),                                     \
    }
    VkWriteDescriptorSet writes[] = {
        // ycbcr_to_rgba: binding 1 = currFrameRgba (storage). binding 0 = AHB view (per-frame, set in dispatch_fruc).
        FRUC_W(FRUC_PIPE_YCBCR,     1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           &iiCurr),

        // motionest: 0=prev (sampled), 1=curr (sampled), 2=prev_mv (sampled), 3=mv (storage).
        FRUC_W(FRUC_PIPE_ME,        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiPrev),
        FRUC_W(FRUC_PIPE_ME,        1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiCurr),
        FRUC_W(FRUC_PIPE_ME,        2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiPrevMv),
        FRUC_W(FRUC_PIPE_ME,        3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           &iiMv),

        // mv_median: 0=mv (sampled), 1=filt_mv (storage).
        FRUC_W(FRUC_PIPE_MV_MEDIAN, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiMv),
        FRUC_W(FRUC_PIPE_MV_MEDIAN, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           &iiFiltMv),

        // warp: 0=prev (sampled), 1=curr (sampled), 2=filt_mv (sampled), 3=interp (storage).
        FRUC_W(FRUC_PIPE_WARP,      0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiPrev),
        FRUC_W(FRUC_PIPE_WARP,      1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiCurr),
        FRUC_W(FRUC_PIPE_WARP,      2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  &iiFiltMv),
        FRUC_W(FRUC_PIPE_WARP,      3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,           &iiInterp),
    };
#undef FRUC_W
    be->vkUpdateDescriptorSets(be->device, (uint32_t)(sizeof(writes)/sizeof(writes[0])),
                               writes, 0, NULL);

    // 9. One-shot init pass: transition all 6 images UNDEFINED → GENERAL,
    //    clear prevFrameRgba and prevMotionField to 0 so the very first
    //    frame's motionest reads sane values. Reuses be->cmdBuffer (not
    //    yet submitted on this first frame) + waits idle before returning.
    VkImageSubresourceRange fullColorRange = {
        .aspectMask   = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    be->vkResetCommandBuffer(be->cmdBuffer, 0);
    VkCommandBufferBeginInfo bbi_init = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (be->vkBeginCommandBuffer(be->cmdBuffer, &bbi_init) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] init clear: vkBeginCommandBuffer failed");
        return -1;
    }
    VkImageMemoryBarrier toGeneral[FRUC_NUM_IMAGES];
    for (int i = 0; i < FRUC_NUM_IMAGES; i++) {
        toGeneral[i] = (VkImageMemoryBarrier){
            .sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask        = 0,
            .dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout            = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED,
            .image                = be->fImage[i],
            .subresourceRange     = fullColorRange,
        };
    }
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, FRUC_NUM_IMAGES, toGeneral);

    VkClearColorValue zeroRgba = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkClearColorValue zeroInt  = { .int32   = { 0, 0, 0, 0 } };
    be->vkCmdClearColorImage(be->cmdBuffer, be->fImage[FRUC_IMG_PREV],
                             VK_IMAGE_LAYOUT_GENERAL, &zeroRgba, 1, &fullColorRange);
    be->vkCmdClearColorImage(be->cmdBuffer, be->fImage[FRUC_IMG_PREV_MV],
                             VK_IMAGE_LAYOUT_GENERAL, &zeroInt,  1, &fullColorRange);

    VkMemoryBarrier mb_init = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb_init, 0, NULL, 0, NULL);

    if (be->vkEndCommandBuffer(be->cmdBuffer) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] init clear: vkEndCommandBuffer failed");
        return -1;
    }
    VkSubmitInfo si_init = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &be->cmdBuffer,
    };
    if (be->vkQueueSubmit(be->graphicsQueue, 1, &si_init, VK_NULL_HANDLE) != VK_SUCCESS) {
        LOGE("[VKBE-COMPUTE] init clear: vkQueueSubmit failed");
        return -1;
    }
    be->vkQueueWaitIdle(be->graphicsQueue);

    // 10. §I.C.4.a — second graphics pipeline that samples interpFrame.
    //     Same renderPass + fullscreen.vert + video_sample.frag as the
    //     existing graphics pipeline; only the descriptor layout differs
    //     (immutable sampler is fLinearSampler instead of ycbcrSampler).
    {
        VkDescriptorSetLayoutBinding b = {
            .binding         = 0,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = &be->fLinearSampler,
        };
        VkDescriptorSetLayoutCreateInfo dslci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &b,
        };
        if (be->vkCreateDescriptorSetLayout(be->device, &dslci, NULL,
                                             &be->fInterpDescLayout) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] interp graphics: descset_layout failed");
            return -1;
        }
    }
    {
        VkDescriptorSetAllocateInfo dsai = {
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = be->fDescPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &be->fInterpDescLayout,
        };
        if (be->vkAllocateDescriptorSets(be->device, &dsai, &be->fInterpDescSet) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] interp graphics: AllocateDescriptorSets failed");
            return -1;
        }
        VkDescriptorImageInfo iiInterpDisplay = {
            .sampler     = VK_NULL_HANDLE,
            .imageView   = be->fImageView[FRUC_IMG_INTERP],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkWriteDescriptorSet wdsInterp = {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = be->fInterpDescSet,
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo      = &iiInterpDisplay,
        };
        be->vkUpdateDescriptorSets(be->device, 1, &wdsInterp, 0, NULL);
    }
    {
        // Same uvScale push constant as video_sample.frag (FRAGMENT, 8 bytes).
        VkPushConstantRange pcRange = {
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset     = 0,
            .size       = 8,
        };
        VkPipelineLayoutCreateInfo plci = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &be->fInterpDescLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcRange,
        };
        if (be->vkCreatePipelineLayout(be->device, &plci, NULL, &be->fInterpPipeLayout) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] interp graphics: vkCreatePipelineLayout failed");
            return -1;
        }
    }
    {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = be->vertShader, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = be->fragShader, .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkViewport vp = { 0, 0,
            (float)be->swapchainExtent.width, (float)be->swapchainExtent.height,
            0.0f, 1.0f };
        VkRect2D sc = { {0, 0}, be->swapchainExtent };
        VkPipelineViewportStateCreateInfo vpst = {
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .pViewports = &vp,
            .scissorCount  = 1, .pScissors  = &sc,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode      = VK_POLYGON_MODE_FILL,
            .cullMode         = VK_CULL_MODE_NONE,
            .frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth        = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState cbAtt = {
            .blendEnable    = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments    = &cbAtt,
        };
        VkGraphicsPipelineCreateInfo gpci = {
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount          = 2,        .pStages           = stages,
            .pVertexInputState   = &vi,      .pInputAssemblyState = &ia,
            .pViewportState      = &vpst,    .pRasterizationState = &rs,
            .pMultisampleState   = &ms,      .pColorBlendState  = &cb,
            .layout              = be->fInterpPipeLayout,
            .renderPass          = be->renderPass,
            .subpass             = 0,
        };
        if (be->vkCreateGraphicsPipelines(be->device, VK_NULL_HANDLE, 1, &gpci, NULL,
                                           &be->fInterpPipeline) != VK_SUCCESS) {
            LOGE("[VKBE-COMPUTE] interp graphics: vkCreateGraphicsPipelines failed");
            return -1;
        }
    }

    // §I.D — bring up the in-flight ring (2-slot cmdbuf + sems + fence per
    // slot) for dual-present path. Failure is fatal here because dispatch
    // path now relies on the ring; fall-back caller will see fInitialized=0
    // staying as-is and stay on single-present graphics pipeline.
    if (init_in_flight_ring(be) != 0) {
        LOGE("[VKBE-COMPUTE] in-flight ring init failed; compute path disabled");
        return -1;
    }

    LOGI("[VKBE-COMPUTE] init done: 6 storage images (W=%u H=%u, mvW=%u mvH=%u), "
         "4 pipelines (ycbcr/motionest_q1/mv_median/warp_q1), 4 descsets, "
         "static bindings written, prev images cleared, interp graphics pipeline ready, "
         "in-flight ring ready",
         W, H, mvW, mvH);
    be->fInitialized = 1;
    return 0;
}

static int init_in_flight_ring(vk_backend_t* be)
{
    if (be->fRingInitialized) return 0;

    // Load fence procs (other per-frame procs already loaded by load_render_procs).
    be->vkCreateFence    = LOAD_DEVICE_PROC(be, vkCreateFence);
    be->vkDestroyFence   = LOAD_DEVICE_PROC(be, vkDestroyFence);
    be->vkWaitForFences  = LOAD_DEVICE_PROC(be, vkWaitForFences);
    be->vkResetFences    = LOAD_DEVICE_PROC(be, vkResetFences);
    if (!be->vkCreateFence || !be->vkDestroyFence ||
        !be->vkWaitForFences || !be->vkResetFences) {
        LOGE("[VKBE-RING] fence PFNs missing");
        return -1;
    }

    // Allocate VK_FRAMES_IN_FLIGHT cmdbuffers from the existing pool.
    VkCommandBufferAllocateInfo cbai = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = be->cmdPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_FRAMES_IN_FLIGHT,
    };
    if (be->vkAllocateCommandBuffers(be->device, &cbai, be->fSlotCmdBuf) != VK_SUCCESS) {
        LOGE("[VKBE-RING] vkAllocateCommandBuffers failed");
        return -1;
    }

    // Per slot: 2 acquireSems (one per pass) + 2 renderDoneSems + 1 fence
    // (signaled-initial so first frame's vkWaitForFences returns immediately).
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo     fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        for (int p = 0; p < 2; p++) {
            if (be->vkCreateSemaphore(be->device, &sci, NULL,
                                       &be->fSlotAcquireSem[i][p]) != VK_SUCCESS ||
                be->vkCreateSemaphore(be->device, &sci, NULL,
                                       &be->fSlotRenderDoneSem[i][p]) != VK_SUCCESS) {
                LOGE("[VKBE-RING] vkCreateSemaphore[%u][%d] failed", i, p);
                return -1;
            }
        }
        if (be->vkCreateFence(be->device, &fci, NULL, &be->fSlotInFlightFence[i]) != VK_SUCCESS) {
            LOGE("[VKBE-RING] vkCreateFence[%u] failed", i);
            return -1;
        }
    }

    be->fCurrentSlot = 0;
    be->fRingInitialized = 1;
    LOGI("[VKBE-RING] in-flight ring ready: %d slots × (1 cmdbuf + 2 acquireSem + 2 renderDoneSem + 1 fence)",
         VK_FRAMES_IN_FLIGHT);
    return 0;
}

static void destroy_in_flight_ring(vk_backend_t* be)
{
    if (!be->fRingInitialized) return;

    for (uint32_t i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        // Free pending AHB resources still tracked in this slot.
        if (be->fSlotHasPending[i]) {
            if (be->fSlotPendingView[i]) be->vkDestroyImageView(be->device, be->fSlotPendingView[i], NULL);
            if (be->fSlotPendingImg[i])  be->vkDestroyImage(be->device, be->fSlotPendingImg[i], NULL);
            if (be->fSlotPendingMem[i])  be->vkFreeMemory(be->device, be->fSlotPendingMem[i], NULL);
            be->fSlotPendingView[i] = VK_NULL_HANDLE;
            be->fSlotPendingImg[i]  = VK_NULL_HANDLE;
            be->fSlotPendingMem[i]  = VK_NULL_HANDLE;
            be->fSlotHasPending[i]  = 0;
        }
        if (be->fSlotInFlightFence[i]) {
            be->vkDestroyFence(be->device, be->fSlotInFlightFence[i], NULL);
            be->fSlotInFlightFence[i] = VK_NULL_HANDLE;
        }
        for (int p = 0; p < 2; p++) {
            if (be->fSlotAcquireSem[i][p]) {
                be->vkDestroySemaphore(be->device, be->fSlotAcquireSem[i][p], NULL);
                be->fSlotAcquireSem[i][p] = VK_NULL_HANDLE;
            }
            if (be->fSlotRenderDoneSem[i][p]) {
                be->vkDestroySemaphore(be->device, be->fSlotRenderDoneSem[i][p], NULL);
                be->fSlotRenderDoneSem[i][p] = VK_NULL_HANDLE;
            }
        }
        // cmdbufs are freed implicitly when cmdPool is destroyed.
        be->fSlotCmdBuf[i] = VK_NULL_HANDLE;
    }
    be->fRingInitialized = 0;
    LOGI("[VKBE-RING] destroyed");
}

static void destroy_compute_pipelines(vk_backend_t* be)
{
    if (!be->fInitialized) return;

    // §I.D — drain ring first so any in-flight GPU work using compute
    // resources finishes before we tear them down.
    destroy_in_flight_ring(be);

    // §I.C.4.a interp graphics pipeline first (descSet was allocated from
    // fDescPool which we destroy below; pipeline / layouts are independent).
    if (be->fInterpPipeline)    be->vkDestroyPipeline(be->device, be->fInterpPipeline, NULL);
    if (be->fInterpPipeLayout)  be->vkDestroyPipelineLayout(be->device, be->fInterpPipeLayout, NULL);
    if (be->fInterpDescLayout)  be->vkDestroyDescriptorSetLayout(be->device, be->fInterpDescLayout, NULL);
    be->fInterpPipeline   = VK_NULL_HANDLE;
    be->fInterpPipeLayout = VK_NULL_HANDLE;
    be->fInterpDescLayout = VK_NULL_HANDLE;
    be->fInterpDescSet    = VK_NULL_HANDLE;

    if (be->fDescPool) {
        be->vkDestroyDescriptorPool(be->device, be->fDescPool, NULL);
        be->fDescPool = VK_NULL_HANDLE;
    }

    // §I.C.5.a Q0/Q2 variants destroyed first (Q1 entries are aliases of
    // fShader[FRUC_PIPE_ME]/fPipeline[FRUC_PIPE_ME] etc, freed below).
    for (int q = 0; q < 3; q++) {
        if (q == 1) continue;  // alias, freed via fShader[]/fPipeline[]
        if (be->fMePipelineQ[q])   be->vkDestroyPipeline(be->device, be->fMePipelineQ[q], NULL);
        if (be->fWarpPipelineQ[q]) be->vkDestroyPipeline(be->device, be->fWarpPipelineQ[q], NULL);
        if (be->fMeShaderQ[q])     be->vkDestroyShaderModule(be->device, be->fMeShaderQ[q], NULL);
        if (be->fWarpShaderQ[q])   be->vkDestroyShaderModule(be->device, be->fWarpShaderQ[q], NULL);
        be->fMePipelineQ[q]   = VK_NULL_HANDLE;
        be->fWarpPipelineQ[q] = VK_NULL_HANDLE;
        be->fMeShaderQ[q]     = VK_NULL_HANDLE;
        be->fWarpShaderQ[q]   = VK_NULL_HANDLE;
    }
    // Clear the Q1 alias slots so they don't dangle (the actual handles
    // are freed via fPipeline[FRUC_PIPE_ME] / fShader[FRUC_PIPE_ME] below).
    be->fMePipelineQ[1]   = VK_NULL_HANDLE;
    be->fWarpPipelineQ[1] = VK_NULL_HANDLE;
    be->fMeShaderQ[1]     = VK_NULL_HANDLE;
    be->fWarpShaderQ[1]   = VK_NULL_HANDLE;

    for (int i = 0; i < FRUC_NUM_PIPELINES; i++) {
        if (be->fPipeline[i])   be->vkDestroyPipeline(be->device, be->fPipeline[i], NULL);
        if (be->fPipeLayout[i]) be->vkDestroyPipelineLayout(be->device, be->fPipeLayout[i], NULL);
        if (be->fDescLayout[i]) be->vkDestroyDescriptorSetLayout(be->device, be->fDescLayout[i], NULL);
        if (be->fShader[i])     be->vkDestroyShaderModule(be->device, be->fShader[i], NULL);
        be->fPipeline[i]   = VK_NULL_HANDLE;
        be->fPipeLayout[i] = VK_NULL_HANDLE;
        be->fDescLayout[i] = VK_NULL_HANDLE;
        be->fShader[i]     = VK_NULL_HANDLE;
        be->fDescSet[i]    = VK_NULL_HANDLE;
    }
    if (be->fLinearSampler) {
        be->vkDestroySampler(be->device, be->fLinearSampler, NULL);
        be->fLinearSampler = VK_NULL_HANDLE;
    }
    for (int i = 0; i < FRUC_NUM_IMAGES; i++) {
        if (be->fImageView[i]) be->vkDestroyImageView(be->device, be->fImageView[i], NULL);
        if (be->fImage[i])     be->vkDestroyImage(be->device, be->fImage[i], NULL);
        if (be->fImageMem[i])  be->vkFreeMemory(be->device, be->fImageMem[i], NULL);
        be->fImageView[i] = VK_NULL_HANDLE;
        be->fImage[i]     = VK_NULL_HANDLE;
        be->fImageMem[i]  = VK_NULL_HANDLE;
    }
    be->fInitialized = 0;
    LOGI("[VKBE-COMPUTE] destroyed");
}

// ============================================================
// §I.C.3.b — FRUC compute dispatch (still not wired to swapchain)
// ============================================================
//
// Records 4 compute dispatches + prev-rotate copies into the per-frame
// command buffer (caller has already begun it and applied the AHB
// inAcquire barrier). The graphics render pass that follows is
// untouched — it still draws the AHB image directly to swapchain, so
// interpFrame is computed but **not displayed**. §I.C.4 wires it in.

static int dispatch_fruc(vk_backend_t* be, VkImageView ahbView)
{
    if (!be->fInitialized) return 0;

    // 1. Per-frame: rebind YCBCR set's binding 0 to the just-imported AHB
    //    view. Sampler is immutable in the layout (the ycbcr conversion
    //    sampler), so we only fill imageView + layout.
    VkDescriptorImageInfo dii = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = ahbView,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wds = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = be->fDescSet[FRUC_PIPE_YCBCR],
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &dii,
    };
    be->vkUpdateDescriptorSets(be->device, 1, &wds, 0, NULL);

    const uint32_t W      = (uint32_t)be->videoWidth;
    const uint32_t H      = (uint32_t)be->videoHeight;
    const uint32_t BLOCK  = 64;
    const uint32_t mvW    = (W + BLOCK - 1) / BLOCK;
    const uint32_t mvH    = (H + BLOCK - 1) / BLOCK;
    const uint32_t gW     = (W + 7) / 8;
    const uint32_t gH     = (H + 7) / 8;
    const uint32_t gMvW   = (mvW + 7) / 8;
    const uint32_t gMvH   = (mvH + 7) / 8;

    // Memory barrier to insert between adjacent compute dispatches.
    VkMemoryBarrier mb_compute = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };

    // ---- Dispatch 1: ycbcr_to_rgba (AHB → currFrameRgba) ----
    be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          be->fPipeline[FRUC_PIPE_YCBCR]);
    be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                be->fPipeLayout[FRUC_PIPE_YCBCR], 0, 1,
                                &be->fDescSet[FRUC_PIPE_YCBCR], 0, NULL);
    {
        uint32_t pc[2] = { W, H };
        be->vkCmdPushConstants(be->cmdBuffer, be->fPipeLayout[FRUC_PIPE_YCBCR],
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    }
    be->vkCmdDispatch(be->cmdBuffer, gW, gH, 1);
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb_compute, 0, NULL, 0, NULL);

    // ---- Dispatch 2: motionest (prev+curr+prev_mv → mv) ----
    // §I.C.5.a: pick Q variant; default Q1.
    int qIdx = (be->fQualityLevel >= 0 && be->fQualityLevel <= 2) ? be->fQualityLevel : 1;
    be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          be->fMePipelineQ[qIdx]);
    be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                be->fPipeLayout[FRUC_PIPE_ME], 0, 1,
                                &be->fDescSet[FRUC_PIPE_ME], 0, NULL);
    {
        uint32_t pc[3] = { W, H, BLOCK };
        be->vkCmdPushConstants(be->cmdBuffer, be->fPipeLayout[FRUC_PIPE_ME],
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    }
    be->vkCmdDispatch(be->cmdBuffer, gMvW, gMvH, 1);
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb_compute, 0, NULL, 0, NULL);

    // ---- Dispatch 3: mv_median (mv → filt_mv) ----
    be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          be->fPipeline[FRUC_PIPE_MV_MEDIAN]);
    be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                be->fPipeLayout[FRUC_PIPE_MV_MEDIAN], 0, 1,
                                &be->fDescSet[FRUC_PIPE_MV_MEDIAN], 0, NULL);
    {
        uint32_t pc[2] = { mvW, mvH };
        be->vkCmdPushConstants(be->cmdBuffer, be->fPipeLayout[FRUC_PIPE_MV_MEDIAN],
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    }
    be->vkCmdDispatch(be->cmdBuffer, gMvW, gMvH, 1);
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb_compute, 0, NULL, 0, NULL);

    // ---- Dispatch 4: warp (prev+curr+filt_mv → interp) ----
    // §I.C.5.a: pick Q variant matching the ME variant.
    be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          be->fWarpPipelineQ[qIdx]);
    be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                be->fPipeLayout[FRUC_PIPE_WARP], 0, 1,
                                &be->fDescSet[FRUC_PIPE_WARP], 0, NULL);
    {
        // Layout: uvec3 (4+4+4) then float (4) = 16 bytes total.
        uint32_t pc[4];
        pc[0] = W; pc[1] = H; pc[2] = BLOCK;
        float blend = 0.5f;
        memcpy(&pc[3], &blend, sizeof(blend));
        be->vkCmdPushConstants(be->cmdBuffer, be->fPipeLayout[FRUC_PIPE_WARP],
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    }
    be->vkCmdDispatch(be->cmdBuffer, gW, gH, 1);

    // ---- Prev rotate: currFrameRgba → prevFrameRgba, motionField → prevMotionField. ----
    // Compute writes/reads must complete before transfer reads/writes.
    VkMemoryBarrier mb_compute_to_xfer = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &mb_compute_to_xfer, 0, NULL, 0, NULL);

    VkImageCopy copyRgba = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                            .baseArrayLayer = 0, .layerCount = 1 },
        .srcOffset      = { 0, 0, 0 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                            .baseArrayLayer = 0, .layerCount = 1 },
        .dstOffset      = { 0, 0, 0 },
        .extent         = { W, H, 1 },
    };
    be->vkCmdCopyImage(be->cmdBuffer,
        be->fImage[FRUC_IMG_CURR], VK_IMAGE_LAYOUT_GENERAL,
        be->fImage[FRUC_IMG_PREV], VK_IMAGE_LAYOUT_GENERAL,
        1, &copyRgba);

    VkImageCopy copyMv = {
        .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                            .baseArrayLayer = 0, .layerCount = 1 },
        .srcOffset      = { 0, 0, 0 },
        .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0,
                            .baseArrayLayer = 0, .layerCount = 1 },
        .dstOffset      = { 0, 0, 0 },
        .extent         = { mvW, mvH, 1 },
    };
    be->vkCmdCopyImage(be->cmdBuffer,
        be->fImage[FRUC_IMG_MV],      VK_IMAGE_LAYOUT_GENERAL,
        be->fImage[FRUC_IMG_PREV_MV], VK_IMAGE_LAYOUT_GENERAL,
        1, &copyMv);

    // Final barrier: rotated prev images must be visible to next frame's compute.
    VkMemoryBarrier mb_xfer_to_compute = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    be->vkCmdPipelineBarrier(be->cmdBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &mb_xfer_to_compute, 0, NULL, 0, NULL);

    if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
        LOGI("[VKBE-COMPUTE] dispatch frame #%d: ycbcr+motionest_q%d+mv_median+warp_q%d + prev rotate",
             be->frameCounter, qIdx, qIdx);
    }
    return 0;
}

// §I.C.6 — monotonic ns clock for VK_GOOGLE_display_timing PTS values.
// Pixel 5 NDK 27+ ships clock_gettime(CLOCK_MONOTONIC) — same clock the
// driver uses to interpret VkPresentTimeGOOGLE.desiredPresentTime.
static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// ---------- B.2c.3a: per-frame steady-state clear+present ----------

static int render_clear_frame(vk_backend_t* be)
{
    if (!be->renderInitialized) return -1;

    // Cycle the clear color so the screen visibly animates — useful for
    // proving the loop is firing per MediaCodec frame, and for screen-cap
    // visual diff against a black-screen failure mode.
    int t = be->frameCounter++;
    float r = ((t      ) & 0xFF) / 255.0f;
    float g = ((t >>  4) & 0xFF) / 255.0f;
    float b = ((t >>  2) & 0xFF) / 255.0f;
    VkClearColorValue clearColor = { .float32 = { r, g, b, 1.0f } };

    uint32_t imgIdx = 0;
    VkResult r1 = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                            be->acquireSem, VK_NULL_HANDLE, &imgIdx);
    if (r1 != VK_SUCCESS && r1 != VK_SUBOPTIMAL_KHR) {
        if (be->frameCounter <= 5 || be->frameCounter % 60 == 0)
            LOGW("vkAcquireNextImageKHR returned %d at frame %d", r1, be->frameCounter);
        return -1;
    }

    be->vkResetCommandBuffer(be->cmdBuffer, 0);
    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (be->vkBeginCommandBuffer(be->cmdBuffer, &bbi) != VK_SUCCESS) return -1;

    if (be->graphicsInitialized) {
        // §I.B.2c.3c.2 path: render pass + fullscreen triangle test pattern.
        // Render pass loadOp=CLEAR with the cycling color, then frag shader
        // overwrites with the UV gradient. Final layout PRESENT_SRC_KHR
        // is set by the render pass itself.
        VkClearValue cv = { .color = clearColor };
        VkRenderPassBeginInfo rpbi = {
            .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass   = be->renderPass,
            .framebuffer  = be->framebuffers[imgIdx],
            .renderArea   = { {0, 0}, be->swapchainExtent },
            .clearValueCount = 1,
            .pClearValues = &cv,
        };
        be->vkCmdBeginRenderPass(be->cmdBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              be->graphicsPipeline);
        be->vkCmdDraw(be->cmdBuffer, 3, 1, 0, 0);   // fullscreen triangle
        be->vkCmdEndRenderPass(be->cmdBuffer);
    } else {
        // Fallback: B.2c.3a clear-only path. Used if pipeline init failed.
        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        };
        VkImageMemoryBarrier toDst = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = be->swapchainImages[imgIdx],
            .subresourceRange = range,
        };
        be->vkCmdPipelineBarrier(be->cmdBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &toDst);
        be->vkCmdClearColorImage(be->cmdBuffer, be->swapchainImages[imgIdx],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &clearColor, 1, &range);
        VkImageMemoryBarrier toPresent = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = be->swapchainImages[imgIdx],
            .subresourceRange = range,
        };
        be->vkCmdPipelineBarrier(be->cmdBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 1, &toPresent);
    }

    if (be->vkEndCommandBuffer(be->cmdBuffer) != VK_SUCCESS) return -1;

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->acquireSem,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1,
        .pCommandBuffers = &be->cmdBuffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &be->renderDoneSem,
    };
    if (be->vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return -1;

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &be->renderDoneSem,
        .swapchainCount = 1,
        .pSwapchains = &be->swapchain,
        .pImageIndices = &imgIdx,
    };
    VkResult r2 = be->vkQueuePresentKHR(be->graphicsQueue, &pi);
    if (r2 != VK_SUCCESS && r2 != VK_SUBOPTIMAL_KHR) {
        LOGW("vkQueuePresentKHR returned %d at frame %d", r2, be->frameCounter);
        return -1;
    }

    // Simple sync — eat the V-sync block per frame. Phase D will do real
    // async with timeline semaphores / fence-based fast path.
    be->vkQueueWaitIdle(be->graphicsQueue);

    if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
        LOGI("render_clear_frame #%d (img=%u, color=%.2f,%.2f,%.2f)",
             be->frameCounter, imgIdx, r, g, b);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeRenderClearFrame(JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return -1;
    return (jint)render_clear_frame(be);
}

// Per-frame video render: import AHB → bind → draw → present → cleanup.
static int render_ahb_frame(vk_backend_t* be, AHardwareBuffer* ahb)
{
    // First-frame lazy init of pipeline once we know the externalFormat
    // from the AHB. ensure_ycbcr_sampler runs first (called by import).
    if (!be->graphicsInitialized) {
        VkAndroidHardwareBufferFormatPropertiesANDROID fmt = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
        };
        VkAndroidHardwareBufferPropertiesANDROID propsOnly = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
            .pNext = &fmt,
        };
        if (be->vkGetAndroidHardwareBufferPropertiesANDROID(be->device, ahb, &propsOnly) == VK_SUCCESS &&
            fmt.externalFormat != 0) {
            ensure_ycbcr_sampler(be, &fmt);
            if (init_graphics_pipeline(be) != 0) {
                LOGW("lazy graphics pipeline init failed; staying on clear-only path");
                return render_clear_frame(be);
            }
            // §I.C.3.a: bring up FRUC compute resources alongside graphics.
            // Failure here is non-fatal — graphics keeps running passthrough.
            if (init_compute_pipelines(be) != 0) {
                LOGW("[VKBE-COMPUTE] lazy init failed; FRUC compute disabled this session");
            }
        } else {
            return render_clear_frame(be);
        }
    }

    // Import the AHB into VkImage+memory+view (kept alive for the GPU).
    VkImage   imgIn  = VK_NULL_HANDLE;
    VkDeviceMemory memIn = VK_NULL_HANDLE;
    VkImageView viewIn = VK_NULL_HANDLE;
    uint32_t srcW = 0, srcH = 0;
    if (do_import_ahb(be, ahb, /*keepAlive=*/1, &imgIn, &memIn, &viewIn, &srcW, &srcH) != 0) {
        return render_clear_frame(be);
    }

    // Update descriptor set: combined image sampler binding 0 → (sampler
    // is immutable in the layout; we only fill imageView + layout).
    VkDescriptorImageInfo dii = {
        .sampler     = VK_NULL_HANDLE,
        .imageView   = viewIn,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet wds = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = be->descSet,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &dii,
    };
    be->vkUpdateDescriptorSets(be->device, 1, &wds, 0, NULL);

    // ============================================================
    // §I.D — fall-back path: single-present via legacy be->cmdBuffer +
    // vkQueueWaitIdle. Used only when compute init failed (fInitialized=0)
    // — sample AHB directly via existing graphicsPipeline.
    // ============================================================
    if (!be->fInitialized) {
        uint32_t imgIdx = 0;
        VkResult r = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                               be->acquireSem, VK_NULL_HANDLE, &imgIdx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            LOGW("acquire failed: %d", r);
            goto fail_cleanup_single;
        }

        be->vkResetCommandBuffer(be->cmdBuffer, 0);
        VkCommandBufferBeginInfo bbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        if (be->vkBeginCommandBuffer(be->cmdBuffer, &bbi) != VK_SUCCESS) goto fail_cleanup_single;

        VkImageSubresourceRange range = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0, .levelCount = 1,
            .baseArrayLayer = 0, .layerCount = 1,
        };
        VkImageMemoryBarrier inAcquireSingle = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = be->graphicsQueueFamily,
            .image = imgIn,
            .subresourceRange = range,
        };
        be->vkCmdPipelineBarrier(be->cmdBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &inAcquireSingle);

        VkClearValue cv = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = {
            .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass   = be->renderPass,
            .framebuffer  = be->framebuffers[imgIdx],
            .renderArea   = { {0, 0}, be->swapchainExtent },
            .clearValueCount = 1,
            .pClearValues = &cv,
        };
        be->vkCmdBeginRenderPass(be->cmdBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        be->vkCmdBindPipeline(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, be->graphicsPipeline);
        be->vkCmdBindDescriptorSets(be->cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    be->pipelineLayout, 0, 1, &be->descSet, 0, NULL);
        float uvScale[2] = {
            (be->videoWidth  > 0 && be->ahbPaddedWidth  > 0)
                ? (float)be->videoWidth  / (float)be->ahbPaddedWidth  : 1.0f,
            (be->videoHeight > 0 && be->ahbPaddedHeight > 0)
                ? (float)be->videoHeight / (float)be->ahbPaddedHeight : 1.0f,
        };
        be->vkCmdPushConstants(be->cmdBuffer, be->pipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uvScale), uvScale);
        be->vkCmdDraw(be->cmdBuffer, 3, 1, 0, 0);
        be->vkCmdEndRenderPass(be->cmdBuffer);

        if (be->vkEndCommandBuffer(be->cmdBuffer) != VK_SUCCESS) goto fail_cleanup_single;

        VkPipelineStageFlags waitStageSingle = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &be->acquireSem,
            .pWaitDstStageMask = &waitStageSingle,
            .commandBufferCount = 1,
            .pCommandBuffers = &be->cmdBuffer,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &be->renderDoneSem,
        };
        if (be->vkQueueSubmit(be->graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
            goto fail_cleanup_single;

        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &be->renderDoneSem,
            .swapchainCount = 1,
            .pSwapchains = &be->swapchain,
            .pImageIndices = &imgIdx,
        };
        r = be->vkQueuePresentKHR(be->graphicsQueue, &pi);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) LOGW("present failed: %d", r);

        be->vkQueueWaitIdle(be->graphicsQueue);
        be->frameCounter++;
        if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
            LOGI("render_ahb_frame #%d (img=%u, src=%ux%u) [single]",
                 be->frameCounter, imgIdx, srcW, srcH);
        }

    fail_cleanup_single:
        if (viewIn) be->vkDestroyImageView(be->device, viewIn, NULL);
        if (imgIn)  be->vkDestroyImage(be->device, imgIn, NULL);
        if (memIn)  be->vkFreeMemory(be->device, memIn, NULL);
        return 0;
    }

    // ============================================================
    // §I.D — fast path: dual-present via in-flight ring (NO waitIdle)
    // §I.D.b — degrades to single present when input fps ≈ display fps
    // ============================================================
    // Frame N+1's CPU work (record + AHB import + descriptor update)
    // overlaps frame N-1's GPU work. 1 cmdbuf with 1 or 2 render passes
    // depending on mode. Mode decision: dual present (interp + real) only
    // makes sense when input < display (so FRUC adds frames); when input
    // ≈ display, dual present's 2× vsync wait throttles input rate, so
    // we drop to single present (real only) to match input.
    uint32_t slot = be->fCurrentSlot;

    // Update input-fps sliding window. Measurement is the rate at which
    // nativeRenderFrame is called (= effective MediaCodec ImageReader
    // delivery rate). Two-tier publish so the first decision settles
    // sub-second:
    //   - early publish: ≥ 30 frames AND ≥ 200ms window → ~30 frame avg
    //   - normal publish: window ≥ 1000ms → 1s sliding average
    {
        uint64_t now_ns = monotonic_ns();
        if (be->fInputWindowStartNs == 0) be->fInputWindowStartNs = now_ns;
        be->fInputWindowFrames++;
        uint64_t window_ns = now_ns - be->fInputWindowStartNs;
        int early_quick = (be->fInputFpsRecent == 0.0f &&
                           be->fInputWindowFrames >= 30 &&
                           window_ns >= 200000000ULL);
        int normal_window = (window_ns >= 1000000000ULL);
        if (early_quick || normal_window) {
            be->fInputFpsRecent = (float)be->fInputWindowFrames * 1.0e9f / (float)window_ns;
            be->fInputWindowStartNs = now_ns;
            be->fInputWindowFrames = 0;
        }
    }
    // displayHz priority: hinted refresh (§I.D.c trustworthy if hint
    // accepted) > driver-cached refresh (vkGetRefreshCycleDurationGOOGLE,
    // may be stale on Adreno 620) > 60 Hz fallback.
    float displayHz = (be->fHintedRefreshHz > 0.0f)
        ? be->fHintedRefreshHz
        : ((be->fRefreshDurationNs > 0)
            ? (1.0e9f / (float)be->fRefreshDurationNs)
            : 60.0f);
    // Dual present's value: input < display headroom for interp frame.
    // Test: would (input × 2) fit in display refresh budget? i.e.
    //     2 × inputFps ≤ displayHz × 1.05 (small margin for jitter)
    // Examples:
    //   input 60, display 60 → 120 > 63   → single (default at 60Hz/60fps)
    //   input 60, display 90 → 120 > 94.5 → single (no headroom)
    //   input 45, display 90 → 90  ≤ 94.5 → dual ✓
    //   input 30, display 60 → 60  ≤ 63   → dual ✓ (low-fps stream gets boost)
    //
    // Default single: avoids the "start in dual → input throttle → measurement
    // says low → stay dual" stuck loop observed in v1.2.163 with ALWAYS
    // change strategy. measurement-zero state means "give clean
    // baseline first" — single mode lets input flow at full rate, then
    // measurement decides whether dual would actually fit.
    int singleMode = 1;
    float doubled = be->fInputFpsRecent * 2.0f;
    float threshold = displayHz * 1.05f;
    if (be->fInputFpsRecent > 0.0f) {
        if (doubled <= threshold) {
            singleMode = 0;  // dual fits — go for FRUC
        }
    }
    if (singleMode != be->fLastSingleMode) {
        const char* hzSrc = (be->fHintedRefreshHz > 0.0f)
            ? "Java-max" : ((be->fRefreshDurationNs > 0) ? "drv-cache" : "60-fb");
        LOGI("[VKBE-RING] mode change: %s → %s | input ~%.1f FPS | display %.1f Hz (%s) | "
             "criterion: 2×input=%.1f %s threshold=%.1f",
             be->fLastSingleMode ? "single" : "dual",
             singleMode           ? "single" : "dual",
             (double)be->fInputFpsRecent, (double)displayHz, hzSrc,
             (double)doubled, (singleMode ? ">" : "≤"), (double)threshold);
        be->fLastSingleMode = singleMode;
    }

    // Wait for slot's previous in-flight GPU work to drain. First frame
    // hits a signaled fence (init_in_flight_ring set SIGNALED bit), so
    // returns immediately. Subsequent frames in this slot wait for the
    // submit from N-VK_FRAMES_IN_FLIGHT frames ago to finish.
    be->vkWaitForFences(be->device, 1, &be->fSlotInFlightFence[slot], VK_TRUE, UINT64_MAX);

    // Old slot: drop AHB resources from the work that just finished. They
    // were stored in fSlotPendingImg/Mem/View at end of that frame; now
    // that the fence signaled, GPU is done with them.
    if (be->fSlotHasPending[slot]) {
        if (be->fSlotPendingView[slot]) be->vkDestroyImageView(be->device, be->fSlotPendingView[slot], NULL);
        if (be->fSlotPendingImg[slot])  be->vkDestroyImage(be->device, be->fSlotPendingImg[slot], NULL);
        if (be->fSlotPendingMem[slot])  be->vkFreeMemory(be->device, be->fSlotPendingMem[slot], NULL);
        be->fSlotPendingView[slot] = VK_NULL_HANDLE;
        be->fSlotPendingImg[slot]  = VK_NULL_HANDLE;
        be->fSlotPendingMem[slot]  = VK_NULL_HANDLE;
        be->fSlotHasPending[slot]  = 0;
    }
    be->vkResetFences(be->device, 1, &be->fSlotInFlightFence[slot]);

    // Acquire 1 (single mode) or 2 (dual mode) swapchain images.
    // singleMode uses slot.acquireSem[0] / renderDoneSem[0] for the only
    // pass; dual mode uses both [0]/[1].
    uint32_t imgIdxPass1 = 0, imgIdxPass2 = 0;
    VkResult rA1 = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                              be->fSlotAcquireSem[slot][0], VK_NULL_HANDLE,
                                              &imgIdxPass1);
    if (rA1 != VK_SUCCESS && rA1 != VK_SUBOPTIMAL_KHR) {
        LOGW("[VKBE-RING] PASS 1 acquire failed: %d", rA1);
        goto ring_fail_drop_imported;
    }
    if (!singleMode) {
        VkResult rA2 = be->vkAcquireNextImageKHR(be->device, be->swapchain, 100000000ULL,
                                                  be->fSlotAcquireSem[slot][1], VK_NULL_HANDLE,
                                                  &imgIdxPass2);
        if (rA2 != VK_SUCCESS && rA2 != VK_SUBOPTIMAL_KHR) {
            LOGW("[VKBE-RING] PASS 2 acquire failed: %d", rA2);
            goto ring_fail_drop_imported;
        }
    }

    // Record cmdbuf. dispatch_fruc still uses be->cmdBuffer internally —
    // temporarily swap the legacy cmdbuf pointer to slot.cmdbuf for the
    // duration of recording so dispatch_fruc emits commands into the ring
    // cmdbuf. Saved+restored deterministically; we hold no async state.
    VkCommandBuffer slotCmd = be->fSlotCmdBuf[slot];
    VkCommandBuffer saveLegacyCmd = be->cmdBuffer;
    be->cmdBuffer = slotCmd;

    be->vkResetCommandBuffer(slotCmd, 0);
    VkCommandBufferBeginInfo bbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (be->vkBeginCommandBuffer(slotCmd, &bbi) != VK_SUCCESS) {
        be->cmdBuffer = saveLegacyCmd;
        goto ring_fail_drop_imported;
    }

    // inAcquire on imgIn — same as before, dst stages = COMPUTE | FRAGMENT
    // because both render passes (compute via dispatch_fruc, graphics via
    // PASS 2) sample imgIn.
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0, .levelCount = 1,
        .baseArrayLayer = 0, .layerCount = 1,
    };
    VkImageMemoryBarrier inAcquireRing = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = be->graphicsQueueFamily,
        .image = imgIn,
        .subresourceRange = range,
    };
    be->vkCmdPipelineBarrier(slotCmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &inAcquireRing);

    // FRUC compute pipeline (dispatch_fruc uses be->cmdBuffer = slotCmd).
    dispatch_fruc(be, viewIn);

    // compute → fragment barrier so render pass 1 can sample interpFrame.
    {
        VkMemoryBarrier mb_compute_to_frag = {
            .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        be->vkCmdPipelineBarrier(slotCmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 1, &mb_compute_to_frag, 0, NULL, 0, NULL);
    }

    // ---- Render pass 1 (interp): sample interpFrame → imgIdxPass1 (dual only) ----
    if (!singleMode) {
        VkClearValue cv = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = {
            .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass   = be->renderPass,
            .framebuffer  = be->framebuffers[imgIdxPass1],
            .renderArea   = { {0, 0}, be->swapchainExtent },
            .clearValueCount = 1,
            .pClearValues = &cv,
        };
        be->vkCmdBeginRenderPass(slotCmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        be->vkCmdBindPipeline(slotCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, be->fInterpPipeline);
        be->vkCmdBindDescriptorSets(slotCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    be->fInterpPipeLayout, 0, 1, &be->fInterpDescSet, 0, NULL);
        float uvInterp[2] = { 1.0f, 1.0f };
        be->vkCmdPushConstants(slotCmd, be->fInterpPipeLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uvInterp), uvInterp);
        be->vkCmdDraw(slotCmd, 3, 1, 0, 0);
        be->vkCmdEndRenderPass(slotCmd);
    }

    // ---- Render pass 2 (real): sample AHB direct ----
    // Target framebuffer = imgIdxPass2 in dual mode, imgIdxPass1 in single
    // mode (we only acquired one image and skipped the interp pass).
    {
        uint32_t realFbIdx = singleMode ? imgIdxPass1 : imgIdxPass2;
        VkClearValue cv = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
        VkRenderPassBeginInfo rpbi = {
            .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass   = be->renderPass,
            .framebuffer  = be->framebuffers[realFbIdx],
            .renderArea   = { {0, 0}, be->swapchainExtent },
            .clearValueCount = 1,
            .pClearValues = &cv,
        };
        be->vkCmdBeginRenderPass(slotCmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        be->vkCmdBindPipeline(slotCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, be->graphicsPipeline);
        be->vkCmdBindDescriptorSets(slotCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    be->pipelineLayout, 0, 1, &be->descSet, 0, NULL);
        float uvReal[2] = {
            (be->videoWidth  > 0 && be->ahbPaddedWidth  > 0)
                ? (float)be->videoWidth  / (float)be->ahbPaddedWidth  : 1.0f,
            (be->videoHeight > 0 && be->ahbPaddedHeight > 0)
                ? (float)be->videoHeight / (float)be->ahbPaddedHeight : 1.0f,
        };
        be->vkCmdPushConstants(slotCmd, be->pipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uvReal), uvReal);
        if (be->frameCounter == 0) {
            LOGI("uvScale push constants: x=%.4f (video %d / ahb %d) "
                 "y=%.4f (video %d / ahb %d)",
                 uvReal[0], be->videoWidth,  be->ahbPaddedWidth,
                 uvReal[1], be->videoHeight, be->ahbPaddedHeight);
        }
        be->vkCmdDraw(slotCmd, 3, 1, 0, 0);
        be->vkCmdEndRenderPass(slotCmd);
    }

    if (be->vkEndCommandBuffer(slotCmd) != VK_SUCCESS) {
        be->cmdBuffer = saveLegacyCmd;
        goto ring_fail_drop_imported;
    }
    be->cmdBuffer = saveLegacyCmd;

    // Single submit with 1 (single mode) or 2 (dual) wait/signal sems +
    // fence. Acquires gate at COLOR_ATTACHMENT_OUTPUT.
    VkPipelineStageFlags waitStages[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphore waitSems[2]   = { be->fSlotAcquireSem[slot][0],   be->fSlotAcquireSem[slot][1] };
    VkSemaphore signalSems[2] = { be->fSlotRenderDoneSem[slot][0], be->fSlotRenderDoneSem[slot][1] };
    uint32_t semCount = singleMode ? 1u : 2u;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = semCount,
        .pWaitSemaphores = waitSems,
        .pWaitDstStageMask = waitStages,
        .commandBufferCount = 1,
        .pCommandBuffers = &slotCmd,
        .signalSemaphoreCount = semCount,
        .pSignalSemaphores = signalSems,
    };
    if (be->vkQueueSubmit(be->graphicsQueue, 1, &si, be->fSlotInFlightFence[slot]) != VK_SUCCESS) {
        LOGW("[VKBE-RING] vkQueueSubmit failed");
        goto ring_fail_drop_imported;
    }

    // §I.C.6 — PTS for the present(s). In single mode we only have 1
    // present; PASS 1 here = the real frame (we mapped imgIdxPass1 to
    // graphics pipeline in render pass 2 above).
    uint64_t now_ns = monotonic_ns();
    if (singleMode) {
        // Single present (real frame at imgIdxPass1, signaled by sem[0]).
        VkPresentTimeGOOGLE pt = { ++be->fPresentId, now_ns };
        VkPresentTimesInfoGOOGLE pti = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .swapchainCount = 1, .pTimes = &pt,
        };
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = be->fDisplayTimingSupported ? &pti : NULL,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &be->fSlotRenderDoneSem[slot][0],
            .swapchainCount = 1,
            .pSwapchains = &be->swapchain,
            .pImageIndices = &imgIdxPass1,
        };
        VkResult rp = be->vkQueuePresentKHR(be->graphicsQueue, &pi);
        if (rp != VK_SUCCESS && rp != VK_SUBOPTIMAL_KHR) LOGW("[VKBE-RING] single present failed: %d", rp);
    } else {
        // Dual present (interp at imgIdxPass1, real at imgIdxPass2).
        VkPresentTimeGOOGLE pt1 = { ++be->fPresentId, now_ns };
        VkPresentTimesInfoGOOGLE pti1 = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .swapchainCount = 1, .pTimes = &pt1,
        };
        VkPresentInfoKHR pi1 = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = be->fDisplayTimingSupported ? &pti1 : NULL,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &be->fSlotRenderDoneSem[slot][0],
            .swapchainCount = 1,
            .pSwapchains = &be->swapchain,
            .pImageIndices = &imgIdxPass1,
        };
        VkResult rp1 = be->vkQueuePresentKHR(be->graphicsQueue, &pi1);
        if (rp1 != VK_SUCCESS && rp1 != VK_SUBOPTIMAL_KHR) LOGW("[VKBE-RING] PASS 1 present failed: %d", rp1);

        uint64_t pass2_desired = (be->fRefreshDurationNs > 0)
            ? now_ns + be->fRefreshDurationNs / 2
            : monotonic_ns();
        VkPresentTimeGOOGLE pt2 = { ++be->fPresentId, pass2_desired };
        VkPresentTimesInfoGOOGLE pti2 = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,
            .swapchainCount = 1, .pTimes = &pt2,
        };
        VkPresentInfoKHR pi2 = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = be->fDisplayTimingSupported ? &pti2 : NULL,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &be->fSlotRenderDoneSem[slot][1],
            .swapchainCount = 1,
            .pSwapchains = &be->swapchain,
            .pImageIndices = &imgIdxPass2,
        };
        VkResult rp2 = be->vkQueuePresentKHR(be->graphicsQueue, &pi2);
        if (rp2 != VK_SUCCESS && rp2 != VK_SUBOPTIMAL_KHR) LOGW("[VKBE-RING] PASS 2 present failed: %d", rp2);
    }

    // Stash imgIn / memIn / viewIn into slot.pendingImg/Mem/View — they're
    // freed at start of next time this slot is reused (after that slot's
    // fence signals → GPU done with these handles).
    be->fSlotPendingImg[slot]  = imgIn;
    be->fSlotPendingMem[slot]  = memIn;
    be->fSlotPendingView[slot] = viewIn;
    be->fSlotHasPending[slot]  = 1;

    be->fCurrentSlot = (slot + 1) % VK_FRAMES_IN_FLIGHT;
    be->frameCounter++;
    // Only count an "interpolated frame" when we actually synthesized one
    // (dual mode); single mode is just real-frame passthrough through
    // compute pipeline + graphics → no extra display frame produced.
    if (!singleMode) {
        be->fInterpolatedCount++;
    }

    if (be->frameCounter <= 5 || be->frameCounter % 120 == 0) {
        if (singleMode) {
            LOGI("render_ahb_frame #%d (slot=%u img=%u, src=%ux%u) [single, ring, in~%.1f]",
                 be->frameCounter, slot, imgIdxPass1, srcW, srcH,
                 (double)be->fInputFpsRecent);
        } else {
            LOGI("render_ahb_frame #%d (slot=%u img=[%u,%u], src=%ux%u) [dual, ring, in~%.1f]",
                 be->frameCounter, slot, imgIdxPass1, imgIdxPass2, srcW, srcH,
                 (double)be->fInputFpsRecent);
        }
    }

    // §I.C.6 — periodic past-timing diagnostic (every 120 frames).
    if (be->fDisplayTimingSupported && be->vkGetPastPresentationTimingGOOGLE &&
        be->frameCounter > 0 && be->frameCounter % 120 == 0) {
        uint32_t numPast = 0;
        be->vkGetPastPresentationTimingGOOGLE(be->device, be->swapchain, &numPast, NULL);
        if (numPast > 8) numPast = 8;
        if (numPast > 0) {
            VkPastPresentationTimingGOOGLE past[8];
            if (be->vkGetPastPresentationTimingGOOGLE(be->device, be->swapchain,
                                                       &numPast, past) == VK_SUCCESS) {
                for (uint32_t i = 0; i < numPast; i++) {
                    int64_t delta_ns = (int64_t)past[i].actualPresentTime
                                     - (int64_t)past[i].desiredPresentTime;
                    LOGI("[VK-DISPLAY-TIMING] past id=%u delta=%+lld ns (margin=%llu)",
                         past[i].presentID, (long long)delta_ns,
                         (unsigned long long)past[i].presentMargin);
                }
            }
        }
    }
    return 0;

ring_fail_drop_imported:
    if (viewIn) be->vkDestroyImageView(be->device, viewIn, NULL);
    if (imgIn)  be->vkDestroyImage(be->device, imgIn, NULL);
    if (memIn)  be->vkFreeMemory(be->device, memIn, NULL);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeGetInterpolatedCount(
    JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return 0;
    return (jint)be->fInterpolatedCount;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VkBackend_nativeSetQualityLevel(
    JNIEnv* env, jclass clazz, jlong handle, jint level)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return;
    int q = (int)level;
    if (q < 0) q = 0;
    if (q > 2) q = 2;
    if (q != be->fQualityLevel) {
        LOGI("[VKBE-COMPUTE] qualityLevel %d -> %d (motionest_q%d + warp_q%d)",
             be->fQualityLevel, q, q, q);
        be->fQualityLevel = q;
    }
}

JNIEXPORT jint JNICALL
Java_com_limelight_binding_video_VkBackend_nativeRenderFrame(
    JNIEnv* env, jclass clazz, jlong handle, jobject jHwBuffer)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return -1;
    if (!jHwBuffer) return (jint)render_clear_frame(be);
    AHardwareBuffer* ahb = AHardwareBuffer_fromHardwareBuffer(env, jHwBuffer);
    if (!ahb) return (jint)render_clear_frame(be);
    return (jint)render_ahb_frame(be, ahb);
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_video_VkBackend_nativeDestroy(JNIEnv* env, jclass clazz, jlong handle)
{
    vk_backend_t* be = (vk_backend_t*)(uintptr_t)handle;
    if (!be) return;
    LOGI("destroying backend handle=%p (frames rendered=%d)", be, be->frameCounter);
    destroy_compute_pipelines(be);
    destroy_graphics_pipeline(be);
    destroy_ycbcr_sampler(be);
    destroy_render_resources(be);
    if (be->swapchainImages) {
        free(be->swapchainImages);
        be->swapchainImages = NULL;
    }
    if (be->swapchain && be->vkDestroySwapchainKHR) {
        be->vkDestroySwapchainKHR(be->device, be->swapchain, NULL);
        be->swapchain = VK_NULL_HANDLE;
    }
    if (be->surface && be->vkDestroySurfaceKHR) {
        be->vkDestroySurfaceKHR(be->instance, be->surface, NULL);
        be->surface = VK_NULL_HANDLE;
    }
    if (be->nativeWindow) {
        ANativeWindow_release(be->nativeWindow);
        be->nativeWindow = NULL;
    }
    if (be->device && be->vkDestroyDevice) {
        be->vkDestroyDevice(be->device, NULL);
        be->device = VK_NULL_HANDLE;
    }
    if (be->instance && be->vkDestroyInstance) {
        be->vkDestroyInstance(be->instance, NULL);
        be->instance = VK_NULL_HANDLE;
    }
    if (be->libvulkan) {
        dlclose(be->libvulkan);
        be->libvulkan = NULL;
    }
    free(be);
}
