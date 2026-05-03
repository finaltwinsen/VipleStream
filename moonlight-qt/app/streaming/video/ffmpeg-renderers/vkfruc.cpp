// VipleStream §J.3.e.2.i — VkFrucRenderer
// See vkfruc.h header + docs/J.3.e.2.i_vulkan_native_renderer.md.

#include "vkfruc.h"
#include "vkfruc-aftermath.h"
#include "settings/streamingpreferences.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

// SSE2 intrinsics for renderFrameSw's YUV420P→NV12 UV-plane interleave
// fast path.  All currently-supported moonlight-qt build targets are x86
// with SSE2 available.
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

// §J.3.e.2.i.3.a — FFmpeg 6.1 (LIBAVCODEC_VERSION_MAJOR=60) used a
// MESA AV1 ext name; 7.0 (61+) uses official KHR.  AVVulkanDeviceContext
// also bumped its queue-family struct shape between the two.  We
// follow PlVkRenderer's compile-time gating.
#ifndef VK_KHR_video_decode_av1
#define VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME "VK_KHR_video_decode_av1"
#endif

// Shared lock for ffmpeg vkQueueSubmit serialisation (per AVVulkanDeviceContext
// contract).  ffmpeg may submit on multiple threads; our renderFrame is
// single-threaded, but ffmpeg can submit decode work concurrently.
static std::mutex s_VkFrucQueueLock;

// §J.3.e.2.i.6 — process-wide ref count for ncnn::create_gpu_instance.
// Multiple VkFrucRenderer instances (test probes + real) each call create
// in createFrucComputeResources; we must call destroy ONLY when the last
// instance tears down — otherwise teardown order vs ncnn's static dtors
// causes SIGSEGV on process exit (observed in v1.3.151 dual-present test).
// PlVkRenderer pattern (plvk.cpp:158): ncnn::destroy_gpu_instance() must
// run BEFORE the renderer's VkDevice is destroyed.
static std::atomic<int> s_NcnnRefCount(0);

VkFrucRenderer::VkFrucRenderer(int pass)
    : IFFmpegRenderer(RendererType::Vulkan)
    , m_Pass(pass)
{
    // §J.3.e.2.i.3.e-SW — opt into software-decode upload path when 設定
    // 是 RS_VULKAN（v1.3.175 default）或 env var 是設的（dev / probe）.
    // Bypasses FFmpeg-Vulkan hwcontext entirely (currently broken; see
    // docs/J.3.e.2.i_vulkan_native_renderer.md).
    //
    // Preference path：當使用者在 Settings dropdown 選 Vulkan 渲染器，
    // m_SwMode / m_FrucMode / m_DualMode 都自動開（FRUC 跟 dual 還受
    // enableFrameInterpolation 控制 —— 沒勾補幀就只跑 SW upload + single
    // present）.  Env-var path 留給 dev/CI 測試獨立子集 (e.g. 只想測 SW
    // upload 不想跑 FRUC 用 VIPLE_VKFRUC_SW=1 + 不設 FRUC/DUAL).
    auto* prefs = StreamingPreferences::get(nullptr);
    bool prefsWantVulkan = prefs && prefs->rendererSelection == StreamingPreferences::RS_VULKAN;
    bool prefsWantInterp = prefs && prefs->enableFrameInterpolation;
    m_SwMode   = qEnvironmentVariableIntValue("VIPLE_VKFRUC_SW")   != 0 || prefsWantVulkan;
    m_FrucMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_FRUC") != 0 || (prefsWantVulkan && prefsWantInterp);
    m_DualMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_DUAL") != 0 || (prefsWantVulkan && prefsWantInterp);
    // §J.3.e.2.i.8 Phase 3d.4i — diagnostic override: forces FRUC + dual mode
    // OFF so we can isolate whether the AV1 grey-green flicker comes from the
    // decode path or the FRUC interpolation/dual-present blending path.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_NO_FRUC") != 0) {
        m_FrucMode = false;
        m_DualMode = false;
    }
    // §J.3.e.2.i.8 Phase 1.5c — when running in ONLY mode (Pacer driven by
    // synth AVFrame from native decode, no FFmpeg avcodec_send_packet),
    // force FRUC + dual mode OFF.  Phase 2.5's graphics-queue image→buffer
    // copy of m_SwUploadImage races against the previous frame's fragment-
    // shader sample on cross-cmd-buf submissions when ONLY's high
    // submission rate (= network packet rate) is reached → DEVICE_LOST in
    // ~3 frames on NV 596.84 (v1.3.278 testing).  Until Phase 2.5
    // architectural fix moves the copy to the decode queue, ONLY mode is
    // exclusive of FRUC.  User can still get FRUC via PARALLEL mode (slower
    // FFmpeg-bound but FRUC works).  Allow override for forensics via
    // VIPLE_VKFRUC_ONLY_FORCE_FRUC=1 (testing-only — expect crash).
    // §J.3.e.2.i.8 Phase 1.7e — env name renamed to *_DANGEROUS so a stale
    // `setx VIPLE_VKFRUC_NATIVE_DECODE_ONLY=1` from prior debug sessions
    // doesn't silently flip into the known-broken NVDEC-fault path.
    static const bool s_onlyMode =
        qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS") != 0;
    static const bool s_onlyForceFruc =
        qEnvironmentVariableIntValue("VIPLE_VKFRUC_ONLY_FORCE_FRUC") != 0;
    if (s_onlyMode && (m_FrucMode || m_DualMode) && !s_onlyForceFruc) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.5c — ONLY mode active, "
                    "auto-disabling FRUC + dual-present (Phase 2.5 image→buffer "
                    "copy collides with high-rate submissions → DEVICE_LOST). "
                    "Set VIPLE_VKFRUC_ONLY_FORCE_FRUC=1 to override (debug).");
        m_FrucMode = false;
        m_DualMode = false;
    }
    // §J.3.e.2.i.7 HW path retry：VIPLE_VKFRUC_HW=1 強制走 FFmpeg-Vulkan
    // hwcontext (override SW path).  搭配 mirror-libplacebo ext list 嘗試
    // 解掉 v1.3.123-136 的 NV nvoglv64 NULL deref crash.  Init 失敗或
    // crash 時 cascade fallback 到 PlVkRenderer.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_HW") != 0) {
        m_SwMode = false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=%d, swMode=%d, frucMode=%d, dualMode=%d, prefs=%s)",
                pass, m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0,
                prefsWantVulkan ? "RS_VULKAN" : "n/a");
}

// §J.3.e.2.i.3.e-SW — declare NV12 as preferred so FFmpeg get_format()
// picks software NV12 output when our renderer is selected via the
// software cascade.  In Vulkan-hwaccel mode (m_SwMode=false), we don't
// participate in the software cascade — return base default (will be
// ignored anyway because Vulkan path goes through createHwAccelRenderer).
AVPixelFormat VkFrucRenderer::getPreferredPixelFormat(int videoFormat)
{
    if (m_SwMode) {
        // FFmpeg software h264 / hevc / av1 decoders default to YUV420P
        // (3 planes: Y + U + V).  We accept both YUV420P and NV12 in
        // renderFrameSw() and re-pack into our NV12 multi-plane VkImage.
        return AV_PIX_FMT_YUV420P;
    }
    // §J.3.e.2.i.7 HW path: 告訴 FFmpeg get_format() 走 Vulkan HW decode
    // (AVVkFrame).  之前 v1.3.123-136 沒回這個 → FFmpeg 走 yuv420p CPU
    // path 反而 cascade 出問題.  m_SwMode=false 時必須 return AV_PIX_FMT_VULKAN.
    (void)videoFormat;
    return AV_PIX_FMT_VULKAN;
}

bool VkFrucRenderer::isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat)
{
    if (m_SwMode) {
        return pixelFormat == AV_PIX_FMT_YUV420P || pixelFormat == AV_PIX_FMT_NV12;
    }
    // §J.3.e.2.i.7 HW path
    (void)videoFormat;
    return pixelFormat == AV_PIX_FMT_VULKAN;
}

VkFrucRenderer::~VkFrucRenderer()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 dtor");
    teardown();
    if (m_HwDeviceCtx != nullptr) {
        av_buffer_unref(&m_HwDeviceCtx);
        m_HwDeviceCtx = nullptr;
    }
}

// AVVulkanDeviceContext lock_queue / unlock_queue callbacks.  ffmpeg
// uses these to serialise vkQueueSubmit calls on shared queues.  We
// only have one graphics queue family so a single mutex suffices.
void VkFrucRenderer::lockQueueStub(struct AVHWDeviceContext*, uint32_t, uint32_t)
{
    s_VkFrucQueueLock.lock();
}
void VkFrucRenderer::unlockQueueStub(struct AVHWDeviceContext*, uint32_t, uint32_t)
{
    s_VkFrucQueueLock.unlock();
}

// §J.3.e.2.i — FRUC status getters 給 perf overlay (ffmpeg.cpp:1073-1107) 跟
// Pacer::renderFrame (pacer.cpp:348) 用.  D3D11VARenderer 對應在
// d3d11va.cpp:2306-2322.  VkFruc 的 FRUC 走 native Vulkan compute (ME→
// median→warp) + dual-present，所以判斷條件是 m_FrucMode + m_FrucReady +
// m_DualMode 三者皆 true.  m_FRUCPaused 由 base class 提供 (renderer.h:310)，
// Ctrl+Alt+Shift+F 切換；目前 VkFruc 的 renderFrameSw 還沒接 pause 跳過 dual-
// present 的邏輯，但 lastFrameHadFRUCInterp 仍尊重它讓 stats 一致.
bool VkFrucRenderer::isFRUCActive() const
{
    return m_FrucMode && m_FrucReady && m_DualMode;
}

bool VkFrucRenderer::lastFrameHadFRUCInterp() const
{
    return m_FrucMode && m_FrucReady && m_DualMode && !m_FRUCPaused.load();
}

const char* VkFrucRenderer::getFRUCBackendName() const
{
    return "VkFruc-Vulkan compute";
}

void VkFrucRenderer::teardown()
{
    // §J.3.e.2.i.3.e — drain GPU first.  Pending submits may still hold
    // image views / descriptor sets / cmd buffers; if we destroy them
    // mid-flight the driver will explode (or silently corrupt).
    if (m_Device != VK_NULL_HANDLE && m_pfnGetInstanceProcAddr) {
        auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        if (pfnGetDevPa) {
            auto pfnDeviceWaitIdle = (PFN_vkDeviceWaitIdle)pfnGetDevPa(
                m_Device, "vkDeviceWaitIdle");
            if (pfnDeviceWaitIdle) {
                pfnDeviceWaitIdle(m_Device);
            }
        }
    }

    // Order: device-owned objects (descriptor pool → in-flight ring →
    // pipelines → render-pass → layouts → sampler) → swapchain → device
    // → surface → instance.  Pipelines reference render pass + descriptor
    // set layout, so they go first; sampler-conversion holds the
    // immutable sampler used in descriptor set layout, so layouts go
    // before the sampler.
    destroyNvVideoParser();         // §J.3.e.2.i.8 native VK decode parser
    destroyDecodeCommandResources(); // §J.3.e.2.i.8 Phase 1.3c decode cmd
    destroyVideoSession();          // §J.3.e.2.i.8 native VK decode
    destroyOverlayResources();      // §J.3.e.2.i overlay
    destroyInterpGraphicsPipeline(); // §J.3.e.2.i.4.2
    destroyFrucComputeResources(); // §J.3.e.2.i.4
    destroySwUploadResources();   // §J.3.e.2.i.3.e-SW
    destroyDescriptorPool();
    destroyInFlightRing();
    destroyGraphicsPipeline();
    destroyRenderPassAndFramebuffers();
    destroyYcbcrSamplerAndLayouts();
    destroySwapchain();
    if (m_Device != VK_NULL_HANDLE && m_pfnDestroyDevice) {
        m_pfnDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_Surface != VK_NULL_HANDLE && m_pfnDestroySurfaceKHR) {
        m_pfnDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    if (m_DebugMessenger != VK_NULL_HANDLE && m_Instance != VK_NULL_HANDLE && m_pfnGetInstanceProcAddr) {
        auto pfn = (PFN_vkDestroyDebugUtilsMessengerEXT)m_pfnGetInstanceProcAddr(
            m_Instance, "vkDestroyDebugUtilsMessengerEXT");
        if (pfn) pfn(m_Instance, m_DebugMessenger, nullptr);
        m_DebugMessenger = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE && m_pfnDestroyInstance) {
        m_pfnDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::createInstanceAndSurface(SDL_Window* window)
{
    // §J.3.e.2.i.8 Phase 1.6 — enable Nsight Aftermath GPU crash dump
    // collection BEFORE we create the Vulkan instance/device.  Idempotent;
    // first call wins, subsequent calls are no-ops.  When the device goes
    // into VK_ERROR_DEVICE_LOST (NV TDR / driver fault), Aftermath writes
    // a `.nv-gpudmp` to %TEMP% so we can post-mortem load in Nsight Graphics
    // and see which cmd buffer / which dispatch tipped the GPU over.  No-op
    // if SDK not compiled in (release builds without 3rdparty/aftermath_sdk
    // present remain SDK-free).  Only meaningful on NVIDIA cards.
    VipleAftermath::Ensure();

    m_pfnGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!m_pfnGetInstanceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetVkGetInstanceProcAddr failed: %s",
                     SDL_GetError());
        return false;
    }

    auto pfnCreateInstance =
        (PFN_vkCreateInstance)m_pfnGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!pfnCreateInstance) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 getProc(vkCreateInstance) NULL");
        return false;
    }

    unsigned int extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #1: %s",
                     SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(extCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, exts.data())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #2: %s",
                     SDL_GetError());
        return false;
    }

    // §J.3.e.2.i.8 Phase 1.3d.2 debug — opt-in VK_EXT_debug_utils so validation
    // layer messages route through SDL_Log into our log file.  Gated on
    // VIPLE_VKFRUC_VULKAN_DEBUG=1 so production builds stay extension-minimal.
    bool wantDebugUtils = qEnvironmentVariableIntValue("VIPLE_VKFRUC_VULKAN_DEBUG") != 0;
    if (wantDebugUtils) {
        // Verify the extension is exposed by the loader before requesting it
        // (would fail vkCreateInstance with VK_ERROR_EXTENSION_NOT_PRESENT
        // otherwise).
        auto pfnEnumExt = (PFN_vkEnumerateInstanceExtensionProperties)m_pfnGetInstanceProcAddr(
            nullptr, "vkEnumerateInstanceExtensionProperties");
        bool dbgUtilsAvail = false;
        if (pfnEnumExt) {
            uint32_t propCount = 0;
            pfnEnumExt(nullptr, &propCount, nullptr);
            std::vector<VkExtensionProperties> props(propCount);
            pfnEnumExt(nullptr, &propCount, props.data());
            for (auto& p : props) {
                if (strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                    dbgUtilsAvail = true;
                    break;
                }
            }
        }
        if (dbgUtilsAvail) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.8 debug: VK_EXT_debug_utils requested "
                        "(VIPLE_VKFRUC_VULKAN_DEBUG=1) — validation messages will route to log");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.8 debug: VIPLE_VKFRUC_VULKAN_DEBUG=1 set "
                        "but VK_EXT_debug_utils not exposed (no validation layer loaded?)");
        }
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VipleStreamFruc";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "VipleStream";
    appInfo.engineVersion = 1;
    // §J.3.e.2.i.3.a CRITICAL — FFmpeg's hwcontext_vulkan.h documents
    // `VkInstance inst` requires "at least version 1.3" (libavutil 60+).
    // 1.1 here causes the NV driver dispatch table to NOT populate
    // 1.3-core PFNs (synchronization2, dynamic_rendering, etc.) that
    // FFmpeg's decoder calls at submitDecodeUnit time → NULL deref at
    // offset 0xF0 in nvoglv64 (verified via cdb on minidump
    // VipleStream-1777464748.dmp; root cause of v1.3.123-133 crashes).
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // §J.3.e.2.i.8 Phase 1.5c — when VIPLE_VKFRUC_VULKAN_DEBUG=1 and the
    // Khronos validation layer is installed (LunarG SDK or
    // VK_LAYER_PATH set), enable it.  Routes spec violations + sync errors
    // through the debug messenger to SDL_Log, critical infra for diagnosing
    // DEVICE_LOST events that production driver hides as silent GPU faults.
    std::vector<const char*> layers;
    if (wantDebugUtils) {
        auto pfnEnumLayer = (PFN_vkEnumerateInstanceLayerProperties)m_pfnGetInstanceProcAddr(
            nullptr, "vkEnumerateInstanceLayerProperties");
        if (pfnEnumLayer) {
            uint32_t lcount = 0;
            pfnEnumLayer(&lcount, nullptr);
            std::vector<VkLayerProperties> lprops(lcount);
            pfnEnumLayer(&lcount, lprops.data());
            for (auto& l : lprops) {
                if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] §J.3.e.2.i.8 debug: VK_LAYER_KHRONOS_validation "
                                "enabled (spec=v%u impl=%u)", l.specVersion, l.implementationVersion);
                    break;
                }
            }
            if (layers.empty()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 debug: VIPLE_VKFRUC_VULKAN_DEBUG=1 "
                            "set but VK_LAYER_KHRONOS_validation not found — install LunarG "
                            "Vulkan SDK or set VK_LAYER_PATH to the Khronos validation layer dir");
            }
        }
    }

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    VkResult rc = pfnCreateInstance(&ici, nullptr, &m_Instance);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateInstance rc=%d", (int)rc);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkInstance created (exts=%u)",
                (unsigned)exts.size());

    // §J.3.e.2.i.8 Phase 1.3d.2 debug — register debug messenger if extension
    // was enabled.  Errors / warnings flow through SDL_Log; verbose info is
    // suppressed by default to avoid drowning the log.
    if (wantDebugUtils) {
        auto pfnCreateMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)m_pfnGetInstanceProcAddr(
            m_Instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreateMessenger) {
            VkDebugUtilsMessengerCreateInfoEXT mci = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = &VkFrucRenderer::debugMessengerCallback;
            mci.pUserData       = this;
            if (pfnCreateMessenger(m_Instance, &mci, nullptr, &m_DebugMessenger) == VK_SUCCESS) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 debug messenger registered");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkCreateDebugUtilsMessengerEXT failed");
            }
        }
    }

    m_pfnDestroyInstance = (PFN_vkDestroyInstance)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroyInstance");
    m_pfnDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroySurfaceKHR");
    if (!m_pfnDestroyInstance || !m_pfnDestroySurfaceKHR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 instance proc table incomplete");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, m_Instance, &m_Surface)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_CreateSurface: %s",
                     SDL_GetError());
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkSurfaceKHR created");
    return true;
}

bool VkFrucRenderer::pickPhysicalDeviceAndQueue()
{
    auto pfnEnumPDs = (PFN_vkEnumeratePhysicalDevices)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumeratePhysicalDevices");
    auto pfnGetPDProps = (PFN_vkGetPhysicalDeviceProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceProperties");
    auto pfnGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnGetSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    auto pfnEnumDevExts = (PFN_vkEnumerateDeviceExtensionProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumerateDeviceExtensionProperties");
    (void)pfnEnumDevExts;
    if (!pfnEnumPDs || !pfnGetPDProps || !pfnGetQFP || !pfnGetSurfSupport) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD proc PFN load failed");
        return false;
    }

    uint32_t pdCount = 0;
    pfnEnumPDs(m_Instance, &pdCount, nullptr);
    if (pdCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    pfnEnumPDs(m_Instance, &pdCount, pds.data());

    // Pick first device with a queue family that supports both graphics
    // and present on m_Surface.  Prefer DISCRETE_GPU; fall back to whatever
    // works.  PC NVIDIA / AMD / Intel typically have a "universal" queue
    // family — same pattern as Android's pick_physical_device_and_queue.
    int discreteIdx = -1, anyIdx = -1;
    uint32_t discreteQF = UINT32_MAX, anyQF = UINT32_MAX;
    for (uint32_t i = 0; i < pdCount; i++) {
        VkPhysicalDeviceProperties pdp = {};
        pfnGetPDProps(pds[i], &pdp);

        uint32_t qfCount = 0;
        pfnGetQFP(pds[i], &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        pfnGetQFP(pds[i], &qfCount, qfs.data());

        for (uint32_t qf = 0; qf < qfCount; qf++) {
            VkBool32 surfaceOk = VK_FALSE;
            pfnGetSurfSupport(pds[i], qf, m_Surface, &surfaceOk);
            const bool gfxOk = (qfs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (!surfaceOk || !gfxOk) continue;

            if (anyIdx < 0) { anyIdx = (int)i; anyQF = qf; }
            if (pdp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && discreteIdx < 0) {
                discreteIdx = (int)i; discreteQF = qf;
            }
            break;  // one queue family per device is enough for our pick
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD[%u] '%s' type=%d apiVer=%u",
                    i, pdp.deviceName, (int)pdp.deviceType, pdp.apiVersion);
    }

    if (discreteIdx >= 0) {
        m_PhysicalDevice = pds[discreteIdx];
        m_QueueFamily    = discreteQF;
    } else if (anyIdx >= 0) {
        m_PhysicalDevice = pds[anyIdx];
        m_QueueFamily    = anyQF;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no PD with graphics+present queue");
        return false;
    }

    VkPhysicalDeviceProperties chosen = {};
    pfnGetPDProps(m_PhysicalDevice, &chosen);

    // §J.3.e.2.i.3.a — also locate a queue family with VIDEO_DECODE_BIT_KHR
    // so ffmpeg can decode H264/H265/AV1 into AVVkFrame on our device.
    // NV typically exposes this on QF=3 (separate from graphics QF=0).
    uint32_t qfCount = 0;
    pfnGetQFP(m_PhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    pfnGetQFP(m_PhysicalDevice, &qfCount, qfs.data());
    for (uint32_t qf = 0; qf < qfCount; qf++) {
        if (qfs[qf].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            m_DecodeQueueFamily = qf;
            m_DecodeQueueCount  = qfs[qf].queueCount;
            break;
        }
    }
    // §J.3.e.2.i.8 — VK_KHR_video_decode capability probe.  Query whether
    // this device supports H.264 / H.265 / AV1 decode via raw Vulkan API
    // (跳過 FFmpeg).  Probe only — actual decode session 在 i.8.* sub-phase
    // 實作.  Log 結果讓使用者 + 後續 session 知道 driver 可走的 codec.
    //
    // v1.3.192 第一步：先 enumerate device extensions，看 driver 是否開放
    // VK_KHR_video_queue / video_decode_h264 / h265 / av1.  上一個 session
    // probe 全 NOT SUPPORTED (rc=-3) 可能就是因為 ext 根本沒開放，不是
    // profile struct 問題.
    {
        auto pfnEnumDevExts3 = (PFN_vkEnumerateDeviceExtensionProperties)m_pfnGetInstanceProcAddr(
            m_Instance, "vkEnumerateDeviceExtensionProperties");
        if (pfnEnumDevExts3) {
            uint32_t devExtCount = 0;
            pfnEnumDevExts3(m_PhysicalDevice, nullptr, &devExtCount, nullptr);
            std::vector<VkExtensionProperties> devExtProps(devExtCount);
            pfnEnumDevExts3(m_PhysicalDevice, nullptr, &devExtCount, devExtProps.data());
            const char* probeExts[] = {
                VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
                VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
            };
            for (const char* e : probeExts) {
                bool found = false;
                for (const auto& p : devExtProps) {
                    if (strcmp(p.extensionName, e) == 0) { found = true; break; }
                }
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 ext %s: %s",
                            e, found ? "AVAILABLE" : "MISSING");
            }
        }
    }

    auto pfnGetVidCaps = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    if (pfnGetVidCaps) {
        struct CodecProbe {
            const char* name;
            VkVideoCodecOperationFlagBitsKHR op;
            uint32_t profile;  // codec-specific profile
        };
        // Profile values: H.264 Main=1, H.265 Main=0, AV1 Main=0
        const CodecProbe probes[] = {
            { "H.264 Main", VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
              STD_VIDEO_H264_PROFILE_IDC_MAIN },
            { "H.265 Main", VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
              STD_VIDEO_H265_PROFILE_IDC_MAIN },
#if LIBAVCODEC_VERSION_MAJOR >= 61
            { "AV1 Main",   VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR,
              STD_VIDEO_AV1_PROFILE_MAIN },
#endif
        };
        for (const auto& probe : probes) {
            // §J.3.e.2.i.8 v1.3.193: profile INPUT pNext + caps OUTPUT pNext
            // 都需要 codec-specific struct.  上一版只接 input 沒接 output，
            // NV driver 看到 caps 的 pNext 沒對應 codec struct → rc=-3.
            //
            // 標準鏈接:
            //   profileInput.pNext → VkVideoDecode<Codec>ProfileInfoKHR
            //   caps.pNext         → VkVideoDecodeCapabilitiesKHR
            //                            → VkVideoDecode<Codec>CapabilitiesKHR
            VkVideoDecodeH264ProfileInfoKHR h264In  = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR };
            VkVideoDecodeH265ProfileInfoKHR h265In  = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR };
            VkVideoDecodeAV1ProfileInfoKHR  av1In   = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR };
            VkVideoDecodeH264CapabilitiesKHR h264Out = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR };
            VkVideoDecodeH265CapabilitiesKHR h265Out = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR };
            VkVideoDecodeAV1CapabilitiesKHR  av1Out  = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR };
            VkVideoProfileInfoKHR profInfo = {};
            profInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
            profInfo.videoCodecOperation = probe.op;
            profInfo.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
            profInfo.lumaBitDepth        = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
            profInfo.chromaBitDepth      = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

            VkVideoCapabilitiesKHR caps = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR };
            VkVideoDecodeCapabilitiesKHR decCaps = { VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR };
            caps.pNext = &decCaps;

            if (probe.op == VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) {
                h264In.stdProfileIdc = (StdVideoH264ProfileIdc)probe.profile;
                h264In.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
                profInfo.pNext = &h264In;
                decCaps.pNext = &h264Out;  // H.264-specific caps output
            } else if (probe.op == VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) {
                h265In.stdProfileIdc = (StdVideoH265ProfileIdc)probe.profile;
                profInfo.pNext = &h265In;
                decCaps.pNext = &h265Out;
            } else if (probe.op == VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) {
                av1In.stdProfile = (StdVideoAV1Profile)probe.profile;
                av1In.filmGrainSupport = VK_FALSE;
                profInfo.pNext = &av1In;
                decCaps.pNext = &av1Out;
            }
            VkResult vr = pfnGetVidCaps(m_PhysicalDevice, &profInfo, &caps);
            if (vr == VK_SUCCESS) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 codec %s: SUPPORTED "
                            "maxRefs=%u maxDPB=%u min=%ux%u max=%ux%u flags=%x "
                            "pictureGranularity=%ux%u",
                            probe.name,
                            caps.maxActiveReferencePictures, caps.maxDpbSlots,
                            caps.minCodedExtent.width, caps.minCodedExtent.height,
                            caps.maxCodedExtent.width, caps.maxCodedExtent.height,
                            (unsigned)decCaps.flags,
                            caps.pictureAccessGranularity.width,
                            caps.pictureAccessGranularity.height);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.8 codec %s: NOT SUPPORTED (rc=%d)",
                            probe.name, (int)vr);
            }
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkGetPhysicalDeviceVideoCapabilitiesKHR PFN missing — VK video decode probe skipped");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 picked '%s' (QF gfx=%u, decode=%u%s) %s",
                chosen.deviceName, m_QueueFamily,
                m_DecodeQueueFamily,
                m_DecodeQueueFamily == UINT32_MAX ? " — NO VIDEO DECODE QF!" : "",
                discreteIdx >= 0 ? "(DISCRETE_GPU)" : "(integrated/other)");
    return true;
}

bool VkFrucRenderer::createLogicalDevice()
{
    auto pfnCreateDevice = (PFN_vkCreateDevice)m_pfnGetInstanceProcAddr(
        m_Instance, "vkCreateDevice");
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnCreateDevice || !pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc PFN load failed");
        return false;
    }

    float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> qcis;
    {
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_QueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        qcis.push_back(qci);
    }
    if (m_DecodeQueueFamily != UINT32_MAX && m_DecodeQueueFamily != m_QueueFamily) {
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_DecodeQueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        qcis.push_back(qci);
    }

    // §J.3.e.2.i.7 HW path：mirror libplacebo's k_OptionalDeviceExtensions
    // (plvk.cpp:38-69) — FFmpeg hwcontext_vulkan 期待這些 ext 已 enable
    // 才能正確 build PFN dispatch table; v1.3.123-136 crash 的最可能 root
    // cause 就是我們提供的 ext list 缺東西，driver 內部 dispatch state mis-
    // init → NULL deref.
    //
    // 做法：query device 支援哪些，filter wanted ∩ available, enable 並把
    // 結果存到 m_EnabledDevExts class member —— populateAvHwDeviceCtx 會
    // 把這個傳給 vkCtx->enabled_dev_extensions, 必須完全一致.
    std::vector<const char*> wantedDevExts = {
        // Required for our own swapchain / ycbcr render path
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,

        // Video decode — FFmpeg-Vulkan hwcontext 必需
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
#if LIBAVCODEC_VERSION_MAJOR >= 61
        VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME,
#endif

        // Sync — FFmpeg 也會用
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,

        // libplacebo k_OptionalDeviceExtensions (plvk.cpp:38)：FFmpeg
        // hwcontext_vulkan 在 dispatch table 建構時會 lookup 這些 PFN.
        // 沒 enable 的話對應 PFN 是 NULL → NV driver dereference NULL.
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
#ifdef Q_OS_WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#endif
    };
    // §J.3.e.2.i.8 Phase 1.6 — opt-in NV device diagnostics extensions for
    // Aftermath crash dump.  Enables driver-side cmd buffer tracking so the
    // dump can pinpoint last in-flight work.  Only request when SDK is loaded
    // — otherwise the extra extensions just sit there inert.
    if (VipleAftermath::IsActive()) {
        wantedDevExts.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        wantedDevExts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    }

    // Query device extensions supported on this physical device, filter
    // wantedDevExts ∩ available.
    auto pfnEnumDevExts2 = (PFN_vkEnumerateDeviceExtensionProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumerateDeviceExtensionProperties");
    std::vector<VkExtensionProperties> devExtProps;
    if (pfnEnumDevExts2) {
        uint32_t devExtCount = 0;
        pfnEnumDevExts2(m_PhysicalDevice, nullptr, &devExtCount, nullptr);
        devExtProps.resize(devExtCount);
        pfnEnumDevExts2(m_PhysicalDevice, nullptr, &devExtCount, devExtProps.data());
    }
    auto extSupported = [&](const char* name) -> bool {
        for (const auto& p : devExtProps) {
            if (strcmp(p.extensionName, name) == 0) return true;
        }
        return false;
    };
    m_EnabledDevExts.clear();
    for (const char* e : wantedDevExts) {
        if (extSupported(e)) m_EnabledDevExts.push_back(e);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.7 HW-ext filter: %zu wanted, %zu enabled",
                wantedDevExts.size(), m_EnabledDevExts.size());

    std::vector<const char*>& devExts = m_EnabledDevExts;  // alias for following code

    // §J.3.e.2.i.3.a — feature chain stored in members so it persists for
    // FFmpeg's lifetime (vkCtx->device_features.pNext walks this chain
    // long after createLogicalDevice returns).  Mirrors what libplacebo
    // does in PlVkRenderer: query EVERYTHING the device supports via
    // vkGetPhysicalDeviceFeatures2 and enable it all when creating the
    // device.  FFmpeg's hwcontext_vulkan internal code paths assume the
    // device has full feature set (shaderImageGatherExtended,
    // fragmentStoresAndAtomics, etc.); enabling only a minimal subset
    // causes NV driver NULL deref in submitDecodeUnit (v1.3.123-135).
    m_Sync2Feat = {};
    m_Sync2Feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;

    m_TimelineFeat = {};
    m_TimelineFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    m_TimelineFeat.pNext = &m_Sync2Feat;

    m_YcbcrFeat = {};
    m_YcbcrFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    m_YcbcrFeat.pNext = &m_TimelineFeat;

    m_DevFeat2 = {};
    m_DevFeat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    m_DevFeat2.pNext = &m_YcbcrFeat;

    // Query the physical device for all supported features — populate
    // m_DevFeat2 + chain in-place.  Then we enable everything the device
    // says it supports.
    auto pfnGetPhysFeat2 = (PFN_vkGetPhysicalDeviceFeatures2)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceFeatures2");
    if (pfnGetPhysFeat2) {
        pfnGetPhysFeat2(m_PhysicalDevice, &m_DevFeat2);
    } else {
        // Fallback: hardcode the minimum set we know we need.
        m_Sync2Feat.synchronization2         = VK_TRUE;
        m_TimelineFeat.timelineSemaphore     = VK_TRUE;
        m_YcbcrFeat.samplerYcbcrConversion   = VK_TRUE;
    }

    // §J.3.e.2.i.8 Phase 1.6 — Aftermath device diagnostics config.  When
    // the matching extension is enabled, this struct tells the NV driver to
    // track shader debug info, automatic cmd buffer markers + resource
    // tracking — all of which surface in the GPU crash dump.  Only attached
    // when Aftermath is active; otherwise harmless to leave detached.
    VkDeviceDiagnosticsConfigCreateInfoNV diagCfg = {
        VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV
    };
    diagCfg.flags =
          VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV
        | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV
        | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV;
    bool diagCfgEnabled = false;
    for (const auto* ext : devExts) {
        if (strcmp(ext, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME) == 0) {
            diagCfgEnabled = true;
            break;
        }
    }
    if (diagCfgEnabled && VipleAftermath::IsActive()) {
        diagCfg.pNext = (void*)&m_DevFeat2;
    }

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = (diagCfgEnabled && VipleAftermath::IsActive())
                    ? (const void*)&diagCfg
                    : (const void*)&m_DevFeat2;
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    VkResult rc = pfnCreateDevice(m_PhysicalDevice, &dci, nullptr, &m_Device);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateDevice rc=%d", (int)rc);
        return false;
    }

    m_pfnDestroyDevice = (PFN_vkDestroyDevice)pfnGetDeviceProcAddr(m_Device, "vkDestroyDevice");
    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)pfnGetDeviceProcAddr(m_Device, "vkGetDeviceQueue");
    if (!m_pfnDestroyDevice || !pfnGetDeviceQueue) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc table incomplete");
        return false;
    }
    pfnGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_GraphicsQueue);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkDevice created (%s, QF=%u, queue=%p)",
                "KHR_swapchain + KHR_sampler_ycbcr_conversion",
                m_QueueFamily, (void*)m_GraphicsQueue);
    return true;
}

bool VkFrucRenderer::createSwapchain()
{
    auto pfnGetSurfCaps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto pfnGetSurfFmts = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    auto pfnGetSurfModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnGetSurfCaps || !pfnGetSurfFmts || !pfnGetSurfModes || !pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b PFN load (instance) failed");
        return false;
    }
    auto pfnCreateSwapchain = (PFN_vkCreateSwapchainKHR)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSwapchainKHR");
    auto pfnGetSwapImgs = (PFN_vkGetSwapchainImagesKHR)pfnGetDeviceProcAddr(
        m_Device, "vkGetSwapchainImagesKHR");
    auto pfnCreateImgView = (PFN_vkCreateImageView)pfnGetDeviceProcAddr(
        m_Device, "vkCreateImageView");
    if (!pfnCreateSwapchain || !pfnGetSwapImgs || !pfnCreateImgView) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b PFN load (device) failed");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps = {};
    if (pfnGetSurfCaps(m_PhysicalDevice, m_Surface, &caps) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
        return false;
    }

    // Image count.  Default: minImageCount + 1 for triple buffering.
    // Dual-present (i.4.2/i.5) needs at least 4 images so vkAcquireNextImageKHR
    // doesn't block — we acquire 2 per frame, present 2 per frame, with FIFO
    // only ~3 in flight at a time leaves no headroom for source-rate jitter.
    //
    // §J.3.e.2.i.8 Phase 1.5c — ONLY mode (synth-frame Pacer drive at network
    // packet rate, ~60-90fps) needs deeper swapchain to absorb submission /
    // present rate variance.  3 images @ IMMEDIATE present caused
    // vkAcquireNextImageKHR to fail with NOT_READY after ~60 frames in
    // v1.3.279 testing (no spare images while previous presents in flight).
    // Bump to minImageCount + 4 → typically 5-6 images with maxImageCount cap.
    uint32_t imageCount = caps.minImageCount + 1;
    if (m_DualMode) {
        // (std::max) parens defeat Windows.h max() macro pollution.
        uint32_t want = caps.minImageCount + 2;  // = 4 typically
        if (want > imageCount) imageCount = want;
    }
    // §J.3.e.2.i.8 Phase 1.7e — see ctor comment; env renamed to *_DANGEROUS.
    static const bool s_onlyModeForSwapDepth =
        qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS") != 0;
    if (s_onlyModeForSwapDepth) {
        uint32_t want = caps.minImageCount + 4;  // = ~6 typically
        if (want > imageCount) imageCount = want;
    }
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Extent: prefer surface's currentExtent; fall back to SDL's window
    // size if currentExtent is { 0xFFFFFFFF, 0xFFFFFFFF } (some drivers
    // signal "renderer chooses" with that value).
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(m_Window, &w, &h);
        extent.width  = (uint32_t)w;
        extent.height = (uint32_t)h;
    }
    m_SwapchainExtent = extent;

    // Format: pick BGRA8 SRGB / UNORM if available, else first.
    uint32_t fmtCount = 0;
    pfnGetSurfFmts(m_PhysicalDevice, m_Surface, &fmtCount, nullptr);
    if (fmtCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b no supported surface formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    pfnGetSurfFmts(m_PhysicalDevice, m_Surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM
            && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f; break;
        }
    }
    m_SwapchainFormat     = chosen.format;
    m_SwapchainColorSpace = chosen.colorSpace;

    // Present mode selection.
    //   • Single-present (no DUAL):    IMMEDIATE → MAILBOX → FIFO (low latency)
    //   • Dual-present (m_DualMode):   FIFO REQUIRED — IMMEDIATE makes the
    //     second present overwrite the first almost instantly, killing
    //     the interp+real alternation.  FIFO locks 2 presents to 2
    //     consecutive vsync slots → 60 Hz panel sees true 60fps display.
    //   • ONLY mode:                   MAILBOX → FIFO (avoid IMMEDIATE — at
    //     synth-frame submission rate of 60-90fps, IMMEDIATE backlogs the
    //     swapchain since each present is queued without replacing.  MAILBOX
    //     keeps the latest frame and discards older ones, so acquire never
    //     starves.  Falls back to FIFO if MAILBOX unsupported on this
    //     surface — also fine, lower latency penalty than IMMEDIATE backlog.)
    //   • VIPLE_VK_FRUC_VSYNC=1 forces FIFO regardless (legacy).
    uint32_t modeCount = 0;
    pfnGetSurfModes(m_PhysicalDevice, m_Surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    pfnGetSurfModes(m_PhysicalDevice, m_Surface, &modeCount, modes.data());
    VkPresentModeKHR pmode = VK_PRESENT_MODE_FIFO_KHR;  // always supported per spec
    bool wantVsync = m_DualMode || qEnvironmentVariableIntValue("VIPLE_VK_FRUC_VSYNC");
    // §J.3.e.2.i.8 Phase 1.7e — see ctor comment; env renamed to *_DANGEROUS.
    static const bool s_onlyModeForPresent =
        qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_DECODE_ONLY_DANGEROUS") != 0;
    if (s_onlyModeForPresent && !wantVsync) {
        // ONLY mode: prefer MAILBOX (no backlog) over IMMEDIATE (backlogs at
        // high submit rate → swapchain over-acquire).  FIFO as fallback.
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { pmode = m; break; }
        }
    } else if (!wantVsync) {
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { pmode = m; break; }
            if (m == VK_PRESENT_MODE_MAILBOX_KHR && pmode == VK_PRESENT_MODE_FIFO_KHR) {
                pmode = m;  // mailbox as 2nd choice
            }
        }
    } else if (m_DualMode) {
        // §J.3.e.2.i.7 R1: dual mode 試 FIFO_RELAXED — 跟 FIFO 一樣 vsync
        // 鎖定每個 present 到 slot，但 frame 遲到時允許 tearing 而非等下
        // 個 vsync.  目標降 p99/p99.9 jitter (baseline p99=21.57 ~2× mean,
        // 表示 1% cycles miss vsync slot).  FIFO_RELAXED 不一定所有 driver
        // 支援，找不到就 fallback 回普通 FIFO.
        for (auto m : modes) {
            if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) { pmode = m; break; }
        }
    }
    m_SwapchainPresentMode = pmode;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2.b chose present mode %d (%s)",
                (int)pmode,
                pmode == VK_PRESENT_MODE_IMMEDIATE_KHR    ? "IMMEDIATE (no vsync)" :
                pmode == VK_PRESENT_MODE_MAILBOX_KHR      ? "MAILBOX (vsync, no tearing)" :
                pmode == VK_PRESENT_MODE_FIFO_KHR         ? "FIFO (vsync locked)" :
                pmode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED (vsync, late tearing)" :
                "other");

    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = m_Surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.queueFamilyIndexCount = 0;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = pmode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    if (pfnCreateSwapchain(m_Device, &sci, nullptr, &m_Swapchain) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkCreateSwapchainKHR failed");
        return false;
    }

    uint32_t imgCnt = 0;
    pfnGetSwapImgs(m_Device, m_Swapchain, &imgCnt, nullptr);
    m_SwapchainImages.resize(imgCnt);
    pfnGetSwapImgs(m_Device, m_Swapchain, &imgCnt, m_SwapchainImages.data());

    m_SwapchainViews.resize(imgCnt, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imgCnt; i++) {
        VkImageViewCreateInfo ivCi = {};
        ivCi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivCi.image = m_SwapchainImages[i];
        ivCi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivCi.format = chosen.format;
        ivCi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivCi.subresourceRange.levelCount = 1;
        ivCi.subresourceRange.layerCount = 1;
        if (pfnCreateImgView(m_Device, &ivCi, nullptr, &m_SwapchainViews[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkCreateImageView failed (i=%u)", i);
            return false;
        }
    }

    // §J.3.e.2.i.8 Phase 1.5c-final — per-image renderDone binary semaphore.
    // One per swapchain image, indexed by image idx returned by
    // vkAcquireNextImageKHR.  Submit signals m_SwapchainRenderDoneSem[idx],
    // present waits the same sem.  Vulkan's swapchain reuse rule guarantees
    // image idx X won't be re-acquired until present consumed sem[X], so
    // sem reuse is naturally serialized — fixes VUID-vkQueueSubmit-
    // pSignalSemaphores-00067 in ONLY mode.
    auto pfnCreateSemaphore2 = (PFN_vkCreateSemaphore)pfnGetDeviceProcAddr(m_Device, "vkCreateSemaphore");
    if (!pfnCreateSemaphore2) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2.b vkCreateSemaphore PFN missing");
        return false;
    }
    m_SwapchainRenderDoneSem.assign(imgCnt, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < imgCnt; i++) {
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pfnCreateSemaphore2(m_Device, &sci, nullptr, &m_SwapchainRenderDoneSem[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.5c-final per-image renderDone sem[%u] create failed", i);
            return false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2.b VkSwapchainKHR created "
                "(format=%d colorSpace=%d extent=%ux%u images=%u presentMode=%d)",
                (int)chosen.format, (int)chosen.colorSpace,
                extent.width, extent.height, imgCnt, (int)pmode);
    return true;
}

void VkFrucRenderer::destroySwapchain()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyImgView = (PFN_vkDestroyImageView)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyImageView");
    auto pfnDestroySwapchain = (PFN_vkDestroySwapchainKHR)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySwapchainKHR");
    auto pfnDestroySem = (PFN_vkDestroySemaphore)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySemaphore");
    // §J.3.e.2.i.8 Phase 1.5c-final — per-image renderDone sems
    for (auto s : m_SwapchainRenderDoneSem) {
        if (s != VK_NULL_HANDLE && pfnDestroySem)
            pfnDestroySem(m_Device, s, nullptr);
    }
    m_SwapchainRenderDoneSem.clear();
    for (auto v : m_SwapchainViews) {
        if (v != VK_NULL_HANDLE && pfnDestroyImgView)
            pfnDestroyImgView(m_Device, v, nullptr);
    }
    m_SwapchainViews.clear();
    m_SwapchainImages.clear();
    if (m_Swapchain != VK_NULL_HANDLE && pfnDestroySwapchain) {
        pfnDestroySwapchain(m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    if (!createInstanceAndSurface(m_Window)) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!pickPhysicalDeviceAndQueue()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createLogicalDevice()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createSwapchain()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    m_VideoFormat = params->videoFormat;
    // §J.3.e.2.i.3.e-SW — skip AVHWDeviceContext + AVVulkanDeviceContext
    // setup when in software-upload mode; we don't bridge ffmpeg to our
    // VkDevice in that path (frames come in CPU memory as AV_PIX_FMT_NV12).
    if (!m_SwMode) {
        if (!populateAvHwDeviceCtx(m_VideoFormat)) {
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            teardown();
            return false;
        }
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW skipping AVHWDeviceContext "
                    "(software upload mode active)");
    }

    // §J.3.e.2.i.8 — native VK_KHR_video_decode setup is INDEPENDENT of the
    // FFmpeg-Vulkan bridge path (m_SwMode flag).  SW mode does FFmpeg→staging
    // upload for the *display* path, while native decode replaces the *decode*
    // step entirely (NAL bytes → vkCmdDecodeVideoKHR).  These two pipelines
    // don't share VkImages, so we always provision native decode resources
    // when the device exposes the required extensions; whether NAL units
    // actually route through them is gated by acceptsNativeDecode() (env var
    // VIPLE_VKFRUC_NATIVE_DECODE=1).
    //
    // Until v1.3.221 this whole block lived inside `if (!m_SwMode)` — meaning
    // native decode never instantiated when the user (typical) ran in SW
    // mode.  The fix: lift it out so Phase 1.3d.1 vkCmdDecodeVideoKHR submits
    // are reachable from the SW display pipeline as well.
    if (!createVideoSession(m_VideoFormat)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createVideoSession failed — Phase 1 native decode 路徑 skip");
        destroyVideoSession();
    } else if (!createVideoSessionParameters(m_VideoFormat)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createVideoSessionParameters failed");
        destroyVideoSession();
    } else if (!createDecodeCommandResources()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createDecodeCommandResources failed");
        destroyDecodeCommandResources();
        destroyVideoSession();
    } else if (!createNvVideoParser()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 createNvVideoParser failed — fallback to legacy NAL type counter");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 native decode chain READY "
                    "— session+params+cmd+parser+DPB-pool. Awaiting VIPLE_VKFRUC_NATIVE_DECODE=1 + NAL packets.");
    }
    if (!createYcbcrSamplerAndLayouts()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createRenderPassAndFramebuffers()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createGraphicsPipeline()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createInFlightRing()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createDescriptorPool()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!loadRenderTimePfns()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }

    // §J.3.e.2.i — overlay 資源（pipeline/shader/sampler/desc pool/spinlock
    // state）.  v1.3.171 一度懷疑 createOverlayResources 觸發 VK_ERROR_
    // DEVICE_LOST 而加 VIPLE_VKFRUC_OVERLAY env-var gate，但 v1.3.172~174
    // 五次測試都沒重現，確認當時是 transient driver state.  v1.3.175 拆
    // gate，跟著 RS_VULKAN 預設一起 default-on，使用者不必設環境變數
    // 也能用 Ctrl+Alt+Shift+S 開效能 overlay.  失敗仍 non-fatal — 只是
    // overlay 看不到，串流照跑.
    if (!createOverlayResources()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i overlay 資源建立失敗，效能資訊 overlay 將不會顯示");
        destroyOverlayResources();  // best-effort 清掉部分建好的 handle
    }

    // §J.3.e.2.i.3.e-SW — allocate upload buffer + image when in software
    // mode.  Size from params.  We use the source resolution; if the
    // stream switches resolution mid-session we'd need to recreate, but
    // moonlight streams stay at requested resolution for the session.
    if (m_SwMode) {
        if (!createSwUploadResources(params->width, params->height)) {
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            teardown();
            return false;
        }

        // [VIPLE-NET-WARN] Startup-time warn for the verified decoder-throughput
        // cap combo: SW HEVC + 1440p+ + 90+fps caps at ~50fps real on this class
        // of mobile CPU.  The runtime [VIPLE-NET-WARN] in ffmpeg.cpp also catches
        // this after 5s, but firing at startup gives the user the actionable hint
        // before they wait for frame drops.  See project_hevc_1440p_decode_cap
        // memory + docs/TODO.md §J HEVC 1440p120 decoder-throughput cap.
        const bool isHevc = (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0;
        const bool highRes = params->height >= 1440;
        const bool highFps = params->frameRate >= 90;
        if (isHevc && highRes && highFps) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-NET-WARN] SW HEVC %dx%d@%dfps via VkFrucRenderer is known "
                        "to cap at ~50fps real on mobile CPU classes (libavcodec FRAME-thread "
                        "throughput limit, NOT packet loss).  For full %dfps consider: switch "
                        "to default D3D11 renderer (uses NVDEC/DXVA HW decode), reduce to "
                        "1080p, or pick H.264/AV1 codec.",
                        params->width, params->height, params->frameRate, params->frameRate);
        }
    }

    // §J.3.e.2.i.4 — FRUC compute pipeline (motion estimate + median filter
    // + warp).  Independent of SW vs HW path; runs on storage buffers in
    // either mode.  Only allocate when m_FrucMode is set (env-var gated).
    if (m_FrucMode) {
        if (!createFrucComputeResources(params->width, params->height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4 init failed — disabling FRUC for "
                        "this session (renderer keeps running without compute chain)");
            // Don't fail init; just disable FRUC for this session.
            m_FrucMode = false;
            m_DualMode = false;
        }
    }

    // §J.3.e.2.i.4.2 / i.5 — interp graphics pipeline (depends on FRUC
    // compute being ready, since it samples m_FrucInterpRgbBuf).
    if (m_DualMode && m_FrucMode) {
        if (!createInterpGraphicsPipeline()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 interp graphics pipeline init "
                        "failed — disabling dual-present");
            m_DualMode = false;
        }
    } else if (m_DualMode && !m_FrucMode) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 VIPLE_VKFRUC_DUAL=1 requires "
                    "VIPLE_VKFRUC_FRUC=1 — disabling dual-present");
        m_DualMode = false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e initialize SUCCESS: instance/PD/device/"
                "swapchain + %s + ycbcr-sampler+layouts + render pass + graphics pipeline"
                " + in-flight ring + descriptor pool + render-time PFNs%s"
                " — renderFrame is now live",
                m_SwMode ? "SW upload buffer/image (no AVHWDeviceContext)"
                         : "AVHWDeviceContext",
                m_SwMode ? " (SW mode: AV_PIX_FMT_NV12 from CPU memory)"
                         : " (HW mode: AV_PIX_FMT_VULKAN)");

    return true;
}

// §J.3.e.2.i.3.c — pre-compiled SPIR-V for fullscreen-triangle vertex
// shader + NV12 ycbcr-sampler fragment shader.  Sources live in
// vkfruc-shaders/vkfruc.{vert,frag}; build_shaders.cmd compiles them via
// glslc (Android NDK / Vulkan SDK) into .spv + .spv.h.
//
// Pattern matches moonlight-android (.spv.h files are checked-in so end
// users do NOT need to run build_shaders.cmd).  ncnn::compile_spirv_module
// can't be reused here — it's hardcoded to compute stage; pre-compile
// avoids vendoring glslang into the renderer.
#include "vkfruc-shaders/vkfruc.vert.spv.h"
#include "vkfruc-shaders/vkfruc.frag.spv.h"
#include "vkfruc-shaders/vkfruc_interp.frag.spv.h"
#include "vkfruc-shaders/vkfruc_overlay.frag.spv.h"

#include "streaming/streamutils.h"  // not strictly needed, but for any utility
#include "streaming/session.h"

bool VkFrucRenderer::createRenderPassAndFramebuffers()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateRenderPass = (PFN_vkCreateRenderPass)pfnGetDeviceProcAddr(
        m_Device, "vkCreateRenderPass");
    auto pfnCreateFB = (PFN_vkCreateFramebuffer)pfnGetDeviceProcAddr(
        m_Device, "vkCreateFramebuffer");
    if (!pfnCreateRenderPass || !pfnCreateFB) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c PFN load (renderpass) failed");
        return false;
    }

    VkAttachmentDescription colorAttach = {};
    colorAttach.format = m_SwapchainFormat;
    colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // Subpass dependency: external → subpass 0, ensuring the swapchain
    // image acquire has happened before color attachment write.
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCi = {};
    rpCi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCi.attachmentCount = 1;
    rpCi.pAttachments = &colorAttach;
    rpCi.subpassCount = 1;
    rpCi.pSubpasses = &subpass;
    rpCi.dependencyCount = 1;
    rpCi.pDependencies = &dep;

    if (pfnCreateRenderPass(m_Device, &rpCi, nullptr, &m_RenderPass) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateRenderPass failed");
        return false;
    }

    m_Framebuffers.resize(m_SwapchainViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m_SwapchainViews.size(); i++) {
        VkFramebufferCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = m_RenderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &m_SwapchainViews[i];
        fci.width = m_SwapchainExtent.width;
        fci.height = m_SwapchainExtent.height;
        fci.layers = 1;
        if (pfnCreateFB(m_Device, &fci, nullptr, &m_Framebuffers[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateFramebuffer #%zu failed", i);
            return false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.c render pass + %zu framebuffers ready",
                m_Framebuffers.size());
    return true;
}

void VkFrucRenderer::destroyRenderPassAndFramebuffers()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyFB = (PFN_vkDestroyFramebuffer)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyFramebuffer");
    auto pfnDestroyRP = (PFN_vkDestroyRenderPass)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyRenderPass");
    for (auto fb : m_Framebuffers) {
        if (fb && pfnDestroyFB) pfnDestroyFB(m_Device, fb, nullptr);
    }
    m_Framebuffers.clear();
    if (m_RenderPass && pfnDestroyRP) {
        pfnDestroyRP(m_Device, m_RenderPass, nullptr);
        m_RenderPass = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::createGraphicsPipeline()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkCreateShaderModule");
    auto pfnCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)pfnGetDeviceProcAddr(
        m_Device, "vkCreateGraphicsPipelines");
    auto pfnDestroyShaderModule = (PFN_vkDestroyShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyShaderModule");
    if (!pfnCreateShaderModule || !pfnCreateGraphicsPipelines || !pfnDestroyShaderModule) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c PFN load (pipeline) failed");
        return false;
    }

    // §J.3.e.2.i.3.c — load pre-compiled SPIR-V.  xxd emits unsigned char
    // arrays; SPIR-V is little-endian uint32 and the byte layout in the
    // .spv file matches little-endian uint32 directly, so reinterpret_cast
    // is fine on every supported target.  The _len constant is the size
    // in BYTES (must be multiple of 4 — guaranteed by glslc).
    auto buildShader = [&](const char* tag,
                           const unsigned char* spv,
                           unsigned int spvLen,
                           VkShaderModule& outMod) -> bool {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spvLen;
        smCi.pCode = reinterpret_cast<const uint32_t*>(spv);
        VkResult vr = pfnCreateShaderModule(m_Device, &smCi, nullptr, &outMod);
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateShaderModule(%s) "
                         "failed (%d)", tag, (int)vr);
            return false;
        }
        return true;
    };

    if (!buildShader("vert", vkfruc_vert_spv, vkfruc_vert_spv_len, m_VertShaderModule)) {
        return false;
    }
    if (!buildShader("frag", vkfruc_frag_spv, vkfruc_frag_spv_len, m_FragShaderModule)) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertShaderModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_FragShaderModule;
    stages[1].pName  = "main";

    // No vertex buffer — fullscreen triangle uses gl_VertexIndex only.
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = {};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)m_SwapchainExtent.width;
    viewport.height   = (float)m_SwapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = {};
    scissor.offset = { 0, 0 };
    scissor.extent = m_SwapchainExtent;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports    = &viewport;
    vp.scissorCount  = 1;
    vp.pScissors     = &scissor;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;          // 3-vertex cover triangle, no culling
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount  = 1;
    cb.pAttachments     = &cba;

    // Dynamic state: viewport + scissor so swapchain resize doesn't force
    // pipeline recreate.  i.3.c-first-ship will use static viewport above
    // (resize handled by destroying & re-creating swapchain + pipeline);
    // i.6+ may switch to fully dynamic.  For now keep it simple — declare
    // no dynamic state.

    VkGraphicsPipelineCreateInfo gpCi = {};
    gpCi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCi.stageCount          = 2;
    gpCi.pStages             = stages;
    gpCi.pVertexInputState   = &vi;
    gpCi.pInputAssemblyState = &ia;
    gpCi.pViewportState      = &vp;
    gpCi.pRasterizationState = &rs;
    gpCi.pMultisampleState   = &ms;
    gpCi.pColorBlendState    = &cb;
    gpCi.layout              = m_GraphicsPipelineLayout;
    gpCi.renderPass          = m_RenderPass;
    gpCi.subpass             = 0;

    VkResult vr = pfnCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE,
                                             1, &gpCi, nullptr,
                                             &m_GraphicsPipeline);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.c vkCreateGraphicsPipelines "
                     "failed (%d)", (int)vr);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.c graphics pipeline ready "
                "(vert=%u B, frag=%u B SPIR-V)",
                (unsigned)vkfruc_vert_spv_len,
                (unsigned)vkfruc_frag_spv_len);
    return true;
}

void VkFrucRenderer::destroyGraphicsPipeline()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyShader = (PFN_vkDestroyShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyShaderModule");
    auto pfnDestroyPipeline = (PFN_vkDestroyPipeline)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyPipeline");
    if (m_GraphicsPipeline && pfnDestroyPipeline) {
        pfnDestroyPipeline(m_Device, m_GraphicsPipeline, nullptr);
        m_GraphicsPipeline = VK_NULL_HANDLE;
    }
    if (m_FragShaderModule && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_FragShaderModule, nullptr);
        m_FragShaderModule = VK_NULL_HANDLE;
    }
    if (m_VertShaderModule && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_VertShaderModule, nullptr);
        m_VertShaderModule = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.4.2 / §J.3.e.2.i.5 — second graphics pipeline that displays
// m_FrucInterpRgbBuf (planar fp32 RGB storage buffer) via a fragment
// shader that reads the buffer directly.  Reuses the same vert shader as
// the real-frame pipeline.
//
// Different from m_GraphicsPipeline because:
//   • Different DSL: storage buffer (binding 0) instead of combined image
//     sampler with immutable ycbcr conversion
//   • Different push constant range (16 bytes: srcW + srcH)
//   • Different fragment shader (vkfruc_interp.frag)
bool VkFrucRenderer::createInterpGraphicsPipeline()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)pfnGetDeviceProcAddr(
        m_Device, "vkCreateShaderModule");
    auto pfnCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)pfnGetDeviceProcAddr(
        m_Device, "vkCreateGraphicsPipelines");
    auto pfnCreateDsl = (PFN_vkCreateDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePL = (PFN_vkCreatePipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreatePipelineLayout");
    auto pfnCreateDescPool = (PFN_vkCreateDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDescSets = (PFN_vkAllocateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets = (PFN_vkUpdateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkUpdateDescriptorSets");
    if (!pfnCreateShaderModule || !pfnCreateGraphicsPipelines || !pfnCreateDsl
        || !pfnCreatePL || !pfnCreateDescPool || !pfnAllocDescSets || !pfnUpdateDescSets) {
        return false;
    }

    // ---- Create interp frag shader module ----
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = vkfruc_interp_frag_spv_len;
        smCi.pCode = reinterpret_cast<const uint32_t*>(vkfruc_interp_frag_spv);
        if (pfnCreateShaderModule(m_Device, &smCi, nullptr, &m_InterpFragShaderMod) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 vkCreateShaderModule(interp frag) failed");
            return false;
        }
    }

    // ---- DSL: 1 storage buffer at binding 0 (interpRGB) ----
    VkDescriptorSetLayoutBinding dslB = {};
    dslB.binding = 0;
    dslB.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    dslB.descriptorCount = 1;
    dslB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 1;
    dslCi.pBindings = &dslB;
    if (pfnCreateDsl(m_Device, &dslCi, nullptr, &m_InterpDescSetLayout) != VK_SUCCESS) return false;

    // ---- Pipeline layout with 16-byte push const (srcW, srcH) ----
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = 16;
    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &m_InterpDescSetLayout;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges = &pcRange;
    if (pfnCreatePL(m_Device, &plCi, nullptr, &m_InterpPipelineLayout) != VK_SUCCESS) return false;

    // ---- Graphics pipeline (reuses vert shader from m_GraphicsPipeline) ----
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertShaderModule;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_InterpFragShaderMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {};
    viewport.width    = (float)m_SwapchainExtent.width;
    viewport.height   = (float)m_SwapchainExtent.height;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = { {0,0}, m_SwapchainExtent };
    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports    = &viewport;
    vp.scissorCount  = 1;
    vp.pScissors     = &scissor;
    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType            = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount  = 1;
    cb.pAttachments     = &cba;

    VkGraphicsPipelineCreateInfo gpCi = {};
    gpCi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCi.stageCount          = 2;
    gpCi.pStages             = stages;
    gpCi.pVertexInputState   = &vi;
    gpCi.pInputAssemblyState = &ia;
    gpCi.pViewportState      = &vp;
    gpCi.pRasterizationState = &rs;
    gpCi.pMultisampleState   = &ms;
    gpCi.pColorBlendState    = &cb;
    gpCi.layout              = m_InterpPipelineLayout;
    gpCi.renderPass          = m_RenderPass;  // shared render pass
    gpCi.subpass             = 0;
    if (pfnCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &gpCi, nullptr,
                                    &m_InterpPipeline) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 vkCreateGraphicsPipelines(interp) failed");
        return false;
    }

    // ---- Descriptor pool + set bound to interpRGB ----
    VkDescriptorPoolSize ps = {};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 1;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &ps;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_InterpDescPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo asi = {};
    asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    asi.descriptorPool = m_InterpDescPool;
    asi.descriptorSetCount = 1;
    asi.pSetLayouts = &m_InterpDescSetLayout;
    if (pfnAllocDescSets(m_Device, &asi, &m_InterpDescSet) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bi = {};
    bi.buffer = m_FrucInterpRgbBuf;   // bound by FRUC compute init
    bi.offset = 0;
    bi.range  = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wds = {};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = m_InterpDescSet;
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds.pBufferInfo     = &bi;
    pfnUpdateDescSets(m_Device, 1, &wds, 0, nullptr);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4.2 interp pipeline ready (frag=%u B SPIR-V)",
                (unsigned)vkfruc_interp_frag_spv_len);
    return true;
}

void VkFrucRenderer::destroyInterpGraphicsPipeline()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyShader   = (PFN_vkDestroyShaderModule)getDevPa(m_Device, "vkDestroyShaderModule");
    auto pfnDestroyPipeline = (PFN_vkDestroyPipeline)getDevPa(m_Device, "vkDestroyPipeline");
    auto pfnDestroyPL       = (PFN_vkDestroyPipelineLayout)getDevPa(m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl      = (PFN_vkDestroyDescriptorSetLayout)getDevPa(m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroyDescPool = (PFN_vkDestroyDescriptorPool)getDevPa(m_Device, "vkDestroyDescriptorPool");
    if (m_InterpDescPool && pfnDestroyDescPool) {
        pfnDestroyDescPool(m_Device, m_InterpDescPool, nullptr);
        m_InterpDescPool = VK_NULL_HANDLE;
        m_InterpDescSet  = VK_NULL_HANDLE;
    }
    if (m_InterpPipeline && pfnDestroyPipeline) {
        pfnDestroyPipeline(m_Device, m_InterpPipeline, nullptr);
        m_InterpPipeline = VK_NULL_HANDLE;
    }
    if (m_InterpPipelineLayout && pfnDestroyPL) {
        pfnDestroyPL(m_Device, m_InterpPipelineLayout, nullptr);
        m_InterpPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_InterpDescSetLayout && pfnDestroyDsl) {
        pfnDestroyDsl(m_Device, m_InterpDescSetLayout, nullptr);
        m_InterpDescSetLayout = VK_NULL_HANDLE;
    }
    if (m_InterpFragShaderMod && pfnDestroyShader) {
        pfnDestroyShader(m_Device, m_InterpFragShaderMod, nullptr);
        m_InterpFragShaderMod = VK_NULL_HANDLE;
    }
}

// =====================================================================
// §J.3.e.2.i — performance overlay rendering (Ctrl+Alt+Shift+S 效能資訊)
//
// moonlight-qt's OverlayManager renders overlay text into SDL_Surface
// (RGBA8/ARGB8888).  Our notifyOverlayUpdated handler grabs the surface,
// uploads to a per-overlay-type VkImage via host-coherent staging buffer,
// and drawOverlayInRenderPass composites it onto the swapchain via a
// fullscreen-tri pipeline whose frag shader discards outside the rect
// (alpha-blended onto the underlying video draw of the same render pass).
// =====================================================================

bool VkFrucRenderer::createOverlayResources()
{
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)getDevPa(m_Device, "vkCreateShaderModule");
    auto pfnCreateSampler      = (PFN_vkCreateSampler)getDevPa(m_Device, "vkCreateSampler");
    auto pfnCreateDsl          = (PFN_vkCreateDescriptorSetLayout)getDevPa(m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePL           = (PFN_vkCreatePipelineLayout)getDevPa(m_Device, "vkCreatePipelineLayout");
    auto pfnCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)getDevPa(m_Device, "vkCreateGraphicsPipelines");
    auto pfnCreateDescPool     = (PFN_vkCreateDescriptorPool)getDevPa(m_Device, "vkCreateDescriptorPool");
    if (!pfnCreateShaderModule || !pfnCreateSampler || !pfnCreateDsl
        || !pfnCreatePL || !pfnCreateGraphicsPipelines || !pfnCreateDescPool) return false;

    // Frag shader (vkfruc_overlay.frag pre-compiled)
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = vkfruc_overlay_frag_spv_len;
        smCi.pCode = reinterpret_cast<const uint32_t*>(vkfruc_overlay_frag_spv);
        if (pfnCreateShaderModule(m_Device, &smCi, nullptr, &m_OverlayFragShaderMod) != VK_SUCCESS) return false;
    }

    // Linear sampler (no ycbcr, no mipmap)
    {
        VkSamplerCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (pfnCreateSampler(m_Device, &sci, nullptr, &m_OverlaySampler) != VK_SUCCESS) return false;
    }

    // DSL: 1 combined image sampler at binding 0 (NOT immutable — different
    // texture per overlay type)
    VkDescriptorSetLayoutBinding dslB = {};
    dslB.binding = 0;
    dslB.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslB.descriptorCount = 1;
    dslB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 1;
    dslCi.pBindings = &dslB;
    if (pfnCreateDsl(m_Device, &dslCi, nullptr, &m_OverlayDescSetLayout) != VK_SUCCESS) return false;

    // Pipeline layout: 16-byte push const for rect [vec2 min, vec2 max]
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcRange.size = 16;
    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &m_OverlayDescSetLayout;
    plCi.pushConstantRangeCount = 1;
    plCi.pPushConstantRanges = &pcRange;
    if (pfnCreatePL(m_Device, &plCi, nullptr, &m_OverlayPipelineLayout) != VK_SUCCESS) return false;

    // Graphics pipeline: reuse vert shader, alpha blending on, no depth.
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_VertShaderModule;  // reused fullscreen tri
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_OverlayFragShaderMod;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {};
    viewport.width = (float)m_SwapchainExtent.width;
    viewport.height = (float)m_SwapchainExtent.height;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor = { {0,0}, m_SwapchainExtent };
    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.pViewports = &viewport;
    vp.scissorCount = 1;  vp.pScissors  = &scissor;
    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Alpha blending (overlay composited over video)
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpCi = {};
    gpCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpCi.stageCount = 2;
    gpCi.pStages = stages;
    gpCi.pVertexInputState = &vi;
    gpCi.pInputAssemblyState = &ia;
    gpCi.pViewportState = &vp;
    gpCi.pRasterizationState = &rs;
    gpCi.pMultisampleState = &ms;
    gpCi.pColorBlendState = &cb;
    gpCi.layout = m_OverlayPipelineLayout;
    gpCi.renderPass = m_RenderPass;
    gpCi.subpass = 0;
    if (pfnCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &gpCi, nullptr,
                                    &m_OverlayPipeline) != VK_SUCCESS) return false;

    // Descriptor pool: kOverlayMax sets, one COMBINED_IMAGE_SAMPLER each
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kOverlayMax;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = kOverlayMax;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &poolSize;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_OverlayDescPool) != VK_SUCCESS) return false;

    // §J.3.e.2.i overlay：m_OverlayLock 是 zero-initialised int (header) —
    // SDL_AtomicLock spinlock 不需要顯式 create / destroy。

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i overlay resources ready (frag=%u B SPIR-V, atomic-lock)",
                (unsigned)vkfruc_overlay_frag_spv_len);
    return true;
}

void VkFrucRenderer::destroyOverlayResources()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyImage   = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnDestroyView    = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnDestroyBuffer  = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem        = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnUnmapMem       = (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory");
    auto pfnDestroyPipe    = (PFN_vkDestroyPipeline)getDevPa(m_Device, "vkDestroyPipeline");
    auto pfnDestroyPL      = (PFN_vkDestroyPipelineLayout)getDevPa(m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl     = (PFN_vkDestroyDescriptorSetLayout)getDevPa(m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroyShader  = (PFN_vkDestroyShaderModule)getDevPa(m_Device, "vkDestroyShaderModule");
    auto pfnDestroySampler = (PFN_vkDestroySampler)getDevPa(m_Device, "vkDestroySampler");
    auto pfnDestroyDescPool = (PFN_vkDestroyDescriptorPool)getDevPa(m_Device, "vkDestroyDescriptorPool");

    for (uint32_t i = 0; i < kOverlayMax; i++) {
        if (m_OverlayView[i] && pfnDestroyView)   { pfnDestroyView(m_Device, m_OverlayView[i], nullptr); m_OverlayView[i] = VK_NULL_HANDLE; }
        if (m_OverlayImage[i] && pfnDestroyImage) { pfnDestroyImage(m_Device, m_OverlayImage[i], nullptr); m_OverlayImage[i] = VK_NULL_HANDLE; }
        if (m_OverlayMem[i] && pfnFreeMem)        { pfnFreeMem(m_Device, m_OverlayMem[i], nullptr); m_OverlayMem[i] = VK_NULL_HANDLE; }
        if (m_OverlayStagingMapped[i] && pfnUnmapMem) { pfnUnmapMem(m_Device, m_OverlayStagingMem[i]); m_OverlayStagingMapped[i] = nullptr; }
        if (m_OverlayStagingBuf[i] && pfnDestroyBuffer) { pfnDestroyBuffer(m_Device, m_OverlayStagingBuf[i], nullptr); m_OverlayStagingBuf[i] = VK_NULL_HANDLE; }
        if (m_OverlayStagingMem[i] && pfnFreeMem) { pfnFreeMem(m_Device, m_OverlayStagingMem[i], nullptr); m_OverlayStagingMem[i] = VK_NULL_HANDLE; }
        m_OverlayDescSet[i] = VK_NULL_HANDLE;  // freed implicitly with pool
    }
    if (m_OverlayDescPool && pfnDestroyDescPool) { pfnDestroyDescPool(m_Device, m_OverlayDescPool, nullptr); m_OverlayDescPool = VK_NULL_HANDLE; }
    if (m_OverlayPipeline && pfnDestroyPipe)     { pfnDestroyPipe(m_Device, m_OverlayPipeline, nullptr); m_OverlayPipeline = VK_NULL_HANDLE; }
    if (m_OverlayPipelineLayout && pfnDestroyPL) { pfnDestroyPL(m_Device, m_OverlayPipelineLayout, nullptr); m_OverlayPipelineLayout = VK_NULL_HANDLE; }
    if (m_OverlayDescSetLayout && pfnDestroyDsl) { pfnDestroyDsl(m_Device, m_OverlayDescSetLayout, nullptr); m_OverlayDescSetLayout = VK_NULL_HANDLE; }
    if (m_OverlayFragShaderMod && pfnDestroyShader) { pfnDestroyShader(m_Device, m_OverlayFragShaderMod, nullptr); m_OverlayFragShaderMod = VK_NULL_HANDLE; }
    if (m_OverlaySampler && pfnDestroySampler)   { pfnDestroySampler(m_Device, m_OverlaySampler, nullptr); m_OverlaySampler = VK_NULL_HANDLE; }

    // §J.3.e.2.i — drain stashed surfaces under spinlock.  By此時
    // OverlayManager 已經 setOverlayRenderer(nullptr)（見 ffmpeg.cpp:294），
    // 不會再有新 callback 進來；spinlock 只是保護任何 in-flight
    // notifyOverlayUpdated 完成。SDL_AtomicLock 是純 int spinlock，沒有
    // destroy 步驟 —— 這裡若拿不到 lock 就 spin，但因為 deregister 已經
    // 發生，最多 spin 一個 in-flight call 的時間（μs 等級）。
    SDL_AtomicLock(&m_OverlayLock);
    for (uint32_t i = 0; i < kOverlayMax; i++) {
        if (m_OverlayStashedSurface[i]) {
            SDL_FreeSurface(m_OverlayStashedSurface[i]);
            m_OverlayStashedSurface[i] = nullptr;
        }
        m_OverlayStashedDisable[i] = false;
    }
    SDL_AtomicUnlock(&m_OverlayLock);
}

void VkFrucRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    // §J.3.e.2.i overlay — 跟 D3D11VARenderer::notifyOverlayUpdated 一樣，
    // 這個 callback 可能在任意 thread 進來（OverlayManager 在 render thread
    // 之外的 SDL event/UI thread 觸發），所以 **絕對不能** 碰 cmd buffer
    // 或 queue submit.  我們只做兩件事：
    //   1. 從 OverlayManager 把新 SDL_Surface「atomic-swap 出來」（拿走
    //      所有權，OverlayManager 之後會自己 free 上一張舊的）
    //   2. 在 spinlock 內把 surface 塞到 stash slot；如果 slot 已有舊的
    //      stash，直接 free 它（沒被 drain 到的就是過時的）
    //
    // 真正的 GPU 工作（vkCreateImage/vkAllocateMemory/cmdCopyBufferToImage/
    // barriers）全部在 render thread 的 drainOverlayStash + uploadPendingOverlay
    // 做.  notifyOverlayUpdated → drainOverlayStash 透過 SDL_AtomicLock
    // 同步.
    if (type >= kOverlayMax) return;

    Overlay::OverlayManager& mgr = Session::get()->getOverlayManager();
    SDL_Surface* newSurface  = mgr.getUpdatedOverlaySurface(type);  // ownership transfers to us
    bool         enabledNow  = mgr.isOverlayEnabled(type);

    SDL_AtomicLock(&m_OverlayLock);
    // 取代任何 in-flight 但未被 drain 的 stash.
    if (m_OverlayStashedSurface[type]) {
        SDL_FreeSurface(m_OverlayStashedSurface[type]);
        m_OverlayStashedSurface[type] = nullptr;
    }
    if (newSurface) {
        m_OverlayStashedSurface[type] = newSurface;
        m_OverlayStashedDisable[type] = false;
    } else if (!enabledNow) {
        // overlay 剛被關掉 (Ctrl+Alt+Shift+D 第二下), 透過 disable flag
        // 通知 drain 把 m_OverlayHasContent 清掉.
        m_OverlayStashedDisable[type] = true;
    }
    SDL_AtomicUnlock(&m_OverlayLock);
}

void VkFrucRenderer::drainOverlayStash()
{
    // §J.3.e.2.i — runs on render thread; safe to do Vulkan work.
    if (!m_OverlayPipeline) return;

    // 用 SDL_AtomicTryLock —— 拿不到 lock 就跳這幀，下一幀再試.  D3D11VA 的
    // renderOverlay 也是同樣 pattern (d3d11va.cpp:1260)，避免 render block.
    SDL_Surface* surfaces[kOverlayMax] = {};
    bool disables[kOverlayMax] = {};
    if (!SDL_AtomicTryLock(&m_OverlayLock)) return;
    for (uint32_t i = 0; i < kOverlayMax; i++) {
        surfaces[i] = m_OverlayStashedSurface[i];
        disables[i] = m_OverlayStashedDisable[i];
        m_OverlayStashedSurface[i] = nullptr;
        m_OverlayStashedDisable[i] = false;
    }
    SDL_AtomicUnlock(&m_OverlayLock);

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateImage = (PFN_vkCreateImage)getDevPa(m_Device, "vkCreateImage");
    auto pfnGetImgMemReq = (PFN_vkGetImageMemoryRequirements)getDevPa(m_Device, "vkGetImageMemoryRequirements");
    auto pfnAllocMem = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindImgMem = (PFN_vkBindImageMemory)getDevPa(m_Device, "vkBindImageMemory");
    auto pfnCreateView = (PFN_vkCreateImageView)getDevPa(m_Device, "vkCreateImageView");
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnBindBufMem = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnMapMem = (PFN_vkMapMemory)getDevPa(m_Device, "vkMapMemory");
    auto pfnGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    auto pfnAllocDescSets = (PFN_vkAllocateDescriptorSets)getDevPa(m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets = (PFN_vkUpdateDescriptorSets)getDevPa(m_Device, "vkUpdateDescriptorSets");
    auto pfnDestroyImage = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnDestroyView = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnUnmapMem = (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory");
    if (!pfnCreateImage || !pfnAllocMem || !pfnGetMemProps) {
        for (auto* s : surfaces) if (s) SDL_FreeSurface(s);
        return;
    }

    for (uint32_t type = 0; type < kOverlayMax; type++) {
        if (disables[type]) {
            m_OverlayHasContent[type] = false;
            continue;
        }
        SDL_Surface* surface = surfaces[type];
        if (!surface) continue;

        int w = surface->w, h = surface->h;
        if (w == 0 || h == 0) { SDL_FreeSurface(surface); continue; }

    VkPhysicalDeviceMemoryProperties memProps;
    pfnGetMemProps(m_PhysicalDevice, &memProps);
    auto findMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags wanted) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & wanted) == wanted)
                return (int)i;
        }
        return -1;
    };

    // (Re)alloc image if size mismatched (or first time).
    if (m_OverlayWidth[type] != w || m_OverlayHeight[type] != h) {
        // Free old (if any).
        if (m_OverlayView[type])  { pfnDestroyView(m_Device, m_OverlayView[type], nullptr);   m_OverlayView[type] = VK_NULL_HANDLE; }
        if (m_OverlayImage[type]) { pfnDestroyImage(m_Device, m_OverlayImage[type], nullptr); m_OverlayImage[type] = VK_NULL_HANDLE; }
        if (m_OverlayMem[type])   { pfnFreeMem(m_Device, m_OverlayMem[type], nullptr);        m_OverlayMem[type] = VK_NULL_HANDLE; }
        if (m_OverlayStagingMapped[type]) { pfnUnmapMem(m_Device, m_OverlayStagingMem[type]); m_OverlayStagingMapped[type] = nullptr; }
        if (m_OverlayStagingBuf[type]) { pfnDestroyBuffer(m_Device, m_OverlayStagingBuf[type], nullptr); m_OverlayStagingBuf[type] = VK_NULL_HANDLE; }
        if (m_OverlayStagingMem[type]) { pfnFreeMem(m_Device, m_OverlayStagingMem[type], nullptr); m_OverlayStagingMem[type] = VK_NULL_HANDLE; }

        // VkImage RGBA8
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_B8G8R8A8_UNORM;  // matches SDL_PIXELFORMAT_ARGB8888 byte order
        ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (pfnCreateImage(m_Device, &ici, nullptr, &m_OverlayImage[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }
        VkMemoryRequirements mr;
        pfnGetImgMemReq(m_Device, m_OverlayImage[type], &mr);
        int mti = findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mti < 0) { SDL_FreeSurface(surface); return; }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = (uint32_t)mti;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_OverlayMem[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }
        if (pfnBindImgMem(m_Device, m_OverlayImage[type], m_OverlayMem[type], 0) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }
        VkImageViewCreateInfo vci = {};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = m_OverlayImage[type];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_B8G8R8A8_UNORM;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (pfnCreateView(m_Device, &vci, nullptr, &m_OverlayView[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }

        // Staging buffer (host-visible coherent)
        VkDeviceSize stagingSize = (VkDeviceSize)surface->pitch * h;
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = stagingSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &m_OverlayStagingBuf[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }
        VkMemoryRequirements bmr;
        pfnGetBufMemReq(m_Device, m_OverlayStagingBuf[type], &bmr);
        int bmti = findMemType(bmr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (bmti < 0) { SDL_FreeSurface(surface); return; }
        VkMemoryAllocateInfo bmai = {};
        bmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        bmai.allocationSize = bmr.size;
        bmai.memoryTypeIndex = (uint32_t)bmti;
        if (pfnAllocMem(m_Device, &bmai, nullptr, &m_OverlayStagingMem[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }
        if (pfnBindBufMem(m_Device, m_OverlayStagingBuf[type], m_OverlayStagingMem[type], 0) != VK_SUCCESS
            || pfnMapMem(m_Device, m_OverlayStagingMem[type], 0, VK_WHOLE_SIZE, 0, &m_OverlayStagingMapped[type]) != VK_SUCCESS) {
            SDL_FreeSurface(surface); return;
        }

        // Descriptor set (allocate or re-use)
        if (m_OverlayDescSet[type] == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo asi = {};
            asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            asi.descriptorPool = m_OverlayDescPool;
            asi.descriptorSetCount = 1;
            asi.pSetLayouts = &m_OverlayDescSetLayout;
            if (pfnAllocDescSets(m_Device, &asi, &m_OverlayDescSet[type]) != VK_SUCCESS) {
                SDL_FreeSurface(surface); return;
            }
        }
        // Update descriptor with the new view + sampler
        VkDescriptorImageInfo dii = {};
        dii.sampler = m_OverlaySampler;
        dii.imageView = m_OverlayView[type];
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds = {};
        wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds.dstSet = m_OverlayDescSet[type];
        wds.dstBinding = 0;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.pImageInfo = &dii;
        pfnUpdateDescSets(m_Device, 1, &wds, 0, nullptr);

        m_OverlayWidth[type] = w;
        m_OverlayHeight[type] = h;
        m_OverlayPitch[type] = surface->pitch;
        m_OverlayStagingSize[type] = (size_t)stagingSize;
        m_OverlayLayoutInited[type] = false;  // first cmdCopyBufferToImage will UNDEFINED→DST
    }

        // memcpy surface pixels to staging
        if (m_OverlayStagingMapped[type] && surface->pixels) {
            memcpy(m_OverlayStagingMapped[type], surface->pixels, m_OverlayStagingSize[type]);
            m_OverlayPending[type] = true;
            m_OverlayHasContent[type] = true;
        }
        SDL_FreeSurface(surface);
    }  // for-type loop end
}  // drainOverlayStash() end

void VkFrucRenderer::uploadPendingOverlay(VkCommandBuffer cmd)
{
    if (!m_OverlayPipeline) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)getDevPa(m_Device, "vkCmdCopyBufferToImage");
    auto pfnCmdPipelineBarrier   = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");

    for (uint32_t type = 0; type < kOverlayMax; type++) {
        if (!m_OverlayPending[type] || !m_OverlayImage[type]) continue;
        VkImageLayout oldLayout = m_OverlayLayoutInited[type]
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageMemoryBarrier toDst = {};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.srcAccessMask = m_OverlayLayoutInited[type] ? VK_ACCESS_SHADER_READ_BIT : (VkAccessFlags)0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = oldLayout;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = m_OverlayImage[type];
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.levelCount = 1;
        toDst.subresourceRange.layerCount = 1;
        pfnCmdPipelineBarrier(cmd,
            m_OverlayLayoutInited[type] ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy reg = {};
        reg.bufferRowLength = (uint32_t)(m_OverlayPitch[type] / 4);  // pitch in pixels
        reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        reg.imageSubresource.layerCount = 1;
        reg.imageExtent = { (uint32_t)m_OverlayWidth[type], (uint32_t)m_OverlayHeight[type], 1 };
        pfnCmdCopyBufferToImage(cmd, m_OverlayStagingBuf[type], m_OverlayImage[type],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

        VkImageMemoryBarrier toShader = toDst;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pfnCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShader);

        m_OverlayPending[type] = false;
        m_OverlayLayoutInited[type] = true;
    }
}

void VkFrucRenderer::drawOverlayInRenderPass(VkCommandBuffer cmd)
{
    if (!m_OverlayPipeline) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdPushConstants = (PFN_vkCmdPushConstants)getDevPa(m_Device, "vkCmdPushConstants");

    bool boundPipeline = false;
    for (uint32_t type = 0; type < kOverlayMax; type++) {
        if (!m_OverlayHasContent[type] || !m_OverlayDescSet[type]) continue;
        // 對齊 D3D11VARenderer::createOverlayVertexBuffer (d3d11va.cpp:1620):
        // surface 原 pixel size、不縮放、不 padding.  OverlayDebug 在左上,
        // OverlayStatusUpdate 在左下 (跟 D3D11 註解 "Top left" / "Bottom Left"
        // 相符 —— 雖然 D3D11 用 screen-space y 經 NDC flip 換算,Vulkan UV
        // 直接用 [0,1] 表示).
        float scW = (float)m_SwapchainExtent.width;
        float scH = (float)m_SwapchainExtent.height;
        float ow  = (float)m_OverlayWidth[type];
        float oh  = (float)m_OverlayHeight[type];
        float xMin, xMax, yMin, yMax;
        if (type == 0 /*OverlayDebug*/) {
            xMin = 0.0f; yMin = 0.0f;          // 左上角
        } else /*OverlayStatusUpdate*/ {
            xMin = 0.0f; yMin = 1.0f - oh / scH;  // 左下角
        }
        xMax = xMin + ow / scW;
        yMax = yMin + oh / scH;
        // 萬一 overlay surface 比 swapchain 還大，clamp 到 [0,1] 不要超出.
        if (xMax > 1.0f) xMax = 1.0f;
        if (yMax > 1.0f) yMax = 1.0f;

        if (!boundPipeline) {
            m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_OverlayPipeline);
            boundPipeline = true;
        }
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       m_OverlayPipelineLayout, 0,
                                       1, &m_OverlayDescSet[type], 0, nullptr);
        struct { float xMin, yMin, xMax, yMax; } pcRect = { xMin, yMin, xMax, yMax };
        if (pfnCmdPushConstants) {
            pfnCmdPushConstants(cmd, m_OverlayPipelineLayout,
                                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pcRect), &pcRect);
        }
        m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
    }
}

// §J.3.e.2.i.3.d — per-slot in-flight ring.  Mirrors Android's
// init_in_flight_ring (vk_backend.c:2791): one VkCommandPool with
// RESET_COMMAND_BUFFER_BIT, kFrucFramesInFlight cmd buffers allocated
// from it, and per-slot semaphore pairs + a signaled-initial fence.
//
// The fence is created VK_FENCE_CREATE_SIGNALED_BIT so the very first
// renderFrame() can vkWaitForFences without blocking (slot 0 has no
// prior submit to wait on).
bool VkFrucRenderer::createInFlightRing()
{
    if (m_RingInitialized) return true;

    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateCmdPool = (PFN_vkCreateCommandPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateCommandPool");
    auto pfnAllocCmdBufs = (PFN_vkAllocateCommandBuffers)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateCommandBuffers");
    auto pfnCreateSem = (PFN_vkCreateSemaphore)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSemaphore");
    auto pfnCreateFence = (PFN_vkCreateFence)pfnGetDeviceProcAddr(
        m_Device, "vkCreateFence");
    if (!pfnCreateCmdPool || !pfnAllocCmdBufs || !pfnCreateSem || !pfnCreateFence) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d PFN load (ring) failed");
        return false;
    }

    // §J.3.e.2.i.7 R8: 加 TRANSIENT_BIT — 讓 driver 知道 cmd buffer 短命
    // (re-record every frame)，可以選用 cheaper allocation strategy.
    VkCommandPoolCreateInfo pci = {};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                         | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = m_QueueFamily;
    if (pfnCreateCmdPool(m_Device, &pci, nullptr, &m_CmdPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = m_CmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFrucFramesInFlight;
    if (pfnAllocCmdBufs(m_Device, &cbai, m_SlotCmdBuf) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkAllocateCommandBuffers failed");
        return false;
    }

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        for (int p = 0; p < 2; p++) {
            if (pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotAcquireSem[i][p]) != VK_SUCCESS ||
                pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotRenderDoneSem[i][p]) != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateSemaphore[%u][%d] failed",
                             i, p);
                return false;
            }
        }
        if (pfnCreateFence(m_Device, &fci, nullptr,
                           &m_SlotInFlightFence[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateFence[%u] failed", i);
            return false;
        }
    }

    m_CurrentSlot = 0;
    m_RingInitialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.d in-flight ring ready: %u slots × "
                "(1 cmdbuf + 2 acquireSem + 2 renderDoneSem + 1 fence)",
                (unsigned)kFrucFramesInFlight);
    return true;
}

void VkFrucRenderer::destroyInFlightRing()
{
    if (!m_RingInitialized || m_Device == VK_NULL_HANDLE) {
        // Even if !initialized, drop the cmd pool if it leaked half-way through
        // createInFlightRing failure.
    }

    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroySem = (PFN_vkDestroySemaphore)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySemaphore");
    auto pfnDestroyFence = (PFN_vkDestroyFence)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyFence");
    auto pfnDestroyCmdPool = (PFN_vkDestroyCommandPool)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyCommandPool");

    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        if (m_SlotInFlightFence[i] && pfnDestroyFence) {
            pfnDestroyFence(m_Device, m_SlotInFlightFence[i], nullptr);
            m_SlotInFlightFence[i] = VK_NULL_HANDLE;
        }
        for (int p = 0; p < 2; p++) {
            if (m_SlotAcquireSem[i][p] && pfnDestroySem) {
                pfnDestroySem(m_Device, m_SlotAcquireSem[i][p], nullptr);
                m_SlotAcquireSem[i][p] = VK_NULL_HANDLE;
            }
            if (m_SlotRenderDoneSem[i][p] && pfnDestroySem) {
                pfnDestroySem(m_Device, m_SlotRenderDoneSem[i][p], nullptr);
                m_SlotRenderDoneSem[i][p] = VK_NULL_HANDLE;
            }
        }
        // Cmd buffers freed implicitly when cmd pool is destroyed below.
        m_SlotCmdBuf[i] = VK_NULL_HANDLE;
    }
    if (m_CmdPool && pfnDestroyCmdPool) {
        pfnDestroyCmdPool(m_Device, m_CmdPool, nullptr);
        m_CmdPool = VK_NULL_HANDLE;
    }
    m_RingInitialized = false;
}

// §J.3.e.2.i.3.b — VkSamplerYcbcrConversion + VkSampler + descriptor
// set layout + pipeline layout.  Maps NV12 (G8_B8R8_2PLANE_420_UNORM)
// to a single combined image sampler binding the fragment shader sees
// as `sampler2D` returning RGB (driver-side YCbCr→RGB conversion).
//
// Algorithm choice: BT.709 narrow range (matches Sunshine / standard
// streaming output).  10-bit (P010 → G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16)
// deferred to §J.3.e.2.j HDR.
bool VkFrucRenderer::createYcbcrSamplerAndLayouts()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateYcbcrConv = (PFN_vkCreateSamplerYcbcrConversion)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSamplerYcbcrConversion");
    auto pfnCreateSampler = (PFN_vkCreateSampler)pfnGetDeviceProcAddr(
        m_Device, "vkCreateSampler");
    auto pfnCreateDsl = (PFN_vkCreateDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePL = (PFN_vkCreatePipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkCreatePipelineLayout");
    if (!pfnCreateYcbcrConv || !pfnCreateSampler || !pfnCreateDsl || !pfnCreatePL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b PFN load failed");
        return false;
    }

    VkSamplerYcbcrConversionCreateInfo yci = {};
    yci.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    yci.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
    yci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    yci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    yci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    yci.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    yci.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    yci.chromaFilter = VK_FILTER_LINEAR;
    yci.forceExplicitReconstruction = VK_FALSE;
    if (pfnCreateYcbcrConv(m_Device, &yci, nullptr, &m_YcbcrConversion) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateSamplerYcbcrConversion failed");
        return false;
    }

    VkSamplerYcbcrConversionInfo ycbInfo = {};
    ycbInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbInfo.conversion = m_YcbcrConversion;

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.pNext = &ycbInfo;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxAnisotropy = 1.0f;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    if (pfnCreateSampler(m_Device, &sci, nullptr, &m_YcbcrSampler) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateSampler failed");
        return false;
    }

    // Descriptor set layout: 1 combined image sampler with IMMUTABLE
    // sampler (required when using ycbcr conversion — driver bakes
    // conversion into the descriptor).
    VkDescriptorSetLayoutBinding dslBinding = {};
    dslBinding.binding = 0;
    dslBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslBinding.descriptorCount = 1;
    dslBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    dslBinding.pImmutableSamplers = &m_YcbcrSampler;

    VkDescriptorSetLayoutCreateInfo dslCi = {};
    dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCi.bindingCount = 1;
    dslCi.pBindings = &dslBinding;
    if (pfnCreateDsl(m_Device, &dslCi, nullptr, &m_DescSetLayout) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo plCi = {};
    plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCi.setLayoutCount = 1;
    plCi.pSetLayouts = &m_DescSetLayout;
    if (pfnCreatePL(m_Device, &plCi, nullptr, &m_GraphicsPipelineLayout) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.b vkCreatePipelineLayout failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.b ycbcr sampler+layouts ready "
                "(format=NV12, model=BT709, range=narrow, magFilter=linear)");
    return true;
}

void VkFrucRenderer::destroyYcbcrSamplerAndLayouts()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyPL = (PFN_vkDestroyPipelineLayout)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl = (PFN_vkDestroyDescriptorSetLayout)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroySampler = (PFN_vkDestroySampler)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySampler");
    auto pfnDestroyYcbcrConv = (PFN_vkDestroySamplerYcbcrConversion)pfnGetDeviceProcAddr(
        m_Device, "vkDestroySamplerYcbcrConversion");
    if (m_GraphicsPipelineLayout && pfnDestroyPL) {
        pfnDestroyPL(m_Device, m_GraphicsPipelineLayout, nullptr);
        m_GraphicsPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_DescSetLayout && pfnDestroyDsl) {
        pfnDestroyDsl(m_Device, m_DescSetLayout, nullptr);
        m_DescSetLayout = VK_NULL_HANDLE;
    }
    if (m_YcbcrSampler && pfnDestroySampler) {
        pfnDestroySampler(m_Device, m_YcbcrSampler, nullptr);
        m_YcbcrSampler = VK_NULL_HANDLE;
    }
    if (m_YcbcrConversion && pfnDestroyYcbcrConv) {
        pfnDestroyYcbcrConv(m_Device, m_YcbcrConversion, nullptr);
        m_YcbcrConversion = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.3.a — populate AVVulkanDeviceContext with our handles so
// ffmpeg's Vulkan video decoder builds AVVkFrame on OUR VkDevice.
// Mirrors PlVkRenderer::populateQueues + the AVHWDeviceContext setup
// block in PlVkRenderer::completeInitialization.
bool VkFrucRenderer::populateAvHwDeviceCtx(int videoFormat)
{
    auto pfnGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!pfnGetQFP) return false;

    m_HwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (m_HwDeviceCtx == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.a av_hwdevice_ctx_alloc(VULKAN) failed");
        return false;
    }
    auto hwDeviceContext = (AVHWDeviceContext*)m_HwDeviceCtx->data;
    hwDeviceContext->user_opaque = this;

    auto vkCtx = (AVVulkanDeviceContext*)hwDeviceContext->hwctx;
    vkCtx->get_proc_addr = m_pfnGetInstanceProcAddr;
    vkCtx->inst          = m_Instance;
    vkCtx->phys_dev      = m_PhysicalDevice;
    vkCtx->act_dev       = m_Device;

    // §J.3.e.2.i.3.a CRITICAL — device_features must reflect EXACTLY what
    // we enabled at vkCreateDevice (ffmpeg/NV driver dereferences feature
    // state from this struct's pNext chain to find timeline_semaphore /
    // synchronization2 / sampler_ycbcr_conversion state objects).  If any
    // are missing or NULL, NV's nvoglv64 internal state at offset 0xF0
    // is NULL → crash on decoder thread (v1.3.123-132 root cause; see
    // doc/J.3.e.2.i_vulkan_native_renderer.md).
    //
    // m_DevFeat2 / m_YcbcrFeat / m_TimelineFeat / m_Sync2Feat are
    // PERSISTENT MEMBERS (not stack-allocated).  Copying device_features
    // is a struct copy of VkPhysicalDeviceFeatures2 BUT pNext is a raw
    // pointer that survives the copy and points back into our member
    // chain — exactly what FFmpeg needs.
    vkCtx->device_features = m_DevFeat2;

    // §J.3.e.2.i.7 — extension list MUST match exactly what we passed to
    // vkCreateDevice (otherwise FFmpeg's hwcontext_vulkan PFN dispatch table
    // mis-init → NV driver NULL deref crash).  m_EnabledDevExts 是
    // createLogicalDevice 過濾過的 wanted ∩ available 集合.
    static const char* kInstExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef Q_OS_WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };
    vkCtx->enabled_inst_extensions    = kInstExts;
    vkCtx->nb_enabled_inst_extensions = (int)(sizeof(kInstExts) / sizeof(kInstExts[0]));
    vkCtx->enabled_dev_extensions     = m_EnabledDevExts.data();
    vkCtx->nb_enabled_dev_extensions  = (int)m_EnabledDevExts.size();

#if LIBAVUTIL_VERSION_INT > AV_VERSION_INT(58, 9, 100)
    vkCtx->lock_queue   = lockQueueStub;
    vkCtx->unlock_queue = unlockQueueStub;
#endif

    // Queue family info — libavutil 60 (FFmpeg 8 master) STILL has
    // FF_API_VULKAN_FIXED_QUEUES=1, so the deprecated old-style discrete
    // fields (queue_family_*_index, nb_*_queues) are still part of the
    // struct AND still accessed by FFmpeg's internal code paths.  Fill
    // BOTH old AND new style to avoid zero-init values being misused.
    // (v1.3.123-134 crash root cause: only new-style qf[] was filled,
    // old fields zero → FFmpeg internal queue lookup uses queue family 0
    // for decode-only ops → NV driver NULL deref via uninitialised
    // dispatch state).
    uint32_t qfCount = 0;
    pfnGetQFP(m_PhysicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    pfnGetQFP(m_PhysicalDevice, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        vkCtx->qf[i].idx        = i;
        vkCtx->qf[i].num        = qfs[i].queueCount;
        vkCtx->qf[i].flags      = (VkQueueFlagBits)qfs[i].queueFlags;
        vkCtx->qf[i].video_caps = (VkVideoCodecOperationFlagBitsKHR)0;  // ffmpeg re-probes
    }
    vkCtx->nb_qf = qfCount;
#if FF_API_VULKAN_FIXED_QUEUES
    // Mirror our actual queue layout into the deprecated fields.
    vkCtx->queue_family_index        = (int)m_QueueFamily;
    vkCtx->nb_graphics_queues        = 1;
    vkCtx->queue_family_tx_index     = (int)m_QueueFamily;
    vkCtx->nb_tx_queues              = 1;
    vkCtx->queue_family_comp_index   = (int)m_QueueFamily;
    vkCtx->nb_comp_queues            = 1;
    vkCtx->queue_family_encode_index = -1;
    vkCtx->nb_encode_queues          = 0;
    vkCtx->queue_family_decode_index = (int)m_DecodeQueueFamily;
    vkCtx->nb_decode_queues          = (int)m_DecodeQueueCount;
#endif
    (void)videoFormat;

    int err = av_hwdevice_ctx_init(m_HwDeviceCtx);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.a av_hwdevice_ctx_init failed: %d", err);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.a AVHWDeviceContext init OK "
                "(QF gfx=%u decode=%u, %u queue families wired)",
                m_QueueFamily, m_DecodeQueueFamily, qfCount);
    return true;
}

// §J.3.e.2.i.8 Phase 1.0 — VkVideoSessionKHR scaffold (skip FFmpeg entirely).
//
// 流程：
//   1. 從 videoFormat 推 codec (H.264 / H.265 / AV1)
//   2. 建 VkVideoProfileInfoKHR (跟 probe 同邏輯, codec-specific pNext)
//   3. vkCreateVideoSessionKHR (queueFamilyIndex = m_DecodeQueueFamily,
//      pictureFormat = NV12, max DPB / refs from probe values)
//   4. vkGetVideoSessionMemoryRequirementsKHR → N 個 binding requirements
//   5. 逐一 vkAllocateMemory + 收集 BindSessionMemoryInfoKHR
//   6. vkBindVideoSessionMemoryKHR 一次 bind 所有
//
// Phase 1.0 stops here.  m_VideoSessionParams 在 1.1 SPS/PPS parse 後才建.
bool VkFrucRenderer::createVideoSession(int videoFormat)
{
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateVideoSession  = (PFN_vkCreateVideoSessionKHR)getDevPa(m_Device, "vkCreateVideoSessionKHR");
    auto pfnGetVidSessMemReq    = (PFN_vkGetVideoSessionMemoryRequirementsKHR)getDevPa(m_Device, "vkGetVideoSessionMemoryRequirementsKHR");
    auto pfnBindVidSessMem      = (PFN_vkBindVideoSessionMemoryKHR)getDevPa(m_Device, "vkBindVideoSessionMemoryKHR");
    auto pfnAllocMem            = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnGetMemProps         = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateVideoSession || !pfnGetVidSessMemReq || !pfnBindVidSessMem
        || !pfnAllocMem || !pfnGetMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 video session PFN missing");
        return false;
    }

    // ── Step 1: codec profile from videoFormat ──
    // §J.3.e.2.i.8 Phase 1.3 — populate as MEMBERS (m_VideoProfile + matching
    // codec-specific m_*ProfileInfo) so they stay alive for bitstream buffer +
    // DPB image creation that needs to chain VkVideoProfileListInfoKHR.
    VkVideoCodecOperationFlagBitsKHR codecOp;
    const char* codecName;
    const char* stdName;
    uint32_t    stdVersion = 0;
    void* codecProfilePNext = nullptr;
    uint32_t maxDpbSlots, maxRefs;
    m_H264ProfileInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR };
    m_H265ProfileInfo = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR };
    m_AV1ProfileInfo  = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR  };
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        codecName = "H.264 Main";
        stdName = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_EXTENSION_NAME;
        stdVersion = VK_STD_VULKAN_VIDEO_CODEC_H264_DECODE_SPEC_VERSION;
        m_H264ProfileInfo.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        m_H264ProfileInfo.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
        codecProfilePNext = &m_H264ProfileInfo;
        maxDpbSlots = 17; maxRefs = 16;
    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
        codecName = "H.265 Main";
        stdName = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_EXTENSION_NAME;
        stdVersion = VK_STD_VULKAN_VIDEO_CODEC_H265_DECODE_SPEC_VERSION;
        m_H265ProfileInfo.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
        codecProfilePNext = &m_H265ProfileInfo;
        maxDpbSlots = 16; maxRefs = 16;
    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        codecOp = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
        codecName = "AV1 Main";
        stdName = VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_EXTENSION_NAME;
        stdVersion = VK_STD_VULKAN_VIDEO_CODEC_AV1_DECODE_SPEC_VERSION;
        m_AV1ProfileInfo.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
        // §J.3.e.2.i.8 Phase 3d.5 — filmGrainSupport=TRUE didn't help and
        // introduced VUID-07212 cleanup warning; reverted to FALSE.  The
        // remaining black output is not film-grain related.
        m_AV1ProfileInfo.filmGrainSupport = VK_FALSE;
        codecProfilePNext = &m_AV1ProfileInfo;
        maxDpbSlots = 16; maxRefs = 16;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 unsupported videoFormat=0x%x", videoFormat);
        return false;
    }

    m_VideoProfile = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR };
    m_VideoProfile.pNext               = codecProfilePNext;
    m_VideoProfile.videoCodecOperation = codecOp;
    m_VideoProfile.chromaSubsampling   = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m_VideoProfile.lumaBitDepth        = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_VideoProfile.chromaBitDepth      = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m_VideoProfileReady                = true;
    VkVideoProfileInfoKHR& profile = m_VideoProfile;  // alias for downstream code below

    // ── Step 2: Std header version (codec-specific extension struct) ──
    VkExtensionProperties stdHeaderVer = {};
    strncpy_s(stdHeaderVer.extensionName, sizeof(stdHeaderVer.extensionName), stdName, _TRUNCATE);
    stdHeaderVer.specVersion = stdVersion;

    // ── Step 3: vkCreateVideoSessionKHR ──
    VkVideoSessionCreateInfoKHR vsci = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR };
    vsci.queueFamilyIndex             = m_DecodeQueueFamily;
    vsci.flags                        = 0;
    vsci.pVideoProfile                = &profile;
    vsci.pictureFormat                = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
    vsci.maxCodedExtent               = { 4096, 4096 };  // ≤4K, fits H.264/H.265 maxRefs probe
    vsci.referencePictureFormat       = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    vsci.maxDpbSlots                  = maxDpbSlots;
    vsci.maxActiveReferencePictures   = maxRefs;
    vsci.pStdHeaderVersion            = &stdHeaderVer;

    VkResult vr = pfnCreateVideoSession(m_Device, &vsci, nullptr, &m_VideoSession);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkCreateVideoSessionKHR(%s) rc=%d",
                     codecName, (int)vr);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 VkVideoSessionKHR created (%s, dpb=%u refs=%u, max=4096x4096)",
                codecName, maxDpbSlots, maxRefs);

    // ── Step 4-6: memory bindings ──
    uint32_t reqCount = 0;
    pfnGetVidSessMemReq(m_Device, m_VideoSession, &reqCount, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(reqCount, { VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR });
    pfnGetVidSessMemReq(m_Device, m_VideoSession, &reqCount, reqs.data());

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetMemProps(m_PhysicalDevice, &memProps);
    // Find memory type matching memoryTypeBits.  優先嘗試 DEVICE_LOCAL,
    // 找不到就退到任何 driver 允許的 type —— video session 某些 binding
    // (如 control state / context buffer) driver 給非 DEVICE_LOCAL 的
    // memory 要求是正常的.
    auto findMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags preferred) -> int {
        // Pass 1: try preferred (DEVICE_LOCAL)
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & preferred) == preferred) return (int)i;
        }
        // Pass 2: any allowed type
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if (typeBits & (1u << i)) return (int)i;
        }
        return -1;
    };

    std::vector<VkBindVideoSessionMemoryInfoKHR> binds(reqCount, { VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR });
    m_VideoSessionMem.assign(reqCount, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < reqCount; i++) {
        int mti = findMemType(reqs[i].memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mti < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 video session mem[%u] no compatible memory type (typeBits=0x%x)",
                         i, reqs[i].memoryRequirements.memoryTypeBits);
            return false;
        }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize  = reqs[i].memoryRequirements.size;
        mai.memoryTypeIndex = (uint32_t)mti;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_VideoSessionMem[i]) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 video session mem[%u] vkAllocateMemory failed (size=%llu)",
                         i, (unsigned long long)reqs[i].memoryRequirements.size);
            return false;
        }
        binds[i].memoryBindIndex = reqs[i].memoryBindIndex;
        binds[i].memory          = m_VideoSessionMem[i];
        binds[i].memoryOffset    = 0;
        binds[i].memorySize      = reqs[i].memoryRequirements.size;
    }
    if (pfnBindVidSessMem(m_Device, m_VideoSession, reqCount, binds.data()) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkBindVideoSessionMemoryKHR failed");
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 video session memory bound (%u bindings)",
                reqCount);

    m_VideoCodec = videoFormat;  // remember which codec for Phase 1.1+
    return true;
}

// §J.3.e.2.i.8 Phase 1.1 — VkVideoSessionParametersKHR (empty slots).
//
// 建空的 parameters object，預留 max VPS/SPS/PPS slots. 之後 stream 收到
// VPS/SPS/PPS NAL 時用 vkUpdateVideoSessionParametersKHR 動態加進去 ——
// 不需要 init 時就有 parameter sets.
//
// H.264: maxStdSPSCount + maxStdPPSCount
// H.265: maxStdVPSCount + maxStdSPSCount + maxStdPPSCount
// AV1:   maxStdSequenceHeaderCount
bool VkFrucRenderer::createVideoSessionParameters(int videoFormat)
{
    if (m_VideoSession == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateVidSessParams = (PFN_vkCreateVideoSessionParametersKHR)getDevPa(
        m_Device, "vkCreateVideoSessionParametersKHR");
    if (!pfnCreateVidSessParams) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkCreateVideoSessionParametersKHR PFN missing");
        return false;
    }

    VkVideoSessionParametersCreateInfoKHR vspCi = {
        VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    vspCi.videoSession = m_VideoSession;
    vspCi.videoSessionParametersTemplate = VK_NULL_HANDLE;

    // codec-specific create struct
    VkVideoDecodeH264SessionParametersCreateInfoKHR h264ParamsCi = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    VkVideoDecodeH265SessionParametersCreateInfoKHR h265ParamsCi = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    VkVideoDecodeAV1SessionParametersCreateInfoKHR av1ParamsCi = {
        VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR
    };
    const char* codecName;
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        h264ParamsCi.maxStdSPSCount  = 32;  // H.264 spec max
        h264ParamsCi.maxStdPPSCount  = 256;
        h264ParamsCi.pParametersAddInfo = nullptr;  // empty initial
        vspCi.pNext = &h264ParamsCi;
        codecName = "H.264";
    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        h265ParamsCi.maxStdVPSCount  = 16;   // H.265 spec max
        h265ParamsCi.maxStdSPSCount  = 16;
        h265ParamsCi.maxStdPPSCount  = 64;
        h265ParamsCi.pParametersAddInfo = nullptr;
        vspCi.pNext = &h265ParamsCi;
        codecName = "H.265";
    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        // AV1 sequence header: only 1 active per session typically.
        // Need to provide a non-null pStdSequenceHeader at create time
        // (AV1 spec says session params requires a sequence header).
        // For Phase 1.1 placeholder: zero-init struct (will fail if driver
        // strictly validates).  Phase 1.2 will populate from parsed AV1
        // sequence OBU.
        static StdVideoAV1SequenceHeader av1SeqHdr = {};
        av1ParamsCi.pStdSequenceHeader = &av1SeqHdr;
        vspCi.pNext = &av1ParamsCi;
        codecName = "AV1";
    } else {
        return false;
    }

    VkResult vr = pfnCreateVidSessParams(m_Device, &vspCi, nullptr, &m_VideoSessionParams);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 vkCreateVideoSessionParametersKHR(%s) rc=%d",
                     codecName, (int)vr);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 VkVideoSessionParametersKHR created (%s, empty slots)",
                codecName);
    return true;
}

// §J.3.e.2.i.8 Phase 1.2 — native VK decode hook (renderer.h interface).
// 只有 m_VideoSession 真的建好才接收 NAL bytes.  Phase 1.1b parser 還沒接,
// 目前只統計 NAL types + log 每 5 秒給 diagnostic.
bool VkFrucRenderer::acceptsNativeDecode() const
{
    // Phase 1 disabled by default: 不在 Phase 1.x 完整前接管 decode.
    // 設 VIPLE_VKFRUC_NATIVE_DECODE=1 才 opt-in.  之後 Phase 1.4 完成且
    // 走通 stream 後拆 gate.
    static const bool wantNative = qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_DECODE") != 0;
    return wantNative && (m_VideoSession != VK_NULL_HANDLE);
}

bool VkFrucRenderer::isNativelyDecodingCurrentCodec() const
{
    // §J.3.e.2.i.8 Phase 3d.3 — accurate per-codec native-decode probe.
    //
    //   * H.265: Phase 1 + 1.5b stable, vkCmdDecodeVideoKHR fires every frame.
    //   * H.264: Phase 2 (v1.3.273+) parser ported, submitDecodeFrameH264 lands
    //     in the same patch.  Treated like H.265 — always native when env var on.
    //   * AV1: parser dispatch + sequence-header rebuild work, but the actual
    //     vkCmdDecodeVideoKHR submit is gated behind VIPLE_VKFRUC_NATIVE_AV1_SUBMIT
    //     pending Phase 3d.5 GPU-side grey diagnosis; without that env var
    //     AV1 streams flow through FFmpeg libdav1d so we report "not native".
    if (!acceptsNativeDecode()) return false;
    if (m_VideoCodec & VIDEO_FORMAT_MASK_H265) return true;
    if (m_VideoCodec & VIDEO_FORMAT_MASK_H264) return true;
    if (m_VideoCodec & VIDEO_FORMAT_MASK_AV1) {
        static const bool av1SubmitEnabled =
            qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_AV1_SUBMIT") != 0;
        return av1SubmitEnabled;
    }
    return false;
}

void VkFrucRenderer::submitNativeDecodeUnit(const uint8_t* data, size_t len)
{
    // §J.3.e.2.i.8 Phase 1.1c — 把 NAL bytes 餵給 NvVideoParser.
    // Parser 內部解析 + 觸發 callback (StartVideoSequence / UpdatePictureParameters /
    // DecodePictureWithParameters) on VkFrucDecoderHandler.
    feedNalToNvParser(data, len);

    // Annex-B byte-stream NAL units: 0x000001 or 0x00000001 start code,
    // 然後 NAL header (HEVC 是 2 bytes, H.264 是 1 byte).
    // 此處掃 start codes 切出每個 NAL，分類 type 計數 (legacy diagnostic).
    if (!data || len < 4) return;
    // First-call diagnostic log so we know the intercept actually fires.
    if (m_NalCounts.total_packets == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 native decode intercept ACTIVE — "
                    "first packet len=%zu first6bytes=%02x %02x %02x %02x %02x %02x",
                    len, data[0], data[1], data[2], data[3], data[4], data[5]);
    }
    m_NalCounts.total_packets++;
    m_NalCounts.total_bytes += len;

    auto isStartCode = [](const uint8_t* p, size_t remain) -> int {
        if (remain >= 4 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) return 4;
        if (remain >= 3 && p[0]==0 && p[1]==0 && p[2]==1)            return 3;
        return 0;
    };

    // Find each NAL unit boundary.
    size_t i = 0;
    while (i < len) {
        int sc = isStartCode(data + i, len - i);
        if (!sc) { i++; continue; }
        size_t nalStart = i + sc;
        if (nalStart >= len) break;

        // Find next start code OR end of buffer.
        size_t nalEnd = len;
        for (size_t j = nalStart + 1; j + 2 < len; j++) {
            if (data[j] == 0 && data[j+1] == 0 && (data[j+2] == 1 || (data[j+2] == 0 && j+3 < len && data[j+3] == 1))) {
                nalEnd = j;
                break;
            }
        }

        // HEVC NAL header: forbidden_zero_bit(1) | nal_unit_type(6) | layer_id(6) | tid_plus1(3)
        // First byte: ((data[0] >> 1) & 0x3F) = nal_unit_type (HEVC).
        // For H.264: nal_unit_type = data[0] & 0x1F (5 bits).  Phase 1 focuses HEVC.
        uint8_t firstByte = data[nalStart];
        int hevcNalType = (firstByte >> 1) & 0x3F;
        switch (hevcNalType) {
            case 19: case 20:                  m_NalCounts.idr_slice++;       break;  // IDR_W_RADL / IDR_N_LP
            case 0:  case 1:  case 2:  case 3:                                       // TRAIL/TSA/STSA
            case 4:  case 5:  case 6:  case 7:                                       // RADL/RASL
            case 8:  case 9:                                                          // RASL_N etc
            case 16: case 17: case 18:                                               // BLA
            case 21:                          m_NalCounts.trailing_slice++;  break;  // CRA
            case 32:                          m_NalCounts.vps++;             break;
            case 33:                          m_NalCounts.sps++;             break;
            case 34:                          m_NalCounts.pps++;             break;
            case 35:                          m_NalCounts.aud++;             break;
            case 39:                          m_NalCounts.prefix_sei++;      break;
            default:                          m_NalCounts.other++;           break;
        }
        i = nalEnd;
    }

    // Periodic diagnostic log (every ~1s based on packet count at 60fps).
    if ((m_NalCounts.total_packets % 60) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.8 NAL types VPS=%llu SPS=%llu PPS=%llu IDR=%llu trail=%llu AUD=%llu SEI=%llu other=%llu (packets=%llu, bytes=%.2f MB)",
                    (unsigned long long)m_NalCounts.vps,
                    (unsigned long long)m_NalCounts.sps,
                    (unsigned long long)m_NalCounts.pps,
                    (unsigned long long)m_NalCounts.idr_slice,
                    (unsigned long long)m_NalCounts.trailing_slice,
                    (unsigned long long)m_NalCounts.aud,
                    (unsigned long long)m_NalCounts.prefix_sei,
                    (unsigned long long)m_NalCounts.other,
                    (unsigned long long)m_NalCounts.total_packets,
                    m_NalCounts.total_bytes / (1024.0 * 1024.0));
    }
}

// §J.3.e.2.i.8 Phase 1.3d.2 debug — Vulkan validation messages → SDL_Log.
VkBool32 VKAPI_PTR VkFrucRenderer::debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
{
    if (!pCallbackData || !pCallbackData->pMessage) return VK_FALSE;
    int prio = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? SDL_LOG_PRIORITY_ERROR :
               (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? SDL_LOG_PRIORITY_WARN  :
                                                                              SDL_LOG_PRIORITY_INFO;
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, (SDL_LogPriority)prio,
                   "[VVL] %s: %s",
                   pCallbackData->pMessageIdName ? pCallbackData->pMessageIdName : "?",
                   pCallbackData->pMessage);
    return VK_FALSE;
}

void VkFrucRenderer::destroyVideoSession()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyVidSessParams = (PFN_vkDestroyVideoSessionParametersKHR)getDevPa(m_Device, "vkDestroyVideoSessionParametersKHR");
    auto pfnDestroyVidSess       = (PFN_vkDestroyVideoSessionKHR)getDevPa(m_Device, "vkDestroyVideoSessionKHR");
    auto pfnFreeMem              = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    if (m_VideoSessionParams && pfnDestroyVidSessParams) {
        pfnDestroyVidSessParams(m_Device, m_VideoSessionParams, nullptr);
        m_VideoSessionParams = VK_NULL_HANDLE;
    }
    if (m_VideoSession && pfnDestroyVidSess) {
        pfnDestroyVidSess(m_Device, m_VideoSession, nullptr);
        m_VideoSession = VK_NULL_HANDLE;
    }
    if (pfnFreeMem) {
        for (auto m : m_VideoSessionMem) {
            if (m) pfnFreeMem(m_Device, m, nullptr);
        }
    }
    m_VideoSessionMem.clear();

    // §J.3.e.2.i.8 Phase 1.1d — reset H.265 param-set tracking so a restart
    // re-uploads VPS/SPS/PPS into the new session params object.
    m_pfnUpdateVideoSessionParams = nullptr;
    m_H265SessionParamsSeq        = 0;
    m_H265VpsSeqSeen.clear();
    m_H265SpsSeqSeen.clear();
    m_H265PpsSeqSeen.clear();
    // §J.3.e.2.i.8 Phase 2 — same for H.264 (SPS+PPS, no VPS).
    m_H264SessionParamsSeq        = 0;
    m_H264SpsSeqSeen.clear();
    m_H264PpsSeqSeen.clear();

    // §J.3.e.2.i.8 Phase 3b.1 — reset AV1 sequence-header tracking so a restart
    // rebuilds session params from the fresh placeholder + first parser-supplied
    // sequence header.
    m_pfnDestroyVideoSessionParams = nullptr;
    m_pfnCreateVideoSessionParams  = nullptr;
    m_AV1SeqHdrSeqSeen             = 0;
    m_AV1SeqHdrApplied             = false;

    // §J.3.e.2.i.8 Phase 1.3 — invalidate codec profile so subsequent bitstream
    // buffer / DPB image allocations refuse to chain into stale state.
    m_VideoProfileReady = false;
}

// §J.3.e.2.i.8 Phase 1.3a — GPU bitstream buffer factory.
//
// Parser asks for a buffer via GetBitstreamBuffer; we hand back a host-visible
// + host-coherent VkBuffer with VIDEO_DECODE_SRC_BIT_KHR usage and the codec
// profile chained in pNext.  Persistently mapped (parser writes NAL bytes via
// returned mapped pointer; decode queue reads via VkBuffer handle).
//
// Sharing mode EXCLUSIVE — buffer ownership lives on m_DecodeQueueFamily; host
// mapping doesn't need queue-family ownership transfer per Vulkan spec.
bool VkFrucRenderer::createGpuBitstreamBuffer(VkDeviceSize size,
                                              VkBuffer& outBuffer,
                                              VkDeviceMemory& outMemory,
                                              void*& outMappedPtr,
                                              VkDeviceSize& outActualSize)
{
    outBuffer    = VK_NULL_HANDLE;
    outMemory    = VK_NULL_HANDLE;
    outMappedPtr = nullptr;
    outActualSize = 0;

    if (!m_VideoProfileReady || m_Device == VK_NULL_HANDLE) {
        return false;
    }

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateBuffer    = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnDestroyBuffer   = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnGetBufMemReq    = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem        = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnFreeMem         = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnBindBufMem      = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnMapMem          = (PFN_vkMapMemory)getDevPa(m_Device, "vkMapMemory");
    auto pfnGetMemProps     = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem
        || !pfnMapMem || !pfnGetMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a bitstream buffer PFN missing");
        return false;
    }

    VkVideoProfileListInfoKHR profileList = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR };
    profileList.profileCount = 1;
    profileList.pProfiles    = &m_VideoProfile;

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.pNext       = &profileList;
    bci.size        = size;
    bci.usage       = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bci.queueFamilyIndexCount = 0;
    bci.pQueueFamilyIndices   = nullptr;

    VkResult vr = pfnCreateBuffer(m_Device, &bci, nullptr, &outBuffer);
    if (vr != VK_SUCCESS || outBuffer == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a vkCreateBuffer rc=%d size=%llu",
                     (int)vr, (unsigned long long)size);
        return false;
    }

    VkMemoryRequirements memReq = {};
    pfnGetBufMemReq(m_Device, outBuffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetMemProps(m_PhysicalDevice, &memProps);

    // Pick host-visible + host-coherent so writes are immediately visible to GPU.
    int mti = -1;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want) {
            mti = (int)i;
            break;
        }
    }
    if (mti < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a no host-coherent memory type "
                     "(typeBits=0x%x)", memReq.memoryTypeBits);
        if (pfnDestroyBuffer) pfnDestroyBuffer(m_Device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = (uint32_t)mti;

    vr = pfnAllocMem(m_Device, &mai, nullptr, &outMemory);
    if (vr != VK_SUCCESS || outMemory == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a vkAllocateMemory rc=%d size=%llu",
                     (int)vr, (unsigned long long)memReq.size);
        if (pfnDestroyBuffer) pfnDestroyBuffer(m_Device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    vr = pfnBindBufMem(m_Device, outBuffer, outMemory, 0);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a vkBindBufferMemory rc=%d", (int)vr);
        if (pfnFreeMem) pfnFreeMem(m_Device, outMemory, nullptr);
        if (pfnDestroyBuffer) pfnDestroyBuffer(m_Device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        outMemory = VK_NULL_HANDLE;
        return false;
    }

    vr = pfnMapMem(m_Device, outMemory, 0, memReq.size, 0, &outMappedPtr);
    if (vr != VK_SUCCESS || !outMappedPtr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3a vkMapMemory rc=%d", (int)vr);
        if (pfnFreeMem) pfnFreeMem(m_Device, outMemory, nullptr);
        if (pfnDestroyBuffer) pfnDestroyBuffer(m_Device, outBuffer, nullptr);
        outBuffer    = VK_NULL_HANDLE;
        outMemory    = VK_NULL_HANDLE;
        outMappedPtr = nullptr;
        return false;
    }

    outActualSize = memReq.size;
    return true;
}

void VkFrucRenderer::destroyGpuBitstreamBuffer(VkBuffer buffer, VkDeviceMemory memory)
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnUnmapMem      = (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory");
    auto pfnFreeMem       = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    if (memory != VK_NULL_HANDLE && pfnUnmapMem) pfnUnmapMem(m_Device, memory);
    if (buffer != VK_NULL_HANDLE && pfnDestroyBuffer) pfnDestroyBuffer(m_Device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE && pfnFreeMem) pfnFreeMem(m_Device, memory, nullptr);
}

// §J.3.e.2.i.8 Phase 1.3c — decode queue handle + cmd pool + cmd buffer + fence + sem.
//
// Allocated once after createVideoSession.  Destroyed before destroyVideoSession
// so the cmd buffer doesn't outlive the session it references.
bool VkFrucRenderer::createDecodeCommandResources()
{
    if (m_DecodeQueueFamily == UINT32_MAX || m_Device == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c no decode queue family available");
        return false;
    }

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnGetDeviceQueue       = (PFN_vkGetDeviceQueue)getDevPa(m_Device, "vkGetDeviceQueue");
    auto pfnCreateCommandPool    = (PFN_vkCreateCommandPool)getDevPa(m_Device, "vkCreateCommandPool");
    auto pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)getDevPa(m_Device, "vkAllocateCommandBuffers");
    auto pfnCreateFence          = (PFN_vkCreateFence)getDevPa(m_Device, "vkCreateFence");
    auto pfnCreateSemaphore      = (PFN_vkCreateSemaphore)getDevPa(m_Device, "vkCreateSemaphore");
    if (!pfnGetDeviceQueue || !pfnCreateCommandPool || !pfnAllocateCommandBuffers
        || !pfnCreateFence || !pfnCreateSemaphore) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c decode cmd PFN missing");
        return false;
    }

    // Decode queue: index 0 of the decode queue family (we only requested 1
    // queue from this family in createLogicalDevice).
    pfnGetDeviceQueue(m_Device, m_DecodeQueueFamily, 0, &m_DecodeQueue);
    if (m_DecodeQueue == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c vkGetDeviceQueue returned NULL "
                     "for QF %u", m_DecodeQueueFamily);
        return false;
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = m_DecodeQueueFamily;
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (pfnCreateCommandPool(m_Device, &cpi, nullptr, &m_DecodeCmdPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c vkCreateCommandPool failed");
        destroyDecodeCommandResources();
        return false;
    }

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = m_DecodeCmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (pfnAllocateCommandBuffers(m_Device, &cbai, &m_DecodeCmdBuf) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c vkAllocateCommandBuffers failed");
        destroyDecodeCommandResources();
        return false;
    }

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first wait succeeds immediately
    if (pfnCreateFence(m_Device, &fci, nullptr, &m_DecodeFence) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c vkCreateFence failed");
        destroyDecodeCommandResources();
        return false;
    }

    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (pfnCreateSemaphore(m_Device, &sci, nullptr, &m_DecodeDoneSem) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c vkCreateSemaphore failed");
        destroyDecodeCommandResources();
        return false;
    }

    // §J.3.e.2.i.8 Phase 1.5b — timeline semaphore for cross-queue sync.
    // Initial value 0; first decode signals 1, graphics waits >=1, etc.
    VkSemaphoreTypeCreateInfo timelineType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue  = 0;
    VkSemaphoreCreateInfo timelineSci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    timelineSci.pNext = &timelineType;
    if (pfnCreateSemaphore(m_Device, &timelineSci, nullptr, &m_TimelineSem) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.5b timeline vkCreateSemaphore failed");
        destroyDecodeCommandResources();
        return false;
    }
    m_TimelineNext.store(1);
    m_LastDecodeValue.store(0);

    // §J.3.e.2.i.8 Phase 1.5c — graphics→decode timeline semaphore, mirrors
    // m_TimelineSem in the opposite direction.  Replaces racey m_LastGraphicsFence.
    if (pfnCreateSemaphore(m_Device, &timelineSci, nullptr, &m_GfxTimelineSem) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.5c gfx timeline vkCreateSemaphore failed");
        destroyDecodeCommandResources();
        return false;
    }
    m_GfxTimelineNext.store(1);
    m_LastGraphicsValue.store(0);

    m_DecodeCmdReady   = true;
    m_DecodeNeedsReset = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 1.3c decode cmd resources ready "
                "(QF=%u, queue=%p, pool/cmdbuf/fence/sem allocated)",
                m_DecodeQueueFamily, (void*)m_DecodeQueue);
    return true;
}

void VkFrucRenderer::destroyDecodeCommandResources()
{
    if (m_Device == VK_NULL_HANDLE) {
        m_DecodeCmdReady = false;
        return;
    }
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDeviceWaitIdle    = (PFN_vkDeviceWaitIdle)getDevPa(m_Device, "vkDeviceWaitIdle");
    auto pfnDestroySemaphore  = (PFN_vkDestroySemaphore)getDevPa(m_Device, "vkDestroySemaphore");
    auto pfnDestroyFence      = (PFN_vkDestroyFence)getDevPa(m_Device, "vkDestroyFence");
    auto pfnFreeCommandBuffers= (PFN_vkFreeCommandBuffers)getDevPa(m_Device, "vkFreeCommandBuffers");
    auto pfnDestroyCommandPool= (PFN_vkDestroyCommandPool)getDevPa(m_Device, "vkDestroyCommandPool");

    // Idle the device so any in-flight decode submission completes before we
    // free its referenced cmd buffer / fence.
    if (pfnDeviceWaitIdle) pfnDeviceWaitIdle(m_Device);

    if (m_DecodeDoneSem  != VK_NULL_HANDLE && pfnDestroySemaphore)
        pfnDestroySemaphore(m_Device, m_DecodeDoneSem, nullptr);
    if (m_TimelineSem != VK_NULL_HANDLE && pfnDestroySemaphore)
        pfnDestroySemaphore(m_Device, m_TimelineSem, nullptr);
    m_TimelineSem = VK_NULL_HANDLE;
    m_TimelineNext.store(1);
    m_LastDecodeValue.store(0);
    if (m_GfxTimelineSem != VK_NULL_HANDLE && pfnDestroySemaphore)
        pfnDestroySemaphore(m_Device, m_GfxTimelineSem, nullptr);
    m_GfxTimelineSem = VK_NULL_HANDLE;
    m_GfxTimelineNext.store(1);
    m_LastGraphicsValue.store(0);
    if (m_DecodeFence    != VK_NULL_HANDLE && pfnDestroyFence)
        pfnDestroyFence(m_Device, m_DecodeFence, nullptr);
    if (m_DecodeCmdBuf   != VK_NULL_HANDLE && pfnFreeCommandBuffers && m_DecodeCmdPool != VK_NULL_HANDLE)
        pfnFreeCommandBuffers(m_Device, m_DecodeCmdPool, 1, &m_DecodeCmdBuf);
    if (m_DecodeCmdPool  != VK_NULL_HANDLE && pfnDestroyCommandPool)
        pfnDestroyCommandPool(m_Device, m_DecodeCmdPool, nullptr);

    m_DecodeDoneSem    = VK_NULL_HANDLE;
    m_DecodeFence      = VK_NULL_HANDLE;
    m_DecodeCmdBuf     = VK_NULL_HANDLE;
    m_DecodeCmdPool    = VK_NULL_HANDLE;
    m_DecodeQueue      = VK_NULL_HANDLE;
    m_DecodeCmdReady   = false;
    m_DecodeNeedsReset = true;
}

bool VkFrucRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    (void)options;
    // §J.3.e.2.i.3.e-SW — software-upload mode: no hwaccel binding, ffmpeg
    // decodes in CPU into AV_PIX_FMT_NV12 frames that we upload via our
    // staging buffer.
    if (m_SwMode) {
        (void)context;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW prepareDecoderContext OK "
                    "(software decode path; no hw_device_ctx binding)");
        return true;
    }
    if (m_HwDeviceCtx == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.a prepareDecoderContext called "
                    "without m_HwDeviceCtx ready");
        return false;
    }
    context->hw_device_ctx = av_buffer_ref(m_HwDeviceCtx);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.a prepareDecoderContext OK — ffmpeg "
                "will decode into AVVkFrame on our VkDevice");
    return true;
}

// §J.3.e.2.i.3.e — descriptor pool sized for kFrucFramesInFlight
// COMBINED_IMAGE_SAMPLER bindings (one per ring slot).  We pre-allocate
// the descriptor sets at init and re-update each frame to reference the
// new AVVkFrame's image view.
bool VkFrucRenderer::createDescriptorPool()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateDP = (PFN_vkCreateDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDS = (PFN_vkAllocateDescriptorSets)pfnGetDeviceProcAddr(
        m_Device, "vkAllocateDescriptorSets");
    if (!pfnCreateDP || !pfnAllocDS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e descriptor PFN load failed");
        return false;
    }

    // §J.3.e.2.i.3.e (v1.3.304 AMD fix v2) — descriptor pool sizing for
    // ycbcr immutable sampler, *driver-queried* multiplier.
    //
    // History:
    //   v1.3.303: ×4 hard-coded over-provision (assumed plane-count + headroom)
    //             — failed on AMD Vega 10 again (Ryzen log 1777706526.log
    //             still hits vkAllocateDescriptorSets failed at line 43).
    //   v1.3.304: query the actual count via vkGetPhysicalDeviceImage-
    //             FormatProperties2 + VkSamplerYcbcrConversionImage-
    //             FormatProperties::combinedImageSamplerDescriptorCount.
    //
    // Spec rationale (Vulkan §14.2.1):
    //   "When using a VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER with a
    //    sampler that uses Y'CBCR conversion, the driver may consume
    //    `combinedImageSamplerDescriptorCount` pool descriptors for each
    //    descriptor in the set."
    //
    // NV reports 1 (so ×1 always worked).  AMD reports up to 6 on some
    // multi-plane pixel formats (driver-internal storage of plane views +
    // mip / sampler caches).  Querying gives the actual number; we pick
    // a conservative fallback of 8 if the query fails (max we've seen on
    // any driver report is 4–6).
    // Default: brute-force over-provision.  Try driver query first to get
    // the exact number, but fall back to a generous static value so we
    // never under-size on drivers where the PFN resolve doesn't surface.
    uint32_t ycbcrDescCount = 16;  // generous static fallback
    bool     queryHit       = false;
    auto pfnGetPDIFP2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!pfnGetPDIFP2) {
        pfnGetPDIFP2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    if (pfnGetPDIFP2 && m_PhysicalDevice != VK_NULL_HANDLE) {
        VkSamplerYcbcrConversionImageFormatProperties ycbcrProps = {
            VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES
        };
        VkImageFormatProperties2 ifp2 = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        ifp2.pNext = &ycbcrProps;
        VkPhysicalDeviceImageFormatInfo2 ifInfo = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2
        };
        ifInfo.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
        ifInfo.type   = VK_IMAGE_TYPE_2D;
        ifInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        ifInfo.usage  = VK_IMAGE_USAGE_SAMPLED_BIT;
        VkResult vr = pfnGetPDIFP2(m_PhysicalDevice, &ifInfo, &ifp2);
        if (vr == VK_SUCCESS && ycbcrProps.combinedImageSamplerDescriptorCount > 0) {
            // Pick the larger of driver-reported and our fallback so a
            // driver that mis-reports a low value still gets enough pool.
            uint32_t reported = ycbcrProps.combinedImageSamplerDescriptorCount;
            ycbcrDescCount = (reported > ycbcrDescCount) ? reported : ycbcrDescCount;
            queryHit = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.3.e driver-reported ycbcr "
                        "combinedImageSamplerDescriptorCount=%u for NV12 — "
                        "using effective multiplier %u",
                        reported, ycbcrDescCount);
        }
    }
    if (!queryHit) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e ycbcr property query "
                    "unavailable (pfn=%p) — using static multiplier %u",
                    (void*)pfnGetPDIFP2, ycbcrDescCount);
    }

    VkDescriptorPoolSize poolSize = {};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kFrucFramesInFlight * ycbcrDescCount;

    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets       = kFrucFramesInFlight;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes    = &poolSize;
    if (pfnCreateDP(m_Device, &dpCi, nullptr, &m_DescPool) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetLayout layouts[kFrucFramesInFlight];
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        layouts[i] = m_DescSetLayout;
    }
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = m_DescPool;
    dsai.descriptorSetCount = kFrucFramesInFlight;
    dsai.pSetLayouts        = layouts;
    if (pfnAllocDS(m_Device, &dsai, m_SlotDescSet) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e vkAllocateDescriptorSets failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e descriptor pool + %u sets ready",
                (unsigned)kFrucFramesInFlight);
    return true;
}

void VkFrucRenderer::destroyDescriptorPool()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyDP = (PFN_vkDestroyDescriptorPool)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyDescriptorPool");
    auto pfnDestroyView = (PFN_vkDestroyImageView)pfnGetDeviceProcAddr(
        m_Device, "vkDestroyImageView");

    // Free any pending image views (per-slot views from the last in-flight
    // frame).  Caller has already drained the queue via vkDeviceWaitIdle
    // (in teardown sequence) so these are safe to destroy.
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        if (m_SlotPendingView[i] && pfnDestroyView) {
            pfnDestroyView(m_Device, m_SlotPendingView[i], nullptr);
            m_SlotPendingView[i] = VK_NULL_HANDLE;
        }
        m_SlotDescSet[i] = VK_NULL_HANDLE;  // freed implicitly with pool
    }
    if (m_DescPool && pfnDestroyDP) {
        pfnDestroyDP(m_Device, m_DescPool, nullptr);
        m_DescPool = VK_NULL_HANDLE;
    }
}

// §J.3.e.2.i.3.e — cache hot-path PFNs to avoid resolving via
// vkGetDeviceProcAddr on every renderFrame call (~120 fps in worst case).
bool VkFrucRenderer::loadRenderTimePfns()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnGetDeviceProcAddr) return false;

#define LOAD_RT(name)                                                          \
    m_RtPfn.name = (PFN_vk##name)pfnGetDeviceProcAddr(m_Device, "vk" #name);   \
    if (!m_RtPfn.name) {                                                       \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,                             \
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e PFN miss: vk" #name);     \
        return false;                                                          \
    }

    LOAD_RT(AcquireNextImageKHR);
    LOAD_RT(QueuePresentKHR);
    LOAD_RT(QueueSubmit);
    LOAD_RT(BeginCommandBuffer);
    LOAD_RT(EndCommandBuffer);
    LOAD_RT(ResetCommandBuffer);
    LOAD_RT(CmdPipelineBarrier);
    LOAD_RT(CmdBeginRenderPass);
    LOAD_RT(CmdEndRenderPass);
    LOAD_RT(CmdBindPipeline);
    LOAD_RT(CmdBindDescriptorSets);
    LOAD_RT(CmdDraw);
    LOAD_RT(WaitForFences);
    LOAD_RT(ResetFences);
    LOAD_RT(UpdateDescriptorSets);
    LOAD_RT(CreateImageView);
    LOAD_RT(DestroyImageView);
    LOAD_RT(CmdCopyBufferToImage);
    // §J.3.e.2.i.8 Phase 2.5 — image→buffer for native NV12 mirror.
    LOAD_RT(CmdCopyImageToBuffer);

#undef LOAD_RT
    return true;
}

// §J.3.e.2.i.3.e-SW — staging buffer + multi-plane NV12 VkImage for the
// software-decode upload path.  Allocates ONCE for the source resolution
// and reuses the image+buffer per-frame (Y/UV memcpy + cmdCopyBufferToImage).
bool VkFrucRenderer::createSwUploadResources(int width, int height)
{
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateBuffer = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem    = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem  = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnMapMem      = (PFN_vkMapMemory)getDevPa(m_Device, "vkMapMemory");
    auto pfnCreateImage = (PFN_vkCreateImage)getDevPa(m_Device, "vkCreateImage");
    auto pfnGetImgMemReq = (PFN_vkGetImageMemoryRequirements)getDevPa(m_Device, "vkGetImageMemoryRequirements");
    auto pfnBindImgMem  = (PFN_vkBindImageMemory)getDevPa(m_Device, "vkBindImageMemory");
    auto pfnGetMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem ||
        !pfnMapMem || !pfnCreateImage || !pfnGetImgMemReq || !pfnBindImgMem ||
        !pfnGetMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW PFN load failed");
        return false;
    }

    m_SwImageWidth  = width;
    m_SwImageHeight = height;
    // NV12: Y plane = w×h bytes, UV plane = (w×h)/2 bytes (interleaved at half
    // height).  Allocate kFrucFramesInFlight back-to-back per-slot regions so
    // the SW upload path (m_FrucMode=false) can pipeline CPU memcpy of frame N
    // with previous frame N-1's GPU vkCmdCopyBufferToImage.  At 4K (12 MB / slot)
    // this trims ~2-3ms of CPU memcpy off the per-frame critical path; the
    // memory cost (24 MB host-coherent total at 4K, 6 MB at 1080p) is trivial.
    m_SwStagingPerSlot = (size_t)width * height * 3 / 2;
    m_SwStagingSize    = m_SwStagingPerSlot * kFrucFramesInFlight;

    VkPhysicalDeviceMemoryProperties memProps;
    pfnGetMemProps(m_PhysicalDevice, &memProps);
    auto findMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags wanted) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return (int)i;
            }
        }
        return -1;
    };

    // Staging buffer: host-visible coherent.  STORAGE_BUFFER usage added
    // (in addition to TRANSFER_SRC) so §J.3.e.2.i.4.1 NV12→RGB compute can
    // sample raw NV12 bytes via storage buffer descriptor with Y at offset
    // 0 and UV at offset W*H — avoids an extra image→buffer copy step.
    {
        VkBufferCreateInfo bci = {};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = m_SwStagingSize;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &m_SwStagingBuffer) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateBuffer failed");
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, m_SwStagingBuffer, &mr);
        int memTypeIdx = findMemType(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memTypeIdx < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW no host-visible+coherent memory type");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)memTypeIdx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_SwStagingMem) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkAllocateMemory(staging) failed");
            return false;
        }
        if (pfnBindBufMem(m_Device, m_SwStagingBuffer, m_SwStagingMem, 0) != VK_SUCCESS ||
            pfnMapMem(m_Device, m_SwStagingMem, 0, VK_WHOLE_SIZE, 0, &m_SwStagingMapped) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW staging bind/map failed");
            return false;
        }
    }

    // §J.3.e.2.i.8 Phase 2.5 — single gpu-only NV12 buffer (DEVICE_LOCAL).
    // When native VK decode is active, renderFrameSw copies m_SwUploadImage
    // → m_SwFrucNv12Buf at the start of its graphics-queue cmd buf, then FRUC
    // NV12→RGB compute reads from here instead of m_SwStagingBuffer (which
    // holds FFmpeg's parallel SW decode output).  Removes the source
    // asymmetry that caused 3-4 Hz blur/sharp flicker in v1.3.275 dual-present
    // FRUC.
    //
    // v1.3.278 — Reverted from per-slot back to single buffer.  Per-slot
    // (v1.3.277) caused reliable VK_ERROR_DEVICE_LOST after ~1380 frames on
    // NV 596.84 + RTX 3060.  v1.3.276 single-buffer ran 2940+ frames stable
    // with a theoretical cross-submission WAW race manifesting as residual
    // "occasional blur" but no crash.  Trade-off: visual blur is acceptable,
    // hard crash is not.  Future work — diagnose per-slot crash (likely a
    // descriptor-set / buffer interaction with NV driver) before re-enabling.
    {
        VkBufferCreateInfo bci = {};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = m_SwStagingSize;  // same NV12 size as staging
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // graphics queue only
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &m_SwFrucNv12Buf) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2.5 vkCreateBuffer(SwFrucNv12) failed");
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, m_SwFrucNv12Buf, &mr);
        int memTypeIdx = findMemType(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memTypeIdx < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2.5 no DEVICE_LOCAL memory type for SwFrucNv12Buf");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)memTypeIdx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_SwFrucNv12BufMem) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2.5 vkAllocateMemory(SwFrucNv12) failed");
            return false;
        }
        if (pfnBindBufMem(m_Device, m_SwFrucNv12Buf, m_SwFrucNv12BufMem, 0) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.8 Phase 2.5 vkBindBufferMemory(SwFrucNv12) failed");
            return false;
        }
    }

    // Upload image: NV12 multi-plane, sampled + transfer-dst
    {
        VkImageCreateInfo ici = {};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        ici.extent.width  = (uint32_t)width;
        ici.extent.height = (uint32_t)height;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // §J.3.e.2.i.8 Phase 2.5 — TRANSFER_SRC added so renderFrameSw's graphics
        // cmd buf can vkCmdCopyImageToBuffer this image (NV12) into m_SwFrucNv12Buf
        // for FRUC compute consumption when native VK decode is active.  Avoids
        // FRUC sampling FFmpeg's parallel m_SwStagingBuffer (source asymmetry).
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                          | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT;
        // §J.3.e.2.i.8 Phase 1.4 — decode queue may also write to this image
        // (vkCmdCopyImage from DPB layer).  Use CONCURRENT sharing across
        // [graphics, decode] QFs to skip ownership transfer barriers.
        uint32_t swQfs[2] = { m_QueueFamily, m_DecodeQueueFamily };
        bool swConcurrent = (m_DecodeQueueFamily != UINT32_MAX) && (m_QueueFamily != m_DecodeQueueFamily);
        ici.sharingMode           = swConcurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        ici.queueFamilyIndexCount = swConcurrent ? 2u : 0u;
        ici.pQueueFamilyIndices   = swConcurrent ? swQfs : nullptr;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (pfnCreateImage(m_Device, &ici, nullptr, &m_SwUploadImage) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateImage failed");
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetImgMemReq(m_Device, m_SwUploadImage, &mr);
        int memTypeIdx = findMemType(mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memTypeIdx < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW no device-local memory type");
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)memTypeIdx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_SwUploadImageMem) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkAllocateMemory(image) failed");
            return false;
        }
        if (pfnBindImgMem(m_Device, m_SwUploadImage, m_SwUploadImageMem, 0) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkBindImageMemory failed");
            return false;
        }
    }

    // Image view with our YCbCr conversion baked in (matches descriptor
    // set layout's immutable sampler).  Built once, reused for all frames.
    {
        auto pfnCreateView = (PFN_vkCreateImageView)getDevPa(m_Device, "vkCreateImageView");
        VkSamplerYcbcrConversionInfo convInfo = {};
        convInfo.sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        convInfo.conversion = m_YcbcrConversion;

        VkImageViewCreateInfo vci = {};
        vci.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.pNext      = &convInfo;
        vci.image      = m_SwUploadImage;
        vci.viewType   = VK_IMAGE_VIEW_TYPE_2D;
        vci.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.layerCount     = 1;
        if (pfnCreateView(m_Device, &vci, nullptr, &m_SwUploadView) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW vkCreateImageView failed");
            return false;
        }
    }

    // Descriptor sets — both ring slots' descriptor sets point at the
    // same upload image view (we don't need per-slot views since the
    // image is single-buffered and reused).  Update once.
    {
        VkDescriptorImageInfo dii = {};
        dii.imageView   = m_SwUploadView;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet wds[kFrucFramesInFlight] = {};
        for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
            wds[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet          = m_SlotDescSet[i];
            wds[i].dstBinding      = 0;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds[i].pImageInfo      = &dii;
        }
        m_RtPfn.UpdateDescriptorSets(m_Device, kFrucFramesInFlight, wds, 0, nullptr);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.3.e-SW upload resources ready: "
                "%dx%d NV12, staging=%zu B, image=DEVICE_LOCAL",
                width, height, m_SwStagingSize);
    return true;
}

void VkFrucRenderer::destroySwUploadResources()
{
    if (m_Device == VK_NULL_HANDLE) return;
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyView   = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
    auto pfnDestroyImage  = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
    auto pfnDestroyBuffer = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem       = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnUnmapMem      = (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory");

    if (m_SwUploadView && pfnDestroyView) {
        pfnDestroyView(m_Device, m_SwUploadView, nullptr);
        m_SwUploadView = VK_NULL_HANDLE;
    }
    if (m_SwUploadImage && pfnDestroyImage) {
        pfnDestroyImage(m_Device, m_SwUploadImage, nullptr);
        m_SwUploadImage = VK_NULL_HANDLE;
    }
    if (m_SwUploadImageMem && pfnFreeMem) {
        pfnFreeMem(m_Device, m_SwUploadImageMem, nullptr);
        m_SwUploadImageMem = VK_NULL_HANDLE;
    }
    if (m_SwStagingMapped && pfnUnmapMem) {
        pfnUnmapMem(m_Device, m_SwStagingMem);
        m_SwStagingMapped = nullptr;
    }
    if (m_SwStagingBuffer && pfnDestroyBuffer) {
        pfnDestroyBuffer(m_Device, m_SwStagingBuffer, nullptr);
        m_SwStagingBuffer = VK_NULL_HANDLE;
    }
    // §J.3.e.2.i.8 Phase 2.5 — release gpu-only NV12 mirror buffer
    if (m_SwFrucNv12Buf && pfnDestroyBuffer) {
        pfnDestroyBuffer(m_Device, m_SwFrucNv12Buf, nullptr);
        m_SwFrucNv12Buf = VK_NULL_HANDLE;
    }
    if (m_SwFrucNv12BufMem && pfnFreeMem) {
        pfnFreeMem(m_Device, m_SwFrucNv12BufMem, nullptr);
        m_SwFrucNv12BufMem = VK_NULL_HANDLE;
    }
    if (m_SwStagingMem && pfnFreeMem) {
        pfnFreeMem(m_Device, m_SwStagingMem, nullptr);
        m_SwStagingMem = VK_NULL_HANDLE;
    }
}

// =====================================================================
// §J.3.e.2.i.4 — FRUC compute pipeline integration
//
// Port of PlVkRenderer::initFrucGenericResources / runFrucGenericComputePass
// (plvk.cpp:3604-4180).  Builds 3 compute pipelines (motionest / mv_median /
// warp), allocates planar fp32 RGB buffers + MV buffers, runs the chain
// every frame after our SW upload.
//
// i.4 first ship — placeholder bufRGB pair (zeros), no NV12→RGB feed yet,
// no interp display.  Validates compute pipeline integration in our
// VkFrucRenderer.  Future expansions:
//   • i.4.1 — add NV12→RGB compute feed from m_SwUploadImage
//   • i.4.2 — display m_FrucInterpRgbBuf via dual-present (with §J.3.e.2.i.5)
//
// Shader sources (kFrucMotionEstShaderGlsl etc.) are defined in plvk.cpp;
// extern-declared here so we can compile them without copy/paste.
// =====================================================================

// Forward declarations — defined in plvk.cpp (external linkage; not
// `static`).  Use C++ linkage to match the definitions there.
extern const char* kFrucMotionEstShaderGlsl;
extern const char* kFrucMvMedianShaderGlsl;
extern const char* kFrucWarpShaderGlsl;

// §J.3.e.2.i.4.1 — NV12 → planar fp32 RGB compute shader.  Reuses the
// algorithm from PlVkRenderer's §J.3.e.2.c kNv12RgbShaderGlsl (BT.709
// limited-range YCbCr → linear sRGB), but with a SINGLE input storage
// buffer (m_SwStagingBuffer): binding 0 reads the whole staging buffer
// as uint array; we compute Y plane offsets directly (offset 0..W*H) and
// UV plane offsets (W*H..W*H*3/2).
//
// PlVkRenderer keeps that shader `static` — can't extern.  Inline copy
// here, simplified to single-buffer.
static const char* kVkFrucNv12RgbShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Single buffer holding NV12: Y plane (W*H bytes) + UV plane (W*H/2 bytes,
// interleaved at half-resolution).  Read as uint32 array (4 bytes/elem).
layout(binding = 0) readonly  buffer NV12_in { uint  data[]; } nv12;
layout(binding = 1) writeonly buffer RGB_out { float data[]; } rgbOut;
layout(push_constant) uniform Params { int w; int h; int uvByteOffset; int _pad; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;

    // Y at byte offset (y * w + x), within [0, w*h)
    int yByteIdx = y * p.w + x;
    uint yWord   = nv12.data[yByteIdx >> 2];
    uint yByte   = (yWord >> ((yByteIdx & 3) * 8)) & 0xFFu;
    float Y_raw  = float(yByte) * (1.0 / 255.0);

    // UV plane starts at byte offset uvByteOffset (= w*h)
    int chromaX = x >> 1;
    int chromaY = y >> 1;
    int uvByteI = p.uvByteOffset + (chromaY * (p.w >> 1) + chromaX) * 2;
    uint uvWord0 = nv12.data[uvByteI >> 2];
    uint uvWord1 = nv12.data[(uvByteI + 1) >> 2];
    uint cbByte  = (uvWord0 >> ((uvByteI       & 3) * 8)) & 0xFFu;
    uint crByte  = (uvWord1 >> (((uvByteI + 1) & 3) * 8)) & 0xFFu;
    float Cb_raw = float(cbByte) * (1.0 / 255.0);
    float Cr_raw = float(crByte) * (1.0 / 255.0);

    // BT.709 limited-range YCbCr → linear sRGB
    float Y_n  = (Y_raw  - 16.0  / 255.0) * (255.0 / 219.0);
    float Cb_n = (Cb_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float Cr_n = (Cr_raw - 128.0 / 255.0) * (255.0 / 224.0);
    float r = clamp(Y_n + 1.5748 * Cr_n,                       0.0, 1.0);
    float g = clamp(Y_n - 0.1873 * Cb_n - 0.4681 * Cr_n,       0.0, 1.0);
    float b = clamp(Y_n + 1.8556 * Cb_n,                       0.0, 1.0);

    int outIdx    = y * p.w + x;
    int planeSize = p.w * p.h;
    rgbOut.data[outIdx + 0 * planeSize] = r;
    rgbOut.data[outIdx + 1 * planeSize] = g;
    rgbOut.data[outIdx + 2 * planeSize] = b;
}
)GLSL";

#include <ncnn/gpu.h>  // for ncnn::compile_spirv_module + ncnn::Option

bool VkFrucRenderer::createFrucComputeResources(int width, int height)
{
    if (m_FrucReady || m_FrucDisabled) return m_FrucReady;

    const uint32_t BLOCK_SIZE = 8;
    const uint32_t mvW = ((uint32_t)width  + BLOCK_SIZE - 1) / BLOCK_SIZE;
    const uint32_t mvH = ((uint32_t)height + BLOCK_SIZE - 1) / BLOCK_SIZE;
    m_FrucMvWidth  = mvW;
    m_FrucMvHeight = mvH;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: enter (W=%d H=%d block=%u mv=%ux%u)",
                width, height, BLOCK_SIZE, mvW, mvH);

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateShaderModule = (PFN_vkCreateShaderModule)getDevPa(m_Device, "vkCreateShaderModule");
    auto pfnCreateDsl          = (PFN_vkCreateDescriptorSetLayout)getDevPa(m_Device, "vkCreateDescriptorSetLayout");
    auto pfnCreatePipeLay      = (PFN_vkCreatePipelineLayout)getDevPa(m_Device, "vkCreatePipelineLayout");
    auto pfnCreateComputePipes = (PFN_vkCreateComputePipelines)getDevPa(m_Device, "vkCreateComputePipelines");
    auto pfnCreateBuffer       = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq       = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem           = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem         = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnCreateDescPool     = (PFN_vkCreateDescriptorPool)getDevPa(m_Device, "vkCreateDescriptorPool");
    auto pfnAllocDescSets      = (PFN_vkAllocateDescriptorSets)getDevPa(m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets     = (PFN_vkUpdateDescriptorSets)getDevPa(m_Device, "vkUpdateDescriptorSets");
    auto pfnGetPdMemProps      = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateShaderModule || !pfnCreateDsl || !pfnCreatePipeLay || !pfnCreateComputePipes
        || !pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem
        || !pfnCreateDescPool || !pfnAllocDescSets || !pfnUpdateDescSets || !pfnGetPdMemProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 PFN load failed");
        m_FrucDisabled = true;
        return false;
    }

    // §J.3.e.2.i.4 — ncnn::compile_spirv_module needs ncnn's Vulkan context
    // initialised.  PlVkRenderer does this via create_gpu_instance_external
    // (sharing libplacebo's VkDevice).  We don't have libplacebo here; use
    // the plain create_gpu_instance() which creates ncnn's own context.
    // Idempotent: returns 0 if already initialised.
    int ncnnInit = ncnn::create_gpu_instance();
    if (ncnnInit == 0) {
        // Successfully created or already created (idempotent).  Track
        // refcount so destroyFrucComputeResources knows to call destroy
        // when the last renderer instance tears down.
        m_NcnnInited = true;
        s_NcnnRefCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 ncnn::create_gpu_instance failed rc=%d "
                    "(may already be claimed; compile may still work)", ncnnInit);
    }

    auto buildPipeline = [&](const char* tag, const char* glsl, int numBindings,
                              uint32_t pcSize,
                              VkShaderModule& outMod, VkDescriptorSetLayout& outDsl,
                              VkPipelineLayout& outPL, VkPipeline& outPipe) -> bool {
        std::vector<uint32_t> spirv;
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(glsl, opt, spirv) != 0 || spirv.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.4 %s: compile_spirv_module failed", tag);
            return false;
        }
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(m_Device, &smCi, nullptr, &outMod) != VK_SUCCESS) return false;
        std::vector<VkDescriptorSetLayoutBinding> dslB(numBindings);
        for (int i = 0; i < numBindings; i++) {
            dslB[i].binding = (uint32_t)i;
            dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            dslB[i].descriptorCount = 1;
            dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = (uint32_t)numBindings;
        dslCi.pBindings = dslB.data();
        if (pfnCreateDsl(m_Device, &dslCi, nullptr, &outDsl) != VK_SUCCESS) return false;
        VkPushConstantRange pcRange = {};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.size = pcSize;
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &outDsl;
        plCi.pushConstantRangeCount = pcSize > 0 ? 1 : 0;
        plCi.pPushConstantRanges = pcSize > 0 ? &pcRange : nullptr;
        if (pfnCreatePipeLay(m_Device, &plCi, nullptr, &outPL) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = outMod;
        cpCi.stage.pName = "main";
        cpCi.layout = outPL;
        if (pfnCreateComputePipes(m_Device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &outPipe) != VK_SUCCESS) return false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 %s: pipeline built (spv=%zu B, %d bind, pc=%u B)",
                    tag, spirv.size() * sizeof(uint32_t), numBindings, pcSize);
        return true;
    };

    // §J.3.e.2.i.4.1 — NV12 → RGB compute pipeline (2 bindings, 16-byte
    // push const = w/h/uvByteOffset/_pad).
    if (!buildPipeline("NV12->RGB", kVkFrucNv12RgbShaderGlsl, 2, 16,
                        m_FrucNv12RgbShaderMod, m_FrucNv12RgbDsl,
                        m_FrucNv12RgbPipeLay, m_FrucNv12RgbPipeline)) {
        m_FrucDisabled = true; return false;
    }

    if (!buildPipeline("ME", kFrucMotionEstShaderGlsl, 4, 24,
                        m_FrucMeShaderMod, m_FrucMeDsl, m_FrucMePipeLay, m_FrucMePipeline)) {
        m_FrucDisabled = true; return false;
    }
    if (!buildPipeline("Median", kFrucMvMedianShaderGlsl, 2, 16,
                        m_FrucMedianShaderMod, m_FrucMedianDsl, m_FrucMedianPipeLay, m_FrucMedianPipeline)) {
        m_FrucDisabled = true; return false;
    }
    if (!buildPipeline("Warp", kFrucWarpShaderGlsl, 4, 24,
                        m_FrucWarpShaderMod, m_FrucWarpDsl, m_FrucWarpPipeLay, m_FrucWarpPipeline)) {
        m_FrucDisabled = true; return false;
    }

    // === Allocate buffers (DEVICE_LOCAL) ===
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(m_PhysicalDevice, &memProps);
    auto pickMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags want) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want)
                return (int)i;
        }
        return -1;
    };
    auto allocBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer& outBuf, VkDeviceMemory& outMem) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq = {};
        pfnGetBufMemReq(m_Device, outBuf, &memReq);
        int mti = pickMemType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mti < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = (uint32_t)mti;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        return pfnBindBufMem(m_Device, outBuf, outMem, 0) == VK_SUCCESS;
    };
    const VkDeviceSize sizeRGB = (VkDeviceSize)width * height * 3 * sizeof(float);
    const VkDeviceSize sizeMV  = (VkDeviceSize)mvW * mvH * 2 * sizeof(int);

    // §J.3.e.2.i.8 Phase 1.5c — TRANSFER_SRC_BIT added because runFrucComputeChain
    // ends with vkCmdCopyBuffer m_FrucCurrRgbBuf → m_FrucPrevRgbBuf for next-
    // frame ME ping-pong.  Validation VUID-vkCmdCopyBuffer-srcBuffer-00118 fires
    // without it (caught in v1.3.287 PARALLEL mode test with VK_LAYER_KHRONOS_validation).
    if (!allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                         | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  m_FrucPrevRgbBuf, m_FrucPrevRgbBufMem)
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_FrucCurrRgbBuf, m_FrucCurrRgbBufMem)
        // §J.3.e.2.i.8 Phase 1.5c — TRANSFER_SRC on m_FrucMvBuf because the
        // median pass cmdCopyBuffer's it into m_FrucMvFilteredBuf.
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_FrucMvBuf, m_FrucMvBufMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucPrevMvBuf, m_FrucPrevMvMem)
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     m_FrucInterpRgbBuf, m_FrucInterpRgbMem)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: buffer alloc failed");
        m_FrucDisabled = true;
        return false;
    }

    // === Descriptor pool: 5 sets × (2+2+4+2+4) = 14 storage-buffer descriptors ===
    // (i.4.1 added NV12→RGB with 2 bindings → 1 more set, 2 more descriptors)
    // (Phase 2.5 added 2nd NV12→RGB descriptor set whose binding 0 points at
    //  m_SwFrucNv12Buf for native-decode source → +1 set, +2 descriptors)
    // (v1.3.278 reverted Phase 2.5h per-slot variant — single descset only.)
    VkDescriptorPoolSize pSize = {};
    pSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pSize.descriptorCount = 14;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 5;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &pSize;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_FrucDescPool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: vkCreateDescriptorPool failed");
        m_FrucDisabled = true; return false;
    }

    auto allocAndUpdateSet = [&](VkDescriptorSetLayout dsl,
                                  std::vector<VkBuffer> bufs,
                                  VkDescriptorSet& outDs) -> bool {
        VkDescriptorSetAllocateInfo asi = {};
        asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        asi.descriptorPool = m_FrucDescPool;
        asi.descriptorSetCount = 1;
        asi.pSetLayouts = &dsl;
        if (pfnAllocDescSets(m_Device, &asi, &outDs) != VK_SUCCESS) return false;
        std::vector<VkDescriptorBufferInfo> bi(bufs.size());
        std::vector<VkWriteDescriptorSet> wr(bufs.size());
        for (size_t i = 0; i < bufs.size(); i++) {
            bi[i].buffer = bufs[i];
            bi[i].offset = 0;
            bi[i].range = VK_WHOLE_SIZE;
            wr[i] = {};
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = outDs;
            wr[i].dstBinding = (uint32_t)i;
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[i].pBufferInfo = &bi[i];
        }
        pfnUpdateDescSets(m_Device, (uint32_t)wr.size(), wr.data(), 0, nullptr);
        return true;
    };

    // §J.3.e.2.i.4.1 NV12→RGB descriptor: binding 0 = staging buffer
    // (entire NV12), binding 1 = currRGB output buffer.
    //
    // §J.3.e.2.i.8 Phase 2.5 — second NV12→RGB descriptor set with the same DSL
    // but binding 0 pointing at m_SwFrucNv12Buf (gpu-only NV12 mirror filled by
    // graphics-queue image→buffer copy from m_SwUploadImage when native VK
    // decode is active).  runFrucComputeChain selects between the two via the
    // useNativeSrc parameter.
    if (!allocAndUpdateSet(m_FrucNv12RgbDsl,
                           { m_SwStagingBuffer, m_FrucCurrRgbBuf },
                           m_FrucNv12RgbDescSet)
        || !allocAndUpdateSet(m_FrucNv12RgbDsl,
                              { m_SwFrucNv12Buf, m_FrucCurrRgbBuf },
                              m_FrucNv12RgbDescSetNative)
        || !allocAndUpdateSet(m_FrucMeDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucPrevMvBuf, m_FrucMvBuf },
                              m_FrucMeDescSet)
        || !allocAndUpdateSet(m_FrucMedianDsl,
                              { m_FrucMvBuf, m_FrucMvFilteredBuf },
                              m_FrucMedianDescSet)
        || !allocAndUpdateSet(m_FrucWarpDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvFilteredBuf, m_FrucInterpRgbBuf },
                              m_FrucWarpDescSet)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: descriptor set alloc/update failed");
        m_FrucDisabled = true; return false;
    }

    // §J.3.e.2.i.6 GPU timestamp query pool + timestampPeriod cache.
    // Optional — failure here doesn't block FRUC compute, just disables timing.
    {
        auto pfnCreateQueryPool = (PFN_vkCreateQueryPool)getDevPa(m_Device, "vkCreateQueryPool");
        auto pfnGetPhysProps    = (PFN_vkGetPhysicalDeviceProperties)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetPhysicalDeviceProperties");
        if (pfnCreateQueryPool && pfnGetPhysProps) {
            VkPhysicalDeviceProperties props = {};
            pfnGetPhysProps(m_PhysicalDevice, &props);
            m_FrucTimerNsPerTick = (double)props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qpCi = {};
            qpCi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpCi.queryType  = VK_QUERY_TYPE_TIMESTAMP;
            qpCi.queryCount = 2 * kFrucFramesInFlight;  // 2 timestamps per slot
            if (pfnCreateQueryPool(m_Device, &qpCi, nullptr, &m_FrucTimerPool) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.6 timestamp pool create failed (non-fatal)");
                m_FrucTimerPool = VK_NULL_HANDLE;
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.6 timestamp pool ready "
                            "(period=%.2f ns/tick, %u queries)",
                            m_FrucTimerNsPerTick, qpCi.queryCount);
            }
        }
    }

    m_FrucReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: PASS — 3 pipelines + 6 buffers + 3 descSets ready "
                "(sizeRGB=%llu, sizeMV=%llu)",
                (unsigned long long)sizeRGB, (unsigned long long)sizeMV);
    return true;
}

void VkFrucRenderer::destroyFrucComputeResources()
{
    if (!m_FrucReady && !m_FrucMePipeline) return;
    if (m_Device == VK_NULL_HANDLE) return;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnDestroyPipe     = (PFN_vkDestroyPipeline)getDevPa(m_Device, "vkDestroyPipeline");
    auto pfnDestroyPL       = (PFN_vkDestroyPipelineLayout)getDevPa(m_Device, "vkDestroyPipelineLayout");
    auto pfnDestroyDsl      = (PFN_vkDestroyDescriptorSetLayout)getDevPa(m_Device, "vkDestroyDescriptorSetLayout");
    auto pfnDestroyShader   = (PFN_vkDestroyShaderModule)getDevPa(m_Device, "vkDestroyShaderModule");
    auto pfnDestroyBuf      = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
    auto pfnFreeMem         = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
    auto pfnDestroyDescPool = (PFN_vkDestroyDescriptorPool)getDevPa(m_Device, "vkDestroyDescriptorPool");

#define DESTROY_PIPE(p, l, d, s)                                          \
    if (p && pfnDestroyPipe)   { pfnDestroyPipe(m_Device,   p, nullptr); p = VK_NULL_HANDLE; } \
    if (l && pfnDestroyPL)     { pfnDestroyPL(m_Device,     l, nullptr); l = VK_NULL_HANDLE; } \
    if (d && pfnDestroyDsl)    { pfnDestroyDsl(m_Device,    d, nullptr); d = VK_NULL_HANDLE; } \
    if (s && pfnDestroyShader) { pfnDestroyShader(m_Device, s, nullptr); s = VK_NULL_HANDLE; }
    DESTROY_PIPE(m_FrucNv12RgbPipeline, m_FrucNv12RgbPipeLay, m_FrucNv12RgbDsl, m_FrucNv12RgbShaderMod)
    DESTROY_PIPE(m_FrucMePipeline,      m_FrucMePipeLay,      m_FrucMeDsl,      m_FrucMeShaderMod)
    DESTROY_PIPE(m_FrucMedianPipeline,  m_FrucMedianPipeLay,  m_FrucMedianDsl,  m_FrucMedianShaderMod)
    DESTROY_PIPE(m_FrucWarpPipeline,    m_FrucWarpPipeLay,    m_FrucWarpDsl,    m_FrucWarpShaderMod)
#undef DESTROY_PIPE

#define DESTROY_BUF(b, m)                                          \
    if (b && pfnDestroyBuf) { pfnDestroyBuf(m_Device, b, nullptr); b = VK_NULL_HANDLE; } \
    if (m && pfnFreeMem)    { pfnFreeMem(m_Device,    m, nullptr); m = VK_NULL_HANDLE; }
    DESTROY_BUF(m_FrucPrevRgbBuf,    m_FrucPrevRgbBufMem)
    DESTROY_BUF(m_FrucCurrRgbBuf,    m_FrucCurrRgbBufMem)
    DESTROY_BUF(m_FrucMvBuf,         m_FrucMvBufMem)
    DESTROY_BUF(m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
    DESTROY_BUF(m_FrucPrevMvBuf,     m_FrucPrevMvMem)
    DESTROY_BUF(m_FrucInterpRgbBuf,  m_FrucInterpRgbMem)
#undef DESTROY_BUF

    if (m_FrucDescPool && pfnDestroyDescPool) {
        pfnDestroyDescPool(m_Device, m_FrucDescPool, nullptr);
        m_FrucDescPool = VK_NULL_HANDLE;
    }

    // §J.3.e.2.i.6 timestamp pool
    auto pfnDestroyQueryPool = (PFN_vkDestroyQueryPool)getDevPa(m_Device, "vkDestroyQueryPool");
    if (m_FrucTimerPool && pfnDestroyQueryPool) {
        pfnDestroyQueryPool(m_Device, m_FrucTimerPool, nullptr);
        m_FrucTimerPool = VK_NULL_HANDLE;
    }

    // §J.3.e.2.i.6 teardown crash fix — when the last ref drops, call
    // ncnn::destroy_gpu_instance() so ncnn's internal Vulkan resources
    // are released BEFORE process exit (their static dtors otherwise hit
    // stale state that we already destroyed → SIGSEGV).  Use fetch_sub:
    // returns OLD value, so old==1 means we're the last one releasing.
    if (m_NcnnInited) {
        if (s_NcnnRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.6 last ref — calling "
                        "ncnn::destroy_gpu_instance()");
            ncnn::destroy_gpu_instance();
        }
        m_NcnnInited = false;
    }

    m_FrucReady = false;
}

// §J.3.e.2.i.4 — record FRUC compute chain into the existing renderFrame
// command buffer (we don't use a separate compute queue/cmdpool — runs on
// our universal graphics queue with explicit pipeline barriers).
//
// Push constant layouts (must match the GLSL shader expectations from
// PlVkRenderer):
//   ME     (24 bytes): vec2 invSize / int mvW / int mvH / int blockSize
//                       — but actually: int srcW,srcH,mvW,mvH,blockSize,frameNum
//   Median (16 bytes): int mvW,mvH,radius,reserved
//   Warp   (24 bytes): int srcW,srcH,mvW,mvH,blockSize,frameNum
bool VkFrucRenderer::runFrucComputeChain(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                          bool useNativeSrc)
{
    if (!m_FrucReady) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdBindPipeline    = (PFN_vkCmdBindPipeline)getDevPa(m_Device, "vkCmdBindPipeline");
    auto pfnCmdBindDescSets    = (PFN_vkCmdBindDescriptorSets)getDevPa(m_Device, "vkCmdBindDescriptorSets");
    auto pfnCmdPushConst       = (PFN_vkCmdPushConstants)getDevPa(m_Device, "vkCmdPushConstants");
    auto pfnCmdDispatch        = (PFN_vkCmdDispatch)getDevPa(m_Device, "vkCmdDispatch");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    auto pfnCmdCopyBuffer      = (PFN_vkCmdCopyBuffer)getDevPa(m_Device, "vkCmdCopyBuffer");
    auto pfnCmdResetQueryPool  = (PFN_vkCmdResetQueryPool)getDevPa(m_Device, "vkCmdResetQueryPool");
    auto pfnCmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp)getDevPa(m_Device, "vkCmdWriteTimestamp");
    auto pfnGetQueryPoolResults = (PFN_vkGetQueryPoolResults)getDevPa(m_Device, "vkGetQueryPoolResults");
    if (!pfnCmdBindPipeline || !pfnCmdBindDescSets || !pfnCmdPushConst
        || !pfnCmdDispatch || !pfnCmdPipelineBarrier || !pfnCmdCopyBuffer) return false;

    // §J.3.e.2.i.6 — read PREVIOUS pass's timestamps for THIS slot (fence
    // wait at start of renderFrameSw guarantees GPU finished).  Skip on
    // first iteration when not yet armed.
    const uint32_t timerSlot = m_FrucTimerSlot;
    const uint32_t timerBase = timerSlot * 2;
    if (m_FrucTimerPool && pfnGetQueryPoolResults && pfnCmdResetQueryPool
        && pfnCmdWriteTimestamp && m_FrucTimerArmed[timerSlot]) {
        uint64_t ts[2] = {};
        VkResult qr = pfnGetQueryPoolResults(m_Device, m_FrucTimerPool,
            timerBase, 2, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (qr == VK_SUCCESS && ts[1] >= ts[0]) {
            uint64_t deltaTicks = ts[1] - ts[0];
            double deltaUs = (double)deltaTicks * m_FrucTimerNsPerTick / 1000.0;
            m_FrucGpuUsAccum += deltaUs;
            m_FrucGpuUsCount++;
        }
    }
    // Reset queries for this slot before re-using.
    if (m_FrucTimerPool && pfnCmdResetQueryPool) {
        pfnCmdResetQueryPool(cmd, m_FrucTimerPool, timerBase, 2);
    }
    m_FrucTimerSlot = (m_FrucTimerSlot + 1) % kFrucFramesInFlight;

    const uint32_t mvW = m_FrucMvWidth;
    const uint32_t mvH = m_FrucMvHeight;
    const uint32_t BLOCK_SIZE = 8;
    const uint32_t MEDIAN_RADIUS = 1;
    const uint32_t frameNum = (uint32_t)(m_FrucFrameCount++);

    auto bufBarrier = [&](VkBuffer b,
                          VkPipelineStageFlags srcStage,
                          VkPipelineStageFlags dstStage,
                          VkAccessFlags srcAcc, VkAccessFlags dstAcc) {
        VkBufferMemoryBarrier bmb = {};
        bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask = srcAcc;
        bmb.dstAccessMask = dstAcc;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = b;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };
    auto computeBufBarrier = [&](VkBuffer b) {
        bufBarrier(b,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    // §J.3.e.2.i.6 — write chain_start timestamp BEFORE first dispatch.
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 0);
    }

    // ---- Stage 0 (i.4.1): NV12 → planar fp32 RGB ----
    // Reads NV12 source → writes m_FrucCurrRgbBuf.
    //
    // §J.3.e.2.i.8 Phase 2.5 (FIXES the v1.3.275 KNOWN LIMITATION):
    //   When useNativeSrc=true, binding 0 of this pipeline points at
    //   m_SwFrucNv12Buf (gpu-only NV12 mirror of m_SwUploadImage, populated
    //   by graphics-queue vkCmdCopyImageToBuffer at the start of
    //   renderFrameSw's cmd buf, after a TRANSFER-stage timeline-sem wait
    //   on decode-queue completion).  When useNativeSrc=false, binding 0
    //   stays on m_SwStagingBuffer (FFmpeg's host-coherent memcpy buffer).
    //   Both descriptor sets share m_FrucNv12RgbDsl; only binding 0 differs.
    //   This keeps FRUC's interpolation source identical to the real-frame
    //   sample source, eliminating the dual-present source asymmetry that
    //   produced 3-4 Hz blur/sharp flicker.
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeline);
    VkDescriptorSet nv12RgbDescSet = useNativeSrc ? m_FrucNv12RgbDescSetNative
                                                  : m_FrucNv12RgbDescSet;
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeLay,
                       0, 1, &nv12RgbDescSet, 0, nullptr);
    {
        struct { int w, h, uvByteOffset, _pad; } pcN = {
            (int)width, (int)height, (int)(width * height), 0
        };
        pfnCmdPushConst(cmd, m_FrucNv12RgbPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcN), &pcN);
    }
    pfnCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    computeBufBarrier(m_FrucCurrRgbBuf);

    // ---- Stage 1: motion estimation ----
    // Push constant layout MUST match shader's struct order exactly:
    //   ME shader (plvk.cpp:1275-1282): frameWidth, frameHeight, blockSize,
    //   mvWidth, mvHeight, _pad0  — 24 bytes total
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeLay,
                       0, 1, &m_FrucMeDescSet, 0, nullptr);
    {
        struct {
            uint32_t frameWidth, frameHeight, blockSize, mvWidth, mvHeight, _pad0;
        } pcME = {
            (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
            (uint32_t)mvW, (uint32_t)mvH, (uint32_t)frameNum
        };
        pfnCmdPushConst(cmd, m_FrucMePipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcME), &pcME);
    }
    pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
    computeBufBarrier(m_FrucMvBuf);

    // ---- Stage 2: MV median filter ----
    // §J.3.e.2.i.7 R5: 把 median compute pass 換成 cmdCopyBuffer
    // (m_FrucMvBuf → m_FrucMvFilteredBuf)，等同 noop 過濾.  目標是測 median
    // 對視覺品質的實際貢獻 vs GPU 時間節省 (median 預估佔 0.2-0.3ms).
    // Warp 仍從 m_FrucMvFilteredBuf 讀，binding 不變.
    {
        VkBufferCopy cp = {};
        cp.size = (VkDeviceSize)mvW * mvH * 2 * sizeof(int);  // mvX + mvY per block
        pfnCmdCopyBuffer(cmd, m_FrucMvBuf, m_FrucMvFilteredBuf, 1, &cp);
    }
    computeBufBarrier(m_FrucMvFilteredBuf);

    // ---- Stage 3: warp ----
    // Push constant layout MUST match shader's struct order exactly:
    //   Warp shader (plvk.cpp:1563-1570): frameWidth, frameHeight,
    //   mvBlockSize, mvWidth, mvHeight, blendFactor (float, NOT int)
    //   — 24 bytes total.  blendFactor=0.5 = midpoint interpolation.
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeLay,
                       0, 1, &m_FrucWarpDescSet, 0, nullptr);
    {
        struct {
            uint32_t frameWidth, frameHeight, mvBlockSize, mvWidth, mvHeight;
            float    blendFactor;
        } pcWarp = {
            (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
            (uint32_t)mvW, (uint32_t)mvH, 0.5f
        };
        pfnCmdPushConst(cmd, m_FrucWarpPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcWarp), &pcWarp);
    }
    // Warp shader local_size = 8x8 (plvk.cpp:1551), one thread per pixel.
    // Bug history: dispatched (W+15)/16 = W/16 workgroups → only covered
    // 1/4 of interpRGB → top-left quadrant correct, other 3 quadrants left
    // with stale/uninit garbage → visible flicker on dual-present (user
    // observation: 第二象限 only quadrant without flicker).
    pfnCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
    computeBufBarrier(m_FrucInterpRgbBuf);

    // ---- Stage 4 (i.4.1): currRGB → prevRGB for next frame's ME ----
    // ME shader reads (prevRGB, currRGB) — we want prevRGB to be the
    // PREVIOUS frame's RGB on next call.  Copy curr→prev at end of chain.
    // (Alternative: ping-pong descriptors, but cmdCopyBuffer is simpler.)
    bufBarrier(m_FrucCurrRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bufBarrier(m_FrucPrevRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy cpyRegion = {};
    cpyRegion.srcOffset = 0;
    cpyRegion.dstOffset = 0;
    cpyRegion.size      = (VkDeviceSize)width * height * 3 * sizeof(float);
    pfnCmdCopyBuffer(cmd, m_FrucCurrRgbBuf, m_FrucPrevRgbBuf, 1, &cpyRegion);
    // Make prev visible to next frame's compute reads (via implicit chain
    // — next frame's NV12→RGB writes currRGB, ME reads both).
    bufBarrier(m_FrucPrevRgbBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // §J.3.e.2.i.6 — write chain_end timestamp AFTER last barrier.
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 1);
        m_FrucTimerArmed[timerSlot] = true;
    }

    return true;
}

// §J.3.e.2.i.3.e — full renderFrame impl.
//
// Per-frame steps (single-present; dual-present added in i.5):
//   1. Slot rotation + vkWaitForFences (drain prior frame in this slot)
//   2. Destroy slot's pending image view (from the prior frame in this slot)
//   3. vkAcquireNextImageKHR with slot's acquireSem[0]
//   4. Lock AVVkFrame, build VkImageView with VkSamplerYcbcrConversionInfo
//   5. Update slot's descriptor set with the new view
//   6. Record cmd buffer:
//        a. QFOT acquire + layout transition for AVVkFrame.img[0]
//        b. vkCmdBeginRenderPass on framebuffer[imgIdx]
//        c. Bind pipeline + descriptor set
//        d. vkCmdDraw(3, 1, 0, 0)
//        e. vkCmdEndRenderPass
//   7. vkQueueSubmit:
//        - waitSems = [acquireSem[0], AVVkFrame.sem[0]@sem_value[0]]
//        - signalSems = [renderDoneSem[0], AVVkFrame.sem[0]@sem_value[0]+1]
//        - fence = inFlightFence (CPU drains here next iteration)
//   8. Update AVVkFrame.layout/access/queue_family/sem_value
//   9. Unlock AVVkFrame
//  10. vkQueuePresentKHR waiting on renderDoneSem[0]
void VkFrucRenderer::renderFrame(AVFrame* frame)
{
    // §J.3.e.2.i.3.e-SW dispatch: software upload path validates i.3
    // graphics pipeline in isolation from FFmpeg-Vulkan hwcontext.
    if (m_SwMode) {
        renderFrameSw(frame);
        return;
    }

    static std::atomic<uint64_t> s_FrameCount{0};
    uint64_t fnum = s_FrameCount.fetch_add(1, std::memory_order_relaxed);
    bool firstFrame = (fnum < 3);   // log first 3 frames for bisect coverage
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.3.e renderFrame#%llu ENTRY frame=%p data[0]=%p "
                    "hw_frames_ctx=%p", (unsigned long long)fnum, (void*)frame,
                    frame ? frame->data[0] : nullptr,
                    frame ? (void*)frame->hw_frames_ctx : nullptr);
    }

    // §J.3.e.2.i.3.e DIAGNOSTIC: VIPLE_VKFRUC_DIAG_EMPTY=1 returns
    // immediately — pure ABI smoke test for IFFmpegRenderer interface.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_DIAG_EMPTY") != 0) {
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_EMPTY: empty return",
                                     (unsigned long long)fnum);
        return;
    }

    // §J.3.e.2.i.3.e DIAGNOSTIC: when VIPLE_VKFRUC_DIAG_NOAVVKFRAME=1, skip
    // ALL AVVkFrame interaction (no lock_frame, no image view, no descriptor
    // update, no sem wait/signal, no state mutation) and just clear-and-present
    // the swapchain.  Isolates whether the v1.3.123-130 crash-after-frame#0 is
    // in the AVVkFrame interaction or in the cmd record/submit/present cycle
    // itself.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_DIAG_NOAVVKFRAME") != 0) {
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_NOAVVKFRAME: clear-only path",
                                     (unsigned long long)fnum);

        uint32_t slot = m_CurrentSlot;
        m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;
        m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot], VK_TRUE, UINT64_MAX);
        m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);

        uint32_t imgIdx = 0;
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdx);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) return;

        VkCommandBuffer cmd = m_SlotCmdBuf[slot];
        m_RtPfn.ResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

        VkClearValue clearVal = {};
        clearVal.color.float32[0] = 0.5f;  // mid-grey so we can see it's working
        clearVal.color.float32[1] = 0.0f;
        clearVal.color.float32[2] = 0.5f;
        clearVal.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rpbi = {};
        rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbi.renderPass           = m_RenderPass;
        rpbi.framebuffer          = m_Framebuffers[imgIdx];
        rpbi.renderArea.offset    = { 0, 0 };
        rpbi.renderArea.extent    = m_SwapchainExtent;
        rpbi.clearValueCount      = 1;
        rpbi.pClearValues         = &clearVal;
        m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        // No bind/draw — clear-only.
        m_RtPfn.CmdEndRenderPass(cmd);
        m_RtPfn.EndCommandBuffer(cmd);

        VkPipelineStageFlags waitMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_SlotAcquireSem[slot][0];
        si.pWaitDstStageMask    = &waitMask;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_SlotRenderDoneSem[slot][0];
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }

        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_Swapchain;
        pi.pImageIndices      = &imgIdx;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
        }
        if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                     "[VIPLE-VKFRUC] frame#%llu DIAG_NOAVVKFRAME OK",
                                     (unsigned long long)fnum);
        return;  // bypass real path
    }

    if (frame == nullptr || frame->data[0] == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] renderFrame: null frame/data");
        return;
    }
    if (frame->hw_frames_ctx == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] renderFrame: null hw_frames_ctx — non-hwaccel frame");
        return;
    }

    AVVkFrame* vkf = (AVVkFrame*)frame->data[0];
    AVHWFramesContext* fc = (AVHWFramesContext*)frame->hw_frames_ctx->data;
    AVVulkanFramesContext* vkfc = (AVVulkanFramesContext*)fc->hwctx;
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkf=%p img[0]=%p img[1]=%p layout[0]=%d "
                    "queue_family[0]=%u sem[0]=%p sem_value[0]=%llu fc=%p vkfc=%p "
                    "lock_frame=%p", (void*)vkf,
                    vkf ? (void*)vkf->img[0] : nullptr,
                    vkf ? (void*)vkf->img[1] : nullptr,
                    vkf ? (int)vkf->layout[0] : -1,
                    vkf ? (unsigned)vkf->queue_family[0] : 0,
                    vkf ? (void*)vkf->sem[0] : nullptr,
                    vkf ? (unsigned long long)vkf->sem_value[0] : 0,
                    (void*)fc, (void*)vkfc,
                    vkfc ? (void*)vkfc->lock_frame : nullptr);
    }

    // ---- 1. Slot rotation + fence wait ----
    uint32_t slot = m_CurrentSlot;
    m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;

    m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot],
                          VK_TRUE, UINT64_MAX);
    m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);

    // ---- 2. Destroy slot's pending image view from prior frame ----
    if (m_SlotPendingView[slot] != VK_NULL_HANDLE) {
        m_RtPfn.DestroyImageView(m_Device, m_SlotPendingView[slot], nullptr);
        m_SlotPendingView[slot] = VK_NULL_HANDLE;
    }

    // ---- 3. Acquire next swapchain image ----
    uint32_t imgIdx = 0;
    VkResult vr = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                              m_SlotAcquireSem[slot][0],
                                              VK_NULL_HANDLE, &imgIdx);
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkAcquireNextImageKHR failed (%d)", (int)vr);
        // Re-sign the fence so next iteration doesn't deadlock; resize / recreate
        // is i.6 work — for now just bail.
        m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
        return;
    }

    // ---- 4. Lock AVVkFrame, build image view ----
    // §J.3.e.2.i.3.e: lock_frame is documented as required when mutating
    // AVVkFrame metadata; in practice FFmpeg-vulkan always sets it via its
    // default mutex helper but defensive null-check is cheap insurance.
    if (vkfc->lock_frame) vkfc->lock_frame(fc, vkf);
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 after lock_frame OK — building image view");
    }

    VkSamplerYcbcrConversionInfo convInfo = {};
    convInfo.sType      = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    convInfo.conversion = m_YcbcrConversion;

    VkImageViewCreateInfo viewCi = {};
    viewCi.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCi.pNext      = &convInfo;
    viewCi.image      = vkf->img[0];
    viewCi.viewType   = VK_IMAGE_VIEW_TYPE_2D;
    viewCi.format     = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    viewCi.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    viewCi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCi.subresourceRange.baseMipLevel   = 0;
    viewCi.subresourceRange.levelCount     = 1;
    viewCi.subresourceRange.baseArrayLayer = 0;
    viewCi.subresourceRange.layerCount     = 1;

    VkImageView frameView = VK_NULL_HANDLE;
    VkResult viewVr = m_RtPfn.CreateImageView(m_Device, &viewCi, nullptr, &frameView);
    if (viewVr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkCreateImageView for AVVkFrame failed (%d)", (int)viewVr);
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
        return;
    }
    m_SlotPendingView[slot] = frameView;
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 image view created OK (%p)", (void*)frameView);
    }

    // ---- 5. Update slot's descriptor set with the new view ----
    VkDescriptorImageInfo dii = {};
    dii.sampler     = VK_NULL_HANDLE;  // immutable, baked into layout
    dii.imageView   = frameView;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = m_SlotDescSet[slot];
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    m_RtPfn.UpdateDescriptorSets(m_Device, 1, &wds, 0, nullptr);

    // ---- 6. Record cmd buffer ----
    VkCommandBuffer cmd = m_SlotCmdBuf[slot];
    m_RtPfn.ResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

    //  6a. Pipeline barrier — layout transition only.  Use
    //      VK_QUEUE_FAMILY_IGNORED on both sides (PlVkRenderer pattern):
    //      AVVkFrame.sem timeline semaphore wait in vkQueueSubmit below
    //      provides cross-queue-family execution + memory dependency, so
    //      explicit QFOT release/acquire is unnecessary (and would hang
    //      without a matching release-side barrier on the decoder queue,
    //      which FFmpeg-vulkan does not always issue).
    VkImageMemoryBarrier acquireBar = {};
    acquireBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquireBar.srcAccessMask       = 0;
    acquireBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    acquireBar.oldLayout           = vkf->layout[0];
    acquireBar.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    acquireBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    acquireBar.image               = vkf->img[0];
    acquireBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    acquireBar.subresourceRange.baseMipLevel   = 0;
    acquireBar.subresourceRange.levelCount     = 1;
    acquireBar.subresourceRange.baseArrayLayer = 0;
    acquireBar.subresourceRange.layerCount     = 1;
    m_RtPfn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &acquireBar);
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 cmd record: barrier issued (oldLayout=%d) "
                    "imgIdx=%u fb=%p extent=%ux%u pipeline=%p layout=%p descSet=%p",
                    (int)vkf->layout[0], imgIdx,
                    (void*)m_Framebuffers[imgIdx],
                    m_SwapchainExtent.width, m_SwapchainExtent.height,
                    (void*)m_GraphicsPipeline, (void*)m_GraphicsPipelineLayout,
                    (void*)m_SlotDescSet[slot]);
    }

    //  6b. Begin render pass — clear to opaque black.
    VkClearValue clearVal = {};
    clearVal.color.float32[0] = 0.0f;
    clearVal.color.float32[1] = 0.0f;
    clearVal.color.float32[2] = 0.0f;
    clearVal.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass           = m_RenderPass;
    rpbi.framebuffer          = m_Framebuffers[imgIdx];
    rpbi.renderArea.offset    = { 0, 0 };
    rpbi.renderArea.extent    = m_SwapchainExtent;
    rpbi.clearValueCount      = 1;
    rpbi.pClearValues         = &clearVal;
    m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBeginRenderPass OK");

    //  6c. Bind pipeline + descriptor set, draw 3 vertices (fullscreen tri).
    m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBindPipeline OK");
    m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_GraphicsPipelineLayout, 0,
                                  1, &m_SlotDescSet[slot], 0, nullptr);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBindDescriptorSets OK");
    m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdDraw OK");

    // §J.3.f bug fix (2026-05-04) — HW path was missing the overlay draw
    // call that exists in renderFrameSw (lines 5513, 5533).  Without this,
    // Ctrl+Alt+Shift+S to toggle the perf overlay does nothing visible
    // when running m_SwMode=0 + FRUC + HW decode (notifyOverlayUpdated
    // stashes the surface but no consumer in HW renderFrame).
    drawOverlayInRenderPass(cmd);

    m_RtPfn.CmdEndRenderPass(cmd);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdEndRenderPass OK");

    //  6d. Transition AVVkFrame.img[0] to VK_IMAGE_LAYOUT_GENERAL so
    //      FFmpeg's decoder can re-use it as a reference frame.  We
    //      can't transition back to VIDEO_DECODE_DPB_KHR here because
    //      that layout is restricted to queues with VK_QUEUE_VIDEO_DECODE_BIT
    //      (spec).  GENERAL is universally compatible — FFmpeg's next
    //      decode does its own barrier GENERAL → DECODE_DPB on its
    //      decode queue (valid).  We update vkf->layout[0] = GENERAL
    //      below so FFmpeg knows the image's current layout when it
    //      issues that next barrier.
    VkImageMemoryBarrier releaseBar = {};
    releaseBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    releaseBar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    releaseBar.dstAccessMask       = 0;
    releaseBar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    releaseBar.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    releaseBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    releaseBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    releaseBar.image               = vkf->img[0];
    releaseBar.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    releaseBar.subresourceRange.baseMipLevel   = 0;
    releaseBar.subresourceRange.levelCount     = 1;
    releaseBar.subresourceRange.baseArrayLayer = 0;
    releaseBar.subresourceRange.layerCount     = 1;
    m_RtPfn.CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &releaseBar);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 release barrier issued (to GENERAL)");

    VkResult endVr = m_RtPfn.EndCommandBuffer(cmd);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 EndCommandBuffer returned %d", (int)endVr);

    // ---- 7. Submit ----
    // Wait on (a) swapchain acquire (color attachment write must wait), and
    // (b) AVVkFrame's timeline semaphore at the value FFmpeg signaled when
    // decode finished (fragment shader read must wait).
    VkSemaphore     waitSems[2]   = { m_SlotAcquireSem[slot][0], vkf->sem[0] };
    VkPipelineStageFlags waitMasks[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    };
    uint64_t        waitVals[2]   = { 0, vkf->sem_value[0] };

    // Signal (a) renderDoneSem (consumed by present), and (b) AVVkFrame
    // timeline at sem_value[0]+1 so FFmpeg knows we're done with the frame.
    VkSemaphore signalSems[2]     = { m_SlotRenderDoneSem[slot][0], vkf->sem[0] };
    uint64_t    signalVals[2]     = { 0, vkf->sem_value[0] + 1 };

    VkTimelineSemaphoreSubmitInfo tssi = {};
    tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tssi.waitSemaphoreValueCount   = 2;
    tssi.pWaitSemaphoreValues      = waitVals;
    tssi.signalSemaphoreValueCount = 2;
    tssi.pSignalSemaphoreValues    = signalVals;

    VkSubmitInfo si = {};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = &tssi;
    si.waitSemaphoreCount   = 2;
    si.pWaitSemaphores      = waitSems;
    si.pWaitDstStageMask    = waitMasks;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cmd;
    si.signalSemaphoreCount = 2;
    si.pSignalSemaphores    = signalSems;

    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 about to QueueSubmit (queue=%p fence=%p sem[wait]=%p+%llu sem[signal]=%p+%llu)",
                                (void*)m_GraphicsQueue, (void*)m_SlotInFlightFence[slot],
                                (void*)vkf->sem[0], (unsigned long long)waitVals[1],
                                (void*)vkf->sem[0], (unsigned long long)signalVals[1]);
    {
        std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
        vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
    }
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] vkQueueSubmit failed (%d)", (int)vr);
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
        return;
    }
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkQueueSubmit OK — first GPU work in flight");
    }

    // ---- 8. Update AVVkFrame state ----
    // §J.3.e.2.i.3.e: tell FFmpeg the image's new state so it can issue
    // a correct barrier (GENERAL → DECODE_DPB) on its decode queue when
    // it re-uses this image as a reference frame.
    vkf->access[0]     = (VkAccessFlagBits)0;            // matches dstAccess of releaseBar (some FFmpeg builds type access[] as VkAccessFlagBits not uint32_t)
    vkf->layout[0]     = VK_IMAGE_LAYOUT_GENERAL;        // matches newLayout of releaseBar
    // queue_family[0] kept as IGNORED (no QFOT was performed)
    vkf->sem_value[0] += 1;

    // ---- 9. Unlock AVVkFrame ----
    if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);

    // ---- 10. Present ----
    VkPresentInfoKHR pi = {};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_Swapchain;
    pi.pImageIndices      = &imgIdx;

    {
        std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
        vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
    }
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] vkQueuePresentKHR returned %d", (int)vr);
        // Don't bail — outer caller may handle resize; defer to i.6.
    }
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkQueuePresentKHR OK — first frame complete");
    }
}

// §J.3.e.2.i.3.e-SW — software-upload renderFrame.  Dispatched from
// renderFrame() when m_SwMode is set.  Frame is AV_PIX_FMT_NV12 in CPU
// memory: data[0]=Y plane (linesize[0] stride), data[1]=UV plane
// (linesize[1] stride).  We:
//   1. memcpy Y + UV planes into the persistent staging buffer
//   2. Acquire next swapchain image (binary acquire sem)
//   3. Wait this slot's fence, reset
//   4. Record cmd buffer:
//        a. barrier upload image → TRANSFER_DST_OPTIMAL (or UNDEFINED→DST first frame)
//        b. vkCmdCopyBufferToImage staging → image (one region per plane)
//        c. barrier upload image → SHADER_READ_ONLY_OPTIMAL
//        d. begin renderpass on framebuffer[imgIdx]
//        e. bind pipeline + descriptor (already pointing at upload image view)
//        f. cmdDraw(3,1,0,0)
//        g. end renderpass
//   5. submit (wait acquireSem, signal renderDoneSem + fence) + present
void VkFrucRenderer::renderFrameSw(AVFrame* frame)
{
    static std::atomic<uint64_t> s_FrameCountSw{0};
    uint64_t fnum = s_FrameCountSw.fetch_add(1, std::memory_order_relaxed);
    bool firstFrame = (fnum < 3);

    // §J.3.e.2.i.6 SW-PROF — per-phase profile timestamps for [VIPLE-VKFRUC-SW-PROF]
    // breakdown logging.  Recorded with steady_clock::now() at each phase boundary
    // so we can identify which step (memcpy / fence / submit / present) is the
    // largest contributor to p95/p99 frame time variance.
    auto _profT0 = std::chrono::steady_clock::now();
    auto _profT1 = _profT0;  // after memcpy
    auto _profT2 = _profT0;  // after WaitForFences
    auto _profT3 = _profT0;  // after QueueSubmit
    auto _profT4 = _profT0;  // after QueuePresent

    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-SW] frame#%llu ENTRY format=%d w=%d h=%d "
                    "data[0]=%p data[1]=%p linesize[0]=%d linesize[1]=%d",
                    (unsigned long long)fnum, (int)frame->format,
                    frame->width, frame->height,
                    (void*)frame->data[0], (void*)frame->data[1],
                    frame->linesize[0], frame->linesize[1]);
    }

    if (frame == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC-SW] null frame");
        return;
    }
    // §J.3.e.2.i.8 Phase 1.5 — gate logic:
    //   isSynthFrame   = data[0] is null (Phase 1.5 ffmpeg.cpp path)
    //   useNativeDecode = native chain has produced a sample-able m_SwUploadImage
    // Combinations:
    //   synth + native ready       → render native (Phase 1.5 default)
    //   synth + native NOT ready   → drop frame (Pacer delivers next)
    //   real ffmpeg + native ready → render native (parallel mode — prioritize native data)
    //   real ffmpeg + native NOT   → render via staging upload (Phase 0/1 SW path)
    bool isSynthFrame = (frame->data[0] == nullptr);
    bool useNativeDecodeEarly = (m_NewestDecodedSlot.load(std::memory_order_acquire) >= 0)
                              && (m_DpbSharedImage != VK_NULL_HANDLE)
                              && m_SwImageLayoutInited;

    // §J.3.e.2.i.8 Phase 1.5 attempt — earlier we tried CPU-side WaitForFences
    // on m_DecodeFence here, but Pacer-thread WaitForFences races
    // submitDecodeFrame's ResetFences → VUID-vkResetFences-pFences-01123.
    // Removed; sync to next decode is via decode submit's WaitForFences on
    // m_LastGraphicsFence (graphics queue's own per-slot fence).

    if (isSynthFrame && !useNativeDecodeEarly) {
        // Synthetic frame but native decode chain not ready yet (slot not
        // published / DPB not allocated / first decode not yet completed).
        // Drop this frame quietly — Pacer will deliver the next one.
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] synth frame dropped — native decode not ready "
                        "(newestSlot=%d dpbImg=%p layoutInited=%d)",
                        m_NewestDecodedSlot.load(),
                        (void*)m_DpbSharedImage, (int)m_SwImageLayoutInited);
        }
        return;
    }
    if (!isSynthFrame) {
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_NV12) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] unexpected pixfmt %d (want YUV420P=%d or NV12=%d)",
                        frame->format, (int)AV_PIX_FMT_YUV420P, (int)AV_PIX_FMT_NV12);
            return;
        }
        if (frame->width != m_SwImageWidth || frame->height != m_SwImageHeight) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] resolution mismatch %dx%d vs allocated %dx%d",
                        frame->width, frame->height, m_SwImageWidth, m_SwImageHeight);
            return;
        }
    }
    // Synth frames: dimensions trusted from m_SwImageWidth/Height (set at
    // createSwUploadResources time using actual stream params).

    // ---- 1a. Slot rotation + fence wait BEFORE memcpy ----
    // Async upload pipelining: writing the staging buffer for this slot
    // races with the GPU read from the SAME slot's prior submission.  Wait
    // on the per-slot in-flight fence first → guarantees prior CmdCopyBuffer-
    // ToImage retired this slot's region.  CPU memcpy of slot N then runs in
    // parallel with the GPU upload of slot N±1 from the previous frame.
    uint32_t slot = m_CurrentSlot;
    m_CurrentSlot = (m_CurrentSlot + 1) % kFrucFramesInFlight;
    // §J.3.e.2.i.8 Phase 1.5c-final — early-out if previous call detected
    // device-lost.  ONLY mode at high submission rate triggers GPU TDR
    // / driver hang after 24-57 sec on NV 596.84.  Without this gate we'd
    // cascade through hundreds of VUID errors per second.
    if (m_DeviceLost.load(std::memory_order_acquire)) {
        return;
    }
    {
        VkResult wfRes = m_RtPfn.WaitForFences(m_Device, 1, &m_SlotInFlightFence[slot], VK_TRUE, UINT64_MAX);
        if (wfRes != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] §J.3.e.2.i.8 1.5c — WaitForFences slot=%u rc=%d (device lost) — disabling native path",
                         slot, (int)wfRes);
            m_DeviceLost.store(true, std::memory_order_release);
            return;
        }
        VkResult rfRes = m_RtPfn.ResetFences(m_Device, 1, &m_SlotInFlightFence[slot]);
        if (rfRes != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] §J.3.e.2.i.8 1.5c — ResetFences slot=%u rc=%d (device lost) — disabling native path",
                         slot, (int)rfRes);
            m_DeviceLost.store(true, std::memory_order_release);
            return;
        }
    }
    _profT2 = std::chrono::steady_clock::now();

    // ---- 1b. memcpy/repack Y + UV into staging (NV12 layout) ----
    // FRUC compute path's m_FrucNv12RgbDescSet has a hard-coded {Y@0, UV@W*H}
    // binding pointing into m_SwStagingBuffer (slot 0 region).  When FRUC is
    // active we always write to slot 0 so its read sees the freshest frame —
    // forfeiting the async win for that path but preserving its current
    // semantics until the FRUC desc sets are made per-slot.  When FRUC is off
    // (the SW-only benchmark / production path), each slot writes its own
    // region and the GPU reads its own region in CmdCopyBufferToImage below.
    const int W = m_SwImageWidth;
    const int H = m_SwImageHeight;
    const VkDeviceSize stagingSlotOffset =
        m_FrucMode ? 0 : (VkDeviceSize)slot * m_SwStagingPerSlot;
    auto _profT1aY = std::chrono::steady_clock::now();
    if (!useNativeDecodeEarly) {
        uint8_t* dst = (uint8_t*)m_SwStagingMapped + stagingSlotOffset;
        // Y plane: same layout for both YUV420P and NV12, just stride-fix
        // copy.  Tested manual SSE2 non-temporal stores (Round 14) and they
        // *regressed* — modern libc memcpy is already hand-tuned with
        // prefetching + NT-store cadence + alignment handling that beats a
        // naive 64-byte _mm_stream_si128 loop.  Empirical at 4K AV1: NT-store
        // raised mem_Y mean 770→920µs and dropped AV1 throughput 76→62fps.
        // Conclusion: trust the compiler's memcpy here; revisit only if a
        // future profile shows libc memcpy NOT using NT-stores on this path.
        for (int y = 0; y < H; y++) {
            memcpy(dst + y * W, frame->data[0] + y * frame->linesize[0], W);
        }
        _profT1aY = std::chrono::steady_clock::now();
        // UV plane:
        //   NV12 input → already interleaved, plain memcpy each row (W bytes, H/2 rows)
        //   YUV420P input → 3 planes (Y, U, V); interleave U+V to get NV12 UV layout
        uint8_t* uvDst = dst + W * H;
        if (frame->format == AV_PIX_FMT_NV12) {
            if (frame->data[1] == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-SW] NV12 frame missing data[1]");
                return;
            }
            for (int y = 0; y < H / 2; y++) {
                memcpy(uvDst + y * W, frame->data[1] + y * frame->linesize[1], W);
            }
        } else {
            // YUV420P: data[1]=U plane (W/2 × H/2), data[2]=V plane (W/2 × H/2)
            if (frame->data[1] == nullptr || frame->data[2] == nullptr) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-SW] YUV420P frame missing U/V plane");
                return;
            }
            // U+V byte-by-byte interleave is the dominant memcpy cost at 4K
            // (~1.24ms vs Y plane ~760us, profiled with [VIPLE-VKFRUC-SW-PROF]
            // mem_Y / mem_UV split).  SSE2 _mm_unpacklo/hi_epi8 interleaves
            // 16 bytes of U with 16 bytes of V into 32 bytes UV in one
            // instruction pair → ~3-5× faster on the inner loop, mostly
            // memory-bandwidth limited at the 32-byte stores.  All x86
            // moonlight-qt build targets carry SSE2; arm builds (none today)
            // would need a NEON path or fall back to the scalar loop.
            const int halfW = W / 2;
            for (int y = 0; y < H / 2; y++) {
                const uint8_t* uRow = frame->data[1] + y * frame->linesize[1];
                const uint8_t* vRow = frame->data[2] + y * frame->linesize[2];
                uint8_t* dstRow = uvDst + y * W;
                int x = 0;
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
                for (; x + 16 <= halfW; x += 16) {
                    __m128i u = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uRow + x));
                    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vRow + x));
                    __m128i lo = _mm_unpacklo_epi8(u, v);
                    __m128i hi = _mm_unpackhi_epi8(u, v);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dstRow + 2 * x),       lo);
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dstRow + 2 * x + 16),  hi);
                }
#endif
                // Scalar tail (handles the final < 16 bytes per row, plus the
                // entire row when SSE2 isn't available).
                for (; x < halfW; x++) {
                    dstRow[2 * x + 0] = uRow[x];  // U
                    dstRow[2 * x + 1] = vRow[x];  // V
                }
            }
        }
    }
    // §J.3.e.2.i.8 Phase 1.5 — when useNativeDecodeEarly, the staging memcpy
    // is skipped entirely.  Decode-queue cmd buffer in submitDecodeFrame
    // already copied DPB layer → m_SwUploadImage in SHADER_READ_ONLY layout.
    _profT1 = std::chrono::steady_clock::now();
    // (Y vs UV memcpy split timing is captured below in the stats block.)
    double yMemUs  = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(_profT1aY - _profT2).count();
    double uvMemUs = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(_profT1 - _profT1aY).count();

    // Acquire 1 (real) image for single mode, or 2 (interp + real) for dual.
    uint32_t imgIdxA = 0;  // interp slot (only used in dual mode)
    uint32_t imgIdxB = 0;  // real frame slot (always)
    if (m_DualMode) {
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdxA);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: acquire interp imgA failed (%d)", (int)vrA);
            return;
        }
        VkResult vrB = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][1],
                                                    VK_NULL_HANDLE, &imgIdxB);
        if (vrB != VK_SUCCESS && vrB != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: acquire real imgB failed (%d)", (int)vrB);
            return;
        }
    } else {
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdxB);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] vkAcquireNextImageKHR failed (%d)", (int)vrA);
            return;
        }
    }
    uint32_t imgIdx = imgIdxB;  // legacy alias for the existing single-render code below

    // ---- 4. Record cmd buffer ----
    VkCommandBuffer cmd = m_SlotCmdBuf[slot];
    m_RtPfn.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    m_RtPfn.BeginCommandBuffer(cmd, &cbbi);

    // §J.3.e.2.i.8 Phase 1.4 — when native decode produced a fresh slot,
    // the decode-queue cmd buffer (in submitDecodeFrame) already copied DPB→
    // m_SwUploadImage and left it in SHADER_READ_ONLY layout.  Skip the
    // staging upload + layout transitions entirely; just sample.
    int nativeSlot = m_NewestDecodedSlot.load(std::memory_order_acquire);
    // Reuse useNativeDecodeEarly's semantics — keeps the early-return path
    // (synth frame dropped if !ready) consistent with the late barrier gating.
    bool useNativeDecode = useNativeDecodeEarly;
    {
        static std::atomic<uint64_t> s_renderCount{0};
        uint64_t r = s_renderCount.fetch_add(1) + 1;
        if (r == 1 || (r % 120) == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] §J.3.e.2.i.8 renderFrameSw#%llu — nativeSlot=%d "
                        "useNative=%d", (unsigned long long)r, nativeSlot, (int)useNativeDecode);
        }
    }

    if (!useNativeDecode) {
        // 4a. Barrier upload image → TRANSFER_DST_OPTIMAL (oldLayout depends
        // on whether this is the first frame).
        VkImageLayout oldImgLayout = m_SwImageLayoutInited
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageMemoryBarrier toDstBar = {};
        toDstBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDstBar.srcAccessMask       = m_SwImageLayoutInited ? VK_ACCESS_SHADER_READ_BIT : (VkAccessFlags)0;
        toDstBar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDstBar.oldLayout           = oldImgLayout;
        toDstBar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDstBar.image               = m_SwUploadImage;
        toDstBar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDstBar.subresourceRange.levelCount = 1;
        toDstBar.subresourceRange.layerCount = 1;
        m_RtPfn.CmdPipelineBarrier(cmd,
            m_SwImageLayoutInited ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toDstBar);

        // 4b. Staging buffer → image, two regions per plane.
        // bufferOffset starts at this slot's region within the unified staging
        // buffer (0 when m_FrucMode is on — see memcpy comment above).
        VkBufferImageCopy regions[2] = {};
        regions[0].bufferOffset      = stagingSlotOffset + 0;
        regions[0].bufferRowLength   = (uint32_t)W;
        regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        regions[0].imageSubresource.layerCount = 1;
        regions[0].imageExtent       = { (uint32_t)W, (uint32_t)H, 1 };
        regions[1].bufferOffset      = stagingSlotOffset + (VkDeviceSize)W * H;
        regions[1].bufferRowLength   = (uint32_t)W / 2;
        regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        regions[1].imageSubresource.layerCount = 1;
        regions[1].imageExtent       = { (uint32_t)W / 2, (uint32_t)H / 2, 1 };
        m_RtPfn.CmdCopyBufferToImage(cmd, m_SwStagingBuffer, m_SwUploadImage,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      2, regions);

        // 4c. Barrier upload image → SHADER_READ_ONLY_OPTIMAL.
        VkImageMemoryBarrier toShaderBar = toDstBar;
        toShaderBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShaderBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShaderBar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShaderBar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShaderBar);
    }
    // §J.3.e.2.i.8 Phase 1.4 — when useNativeDecode, decode-queue cmd buffer
    // already wrote into m_SwUploadImage and left it in SHADER_READ_ONLY,
    // so this whole 4a-4c upload block is skipped.

    // §J.3.e.2.i.8 Phase 2.5 — when native decode is active AND FRUC is on,
    // mirror m_SwUploadImage → m_SwFrucNv12Buf (gpu-only) so FRUC's NV12→RGB
    // compute reads exactly the same content the real-frame fragment shader
    // samples.  Without this, FRUC reads m_SwStagingBuffer (FFmpeg's parallel
    // SW decode output) and the real frame samples m_SwUploadImage (native
    // decode output) → source asymmetry → 3-4 Hz blur/sharp flicker.
    //
    // Layout flow:
    //   m_SwUploadImage: SHADER_READ_ONLY (decode QF wrote, our timeline-sem
    //   wait at TRANSFER stage gates this transition) → TRANSFER_SRC →
    //   copy → SHADER_READ_ONLY (so the later fragment-shader passes still
    //   see it sampled).
    //   m_SwFrucNv12Buf: TRANSFER_WRITE → SHADER_READ (compute), barrier
    //   inserted before runFrucComputeChain dispatches Stage 0.
    //
    // Skip on the very first frame (m_SwImageLayoutInited=false) — there's
    // nothing valid to copy from yet, FRUC just sees stale prevRGB once.
    if (useNativeDecode && m_FrucMode && m_FrucReady && m_SwImageLayoutInited) {
        // m_SwUploadImage SHADER_READ_ONLY → TRANSFER_SRC_OPTIMAL
        VkImageMemoryBarrier toSrcBar = {};
        toSrcBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrcBar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        toSrcBar.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        toSrcBar.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSrcBar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrcBar.image               = m_SwUploadImage;
        toSrcBar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrcBar.subresourceRange.levelCount = 1;
        toSrcBar.subresourceRange.layerCount = 1;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toSrcBar);

        // Image → buffer copy.  Two regions for NV12 multi-plane image:
        //   region 0: PLANE_0 (Y) → buffer offset 0,        size W×H
        //   region 1: PLANE_1 (UV) → buffer offset W×H,     size (W/2)×(H/2)×2
        // Layout in m_SwFrucNv12Buf matches m_SwStagingBuffer (Y at 0,
        // UV-interleaved at W×H), so the existing NV12→RGB shader reads either
        // buffer identically.
        VkBufferImageCopy regs[2] = {};
        const int W = m_SwImageWidth;
        const int H = m_SwImageHeight;
        regs[0].bufferOffset      = 0;
        regs[0].bufferRowLength   = (uint32_t)W;
        regs[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        regs[0].imageSubresource.layerCount = 1;
        regs[0].imageExtent       = { (uint32_t)W, (uint32_t)H, 1 };
        regs[1].bufferOffset      = (VkDeviceSize)W * H;
        regs[1].bufferRowLength   = (uint32_t)W / 2;
        regs[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        regs[1].imageSubresource.layerCount = 1;
        regs[1].imageExtent       = { (uint32_t)W / 2, (uint32_t)H / 2, 1 };
        m_RtPfn.CmdCopyImageToBuffer(cmd, m_SwUploadImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            m_SwFrucNv12Buf, 2, regs);

        // m_SwUploadImage TRANSFER_SRC → SHADER_READ_ONLY (so later fragment
        // shader passes can sample it for the real-frame render pass).
        VkImageMemoryBarrier toShaderBar = toSrcBar;
        toShaderBar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toShaderBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShaderBar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toShaderBar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toShaderBar);

        // m_SwFrucNv12Buf TRANSFER_WRITE → COMPUTE_SHADER_READ.
        VkBufferMemoryBarrier bufBar = {};
        bufBar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bufBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bufBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bufBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBar.buffer = m_SwFrucNv12Buf;
        bufBar.offset = 0;
        bufBar.size   = VK_WHOLE_SIZE;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bufBar, 0, nullptr);
    }

    // §J.3.e.2.i overlay：drain stash → memcpy 到 staging（可能 alloc 新
    // image 若尺寸變了），然後 uploadPendingOverlay 在 cmd buffer 內把
    // staging copy 進 image + barrier 到 SHADER_READ_ONLY_OPTIMAL.  必須
    // 在 BeginRenderPass 之前做（cmdCopyBufferToImage 不能在 render pass
    // 裡）.
    drainOverlayStash();
    uploadPendingOverlay(cmd);

    // §J.3.e.2.i.4 — FRUC compute chain (ME → median → warp).  Records
    // dispatches into the same cmd buffer as our graphics rendering; the
    // GPU executes them in order.  Outputs to m_FrucInterpRgbBuf which is
    // not yet displayed (i.4.2 will add dual-present); for now we just
    // verify the chain runs without crash.
    if (m_FrucMode && m_FrucReady) {
        // §J.3.e.2.i.8 Phase 2.5 — useNativeDecode gates FRUC's NV12 source.
        runFrucComputeChain(cmd, (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight,
                            useNativeDecode);
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu FRUC compute chain dispatched (%ux%u "
                        "block=8 mv=%ux%u)",
                        (unsigned long long)fnum,
                        (unsigned)m_SwImageWidth, (unsigned)m_SwImageHeight,
                        (unsigned)m_FrucMvWidth, (unsigned)m_FrucMvHeight);
        }
    }

    VkClearValue clearVal = {};
    clearVal.color.float32[3] = 1.0f;

    // §J.3.e.2.i.4.2 dual-present: first render pass writes interp via
    // m_InterpPipeline (samples bufInterpRGB) into framebuffer[imgIdxA].
    if (m_DualMode) {
        // Need pfnCmdPushConstants for interp shader; load via m_RtPfn isn't
        // there for push-const, so resolve here.
        auto getDevPa2 = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdPushConst = (PFN_vkCmdPushConstants)getDevPa2(m_Device, "vkCmdPushConstants");

        VkRenderPassBeginInfo rpbiA = {};
        rpbiA.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbiA.renderPass           = m_RenderPass;
        rpbiA.framebuffer          = m_Framebuffers[imgIdxA];
        rpbiA.renderArea.extent    = m_SwapchainExtent;
        rpbiA.clearValueCount      = 1;
        rpbiA.pClearValues         = &clearVal;
        m_RtPfn.CmdBeginRenderPass(cmd, &rpbiA, VK_SUBPASS_CONTENTS_INLINE);
        m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_InterpPipelineLayout, 0,
                                      1, &m_InterpDescSet, 0, nullptr);
        struct { int srcW, srcH, _pad0, _pad1; } pcInterp = {
            (int)m_SwImageWidth, (int)m_SwImageHeight, 0, 0
        };
        if (pfnCmdPushConst) {
            pfnCmdPushConst(cmd, m_InterpPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(pcInterp), &pcInterp);
        }
        m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
        // overlay 疊在 interp pass 上（dual mode 下兩 pass 都疊，跟 D3D11VA
        // dual-FRUC 一樣 d3d11va.cpp:1107、1139）。
        drawOverlayInRenderPass(cmd);
        m_RtPfn.CmdEndRenderPass(cmd);
    }

    // Real-frame render pass (always runs) — samples m_SwUploadImage NV12
    // via ycbcr conversion sampler.  In dual mode goes to imgIdxB; in
    // single mode goes to imgIdx (== imgIdxB).
    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass           = m_RenderPass;
    rpbi.framebuffer          = m_Framebuffers[imgIdx];
    rpbi.renderArea.extent    = m_SwapchainExtent;
    rpbi.clearValueCount      = 1;
    rpbi.pClearValues         = &clearVal;
    m_RtPfn.CmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
    m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  m_GraphicsPipelineLayout, 0,
                                  1, &m_SlotDescSet[slot], 0, nullptr);
    m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
    drawOverlayInRenderPass(cmd);  // overlay 疊在 real frame pass 上.
    m_RtPfn.CmdEndRenderPass(cmd);
    m_RtPfn.EndCommandBuffer(cmd);
    m_SwImageLayoutInited = true;  // image is now in SHADER_READ_ONLY for next frame's barrier

    // ---- 5. Submit + present ----
    // §J.3.e.2.i.8 Phase 1.5b — when sampling native-decoded m_SwUploadImage,
    // wait on decode's timeline value before fragment-shader sample.  Skip
    // the timeline wait if no decode has signaled yet (timelineWaitValue == 0).
    //
    // §J.3.e.2.i.8 Phase 2.5 — wait stage moved to TRANSFER_BIT when both
    // native decode + FRUC are active, because we now also do an image→buffer
    // copy (m_SwUploadImage → m_SwFrucNv12Buf) at TRANSFER stage before the
    // FRUC compute reads m_SwFrucNv12Buf.  TRANSFER fires earlier in the cmd
    // buf than fragment-shader sampling, so a wait at TRANSFER blocks the
    // copy until decode has finished writing the image — and the in-cmd-buf
    // image layout barriers chain that dependency forward to the later
    // fragment-shader sample, so the real-frame render still sees up-to-date
    // pixels.  Without FRUC, no transfer needs to wait → keep FRAGMENT_SHADER.
    uint64_t timelineWaitValue = useNativeDecode ? m_LastDecodeValue.load(std::memory_order_acquire) : 0;
    const VkPipelineStageFlags timelineWaitStage =
        (useNativeDecode && m_FrucMode && m_FrucReady)
            ? VK_PIPELINE_STAGE_TRANSFER_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkResult vr;
    // §J.3.e.2.i.8 Phase 1.5c — always allocate gfx timeline signal value (so
    // decode side can always vkWaitSemaphores on the latest published value).
    uint64_t gfxSignalVal = m_GfxTimelineNext.fetch_add(1, std::memory_order_acq_rel);
    if (m_DualMode) {
        // Dual: wait both acquire sems (pass 0 = interp, pass 1 = real),
        // signal both per-IMAGE renderDone sems + m_GfxTimelineSem (gfx→decode sync).
        // §J.3.e.2.i.8 Phase 1.5c-final — renderDone sems indexed by swapchain
        // image idx (not slot), so reuse is gated by Vulkan's swapchain image
        // re-acquire rule (image idx X re-acquired only after present consumed
        // its sem) → no VUID-vkQueueSubmit-pSignalSemaphores-00067 race.
        VkSemaphore waitSems[3]   = { m_SlotAcquireSem[slot][0], m_SlotAcquireSem[slot][1], m_TimelineSem };
        VkSemaphore signalSems[3] = { m_SwapchainRenderDoneSem[imgIdxA], m_SwapchainRenderDoneSem[imgIdxB], m_GfxTimelineSem };
        VkPipelineStageFlags waitMasks[3] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            timelineWaitStage,
        };
        // Timeline values: 0 ignored for binary sems; real value for timeline.
        uint64_t waitVals[3]   = { 0, 0, timelineWaitValue };
        uint64_t signalVals[3] = { 0, 0, gfxSignalVal };
        VkTimelineSemaphoreSubmitInfo tssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        tssi.waitSemaphoreValueCount   = 3;
        tssi.pWaitSemaphoreValues      = waitVals;
        tssi.signalSemaphoreValueCount = 3;
        tssi.pSignalSemaphoreValues    = signalVals;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = &tssi;  // always attach now (gfx sem always signals)
        si.waitSemaphoreCount   = (timelineWaitValue > 0) ? 3 : 2;
        si.pWaitSemaphores      = waitSems;
        si.pWaitDstStageMask    = waitMasks;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 3;
        si.pSignalSemaphores    = signalSems;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] dual: vkQueueSubmit failed (%d) — disabling native path", (int)vr);
            if (vr == VK_ERROR_DEVICE_LOST) m_DeviceLost.store(true, std::memory_order_release);
            return;
        }
        // §J.3.e.2.i.8 Phase 1.5c — publish gfx timeline value so decode-thread
        // submitDecodeFrame can vkWaitSemaphores on it before recording its next
        // m_SwUploadImage write.  Replaces racey m_LastGraphicsFence pattern.
        m_LastGraphicsValue.store(gfxSignalVal, std::memory_order_release);

        // §J.3.e.2.i.8 Phase 1.5c-final — present per-image renderDone sems.
        VkPresentInfoKHR piA = {};
        piA.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piA.waitSemaphoreCount = 1;
        piA.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxA];
        piA.swapchainCount     = 1;
        piA.pSwapchains        = &m_Swapchain;
        piA.pImageIndices      = &imgIdxA;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piA);
        }
        VkPresentInfoKHR piB = {};
        piB.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piB.waitSemaphoreCount = 1;
        piB.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxB];
        piB.swapchainCount     = 1;
        piB.pSwapchains        = &m_Swapchain;
        piB.pImageIndices      = &imgIdxB;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            VkResult vrB = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piB);
            if (vrB != VK_SUCCESS && vrB != VK_SUBOPTIMAL_KHR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-SW] dual: present(real) returned %d", (int)vrB);
            }
        }
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu DUAL OK — interp imgA=%u + real imgB=%u "
                        "presented", (unsigned long long)fnum, imgIdxA, imgIdxB);
        }
    } else {
        // Single-present (legacy SW path, m_DualMode off)
        // §J.3.e.2.i.8 Phase 1.5c-final — renderDone sem per-image (imgIdx).
        VkSemaphore singleWaitSems[2] = { m_SlotAcquireSem[slot][0], m_TimelineSem };
        VkSemaphore singleSignalSems[2] = { m_SwapchainRenderDoneSem[imgIdx], m_GfxTimelineSem };
        VkPipelineStageFlags singleWaitMasks[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            timelineWaitStage,  // §J.3.e.2.i.8 Phase 2.5 — see comment above.
        };
        uint64_t singleWaitVals[2]   = { 0, timelineWaitValue };
        uint64_t singleSignalVals[2] = { 0, gfxSignalVal };
        VkTimelineSemaphoreSubmitInfo singleTssi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        singleTssi.waitSemaphoreValueCount   = 2;
        singleTssi.pWaitSemaphoreValues      = singleWaitVals;
        singleTssi.signalSemaphoreValueCount = 2;
        singleTssi.pSignalSemaphoreValues    = singleSignalVals;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = &singleTssi;  // always attach (gfx sem always signals)
        si.waitSemaphoreCount   = (timelineWaitValue > 0) ? 2 : 1;
        si.pWaitSemaphores      = singleWaitSems;
        si.pWaitDstStageMask    = singleWaitMasks;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 2;
        si.pSignalSemaphores    = singleSignalSems;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        _profT3 = std::chrono::steady_clock::now();
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-SW] vkQueueSubmit failed (%d) — disabling native path", (int)vr);
            if (vr == VK_ERROR_DEVICE_LOST) m_DeviceLost.store(true, std::memory_order_release);
            return;
        }
        // §J.3.e.2.i.8 Phase 1.5c — see dual-mode comment above.
        m_LastGraphicsValue.store(gfxSignalVal, std::memory_order_release);

        // §J.3.e.2.i.8 Phase 1.5c-final — present per-image renderDone sem.
        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdx];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_Swapchain;
        pi.pImageIndices      = &imgIdx;
        {
            std::lock_guard<std::mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
        }
        _profT4 = std::chrono::steady_clock::now();
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] vkQueuePresentKHR returned %d", (int)vr);
        }
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-SW] frame#%llu OK — upload+render+present complete",
                        (unsigned long long)fnum);
        }
    }

    // §J.3.e.2.i.6 — periodic [VIPLE-VKFRUC-Stats] benchmark logging.
    // Mirrors D3D11+GenericFRUC's [VIPLE-PRESENT-Stats] format so we can
    // compare apples-to-apples.  Tracks frame-to-frame intervals (= present
    // pacing).  In dual mode, reports both real and "effective" (real +
    // interp counted as separate display events).  Emit every ~5 sec.
    {
        using namespace std::chrono;
        static thread_local steady_clock::time_point s_LastPresent{};
        static thread_local steady_clock::time_point s_StatsBucketStart{};
        static thread_local std::vector<double> s_FrameMsRing;  // intervals in current 5s bucket
        // §J.3.e.2.i.6 SW-PROF — per-phase microsecond samples in current bucket
        static thread_local std::vector<double> s_ProfMemUs;
        static thread_local std::vector<double> s_ProfMemY_Us;
        static thread_local std::vector<double> s_ProfMemUV_Us;
        static thread_local std::vector<double> s_ProfWaitUs;
        static thread_local std::vector<double> s_ProfSubmitUs;
        static thread_local std::vector<double> s_ProfPresentUs;
        static thread_local std::vector<double> s_ProfTotalUs;
        // record this frame's phase deltas.  With the async-pipelining reorder
        // the timestamp sequence is T0 → T2 (after WaitForFences) → T1aY
        // (after Y-plane memcpy) → T1 (after UV-plane memcpy) → T3 (after
        // QueueSubmit) → T4 (after QueuePresent).
        if (_profT4 > _profT0) {
            s_ProfWaitUs.push_back(duration_cast<duration<double, std::micro>>(_profT2 - _profT0).count());
            s_ProfMemUs.push_back(duration_cast<duration<double, std::micro>>(_profT1 - _profT2).count());
            s_ProfMemY_Us.push_back(yMemUs);
            s_ProfMemUV_Us.push_back(uvMemUs);
            s_ProfSubmitUs.push_back(duration_cast<duration<double, std::micro>>(_profT3 - _profT1).count());
            s_ProfPresentUs.push_back(duration_cast<duration<double, std::micro>>(_profT4 - _profT3).count());
            s_ProfTotalUs.push_back(duration_cast<duration<double, std::micro>>(_profT4 - _profT0).count());
        }
        static thread_local uint64_t s_CumulReal   = 0;
        static thread_local uint64_t s_CumulInterp = 0;

        auto now = steady_clock::now();
        if (s_LastPresent.time_since_epoch().count() != 0) {
            double dtMs = duration_cast<duration<double, std::milli>>(now - s_LastPresent).count();
            s_FrameMsRing.push_back(dtMs);
        } else {
            s_StatsBucketStart = now;
        }
        s_LastPresent = now;
        s_CumulReal++;
        if (m_DualMode) s_CumulInterp++;

        // Emit every ~5 seconds of wall time in the current bucket.
        double bucketSec = duration_cast<duration<double>>(now - s_StatsBucketStart).count();
        if (bucketSec >= 5.0 && !s_FrameMsRing.empty()) {
            // Compute percentiles by sorted copy (small N=~150 at 30fps).
            std::vector<double> sorted = s_FrameMsRing;
            std::sort(sorted.begin(), sorted.end());
            auto pct = [&](double q) -> double {
                size_t idx = (size_t)((sorted.size() - 1) * q + 0.5);
                if (idx >= sorted.size()) idx = sorted.size() - 1;
                return sorted[idx];
            };
            double sum = 0;
            for (double v : sorted) sum += v;
            double mean = sum / sorted.size();
            double fps = sorted.size() / bucketSec;

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] %s n=%zu fps=%.2f ft_mean=%.2fms "
                        "p50=%.2f p95=%.2f p99=%.2f p99.9=%.2f (window %.1fs)",
                        m_DualMode ? "dual-present" : "single-present",
                        sorted.size(), fps, mean,
                        pct(0.50), pct(0.95), pct(0.99), pct(0.999),
                        bucketSec);
            // §J.3.e.2.i.6 — GPU compute chain timing (NV12->RGB + ME +
            // Median + Warp), averaged over the window.
            double gpuMeanUs = (m_FrucGpuUsCount > 0)
                                ? (m_FrucGpuUsAccum / m_FrucGpuUsCount) : 0.0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] cumul real=%llu interp=%llu "
                        "compute_gpu_mean=%.3fms (n=%d) "
                        "(swMode=%d frucMode=%d dualMode=%d)",
                        (unsigned long long)s_CumulReal,
                        (unsigned long long)s_CumulInterp,
                        gpuMeanUs / 1000.0,
                        m_FrucGpuUsCount,
                        m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0);
            m_FrucGpuUsAccum = 0.0;
            m_FrucGpuUsCount = 0;

            // §J.3.e.2.i.6 SW-PROF — emit per-phase breakdown when we have
            // single-mode samples (synth / dual frames don't measure all 4
            // phases, so the arrays may be smaller than s_FrameMsRing).
            auto phasePctLog = [&](const char* name, std::vector<double>& vec) {
                if (vec.empty()) return;
                std::sort(vec.begin(), vec.end());
                auto pp = [&](double q) -> double {
                    size_t idx = (size_t)((vec.size() - 1) * q + 0.5);
                    if (idx >= vec.size()) idx = vec.size() - 1;
                    return vec[idx];
                };
                double sum = 0; for (double v : vec) sum += v;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-SW-PROF] %-8s n=%zu mean=%.0fus p50=%.0f p95=%.0f p99=%.0f max=%.0f",
                            name, vec.size(), sum / vec.size(),
                            pp(0.50), pp(0.95), pp(0.99), vec.back());
                vec.clear();
            };
            phasePctLog("memcpy",   s_ProfMemUs);
            phasePctLog(" mem_Y",   s_ProfMemY_Us);
            phasePctLog(" mem_UV",  s_ProfMemUV_Us);
            phasePctLog("fence",    s_ProfWaitUs);
            phasePctLog("submit",   s_ProfSubmitUs);
            phasePctLog("present",  s_ProfPresentUs);
            phasePctLog("totalfn",  s_ProfTotalUs);

            s_FrameMsRing.clear();
            s_StatsBucketStart = now;
        }
    }
}

int VkFrucRenderer::getDecoderCapabilities()  { return 0; }
int VkFrucRenderer::getRendererAttributes()    { return 0; }

#endif // HAVE_LIBPLACEBO_VULKAN
