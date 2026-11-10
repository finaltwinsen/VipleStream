// VipleStream 2.0 §VR M3a X2 — XrVideo 實作。設計見 xrvideo.h。

#include "xrvideo.h"

#include "xrphase.h"  // §VR-XRLOOP：相位換算（header-only）
#include "streaming/video/ffmpeg-renderers/plvk_common.h"
#include "streaming/vr/vrframemeta.h"
#include "streaming/video/pyrowave_decoder_bridge.h"  // §PYROWAVE-XR-ZC：PyroWaveVkInterop
#include "streaming/video/bitstreamdump.h"  // §VR-REC：開了 --dump-bitstream 時記下每一格顯示哪一張

#include <QFile>
#include <QImage>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>
#include <libplacebo/shaders/colorspace.h>  // §VR-UNWARP-YUV：pl_shader_decode_color／pl_shader_color_map

namespace {

void lockQueueCb(void* ctx, uint32_t, uint32_t)
{
    static_cast<XrContext*>(ctx)->queueMutex().lock();
}

void unlockQueueCb(void* ctx, uint32_t, uint32_t)
{
    static_cast<XrContext*>(ctx)->queueMutex().unlock();
}

// §PYROWAVE-XR-ZC：FFmpeg（hwcontext_vulkan）對共用 queue 的鎖，同一把 queueMutex
void avLockQueueCb(AVHWDeviceContext* dev, uint32_t, uint32_t)
{
    static_cast<XrContext*>(dev->user_opaque)->queueMutex().lock();
}

void avUnlockQueueCb(AVHWDeviceContext* dev, uint32_t, uint32_t)
{
    static_cast<XrContext*>(dev->user_opaque)->queueMutex().unlock();
}

uint64_t nowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename T>
T xrS(XrStructureType type)
{
    T s;
    std::memset(&s, 0, sizeof(s));
    s.type = type;
    return s;
}

const char* modeName(int m)
{
    switch (m) {
    case 1: return "srgb-mutable";
    case 2: return "rgba16f-linear";
    case 3: return "unorm8-linear";
    default: return "none";
    }
}

}  // namespace

XrVideo::XrVideo(XrContext* ctx, const Config& config) : m_Ctx(ctx), m_Config(config)
{
    if (m_Config.synth) {
        // §VR-SYNTH 的第二次發布（合成的那一半）不在 queue 鎖內，atomicPublish 的保證（queue 鎖內看到 releaseNo 變了，
        // 快照就一定是新的那一張）對它不成立，它也不叫醒 waitNewerThan()。XrContext::bringUp 本來就不會兩個一起開，
        // 這裡再擋一次，免得日後只改到其中一邊。
        // flightNotify（§VR-LATCH-FLIGHT）不在這裡關：它只看第一次發布，補幀開著時照樣作用（見 xrvideo.h）
        m_Config.atomicPublish = false;
        m_Config.notifyPublish = false;
    }
}

XrVideo::~XrVideo()
{
    destroy();
}

pl_gpu XrVideo::gpu() const
{
    return m_Vulkan != nullptr ? m_Vulkan->gpu : nullptr;
}

// §VR-PASS：libplacebo 每畫完一個 pass 呼叫一次（影像 render thread，在 pl_dispatch_finish／pl_render_image 裡）。
// last＝這個 pass 最近一次量到的 GPU 時間（奈秒）；結果是晚幾格才查得到的，所以它不是「這一次」的時間，偶爾連續
// 兩次拿到同一筆、或一次進來兩筆只看到後面那一筆——60 秒的平均與極值夠用。num_samples＝0＝還沒有任何結果
// （這顆驅動的佇列不支援時間戳查詢的話永遠是 0）
void XrVideo::passInfoCb(void* priv, const pl_dispatch_info* info)
{
    if (info == nullptr || info->num_samples <= 0) {
        return;
    }
    static_cast<XrVideo*>(priv)->m_PassStats.note(
        PassStats::slotForDescription(info->shader != nullptr ? info->shader->description : nullptr), info->last);
}

void XrVideo::renderInfoCb(void* priv, const pl_render_info* info)
{
    if (info == nullptr || info->pass == nullptr || info->pass->num_samples <= 0) {
        return;
    }
    static_cast<XrVideo*>(priv)->m_PassStats.note(PassStats::slotForConvert(info->index), info->pass->last);
}

bool XrVideo::init(QString* error)
{
    pl_log_params lp = {};
    lp.log_cb = PlvkCommon::logCallback;
    lp.log_level = PL_LOG_WARN;
    m_Log = pl_log_create(PL_API_VER, &lp);

    const std::vector<const char*>& exts = m_Ctx->vkEnabledDeviceExtensions();
    pl_vulkan_import_params ip = {};
    ip.instance = m_Ctx->vkInstance();
    ip.get_proc_addr = m_Ctx->vkGetInstanceProcAddr();
    ip.phys_device = m_Ctx->vkPhysicalDevice();
    ip.device = m_Ctx->vkDevice();
    ip.extensions = exts.empty() ? nullptr : exts.data();
    ip.num_extensions = static_cast<int>(exts.size());
    ip.queue_graphics.index = m_Ctx->vkQueueFamily();
    ip.queue_graphics.count = 1;
    // compute／transfer 留 {0}：libplacebo 會退用 graphics queue（XrContext 只建了一個 queue）
    ip.features = m_Ctx->vkEnabledFeatures();
    ip.lock_queue = lockQueueCb;
    ip.unlock_queue = unlockQueueCb;
    ip.queue_ctx = m_Ctx;
    ip.max_api_version = m_Ctx->vkApiVersion();
    if (ip.features == nullptr) {
        *error = QStringLiteral("Vulkan device is below 1.2 (libplacebo needs 1.2 features)");
        return false;
    }
    m_Vulkan = pl_vulkan_import(m_Log, &ip);
    if (m_Vulkan == nullptr) {
        *error = QStringLiteral("pl_vulkan_import failed");
        return false;
    }
    m_Renderer = pl_renderer_create(m_Log, m_Vulkan->gpu);
    if (m_Renderer == nullptr) {
        *error = QStringLiteral("pl_renderer_create failed");
        return false;
    }
    // §VR-PASS：pl_render_image 一律用這一份參數（內容就是 pl_render_fast_params）；量測開著時多掛一個 callback，
    // libplacebo 每畫完一個 pass 把它的 GPU 計時交給我們
    m_RenderParams = pl_render_fast_params;
    // §VR-UPSCALE（2026-10-29）：串流影像放大改用 Catmull-Rom（pl_render_fast_params 是 GPU 取樣的雙線性）。離線評估
    // （scripts/benchmark/layered/p2_pw_res.py）每眼 1152 放大到 2160 時 Lanczos／bicubic 比雙線性 VMAF 高 4～10、同位元率
    // 追平 2160 原生。Lanczos（libplacebo，全解析度中間圖＋兩趟）頭盔實測 GPU 3.4→7.4 ms、超過 144 Hz 週期，不能用。
    // 注視點路徑（renderUnwarp）的放大在還原 shader 裡做（kUnwarpBody 的 9 次取樣 Catmull-Rom），這裡只管沒開注視點時
    m_RenderParams.upscaler = &pl_filter_catmull_rom;
    if (m_Config.passTimers) {
        m_RenderParams.info_callback = &XrVideo::renderInfoCb;
        m_RenderParams.info_priv = this;
    }
    if (m_Config.synth) {
        m_Dp = pl_dispatch_create(m_Log, m_Vulkan->gpu);
        if (m_Dp == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] pl_dispatch_create failed - frame synthesis off");
            m_Config.synth = false;
        }
        else if (m_Config.passTimers) {
            pl_dispatch_callback(m_Dp, this, &XrVideo::passInfoCb);  // §VR-PASS：我們自己的 pass
        }
    }
    pl_vulkan_sem_params sp = {};
    sp.type = VK_SEMAPHORE_TYPE_TIMELINE;
    sp.initial_value = 0;
    m_Sem = pl_vulkan_sem_create(m_Vulkan->gpu, &sp);
    if (m_Sem == VK_NULL_HANDLE) {
        *error = QStringLiteral("pl_vulkan_sem_create(timeline) failed");
        return false;
    }
    m_SemValue = 0;
    if (!m_Config.testJitter.isEmpty()) {
        // dev（--vr-test-jitter）：格式在命令列解析時已經檢查過，這裡再不對就不注入
        std::string jerr;
        if (m_Jitter.configure(m_Config.testJitter.toStdString(), &jerr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev test jitter: delaying stream frames before render (%s)",
                        m_Jitter.spec().c_str());
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev test jitter ignored (%s): %s",
                        qUtf8Printable(m_Config.testJitter), jerr.c_str());
        }
    }
    m_Diag.reserveWindow();  // §VR-XRLOOP：量測的視窗（每 10 s 由 takeStatsDeferred 整包換掉）
    m_Usable.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        m_StopThread = false;
    }
    m_Thread = std::thread(&XrVideo::renderThreadMain, this);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] video: libplacebo imported the XR VkDevice (api %u.%u, %d device exts) upscaler=catmull-rom",
                VK_API_VERSION_MAJOR(m_Vulkan->api_version), VK_API_VERSION_MINOR(m_Vulkan->api_version),
                ip.num_extensions);
    return true;
}

void XrVideo::markUnusable(const char* why)
{
    if (m_Usable.exchange(false)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video path disabled: %s", why);
    }
}

void XrVideo::setSession(XrSession session)
{
    std::lock_guard<std::mutex> lk(m_MailboxMutex);
    m_Session = session;
}

void XrVideo::destroySwapchainSet(SwapchainSet& s)
{
    pl_gpu g = gpu();
    for (pl_tex& t : s.wrapped) {
        if (t != nullptr && g != nullptr) {
            pl_tex_destroy(g, &t);
        }
    }
    s.wrapped.clear();
    if (s.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(s.swapchain);
        s.swapchain = XR_NULL_HANDLE;
    }
    s.width = s.height = 0;
}

void XrVideo::destroyAllSwapchains()
{
    std::vector<SwapchainSet> retired;
    {
        std::lock_guard<std::mutex> lk(m_SwMutex);
        retired.swap(m_Retired);
        m_HavePublished = false;
        m_Published = Current();
    }
    for (SwapchainSet& s : retired) {
        destroySwapchainSet(s);
    }
    destroySwapchainSet(m_Sw);
    for (int i = 0; i < kRing; i++) {  // §VR-BUFFER
        destroySwapchainSet(m_RingSets[i]);
    }
    {
        std::lock_guard<std::mutex> lk(m_SwMutex);
        for (Current& c : m_RingPub) {
            c = Current();
        }
        m_BufLastShown = 0;
        m_BufShownCount = 0;
        m_BufDepth = 0;  // §VR-BUFFER-AUTO：下一場從 1 格重新開始
    }
    m_RingNext = 0;
    m_SwFormat = 0;
    m_ViewFormat = VK_FORMAT_UNDEFINED;
    m_Mode = TargetMode::None;
}

void XrVideo::reapRetired()
{
    std::vector<SwapchainSet> dead;
    {
        std::lock_guard<std::mutex> lk(m_SwMutex);
        for (auto it = m_Retired.begin(); it != m_Retired.end();) {
            // XR thread 已送出引用更新一代的 xrEndFrame：舊的一代不再被任何 layer 引用
            if (it->generation < m_XrAckGeneration) {
                dead.push_back(std::move(*it));
                it = m_Retired.erase(it);
            }
            else {
                ++it;
            }
        }
    }
    for (SwapchainSet& s : dead) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video swapchain gen=%llu %dx%d retired and destroyed",
                    static_cast<unsigned long long>(s.generation), s.width, s.height);
        destroySwapchainSet(s);
    }
}

void XrVideo::destroy(bool gpuWedged)
{
    m_Usable.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        m_StopThread = true;
    }
    m_MailboxCv.notify_all();
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
    // §REVIEW-1104：補幀幀間隔基準改成成員，拆掉時歸零，下一場才不會沿用上一場的幀率
    m_NominalDelta = 0;
    if (gpuWedged) {
        // 所有 libplacebo 釋放都會等 GPU：只收掉 XR swapchain handle 與沒被 GPU 參照的 mailbox 幀，其餘洩漏
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] video: GPU wedged - leaking libplacebo objects and %d in-flight frame(s)",
                    static_cast<int>(m_WedgedFrames.size()));
        // §REVIEW-1104：wedge 也要銷毀 ring 與投影 swapchain（session 的 child），只清 handle，不 pl_tex_destroy
        for (SwapchainSet* s : {&m_Sw, &m_Proj}) {
            if (s->swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(s->swapchain);
                s->swapchain = XR_NULL_HANDLE;
            }
        }
        for (int i = 0; i < kRing; i++) {
            if (m_RingSets[i].swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(m_RingSets[i].swapchain);
                m_RingSets[i].swapchain = XR_NULL_HANDLE;
            }
        }
        for (SwapchainSet& s : m_Retired) {
            if (s.swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(s.swapchain);
                s.swapchain = XR_NULL_HANDLE;
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_MailboxMutex);
            av_frame_free(&m_Pending);
        }
        m_WedgedFrames.clear();  // 刻意不 free
#ifdef HAVE_DRM
        m_DrmCache.abandon();    // 匯入的材質也可能還被 GPU 參照
#endif
        m_Vulkan = nullptr;
        m_Renderer = nullptr;
        m_Dp = nullptr;  // 刻意洩漏（同上）
        m_Full = m_Flow = nullptr;
        m_Equi[0] = m_Equi[1] = nullptr;
        m_DpFrame = nullptr;
        m_Proj = SwapchainSet();
        m_ProjSem = VK_NULL_HANDLE;
        m_Small[0] = m_Small[1] = nullptr;
        m_Mid[0] = m_Mid[1] = m_FlowC = nullptr;
        m_Sem = VK_NULL_HANDLE;
        m_PwHwDeviceCtx = nullptr;  // 刻意洩漏（同上）
        m_Log = nullptr;
        return;
    }
    if (m_Vulkan != nullptr) {
        pl_gpu_finish(m_Vulkan->gpu);
    }
    destroyAllSwapchains();
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        av_frame_free(&m_Pending);
    }
    if (m_Vulkan != nullptr) {
#ifdef HAVE_DRM
        m_DrmCache.clear(m_Vulkan->gpu, "video destroyed");
#endif
        for (pl_tex& t : m_Tex) {
            pl_tex_destroy(m_Vulkan->gpu, &t);
        }
        pl_tex_destroy(m_Vulkan->gpu, &m_Full);
        pl_tex_destroy(m_Vulkan->gpu, &m_Equi[0]);
        pl_tex_destroy(m_Vulkan->gpu, &m_Equi[1]);
        destroySwapchainSet(m_Proj);
        if (m_DpFrame != nullptr) {
            pl_dispatch_destroy(&m_DpFrame);
        }
        if (m_ProjSem != VK_NULL_HANDLE) {
            pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_ProjSem);
        }
        pl_tex_destroy(m_Vulkan->gpu, &m_Flow);
        pl_tex_destroy(m_Vulkan->gpu, &m_Small[0]);
        pl_tex_destroy(m_Vulkan->gpu, &m_Small[1]);
        pl_tex_destroy(m_Vulkan->gpu, &m_Mid[0]);
        pl_tex_destroy(m_Vulkan->gpu, &m_Mid[1]);
        pl_tex_destroy(m_Vulkan->gpu, &m_FlowC);
        if (m_Dp != nullptr) {
            pl_dispatch_destroy(&m_Dp);
        }
        {
            std::lock_guard<std::mutex> lk(m_TestMutex);
            for (pl_tex& t : m_TestTex) {
                pl_tex_destroy(m_Vulkan->gpu, &t);
            }
        }
        if (m_Sem != VK_NULL_HANDLE) {
            pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_Sem);
        }
        pl_renderer_destroy(&m_Renderer);
        // 匯入的 device 不會被銷毀（只釋放 libplacebo 自己的資源）
        pl_vulkan_destroy(&m_Vulkan);
    }
    // PyroWave 的幀（frames context）在 decoder 與上面的 mailbox 都放掉了，這裡只剩 device context（不會銷毀 device）
    av_buffer_unref(&m_PwHwDeviceCtx);
    m_Sem = VK_NULL_HANDLE;
    pl_log_destroy(&m_Log);
}

uint64_t XrVideo::waitingAgeUs() const
{
    const uint64_t since = m_WaitSinceUs.load(std::memory_order_relaxed);
    if (since == 0) {
        return 0;
    }
    const uint64_t now = nowUs();
    return now > since ? now - since : 0;
}

void XrVideo::submit(const AVFrame* frame)
{
    if (!usable() || frame == nullptr) {
        return;
    }
    if (m_Config.testStallMs > 0) {
        // dev（--xr-test-stall-ms）：第一幀進來 20 s 後丟掉 testStallMs 毫秒的幀，驗 stale 轉換
        const uint64_t now = nowUs();
        if (m_FirstSubmitUs == 0) {
            m_FirstSubmitUs = now;
        }
        const uint64_t start = m_FirstSubmitUs + 20000000ull;
        if (now >= start && now < start + static_cast<uint64_t>(m_Config.testStallMs) * 1000ull) {
            if (!m_TestStallLogged) {
                m_TestStallLogged = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] dev test stall: dropping frames for %d ms",
                            m_Config.testStallMs);
            }
            m_TestStallDropped++;
            return;
        }
    }
    AVFrame* c = av_frame_clone(frame);
    if (c == nullptr) {
        return;
    }
    bool overwrote = false;
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        if (m_Pending != nullptr) {
            av_frame_free(&m_Pending);
            overwrote = true;
        }
        m_Pending = c;
        m_PendingAtUs = nowUs();  // §VR-XRLOOP：放進 mailbox 的時刻（影像 render thread 取出時算等了多久）
        if (m_WaitSinceUs.load(std::memory_order_relaxed) == 0) {
            m_WaitSinceUs.store(m_PendingAtUs, std::memory_order_relaxed);  // §VR-RATE-CAUSE：已經有在等的就保留較早的
        }
    }
    m_MailboxCv.notify_one();
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    m_Received++;
    if (overwrote) {
        m_Overwritten++;
    }
}

bool XrVideo::mapFrame(const AVFrame* frame, pl_frame* out, pl_tex* texSet, bool useCache)
{
    const AVFrame* src = frame;
    AVFrame* split = nullptr;
    bool ok = false;
#ifdef HAVE_DRM
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        const char* outcome = nullptr;
        split = PlvkCommon::splitDrmPrimeLayers(frame, &outcome);
        if (split != nullptr) {
            src = split;
        }
        if (useCache) {
            ok = m_DrmCache.map(m_Vulkan->gpu, src, out);
            if (!ok && !m_LoggedCacheFallback) {
                m_LoggedCacheFallback = true;
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-XR] dmabuf cache not applicable, using pl_map_avframe_ex");
            }
        }
    }
#else
    (void)useCache;
#endif
    if (!ok) {
        pl_avframe_params mp = {};
        mp.frame = src;
        mp.tex = texSet;
        ok = pl_map_avframe_ex(m_Vulkan->gpu, out, &mp);
    }
    // libplacebo 成功時自己 clone 一份持有到 pl_unmap_avframe；快取路徑不持有幀（材質在快取裡，解碼幀由
    // 呼叫端持有到 GPU 做完）。兩種情況這份改寫描述子都可以放掉
    av_frame_free(&split);
    if (ok) {
        PlvkCommon::fixupMappedFrame(frame, out, false);
        if (!m_LoggedColor.exchange(true)) {
            const char* rn = av_color_range_name(frame->color_range);
            const char* sn = av_color_space_name(frame->colorspace);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-XR] video color: frame range=%s matrix=%s -> levels=%s",
                        rn ? rn : "?", sn ? sn : "?",
                        out->repr.levels == PL_COLOR_LEVELS_LIMITED ? "limited" : "full");
        }
    }
    return ok;
}

bool XrVideo::pyroWaveInterop(PyroWaveVkInterop* out)
{
    if (m_Vulkan == nullptr || !m_Ctx->pyroWaveReady() || m_Ctx->vkEnabledFeatures() == nullptr) {
        return false;
    }
    const std::vector<const char*>& exts = m_Ctx->vkEnabledDeviceExtensions();
    if (m_PwHwDeviceCtx == nullptr) {
        AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (ref == nullptr) {
            return false;
        }
        auto* dev = reinterpret_cast<AVHWDeviceContext*>(ref->data);
        dev->user_opaque = m_Ctx;  // avLockQueueCb
        auto* vk = static_cast<AVVulkanDeviceContext*>(dev->hwctx);
        vk->get_proc_addr = m_Ctx->vkGetInstanceProcAddr();
        vk->inst = m_Ctx->vkInstance();
        vk->phys_dev = m_Ctx->vkPhysicalDevice();
        vk->act_dev = m_Ctx->vkDevice();
        vk->device_features = *m_Ctx->vkEnabledFeatures();
        vk->enabled_inst_extensions = nullptr;  // XrContext 沒有自己開 instance 擴充（runtime 加的我們不知道）
        vk->nb_enabled_inst_extensions = 0;
        vk->enabled_dev_extensions = exts.empty() ? nullptr : exts.data();
        vk->nb_enabled_dev_extensions = static_cast<int>(exts.size());
#if LIBAVUTIL_VERSION_INT > AV_VERSION_INT(58, 9, 100)
        vk->lock_queue = avLockQueueCb;
        vk->unlock_queue = avUnlockQueueCb;
#endif
        // XrContext 只建了一個 graphics queue（同 family 也有 compute／transfer），全部指向它
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
        vk->qf[0].idx = static_cast<int>(m_Ctx->vkQueueFamily());
        vk->qf[0].num = 1;
        vk->qf[0].flags = static_cast<VkQueueFlagBits>(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT);
        vk->qf[0].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>(0);
        vk->nb_qf = 1;
#else
        vk->queue_family_index = static_cast<int>(m_Ctx->vkQueueFamily());
        vk->nb_graphics_queues = 1;
        vk->queue_family_tx_index = static_cast<int>(m_Ctx->vkQueueFamily());
        vk->nb_tx_queues = 1;
        vk->queue_family_comp_index = static_cast<int>(m_Ctx->vkQueueFamily());
        vk->nb_comp_queues = 1;
#endif
        const int err = av_hwdevice_ctx_init(ref);
        if (err < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE-ZC] av_hwdevice_ctx_init on the XR VkDevice failed: %d - copy path", err);
            av_buffer_unref(&ref);
            return false;
        }
        m_PwHwDeviceCtx = ref;
    }

    PyroWaveVkInterop info;
    info.getInstanceProcAddr = m_Ctx->vkGetInstanceProcAddr();
    info.instance = m_Ctx->vkInstance();
    info.physicalDevice = m_Ctx->vkPhysicalDevice();
    info.device = m_Ctx->vkDevice();
    info.apiVersion = m_Ctx->vkApiVersion();
    info.instanceExtensions = nullptr;
    info.numInstanceExtensions = 0;
    info.deviceExtensions = exts.empty() ? nullptr : exts.data();
    info.numDeviceExtensions = static_cast<int>(exts.size());
    info.features = m_Ctx->vkEnabledFeatures();
    // 只有一個 queue：compute／transfer 留空（bridge 依 family 去重、submit 一律用 graphics）
    info.graphics = {m_Ctx->vkQueueFamily(), 1};
    info.lockQueue = lockQueueCb;
    info.unlockQueue = unlockQueueCb;
    info.lockOpaque = m_Ctx;
    info.hwDeviceCtx = m_PwHwDeviceCtx;
    *out = info;
    return true;
}

bool XrVideo::testMap(const AVFrame* frame)
{
    if (m_Vulkan == nullptr || frame == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(m_TestMutex);
    pl_frame mapped;
    std::memset(&mapped, 0, sizeof(mapped));
    if (!mapFrame(frame, &mapped, m_TestTex, false)) {
        const char* fmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video test map failed (format=%s)", fmt ? fmt : "?");
        return false;
    }
    pl_unmap_avframe(m_Vulkan->gpu, &mapped);
    pl_gpu_finish(m_Vulkan->gpu);
    return true;
}

bool XrVideo::ensureSwapchain(XrSession session, int width, int height)
{
    if (m_Sw.swapchain != XR_NULL_HANDLE && m_Sw.width == width && m_Sw.height == height) {
        return true;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (m_Sw.swapchain != XR_NULL_HANDLE) {
        // 解析度改變：舊的一代可能還被 XR thread 的 layer 引用（最後 release 的那張），先退休，
        // 等 XR thread 用新一代送出 xrEndFrame 後由 reapRetired() 銷毀
        std::lock_guard<std::mutex> lk(m_SwMutex);
        m_Retired.push_back(std::move(m_Sw));
        m_Sw = SwapchainSet();
    }

    uint32_t n = 0;
    xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(session, n, &n, formats.data());
    auto offered = [&](VkFormat f) {
        return std::find(formats.begin(), formats.end(), static_cast<int64_t>(f)) != formats.end();
    };

    struct Cand { VkFormat sw; VkFormat view; TargetMode mode; };
    const Cand cands[] = {
        {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, TargetMode::SrgbMutable},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, TargetMode::SrgbMutable},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, TargetMode::Float16Linear},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, TargetMode::Unorm8Linear},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, TargetMode::Unorm8Linear},
    };
    const VkImageUsageFlags vkUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    for (const Cand& c : cands) {
        if (!offered(c.sw)) {
            continue;
        }
        XrSwapchainCreateInfo ci = xrS<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if (c.mode == TargetMode::SrgbMutable) {
            ci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        }
        ci.format = c.sw;
        ci.sampleCount = 1;
        ci.width = static_cast<uint32_t>(width);
        ci.height = static_cast<uint32_t>(height);
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrSwapchain sc = XR_NULL_HANDLE;
        XrResult r = xrCreateSwapchain(session, &ci, &sc);
        if (XR_FAILED(r)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video swapchain fmt=%d %s: xrCreateSwapchain=%d; trying next",
                        static_cast<int>(c.sw), modeName(static_cast<int>(c.mode)), static_cast<int>(r));
            continue;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages(sc, 0, &count, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(count, xrS<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
        r = xrEnumerateSwapchainImages(sc, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        if (XR_FAILED(r) || count == 0) {
            xrDestroySwapchain(sc);
            continue;
        }
        std::vector<pl_tex> wrapped;
        bool wrapOk = true;
        for (uint32_t i = 0; i < count; i++) {
            pl_vulkan_wrap_params wp = {};
            wp.image = imgs[i].image;
            wp.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            wp.width = width;
            wp.height = height;
            wp.depth = 0;
            wp.format = c.view;
            wp.usage = vkUsage;
            wp.debug_tag = PL_DEBUG_TAG;
            pl_tex t = pl_vulkan_wrap(m_Vulkan->gpu, &wp);
            if (t == nullptr || !t->params.renderable) {
                if (t != nullptr) {
                    pl_tex_destroy(m_Vulkan->gpu, &t);
                }
                wrapOk = false;
                break;
            }
            wrapped.push_back(t);
        }
        if (!wrapOk) {
            for (pl_tex& t : wrapped) {
                pl_tex_destroy(m_Vulkan->gpu, &t);
            }
            xrDestroySwapchain(sc);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video swapchain fmt=%d view=%d %s: pl_vulkan_wrap not renderable; trying next",
                        static_cast<int>(c.sw), static_cast<int>(c.view), modeName(static_cast<int>(c.mode)));
            continue;
        }
        m_Sw.swapchain = sc;
        m_Sw.width = width;
        m_Sw.height = height;
        m_Sw.wrapped = std::move(wrapped);
        m_Sw.generation = m_NextGeneration++;
        m_SwFormat = c.sw;
        m_ViewFormat = c.view;
        m_Mode = c.mode;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] video swapchain gen=%llu %dx%d fmt=%d view=%d mode=%s images=%u",
                    static_cast<unsigned long long>(m_Sw.generation), width, height, static_cast<int>(c.sw),
                    static_cast<int>(c.view), modeName(static_cast<int>(c.mode)), count);
        return true;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] no usable video swapchain format for %dx%d", width, height);
    return false;
}

QImage XrVideo::readbackTexture(pl_tex tex)
{
    if (m_Mode == TargetMode::Float16Linear || !tex->params.host_readable) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump skipped (mode=%s hostReadable=%d)",
                    modeName(static_cast<int>(m_Mode)), tex->params.host_readable ? 1 : 0);
        return QImage();
    }
    const int w = tex->params.w;
    const int h = tex->params.h;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    pl_tex_transfer_params tp = {};
    tp.tex = tex;
    tp.ptr = buf.data();
    tp.row_pitch = static_cast<size_t>(w) * 4;
    if (!pl_tex_download(m_Vulkan->gpu, &tp)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump: pl_tex_download failed");
        return QImage();
    }
    const bool bgra = m_ViewFormat == VK_FORMAT_B8G8R8A8_UNORM;
    // Format_ARGB32 在 little-endian 記憶體中就是 B,G,R,A
    QImage img(buf.data(), w, h, w * 4, bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    // copy()：img 借用的是 buf（區域變數），尺寸沒變時 scaled() 可能直接共用同一塊資料
    return img.scaled(QSize(1280, 1280), Qt::KeepAspectRatio, Qt::SmoothTransformation).copy();
}

void XrVideo::saveDump(const QImage& img, const QString& path, int srcW, int srcH)
{
    if (img.isNull()) {
        return;
    }
    if (img.save(path, "PNG")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump: %s (%dx%d from %dx%d, mode=%s)",
                    qUtf8Printable(path), img.width(), img.height(), srcW, srcH, modeName(static_cast<int>(m_Mode)));
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump: saving %s failed", qUtf8Printable(path));
    }
}

void XrVideo::dumpTexture(pl_tex tex, const QString& path)
{
    saveDump(readbackTexture(tex), path, tex->params.w, tex->params.h);
}

void XrVideo::dumpSynthInputs(const QString& prevPath, const QString& curPath, const QString& flowPath)
{
    pl_gpu g = m_Vulkan->gpu;
    // renderSynth 結束時 m_SmallCur 已經換到下一張要寫的那一格：它現在指的是上一張，另一格才是這一張
    const pl_tex smalls[2] = {m_Small[m_SmallCur], m_Small[m_SmallCur ^ 1]};
    const QString* paths[2] = {&prevPath, &curPath};
    pl_tex flow = m_Flow;
    for (pl_tex t : {smalls[0], smalls[1], flow}) {
        if (t == nullptr || !t->params.host_readable) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] synth input dump skipped (texture not readable)");
            return;
        }
    }
    auto download = [g](pl_tex t, std::vector<uint8_t>* buf) {
        const size_t texel = t->params.format->texel_size;
        buf->resize(static_cast<size_t>(t->params.w) * static_cast<size_t>(t->params.h) * texel);
        pl_tex_transfer_params tp = {};
        tp.tex = t;
        tp.ptr = buf->data();
        tp.row_pitch = static_cast<size_t>(t->params.w) * texel;
        return pl_tex_download(g, &tp);
    };
    for (int i = 0; i < 2; i++) {
        pl_tex t = smalls[i];
        std::vector<uint8_t> buf;
        if (t->params.format->texel_size != 4 || !download(t, &buf)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] synth input dump: reading the 1/8 image failed");
            return;
        }
        // 色版順序照材質的格式（多半是 RGBA）；離線只拿它算亮度，順序不影響比對
        const QImage img(buf.data(), t->params.w, t->params.h, t->params.w * 4, QImage::Format_RGBA8888);
        if (!img.save(*paths[i], "PNG")) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] synth input dump: saving %s failed", qUtf8Printable(*paths[i]));
            return;
        }
    }
    std::vector<uint8_t> fbuf;
    QFile out(flowPath);
    if (!download(flow, &fbuf) || !out.open(QIODevice::WriteOnly) ||
        out.write(reinterpret_cast<const char*>(fbuf.data()), static_cast<qint64>(fbuf.size())) != static_cast<qint64>(fbuf.size())) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] synth input dump: saving %s failed", qUtf8Printable(flowPath));
        return;
    }
    float fv[4];
    float fvCyR;
    float fvU0;
    loadFoveation(fv, &fvCyR, &fvU0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] synth input dump: %s small=%dx%d(%s) flow=%dx%d(%s, %d bytes/texel) fv=(%.4f,%.4f,%.4f,%.4f,%.4f) u0=%.4f",
                qUtf8Printable(flowPath), smalls[1]->params.w, smalls[1]->params.h, smalls[1]->params.format->name,
                flow->params.w, flow->params.h, flow->params.format->name, static_cast<int>(flow->params.format->texel_size),
                fv[0], fv[1], fv[2], fv[3], fvCyR, fvU0);
}

bool XrVideo::renderNewFrame(AVFrame* frame, uint32_t mboxUs)
{
    const uint64_t t0 = nowUs();
    // §VR-LDIAG：發布紀錄要帶 pts（host 的 Present 時刻，90 kHz）；發布時 frame 可能已經放掉，先抄下來
    const int64_t pts90 = frame->pts;
    // M4a R1/R4：PCVR 的 render pose（decoder 以 pts 查到後放進 VrRenderMetaRing）。只在這裡查一次：
    // render 要 10～20 ms，期間 ring 會被後面的幀寫入，R1 在 render 之後才查，碰撞時查不到 → 發布成
    // 無 meta → XR thread 當成 no-video 閃一幀 loading（R4 實測每 10 s 約 2 次）。
    bool hasMeta = false;
    VIPLE_VR_FRAME_META meta = {};
    if (frame->pts != AV_NOPTS_VALUE && VrRenderMetaRing::lookup(frame->pts, &meta)) {
        hasMeta = meta.present != 0;
    }
    if (m_Config.requireMeta && !hasMeta) {
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_NoMetaDropped++;
        return false;
    }
    if (m_Immersive.load(std::memory_order_acquire)) {
        return renderEquiFrame(frame, t0);  // §VR-PLAYER（V2）
    }

    // §REVIEW-1104：平面路徑錄製與送出 libplacebo 時拿送出鎖，和全景投影互斥，否則驅動會在 tu_autotune::on_submit 崩潰
    std::unique_lock<std::mutex> submitLock(m_PlSubmitMutex);

    pl_gpu g = m_Vulkan->gpu;
    // §VR-XRLOOP（量測）：這一張影像的 swapchain 呼叫花多久。先記在這裡，等下面本來就要拿 m_StatsMutex 的地方
    // 一起寫回（flushSwLocked）——不為了量測多拿一次鎖
    struct {
        uint32_t waitUs[2] = {};  // xrWaitSwapchainImage；補幀開著時一張影像 acquire 兩次
        int waits = 0;
        uint32_t notOk = 0;       // 回的不是 XR_SUCCESS 的次數
        uint32_t relMaxUs = 0;    // xrReleaseSwapchainImage 的最大值
    } swm;
    auto sat32 = [](uint64_t v) { return static_cast<uint32_t>(std::min<uint64_t>(v, 0xffffffffu)); };
    auto flushSwLocked = [&]() {  // 呼叫端持 m_StatsMutex
        for (int i = 0; i < swm.waits; i++) {
            m_Diag.swWaitUs.push_back(swm.waitUs[i]);
        }
        m_Diag.swNotOk += swm.notOk;
        m_Diag.relMaxUs = std::max(m_Diag.relMaxUs, swm.relMaxUs);
        swm.waits = 0;
        swm.notOk = 0;
        swm.relMaxUs = 0;
    };
    // 取一張 swapchain 影像並交給 libplacebo。OpenXR（Vulkan）：acquire 後彩色影像在 COLOR_ATTACHMENT_OPTIMAL，
    // release 前也要回到這個 layout
    auto acquire = [&](pl_tex* texOut) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = xrS<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
        XrResult r;
        {
            std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
            r = xrAcquireSwapchainImage(m_Sw.swapchain, &ai, &idx);
        }
        if (XR_FAILED(r) || idx >= m_Sw.wrapped.size()) {
            return false;
        }
        XrSwapchainImageWaitInfo wi = xrS<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wi.timeout = 100000000;  // 100 ms
        // §VR-XRLOOP：只量這個呼叫花多久、回的是不是 XR_SUCCESS（等到逾時回 XR_TIMEOUT_EXPIRED，也是成功碼）。
        // 流程不看結果，和原本一樣
        const uint64_t tw0 = nowUs();
        const XrResult wr = xrWaitSwapchainImage(m_Sw.swapchain, &wi);
        if (swm.waits < 2) {
            swm.waitUs[swm.waits++] = sat32(nowUs() - tw0);
        }
        swm.notOk += wr != XR_SUCCESS ? 1u : 0u;
        pl_vulkan_release_params rp = {};
        rp.tex = m_Sw.wrapped[idx];
        rp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rp.qf = VK_QUEUE_FAMILY_IGNORED;
        pl_vulkan_release_ex(g, &rp);
        *texOut = m_Sw.wrapped[idx];
        return true;
    };
    // GPU 做完才 release 給 runtime、才放掉解碼幀。X3 起這裡是影像 render thread，不再阻塞 XR frame thread。
    // M4a 收尾：先以有上限的 timeline 等待確認 hold 的 semaphore 到了（pl_gpu_finish 沒有逾時；runtime 串流
    // 中死掉時 GPU 工作可能永遠等不到結果，R4 在 S1 卡了 7 分鐘），到了再 pl_gpu_finish（此時立即返回）。
    // 回傳 1＝做完、0＝hold 失敗（照舊 pl_gpu_finish）、-1＝GPU 卡住
    // §VR-XRLOOP：submitUs 帶回 hold 返回的時刻——這張影像的每一筆 GPU 工作到這裡都送出去了（hold 失敗時不動它）
    // §VR-LATCH-FLIGHT：flight＝這一批做完就會發布成新的一張（第一批、畫成功了、而且這一場開了 flightNotify）。
    // 送出之後、等 GPU 之前就標成「在途」——等 GPU 做完才標就沒有意義了。先存時刻、再存序號（兩個是一對，見 xrvideo.h）；
    // m_Published.seq 只有這條執行緒會寫，不必拿 m_SwMutex 讀。標了之後只有兩條路：發布（下面清掉），或 GPU 卡住
    // （wedged 清掉）。這兩個 atomic 一律用預設的順序（seq_cst），flightInfo() 的讀法靠它
    auto finishGpu = [&](pl_tex tex, uint64_t& submitUs, bool flight) {
        pl_vulkan_hold_params hp = {};
        hp.tex = tex;
        hp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hp.qf = VK_QUEUE_FAMILY_IGNORED;
        hp.semaphore.sem = m_Sem;
        hp.semaphore.value = ++m_SemValue;
        if (!pl_vulkan_hold_ex(g, &hp)) {
            // §REVIEW-1104：hold 失敗時 pl_gpu_finish 會等 GPU，這段不持送出鎖
            submitLock.unlock();
            pl_gpu_finish(g);
            submitLock.lock();
            return 0;
        }
        submitUs = nowUs();
        if (flight) {
            m_FlightSubmitUs.store(submitUs);
            m_FlightSeq.store(m_Published.seq + 1);
        }
        // §REVIEW-1104：等 GPU 最長 1 秒，這段不持送出鎖，返回後的 unmap 與補幀才再互斥
        submitLock.unlock();
        const bool gpuDone = m_Ctx->waitSemaphoreBounded(m_Sem, hp.semaphore.value, 1000000000ull);
        submitLock.lock();
        if (!gpuDone) {
            return -1;
        }
        pl_gpu_finish(g);
        return 1;
    };
    auto releaseImage = [&]() {
        XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        const uint64_t tr0 = nowUs();  // §VR-XRLOOP：這個呼叫本身花多久（前後各取一次時鐘，都在 queue 鎖內）
        xrReleaseSwapchainImage(m_Sw.swapchain, &ri);
        swm.relMaxUs = std::max(swm.relMaxUs, sat32(nowUs() - tr0));
        const uint64_t no = m_ReleaseNo.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m_CurSlot >= 0) {
            m_RingRel[m_CurSlot].store(no, std::memory_order_relaxed);  // §VR-BUFFER
        }
        return no;
    };
    auto wedged = [&](const char* why) {
        if (m_Config.flightNotify) {
            // §VR-LATCH-FLIGHT：這一張不會發布了。先清在途的標記，frame thread 才不會繼續為它等（下面印 log 要時間）
            m_FlightSeq.store(0);
        }
        m_WedgedFrames.push_back(frame);  // 仍被 GPU 參照：不 unmap、不 free
        m_Ctx->markGpuWedged(why);
        markUnusable("GPU wedged");
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        flushSwLocked();
    };

    pl_tex tex = nullptr;
    if (!acquire(&tex)) {
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        return false;
    }

    pl_frame mapped;
    std::memset(&mapped, 0, sizeof(mapped));
    const bool mappedOk = mapFrame(frame, &mapped, m_Tex, true);
    bool rendered = false;
    int insetW = 0, insetH = 0;
    // §VR-FOVEA-LAYERS：swapchain 的高度＝影像＋中央區那一條（outputSize 用同一個條件算的）。高度對不上（條件剛變、
    // swapchain 還是舊的）就這張不畫中央區
    const bool wantInset = insetSize(frame->width, frame->height, &insetW, &insetH) && m_Sw.height == frame->height + insetH &&
                           m_Sw.width >= 2 * insetW;
    // §VR-FOVEA-TILES：解碼出來的是分塊的畫面（上排全視野、下排注視點）。swapchain 用同樣的排法、同樣的尺寸
    // （outputSize 這時不放大）；尺寸對不上（swapchain 還是舊的）就這張當一般影像畫，下一張就對了
    int tileW = 0, tileH = 0, tileRW = 0, tileRH = 0;
    const bool tiles = !m_Config.synth && tilesFor(frame->width, frame->height, &tileW, &tileH, &tileRW, &tileRH) &&
                       m_Sw.width == frame->width && m_Sw.height == frame->height;
    // swapchain 裡一張影像的高度（分塊時＝上排全視野那一列）
    const int frameH = tiles ? tileH : (m_Config.synth ? m_Sw.height / 2 : (wantInset ? m_Sw.height - insetH : m_Sw.height));
    m_PendInset = false;
    const bool fovea = m_FvA.load(std::memory_order_relaxed) < 0.999f;
    pl_frame target;
    std::memset(&target, 0, sizeof(target));
    if (mappedOk) {
        target.num_planes = 1;
        target.planes[0].texture = tex;
        target.planes[0].components = 4;
        target.planes[0].component_mapping[0] = PL_CHANNEL_R;
        target.planes[0].component_mapping[1] = PL_CHANNEL_G;
        target.planes[0].component_mapping[2] = PL_CHANNEL_B;
        target.planes[0].component_mapping[3] = PL_CHANNEL_A;
        target.repr = pl_color_repr_rgb;
        target.color = pl_color_space_srgb;
        if (m_Mode != TargetMode::SrgbMutable) {
            target.color.transfer = PL_COLOR_TRC_LINEAR;
        }
        // 第一張：只畫真的那一張（開著合成時 swapchain 是兩倍高，畫在上半）。和沒開合成時一樣的工作量，
        // 所以真的那一張不會被合成的運算拖慢。
        pl_frame t1 = target;
        if (m_Config.synth || wantInset) {
            t1.crop.x0 = 0.0f;
            t1.crop.y0 = 0.0f;
            t1.crop.x1 = static_cast<float>(m_Sw.width);
            t1.crop.y1 = static_cast<float>(frameH);
        }
        if (tiles) {
            const char* why = nullptr;
            rendered = !m_TilesFailed && renderTiles(&mapped, &target, tex, tileW, tileH, &why);
            if (!rendered) {
                if (!m_TilesFailed) {
                    m_TilesFailed = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-FOVEA] tiles: direct pass not used (%s) - plain conversion for this session, the gaze tile keeps a hard edge",
                                why ? why : "?");
                }
                rendered = pl_render_image(m_Renderer, &mapped, &target, &m_RenderParams);  // 整張 1:1，排法不變
            }
            if (rendered && hasMeta && meta.foveated) {
                // 0x82 的四欄在分塊時是注視點那塊左上角的算圖像素座標（common-c 轉成 ／65535 的 float，乘回去就是原值）
                m_PendInset = true;
                for (int e = 0; e < 2; e++) {
                    const long px = std::clamp(std::lround(meta.foveaCentre[e][0] * 65535.0f), 0L, static_cast<long>(tileRW - tileW));
                    const long py = std::clamp(std::lround(meta.foveaCentre[e][1] * 65535.0f), 0L, static_cast<long>(tileRH - tileH));
                    m_PendInsetRect[e].offset = {e * tileW, tileH};
                    m_PendInsetRect[e].extent = {tileW, tileH};
                    m_PendInsetOut[e][0] = static_cast<float>(px) / static_cast<float>(tileRW);
                    m_PendInsetOut[e][1] = static_cast<float>(py) / static_cast<float>(tileRH);
                    m_PendInsetOut[e][2] = static_cast<float>(px + tileW) / static_cast<float>(tileRW);
                    m_PendInsetOut[e][3] = static_cast<float>(py + tileH) / static_cast<float>(tileRH);
                }
            }
        }
        else if (fovea && !m_UnwarpFailed) {
            rendered = renderUnwarp(&mapped, &t1, tex, frame->width, frame->height, hasMeta, meta);
            if (rendered && wantInset && !m_UnwarpYuvFailed) {
                float fv[4];
                float cyR;
                float u0;
                loadFoveation(fv, &cyR, &u0);
                if (hasMeta && meta.foveated) {
                    fv[1] = meta.foveaCentre[0][0];
                    fv[2] = meta.foveaCentre[1][0];
                    fv[3] = meta.foveaCentre[0][1];
                    cyR = meta.foveaCentre[1][1];
                }
                if (renderInsets(&mapped, tex, frameH, insetW, insetH, fv, cyR, u0)) {
                    m_PendInset = true;
                    // 中央區在每眼影像裡的範圍：[c−a·u0·c, c+a·u0·(1−c)]（x、y 各自；c＝這張的注視中心）
                    const float au = fv[0] * u0;
                    for (int e = 0; e < 2; e++) {
                        m_PendInsetRect[e].offset = {e * insetW, frameH};
                        m_PendInsetRect[e].extent = {insetW, insetH};
                        const float cx = std::clamp(e == 0 ? fv[1] : fv[2], 0.05f, 0.95f);
                        const float cy = std::clamp(e == 0 ? fv[3] : cyR, 0.05f, 0.95f);
                        m_PendInsetOut[e][0] = cx * (1.0f - au);
                        m_PendInsetOut[e][1] = cy * (1.0f - au);
                        m_PendInsetOut[e][2] = cx + au * (1.0f - cx);
                        m_PendInsetOut[e][3] = cy + au * (1.0f - cy);
                    }
                }
                else {
                    // 這一張的上方已經是解碼原尺寸的周邊（不會自己變清楚）：照樣發布，下一張起 outputSize 改回單一大圖
                    m_InsetFailed = true;
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-VR-FOVEA] centre layer pass failed - back to one full-size image for this session");
                }
            }
        }
        else {
            rendered = pl_render_image(m_Renderer, &mapped, &t1, &m_RenderParams);
        }
        if (!rendered) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] pl_render_image failed");
        }
    }
    else if (!m_LoggedFormat) {
        const char* fmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] video frame map failed (format=%s)", fmt ? fmt : "?");
        m_LoggedFormat = true;
    }

    m_TotalDrawn += rendered ? 1 : 0;
    // §VR-SYNTH：開著合成時等到第 2400 張之後、而且那一張真的有合成影像才存（存第二張：上半真的、下半合成）。
    // 2026-10-10：開著合成時一共存 kSynthDumpPairs 組、每 480 張一組（半速 60 Hz 是每 8 秒）——只看一組抓不到偶發的錯；
    // 第 2 組起檔名在副檔名之前加 -2、-3…。沒開合成時照舊只存一張。
    const int dumpGoal = m_Config.synth ? kSynthDumpPairs : 1;
    const uint64_t dumpAt = m_Config.synth ? 2400u + 480u * static_cast<uint64_t>(m_DumpsDone)
                                           : static_cast<uint64_t>(std::max(1, m_Config.dumpAfterFrames));
    const bool wantDump = rendered && m_DumpsDone < dumpGoal && !m_Config.dumpPath.isEmpty() && m_TotalDrawn >= dumpAt;
    // 這一組的檔名：第 1 組用原本的路徑，之後在副檔名之前插 -<第幾組>；suffix 再接在後面（第一次發布的那一張是 -first）
    auto dumpPathFor = [&](const char* suffix) {
        QString p = m_Config.dumpPath;
        QString ins = m_DumpsDone > 0 ? QStringLiteral("-%1").arg(m_DumpsDone + 1) : QString();
        ins += QLatin1String(suffix);
        const int dot = p.lastIndexOf(QLatin1Char('.'));
        if (dot > p.lastIndexOf(QLatin1Char('/'))) {
            p.insert(dot, ins);
        }
        else {
            p += ins;
        }
        return p;
    };
    if (wantDump && !m_Config.synth) {
        dumpTexture(tex, dumpPathFor(""));  // 必須在 hold 之前（download 需要 libplacebo 仍持有影像）
        m_DumpsDone++;
    }
    // §VR-SYNTH 的檢查（dev）：開著合成時，第一次發布的這一張（只有真的）也讀回來，等下面確定這一幀有合成影像
    // 再和第二張一起存——兩張的上半應該一樣。2026-10-10：注視點還原＋合成一起開時這一張畫錯過（renderUnwarp），
    // 只存第二張看不出來。讀回會卡住這一幀幾十毫秒，只在帶了 --xr-dump-frame 的那一張做。
    QImage firstDump;
    const int firstDumpW = tex->params.w, firstDumpH = tex->params.h;
    if (wantDump && m_Config.synth) {
        firstDump = readbackTexture(tex);
    }

    const uint64_t tCpuEnd = nowUs();
    uint64_t tSubmit = 0;  // §VR-XRLOOP：送出的時刻（hold 返回）
    // §VR-LATCH-FLIGHT：在途標記的保險。該清的地方（發布之後、wedged）都明確清了；這個物件保證不管之後從哪一條路
    // 離開這個函式，標記都不會留著——留著的話 frame thread 會為一張不會來的影像每一格等到上限。
    // 要宣告在函式的作用域（不可以放進 finishGpu：lambda 一返回就清掉了）。這一場沒開 flightNotify 時什麼都不做
    struct FlightClear {
        explicit FlightClear(std::atomic<uint64_t>* s) : seq(s) {}
        ~FlightClear()
        {
            if (seq != nullptr) {
                seq->store(0);
            }
        }
        FlightClear(const FlightClear&) = delete;
        FlightClear& operator=(const FlightClear&) = delete;
        std::atomic<uint64_t>* seq;
    } flightClear(m_Config.flightNotify ? &m_FlightSeq : nullptr);
    const int fin = finishGpu(tex, tSubmit, m_Config.flightNotify && rendered);
    if (fin < 0) {
        wedged("video render did not finish within 1 s");
        return false;
    }
    const bool held = fin > 0;
    const uint64_t tGpuEnd = nowUs();
    // 第二張要再用一次解碼幀（mapped）：做合成時先不放掉
    const bool doSynth = m_Config.synth && !m_SynthFailed && mappedOk && rendered && held;
    if (!doSynth) {
        if (mappedOk) {
            pl_unmap_avframe(g, &mapped);
        }
        // hasMeta／meta 已在函式開頭查好，跟著這張影像一起發布
        av_frame_free(&frame);
    }

    // 發布（呼叫端持 m_SwMutex）：frame thread 的 current() 從這裡開始看得到這張影像
    auto publishLocked = [&](uint64_t releaseNo, uint64_t tPub) {
        m_Published.releaseNo = releaseNo;
        m_Published.subImage.swapchain = m_Sw.swapchain;
        m_Published.subImage.imageRect.offset = {0, 0};
        m_Published.subImage.imageRect.extent = {m_Sw.width, frameH};
        m_Published.subImage.imageArrayIndex = 0;
        m_Published.aspect = static_cast<float>(m_Sw.width) / static_cast<float>(frameH);
        m_Published.hasSynth = false;
        m_Published.synthRect.offset = {0, frameH};
        m_Published.synthRect.extent = {m_Sw.width, frameH};
        m_Published.synthMeta = {};
        m_Published.hasInset = m_PendInset;  // §VR-FOVEA-LAYERS
        for (int e = 0; e < 2; e++) {
            m_Published.insetRect[e] = m_PendInsetRect[e];
            for (int k = 0; k < 4; k++) {
                m_Published.insetOut[e][k] = m_PendInsetOut[e][k];
            }
        }
        m_Published.generation = m_Sw.generation;
        m_Published.lastDrawnUs = tGpuEnd;
        m_Published.seq++;
        m_Published.renderUs = static_cast<uint32_t>(std::min<uint64_t>(tGpuEnd - t0, 0xffffffffu));
        m_Published.hasMeta = hasMeta;
        m_Published.meta = meta;
        m_HavePublished = true;
        m_Published.publishedUs = tPub;
        m_Published.slot = m_CurSlot;  // §VR-BUFFER
        // §VR-LDIAG：每一張發布的影像記一筆（被跳過、沒顯示的也有），frame thread 用 drainPublished() 抄走
        m_PubRing[m_PubHead++ % kPubRing] = {m_Published.seq, pts90, t0, tGpuEnd, tPub, tSubmit};
        if (m_CurSlot >= 0) {
            m_RingPub[m_CurSlot] = m_Published;  // §VR-BUFFER
        }
    };
    if (m_Config.atomicPublish) {
        // §VR-LATCH-RELATCH：release 與發布放進同一段 queue 鎖。原本 release 之後、發布之前有一小段空窗——frame thread
        // 在 xrEndFrame 的 queue 鎖內看到 releaseNo 變了，快照卻可能還是舊的；放在一起就保證 peek() 拿到的是新的那一張。
        // 唯一的巢狀鎖：queueMutex → m_SwMutex（見 xrvideo.h 檔頭的執行緒模型）。
        XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        std::lock_guard<std::mutex> q(m_Ctx->queueMutex());
        const uint64_t tr0 = nowUs();  // §VR-XRLOOP：release 這個呼叫本身花多久（「之後」的時刻就用下面的 tPub）
        xrReleaseSwapchainImage(m_Sw.swapchain, &ri);
        const uint64_t releaseNo = m_ReleaseNo.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m_CurSlot >= 0) {
            m_RingRel[m_CurSlot].store(releaseNo, std::memory_order_relaxed);  // §VR-BUFFER
        }
        const uint64_t tPub = nowUs();
        if (rendered && held) {
            std::lock_guard<std::mutex> lk(m_SwMutex);
            publishLocked(releaseNo, tPub);
        }
        swm.relMaxUs = std::max(swm.relMaxUs, sat32(tPub - tr0));
    }
    else {
        const uint64_t releaseNo = releaseImage();
        const uint64_t tPub = nowUs();
        if (rendered && held) {
            std::lock_guard<std::mutex> lk(m_SwMutex);
            publishLocked(releaseNo, tPub);
        }
    }
    if (m_Config.flightNotify) {
        // §VR-LATCH-FLIGHT：發布了（seq 已經加一），或這一張沒有發布：不再是「在途」。排在發布之後才清——frame thread
        // 若剛好在這兩步之間讀，看到的是 flightSeq == latestSeq，判成沒有在途、不等，接著本來就挑得到新的那一張
        m_FlightSeq.store(0);
    }
    if ((m_Config.notifyPublish || m_Config.flightNotify) && rendered && held) {
        // §VR-LATCH-WAIT／§VR-LATCH-FLIGHT：frame thread 可能正在等這一格的影像（waitNewerThan）。鎖都放掉了才叫醒。
        // 以「有發布」為準（上面的 release 是無條件的，沒畫成功的那一張 seq 不會變）。
        // §VR-SYNTH 的第二次發布（下面）沿用同一個 seq，不叫
        m_PubCv.notify_all();
    }
    if (m_Config.flightNotify && rendered && held) {
        // §VR-LATCH-FLIGHT：這一張的 GPU 時間（送出 → 做完）餵給典型值的估計。排在叫醒之後，不拖慢等著的 frame thread；
        // hold 失敗的那一張沒有送出的時刻，不餵（held 為假）。補幀那一批不算
        if (m_GpuTyp.push(sat32(tGpuEnd > tSubmit ? tGpuEnd - tSubmit : 0))) {
            m_GpuTypUs.store(m_GpuTyp.typUs(), std::memory_order_relaxed);
        }
    }
    // §VR-XRLOOP（量測，這一張已經發布了才算）：送出與 GPU 做完的時刻換成相位。顯示格點由 frame thread 每格更新；
    // 沒有格點（非 PCVR、frame thread 還沒跑到，或這一格的預測顯示時刻換算不出來）就不記。兩批 GPU 工作落在哪個
    // 相位、有沒有壓到合成器，以前只有頭盔的 gpu-trace 看得到
    const uint64_t grid = m_Grid.load(std::memory_order_relaxed);  // （週期 << 32）｜（格點對週期的餘數），見 setDisplayGrid
    const uint32_t gridPeriodUs = static_cast<uint32_t>(grid >> 32);
    const uint64_t gridUs = grid & 0xffffffffull;
    const bool phaseOk = gridPeriodUs != 0 && rendered && held;
    const uint32_t subPhase = phaseOk ? xrphase::phaseUs(tSubmit, gridUs, gridPeriodUs) : 0u;
    const uint32_t donePhase = phaseOk ? xrphase::phaseUs(tGpuEnd, gridUs, gridPeriodUs) : 0u;
    // 送出的相位會不會撞上合成器自己那一筆：相位窗只量過 120 Hz，別的更新率不分類
    const bool classify = phaseOk && xrphase::is120Hz(gridPeriodUs);
    const xrphase::Handoff handoff = classify ? xrphase::handoffClass(xrphase::ldiagToVblankUs(subPhase, gridPeriodUs))
                                              : xrphase::Handoff::Clear;
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        if (rendered && held) {
            m_Drawn++;
            m_CpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tCpuEnd - t0, 0xffffffffu)));
            m_GpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tGpuEnd - tCpuEnd, 0xffffffffu)));
            m_Diag.mboxUs.push_back(mboxUs);
            if (phaseOk) {
                m_Diag.subPhaseUs.push_back(subPhase);
                m_Diag.donePhaseUs.push_back(donePhase);
                m_Diag.phasePeriodUs = gridPeriodUs;  // 印的時候要知道這些相位是對哪個週期取的（環狀的中位數）
            }
            if (classify) {
                m_Diag.handoffN++;
                m_Diag.handoffPreempt += handoff == xrphase::Handoff::Preempt ? 1u : 0u;
                m_Diag.handoffQueued += handoff == xrphase::Handoff::Queued ? 1u : 0u;
            }
        }
        else {
            m_RenderErrors++;
        }
        flushSwLocked();
    }
    if (!doSynth) {
        return rendered && held;
    }

    // 第二張（§VR-SYNTH）：上半再畫一次真的、下半是合成的。做完後以同一個 seq 再發布一次——frame thread 在這張
    // 影像第二次被顯示時（半速串流的另一格）改用下半；來不及的話那一格照舊重送真的。
    pl_tex tex2 = nullptr;
    bool synthOk = false;
    VIPLE_VR_FRAME_META synthMeta = {};
    if (!acquire(&tex2)) {
        pl_unmap_avframe(g, &mapped);
        av_frame_free(&frame);
        return true;
    }
    pl_frame t2 = target;
    t2.planes[0].texture = tex2;
    const bool r2 = renderSynth(&mapped, &t2, tex2, frame->width, frame->height, hasMeta, meta, frame->pts, &synthOk, &synthMeta);
    if (r2 && wantDump && (synthOk || m_SynthFailed)) {
        dumpTexture(tex2, dumpPathFor(""));
        // 第一次發布的那一張存成 <路徑>-first.png（副檔名之前插 -first）
        saveDump(firstDump, dumpPathFor("-first"), firstDumpW, firstDumpH);
        if (synthOk) {
            // 估位移用到的兩張縮圖與位移場：<路徑>-prev8.png、-cur8.png、-flow.bin
            QString flowPath = dumpPathFor("-flow");
            if (flowPath.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) {
                flowPath.chop(4);
            }
            flowPath += QLatin1String(".bin");
            dumpSynthInputs(dumpPathFor("-prev8"), dumpPathFor("-cur8"), flowPath);
        }
        m_DumpsDone++;
    }
    uint64_t tSynthSubmit = 0;  // §VR-XRLOOP：補幀那一批送出的時刻
    // §VR-LATCH-FLIGHT：這一批不標在途——它做完之後的第二次發布不改 seq，frame thread 等不到它
    const int fin2 = finishGpu(tex2, tSynthSubmit, false);
    if (fin2 < 0) {
        wedged("synthesized frame did not finish within 1 s");
        return false;
    }
    const uint64_t tSynthEnd = nowUs();
    pl_unmap_avframe(g, &mapped);
    av_frame_free(&frame);
    const uint64_t releaseNo2 = releaseImage();
    if (r2 && fin2 > 0) {
        std::lock_guard<std::mutex> lk(m_SwMutex);
        m_Published.releaseNo = releaseNo2;
        m_Published.hasSynth = synthOk;
        m_Published.synthMeta = synthMeta;
    }
    // §VR-XRLOOP：補幀那一批的兩個相位（格點沿用這一張第一批讀到的那一組；hold 失敗就沒有送出的時刻，不記）
    const bool synthPhaseOk = gridPeriodUs != 0 && r2 && fin2 > 0;
    const uint32_t synthSubPhase = synthPhaseOk ? xrphase::phaseUs(tSynthSubmit, gridUs, gridPeriodUs) : 0u;
    const uint32_t synthDonePhase = synthPhaseOk ? xrphase::phaseUs(tSynthEnd, gridUs, gridPeriodUs) : 0u;
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_SynthDrawn += synthOk ? 1 : 0;
        m_SynthGpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tSynthEnd - tGpuEnd, 0xffffffffu)));
        if (synthPhaseOk) {
            m_Diag.synthSubPhaseUs.push_back(synthSubPhase);
            m_Diag.synthDonePhaseUs.push_back(synthDonePhase);
            m_Diag.phasePeriodUs = gridPeriodUs;
        }
        flushSwLocked();
    }
    return true;
}

bool XrVideo::bufferTotals(uint32_t* lag1, uint32_t* lag0) const
{
    if (m_Config.buffer <= 0) {
        return false;
    }
    *lag1 = m_BufLag1Total.load(std::memory_order_relaxed);
    *lag0 = m_BufLag0Total.load(std::memory_order_relaxed);
    return true;
}

// §VR-BUFFER：每 10 s 印一行 frame thread 的挑選結果（影像 render thread 呼叫，不佔 frame thread 的時間）
void XrVideo::bufferMaybeLog()
{
    const uint64_t now = nowUs();
    if (m_BufLogUs == 0) {
        m_BufLogUs = now;
        return;
    }
    if (now - m_BufLogUs < 10000000ull) {
        return;
    }
    m_BufLogUs = now;
    const uint32_t lag1 = m_BufLag1.exchange(0), lag0 = m_BufLag0.exchange(0);
    const uint32_t rep = m_BufRepeat.exchange(0), skip = m_BufSkipped.exchange(0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-BUFFER] 10s buffer=%d%s depth=%d up=%u down=%u shown lag1=%u lag0=%u repeat=%u skipped=%u shed=%u",
                m_Config.buffer, m_Config.bufferAuto ? " auto" : "", m_BufDepthLog.load(std::memory_order_relaxed),
                m_BufUp.exchange(0), m_BufDown.exchange(0), lag1, lag0, rep, skip, m_BufShed.exchange(0));
}

void XrVideo::renderThreadMain()
{
    while (true) {
        AVFrame* f = nullptr;
        XrSession session = XR_NULL_HANDLE;
        uint64_t pendingAtUs = 0;
        {
            std::unique_lock<std::mutex> lk(m_MailboxMutex);
            m_MailboxCv.wait_for(lk, std::chrono::milliseconds(100),
                                 [this] { return m_StopThread || m_Pending != nullptr; });
            if (m_StopThread) {
                break;
            }
            f = m_Pending;
            m_Pending = nullptr;
            pendingAtUs = m_PendingAtUs;
            session = m_Session;
        }
        // §VR-XRLOOP：這一幀在 mailbox 裡等了多久（放進去 → 取出）；只進統計。dev 的抖動注入睡在這之後，不算在內
        uint32_t mboxUs = 0;
        if (f != nullptr) {
            const uint64_t takenUs = nowUs();
            mboxUs = static_cast<uint32_t>(std::min<uint64_t>(takenUs > pendingAtUs ? takenUs - pendingAtUs : 0, 0xffffffffu));
        }
        reapRetired();
        if (f == nullptr) {
            continue;
        }
        // §VR-RATE-CAUSE：這一張處理完（發布、丟掉或失敗都一樣）之後，「在等的」換成 mailbox 裡那一張（沒有＝0）
        struct WaitReset {
            XrVideo* v;
            ~WaitReset()
            {
                std::lock_guard<std::mutex> lk(v->m_MailboxMutex);
                v->m_WaitSinceUs.store(v->m_Pending != nullptr ? v->m_PendingAtUs : 0, std::memory_order_relaxed);
            }
        } waitReset{this};
        if (m_Ctx->gpuWedged()) {
            // M4a 收尾：已判定 GPU 卡住——不再送新工作（新工作只會跟著卡住）
            av_frame_free(&f);
            continue;
        }
        // dev（--vr-test-jitter）：先睡一下再畫，等同這一張晚到（到達時刻在 renderNewFrame 才取）。不持任何鎖；
        // 睡的時候進來的幀照舊放進 mailbox（latest wins）
        if (const uint32_t jitUs = jitterNextUs()) {
            std::this_thread::sleep_for(std::chrono::microseconds(jitUs));
        }
        int outW = f->width, outH = f->height;
        outputSize(f->width, f->height, &outW, &outH);
        // §VR-BUFFER：這一張寫進下一條 swapchain（換進 m_Sw，畫完換回去；見 xrvideo.h 的 m_RingSets）
        const int slot = m_Config.buffer > 0 ? m_RingNext : -1;
        if (slot >= 0) {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            std::swap(m_Sw, m_RingSets[slot]);
            m_CurSlot = slot;
        }
        // 只有真的發布了才換下一條：沒畫成功時下一張還是寫這一條（它仍是第三新的），不會輪到正要顯示的那一條。
        // m_Published.seq 只有這條執行緒會寫，不必拿鎖讀
        struct RingRestore {
            XrVideo* v;
            int slot;
            uint64_t seqBefore;
            ~RingRestore()
            {
                if (slot >= 0) {
                    std::lock_guard<std::mutex> lk(v->m_StatsMutex);
                    std::swap(v->m_Sw, v->m_RingSets[slot]);
                    v->m_CurSlot = -1;
                    if (v->m_Published.seq != seqBefore) {
                        v->m_RingNext = (slot + 1) % v->ringSize();
                    }
                }
            }
        } ringRestore{this, slot, m_Published.seq};
        if (slot >= 0) {
            bufferMaybeLog();
        }
        if (session != XR_NULL_HANDLE && usable() && ensureSwapchain(session, outW, m_Config.synth ? outH * 2 : outH)) {
            renderNewFrame(f, mboxUs);
            if (m_Config.passTimers) {
                // §VR-PASS：每 60 秒印一行各個 pass 的 GPU 時間（這一張已經發布了，不佔它的時間）
                const std::string line = m_PassStats.takeLine(nowUs());
                if (!line.empty()) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", line.c_str());
                }
            }
        }
        else {
            av_frame_free(&f);
            if (session != XR_NULL_HANDLE && usable() && m_Sw.swapchain == XR_NULL_HANDLE) {
                markUnusable("no usable video swapchain");
            }
        }
    }
}

bool XrVideo::current(Current* out)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    // §VR-REC：開了 --dump-bitstream 時記下這一格顯示的是哪一張（從發布紀錄查它的 rtpTimestamp），給離線重建用
    const auto logShown = [this](uint64_t seq, uint64_t newest, bool synth) {
        int64_t pts = -1;
        const uint64_t n = std::min<uint64_t>(m_PubHead, kPubRing);
        for (uint64_t i = 0; i < n; ++i) {
            const PubRec& p = m_PubRing[(m_PubHead - 1 - i) % kPubRing];
            if (p.seq == seq) {
                pts = p.pts90;
                break;
            }
        }
        BitstreamDump::logDisplay(nowUs(), seq, pts, newest, synth);
    };
    if (!m_HavePublished || m_Published.aspect <= 0.0f) {
        return false;
    }
    *out = m_Published;
    out->fbSeq = m_Published.seq;  // §VR-BUFFER：host 節拍一律照最新的一張
    out->fbDrawnUs = m_Published.lastDrawnUs;
    if (m_Config.buffer > 0) {
        // §VR-BUFFER：要顯示的序號＝max(上次顯示的下一張, 最新往前數 buffer 張)，不超過最新。那幾張必定還在各自的
        // swapchain 上（寫入的是第 buffer+2 新的那一條）；找不到（那一張沒畫成功）就退回最新的一張
        const uint64_t newest = m_Published.seq;
        if (m_BufDepth == 0) {
            m_BufAuto.reset(m_Config.buffer);
            m_BufDepth = m_Config.bufferAuto ? m_BufAuto.depth : m_Config.buffer;
            m_BufDepthLog.store(m_BufDepth, std::memory_order_relaxed);
        }
        // §VR-RATE-AUTO：半速時每張本來就佔兩格。11-08 第一版「緩衝少一格」實際等於沒有緩衝（每 10 秒 720 格裡 713 格
        // 顯示最新的那張），稍微晚到就變成「一張 3 格、下一張 1 格」的節奏抖動（每 10 秒 5～15 次）。改成：自動緩衝到
        // 2 格時鎖住節奏——這一張還沒顯示滿 div 格就繼續顯示它（就算新的一張已經到了）。晚到一次節奏就往後挪一格，
        // 之後的每一張都多一格餘裕；積到落後兩張以上才跳過追上。1 格時照舊一到就顯示（延遲最低）
        const int streamDiv = m_StreamDiv.load(std::memory_order_relaxed);
        uint64_t want;
        if (streamDiv >= 2 && m_BufDepth >= 2 && m_BufLastShown != 0 && m_BufShownCount < static_cast<uint32_t>(streamDiv) &&
            newest > m_BufLastShown && newest - m_BufLastShown <= 1) {
            want = m_BufLastShown;
        }
        else {
            int pickDepth = streamDiv >= 2 ? (m_BufDepth >= 2 ? 1 : 0) : m_BufDepth;
            // §VR-BUFFER-SHED：深度 2 卻連續幾秒都排兩張（斷線後一口氣補進來）→ 這一格跳一張、回到落後 1 張
            if (m_Config.bufferAuto && m_BufAuto.wantShed(m_BufLastShown, newest, nowUs(), streamDiv)) {
                pickDepth = 1;
                m_BufShed.fetch_add(1, std::memory_order_relaxed);
            }
            want = xrbuf::pick(m_BufLastShown, newest, pickDepth);
        }
        if (want > newest) {
            want = newest;
        }
        bool found = false;
        if (want != newest) {
            for (const Current& c : m_RingPub) {
                if (c.seq == want && c.aspect > 0.0f && c.generation != 0) {
                    *out = c;
                    out->fbSeq = m_Published.seq;
                    out->fbDrawnUs = m_Published.lastDrawnUs;
                    found = true;
                    break;
                }
            }
        }
        const uint64_t shown = found ? want : newest;
        if (shown == m_BufLastShown) {
            m_BufRepeat.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            (shown == newest ? m_BufLag0 : m_BufLag1).fetch_add(1, std::memory_order_relaxed);
            (shown == newest ? m_BufLag0Total : m_BufLag1Total).fetch_add(1, std::memory_order_relaxed);
            if (m_BufLastShown != 0 && shown > m_BufLastShown + 1) {
                m_BufSkipped.fetch_add(static_cast<uint32_t>(shown - m_BufLastShown - 1), std::memory_order_relaxed);
            }
        }
        m_BufShownCount = shown == m_BufLastShown ? m_BufShownCount + 1 : 1;  // §VR-RATE-AUTO：這一張顯示了幾格
        if (m_Config.bufferAuto) {
            const int before = m_BufAuto.depth;
            if (m_BufAuto.step(shown, m_BufLastShown, newest, nowUs(), streamDiv)) {
                m_BufDepth = m_BufAuto.depth;
                m_BufDepthLog.store(m_BufDepth, std::memory_order_relaxed);
                (m_BufDepth > before ? m_BufUp : m_BufDown).fetch_add(1, std::memory_order_relaxed);
            }
        }
        m_BufLastShown = shown;
        if (BitstreamDump::isEnabled()) {
            logShown(shown, newest, false);
        }
        return true;
    }
    if (m_Config.synth) {
        // §VR-SYNTH：一張新影像第一次被顯示用真的那一半；同一張再被顯示（半速串流時每張會顯示兩次）就換成
        // 合成的那一半與外插的姿態。沒有合成影像（第一張、掉過幀、合成失敗）時照舊重送真的那一半。
        if (m_Published.seq != m_FirstShownSeq) {
            m_FirstShownSeq = m_Published.seq;
        }
        else if (m_Published.hasSynth) {
            out->subImage.imageRect = m_Published.synthRect;
            out->meta = m_Published.synthMeta;
            out->synthShown = true;
            m_SynthShown.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (BitstreamDump::isEnabled()) {
        logShown(out->seq, m_Published.seq, out->synthShown);
    }
    return true;
}

uint64_t XrVideo::releaseNoFor(const Current& cur) const
{
    if (cur.slot >= 0 && cur.slot < kRing) {
        return m_RingRel[cur.slot].load(std::memory_order_relaxed);
    }
    return releaseNo();
}

bool XrVideo::peek(Current* out)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    if (!m_HavePublished || m_Published.aspect <= 0.0f) {
        return false;
    }
    *out = m_Published;
    return true;
}

bool XrVideo::latest(uint64_t* seq, uint64_t* pubUs)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    if (!m_HavePublished) {
        return false;
    }
    *seq = m_Published.seq;
    *pubUs = m_Published.publishedUs;
    return true;
}

bool XrVideo::waitNewerThan(uint64_t seq, uint64_t deadlineUs)
{
    // nowUs() 與 frame thread 的時鐘都是 steady_clock，deadlineUs 可以直接當成它的時間點
    const std::chrono::steady_clock::time_point deadline{std::chrono::microseconds(static_cast<int64_t>(deadlineUs))};
    std::unique_lock<std::mutex> lk(m_SwMutex);
    return m_PubCv.wait_until(lk, deadline, [&] { return m_HavePublished && m_Published.seq != seq; });
}

void XrVideo::flightInfo(uint64_t* latestSeq, uint64_t* flightSeq, uint64_t* flightSubmitUs)
{
    {
        // 快照的任何欄位，影像 render thread 以外的執行緒都要拿 m_SwMutex 讀。還沒有發布過時 seq 是 0
        std::lock_guard<std::mutex> lk(m_SwMutex);
        *latestSeq = m_Published.seq;
    }
    // 在途的標記是兩個 atomic（鎖外讀，影像 render thread 設、清它們時不拿鎖）：先讀序號、再讀時刻、再讀一次序號。
    // 不是 0 的序號每次都比上一次大，所以兩次讀到同一個值＝中間沒有換過一張，那個時刻就是這一張的；
    // 對不上（剛好在換）就當成沒有在途——頂多這一格少等一次。
    // 先讀到舊的 latestSeq、之後才讀到下一張的標記，xrflight::decide 的 F1（flightSeq == latestSeq + 1）會擋掉
    const uint64_t seq = m_FlightSeq.load();
    const uint64_t submitUs = m_FlightSubmitUs.load();
    const bool same = m_FlightSeq.load() == seq;
    *flightSeq = same ? seq : 0;
    *flightSubmitUs = submitUs;
}

size_t XrVideo::drainPublished(uint64_t afterSeq, uint64_t notAfterUs, PubRec* out, size_t max)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    size_t n = 0;
    for (uint64_t i = m_PubHead > kPubRing ? m_PubHead - kPubRing : 0; i < m_PubHead && n < max; i++) {
        const PubRec& r = m_PubRing[i % kPubRing];
        if (r.seq <= afterSeq) {
            continue;
        }
        if (r.pubUs > notAfterUs) {
            break;  // 發布時刻隨序號遞增：後面的更晚，留給下一次
        }
        out[n++] = r;
    }
    return n;
}

uint32_t XrVideo::jitterNextUs()
{
    if (!m_Jitter.enabled()) {
        return 0;
    }
    const uint32_t d = m_Jitter.nextUs(nowUs());
    if (d > 0) {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_JitUs.push_back(d);
    }
    return d;
}

namespace {

// 位移估計。頭部轉動造成的影像位移由兩張的算圖姿態逐像素算出來（vs_w＝轉動量；那一部分交給頭盔的重投影處理），
// 這裡只找「扣掉它之後還剩多少」：對每個像素，在上一張裡找 3x3 稀疏區塊最像的位置，用拋物線取到小數格。
// 不夠確定（比「沒有位移」好不到 40%）就當成 0——靜止的場景原樣保留，只有真的在動的物體會被外插。
// 輸出 rg＝剩餘位移（上一張的位置 − 現在的位置 − vs_g，整張 SBS 影像的 UV 單位）。搜尋不跨過左右眼。
// 跑兩次（vs_p＝搜尋半徑、要不要用起點）：先在 1/16 尺寸、半徑 6、沒有起點；再在 1/8 尺寸、半徑 3，以 1/16 那一層
// 的結果（vs_init）為起點。粗的一層沒過門檻時輸出 0，細的一層就等於在 0 附近找。
const char* kFlowHeader = R"GLSL(
float vs_lum(sampler2D t, vec2 uv) { return dot(texture(t, uv).rgb, vec3(0.299, 0.587, 0.114)); }
float vs_c0[9];
float vs_cost(sampler2D prev, vec2 uv, vec2 off, vec2 px, float xmin, float xmax) {
    float c = 0.0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 p = uv + (vec2(float(i), float(j)) * 1.5 + off) * px;
            p.x = clamp(p.x, xmin, xmax);
            c += abs(vs_c0[(j + 1) * 3 + (i + 1)] - vs_lum(prev, p));
        }
    }
    return c;
}
)GLSL";

const char* kFlowBody = R"GLSL(
vec2 sz = vec2(textureSize(vs_cur, 0));
vec2 px = 1.0 / sz;
vec2 uv = gl_FragCoord.xy * px;
float xmin = (uv.x < 0.5 ? 0.0 : 0.5) + 0.5 * px.x;
float xmax = xmin + 0.5 - px.x;
// 頭部轉動造成的位移，逐像素算（小轉角）：這個像素的視線方向是 (tx, ty, -1)，頭轉 w（弧度，機體座標）之後
// tx 變 tx + wy(1+tx²) + wz·ty − wx·tx·ty、ty 變 ty − wx(1+ty²) − wz·tx + wy·tx·ty。只用中心的值（原本的做法）
// 在影像邊緣會差到每幀好幾個像素，靜止的景物看起來「還有位移」，搜尋範圍加大之後窗格這類重複的圖樣就會被配錯。
// §VR-FOVEA：影像是壓縮過的時候，先把座標換回均勻取樣算視線方向，位移再除以這個位置的斜率。
float ex = (uv.x < 0.5 ? uv.x : uv.x - 0.5) * 2.0;
float cx = uv.x < 0.5 ? vs_fv.y : vs_fv.z;
float cy = uv.x < 0.5 ? vs_fv.w : vs_cyr;
float extx = ex < cx ? cx : 1.0 - cx;
float exty = uv.y < cy ? cy : 1.0 - cy;
float ux = (ex - cx) / extx;
float uy = (uv.y - cy) / exty;
float fb = 1.0 - vs_fv.x;
float tx = (vs_fv.x * ux + fb * ux * ux * ux) * extx * vs_tan.x;
float ty = -(vs_fv.x * uy + fb * uy * uy * uy) * exty * vs_tan.y;
// §VR-FOVEA-INSET：分段曲線（中央段斜率 a、外面 k2）
float ik2 = vs_fu > 0.0 ? (1.0 - vs_fv.x * vs_fu) / (1.0 - vs_fu) : 1.0;
if (vs_fu > 0.0) {
    float fx = abs(ux) <= vs_fu ? vs_fv.x * abs(ux) : vs_fv.x * vs_fu + ik2 * (abs(ux) - vs_fu);
    float fy = abs(uy) <= vs_fu ? vs_fv.x * abs(uy) : vs_fv.x * vs_fu + ik2 * (abs(uy) - vs_fu);
    tx = sign(ux) * fx * extx * vs_tan.x;
    ty = -sign(uy) * fy * exty * vs_tan.y;
}
float dtx = vs_w.y * (1.0 + tx * tx) + vs_w.z * ty - vs_w.x * tx * ty;
float dty = -vs_w.x * (1.0 + ty * ty) - vs_w.z * tx + vs_w.y * tx * ty;
vec2 slope = vs_fu > 0.0 ? vec2(abs(ux) <= vs_fu ? vs_fv.x : ik2, abs(uy) <= vs_fu ? vs_fv.x : ik2)
                        : vec2(vs_fv.x + 3.0 * fb * ux * ux, vs_fv.x + 3.0 * fb * uy * uy);
vec2 g = vec2(-0.5 * dtx / vs_tan.x, dty / vs_tan.y) / slope * sz;
for (int j = -1; j <= 1; j++) {
    for (int i = -1; i <= 1; i++) {
        vec2 p = uv + vec2(float(i), float(j)) * 1.5 * px;
        p.x = clamp(p.x, xmin, xmax);
        vs_c0[(j + 1) * 3 + (i + 1)] = vs_lum(vs_cur, p);
    }
}
float czero = vs_cost(vs_prev, uv, g, px, xmin, xmax);
float best = czero;
vec2 bd = vec2(0.0);
vec2 init = vec2(0.0);
if (vs_p.y > 0.5) {
    init = texture(vs_init, uv).xy * sz;  // 粗的一層給的剩餘位移（UV）換成這一層的格數
}
int rad = int(vs_p.x + 0.5);
for (int dy = -rad; dy <= rad; dy++) {
    for (int dx = -rad; dx <= rad; dx++) {
        vec2 d = init + vec2(float(dx), float(dy));
        float c = vs_cost(vs_prev, uv, g + d, px, xmin, xmax) + 0.004 * length(d);
        if (c < best) { best = c; bd = d; }
    }
}
vec2 r = vec2(0.0);
if (czero - best > 0.4 * czero + 0.08) {
    float cxm = vs_cost(vs_prev, uv, g + bd + vec2(-1.0, 0.0), px, xmin, xmax);
    float cxp = vs_cost(vs_prev, uv, g + bd + vec2(1.0, 0.0), px, xmin, xmax);
    float cym = vs_cost(vs_prev, uv, g + bd + vec2(0.0, -1.0), px, xmin, xmax);
    float cyp = vs_cost(vs_prev, uv, g + bd + vec2(0.0, 1.0), px, xmin, xmax);
    float bc = vs_cost(vs_prev, uv, g + bd, px, xmin, xmax);
    vec2 sub = vec2(0.0);
    float denx = cxm - 2.0 * bc + cxp;
    float deny = cym - 2.0 * bc + cyp;
    if (denx > 1e-5) { sub.x = clamp(0.5 * (cxm - cxp) / denx, -0.5, 0.5); }
    if (deny > 1e-5) { sub.y = clamp(0.5 * (cym - cyp) / deny, -0.5, 0.5); }
    r = bd + sub;
}
color = vec4(r * px, best, 1.0);
)GLSL";

// 合成＋複製：上半原樣複製這一張；下半把這一張沿（平滑過的）剩餘位移往前推半個串流週期——內容從 uv＋mv 移到 uv，
// 再過半個週期會到 uv − 0.5·mv，所以反向取樣 uv ＋ 0.5·mv。取樣不跨過左右眼的分界。
const char* kCombineBody = R"GLSL(
vec2 tsz = vec2(textureSize(vs_full, 0));
vec2 fc = gl_FragCoord.xy;
bool bottom = fc.y >= vs_osz.y;
vec2 uv = vs_unwarp(vec2(fc.x, bottom ? fc.y - vs_osz.y : fc.y) / vs_osz, vs_fv, vs_cyr, vs_fu);
if (!bottom) {
    color = texture(vs_full, uv);
} else {
    vec2 fpx = 0.75 / vec2(textureSize(vs_flow, 0));
    float fmin = (uv.x < 0.5 ? 0.0 : 0.5) + fpx.x;
    float fmax = fmin + 0.5 - 2.0 * fpx.x;
    vec2 mv = vec2(0.0);
    for (int j = -1; j <= 1; j += 2) {
        for (int i = -1; i <= 1; i += 2) {
            vec2 q = uv + vec2(float(i), float(j)) * fpx;
            q.x = clamp(q.x, fmin, fmax);
            mv += texture(vs_flow, q).xy;
        }
    }
    mv *= 0.25;
    // 物體邊緣：四點裡有的是物體的位移、有的是背景的 0，平均會讓邊緣只走一半、拖出殘影。最大的那個明顯大於平均時
    // 就是在邊緣上——前緣（物體要到的地方）畫出物體，後緣（物體剛離開的地方）從更後面取背景。
    vec2 mvMax = vec2(0.0);
    for (int j = -1; j <= 1; j += 2) {
        for (int i = -1; i <= 1; i += 2) {
            vec2 q = uv + vec2(float(i), float(j)) * fpx;
            q.x = clamp(q.x, fmin, fmax);
            vec2 m = texture(vs_flow, q).xy;
            if (dot(m, m) > dot(mvMax, mvMax)) { mvMax = m; }
        }
    }
    if (length(mvMax * tsz) > 1.5 * length(mv * tsz) + 1.0) {
        // 但旁邊的背景不能跟著被拖走（第一版直接用最大的，手把旁邊的文字被拉歪）：往回取樣的那一點在物體上
        // （那裡的位移也大）＝物體會蓋到這裡；或這一點現在就在物體上（後緣）——才用最大的。兩邊都是背景就不動。
        vec2 sq = uv + vs_k * mvMax;
        sq.x = clamp(sq.x, fmin, fmax);
        vec2 cq = vec2(clamp(uv.x, fmin, fmax), uv.y);
        vec2 ms = texture(vs_flow, sq).xy;
        vec2 mc = texture(vs_flow, cq).xy;
        float big = dot(mvMax, mvMax);
        mv = (dot(ms, ms) > 0.25 * big || dot(mc, mc) > 0.25 * big) ? mvMax : vec2(0.0);
    }
    float xmin = (uv.x < 0.5 ? 0.0 : 0.5) + 0.5 / tsz.x;
    vec2 s = uv + vs_k * mv;
    s.x = clamp(s.x, xmin, xmin + 0.5 - 1.0 / tsz.x);
    s.y = clamp(s.y, 0.5 / tsz.y, 1.0 - 0.5 / tsz.y);
    color = texture(vs_full, s);
}
)GLSL";

// §VR-FOVEA：還原注視點編碼。輸出（swapchain）是均勻取樣的座標，解碼出來的影像是壓縮過的：對每一軸解
// (1−a)·e³ + a·e = t（Cardano，再做一次牛頓法修掉 float 的誤差）。fv＝(a, cxL, cxR, cyL)、cyR＝右眼的垂直中心；
// a ≥ 0.999 時原樣回傳。
// 2026-10-10：垂直中心原本兩眼共用左眼的值。host 是每眼照自己的上下視角算中心壓縮的，而 Steam Frame 兩眼的上下視角
// 不同（cy 左 0.3978、右 0.4077）：右眼還原出來的畫面中央往上偏 (cyR−cyL)·(1−a) 個影像高度，注視點 150% 時約 0.6°，
// 只有右眼偏＝兩眼之間的垂直視差。
const char* kFovHeader = R"GLSL(
float vs_inv1(float x, float c, float a, float u0) {
    float ext = x < c ? c : 1.0 - c;
    float t = (x - c) / ext;
    if (u0 > 0.0) {
        // §VR-FOVEA-INSET：分段曲線的反解（host 的 fovea_warp_inset：|e| ≤ u0 時 t = a·e，外面線性到邊緣）
        float at = abs(t);
        float t0 = a * u0;
        float k2 = (1.0 - t0) / (1.0 - u0);
        float ae = at <= t0 ? at / a : u0 + (at - t0) / k2;
        return c + sign(t) * clamp(ae, 0.0, 1.0) * ext;
    }
    float b = 1.0 - a;
    float p = a / b;
    float q = t / b;
    float d = sqrt(q * q * 0.25 + p * p * p / 27.0);
    float u = q * 0.5 + d;
    float v = q * 0.5 - d;
    float e = sign(u) * pow(abs(u), 1.0 / 3.0) + sign(v) * pow(abs(v), 1.0 / 3.0);
    e -= (b * e * e * e + a * e - t) / (3.0 * b * e * e + a);
    return c + clamp(e, -1.0, 1.0) * ext;
}
vec2 vs_unwarp(vec2 uv, vec4 fv, float cyR, float u0) {
    if (fv.x >= 0.999) { return uv; }
    bool right = uv.x >= 0.5;
    float ex = (right ? uv.x - 0.5 : uv.x) * 2.0;
    float x = vs_inv1(ex, right ? fv.z : fv.y, fv.x, u0);
    float y = vs_inv1(uv.y, right ? cyR : fv.w, fv.x, u0);
    return vec2((right ? 0.5 : 0.0) + 0.5 * x, y);
}
)GLSL";

// §VR-UPSCALE：Catmull-Rom 放大，借硬體雙線性把 4x4 個點併成 9 次取樣（中間兩個權重同號，可合成一次雙線性）。
// vs_fsz＝vs_full 的像素尺寸。只在還原 pass 用（每眼 1152 放大到 2304 那一步）
const char* kCatromHeader = R"GLSL(
vec4 vs_catrom(vec2 uv) {
    vec2 sp = uv * vs_fsz;
    vec2 p1 = floor(sp - 0.5) + 0.5;
    vec2 f = sp - p1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (p1 - 1.0) / vs_fsz;
    vec2 t3 = (p1 + 2.0) / vs_fsz;
    vec2 t12 = (p1 + w2 / w12) / vs_fsz;
    vec4 r = texture(vs_full, vec2(t0.x, t0.y)) * (w0.x * w0.y)
           + texture(vs_full, vec2(t12.x, t0.y)) * (w12.x * w0.y)
           + texture(vs_full, vec2(t3.x, t0.y)) * (w3.x * w0.y)
           + texture(vs_full, vec2(t0.x, t12.y)) * (w0.x * w12.y)
           + texture(vs_full, vec2(t12.x, t12.y)) * (w12.x * w12.y)
           + texture(vs_full, vec2(t3.x, t12.y)) * (w3.x * w12.y)
           + texture(vs_full, vec2(t0.x, t3.y)) * (w0.x * w3.y)
           + texture(vs_full, vec2(t12.x, t3.y)) * (w12.x * w3.y)
           + texture(vs_full, vec2(t3.x, t3.y)) * (w3.x * w3.y);
    return clamp(r, 0.0, 1.0);
}
)GLSL";

const char* kUnwarpBody = R"GLSL(
color = vs_catrom(vs_unwarp(gl_FragCoord.xy / vs_osz, vs_fv, vs_cyr, vs_fu));
)GLSL";

// §VR-UNWARP-YUV（2026-11-10）：還原直接讀解碼出的 YUV 兩個平面，不先轉成一張 RGB 的中間圖（m_Full）。
// 11-10 和 Steam 內建串流對照：頭盔多耗約 4 W，來源是「轉 RGB → 還原成大圖 → 合成器再讀」這幾趟的記憶體頻寬；
// Steam 的合成器是用 YCbCr 取樣器直接讀解碼器的輸出（vrcompositor 的 CompositorImportDmaBuf＋SamplerYcbcrConversion）。
// 亮度用 Catmull-Rom（同 vs_catrom 的 9 次取樣，座標是亮度平面的像素），色度用雙線性（色度平面本來就是一半解析度）。
// vs_crop＝(x0, y0, 寬, 高) 亮度像素；vs_csh＝色度平面相對亮度的位移（亮度像素，libplacebo 的 plane.shift，
// 取樣位置＝(亮度座標 − shift) ÷ 比例，同 libplacebo renderer）；vs_rr＝亮度/色度的像素比；vs_ysz、vs_csz＝兩個平面
// 材質的尺寸。輸出 (Y, Cb, Cr)，之後接 libplacebo 自己的 pl_shader_decode_color＋pl_shader_color_map
// （和原本 pl_render_image 用的同一套轉換）
const char* kCatromYHeader = R"GLSL(
float vs_caty(vec2 sp) {
    vec2 p1 = floor(sp - 0.5) + 0.5;
    vec2 f = sp - p1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (p1 - 1.0) / vs_ysz;
    vec2 t3 = (p1 + 2.0) / vs_ysz;
    vec2 t12 = (p1 + w2 / w12) / vs_ysz;
    float r = texture(vs_y, vec2(t0.x, t0.y)).r * (w0.x * w0.y)
            + texture(vs_y, vec2(t12.x, t0.y)).r * (w12.x * w0.y)
            + texture(vs_y, vec2(t3.x, t0.y)).r * (w3.x * w0.y)
            + texture(vs_y, vec2(t0.x, t12.y)).r * (w0.x * w12.y)
            + texture(vs_y, vec2(t12.x, t12.y)).r * (w12.x * w12.y)
            + texture(vs_y, vec2(t3.x, t12.y)).r * (w3.x * w12.y)
            + texture(vs_y, vec2(t0.x, t3.y)).r * (w0.x * w3.y)
            + texture(vs_y, vec2(t12.x, t3.y)).r * (w12.x * w3.y)
            + texture(vs_y, vec2(t3.x, t3.y)).r * (w3.x * w3.y);
    return clamp(r, 0.0, 1.0);
}
)GLSL";

const char* kUnwarpYuvBody = R"GLSL(
vec2 uvn = vs_unwarp(gl_FragCoord.xy / vs_osz, vs_fv, vs_cyr, vs_fu);
vec2 lp = vs_crop.xy + uvn * vs_crop.zw;
// §VR-FOVEA-LAYERS：周邊畫成解碼原尺寸（約 1:1）時 Catmull-Rom 沒有好處，改一次雙線性（vs_bil＝1）。
// 但中央區在這一層是縮小的（每個輸出像素跨 1/a≈1.9 個編碼像素）：單點雙線性會漏掉一半像素、轉頭時閃，
// 而且那一塊正好是 quad 漸層帶底下的內容（11-10 配戴：接縫明顯）→ 用相鄰輸出像素的位置差估出覆蓋範圍，
// 超過 1.25 個像素就取 4 個雙線性點平均（每點涵蓋 2×2，合起來約等於覆蓋範圍）
float yy;
if (vs_bil > 0.5) {
    vec2 lpx = vs_crop.xy + vs_unwarp((gl_FragCoord.xy + vec2(1.0, 0.0)) / vs_osz, vs_fv, vs_cyr, vs_fu) * vs_crop.zw;
    vec2 lpy = vs_crop.xy + vs_unwarp((gl_FragCoord.xy + vec2(0.0, 1.0)) / vs_osz, vs_fv, vs_cyr, vs_fu) * vs_crop.zw;
    vec2 fd = vec2(abs(lpx.x - lp.x), abs(lpy.y - lp.y));
    if (max(fd.x, fd.y) > 1.25) {
        vec2 o = 0.25 * clamp(fd, vec2(1.0), vec2(3.0));  // 上限：左右眼交界那一欄的差是半張圖寬，不是縮放
        yy = 0.25 * (texture(vs_y, (lp + vec2(-o.x, -o.y)) / vs_ysz).r + texture(vs_y, (lp + vec2(o.x, -o.y)) / vs_ysz).r
                   + texture(vs_y, (lp + vec2(-o.x, o.y)) / vs_ysz).r + texture(vs_y, (lp + vec2(o.x, o.y)) / vs_ysz).r);
    } else {
        yy = texture(vs_y, lp / vs_ysz).r;
    }
} else {
    yy = vs_caty(lp);
}
vec2 cc = texture(vs_uv, ((lp - vs_csh) / vs_rr) / vs_csz).rg;
color = vec4(yy, cc.x, cc.y, 1.0);
)GLSL";

// 縮到 1/8：每個輸出像素取來源 8x8 區塊的平均（4x4 個雙線性取樣點，各涵蓋 2x2）。直接雙線性縮 8 倍只看得到其中
// 4 個像素，細字與細線會隨畫面微小的移動閃爍，位移估計會把它當成物體在動。
const char* kDownBody = R"GLSL(
vec2 tsz = vec2(textureSize(vs_full, 0));
vec2 osz = vec2(textureSize(vs_ref, 0));
vec2 c = gl_FragCoord.xy / osz;
vec2 st = 1.0 / tsz;
vec3 acc = vec3(0.0);
for (int j = 0; j < 4; j++) {
    for (int i = 0; i < 4; i++) {
        acc += texture(vs_full, c + (vec2(float(i), float(j)) * 2.0 - 3.0) * st).rgb;
    }
}
color = vec4(acc / 16.0, 1.0);
)GLSL";

pl_shader_desc sampledTex(const char* name, pl_tex tex)
{
    pl_shader_desc d = {};
    d.desc.name = name;
    d.desc.type = PL_DESC_SAMPLED_TEX;
    d.binding.object = tex;
    d.binding.address_mode = PL_TEX_ADDRESS_CLAMP;
    d.binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
    return d;
}

}  // namespace

// §VR-SYNTH：mapped → m_Full／m_Small；有上一張就估位移；最後一個 pass 把上半（複製）與下半（合成）寫進 swapchain。
// 回傳 false＝真的那一張也沒畫成功。*synthOk＝下半是合成影像（否則下半只是這一張的複製，不拿來顯示）。
bool XrVideo::renderSynth(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int fw, int fh,
                          bool hasMeta, const VIPLE_VR_FRAME_META& meta, int64_t pts, bool* synthOk, VIPLE_VR_FRAME_META* synthMeta)
{
    pl_gpu g = m_Vulkan->gpu;
    *synthOk = false;
    const auto caps = static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
    const bool wantFloat = m_Mode == TargetMode::Float16Linear;
    const float ow = static_cast<float>(m_Sw.width), oh = static_cast<float>(m_Sw.height / 2);  // swapchain 裡一張影像的尺寸
    float fv[4];
    float fvCyR;
    float fvU0;
    loadFoveation(fv, &fvCyR, &fvU0);
    if (hasMeta && meta.foveated) {
        fv[1] = meta.foveaCentre[0][0];
        fv[2] = meta.foveaCentre[1][0];
        fv[3] = meta.foveaCentre[0][1];
        fvCyR = meta.foveaCentre[1][1];
    }
    float osz[2] = {ow, oh};
    pl_fmt fmtSmall = pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, caps);
    pl_fmt fmtFlow = pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, caps);
    const int sw = std::max(16, fw / 8), sh = std::max(16, fh / 8);
    const int prevSmallW = m_Small[0] != nullptr ? m_Small[0]->params.w : 0;
    const int prevSmallH = m_Small[0] != nullptr ? m_Small[0]->params.h : 0;
    // dev：帶了 --xr-dump-frame 時這幾張要能讀回（dumpSynthInputs）。格式不支援讀回就照舊建立，只是那幾個檔不會存
    const bool devRead = !m_Config.dumpPath.isEmpty();
    auto mk = [g, devRead](pl_tex* t, int w, int h, pl_fmt f) {
        pl_tex_params tp = {};
        tp.w = w;
        tp.h = h;
        tp.format = f;
        tp.sampleable = true;
        tp.renderable = true;
        tp.host_readable = devRead && (f->caps & PL_FMT_CAP_HOST_READABLE) != 0;
        return pl_tex_recreate(g, t, &tp);
    };
    (void) wantFloat;
    const int mw = std::max(8, sw / 2), mh = std::max(8, sh / 2);
    if (fmtSmall == nullptr || fmtFlow == nullptr || !ensureFull(fw, fh) ||
        !mk(&m_Mid[0], mw, mh, fmtSmall) || !mk(&m_Mid[1], mw, mh, fmtSmall) || !mk(&m_FlowC, mw, mh, fmtFlow) ||
        !mk(&m_Small[0], sw, sh, fmtSmall) || !mk(&m_Small[1], sw, sh, fmtSmall) || !mk(&m_Flow, sw, sh, fmtFlow)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] texture setup failed - frame synthesis off for this session");
        m_SynthFailed = true;
        pl_frame t = *targetProto;
        t.crop.x0 = 0.0f;
        t.crop.y0 = 0.0f;
        t.crop.x1 = ow;
        t.crop.y1 = oh;
        return pl_render_image(m_Renderer, mapped, &t, &m_RenderParams);
    }
    if (m_FrameH != fh) {
        m_FrameH = fh;
        m_HavePrevSmall = false;
        m_HavePrevMeta = false;
    }
    if ((prevSmallW != 0 && prevSmallW != sw) || (prevSmallH != 0 && prevSmallH != sh)) {
        // §REVIEW-1104：縮圖寬或高變了，pl_tex_recreate 後內容未定義，不能拿來估位移
        m_HavePrevSmall = false;
    }

    pl_frame tFull = *targetProto;
    tFull.planes[0].texture = m_Full;
    // crop 是 swapchain 上的範圍，不能帶到中間這一張（見 renderUnwarp）：歸零＝畫滿整張 m_Full
    tFull.crop.x0 = 0.0f;
    tFull.crop.y0 = 0.0f;
    tFull.crop.x1 = 0.0f;
    tFull.crop.y1 = 0.0f;
    if (!pl_render_image(m_Renderer, mapped, &tFull, &m_RenderParams)) {
        // §REVIEW-1104：畫進 m_Full 失敗也要關掉合成，並把真幀畫進已 acquire 的 swapchain，否則 release 的是沒畫過的影像
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] render to full failed - frame synthesis off for this session");
        m_SynthFailed = true;
        pl_frame t = *targetProto;
        t.crop.x0 = 0.0f;
        t.crop.y0 = 0.0f;
        t.crop.x1 = ow;
        t.crop.y1 = oh;
        return pl_render_image(m_Renderer, mapped, &t, &m_RenderParams);
    }
    const int cur = m_SmallCur, prev = cur ^ 1;
    bool smallOk = false;
    {
        // vs_ref 只用來取輸出尺寸（另一張縮圖，尺寸相同）
        pl_shader shd = pl_dispatch_begin(m_Dp);
        pl_shader_desc dd[2] = {sampledTex("vs_full", m_Full), sampledTex("vs_ref", m_Small[prev])};
        pl_custom_shader cs = {};
        cs.description = "viple vr synth downscale";
        cs.body = kDownBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 2;
        if (pl_shader_custom(shd, &cs)) {
            pl_dispatch_params dpar = {};
            dpar.shader = &shd;
            dpar.target = m_Small[cur];
            smallOk = pl_dispatch_finish(m_Dp, &dpar);
        }
        else {
            pl_dispatch_abort(m_Dp, &shd);
        }
    }
    if (smallOk) {
        // 1/16：1/8 那一張再縮一半（雙線性取樣點落在 2x2 的中心＝四個像素的平均）
        pl_shader shd = pl_dispatch_begin(m_Dp);
        pl_shader_desc dd[2] = {sampledTex("vs_full", m_Small[cur]), sampledTex("vs_ref", m_Mid[prev])};
        pl_custom_shader cs = {};
        cs.description = "viple vr synth downscale 16";
        cs.body = "color = vec4(texture(vs_full, gl_FragCoord.xy / vec2(textureSize(vs_ref, 0))).rgb, 1.0);";
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 2;
        smallOk = false;
        if (pl_shader_custom(shd, &cs)) {
            pl_dispatch_params dpar = {};
            dpar.shader = &shd;
            dpar.target = m_Mid[cur];
            smallOk = pl_dispatch_finish(m_Dp, &dpar);
        }
        else {
            pl_dispatch_abort(m_Dp, &shd);
        }
    }

    // 位移：要有上一張、兩張的算圖姿態、而且是連續的兩幀（pts 差 1；掉過幀就不合成），轉動也不能太大
    bool flowOk = false;
    float half[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    // 這裡的 pts 已經不是幀號（decoder 把它換成呈現時間）：用間隔的移動平均當一幀的長度，超過 1.6 倍就是中間掉過幀
    // §REVIEW-1104：幀間隔基準改成成員，destroy 會歸零；單次大間隔仍不上修
    const int64_t ptsDelta = pts - m_PrevPts;
    if (m_HavePrevMeta && ptsDelta > 0) {
        if (m_NominalDelta == 0) {
            m_NominalDelta = ptsDelta;
        }
        else if (ptsDelta < m_NominalDelta * 3) {
            m_NominalDelta = (m_NominalDelta * 7 + ptsDelta) / 8;
        }
    }
    const bool consecutive = ptsDelta > 0 && m_NominalDelta > 0 && ptsDelta * 10 <= m_NominalDelta * 16;
    if (smallOk && m_HavePrevSmall && hasMeta && m_HavePrevMeta && consecutive) {
        // d＝q_prev⁻¹·q_cur（上一張到這一張的轉動，機體座標）；half＝它的一半＝normalize(d ＋ 1)
        const float* a = m_PrevMeta.renderRot;
        const float* b = meta.renderRot;
        const float ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
        float d[4] = {
            aw * b[0] + ax * b[3] + ay * b[2] - az * b[1],
            aw * b[1] - ax * b[2] + ay * b[3] + az * b[0],
            aw * b[2] + ax * b[1] - ay * b[0] + az * b[3],
            aw * b[3] - ax * b[0] - ay * b[1] - az * b[2],
        };
        if (d[3] < 0.0f) {
            d[0] = -d[0]; d[1] = -d[1]; d[2] = -d[2]; d[3] = -d[3];
        }
        if (d[3] > 0.9962f) {  // 兩張之間轉不到 10°
            float h[4] = {d[0], d[1], d[2], d[3] + 1.0f};
            const float n = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2] + h[3] * h[3]);
            if (n > 1e-6f) {
                for (int i = 0; i < 4; i++) {
                    half[i] = h[i] / n;
                }
                // 頭部轉動造成的整張影像位移（小角度近似）：ω＝2·d.xyz（弧度，機體座標）。往左轉（ω.y > 0）景物往右移、
                // 抬頭（ω.x > 0）景物往下移；mv 的定義是「上一張的位置 − 現在的位置」，所以取負號。一隻眼佔 UV 寬度的一半。
                const float tanW = std::max(0.5f, m_FovTanW.load(std::memory_order_relaxed));
                const float tanH = std::max(0.5f, m_FovTanH.load(std::memory_order_relaxed));
                float wvec[3] = {2.0f * d[0], 2.0f * d[1], 2.0f * d[2]};
                float tvec[2] = {tanW, tanH};
                // 兩層：1/16 尺寸半徑 6（起點 0）→ 1/8 尺寸半徑 3（起點＝粗層的結果）。vs_init 一定要綁一張材質：
                // 粗層不用它（vs_p.y＝0），綁 m_Flow（這個 pass 不寫它）
                auto runFlow = [&](pl_tex curT, pl_tex prevT, pl_tex initT, pl_tex target, float radius, float useInit) {
                    float pvec[2] = {radius, useInit};
                    pl_shader shf = pl_dispatch_begin(m_Dp);
                    pl_shader_desc fd[3] = {sampledTex("vs_cur", curT), sampledTex("vs_prev", prevT), sampledTex("vs_init", initT)};
                    pl_shader_var gv[6] = {};
                    gv[0].var = pl_var_vec3("vs_w");
                    gv[0].data = wvec;
                    gv[0].dynamic = true;
                    gv[3].var = pl_var_vec2("vs_tan");
                    gv[3].data = tvec;
                    gv[3].dynamic = true;
                    gv[1].var = pl_var_vec4("vs_fv");
                    gv[1].data = fv;
                    gv[1].dynamic = true;
                    gv[2].var = pl_var_vec2("vs_p");
                    gv[2].data = pvec;
                    gv[2].dynamic = true;
                    gv[4].var = pl_var_float("vs_cyr");
                    gv[4].data = &fvCyR;
                    gv[4].dynamic = true;
                    gv[5].var = pl_var_float("vs_fu");  // §VR-FOVEA-INSET
                    gv[5].data = &fvU0;
                    gv[5].dynamic = true;
                    pl_custom_shader cs = {};
                    cs.description = "viple vr synth flow";
                    cs.header = kFlowHeader;
                    cs.body = kFlowBody;
                    cs.input = PL_SHADER_SIG_NONE;
                    cs.output = PL_SHADER_SIG_COLOR;
                    cs.descriptors = fd;
                    cs.num_descriptors = 3;
                    cs.variables = gv;
                    cs.num_variables = 6;
                    if (!pl_shader_custom(shf, &cs)) {
                        pl_dispatch_abort(m_Dp, &shf);
                        return false;
                    }
                    pl_dispatch_params dpar = {};
                    dpar.shader = &shf;
                    dpar.target = target;
                    return pl_dispatch_finish(m_Dp, &dpar);
                };
                flowOk = runFlow(m_Mid[cur], m_Mid[prev], m_Flow, m_FlowC, 6.0f, 0.0f) &&
                         runFlow(m_Small[cur], m_Small[prev], m_FlowC, m_Flow, 3.0f, 1.0f);
                if (!flowOk) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] flow pass failed - frame synthesis off for this session");
                    m_SynthFailed = true;
                }
            }
        }
    }

    // 上半複製、下半合成（沒有位移場時 k＝0：下半只是複製，不會被拿來顯示）
    float k = flowOk ? 0.5f : 0.0f;
    bool combined = false;
    {
        pl_shader shc = pl_dispatch_begin(m_Dp);
        pl_shader_desc cd[2] = {sampledTex("vs_full", m_Full), sampledTex("vs_flow", m_Flow)};
        pl_shader_var kv[5] = {};
        kv[0].var = pl_var_float("vs_k");
        kv[0].data = &k;
        kv[0].dynamic = true;
        kv[1].var = pl_var_vec2("vs_osz");
        kv[1].data = osz;
        kv[1].dynamic = true;
        kv[2].var = pl_var_vec4("vs_fv");
        kv[2].data = fv;
        kv[2].dynamic = true;
        kv[3].var = pl_var_float("vs_cyr");
        kv[3].data = &fvCyR;
        kv[3].dynamic = true;
        kv[4].var = pl_var_float("vs_fu");  // §VR-FOVEA-INSET
        kv[4].data = &fvU0;
        kv[4].dynamic = true;
        pl_custom_shader cs = {};
        cs.description = "viple vr synth combine";
        cs.header = kFovHeader;
        cs.body = kCombineBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = cd;
        cs.num_descriptors = 2;
        cs.variables = kv;
        cs.num_variables = 5;
        if (pl_shader_custom(shc, &cs)) {
            pl_dispatch_params dpar = {};
            dpar.shader = &shc;
            dpar.target = swTex;
            combined = pl_dispatch_finish(m_Dp, &dpar);
        }
        else {
            pl_dispatch_abort(m_Dp, &shc);
        }
    }
    if (!combined) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] combine pass failed - frame synthesis off for this session");
        m_SynthFailed = true;
        pl_frame t = *targetProto;
        t.crop.x0 = 0.0f;
        t.crop.y0 = 0.0f;
        t.crop.x1 = ow;
        t.crop.y1 = oh;
        return pl_render_image(m_Renderer, mapped, &t, &m_RenderParams);
    }

    if (flowOk) {
        // 合成那一格沿用這一張的算圖姿態：頭部的轉動交給頭盔的重投影（位移場已經扣掉那一部分），
        // 這裡只外插場景裡在動的東西
        *synthMeta = meta;
        (void) half;
        *synthOk = true;
    }
    m_HavePrevSmall = smallOk;
    m_HavePrevMeta = hasMeta;
    m_PrevMeta = meta;
    m_PrevPts = pts;
    m_SmallCur = prev;
    return true;
}

void XrVideo::loadFoveation(float fv[4], float* cyR, float* u0) const
{
    *u0 = m_FvU0.load(std::memory_order_relaxed);
    fv[0] = m_FvA.load(std::memory_order_relaxed);
    fv[1] = m_FvCxL.load(std::memory_order_relaxed);
    fv[2] = m_FvCxR.load(std::memory_order_relaxed);
    fv[3] = m_FvCyL.load(std::memory_order_relaxed);
    *cyR = m_FvCyR.load(std::memory_order_relaxed);
    if (!(fv[0] >= 0.4f && fv[0] < 0.999f)) {
        fv[0] = 1.0f;
    }
}

// §VR-FOVEA-LAYERS：分段注視點（u0＞0）的中央 |e|≤u0 在編碼影像裡是 1:1、密度最高的一塊，原樣放進 swapchain 下方，
// 每眼 u0×(每眼寬) × u0×(高)。只在直接讀 YUV 的還原可用、沒開補幀（補幀用兩倍高的 swapchain）時才做
bool XrVideo::tilesFor(int fw, int fh, int* tw, int* th, int* rw, int* rh) const
{
    const int w = m_TileW.load(std::memory_order_relaxed);
    const int h = m_TileH.load(std::memory_order_relaxed);
    const int renderW = m_TileRW.load(std::memory_order_relaxed);
    const int renderH = m_TileRH.load(std::memory_order_relaxed);
    if (w < 64 || h < 64 || renderW <= w || renderH <= h || fw != 2 * w || fh != 2 * h) {
        return false;
    }
    *tw = w;
    *th = h;
    *rw = renderW;
    *rh = renderH;
    return true;
}

bool XrVideo::insetSize(int fw, int fh, int* iw, int* ih) const
{
    int tw = 0, th = 0, rw = 0, rh = 0;
    if (tilesFor(fw, fh, &tw, &th, &rw, &rh)) {
        return false;  // §VR-FOVEA-TILES：分塊的畫面沒有幾何變形，swapchain 就是解碼尺寸
    }
    const float a = m_FvA.load(std::memory_order_relaxed);
    const float u0 = m_FvU0.load(std::memory_order_relaxed);
    if (!m_Config.fovLayers || m_InsetFailed || m_Config.synth || !m_Config.unwarpYuv || m_UnwarpYuvFailed ||
        m_UnwarpFailed || !(a >= 0.4f && a < 0.999f) || !(u0 > 0.05f && u0 < 0.95f) || fw < 4 || fh < 2) {
        return false;
    }
    *iw = std::max(2, static_cast<int>(u0 * static_cast<float>(fw / 2) + 0.5f));
    *ih = std::max(2, static_cast<int>(u0 * static_cast<float>(fh) + 0.5f));
    return true;
}

void XrVideo::outputSize(int fw, int fh, int* ow, int* oh) const
{
    *ow = fw;
    *oh = fh;
    int iw = 0, ih = 0;
    if (insetSize(fw, fh, &iw, &ih)) {
        // §VR-FOVEA-LAYERS：周邊＝解碼原尺寸（上方），兩眼的中央區並排在下方一條
        *oh = fh + ih;
        return;
    }
    const float a = m_FvA.load(std::memory_order_relaxed);
    if (a >= 0.4f && a < 0.999f && fw > 0 && fh > 0) {
        // 正前方的像素密度是 1/a 倍：swapchain 跟著放大才放得下那些細節。每眼最高 2304（頭盔面板每眼 2160）
        const float s = std::min(1.0f / a, std::clamp(m_Config.unwarpMax, 1.0f, 2.0f));  // §VR-UNWARP-MAX
        int h = std::min(static_cast<int>(static_cast<float>(fh) * s + 0.5f), 2304);
        h = std::max(h, fh) & ~1;
        *oh = h;
        *ow = static_cast<int>(static_cast<int64_t>(fw) * h / fh) & ~1;
    }
}

// m_Full：解碼出來的影像轉成 RGB 的那一張（影像原本的尺寸），之後的 pass 從它取樣
bool XrVideo::ensureFull(int fw, int fh)
{
    pl_gpu g = m_Vulkan->gpu;
    const auto caps = static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
    pl_fmt fmt = m_Mode == TargetMode::Float16Linear ? pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, caps)
                                                     : pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, caps);
    if (fmt == nullptr) {
        return false;
    }
    pl_tex_params tp = {};
    tp.w = fw;
    tp.h = fh;
    tp.format = fmt;
    tp.sampleable = true;
    tp.renderable = true;
    return pl_tex_recreate(g, &m_Full, &tp);
}

// §VR-FOVEA（不合成中間格時）：mapped → m_Full → 還原後寫進 swapchain。失敗就改回直接畫（影像是壓縮過的樣子）。
// §VR-UNWARP-YUV：見 kUnwarpYuvBody。只處理「亮度一個平面＋CbCr 交錯一個平面」（NV12／P010 這類）；其他排法回 false
bool XrVideo::renderUnwarpYuv(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, bool hasMeta,
                              const VIPLE_VR_FRAME_META& meta, const char** why)
{
    if (mapped->num_planes != 2 || mapped->planes[0].texture == nullptr || mapped->planes[1].texture == nullptr) {
        *why = "not two planes";
        return false;
    }
    const pl_plane& py = mapped->planes[0];
    const pl_plane& pc = mapped->planes[1];
    if (py.components != 1 || py.component_mapping[0] != PL_CHANNEL_Y || pc.components != 2 ||
        pc.component_mapping[0] != PL_CHANNEL_CB || pc.component_mapping[1] != PL_CHANNEL_CR) {
        *why = "plane layout is not Y + CbCr";
        return false;
    }
    if (!pl_color_system_is_ycbcr_like(mapped->repr.sys)) {
        *why = "colour system is not YCbCr";
        return false;
    }
    const pl_tex ty = py.texture;
    const pl_tex tc = pc.texture;
    if (!(ty->params.format->caps & PL_FMT_CAP_LINEAR) || !(tc->params.format->caps & PL_FMT_CAP_LINEAR) ||
        !ty->params.sampleable || !tc->params.sampleable) {
        *why = "planes are not linearly sampleable";
        return false;
    }
    const float yw = static_cast<float>(ty->params.w);
    const float yh = static_cast<float>(ty->params.h);
    const float cw = static_cast<float>(tc->params.w);
    const float ch = static_cast<float>(tc->params.h);
    float crop[4] = {mapped->crop.x0, mapped->crop.y0, mapped->crop.x1 - mapped->crop.x0, mapped->crop.y1 - mapped->crop.y0};
    if (crop[2] <= 0.0f || crop[3] <= 0.0f) {
        crop[0] = 0.0f;
        crop[1] = 0.0f;
        crop[2] = yw;
        crop[3] = yh;
    }
    float ysz[2] = {yw, yh};
    float csz[2] = {cw, ch};
    float rr[2] = {std::round(yw / cw), std::round(yh / ch)};  // libplacebo 也只認整數倍（無條件進位的那一格不算）
    float csh[2] = {pc.shift_x, pc.shift_y};
    float fv[4];
    float fvCyR;
    float fvU0;
    loadFoveation(fv, &fvCyR, &fvU0);
    if (hasMeta && meta.foveated) {
        fv[1] = meta.foveaCentre[0][0];
        fv[2] = meta.foveaCentre[1][0];
        fv[3] = meta.foveaCentre[0][1];
        fvCyR = meta.foveaCentre[1][1];
    }
    // §VR-FOVEA-LAYERS：targetProto 有 crop（下方留給中央區）時只畫上面那一塊
    const int outH = targetProto->crop.y1 > 0.0f ? static_cast<int>(targetProto->crop.y1 + 0.5f)
                                                 : (m_Config.synth ? m_Sw.height / 2 : m_Sw.height);
    float osz[2] = {static_cast<float>(m_Sw.width), static_cast<float>(outH)};
    if (!m_LoggedUnwarpYuvGeom) {
        m_LoggedUnwarpYuvGeom = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-FOVEA] YUV unwarp: luma %dx%d chroma %dx%d crop %.0f,%.0f %.0fx%.0f chroma shift %.2f,%.2f sys=%d levels=%d -> %dx%d",
                    ty->params.w, ty->params.h, tc->params.w, tc->params.h, crop[0], crop[1], crop[2], crop[3],
                    csh[0], csh[1], static_cast<int>(mapped->repr.sys), static_cast<int>(mapped->repr.levels),
                    static_cast<int>(osz[0]), static_cast<int>(osz[1]));
    }

    pl_shader sh = pl_dispatch_begin(m_Dp);
    pl_shader_desc dd[2] = {sampledTex("vs_y", ty), sampledTex("vs_uv", tc)};
    // 輸出不比解碼影像大（兩圖層的周邊）時亮度改一次雙線性
    float bil = osz[1] <= crop[3] + 0.5f ? 1.0f : 0.0f;
    pl_shader_var vv[10] = {};
    vv[0].var = pl_var_vec2("vs_osz");
    vv[0].data = osz;
    vv[1].var = pl_var_vec4("vs_fv");
    vv[1].data = fv;
    vv[2].var = pl_var_float("vs_cyr");
    vv[2].data = &fvCyR;
    vv[3].var = pl_var_float("vs_fu");
    vv[3].data = &fvU0;
    vv[4].var = pl_var_vec4("vs_crop");
    vv[4].data = crop;
    vv[5].var = pl_var_vec2("vs_ysz");
    vv[5].data = ysz;
    vv[6].var = pl_var_vec2("vs_csz");
    vv[6].data = csz;
    vv[7].var = pl_var_vec2("vs_rr");
    vv[7].data = rr;
    vv[8].var = pl_var_vec2("vs_csh");
    vv[8].data = csh;
    vv[9].var = pl_var_float("vs_bil");
    vv[9].data = &bil;
    for (pl_shader_var& v : vv) {
        v.dynamic = true;
    }
    static const std::string s_Header = std::string(kFovHeader) + kCatromYHeader;
    pl_custom_shader cs = {};
    cs.description = "viple vr fovea unwarp yuv";  // PassStats 照 "fovea unwarp" 歸到 unwarp
    cs.header = s_Header.c_str();
    cs.body = kUnwarpYuvBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = dd;
    cs.num_descriptors = 2;
    cs.variables = vv;
    cs.num_variables = 10;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(m_Dp, &sh);
        *why = "custom shader rejected";
        return false;
    }
    // 和 pl_render_image 同一套：YCbCr（含 limited range、位元深度）→ RGB，再從影像的色彩空間轉到 swapchain 的
    pl_color_repr repr = mapped->repr;
    repr.alpha = PL_ALPHA_NONE;
    pl_shader_decode_color(sh, &repr, nullptr);
    pl_color_space src = mapped->color;
    pl_color_space dst = targetProto->color;
    pl_color_space_infer_map(&src, &dst);
    pl_shader_color_map(sh, nullptr, src, dst, nullptr, false);
    pl_dispatch_params dpar = {};
    dpar.shader = &sh;
    dpar.target = swTex;
    dpar.rect = {0, 0, m_Sw.width, outH};
    if (!pl_dispatch_finish(m_Dp, &dpar)) {
        *why = "dispatch failed";
        return false;
    }
    return true;
}

// §VR-FOVEA-LAYERS：兩眼的中央區（編碼影像裡 1:1 的那一塊）原樣搬到 swapchain 的 y＝top 那一條，左眼在左、右眼在右，
// 各 iw×ih。取樣同 kUnwarpYuvBody（亮度 Catmull-Rom、色度雙線性、libplacebo 的色彩轉換），最後把四邊做 alpha 漸層
// （預乘），quad 疊在 projection 上時邊緣不會有一條硬線
static const char* const kInsetBody = R"GLSL(
vec2 ip = gl_FragCoord.xy - vec2(0.0, vs_top);
float eye = ip.x >= vs_isz.x ? 1.0 : 0.0;
vec2 iq = (ip - vec2(eye * vs_isz.x, 0.0)) / vs_isz;
vec2 ic = eye > 0.5 ? vec2(vs_fv.z, vs_cyr) : vec2(vs_fv.y, vs_fv.w);
vec2 ie = ic * (1.0 - vs_fu) + iq * vs_fu;
vec2 uvn = vec2(eye * 0.5 + 0.5 * ie.x, ie.y);
vec2 lp = vs_crop.xy + uvn * vs_crop.zw;
// 中央區是原樣搬移（1:1）：亮度對齊到最近的像素中心直接取（位移不到半個像素），不做 9 次取樣也不會變糊
float yy = texture(vs_y, (floor(lp) + 0.5) / vs_ysz).r;
vec2 cc = texture(vs_uv, ((lp - vs_csh) / vs_rr) / vs_csz).rg;
color = vec4(yy, cc.x, cc.y, 1.0);
)GLSL";

// 2026-11-10：alpha 預乘要在線性光做。合成器把 swapchain（sRGB）解成線性光再混合（quad layer 的 alpha 當預乘），
// 2.0.118～2.0.120 是直接在 sRGB 編碼值上乘 alpha：解回線性後變成 lin·fa^2.2，比該有的 lin·fa 暗——漸層帶亮度只剩
// 約 80%，配戴時就是中央區外圍一圈暗框。vs_enc*＝1：這趟輸出的是 sRGB 編碼值（SrgbMutable），先解回線性、乘 alpha、
// 再編回去；0：輸出本來就是線性（硬體寫入時才編碼），直接乘
static const char* const kInsetFeatherBody = R"GLSL(
vec2 fp = gl_FragCoord.xy - vec2(0.0, vs_top2);
float fx = fp.x >= vs_isz2.x ? fp.x - vs_isz2.x : fp.x;
vec2 fq = vec2(fx, fp.y) / vs_isz2;
float fm = min(min(fq.x, 1.0 - fq.x), min(fq.y, 1.0 - fq.y));
float fa = smoothstep(0.0, vs_fe, fm);
if (vs_enc2 > 0.5) {
    vec3 fc = max(color.rgb, vec3(0.0));
    vec3 fl = mix(fc / 12.92, pow((fc + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), fc)) * fa;
    color.rgb = mix(fl * 12.92, 1.055 * pow(fl, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), fl));
} else {
    color.rgb *= fa;
}
color.a = fa;
)GLSL";

// §VR-FOVEA-TILES（2026-11-10）：解碼出來的畫面和 swapchain 同尺寸、同排法，整張 1:1 搬——亮度取所在像素、色度雙線性
// （4:2:0 的取樣位置照 libplacebo 的平面位移），輸出 (Y, Cb, Cr)，之後接 libplacebo 的色彩轉換
static const char* const kTilesBody = R"GLSL(
vec2 lp = vs_crop.xy + gl_FragCoord.xy;
float yy = texture(vs_y, (floor(lp) + 0.5) / vs_ysz).r;
vec2 cc = texture(vs_uv, ((lp - vs_csh) / vs_rr) / vs_csz).rg;
color = vec4(yy, cc.x, cc.y, 1.0);
)GLSL";

// 下排注視點那兩塊的邊緣漸層，照 Steam 自己的 OpenXR 客戶端（Quest 版 Steam Link，libvrlink_scene.so 內嵌的 GLSL）：
//   alpha = pow(1.05 − 1.06·(|2dx|^10 + |2dy|^10), 1.5)，dx、dy＝離那一塊中心的距離（0～0.5）
// ＝指數 10 的圓角方形：離中心 74% 半寬以內是 1，到邊緣降到 0（圓角只丟掉約 1.4% 的像素）。上排全視野 alpha＝1。
// 預乘在線性光做（見 kInsetFeatherBody 上面的說明）
static const char* const kTilesMaskBody = R"GLSL(
float ta = 1.0;
vec2 tp = gl_FragCoord.xy - vec2(0.0, vs_tile.y);
if (tp.y >= 0.0) {
    float tx = tp.x >= vs_tile.x ? tp.x - vs_tile.x : tp.x;
    vec2 td = abs(vec2(tx, tp.y) / vs_tile - 0.5) * 2.0;
    vec2 td2 = td * td;
    vec2 td4 = td2 * td2;
    vec2 td10 = td4 * td4 * td2;
    ta = pow(clamp(1.05 - 1.06 * (td10.x + td10.y), 0.0, 1.0), 1.5);
    if (tp.x >= 2.0 * vs_tile.x) {
        ta = 0.0;
    }
    if (ta < 1.0) {
        if (vs_enc > 0.5) {
            vec3 tc = max(color.rgb, vec3(0.0));
            vec3 tl = mix(tc / 12.92, pow((tc + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), tc)) * ta;
            color.rgb = mix(tl * 12.92, 1.055 * pow(tl, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), tl));
        } else {
            color.rgb *= ta;
        }
    }
}
color.a = ta;
)GLSL";

bool XrVideo::renderInsets(pl_frame* mapped, pl_tex swTex, int top, int iw, int ih, const float fv[4], float cyR, float u0)
{
    if (mapped->num_planes != 2 || mapped->planes[0].texture == nullptr || mapped->planes[1].texture == nullptr) {
        return false;
    }
    const pl_plane& pc = mapped->planes[1];
    const pl_tex ty = mapped->planes[0].texture;
    const pl_tex tc = pc.texture;
    const float yw = static_cast<float>(ty->params.w);
    const float yh = static_cast<float>(ty->params.h);
    const float cw = static_cast<float>(tc->params.w);
    const float ch = static_cast<float>(tc->params.h);
    float crop[4] = {mapped->crop.x0, mapped->crop.y0, mapped->crop.x1 - mapped->crop.x0, mapped->crop.y1 - mapped->crop.y0};
    if (crop[2] <= 0.0f || crop[3] <= 0.0f) {
        crop[0] = 0.0f;
        crop[1] = 0.0f;
        crop[2] = yw;
        crop[3] = yh;
    }
    float ysz[2] = {yw, yh};
    float csz[2] = {cw, ch};
    float rr[2] = {std::round(yw / cw), std::round(yh / ch)};
    float csh[2] = {pc.shift_x, pc.shift_y};
    float fvv[4] = {fv[0], fv[1], fv[2], fv[3]};
    float cyr = cyR;
    float fu = u0;
    float topF = static_cast<float>(top);
    float isz[2] = {static_cast<float>(iw), static_cast<float>(ih)};
    // 漸層寬度＝中央區邊長的 20%（11-10 配戴：8% 接縫太明顯，中央 1:1 與周邊的密度差約 4.5 倍、8% 只有約 3°
    // 就換過去；20% 約 8°，完全清晰的範圍仍有中央區的 60%，眼動追蹤下約 ±12°）
    float fe = 0.20f;

    pl_shader sh = pl_dispatch_begin(m_Dp);
    pl_shader_desc dd[2] = {sampledTex("vs_y", ty), sampledTex("vs_uv", tc)};
    pl_shader_var vv[10] = {};
    vv[0].var = pl_var_float("vs_top");
    vv[0].data = &topF;
    vv[1].var = pl_var_vec2("vs_isz");
    vv[1].data = isz;
    vv[2].var = pl_var_vec4("vs_fv");
    vv[2].data = fvv;
    vv[3].var = pl_var_float("vs_cyr");
    vv[3].data = &cyr;
    vv[4].var = pl_var_float("vs_fu");
    vv[4].data = &fu;
    vv[5].var = pl_var_vec4("vs_crop");
    vv[5].data = crop;
    vv[6].var = pl_var_vec2("vs_ysz");
    vv[6].data = ysz;
    vv[7].var = pl_var_vec2("vs_csz");
    vv[7].data = csz;
    vv[8].var = pl_var_vec2("vs_rr");
    vv[8].data = rr;
    vv[9].var = pl_var_vec2("vs_csh");
    vv[9].data = csh;
    for (pl_shader_var& v : vv) {
        v.dynamic = true;
    }
    pl_custom_shader cs = {};
    cs.description = "viple vr fovea centre";
    cs.header = kCatromYHeader;
    cs.body = kInsetBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = dd;
    cs.num_descriptors = 2;
    cs.variables = vv;
    cs.num_variables = 10;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(m_Dp, &sh);
        return false;
    }
    pl_color_repr repr = mapped->repr;
    repr.alpha = PL_ALPHA_NONE;
    pl_shader_decode_color(sh, &repr, nullptr);
    pl_color_space src = mapped->color;
    pl_color_space dst = pl_color_space_srgb;
    if (m_Mode != TargetMode::SrgbMutable) {
        dst.transfer = PL_COLOR_TRC_LINEAR;
    }
    pl_color_space_infer_map(&src, &dst);
    pl_shader_color_map(sh, nullptr, src, dst, nullptr, false);
    float enc2 = m_Mode == TargetMode::SrgbMutable ? 1.0f : 0.0f;  // 輸出是 sRGB 編碼值 → 預乘前先解回線性
    pl_shader_var fvar[4] = {};
    fvar[0].var = pl_var_float("vs_top2");
    fvar[0].data = &topF;
    fvar[1].var = pl_var_vec2("vs_isz2");
    fvar[1].data = isz;
    fvar[2].var = pl_var_float("vs_fe");
    fvar[2].data = &fe;
    fvar[3].var = pl_var_float("vs_enc2");
    fvar[3].data = &enc2;
    for (pl_shader_var& v : fvar) {
        v.dynamic = true;
    }
    pl_custom_shader fs = {};
    fs.description = "viple vr fovea centre feather";
    fs.body = kInsetFeatherBody;
    fs.input = PL_SHADER_SIG_COLOR;
    fs.output = PL_SHADER_SIG_COLOR;
    fs.variables = fvar;
    fs.num_variables = 4;
    if (!pl_shader_custom(sh, &fs)) {
        pl_dispatch_abort(m_Dp, &sh);
        return false;
    }
    pl_dispatch_params dpar = {};
    dpar.shader = &sh;
    dpar.target = swTex;
    dpar.rect = {0, top, 2 * iw, top + ih};
    if (!m_LoggedInset) {
        m_LoggedInset = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-FOVEA] centre layers: %dx%d per eye at y=%d (u0=%.3f a=%.3f) under a %dx%d periphery",
                    iw, ih, top, static_cast<double>(u0), static_cast<double>(fv[0]), m_Sw.width, top);
    }
    return pl_dispatch_finish(m_Dp, &dpar);
}

// §VR-FOVEA-TILES：見 kTilesBody／kTilesMaskBody。只處理「亮度一個平面＋CbCr 交錯一個平面」（同 renderUnwarpYuv），
// 其他排法回 false，呼叫端改用 pl_render_image（畫面一樣對，只是注視點那塊沒有漸層）
bool XrVideo::renderTiles(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int tw, int th, const char** why)
{
    if (m_Dp == nullptr) {
        m_Dp = pl_dispatch_create(m_Log, m_Vulkan->gpu);  // 沒開合成時 init() 不會建
        if (m_Dp != nullptr && m_Config.passTimers) {
            pl_dispatch_callback(m_Dp, this, &XrVideo::passInfoCb);  // §VR-PASS
        }
    }
    if (m_Dp == nullptr) {
        *why = "no dispatch";
        return false;
    }
    if (mapped->num_planes != 2 || mapped->planes[0].texture == nullptr || mapped->planes[1].texture == nullptr) {
        *why = "not two planes";
        return false;
    }
    const pl_plane& py = mapped->planes[0];
    const pl_plane& pc = mapped->planes[1];
    if (py.components != 1 || py.component_mapping[0] != PL_CHANNEL_Y || pc.components != 2 ||
        pc.component_mapping[0] != PL_CHANNEL_CB || pc.component_mapping[1] != PL_CHANNEL_CR) {
        *why = "plane layout is not Y + CbCr";
        return false;
    }
    if (!pl_color_system_is_ycbcr_like(mapped->repr.sys)) {
        *why = "colour system is not YCbCr";
        return false;
    }
    const pl_tex ty = py.texture;
    const pl_tex tc = pc.texture;
    if (!(ty->params.format->caps & PL_FMT_CAP_LINEAR) || !(tc->params.format->caps & PL_FMT_CAP_LINEAR) ||
        !ty->params.sampleable || !tc->params.sampleable) {
        *why = "planes are not linearly sampleable";
        return false;
    }
    const float yw = static_cast<float>(ty->params.w);
    const float yh = static_cast<float>(ty->params.h);
    const float cw = static_cast<float>(tc->params.w);
    const float ch = static_cast<float>(tc->params.h);
    float crop[4] = {mapped->crop.x0, mapped->crop.y0, mapped->crop.x1 - mapped->crop.x0, mapped->crop.y1 - mapped->crop.y0};
    if (crop[2] <= 0.0f || crop[3] <= 0.0f) {
        crop[0] = 0.0f;
        crop[1] = 0.0f;
        crop[2] = yw;
        crop[3] = yh;
    }
    float ysz[2] = {yw, yh};
    float csz[2] = {cw, ch};
    float rr[2] = {std::round(yw / cw), std::round(yh / ch)};
    float csh[2] = {pc.shift_x, pc.shift_y};
    float tile[2] = {static_cast<float>(tw), static_cast<float>(th)};
    float enc = m_Mode == TargetMode::SrgbMutable ? 1.0f : 0.0f;
    if (!m_LoggedTiles) {
        m_LoggedTiles = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-FOVEA] tiles: %dx%d per tile, frame %dx%d -> swapchain %dx%d 1:1 (whole view on top, gaze tiles below with a rounded soft edge); "
                    "luma %dx%d chroma %dx%d crop %.0f,%.0f chroma shift %.2f,%.2f sys=%d levels=%d",
                    tw, th, static_cast<int>(crop[2]), static_cast<int>(crop[3]), m_Sw.width, m_Sw.height, ty->params.w, ty->params.h,
                    tc->params.w, tc->params.h, crop[0], crop[1], csh[0], csh[1], static_cast<int>(mapped->repr.sys),
                    static_cast<int>(mapped->repr.levels));
    }

    pl_shader sh = pl_dispatch_begin(m_Dp);
    pl_shader_desc dd[2] = {sampledTex("vs_y", ty), sampledTex("vs_uv", tc)};
    pl_shader_var vv[5] = {};
    vv[0].var = pl_var_vec4("vs_crop");
    vv[0].data = crop;
    vv[1].var = pl_var_vec2("vs_ysz");
    vv[1].data = ysz;
    vv[2].var = pl_var_vec2("vs_csz");
    vv[2].data = csz;
    vv[3].var = pl_var_vec2("vs_rr");
    vv[3].data = rr;
    vv[4].var = pl_var_vec2("vs_csh");
    vv[4].data = csh;
    for (pl_shader_var& v : vv) {
        v.dynamic = true;
    }
    pl_custom_shader cs = {};
    cs.description = "viple vr fovea unwarp tiles";  // PassStats 照 "fovea unwarp" 歸到 unwarp（沿用同一格，方便和之前的數字比）
    cs.body = kTilesBody;
    cs.input = PL_SHADER_SIG_NONE;
    cs.output = PL_SHADER_SIG_COLOR;
    cs.descriptors = dd;
    cs.num_descriptors = 2;
    cs.variables = vv;
    cs.num_variables = 5;
    if (!pl_shader_custom(sh, &cs)) {
        pl_dispatch_abort(m_Dp, &sh);
        *why = "custom shader rejected";
        return false;
    }
    // 和 pl_render_image 同一套：YCbCr（含 limited range、位元深度）→ RGB，再從影像的色彩空間轉到 swapchain 的
    pl_color_repr repr = mapped->repr;
    repr.alpha = PL_ALPHA_NONE;
    pl_shader_decode_color(sh, &repr, nullptr);
    pl_color_space src = mapped->color;
    pl_color_space dst = targetProto->color;
    pl_color_space_infer_map(&src, &dst);
    pl_shader_color_map(sh, nullptr, src, dst, nullptr, false);
    pl_shader_var mv[2] = {};
    mv[0].var = pl_var_vec2("vs_tile");
    mv[0].data = tile;
    mv[1].var = pl_var_float("vs_enc");
    mv[1].data = &enc;
    for (pl_shader_var& v : mv) {
        v.dynamic = true;
    }
    pl_custom_shader ms = {};
    ms.description = "viple vr fovea tiles mask";
    ms.body = kTilesMaskBody;
    ms.input = PL_SHADER_SIG_COLOR;
    ms.output = PL_SHADER_SIG_COLOR;
    ms.variables = mv;
    ms.num_variables = 2;
    if (!pl_shader_custom(sh, &ms)) {
        pl_dispatch_abort(m_Dp, &sh);
        *why = "mask shader rejected";
        return false;
    }
    pl_dispatch_params dpar = {};
    dpar.shader = &sh;
    dpar.target = swTex;
    dpar.rect = {0, 0, m_Sw.width, m_Sw.height};
    if (!pl_dispatch_finish(m_Dp, &dpar)) {
        *why = "dispatch failed";
        return false;
    }
    return true;
}

bool XrVideo::renderUnwarp(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int fw, int fh,
                           bool hasMeta, const VIPLE_VR_FRAME_META& meta)
{
    bool ok = false;
    if (m_Dp == nullptr) {
        m_Dp = pl_dispatch_create(m_Log, m_Vulkan->gpu);  // 沒開合成時 init() 不會建
        if (m_Dp != nullptr && m_Config.passTimers) {
            pl_dispatch_callback(m_Dp, this, &XrVideo::passInfoCb);  // §VR-PASS
        }
    }
    if (m_Dp != nullptr && m_Config.unwarpYuv && !m_UnwarpYuvFailed) {
        // §VR-UNWARP-YUV：成功就不需要 m_Full；失敗（格式不合或 shader 失敗）整場改回原本的兩趟
        const char* why = nullptr;
        if (renderUnwarpYuv(mapped, targetProto, swTex, hasMeta, meta, &why)) {
            if (!m_LoggedUnwarpYuv) {
                m_LoggedUnwarpYuv = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-FOVEA] unwarp reads the decoded YUV planes directly (no RGB intermediate)");
            }
            return true;
        }
        m_UnwarpYuvFailed = true;
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-FOVEA] direct YUV unwarp not used (%s) - converting to RGB first as before", why ? why : "?");
    }
    if (m_Dp != nullptr && ensureFull(fw, fh)) {
        pl_frame tFull = *targetProto;
        tFull.planes[0].texture = m_Full;
        // 2026-10-10：targetProto 的 crop 是 swapchain 上的範圍——開著合成（§VR-SYNTH）時是兩倍高影像的上半，比 m_Full
        // 大。帶到中間這一張，libplacebo 會把目的範圍夾在 m_Full 之內、來源跟著裁：只畫到原始影像左上角的一部分
        // （每眼 1152、注視點 150% 時是左上 2/3）再放大，右眼看到的大半是左眼的內容。第二次發布的那一張上半是
        // renderSynth 重畫的（對的），所以只有「第一次顯示時第二次發布還沒完成」的幀會顯示到這一張：配戴實測佔串流幀
        // 的 16～32%，每秒十幾格整張錯位的畫面。歸零＝畫滿整張 m_Full。
        tFull.crop.x0 = 0.0f;
        tFull.crop.y0 = 0.0f;
        tFull.crop.x1 = 0.0f;
        tFull.crop.y1 = 0.0f;
        if (!pl_render_image(m_Renderer, mapped, &tFull, &m_RenderParams)) {
            return false;
        }
        float fv[4];
        float fvCyR;
        float fvU0;
        loadFoveation(fv, &fvCyR, &fvU0);
        if (hasMeta && meta.foveated) {
            fv[1] = meta.foveaCentre[0][0];
            fv[2] = meta.foveaCentre[1][0];
            fv[3] = meta.foveaCentre[0][1];
            fvCyR = meta.foveaCentre[1][1];
        }
        // swapchain 可能是兩倍高（合成壞掉之後）：一張影像的高度由寬度照比例算
        float osz[2] = {static_cast<float>(m_Sw.width), static_cast<float>(m_Config.synth ? m_Sw.height / 2 : m_Sw.height)};
        pl_shader sh = pl_dispatch_begin(m_Dp);
        pl_shader_desc dd[1] = {sampledTex("vs_full", m_Full)};
        float fsz[2] = {static_cast<float>(fw), static_cast<float>(fh)};
        pl_shader_var vv[5] = {};
        vv[0].var = pl_var_vec2("vs_osz");
        vv[0].data = osz;
        vv[0].dynamic = true;
        vv[1].var = pl_var_vec4("vs_fv");
        vv[1].data = fv;
        vv[1].dynamic = true;
        vv[2].var = pl_var_float("vs_cyr");
        vv[2].data = &fvCyR;
        vv[2].dynamic = true;
        vv[3].var = pl_var_vec2("vs_fsz");  // §VR-UPSCALE
        vv[3].data = fsz;
        vv[3].dynamic = true;
        vv[4].var = pl_var_float("vs_fu");  // §VR-FOVEA-INSET
        vv[4].data = &fvU0;
        vv[4].dynamic = true;
        static const std::string s_UnwarpHeader = std::string(kFovHeader) + kCatromHeader;
        pl_custom_shader cs = {};
        cs.description = "viple vr fovea unwarp";
        cs.header = s_UnwarpHeader.c_str();
        cs.body = kUnwarpBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 1;
        cs.variables = vv;
        cs.num_variables = 5;
        if (pl_shader_custom(sh, &cs)) {
            pl_dispatch_params dpar = {};
            dpar.shader = &sh;
            dpar.target = swTex;
            ok = pl_dispatch_finish(m_Dp, &dpar);
        }
        else {
            pl_dispatch_abort(m_Dp, &sh);
        }
    }
    if (ok) {
        return true;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VR-FOVEA] unwarp pass failed - showing the stream without undoing the foveated encoding for this session");
    m_UnwarpFailed = true;
    return pl_render_image(m_Renderer, mapped, targetProto, &m_RenderParams);
}

// ── §VR-PLAYER（V2）：180°／360° ──

namespace {

// 每個輸出像素：這一眼的視線（view 空間的 tan → 世界方向，扣掉內容的正前方朝向）→ 經緯度 → 全景影像上的位置。
// vs_tan＝(tan 左, tan 右, tan 上, tan 下)（左、下是負的）；vs_src＝這一眼在全景影像裡的範圍（x0, y0, 寬, 高，0..1）；
// vs_mode.x＝1（180°）或 2（360°）、vs_mode.y＝1 時輸出要換成線性值（swapchain 不是 sRGB 格式）
// 等角立方體（EAC，YouTube 的 360°）：方向 → 六個面之一 → 面上的角度（等角）→ 3×2 排法裡的位置。排法與每個面的
// 轉向照 FFmpeg v360 的 eac（上排左、前、右；下排下、後、上，各轉 270／90／270 度）；座標換成 v360 的（y 朝下、z 朝前）。
// 2026-10-19 用 v360 轉出來的 EAC 再用這個公式轉回經緯圖驗過（和原圖每像素平均差 1.7／765）
const char* kEquiHeader = R"GLSL(
vec2 vs_eac(vec3 d) {
    vec3 v = vec3(d.x, -d.y, -d.z);
    float phi = atan(v.x, v.z);
    float th = asin(clamp(v.y, -1.0, 1.0));
    int dir = 0;
    float pn = phi;
    if (phi >= -0.78539816 && phi < 0.78539816) { dir = 0; pn = phi; }
    else if (phi >= -2.35619449 && phi < -0.78539816) { dir = 2; pn = phi + 1.57079633; }
    else if (phi >= 0.78539816 && phi < 2.35619449) { dir = 3; pn = phi - 1.57079633; }
    else { dir = 1; pn = phi + (phi > 0.0 ? -3.14159265 : 3.14159265); }
    float t = atan(cos(pn));
    if (th > t) { dir = 5; } else if (th < -t) { dir = 4; }
    float uf; float vf; float face; float rot;
    if (dir == 3) { uf = -v.z / v.x; vf = v.y / v.x; face = 2.0; rot = 0.0; }
    else if (dir == 2) { uf = -v.z / v.x; vf = -v.y / v.x; face = 0.0; rot = 0.0; }
    else if (dir == 4) { uf = -v.x / v.y; vf = -v.z / v.y; face = 5.0; rot = 270.0; }
    else if (dir == 5) { uf = v.x / v.y; vf = -v.z / v.y; face = 3.0; rot = 270.0; }
    else if (dir == 0) { uf = v.x / v.z; vf = v.y / v.z; face = 1.0; rot = 0.0; }
    else { uf = v.x / v.z; vf = -v.y / v.z; face = 4.0; rot = 90.0; }
    if (rot > 45.0 && rot < 135.0) { float tmp = uf; uf = -vf; vf = tmp; }
    else if (rot > 225.0) { float tmp = -uf; uf = vf; vf = tmp; }
    uf = 0.63661977 * atan(uf) + 0.5;
    vf = 0.63661977 * atan(vf) + 0.5;
    return vec2((uf + mod(face, 3.0)) / 3.0, vf * 0.5 + 0.5 * floor(face / 3.0));
}
)GLSL";

const char* kEquiBody = R"GLSL(
vec2 uv = (gl_FragCoord.xy - vs_off) / vs_esz;
vec3 dv = vec3(mix(vs_tan.x, vs_tan.y, uv.x), mix(vs_tan.z, vs_tan.w, uv.y), -1.0);
vec3 d = normalize(vs_rot * dv);
float lon = atan(d.x, -d.z);
float lat = asin(clamp(d.y, -1.0, 1.0));
float ex = vs_mode.x > 1.5 ? fract(0.5 + lon * 0.15915494) : 0.5 + lon * 0.31830989;
float ey = 0.5 - lat * 0.31830989;
vec2 e2 = vs_mode.x > 2.5 ? vs_eac(d) : vec2(ex, ey);
vec3 c = texture(vs_equi, vs_src.xy + e2 * vs_src.zw).rgb;
if (vs_mode.y > 0.5) {
    c = pow(c, vec3(2.2));
}
bool outside = vs_mode.x < 1.5 && abs(lon) > 1.5707964;
color = outside ? vec4(0.0, 0.0, 0.0, 1.0) : vec4(c, 1.0);
)GLSL";

}  // namespace

bool XrVideo::immersiveReady()
{
    std::lock_guard<std::mutex> lk(m_EquiMutex);
    return m_EquiReady >= 0;
}

bool XrVideo::renderEquiFrame(AVFrame* frame, uint64_t t0)
{
    pl_gpu g = m_Vulkan->gpu;
    int slot = 0;
    {
        // 寫另一張；frame thread 正在取樣那一張時等它一下（它每一格只取樣一次，很快）
        std::unique_lock<std::mutex> lk(m_EquiMutex);
        slot = m_EquiReady == 0 ? 1 : 0;
        m_EquiCv.wait_for(lk, std::chrono::milliseconds(50), [&] { return m_EquiInUse != slot; });
        if (m_EquiInUse == slot) {
            av_frame_free(&frame);
            return false;
        }
    }
    const auto caps = static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_LINEAR);
    pl_fmt fmt = pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, caps);
    pl_tex_params tp = {};
    tp.w = frame->width;
    tp.h = frame->height;
    tp.format = fmt;
    tp.sampleable = true;
    tp.renderable = true;
    std::unique_lock<std::mutex> submitLock(m_PlSubmitMutex);
    if (fmt == nullptr || !pl_tex_recreate(g, &m_Equi[slot], &tp)) {
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        return false;
    }
    pl_frame mapped;
    std::memset(&mapped, 0, sizeof(mapped));
    const bool mappedOk = mapFrame(frame, &mapped, m_Tex, true);
    bool ok = false;
    if (mappedOk) {
        // 存 sRGB 編碼的值（和平面模式寫進 swapchain 的值相同），frame thread 取樣時視需要再換成線性
        pl_frame target;
        std::memset(&target, 0, sizeof(target));
        target.num_planes = 1;
        target.planes[0].texture = m_Equi[slot];
        target.planes[0].components = 4;
        target.planes[0].component_mapping[0] = PL_CHANNEL_R;
        target.planes[0].component_mapping[1] = PL_CHANNEL_G;
        target.planes[0].component_mapping[2] = PL_CHANNEL_B;
        target.planes[0].component_mapping[3] = PL_CHANNEL_A;
        target.repr = pl_color_repr_rgb;
        target.color = pl_color_space_srgb;
        ok = pl_render_image(m_Renderer, &mapped, &target, &m_RenderParams);
        if (!ok) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] equirect: pl_render_image failed");
        }
    }
    const uint64_t tCpu = nowUs();
    // GPU 做完才發布、才放掉解碼幀（有上限：GPU 卡住時不要永遠等）
    pl_gpu_flush(g);
    submitLock.unlock();
    // 輪詢也會回收 libplacebo 的 command buffer：在鎖內只查不等（0 逾時），等待放在鎖外，frame thread 不會被擋
    bool busy = true;
    const uint64_t pollEnd = nowUs() + 1000000ull;
    for (;;) {
        {
            std::lock_guard<std::mutex> pl(m_PlSubmitMutex);
            busy = pl_tex_poll(g, m_Equi[slot], 0);
        }
        if (!busy || nowUs() >= pollEnd) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    if (busy) {
        m_WedgedFrames.push_back(frame);
        m_Ctx->markGpuWedged("equirect render did not finish within 1 s");
        markUnusable("GPU wedged");
        return false;
    }
    if (mappedOk) {
        std::lock_guard<std::mutex> ul(m_PlSubmitMutex);
        pl_unmap_avframe(g, &mapped);
    }
    av_frame_free(&frame);
    const uint64_t tEnd = nowUs();
    if (ok) {
        std::lock_guard<std::mutex> lk(m_EquiMutex);
        m_EquiReady = slot;
    }
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    if (ok) {
        m_Drawn++;
        m_CpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tCpu - t0, 0xffffffffu)));
        m_GpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tEnd - tCpu, 0xffffffffu)));
    }
    else {
        m_RenderErrors++;
    }
    return ok;
}

bool XrVideo::ensureProjSwapchain(XrSession session, int width, int height)
{
    if (m_Proj.swapchain != XR_NULL_HANDLE && m_Proj.width == width && m_Proj.height == height) {
        return true;
    }
    destroySwapchainSet(m_Proj);
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    xrEnumerateSwapchainFormats(session, n, &n, formats.data());
    struct Cand {
        VkFormat sw;
        VkFormat view;
        bool linear;
    };
    // 同影像 swapchain 的順序：sRGB 格式用 UNORM 的 view 寫 sRGB 編碼的值；只有線性格式時把值換回線性
    const Cand cands[] = {
        {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, false},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, false},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, true},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, true},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, true},
    };
    const VkImageUsageFlags vkUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (const Cand& c : cands) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(c.sw)) == formats.end()) {
            continue;
        }
        XrSwapchainCreateInfo ci = xrS<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        if (c.sw != c.view) {
            ci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        }
        ci.format = c.sw;
        ci.sampleCount = 1;
        ci.width = static_cast<uint32_t>(width);
        ci.height = static_cast<uint32_t>(height);
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrSwapchain sc = XR_NULL_HANDLE;
        if (XR_FAILED(xrCreateSwapchain(session, &ci, &sc))) {
            continue;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages(sc, 0, &count, nullptr);
        std::vector<XrSwapchainImageVulkan2KHR> imgs(count, xrS<XrSwapchainImageVulkan2KHR>(XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR));
        if (count == 0 ||
            XR_FAILED(xrEnumerateSwapchainImages(sc, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) {
            xrDestroySwapchain(sc);
            continue;
        }
        std::vector<pl_tex> wrapped;
        bool wrapOk = true;
        for (uint32_t i = 0; i < count; i++) {
            pl_vulkan_wrap_params wp = {};
            wp.image = imgs[i].image;
            wp.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            wp.width = width;
            wp.height = height;
            wp.format = c.view;
            wp.usage = vkUsage;
            wp.debug_tag = PL_DEBUG_TAG;
            pl_tex t = pl_vulkan_wrap(m_Vulkan->gpu, &wp);
            if (t == nullptr || !t->params.renderable) {
                if (t != nullptr) {
                    pl_tex_destroy(m_Vulkan->gpu, &t);
                }
                wrapOk = false;
                break;
            }
            wrapped.push_back(t);
        }
        if (!wrapOk) {
            for (pl_tex& t : wrapped) {
                pl_tex_destroy(m_Vulkan->gpu, &t);
            }
            xrDestroySwapchain(sc);
            continue;
        }
        m_Proj.swapchain = sc;
        m_Proj.width = width;
        m_Proj.height = height;
        m_Proj.wrapped = std::move(wrapped);
        m_ProjLinear = c.linear;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] immersive swapchain %dx%d fmt=%d linear=%d images=%u", width, height,
                    static_cast<int>(c.sw), c.linear ? 1 : 0, count);
        return true;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] no usable immersive swapchain format for %dx%d", width, height);
    return false;
}

bool XrVideo::renderImmersive(XrSession session, const XrView views[2], int proj, int stereo, float yawRad, float eyeOffsetRad, int eyeW,
                              int eyeH, XrSwapchainSubImage out[2])
{
    if (m_ProjFailed || m_Vulkan == nullptr) {
        return false;
    }
    pl_gpu g = m_Vulkan->gpu;
    if (m_DpFrame == nullptr) {
        m_DpFrame = pl_dispatch_create(m_Log, g);
        pl_vulkan_sem_params sp = {};
        sp.type = VK_SEMAPHORE_TYPE_TIMELINE;
        sp.initial_value = 0;
        m_ProjSem = pl_vulkan_sem_create(g, &sp);
        if (m_DpFrame == nullptr || m_ProjSem == VK_NULL_HANDLE) {
            m_ProjFailed = true;
            return false;
        }
    }
    if (!ensureProjSwapchain(session, eyeW * 2, eyeH)) {
        m_ProjFailed = true;
        return false;
    }
    int slot = -1;
    {
        std::lock_guard<std::mutex> lk(m_EquiMutex);
        if (m_EquiReady < 0 || m_Equi[m_EquiReady] == nullptr) {
            return false;
        }
        slot = m_EquiReady;
        m_EquiInUse = slot;
    }
    struct InUse {
        XrVideo* v;
        ~InUse()
        {
            if (v == nullptr) {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(v->m_EquiMutex);
                v->m_EquiInUse = -1;
            }
            v->m_EquiCv.notify_all();
        }
    } inUse{this};

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = xrS<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    {
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        if (XR_FAILED(xrAcquireSwapchainImage(m_Proj.swapchain, &ai, &idx))) {
            return false;
        }
        if (idx >= m_Proj.wrapped.size()) {
            XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            xrReleaseSwapchainImage(m_Proj.swapchain, &ri);
            return false;
        }
    }
    XrSwapchainImageWaitInfo wi = xrS<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wi.timeout = 50000000;  // 50 ms
    xrWaitSwapchainImage(m_Proj.swapchain, &wi);
    std::unique_lock<std::mutex> submitLock(m_PlSubmitMutex);
    pl_tex dst = m_Proj.wrapped[idx];
    pl_vulkan_release_params rp = {};
    rp.tex = dst;
    rp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rp.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(g, &rp);

    bool ok = true;
    // 一隻眼：畫進 target 的 [x0, x0+eyeW) 那一段（dev 讀回時畫進另一張可讀回的材質）
    auto drawEye = [&](int e, pl_tex target, int x0, bool linear) -> bool {
        // 遠近：左眼的內容往左轉、右眼往右轉（加一點不交叉的視差，景物往遠處退；使用者 2026-10-19：VR180 的片太近、要鬥雞眼）
        const float eyeYaw = yawRad + (e == 0 ? eyeOffsetRad : -eyeOffsetRad);
        const float cy = std::cos(-eyeYaw), sy = std::sin(-eyeYaw);
        // 這一眼的朝向（四元數 → 矩陣），再繞 Y 轉 −yaw：內容的正前方對到置中時的頭部朝向
        const XrQuaternionf& q = views[e].pose.orientation;
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        const float r[3][3] = {{1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
                               {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
                               {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)}};
        // Ry(-yaw)：x' = c·x + s·z、z' = −s·x + c·z
        float m[3][3];
        for (int col = 0; col < 3; col++) {
            m[0][col] = cy * r[0][col] + sy * r[2][col];
            m[1][col] = r[1][col];
            m[2][col] = -sy * r[0][col] + cy * r[2][col];
        }
        float rot[9];  // GLSL mat3 是 column-major
        for (int col = 0; col < 3; col++) {
            for (int row = 0; row < 3; row++) {
                rot[col * 3 + row] = m[row][col];
            }
        }
        const XrFovf& f = views[e].fov;
        float tanv[4] = {std::tan(f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(f.angleDown)};
        float src[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        if (stereo == 1) {
            src[0] = e == 0 ? 0.0f : 0.5f;
            src[2] = 0.5f;
        }
        else if (stereo == 2) {
            src[1] = e == 0 ? 0.0f : 0.5f;
            src[3] = 0.5f;
        }
        float off[2] = {static_cast<float>(x0), 0.0f};
        float esz[2] = {static_cast<float>(eyeW), static_cast<float>(eyeH)};
        float mode[2] = {static_cast<float>(proj), linear ? 1.0f : 0.0f};
        pl_shader sh = pl_dispatch_begin(m_DpFrame);
        pl_shader_desc dd[1] = {sampledTex("vs_equi", m_Equi[slot])};
        pl_shader_var vv[6] = {};
        vv[0].var = pl_var_mat3("vs_rot");
        vv[0].data = rot;
        vv[0].dynamic = true;
        vv[1].var = pl_var_vec4("vs_tan");
        vv[1].data = tanv;
        vv[1].dynamic = true;
        vv[2].var = pl_var_vec4("vs_src");
        vv[2].data = src;
        vv[2].dynamic = true;
        vv[3].var = pl_var_vec2("vs_off");
        vv[3].data = off;
        vv[3].dynamic = true;
        vv[4].var = pl_var_vec2("vs_esz");
        vv[4].data = esz;
        vv[4].dynamic = true;
        vv[5].var = pl_var_vec2("vs_mode");
        vv[5].data = mode;
        vv[5].dynamic = true;
        pl_custom_shader cs = {};
        cs.description = "viple player equirect";
        cs.header = kEquiHeader;
        cs.body = kEquiBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 1;
        cs.variables = vv;
        cs.num_variables = 6;
        if (!pl_shader_custom(sh, &cs)) {
            pl_dispatch_abort(m_DpFrame, &sh);
            return false;
        }
        pl_dispatch_params dpar = {};
        dpar.shader = &sh;
        dpar.target = target;
        dpar.rect.x0 = x0;
        dpar.rect.y0 = 0;
        dpar.rect.x1 = x0 + eyeW;
        dpar.rect.y1 = eyeH;
        return pl_dispatch_finish(m_DpFrame, &dpar);
    };
    for (int e = 0; e < 2 && ok; e++) {
        ok = drawEye(e, dst, e * eyeW, m_ProjLinear);
    }
    // dev（--xr-dump-frame）：第 600 格把左眼另外畫一張可讀回的存成 PNG（swapchain 影像讀不回來）。只做一次，那一格會卡一下
    if (ok && !m_Config.dumpPath.isEmpty() && ++m_ImmFrames == 600) {
        pl_tex_params tp = {};
        tp.w = eyeW;
        tp.h = eyeH;
        tp.format = pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, static_cast<pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_HOST_READABLE));
        tp.renderable = true;
        tp.host_readable = true;
        pl_tex dumpTex = tp.format != nullptr ? pl_tex_create(g, &tp) : nullptr;
        if (dumpTex != nullptr && drawEye(0, dumpTex, 0, false)) {
            QString path = m_Config.dumpPath;
            const int dot = path.lastIndexOf(QLatin1Char('.'));
            path.insert(dot > path.lastIndexOf(QLatin1Char('/')) ? dot : path.size(), QStringLiteral("-immersive"));
            saveDump(readbackTexture(dumpTex), path, eyeW, eyeH);
        }
        pl_tex_destroy(g, &dumpTex);
    }
    // 還給 runtime 之前：交回影像（layout 回到 COLOR_ATTACHMENT_OPTIMAL，GPU 工作在這裡送出）。不等 GPU 做完：
    // runtime 會和 session 用的同一條 queue 同步（OpenXR 的 Vulkan 綁定；範例程式也是送出就 release）。2026-10-19 頭盔：
    // 原本每一格等做完，frame thread 一格要 6.5 ms、漏幀 1.3%（GPU 同時在跑合成器，等的大多是排隊）。
    // 全景材質不必等：render thread 之後寫它的工作排在這一批後面（同一條 queue，libplacebo 會插 barrier）
    pl_vulkan_hold_params hp = {};
    hp.tex = dst;
    hp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hp.qf = VK_QUEUE_FAMILY_IGNORED;
    hp.semaphore.sem = m_ProjSem;
    hp.semaphore.value = ++m_ProjSemValue;
    if (!pl_vulkan_hold_ex(g, &hp)) {
        pl_gpu_finish(g);
    }
    submitLock.unlock();
    {
        XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        xrReleaseSwapchainImage(m_Proj.swapchain, &ri);
    }
    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] immersive shader failed - 180/360 view disabled for this session");
        m_ProjFailed = true;
        return false;
    }
    for (int e = 0; e < 2; e++) {
        out[e].swapchain = m_Proj.swapchain;
        out[e].imageRect.offset = {e * eyeW, 0};
        out[e].imageRect.extent = {eyeW, eyeH};
        out[e].imageArrayIndex = 0;
    }
    return true;
}

void XrVideo::frameEnded(uint64_t generation)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    if (generation > m_XrAckGeneration) {
        m_XrAckGeneration = generation;
    }
}

std::function<std::string()> XrVideo::takeStatsDeferred()
{
    // §VR-LOGASYNC（2026-10-18）：原本整行在 frame thread 算（頭盔上約 200～250 µs，而且三次排序是在 m_StatsMutex 裡做的，
    // decoder 與影像 render thread 也跟著等）。改成鎖裡只把這個視窗的資料整包換出來，排序與組字串在回傳的函式裡做，
    // XrContext 交給 log 執行緒呼叫。換進來的是事先配好空間的新容器，render thread 往裡面推不必配置記憶體
    struct Snap {
        uint64_t received = 0, drawn = 0, overwritten = 0, renderErrors = 0, stallDropped = 0, noMetaDropped = 0;
        uint64_t synthDrawn = 0, synthShown = 0;
        bool synth = false, synthFailed = false, jitter = false;
        const char* mode = "";
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> cpu, gpu, synthGpu, jit;
        RenderDiag diag;
    };
    auto snap = std::make_shared<Snap>();
    for (std::vector<uint32_t>* v : {&snap->cpu, &snap->gpu, &snap->synthGpu, &snap->jit}) {
        v->reserve(2048);
    }
    // 換進 m_Diag 的那一包（鎖外先配好；分位數的緩衝區 rank 不在這裡配，到了算的時候才配，不在 frame thread 清 64 KB）
    for (std::vector<uint32_t>* v : {&snap->diag.mboxUs, &snap->diag.swWaitUs, &snap->diag.subPhaseUs, &snap->diag.donePhaseUs,
                                     &snap->diag.synthSubPhaseUs, &snap->diag.synthDonePhaseUs}) {
        v->reserve(2048);
    }
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        snap->received = m_Received;
        snap->drawn = m_Drawn;
        snap->overwritten = m_Overwritten;
        snap->renderErrors = m_RenderErrors;
        snap->stallDropped = m_TestStallDropped;
        snap->noMetaDropped = m_NoMetaDropped;
        snap->mode = modeName(static_cast<int>(m_Mode));
        snap->w = m_Sw.width;
        snap->h = m_Sw.height;
        snap->synth = m_Config.synth;
        snap->jitter = m_Jitter.enabled();
        snap->cpu.swap(m_CpuUs);
        snap->gpu.swap(m_GpuUs);
        if (snap->synth) {
            // §VR-SYNTH：這個視窗合成了幾張、顯示了幾次合成的那一格
            snap->synthDrawn = m_SynthDrawn;
            snap->synthShown = m_SynthShown.exchange(0, std::memory_order_relaxed);
            snap->synthFailed = m_SynthFailed;
            snap->synthGpu.swap(m_SynthGpuUs);
            m_SynthDrawn = 0;
        }
        if (snap->jitter) {
            snap->jit.swap(m_JitUs);
        }
        m_Received = m_Drawn = m_Overwritten = m_RenderErrors = 0;
        std::swap(m_Diag, snap->diag);  // 新的那一包已經配好空間；換出來的這一包交給回傳的函式
    }
    return [snap]() -> std::string {
        auto pct = [](std::vector<uint32_t>& v, double* p50, double* p95) {
            *p50 = *p95 = 0.0;
            if (v.empty()) {
                return;
            }
            std::sort(v.begin(), v.end());
            *p50 = v[v.size() / 2] / 1000.0;
            *p95 = v[std::min(v.size() - 1, (v.size() * 95) / 100)] / 1000.0;
        };
        Snap& d = *snap;
        double c50, c95, g50, g95, s50, s95;
        pct(d.cpu, &c50, &c95);
        pct(d.gpu, &g50, &g95);
        pct(d.synthGpu, &s50, &s95);
        QString line = QStringLiteral("recv=%1 drawn=%2 overwritten=%3 errors=%4 renderThread cpu p50=%5 p95=%6 ms "
                                      "gpu p50=%7 p95=%8 ms mode=%9 %10x%11 stallDropped=%12 noMetaDropped=%13")
                           .arg(d.received).arg(d.drawn).arg(d.overwritten).arg(d.renderErrors)
                           .arg(c50, 0, 'f', 2).arg(c95, 0, 'f', 2).arg(g50, 0, 'f', 2).arg(g95, 0, 'f', 2)
                           .arg(QLatin1String(d.mode))
                           .arg(d.w).arg(d.h).arg(d.stallDropped).arg(d.noMetaDropped);
        if (d.synth) {
            // 第五版起真的那一張先發布：上面的 gpu 是真的那一張，synthGpu 是之後合成那一張多花的時間
            line += QStringLiteral(" synth drawn=%1 shown=%2 synthGpu p50=%3 p95=%4 ms%5").arg(d.synthDrawn)
                        .arg(d.synthShown)
                        .arg(s50, 0, 'f', 2).arg(s95, 0, 'f', 2)
                        .arg(d.synthFailed ? QStringLiteral(" (FAILED: off)") : QString());
        }
        if (d.jitter) {
            // dev（--vr-test-jitter）：這個視窗有幾張被延後、延後時間的 p95
            double j50, j95;
            const size_t n = d.jit.size();
            pct(d.jit, &j50, &j95);
            line += QStringLiteral(" jit n=%1 p95=%2 ms").arg(n).arg(j95, 0, 'f', 2);
        }
        d.diag.rank.prepare();  // 已經配過的話不會再配
        return line.toStdString() + d.diag.statsTail(d.synth);
    };
}
