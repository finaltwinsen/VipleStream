// VipleStream §I.F — NCNN-Vulkan FRUC backend (phase 1: probe-only).

#ifdef _WIN32

#include "ncnnfruc.h"
#include "ncnn_rife_warp.h"
#include "path.h"

#include <SDL.h>
#include <QDir>

#include <dxgi1_2.h>
#include <wrl/client.h>

// vulkan_win32.h pulls in the Win32-specific structs (VkImportMemoryWin32HandleInfoKHR,
// VkMemoryWin32HandlePropertiesKHR) and PFN typedefs that vulkan_core.h alone doesn't.
// Define VK_USE_PLATFORM_WIN32_KHR so vulkan.h opts those into its include chain.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#include <vulkan/vulkan_win32.h>

#include <ncnn/net.h>
#include <ncnn/gpu.h>
#include <ncnn/mat.h>
#include <ncnn/pipeline.h>
#include <ncnn/option.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Median of a small float vector (no need for nth_element optimisation).
double median(std::vector<double> xs)
{
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    return xs[xs.size() / 2];
}

// Lazy global ncnn Vulkan-instance handle.  ncnn requires
// ncnn::create_gpu_instance() be called before any Vulkan work and
// destroy_gpu_instance() at shutdown.  We refcount across NcnnFRUC
// instances since the cascade may construct/destruct several during
// probe and only the last destruction should tear down Vulkan.
std::atomic<int> g_NcnnGpuRefcount { 0 };

bool acquireNcnnGpu()
{
    if (g_NcnnGpuRefcount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        if (ncnn::create_gpu_instance() != 0) {
            g_NcnnGpuRefcount.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
    }
    return true;
}

void releaseNcnnGpu()
{
    if (g_NcnnGpuRefcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        ncnn::destroy_gpu_instance();
    }
}

} // namespace

NcnnFRUC::NcnnFRUC() = default;

NcnnFRUC::~NcnnFRUC()
{
    destroy();
}

bool NcnnFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    if (!device || width == 0 || height == 0) return false;

    m_Device = device;
    m_Width  = width;
    m_Height = height;

    // Acquire the global ncnn Vulkan instance.  Failure here means no
    // Vulkan loader / no compatible GPU — caller will fall through to
    // DirectMLFRUC / GenericFRUC.
    if (!acquireNcnnGpu()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] ncnn::create_gpu_instance() failed — "
                    "Vulkan loader missing or no compatible GPU");
        return false;
    }

    const int gpuCount = ncnn::get_gpu_count();
    if (gpuCount <= 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] no Vulkan-capable GPU enumerated by ncnn");
        releaseNcnnGpu();
        return false;
    }

    // Pick GPU 0 by default.  Future iteration: pick the same GPU
    // that's running the D3D11 renderer (LUID match) so the eventual
    // shared-texture bridge doesn't have to cross GPUs.
    m_GpuIndex = 0;
    const ncnn::GpuInfo& gi = ncnn::get_gpu_info(m_GpuIndex);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] picked GPU %d: %s "
                "(api=%u.%u driver=%u.%u, fp16-pack=%d/storage=%d/arith=%d, "
                "subgroup=%u, queueC=%u/T=%u)",
                m_GpuIndex, gi.device_name(),
                VK_VERSION_MAJOR(gi.api_version()),
                VK_VERSION_MINOR(gi.api_version()),
                VK_VERSION_MAJOR(gi.driver_version()),
                VK_VERSION_MINOR(gi.driver_version()),
                gi.support_fp16_packed() ? 1 : 0,
                gi.support_fp16_storage() ? 1 : 0,
                gi.support_fp16_arithmetic() ? 1 : 0,
                gi.subgroup_size(),
                gi.compute_queue_count(),
                gi.transfer_queue_count());

    if (!loadModel()) {
        releaseNcnnGpu();
        return false;
    }

    // §I.F Phase A.5 (v1.3.21) — full D3D11 texture set for CPU-staged
    // bridge between D3D11 (caller) and ncnn::Mat (NCNN Vulkan).
    //
    // Render → caller writes decoded RGBA8 frame here (RTV).
    // Staging-curr → CopyResource() target, CPU-readable, used to lift
    //                pixels into m_PrevMat for the next inference.
    // Output → Vulkan-side interp result lands here (after staging-out
    //          → CopyResource), caller blits to swapchain via SRV.
    // Staging-out → CPU-writable, holds ncnn output until CopyResource.
    // Phase B.2: SHARED_NTHANDLE on render + output textures so we
    // can export DXGI handles for Vulkan import (B.3).  The flag set
    // is identical to DirectMLFRUC's createD3D11Textures (which uses
    // the same handles for D3D12 / DML EP wrap).
    D3D11_TEXTURE2D_DESC renderDesc = {};
    renderDesc.Width = m_Width;
    renderDesc.Height = m_Height;
    renderDesc.MipLevels = 1;
    renderDesc.ArraySize = 1;
    renderDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderDesc.SampleDesc.Count = 1;
    renderDesc.Usage = D3D11_USAGE_DEFAULT;
    renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    renderDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(m_Device->CreateTexture2D(&renderDesc, nullptr, m_RenderTex.GetAddressOf())) ||
        FAILED(m_Device->CreateRenderTargetView(m_RenderTex.Get(), nullptr, m_RenderRTV.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_RenderTex.Get(), nullptr, m_LastSRV.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] render-tex creation failed");
        destroy();
        return false;
    }

    D3D11_TEXTURE2D_DESC outDesc = renderDesc;
    outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_Device->CreateTexture2D(&outDesc, nullptr, m_OutputTex.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_OutputTex.Get(), nullptr, m_OutputSRV.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] output-tex creation failed");
        destroy();
        return false;
    }

    // Export DXGI NT handles — to be opened by Vulkan in B.3.
    // CreateSharedHandle on a DXGIResource1 with name=nullptr,
    // access=GENERIC_ALL gives an unnamed NT handle ownable by us.
    auto exportHandle = [&](ID3D11Texture2D* tex, HANDLE* outHandle, const char* label) -> bool {
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgi;
        if (FAILED(tex->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] QueryInterface(IDXGIResource1) failed for %s", label);
            return false;
        }
        HRESULT hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, outHandle);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] CreateSharedHandle(%s) failed 0x%08lx",
                        label, hr);
            return false;
        }
        return true;
    };
    if (!exportHandle(m_RenderTex.Get(), &m_SharedRenderHandle, "render") ||
        !exportHandle(m_OutputTex.Get(), &m_SharedOutputHandle, "output")) {
        // Phase A.5 CPU-staging path still works without these handles,
        // so don't fail init — just log + continue.  B.3+ shared path
        // checks m_SharedPathReady before attempting the GPU shortcut.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] DXGI handle export failed — staying on CPU staging path");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] DXGI shared handles exported "
                    "(render=%p, output=%p) — ready for Vulkan import",
                    m_SharedRenderHandle, m_SharedOutputHandle);
        // Phase B.3: try to wire the Vulkan side now.  If it works
        // m_SharedPathReady is set and Phase B.4 (preprocess shader)
        // can take over from CPU staging.
        if (importSharedTexturesIntoVulkan()) {
            // Phase B.4a — try to compile the GPU conversion shaders.
            // Failure is non-fatal: shaders not present means
            // m_SharedPathReady stays false and CPU staging continues
            // to operate as it does today.  m_SharedPathReady itself
            // doesn't flip until B.4b wires submitFrame to use the
            // dispatched pipelines.
            compileSharedPathPipelines();
        }
    }

    // STAGING textures must NOT have SHARED_NTHANDLE — D3D11 rejects
    // the combination.  Copy from renderDesc but clear MiscFlags.
    D3D11_TEXTURE2D_DESC stagingCurrDesc = renderDesc;
    stagingCurrDesc.Usage = D3D11_USAGE_STAGING;
    stagingCurrDesc.BindFlags = 0;
    stagingCurrDesc.MiscFlags = 0;
    stagingCurrDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_Device->CreateTexture2D(&stagingCurrDesc, nullptr, m_StagingCurrTex.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] staging-curr-tex creation failed");
        destroy();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingOutDesc = renderDesc;
    stagingOutDesc.Usage = D3D11_USAGE_STAGING;
    stagingOutDesc.BindFlags = 0;
    stagingOutDesc.MiscFlags = 0;
    stagingOutDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_Device->CreateTexture2D(&stagingOutDesc, nullptr, m_StagingOutTex.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] staging-out-tex creation failed");
        destroy();
        return false;
    }

    // Pre-build the constant-fill timestep input (1×1×H×W = 0.5 for
    // midpoint interpolation between prev and curr frames).  RIFE
    // 4.25-lite reads it as in2.
    m_TimestepMat = ncnn::Mat(static_cast<int>(m_Width), static_cast<int>(m_Height), 1);
    m_TimestepMat.fill(0.5f);
    m_FrameCount = 0;

    // Phase B.1 capability probe — log whether the Vulkan device that
    // ncnn picked supports the bits we need to bypass CPU staging in
    // a future phase B.2-5: external memory win32 import (so a D3D11
    // SHARED_NTHANDLE texture can be opened as a VkImage on the same
    // physical adapter).  We can't link vulkan-1.lib (would need the
    // Vulkan SDK installed), so dynamically load via vulkan-1.dll
    // (always present on Win10 1903+ since ncnn's already running).
    {
        HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (!vk) vk = ::LoadLibraryW(L"vulkan-1.dll");
        if (vk) {
            auto pfnGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
            auto pfnEnumDeviceExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                ::GetProcAddress(vk, "vkEnumerateDeviceExtensionProperties"));

            const VkPhysicalDevice phys = gi.physical_device();
            const VkDevice         dev  = ncnn::get_gpu_device(m_GpuIndex)->vkdevice();

            bool extMemWin32 = false, extMem = false;
            bool memReq2 = false, dedicatedAlloc = false;
            if (pfnEnumDeviceExt) {
                uint32_t extCount = 0;
                pfnEnumDeviceExt(phys, nullptr, &extCount, nullptr);
                std::vector<VkExtensionProperties> exts(extCount);
                pfnEnumDeviceExt(phys, nullptr, &extCount, exts.data());
                auto hasExt = [&](const char* name) {
                    for (const auto& e : exts) {
                        if (strcmp(e.extensionName, name) == 0) return true;
                    }
                    return false;
                };
                extMemWin32    = hasExt("VK_KHR_external_memory_win32");
                extMem         = hasExt("VK_KHR_external_memory");
                memReq2        = hasExt("VK_KHR_get_memory_requirements2");
                dedicatedAlloc = hasExt("VK_KHR_dedicated_allocation");
            }

            void* fpImport = nullptr;
            void* fpProps  = nullptr;
            if (pfnGetDeviceProcAddr && dev) {
                fpImport = (void*)pfnGetDeviceProcAddr(dev, "vkGetMemoryWin32HandleKHR");
                fpProps  = (void*)pfnGetDeviceProcAddr(dev, "vkGetMemoryWin32HandlePropertiesKHR");
            }

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B capability probe: "
                        "ext_mem_win32=%d ext_mem=%d mem_req2=%d ded_alloc=%d "
                        "fp_get_handle=%d fp_get_handle_props=%d",
                        extMemWin32, extMem, memReq2, dedicatedAlloc,
                        fpImport != nullptr, fpProps != nullptr);

            if (extMemWin32 && fpImport && fpProps && extMem) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC-NCNN] phase-B feasible — D3D11→VkImage shared-texture path can be wired");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC-NCNN] phase-B BLOCKED — fall back to CPU staging permanently on this device");
            }
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B probe skipped: vulkan-1.dll not loadable");
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] initialized: %ux%u, model=%s "
                "(phase 1: probe-only, submitFrame is a no-op until shared-texture lands)",
                m_Width, m_Height, m_ModelDir.c_str());

    m_Initialized.store(true, std::memory_order_release);
    return true;
}

bool NcnnFRUC::loadModel()
{
    QString paramPath = Path::getDataFilePath(
        QString::fromStdString(m_ModelDir + "/flownet.param"));
    QString binPath = Path::getDataFilePath(
        QString::fromStdString(m_ModelDir + "/flownet.bin"));

    if (paramPath.isEmpty() || binPath.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] RIFE model not found in data path: %s",
                    m_ModelDir.c_str());
        return false;
    }

    m_Net = std::make_unique<ncnn::Net>();
    m_Net->opt.use_vulkan_compute  = true;
    // RIFE 4.25-lite is fp16-trained without int8 calibration, so we
    // can't use rife-ncnn-vulkan's int8_storage trick (without
    // calibration ncnn does fp16→int8 round-trip with default scale,
    // which is SLOWER than fp16-direct).  Stick with full fp16 path.
    m_Net->opt.use_fp16_packed     = true;
    m_Net->opt.use_fp16_storage    = true;
    m_Net->opt.use_fp16_arithmetic = true;
    m_Net->opt.use_int8_storage    = false;
    m_Net->opt.use_int8_arithmetic = false;
    m_Net->set_vulkan_device(m_GpuIndex);

    // Register the custom rife.Warp layer extracted from
    // rife-ncnn-vulkan source.  Stock ncnn doesn't ship this layer,
    // so without registration load_param() fails with rc=-1 the
    // moment the parser hits "rife.Warp" in the network.
    if (m_Net->register_custom_layer("rife.Warp",
                                     viple::createRifeWarp,
                                     viple::destroyRifeWarp,
                                     nullptr) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] register_custom_layer(rife.Warp) failed");
        m_Net.reset();
        return false;
    }

    // Open files ourselves via _wfopen and pass FILE* into ncnn —
    // bypasses ncnn's internal path handling, which on this build of
    // ncnn 20260113 returns rc=-1 even for paths that exist + are
    // readable.  We need to convert paths to native Windows
    // backslash separators because Qt-style forward slashes confuse
    // some old fopen() variants on certain code pages.
    std::wstring paramWide = QDir::toNativeSeparators(paramPath).toStdWString();
    std::wstring binWide   = QDir::toNativeSeparators(binPath).toStdWString();

    FILE* paramFp = _wfopen(paramWide.c_str(), L"rb");
    if (!paramFp) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] _wfopen(param) failed errno=%d path=%s",
                    errno, paramPath.toUtf8().constData());
        m_Net.reset();
        return false;
    }
    int paramRc = m_Net->load_param(paramFp);
    fclose(paramFp);
    if (paramRc != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] load_param rc=%d (parser rejected file)",
                    paramRc);
        m_Net.reset();
        return false;
    }

    FILE* binFp = _wfopen(binWide.c_str(), L"rb");
    if (!binFp) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] _wfopen(bin) failed errno=%d path=%s",
                    errno, binPath.toUtf8().constData());
        m_Net.reset();
        return false;
    }
    int modelRc = m_Net->load_model(binFp);
    fclose(binFp);
    if (modelRc != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] load_model rc=%d", modelRc);
        m_Net.reset();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] loaded RIFE model from %s",
                m_ModelDir.c_str());
    return true;
}

double NcnnFRUC::probeInferenceCost(int warmup, int iterations)
{
    if (!m_Net) return -1.0;

    // Build dummy NCHW inputs.  RIFE 4.25-lite expects:
    //   in0: [1, 3, H, W] prev RGB (fp32, 0..1)
    //   in1: [1, 3, H, W] curr RGB (fp32, 0..1)
    //   in2: [1, 1, H, W] timestep plane (fp32, all 0.5 for midpoint)
    ncnn::Mat in0(static_cast<int>(m_Width), static_cast<int>(m_Height), 3);
    ncnn::Mat in1(static_cast<int>(m_Width), static_cast<int>(m_Height), 3);
    ncnn::Mat in2(static_cast<int>(m_Width), static_cast<int>(m_Height), 1);
    in0.fill(0.25f);
    in1.fill(0.50f);
    in2.fill(0.50f);

    auto runOne = [&]() -> bool {
        ncnn::Extractor ex = m_Net->create_extractor();
        ex.input("in0", in0);
        ex.input("in1", in1);
        ex.input("in2", in2);
        ncnn::Mat out;
        return ex.extract("out0", out) == 0;
    };

    // Warmup: ncnn lazily compiles SPIR-V on first dispatch, so the
    // first iteration is dominated by shader compile rather than
    // inference time.  Discard.
    for (int i = 0; i < warmup; ++i) {
        if (!runOne()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] probe warmup #%d failed", i);
            return -1.0;
        }
    }

    std::vector<double> samples;
    samples.reserve(iterations);
    for (int i = 0; i < iterations; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        if (!runOne()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] probe iteration #%d failed", i);
            return -1.0;
        }
        auto t1 = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    const double med = median(samples);
    const auto [mn, mx] = std::minmax_element(samples.begin(), samples.end());

    // Phase B perf-honesty correction: until B.4 lands the shared-
    // texture preprocess shader, every submitFrame eats ~30 ms of
    // CPU staging on top of pure inference (RGBA8↔fp32 conversion +
    // GPU↔CPU memcpy at 1080p).  Probe alone reports ~17 ms; if we
    // hand that to the cascade, it picks NCNN over Generic and the
    // real-world frame budget overflows → user-visible flicker.  Add
    // a staging estimate now so the cascade picks NCNN only when
    // there's headroom for both inference and staging.  Once
    // m_SharedPathReady flips true (B.5), the estimate drops to a
    // small fixed GPU-compute cost.
    const double stagingMs = m_SharedPathReady ? 2.0 : 30.0;
    const double effective = med + stagingMs;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] probe @ %ux%u: median=%.2fms (min=%.2f max=%.2f n=%d, warmup=%d) "
                "+ staging ~%.0f ms = effective %.2f ms %s",
                m_Width, m_Height, med, *mn, *mx, iterations, warmup,
                stagingMs, effective,
                m_SharedPathReady ? "[shared path]" : "[CPU staging]");

    m_LastInferenceMs = med;
    return effective;
}

// ---- Phase B.3: D3D11 NT handle → Vulkan VkImage import ----

bool NcnnFRUC::importSharedTexturesIntoVulkan()
{
    if (!m_SharedRenderHandle || !m_SharedOutputHandle) return false;
    if (!m_Net) return false;

    HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) vk = ::LoadLibraryW(L"vulkan-1.dll");
    if (!vk) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.3: vulkan-1.dll missing");
        return false;
    }

    auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
    if (!pfnGDPA) return false;

    const VkDevice dev = ncnn::get_gpu_device(m_GpuIndex)->vkdevice();

    // Resolve every entry point we need.  Bail loudly if any miss —
    // the device must have been created with the right extensions
    // (we patched ncnn for that in v1.3.30).
    auto pfnCreateImage   = (PFN_vkCreateImage)    pfnGDPA(dev, "vkCreateImage");
    auto pfnDestroyImage  = (PFN_vkDestroyImage)   pfnGDPA(dev, "vkDestroyImage");
    auto pfnGetReqs       = (PFN_vkGetImageMemoryRequirements) pfnGDPA(dev, "vkGetImageMemoryRequirements");
    auto pfnAllocate      = (PFN_vkAllocateMemory) pfnGDPA(dev, "vkAllocateMemory");
    auto pfnFreeMemory    = (PFN_vkFreeMemory)     pfnGDPA(dev, "vkFreeMemory");
    auto pfnBindImage     = (PFN_vkBindImageMemory) pfnGDPA(dev, "vkBindImageMemory");
    auto pfnGetHandleProps = (PFN_vkGetMemoryWin32HandlePropertiesKHR)
        pfnGDPA(dev, "vkGetMemoryWin32HandlePropertiesKHR");
    if (!pfnCreateImage || !pfnDestroyImage || !pfnGetReqs ||
        !pfnAllocate || !pfnFreeMemory || !pfnBindImage || !pfnGetHandleProps) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.3: required Vulkan entry points missing");
        return false;
    }

    auto importOne = [&](HANDLE shared, VkImage* outImage, VkDeviceMemory* outMem,
                         const char* label, bool wantStorageBit) -> bool {
        // Step 1: VkImage with external-memory create info.
        VkExternalMemoryImageCreateInfo extInfo = {};
        extInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo ici = {};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext         = &extInfo;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent        = { m_Width, m_Height, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (wantStorageBit) {
            ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult r = pfnCreateImage(dev, &ici, nullptr, outImage);
        if (r != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.3 vkCreateImage(%s) failed rc=%d",
                        label, (int)r);
            return false;
        }

        // Step 2: query image memory requirements (size, alignment,
        // memoryTypeBits) so the imported memory has matching size.
        VkMemoryRequirements memReq = {};
        pfnGetReqs(dev, *outImage, &memReq);

        // Step 3: query memoryTypeBits constraints from the imported
        // handle so we can intersect with the image's requirements
        // and pick a memory type that is valid for both.
        VkMemoryWin32HandlePropertiesKHR hp = {};
        hp.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
        r = pfnGetHandleProps(dev,
                              VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                              shared, &hp);
        if (r != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.3 GetHandleProps(%s) failed rc=%d",
                        label, (int)r);
            pfnDestroyImage(dev, *outImage, nullptr);
            *outImage = VK_NULL_HANDLE;
            return false;
        }

        const uint32_t memTypeBits = memReq.memoryTypeBits & hp.memoryTypeBits;
        if (memTypeBits == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.3 (%s) memoryTypeBits intersection empty: img=0x%x handle=0x%x",
                        label, memReq.memoryTypeBits, hp.memoryTypeBits);
            pfnDestroyImage(dev, *outImage, nullptr);
            *outImage = VK_NULL_HANDLE;
            return false;
        }
        // Pick the lowest set bit from the intersection — typical
        // host-coherent / device-local mem on the intersection map.
        uint32_t memTypeIndex = 0;
        for (uint32_t i = 0; i < 32; ++i) {
            if (memTypeBits & (1u << i)) { memTypeIndex = i; break; }
        }

        // Step 4: VkAllocateMemory with VkImportMemoryWin32HandleInfoKHR.
        // Per spec, memory must be DEDICATED to the image when the
        // handle came from D3D11.
        VkMemoryDedicatedAllocateInfo ded = {};
        ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        ded.image = *outImage;

        VkImportMemoryWin32HandleInfoKHR imp = {};
        imp.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        imp.pNext      = &ded;
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        imp.handle     = shared;

        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext           = &imp;
        mai.allocationSize  = memReq.size;
        mai.memoryTypeIndex = memTypeIndex;

        r = pfnAllocate(dev, &mai, nullptr, outMem);
        if (r != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.3 vkAllocateMemory(%s) failed rc=%d "
                        "(memSize=%llu memType=%u)",
                        label, (int)r,
                        (unsigned long long)memReq.size, memTypeIndex);
            pfnDestroyImage(dev, *outImage, nullptr);
            *outImage = VK_NULL_HANDLE;
            return false;
        }

        // Step 5: bind image to the imported memory at offset 0.
        r = pfnBindImage(dev, *outImage, *outMem, 0);
        if (r != VK_SUCCESS) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.3 vkBindImageMemory(%s) failed rc=%d",
                        label, (int)r);
            pfnFreeMemory(dev, *outMem, nullptr);
            *outMem = VK_NULL_HANDLE;
            pfnDestroyImage(dev, *outImage, nullptr);
            *outImage = VK_NULL_HANDLE;
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.3 imported %s: VkImage=%p VkMemory=%p (size=%llu memType=%u)",
                    label, (void*)*outImage, (void*)*outMem,
                    (unsigned long long)memReq.size, memTypeIndex);
        return true;
    };

    VkImage renderImg = VK_NULL_HANDLE, outputImg = VK_NULL_HANDLE;
    VkDeviceMemory renderMem = VK_NULL_HANDLE, outputMem = VK_NULL_HANDLE;

    if (!importOne(m_SharedRenderHandle, &renderImg, &renderMem, "render", false) ||
        !importOne(m_SharedOutputHandle, &outputImg, &outputMem, "output", true)) {
        // Cleanup partial state — destroy whatever did succeed.
        if (renderImg) pfnDestroyImage(dev, renderImg, nullptr);
        if (renderMem) pfnFreeMemory(dev, renderMem, nullptr);
        if (outputImg) pfnDestroyImage(dev, outputImg, nullptr);
        if (outputMem) pfnFreeMemory(dev, outputMem, nullptr);
        return false;
    }

    m_VkRenderImage = (void*)renderImg;
    m_VkOutputImage = (void*)outputImg;
    m_VkRenderMem   = (void*)renderMem;
    m_VkOutputMem   = (void*)outputMem;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] phase-B.3 SUCCESS — D3D11 textures opened as Vulkan VkImage. "
                "Phase B.4 (preprocess compute shader) is the next missing piece.");
    return true;
}

// ---- Phase B.4a: GLSL → SPIR-V → ncnn::Pipeline ----
//
// Two compute shaders translate between the two pixel layouts that
// straddle the D3D11 / RIFE boundary:
//
//   pre  : packed RGBA8 buffer  →  planar fp32 buffer (3 channels, 0..1)
//   post : planar fp32 buffer    →  packed RGBA8 buffer (clamped 0..255)
//
// ncnn::Pipeline auto-reflects bindings + push constants via
// SPIRV-Reflect, so the GLSL just declares storage buffers + a tiny
// push-constant block (w, h) and ncnn handles the layout setup.
// Workgroup is 8×8×1 — empirically the sweet spot for memcpy-bound
// shaders on most GPUs (256 invocations / wave amortises descriptor
// fetch overhead without flooding the LLC).
//
// The bindings + dispatcher set on record_pipeline don't have to be
// the same VkMat — we pass a "shape mat" (W×H×1) so ncnn divides by
// local_size to get the right groupCount.

namespace {

const char* kPreShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) readonly buffer PackedRGBA8 { uint data[]; } src;
layout(binding = 1) writeonly buffer PlanarFp32 { float data[]; } dst;
layout(push_constant) uniform Params { int w; int h; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;
    uint pix = src.data[y * p.w + x];
    float r = float((pix >>  0) & 0xFFu) * (1.0 / 255.0);
    float g = float((pix >>  8) & 0xFFu) * (1.0 / 255.0);
    float b = float((pix >> 16) & 0xFFu) * (1.0 / 255.0);
    int idx = y * p.w + x;
    int plane = p.w * p.h;
    dst.data[idx + 0 * plane] = r;
    dst.data[idx + 1 * plane] = g;
    dst.data[idx + 2 * plane] = b;
}
)GLSL";

const char* kPostShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) readonly buffer PlanarFp32 { float data[]; } src;
layout(binding = 1) writeonly buffer PackedRGBA8 { uint data[]; } dst;
layout(push_constant) uniform Params { int w; int h; } p;

void main() {
    int x = int(gl_GlobalInvocationID.x);
    int y = int(gl_GlobalInvocationID.y);
    if (x >= p.w || y >= p.h) return;
    int idx = y * p.w + x;
    int plane = p.w * p.h;
    float r = clamp(src.data[idx + 0 * plane], 0.0, 1.0);
    float g = clamp(src.data[idx + 1 * plane], 0.0, 1.0);
    float b = clamp(src.data[idx + 2 * plane], 0.0, 1.0);
    uint pix = uint(r * 255.0)
             | (uint(g * 255.0) <<  8)
             | (uint(b * 255.0) << 16)
             | (uint(255)       << 24);
    dst.data[idx] = pix;
}
)GLSL";

} // namespace

bool NcnnFRUC::compileSharedPathPipelines()
{
    if (!m_Net) return false;

    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
    if (!vkdev) return false;

    // Use a stripped-down Option for shader compile.  We don't need
    // the fp16 / packing knobs ncnn uses for its own ops because our
    // two shaders are just byte-shuffles + scale.  Leave use_vulkan_compute
    // on so compile_spirv_module emits a Vulkan-target SPIR-V.
    ncnn::Option opt;
    opt.use_vulkan_compute = true;

    auto buildPipeline = [&](const char* glsl, const char* label,
                             ncnn::Pipeline*& outPipeline) -> bool {
        std::vector<uint32_t> spirv;
        int rc = ncnn::compile_spirv_module(glsl, opt, spirv);
        if (rc != 0 || spirv.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.4a compile_spirv_module(%s) failed rc=%d (size=%zu)",
                        label, rc, spirv.size());
            return false;
        }

        auto* pipe = new ncnn::Pipeline(vkdev);
        // GLSL declares local_size 8×8×1 already; tell ncnn the same so
        // its dispatcher math matches.  set_local_size_xyz overrides
        // shader's declared layout.
        pipe->set_local_size_xyz(8, 8, 1);

        // No specialization constants — the shader is parameterised
        // entirely via push constants (w, h).
        std::vector<ncnn::vk_specialization_type> specs;
        rc = pipe->create(spirv.data(), spirv.size() * sizeof(uint32_t), specs);
        if (rc != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.4a Pipeline::create(%s) failed rc=%d",
                        label, rc);
            delete pipe;
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4a compiled %s pipeline "
                    "(spv=%zu bytes, local=8x8x1)",
                    label, spirv.size() * sizeof(uint32_t));
        outPipeline = pipe;
        return true;
    };

    if (!buildPipeline(kPreShaderGlsl, "pre", m_PipelinePre)) return false;
    if (!buildPipeline(kPostShaderGlsl, "post", m_PipelinePost)) {
        delete m_PipelinePre;
        m_PipelinePre = nullptr;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] phase-B.4a SUCCESS — both conversion pipelines "
                "compiled and ready (B.4b will wire dispatch into submitFrame)");
    return true;
}

// ---- pixel format conversion helpers ----
// D3D11 Map gives us interleaved RGBA8 with row pitch >= w*4 (driver
// alignment).  ncnn::Mat for RIFE uses planar [C, H, W] fp32 in 0..1.
// We do the conversion CPU-side; per frame this is ~10 ms at 1080p,
// dominating the staging cost. Phase B (D3D11→D3D12→Vulkan shared
// texture) will eliminate this in a future iteration.

namespace {

void rgba8RowToPlanarFp32(const unsigned char* src, int w,
                          float* outR, float* outG, float* outB)
{
    constexpr float kInv255 = 1.0f / 255.0f;
    for (int x = 0; x < w; ++x) {
        outR[x] = src[x * 4 + 0] * kInv255;
        outG[x] = src[x * 4 + 1] * kInv255;
        outB[x] = src[x * 4 + 2] * kInv255;
    }
}

void planarFp32RowToRgba8(const float* inR, const float* inG, const float* inB,
                          unsigned char* dst, int w)
{
    for (int x = 0; x < w; ++x) {
        float r = inR[x] * 255.0f;
        float g = inG[x] * 255.0f;
        float b = inB[x] * 255.0f;
        if (r < 0.0f) r = 0.0f; else if (r > 255.0f) r = 255.0f;
        if (g < 0.0f) g = 0.0f; else if (g > 255.0f) g = 255.0f;
        if (b < 0.0f) b = 0.0f; else if (b > 255.0f) b = 255.0f;
        dst[x * 4 + 0] = static_cast<unsigned char>(r);
        dst[x * 4 + 1] = static_cast<unsigned char>(g);
        dst[x * 4 + 2] = static_cast<unsigned char>(b);
        dst[x * 4 + 3] = 255;
    }
}

// D3D11 staging-tex Map() → ncnn::Mat (1, 3, H, W) fp32 normalized.
// out_mat must be created as ncnn::Mat(W, H, 3) before the call.
bool stagingToMat(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging,
                  int w, int h, ncnn::Mat& out_mat)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    const auto* base = static_cast<const unsigned char*>(m.pData);
    float* r = out_mat.channel(0);
    float* g = out_mat.channel(1);
    float* b = out_mat.channel(2);
    for (int y = 0; y < h; ++y) {
        rgba8RowToPlanarFp32(base + y * m.RowPitch, w,
                             r + y * w, g + y * w, b + y * w);
    }
    ctx->Unmap(staging, 0);
    return true;
}

// ncnn::Mat (3, H, W) fp32 → D3D11 staging-tex Map(WRITE).
bool matToStaging(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging,
                  int w, int h, const ncnn::Mat& in_mat)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &m))) return false;
    auto* base = static_cast<unsigned char*>(m.pData);
    const float* r = in_mat.channel(0);
    const float* g = in_mat.channel(1);
    const float* b = in_mat.channel(2);
    for (int y = 0; y < h; ++y) {
        planarFp32RowToRgba8(r + y * w, g + y * w, b + y * w,
                             base + y * m.RowPitch, w);
    }
    ctx->Unmap(staging, 0);
    return true;
}

} // namespace

bool NcnnFRUC::submitFrame(ID3D11DeviceContext* ctx, double /*timestamp*/)
{
    if (!ctx || !m_Net) return false;

    // CRITICAL: same dance as GenericFRUC — caller wrote into RTV;
    // we must unbind it before lifting the texture as SRV / staging
    // source, otherwise D3D11 silently drops the SRV bind.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Lift the freshly-rendered frame into a CPU-readable staging
    // texture, then convert RGBA8 → planar fp32 normalized.
    ctx->CopyResource(m_StagingCurrTex.Get(), m_RenderTex.Get());
    ncnn::Mat curr_mat(static_cast<int>(m_Width), static_cast<int>(m_Height), 3);
    if (!stagingToMat(ctx, m_StagingCurrTex.Get(),
                      static_cast<int>(m_Width), static_cast<int>(m_Height),
                      curr_mat)) {
        return false;
    }

    // First frame: stash and skip.  RIFE needs prev + curr; we have
    // only curr so far.  Caller will see "no interp" but the real
    // frame still presents normally via getLastRenderSRV().
    if (m_FrameCount == 0) {
        m_PrevMat = curr_mat;
        m_FrameCount = 1;
        return false;
    }

    // RIFE 4.25-lite forward: in0=prev RGB, in1=curr RGB, in2=timestep
    // plane (constant 0.5 for midpoint).  out0 is the interpolated
    // RGB tensor at t=0.5 between prev and curr.
    auto t0 = std::chrono::steady_clock::now();
    ncnn::Extractor ex = m_Net->create_extractor();
    ex.input("in0", m_PrevMat);
    ex.input("in1", curr_mat);
    ex.input("in2", m_TimestepMat);
    ncnn::Mat out_mat;
    if (ex.extract("out0", out_mat) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] extract(out0) failed @ frame %d",
                    m_FrameCount);
        m_PrevMat = curr_mat;
        m_FrameCount++;
        return false;
    }
    auto t1 = std::chrono::steady_clock::now();
    m_LastInferenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Validate output shape — if RIFE returns garbage (e.g. wrong
    // channel count, empty mat) writing it to staging would emit a
    // black or uninitialized frame which the user sees as "flicker".
    // Bail out and let the caller fall back to real-frame-only.
    if (out_mat.empty() || out_mat.w != static_cast<int>(m_Width) ||
        out_mat.h != static_cast<int>(m_Height) || out_mat.c != 3) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] extract returned unexpected shape "
                    "(w=%d h=%d c=%d empty=%d) @ frame %d — declining interp",
                    out_mat.w, out_mat.h, out_mat.c, (int)out_mat.empty(),
                    m_FrameCount);
        m_PrevMat = curr_mat;
        m_FrameCount++;
        return false;
    }

    // Output back to D3D11: fp32 → RGBA8 staging → CopyResource to
    // SRV-side output texture.
    if (!matToStaging(ctx, m_StagingOutTex.Get(),
                      static_cast<int>(m_Width), static_cast<int>(m_Height),
                      out_mat)) {
        m_PrevMat = curr_mat;
        m_FrameCount++;
        return false;
    }
    ctx->CopyResource(m_OutputTex.Get(), m_StagingOutTex.Get());

    // Save current frame as the "prev" tensor for the next call.
    // ncnn::Mat is reference-counted internally so this is cheap.
    m_PrevMat = curr_mat;
    m_FrameCount++;
    return true;
}

void NcnnFRUC::skipFrame(ID3D11DeviceContext* ctx)
{
    if (!ctx) return;
    // Even when caller asks us to skip interp (late frame, poor
    // network), we still stash this frame as the new "prev" so the
    // next interp uses the freshest reference.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    ctx->CopyResource(m_StagingCurrTex.Get(), m_RenderTex.Get());
    ncnn::Mat next_mat(static_cast<int>(m_Width), static_cast<int>(m_Height), 3);
    if (stagingToMat(ctx, m_StagingCurrTex.Get(),
                     static_cast<int>(m_Width), static_cast<int>(m_Height),
                     next_mat)) {
        m_PrevMat = next_mat;
        if (m_FrameCount == 0) m_FrameCount = 1;
    }
}

void NcnnFRUC::destroy()
{
    // Phase B.4a: release the GPU conversion pipelines BEFORE m_Net.
    // ncnn::Pipeline holds references into the VulkanDevice that
    // m_Net's destructor would otherwise tear down out from under us.
    if (m_PipelinePre) {
        delete m_PipelinePre;
        m_PipelinePre = nullptr;
    }
    if (m_PipelinePost) {
        delete m_PipelinePost;
        m_PipelinePost = nullptr;
    }
    if (m_Net) {
        m_Net.reset();
        releaseNcnnGpu();
    }
    m_PrevMat.release();
    m_TimestepMat.release();
    m_FrameCount = 0;
    // Close DXGI shared handles before releasing the underlying
    // D3D11 textures (CloseHandle is safe even if Vulkan-side imports
    // were already done — Vulkan keeps its own ref to the memory).
    if (m_SharedRenderHandle) {
        ::CloseHandle(m_SharedRenderHandle);
        m_SharedRenderHandle = nullptr;
    }
    if (m_SharedOutputHandle) {
        ::CloseHandle(m_SharedOutputHandle);
        m_SharedOutputHandle = nullptr;
    }
    m_SharedPathReady = false;
    m_StagingOutTex.Reset();
    m_OutputSRV.Reset();
    m_OutputTex.Reset();
    m_StagingCurrTex.Reset();
    m_LastSRV.Reset();
    m_RenderRTV.Reset();
    m_RenderTex.Reset();
    m_Device.Reset();
    m_Initialized.store(false, std::memory_order_release);
}

#endif // _WIN32
