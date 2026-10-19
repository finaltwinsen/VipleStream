// VipleStream 2.0 §VR M3a X2 — XrVideo 實作。設計見 xrvideo.h。

#include "xrvideo.h"

#include "xrphase.h"  // §VR-XRLOOP：相位換算（header-only）
#include "streaming/video/ffmpeg-renderers/plvk_common.h"
#include "streaming/vr/vrframemeta.h"

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
}

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#include <libplacebo/dispatch.h>
#include <libplacebo/shaders/custom.h>

namespace {

void lockQueueCb(void* ctx, uint32_t, uint32_t)
{
    static_cast<XrContext*>(ctx)->queueMutex().lock();
}

void unlockQueueCb(void* ctx, uint32_t, uint32_t)
{
    static_cast<XrContext*>(ctx)->queueMutex().unlock();
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
                "[VIPLE-XR] video: libplacebo imported the XR VkDevice (api %u.%u, %d device exts)",
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
    if (gpuWedged) {
        // 所有 libplacebo 釋放都會等 GPU：只收掉 XR swapchain handle 與沒被 GPU 參照的 mailbox 幀，其餘洩漏
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] video: GPU wedged - leaking libplacebo objects and %d in-flight frame(s)",
                    static_cast<int>(m_WedgedFrames.size()));
        for (SwapchainSet* s : {&m_Sw}) {
            if (s->swapchain != XR_NULL_HANDLE) {
                xrDestroySwapchain(s->swapchain);
                s->swapchain = XR_NULL_HANDLE;
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
        m_Small[0] = m_Small[1] = nullptr;
        m_Mid[0] = m_Mid[1] = m_FlowC = nullptr;
        m_Sem = VK_NULL_HANDLE;
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
    m_Sem = VK_NULL_HANDLE;
    pl_log_destroy(&m_Log);
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
    loadFoveation(fv, &fvCyR);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] synth input dump: %s small=%dx%d(%s) flow=%dx%d(%s, %d bytes/texel) fv=(%.4f,%.4f,%.4f,%.4f,%.4f)",
                qUtf8Printable(flowPath), smalls[1]->params.w, smalls[1]->params.h, smalls[1]->params.format->name,
                flow->params.w, flow->params.h, flow->params.format->name, static_cast<int>(flow->params.format->texel_size),
                fv[0], fv[1], fv[2], fv[3], fvCyR);
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
            pl_gpu_finish(g);
            return 0;
        }
        submitUs = nowUs();
        if (flight) {
            m_FlightSubmitUs.store(submitUs);
            m_FlightSeq.store(m_Published.seq + 1);
        }
        if (!m_Ctx->waitSemaphoreBounded(m_Sem, hp.semaphore.value, 1000000000ull)) {
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
        return m_ReleaseNo.fetch_add(1, std::memory_order_relaxed) + 1;
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
    const int frameH = m_Config.synth ? m_Sw.height / 2 : m_Sw.height;  // swapchain 裡一張影像的高度
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
        if (m_Config.synth) {
            t1.crop.x0 = 0.0f;
            t1.crop.y0 = 0.0f;
            t1.crop.x1 = static_cast<float>(m_Sw.width);
            t1.crop.y1 = static_cast<float>(frameH);
        }
        if (fovea && !m_UnwarpFailed) {
            rendered = renderUnwarp(&mapped, &t1, tex, frame->width, frame->height);
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
        m_Published.generation = m_Sw.generation;
        m_Published.lastDrawnUs = tGpuEnd;
        m_Published.seq++;
        m_Published.renderUs = static_cast<uint32_t>(std::min<uint64_t>(tGpuEnd - t0, 0xffffffffu));
        m_Published.hasMeta = hasMeta;
        m_Published.meta = meta;
        m_HavePublished = true;
        m_Published.publishedUs = tPub;
        // §VR-LDIAG：每一張發布的影像記一筆（被跳過、沒顯示的也有），frame thread 用 drainPublished() 抄走
        m_PubRing[m_PubHead++ % kPubRing] = {m_Published.seq, pts90, t0, tGpuEnd, tPub, tSubmit};
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
    if (!m_HavePublished || m_Published.aspect <= 0.0f) {
        return false;
    }
    *out = m_Published;
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
    return true;
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
float dtx = vs_w.y * (1.0 + tx * tx) + vs_w.z * ty - vs_w.x * tx * ty;
float dty = -vs_w.x * (1.0 + ty * ty) - vs_w.z * tx + vs_w.y * tx * ty;
vec2 slope = vec2(vs_fv.x + 3.0 * fb * ux * ux, vs_fv.x + 3.0 * fb * uy * uy);
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
vec2 uv = vs_unwarp(vec2(fc.x, bottom ? fc.y - vs_osz.y : fc.y) / vs_osz, vs_fv, vs_cyr);
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
float vs_inv1(float x, float c, float a) {
    float ext = x < c ? c : 1.0 - c;
    float t = (x - c) / ext;
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
vec2 vs_unwarp(vec2 uv, vec4 fv, float cyR) {
    if (fv.x >= 0.999) { return uv; }
    bool right = uv.x >= 0.5;
    float ex = (right ? uv.x - 0.5 : uv.x) * 2.0;
    float x = vs_inv1(ex, right ? fv.z : fv.y, fv.x);
    float y = vs_inv1(uv.y, right ? cyR : fv.w, fv.x);
    return vec2((right ? 0.5 : 0.0) + 0.5 * x, y);
}
)GLSL";

const char* kUnwarpBody = R"GLSL(
color = texture(vs_full, vs_unwarp(gl_FragCoord.xy / vs_osz, vs_fv, vs_cyr));
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
    loadFoveation(fv, &fvCyR);
    float osz[2] = {ow, oh};
    pl_fmt fmtSmall = pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, caps);
    pl_fmt fmtFlow = pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, caps);
    const int sw = std::max(16, fw / 8), sh = std::max(16, fh / 8);
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

    pl_frame tFull = *targetProto;
    tFull.planes[0].texture = m_Full;
    // crop 是 swapchain 上的範圍，不能帶到中間這一張（見 renderUnwarp）：歸零＝畫滿整張 m_Full
    tFull.crop.x0 = 0.0f;
    tFull.crop.y0 = 0.0f;
    tFull.crop.x1 = 0.0f;
    tFull.crop.y1 = 0.0f;
    if (!pl_render_image(m_Renderer, mapped, &tFull, &m_RenderParams)) {
        return false;
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
    static int64_t s_nominalDelta = 0;
    const int64_t ptsDelta = pts - m_PrevPts;
    if (m_HavePrevMeta && ptsDelta > 0) {
        if (s_nominalDelta == 0) {
            s_nominalDelta = ptsDelta;
        }
        else if (ptsDelta < s_nominalDelta * 3) {
            s_nominalDelta = (s_nominalDelta * 7 + ptsDelta) / 8;
        }
    }
    const bool consecutive = ptsDelta > 0 && s_nominalDelta > 0 && ptsDelta * 10 <= s_nominalDelta * 16;
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
                    pl_shader_var gv[5] = {};
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
                    pl_custom_shader cs = {};
                    cs.description = "viple vr synth flow";
                    cs.header = kFlowHeader;
                    cs.body = kFlowBody;
                    cs.input = PL_SHADER_SIG_NONE;
                    cs.output = PL_SHADER_SIG_COLOR;
                    cs.descriptors = fd;
                    cs.num_descriptors = 3;
                    cs.variables = gv;
                    cs.num_variables = 5;
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
        pl_shader_var kv[4] = {};
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
        pl_custom_shader cs = {};
        cs.description = "viple vr synth combine";
        cs.header = kFovHeader;
        cs.body = kCombineBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = cd;
        cs.num_descriptors = 2;
        cs.variables = kv;
        cs.num_variables = 4;
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

void XrVideo::loadFoveation(float fv[4], float* cyR) const
{
    fv[0] = m_FvA.load(std::memory_order_relaxed);
    fv[1] = m_FvCxL.load(std::memory_order_relaxed);
    fv[2] = m_FvCxR.load(std::memory_order_relaxed);
    fv[3] = m_FvCyL.load(std::memory_order_relaxed);
    *cyR = m_FvCyR.load(std::memory_order_relaxed);
    if (!(fv[0] >= 0.4f && fv[0] < 0.999f)) {
        fv[0] = 1.0f;
    }
}

void XrVideo::outputSize(int fw, int fh, int* ow, int* oh) const
{
    *ow = fw;
    *oh = fh;
    const float a = m_FvA.load(std::memory_order_relaxed);
    if (a >= 0.4f && a < 0.999f && fw > 0 && fh > 0) {
        // 正前方的像素密度是 1/a 倍：swapchain 跟著放大才放得下那些細節。每眼最高 2304（頭盔面板每眼 2160）
        const float s = std::min(1.0f / a, 2.0f);
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
bool XrVideo::renderUnwarp(pl_frame* mapped, const pl_frame* targetProto, pl_tex swTex, int fw, int fh)
{
    bool ok = false;
    if (m_Dp == nullptr) {
        m_Dp = pl_dispatch_create(m_Log, m_Vulkan->gpu);  // 沒開合成時 init() 不會建
        if (m_Dp != nullptr && m_Config.passTimers) {
            pl_dispatch_callback(m_Dp, this, &XrVideo::passInfoCb);  // §VR-PASS
        }
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
        loadFoveation(fv, &fvCyR);
        // swapchain 可能是兩倍高（合成壞掉之後）：一張影像的高度由寬度照比例算
        float osz[2] = {static_cast<float>(m_Sw.width), static_cast<float>(m_Config.synth ? m_Sw.height / 2 : m_Sw.height)};
        pl_shader sh = pl_dispatch_begin(m_Dp);
        pl_shader_desc dd[1] = {sampledTex("vs_full", m_Full)};
        pl_shader_var vv[3] = {};
        vv[0].var = pl_var_vec2("vs_osz");
        vv[0].data = osz;
        vv[0].dynamic = true;
        vv[1].var = pl_var_vec4("vs_fv");
        vv[1].data = fv;
        vv[1].dynamic = true;
        vv[2].var = pl_var_float("vs_cyr");
        vv[2].data = &fvCyR;
        vv[2].dynamic = true;
        pl_custom_shader cs = {};
        cs.description = "viple vr fovea unwarp";
        cs.header = kFovHeader;
        cs.body = kUnwarpBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 1;
        cs.variables = vv;
        cs.num_variables = 3;
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
