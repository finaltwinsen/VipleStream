// VipleStream 2.0 §VR M3a X2 — XrVideo 實作。設計見 xrvideo.h。

#include "xrvideo.h"

#include "streaming/video/ffmpeg-renderers/plvk_common.h"
#include "streaming/vr/vrframemeta.h"

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

XrVideo::XrVideo(XrContext* ctx, const Config& config) : m_Ctx(ctx), m_Config(config) {}

XrVideo::~XrVideo()
{
    destroy();
}

pl_gpu XrVideo::gpu() const
{
    return m_Vulkan != nullptr ? m_Vulkan->gpu : nullptr;
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
    if (m_Config.synth) {
        m_Dp = pl_dispatch_create(m_Log, m_Vulkan->gpu);
        if (m_Dp == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-SYNTH] pl_dispatch_create failed - frame synthesis off");
            m_Config.synth = false;
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

void XrVideo::dumpTexture(pl_tex tex)
{
    if (m_Mode == TargetMode::Float16Linear || !tex->params.host_readable) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump skipped (mode=%s hostReadable=%d)",
                    modeName(static_cast<int>(m_Mode)), tex->params.host_readable ? 1 : 0);
        return;
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
        return;
    }
    const bool bgra = m_ViewFormat == VK_FORMAT_B8G8R8A8_UNORM;
    // Format_ARGB32 在 little-endian 記憶體中就是 B,G,R,A
    QImage img(buf.data(), w, h, w * 4, bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    QImage scaled = img.scaled(QSize(1280, 1280), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (scaled.save(m_Config.dumpPath, "PNG")) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump: %s (%dx%d from %dx%d, mode=%s)",
                    qUtf8Printable(m_Config.dumpPath), scaled.width(), scaled.height(), w, h,
                    modeName(static_cast<int>(m_Mode)));
    }
    else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] frame dump: saving %s failed", qUtf8Printable(m_Config.dumpPath));
    }
}

bool XrVideo::renderNewFrame(AVFrame* frame)
{
    const uint64_t t0 = nowUs();
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
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = xrS<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    XrResult r;
    {
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        r = xrAcquireSwapchainImage(m_Sw.swapchain, &ai, &idx);
    }
    if (XR_FAILED(r) || idx >= m_Sw.wrapped.size()) {
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        return false;
    }
    XrSwapchainImageWaitInfo wi = xrS<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wi.timeout = 100000000;  // 100 ms
    xrWaitSwapchainImage(m_Sw.swapchain, &wi);

    pl_gpu g = m_Vulkan->gpu;
    pl_tex tex = m_Sw.wrapped[idx];

    // OpenXR（Vulkan）：acquire 後彩色影像在 COLOR_ATTACHMENT_OPTIMAL，release 前也要回到這個 layout
    pl_vulkan_release_params rp = {};
    rp.tex = tex;
    rp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rp.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(g, &rp);

    pl_frame mapped;
    std::memset(&mapped, 0, sizeof(mapped));
    const bool mappedOk = mapFrame(frame, &mapped, m_Tex, true);
    bool rendered = false;
    bool synthOk = false;
    VIPLE_VR_FRAME_META synthMeta = {};
    const int frameH = m_Config.synth ? m_Sw.height / 2 : m_Sw.height;  // swapchain 裡一張影像的高度
    const bool fovea = m_FvA.load(std::memory_order_relaxed) < 0.999f;
    if (mappedOk) {
        pl_frame target;
        std::memset(&target, 0, sizeof(target));
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
        if (m_Config.synth && !m_SynthFailed) {
            rendered = renderSynth(&mapped, &target, tex, frame->width, frame->height, hasMeta, meta, frame->pts, &synthOk, &synthMeta);
        }
        else if (fovea && !m_UnwarpFailed) {
            rendered = renderUnwarp(&mapped, &target, tex, frame->width, frame->height);
        }
        else {
            if (m_Config.synth) {
                // 合成壞掉之後：swapchain 仍是兩倍高，真的那一張照樣只畫上半
                target.crop.x0 = 0.0f;
                target.crop.y0 = 0.0f;
                target.crop.x1 = static_cast<float>(m_Sw.width);
                target.crop.y1 = static_cast<float>(frameH);
            }
            rendered = pl_render_image(m_Renderer, &mapped, &target, &pl_render_fast_params);
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
    // §VR-SYNTH：開著合成時等到第 2400 張之後、而且這一張真的有合成影像才存（上半真的、下半合成）
    if (rendered && !m_Dumped && !m_Config.dumpPath.isEmpty() &&
        m_TotalDrawn >= static_cast<uint64_t>(m_Config.synth ? 2400 : std::max(1, m_Config.dumpAfterFrames)) &&
        (!m_Config.synth || synthOk || m_SynthFailed)) {
        m_Dumped = true;
        dumpTexture(tex);  // 必須在 hold 之前（download 需要 libplacebo 仍持有影像）
    }

    pl_vulkan_hold_params hp = {};
    hp.tex = tex;
    hp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    hp.qf = VK_QUEUE_FAMILY_IGNORED;
    hp.semaphore.sem = m_Sem;
    hp.semaphore.value = ++m_SemValue;
    const bool held = pl_vulkan_hold_ex(g, &hp);
    const uint64_t tCpuEnd = nowUs();
    // GPU 做完才 release 給 runtime、才放掉解碼幀。X3 起這裡是影像 render thread，不再阻塞 XR frame thread。
    // M4a 收尾：先以有上限的 timeline 等待確認 hold 的 semaphore 到了（pl_gpu_finish 沒有逾時；runtime 串流
    // 中死掉時 GPU 工作可能永遠等不到結果，R4 在 S1 卡了 7 分鐘），到了再 pl_gpu_finish（此時立即返回）。
    if (held && !m_Ctx->waitSemaphoreBounded(m_Sem, hp.semaphore.value, 1000000000ull)) {
        m_WedgedFrames.push_back(frame);  // 仍被 GPU 參照：不 unmap、不 free
        m_Ctx->markGpuWedged("video render did not finish within 1 s");
        markUnusable("GPU wedged");
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        return false;
    }
    pl_gpu_finish(g);
    const uint64_t tGpuEnd = nowUs();
    if (mappedOk) {
        pl_unmap_avframe(g, &mapped);
    }
    // hasMeta／meta 已在函式開頭查好，跟著這張影像一起發布
    av_frame_free(&frame);

    XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    uint64_t releaseNo = 0;
    {
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        xrReleaseSwapchainImage(m_Sw.swapchain, &ri);
        releaseNo = m_ReleaseNo.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (rendered && held) {
        std::lock_guard<std::mutex> lk(m_SwMutex);
        m_Published.releaseNo = releaseNo;
        m_Published.subImage.swapchain = m_Sw.swapchain;
        m_Published.subImage.imageRect.offset = {0, 0};
        m_Published.subImage.imageRect.extent = {m_Sw.width, frameH};
        m_Published.subImage.imageArrayIndex = 0;
        m_Published.aspect = static_cast<float>(m_Sw.width) / static_cast<float>(frameH);
        m_Published.hasSynth = synthOk;
        m_Published.synthRect.offset = {0, frameH};
        m_Published.synthRect.extent = {m_Sw.width, frameH};
        m_Published.synthMeta = synthMeta;
        m_Published.generation = m_Sw.generation;
        m_Published.lastDrawnUs = tGpuEnd;
        m_Published.seq++;
        m_Published.renderUs = static_cast<uint32_t>(std::min<uint64_t>(tGpuEnd - t0, 0xffffffffu));
        m_Published.hasMeta = hasMeta;
        m_Published.meta = meta;
        m_HavePublished = true;
    }

    std::lock_guard<std::mutex> lk(m_StatsMutex);
    if (rendered && held) {
        m_Drawn++;
        m_SynthDrawn += synthOk ? 1 : 0;
        m_CpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tCpuEnd - t0, 0xffffffffu)));
        m_GpuUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(tGpuEnd - tCpuEnd, 0xffffffffu)));
    }
    else {
        m_RenderErrors++;
    }
    return rendered && held;
}

void XrVideo::renderThreadMain()
{
    while (true) {
        AVFrame* f = nullptr;
        XrSession session = XR_NULL_HANDLE;
        {
            std::unique_lock<std::mutex> lk(m_MailboxMutex);
            m_MailboxCv.wait_for(lk, std::chrono::milliseconds(100),
                                 [this] { return m_StopThread || m_Pending != nullptr; });
            if (m_StopThread) {
                break;
            }
            f = m_Pending;
            m_Pending = nullptr;
            session = m_Session;
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
        int outW = f->width, outH = f->height;
        outputSize(f->width, f->height, &outW, &outH);
        if (session != XR_NULL_HANDLE && usable() && ensureSwapchain(session, outW, m_Config.synth ? outH * 2 : outH)) {
            renderNewFrame(f);
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

namespace {

// 位移估計。頭部轉動造成的整張影像位移 vs_g 由呼叫端從兩張的算圖姿態算好傳進來（那一部分交給頭盔的重投影處理），
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
vec2 g = vs_g * sz;
{
    // §VR-FOVEA：vs_g 是均勻取樣時的位移；壓縮過的影像裡，同樣的視角位移在每個位置佔的像素數是它除以那裡的斜率
    float ex = (uv.x < 0.5 ? uv.x : uv.x - 0.5) * 2.0;
    float cx = uv.x < 0.5 ? vs_fv.y : vs_fv.z;
    float tx = (ex - cx) / (ex < cx ? cx : 1.0 - cx);
    float ty = (uv.y - vs_fv.w) / (uv.y < vs_fv.w ? vs_fv.w : 1.0 - vs_fv.w);
    float b = 1.0 - vs_fv.x;
    g /= vec2(vs_fv.x + 3.0 * b * tx * tx, vs_fv.x + 3.0 * b * ty * ty);
}
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
vec2 uv = vs_unwarp(vec2(fc.x, bottom ? fc.y - vs_osz.y : fc.y) / vs_osz, vs_fv);
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
// (1−a)·e³ + a·e = t（Cardano，再做一次牛頓法修掉 float 的誤差）。fv＝(a, cxL, cxR, cy)；a ≥ 0.999 時原樣回傳。
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
vec2 vs_unwarp(vec2 uv, vec4 fv) {
    if (fv.x >= 0.999) { return uv; }
    bool right = uv.x >= 0.5;
    float ex = (right ? uv.x - 0.5 : uv.x) * 2.0;
    float x = vs_inv1(ex, right ? fv.z : fv.y, fv.x);
    float y = vs_inv1(uv.y, fv.w, fv.x);
    return vec2((right ? 0.5 : 0.0) + 0.5 * x, y);
}
)GLSL";

const char* kUnwarpBody = R"GLSL(
color = texture(vs_full, vs_unwarp(gl_FragCoord.xy / vs_osz, vs_fv));
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
    loadFoveation(fv);
    float osz[2] = {ow, oh};
    pl_fmt fmtSmall = pl_find_fmt(g, PL_FMT_UNORM, 4, 8, 8, caps);
    pl_fmt fmtFlow = pl_find_fmt(g, PL_FMT_FLOAT, 4, 16, 16, caps);
    const int sw = std::max(16, fw / 8), sh = std::max(16, fh / 8);
    auto mk = [g](pl_tex* t, int w, int h, pl_fmt f) {
        pl_tex_params tp = {};
        tp.w = w;
        tp.h = h;
        tp.format = f;
        tp.sampleable = true;
        tp.renderable = true;
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
        return pl_render_image(m_Renderer, mapped, &t, &pl_render_fast_params);
    }
    if (m_FrameH != fh) {
        m_FrameH = fh;
        m_HavePrevSmall = false;
        m_HavePrevMeta = false;
    }

    pl_frame tFull = *targetProto;
    tFull.planes[0].texture = m_Full;
    if (!pl_render_image(m_Renderer, mapped, &tFull, &pl_render_fast_params)) {
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
                float gvec[2] = {-0.5f * (2.0f * d[1]) / tanW, -(2.0f * d[0]) / tanH};
                // 兩層：1/16 尺寸半徑 6（起點 0）→ 1/8 尺寸半徑 3（起點＝粗層的結果）。vs_init 一定要綁一張材質：
                // 粗層不用它（vs_p.y＝0），綁 m_Flow（這個 pass 不寫它）
                auto runFlow = [&](pl_tex curT, pl_tex prevT, pl_tex initT, pl_tex target, float radius, float useInit) {
                    float pvec[2] = {radius, useInit};
                    pl_shader shf = pl_dispatch_begin(m_Dp);
                    pl_shader_desc fd[3] = {sampledTex("vs_cur", curT), sampledTex("vs_prev", prevT), sampledTex("vs_init", initT)};
                    pl_shader_var gv[3] = {};
                    gv[0].var = pl_var_vec2("vs_g");
                    gv[0].data = gvec;
                    gv[0].dynamic = true;
                    gv[1].var = pl_var_vec4("vs_fv");
                    gv[1].data = fv;
                    gv[1].dynamic = true;
                    gv[2].var = pl_var_vec2("vs_p");
                    gv[2].data = pvec;
                    gv[2].dynamic = true;
                    pl_custom_shader cs = {};
                    cs.description = "viple vr synth flow";
                    cs.header = kFlowHeader;
                    cs.body = kFlowBody;
                    cs.input = PL_SHADER_SIG_NONE;
                    cs.output = PL_SHADER_SIG_COLOR;
                    cs.descriptors = fd;
                    cs.num_descriptors = 3;
                    cs.variables = gv;
                    cs.num_variables = 3;
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
        pl_shader_var kv[3] = {};
        kv[0].var = pl_var_float("vs_k");
        kv[0].data = &k;
        kv[0].dynamic = true;
        kv[1].var = pl_var_vec2("vs_osz");
        kv[1].data = osz;
        kv[1].dynamic = true;
        kv[2].var = pl_var_vec4("vs_fv");
        kv[2].data = fv;
        kv[2].dynamic = true;
        pl_custom_shader cs = {};
        cs.description = "viple vr synth combine";
        cs.header = kFovHeader;
        cs.body = kCombineBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = cd;
        cs.num_descriptors = 2;
        cs.variables = kv;
        cs.num_variables = 3;
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
        return pl_render_image(m_Renderer, mapped, &t, &pl_render_fast_params);
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

void XrVideo::loadFoveation(float fv[4]) const
{
    fv[0] = m_FvA.load(std::memory_order_relaxed);
    fv[1] = m_FvCxL.load(std::memory_order_relaxed);
    fv[2] = m_FvCxR.load(std::memory_order_relaxed);
    fv[3] = m_FvCy.load(std::memory_order_relaxed);
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
    }
    if (m_Dp != nullptr && ensureFull(fw, fh)) {
        pl_frame tFull = *targetProto;
        tFull.planes[0].texture = m_Full;
        if (!pl_render_image(m_Renderer, mapped, &tFull, &pl_render_fast_params)) {
            return false;
        }
        float fv[4];
        loadFoveation(fv);
        // swapchain 可能是兩倍高（合成壞掉之後）：一張影像的高度由寬度照比例算
        float osz[2] = {static_cast<float>(m_Sw.width), static_cast<float>(m_Config.synth ? m_Sw.height / 2 : m_Sw.height)};
        pl_shader sh = pl_dispatch_begin(m_Dp);
        pl_shader_desc dd[1] = {sampledTex("vs_full", m_Full)};
        pl_shader_var vv[2] = {};
        vv[0].var = pl_var_vec2("vs_osz");
        vv[0].data = osz;
        vv[0].dynamic = true;
        vv[1].var = pl_var_vec4("vs_fv");
        vv[1].data = fv;
        vv[1].dynamic = true;
        pl_custom_shader cs = {};
        cs.description = "viple vr fovea unwarp";
        cs.header = kFovHeader;
        cs.body = kUnwarpBody;
        cs.input = PL_SHADER_SIG_NONE;
        cs.output = PL_SHADER_SIG_COLOR;
        cs.descriptors = dd;
        cs.num_descriptors = 1;
        cs.variables = vv;
        cs.num_variables = 2;
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
    return pl_render_image(m_Renderer, mapped, targetProto, &pl_render_fast_params);
}

void XrVideo::frameEnded(uint64_t generation)
{
    std::lock_guard<std::mutex> lk(m_SwMutex);
    if (generation > m_XrAckGeneration) {
        m_XrAckGeneration = generation;
    }
}

QString XrVideo::takeStatsLine()
{
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    auto pct = [](std::vector<uint32_t> v, double* p50, double* p95) {
        *p50 = *p95 = 0.0;
        if (v.empty()) {
            return;
        }
        std::sort(v.begin(), v.end());
        *p50 = v[v.size() / 2] / 1000.0;
        *p95 = v[std::min(v.size() - 1, (v.size() * 95) / 100)] / 1000.0;
    };
    double c50, c95, g50, g95;
    pct(m_CpuUs, &c50, &c95);
    pct(m_GpuUs, &g50, &g95);
    QString line = QStringLiteral("recv=%1 drawn=%2 overwritten=%3 errors=%4 renderThread cpu p50=%5 p95=%6 ms "
                                  "gpu p50=%7 p95=%8 ms mode=%9 %10x%11 stallDropped=%12 noMetaDropped=%13")
                       .arg(m_Received).arg(m_Drawn).arg(m_Overwritten).arg(m_RenderErrors)
                       .arg(c50, 0, 'f', 2).arg(c95, 0, 'f', 2).arg(g50, 0, 'f', 2).arg(g95, 0, 'f', 2)
                       .arg(QLatin1String(modeName(static_cast<int>(m_Mode))))
                       .arg(m_Sw.width).arg(m_Sw.height).arg(m_TestStallDropped).arg(m_NoMetaDropped);
    if (m_Config.synth) {
        // §VR-SYNTH：這個視窗合成了幾張、顯示了幾次合成的那一格
        line += QStringLiteral(" synth drawn=%1 shown=%2%3").arg(m_SynthDrawn)
                    .arg(m_SynthShown.exchange(0, std::memory_order_relaxed))
                    .arg(m_SynthFailed ? QStringLiteral(" (FAILED: off)") : QString());
        m_SynthDrawn = 0;
    }
    m_Received = m_Drawn = m_Overwritten = m_RenderErrors = 0;
    m_CpuUs.clear();
    m_GpuUs.clear();
    return line;
}
