// VipleStream 2.0 §VR M3a X2 — XrRenderer：XR 虛擬螢幕的 ffmpeg frontend。
// 只在 HAVE_OPENXR 且 HAVE_LIBPLACEBO_VULKAN 時編譯。設計：vr_architecture.md §2.4。
//
// renderFrame 只把幀交給 XrVideo 的 mailbox（latest wins），不碰 GPU；XR frame thread 才畫。
// testRenderFrame（C20）在 XR 的 pl_gpu 上真的 map 一次再 unmap；失敗回 false 讓 cascade 退到下一個
// 組合。軟體格式也 map 不了代表 libplacebo 在這個 device 上不可用，標記 XrVideo 不可用，之後的
// decoder 嘗試改走平面 frontend（不變式 5：不讓整條串流失敗）。
// 測試實例（separate test decoder，params->testFrameOnly）不把幀送進 mailbox。

#pragma once

#include "renderer.h"

class XrContext;

class XrRenderer : public IFFmpegRenderer
{
public:
    XrRenderer(XrContext* xr, IFFmpegRenderer* backendRenderer);
    virtual ~XrRenderer() override;

    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool testRenderFrame(AVFrame* frame) override;
    virtual bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override;
    virtual int getRendererAttributes() override;

    // FFmpegVideoDecoder 用：這個 frontend 能否用（XrContext 有影像路徑且沒被標成不可用）
    static bool available(XrContext* xr);

private:
    XrContext* m_Xr;
    IFFmpegRenderer* m_Backend;
    bool m_TestFrameOnly = false;
    uint64_t m_Submitted = 0;
};
