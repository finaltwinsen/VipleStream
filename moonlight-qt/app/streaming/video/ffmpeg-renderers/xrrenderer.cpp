// VipleStream 2.0 §VR M3a X2 — XrRenderer 實作。設計見 xrrenderer.h。

#include "xrrenderer.h"

#include "streaming/xr/xrcontext.h"
#include "streaming/xr/xrvideo.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <SDL.h>

#include <algorithm>
#include <chrono>

namespace {

// 軟體解碼常見輸出：這些都 map 不了就代表 libplacebo 在 XR device 上不可用
bool isSoftwareFormat(int format)
{
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return d != nullptr && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

// §XR-HW-READBACK：要先搬到系統記憶體的 hwaccel 格式（DRM_PRIME 由 libplacebo 直接匯入，不搬）
bool needsReadback(const AVFrame* frame)
{
    return frame != nullptr && frame->hw_frames_ctx != nullptr && frame->format == AV_PIX_FMT_D3D11;
}

uint64_t nowUs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

XrRenderer::XrRenderer(XrContext* xr, IFFmpegRenderer* backendRenderer)
    : IFFmpegRenderer(RendererType::Vulkan), m_Xr(xr), m_Backend(backendRenderer)
{
}

XrRenderer::~XrRenderer() = default;

bool XrRenderer::available(XrContext* xr)
{
    return xr != nullptr && xr->video() != nullptr && xr->video()->usable();
}

bool XrRenderer::initialize(PDECODER_PARAMETERS params)
{
    if (!available(m_Xr)) {
        return false;
    }
    m_TestFrameOnly = params->testFrameOnly;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] XrRenderer frontend (backend=%s, testFrameOnly=%d, %dx%d)",
                m_Backend != nullptr ? m_Backend->getRendererName() : "none",
                m_TestFrameOnly ? 1 : 0, params->width, params->height);
    return true;
}

bool XrRenderer::prepareDecoderContext(AVCodecContext*, AVDictionary**)
{
    // frontend 不參與解碼器設定（backend 負責）
    return true;
}

AVFrame* XrRenderer::readbackIfNeeded(const AVFrame* frame, bool* failed)
{
    *failed = false;
    if (!needsReadback(frame)) {
        return nullptr;
    }
    AVFrame* sw = av_frame_alloc();
    if (sw == nullptr) {
        *failed = true;
        return nullptr;
    }
    const uint64_t t0 = nowUs();
    int err = av_hwframe_transfer_data(sw, frame, 0);
    if (err >= 0) {
        err = av_frame_copy_props(sw, frame);
    }
    const uint64_t t1 = nowUs();
    if (err < 0) {
        av_frame_free(&sw);
        *failed = true;
        m_ReadbackFailed++;
        return nullptr;
    }
    m_ReadbackCount++;
    m_ReadbackMs.push_back((t1 - t0) / 1000.0);
    if (m_ReadbackWindowStartUs == 0) {
        m_ReadbackWindowStartUs = t1;
    }
    else if (t1 - m_ReadbackWindowStartUs >= 10000000ull) {
        std::vector<double> v = m_ReadbackMs;
        std::sort(v.begin(), v.end());
        const double p50 = v.empty() ? 0.0 : v[v.size() / 2];
        const double p95 = v.empty() ? 0.0 : v[std::min(v.size() - 1, (v.size() * 95) / 100)];
        const char* swFmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(sw->format));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR] 10s d3d11-readback frames=%llu failed=%llu p50=%.2f p95=%.2f ms (sw=%s %dx%d)",
                    static_cast<unsigned long long>(m_ReadbackCount), static_cast<unsigned long long>(m_ReadbackFailed),
                    p50, p95, swFmt ? swFmt : "?", sw->width, sw->height);
        m_ReadbackMs.clear();
        m_ReadbackCount = 0;
        m_ReadbackFailed = 0;
        m_ReadbackWindowStartUs = t1;
    }
    return sw;
}

void XrRenderer::renderFrame(AVFrame* frame)
{
    if (m_TestFrameOnly || !available(m_Xr)) {
        return;
    }
    bool failed = false;
    AVFrame* sw = readbackIfNeeded(frame, &failed);
    if (failed) {
        return;
    }
    m_Xr->video()->submit(sw != nullptr ? sw : frame);
    if (sw != nullptr) {
        av_frame_free(&sw);
    }
    m_Submitted++;
}

bool XrRenderer::testRenderFrame(AVFrame* frame)
{
    if (!available(m_Xr)) {
        return false;
    }
    if (needsReadback(frame)) {
        bool failed = false;
        AVFrame* sw = readbackIfNeeded(frame, &failed);
        const bool ok = sw != nullptr && m_Xr->video()->testMap(sw);
        if (sw != nullptr) {
            av_frame_free(&sw);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR] test map via d3d11 readback: %s", ok ? "ok" : "failed");
        return ok;
    }
    if (m_Xr->video()->testMap(frame)) {
        return true;
    }
    if (isSoftwareFormat(frame->format)) {
        m_Xr->video()->markUnusable("software frame could not be mapped on the XR device");
    }
    return false;
}

bool XrRenderer::isPixelFormatSupported(int, AVPixelFormat pixelFormat)
{
    // libplacebo 能 map 的：系統記憶體格式與 DRM_PRIME（Linux／Frame）；D3D11／VAAPI 等 hwaccel
    // 格式不直接支援（testRenderFrame 會擋下）
    if (pixelFormat == AV_PIX_FMT_DRM_PRIME || pixelFormat == AV_PIX_FMT_D3D11) {
        return true;  // D3D11：§XR-HW-READBACK 搬到系統記憶體後上傳
    }
    return isSoftwareFormat(pixelFormat);
}

XrSwBackend::XrSwBackend(XrContext* xr) : IFFmpegRenderer(RendererType::Unknown), m_Xr(xr) {}

bool XrSwBackend::initialize(PDECODER_PARAMETERS params)
{
    if (params->testFrameOnly || !XrRenderer::available(m_Xr)) {
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-XR] XrSwBackend: software decode straight into XR (no flat-window GPU device) %dx%d",
                params->width, params->height);
    return true;
}

bool XrSwBackend::prepareDecoderContext(AVCodecContext*, AVDictionary**)
{
    return true;  // 軟體解碼，不設 hwaccel
}

void XrSwBackend::renderFrame(AVFrame*)
{
    // 平面視窗不畫；影像由 frontend（XrRenderer）送進 XR
}

bool XrSwBackend::isPixelFormatSupported(int, AVPixelFormat pixelFormat)
{
    return isSoftwareFormat(pixelFormat);
}

int XrRenderer::getRendererAttributes()
{
    // XR frame thread 自己跟著 runtime 的節拍送幀，Pacer 不需要 vsync 節拍
    return 0;
}
