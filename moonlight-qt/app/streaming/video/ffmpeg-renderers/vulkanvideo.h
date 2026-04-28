// VipleStream §J.3.b — Vulkan-native video decoder using
// VK_KHR_video_decode_av1 / _h265 / _h264 + ffmpeg 8.x AV_HWDEVICE_TYPE_VULKAN.
//
// Goal: eliminate the D3D11→Vulkan bridge that §J.1 hit dead-end on.
// Decoder produces VkImage directly (NV12 / P010 multi-plane); NCNN-Vulkan
// FRUC consumes the VkImage with no copy.
//
// Architecture (§J.3.b.1 skeleton, will grow phase-by-phase):
//
//   VulkanVideoRenderer
//     └─ AVHWDeviceContext (Vulkan)        ← we own / create
//         ├─ VkInstance, VkPhysicalDevice, VkDevice
//         └─ VkQueue (decode family)
//     └─ AVCodecContext.hw_device_ctx       ← passed to ffmpeg
//     └─ get_format callback → AV_PIX_FMT_VULKAN
//     └─ submitDecodeUnit → AVFrame { data[0]=AVVkFrame { VkImage[] } }
//     └─ renderFrame → presents via Vulkan compute / swapchain
//                      (or temporary readback in §J.3.b skeleton phase)
//
// This file is **NOT yet wired into the renderer cascade**.  §J.3.c will
// hook it into Session::initialize.  For §J.3.b.1 it just compiles +
// validates that AV_HWDEVICE_TYPE_VULKAN can be created on this system.
//
// Windows-only for now (matching d3d11va).  Linux/macOS Vulkan path
// follows in §J.4.

#pragma once

#ifdef _WIN32

#include "renderer.h"

#include <atomic>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

class VulkanVideoRenderer : public IFFmpegRenderer
{
public:
    VulkanVideoRenderer();
    virtual ~VulkanVideoRenderer();

    // IFFmpegRenderer
    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    void renderFrame(AVFrame* frame) override;
    InitFailureReason getInitFailureReason() override;

    // IOverlayRenderer (inherited via IFFmpegRenderer → Overlay::IOverlayRenderer)
    void notifyOverlayUpdated(Overlay::OverlayType) override {}

    // §J.3.b.1: returns true iff the runtime environment can support a
    // Vulkan-native decoder — checked by Session::initialize before
    // attempting to instantiate.  A "no" answer steers cascade back to
    // D3D11VARenderer (legacy fallback).
    static bool isFeasibleOnThisSystem();

private:
    // Lazily-created ffmpeg hardware device context.
    AVBufferRef* m_HwDeviceCtx = nullptr;

    // Decoder parameters (resolution / format / fps) cached from initialize().
    DECODER_PARAMETERS m_DecoderParams = {};

    // Frame counter for log throttling — we don't want to log every frame.
    std::atomic<uint64_t> m_FrameCount { 0 };

    // §J.3.b.1: failure reason recorded by initialize() for cascade telemetry.
    InitFailureReason m_InitFailureReason { InitFailureReason::Unknown };
};

#endif // _WIN32
