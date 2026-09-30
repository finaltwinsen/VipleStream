// VipleStream 2.0 §VR M3a X2 — XrRenderer：XR 虛擬螢幕的 ffmpeg frontend。
// 只在 HAVE_OPENXR 且 HAVE_LIBPLACEBO_VULKAN 時編譯。設計：vr_architecture.md §2.4。
//
// renderFrame 只把幀交給 XrVideo 的 mailbox（latest wins），不碰 GPU；XR frame thread 才畫。
// testRenderFrame（C20）在 XR 的 pl_gpu 上真的 map 一次再 unmap；失敗回 false 讓 cascade 退到下一個
// 組合。軟體格式也 map 不了代表 libplacebo 在這個 device 上不可用，標記 XrVideo 不可用，之後的
// decoder 嘗試改走平面 frontend（不變式 5：不讓整條串流失敗）。
// 測試實例（separate test decoder，params->testFrameOnly）不把幀送進 mailbox。
//
// §XR-HW-READBACK（2026-09-30）：Windows 硬解（D3D11VA）的幀 libplacebo 在 XR 的 Vulkan device 上
// map 不了，以前 testRenderFrame 失敗 → cascade 退到 PlVk backend，而 PlVk 自建 Vulkan device，NVIDIA
// 驅動在那一刻讓 SteamVR compositor 停頓約 770 ms（開場 10 s 漏 5.3%）。改為：D3D11 幀先
// av_hwframe_transfer_data 搬到系統記憶體再送 XrVideo（走 SW 上傳），D3D11VA backend 就留得住，
// 不會再建第二個 Vulkan device。讓 FFmpeg Vulkan 硬解共用 XrContext 的 VkDevice 是更好的解法，
// 但 SteamVR 回報 maxApi=1.2、FFmpeg Vulkan 解碼要 1.3 等級功能（synchronization2 等），先不走。

#pragma once

#include "renderer.h"

#include <vector>

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
    // D3D11 等 hwaccel 幀（非 DRM_PRIME）搬到系統記憶體；不需要搬時回 nullptr
    AVFrame* readbackIfNeeded(const AVFrame* frame, bool* failed);

    XrContext* m_Xr;
    IFFmpegRenderer* m_Backend;
    bool m_TestFrameOnly = false;
    uint64_t m_Submitted = 0;
    // §XR-HW-READBACK 統計（renderFrame 呼叫端執行緒）
    uint64_t m_ReadbackWindowStartUs = 0;
    uint64_t m_ReadbackCount = 0;
    uint64_t m_ReadbackFailed = 0;
    std::vector<double> m_ReadbackMs;
};

// §VR M3a X3：XR 模式＋軟體解碼的 backend——不建任何 GPU 物件、不碰平面視窗。
// 原本 SW 解碼的 backend 是 PlVkRenderer：它自己建一個 Vulkan device 並在平面視窗建 swapchain，
// 建 device 的那一刻 NVIDIA 驅動會讓 SteamVR compositor 停頓約 770 ms（S2 開場一次漏 69 幀）。
// 影像由 XrRenderer（frontend）交給 XrVideo 畫進 XR；平面視窗留空。
// 只給真正串流的 decoder 用（params->xr 且非 testFrameOnly）；能力探測照舊走原本的 cascade。
class XrSwBackend : public IFFmpegRenderer
{
public:
    explicit XrSwBackend(XrContext* xr);
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override;
    virtual int getRendererAttributes() override { return 0; }

private:
    XrContext* m_Xr;
};
