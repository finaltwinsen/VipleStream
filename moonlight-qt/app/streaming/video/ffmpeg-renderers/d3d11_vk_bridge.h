// §B Phase B 重啟：D3D11VA HEVC HW decode → Vulkan shared image →
// VkFrucRenderer FRUC bridge.  Background：AMD Radeon 780M iGPU 的 Windows
// Vulkan driver 沒實作 VK_KHR_video_decode_h265，HEVC stream 在這台機器
// 只能走 SW decode.  讓 HEVC 改走 D3D11VA HW decode（driver 100% 支援）
// 然後透過 VK_KHR_external_memory_win32 + SHARED_NTHANDLE import 到
// Vulkan 給 VkFrucRenderer 做 FRUC.  H.264 / AV1 仍走原本的 Vulkan-native
// hwaccel path.
//
// 前作：v1.3.41-47 §I.F / §J.1 已在 ncnnfruc.cpp 做過一輪相同機制（NCNN
// model output 走 D3D11→Vulkan）但在 NVIDIA 596.84 driver 上完整 device-
// lost.  memory project_phase_b_dead_end.md 結論「換 GPU 理論上路 1+
// 應該能 work」.  AMD 780M 是新驗證機會.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// PFN_vkGetMemoryWin32HandlePropertiesKHR / VkImportSemaphoreWin32HandleInfoKHR
// 等 Win32-platform Vulkan types live in vulkan_win32.h, which vulkan.h only
// pulls in when VK_USE_PLATFORM_WIN32_KHR is defined BEFORE the include.
// vkfruc.h already sets this in its own translation unit, but d3d11_vk_bridge.cpp
// 編譯時是獨立 TU, so the define has to come from here.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

class D3D11VkBridge
{
public:
    // §B 啟動時 VIPLE_VKFRUC_D3D11_HEVC_MODE=A1|A2|A3|A4 可切換 ablation.
    // A1 (default) 用 D3D11_TEXTURE_BIT memory handle + D3D12_FENCE_BIT
    // semaphore；A2 換 memory handle 為 D3D12_RESOURCE_BIT；A3 跟 A1 一樣
    // 但 caller-side wait stage 收縮到 TOP_OF_PIPE；A4 是 fallback，繞
    // ID3D12Device 重 open shared handle.  v1.3.45-47 NV 上 A1-A3 全 device-
    // lost；A4 沒實際試過（~250 LOC，留作最後手段）.
    enum class HandleMode {
        A1_D3D11_D3D12FENCE,
        A2_D3D12_D3D12FENCE,
        A3_D3D11_TOP_OF_PIPE,
        A4_D3D12_BRIDGE,
    };

    struct ImportedImage {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t       width  = 0;
        uint32_t       height = 0;
        VkFormat       format = VK_FORMAT_UNDEFINED;
    };

    D3D11VkBridge();
    ~D3D11VkBridge();

    D3D11VkBridge(const D3D11VkBridge&) = delete;
    D3D11VkBridge& operator=(const D3D11VkBridge&) = delete;

    // §B B1 — verify that the D3D11 adapter and the Vulkan physical device
    // refer to the same GPU (LUID compare) and load the Win32-external
    // Vulkan PFNs the later phases need.  Caller passes already-created
    // D3D11 / Vulkan handles; the bridge does not own their lifetime,
    // only borrows for the duration of stream playback.
    bool initialize(IDXGIAdapter1* dxgiAdapter,
                    ID3D11Device5* d3d11Device,
                    VkInstance vkInstance,
                    VkPhysicalDevice vkPhysDev,
                    VkDevice vkDevice,
                    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr,
                    HandleMode mode);

    bool isInitialized() const { return m_Initialized; }
    HandleMode handleMode() const { return m_Mode; }

    // Resolve ablation mode from env VIPLE_VKFRUC_D3D11_HEVC_MODE (A1..A4),
    // defaulting to A1 if unset / invalid.
    static HandleMode resolveHandleModeFromEnv();

    // §B B2 — open a SHARED_NTHANDLE on the supplied D3D11 array texture
    // (e.g. the FFmpeg D3D11VA decoder output pool).  D3D11 texture sharing
    // is per-resource, not per-slice, so the handle covers the whole array
    // and the Vulkan side picks slices via image-view layer index.  Handles
    // are cached by ID3D11Texture2D* so repeated decoder pool re-use does
    // not leak ~1 NT handle per frame.  outArraySize lets the caller
    // know how many layers the imported VkImage will have.
    bool exportTextureSlice(ID3D11Texture2D* texArray,
                            UINT slice,
                            HANDLE* outSharedHandle,
                            uint32_t* outArraySize);

    // §B B3 — import a D3D11 SHARED_NTHANDLE into Vulkan as a VkImage
    // backed by VkDeviceMemory.  Caller (VkFrucRenderer) owns the
    // returned VkImage / VkDeviceMemory lifetime — bridge does NOT track
    // them, so caller MUST vkDestroyImage / vkFreeMemory itself.  The
    // imported image is NV12 layout (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
    // with arraySize layers; caller picks per-frame slice via image-view
    // baseArrayLayer.  Handle type follows m_Mode (A1/A3 → D3D11_TEXTURE,
    // A2/A4 → D3D12_RESOURCE).
    bool importToVulkan(HANDLE sharedHandle,
                        VkFormat fmt,
                        uint32_t width,
                        uint32_t height,
                        uint32_t arraySize,
                        ImportedImage* out);

    // §B B4 — D3D11 ID3D11Fence ↔ Vulkan timeline VkSemaphore bridge.
    // Per-frame sync pattern (see §J.1 in ncnnfruc.cpp 1714-1739):
    //   ctx4->Signal(fence, N)       — D3D11 side "I'm done with N"
    //   vkQueueSubmit waits sem=N    — Vulkan waits for D3D11
    //   ... Vulkan ops on imported VkImage ...
    //   vkQueueSubmit signals sem=N+1 — Vulkan "I'm done with N+1"
    //   ctx4->Wait(fence, N+1)        — D3D11 side waits for Vulkan
    //   bridge.nextValue() +=2 per round
    //
    // Bridge owns the fence + semaphore; caller drives signal/wait through
    // the supplied ID3D11DeviceContext4 (D3D11 side) and self-assembled
    // VkTimelineSemaphoreSubmitInfo (Vulkan side, fetch sem via getter).
    bool createFenceSync();
    void destroyFenceSync();
    void* getD3D11Fence() const { return m_D3D11Fence; }  // ID3D11Fence*
    VkSemaphore getVkSemaphore() const { return m_VkSemaphore; }
    uint64_t nextValue() { return ++m_FenceValue; }       // atomic post-increment is enough
    uint64_t currentValue() const { return m_FenceValue.load(); }
    void signalFromD3D11(ID3D11DeviceContext4* ctx4, uint64_t v);
    void waitOnD3D11(ID3D11DeviceContext4* ctx4, uint64_t v);

private:
    bool loadVulkanWin32Pfns_();

    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_DxgiAdapter;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_D3D11Device;

    VkInstance       m_VkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice m_VkPhysDev  = VK_NULL_HANDLE;
    VkDevice         m_VkDevice   = VK_NULL_HANDLE;

    PFN_vkGetInstanceProcAddr           m_pfnGIPA                    = nullptr;
    PFN_vkGetDeviceProcAddr             m_pfnGDPA                    = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR m_pfnGetMemWin32HandleProps = nullptr;
    PFN_vkImportSemaphoreWin32HandleKHR m_pfnImportSemWin32Handle    = nullptr;
    PFN_vkCreateImage                   m_pfnCreateImage             = nullptr;
    PFN_vkDestroyImage                  m_pfnDestroyImage            = nullptr;
    PFN_vkGetImageMemoryRequirements    m_pfnGetImageMemReq          = nullptr;
    PFN_vkAllocateMemory                m_pfnAllocateMemory          = nullptr;
    PFN_vkFreeMemory                    m_pfnFreeMemory              = nullptr;
    PFN_vkBindImageMemory               m_pfnBindImageMemory         = nullptr;
    PFN_vkCreateSemaphore               m_pfnCreateSemaphore         = nullptr;
    PFN_vkDestroySemaphore              m_pfnDestroySemaphore        = nullptr;

    // §B B4 — D3D11 fence + Vulkan timeline semaphore pair.
    void*       m_D3D11Fence       = nullptr;  // ID3D11Fence* (Release in dtor)
    HANDLE      m_FenceSharedHandle = nullptr;
    VkSemaphore m_VkSemaphore      = VK_NULL_HANDLE;
    std::atomic<uint64_t> m_FenceValue { 0 };

    HandleMode m_Mode        = HandleMode::A1_D3D11_D3D12FENCE;
    bool       m_Initialized = false;

    // §B B2 — per-texture SHARED_NTHANDLE cache.  Decoder reuses the same
    // ID3D11Texture2D* across many frames (just different slices), so we
    // CreateSharedHandle once per texture instead of per frame.
    struct ExportEntry {
        HANDLE   sharedHandle;
        uint32_t arraySize;
    };
    std::mutex                                              m_ExportMutex;
    std::unordered_map<ID3D11Texture2D*, ExportEntry>       m_ExportCache;
};
