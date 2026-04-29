// VipleStream §J.3.e.2.i — VkFrucRenderer
//
// Native Vulkan FRUC renderer ported from
// `moonlight-android/app/src/main/jni/moonlight-core/vk_backend.c`.
// Bypasses libplacebo entirely: owns its own VkSwapchainKHR, calls
// vkAcquireNextImageKHR / vkQueuePresentKHR directly, single
// vkQueueSubmit + 2× vkQueuePresentKHR for dual-present (interp + real).
//
// The reason for the bypass: PC libplacebo's wrapped pl_tex internal
// sync (pl_vulkan_release_ex / pl_vulkan_hold_ex) had a systematic
// frame#2 stall on the override path (§J.3.e.2.f benchmark), and three
// rounds of application-side workarounds (§J.3.e.2.g.A/B/C) couldn't
// resolve it.  The Android impl has been production-tested on Pixel 5,
// so we know the architecture works — we just need to port it.
//
// Rules of engagement:
//   1. opt-in only via VIPLE_VK_FRUC_GENERIC=1
//   2. PlVkRenderer / D3D11VARenderer cascade unaffected
//   3. SDR sRGB only in this phase; HDR / 10-bit deferred to §J.3.e.2.j
//   4. Reuses §J.3.e.2.h.a GLSL compute shaders (motionest / median / warp)
//      — the algorithm layer is already shipped & glslang-validated
//
// Sub-phase split:
//   i.1 — this scaffold (initialize returns false, cascade falls through)
//   i.2 — instance/device/swapchain
//   i.3 — graphics pipeline (NV12→swapchain)
//   i.4 — compute integration
//   i.5 — dual present
//   i.6 — benchmark vs D3D11+GenericFRUC

#pragma once

#include "renderer.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <vulkan/vulkan.h>

#ifdef Q_OS_WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif

class VkFrucRenderer : public IFFmpegRenderer {
public:
    VkFrucRenderer(int pass);
    ~VkFrucRenderer() override;

    // §J.3.e.2.i.1 — scaffold returns false (cascade falls through to
    // PlVkRenderer).  i.2 will replace with real init.
    bool initialize(PDECODER_PARAMETERS params) override;
    bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    void renderFrame(AVFrame* frame) override;

    InitFailureReason getInitFailureReason() override { return m_InitFailureReason; }
    int getDecoderCapabilities() override;
    int getRendererAttributes() override;
    int getDecoderColorspace() override { return COLORSPACE_REC_709; }

    // Overlay::IOverlayRenderer interface — stub for now (i.3+ will wire)
    void notifyOverlayUpdated(Overlay::OverlayType) override {}

private:
    int m_Pass;  // cascade pass index (matches D3D11VARenderer / PlVkRenderer pattern)
    InitFailureReason m_InitFailureReason = InitFailureReason::Unknown;

    // §J.3.e.2.i.2+ — Vulkan handles will live here
    // VkInstance       m_VkInstance       = VK_NULL_HANDLE;
    // VkPhysicalDevice m_PhysicalDevice   = VK_NULL_HANDLE;
    // VkDevice         m_Device           = VK_NULL_HANDLE;
    // VkSurfaceKHR     m_Surface          = VK_NULL_HANDLE;
    // VkSwapchainKHR   m_Swapchain        = VK_NULL_HANDLE;
    // ... etc — see Android vk_backend.c struct vk_backend_s for the
    // full member list this will eventually mirror
};

#endif // HAVE_LIBPLACEBO_VULKAN
