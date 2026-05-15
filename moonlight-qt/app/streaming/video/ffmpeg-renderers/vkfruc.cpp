// VipleStream §J.3.e.2.i — VkFrucRenderer
// See vkfruc.h header + docs/J.3.e.2.i_vulkan_native_renderer.md.

#include "vkfruc.h"
#include "vkfruc-aftermath.h"
#include "path.h"  // §J.3.e.X Path β — Path::getDataFilePath / getCacheFileInfo for RIFE model + pipeline cache
#include "settings/streamingpreferences.h"

// VipleStream §K.1: strncpy_s + _TRUNCATE are MSVC bounds-checked CRT.
// snprintf is cross-platform, NUL-terminates, won't overflow dst buffer.
#ifndef _WIN32
#  include <cstdio>
#  define strncpy_s(dst, dstsz, src, _) snprintf((dst), (dstsz), "%s", (src))
#  define _TRUNCATE 0
#endif

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

// §B-NVOF Phase 3 — NVIDIA Optical Flow SDK 5.0.7 Vulkan interface.
// Header lives in libs/windows/nvofa/include/. Loaded from system
// nvofapi64.dll at runtime via LoadLibrary; no static link required.
#include "nvOpticalFlowVulkan.h"

// §B-DUMP 2026-05-07 — stb_image_write for PNG output (diagnostic frame
// dump path). ncnn ships v1.15 in ncnn/build/native/include/.
// IMPLEMENTATION defined here to inline the impl into vkfruc.cpp's TU.
// ncnn.dll may also include the header internally but its symbols are
// not exported, so no duplicate-symbol collision.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "ncnn/stb_image_write.h"

// §B-DUMP — mkdir for dump directory tree.  Windows uses _mkdir from
// <direct.h>; POSIX uses mkdir from <sys/stat.h>.
#ifdef Q_OS_WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

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
//
// §J.3.e.2.i.16 (v1.4.81) — Changed from std::mutex to std::recursive_mutex:
// FFmpeg 7.x bypasses lock_queue callback by using vkQueueSubmit2 (the
// callback was designed for vkQueueSubmit v1.0 and is now marked deprecated
// — see libavutil/hwcontext_vulkan.h line 178 "Deprecated: use
// VK_KHR_internally_synchronized_queues").  We inject a custom get_proc_addr
// that returns wrapping versions of vkQueueSubmit / 2 / 2KHR.  Those wrappers
// acquire this lock — but our own code ALSO acquires it before calling
// m_RtPfn.QueueSubmit, so a recursive_mutex is needed to avoid deadlock when
// our path goes through the wrapped function pointer.
static std::recursive_mutex s_VkFrucQueueLock;

// §J.3.e.2.i.10 Phase 2B step 5-6 (v1.4.56) — separate lock for the compute
// queue submit path.  v1.4.56 ships all 3 chain submits on the graphics queue
// (cmpCmd through this lock would still serialise behind s_VkFrucQueueLock),
// so this lock is effectively dormant; v1.4.57 will switch the cmpCmd submit
// to m_ComputeQueue and hold this lock instead — keeping CPU thread off
// s_VkFrucQueueLock during compute submit so the two queues don't serialise.
static std::mutex s_VkFrucComputeLock;

// §B-NVOF / §B2 UI 整合 2026-05-07 — env var 跟 settings 的 OR 合併查詢.
// env var 優先 (dev escape hatch / regression bisect)，沒設 env var 才看
// StreamingPreferences (使用者在 Settings UI 勾的). RS_D3D11 path 完全
// ignore — 兩個 helper 只在 VkFrucRenderer 內部呼叫.
static bool vkfrucWantNvOfFromUserOrEnv()
{
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_NV_OF") != 0) return true;
    auto* prefs = StreamingPreferences::get();
    return prefs && prefs->vkfrucEnableNvOf;
}
static bool vkfrucWantTripleFromUserOrEnv()
{
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_TRIPLE") != 0) return true;
    auto* prefs = StreamingPreferences::get();
    return prefs && prefs->vkfrucEnableTriple;
}
// §J.3.e.X Path β — env var > auto-tier (if enabled) > manual prefs > default false.
//
// §J.3.e.2.i.11 (v1.4.69) — auto-tier 啟用後 (default true)，根據
// vkfrucDetectedTier 自動決定 RIFE on/off：
//   ENTRY        → RIFE OFF (no FRUC, native present)
//   PERFORMANCE  → RIFE OFF (block-match path only, chain ~4ms)
//   BALANCED     → RIFE ON  (β.5.1 path, inferDim=128, chain ~10ms)
//   QUALITY      → RIFE ON  (β.5.1 path, inferDim=256, chain ~14ms)
// VIPLE_VKFRUC_NATIVE_RIFE=1 env 仍當 escape hatch 強制開啟 (覆寫 auto-tier).
static bool vkfrucWantNativeRifeFromUserOrEnv()
{
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_NATIVE_RIFE") != 0) return true;
    auto* prefs = StreamingPreferences::get();
    if (!prefs) return false;
    if (prefs->vkfrucRifeAutoTier) {
        // Auto-tier 主控：BALANCED / QUALITY → RIFE ON；其他 → OFF
        return prefs->vkfrucDetectedTier == StreamingPreferences::VGT_BALANCED
            || prefs->vkfrucDetectedTier == StreamingPreferences::VGT_QUALITY;
    }
    return prefs->vkfrucEnableNativeRife;
}
static int vkfrucNativeRifeInferDimFromUserOrEnv()
{
    int env = qEnvironmentVariableIntValue("VIPLE_VKFRUC_RIFE_INFER_DIM");
    if (env > 0) return env;
    auto* prefs = StreamingPreferences::get();
    if (!prefs) return 128;
    if (prefs->vkfrucRifeAutoTier) {
        // Auto-tier 主控：BALANCED=128 (chain ~10ms fit slot), QUALITY=256
        // (chain ~14ms 略邊緣). ENTRY/PERFORMANCE 仍給 128 即便 RIFE OFF
        // (僅在 m_RifeNativeMode=true 時才會被讀).
        switch (prefs->vkfrucDetectedTier) {
            case StreamingPreferences::VGT_QUALITY: return 256;
            case StreamingPreferences::VGT_BALANCED: return 128;
            default:                                 return 128;
        }
    }
    if (prefs->vkfrucNativeRifeInferDim > 0) return prefs->vkfrucNativeRifeInferDim;
    return 256;  // legacy default for manual mode
}

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
    // m_FrucMode / m_DualMode 自動開（受 enableFrameInterpolation 控制
    // —— 沒勾補幀就只跑 single present）.  Env-var path 留給 dev/CI
    // 測試獨立子集.
    //
    // §J.3.f integration (2026-05-04) — m_SwMode 不再因 RS_VULKAN 自動
    // 開.  HW path 經 §J.3.f rebuild FFmpeg 8.1 + extra_hw_frames=1 +
    // overlay round-2 fix 後在 4K120 + FRUC + DUAL × 3 codec 全跑得乾淨
    // (H.264 92 / HEVC 100-104 / AV1 120fps，full-power GPU)，預設 HW
    // 比預設 SW 快 ~10ms decode + 省 CPU upload 工作.  保留
    // VIPLE_VKFRUC_SW=1 作 debug override (e.g. 比較解碼品質 / HW
    // driver broken 時 fallback).
    auto* prefs = StreamingPreferences::get(nullptr);
    bool prefsWantVulkan = prefs && prefs->rendererSelection == StreamingPreferences::RS_VULKAN;
    bool prefsWantInterp = prefs && prefs->enableFrameInterpolation;
    // §B-DUMP — VIPLE_VKFRUC_DUMP_DIR implicitly enables FRUC + DUAL so the
    // dump path actually has interp frames to capture.  Without this, dump
    // wouldn't activate when user only sets DUMP_DIR (their RS_D3D11 default
    // disables FRUC, so m_FrucMode=false → createFrucComputeResources never
    // called → initFrameDump never called → no captures).
    bool envDumpDir = !qgetenv("VIPLE_VKFRUC_DUMP_DIR").isEmpty();
    m_SwMode   = qEnvironmentVariableIntValue("VIPLE_VKFRUC_SW")   != 0 || envDumpDir;
    m_FrucMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_FRUC") != 0 || (prefsWantVulkan && prefsWantInterp) || envDumpDir;
    m_DualMode = qEnvironmentVariableIntValue("VIPLE_VKFRUC_DUAL") != 0 || (prefsWantVulkan && prefsWantInterp) || envDumpDir;
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
    // hwcontext.  §J.3.f integration (2026-05-04) HW 變預設後此 env var
    // 變 redundant (m_SwMode 預設已是 false)，但保留以防 user 設了
    // VIPLE_VKFRUC_SW=1 又想用 VIPLE_VKFRUC_HW=1 強制蓋過.  HW 優先級高.
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_HW") != 0) {
        m_SwMode = false;
    }
    // §B2 2026-05-06 — TRIPLE 60→180 mode opt-in via env var.  Strictly
    // upgrades dual-present to triple-present; only meaningful when both
    // FRUC + DualMode are already on.
    m_TripleMode = m_DualMode && m_FrucMode && vkfrucWantTripleFromUserOrEnv();

    // §J.3.e.X Path β — native RIFE Vulkan integration.  Reads env var
    // OR prefs (env wins).  Default OFF (opt-in beta — known 30-60s
    // device-lost crash on RTX 3060 + NV 596.144 driver, root cause needs
    // Nsight Graphics analysis).
    m_RifeNativeMode = vkfrucWantNativeRifeFromUserOrEnv();
    // §J.3.e.X Path β.5 — flow-extraction + native-res warp.  Default ON when
    // β is on; user can flip to '0' to fall back to β.4 bilinear-up-RGB.
    {
        QByteArray b5 = qgetenv("VIPLE_VKFRUC_RIFE_BETA5");
        m_Beta5Enabled = b5.isEmpty() ? true : (b5.toInt() != 0);
    }
    if (m_RifeNativeMode && !m_FrucMode) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] VIPLE_VKFRUC_NATIVE_RIFE=1 requires FRUC; ignoring");
        m_RifeNativeMode = false;
    }
    // §J.3.e.2.i.10c (2026-05-09) Phase β.9 — TRIPLE + Native RIFE 互斥鎖
    // 解除.  原 β.1/β.2 限制是因為當時 chain 14ms × 2 = 28ms > 60fps slot
    // 16.7ms 不 fit；β.5.2 + 4Y.7 + bicubic 後 chain @ 128 dim ≈ 10ms × 2 =
    // 20ms 仍超 16.7ms 但接近，搭 user UI fps=120 (server 60) + small dim
    // 可勉強進 budget.  使用者承擔效能風險，不再 force off.
    if (m_RifeNativeMode && m_TripleMode) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β9] TRIPLE + Native RIFE coexisting — chain "
            "runs RIFE inference twice per server frame (t=1/3 + t=2/3).  "
            "Recommended: inferDim=128 + UI fps=120 (server 60).");
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=%d, swMode=%d, frucMode=%d, dualMode=%d, tripleMode=%d, rifeNative=%d, prefs=%s)",
                pass, m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0,
                m_TripleMode ? 1 : 0, m_RifeNativeMode ? 1 : 0,
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

// §J.3.e.2.i.16 (v1.4.81) — vkQueueSubmit2 lock injection.
//
// FFmpeg 7.x's hwcontext_vulkan uses vkQueueSubmit2 internally for Vulkan
// Video decode submission, but the deprecated lock_queue callback (above)
// only gets called around vkQueueSubmit v1.0 paths.  Result: FFmpeg's
// decode thread submits to the same VkQueue as our render thread without
// sync, triggering "[VVL] UNASSIGNED-Threading-MultipleThreads-Write"
// violations.  Race manifests under heavier compute pressure as fps→1.
//
// Fix: pass a custom get_proc_addr to FFmpeg via AVVulkanDeviceContext
// that returns OUR wrapping versions of vkQueueSubmit / 2 / 2KHR which
// acquire s_VkFrucQueueLock before dispatching to the real driver.
// s_VkFrucQueueLock is std::recursive_mutex to handle the case where our
// own code (which already locks manually) ends up calling a wrapped PFN.

static PFN_vkGetInstanceProcAddr s_RealGetInstanceProcAddr  = nullptr;
static PFN_vkGetDeviceProcAddr   s_RealGetDeviceProcAddr    = nullptr;
static PFN_vkQueueSubmit         s_RealQueueSubmit          = nullptr;
static PFN_vkQueueSubmit2        s_RealQueueSubmit2         = nullptr;
static PFN_vkQueueSubmit2KHR     s_RealQueueSubmit2KHR      = nullptr;
// §J.3.e.2.i.18 (v1.4.82) — Vulkan spec requires ALL VkQueue operations
// externally synchronised, not just Submit.  Wrap Present + WaitIdle too.
static PFN_vkQueuePresentKHR     s_RealQueuePresentKHR      = nullptr;
static PFN_vkQueueWaitIdle       s_RealQueueWaitIdle        = nullptr;
static PFN_vkQueueBindSparse     s_RealQueueBindSparse      = nullptr;

static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueueSubmit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueueSubmit2(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueueSubmit2KHR(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2KHR* pSubmits, VkFence fence)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

// §J.3.e.2.i.18 (v1.4.82) — Wrap remaining VkQueue ops to fully satisfy
// "VkQueue externally synchronised" Vulkan spec requirement.
static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueuePresentKHR(queue, pPresentInfo);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueueWaitIdle(VkQueue queue)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueueWaitIdle(queue);
}
static VKAPI_ATTR VkResult VKAPI_CALL vkfrucWrappedQueueBindSparse(
    VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence fence)
{
    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
    return s_RealQueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
}

// §J.3.e.2.i.18 macro to dedupe lookup + wrap pattern (3 instance-proc +
// 3 device-proc copies = 6 sites × ~10 LOC each without macro).
#define VKFRUC_PROC_WRAP_CACHE(REAL_TYPE, REAL_VAR, WRAPPER_FN, REAL_LOOKUP) \
    if (!REAL_VAR) { REAL_VAR = reinterpret_cast<REAL_TYPE>(REAL_LOOKUP); }   \
    return reinterpret_cast<PFN_vkVoidFunction>(&WRAPPER_FN)

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkfrucWrappedGetDeviceProcAddr(
    VkDevice device, const char* pName)
{
    if (!s_RealGetDeviceProcAddr) return nullptr;
    if (pName != nullptr) {
        if (strcmp(pName, "vkQueueSubmit") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit, s_RealQueueSubmit,
                vkfrucWrappedQueueSubmit, s_RealGetDeviceProcAddr(device, pName));
        }
        if (strcmp(pName, "vkQueueSubmit2") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit2, s_RealQueueSubmit2,
                vkfrucWrappedQueueSubmit2, s_RealGetDeviceProcAddr(device, pName));
        }
        if (strcmp(pName, "vkQueueSubmit2KHR") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit2KHR, s_RealQueueSubmit2KHR,
                vkfrucWrappedQueueSubmit2KHR, s_RealGetDeviceProcAddr(device, pName));
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueuePresentKHR, s_RealQueuePresentKHR,
                vkfrucWrappedQueuePresentKHR, s_RealGetDeviceProcAddr(device, pName));
        }
        if (strcmp(pName, "vkQueueWaitIdle") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueWaitIdle, s_RealQueueWaitIdle,
                vkfrucWrappedQueueWaitIdle, s_RealGetDeviceProcAddr(device, pName));
        }
        if (strcmp(pName, "vkQueueBindSparse") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueBindSparse, s_RealQueueBindSparse,
                vkfrucWrappedQueueBindSparse, s_RealGetDeviceProcAddr(device, pName));
        }
    }
    return s_RealGetDeviceProcAddr(device, pName);
}

PFN_vkVoidFunction VkFrucRenderer::wrappedGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    if (!s_RealGetInstanceProcAddr) {
        // Should never happen — we set s_RealGetInstanceProcAddr in
        // prepareDecoderContext before assigning this wrapper.
        return nullptr;
    }
    if (pName != nullptr) {
        // §J.3.e.2.i.16 (v1.4.81) — FFmpeg + libplacebo use vkGetDeviceProcAddr
        // for device-level functions (including vkQueueSubmit family).  Hand
        // them our wrapping version so submits get serialised.
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
            if (!s_RealGetDeviceProcAddr) {
                s_RealGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                    s_RealGetInstanceProcAddr(instance, pName));
            }
            return reinterpret_cast<PFN_vkVoidFunction>(&vkfrucWrappedGetDeviceProcAddr);
        }
        if (strcmp(pName, "vkQueueSubmit") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit, s_RealQueueSubmit,
                vkfrucWrappedQueueSubmit, s_RealGetInstanceProcAddr(instance, pName));
        }
        if (strcmp(pName, "vkQueueSubmit2") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit2, s_RealQueueSubmit2,
                vkfrucWrappedQueueSubmit2, s_RealGetInstanceProcAddr(instance, pName));
        }
        if (strcmp(pName, "vkQueueSubmit2KHR") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueSubmit2KHR, s_RealQueueSubmit2KHR,
                vkfrucWrappedQueueSubmit2KHR, s_RealGetInstanceProcAddr(instance, pName));
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueuePresentKHR, s_RealQueuePresentKHR,
                vkfrucWrappedQueuePresentKHR, s_RealGetInstanceProcAddr(instance, pName));
        }
        if (strcmp(pName, "vkQueueWaitIdle") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueWaitIdle, s_RealQueueWaitIdle,
                vkfrucWrappedQueueWaitIdle, s_RealGetInstanceProcAddr(instance, pName));
        }
        if (strcmp(pName, "vkQueueBindSparse") == 0) {
            VKFRUC_PROC_WRAP_CACHE(PFN_vkQueueBindSparse, s_RealQueueBindSparse,
                vkfrucWrappedQueueBindSparse, s_RealGetInstanceProcAddr(instance, pName));
        }
    }
    return s_RealGetInstanceProcAddr(instance, pName);
}

// §J.3.e.2.i — FRUC status getters 給 perf overlay (ffmpeg.cpp:1073-1107) 跟
// Pacer::renderFrame (pacer.cpp:348) 用.  D3D11VARenderer 對應在
// d3d11va.cpp:2306-2322.  VkFruc 的 FRUC 走 native Vulkan compute (ME→
// median→warp) + dual-present，所以判斷條件是 m_FrucMode + m_FrucReady +
// m_DualMode 三者皆 true.  m_FRUCPaused 由 base class 提供 (renderer.h:310)，
// Ctrl+Alt+Shift+F 切換；renderFrameSw 一開頭快照成 frucPausedThisFrame，
// 跳過 dual-acquire + interp render pass + FRUC compute chain (5 sites).
bool VkFrucRenderer::isFRUCActive() const
{
    return m_FrucMode && m_FrucReady && m_DualMode;
}

bool VkFrucRenderer::lastFrameHadFRUCInterp() const
{
    return m_FrucMode && m_FrucReady && m_DualMode && !m_FRUCPaused.load();
}

// §B2 2026-05-06 — TRIPLE 60→180 推 2 張 interp/server frame，
// pacer.cpp:349 累加 frucInterpolatedFrames 要 +2 才符合 effectiveFps 計算.
int VkFrucRenderer::lastFrameInterpolatedCount() const
{
    if (!lastFrameHadFRUCInterp()) return 0;
    return m_TripleMode ? 2 : 1;
}

const char* VkFrucRenderer::getFRUCBackendName() const
{
    return "VkFruc-Vulkan compute";
}

// VipleStream: Ctrl+Alt+Shift+F hotkey 翻轉 m_FRUCPaused，下一幀
// renderFrameSw 快照後跳過 dual-present + FRUC compute.  Server FPS 在
// Session::toggleFRUC (session.cpp:1604) 讀回 m_FRUCPaused 後決定全速還
// 是減半發送.  D3D11VARenderer 對應 d3d11va.cpp:2324.
void VkFrucRenderer::toggleFRUC()
{
    bool paused = !m_FRUCPaused.load();
    m_FRUCPaused.store(paused);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] FRUC %s via hotkey", paused ? "PAUSED" : "RESUMED");
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
    unloadNvOfApi();              // §B-NVOF Phase 3 — release nvofapi64.dll handle
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
    // §B2 follow-up 2026-05-06 — Optical Flow queue probe (VK_NV_optical_flow).
    // VK_QUEUE_OPTICAL_FLOW_BIT_NV 的數值是 0x00000100 (bit 8). NV Ampere+
    // 顯卡會 advertise 這個 queue family；用來 dispatch HW optical flow
    // 取代 block-matching ME.  Probe-only at this phase, no enable yet.
    constexpr VkQueueFlagBits OPTICAL_FLOW_BIT = (VkQueueFlagBits)0x00000100;
    uint32_t opticalFlowQF = UINT32_MAX;
    uint32_t opticalFlowQueueCount = 0;
    for (uint32_t qf = 0; qf < qfCount; qf++) {
        if (qfs[qf].queueFlags & OPTICAL_FLOW_BIT) {
            opticalFlowQF = qf;
            opticalFlowQueueCount = qfs[qf].queueCount;
            break;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-NVOF] §B2-followup queue probe: optical flow QF=%s%u count=%u",
                opticalFlowQF == UINT32_MAX ? "(none) " : "",
                opticalFlowQF == UINT32_MAX ? 0 : opticalFlowQF,
                opticalFlowQueueCount);

    // §J.3.e.2.i.10 Phase 2A — dedicated compute queue family probe.  Look for
    // a QF with VK_QUEUE_COMPUTE_BIT set and VK_QUEUE_GRAPHICS_BIT cleared
    // (i.e. truly dedicated, not the universal QF=0).  On NV Ampere/Ada this
    // is typically QF=2.  When available, FRUC compute (Phase 2B+) gets
    // submitted there so it can overlap with graphics-queue render+present.
    // UINT32_MAX = none found → caller falls back to m_GraphicsQueue submit.
    m_ComputeQueueFamily = UINT32_MAX;
    for (uint32_t qf = 0; qf < qfCount; qf++) {
        const auto flags = qfs[qf].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && !(flags & VK_QUEUE_GRAPHICS_BIT)) {
            m_ComputeQueueFamily = qf;
            break;
        }
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A async-compute QF probe: %s%u%s",
                m_ComputeQueueFamily == UINT32_MAX ? "(none — fallback to graphics QF=" : "QF=",
                m_ComputeQueueFamily == UINT32_MAX ? m_QueueFamily : m_ComputeQueueFamily,
                m_ComputeQueueFamily == UINT32_MAX ? ")" : " (dedicated COMPUTE_BIT, no GRAPHICS_BIT)");

    // §J.3.e.2.i.11 (v1.4.66) — cross-hardware FRUC auto-tier 偵測 (heuristic).
    // 啟動時根據 GPU 強度自動選 path + inferDim，避免使用者手動 tune setting。
    // v1.4.66 只做 deviceName / deviceType / limits 的 heuristic 粗判，存到
    // StreamingPreferences::vkfrucDetectedTier 給 v1.4.68 wire 自動套用用。
    // v1.4.67 會加 Conv2D micro-benchmark dispatch 細修這個結果。
    //
    // 分級邏輯 (粗略，accepts false-positive，benchmark 階段會修正)：
    //   - INTEGRATED_GPU / CPU type           → ENTRY      (內顯 / 老卡 / 沒 DGPU)
    //   - DISCRETE_GPU + deviceName 含 "RTX 40" / "RX 78" / "RX 79" / "Arc B"
    //                                         → QUALITY    (高階卡)
    //   - DISCRETE_GPU + deviceName 含 "RTX 30" / "RTX 20" / "RX 67" / "RX 6800"
    //                                         / "RX 6900" / "Arc A7" / "Arc A580"
    //                                         → BALANCED   (中階卡)
    //   - DISCRETE_GPU + deviceName 含 "GTX 16" / "GTX 10" / "RX 5" / "RX 6500"
    //                                         / "RX 6600" / "Arc A3" / "Arc A380"
    //                                         → PERFORMANCE (入門 dGPU)
    //   - 其他 DISCRETE_GPU 不識別            → BALANCED   (安全中位數)
    //   - 任何 fail / unknown                 → ENTRY      (保守 fallback)
    //
    // v1.4.67 micro-benchmark 會根據實測 GPU time 重新分級，所以這個 heuristic
    // 只是 v1.4.67 跑 benchmark 前的 placeholder + 給 v1.4.67 比對 sanity check。
    auto deduceTierFromDeviceName = [](const VkPhysicalDeviceProperties& p)
        -> StreamingPreferences::VkfrucGpuTier {
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU
            || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            return StreamingPreferences::VGT_ENTRY;
        }
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return StreamingPreferences::VGT_ENTRY;
        }
        const QString name = QString::fromUtf8(p.deviceName);
        auto contains = [&](const char* s) { return name.contains(QLatin1String(s),
                                                                   Qt::CaseInsensitive); };
        // Quality candidates (RTX 40-series / RDNA3 high-end / Arc Battlemage)
        if (contains("RTX 40") || contains("RTX 50") || contains("RX 7800")
            || contains("RX 7900") || contains("RX 9070") || contains("RX 9080")
            || contains("RX 9090") || contains("Arc B")) {
            return StreamingPreferences::VGT_QUALITY;
        }
        // Balanced candidates (RTX 20/30 / RDNA2 high-mid / Arc A7)
        if (contains("RTX 30") || contains("RTX 20") || contains("RTX 16")
            || contains("RX 6700") || contains("RX 6800") || contains("RX 6900")
            || contains("RX 7600") || contains("RX 7700")
            || contains("Arc A7") || contains("Arc A580") || contains("Arc A770")) {
            return StreamingPreferences::VGT_BALANCED;
        }
        // Performance candidates (GTX 10/16-series / RDNA1 / RDNA2 low / Arc A3)
        if (contains("GTX 16") || contains("GTX 10") || contains("RX 5")
            || contains("RX 6500") || contains("RX 6600")
            || contains("Arc A380") || contains("Arc A3") || contains("Arc A580")) {
            return StreamingPreferences::VGT_PERFORMANCE;
        }
        // Unknown DISCRETE_GPU — assume Balanced (safe middle ground).  v1.4.67
        // benchmark will refine.
        return StreamingPreferences::VGT_BALANCED;
    };
    const StreamingPreferences::VkfrucGpuTier heuristicTier = deduceTierFromDeviceName(chosen);
    m_DetectedGpuTier = (int)heuristicTier;
    const char* tierName =
        heuristicTier == StreamingPreferences::VGT_ENTRY       ? "ENTRY"       :
        heuristicTier == StreamingPreferences::VGT_PERFORMANCE ? "PERFORMANCE" :
        heuristicTier == StreamingPreferences::VGT_BALANCED    ? "BALANCED"    :
        heuristicTier == StreamingPreferences::VGT_QUALITY     ? "QUALITY"     : "UNKNOWN";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-TIER] §J.3.e.2.i.11 v1.4.66 heuristic: %s "
                "(deviceName='%s' deviceType=%d) — not yet wired to RIFE on/off "
                "or inferDim selection (v1.4.68 接通); v1.4.67 will refine via "
                "Conv2D micro-benchmark",
                tierName, chosen.deviceName, (int)chosen.deviceType);

    // §J.3.e.2.i.11 (v1.4.66) — 寫回 StreamingPreferences cache，下次啟動可
    // 跳過偵測直接讀 cache (除非 GPU name 變動).  GPU name 變動偵測 + cache
    // 重跑邏輯由 v1.4.67 benchmark 完成；本版只負責填 heuristic + GPU name +
    // save 一次.
    if (auto* prefs = StreamingPreferences::get()) {
        const QString currentGpuName = QString::fromUtf8(chosen.deviceName);
        const bool gpuChanged = (prefs->vkfrucDetectedGpuName != currentGpuName);
        const bool firstRun   = (prefs->vkfrucDetectedTier == StreamingPreferences::VGT_UNKNOWN);
        if (firstRun || gpuChanged) {
            prefs->vkfrucDetectedTier    = heuristicTier;
            prefs->vkfrucDetectedGpuName = currentGpuName;
            prefs->vkfrucBenchmarkNs     = 0;  // not measured yet (v1.4.67)
            prefs->save();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-TIER] cache write (firstRun=%d gpuChanged=%d): "
                        "tier=%s gpu='%s'",
                        firstRun, gpuChanged, tierName,
                        currentGpuName.toUtf8().constData());
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-TIER] cache hit (gpu unchanged): tier=%s "
                        "gpu='%s' benchmarkNs=%lld",
                        tierName, currentGpuName.toUtf8().constData(),
                        (long long)prefs->vkfrucBenchmarkNs);
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
                // §B2 follow-up 2026-05-06 — NVIDIA hardware optical flow
                // (Ampere+).  Used to replace block-matching ME (Stage 1 in
                // FRUC compute chain) with HW OF for higher-quality MV.
                "VK_NV_optical_flow",
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
    // §J.3.e.2.i.10 Phase 2A — request dedicated compute queue when probe at
    // pickPhysicalDeviceAndQueue() found one.  Skip when missing (fallback to
    // graphics queue) or coincidentally same as graphics/decode QF.
    if (m_ComputeQueueFamily != UINT32_MAX
        && m_ComputeQueueFamily != m_QueueFamily
        && m_ComputeQueueFamily != m_DecodeQueueFamily) {
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = m_ComputeQueueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &queuePriority;
        qcis.push_back(qci);
    }
    // §B2 follow-up 2026-05-06 — Optical Flow queue create info (only when
    // VIPLE_VKFRUC_NV_OF=1 + driver advertises the queue family).  Saved at
    // m_OpticalFlowQueueFamily so subsequent OF session creation + cmd buf
    // submission knows which queue to use.  Stored in member here, fetched
    // queue handle below after vkCreateDevice.
    bool wantNvOfQ = vkfrucWantNvOfFromUserOrEnv();
    m_OpticalFlowQueueFamily = UINT32_MAX;
    if (wantNvOfQ) {
        // Re-query queue family properties locally (qfs vector lives in
        // pickPhysicalDevice's scope, not visible here).
        auto pfnGetQFP_OF = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        if (pfnGetQFP_OF) {
            uint32_t qfCountOf = 0;
            pfnGetQFP_OF(m_PhysicalDevice, &qfCountOf, nullptr);
            std::vector<VkQueueFamilyProperties> qfsOf(qfCountOf);
            pfnGetQFP_OF(m_PhysicalDevice, &qfCountOf, qfsOf.data());
            constexpr VkQueueFlagBits kOFBit = (VkQueueFlagBits)0x00000100;
            for (uint32_t qf = 0; qf < qfCountOf; qf++) {
                if (qfsOf[qf].queueFlags & kOFBit) {
                    m_OpticalFlowQueueFamily = qf;
                    break;
                }
            }
        }
        if (m_OpticalFlowQueueFamily != UINT32_MAX
            && m_OpticalFlowQueueFamily != m_QueueFamily
            && m_OpticalFlowQueueFamily != m_DecodeQueueFamily) {
            VkDeviceQueueCreateInfo qci = {};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = m_OpticalFlowQueueFamily;
            qci.queueCount = 1;
            qci.pQueuePriorities = &queuePriority;
            qcis.push_back(qci);
        }
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
#elif defined(Q_OS_LINUX)
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
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
    // §B2 follow-up 2026-05-06 — VK_NV_optical_flow opt-in via env var.
    // Used to replace block-matching ME with HW optical flow on NV Ampere+.
    // Probe loop above already logged whether it's available — only enable
    // when env var set so default builds stay extension-minimal.
    bool wantNvOf = vkfrucWantNvOfFromUserOrEnv();
    if (wantNvOf) {
        wantedDevExts.push_back(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
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

    // §B-NVOF Phase 3c — VkPhysicalDeviceOpticalFlowFeaturesNV chain entry.
    // Linked into pNext only if user opted into VIPLE_VKFRUC_NV_OF=1 AND
    // device has OF queue family.  Otherwise skipped (extension not even
    // enabled, struct sType would be invalid).
    void* nextChain = (void*)&m_Sync2Feat;
    if (m_OpticalFlowQueueFamily != UINT32_MAX) {
        m_OfFeat = {};
        m_OfFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV;
        m_OfFeat.pNext = nextChain;
        nextChain = (void*)&m_OfFeat;
    }

    m_TimelineFeat = {};
    m_TimelineFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    m_TimelineFeat.pNext = nextChain;

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
    // §B2 follow-up 2026-05-06 — fetch OF queue handle if extension enabled.
    if (m_OpticalFlowQueueFamily != UINT32_MAX) {
        pfnGetDeviceQueue(m_Device, m_OpticalFlowQueueFamily, 0, &m_OpticalFlowQueue);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] OF queue fetched (QF=%u handle=%p)",
                    m_OpticalFlowQueueFamily, (void*)m_OpticalFlowQueue);
        // §B-NVOF Phase 3 — load nvofapi64.dll + entry function list.
        // Failure non-fatal: vkfruc falls back to block-matching ME path.
        loadNvOfApi();
    }
    // §J.3.e.2.i.10 Phase 2A — fetch dedicated compute queue handle when
    // available + distinct from graphics/decode QFs.  m_ComputeQueue stays
    // VK_NULL_HANDLE otherwise; Phase 2B+ check that handle for fallback.
    if (m_ComputeQueueFamily != UINT32_MAX
        && m_ComputeQueueFamily != m_QueueFamily
        && m_ComputeQueueFamily != m_DecodeQueueFamily) {
        pfnGetDeviceQueue(m_Device, m_ComputeQueueFamily, 0, &m_ComputeQueue);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A async-compute queue fetched (QF=%u handle=%p)",
                    m_ComputeQueueFamily, (void*)m_ComputeQueue);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkDevice created (%s, QF=%u, queue=%p)",
                "KHR_swapchain + KHR_sampler_ycbcr_conversion",
                m_QueueFamily, (void*)m_GraphicsQueue);
    return true;
}

// §B-NVOF Phase 3b 2026-05-06 — load nvofapi64.dll + populate funcList.
// nvofapi64.dll is shipped by NVIDIA driver in C:\Windows\System32 (Linux:
// libnvidia-opticalflow.so.1). Returns true if all PFN slots populated.
// Non-fatal failure: caller falls back to block-matching ME path.
bool VkFrucRenderer::loadNvOfApi()
{
#ifdef Q_OS_WIN32
    HMODULE hMod = LoadLibraryA("nvofapi64.dll");
    if (!hMod) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] LoadLibraryA(nvofapi64.dll) failed err=%lu — "
                    "falling back to block-matching ME",
                    GetLastError());
        return false;
    }
    typedef NV_OF_STATUS (NVOFAPI* PFNCreateInstanceVk)(uint32_t, NV_OF_VK_API_FUNCTION_LIST*);
    auto pfnCreate = (PFNCreateInstanceVk)GetProcAddress(hMod, "NvOFAPICreateInstanceVk");
    if (!pfnCreate) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] GetProcAddress(NvOFAPICreateInstanceVk) returned NULL");
        FreeLibrary(hMod);
        return false;
    }
    auto* funcList = new NV_OF_VK_API_FUNCTION_LIST{};
    NV_OF_STATUS st = pfnCreate(NV_OF_API_VERSION, funcList);
    if (st != NV_OF_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] NvOFAPICreateInstanceVk(apiVer=0x%x) failed status=%d",
                    (unsigned)NV_OF_API_VERSION, (int)st);
        delete funcList;
        FreeLibrary(hMod);
        return false;
    }
    m_NvOfApiModule = (void*)hMod;
    m_NvOfFuncList  = (void*)funcList;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-NVOF] loaded nvofapi64.dll OK — funcList populated "
                "(create=%p init=%p register=%p execute=%p destroy=%p)",
                (void*)funcList->nvCreateOpticalFlowVk,
                (void*)funcList->nvOFInit,
                (void*)funcList->nvOFRegisterResourceVk,
                (void*)funcList->nvOFExecuteVk,
                (void*)funcList->nvOFDestroy);
    return true;
#else
    // Linux: dlopen("libnvidia-opticalflow.so.1") path; deferred until §B-NVOF
    // ships on Linux (nvofapi.so distribution differs across NV driver flavours).
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-NVOF] Linux nvofapi loading not yet wired");
    return false;
#endif
}

void VkFrucRenderer::unloadNvOfApi()
{
    if (m_NvOfFuncList) {
        delete (NV_OF_VK_API_FUNCTION_LIST*)m_NvOfFuncList;
        m_NvOfFuncList = nullptr;
    }
#ifdef Q_OS_WIN32
    if (m_NvOfApiModule) {
        FreeLibrary((HMODULE)m_NvOfApiModule);
        m_NvOfApiModule = nullptr;
    }
#endif
}

// §B-NVOF Phase 3c 2026-05-06 — create OF session: nvCreateOpticalFlowVk +
// nvOFInit + alloc NV12 prev/curr input images + R16G16_S10_5_NV flow output
// image + nvOFRegisterResourceVk × 3.  Failure non-fatal: caller falls back.
//
// Image format choices follow SDK NvOFUtilsVulkan::NvOFBufferFormatToVkFormat:
//   NV_OF_BUFFER_FORMAT_NV12     → VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
//   NV_OF_BUFFER_FORMAT_SHORT2   → VK_FORMAT_R16G16_S10_5_NV (Q10.5 fixed)
//
// Output grid 4 chosen per NV_OF_OUTPUT_VECTOR_GRID_SIZE_4 (4×4 px per MV).
// At 1080p → flow image 480×270 = 129 600 vectors × 4 B = 518 KB.
bool VkFrucRenderer::createOpticalFlowSession(uint32_t width, uint32_t height)
{
    if (!m_NvOfFuncList) return false;
    if (m_NvOfReady) return true;

    auto* funcList = (NV_OF_VK_API_FUNCTION_LIST*)m_NvOfFuncList;
    NvOFHandle hOf = nullptr;
    NV_OF_STATUS st = funcList->nvCreateOpticalFlowVk(m_Instance, m_PhysicalDevice, m_Device, &hOf);
    if (st != NV_OF_SUCCESS || !hOf) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] nvCreateOpticalFlowVk failed status=%d", (int)st);
        return false;
    }
    m_NvOfHandle = (void*)hOf;
    m_NvOfWidth  = width;
    m_NvOfHeight = height;

    // §B-NVOF Phase 7C 2026-05-06 — VIPLE_VKFRUC_NV_OF_PERF env var
    // (slow|medium|fast) maps to NV_OF_PERF_LEVEL.  SLOW=5 = best quality,
    // MEDIUM=10 = default balance, FAST=20 = lowest GPU cost.
    QByteArray perfEnv = qgetenv("VIPLE_VKFRUC_NV_OF_PERF");
    NV_OF_PERF_LEVEL perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    const char* perfLabel = "MEDIUM";
    if (perfEnv == "slow" || perfEnv == "SLOW") {
        perfLevel = NV_OF_PERF_LEVEL_SLOW;
        perfLabel = "SLOW";
    } else if (perfEnv == "fast" || perfEnv == "FAST") {
        perfLevel = NV_OF_PERF_LEVEL_FAST;
        perfLabel = "FAST";
    }
    // §B-NVOF Phase 7D 2026-05-06 — VIPLE_VKFRUC_NV_OF_GRID env var (1|2|4).
    // Smaller grid = more flow vectors per frame = higher precision but
    // more compute/bandwidth.  grid=4 default; grid=1 max precision (8x
    // staging buffer at 1080p, 8x8 average per mv cell in converter).
    QByteArray gridEnv = qgetenv("VIPLE_VKFRUC_NV_OF_GRID");
    // Default grid=2 from §B-NVOF Phase 7CD benchmark (best precision /
    // smoothing trade-off; beats both grid=1 and grid=4 on testufo).
    NV_OF_OUTPUT_VECTOR_GRID_SIZE outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_2;
    m_NvOfGridSize = 2;
    if (gridEnv == "1") {
        outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
        m_NvOfGridSize = 1;
    } else if (gridEnv == "4") {
        outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        m_NvOfGridSize = 4;
    }

    NV_OF_INIT_PARAMS initParams = {};
    initParams.width             = width;
    initParams.height            = height;
    initParams.outGridSize       = outGridSize;
    initParams.hintGridSize      = NV_OF_HINT_VECTOR_GRID_SIZE_8;  // unused unless enableExternalHints
    initParams.mode              = NV_OF_MODE_OPTICALFLOW;
    initParams.perfLevel         = perfLevel;
    initParams.predDirection     = NV_OF_PRED_DIRECTION_FORWARD;
    initParams.inputBufferFormat = NV_OF_BUFFER_FORMAT_NV12;

    st = funcList->nvOFInit(hOf, &initParams);
    if (st != NV_OF_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] nvOFInit (W=%u H=%u grid=%u perf=%s NV12) failed status=%d",
                    width, height, m_NvOfGridSize, perfLabel, (int)st);
        funcList->nvOFDestroy(hOf);
        m_NvOfHandle = nullptr;
        return false;
    }

    // §B-NVOF Phase 7F 2026-05-06 — query device caps post-init for log /
    // validation.  nvOFGetCaps requires nvOFInit to have completed first.
    // If caps mismatch (e.g. driver bumped after we cached unsupported grid),
    // log warn — session is already initialised so we don't reconfigure here,
    // just provide diagnostic for future driver / GPU compat issues.
    auto queryCaps = [&](NV_OF_CAPS capParam, std::vector<uint32_t>& out) -> bool {
        uint32_t sz = 0;
        if (funcList->nvOFGetCaps(hOf, capParam, nullptr, &sz) != NV_OF_SUCCESS || sz == 0) {
            return false;
        }
        out.resize(sz);
        return funcList->nvOFGetCaps(hOf, capParam, out.data(), &sz) == NV_OF_SUCCESS;
    };
    std::vector<uint32_t> supportedGrids;
    std::vector<uint32_t> capsWidth, capsHeight, capsRoiSupported, capsRoiMaxNum;
    if (queryCaps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, supportedGrids)
        && !supportedGrids.empty()) {
        std::string gridListStr;
        for (uint32_t g : supportedGrids) {
            if (!gridListStr.empty()) gridListStr += ",";
            gridListStr += std::to_string(g);
        }
        bool found = false;
        for (uint32_t g : supportedGrids) {
            if (g == m_NvOfGridSize) { found = true; break; }
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF-CAPS] supported output grids [%s], using %u %s",
                    gridListStr.c_str(), m_NvOfGridSize, found ? "(in-list)" : "(MISMATCH!)");
    }
    if (queryCaps(NV_OF_CAPS_WIDTH_MAX, capsWidth) && !capsWidth.empty() &&
        queryCaps(NV_OF_CAPS_HEIGHT_MAX, capsHeight) && !capsHeight.empty()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF-CAPS] device max input %ux%u; "
                    "stream %ux%u %s",
                    capsWidth[0], capsHeight[0], width, height,
                    (width <= capsWidth[0] && height <= capsHeight[0]) ? "OK" : "EXCEEDS!");
    }
    if (queryCaps(NV_OF_CAPS_SUPPORT_ROI, capsRoiSupported) && !capsRoiSupported.empty()) {
        if (queryCaps(NV_OF_CAPS_SUPPORT_ROI_MAX_NUM, capsRoiMaxNum) && !capsRoiMaxNum.empty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF-CAPS] ROI support=%u maxNum=%u",
                        capsRoiSupported[0], capsRoiMaxNum[0]);
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-NVOF] OF session init OK (W=%u H=%u grid=%u perf=%s "
                "predFwd flowDims=%ux%u) — allocating image resources",
                width, height, m_NvOfGridSize, perfLabel,
                width / m_NvOfGridSize, height / m_NvOfGridSize);

    // §B-NVOF Phase 3d 2026-05-06 — allocate 3 VkImages and register them
    // with the OF session as NvOFGPUBufferHandle.
    //   m_NvOfInputCurr / m_NvOfInputPrev: NV12 (G8_B8R8_2PLANE_420_UNORM)
    //     full source size, TRANSFER_DST (we copy vkf->img[0] into them
    //     each frame). prev/curr swapped at end of chain.
    //   m_NvOfFlowImage: R16G16_SFIXED5_NV (Q10.5 fixed-point), flow grid
    //     dimensions, TRANSFER_SRC (we copy result to a staging buffer for
    //     the format converter compute shader to consume in Q1 form).
    auto getDevPaOf = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateImage  = (PFN_vkCreateImage)getDevPaOf(m_Device, "vkCreateImage");
    auto pfnGetImgMemReq = (PFN_vkGetImageMemoryRequirements)getDevPaOf(m_Device, "vkGetImageMemoryRequirements");
    auto pfnAllocMem     = (PFN_vkAllocateMemory)getDevPaOf(m_Device, "vkAllocateMemory");
    auto pfnBindImgMem   = (PFN_vkBindImageMemory)getDevPaOf(m_Device, "vkBindImageMemory");
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateImage || !pfnGetImgMemReq || !pfnAllocMem || !pfnBindImgMem || !pfnGetPdMemProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] PFN load failed for image alloc");
        funcList->nvOFDestroy(hOf);
        m_NvOfHandle = nullptr;
        return false;
    }
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(m_PhysicalDevice, &memProps);
    auto findMemTypeOf = [&](uint32_t typeBits, VkMemoryPropertyFlags wantFlags) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & wantFlags) == wantFlags) {
                return (int)i;
            }
        }
        return -1;
    };
    auto allocImage = [&](VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                          VkImage& outImage, VkDeviceMemory& outMem) -> bool {
        VkImageCreateInfo ici = {};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = fmt;
        ici.extent        = { w, h, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = usage;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (pfnCreateImage(m_Device, &ici, nullptr, &outImage) != VK_SUCCESS) {
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetImgMemReq(m_Device, outImage, &mr);
        int idx = findMemTypeOf(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (idx < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)idx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        if (pfnBindImgMem(m_Device, outImage, outMem, 0) != VK_SUCCESS) return false;
        return true;
    };

    const VkFormat NV12_FMT = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    const VkFormat FLOW_FMT = VK_FORMAT_R16G16_S10_5_NV;
    if (!allocImage(NV12_FMT, width, height, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    m_NvOfInputCurr, m_NvOfInputCurrMem) ||
        !allocImage(NV12_FMT, width, height, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    m_NvOfInputPrev, m_NvOfInputPrevMem) ||
        !allocImage(FLOW_FMT, width / m_NvOfGridSize, height / m_NvOfGridSize,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    m_NvOfFlowImage, m_NvOfFlowImageMem)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] VkImage alloc failed");
        destroyOpticalFlowSession();
        return false;
    }

    auto registerImage = [&](VkImage img, VkFormat fmt, void*& outHandle) -> bool {
        NV_OF_REGISTER_RESOURCE_PARAMS_VK params = {};
        params.image       = img;
        params.format      = fmt;
        params.hOFGpuBuffer = (NvOFGPUBufferHandle*)&outHandle;
        NV_OF_STATUS s = funcList->nvOFRegisterResourceVk(hOf, &params);
        if (s != NV_OF_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] nvOFRegisterResourceVk(format=%d) failed status=%d",
                        (int)fmt, (int)s);
            return false;
        }
        return true;
    };
    if (!registerImage(m_NvOfInputCurr, NV12_FMT, m_NvOfHandleCurr) ||
        !registerImage(m_NvOfInputPrev, NV12_FMT, m_NvOfHandlePrev) ||
        !registerImage(m_NvOfFlowImage, FLOW_FMT, m_NvOfHandleFlow)) {
        destroyOpticalFlowSession();
        return false;
    }

    // §B-NVOF Phase 4a — OF queue command pool + cmd buffer + timeline sem +
    // flow staging buffer.  Per-frame chain (Phase 4b) records the OF cmd
    // buffer once per server frame, submitted to m_OpticalFlowQueue.
    auto pfnCreateCmdPool   = (PFN_vkCreateCommandPool)getDevPaOf(m_Device, "vkCreateCommandPool");
    auto pfnAllocCmdBufs    = (PFN_vkAllocateCommandBuffers)getDevPaOf(m_Device, "vkAllocateCommandBuffers");
    auto pfnCreateSem       = (PFN_vkCreateSemaphore)getDevPaOf(m_Device, "vkCreateSemaphore");
    auto pfnCreateBuffer    = (PFN_vkCreateBuffer)getDevPaOf(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq    = (PFN_vkGetBufferMemoryRequirements)getDevPaOf(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnBindBufMem      = (PFN_vkBindBufferMemory)getDevPaOf(m_Device, "vkBindBufferMemory");
    if (!pfnCreateCmdPool || !pfnAllocCmdBufs || !pfnCreateSem
        || !pfnCreateBuffer || !pfnGetBufMemReq || !pfnBindBufMem) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-NVOF] Phase 4a PFN load failed");
        destroyOpticalFlowSession();
        return false;
    }
    {
        VkCommandPoolCreateInfo cpi = {};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = m_OpticalFlowQueueFamily;
        if (pfnCreateCmdPool(m_Device, &cpi, nullptr, &m_NvOfCmdPool) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] vkCreateCommandPool(OF queue) failed");
            destroyOpticalFlowSession();
            return false;
        }
        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = m_NvOfCmdPool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (pfnAllocCmdBufs(m_Device, &cbai, &m_NvOfCmdBuf) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] vkAllocateCommandBuffers(OF) failed");
            destroyOpticalFlowSession();
            return false;
        }
    }
    {
        VkSemaphoreTypeCreateInfo stci = {};
        stci.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        stci.initialValue  = 0;
        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sci.pNext = &stci;
        if (pfnCreateSem(m_Device, &sci, nullptr, &m_NvOfTimelineSem) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] vkCreateSemaphore(timeline) failed");
            destroyOpticalFlowSession();
            return false;
        }
        m_NvOfTimelineValue = 0;
    }
    // Flow staging buffer: receives vkCmdCopyImageToBuffer of m_NvOfFlowImage.
    // Size = flowW × flowH × 4 bytes (R16G16 = 2 × int16 = 4 B).
    {
        const uint32_t flowW = width / m_NvOfGridSize;
        const uint32_t flowH = height / m_NvOfGridSize;
        VkBufferCreateInfo bci = {};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = (VkDeviceSize)flowW * flowH * 4;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &m_NvOfFlowStaging) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] vkCreateBuffer(flow staging) failed");
            destroyOpticalFlowSession();
            return false;
        }
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, m_NvOfFlowStaging, &mr);
        int idx = findMemTypeOf(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (idx < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] findMemType(flow staging) failed");
            destroyOpticalFlowSession();
            return false;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)idx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &m_NvOfFlowStagingMem) != VK_SUCCESS ||
            pfnBindBufMem(m_Device, m_NvOfFlowStaging, m_NvOfFlowStagingMem, 0) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] flow staging memory alloc/bind failed");
            destroyOpticalFlowSession();
            return false;
        }
    }

    // §B-NVOF Phase 5 — allocate + update converter desc set now that
    // m_NvOfFlowStaging exists.  Pool is m_FrucDescPool sized for 7 sets,
    // converter is the 7th (after NV12RGB normal + native + ME + Median +
    // Warp + Warp2).
    if (m_FrucNvOfConvertDsl != VK_NULL_HANDLE && m_FrucNvOfConvertPipeline != VK_NULL_HANDLE) {
        auto pfnAllocDescSets = (PFN_vkAllocateDescriptorSets)getDevPaOf(
            m_Device, "vkAllocateDescriptorSets");
        auto pfnUpdateDescSets = (PFN_vkUpdateDescriptorSets)getDevPaOf(
            m_Device, "vkUpdateDescriptorSets");
        if (pfnAllocDescSets && pfnUpdateDescSets) {
            VkDescriptorSetAllocateInfo asi = {};
            asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            asi.descriptorPool     = m_FrucDescPool;
            asi.descriptorSetCount = 1;
            asi.pSetLayouts        = &m_FrucNvOfConvertDsl;
            if (pfnAllocDescSets(m_Device, &asi, &m_FrucNvOfConvertDescSet) == VK_SUCCESS) {
                VkDescriptorBufferInfo bi[2] = {};
                bi[0].buffer = m_NvOfFlowStaging;
                bi[0].offset = 0;
                bi[0].range  = VK_WHOLE_SIZE;
                bi[1].buffer = m_FrucMvFilteredBuf;
                bi[1].offset = 0;
                bi[1].range  = VK_WHOLE_SIZE;
                VkWriteDescriptorSet wds[2] = {};
                for (int b = 0; b < 2; b++) {
                    wds[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    wds[b].dstSet          = m_FrucNvOfConvertDescSet;
                    wds[b].dstBinding      = (uint32_t)b;
                    wds[b].descriptorCount = 1;
                    wds[b].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    wds[b].pBufferInfo     = &bi[b];
                }
                pfnUpdateDescSets(m_Device, 2, wds, 0, nullptr);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-NVOF] NvOFConvert desc set alloc/update OK "
                            "(staging→mv_filtered)");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-NVOF] vkAllocateDescriptorSets(NvOFConvert) failed — "
                            "OF result not consumed by chain");
            }
        }
    }

    m_NvOfReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-NVOF] OF session READY — 3 images registered "
                "(curr=%p prev=%p flow=%p) + cmdPool/cmdBuf/timelineSem/flowStaging",
                m_NvOfHandleCurr, m_NvOfHandlePrev, m_NvOfHandleFlow);
    return true;
}

void VkFrucRenderer::destroyOpticalFlowSession()
{
    if (!m_NvOfFuncList) return;
    auto* funcList = (NV_OF_VK_API_FUNCTION_LIST*)m_NvOfFuncList;

    // §B-NVOF Phase 3d — unregister resources before destroying handles.
    // Order: handles → images/memory → session.
    auto unregisterIfAny = [&](void*& h) {
        if (!h || !m_NvOfHandle) return;
        NV_OF_UNREGISTER_RESOURCE_PARAMS_VK params = {};
        params.hOFGpuBuffer = (NvOFGPUBufferHandle)h;
        funcList->nvOFUnregisterResourceVk(&params);
        h = nullptr;
    };
    unregisterIfAny(m_NvOfHandleCurr);
    unregisterIfAny(m_NvOfHandlePrev);
    unregisterIfAny(m_NvOfHandleFlow);

    if (m_Device != VK_NULL_HANDLE) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        if (getDevPa) {
            auto pfnDestroyImg     = (PFN_vkDestroyImage)getDevPa(m_Device, "vkDestroyImage");
            auto pfnDestroyView    = (PFN_vkDestroyImageView)getDevPa(m_Device, "vkDestroyImageView");
            auto pfnFreeMem        = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
            auto pfnDestroyBuf     = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
            auto pfnDestroySem     = (PFN_vkDestroySemaphore)getDevPa(m_Device, "vkDestroySemaphore");
            auto pfnDestroyCmdPool = (PFN_vkDestroyCommandPool)getDevPa(m_Device, "vkDestroyCommandPool");
            // §B-NVOF Phase 4a — release cross-queue execution resources.
            if (m_NvOfFlowStaging && pfnDestroyBuf) {
                pfnDestroyBuf(m_Device, m_NvOfFlowStaging, nullptr);
                m_NvOfFlowStaging = VK_NULL_HANDLE;
            }
            if (m_NvOfFlowStagingMem && pfnFreeMem) {
                pfnFreeMem(m_Device, m_NvOfFlowStagingMem, nullptr);
                m_NvOfFlowStagingMem = VK_NULL_HANDLE;
            }
            if (m_NvOfTimelineSem && pfnDestroySem) {
                pfnDestroySem(m_Device, m_NvOfTimelineSem, nullptr);
                m_NvOfTimelineSem = VK_NULL_HANDLE;
            }
            // m_NvOfCmdBuf freed implicitly when pool is destroyed.
            if (m_NvOfCmdPool && pfnDestroyCmdPool) {
                pfnDestroyCmdPool(m_Device, m_NvOfCmdPool, nullptr);
                m_NvOfCmdPool = VK_NULL_HANDLE;
                m_NvOfCmdBuf  = VK_NULL_HANDLE;
            }
            if (m_NvOfFlowImageView && pfnDestroyView) {
                pfnDestroyView(m_Device, m_NvOfFlowImageView, nullptr);
                m_NvOfFlowImageView = VK_NULL_HANDLE;
            }
#define DESTROY_IMG_MEM(img, mem)                                            \
            if (img && pfnDestroyImg) { pfnDestroyImg(m_Device, img, nullptr); img = VK_NULL_HANDLE; } \
            if (mem && pfnFreeMem)    { pfnFreeMem(m_Device, mem, nullptr);    mem = VK_NULL_HANDLE; }
            DESTROY_IMG_MEM(m_NvOfInputCurr,  m_NvOfInputCurrMem)
            DESTROY_IMG_MEM(m_NvOfInputPrev,  m_NvOfInputPrevMem)
            DESTROY_IMG_MEM(m_NvOfFlowImage,  m_NvOfFlowImageMem)
#undef DESTROY_IMG_MEM
        }
    }

    if (m_NvOfHandle) {
        funcList->nvOFDestroy((NvOFHandle)m_NvOfHandle);
        m_NvOfHandle = nullptr;
    }
    m_NvOfReady  = false;
    m_NvOfWidth  = 0;
    m_NvOfHeight = 0;
}

// §B-DUMP 2026-05-07 — fp32 planar RGB → uint8 packed RGBA.  Source layout:
// 3 contiguous planes [R-plane (W*H fp32)] [G-plane] [B-plane], each value
// nominally 0..1 (clamped here).  Output layout: W*H × RGBA (4 bytes / pixel),
// suitable for stbi_write_png.
void VkFrucRenderer::planarFp32RgbToUint8Rgba(const float* src, uint8_t* dst,
                                              uint32_t w, uint32_t h)
{
    const uint32_t plane = w * h;
    const float* rPlane = src;
    const float* gPlane = src + plane;
    const float* bPlane = src + 2u * plane;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t i = y * w + x;
            float r = rPlane[i];
            float g = gPlane[i];
            float b = bPlane[i];
            r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
            const uint32_t o = i * 4u;
            dst[o + 0] = (uint8_t)(r * 255.0f + 0.5f);
            dst[o + 1] = (uint8_t)(g * 255.0f + 0.5f);
            dst[o + 2] = (uint8_t)(b * 255.0f + 0.5f);
            dst[o + 3] = 0xFF;
        }
    }
}

// §B-DUMP 2026-05-07 — best-effort init: read env var, alloc per-slot
// host-visible staging buffers, mkdir dump dir, spawn writer thread.
// Returns true if dump enabled (caller can short-circuit per-frame copy when
// false).  Failure non-fatal — disables m_DumpEnabled and lets the rest of
// the renderer run unaffected.
//
// Layout note: 2026-05-07 unified to flat single-dir naming
// (`frame_NNNN_real.bmp` / `frame_NNNN_interp.bmp`, or `_interp1` /
// `_interp2` for TRIPLE) to match D3D11VARenderer's dump format.  Older
// `all/` + `real/` subdir pair removed — caused asymmetric drops under
// writer-thread backpressure (real always pushed first, all/ entries
// systematically lost).
bool VkFrucRenderer::initFrameDump()
{
    QByteArray dirEnv = qgetenv("VIPLE_VKFRUC_DUMP_DIR");
    if (dirEnv.isEmpty()) {
        m_DumpEnabled = false;
        return false;
    }
    m_DumpDir = dirEnv.constData();
    int n = qEnvironmentVariableIntValue("VIPLE_VKFRUC_DUMP_FRAMES");
    if (n > 0) m_DumpFramesTotal = n;
    int delay = qEnvironmentVariableIntValue("VIPLE_VKFRUC_DUMP_DELAY_MS");
    if (delay > 0) m_DumpDelayMs = delay;
    using namespace std::chrono;
    m_DumpSessionStartMs = duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();

    // mkdir dump dir (Windows _mkdir).  Tolerate "exists".  Flat layout
    // — no subdirs (see note in dtor comment above).
    auto mkdirIgnoreExists = [](const std::string& p) {
#ifdef Q_OS_WIN32
        _mkdir(p.c_str());
#else
        mkdir(p.c_str(), 0755);
#endif
    };
    mkdirIgnoreExists(m_DumpDir);

    // Reset flat-dir counter so each capture session starts at 0000.
    m_DumpDisplayCounter = 0;

    // Allocate staging buffers — sized for 1080p RGB fp32 (W*H*3*4 ≈ 25 MB).
    // Same alloc helper as createOpticalFlowSession but bound to
    // HOST_VISIBLE_COHERENT memory (no flush needed).
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateBuffer  = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq  = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem      = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem    = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnMapMem        = (PFN_vkMapMemory)getDevPa(m_Device, "vkMapMemory");
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem
        || !pfnMapMem || !pfnGetPdMemProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-DUMP] PFN load failed — disabling dump");
        return false;
    }
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(m_PhysicalDevice, &memProps);
    // §B-DUMP — CRUCIAL: prefer HOST_CACHED.  Without HOST_CACHED, NV
    // driver maps coherent-only memory as Write-Combine, where CPU reads
    // are ~200MB/s (uncached burst).  Reading 25MB staging at WC speed =
    // 125ms — render thread chokes at 1-3fps during capture.  HOST_CACHED
    // gives ~10GB/s reads (50× faster).  Most dGPUs expose CACHED+COHERENT;
    // fall back to coherent-only (slow but correct) if not.
    auto findHostVisibleType = [&](uint32_t bits) -> int {
        const VkMemoryPropertyFlags preferred =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        const VkMemoryPropertyFlags fallback =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & preferred) == preferred) {
                return (int)i;
            }
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-DUMP] no HOST_CACHED memory available — "
                    "falling back to WC; expect render-thread slowdown during dump");
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & fallback) == fallback) {
                return (int)i;
            }
        }
        return -1;
    };

    // Use exact source dimensions (matching m_FrucCurrRgbBuf alloc size in
    // createFrucComputeResources).  Fall back to 1080p if not yet stamped.
    uint32_t srcW = m_FrucSrcWidth  ? m_FrucSrcWidth  : 1920;
    uint32_t srcH = m_FrucSrcHeight ? m_FrucSrcHeight : 1080;
    m_DumpStagingSize = (VkDeviceSize)srcW * srcH * 3 * sizeof(float);

    auto allocStaging = [&](VkBuffer& outBuf, VkDeviceMemory& outMem, void*& outMap) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = m_DumpStagingSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, outBuf, &mr);
        int idx = findHostVisibleType(mr.memoryTypeBits);
        if (idx < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)idx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        if (pfnBindBufMem(m_Device, outBuf, outMem, 0) != VK_SUCCESS) return false;
        if (pfnMapMem(m_Device, outMem, 0, VK_WHOLE_SIZE, 0, &outMap) != VK_SUCCESS) return false;
        return true;
    };
    bool ok = true;
    for (uint32_t s = 0; s < kFrucFramesInFlight; ++s) {
        ok = ok && allocStaging(m_DumpStagingReal[s],    m_DumpStagingRealMem[s],    m_DumpStagingRealMap[s]);
        ok = ok && allocStaging(m_DumpStagingInterp1[s], m_DumpStagingInterp1Mem[s], m_DumpStagingInterp1Map[s]);
        ok = ok && allocStaging(m_DumpStagingInterp2[s], m_DumpStagingInterp2Mem[s], m_DumpStagingInterp2Map[s]);
    }

    // §B-DUMP MV — separate alloc with smaller size (mvW*mvH*2*int).
    m_DumpStagingMvSize = (VkDeviceSize)m_FrucMvWidth * m_FrucMvHeight * 2u * sizeof(int);
    auto allocMvStaging = [&](VkBuffer& outBuf, VkDeviceMemory& outMem, void*& outMap) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = m_DumpStagingMvSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        pfnGetBufMemReq(m_Device, outBuf, &mr);
        int idx = findHostVisibleType(mr.memoryTypeBits);
        if (idx < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = (uint32_t)idx;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        if (pfnBindBufMem(m_Device, outBuf, outMem, 0) != VK_SUCCESS) return false;
        if (pfnMapMem(m_Device, outMem, 0, VK_WHOLE_SIZE, 0, &outMap) != VK_SUCCESS) return false;
        return true;
    };
    for (uint32_t s = 0; s < kFrucFramesInFlight; ++s) {
        ok = ok && allocMvStaging(m_DumpStagingMv[s], m_DumpStagingMvMem[s], m_DumpStagingMvMap[s]);
    }
    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-DUMP] staging buffer alloc failed — disabling dump");
        teardownFrameDump();
        return false;
    }

    // Spawn writer thread.
    m_DumpWriterStop.store(false, std::memory_order_release);
    m_DumpWriterThread = std::thread([this]() {
        for (;;) {
            DumpJob job;
            {
                std::unique_lock<std::mutex> lk(m_DumpQueueMutex);
                m_DumpQueueCv.wait(lk, [&] {
                    return !m_DumpQueue.empty()
                        || m_DumpWriterStop.load(std::memory_order_acquire);
                });
                if (m_DumpQueue.empty()) break;  // stop requested
                job = std::move(m_DumpQueue.front());
                m_DumpQueue.pop();
            }
            // §B-DUMP — BMP not PNG.  PNG @ level 8 = 100-200ms / 1080p frame
            // → writer thread can't keep up at 60fps × 2-3 buffers = 120-180
            // BMPs/sec.  BMP is uncompressed (~5ms encode, file ~8MB).  Disk
            // usage trade-off acceptable for short diagnostic capture.
            int rc = stbi_write_bmp(job.path.c_str(),
                                    (int)job.width, (int)job.height, 4,
                                    job.rgba.data());
            if (rc == 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-DUMP] stbi_write_bmp failed for %s",
                            job.path.c_str());
            } else {
                // Log every write for visibility (volume is low: <= ~30 BMPs).
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-DUMP] wrote %s",
                            job.path.c_str());
            }
            ++m_DumpFramesWritten;
        }
    });

    m_DumpEnabled = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-DUMP] enabled — dir='%s' frames=%d delay=%lldms "
                "format=BMP queueCap=%zu (staging %u×%u fp32 RGB ≈ %llu MB × "
                "%u buffers, mem-type=HOST_CACHED preferred)",
                m_DumpDir.c_str(), m_DumpFramesTotal,
                (long long)m_DumpDelayMs, kDumpQueueCap,
                srcW, srcH,
                (unsigned long long)(m_DumpStagingSize / (1024 * 1024)),
                kFrucFramesInFlight * 3u);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-DUMP] NOTE: dump captures the EXACT same "
                "interp output as live streaming.  For low-motion content "
                "(static desktop, sub-pixel motion) the production Balanced "
                "mode collapses interp to curr, so 'interp ≈ real' is expected. "
                "To see clear ghosting proof FRUC chain runs, add "
                "VIPLE_VKFRUC_WARP_NO_MV=1 (forces 50/50 cross-fade, ignores MV).");
    return true;
}

void VkFrucRenderer::teardownFrameDump()
{
    // Signal writer thread to drain + stop.
    if (m_DumpWriterThread.joinable()) {
        m_DumpWriterStop.store(true, std::memory_order_release);
        m_DumpQueueCv.notify_all();
        m_DumpWriterThread.join();
    }
    // Free GPU resources.
    if (m_Device != VK_NULL_HANDLE && m_pfnGetInstanceProcAddr) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnDestroyBuf = getDevPa ? (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer") : nullptr;
        auto pfnFreeMem    = getDevPa ? (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory") : nullptr;
        auto pfnUnmap      = getDevPa ? (PFN_vkUnmapMemory)getDevPa(m_Device, "vkUnmapMemory") : nullptr;
        for (uint32_t s = 0; s < kFrucFramesInFlight; ++s) {
#define DESTROY_DUMP_STAGING(buf, mem, map)                                 \
            if (map && pfnUnmap) { pfnUnmap(m_Device, mem); map = nullptr; } \
            if (buf && pfnDestroyBuf) { pfnDestroyBuf(m_Device, buf, nullptr); buf = VK_NULL_HANDLE; } \
            if (mem && pfnFreeMem)    { pfnFreeMem(m_Device, mem, nullptr);    mem = VK_NULL_HANDLE; }
            DESTROY_DUMP_STAGING(m_DumpStagingReal[s],    m_DumpStagingRealMem[s],    m_DumpStagingRealMap[s])
            DESTROY_DUMP_STAGING(m_DumpStagingInterp1[s], m_DumpStagingInterp1Mem[s], m_DumpStagingInterp1Map[s])
            DESTROY_DUMP_STAGING(m_DumpStagingInterp2[s], m_DumpStagingInterp2Mem[s], m_DumpStagingInterp2Map[s])
            DESTROY_DUMP_STAGING(m_DumpStagingMv[s],       m_DumpStagingMvMem[s],       m_DumpStagingMvMap[s])
#undef DESTROY_DUMP_STAGING
        }
    }
    // Reset slot records.
    for (uint32_t s = 0; s < kFrucFramesInFlight; ++s) {
        m_DumpSlotRec[s] = { -1, false };
    }
    m_DumpEnabled = false;
}

// §B-DUMP — flush pending dump record at slot fence-wait reuse point.
// Caller has just done WaitForFences(m_SlotInFlightFence[slotIdx]) → GPU
// finished writing m_DumpStaging*[slotIdx]; host-coherent so no flush.
//
// Cost: ~3-8ms per call on render thread (fp32→uint8 conversion of 2-3
// 1080p planes).  Runs only N times during 1s capture window — after
// capture done, m_DumpFramesQueued >= total and we still flush remaining
// pending records (drain ring), then quietly skip on subsequent calls.
void VkFrucRenderer::flushDumpSlotIfPending(uint32_t slotIdx)
{
    if (!m_DumpEnabled) return;
    if (slotIdx >= kFrucFramesInFlight) return;
    DumpSlotRec rec = m_DumpSlotRec[slotIdx];
    if (rec.serverFrameIdx < 0) return;

    const uint32_t srcW = m_FrucSrcWidth  ? m_FrucSrcWidth  : 1920;
    const uint32_t srcH = m_FrucSrcHeight ? m_FrucSrcHeight : 1080;
    const size_t  rgbaBytes = (size_t)srcW * srcH * 4u;
    char nbuf[64] = {};

    // §B-DUMP layout (matches D3D11VARenderer.dumpBackbufferIfActive
    // 2026-05-07): single flat dir, monotonic counter, suffix tells type.
    // Sorted by name → display order:
    //   DUAL   (60→120): file 2N = real_N,            file 2N+1 = interp(N,N+1)
    //   TRIPLE (60→180): file 3N = real_N,            file 3N+1 = interp1, file 3N+2 = interp2
    // analyze_fruc_compare.py's existing D3D11 forward-indexing path
    // (interp at slot 2N+1 = mid(real_2N, real_2N+2)) just works with
    // this layout — no vkfruc-specific code path needed downstream.
    //
    // First-cycle skip: we drop the interp(s) on the very first cycle
    // (when counter == 0 going in, i.e. no real has been committed yet)
    // because prev is undefined at that point so the warp output is
    // garbage.  This mirrors NvOFFRUC's behavior (`hasInterp=false` on
    // its first call) — D3D11 dump skips the first interp the same way.
    // Result: file 0 is always real_0.
    //
    // Atomicity: previous design enqueued 3 (DUAL: 1 real + 2 all/) jobs
    // independently into the queue.  When writer-thread couldn't keep up,
    // each enqueue's `size >= cap` check made an independent drop decision,
    // and because real/ was always pushed first it would systematically
    // succeed while subsequent all/ jobs got dropped — so users saw
    // real=55 / all=38 even though both should have been the same multiple
    // of server-frame count.  Fix: build all candidates outside the queue
    // lock, then take the lock once and either commit them all or drop
    // them all.  Counter only advances on commit.
    DumpJob candidates[3];
    char    candPaths[3][96] = {};
    int     candCount = 0;

    auto buildCandidate = [&](const float* src, const char* path) {
        DumpJob& job = candidates[candCount];
        job.width  = srcW;
        job.height = srcH;
        job.rgba.resize(rgbaBytes);
        planarFp32RgbToUint8Rgba(src, job.rgba.data(), srcW, srcH);
        job.path = path;
        ++candCount;
    };

    int        tentativeCounter = m_DumpDisplayCounter;
    const bool firstCycle       = (m_DumpDisplayCounter == 0);

    // Within each non-first cycle: interp(s) FIRST, then real — matches
    // display order (interp shows between prev real and curr real, then
    // curr real shows last).  Sorted by filename this produces the
    // alternating pattern downstream tools expect:
    //   firstCycle (no prev → no interp): file 0 = real_0
    //   cycle 1: file 1 = interp(0,1),                file 2 = real_1
    //   cycle 2: file 3 = interp(1,2),                file 4 = real_2
    //   ...
    // → file 2N = real_N, file 2N+1 = interp(real_2N, real_2N+2).  Same
    // forward-indexing convention as D3D11VARenderer dumps.

    if (!firstCycle) {
        if (rec.wasTriple) {
            snprintf(candPaths[0], sizeof(candPaths[0]),
                     "%s/frame_%04d_interp1.bmp", m_DumpDir.c_str(), tentativeCounter++);
            buildCandidate((const float*)m_DumpStagingInterp1Map[slotIdx], candPaths[0]);

            snprintf(candPaths[1], sizeof(candPaths[1]),
                     "%s/frame_%04d_interp2.bmp", m_DumpDir.c_str(), tentativeCounter++);
            buildCandidate((const float*)m_DumpStagingInterp2Map[slotIdx], candPaths[1]);
        } else {
            snprintf(candPaths[0], sizeof(candPaths[0]),
                     "%s/frame_%04d_interp.bmp", m_DumpDir.c_str(), tentativeCounter++);
            buildCandidate((const float*)m_DumpStagingInterp1Map[slotIdx], candPaths[0]);
        }
    }

    // Real (always — present last in the cycle).
    char* realPath = candPaths[candCount];
    snprintf(realPath, sizeof(candPaths[0]),
             "%s/frame_%04d_real.bmp", m_DumpDir.c_str(), tentativeCounter++);
    buildCandidate((const float*)m_DumpStagingRealMap[slotIdx], realPath);

    // Atomic commit-or-drop under the queue lock.
    bool dropped = false;
    size_t queueSizeForLog = 0;
    {
        std::lock_guard<std::mutex> lk(m_DumpQueueMutex);
        if (m_DumpQueue.size() + (size_t)candCount > kDumpQueueCap) {
            dropped = true;
            queueSizeForLog = m_DumpQueue.size();
        } else {
            for (int i = 0; i < candCount; ++i) {
                m_DumpQueue.push(std::move(candidates[i]));
            }
            m_DumpDisplayCounter = tentativeCounter;
        }
    }
    if (dropped) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-DUMP] queue full (%zu/%zu) — dropping whole "
            "frame (%d entries; counter held at %d, server frame=%d)",
            queueSizeForLog, kDumpQueueCap, candCount,
            m_DumpDisplayCounter, rec.serverFrameIdx);
    } else {
        m_DumpQueueCv.notify_all();
    }

    // §B-DUMP MV — analyze MV buffer + write summary.  Format: int32 pairs
    // (mvX, mvY) in Q1 (×2 of pixel value, so mv=2 means 1 pixel actual).
    if (m_DumpStagingMvMap[slotIdx] && m_FrucMvWidth && m_FrucMvHeight) {
        const int32_t* mvData = (const int32_t*)m_DumpStagingMvMap[slotIdx];
        const uint32_t mvCount = m_FrucMvWidth * m_FrucMvHeight;
        int zeros = 0, nonzeros = 0;
        int maxAbsX = 0, maxAbsY = 0;
        long long sumAbsX = 0, sumAbsY = 0;
        for (uint32_t i = 0; i < mvCount; ++i) {
            int x = mvData[i * 2 + 0];
            int y = mvData[i * 2 + 1];
            if (x == 0 && y == 0) ++zeros; else ++nonzeros;
            int ax = x < 0 ? -x : x;
            int ay = y < 0 ? -y : y;
            sumAbsX += ax; sumAbsY += ay;
            if (ax > maxAbsX) maxAbsX = ax;
            if (ay > maxAbsY) maxAbsY = ay;
        }
        double meanAbsX = (double)sumAbsX / mvCount;
        double meanAbsY = (double)sumAbsY / mvCount;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-DUMP-MV] frame=%d mv=%ux%u zero=%d/%d (%.1f%%) "
            "nonzero=%d mean|x|=%.2f mean|y|=%.2f max|x|=%d max|y|=%d "
            "(Q1 units: 1 LSB = 0.5 pixel; >=2 needed for warp to engage)",
            rec.serverFrameIdx, m_FrucMvWidth, m_FrucMvHeight,
            zeros, mvCount, 100.0 * zeros / mvCount, nonzeros,
            meanAbsX, meanAbsY, maxAbsX, maxAbsY);

        // Also save the raw MV buffer as a binary file for later analysis.
        snprintf(nbuf, sizeof(nbuf), "%s/mv_frame_%04d.bin",
                 m_DumpDir.c_str(), rec.serverFrameIdx);
        FILE* fp = fopen(nbuf, "wb");
        if (fp) {
            fwrite(mvData, 1, (size_t)m_DumpStagingMvSize, fp);
            fclose(fp);
        }
    }

    // Slot consumed — clear so we don't re-enqueue if writer thread is slow.
    m_DumpSlotRec[slotIdx] = { -1, false };

    // Done log: when both queue full + ring drained.
    if (!m_DumpDoneLogged
        && m_DumpFramesQueued >= m_DumpFramesTotal) {
        bool allDrained = true;
        for (uint32_t s = 0; s < kFrucFramesInFlight; ++s) {
            if (m_DumpSlotRec[s].serverFrameIdx >= 0) { allDrained = false; break; }
        }
        if (allDrained) {
            m_DumpDoneLogged = true;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-DUMP] capture complete — %d server frames "
                "queued for PNG encode (PNG writer thread continues to drain "
                "queue in background)", m_DumpFramesQueued);
        }
    }
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
    if (m_TripleMode) {
        // §B2 2026-05-06 — TRIPLE 60→180 acquires 3 images per frame →
        // need minImageCount + 3 (typically 5) to keep vkAcquireNextImageKHR
        // from blocking when previous frame's 3 presents still in flight.
        uint32_t want = caps.minImageCount + 3;  // = 5 typically
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

    // ---- Descriptor pool + 3 sets ----
    // Set 1: m_InterpDescSet      → m_FrucInterpRgbBuf  (interp slot 1, midpoint or 1/3)
    // Set 2: m_InterpDescSet2     → m_FrucInterpRgbBuf2 (interp slot 2 for TRIPLE 2/3)
    // Set 3: m_RealCurrRgbDescSet → m_FrucCurrRgbBuf    (real slot in §B-quality (d) path)
    VkDescriptorPoolSize ps = {};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 3;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 3;
    dpCi.poolSizeCount = 1;
    dpCi.pPoolSizes = &ps;
    if (pfnCreateDescPool(m_Device, &dpCi, nullptr, &m_InterpDescPool) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo asi = {};
    asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    asi.descriptorPool = m_InterpDescPool;
    asi.descriptorSetCount = 1;
    asi.pSetLayouts = &m_InterpDescSetLayout;
    if (pfnAllocDescSets(m_Device, &asi, &m_InterpDescSet) != VK_SUCCESS) return false;
    if (pfnAllocDescSets(m_Device, &asi, &m_InterpDescSet2) != VK_SUCCESS) return false;
    if (pfnAllocDescSets(m_Device, &asi, &m_RealCurrRgbDescSet) != VK_SUCCESS) return false;

    VkDescriptorBufferInfo biInterp = {};
    biInterp.buffer = m_FrucInterpRgbBuf;
    biInterp.offset = 0;
    biInterp.range  = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo biInterp2 = {};
    biInterp2.buffer = m_FrucInterpRgbBuf2;
    biInterp2.offset = 0;
    biInterp2.range  = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo biCurr = {};
    biCurr.buffer = m_FrucCurrRgbBuf;
    biCurr.offset = 0;
    biCurr.range  = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wds[3] = {};
    wds[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[0].dstSet          = m_InterpDescSet;
    wds[0].dstBinding      = 0;
    wds[0].descriptorCount = 1;
    wds[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[0].pBufferInfo     = &biInterp;
    wds[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[1].dstSet          = m_InterpDescSet2;
    wds[1].dstBinding      = 0;
    wds[1].descriptorCount = 1;
    wds[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[1].pBufferInfo     = &biInterp2;
    wds[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds[2].dstSet          = m_RealCurrRgbDescSet;
    wds[2].dstBinding      = 0;
    wds[2].descriptorCount = 1;
    wds[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[2].pBufferInfo     = &biCurr;
    pfnUpdateDescSets(m_Device, 3, wds, 0, nullptr);

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
        // §J.3.e.X Path β crash fix (Aftermath dump 2026-05-08, symbolized
        // callstack VkFrucRenderer::drawOverlayInRenderPass) — wait for
        // GPU to finish BEFORE destroying the old overlay image/view.  In
        // dual-present + Path β path, the chain is 14ms vs block-match 4ms,
        // so more cmd buffers are in flight per overlay update — multiple
        // concurrent submits may still reference the old VkImageView via
        // descriptor sets when surface dimensions change → fragment shader
        // page-faults on the freed resource → VK_ERROR_DEVICE_LOST after
        // 30-60s sustained streaming.  Only fires on actual overlay resize
        // (rare during a stream — typically once at first overlay activation
        // or when text content changes dim significantly).  Cost: ~1 frame
        // worth of GPU wait, amortized ~zero per-frame.
        if (m_OverlayImage[type]) {
            auto pfnDeviceWaitIdle = (PFN_vkDeviceWaitIdle)
                ((PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(m_Instance, "vkGetDeviceProcAddr"))
                (m_Device, "vkDeviceWaitIdle");
            if (pfnDeviceWaitIdle) {
                pfnDeviceWaitIdle(m_Device);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] overlay resize %dx%d → %dx%d — "
                    "vkDeviceWaitIdle before destroying old image (race fix)",
                    m_OverlayWidth[type], m_OverlayHeight[type], w, h);
            }
        }
        // Free old (if any) — now safe since GPU has drained any references.
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
        // §B2 2026-05-06 — m_SlotAcquireSem 第 3 個 [2] 給 TRIPLE 的第三張
        // acquire；DUAL 只用 [0]/[1].  RenderDoneSem 維持 [2] 因為 dual-
        // present 用 per-image m_SwapchainRenderDoneSem signal/wait（per-
        // slot 的 RenderDoneSem 主要給 single-present mode 用）.
        for (int p = 0; p < 3; p++) {
            if (pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotAcquireSem[i][p]) != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateSemaphore acquire[%u][%d] failed",
                             i, p);
                return false;
            }
        }
        for (int p = 0; p < 2; p++) {
            if (pfnCreateSem(m_Device, &sci, nullptr,
                             &m_SlotRenderDoneSem[i][p]) != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-VKFRUC] §J.3.e.2.i.3.d vkCreateSemaphore renderDone[%u][%d] failed",
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

    // §J.3.e.2.i.10 Phase 2A — async-compute queue per-slot resources.
    // Created here so they share the slot ring lifecycle.  All non-fatal:
    // any failure leaves m_ComputeCmdPool / m_ComputeTimelineSem at
    // VK_NULL_HANDLE and Phase 2B+ falls back to graphics-queue submit.
    if (m_ComputeQueue != VK_NULL_HANDLE
        && m_ComputeQueueFamily != UINT32_MAX) {
        VkCommandPoolCreateInfo cpci = {};
        cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                              | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cpci.queueFamilyIndex = m_ComputeQueueFamily;
        if (pfnCreateCmdPool(m_Device, &cpci, nullptr, &m_ComputeCmdPool) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo ccbai = {};
            ccbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ccbai.commandPool        = m_ComputeCmdPool;
            ccbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ccbai.commandBufferCount = kFrucFramesInFlight;
            if (pfnAllocCmdBufs(m_Device, &ccbai, m_ComputeCmdBuf) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A vkAllocateCommandBuffers (compute) failed — fallback");
                auto pfnDestroyCmdPoolFb = (PFN_vkDestroyCommandPool)pfnGetDeviceProcAddr(
                    m_Device, "vkDestroyCommandPool");
                if (pfnDestroyCmdPoolFb) pfnDestroyCmdPoolFb(m_Device, m_ComputeCmdPool, nullptr);
                m_ComputeCmdPool = VK_NULL_HANDLE;
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A vkCreateCommandPool (compute) failed — fallback");
            m_ComputeCmdPool = VK_NULL_HANDLE;
        }

        // Timeline semaphore — initial value 0; compute submits signal
        // m_ComputeTimelineValue+=1, graphics submits wait that value.
        // Vulkan 1.2 core feature (VkPhysicalDeviceVulkan12Features::
        // timelineSemaphore must be enabled at device create — verify in
        // Phase 2B before relying on it).
        VkSemaphoreTypeCreateInfo tsci = {};
        tsci.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        tsci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        tsci.initialValue  = 0;
        VkSemaphoreCreateInfo tsem = {};
        tsem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        tsem.pNext = &tsci;
        if (pfnCreateSem(m_Device, &tsem, nullptr, &m_ComputeTimelineSem) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A timeline semaphore create failed — fallback");
            m_ComputeTimelineSem = VK_NULL_HANDLE;
        }
        m_ComputeTimelineValue = 0;

        const bool full = (m_ComputeCmdPool != VK_NULL_HANDLE
                       && m_ComputeTimelineSem != VK_NULL_HANDLE);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A async-compute "
                    "infrastructure %s (pool=%p sem=%p, %u cmd buffers)",
                    full ? "READY" : "PARTIAL — fallback",
                    (void*)m_ComputeCmdPool, (void*)m_ComputeTimelineSem,
                    (unsigned)kFrucFramesInFlight);

        // §J.3.e.2.i.10 Phase 2B (v1.4.54 env stub; v1.4.55-57 wiring;
        // v1.4.58 default flip).  Branching at the phase2BActive gate in
        // renderFrame checks (m_AsyncComputeRequested && m_AsyncComputeAvailable).
        //
        // v1.4.58 — default ON.  VIPLE_RIFE_VK_ASYNC_COMPUTE=0 is now the
        // explicit opt-out (bisect aid / driver-issue fallback); unset or
        // any non-"0" value enables the async-compute path.  Path will
        // still fall back to v1.4.55 single-cmd-buf flow whenever
        // m_AsyncComputeAvailable demotes to false (single-QF GPU, alloc
        // failure, etc.), so this flip can't cause a hard crash on
        // unsupported devices.
        m_AsyncComputeAvailable = full;
        const char* asyncEnv = std::getenv("VIPLE_RIFE_VK_ASYNC_COMPUTE");
        m_AsyncComputeRequested = (asyncEnv == nullptr
                                   || asyncEnv[0] == '\0'
                                   || asyncEnv[0] != '0');
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] Phase 2B async-compute gate: requested=%d "
                    "available=%d → would-be-active=%d (env "
                    "VIPLE_RIFE_VK_ASYNC_COMPUTE=%s, default ON v1.4.58)",
                    (int)m_AsyncComputeRequested, (int)m_AsyncComputeAvailable,
                    (int)(m_AsyncComputeRequested && m_AsyncComputeAvailable),
                    asyncEnv ? asyncEnv : "(unset, default ON)");

        // §J.3.e.2.i.25 (v1.4.87) — FRUC chain async compute gate.
        // 跟 RIFE path 共用底層 m_ComputeQueue + m_ComputeCmdBuf + m_ComputeTimelineSem,
        // 但獨立的 enable flag.  v1.4.87 純框架 commit, gate 預設 0 (不啟用).
        // v1.4.88 才真切 cmd buf + submit 到 compute queue + cross-QF barrier.
        m_FrucChainAsyncAvailable = full;
        const char* frucAsyncEnv = std::getenv("VIPLE_VKFRUC_FRUC_ASYNC");
        m_FrucChainAsyncRequested = (frucAsyncEnv != nullptr
                                     && frucAsyncEnv[0] != '\0'
                                     && frucAsyncEnv[0] != '0');
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-FRUC-ASYNC] gate: requested=%d available=%d "
                    "→ would-be-active=%d (env VIPLE_VKFRUC_FRUC_ASYNC=%s, "
                    "v1.4.87 framework-only commit, dispatch wiring lands v1.4.88)",
                    (int)m_FrucChainAsyncRequested, (int)m_FrucChainAsyncAvailable,
                    (int)(m_FrucChainAsyncRequested && m_FrucChainAsyncAvailable),
                    frucAsyncEnv ? frucAsyncEnv : "(unset, default OFF)");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2A async-compute "
                    "skipped (no dedicated compute QF on this GPU; "
                    "FRUC compute will use graphics queue)");
    }

    // §J.3.e.2.i.10f Path D — early-release per-slot copy cmd buffers.
    // Allocated from m_CmdPool (graphics queue) so they share lifecycle with
    // m_SlotCmdBuf.  See header comment near m_SlotCopyCmdBuf for the
    // two-submit pattern that releases AVFrame pool image after just the
    // ~100us image copy instead of after the full ~20ms FRUC chain.
    {
        VkCommandBufferAllocateInfo copyCbai = {};
        copyCbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        copyCbai.commandPool        = m_CmdPool;
        copyCbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        copyCbai.commandBufferCount = kFrucFramesInFlight;
        if (pfnAllocCmdBufs(m_Device, &copyCbai, m_SlotCopyCmdBuf) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.10f Path D vkAllocateCommandBuffers (copy) failed — fallback to single-submit");
            for (uint32_t i = 0; i < kFrucFramesInFlight; i++) m_SlotCopyCmdBuf[i] = VK_NULL_HANDLE;
        } else {
            // Timeline sem for cmd-buffer chaining (copy submit signals N,
            // chain submit waits N).  Independent of m_ComputeTimelineSem.
            VkSemaphoreTypeCreateInfo tsci = {};
            tsci.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
            tsci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            tsci.initialValue  = 0;
            VkSemaphoreCreateInfo tsem = {};
            tsem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            tsem.pNext = &tsci;
            if (pfnCreateSem(m_Device, &tsem, nullptr, &m_CopyDoneSem) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10f Path D timeline sem create failed — fallback");
                m_CopyDoneSem = VK_NULL_HANDLE;
            }
            m_CopyDoneNext.store(1);
            const bool ok = (m_CopyDoneSem != VK_NULL_HANDLE);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.10f Path D early-release "
                        "two-submit infra %s (sem=%p, %u copy cmd buffers)",
                        ok ? "READY" : "PARTIAL — fallback",
                        (void*)m_CopyDoneSem, (unsigned)kFrucFramesInFlight);
        }
    }

    // §J.3.e.2.i.10 Phase 2B step 2 (v1.4.55) — alloc per-slot pre/post
    // graphics-queue cmd buffers from m_CmdPool.  These carry the
    // bilinear-DOWN (pre) and warp/UP/render-passes/present (post) halves
    // of the 3-submit chain landed in v1.4.56+.  Allocation is gated on
    // m_AsyncComputeAvailable so single-QF GPUs / Phase 2A failure don't
    // burn 2 × N cmd buffers worth of GPU memory for an inactive path.
    if (m_AsyncComputeAvailable) {
        VkCommandBufferAllocateInfo preCbai = {};
        preCbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        preCbai.commandPool        = m_CmdPool;
        preCbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        preCbai.commandBufferCount = kFrucFramesInFlight;
        if (pfnAllocCmdBufs(m_Device, &preCbai, m_SlotPreCmdBuf) != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B pre-cmd alloc "
                        "failed — async-compute path will fall back to single-submit");
            for (uint32_t i = 0; i < kFrucFramesInFlight; i++) m_SlotPreCmdBuf[i] = VK_NULL_HANDLE;
            m_AsyncComputeAvailable = false;  // demote: pre/post mandatory for async path
        } else {
            VkCommandBufferAllocateInfo postCbai = preCbai;
            if (pfnAllocCmdBufs(m_Device, &postCbai, m_SlotPostCmdBuf) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B post-cmd alloc "
                            "failed — async-compute path will fall back to single-submit");
                for (uint32_t i = 0; i < kFrucFramesInFlight; i++) m_SlotPostCmdBuf[i] = VK_NULL_HANDLE;
                m_AsyncComputeAvailable = false;
            } else {
                m_AsyncComputeNext.store(1);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B step 2 pre/post "
                            "cmd buffers READY (%u pre + %u post on graphics QF, "
                            "compute timeline V starts at 1)",
                            (unsigned)kFrucFramesInFlight, (unsigned)kFrucFramesInFlight);
            }
        }
    }

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
        for (int p = 0; p < 3; p++) {
            if (m_SlotAcquireSem[i][p] && pfnDestroySem) {
                pfnDestroySem(m_Device, m_SlotAcquireSem[i][p], nullptr);
                m_SlotAcquireSem[i][p] = VK_NULL_HANDLE;
            }
        }
        for (int p = 0; p < 2; p++) {
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

    // §J.3.e.2.i.10 Phase 2A — async-compute pool + timeline sem teardown.
    // Cmd buffers are freed implicitly when the pool is destroyed.
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        m_ComputeCmdBuf[i] = VK_NULL_HANDLE;
    }
    if (m_ComputeCmdPool && pfnDestroyCmdPool) {
        pfnDestroyCmdPool(m_Device, m_ComputeCmdPool, nullptr);
        m_ComputeCmdPool = VK_NULL_HANDLE;
    }
    if (m_ComputeTimelineSem && pfnDestroySem) {
        pfnDestroySem(m_Device, m_ComputeTimelineSem, nullptr);
        m_ComputeTimelineSem = VK_NULL_HANDLE;
    }
    m_ComputeTimelineValue = 0;

    // §J.3.e.2.i.10f Path D — copy cmd buffers (allocated from m_CmdPool,
    // freed implicitly above when m_CmdPool destroyed) + timeline sem.
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        m_SlotCopyCmdBuf[i] = VK_NULL_HANDLE;
    }
    if (m_CopyDoneSem && pfnDestroySem) {
        pfnDestroySem(m_Device, m_CopyDoneSem, nullptr);
        m_CopyDoneSem = VK_NULL_HANDLE;
    }
    m_CopyDoneNext.store(1);

    // §J.3.e.2.i.10 Phase 2B step 2 (v1.4.55) — pre/post cmd buffers
    // (allocated from m_CmdPool, freed implicitly when pool destroyed
    // above; we just null the handles so re-init doesn't see stale ones).
    for (uint32_t i = 0; i < kFrucFramesInFlight; i++) {
        m_SlotPreCmdBuf[i]  = VK_NULL_HANDLE;
        m_SlotPostCmdBuf[i] = VK_NULL_HANDLE;
    }
    m_AsyncComputeNext.store(1);

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
    // §J.3.e.2.i.16 (v1.4.81) — inject wrappedGetInstanceProcAddr to serialise
    // FFmpeg 7.x's vkQueueSubmit2 against our render-thread submits.
    // s_RealGetInstanceProcAddr cached BEFORE assigning the wrapper so it
    // can fall through to the real loader.
    s_RealGetInstanceProcAddr = m_pfnGetInstanceProcAddr;
    vkCtx->get_proc_addr = wrappedGetInstanceProcAddr;
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
#elif defined(Q_OS_LINUX)
        // String literals avoid dragging <vulkan/vulkan_{xlib,xcb,wayland}.h>
        // (which pull in X11 / XCB / Wayland headers).  These names are part
        // of the Vulkan spec and stable; SDL_Vulkan_GetInstanceExtensions
        // already enables the right one at instance creation, this list just
        // tells ffmpeg's hwcontext_vulkan dispatch table what we have.
        "VK_KHR_xlib_surface",
        "VK_KHR_xcb_surface",
        "VK_KHR_wayland_surface",
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

    // §J.3.e.2.i.8 Phase 2.5 / §B1a 2026-05-06 — m_SwFrucNv12Buf creation
    // moved to createFrucComputeResources so HW path also gets the mirror
    // buffer (HW renderFrame copies AVVkFrame.img[0] → m_SwFrucNv12Buf for
    // FRUC compute consumption, mirroring SW path's m_SwUploadImage source).

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
    // §B1a 2026-05-06 — m_SwFrucNv12Buf release moved to
    // destroyFrucComputeResources (FRUC-owned across SW + HW path).
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
extern const char* kFrucMotionEstBackwardShaderGlsl;     // §J.3.e.2.i.17 v1.4.82
extern const char* kFrucMvUncertaintyFlagShaderGlsl;     // §J.3.e.2.i.19 v1.4.83
extern const char* kFrucMotionEstFine4x4ShaderGlsl;      // §J.3.e.2.i.19 v1.4.83
extern const char* kFrucRgbDownscaleHalfShaderGlsl;      // §J.3.e.2.i.23 v1.4.85
extern const char* kFrucMotionEstHalfResShaderGlsl;      // §J.3.e.2.i.23 v1.4.85
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

// §B-NVOF Phase 5 2026-05-06 — SFIXED5 (Q10.5 fixed-point) → Q1 (×2 of integer
// pixel) format converter.  Reads m_NvOfFlowStaging (storage buffer copy of
// m_NvOfFlowImage VK_FORMAT_R16G16_S10_5_NV), 4 bytes/cell = packed int16
// pair (low=x, high=y) in S10.5 fixed point.  SFIXED5 V represents pixel
// V/32; Q1 = round(pixel * 2) = round(V/16).  Output written to existing
// m_FrucMvFilteredBuf so downstream warp shader consumes unchanged.
//
// §B-NVOF Phase 7D 2026-05-06 — dynamic scale (flowW/mvW × flowH/mvH).
// grid=4 → 2x2 average (4 reads); grid=2 → 4x4 (16 reads); grid=1 → 8x8
// (64 reads). Loop runs at variable bounds determined by push constants.
static const char* kVkFrucNvOfConvertShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// flowIn.data[idx] = packed int16x2 (low 16 bit = x, high 16 bit = y), each
// component is signed Q10.5 fixed-point flow vector.
layout(binding = 0, std430) readonly buffer FlowIn {
    int data[];
} flowIn;

// mvOut.data is pairs of int (Q1 x, Q1 y), matches m_FrucMvFilteredBuf.
layout(binding = 1, std430) writeonly buffer MvOut {
    int data[];
} mvOut;

layout(push_constant) uniform PC {
    uint flowW;
    uint flowH;
    uint mvW;
    uint mvH;
} p;

ivec2 readFlow(uint x, uint y) {
    if (x >= p.flowW || y >= p.flowH) return ivec2(0);
    int packed = flowIn.data[y * p.flowW + x];
    int rawX = (packed << 16) >> 16;
    int rawY = packed >> 16;
    return ivec2(rawX, rawY);
}

void main() {
    uint mvX = gl_GlobalInvocationID.x;
    uint mvY = gl_GlobalInvocationID.y;
    if (mvX >= p.mvW || mvY >= p.mvH) return;

    // §B-NVOF A'.2 2026-05-07 — consensus-max instead of plain average.
    //
    // Why: testufo + most game content has small fast-moving subjects (UFO,
    // mouse cursor, particle, projectile) on mostly-static background.  An
    // 8-px MV cell covers up to 64 source pixels = 16 NVOF flow cells (at
    // grid=2).  When the subject occupies only 1-3 of those 16 cells, plain
    // averaging dilutes its motion by 5-15× toward 0:
    //
    //   1 cell  with flow = 60 SFIXED5  (real UFO motion ~ 30 pixels)
    //   15 cells with flow = 0          (static stars)
    //   →  average = 60/16 = 3.75       (rounds to Q1=0 → cross-fade)
    //
    // The §B-DUMP-MV log on testufo confirmed this: 85% MV=0 + max only 5
    // LSB Q1 across full 240×135 grid even though UFO moves 30+ px/frame.
    //
    // Fix:
    //   1. Find max-magnitude flow cell (the strongest signal in the block)
    //   2. Apply noise floor: ignore if max² < 16 (= < 0.125 px).  NVOF gives
    //      random spikes of 4-8 LSB in low-contrast / repetitive textures
    //      (star fields, gradient skies); these are noise, not real motion.
    //   3. Consensus: count cells with mag² >= 0.25 * maxMag² (within half
    //      magnitude of max).  ≥2 in agreement = real motion → average them
    //      (smooths sub-pixel jitter); <2 = lone spike → use max as-is but
    //      we already passed noise floor so likely UFO partially in cell.
    //
    // Trade-off vs plain max: consensus check rejects isolated noise spikes
    // (lone cell with random 4-8 LSB on static background) but preserves
    // genuine motion (multiple cells in similar direction on subject).

    uint scaleX = p.flowW / p.mvW;
    uint scaleY = p.flowH / p.mvH;
    uint count  = scaleX * scaleY;
    uint fxBase = mvX * scaleX;
    uint fyBase = mvY * scaleY;

    // Read all flow cells + compute magnitudes.
    // Buffer with worst-case grid=1 (64 cells) static array bounded.
    const uint MAX_CELLS = 64u;
    ivec2 cells[MAX_CELLS];
    int   mag2s[MAX_CELLS];
    uint  n = 0u;
    int   maxMag2 = 0;
    ivec2 maxFlow = ivec2(0);
    for (uint dy = 0u; dy < scaleY; dy++) {
        for (uint dx = 0u; dx < scaleX; dx++) {
            ivec2 v = readFlow(fxBase + dx, fyBase + dy);
            int m2 = v.x * v.x + v.y * v.y;
            if (n < MAX_CELLS) {
                cells[n] = v;
                mag2s[n] = m2;
                n++;
            }
            if (m2 > maxMag2) {
                maxMag2 = m2;
                maxFlow = v;
            }
        }
    }

    // Noise floor: SFIXED5 has 1/32 px resolution. mag² < 16 = magnitude < 4
    // SFIXED5 = < 0.125 px.  Below this, treat as static (noise).
    const int NOISE_FLOOR_MAG2 = 16;
    if (maxMag2 < NOISE_FLOOR_MAG2) {
        uint idx0 = (mvY * p.mvW + mvX) * 2u;
        mvOut.data[idx0]      = 0;
        mvOut.data[idx0 + 1u] = 0;
        return;
    }

    // Consensus: gather cells with mag² >= maxMag² / 4 (within ±50% magnitude).
    // Multiply threshold instead of float division for integer safety.
    int threshScaled = maxMag2;  // compare 4 * mag² >= maxMag²
    ivec2 consensusSum = ivec2(0);
    int   consensusN   = 0;
    for (uint i = 0u; i < n; i++) {
        if (4 * mag2s[i] >= threshScaled) {
            consensusSum += cells[i];
            consensusN++;
        }
    }

    // ≥2 cells in consensus = real subject motion (average them for sub-pixel
    // smoothness).  Lone cell above noise floor = use max as-is (likely a
    // UFO/cursor partially overlapping a single MV cell — better to keep the
    // motion than zero it).
    ivec2 chosen = (consensusN >= 2)
                 ? (consensusSum / consensusN)
                 : maxFlow;

    // SFIXED5 (1/32 px) → Q1 (1/2 px).  Divide by 16, signed round-toward-0.
    // §B-NVOF Phase 7A reverted Q5 attempt: per-cell Q5 was noisier than Q1
    // averaging-then-quantising.  Now with consensus-max upstream, Q1 still
    // adequate (warp threshold = 1 px = 2 LSB Q1).  Q5 sample-direct path
    // remains a candidate if A'.2 still has dilution issues.
    ivec2 q1 = chosen / 16;

    uint outIdx = (mvY * p.mvW + mvX) * 2u;
    mvOut.data[outIdx]      = q1.x;
    mvOut.data[outIdx + 1u] = q1.y;
}
)GLSL";

// §J.3.e.X Path β.5 — bilinear up shader for flow tensors with magnitude
// scaling.  RIFE-v4-lite emits flow values in pixels at the model's spatial
// dim (e.g. 256×128); to use them at source dim (e.g. 1920×1080) the values
// themselves must be scaled by srcW/inferW (x channels) or srcH/inferH (y).
// Channel layout: ch 0 = flow_prev.x, ch 1 = flow_prev.y, ch 2 = flow_curr.x,
// ch 3 = flow_curr.y.  Even channels (0/2) get magScaleW; odd (1/3) magScaleH.
// Mask (1ch) doesn't use this shader; it uses the regular bilinear pipeline
// which has scale = 1.0 effectively.
static const char* kRifeFlowBilinearShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set=0, binding=0) readonly  buffer InBuf  { float in_buf[];  };
layout(set=0, binding=1) writeonly buffer OutBuf { float out_buf[]; };
layout(push_constant) uniform PC {
    int   inH;
    int   inW;
    int   outH;
    int   outW;
    int   channels;
    float scaleH;     // input H / output H (bilinear sample positioning)
    float scaleW;     // input W / output W
    float magScaleX;  // = output W / input W (flow x magnitude scaling)
    float magScaleY;  // = output H / input H (flow y magnitude scaling)
} pc;
void main() {
    int ow = int(gl_GlobalInvocationID.x);
    int oh = int(gl_GlobalInvocationID.y);
    int c  = int(gl_GlobalInvocationID.z);
    if (ow >= pc.outW || oh >= pc.outH || c >= pc.channels) return;

    float inh_f = (float(oh) + 0.5) * pc.scaleH - 0.5;
    float inw_f = (float(ow) + 0.5) * pc.scaleW - 0.5;
    int ih0 = int(floor(inh_f)); int iw0 = int(floor(inw_f));
    float fh = inh_f - float(ih0); float fw = inw_f - float(iw0);
    int ih1 = ih0 + 1; int iw1 = iw0 + 1;
    ih0 = clamp(ih0, 0, pc.inH - 1);
    ih1 = clamp(ih1, 0, pc.inH - 1);
    iw0 = clamp(iw0, 0, pc.inW - 1);
    iw1 = clamp(iw1, 0, pc.inW - 1);

    int planeBase = c * pc.inH * pc.inW;
    float v00 = in_buf[planeBase + ih0 * pc.inW + iw0];
    float v01 = in_buf[planeBase + ih0 * pc.inW + iw1];
    float v10 = in_buf[planeBase + ih1 * pc.inW + iw0];
    float v11 = in_buf[planeBase + ih1 * pc.inW + iw1];
    float v0  = v00 * (1.0 - fw) + v01 * fw;
    float v1  = v10 * (1.0 - fw) + v11 * fw;
    float v   = v0  * (1.0 - fh) + v1  * fh;

    // Magnitude scale: even ch = x flow → magScaleX, odd ch = y flow → magScaleY.
    if ((c & 1) == 0) v *= pc.magScaleX;
    else              v *= pc.magScaleY;

    out_buf[(c * pc.outH + oh) * pc.outW + ow] = v;
}
)GLSL";

// §J.3.e.X Path β.5.1 — native-res warp+blend shader, samples flow + mask
// at INFER DIM (no bilinear UP needed).  Saves 41 MB/frame allocation +
// write bandwidth + 2 dispatches vs β.5.0.
//
// Reads:
//   prev_1080p    (CHW fp32, 3 channels, source dim)
//   curr_1080p    (CHW fp32, 3 channels, source dim)
//   flow_inferDim (4 channels = prev_x, prev_y, curr_x, curr_y at infer dim)
//   mask_inferDim (1 channel at infer dim)
// Writes:
//   interp_out (CHW fp32, 3 channels, source dim)
//
// Per output pixel (x, y) at source dim:
//   1. Compute infer-dim coordinates (xi, yi) = (x * inferW/srcW, y * inferH/srcH)
//   2. Bilinear-sample flow at (xi, yi) → (dxP, dyP, dxC, dyC) at infer-dim units
//   3. Scale flow by magScale = srcDim/inferDim → motion vectors at source-dim
//   4. Bilinear-sample mask at (xi, yi) → m
//   5. prev_warped = sample prev at (x + dxP * magScale, y + dyP * magScale)
//   6. curr_warped = sample curr at (x + dxC * magScale, y + dyC * magScale)
//   7. out = prev_warped * m + curr_warped * (1 - m)
static const char* kRifeNativeWarpShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set=0, binding=0) readonly  buffer PrevBuf { float prev_buf[]; };
layout(set=0, binding=1) readonly  buffer CurrBuf { float curr_buf[]; };
layout(set=0, binding=2) readonly  buffer FlowBuf { float flow_buf[]; };  // 4ch CHW @ infer dim
layout(set=0, binding=3) readonly  buffer MaskBuf { float mask_buf[]; };  // 1ch HW @ infer dim
layout(set=0, binding=4) writeonly buffer OutBuf  { float out_buf[];  };
layout(push_constant) uniform PC {
    int   W;       // source width
    int   H;       // source height
    int   inferW;  // flow + mask width (infer dim)
    int   inferH;  // flow + mask height
    float magScaleX;  // = float(W) / float(inferW)  — flow magnitude scaling
    float magScaleY;  // = float(H) / float(inferH)
    int   _pad0;
    int   _pad1;
} pc;

// Bilinear sample helper for source-dim buffers (used by prev/curr)
float sampleSrcPrev(int channelBase, float x, float y) {
    int x0 = int(floor(x)); int y0 = int(floor(y));
    int x1 = x0 + 1; int y1 = y0 + 1;
    float fx = x - float(x0); float fy = y - float(y0);
    x0 = clamp(x0, 0, pc.W - 1); x1 = clamp(x1, 0, pc.W - 1);
    y0 = clamp(y0, 0, pc.H - 1); y1 = clamp(y1, 0, pc.H - 1);
    float v00 = prev_buf[channelBase + y0 * pc.W + x0];
    float v01 = prev_buf[channelBase + y0 * pc.W + x1];
    float v10 = prev_buf[channelBase + y1 * pc.W + x0];
    float v11 = prev_buf[channelBase + y1 * pc.W + x1];
    float v0 = v00 * (1.0 - fx) + v01 * fx;
    float v1 = v10 * (1.0 - fx) + v11 * fx;
    return v0 * (1.0 - fy) + v1 * fy;
}
float sampleSrcCurr(int channelBase, float x, float y) {
    int x0 = int(floor(x)); int y0 = int(floor(y));
    int x1 = x0 + 1; int y1 = y0 + 1;
    float fx = x - float(x0); float fy = y - float(y0);
    x0 = clamp(x0, 0, pc.W - 1); x1 = clamp(x1, 0, pc.W - 1);
    y0 = clamp(y0, 0, pc.H - 1); y1 = clamp(y1, 0, pc.H - 1);
    float v00 = curr_buf[channelBase + y0 * pc.W + x0];
    float v01 = curr_buf[channelBase + y0 * pc.W + x1];
    float v10 = curr_buf[channelBase + y1 * pc.W + x0];
    float v11 = curr_buf[channelBase + y1 * pc.W + x1];
    float v0 = v00 * (1.0 - fx) + v01 * fx;
    float v1 = v10 * (1.0 - fx) + v11 * fx;
    return v0 * (1.0 - fy) + v1 * fy;
}
// §J.3.e.X Path β.5.2 — Catmull-Rom bicubic upsampling for infer-dim
// flow + mask (256x128 -> 1080p, 7.5x ratio).  Bilinear was visibly
// blurring high-frequency content vs sharp real frame; bicubic 4x4
// kernel preserves edges much better.  ~16 buffer reads + 20 ALU ops
// per sample (vs bilinear 4 reads + 3 lerps), warp shader cost goes
// from ~0.4ms to ~0.8ms on RTX 3060 — still well within 60fps DUAL
// 16.7ms budget.  Catmull-Rom alpha=-0.5 (standard).
//
// sampleSrcPrev / sampleSrcCurr stay bilinear because those are source-
// dim sub-pixel samples (not upsampling), where bilinear's smoothness
// is correct (warp displacement isn't an edge-preserving operation).
//
// Mask is sigmoid output [0,1].  Catmull-Rom can overshoot at sharp
// transitions; clamp to [0,1] at end of sampleMask.

// Catmull-Rom weights for fractional offset fx in [0, 1).  Returns
// weights for samples at integer offsets {-1, 0, +1, +2} relative to
// floor(x).  alpha=-0.5 form:
//   d in [0,1]: 1.5*d^3 - 2.5*d^2 + 1
//   d in [1,2]: -0.5*d^3 + 2.5*d^2 - 4*d + 2
vec4 catmullRomWeights(float fx) {
    float d0 = 1.0 + fx;       // |sample(-1) - x| in [1, 2]
    float d1 = fx;             // |sample( 0) - x| in [0, 1]
    float d2 = 1.0 - fx;       // |sample(+1) - x| in [0, 1]
    float d3 = 2.0 - fx;       // |sample(+2) - x| in [1, 2]
    float w0 = -0.5 * d0*d0*d0 + 2.5 * d0*d0 - 4.0 * d0 + 2.0;
    float w1 =  1.5 * d1*d1*d1 - 2.5 * d1*d1 + 1.0;
    float w2 =  1.5 * d2*d2*d2 - 2.5 * d2*d2 + 1.0;
    float w3 = -0.5 * d3*d3*d3 + 2.5 * d3*d3 - 4.0 * d3 + 2.0;
    return vec4(w0, w1, w2, w3);
}

// Bicubic sample helper for infer-dim flow buffer (4 channels packed CHW)
float sampleFlowChan(int chanIdx, float x, float y) {
    int xi = int(floor(x));
    int yi = int(floor(y));
    float fx = x - float(xi);
    float fy = y - float(yi);
    vec4 wx = catmullRomWeights(fx);
    vec4 wy = catmullRomWeights(fy);
    int base = chanIdx * pc.inferH * pc.inferW;
    int cx0 = clamp(xi - 1, 0, pc.inferW - 1);
    int cx1 = clamp(xi    , 0, pc.inferW - 1);
    int cx2 = clamp(xi + 1, 0, pc.inferW - 1);
    int cx3 = clamp(xi + 2, 0, pc.inferW - 1);
    float row[4];
    for (int dy = 0; dy < 4; ++dy) {
        int yc = clamp(yi - 1 + dy, 0, pc.inferH - 1);
        int rowBase = base + yc * pc.inferW;
        float v0 = flow_buf[rowBase + cx0];
        float v1 = flow_buf[rowBase + cx1];
        float v2 = flow_buf[rowBase + cx2];
        float v3 = flow_buf[rowBase + cx3];
        row[dy] = v0 * wx.x + v1 * wx.y + v2 * wx.z + v3 * wx.w;
    }
    return row[0] * wy.x + row[1] * wy.y + row[2] * wy.z + row[3] * wy.w;
}
// Bicubic sample for infer-dim mask (1 channel HW); clamps to [0,1]
// because Catmull-Rom can overshoot at sigmoid sharp transitions.
float sampleMask(float x, float y) {
    int xi = int(floor(x));
    int yi = int(floor(y));
    float fx = x - float(xi);
    float fy = y - float(yi);
    vec4 wx = catmullRomWeights(fx);
    vec4 wy = catmullRomWeights(fy);
    int cx0 = clamp(xi - 1, 0, pc.inferW - 1);
    int cx1 = clamp(xi    , 0, pc.inferW - 1);
    int cx2 = clamp(xi + 1, 0, pc.inferW - 1);
    int cx3 = clamp(xi + 2, 0, pc.inferW - 1);
    float row[4];
    for (int dy = 0; dy < 4; ++dy) {
        int yc = clamp(yi - 1 + dy, 0, pc.inferH - 1);
        int rowBase = yc * pc.inferW;
        float v0 = mask_buf[rowBase + cx0];
        float v1 = mask_buf[rowBase + cx1];
        float v2 = mask_buf[rowBase + cx2];
        float v3 = mask_buf[rowBase + cx3];
        row[dy] = v0 * wx.x + v1 * wx.y + v2 * wx.z + v3 * wx.w;
    }
    float r = row[0] * wy.x + row[1] * wy.y + row[2] * wy.z + row[3] * wy.w;
    return clamp(r, 0.0, 1.0);
}

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    int c = int(gl_GlobalInvocationID.z);
    if (x >= pc.W || y >= pc.H || c >= 3) return;

    // Map source pixel → infer-dim sample position (centered).
    // Use center-aligned convention: out pixel (x, y) center at (x+0.5, y+0.5)
    // in source domain, that maps to infer-dim center at scale ratio.
    float xi = (float(x) + 0.5) * float(pc.inferW) / float(pc.W) - 0.5;
    float yi = (float(y) + 0.5) * float(pc.inferH) / float(pc.H) - 0.5;

    // Sample flow + mask at infer dim, scale flow magnitude to source-dim units.
    float dxP = sampleFlowChan(0, xi, yi) * pc.magScaleX;
    float dyP = sampleFlowChan(1, xi, yi) * pc.magScaleY;
    float dxC = sampleFlowChan(2, xi, yi) * pc.magScaleX;
    float dyC = sampleFlowChan(3, xi, yi) * pc.magScaleY;
    float m   = sampleMask(xi, yi);

    int channelBase = c * pc.H * pc.W;
    float pSamp = sampleSrcPrev(channelBase, float(x) + dxP, float(y) + dyP);
    float cSamp = sampleSrcCurr(channelBase, float(x) + dxC, float(y) + dyC);

    out_buf[channelBase + y * pc.W + x] = pSamp * m + cSamp * (1.0 - m);
}
)GLSL";

#include <ncnn/gpu.h>  // for ncnn::compile_spirv_module + ncnn::Option

bool VkFrucRenderer::createFrucComputeResources(int width, int height)
{
    if (m_FrucReady || m_FrucDisabled) return m_FrucReady;

    const uint32_t BLOCK_SIZE = 8;  // §B-quality (b) reverted — block 16 在純水平 motion 引入強烈 Y 軸抖動
    const uint32_t mvW = ((uint32_t)width  + BLOCK_SIZE - 1) / BLOCK_SIZE;
    const uint32_t mvH = ((uint32_t)height + BLOCK_SIZE - 1) / BLOCK_SIZE;
    m_FrucMvWidth  = mvW;
    m_FrucMvHeight = mvH;
    m_FrucSrcWidth  = (uint32_t)width;
    m_FrucSrcHeight = (uint32_t)height;

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

    // §J.3.e.2.i.23 (v1.4.85) Pyramid — ME bumped 4→5 bindings (binding 4 =
    // half-res MV), push const 24→28 byte (added uint hasHalfMvPredictor).
    if (!buildPipeline("ME", kFrucMotionEstShaderGlsl, 5, 28,
                        m_FrucMeShaderMod, m_FrucMeDsl, m_FrucMePipeLay, m_FrucMePipeline)) {
        m_FrucDisabled = true; return false;
    }
    if (!buildPipeline("Median", kFrucMvMedianShaderGlsl, 2, 16,
                        m_FrucMedianShaderMod, m_FrucMedianDsl, m_FrucMedianPipeLay, m_FrucMedianPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.17 (v1.4.82) — Backward ME pipeline (bidirectional Phase A).
    // 4 bindings: prevRGB, currRGB, fwdMV (predictor), outBwdMV. Push const
    // 24 byte (same struct as forward ME).
    if (!buildPipeline("MEBackward", kFrucMotionEstBackwardShaderGlsl, 4, 24,
                        m_FrucMeBackwardShaderMod, m_FrucMeBackwardDsl,
                        m_FrucMeBackwardPipeLay, m_FrucMeBackwardPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.23 (v1.4.85) Pyramid — RGB ½ downscale (2 bind, 16 byte pc).
    if (!buildPipeline("DownscaleHalf", kFrucRgbDownscaleHalfShaderGlsl, 2, 16,
                        m_FrucDownscaleHalfShaderMod, m_FrucDownscaleHalfDsl,
                        m_FrucDownscaleHalfPipeLay, m_FrucDownscaleHalfPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid — ½/¼ ME 共用同 pipeline
    // (4 bind: prevRGB, currRGB, coarserMV, outMV; 28 byte pc with hasCoarserMvPredictor).
    if (!buildPipeline("MEHalf", kFrucMotionEstHalfResShaderGlsl, 4, 28,
                        m_FrucMeHalfShaderMod, m_FrucMeHalfDsl,
                        m_FrucMeHalfPipeLay, m_FrucMeHalfPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.19 (v1.4.83) Phase B — Uncertainty flag pre-pass (3 bind, 16 byte pc).
    if (!buildPipeline("Flag", kFrucMvUncertaintyFlagShaderGlsl, 3, 16,
                        m_FrucFlagShaderMod, m_FrucFlagDsl,
                        m_FrucFlagPipeLay, m_FrucFlagPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.19 (v1.4.83) Phase B — 4×4 fine ME (5 bind, 24 byte pc).
    if (!buildPipeline("Fine4x4", kFrucMotionEstFine4x4ShaderGlsl, 5, 24,
                        m_FrucFine4x4ShaderMod, m_FrucFine4x4Dsl,
                        m_FrucFine4x4PipeLay, m_FrucFine4x4Pipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §J.3.e.2.i.19 (v1.4.83) Phase B — Warp bumped 6→8 bindings (binding 6 =
    // fineMV, binding 7 = flag buf). Push const 36→40 byte (added uint hasFineMv).
    if (!buildPipeline("Warp", kFrucWarpShaderGlsl, 8, 40,
                        m_FrucWarpShaderMod, m_FrucWarpDsl, m_FrucWarpPipeLay, m_FrucWarpPipeline)) {
        m_FrucDisabled = true; return false;
    }
    // §B-NVOF Phase 5 — converter pipeline (only if NV OF wanted; cheap to
    // always build when env var enabled even if OF session creation fails
    // later).  4 push consts (uint flowW/H + uint mvW/H = 16 B), 2 storage
    // buffer bindings (flow staging input + mv filtered output).
    if (vkfrucWantNvOfFromUserOrEnv()) {
        if (!buildPipeline("NvOFConvert", kVkFrucNvOfConvertShaderGlsl, 2, 16,
                           m_FrucNvOfConvertShaderMod, m_FrucNvOfConvertDsl,
                           m_FrucNvOfConvertPipeLay, m_FrucNvOfConvertPipeline)) {
            // Non-fatal: just disable OF chain consumption.  Caller falls back
            // to block-matching ME (m_FrucMePipeline already built above).
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] NvOFConvert pipeline build failed — "
                        "OF will still execute but result not consumed");
        }
    }

    // §J.3.e.X Path β.4 — bilinear scale pipeline (down: source RGB →
    // RIFE infer dim, up: RIFE output → m_FrucInterpRgbBuf).  Push consts
    // = 28 bytes (5 ints + 2 floats) rounded to 32 for alignment, 2
    // storage buffer bindings (in / out).  Shader text comes from
    // rife_native_vk.cpp's getInterpBilinearShaderGlsl().
    // §J.3.e.Y 5Y v1.4.63 — shader carries BLOB_T/R/W markers since
    // v1.4.60; must wrap through applyBlobMacros before glslang.  Caller
    // side (vkfruc) buffers are always fp32 (m_RifeDownPrev/Curr/Interp,
    // m_FrucInterpRgbBuf) so pass useFp16Blob=false unconditionally; the
    // fp32↔fp16 conversion lives inside the RIFE module's
    // runInferenceGpuFlow boundary, not here.
    if (m_RifeNativeMode) {
        const std::string bilinearSrc = viple::rife_native_vk::applyBlobMacros(
            viple::rife_native_vk::getInterpBilinearShaderGlsl(),
            /*useFp16Blob*/ false);
        if (!buildPipeline("RifeBilinear",
                           bilinearSrc.c_str(),
                           2, 32,
                           m_RifeBilinearShaderMod, m_RifeBilinearDsl,
                           m_RifeBilinearPipeLay, m_RifeBilinearPipeline)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β] bilinear pipeline build failed — "
                "RIFE init will fall back");
            // Don't disable FRUC overall.  m_RifeNativeMode will be
            // cleared in createRifeNativeResources when this pipeline
            // isn't ready.
        }

        // β.5: extra pipelines for flow extraction path.
        // Flow bilinear up shader: 2 bindings, push const = 36 bytes (5 int + 4 float)
        if (!buildPipeline("RifeFlowBilinear",
                           kRifeFlowBilinearShaderGlsl,
                           2, 36,
                           m_RifeFlowBilinearShaderMod, m_RifeFlowBilinearDsl,
                           m_RifeFlowBilinearPipeLay, m_RifeFlowBilinearPipeline)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β5] flow bilinear pipeline build failed — "
                "β.5 flow path will fall back to β.4 bilinear-up RGB");
        }
        // Native warp shader: 5 bindings, push const = 32 bytes
        // (W, H, inferW, inferH ints + magScaleX, magScaleY floats + 2 pad ints).
        if (!buildPipeline("RifeNativeWarp",
                           kRifeNativeWarpShaderGlsl,
                           5, 32,
                           m_RifeNativeWarpShaderMod, m_RifeNativeWarpDsl,
                           m_RifeNativeWarpPipeLay, m_RifeNativeWarpPipeline)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β5] native warp pipeline build failed — "
                "β.5 will fall back to β.4");
        }
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
    // §J.3.e.2.i.10 Phase 2B step 4 (v1.4.55) — same CONCURRENT-sharing
    // gate as createRifeNativeResources::allocBuf: cross-queue buffers
    // span {graphics QF, compute QF} when async-compute infra is live.
    // m_FrucInterpRgbBuf / m_FrucInterpRgbBuf2 are written by compute-
    // queue warp dispatches and read by graphics-queue render-passes,
    // so they need CONCURRENT just like the RIFE down/flow/mask
    // buffers.  m_FrucPrevRgbBuf / m_FrucCurrRgbBuf / m_FrucMvBuf* stay
    // single-queue (graphics-only nv12rgb + ME + median chain).
    const bool sharingAcrossQfs2 = m_AsyncComputeAvailable
                                && m_QueueFamily != UINT32_MAX
                                && m_ComputeQueueFamily != UINT32_MAX
                                && m_QueueFamily != m_ComputeQueueFamily;
    const uint32_t crossQfs2[2] = { m_QueueFamily, m_ComputeQueueFamily };
    auto allocBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                         VkBuffer& outBuf, VkDeviceMemory& outMem,
                         bool crossQueue = false) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        if (crossQueue && sharingAcrossQfs2) {
            bci.sharingMode           = VK_SHARING_MODE_CONCURRENT;
            bci.queueFamilyIndexCount = 2;
            bci.pQueueFamilyIndices   = crossQfs2;
        } else {
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
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
    const VkDeviceSize sizeRGB  = (VkDeviceSize)width * height * 3 * sizeof(float);
    const VkDeviceSize sizeMV   = (VkDeviceSize)mvW * mvH * 2 * sizeof(int);
    // §B1a 2026-05-06 — NV12 mirror buffer (was created in
    // createSwUploadResources / m_SwStagingSize).  Same size formula —
    // width*height*1.5 (Y plane W*H + UV plane W*H/2) × kFrucFramesInFlight
    // for back-to-back per-slot regions.  HW path also needs this buffer:
    // renderFrame copies AVVkFrame.img[0] (NV12 multi-plane VkImage) to
    // this buffer via vkCmdCopyImageToBuffer for FRUC NV12→RGB compute
    // consumption.  SW path keeps populating it via cmdCopyImageToBuffer
    // from m_SwUploadImage, identical wire format.
    const VkDeviceSize sizeNV12 = (VkDeviceSize)width * height * 3 / 2
                                  * kFrucFramesInFlight;

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
        // §J.3.e.2.i.15 (v1.4.80) — TRANSFER_SRC added: post-frame copy 此
        // buffer 到 m_FrucPrevMvBuf 給下一幀 temporal coherence proxy 用.
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
        // §J.3.e.2.i.17 (v1.4.82) — Backward MV buffers (bidirectional Phase A).
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvBackwardBuf, m_FrucMvBackwardBufMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvBackwardFilteredBuf, m_FrucMvBackwardFilteredMem)
        // §J.3.e.2.i.19 (v1.4.83) Phase B — uncertainty flag buf (1 uint per
        // 8×8 block) + fine MV buf (4× sizeMV at fine 4×4 resolution).
        || !allocBuf((VkDeviceSize)mvW * mvH * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucRefineFlagBuf, m_FrucRefineFlagBufMem)
        || !allocBuf((VkDeviceSize)mvW * 2 * mvH * 2 * 2 * sizeof(int),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvFineBuf, m_FrucMvFineBufMem)
        // §J.3.e.2.i.23 (v1.4.85) Pyramid — ½ res RGB buffers (¼ size of full
        // res RGB each) + ½ res MV buffer (¼ size of full res MV).
        || !allocBuf((VkDeviceSize)(width / 2) * (height / 2) * 3 * sizeof(float),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucPrevRgbHalfBuf, m_FrucPrevRgbHalfBufMem)
        || !allocBuf((VkDeviceSize)(width / 2) * (height / 2) * 3 * sizeof(float),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucCurrRgbHalfBuf, m_FrucCurrRgbHalfBufMem)
        || !allocBuf((VkDeviceSize)(mvW / 2) * (mvH / 2) * 2 * sizeof(int),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvHalfBuf, m_FrucMvHalfBufMem)
        // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid — ¼ res RGB + MV (1/16 size).
        || !allocBuf((VkDeviceSize)(width / 4) * (height / 4) * 3 * sizeof(float),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucPrevRgbQuarterBuf, m_FrucPrevRgbQuarterBufMem)
        || !allocBuf((VkDeviceSize)(width / 4) * (height / 4) * 3 * sizeof(float),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucCurrRgbQuarterBuf, m_FrucCurrRgbQuarterBufMem)
        || !allocBuf((VkDeviceSize)(mvW / 4) * (mvH / 4) * 2 * sizeof(int),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucMvQuarterBuf, m_FrucMvQuarterBufMem)
        || !allocBuf(sizeMV, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_FrucPrevMvBuf, m_FrucPrevMvMem)
        // §B-DUMP needs TRANSFER_SRC for vkCmdCopyBuffer interp → staging.
        // §J.3.e.X Path β.2 需要 TRANSFER_DST 因為 RifeNativeExecutor.
        // runInferenceGpu 內部 vkCmdCopyBuffer(out0_blob → m_FrucInterpRgbBuf)
        // 把 RIFE 推論結果搬進來；warp shader 寫的時候是 STORAGE，但 RIFE
        // 路徑改成 transfer write — 兩個 path 共用一個 buffer 都需要支援.
        // §J.3.e.2.i.10 Phase 2B step 4 (v1.4.55) — cross-queue: compute
        // queue writes (warp dispatch from RIFE chain) → graphics queue
        // reads (interp fragment-sample in render pass).
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_FrucInterpRgbBuf, m_FrucInterpRgbMem, /*crossQueue=*/true)
        // §B2 2026-05-06 — TRIPLE 第二份 interp output buffer.
        // 在 createFrucComputeResources 永遠 alloc 一份（即使 dual mode 也
        // 浪費一塊 buffer），保持 VkDescriptorSet update 邏輯簡單；single-
        // present 模式已 short-circuit 不會 alloc 整個 FRUC chain.
        // (TRANSFER_SRC/DST 同上 reasons — 即使 β.1/β.2 不用 TRIPLE，
        // §B-DUMP 還是會 copy 它）.
        || !allocBuf(sizeRGB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                            | VK_BUFFER_USAGE_TRANSFER_DST_BIT
                            | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     m_FrucInterpRgbBuf2, m_FrucInterpRgbBuf2Mem, /*crossQueue=*/true)
        // §B1a — NV12 mirror, TRANSFER_DST so HW path's cmdCopyImageToBuffer
        // can write into it; STORAGE_BUFFER so NV12→RGB compute reads as
        // raw bytes via Y@offset 0 / UV@offset W*H.  No TRANSFER_SRC since
        // we never copy this buffer onwards.
        || !allocBuf(sizeNV12, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     m_SwFrucNv12Buf, m_SwFrucNv12BufMem)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: buffer alloc failed");
        m_FrucDisabled = true;
        return false;
    }

    // === Descriptor pool: 7 sets × (2+2+4+2+4+4+2) = 20 storage-buffer descriptors ===
    // (§B-NVOF Phase 5 added NvOFConvert desc set: 2 bindings (flow staging + mv filtered),
    //  allocated lazily in createOpticalFlowSession after m_NvOfFlowStaging exists.)
    // (i.4.1 added NV12→RGB with 2 bindings → 1 more set, 2 more descriptors)
    // (Phase 2.5 added 2nd NV12→RGB descriptor set whose binding 0 points at
    //  m_SwFrucNv12Buf for native-decode source → +1 set, +2 descriptors)
    // (v1.3.278 reverted Phase 2.5h per-slot variant — single descset only.)
    // (§B2 2026-05-06 added 2nd warp desc set for TRIPLE → +1 set, +4 descriptors.)
    VkDescriptorPoolSize pSize = {};
    pSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    // §J.3.e.X Path β.4 — 3 extra desc sets for bilinear down/up pipeline
    // (DownPrev / DownCurr / UpInterp), each with 2 storage bindings → +6.
    // Bumped maxSets 7 → 10, descriptorCount 20 → 26.
    // §J.3.e.X Path β.5 — 3 more sets: UpFlow (2 bindings), UpMask (2 bindings),
    // NativeWarp (5 bindings).  +9 storage descriptors.  Bumped 10→13, 26→35.
    // §J.3.e.2.i.15 (v1.4.80) — warp 2 sets each bumped 4→5 bindings = +2 desc.
    // §J.3.e.2.i.17 (v1.4.82) — +2 sets (MeBackward 4 bind + MedianBackward 2
    // bind = 6 desc) + warp sets each bumped 5→6 bindings (+2 desc) = +8 desc.
    // §J.3.e.2.i.19 (v1.4.83) Phase B — +2 sets (Flag 3 bind + Fine4x4 5 bind
    // = 8 desc) + warp sets each bumped 6→8 bindings (+4 desc) = +12 desc.
    // §J.3.e.2.i.23 (v1.4.85) Pyramid — +3 sets (DownscaleHalfPrev 2 +
    // DownscaleHalfCurr 2 + MEHalf 3 = 7 desc) + ME 4→5 bindings (+1 desc) = +8.
    // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid — ½ ME bumped 3→4 bind (+1 desc
    // ×1 set) + 3 new sets (DownscaleHalfPrevQuarter 2 + DownscaleHalfCurrQuarter 2
    // + MEQuarter 4 = 8 desc) = +9 desc.  Total 65→74, sets 20→23.
    pSize.descriptorCount = 74;
    VkDescriptorPoolCreateInfo dpCi = {};
    dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCi.maxSets = 23;
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
        // §J.3.e.2.i.23 (v1.4.85) Pyramid — ME 加 binding 4 = m_FrucMvHalfBuf
        // (½ res MV from pyramid coarse pass).  hasHalfMvPredictor=1 啟用.
        || !allocAndUpdateSet(m_FrucMeDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucPrevMvBuf, m_FrucMvBuf, m_FrucMvHalfBuf },
                              m_FrucMeDescSet)
        // §J.3.e.2.i.23 (v1.4.85) Pyramid — RGB ½ downscale prev: in=prevRgb, out=prevRgbHalf
        || !allocAndUpdateSet(m_FrucDownscaleHalfDsl,
                              { m_FrucPrevRgbBuf, m_FrucPrevRgbHalfBuf },
                              m_FrucDownscaleHalfPrevDescSet)
        // §J.3.e.2.i.23 (v1.4.85) Pyramid — RGB ½ downscale curr: in=currRgb, out=currRgbHalf
        || !allocAndUpdateSet(m_FrucDownscaleHalfDsl,
                              { m_FrucCurrRgbBuf, m_FrucCurrRgbHalfBuf },
                              m_FrucDownscaleHalfCurrDescSet)
        // §J.3.e.2.i.23 (v1.4.85) Pyramid — ½ res ME desc set (4 bind incl. ¼ MV)
        || !allocAndUpdateSet(m_FrucMeHalfDsl,
                              { m_FrucPrevRgbHalfBuf, m_FrucCurrRgbHalfBuf, m_FrucMvQuarterBuf, m_FrucMvHalfBuf },
                              m_FrucMeHalfDescSet)
        // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid — downscale ½→¼ prev
        || !allocAndUpdateSet(m_FrucDownscaleHalfDsl,
                              { m_FrucPrevRgbHalfBuf, m_FrucPrevRgbQuarterBuf },
                              m_FrucDownscaleHalfPrevQuarterDescSet)
        // §J.3.e.2.i.24 — downscale ½→¼ curr
        || !allocAndUpdateSet(m_FrucDownscaleHalfDsl,
                              { m_FrucCurrRgbHalfBuf, m_FrucCurrRgbQuarterBuf },
                              m_FrucDownscaleHalfCurrQuarterDescSet)
        // §J.3.e.2.i.24 — ¼ res ME (reuse ½ ME pipeline, stub binding 2 ¼ MV)
        // hasCoarserMvPredictor=0 so binding 2 not read; bind quarter MV self as stub.
        || !allocAndUpdateSet(m_FrucMeHalfDsl,
                              { m_FrucPrevRgbQuarterBuf, m_FrucCurrRgbQuarterBuf, m_FrucMvQuarterBuf, m_FrucMvQuarterBuf },
                              m_FrucMeQuarterDescSet)
        || !allocAndUpdateSet(m_FrucMedianDsl,
                              { m_FrucMvBuf, m_FrucMvFilteredBuf },
                              m_FrucMedianDescSet)
        // §J.3.e.2.i.17 (v1.4.82) — Backward ME desc set. binding 0/1 = RGB,
        // 2 = fwd MV (m_FrucMvBuf, raw forward) as predictor, 3 = output bwd MV.
        || !allocAndUpdateSet(m_FrucMeBackwardDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvBuf, m_FrucMvBackwardBuf },
                              m_FrucMeBackwardDescSet)
        // §J.3.e.2.i.17 (v1.4.82) — Backward median desc set: reuses median
        // pipeline + DSL, just different I/O buffers.
        || !allocAndUpdateSet(m_FrucMedianDsl,
                              { m_FrucMvBackwardBuf, m_FrucMvBackwardFilteredBuf },
                              m_FrucMedianBackwardDescSet)
        // §J.3.e.2.i.19 (v1.4.83) Phase B — Flag pre-pass desc set:
        //   0 = fwdMV (filtered), 1 = bwdMV (filtered), 2 = flag buf out
        || !allocAndUpdateSet(m_FrucFlagDsl,
                              { m_FrucMvFilteredBuf, m_FrucMvBackwardFilteredBuf, m_FrucRefineFlagBuf },
                              m_FrucFlagDescSet)
        // §J.3.e.2.i.19 (v1.4.83) Phase B — Fine 4×4 ME desc set:
        //   0 = prevRGB, 1 = currRGB, 2 = fwdMV (filtered, parent),
        //   3 = flag buf, 4 = fine MV out
        || !allocAndUpdateSet(m_FrucFine4x4Dsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvFilteredBuf,
                                m_FrucRefineFlagBuf, m_FrucMvFineBuf },
                              m_FrucFine4x4DescSet)
        // §J.3.e.2.i.19 (v1.4.83) Phase B — Warp now has 8 bindings:
        //   4 = prevMv (temporal), 5 = bwdMv (occlusion),
        //   6 = fineMv (4×4 refined), 7 = refineFlag (parent gate)
        || !allocAndUpdateSet(m_FrucWarpDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvFilteredBuf,
                                m_FrucInterpRgbBuf, m_FrucPrevMvBuf, m_FrucMvBackwardFilteredBuf,
                                m_FrucMvFineBuf, m_FrucRefineFlagBuf },
                              m_FrucWarpDescSet)
        || !allocAndUpdateSet(m_FrucWarpDsl,
                              { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf, m_FrucMvFilteredBuf,
                                m_FrucInterpRgbBuf2, m_FrucPrevMvBuf, m_FrucMvBackwardFilteredBuf,
                                m_FrucMvFineBuf, m_FrucRefineFlagBuf },
                              m_FrucWarpDescSet2)) {
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
            // §J.3.g v2: 6 timestamps × kFrucFramesInFlight slots so we can
            // attribute per-stage GPU time (NV12RGB / ME / Median / Warp /
            // Copy).  Was 2 (chain start + end only).
            qpCi.queryCount = 6 * kFrucFramesInFlight;
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
            // §J.3.e.2.i.10 Phase 2B (v1.4.59) — second pool just for the
            // 3-cmd-buf split path: 6 timestamps × kFrucFramesInFlight slots
            // (preCmd start/end, cmpCmd start/end, postCmd start/end).
            // Non-fatal — failure here just disables phase 2B profiling.
            VkQueryPoolCreateInfo qpCi2B = qpCi;
            qpCi2B.queryCount = 6 * kFrucFramesInFlight;
            if (pfnCreateQueryPool(m_Device, &qpCi2B, nullptr, &m_Phase2BTimerPool) != VK_SUCCESS) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B timestamp pool create failed (non-fatal)");
                m_Phase2BTimerPool = VK_NULL_HANDLE;
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B timestamp pool "
                            "ready (%u queries)", qpCi2B.queryCount);
            }
        }
    }

    m_FrucReady = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.4 init: PASS — 3 pipelines + 6 buffers + 3 descSets ready "
                "(sizeRGB=%llu, sizeMV=%llu)",
                (unsigned long long)sizeRGB, (unsigned long long)sizeMV);

    // §B-NVOF Phase 3c — best-effort OF session create.  Failure non-fatal
    // (m_NvOfReady stays false → chain falls back to block-matching ME).
    if (m_NvOfFuncList) {
        createOpticalFlowSession((uint32_t)width, (uint32_t)height);
    }

    // §B-DUMP 2026-05-07 — best-effort diagnostic frame dump init
    // (no-op if VIPLE_VKFRUC_DUMP_DIR unset).  Must be called AFTER
    // m_FrucSrcWidth/Height stamped above and m_FrucCurrRgbBuf alloc'd
    // (so dump staging size matches actual buffer size).
    initFrameDump();

    // §J.3.e.2.i.11 (v1.4.67) — cross-hardware FRUC auto-tier benchmark.
    // 在 createRifeNativeResources 之前跑一次 InterpBilinear dispatch 量
    // GPU wall-clock，根據結果分級 tier 並更新 QSettings.  Skip 條件：
    //   1. 已 cache 過該 GPU 的 benchmark (prefs->vkfrucBenchmarkNs > 0
    //      且 prefs->vkfrucDetectedGpuName 跟當前 GPU name 一致)
    //   2. 缺 m_GraphicsQueue / m_Instance / m_PhysicalDevice / m_Device
    // 失敗 (bench=0) fallback v1.4.66 heuristic tier 不覆寫.
    //
    // Threshold (wall-clock InterpBilinear 256→512×16ch on 3060L ≈ 1.5ms):
    //   < 1.0 ms → QUALITY
    //   1.0-2.5 ms → BALANCED
    //   2.5-6.0 ms → PERFORMANCE
    //   > 6.0 ms → ENTRY
    // Wall-clock 包含 CPU enqueue + ncnn glslang compile + GPU dispatch +
    // fence wait. 因為 warmup pass 已先吃掉 glslang compile cost，measure
    // 主要是 GPU dispatch + CPU sync (~0.5ms 固定)。
    if (auto* prefs = StreamingPreferences::get()) {
        VkPhysicalDeviceProperties pdpBench = {};
        auto pfnGetInstPa = (PFN_vkGetInstanceProcAddr)m_pfnGetInstanceProcAddr;
        auto pfnGetPDPropsBench = (PFN_vkGetPhysicalDeviceProperties)pfnGetInstPa(
            m_Instance, "vkGetPhysicalDeviceProperties");
        if (pfnGetPDPropsBench) pfnGetPDPropsBench(m_PhysicalDevice, &pdpBench);
        const QString currentGpuName = QString::fromUtf8(pdpBench.deviceName);
        // §J.3.e.2.i.11 (v1.4.73) — invalidate stale v1.4.68-72 cache where
        // benchmark wrongly measured CPU compile time (~490ms on any HW).
        // Real GPU dispatch is <50ms even on weak iGPU; treat anything
        // larger as the broken v1.4.68 measurement and force rerun with
        // the new outSubmitWaitNs-based accurate measurement.
        constexpr qint64 kSaneMaxNs = 50LL * 1000LL * 1000LL;  // 50 ms
        const bool cacheValid = (prefs->vkfrucBenchmarkNs > 0)
                             && (prefs->vkfrucBenchmarkNs < kSaneMaxNs)
                             && (prefs->vkfrucDetectedGpuName == currentGpuName);
        if (cacheValid) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-BENCH] §J.3.e.2.i.11 v1.4.67 cache hit: "
                "gpu='%s' benchmark=%.3fms tier=%d (cached, skipping rerun)",
                currentGpuName.toUtf8().constData(),
                (double)prefs->vkfrucBenchmarkNs / 1.0e6,
                (int)prefs->vkfrucDetectedTier);
        } else if (m_GraphicsQueue && m_PhysicalDevice && m_Device && m_Instance) {
            viple::rife_native_vk::VulkanCtx benchCtx = {};
            benchCtx.instance            = m_Instance;
            benchCtx.physicalDevice      = m_PhysicalDevice;
            benchCtx.device              = m_Device;
            benchCtx.computeQueue        = m_GraphicsQueue;  // graphics QF supports compute
            benchCtx.computeQueueFamily  = m_QueueFamily;
            benchCtx.getInstanceProcAddr = (void*)m_pfnGetInstanceProcAddr;
            const uint64_t benchNs = viple::rife_native_vk::benchmarkInterpBilinearOnce(benchCtx);
            if (benchNs > 0) {
                StreamingPreferences::VkfrucGpuTier benchTier;
                if      (benchNs < 1'000'000ULL)        benchTier = StreamingPreferences::VGT_QUALITY;
                else if (benchNs < 2'500'000ULL)        benchTier = StreamingPreferences::VGT_BALANCED;
                else if (benchNs < 6'000'000ULL)        benchTier = StreamingPreferences::VGT_PERFORMANCE;
                else                                    benchTier = StreamingPreferences::VGT_ENTRY;
                const char* benchTierName =
                    benchTier == StreamingPreferences::VGT_QUALITY     ? "QUALITY"     :
                    benchTier == StreamingPreferences::VGT_BALANCED    ? "BALANCED"    :
                    benchTier == StreamingPreferences::VGT_PERFORMANCE ? "PERFORMANCE" :
                    benchTier == StreamingPreferences::VGT_ENTRY       ? "ENTRY"       : "UNKNOWN";
                const char* heurTierName =
                    prefs->vkfrucDetectedTier == StreamingPreferences::VGT_QUALITY     ? "QUALITY"     :
                    prefs->vkfrucDetectedTier == StreamingPreferences::VGT_BALANCED    ? "BALANCED"    :
                    prefs->vkfrucDetectedTier == StreamingPreferences::VGT_PERFORMANCE ? "PERFORMANCE" :
                    prefs->vkfrucDetectedTier == StreamingPreferences::VGT_ENTRY       ? "ENTRY"       : "UNKNOWN";
                const bool match = (benchTier == prefs->vkfrucDetectedTier);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-BENCH] §J.3.e.2.i.11 v1.4.67 measured: "
                    "%.3fms → tier=%s (heuristic=%s, match=%d)",
                    (double)benchNs / 1.0e6, benchTierName, heurTierName, match);
                prefs->vkfrucBenchmarkNs   = (qint64)benchNs;
                prefs->vkfrucDetectedTier  = benchTier;  // 以 benchmark 為主
                prefs->vkfrucDetectedGpuName = currentGpuName;
                prefs->save();
                m_DetectedGpuTier = (int)benchTier;
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-BENCH] §J.3.e.2.i.11 v1.4.67 dispatch failed; "
                    "falling back to v1.4.66 heuristic tier=%d", (int)prefs->vkfrucDetectedTier);
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-BENCH] §J.3.e.2.i.11 v1.4.67 skipped: "
                "VulkanCtx incomplete (no graphics queue / device)");
        }
    }

    // §J.3.e.X Path β — eager init of native RIFE executor (β.1 = init-only
    // proof of life on this VkDevice; β.2 will wire it into runFrucComputeChain).
    // Failure non-fatal: m_RifeNativeReady stays false, block-match remains active.
    if (m_RifeNativeMode) {
        if (!createRifeNativeResources(width, height)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β] init failed — block-match path remains active");
            m_RifeNativeMode  = false;
            m_RifeNativeReady = false;
        }
    }

    return true;
}

void VkFrucRenderer::destroyFrucComputeResources()
{
    if (!m_FrucReady && !m_FrucMePipeline) return;
    if (m_Device == VK_NULL_HANDLE) return;

    // §J.3.e.X Path β — tear down native RIFE first.  Holds VkPipelines /
    // VkBuffers tied to m_Device so it must release before the device-level
    // teardown below.
    destroyRifeNativeResources();

    // §B-DUMP — tear down dump first (joins writer thread, frees staging).
    teardownFrameDump();

    // §B-NVOF Phase 3c — destroy OF session before tearing down compute
    // chain (so registered resources unwind in the correct order).
    destroyOpticalFlowSession();

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
    // §J.3.e.2.i.17 (v1.4.82) — Backward ME pipeline
    DESTROY_PIPE(m_FrucMeBackwardPipeline, m_FrucMeBackwardPipeLay,
                 m_FrucMeBackwardDsl,      m_FrucMeBackwardShaderMod)
    // §J.3.e.2.i.19 (v1.4.83) Phase B — flag + fine4x4 pipelines
    DESTROY_PIPE(m_FrucFlagPipeline,    m_FrucFlagPipeLay,
                 m_FrucFlagDsl,         m_FrucFlagShaderMod)
    DESTROY_PIPE(m_FrucFine4x4Pipeline, m_FrucFine4x4PipeLay,
                 m_FrucFine4x4Dsl,      m_FrucFine4x4ShaderMod)
    // §J.3.e.2.i.23 (v1.4.85) Pyramid — downscale half + ½ res ME pipelines
    DESTROY_PIPE(m_FrucDownscaleHalfPipeline, m_FrucDownscaleHalfPipeLay,
                 m_FrucDownscaleHalfDsl,      m_FrucDownscaleHalfShaderMod)
    DESTROY_PIPE(m_FrucMeHalfPipeline,        m_FrucMeHalfPipeLay,
                 m_FrucMeHalfDsl,             m_FrucMeHalfShaderMod)
    // §J.3.e.X Path β.4
    DESTROY_PIPE(m_RifeBilinearPipeline, m_RifeBilinearPipeLay,
                 m_RifeBilinearDsl,      m_RifeBilinearShaderMod)
    // §J.3.e.X Path β.5
    DESTROY_PIPE(m_RifeFlowBilinearPipeline, m_RifeFlowBilinearPipeLay,
                 m_RifeFlowBilinearDsl,      m_RifeFlowBilinearShaderMod)
    DESTROY_PIPE(m_RifeNativeWarpPipeline,   m_RifeNativeWarpPipeLay,
                 m_RifeNativeWarpDsl,        m_RifeNativeWarpShaderMod)
#undef DESTROY_PIPE

#define DESTROY_BUF(b, m)                                          \
    if (b && pfnDestroyBuf) { pfnDestroyBuf(m_Device, b, nullptr); b = VK_NULL_HANDLE; } \
    if (m && pfnFreeMem)    { pfnFreeMem(m_Device,    m, nullptr); m = VK_NULL_HANDLE; }
    DESTROY_BUF(m_FrucPrevRgbBuf,    m_FrucPrevRgbBufMem)
    DESTROY_BUF(m_FrucCurrRgbBuf,    m_FrucCurrRgbBufMem)
    DESTROY_BUF(m_FrucMvBuf,         m_FrucMvBufMem)
    DESTROY_BUF(m_FrucMvFilteredBuf, m_FrucMvFilteredMem)
    DESTROY_BUF(m_FrucPrevMvBuf,     m_FrucPrevMvMem)
    // §J.3.e.2.i.17 (v1.4.82)
    DESTROY_BUF(m_FrucMvBackwardBuf,         m_FrucMvBackwardBufMem)
    DESTROY_BUF(m_FrucMvBackwardFilteredBuf, m_FrucMvBackwardFilteredMem)
    // §J.3.e.2.i.19 (v1.4.83) Phase B
    DESTROY_BUF(m_FrucRefineFlagBuf, m_FrucRefineFlagBufMem)
    DESTROY_BUF(m_FrucMvFineBuf,     m_FrucMvFineBufMem)
    // §J.3.e.2.i.23 (v1.4.85) Pyramid
    DESTROY_BUF(m_FrucPrevRgbHalfBuf, m_FrucPrevRgbHalfBufMem)
    DESTROY_BUF(m_FrucCurrRgbHalfBuf, m_FrucCurrRgbHalfBufMem)
    DESTROY_BUF(m_FrucMvHalfBuf,      m_FrucMvHalfBufMem)
    // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid
    DESTROY_BUF(m_FrucPrevRgbQuarterBuf, m_FrucPrevRgbQuarterBufMem)
    DESTROY_BUF(m_FrucCurrRgbQuarterBuf, m_FrucCurrRgbQuarterBufMem)
    DESTROY_BUF(m_FrucMvQuarterBuf,      m_FrucMvQuarterBufMem)
    DESTROY_BUF(m_FrucInterpRgbBuf,  m_FrucInterpRgbMem)
    // §B2 2026-05-06 — TRIPLE 第二份 interp output buffer.
    DESTROY_BUF(m_FrucInterpRgbBuf2, m_FrucInterpRgbBuf2Mem)
    // §B1a 2026-05-06 — m_SwFrucNv12Buf moved here from
    // destroySwUploadResources (FRUC-owned, used by both SW + HW path).
    DESTROY_BUF(m_SwFrucNv12Buf,     m_SwFrucNv12BufMem)
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
    // §J.3.e.2.i.10 Phase 2B (v1.4.59) — phase2B-only timestamp pool
    if (m_Phase2BTimerPool && pfnDestroyQueryPool) {
        pfnDestroyQueryPool(m_Device, m_Phase2BTimerPool, nullptr);
        m_Phase2BTimerPool = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < kFrucFramesInFlight; ++i) m_Phase2BTimerArmed[i] = false;

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

// =================================================================
// §J.3.e.X Path β — native RIFE Vulkan integration
// =================================================================
//
// β.1 (this scaffold): init-only proof of life.  Constructs a
// RifeNativeExecutor on VkFrucRenderer 既有的 m_Instance / m_Device /
// m_GraphicsQueue 並 call initialize().  Verifies that the Path β
// premise holds —— ncnnfruc Final.3b 在 NV driver 596.144 撞到的
// dual-VkDevice INITIALIZATION_FAILED 不再發生（因為 Path β
// 完全跳過 ncnn::create_gpu_instance() 的 dedicated VkDevice）.
//
// 不換 chain；ME→median→warp 路徑保留．成功只會在 log 印一行
// `[VIPLE-VKFRUC-RIFE-β] init OK ...`，跟著啟動 stream 後接 β.2.
//
// β.2 (next phase): adds runInferenceGpu(cmd, in0, in1, t, out)
// public API on RifeNativeExecutor + Site 4 chain branch in
// runFrucComputeChain.

bool VkFrucRenderer::createRifeNativeResources(int width, int height)
{
    if (m_RifeNativeReady) return true;
    if (m_Device == VK_NULL_HANDLE
        || m_PhysicalDevice == VK_NULL_HANDLE
        || m_Instance == VK_NULL_HANDLE
        || m_GraphicsQueue == VK_NULL_HANDLE
        || !m_pfnGetInstanceProcAddr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] handles not ready — abort init");
        return false;
    }

    // β.4 — pick infer dim small enough that all RIFE blobs fit BAR AND
    // matches the model's hardcoded /128 alignment requirement.
    //
    // RIFE-v4.25-lite root cause (確認 2026-05-08 via trace_rife_shapes.py):
    //   Layer Resize_47 has hardcoded scale = 1/32 (param 1=2=3.125e-2),
    //   followed by Conv_48 + Conv_50 each stride=2.  So the deepest
    //   encoder feature is at input/128 spatial dim, then the decoder
    //   upsamples by Resize_87 with hardcoded scale ×32 + ConvTranspose
    //   stride 2 + PixelShuffle r=2 (= ×128 total).
    //
    //   Path A (encoder skip): input/128 × 128 = inputH if inputH /128 整除,
    //   else integer-div rounds up giving H = ceil(input/128) × 128.
    //   Path B (decoder direct): input/4 × 4 = input always.
    //   For Add_503 to match, both must equal inputH → require inputH
    //   to be /128 exactly.  Empirical fail matrix at 1080p source:
    //     256×128: PASS (128 = 1×128)
    //     384×192: FAIL (192 = 1.5×128 → 256 vs 192 mismatch)
    //     448×224: FAIL (224 = 1.75×128 → 256 vs 224 mismatch)
    //     512×288: FAIL (288 = 2.25×128 → 384 vs 288 mismatch)
    //
    // 修法：inferW/H 都 round 到 /128 multiple (NOT /32 as before).
    // Override via VIPLE_VKFRUC_RIFE_INFER_DIM=N (width; height auto by aspect).
    // Default 256 → 256×128 for 1080p source.  Latency table on
    // RTX 3060 Laptop (measured 2026-05-08 via [VIPLE-VKFRUC-GPU-PROF]):
    //   256×128 = ~12ms total chain  ✓ fits 60fps DUAL budget (16.7ms)
    //   512×256 = ~30ms total chain  ✗ over budget (drops fps)
    //   768×384 / 1024×512 = even slower, unusable for live
    // Quality scales the other direction (256 has heavy bilinear-up blur).
    // Faster GPUs (RTX 4070+) likely fit 512×256 or higher; user opts in
    // via UI (Settings → Video → Vulkan FRUC → Native RIFE infer dim) or
    // VIPLE_VKFRUC_RIFE_INFER_DIM=512 / 768 / 1024 env var (must be /128).
    //
    // §J.3.e.X β.5.3 (D-lite, 2026-05-08) — round UP instead of DOWN.
    // Previously a user passing e.g. VIPLE_VKFRUC_RIFE_INFER_DIM=192
    // (between 128 and 256) got snapped DOWN to 128 — the opposite of
    // what they wanted (more quality than 128).  Round UP so 192 → 256
    // (slight latency hit, more quality) better matches user intent.
    // Full D (true 192 / 320 / 448 support via asymmetric Add center-
    // crop in inferShapes + dispatchBinaryOp) deferred — the RIFE-v4-
    // lite Add_503 mismatch needs careful per-layer center-crop logic
    // that's a larger undertaking; backlog item, see TODO.md §J.3.e.X.
    int inferW = vkfrucNativeRifeInferDimFromUserOrEnv();
    inferW = ((inferW + 127) / 128) * 128;
    if (inferW < 128) inferW = 128;
    int inferH = (int)((double)inferW * (double)height / (double)width + 0.5);
    inferH = ((inferH + 127) / 128) * 128;
    if (inferH < 128) inferH = 128;
    m_RifeNativeInferW = inferW;
    m_RifeNativeInferH = inferH;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[VIPLE-VKFRUC-RIFE-β] β.4 infer dim chosen: %dx%d (source %dx%d, "
        "down ratio %.2fx)",
        inferW, inferH, width, height, (double)width / (double)inferW);

    // β.4 prerequisite: bilinear pipeline must have built earlier in
    // createFrucComputeResources.  Without it we can't down/upscale.
    if (m_RifeBilinearPipeline == VK_NULL_HANDLE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] init failed: bilinear pipeline missing — "
            "createFrucComputeResources didn't build it (m_RifeNativeMode "
            "may have been false at that point)");
        return false;
    }

    viple::rife_native_vk::VulkanCtx ctx;
    ctx.instance            = m_Instance;
    ctx.physicalDevice      = m_PhysicalDevice;
    ctx.device              = m_Device;
    // §J.3.e.2.i.10 Phase 2B step 4 (v1.4.55) — when async-compute is
    // wired (env=1 + dedicated compute QF + Phase 2A infra READY), route
    // RIFE inference recording onto the compute queue/family so caller
    // can later submit the cmpCmd buffer to m_ComputeQueue.  Without
    // this hand-off, runInferenceGpu records onto whatever cmd buffer
    // the caller provides; with this hand-off, RifeNativeExecutor's
    // internal cmd pool (used by standalone smoke tests) also binds to
    // the compute QF.  Falls back to graphics QF when async path is
    // not requested or unavailable — bit-identical to v1.4.54.
    const bool asyncCtxActive = m_AsyncComputeRequested
                             && m_AsyncComputeAvailable
                             && m_ComputeQueue != VK_NULL_HANDLE
                             && m_ComputeQueueFamily != UINT32_MAX
                             && m_QueueFamily != m_ComputeQueueFamily;
    if (asyncCtxActive) {
        ctx.computeQueueFamily = m_ComputeQueueFamily;
        ctx.computeQueue       = m_ComputeQueue;
        ctx.concurrentSharingQfs[0]  = m_QueueFamily;
        ctx.concurrentSharingQfs[1]  = m_ComputeQueueFamily;
        ctx.concurrentSharingQfCount = 2;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] §J.3.e.2.i.10 Phase 2B ctx wired to "
            "dedicated compute QF=%u queue=%p (graphics QF=%u); "
            "CONCURRENT sharing across both for hand-off buffers",
            m_ComputeQueueFamily, (void*)m_ComputeQueue, m_QueueFamily);
    } else {
        ctx.computeQueueFamily = m_QueueFamily;
        ctx.computeQueue       = m_GraphicsQueue;
    }
    ctx.getInstanceProcAddr = (void*)m_pfnGetInstanceProcAddr;

    QString modelDir = Path::getDataFilePath(QString::fromLatin1("rife-v4.25-lite"));
    if (modelDir.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] init failed: rife-v4.25-lite model dir not found "
            "(expected via Path::getDataFilePath)");
        return false;
    }

    viple::rife_native_vk::RifeNativeExecutor::InitOptions opts;
    opts.ctx = ctx;
    opts.modelDir = modelDir;
    opts.in0Shape.c = 3;
    opts.in0Shape.h = m_RifeNativeInferH;
    opts.in0Shape.w = m_RifeNativeInferW;
    opts.in1Shape.c = 3;
    opts.in1Shape.h = m_RifeNativeInferH;
    opts.in1Shape.w = m_RifeNativeInferW;
    opts.in2Shape.c = 1;
    opts.in2Shape.h = 1;
    opts.in2Shape.w = 1;
    // Cross-launch pipeline cache — saves ~50-300ms on cold start once
    // populated.  Distinct file from ncnnfruc Final.3b 的 cache so Path β
    // 跟 NCNN swap 各自累積（GPU+driver 版本相依，不能跨用）.
    opts.pipelineCachePath = Path::getCacheFileInfo(
        QString::fromLatin1("rife_vkfruc_path_b_pipe.cache"))
        .absoluteFilePath();
    // β.2 — VkFrucRenderer 用 kFrucFramesInFlight (=2) 個 cmd buf ring.
    // 對齊 RifeNativeExecutor 的 per-slot descPool 數量，這樣 runInferenceGpu
    // 拿到 slotIdx 就能 reset 對應的 pool（前一個 cmd 已經 retire 過 fence wait）.
    opts.numFrameSlots = (int)kFrucFramesInFlight;

    m_RifeNative = new viple::rife_native_vk::RifeNativeExecutor();
    if (!m_RifeNative->initialize(opts)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] init failed: RifeNativeExecutor.initialize() "
            "returned false (modelDir=%s, infer=%dx%d) — block-match path "
            "remains active",
            qUtf8Printable(modelDir),
            m_RifeNativeInferW, m_RifeNativeInferH);
        delete m_RifeNative;
        m_RifeNative = nullptr;
        return false;
    }

    auto outShape = m_RifeNative->outputShape();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[VIPLE-VKFRUC-RIFE-β] init OK inferDim=%dx%d outShape=%dx%dx%d "
        "modelDir=%s cache=%s",
        m_RifeNativeInferW, m_RifeNativeInferH,
        outShape.c, outShape.h, outShape.w,
        qUtf8Printable(modelDir),
        qUtf8Printable(opts.pipelineCachePath));

    // β.4 — alloc 3 down/up intermediate buffers (DEVICE_LOCAL, planar
    // fp32 CHW) at infer dim.
    //   m_RifeDownPrev:   target of bilinear downscale of m_FrucPrevRgbBuf
    //   m_RifeDownCurr:   target of bilinear downscale of m_FrucCurrRgbBuf
    //   m_RifeDownInterp: target of RIFE inference (becomes source of
    //                     bilinear upscale to m_FrucInterpRgbBuf).
    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCreateBuffer    = (PFN_vkCreateBuffer)getDevPa(m_Device, "vkCreateBuffer");
    auto pfnGetBufMemReq    = (PFN_vkGetBufferMemoryRequirements)getDevPa(m_Device, "vkGetBufferMemoryRequirements");
    auto pfnAllocMem        = (PFN_vkAllocateMemory)getDevPa(m_Device, "vkAllocateMemory");
    auto pfnBindBufMem      = (PFN_vkBindBufferMemory)getDevPa(m_Device, "vkBindBufferMemory");
    auto pfnAllocDescSets   = (PFN_vkAllocateDescriptorSets)getDevPa(m_Device, "vkAllocateDescriptorSets");
    auto pfnUpdateDescSets  = (PFN_vkUpdateDescriptorSets)getDevPa(m_Device, "vkUpdateDescriptorSets");
    auto pfnGetPdMemProps   = (PFN_vkGetPhysicalDeviceMemoryProperties)
        m_pfnGetInstanceProcAddr(m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnCreateBuffer || !pfnGetBufMemReq || !pfnAllocMem || !pfnBindBufMem
        || !pfnAllocDescSets || !pfnUpdateDescSets || !pfnGetPdMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] β.4 PFN load failed");
        m_RifeNative->shutdown();
        delete m_RifeNative;
        m_RifeNative = nullptr;
        return false;
    }

    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(m_PhysicalDevice, &memProps);
    auto pickMemType = [&](uint32_t typeBits, VkMemoryPropertyFlags want) -> int {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeBits & (1u << i))
                && (memProps.memoryTypes[i].propertyFlags & want) == want) return (int)i;
        }
        return -1;
    };
    // §J.3.e.2.i.10 Phase 2B step 4 (v1.4.55) — when async-compute is
    // available, the down/flow/mask/interp buffers below are written by
    // graphics-queue dispatches and read by compute-queue (or vice
    // versa).  Skip explicit queue-family ownership transfer (QFOT)
    // barrier paired-pair complexity by promoting these specific buffers
    // to VK_SHARING_MODE_CONCURRENT spanning {graphics QF, compute QF}.
    // Buffer is short-lived fp32 planar (256 KiB – 1.5 MiB), so the
    // potential tile-layout perf cost on NV is negligible vs the
    // silent-corruption risk of a wrong release/acquire pair.  Caller
    // still needs access-mask + pipeline-stage barriers; CONCURRENT
    // only waives the QF-ownership clause of VK_*_BARRIER structs.
    const bool sharingAcrossQfs = m_AsyncComputeAvailable
                               && m_QueueFamily != UINT32_MAX
                               && m_ComputeQueueFamily != UINT32_MAX
                               && m_QueueFamily != m_ComputeQueueFamily;
    const uint32_t crossQfs[2] = { m_QueueFamily, m_ComputeQueueFamily };
    auto allocBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                        VkBuffer& outBuf, VkDeviceMemory& outMem) -> bool {
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = size;
        bci.usage = usage;
        if (sharingAcrossQfs) {
            bci.sharingMode           = VK_SHARING_MODE_CONCURRENT;
            bci.queueFamilyIndexCount = 2;
            bci.pQueueFamilyIndices   = crossQfs;
        } else {
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        if (pfnCreateBuffer(m_Device, &bci, nullptr, &outBuf) != VK_SUCCESS) return false;
        VkMemoryRequirements memReq = {};
        pfnGetBufMemReq(m_Device, outBuf, &memReq);
        int mti = pickMemType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mti < 0) return false;
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize  = memReq.size;
        mai.memoryTypeIndex = (uint32_t)mti;
        if (pfnAllocMem(m_Device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        return pfnBindBufMem(m_Device, outBuf, outMem, 0) == VK_SUCCESS;
    };
    const VkDeviceSize sizeDown =
        (VkDeviceSize)inferW * inferH * 3 * sizeof(float);
    const VkBufferUsageFlags downUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                       | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!allocBuf(sizeDown, downUsage, m_RifeDownPrev,   m_RifeDownPrevMem)
        || !allocBuf(sizeDown, downUsage, m_RifeDownCurr,   m_RifeDownCurrMem)
        || !allocBuf(sizeDown, downUsage, m_RifeDownInterp, m_RifeDownInterpMem)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] β.4 down-buffer alloc failed (size=%llu B each)",
            (unsigned long long)sizeDown);
        // Don't goto-cleanup — destroyRifeNativeResources picks up partial state.
        m_RifeNative->shutdown();
        delete m_RifeNative;
        m_RifeNative = nullptr;
        return false;
    }

    // Allocate 3 desc sets from m_FrucDescPool (already bumped to maxSets=10).
    // Bindings: 0 = in, 1 = out (both STORAGE_BUFFER, read-only on 0, write on 1).
    auto allocBilinearDs = [&](VkBuffer inBuf, VkBuffer outBuf,
                                VkDescriptorSet& outDs) -> bool {
        VkDescriptorSetAllocateInfo asi = {};
        asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        asi.descriptorPool = m_FrucDescPool;
        asi.descriptorSetCount = 1;
        asi.pSetLayouts = &m_RifeBilinearDsl;
        if (pfnAllocDescSets(m_Device, &asi, &outDs) != VK_SUCCESS) return false;
        VkDescriptorBufferInfo dbi[2] = {};
        dbi[0].buffer = inBuf;  dbi[0].range = VK_WHOLE_SIZE;
        dbi[1].buffer = outBuf; dbi[1].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet wds[2] = {};
        for (int i = 0; i < 2; ++i) {
            wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = outDs;
            wds[i].dstBinding = (uint32_t)i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &dbi[i];
        }
        pfnUpdateDescSets(m_Device, 2, wds, 0, nullptr);
        return true;
    };
    if (!allocBilinearDs(m_FrucPrevRgbBuf,  m_RifeDownPrev,    m_RifeBilinearDownPrevDs)
        || !allocBilinearDs(m_FrucCurrRgbBuf,  m_RifeDownCurr,    m_RifeBilinearDownCurrDs)
        || !allocBilinearDs(m_RifeDownInterp,  m_FrucInterpRgbBuf, m_RifeBilinearUpInterpDs)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-VKFRUC-RIFE-β] β.4 bilinear desc set alloc failed");
        m_RifeNative->shutdown();
        delete m_RifeNative;
        m_RifeNative = nullptr;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[VIPLE-VKFRUC-RIFE-β] β.4 down/up resources ready: 3 buffers @ %llu B each, "
        "3 bilinear desc sets bound",
        (unsigned long long)sizeDown);

    // §J.3.e.X Path β.5 — alloc flow + mask buffers + 3 desc sets if β.5 active
    // and pipelines built.  Failure → log + fall back to β.4 (m_Beta5Enabled = false).
    bool beta5OK = false;
    if (m_Beta5Enabled
        && m_RifeFlowBilinearPipeline != VK_NULL_HANDLE
        && m_RifeNativeWarpPipeline   != VK_NULL_HANDLE) {

        // β.5.1: sample flow + mask at infer dim directly in warp shader.
        // Drops m_RifeFlow1080Buf (33 MB) + m_RifeMask1080Buf (8 MB) and
        // 2 bilinear UP dispatches per frame.  Saves ~1ms chain time.
        const VkDeviceSize sizeFlowSmall  = (VkDeviceSize)inferW * inferH * 4 * sizeof(float);
        const VkDeviceSize sizeMaskSmall  = (VkDeviceSize)inferW * inferH * 1 * sizeof(float);
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                       | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                       | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (allocBuf(sizeFlowSmall,  usage, m_RifeFlowOutBuf,  m_RifeFlowOutMem)
         && allocBuf(sizeMaskSmall,  usage, m_RifeMaskOutBuf,  m_RifeMaskOutMem)) {

            // Helper to allocate descSet from m_FrucDescPool with arbitrary
            // binding count (= bufs.size()).  Bindings are STORAGE_BUFFER.
            auto allocDsAnyN = [&](VkDescriptorSetLayout dsl,
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
                for (size_t i = 0; i < bufs.size(); ++i) {
                    bi[i].buffer = bufs[i];
                    bi[i].offset = 0;
                    bi[i].range  = VK_WHOLE_SIZE;
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

            // β.5.1: only NativeWarp DS — sample flow + mask at infer dim
            // directly in shader, no bilinear UPs needed.  binding 2 = flow
            // at infer dim, binding 3 = mask at infer dim.
            if (allocDsAnyN(m_RifeNativeWarpDsl,
                            { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf,
                              m_RifeFlowOutBuf, m_RifeMaskOutBuf,
                              m_FrucInterpRgbBuf },
                            m_RifeNativeWarpDs)) {
                beta5OK = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-RIFE-β5] β.5.1 ready: flow %llu B + mask %llu B at "
                    "infer dim only (sampled in warp shader, no 1080p flow alloc)",
                    (unsigned long long)sizeFlowSmall,
                    (unsigned long long)sizeMaskSmall);
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-RIFE-β5] desc set alloc failed — falling back to β.4");
            }
            // §J.3.e.2.i.10c Phase β.9 — TRIPLE 第二個 warp desc set.
            // 唯一跟 m_RifeNativeWarpDs 不同：output binding (idx 4) 換成
            // m_FrucInterpRgbBuf2 (TRIPLE 用的第二份 interp output).  alloc 失
            // 敗 → m_RifeNativeWarpDs2 留 NULL_HANDLE，runRifeNativeStage 偵
            // 測到會 fallback 不跑 t=2/3 inference (TRIPLE 退化成 DUAL output).
            if (m_TripleMode && beta5OK
                && m_FrucInterpRgbBuf2 != VK_NULL_HANDLE) {
                if (allocDsAnyN(m_RifeNativeWarpDsl,
                                { m_FrucPrevRgbBuf, m_FrucCurrRgbBuf,
                                  m_RifeFlowOutBuf, m_RifeMaskOutBuf,
                                  m_FrucInterpRgbBuf2 },
                                m_RifeNativeWarpDs2)) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-RIFE-β9] TRIPLE second warp DS ready (writes interp2)");
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-RIFE-β9] TRIPLE second warp DS alloc failed — "
                        "RIFE will only run @ t=1/3, second present (t=2/3) reuses interp1");
                }
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β5] flow/mask buffer alloc failed — falling back to β.4");
        }
    }
    if (!beta5OK) {
        m_Beta5Enabled = false;  // disable β.5 path; runRifeNativeStage will use β.4 bilinear-up-RGB
    }

    m_RifeNativeReady = true;
    return true;
}

void VkFrucRenderer::destroyRifeNativeResources()
{
    // β.4 down/up buffers — free first (m_FrucDescPool is freed later
    // by destroyFrucComputeResources, which auto-frees the desc sets).
    if (m_Device != VK_NULL_HANDLE && m_pfnGetInstanceProcAddr) {
        auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnDestroyBuf = (PFN_vkDestroyBuffer)getDevPa(m_Device, "vkDestroyBuffer");
        auto pfnFreeMem    = (PFN_vkFreeMemory)getDevPa(m_Device, "vkFreeMemory");
        if (pfnDestroyBuf && pfnFreeMem) {
#define DESTROY_BUF(b, m)                                          \
            if (b) { pfnDestroyBuf(m_Device, b, nullptr); b = VK_NULL_HANDLE; } \
            if (m) { pfnFreeMem(m_Device,    m, nullptr); m = VK_NULL_HANDLE; }
            DESTROY_BUF(m_RifeDownPrev,   m_RifeDownPrevMem)
            DESTROY_BUF(m_RifeDownCurr,   m_RifeDownCurrMem)
            DESTROY_BUF(m_RifeDownInterp, m_RifeDownInterpMem)
            // §J.3.e.X Path β.5 — flow + mask buffers
            DESTROY_BUF(m_RifeFlowOutBuf,  m_RifeFlowOutMem)
            DESTROY_BUF(m_RifeMaskOutBuf,  m_RifeMaskOutMem)
            DESTROY_BUF(m_RifeFlow1080Buf, m_RifeFlow1080Mem)
            DESTROY_BUF(m_RifeMask1080Buf, m_RifeMask1080Mem)
#undef DESTROY_BUF
        }
    }
    m_RifeBilinearDownPrevDs = VK_NULL_HANDLE;
    m_RifeBilinearDownCurrDs = VK_NULL_HANDLE;
    m_RifeBilinearUpInterpDs = VK_NULL_HANDLE;
    m_RifeBilinearUpFlowDs   = VK_NULL_HANDLE;
    m_RifeBilinearUpMaskDs   = VK_NULL_HANDLE;
    m_RifeNativeWarpDs       = VK_NULL_HANDLE;

    if (!m_RifeNative) return;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[VIPLE-VKFRUC-RIFE-β] tearing down native RIFE executor");

    m_RifeNative->shutdown();
    delete m_RifeNative;
    m_RifeNative      = nullptr;
    m_RifeNativeReady = false;
    m_RifeNativeInferW = 0;
    m_RifeNativeInferH = 0;
}

bool VkFrucRenderer::runRifeNativeStage(VkCommandBuffer cmd,
                                         uint32_t width, uint32_t height,
                                         uint32_t slotIdx)
{
    if (!m_RifeNativeReady || !m_RifeNative) return false;
    if (m_RifeBilinearPipeline == VK_NULL_HANDLE) return false;
    if (m_RifeDownPrev == VK_NULL_HANDLE
        || m_RifeDownCurr == VK_NULL_HANDLE
        || m_RifeDownInterp == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    auto pfnCmdBindPipeline    = (PFN_vkCmdBindPipeline)   getDevPa(m_Device, "vkCmdBindPipeline");
    auto pfnCmdBindDescSets    = (PFN_vkCmdBindDescriptorSets)getDevPa(m_Device, "vkCmdBindDescriptorSets");
    auto pfnCmdPushConst       = (PFN_vkCmdPushConstants)  getDevPa(m_Device, "vkCmdPushConstants");
    auto pfnCmdDispatch        = (PFN_vkCmdDispatch)       getDevPa(m_Device, "vkCmdDispatch");
    auto pfnCmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp) getDevPa(m_Device, "vkCmdWriteTimestamp");
    if (!pfnCmdPipelineBarrier || !pfnCmdBindPipeline || !pfnCmdBindDescSets
        || !pfnCmdPushConst || !pfnCmdDispatch) return false;

    // β.4 timing — write into the existing m_FrucTimerPool's slot range
    // [timerBase + 2, timerBase + 4] so the existing
    // [VIPLE-VKFRUC-GPU-PROF] me=…us median=…us warp=…us log gets repurposed:
    //   "me"     = bilinear DOWN time (ts[1] → ts[2])
    //   "median" = RIFE inference time (ts[2] → ts[3])
    //   "warp"   = bilinear UP time (ts[3] → ts[4])
    // Caller (Site 4) computes timerBase from m_FrucTimerSlot but has
    // already advanced the slot for next use.  We need the same base —
    // recompute: the slot just used = (m_FrucTimerSlot + N - 1) % N.
    const uint32_t writeTimerSlot = (m_FrucTimerSlot + kFrucFramesInFlight - 1)
                                    % kFrucFramesInFlight;
    const uint32_t timerBase = writeTimerSlot * 6;
    auto writeTs = [&](uint32_t off) {
        if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
            pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 m_FrucTimerPool, timerBase + off);
        }
    };

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
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };
    auto computeBufBarrier = [&](VkBuffer b) {
        bufBarrier(b,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    };

    // Push-constant struct must match getInterpBilinearShaderGlsl exactly
    // (rife_native_vk.cpp:822-830): 5 ints + 2 floats = 28 bytes.
    struct BilinearPC {
        int32_t inH;
        int32_t inW;
        int32_t outH;
        int32_t outW;
        int32_t channels;
        float   scaleH;
        float   scaleW;
    };
    auto dispatchBilinear = [&](VkDescriptorSet ds,
                                int inW, int inH, int outW, int outH) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeBilinearPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeBilinearPipeLay,
                           0, 1, &ds, 0, nullptr);
        BilinearPC pc;
        pc.inH = inH;  pc.inW = inW;  pc.outH = outH;  pc.outW = outW;
        pc.channels = 3;
        pc.scaleH = (float)inH / (float)outH;
        pc.scaleW = (float)inW / (float)outW;
        pfnCmdPushConst(cmd, m_RifeBilinearPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);
        // 8x8 workgroup; z = channels=3.
        pfnCmdDispatch(cmd, ((uint32_t)outW + 7) / 8, ((uint32_t)outH + 7) / 8, 3);
    };

    // ---- Down step ----
    // Stage 0 left m_FrucCurrRgbBuf in COMPUTE_SHADER_BIT/SHADER_READ access
    // (computeBufBarrier inside runFrucComputeChain).  Bilinear shader reads
    // it as STORAGE_BUFFER → SHADER_READ.  Pipeline stage transition COMPUTE
    // → COMPUTE is implicit (already at the right stage).  No explicit
    // barrier needed for the read side.  Likewise m_FrucPrevRgbBuf is in
    // COMPUTE_SHADER_BIT/SHADER_READ from the previous frame's stage-4
    // barrier.

    // Bilinear writes m_RifeDown{Prev,Curr} as STORAGE_BUFFER (write-only).
    // No barrier needed before — these buffers are private and not yet
    // touched this frame.  Caller's slot fence wait guarantees previous
    // frame's GPU work on these buffers retired.
    dispatchBilinear(m_RifeBilinearDownPrevDs,
                     (int)width, (int)height,
                     m_RifeNativeInferW, m_RifeNativeInferH);
    dispatchBilinear(m_RifeBilinearDownCurrDs,
                     (int)width, (int)height,
                     m_RifeNativeInferW, m_RifeNativeInferH);

    // Down outputs need to be visible to the RIFE inference's vkCmdCopyBuffer
    // (TRANSFER_READ).  COMPUTE_SHADER_WRITE → TRANSFER_READ.
    bufBarrier(m_RifeDownPrev,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bufBarrier(m_RifeDownCurr,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    writeTs(2);  // β.4 timing: after bilinear DOWN ("me" slot in GPU-PROF)

    // m_RifeDownInterp: previous-frame state is either COMPUTE_SHADER_READ
    // (from previous bilinear up read) or unwritten (first frame).  Will be
    // overwritten by RIFE's vkCmdCopyBuffer (TRANSFER_WRITE) so we just need
    // any prior reads to complete.
    bufBarrier(m_RifeDownInterp,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    // ---- RIFE inference ----
    // β.5.1 DUAL: 1× inference @ t=0.5 → 1 interp output (m_FrucInterpRgbBuf)
    // β.9   TRIPLE: 2× inference @ t=1/3 + t=2/3 → 2 interp outputs
    //               (m_FrucInterpRgbBuf + m_FrucInterpRgbBuf2)
    // §J.3.e.X Path β.5 — flow extraction + native-res warp path (default).
    if (m_Beta5Enabled
        && m_RifeNativeWarpPipeline   != VK_NULL_HANDLE
        && m_RifeFlowOutBuf  != VK_NULL_HANDLE
        && m_RifeMaskOutBuf  != VK_NULL_HANDLE
        && m_RifeNativeWarpDs     != VK_NULL_HANDLE) {

        // Source RGB buffers stay readable across both warp passes.  Promote
        // once here.
        bufBarrier(m_FrucCurrRgbBuf,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        bufBarrier(m_FrucPrevRgbBuf,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);

        struct WarpPC {
            int32_t W, H, inferW, inferH;
            float   magScaleX, magScaleY;
            int32_t _pad0, _pad1;
        };
        WarpPC pcW;
        pcW.W = (int32_t)width;  pcW.H = (int32_t)height;
        pcW.inferW = m_RifeNativeInferW;  pcW.inferH = m_RifeNativeInferH;
        pcW.magScaleX = (float)width  / (float)pcW.inferW;
        pcW.magScaleY = (float)height / (float)pcW.inferH;
        pcW._pad0 = 0; pcW._pad1 = 0;

        // Helper: run RIFE inference @ timestep t, then native warp →
        // outBuf via the matching desc set.  Inserts the read-after-write
        // barriers needed for flow/mask sharing across multiple timesteps.
        // `firstPass` controls whether we also clear the prior content of
        // outBuf (TRANSFER_WRITE / SHADER_READ → SHADER_WRITE).
        auto runOneInferAndWarp = [&](float t,
                                       VkBuffer outBuf,
                                       VkDescriptorSet warpDs) -> bool {
            // Inference fills flow/mask via vkCmdCopyBuffer (TRANSFER_WRITE).
            // Caller has already arranged m_RifeDownPrev/Curr in TRANSFER_READ.
            if (!m_RifeNative->runInferenceGpuFlow(cmd, slotIdx,
                    m_RifeDownPrev, m_RifeDownCurr, t,
                    m_RifeFlowOutBuf, m_RifeMaskOutBuf)) {
                return false;
            }
            // Promote flow/mask: TRANSFER_WRITE → SHADER_READ for warp shader.
            bufBarrier(m_RifeFlowOutBuf,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            bufBarrier(m_RifeMaskOutBuf,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            // Promote outBuf to writable.  Allowed prior states: previous-frame
            // SHADER_READ from compositor / TRANSFER_WRITE from race window.
            bufBarrier(outBuf,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_WRITE_BIT);

            pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_RifeNativeWarpPipeline);
            pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                               m_RifeNativeWarpPipeLay, 0, 1,
                               &warpDs, 0, nullptr);
            pfnCmdPushConst(cmd, m_RifeNativeWarpPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcW), &pcW);
            pfnCmdDispatch(cmd, ((uint32_t)width + 7) / 8, ((uint32_t)height + 7) / 8, 3);
            // Promote outBuf to SHADER_READ for downstream display use.
            computeBufBarrier(outBuf);
            return true;
        };

        // §J.3.e.2.i.10c Phase β.9 — TRIPLE: 2× inference @ t=1/3 + t=2/3.
        // Between the two inferences, flow/mask must transition from
        // SHADER_READ (warp1 finished consuming) back to TRANSFER_WRITE
        // (inference 2's vkCmdCopyBuffer overwrites them).
        const bool tripleAndDual = m_TripleMode
                                && m_RifeNativeWarpDs2 != VK_NULL_HANDLE
                                && m_FrucInterpRgbBuf2 != VK_NULL_HANDLE;
        if (tripleAndDual) {
            // Pass 1: t = 1/3 → m_FrucInterpRgbBuf
            if (!runOneInferAndWarp(1.0f / 3.0f,
                                    m_FrucInterpRgbBuf,
                                    m_RifeNativeWarpDs)) {
                return false;
            }
            // Recycle flow/mask buffers for pass 2: SHADER_READ → TRANSFER_WRITE.
            bufBarrier(m_RifeFlowOutBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            bufBarrier(m_RifeMaskOutBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            // Pass 2: t = 2/3 → m_FrucInterpRgbBuf2
            if (!runOneInferAndWarp(2.0f / 3.0f,
                                    m_FrucInterpRgbBuf2,
                                    m_RifeNativeWarpDs2)) {
                return false;
            }
            writeTs(3);  // approx — single GPU-PROF slot covers both inferences
            writeTs(4);
        } else {
            // DUAL midpoint (or TRIPLE-but-DS2-alloc-failed fallback).
            if (!runOneInferAndWarp(0.5f, m_FrucInterpRgbBuf, m_RifeNativeWarpDs)) {
                return false;
            }
            writeTs(3);  // β.5 timing: RIFE inference complete
            writeTs(4);  // β.5 timing: after native warp
        }

        static std::atomic<bool> s_b5LogOnce{false};
        bool exp = false;
        if (s_b5LogOnce.compare_exchange_strong(exp, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β5] β.5.1 chain active: RIFE flow+mask at "
                "%dx%d (infer dim) → native warp samples bilinear in shader → "
                "%ux%u interp out (%s; no 1080p flow buffers, no separate UP dispatch)",
                m_RifeNativeInferW, m_RifeNativeInferH, width, height,
                tripleAndDual ? "TRIPLE 2× inference" : "DUAL 1× inference");
        }
        return true;
    }

    // β.4 fallback path — single inference @ midpoint, output to interp1.
    // (TRIPLE only supported on β.5.1 path above; β.4 fallback stays DUAL.)
    if (!m_RifeNative->runInferenceGpu(cmd, slotIdx,
            m_RifeDownPrev, m_RifeDownCurr, 0.5f, m_RifeDownInterp)) {
        // Restore source buffers (down inputs untouched, no barrier needed).
        // Caller falls back to ME/median/warp.
        return false;
    }

    writeTs(3);  // β.4 timing: after RIFE inference ("median" slot in GPU-PROF)

    // ---- Up step ----
    // RIFE left m_RifeDownInterp in TRANSFER_WRITE.  Bilinear up reads it as
    // SHADER_READ.  Promote.
    bufBarrier(m_RifeDownInterp,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // m_FrucInterpRgbBuf: previous state may be COMPUTE_SHADER_READ from the
    // last interp display, or TRANSFER_WRITE/SHADER_READ from previous β
    // path.  Bilinear up writes to it; we need any prior reads done.
    bufBarrier(m_FrucInterpRgbBuf,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);

    dispatchBilinear(m_RifeBilinearUpInterpDs,
                     m_RifeNativeInferW, m_RifeNativeInferH,
                     (int)width, (int)height);

    // Promote m_FrucInterpRgbBuf to COMPUTE_SHADER_READ to match the state
    // the warp shader leaves it in (post-stage-3 barrier in the legacy chain).
    // Existing renderFrame / renderFrameSw expects this state when binding
    // it as a fragment shader input later.
    computeBufBarrier(m_FrucInterpRgbBuf);
    writeTs(4);  // β.4 timing: after bilinear UP ("warp" slot in GPU-PROF)

    return true;
}

// §J.3.e.2.i.10 Phase 2B step 5-6 (v1.4.56) — DUAL-only β.5.1 split helpers.
//
// These mirror runRifeNativeStage's β.5.1 DUAL path (single inference @
// t=0.5) sliced across 3 cmd buffers.  The renderFrame phase2BActive path
// records each helper into preCmd / cmpCmd / postCmd respectively and
// submits them as a chain (4 submits per frame counting the Path D copy
// submit), connected via m_ComputeTimelineSem.
//
// v1.4.56 ships ALL three submits on the graphics queue (no cross-queue
// hand-off yet) so this is purely a cmd-buf split validation step:
// equivalent to the legacy single-cmd-buf path with extra cmd-buf
// boundaries, expected bit-identical behaviour modulo NV driver cache
// flush noise.  v1.4.57 will retarget the cmpCmd submit to m_ComputeQueue
// — that's the step that actually parallelises with graphics work.

bool VkFrucRenderer::recordRifeDown(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                    uint32_t /*slotIdx*/)
{
    if (!m_RifeNativeReady || !m_RifeNative) return false;
    if (m_RifeBilinearPipeline == VK_NULL_HANDLE) return false;
    if (m_RifeDownPrev == VK_NULL_HANDLE
        || m_RifeDownCurr == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    auto pfnCmdBindPipeline    = (PFN_vkCmdBindPipeline)   getDevPa(m_Device, "vkCmdBindPipeline");
    auto pfnCmdBindDescSets    = (PFN_vkCmdBindDescriptorSets)getDevPa(m_Device, "vkCmdBindDescriptorSets");
    auto pfnCmdPushConst       = (PFN_vkCmdPushConstants)  getDevPa(m_Device, "vkCmdPushConstants");
    auto pfnCmdDispatch        = (PFN_vkCmdDispatch)       getDevPa(m_Device, "vkCmdDispatch");
    if (!pfnCmdPipelineBarrier || !pfnCmdBindPipeline || !pfnCmdBindDescSets
        || !pfnCmdPushConst || !pfnCmdDispatch) return false;

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
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };

    struct BilinearPC {
        int32_t inH, inW, outH, outW, channels;
        float   scaleH, scaleW;
    };
    auto dispatchBilinear = [&](VkDescriptorSet ds,
                                int inW, int inH, int outW, int outH) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeBilinearPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeBilinearPipeLay,
                           0, 1, &ds, 0, nullptr);
        BilinearPC pc;
        pc.inH = inH;  pc.inW = inW;  pc.outH = outH;  pc.outW = outW;
        pc.channels = 3;
        pc.scaleH = (float)inH / (float)outH;
        pc.scaleW = (float)inW / (float)outW;
        pfnCmdPushConst(cmd, m_RifeBilinearPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pc), &pc);
        pfnCmdDispatch(cmd, ((uint32_t)outW + 7) / 8, ((uint32_t)outH + 7) / 8, 3);
    };

    // m_FrucPrevRgbBuf / m_FrucCurrRgbBuf are in COMPUTE_SHADER_READ from
    // the Stage 0 (nv12rgb) computeBufBarrier emitted earlier in preCmd
    // (caller records nv12rgb just before calling us).  No additional
    // barrier needed before the bilinear DOWN reads them as SHADER_READ.
    dispatchBilinear(m_RifeBilinearDownPrevDs,
                     (int)width, (int)height,
                     m_RifeNativeInferW, m_RifeNativeInferH);
    dispatchBilinear(m_RifeBilinearDownCurrDs,
                     (int)width, (int)height,
                     m_RifeNativeInferW, m_RifeNativeInferH);

    // Promote DOWN outputs to TRANSFER_READ for runInferenceGpuFlow's
    // vkCmdCopyBuffer (in cmpCmd).  Cross-cmd-buf execution dependency is
    // provided by the timeline-sem chain on the submit boundary; this
    // barrier handles the cache flush + access-mask transition.
    bufBarrier(m_RifeDownPrev,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bufBarrier(m_RifeDownCurr,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return true;
}

bool VkFrucRenderer::recordRifeInferOnCompute(VkCommandBuffer cmd,
                                               uint32_t /*width*/, uint32_t /*height*/,
                                               uint32_t slotIdx)
{
    if (!m_RifeNativeReady || !m_RifeNative) return false;
    if (m_RifeFlowOutBuf == VK_NULL_HANDLE
        || m_RifeMaskOutBuf == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    if (!pfnCmdPipelineBarrier) return false;

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
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };

    // DUAL midpoint only — TRIPLE gated out of phase2BActive in v1.4.56.
    if (!m_RifeNative->runInferenceGpuFlow(cmd, slotIdx,
            m_RifeDownPrev, m_RifeDownCurr, 0.5f,
            m_RifeFlowOutBuf, m_RifeMaskOutBuf)) {
        return false;
    }
    // Inference left flow/mask in TRANSFER_WRITE (vkCmdCopyBuffer into them).
    // Promote to SHADER_READ for the warp shader (in postCmd).  Cross-cmd-buf
    // execution dep is the timeline sem chain; access-mask flush is here.
    bufBarrier(m_RifeFlowOutBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    bufBarrier(m_RifeMaskOutBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return true;
}

bool VkFrucRenderer::recordRifeWarp(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                    uint32_t /*slotIdx*/)
{
    if (!m_RifeNativeReady) return false;
    if (m_RifeNativeWarpPipeline == VK_NULL_HANDLE
        || m_RifeNativeWarpDs    == VK_NULL_HANDLE) return false;
    if (m_FrucInterpRgbBuf == VK_NULL_HANDLE) return false;

    auto getDevPa = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    auto pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)getDevPa(m_Device, "vkCmdPipelineBarrier");
    auto pfnCmdBindPipeline    = (PFN_vkCmdBindPipeline)   getDevPa(m_Device, "vkCmdBindPipeline");
    auto pfnCmdBindDescSets    = (PFN_vkCmdBindDescriptorSets)getDevPa(m_Device, "vkCmdBindDescriptorSets");
    auto pfnCmdPushConst       = (PFN_vkCmdPushConstants)  getDevPa(m_Device, "vkCmdPushConstants");
    auto pfnCmdDispatch        = (PFN_vkCmdDispatch)       getDevPa(m_Device, "vkCmdDispatch");
    if (!pfnCmdPipelineBarrier || !pfnCmdBindPipeline || !pfnCmdBindDescSets
        || !pfnCmdPushConst || !pfnCmdDispatch) return false;

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
        bmb.size = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 1, &bmb, 0, nullptr);
    };

    // m_FrucPrev/CurrRgbBuf left in COMPUTE_SHADER_READ by Stage 0 chain
    // (TRANSFER_READ promotion happens later in Stage 4 curr→prev copy).
    // Warp shader reads them as SHADER_READ — no transition needed, but we
    // still emit a memory barrier on m_FrucCurrRgbBuf to drain any stale
    // COMPUTE writes from cross-cmd-buf boundary.  m_FrucPrevRgbBuf is in
    // SHADER_READ from the previous frame's Stage 4 barrier (transfer
    // write → shader read) so it's already cache-coherent for this read.
    bufBarrier(m_FrucCurrRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // Promote m_FrucInterpRgbBuf to SHADER_WRITE.  Prior state can be
    // COMPUTE_SHADER_READ from compositor / TRANSFER_WRITE from prior
    // race / SHADER_READ — cover all with broad src masks.
    bufBarrier(m_FrucInterpRgbBuf,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);

    struct WarpPC {
        int32_t W, H, inferW, inferH;
        float   magScaleX, magScaleY;
        int32_t _pad0, _pad1;
    };
    WarpPC pcW;
    pcW.W = (int32_t)width;  pcW.H = (int32_t)height;
    pcW.inferW = m_RifeNativeInferW;  pcW.inferH = m_RifeNativeInferH;
    pcW.magScaleX = (float)width  / (float)pcW.inferW;
    pcW.magScaleY = (float)height / (float)pcW.inferH;
    pcW._pad0 = 0; pcW._pad1 = 0;

    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeNativeWarpPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_RifeNativeWarpPipeLay,
                       0, 1, &m_RifeNativeWarpDs, 0, nullptr);
    pfnCmdPushConst(cmd, m_RifeNativeWarpPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(pcW), &pcW);
    pfnCmdDispatch(cmd, ((uint32_t)width + 7) / 8, ((uint32_t)height + 7) / 8, 3);

    // Promote interp output to SHADER_READ for downstream fragment-shader
    // sample (interp render pass below in postCmd).
    bufBarrier(m_FrucInterpRgbBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return true;
}

// §J.3.e.2.i.26 (v1.4.88) — Cross-queue-family ownership transfer helpers.
//
// Vulkan 規格: 跨 queue family 共享 VK_SHARING_MODE_EXCLUSIVE buffer 必須
// release + acquire 對稱 pair.  release 在 src QF 的 cmd buf, acquire 在
// dst QF 的 cmd buf.  兩 barrier 要對齊 (srcQF / dstQF / srcAccess / dstAccess
// / pipeline stages).
//
// 用法: v1.4.89 把 FRUC chain 拆 cmd buf 時, currRgb (graphics→compute) 跟
// interp (compute→graphics) 都要走 release/acquire pair.
//
// 這 commit (v1.4.87 框架延續) 加 helper 但不接到實際路徑 — 等 v1.4.89.
void VkFrucRenderer::releaseBufferOwnership(VkCommandBuffer cmd, VkBuffer buf,
                                              uint32_t srcQueueFamily, uint32_t dstQueueFamily,
                                              VkAccessFlags srcAccess,
                                              VkPipelineStageFlags srcStage,
                                              VkPipelineStageFlags dstStage)
{
    if (srcQueueFamily == dstQueueFamily) return;  // 同 QF 不需 ownership transfer
    VkBufferMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = 0;  // release: dstAccessMask 必須 0
    b.srcQueueFamilyIndex = srcQueueFamily;
    b.dstQueueFamilyIndex = dstQueueFamily;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    m_RtPfn.CmdPipelineBarrier(cmd, srcStage, dstStage,
        0, 0, nullptr, 1, &b, 0, nullptr);
}

void VkFrucRenderer::acquireBufferOwnership(VkCommandBuffer cmd, VkBuffer buf,
                                              uint32_t srcQueueFamily, uint32_t dstQueueFamily,
                                              VkAccessFlags dstAccess,
                                              VkPipelineStageFlags srcStage,
                                              VkPipelineStageFlags dstStage)
{
    if (srcQueueFamily == dstQueueFamily) return;  // 同 QF 不需 ownership transfer
    VkBufferMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = 0;  // acquire: srcAccessMask 必須 0
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = srcQueueFamily;
    b.dstQueueFamilyIndex = dstQueueFamily;
    b.buffer = buf;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    m_RtPfn.CmdPipelineBarrier(cmd, srcStage, dstStage,
        0, 0, nullptr, 1, &b, 0, nullptr);
}

// §J.3.e.2.i.26 (v1.4.88) — Stub: FRUC chain record on compute cmd buf.
//
// 計畫: 跟 runFrucComputeChain 一樣的 dispatch 序列, 但寫進 cmpCmd 而非
// 主 render cmd, 且在開頭加 acquire ownership / 結尾加 release ownership.
// 本 commit 純 stub return false (gate 未啟動). v1.4.89 才填實作 + 接路徑.
bool VkFrucRenderer::recordFrucChainOnCompute(VkCommandBuffer /*cmpCmd*/,
                                                uint32_t /*width*/, uint32_t /*height*/,
                                                uint32_t /*slot*/)
{
    // v1.4.88 stub — v1.4.89 才實作真正 chain record + ownership transfer.
    return false;
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
                                          bool useNativeSrc, uint32_t slotIdx)
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

    // §J.3.e.2.i.21 (v1.4.83) — Env gate VIPLE_VKFRUC_FRUC_LEVEL 控制補幀層級.
    // 解碼延遲 vs 視覺品質 trade-off knob.
    //   0: v1.4.74 行為 (只 spatial gradient fade)
    //   1: v1.4.76 行為 (+ temporal coherence)
    //   2: v1.4.80 行為 (+ bidirectional ME + occlusion mask)
    //   3: v1.4.81 行為 (+ 4×4 adaptive ME) — v1.4.83 預設
    //   4: v1.4.82 行為 (+ 半像素 subpix; v1.4.83 已撤, 預留 enum)
    static const int s_FrucLevel =
        qEnvironmentVariableIsSet("VIPLE_VKFRUC_FRUC_LEVEL")
            ? qEnvironmentVariableIntValue("VIPLE_VKFRUC_FRUC_LEVEL") : 3;
    const bool levelHasTemporal = (s_FrucLevel >= 1);
    const bool levelHasBackward = (s_FrucLevel >= 2);
    const bool levelHasFine4x4  = (s_FrucLevel >= 3);

    // §J.3.e.2.i.6 / §J.3.g v2 — read PREVIOUS pass's 6 timestamps for THIS
    // slot (fence wait at start of renderFrameSw guarantees GPU finished),
    // compute 5 stage deltas + total, accumulate.  Skip on first iteration
    // when not yet armed.
    const uint32_t timerSlot = m_FrucTimerSlot;
    const uint32_t timerBase = timerSlot * 6;
    if (m_FrucTimerPool && pfnGetQueryPoolResults && pfnCmdResetQueryPool
        && pfnCmdWriteTimestamp && m_FrucTimerArmed[timerSlot]) {
        uint64_t ts[6] = {};
        VkResult qr = pfnGetQueryPoolResults(m_Device, m_FrucTimerPool,
            timerBase, 6, sizeof(ts), ts, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT);
        // ts[5] >= ts[0] is the only sanity check we need; intermediate
        // stages may be equal on some drivers when stage compresses to
        // ~0 ticks (e.g. Median copy on small mvBuf), and that's still
        // a valid measurement — record 0us for that stage.
        if (qr == VK_SUCCESS && ts[5] >= ts[0]) {
            double ns2us = m_FrucTimerNsPerTick / 1000.0;
            for (int i = 0; i < kFrucStageCount; ++i) {
                uint64_t d = (ts[i + 1] >= ts[i]) ? (ts[i + 1] - ts[i]) : 0;
                m_FrucGpuStageUsAccum[i] += (double)d * ns2us;
            }
            double thisFrameChainUs = (double)(ts[5] - ts[0]) * ns2us;
            m_FrucGpuUsAccum += thisFrameChainUs;
            m_FrucGpuUsCount++;

            // §J.3.e.2.i.22 (v1.4.84) — 動態 TRIPLE/DUAL 降階偵測.
            // 把這幀 chain time 寫進 60-frame ring, 算 mean.
            // env VIPLE_VKFRUC_DYNAMIC_TIER:
            //   0 = 完全關閉 (m_DynamicDualDowngrade 鎖在 false)
            //   1 = 自動 (預設)
            //   2 = 強制 DUAL (m_DynamicDualDowngrade 鎖在 true, debug)
            double thisFrameChainMs = thisFrameChainUs / 1000.0;
            m_ChainMeanMsRing[m_ChainMeanMsRingIdx] = thisFrameChainMs;
            m_ChainMeanMsRingIdx = (m_ChainMeanMsRingIdx + 1) % kChainRingSize;
            if (m_ChainMeanMsRingFilled < kChainRingSize) m_ChainMeanMsRingFilled++;

            static const int s_DynamicTierMode =
                qEnvironmentVariableIsSet("VIPLE_VKFRUC_DYNAMIC_TIER")
                    ? qEnvironmentVariableIntValue("VIPLE_VKFRUC_DYNAMIC_TIER") : 1;
            if (s_DynamicTierMode == 2) {
                // 強制 DUAL (debug)
                m_DynamicDualDowngrade.store(true);
            } else if (s_DynamicTierMode == 1 && m_ChainMeanMsRingFilled >= kChainRingSize) {
                double sum = 0.0;
                for (int i = 0; i < kChainRingSize; ++i) sum += m_ChainMeanMsRing[i];
                double meanMs = sum / kChainRingSize;
                const bool currentlyDowngraded = m_DynamicDualDowngrade.load();
                if (meanMs > 5.0 && !currentlyDowngraded) {
                    if (++m_FramesAboveThreshold >= 30) {
                        m_DynamicDualDowngrade.store(true);
                        m_FramesAboveThreshold = 0;
                        m_FramesBelowThreshold = 0;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-AUTOTIER] TRIPLE → DUAL (chain mean=%.2fms > 5ms 連續 30 幀)",
                            meanMs);
                    }
                } else if (meanMs < 3.0 && currentlyDowngraded) {
                    if (++m_FramesBelowThreshold >= 60) {
                        m_DynamicDualDowngrade.store(false);
                        m_FramesAboveThreshold = 0;
                        m_FramesBelowThreshold = 0;
                        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-AUTOTIER] DUAL → TRIPLE (chain mean=%.2fms < 3ms 連續 60 幀)",
                            meanMs);
                    }
                } else {
                    // 反向計數 reset (避免遠端 spike 累積太久觸發誤切)
                    if (meanMs <= 5.0) m_FramesAboveThreshold = 0;
                    if (meanMs >= 3.0) m_FramesBelowThreshold = 0;
                }
            } else if (s_DynamicTierMode == 0) {
                m_DynamicDualDowngrade.store(false);
            }
        }
    }
    // Reset 6 queries for this slot before re-using.
    if (m_FrucTimerPool && pfnCmdResetQueryPool) {
        pfnCmdResetQueryPool(cmd, m_FrucTimerPool, timerBase, 6);
    }
    m_FrucTimerSlot = (m_FrucTimerSlot + 1) % kFrucFramesInFlight;

    const uint32_t mvW = m_FrucMvWidth;
    const uint32_t mvH = m_FrucMvHeight;
    const uint32_t BLOCK_SIZE = 8;  // §B-quality (b) reverted — block 16 在純水平 motion 引入強烈 Y 軸抖動
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

    // §J.3.e.2.i.6 / §J.3.g v2 — write chain_start timestamp (ts[0])
    // BEFORE first dispatch.  Intermediate timestamps follow each stage's
    // bufBarrier, the chain end timestamp goes after the curr→prev copy
    // barrier (see end of function).
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
    // §J.3.g v2 ts[1] — after NV12→RGB barrier
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 1);
    }

    // §J.3.e.X Path β.2 — RIFE inference replaces ME→median→warp.  Runs in
    // place of stages 1+2+3 when m_RifeNativeReady; failure falls through
    // to the existing block-match path (so RIFE init issues never crater
    // the chain).  Stage 4 (curr→prev copy) still runs at the bottom of
    // this function.
    bool rifeHandled = false;
    if (m_RifeNativeReady) {
        if (runRifeNativeStage(cmd, width, height, slotIdx)) {
            rifeHandled = true;
            // β.4 timing — runRifeNativeStage writes ts[2..4] internally
            // at meaningful points (down / RIFE / up).  [VIPLE-VKFRUC-GPU-PROF]
            // labels are nv12rgb / me / median / warp / copy: when β path
            // is active interpret as nv12rgb / DOWN / RIFE / UP / copy.
            static std::atomic<bool> s_rifeChainLogged{false};
            bool exp = false;
            if (s_rifeChainLogged.compare_exchange_strong(exp, true)) {
                auto lt = m_RifeNative->lastTiming();
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-RIFE-β] chain swap active — RIFE replaces "
                    "ME/median/warp (last: seed=%.1fms record=%.1fms gpuWait=%.1fms "
                    "readback=%.1fms; β.2 GPU-resident path = no readback)",
                    lt.seedMs, lt.recordMs, lt.gpuWaitMs, lt.readbackMs);
            }
        } else {
            // First-frame failure (e.g. model shape mismatch at this dim)
            // → permanently disable RIFE for this session to stop log spam
            // and avoid wasted dispatch attempts.  Block-match takes over
            // for subsequent frames.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-RIFE-β] runRifeNativeStage failed at frame#%llu "
                "— DISABLING Path β for this session, ME/median/warp takes "
                "over (likely model shape mismatch; try smaller "
                "VIPLE_VKFRUC_RIFE_INFER_DIM)",
                (unsigned long long)m_FrucFrameCount);
            m_RifeNativeReady = false;
        }
    }

    if (!rifeHandled) {
    // §B-NVOF Phase 4d 2026-05-06 — when NV optical flow chain is ready and
    // we have at least 1 prior OF execute (m_NvOfTimelineValue > 0), replace
    // block-matching Stage 1+2 with HW OF result consumption:
    //   1) vkCmdCopyImageToBuffer m_NvOfFlowImage → m_NvOfFlowStaging
    //   2) format converter compute (SFIXED5 → Q1 + 2×2 average) writes
    //      Q1 int2 to m_FrucMvFilteredBuf — same buffer Stage 2 (median)
    //      writes, so Stage 3 warp consumes unchanged.
    // Note: the flow data is from the PREVIOUS frame's OF execute (1-frame
    // async lag, kicked off after main submit).  CPU vkWaitSemaphores at
    // end of last frame ensures m_NvOfFlowImage is stable before this read.
    const bool useNvOf = m_NvOfReady
        && m_FrucNvOfConvertPipeline != VK_NULL_HANDLE
        && m_FrucNvOfConvertDescSet  != VK_NULL_HANDLE
        && m_NvOfTimelineValue > 0;

    if (useNvOf) {
        const uint32_t flowW = width  / m_NvOfGridSize;
        const uint32_t flowH = height / m_NvOfGridSize;

        // Image layout: SDK leaves m_NvOfFlowImage in GENERAL after exec
        // (most NV drivers); transition GENERAL → TRANSFER_SRC for the copy.
        VkImageMemoryBarrier flowToSrc = {};
        flowToSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        flowToSrc.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;  // OF write
        flowToSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        flowToSrc.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        flowToSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        flowToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        flowToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        flowToSrc.image               = m_NvOfFlowImage;
        flowToSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        flowToSrc.subresourceRange.levelCount = 1;
        flowToSrc.subresourceRange.layerCount = 1;
        pfnCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &flowToSrc);

        // Copy flow image → staging buffer (4 bytes per pixel = R16G16).
        auto pfnCmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)getDevPa(
            m_Device, "vkCmdCopyImageToBuffer");
        if (pfnCmdCopyImageToBuffer) {
            VkBufferImageCopy reg = {};
            reg.bufferOffset      = 0;
            reg.bufferRowLength   = flowW;
            reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            reg.imageSubresource.layerCount = 1;
            reg.imageExtent       = { flowW, flowH, 1 };
            pfnCmdCopyImageToBuffer(cmd, m_NvOfFlowImage,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                m_NvOfFlowStaging, 1, &reg);
        }

        // Image back to GENERAL for next OF execute write.
        VkImageMemoryBarrier flowToGeneral = flowToSrc;
        flowToGeneral.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        flowToGeneral.dstAccessMask = 0;
        flowToGeneral.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        flowToGeneral.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        pfnCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &flowToGeneral);

        // Staging buffer TRANSFER_WRITE → SHADER_READ for converter.
        VkBufferMemoryBarrier stagingBar = {};
        stagingBar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        stagingBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        stagingBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        stagingBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        stagingBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        stagingBar.buffer = m_NvOfFlowStaging;
        stagingBar.size   = VK_WHOLE_SIZE;
        pfnCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &stagingBar, 0, nullptr);

        // Dispatch converter compute: read staging, write Q1 to mv_filtered.
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNvOfConvertPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNvOfConvertPipeLay,
                           0, 1, &m_FrucNvOfConvertDescSet, 0, nullptr);
        {
            struct { uint32_t flowW, flowH, mvW, mvH; } pcConv = {
                flowW, flowH, mvW, mvH
            };
            pfnCmdPushConst(cmd, m_FrucNvOfConvertPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcConv), &pcConv);
        }
        pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
        computeBufBarrier(m_FrucMvFilteredBuf);

        // Both ts[2] and ts[3] still need to be written so per-stage timing
        // in [VIPLE-VKFRUC-GPU-PROF] remains valid (stage 1 & 2 deltas will
        // be 0 / converter time, but at least timestamps stay coherent).
        if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
            pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 m_FrucTimerPool, timerBase + 2);
            pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 m_FrucTimerPool, timerBase + 3);
        }
        static std::atomic<bool> s_nvofChainLogged{false};
        bool exp = false;
        if (s_nvofChainLogged.compare_exchange_strong(exp, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] chain consumes HW OF flow this frame "
                        "(flowDims=%ux%u → mv=%ux%u via %ux%u avg + SFIXED5→Q1)",
                        flowW, flowH, mvW, mvH, flowW / mvW, flowH / mvH);
        }
    } else {
    // ---- Stage 0b: Pyramid ½ downscale + ½ res ME (§J.3.e.2.i.23, v1.4.85) ----
    // 開啟條件: levelHasFine4x4 (≡ 預設 level 3+); 因為 pyramid 是 chain-saving 招式,
    // 在更低 level 不會啟用 (反正那些 level chain 也輕).
    // env VIPLE_VKFRUC_PYRAMID=0 完全關閉 pyramid (debug).
    static const int s_PyramidEnabled =
        qEnvironmentVariableIsSet("VIPLE_VKFRUC_PYRAMID")
            ? qEnvironmentVariableIntValue("VIPLE_VKFRUC_PYRAMID") : 1;
    const bool usePyramid = (s_PyramidEnabled != 0) && levelHasFine4x4;
    if (usePyramid) {
        // §J.3.e.2.i.23 Stage 0b.1: downscale prev RGB → halfPrev
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucDownscaleHalfPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucDownscaleHalfPipeLay,
                           0, 1, &m_FrucDownscaleHalfPrevDescSet, 0, nullptr);
        {
            struct {
                uint32_t inWidth, inHeight, outWidth, outHeight;
            } pcDs = { (uint32_t)width, (uint32_t)height, (uint32_t)(width / 2), (uint32_t)(height / 2) };
            pfnCmdPushConst(cmd, m_FrucDownscaleHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcDs), &pcDs);
        }
        pfnCmdDispatch(cmd, (width / 2 + 7) / 8, (height / 2 + 7) / 8, 1);
        computeBufBarrier(m_FrucPrevRgbHalfBuf);

        // §J.3.e.2.i.23 Stage 0b.2: downscale curr RGB → halfCurr (reuse same pipeline)
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucDownscaleHalfPipeLay,
                           0, 1, &m_FrucDownscaleHalfCurrDescSet, 0, nullptr);
        {
            struct {
                uint32_t inWidth, inHeight, outWidth, outHeight;
            } pcDs = { (uint32_t)width, (uint32_t)height, (uint32_t)(width / 2), (uint32_t)(height / 2) };
            pfnCmdPushConst(cmd, m_FrucDownscaleHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcDs), &pcDs);
        }
        pfnCmdDispatch(cmd, (width / 2 + 7) / 8, (height / 2 + 7) / 8, 1);
        computeBufBarrier(m_FrucCurrRgbHalfBuf);

        // §J.3.e.2.i.24 (v1.4.86) 3-stage Pyramid Stage 0b.3: ½→¼ downscale prev/curr
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucDownscaleHalfPipeLay,
                           0, 1, &m_FrucDownscaleHalfPrevQuarterDescSet, 0, nullptr);
        {
            struct {
                uint32_t inWidth, inHeight, outWidth, outHeight;
            } pcDs = { (uint32_t)(width / 2), (uint32_t)(height / 2),
                       (uint32_t)(width / 4), (uint32_t)(height / 4) };
            pfnCmdPushConst(cmd, m_FrucDownscaleHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcDs), &pcDs);
        }
        pfnCmdDispatch(cmd, (width / 4 + 7) / 8, (height / 4 + 7) / 8, 1);
        computeBufBarrier(m_FrucPrevRgbQuarterBuf);

        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucDownscaleHalfPipeLay,
                           0, 1, &m_FrucDownscaleHalfCurrQuarterDescSet, 0, nullptr);
        {
            struct {
                uint32_t inWidth, inHeight, outWidth, outHeight;
            } pcDs = { (uint32_t)(width / 2), (uint32_t)(height / 2),
                       (uint32_t)(width / 4), (uint32_t)(height / 4) };
            pfnCmdPushConst(cmd, m_FrucDownscaleHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcDs), &pcDs);
        }
        pfnCmdDispatch(cmd, (width / 4 + 7) / 8, (height / 4 + 7) / 8, 1);
        computeBufBarrier(m_FrucCurrRgbQuarterBuf);

        // §J.3.e.2.i.24 Stage 0b.4: ¼ res forward ME → mvQuarter
        // (reuse MEHalf pipeline; hasCoarserMvPredictor=0 全範圍 [16,8,4,2,1])
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMeHalfPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMeHalfPipeLay,
                           0, 1, &m_FrucMeQuarterDescSet, 0, nullptr);
        {
            struct {
                uint32_t halfFrameWidth, halfFrameHeight, blockSize, halfMvWidth, halfMvHeight, hasCoarserMvPredictor;
            } pcMeQ = {
                (uint32_t)(width / 4), (uint32_t)(height / 4), (uint32_t)BLOCK_SIZE,
                (uint32_t)(mvW / 4), (uint32_t)(mvH / 4), 0u  // ¼ res 沒有 coarser predictor
            };
            pfnCmdPushConst(cmd, m_FrucMeHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcMeQ), &pcMeQ);
        }
        pfnCmdDispatch(cmd, (mvW / 4 + 7) / 8, (mvH / 4 + 7) / 8, 1);
        computeBufBarrier(m_FrucMvQuarterBuf);

        // §J.3.e.2.i.23 Stage 0b.5: ½ res forward ME → mvHalf (用 ¼ MV 當 predictor)
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMeHalfPipeLay,
                           0, 1, &m_FrucMeHalfDescSet, 0, nullptr);
        {
            struct {
                uint32_t halfFrameWidth, halfFrameHeight, blockSize, halfMvWidth, halfMvHeight, hasCoarserMvPredictor;
            } pcMeHalf = {
                (uint32_t)(width / 2), (uint32_t)(height / 2), (uint32_t)BLOCK_SIZE,
                (uint32_t)(mvW / 2), (uint32_t)(mvH / 2), 1u  // ½ res 用 ¼ MV 當 predictor
            };
            pfnCmdPushConst(cmd, m_FrucMeHalfPipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcMeHalf), &pcMeHalf);
        }
        pfnCmdDispatch(cmd, (mvW / 2 + 7) / 8, (mvH / 2 + 7) / 8, 1);
        computeBufBarrier(m_FrucMvHalfBuf);
    }

    // ---- Stage 1: motion estimation ----
    // Push constant layout MUST match shader's struct order exactly:
    //   ME shader (plvk.cpp:1275-1282): frameWidth, frameHeight, blockSize,
    //   mvWidth, mvHeight, hasHalfMvPredictor  — 28 bytes total
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMePipeLay,
                       0, 1, &m_FrucMeDescSet, 0, nullptr);
    {
        struct {
            uint32_t frameWidth, frameHeight, blockSize, mvWidth, mvHeight, hasHalfMvPredictor;
        } pcME = {
            (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
            (uint32_t)mvW, (uint32_t)mvH, (uint32_t)(usePyramid ? 1u : 0u)
        };
        pfnCmdPushConst(cmd, m_FrucMePipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcME), &pcME);
    }
    pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
    computeBufBarrier(m_FrucMvBuf);
    // §J.3.g v2 ts[2] — after ME barrier
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 2);
    }

    // ---- Stage 1b: Backward motion estimation (§J.3.e.2.i.17, v1.4.82) ----
    // 用 raw forward MV (m_FrucMvBuf) 當 predictor 跑 curr→prev backward ME.
    // §J.3.e.2.i.21 (v1.4.83) — 受 levelHasBackward 控制; level <= 1 跳過.
    if (levelHasBackward) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMeBackwardPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMeBackwardPipeLay,
                           0, 1, &m_FrucMeBackwardDescSet, 0, nullptr);
        {
            struct {
                uint32_t frameWidth, frameHeight, blockSize, mvWidth, mvHeight, _pad0;
            } pcMeBwd = {
                (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
                (uint32_t)mvW, (uint32_t)mvH, 0
            };
            pfnCmdPushConst(cmd, m_FrucMeBackwardPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcMeBwd), &pcMeBwd);
        }
        pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
        computeBufBarrier(m_FrucMvBackwardBuf);
    }

    // ---- Stage 2: MV median filter ----
    // §J.3.e.2.i.7 R5 had this as a noop cmdCopyBuffer experiment,
    // §B-quality (a) 2026-05-06 — re-enabled real 3×3 median dispatch.
    // baseline_b1b_video showed OF 30Hz alternation = 5.95% (threshold
    // 3% = "strong"), SSIM 30Hz = 5.40%; main suspect is ME's outlier
    // MVs (block matching produces noisy MVs at low-texture / occluded
    // regions).  Median smoothes those out before warp consumes them,
    // making interp frame visually closer to the real frame in motion
    // pattern.  GPU cost ≈ 0.2-0.3 ms (negligible vs 1.0 ms total
    // chain).  See plvk.cpp:1482 kFrucMvMedianShaderGlsl for shader
    // (3×3 9-element sorting network, local_size 8×8, edge-clamp
    // sampling, fast-path for uniform / near-uniform neighbourhoods).
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeline);
    pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeLay,
                       0, 1, &m_FrucMedianDescSet, 0, nullptr);
    {
        struct {
            uint32_t mvWidth, mvHeight, _pad0, _pad1;
        } pcMed = { mvW, mvH, 0, 0 };
        pfnCmdPushConst(cmd, m_FrucMedianPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pcMed), &pcMed);
    }
    pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
    computeBufBarrier(m_FrucMvFilteredBuf);

    // ---- Stage 2b: Backward MV median (§J.3.e.2.i.17, v1.4.82) ----
    // §J.3.e.2.i.21 (v1.4.83) — 受 levelHasBackward 控制 (跟 bwdME 配對).
    if (levelHasBackward) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucMedianPipeLay,
                           0, 1, &m_FrucMedianBackwardDescSet, 0, nullptr);
        {
            struct {
                uint32_t mvWidth, mvHeight, _pad0, _pad1;
            } pcMedBwd = { mvW, mvH, 0, 0 };
            pfnCmdPushConst(cmd, m_FrucMedianPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcMedBwd), &pcMedBwd);
        }
        pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
        computeBufBarrier(m_FrucMvBackwardFilteredBuf);
    }

    // ---- Stage 2c+d: Phase B flag pre-pass + 4×4 fine ME (§J.3.e.2.i.19) ----
    // §J.3.e.2.i.21 (v1.4.83) — 兩個都受 levelHasFine4x4 控制; level <= 2 跳過.
    // 需要 bwdMV 才能算 flag (consistency error) — 兩個 stage 同 gate.
    if (levelHasFine4x4 && levelHasBackward) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucFlagPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucFlagPipeLay,
                           0, 1, &m_FrucFlagDescSet, 0, nullptr);
        {
            struct {
                uint32_t mvWidth, mvHeight, _pad0, _pad1;
            } pcFlag = { mvW, mvH, 0, 0 };
            pfnCmdPushConst(cmd, m_FrucFlagPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcFlag), &pcFlag);
        }
        pfnCmdDispatch(cmd, (mvW + 7) / 8, (mvH + 7) / 8, 1);
        computeBufBarrier(m_FrucRefineFlagBuf);

        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucFine4x4Pipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucFine4x4PipeLay,
                           0, 1, &m_FrucFine4x4DescSet, 0, nullptr);
        {
            struct {
                uint32_t frameWidth, frameHeight, coarseBlockSize, coarseMvWidth, coarseMvHeight, _pad0;
            } pcFine = {
                (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
                (uint32_t)mvW, (uint32_t)mvH, 0
            };
            pfnCmdPushConst(cmd, m_FrucFine4x4PipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcFine), &pcFine);
        }
        pfnCmdDispatch(cmd, (mvW * 2 + 7) / 8, (mvH * 2 + 7) / 8, 1);
        computeBufBarrier(m_FrucMvFineBuf);
    }

    // §J.3.g v2 ts[3] — after all Stage-2 work (median fwd + median bwd + flag + fine4x4)
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 3);
    }
    }  // end if (useNvOf) ... else block-match path

    // ---- Stage 3: warp ----
    // Push constant layout MUST match shader's struct order exactly:
    //   Warp shader (plvk.cpp:1574-1593): frameWidth, frameHeight,
    //   mvBlockSize, mvWidth, mvHeight, blendFactor, tFraction, _pcPad0
    //   — 32 bytes total.
    //   tFraction=0.5 = DUAL midpoint, 1/3 + 2/3 = TRIPLE 兩張 interp.
    //
    // §B-quality 2026-05-06 — blendFactor 分支：
    //   > 1.5   → c2 no-MV cross-fade
    //   < -1.5  → c1 Quality adaptive
    //   < 0     → Balanced cheap-adaptive (default)
    //   >= 0    → c0 fixed blend (用該值當 mix weight)
    // Env var: NO_MV > QUALITY > PURE50 > default.
    float blendFactor;
    const char* modeName = "Balanced cheap-adaptive";
    if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_WARP_NO_MV") != 0) {
        blendFactor = 2.0f;
        modeName = "c2 no-MV (DIAG)";
    } else if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_WARP_QUALITY") != 0) {
        blendFactor = -2.0f;
        modeName = "c1 Quality adaptive";
    } else if (qEnvironmentVariableIntValue("VIPLE_VKFRUC_WARP_PURE50") != 0) {
        blendFactor = 0.5f;
        modeName = "c0 fixed 50/50";
    } else {
        blendFactor = -1.0f;
    }
    static std::atomic<bool> s_warpModeLogged{false};
    bool warpExpected = false;
    if (s_warpModeLogged.compare_exchange_strong(warpExpected, true)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-WARP-MODE] blendFactor=%.1f mode='%s' triple=%d",
                    blendFactor, modeName, m_TripleMode ? 1 : 0);
    }

    auto dispatchWarp = [&](VkDescriptorSet descSet, VkBuffer outputBuf, float tFrac) {
        pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeline);
        pfnCmdBindDescSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucWarpPipeLay,
                           0, 1, &descSet, 0, nullptr);
        // §J.3.e.2.i.21 (v1.4.83) — push const flags 跟著 s_FrucLevel 走.
        // level 0: all 0u (純 v1.4.74 spatial gradient fade)
        // level 1: temporal=1, backward=0, fine=0
        // level 2: temporal=1, backward=1, fine=0
        // level 3+: 全 1 (預設)
        struct {
            uint32_t frameWidth, frameHeight, mvBlockSize, mvWidth, mvHeight;
            float    blendFactor;
            float    tFraction;
            uint32_t hasTemporalMv;
            uint32_t hasBackwardMv;
            uint32_t hasFineMv;
        } pcWarp = {
            (uint32_t)width, (uint32_t)height, (uint32_t)BLOCK_SIZE,
            (uint32_t)mvW, (uint32_t)mvH, blendFactor, tFrac,
            (uint32_t)(levelHasTemporal ? 1u : 0u),
            (uint32_t)(levelHasBackward ? 1u : 0u),
            (uint32_t)(levelHasFine4x4  ? 1u : 0u)
        };
        pfnCmdPushConst(cmd, m_FrucWarpPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                        0, sizeof(pcWarp), &pcWarp);
        // Warp shader local_size = 8x8, one thread per pixel.
        pfnCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
        computeBufBarrier(outputBuf);
    };

    // §J.3.e.2.i.22 (v1.4.84) — effective TRIPLE 受動態降階旁路.
    // 若 m_DynamicDualDowngrade=true (chain 過重觸發)，這幀降回 DUAL 模式:
    // 只 dispatch 1 個 warp (中點 t=0.5)，省 1 個 warp time + 1 個 swapchain
    // image acquire/present cycle (present logic 那邊也判同 flag).
    const bool effectiveTriple = m_TripleMode && !m_DynamicDualDowngrade.load();
    if (effectiveTriple) {
        // §B2 — TRIPLE 兩個 interp 點: 1/3 → InterpRgbBuf, 2/3 → InterpRgbBuf2.
        dispatchWarp(m_FrucWarpDescSet,  m_FrucInterpRgbBuf,  1.0f / 3.0f);
        dispatchWarp(m_FrucWarpDescSet2, m_FrucInterpRgbBuf2, 2.0f / 3.0f);
    } else {
        // DUAL midpoint.
        dispatchWarp(m_FrucWarpDescSet,  m_FrucInterpRgbBuf,  0.5f);
    }
    // §J.3.g v2 ts[4] — after Warp barrier (TRIPLE 是兩次 dispatch 後的 barrier).
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 4);
    }
    }  // end if (!rifeHandled) — Path β.2 chain branch

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

    // §J.3.e.2.i.15 (v1.4.80) — mvFiltered → prevMv for next frame's
    // temporal-coherence fade in warp shader (binding 4).  Also wakes up the
    // forward ME shader's temporal predictor which already reads prevMv.
    // Cost ~5-10us (256KB transfer at sizeMV=240*135*2*4=259KB).
    bufBarrier(m_FrucMvFilteredBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    bufBarrier(m_FrucPrevMvBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy mvCpyRegion = {};
    mvCpyRegion.srcOffset = 0;
    mvCpyRegion.dstOffset = 0;
    mvCpyRegion.size      = (VkDeviceSize)mvW * mvH * 2 * sizeof(int);
    pfnCmdCopyBuffer(cmd, m_FrucMvFilteredBuf, m_FrucPrevMvBuf, 1, &mvCpyRegion);
    bufBarrier(m_FrucPrevMvBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // §J.3.e.2.i.6 / §J.3.g v2 ts[5] — chain_end timestamp AFTER curr→prev
    // copy barrier.  This + ts[0] is the chain total; per-stage deltas are
    // ts[i+1] - ts[i].
    if (m_FrucTimerPool && pfnCmdWriteTimestamp) {
        pfnCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             m_FrucTimerPool, timerBase + 5);
        m_FrucTimerArmed[timerSlot] = true;
    }

    // §B-DUMP 2026-05-07 — diagnostic frame dump (after warmup elapsed).
    // Records cmdCopyBuffer ops into THIS cmd buffer; staging fills happen
    // GPU-side without blocking render thread.  Memcpy + queue push happen
    // at next reuse of slot S (existing fence wait — zero extra sync).
    //
    // m_FrucCurrRgbBuf is already in TRANSFER_READ state (line 5770 barrier
    // for the curr→prev copy).  Interp bufs are in COMPUTE_SHADER_WRITE,
    // need a TRANSFER_READ barrier before we copy.  Both reads independent
    // of downstream FRAGMENT_SHADER_READ in renderFrame's interp barrier
    // (no write hazard — both are reads of same write).
    //
    // §J.3.e.X Path β: dump path runs unchanged under Path β too — empirically
    // verified at v3 (256×128) and v4 (512×256) that capturing 20 frames
    // works fine and produces real RIFE midpoint output.  The barriers below
    // (COMPUTE_SHADER_BIT/SHADER_WRITE → TRANSFER_BIT/TRANSFER_READ on interp
    // buffers) overlap with Path β's actual SHADER_READ state but NV driver
    // tolerates the access-mask mismatch on read-after-write transitions.
    // The earlier "Path β + dump-skip → device lost after 8s" turned out to
    // be CAUSED by skipping the dump (probably the dump's barriers were
    // serving as additional COMPUTE→FRAGMENT sync that the SW path's render
    // pass subpass dep relies on); with dump active, all stable.
    if (m_DumpEnabled
        && slotIdx < kFrucFramesInFlight
        && m_DumpFramesQueued < m_DumpFramesTotal) {
        using namespace std::chrono;
        int64_t nowMs = duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
        if (nowMs - m_DumpSessionStartMs >= m_DumpDelayMs) {
            // Barrier interp bufs from compute_write → transfer_read.
            bufBarrier(m_FrucInterpRgbBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
            if (m_TripleMode) {
                bufBarrier(m_FrucInterpRgbBuf2,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            }
            // Copy to per-slot staging.  Size matches m_FrucCurrRgbBuf alloc
            // = width*height*3*fp32 = m_DumpStagingSize.
            VkBufferCopy dumpRegion = {};
            dumpRegion.size = m_DumpStagingSize;
            pfnCmdCopyBuffer(cmd, m_FrucCurrRgbBuf,
                             m_DumpStagingReal[slotIdx],    1, &dumpRegion);
            pfnCmdCopyBuffer(cmd, m_FrucInterpRgbBuf,
                             m_DumpStagingInterp1[slotIdx], 1, &dumpRegion);
            if (m_TripleMode) {
                pfnCmdCopyBuffer(cmd, m_FrucInterpRgbBuf2,
                                 m_DumpStagingInterp2[slotIdx], 1, &dumpRegion);
            }
            // §B-DUMP MV — also copy m_FrucMvFilteredBuf for diagnostic.
            // It's in COMPUTE_SHADER_READ state from warp; we need TRANSFER_READ.
            bufBarrier(m_FrucMvFilteredBuf,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy mvRegion = {};
            mvRegion.size = m_DumpStagingMvSize;
            pfnCmdCopyBuffer(cmd, m_FrucMvFilteredBuf,
                             m_DumpStagingMv[slotIdx], 1, &mvRegion);
            // Record what's pending in this slot for memcpy at next reuse.
            m_DumpSlotRec[slotIdx] = {
                (int)m_DumpFramesQueued, m_TripleMode
            };
            ++m_DumpFramesQueued;
            if (!m_DumpStarted) {
                m_DumpStarted = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-DUMP] capture started — slot=%u "
                    "(elapsed %lldms ≥ delay %lldms)",
                    slotIdx, (long long)(nowMs - m_DumpSessionStartMs),
                    (long long)m_DumpDelayMs);
            }
            if (m_DumpFramesQueued == m_DumpFramesTotal) {
                // Drain ring (flush slots that still have pending records).
                m_DumpCleanupExtra = (int)kFrucFramesInFlight;
            }
        }
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
        // §J.3.e.2.i.9 — at kFrucFramesInFlight=4 this single-present path
        // can re-trigger VUID-vkQueueSubmit-pSignalSemaphores-00067 because
        // it still uses per-slot m_SlotRenderDoneSem (signal/wait pair below),
        // not the per-image m_SwapchainRenderDoneSem the production DUAL path
        // moved to in Phase 1.5c-final.  Production never sets this env var;
        // if you need DIAG with the bumped ring count, port the per-image
        // sem pattern down here first.
        static_assert(kFrucFramesInFlight <= 4,
                      "DIAG_NOAVVKFRAME path uses per-slot renderDone sem; "
                      "see vkfruc.h kFrucFramesInFlight comment.");

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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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

    // §B-DUMP — fence signaled = GPU finished writing m_DumpStaging*[slot]
    // for the prior use of this slot.  Convert + push to writer thread now,
    // before next cmdbuf record may overwrite staging.  Cheap when no
    // dump pending or m_DumpEnabled == false.
    flushDumpSlotIfPending(slot);

    // ---- 2. Destroy slot's pending image view from prior frame ----
    if (m_SlotPendingView[slot] != VK_NULL_HANDLE) {
        m_RtPfn.DestroyImageView(m_Device, m_SlotPendingView[slot], nullptr);
        m_SlotPendingView[slot] = VK_NULL_HANDLE;
    }

    // ---- 3. Acquire next swapchain image(s) ----
    // §B1b 2026-05-06 — dualPresentThisFrame controls whether we acquire
    // 1 or 2 swapchain images.  Mirrors renderFrameSw line ~5511-5540.
    // §B2 2026-05-06 — triplePresentThisFrame extends to 3 images for
    // 60→180 補幀×3.  Strictly subset of dualPresentThisFrame.
    //   imgIdxA = interp_1 slot (1/3 點 in TRIPLE, midpoint in DUAL)
    //   imgIdxB = real frame slot (always)
    //   imgIdxC = interp_2 slot (only in TRIPLE, 2/3 點)
    const bool dualPresentThisFrame = m_DualMode && m_FrucMode && m_FrucReady
                                      && !m_FRUCPaused.load();
    // §J.3.e.2.i.22 (v1.4.84) — present 端也對齊動態降階,
    // 否則 swapchain 仍 acquire 3 張 image 但 interp_2 內容是舊資料.
    const bool triplePresentThisFrame = dualPresentThisFrame && m_TripleMode
                                        && !m_DynamicDualDowngrade.load();
    // §J.3.e.2.i.10f Path D — early AVFrame release.  Decide if we'll split
    // renderFrame into two graphics-queue submits (1: image→buffer copy +
    // signal vkf->sem early so FFmpeg pool can reuse pool image after just
    // copy; 2: chain + presents with no vkf->img[0] access).  Only safe when
    // the real-frame display path uses our m_FrucCurrRgbBuf via the
    // m_RealCurrRgbDescSet (VIPLE_VKFRUC_REAL_USE_CRGB default ON in
    // DUAL+FRUC mode).  Single-present + ycbcr-sampler path needs vkf
    // alive whole cmd buffer for the fragment shader.
    QByteArray rcrgbEnvEarly = qgetenv("VIPLE_VKFRUC_REAL_USE_CRGB");
    const bool rcrgbOn = rcrgbEnvEarly.isEmpty() ? true : (rcrgbEnvEarly.toInt() != 0);
    const bool useCrgbForReal = dualPresentThisFrame && m_FrucReady && rcrgbOn;
    const bool pathDActive = useCrgbForReal
                          && m_FrucMode && frame && frame->width > 0 && frame->height > 0
                          && m_SlotCopyCmdBuf[slot] != VK_NULL_HANDLE
                          && m_CopyDoneSem != VK_NULL_HANDLE;
    // §J.3.e.2.i.10 Phase 2B step 5-6 (v1.4.56) — opt-in 3-cmd-buf split
    // path.  Conditions (ALL must hold; any false → fallback to v1.4.55
    // single-cmd path bit-identical):
    //   • VIPLE_RIFE_VK_ASYNC_COMPUTE=1 (m_AsyncComputeRequested)
    //   • Phase 2A async-compute infra fully READY (m_AsyncComputeAvailable)
    //   • Path D copy submit active (pathDActive — needed because preCmd
    //     skips image→buffer copy, copy submit owns it)
    //   • DUAL present mode WITHOUT triple (TRIPLE adds a 2nd inference at
    //     t=2/3 the cmpCmd split currently doesn't record; gate-excluded
    //     in v1.4.56, may relax in v1.4.57+)
    //   • RIFE native β.5.1 path is the inference engine (m_Beta5Enabled
    //     + m_RifeNativeReady + warp pipeline/descset ready) — β.4
    //     bilinear-up fallback isn't split in this version
    //   • Per-slot pre/post + ComputeCmdBuf + timeline sem all allocated
    const bool phase2BActive = m_AsyncComputeRequested
                             && m_AsyncComputeAvailable
                             && pathDActive
                             && dualPresentThisFrame
                             && !triplePresentThisFrame
                             && m_Beta5Enabled
                             && m_RifeNativeReady
                             && m_RifeNativeWarpPipeline != VK_NULL_HANDLE
                             && m_RifeNativeWarpDs       != VK_NULL_HANDLE
                             && m_SlotPreCmdBuf[slot]    != VK_NULL_HANDLE
                             && m_SlotPostCmdBuf[slot]   != VK_NULL_HANDLE
                             && m_ComputeCmdBuf[slot]    != VK_NULL_HANDLE
                             && m_ComputeTimelineSem     != VK_NULL_HANDLE;
    uint64_t pathDCopyDoneVal = 0;
    uint32_t imgIdxA = 0;
    uint32_t imgIdxB = 0;
    uint32_t imgIdxC = 0;
    VkResult vr;
    if (dualPresentThisFrame) {
        VkResult vrA = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][0],
                                                    VK_NULL_HANDLE, &imgIdxA);
        if (vrA != VK_SUCCESS && vrA != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] dual: acquire interp imgA failed (%d)", (int)vrA);
            m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
            return;
        }
        VkResult vrB = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                    m_SlotAcquireSem[slot][1],
                                                    VK_NULL_HANDLE, &imgIdxB);
        if (vrB != VK_SUCCESS && vrB != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] dual: acquire real imgB failed (%d)", (int)vrB);
            m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
            return;
        }
        if (triplePresentThisFrame) {
            VkResult vrC = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                                        m_SlotAcquireSem[slot][2],
                                                        VK_NULL_HANDLE, &imgIdxC);
            if (vrC != VK_SUCCESS && vrC != VK_SUBOPTIMAL_KHR) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-VKFRUC] triple: acquire interp2 imgC failed (%d)", (int)vrC);
                m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
                return;
            }
        }
    } else {
        vr = m_RtPfn.AcquireNextImageKHR(m_Device, m_Swapchain, UINT64_MAX,
                                          m_SlotAcquireSem[slot][0],
                                          VK_NULL_HANDLE, &imgIdxB);
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] vkAcquireNextImageKHR failed (%d)", (int)vr);
            m_RtPfn.ResetCommandBuffer(m_SlotCmdBuf[slot], 0);
            return;
        }
    }
    uint32_t imgIdx = imgIdxB;  // legacy alias for the real-frame render pass

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
    // §B1a 2026-05-06 — imageLayout changed SHADER_READ_ONLY_OPTIMAL →
    // GENERAL so vkf->img[0] can stay in GENERAL throughout the frame
    // (acquireBar transitions directly to GENERAL, image-to-buffer copy
    // reads from GENERAL, fragment shader samples from GENERAL, release
    // barrier no-op stays in GENERAL).  Avoids the double-transition
    // SHADER_READ_ONLY → TRANSFER_SRC → SHADER_READ_ONLY that hangs NV's
    // video-decode image tracker (frame 0/1/2 record but no further
    // decoded frames — see commit history).
    VkDescriptorImageInfo dii = {};
    dii.sampler     = VK_NULL_HANDLE;  // immutable, baked into layout
    dii.imageView   = frameView;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet wds = {};
    wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet          = m_SlotDescSet[slot];
    wds.dstBinding      = 0;
    wds.descriptorCount = 1;
    wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo      = &dii;
    m_RtPfn.UpdateDescriptorSets(m_Device, 1, &wds, 0, nullptr);

    // §J.3.e.2.i.10f Path D — when active, FIRST record + submit a small
    // copy cmd buffer that owns vkf->img[0] access (acquireBar + image
    // copies + releaseBar) and signals vkf->sem[0]@V+1 immediately.
    // FFmpeg pool can recycle the image after this ~100us submit instead
    // of waiting for the full ~20ms chain.  The main cmd buffer below
    // skips those ops + cross-syncs via m_CopyDoneSem timeline value.
    if (pathDActive) {
        pathDCopyDoneVal = m_CopyDoneNext.fetch_add(1, std::memory_order_acq_rel);
        VkCommandBuffer copyCmd = m_SlotCopyCmdBuf[slot];
        m_RtPfn.ResetCommandBuffer(copyCmd, 0);

        VkCommandBufferBeginInfo cbbiC = {};
        cbbiC.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbiC.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_RtPfn.BeginCommandBuffer(copyCmd, &cbbiC);

        // (a) acquire barrier: vkf->img[0] from decoder layout → GENERAL
        VkImageMemoryBarrier acqBarC = {};
        acqBarC.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acqBarC.srcAccessMask       = 0;
        acqBarC.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;  // copies only
        acqBarC.oldLayout           = vkf->layout[0];
        acqBarC.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        acqBarC.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acqBarC.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acqBarC.image               = vkf->img[0];
        acqBarC.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        acqBarC.subresourceRange.baseMipLevel   = 0;
        acqBarC.subresourceRange.levelCount     = 1;
        acqBarC.subresourceRange.baseArrayLayer = 0;
        acqBarC.subresourceRange.layerCount     = 1;
        m_RtPfn.CmdPipelineBarrier(copyCmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &acqBarC);

        // (b) drain prior frame's FRUC compute SHADER_READ on m_SwFrucNv12Buf
        // before this frame's TRANSFER_WRITE.
        VkBufferMemoryBarrier preCopyBarC = {};
        preCopyBarC.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        preCopyBarC.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        preCopyBarC.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        preCopyBarC.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarC.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBarC.buffer = m_SwFrucNv12Buf;
        preCopyBarC.offset = 0;
        preCopyBarC.size   = VK_WHOLE_SIZE;
        m_RtPfn.CmdPipelineBarrier(copyCmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &preCopyBarC, 0, nullptr);

        // (c) cmdCopyImageToBuffer: vkf->img[0] (NV12 multi-plane) →
        // m_SwFrucNv12Buf (PLANE_0 @ offset 0, PLANE_1 @ offset W*H).
        const uint32_t hwWc = (uint32_t)frame->width;
        const uint32_t hwHc = (uint32_t)frame->height;
        VkBufferImageCopy regsC[2] = {};
        regsC[0].bufferOffset      = 0;
        regsC[0].bufferRowLength   = hwWc;
        regsC[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        regsC[0].imageSubresource.layerCount = 1;
        regsC[0].imageExtent       = { hwWc, hwHc, 1 };
        regsC[1].bufferOffset      = (VkDeviceSize)hwWc * hwHc;
        regsC[1].bufferRowLength   = hwWc / 2;
        regsC[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        regsC[1].imageSubresource.layerCount = 1;
        regsC[1].imageExtent       = { hwWc / 2, hwHc / 2, 1 };
        m_RtPfn.CmdCopyImageToBuffer(copyCmd, vkf->img[0],
            VK_IMAGE_LAYOUT_GENERAL,
            m_SwFrucNv12Buf, 2, regsC);

        // (d) optional NvOf cmdCopyImage (mirror of original lines ~8004-8086).
        if (m_NvOfReady) {
            VkImageMemoryBarrier ofImgBars[2] = {};
            int ofBarCount = 0;
            for (VkImage img : {m_NvOfInputCurr, m_NvOfInputPrev}) {
                if (m_NvOfTimelineValue == 0) {
                    auto& b = ofImgBars[ofBarCount++];
                    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.srcAccessMask       = 0;
                    b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT
                                          | VK_ACCESS_TRANSFER_READ_BIT
                                          | VK_ACCESS_SHADER_READ_BIT;
                    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image               = img;
                    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                    b.subresourceRange.baseMipLevel   = 0;
                    b.subresourceRange.levelCount     = 1;
                    b.subresourceRange.baseArrayLayer = 0;
                    b.subresourceRange.layerCount     = 1;
                }
            }
            if (ofBarCount > 0) {
                m_RtPfn.CmdPipelineBarrier(copyCmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT
                        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr,
                    (uint32_t)ofBarCount, ofImgBars);
            }
            VkImageCopy ofRegs[2] = {};
            ofRegs[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            ofRegs[0].srcSubresource.layerCount = 1;
            ofRegs[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            ofRegs[0].dstSubresource.layerCount = 1;
            ofRegs[0].extent = { hwWc, hwHc, 1 };
            ofRegs[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            ofRegs[1].srcSubresource.layerCount = 1;
            ofRegs[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            ofRegs[1].dstSubresource.layerCount = 1;
            ofRegs[1].extent = { hwWc / 2, hwHc / 2, 1 };
            auto pfnCmdCopyImage_pd = (PFN_vkCmdCopyImage)((PFN_vkGetDeviceProcAddr)
                m_pfnGetInstanceProcAddr(m_Instance, "vkGetDeviceProcAddr"))(
                m_Device, "vkCmdCopyImage");
            if (pfnCmdCopyImage_pd) {
                pfnCmdCopyImage_pd(copyCmd, vkf->img[0], VK_IMAGE_LAYOUT_GENERAL,
                                   m_NvOfInputCurr, VK_IMAGE_LAYOUT_GENERAL,
                                   2, ofRegs);
            }
        }

        // (e) m_SwFrucNv12Buf TRANSFER_WRITE → SHADER_READ for next submit's
        // nv12rgb compute.  Cross-submit timeline sem provides ordering, but
        // explicit barrier inside copy cmd buffer makes cache flush deterministic.
        VkBufferMemoryBarrier postCopyBarC = {};
        postCopyBarC.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        postCopyBarC.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        postCopyBarC.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        postCopyBarC.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopyBarC.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postCopyBarC.buffer = m_SwFrucNv12Buf;
        postCopyBarC.offset = 0;
        postCopyBarC.size   = VK_WHOLE_SIZE;
        m_RtPfn.CmdPipelineBarrier(copyCmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &postCopyBarC, 0, nullptr);

        // (f) release barrier on vkf->img[0] (GENERAL → GENERAL, cache flush).
        VkImageMemoryBarrier relBarC = {};
        relBarC.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        relBarC.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        relBarC.dstAccessMask       = 0;
        relBarC.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        relBarC.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        relBarC.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        relBarC.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        relBarC.image               = vkf->img[0];
        relBarC.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        relBarC.subresourceRange.baseMipLevel   = 0;
        relBarC.subresourceRange.levelCount     = 1;
        relBarC.subresourceRange.baseArrayLayer = 0;
        relBarC.subresourceRange.layerCount     = 1;
        m_RtPfn.CmdPipelineBarrier(copyCmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &relBarC);

        VkResult endCopyVr = m_RtPfn.EndCommandBuffer(copyCmd);
        if (endCopyVr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-PATH-D] vkEndCommandBuffer (copy) failed (%d)", (int)endCopyVr);
            if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
            return;
        }

        // Submit copy cmd buffer: wait vkf->sem[0]@V (decode-done from FFmpeg),
        // signal vkf->sem[0]@V+1 (release-to-pool) + m_CopyDoneSem@N (chain
        // to main submit).  No fence — main submit's slot fence covers the
        // whole frame's GPU work for next slot's reuse purposes.
        VkSemaphore     copyWaitSems[1]   = { vkf->sem[0] };
        VkPipelineStageFlags copyWaitMasks[1] = { VK_PIPELINE_STAGE_TRANSFER_BIT };
        uint64_t        copyWaitVals[1]   = { vkf->sem_value[0] };
        VkSemaphore     copySigSems[2]    = { vkf->sem[0], m_CopyDoneSem };
        uint64_t        copySigVals[2]    = { vkf->sem_value[0] + 1, pathDCopyDoneVal };
        VkTimelineSemaphoreSubmitInfo copyTssi = {};
        copyTssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        copyTssi.waitSemaphoreValueCount   = 1;
        copyTssi.pWaitSemaphoreValues      = copyWaitVals;
        copyTssi.signalSemaphoreValueCount = 2;
        copyTssi.pSignalSemaphoreValues    = copySigVals;
        VkSubmitInfo copySi = {};
        copySi.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        copySi.pNext                = &copyTssi;
        copySi.waitSemaphoreCount   = 1;
        copySi.pWaitSemaphores      = copyWaitSems;
        copySi.pWaitDstStageMask    = copyWaitMasks;
        copySi.commandBufferCount   = 1;
        copySi.pCommandBuffers      = &copyCmd;
        copySi.signalSemaphoreCount = 2;
        copySi.pSignalSemaphores    = copySigSems;
        VkResult vrCopy;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vrCopy = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &copySi, VK_NULL_HANDLE);
        }
        if (vrCopy != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC-PATH-D] copy QueueSubmit failed (%d)", (int)vrCopy);
            if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
            return;
        }

        // Mirror existing post-submit AVVkFrame state update (was at line ~8540).
        // sem signal value bump must happen WHILE locked so subsequent FFmpeg
        // pool readers see the new value.  After this, FFmpeg can reuse the
        // pool image as soon as GPU reaches sem_value[0]+1 (a few hundred us).
        vkf->access[0]     = (VkAccessFlagBits)0;
        vkf->layout[0]     = VK_IMAGE_LAYOUT_GENERAL;
        vkf->sem_value[0] += 1;

        // Unlock AVFrame — submit 1 is the only thing that touches vkf
        // metadata.  Submit 2 doesn't reference vkf at all.
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);

        static std::atomic<bool> s_pathDLogged{false};
        bool exp = false;
        if (s_pathDLogged.compare_exchange_strong(exp, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-PATH-D] active: image copy + sem signal in copy "
                "cmd buffer; main cmd buffer skips vkf->img[0] access. "
                "AVFrame pool can recycle pool image after ~100us copy GPU work.");
        }
    }

    // ===========================================================
    // §J.3.e.2.i.10 Phase 2B step 5-6 (v1.4.56) — 3-cmd-buf split path
    // ===========================================================
    //
    // phase2BActive gate (computed above) guarantees:
    //   • DUAL present + Path D copy submit just ran (vkf released)
    //   • RIFE β.5.1 path (1× inference @ t=0.5)
    //   • pre/cmp/post cmd buffers + m_ComputeTimelineSem alloc'd
    //
    // Submit chain (v1.4.56 — ALL submits on graphics queue;
    //               v1.4.57 will retarget cmpCmd to m_ComputeQueue):
    //   1. (already submitted) Path D copy → vkf sem + m_CopyDoneSem@N
    //   2. preCmd  : wait copy + acquire[A,B],   signal compute@V_pre
    //   3. cmpCmd  : wait compute@V_pre,         signal compute@V_post
    //   4. postCmd : wait compute@V_post,        signal renderDone[A,B] + fence
    //
    // Single-cmd path (env=0 / phase2BActive=false) untouched below.
    if (phase2BActive) {
        const uint64_t computeV_pre  = m_AsyncComputeNext.fetch_add(2, std::memory_order_acq_rel);
        const uint64_t computeV_post = computeV_pre + 1;

        VkCommandBuffer preCmd  = m_SlotPreCmdBuf[slot];
        VkCommandBuffer cmpCmd  = m_ComputeCmdBuf[slot];
        VkCommandBuffer postCmd = m_SlotPostCmdBuf[slot];

        m_RtPfn.ResetCommandBuffer(preCmd, 0);
        m_RtPfn.ResetCommandBuffer(cmpCmd, 0);
        m_RtPfn.ResetCommandBuffer(postCmd, 0);

        VkCommandBufferBeginInfo cbbi2B = {};
        cbbi2B.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi2B.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        m_RtPfn.BeginCommandBuffer(preCmd,  &cbbi2B);
        m_RtPfn.BeginCommandBuffer(cmpCmd,  &cbbi2B);
        m_RtPfn.BeginCommandBuffer(postCmd, &cbbi2B);

        auto getDevPaP2B = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdBindPipeline_p2b    = (PFN_vkCmdBindPipeline)getDevPaP2B(m_Device, "vkCmdBindPipeline");
        auto pfnCmdBindDescSets_p2b    = (PFN_vkCmdBindDescriptorSets)getDevPaP2B(m_Device, "vkCmdBindDescriptorSets");
        auto pfnCmdPushConst_p2b       = (PFN_vkCmdPushConstants)getDevPaP2B(m_Device, "vkCmdPushConstants");
        auto pfnCmdDispatch_p2b        = (PFN_vkCmdDispatch)getDevPaP2B(m_Device, "vkCmdDispatch");
        auto pfnCmdPipelineBarrier_p2b = (PFN_vkCmdPipelineBarrier)getDevPaP2B(m_Device, "vkCmdPipelineBarrier");
        auto pfnCmdCopyBuffer_p2b      = (PFN_vkCmdCopyBuffer)getDevPaP2B(m_Device, "vkCmdCopyBuffer");
        auto pfnCmdResetQueryPool_p2b  = (PFN_vkCmdResetQueryPool)getDevPaP2B(m_Device, "vkCmdResetQueryPool");
        auto pfnCmdWriteTimestamp_p2b  = (PFN_vkCmdWriteTimestamp)getDevPaP2B(m_Device, "vkCmdWriteTimestamp");
        auto pfnGetQueryPoolResults_p2b = (PFN_vkGetQueryPoolResults)getDevPaP2B(m_Device, "vkGetQueryPoolResults");

        // §J.3.e.2.i.10 Phase 2B (v1.4.59) GPU profiling — read previous
        // pass's 6 timestamps for THIS p2b timer slot (slot fence already
        // wait above guarantees the GPU finished the prior phase2BActive
        // chain that wrote them).  Then accumulate + log every 60 samples.
        const uint32_t p2bTimerSlot = m_Phase2BTimerSlot;
        const uint32_t p2bTimerBase = p2bTimerSlot * 6;
        if (m_Phase2BTimerPool && pfnGetQueryPoolResults_p2b
            && m_Phase2BTimerArmed[p2bTimerSlot]) {
            uint64_t ts[6] = {};
            VkResult qrP2B = pfnGetQueryPoolResults_p2b(m_Device, m_Phase2BTimerPool,
                p2bTimerBase, 6, sizeof(ts), ts, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (qrP2B == VK_SUCCESS && ts[5] >= ts[0]) {
                const double ns2us = m_FrucTimerNsPerTick / 1000.0;
                m_Phase2BGpuPreUsAccum   += (double)(ts[1] - ts[0]) * ns2us;
                m_Phase2BGpuCmpUsAccum   += (double)(ts[3] - ts[2]) * ns2us;
                m_Phase2BGpuPostUsAccum  += (double)(ts[5] - ts[4]) * ns2us;
                m_Phase2BGpuTotalUsAccum += (double)(ts[5] - ts[0]) * ns2us;
                m_Phase2BGpuCount++;
                if (m_Phase2BGpuCount >= 60) {
                    const double n = (double)m_Phase2BGpuCount;
                    const double avgPre   = m_Phase2BGpuPreUsAccum   / n;
                    const double avgCmp   = m_Phase2BGpuCmpUsAccum   / n;
                    const double avgPost  = m_Phase2BGpuPostUsAccum  / n;
                    const double avgTot   = m_Phase2BGpuTotalUsAccum / n;
                    const double serial   = avgPre + avgCmp + avgPost;
                    const double saving   = serial - avgTot;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-PHASE2B-PROF] preCmd=%.0fus cmpCmd=%.0fus "
                        "postCmd=%.0fus chain=%.0fus serial_sum=%.0fus "
                        "parallel_saving=%.0fus (n=%d, mean over last 60 frames)",
                        avgPre, avgCmp, avgPost, avgTot, serial, saving,
                        m_Phase2BGpuCount);
                    m_Phase2BGpuPreUsAccum   = 0.0;
                    m_Phase2BGpuCmpUsAccum   = 0.0;
                    m_Phase2BGpuPostUsAccum  = 0.0;
                    m_Phase2BGpuTotalUsAccum = 0.0;
                    m_Phase2BGpuCount        = 0;
                }
            }
        }
        m_Phase2BTimerSlot = (m_Phase2BTimerSlot + 1) % kFrucFramesInFlight;

        // §J.3.e.2.i.10 Phase 2B (v1.4.59) GPU profiling — reset 6 query
        // slots in preCmd; each cmd buf writes its start timestamp at
        // TOP_OF_PIPE.  Cross-queue write ordering is guaranteed by the
        // timeline-sem chain (preCmd reset/write → cmpCmd wait+write →
        // postCmd wait+write); per spec, reset+write must observe in-cmd
        // execution order but may live in different queues.
        if (m_Phase2BTimerPool && pfnCmdResetQueryPool_p2b && pfnCmdWriteTimestamp_p2b) {
            pfnCmdResetQueryPool_p2b(preCmd, m_Phase2BTimerPool, p2bTimerBase, 6);
            pfnCmdWriteTimestamp_p2b(preCmd,  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 0);
            pfnCmdWriteTimestamp_p2b(cmpCmd,  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 2);
            pfnCmdWriteTimestamp_p2b(postCmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 4);
        }

        const uint32_t hwW = (uint32_t)frame->width;
        const uint32_t hwH = (uint32_t)frame->height;

        // ---- preCmd: Stage 0 (nv12rgb compute) + RIFE bilinear DOWN ----
        // (image→buffer copy already done in Path D copy submit; preCmd
        // skips image-related barriers.  m_SwFrucNv12Buf left in
        // SHADER_READ by Path D postCopyBarC.)
        pfnCmdBindPipeline_p2b(preCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeline);
        pfnCmdBindDescSets_p2b(preCmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FrucNv12RgbPipeLay,
                               0, 1, &m_FrucNv12RgbDescSetNative, 0, nullptr);
        struct { int w, h, uvByteOffset, _pad; } pcN_p2b = {
            (int)hwW, (int)hwH, (int)(hwW * hwH), 0
        };
        pfnCmdPushConst_p2b(preCmd, m_FrucNv12RgbPipeLay, VK_SHADER_STAGE_COMPUTE_BIT,
                            0, sizeof(pcN_p2b), &pcN_p2b);
        pfnCmdDispatch_p2b(preCmd, (hwW + 7) / 8, (hwH + 7) / 8, 1);
        {
            VkBufferMemoryBarrier b = {};
            b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.buffer = m_FrucCurrRgbBuf;
            b.size = VK_WHOLE_SIZE;
            pfnCmdPipelineBarrier_p2b(preCmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b, 0, nullptr);
        }
        if (!recordRifeDown(preCmd, hwW, hwH, slot)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B recordRifeDown FAILED — "
                "permanently falling back to single-cmd path");
            m_RtPfn.EndCommandBuffer(preCmd);
            m_RtPfn.EndCommandBuffer(cmpCmd);
            m_RtPfn.EndCommandBuffer(postCmd);
            // Demote the gate for this session so we don't keep half-recording.
            m_AsyncComputeAvailable = false;
            // Fall through to single-cmd path below by re-acquiring frame
            // state — but we've already done the path D copy submit which
            // unlocked vkf, so it's safer to just bail this frame.
            return;
        }
        if (m_Phase2BTimerPool && pfnCmdWriteTimestamp_p2b) {
            pfnCmdWriteTimestamp_p2b(preCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 1);
        }
        m_RtPfn.EndCommandBuffer(preCmd);

        // ---- cmpCmd: RIFE inference (graphics queue in v1.4.56) ----
        if (!recordRifeInferOnCompute(cmpCmd, hwW, hwH, slot)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B recordRifeInferOnCompute "
                "FAILED — disabling Phase 2B for this session");
            m_RtPfn.EndCommandBuffer(cmpCmd);
            m_RtPfn.EndCommandBuffer(postCmd);
            m_AsyncComputeAvailable = false;
            return;
        }
        if (m_Phase2BTimerPool && pfnCmdWriteTimestamp_p2b) {
            pfnCmdWriteTimestamp_p2b(cmpCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 3);
        }
        m_RtPfn.EndCommandBuffer(cmpCmd);

        // ---- postCmd: warp + Stage 4 + render passes ----
        if (!recordRifeWarp(postCmd, hwW, hwH, slot)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B recordRifeWarp FAILED");
            m_RtPfn.EndCommandBuffer(postCmd);
            m_AsyncComputeAvailable = false;
            return;
        }

        // Stage 4: curr→prev copy (mirror runFrucComputeChain end-block).
        {
            VkBufferMemoryBarrier b1 = {};
            b1.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b1.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b1.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b1.buffer = m_FrucCurrRgbBuf;
            b1.size = VK_WHOLE_SIZE;
            pfnCmdPipelineBarrier_p2b(postCmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &b1, 0, nullptr);
            VkBufferMemoryBarrier b2 = b1;
            b2.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b2.buffer = m_FrucPrevRgbBuf;
            pfnCmdPipelineBarrier_p2b(postCmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 1, &b2, 0, nullptr);
        }
        {
            VkBufferCopy cpy = {};
            cpy.size = (VkDeviceSize)hwW * hwH * 3 * sizeof(float);
            pfnCmdCopyBuffer_p2b(postCmd, m_FrucCurrRgbBuf, m_FrucPrevRgbBuf, 1, &cpy);
        }
        {
            // m_FrucPrevRgbBuf TRANSFER_WRITE → SHADER_READ for next frame's
            // RIFE DOWN read.
            VkBufferMemoryBarrier b = {};
            b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.buffer = m_FrucPrevRgbBuf;
            b.size = VK_WHOLE_SIZE;
            pfnCmdPipelineBarrier_p2b(postCmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &b, 0, nullptr);
        }

        // Overlay drain + upload (cmdCopyBufferToImage must be outside render pass).
        drainOverlayStash();
        uploadPendingOverlay(postCmd);

        // SHADER_WRITE → FRAGMENT_SHADER_READ for interp/curr (real path uses
        // m_FrucCurrRgbBuf via m_RealCurrRgbDescSet — useCrgbForReal=true
        // is guaranteed by phase2BActive gate).
        {
            VkBufferMemoryBarrier rgbBars[2] = {};
            rgbBars[0].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            rgbBars[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            rgbBars[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            rgbBars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            rgbBars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            rgbBars[0].buffer = m_FrucInterpRgbBuf;
            rgbBars[0].size   = VK_WHOLE_SIZE;
            rgbBars[1] = rgbBars[0];
            // m_FrucCurrRgbBuf already in TRANSFER_READ from Stage 4 barrier
            // above; the fragment shader needs SHADER_READ.  Use a separate
            // transition for it.
            rgbBars[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            rgbBars[1].buffer = m_FrucCurrRgbBuf;
            pfnCmdPipelineBarrier_p2b(postCmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 2, rgbBars, 0, nullptr);
        }

        VkClearValue clearVal_p2b = {};
        clearVal_p2b.color.float32[3] = 1.0f;

        // Interp render pass on imgIdxA (DUAL midpoint).
        {
            VkRenderPassBeginInfo rpbiA = {};
            rpbiA.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbiA.renderPass           = m_RenderPass;
            rpbiA.framebuffer          = m_Framebuffers[imgIdxA];
            rpbiA.renderArea.extent    = m_SwapchainExtent;
            rpbiA.clearValueCount      = 1;
            rpbiA.pClearValues         = &clearVal_p2b;
            m_RtPfn.CmdBeginRenderPass(postCmd, &rpbiA, VK_SUBPASS_CONTENTS_INLINE);
            m_RtPfn.CmdBindPipeline(postCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
            m_RtPfn.CmdBindDescriptorSets(postCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          m_InterpPipelineLayout, 0,
                                          1, &m_InterpDescSet, 0, nullptr);
            struct { int srcW, srcH, _pad0, _pad1; } pcInterp = {
                (int)hwW, (int)hwH, 0, 0
            };
            pfnCmdPushConst_p2b(postCmd, m_InterpPipelineLayout,
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(pcInterp), &pcInterp);
            m_RtPfn.CmdDraw(postCmd, 3, 1, 0, 0);
            drawOverlayInRenderPass(postCmd);
            m_RtPfn.CmdEndRenderPass(postCmd);
        }

        // Real-frame render pass on imgIdxB — useCrgbForReal route (m_InterpPipeline
        // + m_RealCurrRgbDescSet reads m_FrucCurrRgbBuf).
        {
            VkRenderPassBeginInfo rpbiB = {};
            rpbiB.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpbiB.renderPass           = m_RenderPass;
            rpbiB.framebuffer          = m_Framebuffers[imgIdxB];
            rpbiB.renderArea.extent    = m_SwapchainExtent;
            rpbiB.clearValueCount      = 1;
            rpbiB.pClearValues         = &clearVal_p2b;
            m_RtPfn.CmdBeginRenderPass(postCmd, &rpbiB, VK_SUBPASS_CONTENTS_INLINE);
            m_RtPfn.CmdBindPipeline(postCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
            m_RtPfn.CmdBindDescriptorSets(postCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                          m_InterpPipelineLayout, 0,
                                          1, &m_RealCurrRgbDescSet, 0, nullptr);
            struct { int srcW, srcH, _pad0, _pad1; } pcReal = {
                (int)hwW, (int)hwH, 0, 0
            };
            pfnCmdPushConst_p2b(postCmd, m_InterpPipelineLayout,
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(pcReal), &pcReal);
            m_RtPfn.CmdDraw(postCmd, 3, 1, 0, 0);
            drawOverlayInRenderPass(postCmd);
            m_RtPfn.CmdEndRenderPass(postCmd);
        }

        // releaseBar on vkf->img[0] already done in Path D copy submit; skip.
        if (m_Phase2BTimerPool && pfnCmdWriteTimestamp_p2b) {
            pfnCmdWriteTimestamp_p2b(postCmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                      m_Phase2BTimerPool, p2bTimerBase + 5);
        }
        m_RtPfn.EndCommandBuffer(postCmd);

        // ---- Submit 4-chain (path D copy submit already done above) ----
        // §J.3.e.2.i.10 Phase 2B step 5-6 — v1.4.56 routes ALL three submits
        // through m_GraphicsQueue (s_VkFrucQueueLock).  v1.4.57 will switch
        // the cmpCmd submit to m_ComputeQueue (s_VkFrucComputeLock).
        //
        // preCmd submit: wait CopyDoneSem@N, signal compute@V_pre.
        //                acquireSems wait moved to postCmd (only the render
        //                pass cares about swapchain image availability).
        {
            VkSemaphore     waitS[1] = { m_CopyDoneSem };
            VkPipelineStageFlags waitM[1] = { VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
            uint64_t        waitV[1] = { pathDCopyDoneVal };
            VkSemaphore     sigS[1]  = { m_ComputeTimelineSem };
            uint64_t        sigV[1]  = { computeV_pre };
            VkTimelineSemaphoreSubmitInfo tssi = {};
            tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tssi.waitSemaphoreValueCount   = 1;
            tssi.pWaitSemaphoreValues      = waitV;
            tssi.signalSemaphoreValueCount = 1;
            tssi.pSignalSemaphoreValues    = sigV;
            VkSubmitInfo si = {};
            si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.pNext                = &tssi;
            si.waitSemaphoreCount   = 1;
            si.pWaitSemaphores      = waitS;
            si.pWaitDstStageMask    = waitM;
            si.commandBufferCount   = 1;
            si.pCommandBuffers      = &preCmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores    = sigS;
            VkResult vrPre;
            {
                std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                vrPre = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, VK_NULL_HANDLE);
            }
            if (vrPre != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B preCmd submit failed (%d)", (int)vrPre);
                return;
            }
        }
        // cmpCmd submit: wait compute@V_pre, signal compute@V_post.
        //   runInferenceGpuFlow's first op is vkCmdCopyBuffer (TRANSFER) so
        //   the wait stage mask is TRANSFER.  v1.4.57: now routed to
        //   m_ComputeQueue under s_VkFrucComputeLock — async compute path
        //   actually parallelises with graphics submits.  Cross-queue sync
        //   relies on:
        //     • m_ComputeTimelineSem (timeline binary across QFs, Phase 2A)
        //     • CONCURRENT sharing on cross-queue buffers (Phase 2B v1.4.55):
        //       m_RifeDownPrev/Curr/Interp, m_RifeFlow/MaskOutBuf
        //     • VulkanCtx::computeQueue/Family set to m_Compute* when
        //       asyncCtxActive (Phase 2B v1.4.55)
        {
            VkPipelineStageFlags waitM[1] = { VK_PIPELINE_STAGE_TRANSFER_BIT };
            VkSemaphore     waitS[1] = { m_ComputeTimelineSem };
            uint64_t        waitV[1] = { computeV_pre };
            VkSemaphore     sigS[1]  = { m_ComputeTimelineSem };
            uint64_t        sigV[1]  = { computeV_post };
            VkTimelineSemaphoreSubmitInfo tssi = {};
            tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tssi.waitSemaphoreValueCount   = 1;
            tssi.pWaitSemaphoreValues      = waitV;
            tssi.signalSemaphoreValueCount = 1;
            tssi.pSignalSemaphoreValues    = sigV;
            VkSubmitInfo si = {};
            si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.pNext                = &tssi;
            si.waitSemaphoreCount   = 1;
            si.pWaitSemaphores      = waitS;
            si.pWaitDstStageMask    = waitM;
            si.commandBufferCount   = 1;
            si.pCommandBuffers      = &cmpCmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores    = sigS;
            VkResult vrCmp;
            {
                // §J.3.e.2.i.10 Phase 2B step 5-6 (v1.4.57) — independent
                // lock from s_VkFrucQueueLock so the compute QF submit
                // doesn't serialise behind concurrent ffmpeg / graphics
                // submits — that would defeat the parallelism we want.
                std::lock_guard<std::mutex> lk(s_VkFrucComputeLock);
                vrCmp = m_RtPfn.QueueSubmit(m_ComputeQueue, 1, &si, VK_NULL_HANDLE);
            }
            if (vrCmp != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B cmpCmd submit failed (%d)", (int)vrCmp);
                return;
            }
        }
        // postCmd submit: wait compute@V_post + acquireSem[A,B], signal
        //                  renderDone[A,B] + fence.
        //   recordRifeWarp's first op is COMPUTE_SHADER (warp dispatch); the
        //   downstream render pass is FRAGMENT_SHADER; Stage 4 + curr→prev
        //   copy is TRANSFER; cover all three in the compute@V_post wait mask.
        //   acquireSems block only the COLOR_ATTACHMENT_OUTPUT stage (render
        //   pass writes), per swapchain image semantics.
        {
            VkPipelineStageFlags waitM[3] = {
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                    | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            };
            VkSemaphore     waitS[3] = { m_ComputeTimelineSem,
                                          m_SlotAcquireSem[slot][0],
                                          m_SlotAcquireSem[slot][1] };
            uint64_t        waitV[3] = { computeV_post, 0, 0 };
            VkSemaphore     sigS[2]  = { m_SwapchainRenderDoneSem[imgIdxA],
                                          m_SwapchainRenderDoneSem[imgIdxB] };
            uint64_t        sigV[2]  = { 0, 0 };
            VkTimelineSemaphoreSubmitInfo tssi = {};
            tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tssi.waitSemaphoreValueCount   = 3;
            tssi.pWaitSemaphoreValues      = waitV;
            tssi.signalSemaphoreValueCount = 2;
            tssi.pSignalSemaphoreValues    = sigV;
            VkSubmitInfo si = {};
            si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.pNext                = &tssi;
            si.waitSemaphoreCount   = 3;
            si.pWaitSemaphores      = waitS;
            si.pWaitDstStageMask    = waitM;
            si.commandBufferCount   = 1;
            si.pCommandBuffers      = &postCmd;
            si.signalSemaphoreCount = 2;
            si.pSignalSemaphores    = sigS;
            VkResult vrPost;
            {
                std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                vrPost = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
            }
            if (vrPost != VK_SUCCESS) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B postCmd submit failed (%d)", (int)vrPost);
                return;
            }
        }

        static std::atomic<bool> s_phase2BLogged{false};
        bool expP2B = false;
        if (s_phase2BLogged.compare_exchange_strong(expP2B, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B wiring active: 3-cmd-buf "
                "chain (gfx-pre → CMP(QF=%u) → gfx-post), timeline V_pre=%llu "
                "V_post=%llu — async-compute parallel with graphics (v1.4.57)",
                (unsigned)m_ComputeQueueFamily,
                (unsigned long long)computeV_pre,
                (unsigned long long)computeV_post);
        }

        // ---- NV-OF marker + execute (mirror of single-cmd path at ~8865) ----
        // Path D copy submit already populated m_NvOfInputCurr; here we
        // fire the OF queue execute and swap input handles, identical to
        // single-cmd path so NV-OF state stays in sync across env toggles.
        if (m_NvOfReady && m_NvOfFuncList) {
            static thread_local uint32_t s_NvOfFrameCountP2B = 0;
            const uint32_t frameNumP2B = s_NvOfFrameCountP2B++;
            if (frameNumP2B == 0) {
                std::swap(m_NvOfInputCurr,    m_NvOfInputPrev);
                std::swap(m_NvOfInputCurrMem, m_NvOfInputPrevMem);
                std::swap(m_NvOfHandleCurr,   m_NvOfHandlePrev);
            }
            if (frameNumP2B > 0) {
                const uint64_t inSigVal  = m_NvOfTimelineValue + 1;
                const uint64_t outSigVal = inSigVal + 1;
                VkTimelineSemaphoreSubmitInfo tsMark = {};
                tsMark.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
                tsMark.signalSemaphoreValueCount = 1;
                tsMark.pSignalSemaphoreValues    = &inSigVal;
                VkSubmitInfo siMark = {};
                siMark.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                siMark.pNext                = &tsMark;
                siMark.commandBufferCount   = 0;
                siMark.signalSemaphoreCount = 1;
                siMark.pSignalSemaphores    = &m_NvOfTimelineSem;
                VkResult vrMark;
                {
                    std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                    vrMark = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &siMark, VK_NULL_HANDLE);
                }
                if (vrMark == VK_SUCCESS) {
                    auto* funcList = (NV_OF_VK_API_FUNCTION_LIST*)m_NvOfFuncList;
                    NV_OF_SYNC_VK waitSync = {};
                    waitSync.semaphore = m_NvOfTimelineSem;
                    waitSync.value     = inSigVal;
                    NV_OF_SYNC_VK signalSync = {};
                    signalSync.semaphore = m_NvOfTimelineSem;
                    signalSync.value     = outSigVal;
                    NV_OF_EXECUTE_INPUT_PARAMS_VK ofIn = {};
                    ofIn.inputFrame      = (NvOFGPUBufferHandle)m_NvOfHandleCurr;
                    ofIn.referenceFrame  = (NvOFGPUBufferHandle)m_NvOfHandlePrev;
                    ofIn.disableTemporalHints = NV_OF_FALSE;
                    ofIn.numWaitSyncs    = 1;
                    ofIn.pWaitSyncs      = &waitSync;
                    NV_OF_EXECUTE_OUTPUT_PARAMS_VK ofOut = {};
                    ofOut.outputBuffer = (NvOFGPUBufferHandle)m_NvOfHandleFlow;
                    ofOut.pSignalSync  = &signalSync;
                    NV_OF_STATUS s = funcList->nvOFExecuteVk((NvOFHandle)m_NvOfHandle,
                                                              &ofIn, &ofOut);
                    if (s == NV_OF_SUCCESS) {
                        m_NvOfTimelineValue = outSigVal;
                        std::swap(m_NvOfInputCurr,    m_NvOfInputPrev);
                        std::swap(m_NvOfInputCurrMem, m_NvOfInputPrevMem);
                        std::swap(m_NvOfHandleCurr,   m_NvOfHandlePrev);
                    }
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC-NVOF] §J.3.e.2.i.10 Phase 2B marker "
                                "QueueSubmit failed vr=%d", (int)vrMark);
                }
            }
        }

        // ---- Present (DUAL only — TRIPLE excluded by gate) ----
        {
            VkPresentInfoKHR piA = {};
            piA.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            piA.waitSemaphoreCount = 1;
            piA.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxA];
            piA.swapchainCount     = 1;
            piA.pSwapchains        = &m_Swapchain;
            piA.pImageIndices      = &imgIdxA;
            VkResult vrPA;
            {
                std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                vrPA = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piA);
            }
            if (vrPA != VK_SUCCESS && vrPA != VK_SUBOPTIMAL_KHR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B present(interp) returned %d", (int)vrPA);
            }
            VkPresentInfoKHR piB = {};
            piB.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            piB.waitSemaphoreCount = 1;
            piB.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxB];
            piB.swapchainCount     = 1;
            piB.pSwapchains        = &m_Swapchain;
            piB.pImageIndices      = &imgIdxB;
            VkResult vrPB;
            {
                std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                vrPB = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piB);
            }
            if (vrPB != VK_SUCCESS && vrPB != VK_SUBOPTIMAL_KHR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.10 Phase 2B present(real) returned %d", (int)vrPB);
            }
        }

        // AVFrame state already updated + unlocked in Path D copy submit
        // (mandatory pathDActive prereq).  Nothing else to do.

        // §J.3.e.2.i.10 Phase 2B (v1.4.59) — mark this p2b timer slot armed
        // so next time the same slot rotates around we read its 6 timestamps.
        // Submit succeeded → GPU will write them.
        if (m_Phase2BTimerPool) {
            m_Phase2BTimerArmed[p2bTimerSlot] = true;
        }
        return;
    }

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
    // §B1a 2026-05-06 — newLayout SHADER_READ_ONLY_OPTIMAL → GENERAL.
    // Image stays in GENERAL throughout the frame to allow image-to-buffer
    // copy + fragment shader sample to coexist without driver-hostile
    // re-transitions on NV video-decode images.  GENERAL is universally
    // sample-compatible (less optimal than SHADER_READ_ONLY but the loss
    // is negligible vs the FRUC chain win).
    // §J.3.e.2.i.10f Path D — when active, this barrier was already
    // emitted in the copy cmd buffer above; main cmd buffer skips it
    // (vkf->img[0] not accessed in this submit).
    if (!pathDActive) {
        VkImageMemoryBarrier acquireBar = {};
        acquireBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquireBar.srcAccessMask       = 0;
        acquireBar.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT
                                       | VK_ACCESS_TRANSFER_READ_BIT;
        acquireBar.oldLayout           = vkf->layout[0];
        acquireBar.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
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
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &acquireBar);
    }
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

    // §B1a 2026-05-06 — HW path FRUC compute integration.
    //
    // Mirror AVVkFrame.img[0] (NV12 multi-plane VkImage) to m_SwFrucNv12Buf
    // via cmdCopyImageToBuffer so the existing FRUC NV12→RGB compute reads
    // identical buffer format as SW path.  Code mirrors renderFrameSw's
    // §J.3.e.2.i.8 Phase 2.5 v2 block, with two adjustments for HW path:
    //
    //   1. Image stays in VK_IMAGE_LAYOUT_GENERAL throughout (instead of
    //      bouncing through TRANSFER_SRC_OPTIMAL).  GENERAL accepts both
    //      transfer reads and shader sampled reads, sidestepping NV's
    //      video-decode image layout tracker which appears to deadlock on
    //      double-transition (SHADER_READ → TRANSFER_SRC → SHADER_READ in
    //      same cmd buffer caused frame 0/1/2 to record successfully but
    //      no further frames decoded — "Video decode unit queue overflow"
    //      after 1s).
    //
    //   2. acquireBar (above) was already set to transition vkf->img[0]
    //      from oldLayout → SHADER_READ_ONLY_OPTIMAL.  We re-issue our
    //      own transition SHADER_READ_ONLY → GENERAL here (cheap), then
    //      run image-to-buffer copy + compute chain.  vkf->img[0]
    //      remains in GENERAL when the fragment shader render pass
    //      samples it later (sampler descriptor's imageLayout was set
    //      to SHADER_READ_ONLY_OPTIMAL but GENERAL is also valid for
    //      sampler — see VUID-VkDescriptorImageInfo-imageLayout-00344
    //      relaxation for storage / general-compatible layouts).
    //
    // No FRUC output presented yet — B1b adds dual-present.
    if (m_FrucMode && m_FrucReady && frame && frame->width > 0 && frame->height > 0) {
        const uint32_t hwW = (uint32_t)frame->width;
        const uint32_t hwH = (uint32_t)frame->height;

        // §J.3.e.2.i.10f Path D — when active, the entire image-copy
        // sequence below (preCopyBar + cmdCopyImageToBuffer + NvOf
        // copy + bufBar) was already done in the copy cmd buffer that
        // ran in submit 1.  m_SwFrucNv12Buf is filled and visible to
        // this submit via m_CopyDoneSem timeline-sem wait.  Skip down
        // straight to runFrucComputeChain.
        if (!pathDActive) {
        // acquireBar (above) put vkf->img[0] in GENERAL with TRANSFER_READ
        // already in dstAccessMask, so cmdCopyImageToBuffer can read it
        // directly without an additional image barrier.  Only need to
        // drain prior frame's FRUC compute SHADER_READ on m_SwFrucNv12Buf
        // before this frame's TRANSFER_WRITE (closes the cross-frame WAW
        // race documented at renderFrameSw line ~5489).
        VkBufferMemoryBarrier preCopyBar = {};
        preCopyBar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        preCopyBar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        preCopyBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        preCopyBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBar.buffer = m_SwFrucNv12Buf;
        preCopyBar.offset = 0;
        preCopyBar.size   = VK_WHOLE_SIZE;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &preCopyBar, 0, nullptr);

        // Image → buffer copy: PLANE_0 (Y) → offset 0, PLANE_1 (UV
        // interleaved at half spatial) → offset W×H.
        VkBufferImageCopy regs[2] = {};
        regs[0].bufferOffset      = 0;
        regs[0].bufferRowLength   = hwW;
        regs[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
        regs[0].imageSubresource.layerCount = 1;
        regs[0].imageExtent       = { hwW, hwH, 1 };
        regs[1].bufferOffset      = (VkDeviceSize)hwW * hwH;
        regs[1].bufferRowLength   = hwW / 2;
        regs[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
        regs[1].imageSubresource.layerCount = 1;
        regs[1].imageExtent       = { hwW / 2, hwH / 2, 1 };
        m_RtPfn.CmdCopyImageToBuffer(cmd, vkf->img[0],
            VK_IMAGE_LAYOUT_GENERAL,
            m_SwFrucNv12Buf, 2, regs);

        // §B-NVOF Phase 4b 2026-05-06 — also copy vkf->img[0] image-to-image
        // into m_NvOfInputCurr (NV12 multi-plane).  This is independent of
        // the buffer copy above; both source-read in parallel.  After GPU
        // signals m_NvOfTimelineSem (Phase 4c submit signal list), we kick
        // off nvOFExecuteVk on the OF queue (kick-off-and-forget for now;
        // chain integration in Phase 4d).
        if (m_NvOfReady) {
            // §B-NVOF Phase 4b 2026-05-06 — image transitions UNDEFINED→GENERAL
            // on first use, then stays GENERAL forever.  GENERAL accepts both
            // TRANSFER_WRITE (vkCmdCopyImage dst) and shader / OF SDK reads
            // without further transitions.  Avoids TRANSFER_DST_OPTIMAL→
            // expected-OF-layout transition (NV SDK didn't document expected
            // layout; first attempt with TRANSFER_DST landed on DEVICE_LOST
            // immediately after first nvOFExecuteVk).
            VkImageMemoryBarrier ofImgBars[2] = {};
            int ofBarCount = 0;
            for (VkImage img : {m_NvOfInputCurr, m_NvOfInputPrev}) {
                if (m_NvOfTimelineValue == 0) {
                    auto& b = ofImgBars[ofBarCount++];
                    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    b.srcAccessMask       = 0;
                    b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT
                                          | VK_ACCESS_TRANSFER_READ_BIT
                                          | VK_ACCESS_SHADER_READ_BIT;
                    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image               = img;
                    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                    b.subresourceRange.baseMipLevel   = 0;
                    b.subresourceRange.levelCount     = 1;
                    b.subresourceRange.baseArrayLayer = 0;
                    b.subresourceRange.layerCount     = 1;
                }
            }
            if (ofBarCount > 0) {
                m_RtPfn.CmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT
                        | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr,
                    (uint32_t)ofBarCount, ofImgBars);
            }
            // PLANE_0 (Y) + PLANE_1 (UV) image→image copy. dst layout GENERAL.
            VkImageCopy ofRegs[2] = {};
            ofRegs[0].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            ofRegs[0].srcSubresource.layerCount = 1;
            ofRegs[0].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
            ofRegs[0].dstSubresource.layerCount = 1;
            ofRegs[0].extent = { hwW, hwH, 1 };
            ofRegs[1].srcSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            ofRegs[1].srcSubresource.layerCount = 1;
            ofRegs[1].dstSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
            ofRegs[1].dstSubresource.layerCount = 1;
            ofRegs[1].extent = { hwW / 2, hwH / 2, 1 };
            auto pfnCmdCopyImage = (PFN_vkCmdCopyImage)((PFN_vkGetDeviceProcAddr)
                m_pfnGetInstanceProcAddr(m_Instance, "vkGetDeviceProcAddr"))(
                m_Device, "vkCmdCopyImage");
            if (pfnCmdCopyImage) {
                pfnCmdCopyImage(cmd, vkf->img[0], VK_IMAGE_LAYOUT_GENERAL,
                                m_NvOfInputCurr, VK_IMAGE_LAYOUT_GENERAL,
                                2, ofRegs);
            }
        }

        // Buffer TRANSFER_WRITE → COMPUTE_SHADER_READ for FRUC's NV12→RGB.
        // No image transition back — vkf->img[0] stays in GENERAL; sampler
        // descriptor's SHADER_READ_ONLY_OPTIMAL spec is relaxed to GENERAL
        // for storage-compatible images (which video decode output is).
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
        }  // end if (!pathDActive) — copy ops above already done in submit 1

        runFrucComputeChain(cmd, hwW, hwH, /*useNativeSrc*/ true, slot);
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-HW] §B1a frame#0 FRUC compute chain dispatched "
                        "(image→buffer copy + chain) (%ux%u mv=%ux%u)",
                        hwW, hwH, m_FrucMvWidth, m_FrucMvHeight);
        }
    }

    // §J.3.f bug fix round 2 (2026-05-04) — overlay needs three steps,
    // first patch only added the third.  Adding the missing two before
    // BeginRenderPass:
    //   * drainOverlayStash() — pop SDL_Surface from atomic stash that
    //     notifyOverlayUpdated populated, alloc GPU image + staging if
    //     surface size changed, mark pendingUpload.
    //   * uploadPendingOverlay(cmd) — record cmdCopyBufferToImage +
    //     barrier inside the cmd buffer (must be OUTSIDE render pass —
    //     cmdCopyBufferToImage is not allowed inside).
    //   * (already done) drawOverlayInRenderPass(cmd) inside render pass
    //     before CmdEndRenderPass.
    drainOverlayStash();
    uploadPendingOverlay(cmd);

    VkClearValue clearVal = {};
    clearVal.color.float32[0] = 0.0f;
    clearVal.color.float32[1] = 0.0f;
    clearVal.color.float32[2] = 0.0f;
    clearVal.color.float32[3] = 1.0f;

    // §B1b 2026-05-06 — interp render pass (only in dual mode).
    //
    // FRUC compute chain above wrote m_FrucInterpRgbBuf via storage-buffer
    // shader writes (compute stage).  Interp pipeline samples it as a
    // storage buffer in fragment stage — Vulkan requires explicit cross-
    // stage barrier (COMPUTE_SHADER write → FRAGMENT_SHADER read).  SW path
    // doesn't issue this barrier explicitly because subpass external
    // dependency in the render pass + lenient NV driver let it work, but
    // we add it here for spec correctness on HW path.
    if (dualPresentThisFrame) {
        // §B-quality (d) 2026-05-06 — barrier 保護所有 fragment-read storage
        // buffers from compute write: m_FrucInterpRgbBuf (interp #1),
        // m_FrucCurrRgbBuf (real path 走 §B-quality (d) 走 compute-NV12→RGB),
        // §B2 m_FrucInterpRgbBuf2 (TRIPLE 第二張 interp).
        const int kRgbBars = triplePresentThisFrame ? 3 : 2;
        VkBufferMemoryBarrier rgbBufBars[3] = {};
        rgbBufBars[0].sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        rgbBufBars[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        rgbBufBars[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rgbBufBars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rgbBufBars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rgbBufBars[0].buffer = m_FrucInterpRgbBuf;
        rgbBufBars[0].offset = 0;
        rgbBufBars[0].size   = VK_WHOLE_SIZE;
        rgbBufBars[1] = rgbBufBars[0];
        rgbBufBars[1].buffer = m_FrucCurrRgbBuf;
        rgbBufBars[2] = rgbBufBars[0];
        rgbBufBars[2].buffer = m_FrucInterpRgbBuf2;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, (uint32_t)kRgbBars, rgbBufBars, 0, nullptr);

        // Resolve push-const PFN (m_RtPfn doesn't carry it).
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
            frame ? frame->width : 0, frame ? frame->height : 0, 0, 0
        };
        if (pfnCmdPushConst) {
            pfnCmdPushConst(cmd, m_InterpPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(pcInterp), &pcInterp);
        }
        m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
        drawOverlayInRenderPass(cmd);
        m_RtPfn.CmdEndRenderPass(cmd);
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-HW] §B1b frame#0 dual: interp render pass OK (imgA=%u)",
                        imgIdxA);
        }
    }

    // §B2 2026-05-06 — TRIPLE 第二張 interp render pass on imgC.  和 interp #1
    // 共用 m_InterpPipeline，但 sample m_FrucInterpRgbBuf2 (binding via
    // m_InterpDescSet2).  push constant {srcW, srcH} 跟 #1 一樣.
    if (triplePresentThisFrame) {
        auto getDevPa2t = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdPushConst2 = (PFN_vkCmdPushConstants)getDevPa2t(m_Device, "vkCmdPushConstants");

        VkRenderPassBeginInfo rpbiC = {};
        rpbiC.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpbiC.renderPass           = m_RenderPass;
        rpbiC.framebuffer          = m_Framebuffers[imgIdxC];
        rpbiC.renderArea.extent    = m_SwapchainExtent;
        rpbiC.clearValueCount      = 1;
        rpbiC.pClearValues         = &clearVal;
        m_RtPfn.CmdBeginRenderPass(cmd, &rpbiC, VK_SUBPASS_CONTENTS_INLINE);
        m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_InterpPipelineLayout, 0,
                                      1, &m_InterpDescSet2, 0, nullptr);
        struct { int srcW, srcH, _pad0, _pad1; } pcInterp2 = {
            frame ? frame->width : 0, frame ? frame->height : 0, 0, 0
        };
        if (pfnCmdPushConst2) {
            pfnCmdPushConst2(cmd, m_InterpPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(pcInterp2), &pcInterp2);
        }
        m_RtPfn.CmdDraw(cmd, 3, 1, 0, 0);
        drawOverlayInRenderPass(cmd);
        m_RtPfn.CmdEndRenderPass(cmd);
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-HW] §B2 frame#0 triple: interp_2 render pass OK (imgC=%u)",
                        imgIdxC);
        }
    }

    //  6b. Begin real-frame render pass — clear to opaque black.
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
    //
    // §B-quality (d) 2026-05-06 — VIPLE_VKFRUC_REAL_USE_CRGB=1 path:
    // dual-mode real frame uses m_InterpPipeline reading m_FrucCurrRgbBuf
    // (compute-NV12→RGB output) instead of m_GraphicsPipeline (ycbcr sampler
    // on vkf->img[0]).  Reason: dual-present alternates [interp, real] at
    // 30Hz and the two NV12→RGB conversions (compute shader vs hardware
    // ycbcr sampler) produce subtly different RGB on the same input —
    // causing "上下抖動" even when interp content is identical to real.
    // Routing both through compute-NV12→RGB removes the asymmetry.
    // Single-mode (no FRUC) keeps original ycbcr path; no risk of regression.
    static std::atomic<bool> s_realPathLogged{false};
    // §B-quality (d) 2026-05-06 — VIPLE_VKFRUC_REAL_USE_CRGB env reads now
    // hoisted to top of renderFrame (line ~7757) for use by Path D too.
    // Local `useCrgbForReal` const inherited from outer scope.
    if (useCrgbForReal) {
        m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_InterpPipeline);
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_InterpPipelineLayout, 0,
                                      1, &m_RealCurrRgbDescSet, 0, nullptr);
        struct { int srcW, srcH, _pad0, _pad1; } pcReal = {
            frame ? frame->width : 0, frame ? frame->height : 0, 0, 0
        };
        auto getDevPa3 = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
            m_Instance, "vkGetDeviceProcAddr");
        auto pfnCmdPushConst3 = (PFN_vkCmdPushConstants)getDevPa3(m_Device, "vkCmdPushConstants");
        if (pfnCmdPushConst3) {
            pfnCmdPushConst3(cmd, m_InterpPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(pcReal), &pcReal);
        }
        bool expected = false;
        if (s_realPathLogged.compare_exchange_strong(expected, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-REAL-PATH] dual-mode real frame using "
                        "m_InterpPipeline + m_FrucCurrRgbBuf (compute-NV12→RGB)");
        }
    } else {
        m_RtPfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_GraphicsPipeline);
        m_RtPfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      m_GraphicsPipelineLayout, 0,
                                      1, &m_SlotDescSet[slot], 0, nullptr);
        bool expected = false;
        if (dualPresentThisFrame &&
            s_realPathLogged.compare_exchange_strong(expected, true)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-REAL-PATH] dual-mode real frame using "
                        "m_GraphicsPipeline + ycbcr sampler (default)");
        }
    }
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VKFRUC] frame#0 CmdBindPipeline OK");
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
    // §B1a 2026-05-06 — oldLayout SHADER_READ_ONLY_OPTIMAL → GENERAL
    // matching B1a's acquireBar change (image stays in GENERAL).  This
    // is now a same-layout barrier — no transition — but still emits
    // the cache flush + execution dep needed before FFmpeg's next decode
    // queue submission re-uses the image.
    // §J.3.e.2.i.10f Path D — when active, this main cmd buffer didn't
    // touch vkf->img[0] (copy submit handled the image; main submit
    // uses our m_FrucCurrRgbBuf for fragment shader).  The releaseBar
    // for vkf->img[0] was already emitted in the copy cmd buffer.  Skip.
    if (!pathDActive) {
        VkImageMemoryBarrier releaseBar = {};
        releaseBar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        releaseBar.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        releaseBar.dstAccessMask       = 0;
        releaseBar.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
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
    }

    VkResult endVr = m_RtPfn.EndCommandBuffer(cmd);
    if (firstFrame) SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VKFRUC] frame#0 EndCommandBuffer returned %d", (int)endVr);

    // ---- 7. Submit ----
    // §B1b 2026-05-06 — dual mode adds wait on second acquire sem +
    // signal on second renderDone sem (per-image, not per-slot, to avoid
    // sem-reuse race when imgIdxA != imgIdxB and they get re-acquired in
    // a different order).  Single mode keeps original 2-sem submit.
    if (dualPresentThisFrame) {
        // §B2 2026-05-06 — TRIPLE adds 4th wait sem (interp_2 acquire) +
        // 4th signal sem (interp_2 swapchain renderDone).  DUAL stays at 3.
        // §B-NVOF Phase 7B 2026-05-06 — when m_NvOfTimelineValue > 0 (= last
        // frame's OF V_out signaled), add wait sem on m_NvOfTimelineSem so
        // this frame's vkCmdCopyImage to (now-swapped) input image waits for
        // last frame's OF to finish reading it.  Replaces CPU vkWaitSemaphores
        // in nvOF kick-off block (saves ~3-5 ms/frame block-on-CPU).
        bool waitNvOf = m_NvOfReady && m_NvOfTimelineValue > 0;
        VkSemaphore          waitSems[5]   = {};
        VkPipelineStageFlags waitMasks[5]  = {};
        uint64_t             waitVals[5]   = {};
        int wIdx = 0;
        waitSems[wIdx]  = m_SlotAcquireSem[slot][0];
        waitMasks[wIdx] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        waitVals[wIdx]  = 0; wIdx++;
        waitSems[wIdx]  = m_SlotAcquireSem[slot][1];
        waitMasks[wIdx] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        waitVals[wIdx]  = 0; wIdx++;
        if (triplePresentThisFrame) {
            waitSems[wIdx]  = m_SlotAcquireSem[slot][2];
            waitMasks[wIdx] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            waitVals[wIdx]  = 0; wIdx++;
        }
        // §J.3.e.2.i.10f Path D — when active, replace vkf->sem wait
        // with m_CopyDoneSem wait (cross-submit chain to copy buffer).
        // Stage mask = COMPUTE_SHADER (nv12rgb is the first consumer of
        // m_SwFrucNv12Buf which copy buffer wrote).  vkf wait already
        // satisfied in copy submit.
        if (pathDActive) {
            waitSems[wIdx]  = m_CopyDoneSem;
            waitMasks[wIdx] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            waitVals[wIdx]  = pathDCopyDoneVal; wIdx++;
        } else {
            waitSems[wIdx]  = vkf->sem[0];
            waitMasks[wIdx] = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                            | VK_PIPELINE_STAGE_TRANSFER_BIT;
            waitVals[wIdx]  = vkf->sem_value[0]; wIdx++;
        }
        if (waitNvOf) {
            waitSems[wIdx]  = m_NvOfTimelineSem;
            waitMasks[wIdx] = VK_PIPELINE_STAGE_TRANSFER_BIT;
            waitVals[wIdx]  = m_NvOfTimelineValue; wIdx++;
        }
        const int kWaitCount = wIdx;
        // §J.3.e.2.i.10f Path D — drop vkf->sem[0] signal from signalSems
        // (copy submit already signaled V+1).  TRIPLE: 4→3 sigs.  DUAL:
        // 3→2 sigs.
        const int kSigCount  = pathDActive ? (triplePresentThisFrame ? 3 : 2)
                                           : (triplePresentThisFrame ? 4 : 3);
        VkSemaphore     signalSems[4] = {
            m_SwapchainRenderDoneSem[imgIdxA],
            m_SwapchainRenderDoneSem[imgIdxB],
            // [2]: TRIPLE → renderDoneC; DUAL non-D → vkf->sem; DUAL+D → unused
            triplePresentThisFrame ? m_SwapchainRenderDoneSem[imgIdxC]
                                   : (pathDActive ? VK_NULL_HANDLE : vkf->sem[0]),
            // [3]: TRIPLE non-D → vkf->sem; D-active → unused; DUAL → unused
            pathDActive ? VK_NULL_HANDLE : vkf->sem[0]
        };
        uint64_t        signalVals[4] = {
            0, 0,
            triplePresentThisFrame ? 0 : (pathDActive ? 0 : (vkf->sem_value[0] + 1)),
            pathDActive ? 0 : (vkf->sem_value[0] + 1)
        };

        VkTimelineSemaphoreSubmitInfo tssi = {};
        tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tssi.waitSemaphoreValueCount   = (uint32_t)kWaitCount;
        tssi.pWaitSemaphoreValues      = waitVals;
        tssi.signalSemaphoreValueCount = (uint32_t)kSigCount;
        tssi.pSignalSemaphoreValues    = signalVals;
        VkSubmitInfo si = {};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = &tssi;
        si.waitSemaphoreCount   = (uint32_t)kWaitCount;
        si.pWaitSemaphores      = waitSems;
        si.pWaitDstStageMask    = waitMasks;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = (uint32_t)kSigCount;
        si.pSignalSemaphores    = signalSems;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] %s: vkQueueSubmit failed (%d)",
                         triplePresentThisFrame ? "triple" : "dual", (int)vr);
            // §J.3.e.2.i.10f Path D — already unlocked after copy submit; skip.
            if (!pathDActive && vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
            return;
        }
    } else {
        VkSemaphore     waitSems[2]   = { m_SlotAcquireSem[slot][0], vkf->sem[0] };
        VkPipelineStageFlags waitMasks[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_TRANSFER_BIT,
        };
        uint64_t        waitVals[2]   = { 0, vkf->sem_value[0] };
        VkSemaphore     signalSems[2] = { m_SlotRenderDoneSem[slot][0], vkf->sem[0] };
        uint64_t        signalVals[2] = { 0, vkf->sem_value[0] + 1 };
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &si, m_SlotInFlightFence[slot]);
        }
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VKFRUC] vkQueueSubmit failed (%d)", (int)vr);
            if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
            return;
        }
    }
    if (firstFrame) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] frame#0 vkQueueSubmit OK — first GPU work in flight");
    }

    // §B-NVOF Phase 4b/4c 2026-05-06 — kick off nvOFExecuteVk for this
    // frame's NV12 input (vkCmdCopyImage above already populated
    // m_NvOfInputCurr inside main cmd buf).  Pattern: small "marker" submit
    // signals m_NvOfTimelineSem at V_in (queue order ensures it fires after
    // main work completes).  nvOFExecuteVk waits V_in, signals V_out.
    // CPU does NOT wait for V_out — fire-and-forget for now (Phase 4d/5
    // will integrate the result into the chain).  Swap curr↔prev handles
    // for next frame.
    if (m_NvOfReady && m_NvOfFuncList) {
        // §B-NVOF Phase 4b — skip the very first frame: m_NvOfInputPrev is
        // still UNDEFINED (never written) so OF on (curr, prev) would read
        // garbage from prev → undefined HW behaviour.  Use the first frame
        // just to populate curr; on swap, that becomes prev for frame 1+.
        static thread_local uint32_t s_NvOfFrameCount = 0;
        const uint32_t frameNum = s_NvOfFrameCount++;
        if (frameNum == 0) {
            // first frame — only swap so frame 1 sees a valid prev.
            std::swap(m_NvOfInputCurr,    m_NvOfInputPrev);
            std::swap(m_NvOfInputCurrMem, m_NvOfInputPrevMem);
            std::swap(m_NvOfHandleCurr,   m_NvOfHandlePrev);
        }
        if (frameNum > 0) {
        const uint64_t inSigVal = m_NvOfTimelineValue + 1;
        const uint64_t outSigVal = inSigVal + 1;
        // 1) graphics-queue marker submit signaling V_in.
        VkTimelineSemaphoreSubmitInfo tsMark = {};
        tsMark.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tsMark.signalSemaphoreValueCount = 1;
        tsMark.pSignalSemaphoreValues    = &inSigVal;
        VkSubmitInfo siMark = {};
        siMark.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        siMark.pNext                = &tsMark;
        siMark.commandBufferCount   = 0;
        siMark.signalSemaphoreCount = 1;
        siMark.pSignalSemaphores    = &m_NvOfTimelineSem;
        VkResult vrMark;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vrMark = m_RtPfn.QueueSubmit(m_GraphicsQueue, 1, &siMark, VK_NULL_HANDLE);
        }
        if (vrMark == VK_SUCCESS) {
            // 2) nvOFExecuteVk on OF queue (SDK auto-submits internally).
            auto* funcList = (NV_OF_VK_API_FUNCTION_LIST*)m_NvOfFuncList;
            NV_OF_SYNC_VK waitSync = {};
            waitSync.semaphore = m_NvOfTimelineSem;
            waitSync.value     = inSigVal;
            NV_OF_SYNC_VK signalSync = {};
            signalSync.semaphore = m_NvOfTimelineSem;
            signalSync.value     = outSigVal;
            NV_OF_EXECUTE_INPUT_PARAMS_VK ofIn = {};
            ofIn.inputFrame      = (NvOFGPUBufferHandle)m_NvOfHandleCurr;
            ofIn.referenceFrame  = (NvOFGPUBufferHandle)m_NvOfHandlePrev;
            ofIn.disableTemporalHints = NV_OF_FALSE;
            ofIn.numWaitSyncs    = 1;
            ofIn.pWaitSyncs      = &waitSync;
            NV_OF_EXECUTE_OUTPUT_PARAMS_VK ofOut = {};
            ofOut.outputBuffer   = (NvOFGPUBufferHandle)m_NvOfHandleFlow;
            ofOut.pSignalSync    = &signalSync;
            NV_OF_STATUS s = funcList->nvOFExecuteVk((NvOFHandle)m_NvOfHandle, &ofIn, &ofOut);
            // Periodic log: don't spam every frame; first frame + every ~5s.
            static std::atomic<int> s_ofExecLogCount{0};
            int n = s_ofExecLogCount.fetch_add(1, std::memory_order_relaxed);
            if (n == 0 || (n % 300 == 0)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-NVOF] nvOFExecuteVk #%d status=%d "
                            "(V_in=%llu V_out=%llu)",
                            n, (int)s,
                            (unsigned long long)inSigVal,
                            (unsigned long long)outSigVal);
            }
            if (s == NV_OF_SUCCESS) {
                m_NvOfTimelineValue = outSigVal;
                // §B-NVOF Phase 7B 2026-05-06 — async cross-queue. Removed
                // CPU vkWaitSemaphores (was ~3-5 ms/frame block at 1080p).
                // Sync now via timeline sem in NEXT frame's main submit wait
                // list (= m_NvOfTimelineValue) — when next frame's cmd buf
                // executes vkCmdCopyImage to swapped input image, it blocks
                // at TRANSFER_BIT until OF has finished reading the to-be-
                // overwritten image. Eliminates the race CPU wait was guarding.
                std::swap(m_NvOfInputCurr,    m_NvOfInputPrev);
                std::swap(m_NvOfInputCurrMem, m_NvOfInputPrevMem);
                std::swap(m_NvOfHandleCurr,   m_NvOfHandlePrev);
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-NVOF] marker QueueSubmit failed vr=%d", (int)vrMark);
        }
        }  // end if (frameNum > 0)
    }

    // ---- 8. Update AVVkFrame state ----
    // §J.3.e.2.i.3.e: tell FFmpeg the image's new state so it can issue
    // a correct barrier (GENERAL → DECODE_DPB) on its decode queue when
    // it re-uses this image as a reference frame.
    // §J.3.e.2.i.10f Path D — when active, this state was already updated
    // after the copy submit (vkf->sem_value[0] += 1, layout = GENERAL,
    // unlock_frame called).  Skip both blocks.
    if (!pathDActive) {
        vkf->access[0]     = (VkAccessFlagBits)0;
        vkf->layout[0]     = VK_IMAGE_LAYOUT_GENERAL;
        vkf->sem_value[0] += 1;

        // ---- 9. Unlock AVVkFrame ----
        if (vkfc->unlock_frame) vkfc->unlock_frame(fc, vkf);
    }

    // ---- 10. Present ----
    // Order:
    //   DUAL:   piA (interp midpoint) → piB (real)
    //   TRIPLE: piA (interp 1/3) → piC (interp 2/3) → piB (real)
    if (dualPresentThisFrame) {
        VkPresentInfoKHR piA = {};
        piA.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piA.waitSemaphoreCount = 1;
        piA.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxA];
        piA.swapchainCount     = 1;
        piA.pSwapchains        = &m_Swapchain;
        piA.pImageIndices      = &imgIdxA;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piA);
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] %s: present(interp%s) returned %d",
                        triplePresentThisFrame ? "triple" : "dual",
                        triplePresentThisFrame ? " 1/3" : "",
                        (int)vr);
        }
        if (triplePresentThisFrame) {
            VkPresentInfoKHR piC = {};
            piC.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            piC.waitSemaphoreCount = 1;
            piC.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxC];
            piC.swapchainCount     = 1;
            piC.pSwapchains        = &m_Swapchain;
            piC.pImageIndices      = &imgIdxC;
            {
                std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
                vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piC);
            }
            if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC] triple: present(interp 2/3) returned %d", (int)vr);
            }
        }
        VkPresentInfoKHR piB = {};
        piB.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        piB.waitSemaphoreCount = 1;
        piB.pWaitSemaphores    = &m_SwapchainRenderDoneSem[imgIdxB];
        piB.swapchainCount     = 1;
        piB.pSwapchains        = &m_Swapchain;
        piB.pImageIndices      = &imgIdxB;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &piB);
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] %s: present(real) returned %d",
                        triplePresentThisFrame ? "triple" : "dual", (int)vr);
        }
        if (firstFrame) {
            if (triplePresentThisFrame) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-HW] §B2 frame#0 TRIPLE OK — interp_1 imgA=%u + interp_2 imgC=%u + real imgB=%u presented",
                            imgIdxA, imgIdxC, imgIdxB);
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VKFRUC-HW] §B1b frame#0 DUAL OK — interp imgA=%u + real imgB=%u presented",
                            imgIdxA, imgIdxB);
            }
        }
    } else {
        VkPresentInfoKHR pi = {};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_SlotRenderDoneSem[slot][0];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_Swapchain;
        pi.pImageIndices      = &imgIdx;
        {
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
            vr = m_RtPfn.QueuePresentKHR(m_GraphicsQueue, &pi);
        }
        if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] vkQueuePresentKHR returned %d", (int)vr);
        }
        if (firstFrame) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC] frame#0 vkQueuePresentKHR OK — first frame complete");
        }
    }

    // §B1a 2026-05-06 — periodic Stats / GPU-PROF emit, ported from
    // renderFrameSw §J.3.e.2.i.6 block.  Independent thread_local state
    // so SW + HW path don't cross-contaminate.  SW-PROF (memcpy/wait/
    // submit/present per-phase) is SW-specific and omitted here.
    {
        using namespace std::chrono;
        static thread_local steady_clock::time_point s_HwLastPresent{};
        static thread_local steady_clock::time_point s_HwBucketStart{};
        static thread_local std::vector<double> s_HwFrameMsRing;
        static thread_local uint64_t s_HwCumulReal = 0;
        static thread_local uint64_t s_HwCumulInterp = 0;
        auto now = steady_clock::now();
        if (s_HwLastPresent.time_since_epoch().count() != 0) {
            double dtMs = duration_cast<duration<double, std::milli>>(now - s_HwLastPresent).count();
            s_HwFrameMsRing.push_back(dtMs);
        } else {
            s_HwBucketStart = now;
        }
        s_HwLastPresent = now;
        s_HwCumulReal++;
        if (triplePresentThisFrame) {
            s_HwCumulInterp += 2;  // §B2: TRIPLE 每 server frame 推 2 張 interp
        } else if (dualPresentThisFrame) {
            s_HwCumulInterp++;
        }

        double bucketSec = duration_cast<duration<double>>(now - s_HwBucketStart).count();
        if (bucketSec >= 5.0 && !s_HwFrameMsRing.empty()) {
            std::vector<double> sorted = s_HwFrameMsRing;
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
            int gpuN = m_FrucGpuUsCount;
            double gpuTotalUs = (gpuN > 0) ? (m_FrucGpuUsAccum / gpuN) : 0.0;
            double sNv12 = (gpuN > 0) ? (m_FrucGpuStageUsAccum[0] / gpuN) : 0.0;
            double sMe   = (gpuN > 0) ? (m_FrucGpuStageUsAccum[1] / gpuN) : 0.0;
            double sMed  = (gpuN > 0) ? (m_FrucGpuStageUsAccum[2] / gpuN) : 0.0;
            double sWarp = (gpuN > 0) ? (m_FrucGpuStageUsAccum[3] / gpuN) : 0.0;
            double sCopy = (gpuN > 0) ? (m_FrucGpuStageUsAccum[4] / gpuN) : 0.0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] cumul real=%llu interp=%llu "
                        "compute_gpu_total=%.3fms (n=%d) "
                        "(swMode=%d frucMode=%d dualMode=%d)",
                        (unsigned long long)s_HwCumulReal,
                        (unsigned long long)s_HwCumulInterp,
                        gpuTotalUs / 1000.0,
                        gpuN,
                        m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-GPU-PROF] nv12rgb=%.0fus me=%.0fus "
                        "median=%.0fus warp=%.0fus copy=%.0fus "
                        "(total=%.0fus, n=%d)",
                        sNv12, sMe, sMed, sWarp, sCopy,
                        gpuTotalUs, gpuN);
            m_FrucGpuUsAccum = 0.0;
            for (int i = 0; i < kFrucStageCount; ++i) {
                m_FrucGpuStageUsAccum[i] = 0.0;
            }
            m_FrucGpuUsCount = 0;
            s_HwFrameMsRing.clear();
            s_HwBucketStart = now;
        }
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

    // VipleStream: Ctrl+Alt+Shift+F runtime hotkey snapshot.  toggleFRUC()
    // 把 m_FRUCPaused 翻轉後，下一幀進這裡讀到的快照值決定本幀是否走
    // dual-present / FRUC compute / interp render pass。一律快照同一份
    // atomic 值避免本幀內 partial state（例如 acquire 走 dual 但 submit
    // 走 single → 半個 swapchain image 沒 release 卡到下一幀）.
    const bool frucPausedThisFrame = m_FRUCPaused.load();
    const bool dualPresentThisFrame = m_DualMode && !frucPausedThisFrame;
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
    // §B-DUMP — same fence-wait flush point as renderFrame (HW path).
    flushDumpSlotIfPending(slot);
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
    if (dualPresentThisFrame) {
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
    if (useNativeDecode && m_FrucMode && m_FrucReady && !frucPausedThisFrame && m_SwImageLayoutInited) {
        // §J.3.e.2.i.8 Phase 2.5 v2 (2026-05-05) — pre-copy barrier on
        // m_SwFrucNv12Buf to drain prior frame's compute SHADER_READ before
        // this frame's TRANSFER_WRITE.  Closes the residual cross-frame WAW
        // race v1.3.275~277 acknowledged: with kFrucFramesInFlight=2 and a
        // single shared NV12 mirror buffer, slot 0's compute may still be
        // reading the buffer when slot 1's vkCmdCopyImageToBuffer
        // overwrites it.  v1.3.276 measured "occasional blur" from this
        // race over 2940+ frames; v1.3.276→277 tried per-slot buffer →
        // reliable VK_ERROR_DEVICE_LOST after ~1380 frames on NV 596.84
        // (driver-side descriptor set / buffer state-tracking issue).
        // Single-buffer + barrier sidesteps the NV bug entirely while
        // serialising the WAW at the command stream level — pipeline
        // barriers respect submission order across cmd buffers on the
        // same queue.  Folded into the same vkCmdPipelineBarrier call as
        // the m_SwUploadImage SHADER_READ → TRANSFER_SRC layout transition
        // so we don't pay an extra barrier dispatch.
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
        VkBufferMemoryBarrier preCopyBar = {};
        preCopyBar.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        preCopyBar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        preCopyBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        preCopyBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        preCopyBar.buffer = m_SwFrucNv12Buf;
        preCopyBar.offset = 0;
        preCopyBar.size   = VK_WHOLE_SIZE;
        m_RtPfn.CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &preCopyBar, 1, &toSrcBar);

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
    if (m_FrucMode && m_FrucReady && !frucPausedThisFrame) {
        // §J.3.e.2.i.8 Phase 2.5 — useNativeDecode gates FRUC's NV12 source.
        runFrucComputeChain(cmd, (uint32_t)m_SwImageWidth, (uint32_t)m_SwImageHeight,
                            useNativeDecode, slot);
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
    if (dualPresentThisFrame) {
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
        (useNativeDecode && m_FrucMode && m_FrucReady && !frucPausedThisFrame)
            ? VK_PIPELINE_STAGE_TRANSFER_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkResult vr;
    // §J.3.e.2.i.8 Phase 1.5c — always allocate gfx timeline signal value (so
    // decode side can always vkWaitSemaphores on the latest published value).
    uint64_t gfxSignalVal = m_GfxTimelineNext.fetch_add(1, std::memory_order_acq_rel);
    if (dualPresentThisFrame) {
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
            std::lock_guard<std::recursive_mutex> lk(s_VkFrucQueueLock);
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
        if (dualPresentThisFrame) s_CumulInterp++;

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
            // §J.3.e.2.i.6 / §J.3.g v2 — GPU compute chain timing per stage,
            // averaged over the window.  Stage attribution is what told us
            // §J.3.g v1's ME-resolution-downsample experiment couldn't help
            // (real bottleneck wasn't ME).  Now we report all 5 stages so
            // the next optimisation lands on the actual hot one.
            int gpuN = m_FrucGpuUsCount;
            double gpuTotalMeanUs = (gpuN > 0) ? (m_FrucGpuUsAccum / gpuN) : 0.0;
            double sNv12 = (gpuN > 0) ? (m_FrucGpuStageUsAccum[0] / gpuN) : 0.0;
            double sMe   = (gpuN > 0) ? (m_FrucGpuStageUsAccum[1] / gpuN) : 0.0;
            double sMed  = (gpuN > 0) ? (m_FrucGpuStageUsAccum[2] / gpuN) : 0.0;
            double sWarp = (gpuN > 0) ? (m_FrucGpuStageUsAccum[3] / gpuN) : 0.0;
            double sCopy = (gpuN > 0) ? (m_FrucGpuStageUsAccum[4] / gpuN) : 0.0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-Stats] cumul real=%llu interp=%llu "
                        "compute_gpu_total=%.3fms (n=%d) "
                        "(swMode=%d frucMode=%d dualMode=%d)",
                        (unsigned long long)s_CumulReal,
                        (unsigned long long)s_CumulInterp,
                        gpuTotalMeanUs / 1000.0,
                        gpuN,
                        m_SwMode ? 1 : 0, m_FrucMode ? 1 : 0, m_DualMode ? 1 : 0);
            // Per-stage breakdown (us, mean over window).  Look for the
            // largest stage; share-of-total tells you where to optimise.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VKFRUC-GPU-PROF] nv12rgb=%.0fus me=%.0fus "
                        "median=%.0fus warp=%.0fus copy=%.0fus "
                        "(total=%.0fus, n=%d)",
                        sNv12, sMe, sMed, sWarp, sCopy,
                        gpuTotalMeanUs, gpuN);
            m_FrucGpuUsAccum = 0.0;
            for (int i = 0; i < kFrucStageCount; ++i) {
                m_FrucGpuStageUsAccum[i] = 0.0;
            }
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
