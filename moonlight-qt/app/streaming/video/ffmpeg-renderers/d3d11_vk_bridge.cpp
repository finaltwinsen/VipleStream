// §B Phase B 重啟 bridge implementation.  See d3d11_vk_bridge.h for context.

#include "d3d11_vk_bridge.h"

#include <SDL.h>
#include <cstring>

D3D11VkBridge::D3D11VkBridge() = default;

D3D11VkBridge::~D3D11VkBridge()
{
    // B4 — tear down fence sync first (so any caller-side sync attempt on
    // a half-destroyed bridge fails cleanly rather than UAF).
    destroyFenceSync();

    // B2 — close every cached shared handle.  Vulkan side's VkImage /
    // VkDeviceMemory teardown happens in VkFrucRenderer (which owns the
    // import descriptors) — bridge only owns the NT handle lifetime.
    {
        std::lock_guard<std::mutex> lk(m_ExportMutex);
        for (auto& kv : m_ExportCache) {
            if (kv.second.sharedHandle) {
                CloseHandle(kv.second.sharedHandle);
            }
        }
        m_ExportCache.clear();
    }
}

D3D11VkBridge::HandleMode D3D11VkBridge::resolveHandleModeFromEnv()
{
    const char* e = SDL_getenv("VIPLE_VKFRUC_D3D11_HEVC_MODE");
    if (!e || !*e) {
        return HandleMode::A1_D3D11_D3D12FENCE;
    }
    if (SDL_strcasecmp(e, "A1") == 0) return HandleMode::A1_D3D11_D3D12FENCE;
    if (SDL_strcasecmp(e, "A2") == 0) return HandleMode::A2_D3D12_D3D12FENCE;
    if (SDL_strcasecmp(e, "A3") == 0) return HandleMode::A3_D3D11_TOP_OF_PIPE;
    if (SDL_strcasecmp(e, "A4") == 0) return HandleMode::A4_D3D12_BRIDGE;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-COMPOSITE] unknown VIPLE_VKFRUC_D3D11_HEVC_MODE='%s' — "
                "falling back to A1 (D3D11_TEXTURE_BIT + D3D12_FENCE_BIT)", e);
    return HandleMode::A1_D3D11_D3D12FENCE;
}

bool D3D11VkBridge::initialize(IDXGIAdapter1* dxgiAdapter,
                               ID3D11Device5* d3d11Device,
                               VkInstance vkInstance,
                               VkPhysicalDevice vkPhysDev,
                               VkDevice vkDevice,
                               PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr,
                               HandleMode mode)
{
    if (m_Initialized) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-COMPOSITE] bridge.initialize called twice — ignoring");
        return true;
    }

    if (!dxgiAdapter || !d3d11Device || !vkInstance || !vkPhysDev ||
        !vkDevice || !pfnGetInstanceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] bridge.initialize null arg "
                     "(dxgi=%p d3d11=%p vkInst=%p vkPhys=%p vkDev=%p gipa=%p)",
                     dxgiAdapter, d3d11Device,
                     (void*)vkInstance, (void*)vkPhysDev, (void*)vkDevice,
                     (void*)pfnGetInstanceProcAddr);
        return false;
    }

    m_DxgiAdapter = dxgiAdapter;
    m_D3D11Device = d3d11Device;
    m_VkInstance  = vkInstance;
    m_VkPhysDev   = vkPhysDev;
    m_VkDevice    = vkDevice;
    m_pfnGIPA     = pfnGetInstanceProcAddr;
    m_Mode        = mode;

    // §B B1 — LUID match.  AMD 780M 整台機器只有一張 GPU，這個 check 在那台
    // 不會 fail；但 dGPU+iGPU 並存的機器（laptop hybrid graphics）會挑錯卡，
    // 必須擋掉.
    DXGI_ADAPTER_DESC1 dxgiDesc = {};
    HRESULT hr = m_DxgiAdapter->GetDesc1(&dxgiDesc);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] IDXGIAdapter1::GetDesc1 failed 0x%08lx",
                     hr);
        return false;
    }

    auto pfnGetPhysProps2 = (PFN_vkGetPhysicalDeviceProperties2)
        m_pfnGIPA(m_VkInstance, "vkGetPhysicalDeviceProperties2");
    if (!pfnGetPhysProps2) {
        pfnGetPhysProps2 = (PFN_vkGetPhysicalDeviceProperties2)
            m_pfnGIPA(m_VkInstance, "vkGetPhysicalDeviceProperties2KHR");
    }
    if (!pfnGetPhysProps2) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] vkGetPhysicalDeviceProperties2 PFN "
                     "missing — driver too old, bridge unusable");
        return false;
    }

    VkPhysicalDeviceIDProperties idProps = {};
    idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 props2 = {};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &idProps;
    pfnGetPhysProps2(m_VkPhysDev, &props2);

    if (!idProps.deviceLUIDValid) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] VkPhysicalDeviceIDProperties."
                     "deviceLUIDValid=false — cannot match adapter, bridge unusable");
        return false;
    }

    static_assert(sizeof(idProps.deviceLUID) == sizeof(dxgiDesc.AdapterLuid),
                  "Vulkan deviceLUID size must equal DXGI AdapterLuid size (8)");
    if (memcmp(idProps.deviceLUID, &dxgiDesc.AdapterLuid,
               sizeof(dxgiDesc.AdapterLuid)) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] LUID mismatch — D3D11 adapter "
                     "'%ls' (vendor=0x%04x) and Vulkan physical device are on "
                     "different GPUs.  Bridge unusable.",
                     dxgiDesc.Description, dxgiDesc.VendorId);
        return false;
    }

    // PFN resolve for B3 (memory import) and B4 (semaphore import).  These
    // ext PFNs only exist when VK_KHR_external_memory_win32 /
    // VK_KHR_external_semaphore_win32 are enabled at vkCreateDevice time.
    // VkFrucRenderer 既有 ext enable list 沒帶這兩個，B5 接通時要補上.
    if (!loadVulkanWin32Pfns_()) {
        return false;
    }

    const char* modeStr =
        (m_Mode == HandleMode::A1_D3D11_D3D12FENCE) ? "A1 (D3D11_TEXTURE + D3D12_FENCE)" :
        (m_Mode == HandleMode::A2_D3D12_D3D12FENCE) ? "A2 (D3D12_RESOURCE + D3D12_FENCE)" :
        (m_Mode == HandleMode::A3_D3D11_TOP_OF_PIPE) ? "A3 (D3D11_TEXTURE + TOP_OF_PIPE wait)" :
                                                       "A4 (D3D12 reopen bridge)";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-COMPOSITE] B1 bridge.initialize OK — adapter='%ls' "
                "vendor=0x%04x mode=%s",
                dxgiDesc.Description, dxgiDesc.VendorId, modeStr);

    m_Initialized = true;
    return true;
}

bool D3D11VkBridge::exportTextureSlice(ID3D11Texture2D* texArray,
                                       UINT slice,
                                       HANDLE* outSharedHandle,
                                       uint32_t* outArraySize)
{
    if (!m_Initialized || !texArray || !outSharedHandle || !outArraySize) {
        return false;
    }
    *outSharedHandle = nullptr;
    *outArraySize    = 0;

    {
        std::lock_guard<std::mutex> lk(m_ExportMutex);
        auto it = m_ExportCache.find(texArray);
        if (it != m_ExportCache.end()) {
            *outSharedHandle = it->second.sharedHandle;
            *outArraySize    = it->second.arraySize;
            (void)slice;  // slice is solved on the Vulkan view side, not export
            return true;
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texArray->GetDesc(&desc);
    if (slice >= desc.ArraySize) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] exportTextureSlice: slice=%u out of "
                     "range (arraySize=%u)", slice, desc.ArraySize);
        return false;
    }
    if ((desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) == 0 ||
        (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) == 0) {
        // Decoder pool wasn't created with SHARED | SHARED_NTHANDLE.  This
        // is exactly the gate B5/B6 widens on D3D11VARenderer's
        // frames-context — log once so we can spot mis-wiring.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] exportTextureSlice: decoder "
                     "texture missing SHARED|SHARED_NTHANDLE MiscFlags=0x%x — "
                     "D3D11VARenderer::prepareDecoderContext did not opt in",
                     desc.MiscFlags);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIResource1> dxgi;
    HRESULT hr = texArray->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] exportTextureSlice: "
                     "QI<IDXGIResource1> failed 0x%08lx", hr);
        return false;
    }

    HANDLE sharedHandle = nullptr;
    hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedHandle);
    if (FAILED(hr) || !sharedHandle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] exportTextureSlice: "
                     "CreateSharedHandle failed 0x%08lx", hr);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_ExportMutex);
        // Double-check after lock — another thread might have just cached.
        auto it = m_ExportCache.find(texArray);
        if (it != m_ExportCache.end()) {
            CloseHandle(sharedHandle);
            *outSharedHandle = it->second.sharedHandle;
            *outArraySize    = it->second.arraySize;
            return true;
        }
        m_ExportCache.emplace(texArray, ExportEntry{ sharedHandle, desc.ArraySize });
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-COMPOSITE] B2 exportTextureSlice: cached new shared "
                "handle for tex=%p (arraySize=%u format=%d %ux%u)",
                texArray, desc.ArraySize, (int)desc.Format,
                desc.Width, desc.Height);
    *outSharedHandle = sharedHandle;
    *outArraySize    = desc.ArraySize;
    return true;
}

bool D3D11VkBridge::loadVulkanWin32Pfns_()
{
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        m_pfnGIPA(m_VkInstance, "vkGetDeviceProcAddr");
    if (!pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] vkGetDeviceProcAddr PFN missing");
        return false;
    }
    m_pfnGDPA = pfnGetDeviceProcAddr;

    m_pfnGetMemWin32HandleProps = (PFN_vkGetMemoryWin32HandlePropertiesKHR)
        m_pfnGDPA(m_VkDevice, "vkGetMemoryWin32HandlePropertiesKHR");
    m_pfnImportSemWin32Handle = (PFN_vkImportSemaphoreWin32HandleKHR)
        m_pfnGDPA(m_VkDevice, "vkImportSemaphoreWin32HandleKHR");

    if (!m_pfnGetMemWin32HandleProps || !m_pfnImportSemWin32Handle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] missing Win32-external PFN: "
                     "vkGetMemoryWin32HandlePropertiesKHR=%p "
                     "vkImportSemaphoreWin32HandleKHR=%p — VkDevice was not "
                     "created with VK_KHR_external_memory_win32 + "
                     "VK_KHR_external_semaphore_win32 enabled.",
                     (void*)m_pfnGetMemWin32HandleProps,
                     (void*)m_pfnImportSemWin32Handle);
        return false;
    }

    m_pfnCreateImage      = (PFN_vkCreateImage)               m_pfnGDPA(m_VkDevice, "vkCreateImage");
    m_pfnDestroyImage     = (PFN_vkDestroyImage)              m_pfnGDPA(m_VkDevice, "vkDestroyImage");
    m_pfnGetImageMemReq   = (PFN_vkGetImageMemoryRequirements)m_pfnGDPA(m_VkDevice, "vkGetImageMemoryRequirements");
    m_pfnAllocateMemory   = (PFN_vkAllocateMemory)            m_pfnGDPA(m_VkDevice, "vkAllocateMemory");
    m_pfnFreeMemory       = (PFN_vkFreeMemory)                m_pfnGDPA(m_VkDevice, "vkFreeMemory");
    m_pfnBindImageMemory  = (PFN_vkBindImageMemory)           m_pfnGDPA(m_VkDevice, "vkBindImageMemory");
    m_pfnCreateSemaphore  = (PFN_vkCreateSemaphore)           m_pfnGDPA(m_VkDevice, "vkCreateSemaphore");
    m_pfnDestroySemaphore = (PFN_vkDestroySemaphore)          m_pfnGDPA(m_VkDevice, "vkDestroySemaphore");

    if (!m_pfnCreateImage || !m_pfnDestroyImage || !m_pfnGetImageMemReq ||
        !m_pfnAllocateMemory || !m_pfnFreeMemory || !m_pfnBindImageMemory ||
        !m_pfnCreateSemaphore || !m_pfnDestroySemaphore) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] core image/memory/semaphore PFN load "
                     "failed (createImage=%p destroyImage=%p getMemReq=%p alloc=%p "
                     "free=%p bindImg=%p createSem=%p destroySem=%p)",
                     (void*)m_pfnCreateImage, (void*)m_pfnDestroyImage,
                     (void*)m_pfnGetImageMemReq, (void*)m_pfnAllocateMemory,
                     (void*)m_pfnFreeMemory, (void*)m_pfnBindImageMemory,
                     (void*)m_pfnCreateSemaphore, (void*)m_pfnDestroySemaphore);
        return false;
    }

    return true;
}

bool D3D11VkBridge::createFenceSync()
{
    if (!m_Initialized) {
        return false;
    }
    if (m_D3D11Fence || m_VkSemaphore != VK_NULL_HANDLE) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC-COMPOSITE] B4 createFenceSync called twice");
        return true;
    }

    // ---- D3D11 side: ID3D11Fence + shared NT handle ----------------------
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    HRESULT hr = m_D3D11Device->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                            IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B4 ID3D11Device5::CreateFence "
                     "rc=0x%08lx", hr);
        return false;
    }
    HANDLE fenceHandle = nullptr;
    hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (FAILED(hr) || !fenceHandle) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B4 ID3D11Fence::CreateSharedHandle "
                     "rc=0x%08lx", hr);
        return false;
    }

    // ---- Vulkan side: open as timeline semaphore -------------------------
    VkSemaphoreTypeCreateInfo semType = {};
    semType.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semType.initialValue  = 0;

    VkExportSemaphoreCreateInfo expInfo = {};
    expInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    expInfo.pNext       = &semType;
    expInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

    VkSemaphoreCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &expInfo;

    VkSemaphore sem = VK_NULL_HANDLE;
    VkResult vr = m_pfnCreateSemaphore(m_VkDevice, &sci, nullptr, &sem);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B4 vkCreateSemaphore (timeline + "
                     "D3D12_FENCE_BIT external) rc=%d — VkDevice needs "
                     "timelineSemaphore feature + VK_KHR_external_semaphore_win32",
                     (int)vr);
        CloseHandle(fenceHandle);
        return false;
    }

    VkImportSemaphoreWin32HandleInfoKHR imp = {};
    imp.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    imp.semaphore  = sem;
    imp.flags      = 0;  // permanent (not temporary)
    imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    imp.handle     = fenceHandle;
    imp.name       = nullptr;

    vr = m_pfnImportSemWin32Handle(m_VkDevice, &imp);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B4 vkImportSemaphoreWin32HandleKHR "
                     "rc=%d (D3D12_FENCE_BIT handle type rejected — NV 596.84 saw "
                     "this before fence-sync work; AMD may differ)",
                     (int)vr);
        m_pfnDestroySemaphore(m_VkDevice, sem, nullptr);
        CloseHandle(fenceHandle);
        return false;
    }

    m_D3D11Fence        = fence.Detach();
    m_FenceSharedHandle = fenceHandle;
    m_VkSemaphore       = sem;
    m_FenceValue.store(0);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-COMPOSITE] B4 createFenceSync READY — "
                "ID3D11Fence=%p VkSemaphore=%p sharedHandle=%p",
                m_D3D11Fence, (void*)m_VkSemaphore, m_FenceSharedHandle);
    return true;
}

void D3D11VkBridge::destroyFenceSync()
{
    if (m_VkSemaphore != VK_NULL_HANDLE && m_pfnDestroySemaphore && m_VkDevice) {
        m_pfnDestroySemaphore(m_VkDevice, m_VkSemaphore, nullptr);
        m_VkSemaphore = VK_NULL_HANDLE;
    }
    if (m_FenceSharedHandle) {
        CloseHandle(m_FenceSharedHandle);
        m_FenceSharedHandle = nullptr;
    }
    if (m_D3D11Fence) {
        static_cast<ID3D11Fence*>(m_D3D11Fence)->Release();
        m_D3D11Fence = nullptr;
    }
    m_FenceValue.store(0);
}

void D3D11VkBridge::signalFromD3D11(ID3D11DeviceContext4* ctx4, uint64_t v)
{
    if (!ctx4 || !m_D3D11Fence) return;
    ctx4->Signal(static_cast<ID3D11Fence*>(m_D3D11Fence), v);
}

void D3D11VkBridge::waitOnD3D11(ID3D11DeviceContext4* ctx4, uint64_t v)
{
    if (!ctx4 || !m_D3D11Fence) return;
    ctx4->Wait(static_cast<ID3D11Fence*>(m_D3D11Fence), v);
}

bool D3D11VkBridge::importToVulkan(HANDLE sharedHandle,
                                   VkFormat fmt,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t arraySize,
                                   ImportedImage* out)
{
    if (!m_Initialized || !sharedHandle || !out || arraySize == 0 ||
        width == 0 || height == 0) {
        return false;
    }
    out->image  = VK_NULL_HANDLE;
    out->memory = VK_NULL_HANDLE;

    // §B B3 — choose handle type from ablation mode.  A1/A3 view the
    // resource as a D3D11 texture; A2 reinterprets it as a D3D12 resource
    // (some drivers — historically NV — only accept the D3D12 family for
    // raw Vulkan ops on D3D-imported memory).  A4 would reopen via an
    // actual ID3D12Device; that path is still TODO (B7 ablation).
    VkExternalMemoryHandleTypeFlagBits hType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    if (m_Mode == HandleMode::A2_D3D12_D3D12FENCE ||
        m_Mode == HandleMode::A4_D3D12_BRIDGE) {
        hType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    }

    VkExternalMemoryImageCreateInfo extImg = {};
    extImg.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImg.handleTypes = hType;

    VkImageCreateInfo ici = {};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = &extImg;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent        = { width, height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = arraySize;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkResult vr = m_pfnCreateImage(m_VkDevice, &ici, nullptr, &image);
    if (vr != VK_SUCCESS || !image) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B3 vkCreateImage rc=%d "
                     "(fmt=%d %ux%u layers=%u handleType=0x%x)",
                     (int)vr, (int)fmt, width, height, arraySize, (unsigned)hType);
        return false;
    }

    VkMemoryRequirements memReq = {};
    m_pfnGetImageMemReq(m_VkDevice, image, &memReq);

    // Determine which Vulkan memory type the imported handle is compatible
    // with.  Spec: intersect (memReq.memoryTypeBits, GetMemoryWin32HandleProps.
    // memoryTypeBits) then prefer DEVICE_LOCAL.
    VkMemoryWin32HandlePropertiesKHR hprops = {};
    hprops.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    vr = m_pfnGetMemWin32HandleProps(m_VkDevice, hType, sharedHandle, &hprops);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B3 vkGetMemoryWin32HandlePropertiesKHR "
                     "rc=%d (handleType=0x%x) — driver rejects shared handle",
                     (int)vr, (unsigned)hType);
        m_pfnDestroyImage(m_VkDevice, image, nullptr);
        return false;
    }
    uint32_t typeBits = memReq.memoryTypeBits & hprops.memoryTypeBits;
    if (typeBits == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B3 no memory type intersects "
                     "(imageReq=0x%x handle=0x%x)",
                     memReq.memoryTypeBits, hprops.memoryTypeBits);
        m_pfnDestroyImage(m_VkDevice, image, nullptr);
        return false;
    }
    // Pick the lowest-index bit — fine for imported memory where all bits
    // come from the same physical allocation.
    uint32_t memTypeIndex = 0;
    while ((typeBits & (1u << memTypeIndex)) == 0) memTypeIndex++;

    VkImportMemoryWin32HandleInfoKHR importInfo = {};
    importInfo.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importInfo.handleType = hType;
    importInfo.handle     = sharedHandle;
    importInfo.name       = nullptr;  // kernel handle, no name

    // Spec strongly recommends dedicated allocation for imported D3D
    // resources; some drivers (incl. AMD historically) require it.
    VkMemoryDedicatedAllocateInfo dedicated = {};
    dedicated.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image  = image;
    dedicated.pNext  = &importInfo;

    VkMemoryAllocateInfo mai = {};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &dedicated;
    mai.allocationSize  = memReq.size;
    mai.memoryTypeIndex = memTypeIndex;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    vr = m_pfnAllocateMemory(m_VkDevice, &mai, nullptr, &mem);
    if (vr != VK_SUCCESS || !mem) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B3 vkAllocateMemory rc=%d "
                     "(size=%llu typeIdx=%u handleType=0x%x) — driver-side reject",
                     (int)vr, (unsigned long long)memReq.size, memTypeIndex,
                     (unsigned)hType);
        m_pfnDestroyImage(m_VkDevice, image, nullptr);
        return false;
    }

    vr = m_pfnBindImageMemory(m_VkDevice, image, mem, 0);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC-COMPOSITE] B3 vkBindImageMemory rc=%d",
                     (int)vr);
        m_pfnFreeMemory(m_VkDevice, mem, nullptr);
        m_pfnDestroyImage(m_VkDevice, image, nullptr);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC-COMPOSITE] B3 importToVulkan OK — image=%p "
                "mem=%p (fmt=%d %ux%u layers=%u typeIdx=%u handleType=0x%x size=%llu)",
                (void*)image, (void*)mem,
                (int)fmt, width, height, arraySize, memTypeIndex,
                (unsigned)hType, (unsigned long long)memReq.size);

    out->image  = image;
    out->memory = mem;
    out->width  = width;
    out->height = height;
    out->format = fmt;
    return true;
}
