// VipleStream 2.0 §VR M3a X2 — PlVkRenderer 與 XrRenderer 共用的 libplacebo 純函式。見 plvk_common.h。

#include "plvk_common.h"

#include <SDL.h>

#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
#ifdef HAVE_DRM
#include <libavutil/hwcontext_drm.h>
#endif
}

#ifdef HAVE_DRM
#include <libdrm/drm_fourcc.h>
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
#endif

void fixupMappedFrame(const AVFrame* frame, pl_frame* mapped)
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
    mapped->repr.levels = PL_COLOR_LEVELS_FULL;
}

}  // namespace PlvkCommon
