// VipleStream 2.0 §VR M3a X2 — XrVideo 實作。設計見 xrvideo.h。

#include "xrvideo.h"

#include "streaming/video/ffmpeg-renderers/plvk_common.h"

#include <QImage>

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

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

void XrVideo::destroySwapchain()
{
    pl_gpu g = gpu();
    for (pl_tex& t : m_Wrapped) {
        if (t != nullptr && g != nullptr) {
            pl_tex_destroy(g, &t);
        }
    }
    m_Wrapped.clear();
    m_Images.clear();
    if (m_Swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_Swapchain);
        m_Swapchain = XR_NULL_HANDLE;
    }
    m_SwWidth = m_SwHeight = 0;
    m_SwFormat = 0;
    m_ViewFormat = VK_FORMAT_UNDEFINED;
    m_Mode = TargetMode::None;
    m_HaveReleased = false;
}

void XrVideo::destroy()
{
    m_Usable.store(false, std::memory_order_release);
    if (m_Vulkan != nullptr) {
        pl_gpu_finish(m_Vulkan->gpu);
    }
    destroySwapchain();
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        av_frame_free(&m_Pending);
    }
    if (m_Vulkan != nullptr) {
        for (pl_tex& t : m_Tex) {
            pl_tex_destroy(m_Vulkan->gpu, &t);
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
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    m_Received++;
    if (overwrote) {
        m_Overwritten++;
    }
}

bool XrVideo::mapFrame(const AVFrame* frame, pl_frame* out, pl_tex* texSet)
{
    const AVFrame* src = frame;
    AVFrame* split = nullptr;
#ifdef HAVE_DRM
    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        const char* outcome = nullptr;
        split = PlvkCommon::splitDrmPrimeLayers(frame, &outcome);
        if (split != nullptr) {
            src = split;
        }
    }
#endif
    pl_avframe_params mp = {};
    mp.frame = src;
    mp.tex = texSet;
    const bool ok = pl_map_avframe_ex(m_Vulkan->gpu, out, &mp);
    // libplacebo 成功時自己 clone 一份持有到 pl_unmap_avframe，這份改寫描述子可以放掉
    av_frame_free(&split);
    if (ok) {
        PlvkCommon::fixupMappedFrame(frame, out);
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
    if (!mapFrame(frame, &mapped, m_TestTex)) {
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
    if (m_Swapchain != XR_NULL_HANDLE && m_SwWidth == width && m_SwHeight == height) {
        return true;
    }
    if (m_Swapchain != XR_NULL_HANDLE) {
        pl_gpu_finish(m_Vulkan->gpu);
        destroySwapchain();
    }
    if (width <= 0 || height <= 0) {
        return false;
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
        m_Swapchain = sc;
        m_SwWidth = width;
        m_SwHeight = height;
        m_SwFormat = c.sw;
        m_ViewFormat = c.view;
        m_Mode = c.mode;
        m_Wrapped = std::move(wrapped);
        m_Images.clear();
        for (uint32_t i = 0; i < count; i++) {
            m_Images.push_back(imgs[i].image);
        }
        m_HaveReleased = false;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] video swapchain %dx%d fmt=%d view=%d mode=%s images=%u",
                    width, height, static_cast<int>(c.sw), static_cast<int>(c.view),
                    modeName(static_cast<int>(c.mode)), count);
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
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = xrS<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
    XrResult r;
    {
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        r = xrAcquireSwapchainImage(m_Swapchain, &ai, &idx);
    }
    if (XR_FAILED(r) || idx >= m_Wrapped.size()) {
        av_frame_free(&frame);
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        m_RenderErrors++;
        return false;
    }
    XrSwapchainImageWaitInfo wi = xrS<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
    wi.timeout = 100000000;  // 100 ms
    xrWaitSwapchainImage(m_Swapchain, &wi);

    pl_gpu g = m_Vulkan->gpu;
    pl_tex tex = m_Wrapped[idx];

    // OpenXR（Vulkan）：acquire 後彩色影像在 COLOR_ATTACHMENT_OPTIMAL，release 前也要回到這個 layout
    pl_vulkan_release_params rp = {};
    rp.tex = tex;
    rp.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rp.qf = VK_QUEUE_FAMILY_IGNORED;
    pl_vulkan_release_ex(g, &rp);

    pl_frame mapped;
    std::memset(&mapped, 0, sizeof(mapped));
    const bool mappedOk = mapFrame(frame, &mapped, m_Tex);
    bool rendered = false;
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
        rendered = pl_render_image(m_Renderer, &mapped, &target, &pl_render_fast_params);
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
    if (rendered && !m_Dumped && !m_Config.dumpPath.isEmpty() &&
        m_TotalDrawn >= static_cast<uint64_t>(std::max(1, m_Config.dumpAfterFrames))) {
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
    // GPU 做完才 release 給 runtime、才放掉解碼幀（pl_gpu_finish 會等到 hold 的 semaphore 觸發）
    pl_gpu_finish(g);
    if (mappedOk) {
        pl_unmap_avframe(g, &mapped);
    }
    av_frame_free(&frame);

    XrSwapchainImageReleaseInfo ri = xrS<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
    {
        std::lock_guard<std::mutex> lk(m_Ctx->queueMutex());
        xrReleaseSwapchainImage(m_Swapchain, &ri);
    }
    m_HaveReleased = true;

    const uint64_t dt = nowUs() - t0;
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    if (rendered && held) {
        m_Drawn++;
        m_RenderUs.push_back(static_cast<uint32_t>(std::min<uint64_t>(dt, 0xffffffffu)));
    }
    else {
        m_RenderErrors++;
    }
    return rendered && held;
}

bool XrVideo::update(XrSession session, XrSwapchainSubImage* subImage, float* aspect)
{
    if (m_Vulkan == nullptr) {
        return false;
    }
    AVFrame* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_MailboxMutex);
        f = m_Pending;
        m_Pending = nullptr;
    }
    if (f != nullptr) {
        if (usable() && ensureSwapchain(session, f->width, f->height)) {
            renderNewFrame(f);
        }
        else {
            av_frame_free(&f);
            if (usable() && m_Swapchain == XR_NULL_HANDLE) {
                markUnusable("no usable video swapchain");
            }
        }
    }
    if (m_Swapchain == XR_NULL_HANDLE || !m_HaveReleased || m_SwHeight <= 0) {
        return false;
    }
    subImage->swapchain = m_Swapchain;
    subImage->imageRect.offset = {0, 0};
    subImage->imageRect.extent = {m_SwWidth, m_SwHeight};
    subImage->imageArrayIndex = 0;
    *aspect = static_cast<float>(m_SwWidth) / static_cast<float>(m_SwHeight);
    return true;
}

QString XrVideo::takeStatsLine()
{
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    double p50 = 0.0, p95 = 0.0;
    if (!m_RenderUs.empty()) {
        std::vector<uint32_t> v = m_RenderUs;
        std::sort(v.begin(), v.end());
        p50 = v[v.size() / 2] / 1000.0;
        p95 = v[std::min(v.size() - 1, (v.size() * 95) / 100)] / 1000.0;
    }
    const QString line = QStringLiteral("recv=%1 drawn=%2 overwritten=%3 errors=%4 render p50=%5 p95=%6 ms mode=%7 %8x%9")
                             .arg(m_Received).arg(m_Drawn).arg(m_Overwritten).arg(m_RenderErrors)
                             .arg(p50, 0, 'f', 2).arg(p95, 0, 'f', 2)
                             .arg(QLatin1String(modeName(static_cast<int>(m_Mode))))
                             .arg(m_SwWidth).arg(m_SwHeight);
    m_Received = m_Drawn = m_Overwritten = m_RenderErrors = 0;
    m_RenderUs.clear();
    return line;
}
