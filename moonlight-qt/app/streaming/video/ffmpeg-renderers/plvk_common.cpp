// VipleStream 2.0 §VR M3a X2 — PlVkRenderer 與 XrRenderer 共用的 libplacebo 純函式。見 plvk_common.h。

#include "plvk_common.h"

#include <SDL.h>

#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
#ifdef HAVE_DRM
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#endif
}

#ifdef HAVE_DRM
#include <libdrm/drm_fourcc.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

// Implementation in plvk_c.c
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
#endif

namespace PlvkCommon {

void logCallback(void*, enum pl_log_level level, const char* msg)
{
    switch (level) {
    case PL_LOG_FATAL:
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_ERR:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_WARN:
        if (strncmp(msg, "Masking `", 9) == 0) {
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_INFO:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_DEBUG:
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_NONE:
    case PL_LOG_TRACE:
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    }
}

#ifdef HAVE_DRM
// §SF-DRMSPLIT：libplacebo 的 pl_map_avframe_drm 只認「每個平面一個 layer」的 DRM_PRIME
// 描述子（VAAPI SEPARATE_LAYERS 的形式）：它逐 layer 用 pl_find_fourcc(layer->format)
// 找單平面格式。Qualcomm iris 的 v4l2m2m（cgutman FFmpeg）輸出單一 layer、兩個平面的
// DRM_FORMAT_NV12，libplacebo 找不到 NV12 就直接回 false、不印任何訊息（release 建置
// 沒有它的 assert），PlVk 的 test decode 失敗而退回 SDL/EGL 前端。
// 這裡改寫成 R8＋GR88（10-bit 是 R16＋GR1616）兩個 layer；dmabuf、offset、pitch、
// modifier 都不動，只是換描述方式。
AVFrame* splitDrmPrimeLayers(const AVFrame* frame, const char** outcome)
{
    *outcome = "as-is";

    auto src = (const AVDRMFrameDescriptor*)frame->data[0];
    if (src == nullptr || src->nb_layers != 1 || src->layers[0].nb_planes != 2) {
        return nullptr;
    }

    uint32_t lumaFormat, chromaFormat;
    switch (src->layers[0].format) {
    case DRM_FORMAT_NV12:
        lumaFormat = DRM_FORMAT_R8;
        chromaFormat = DRM_FORMAT_GR88;
        break;
    case DRM_FORMAT_P010:
        lumaFormat = DRM_FORMAT_R16;
        chromaFormat = DRM_FORMAT_GR1616;
        break;
    default:
        *outcome = "unsupported multi-plane fourcc";
        return nullptr;
    }

    *outcome = "split failed (alloc)";
    AVBufferRef* descBuf = av_buffer_allocz(sizeof(AVDRMFrameDescriptor));
    if (descBuf == nullptr) {
        return nullptr;
    }

    auto dst = (AVDRMFrameDescriptor*)descBuf->data;
    dst->nb_objects = src->nb_objects;
    for (int i = 0; i < src->nb_objects; i++) {
        dst->objects[i] = src->objects[i];
    }
    dst->nb_layers = 2;
    dst->layers[0].format = lumaFormat;
    dst->layers[0].nb_planes = 1;
    dst->layers[0].planes[0] = src->layers[0].planes[0];
    dst->layers[1].format = chromaFormat;
    dst->layers[1].nb_planes = 1;
    dst->layers[1].planes[0] = src->layers[0].planes[1];

    // clone 會對原本的 buf[] 加參照，解碼器的 V4L2 buffer 在 mapping 期間不會被回收
    AVFrame* out = av_frame_clone(frame);
    if (out == nullptr) {
        av_buffer_unref(&descBuf);
        return nullptr;
    }

    // 描述子的記憶體掛在第一個空的 buf 槽，生命週期跟著 frame；pl_map_avframe_ex 會再
    // av_frame_clone 一份自己持有到 pl_unmap_avframe，所以呼叫端 map 完就能放掉這份。
    int slot = -1;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (out->buf[i] == nullptr) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        av_buffer_unref(&descBuf);
        av_frame_free(&out);
        return nullptr;
    }
    out->buf[slot] = descBuf;
    out->data[0] = descBuf->data;
    *outcome = "split into per-plane layers";
    return out;
}

// §SF-DMABUF-CACHE：見 plvk_common.h。原本是 PlVkRenderer::mapDrmPrimeCached（G-α），2026-10-03 搬來
// 這裡讓 XrVideo 共用——XR 路徑當時仍用 pl_map_avframe_ex 每幀匯入，PCVR 4320x2160@120 第 6 分鐘踩到
// 同一個 msm 核心錯誤，頭盔卡死。邏輯不變。
bool DrmTexCache::map(pl_gpu gpu, const AVFrame* frame, pl_frame* out)
{
    static constexpr size_t kMaxEntries = 64;
    auto drm = (const AVDRMFrameDescriptor*)frame->data[0];
    if (drm == nullptr || frame->hw_frames_ctx == nullptr ||
            !(gpu->import_caps.tex & PL_HANDLE_DMA_BUF)) {
        return false;
    }
    auto hwfc = (const AVHWFramesContext*)frame->hw_frames_ctx->data;
    if (hwfc != m_Ctx) {
        clear(gpu, "decoder frames context changed");
        m_Ctx = hwfc;
    }
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(hwfc->sw_format);
    if (desc == nullptr) {
        return false;
    }

    *out = {};
    pl_frame_from_avframe(out, frame);
    if (out->num_planes <= 0 || drm->nb_layers < out->num_planes) {
        *out = {};
        return false;
    }

    for (int n = 0; n < out->num_planes; n++) {
        const AVDRMLayerDescriptor* layer = &drm->layers[n];
        if (layer->nb_planes != 1) {
            *out = {};
            return false;
        }
        const AVDRMPlaneDescriptor* plane = &layer->planes[0];
        const AVDRMObjectDescriptor* object = &drm->objects[plane->object_index];
        pl_fmt fmt = pl_find_fourcc(gpu, layer->format);
        struct stat st;
        if (fmt == nullptr || !pl_fmt_has_modifier(fmt, object->format_modifier) ||
                plane->pitch < 0 || fstat(object->fd, &st) != 0) {
            *out = {};
            return false;
        }
        bool isChroma = n == 1 || n == 2;
        int w = AV_CEIL_RSHIFT(frame->width, isChroma ? desc->log2_chroma_w : 0);
        int h = AV_CEIL_RSHIFT(frame->height, isChroma ? desc->log2_chroma_h : 0);
        // §SF-PITCHCLAMP：iris／v4l2m2m 對 GUI 啟動探測的 1280x720 測試幀回報 frame 寬 1344
        // （對齊後的寬度），但 buffer 的 pitch 只有 1280（size=1280*736*1.5 也對得上），
        // 實際寬度不可能超過 pitch。原本 w > pitch 被 libplacebo validation 擋下，失敗路徑
        // 接著讓整個程式 SIGSEGV（從啟動程式開 GUI 閃一下就消失）。把寬度夾到 pitch 內，
        // 裁切範圍在迴圈後一併夾住。
        if (w > 0 && fmt->texel_size > 0 && plane->pitch > 0 &&
                plane->pitch < (ptrdiff_t)w * fmt->texel_size) {
            int clamped = (int)(plane->pitch / (ptrdiff_t)fmt->texel_size);
            if (!m_LoggedPitchClamp) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "%s dmabuf plane %d width %d exceeds pitch %lld; clamped to %d "
                            "(frame=%dx%d)",
                            m_Tag, n, w, (long long)plane->pitch, clamped, frame->width, frame->height);
                m_LoggedPitchClamp = true;
            }
            w = clamped;
        }
        // 其餘不合法的情況仍交給 libplacebo 前先擋下，並同步留下實際值
        //（log 是非同步的，崩潰時最後幾行會遺失）。
        if (w <= 0 || h <= 0 || plane->pitch < (ptrdiff_t)w * fmt->texel_size) {
            fprintf(stderr, "%s dmabuf plane %d rejected: frame=%dx%d sw_format=%d "
                            "fourcc=0x%08x w=%d h=%d pitch=%lld offset=%lld texel=%zu size=%zu\n",
                    m_Tag, n, frame->width, frame->height, (int)hwfc->sw_format, layer->format, w, h,
                    (long long)plane->pitch, (long long)plane->offset, fmt->texel_size,
                    object->size);
            fflush(stderr);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "%s dmabuf plane %d rejected: frame=%dx%d sw_format=%d "
                        "fourcc=0x%08x w=%d h=%d pitch=%lld",
                        m_Tag, n, frame->width, frame->height, (int)hwfc->sw_format, layer->format,
                        w, h, (long long)plane->pitch);
            *out = {};
            return false;
        }

        Entry* hit = nullptr;
        for (auto& e : m_Entries) {
            if (e.dev == (uint64_t)st.st_dev && e.ino == (uint64_t)st.st_ino &&
                    e.offset == (uint32_t)plane->offset && e.pitch == (uint32_t)plane->pitch &&
                    e.fourcc == layer->format && e.modifier == object->format_modifier &&
                    e.w == w && e.h == h) {
                hit = &e;
                break;
            }
        }

        if (hit != nullptr) {
            m_Hits++;
        }
        else {
            pl_tex_params params = {};
            params.w = w;
            params.h = h;
            params.format = fmt;
            params.sampleable = true;
            params.blit_src = (fmt->caps & PL_FMT_CAP_BLITTABLE) != 0;
            params.import_handle = PL_HANDLE_DMA_BUF;
            params.shared_mem.handle.fd = object->fd;
            params.shared_mem.size = object->size;
            params.shared_mem.offset = plane->offset;
            params.shared_mem.drm_format_mod = object->format_modifier;
            params.shared_mem.stride_w = plane->pitch;
            pl_tex tex = pl_tex_create(gpu, &params);
            if (tex == nullptr) {
                *out = {};
                return false;
            }
            m_Imports++;

            if (m_Entries.size() >= kMaxEntries) {
                // 最久沒用的先走；pl_tex_destroy 會等 GPU 用完才真的釋放
                auto lru = std::min_element(m_Entries.begin(), m_Entries.end(),
                                            [](const Entry& a, const Entry& b) {
                                                return a.lastUse < b.lastUse;
                                            });
                pl_tex_destroy(gpu, &lru->tex);
                m_Entries.erase(lru);
            }

            Entry e;
            e.dev = (uint64_t)st.st_dev;
            e.ino = (uint64_t)st.st_ino;
            e.modifier = object->format_modifier;
            e.offset = (uint32_t)plane->offset;
            e.pitch = (uint32_t)plane->pitch;
            e.fourcc = layer->format;
            e.w = w;
            e.h = h;
            e.tex = tex;
            m_Entries.push_back(e);
            hit = &m_Entries.back();
        }
        hit->lastUse = ++m_Tick;
        out->planes[n].texture = hit->tex;
    }

    // §SF-PITCHCLAMP：寬高被夾過時，裁切範圍不能超出亮度平面的材質
    {
        float maxX = (float)out->planes[0].texture->params.w;
        float maxY = (float)out->planes[0].texture->params.h;
        if (out->crop.x1 > maxX) out->crop.x1 = maxX;
        if (out->crop.y1 > maxY) out->crop.y1 = maxY;
    }

    // 同 libplacebo 的 pl_fix_hwframe_sample_depth 與 P010 的 bit_shift
    out->repr.bits.sample_depth = out->planes[0].texture->params.format->component_depth[0];
    if (hwfc->sw_format == AV_PIX_FMT_P010) {
        out->repr.bits.bit_shift = 6;
    }
    out->user_data = nullptr;

    if ((m_Hits + m_Imports) % 3600 == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "%s dmabuf cache: entries=%zu imports=%llu hits=%llu",
                    m_Tag, m_Entries.size(), (unsigned long long)m_Imports,
                    (unsigned long long)m_Hits);
    }
    return true;
}

void DrmTexCache::clear(pl_gpu gpu, const char* reason)
{
    if (m_Entries.empty()) {
        return;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "%s dmabuf cache cleared (%s): entries=%zu imports=%llu hits=%llu",
                m_Tag, reason, m_Entries.size(), (unsigned long long)m_Imports,
                (unsigned long long)m_Hits);
    for (auto& e : m_Entries) {
        pl_tex_destroy(gpu, &e.tex);
    }
    m_Entries.clear();
}

void DrmTexCache::abandon()
{
    if (!m_Entries.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "%s dmabuf cache abandoned (GPU wedged): leaking %zu imported texture(s)",
                    m_Tag, m_Entries.size());
    }
    m_Entries.clear();
    m_Ctx = nullptr;
}
#endif

void fixupMappedFrame(const AVFrame* frame, pl_frame* mapped, bool forceFullRange)
{
    // libplacebo assumes a minimum luminance value of 0 means the actual value was unknown.
    // Since we assume the host values are correct, we use the PL_COLOR_HDR_BLACK constant to
    // indicate infinite contrast.
    //
    // NB: We also have to check that the AVFrame actually had metadata in the first place,
    // because libplacebo may infer metadata if the frame didn't have any.
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) && !mapped->color.hdr.min_luma) {
        mapped->color.hdr.min_luma = PL_COLOR_HDR_BLACK;
    }

    // HACK: AMF AV1 encoding on the host PC does not set full color range properly in the
    // bitstream data, so libplacebo incorrectly renders the content as limited range.
    //
    // As a workaround, set full range manually in the mapped frame ourselves.
    //
    // §VR M3a：XR 路徑（forceFullRange=false）只在幀明確標 MPEG（limited）時用 limited，
    // 其餘（JPEG、未標示）維持 full，保留上面 AMF 的繞法。
    if (!forceFullRange && frame->color_range == AVCOL_RANGE_MPEG) {
        mapped->repr.levels = PL_COLOR_LEVELS_LIMITED;
    }
    else {
        mapped->repr.levels = PL_COLOR_LEVELS_FULL;
    }
}

}  // namespace PlvkCommon
