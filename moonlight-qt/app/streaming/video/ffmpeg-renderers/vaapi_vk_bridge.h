// §K.linux VAAPI→Vulkan DMA-BUF bridge.
//
// RADV/Vega 10 driver 沒實作 VK_KHR_video_decode_*，但 VAAPI HW decode 正常。
// 這個 bridge 把 vaExportSurfaceHandle() 匯出的 DMA-BUF fd 用
// VK_KHR_external_memory_fd import 成 VkImage（LINEAR NV12），
// 讓 VkFrucRenderer 的 FRUC chain 能吃到 VAAPI HW decode output。
//
// 設計對照 D3D11VkBridge（d3d11_vk_bridge.h），但 sync 更簡單：
// vaeSyncSurface() + DMA-BUF kernel implicit fence 取代
// D3D11Fence↔VkSemaphore timeline。
//
// Pre-req spike (2026-05-21, RADV RAVEN Mesa 26.0.3):
//   modifier = DRM_FORMAT_MOD_LINEAR (0x0)  ← 確認，VK_IMAGE_TILING_LINEAR 直接用
//   VK_KHR_external_memory_fd              ✅
//   VK_EXT_image_drm_format_modifier       ✅ (linear path 不需要，保留備用)
//   VK_KHR_external_semaphore_fd           ✅ (本次不用，留給未來顯式 sync)

#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

class VAAPIVkBridge
{
public:
    struct ImportedFrame {
        VkImage        image;
        VkDeviceMemory memory;
        uint32_t       width;
        uint32_t       height;
    };

    VAAPIVkBridge();
    ~VAAPIVkBridge();

    VAAPIVkBridge(const VAAPIVkBridge&) = delete;
    VAAPIVkBridge& operator=(const VAAPIVkBridge&) = delete;

    // 一次性初始化：儲存 Vulkan handles + 載入所需 PFN。
    // initialize() 內部會確認 VK_KHR_external_memory_fd 支援；失敗時 cascade
    // 跌到 VAAPIRenderer（ffmpeg.cpp pass=1 retry 走 plain VAAPIRenderer）。
    // 由 VkFrucRenderer::initializeCompositeVAAPI() 呼叫（VkDevice 已建立後）。
    bool initialize(VkInstance instance,
                    VkPhysicalDevice physDev,
                    VkDevice device,
                    PFN_vkGetInstanceProcAddr pfnGIPA);

    bool isInitialized() const { return m_Initialized; }

    // 每幀：DMA-BUF fd + modifier + 尺寸 → 匯入為 VkImage。
    // 前置條件：caller 已完成 vaSyncSurface()（確保 decode done）。
    // 語義：bridge 內部 dup() fd 傳給 vkAllocateMemory；caller 的 dmaFd
    //       仍需自行 close()（va surface 生命週期由 caller 管理）。
    // 回傳 true：out->image / out->memory 由 caller 負責
    //   vkDestroyImage(device, out->image, nullptr)
    //   vkFreeMemory(device, out->memory, nullptr)
    bool importFrame(int dmaFd, uint32_t dmaSize, uint64_t modifier,
                     uint32_t width, uint32_t height,
                     ImportedFrame* out);

private:
    bool loadPFNs_();

    VkInstance       m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysDev  = VK_NULL_HANDLE;
    VkDevice         m_Device   = VK_NULL_HANDLE;

    PFN_vkGetInstanceProcAddr        m_pfnGIPA           = nullptr;
    PFN_vkGetDeviceProcAddr          m_pfnGDPA           = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR          m_pfnGetMemFdProps  = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties m_pfnGetPhysMemProps = nullptr;
    PFN_vkCreateImage                       m_pfnCreateImage    = nullptr;
    PFN_vkDestroyImage                      m_pfnDestroyImage   = nullptr;
    PFN_vkGetImageMemoryRequirements        m_pfnGetImageMemReq = nullptr;
    PFN_vkAllocateMemory                    m_pfnAllocateMemory = nullptr;
    PFN_vkFreeMemory                        m_pfnFreeMemory     = nullptr;
    PFN_vkBindImageMemory                   m_pfnBindImageMemory = nullptr;

    bool m_Initialized = false;
};
