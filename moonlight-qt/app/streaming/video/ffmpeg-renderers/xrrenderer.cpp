// VipleStream 2.0 §VR M3a X2 — XrRenderer 實作。設計見 xrrenderer.h。

#include "xrrenderer.h"

#include "streaming/xr/xrcontext.h"
#include "streaming/xr/xrvideo.h"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace {

// 軟體解碼常見輸出：這些都 map 不了就代表 libplacebo 在 XR device 上不可用
bool isSoftwareFormat(int format)
{
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format));
    return d != nullptr && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL);
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

void XrRenderer::renderFrame(AVFrame* frame)
{
    if (m_TestFrameOnly || !available(m_Xr)) {
        return;
    }
    m_Xr->video()->submit(frame);
    m_Submitted++;
}

bool XrRenderer::testRenderFrame(AVFrame* frame)
{
    if (!available(m_Xr)) {
        return false;
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
    if (pixelFormat == AV_PIX_FMT_DRM_PRIME) {
        return true;
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
