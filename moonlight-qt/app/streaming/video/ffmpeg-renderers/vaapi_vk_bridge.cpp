// §K.linux VAAPI→Vulkan DMA-BUF bridge 實作
// 見 vaapi_vk_bridge.h 設計說明。

#include "vaapi_vk_bridge.h"

#include <SDL_log.h>
#include <unistd.h>
#include <cstring>
#include <vector>

// DRM_FORMAT_MOD_LINEAR = 0
static constexpr uint64_t k_ModLinear = 0;

VAAPIVkBridge::VAAPIVkBridge()  = default;
VAAPIVkBridge::~VAAPIVkBridge() = default;

bool VAAPIVkBridge::initialize(VkInstance instance,
                               VkPhysicalDevice physDev,
                               VkDevice device,
                               PFN_vkGetInstanceProcAddr pfnGIPA)
{
    m_Instance = instance;
    m_PhysDev  = physDev;
    m_Device   = device;
    m_pfnGIPA  = pfnGIPA;

    if (!loadPFNs_()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] initialize: PFN load failed");
        return false;
    }
    m_Initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VAAPI-VK] bridge initialized OK");
    return true;
}

bool VAAPIVkBridge::loadPFNs_()
{
    m_pfnGDPA = (PFN_vkGetDeviceProcAddr)
        m_pfnGIPA(m_Instance, "vkGetDeviceProcAddr");
    if (!m_pfnGDPA) return false;

#define LOAD_DEV(name) \
    m_pfn##name = (PFN_vk##name)m_pfnGDPA(m_Device, "vk" #name); \
    if (!m_pfn##name) { \
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, \
                     "[VIPLE-VAAPI-VK] missing PFN: vk" #name); \
        return false; \
    }

    // 縮寫成員名稱無法透過 LOAD_DEV macro 展開，手動 load。
    m_pfnGetMemFdProps = (PFN_vkGetMemoryFdPropertiesKHR)
        m_pfnGDPA(m_Device, "vkGetMemoryFdPropertiesKHR");
    if (!m_pfnGetMemFdProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] missing PFN: vkGetMemoryFdPropertiesKHR");
        return false;
    }
    m_pfnGetImageMemReq = (PFN_vkGetImageMemoryRequirements)
        m_pfnGDPA(m_Device, "vkGetImageMemoryRequirements");
    if (!m_pfnGetImageMemReq) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] missing PFN: vkGetImageMemoryRequirements");
        return false;
    }
    // instance-level function — 從 GIPA 以 VK_NULL_HANDLE instance 查
    m_pfnGetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
        m_pfnGIPA(VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    if (!m_pfnGetPhysMemProps) {
        // 嘗試 instance-specific 路徑
        m_pfnGetPhysMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)
            m_pfnGIPA(m_Instance, "vkGetPhysicalDeviceMemoryProperties");
    }
    if (!m_pfnGetPhysMemProps) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] missing PFN: vkGetPhysicalDeviceMemoryProperties");
        return false;
    }

    LOAD_DEV(CreateImage)
    LOAD_DEV(DestroyImage)
    LOAD_DEV(AllocateMemory)
    LOAD_DEV(FreeMemory)
    LOAD_DEV(BindImageMemory)
#undef LOAD_DEV

    return true;
}

bool VAAPIVkBridge::importFrame(int dmaFd, uint32_t dmaSize, uint64_t modifier,
                                uint32_t width, uint32_t height,
                                ImportedFrame* out)
{
    if (!m_Initialized) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] importFrame: not initialized");
        return false;
    }

    if (modifier != k_ModLinear) {
        // pre-req spike 確認 Vega 10 是 LINEAR；non-linear 路徑留待未來擴充。
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] importFrame: non-linear modifier "
                     "0x%016llx not yet supported",
                     (unsigned long long)modifier);
        return false;
    }

    // --- 建立 LINEAR NV12 VkImage ---
    VkExternalMemoryImageCreateInfo extInfo = {};
    extInfo.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageCreateInfo imgInfo = {};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext         = &extInfo;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    imgInfo.extent        = { width, height, 1 };
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_LINEAR;
    imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkResult res = m_pfnCreateImage(m_Device, &imgInfo, nullptr, &image);
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] vkCreateImage failed: %d", res);
        return false;
    }

    // --- 確認 DMA-BUF fd 與這個 VkImage 的 memory type 相容 ---
    VkMemoryFdPropertiesKHR fdProps = {};
    fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    res = m_pfnGetMemFdProps(m_Device,
                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                             dmaFd, &fdProps);
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] vkGetMemoryFdPropertiesKHR failed: %d", res);
        m_pfnDestroyImage(m_Device, image, nullptr);
        return false;
    }

    VkMemoryRequirements memReq = {};
    m_pfnGetImageMemReq(m_Device, image, &memReq);

    // 從 physDev 找一個 device-local 或 host-visible 且兩邊都接受的 type。
    VkPhysicalDeviceMemoryProperties memProps = {};
    m_pfnGetPhysMemProps(m_PhysDev, &memProps);

    uint32_t typeBits = memReq.memoryTypeBits & fdProps.memoryTypeBits;
    uint32_t typeIdx  = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i))) continue;
        typeIdx = i;
        break;
    }
    if (typeIdx == UINT32_MAX) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] no compatible memory type "
                     "(memReq.typeBits=0x%x fdProps.typeBits=0x%x)",
                     memReq.memoryTypeBits, fdProps.memoryTypeBits);
        m_pfnDestroyImage(m_Device, image, nullptr);
        return false;
    }

    // dup() fd：vkAllocateMemory 消耗它的 fd ref；caller 的原始 fd 維持 VA surface 所有權。
    int importFd = dup(dmaFd);
    if (importFd < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] dup(dmaFd) failed");
        m_pfnDestroyImage(m_Device, image, nullptr);
        return false;
    }

    VkImportMemoryFdInfoKHR importInfo = {};
    importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd         = importFd;  // Vulkan consumes this

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext           = &importInfo;
    allocInfo.allocationSize  = dmaSize;  // DMA-BUF 實際大小，非 memReq.size
    allocInfo.memoryTypeIndex = typeIdx;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    res = m_pfnAllocateMemory(m_Device, &allocInfo, nullptr, &memory);
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] vkAllocateMemory failed: %d "
                     "(dmaSize=%u typeIdx=%u)", res, dmaSize, typeIdx);
        close(importFd);  // AllocateMemory 失敗時 fd 未被消耗
        m_pfnDestroyImage(m_Device, image, nullptr);
        return false;
    }
    // importFd 已由 Vulkan 消耗，不 close()

    res = m_pfnBindImageMemory(m_Device, image, memory, 0);
    if (res != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VAAPI-VK] vkBindImageMemory failed: %d", res);
        m_pfnFreeMemory(m_Device, memory, nullptr);
        m_pfnDestroyImage(m_Device, image, nullptr);
        return false;
    }

    out->image  = image;
    out->memory = memory;
    out->width  = width;
    out->height = height;
    return true;
}
