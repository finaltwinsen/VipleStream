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
#include <ncnn/command.h>

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
        //
        // v1.3.38 — gate ALL of B.4a/B.4b/B.4c behind VIPLE_FRUC_NCNN_SHARED
        // env var.  v1.3.37 ran the new init code paths unconditionally
        // (only m_SharedPathReady flip was gated).  Default users on
        // v1.3.37 reported crashes during a SECOND NcnnFRUC init (codec
        // re-init triggered by AV1 format change).  Until the root cause
        // of those crashes is found, default behavior must match v1.3.34
        // exactly: only B.1+B.2+B.3 code touches Vulkan, CPU staging
        // path runs from submitFrame.  Set the env var to opt back into
        // experimental B.4 territory.
        if (importSharedTexturesIntoVulkan()) {
            const char* sharedEnv = SDL_getenv("VIPLE_FRUC_NCNN_SHARED");
            const bool wantShared = sharedEnv && SDL_atoi(sharedEnv) != 0;
            if (wantShared) {
                if (compileSharedPathPipelines() && initSharedPathResources() && selftestSharedPath()) {
                    m_SharedPathReady = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC-NCNN] phase-B.4c: shared path ENABLED via VIPLE_FRUC_NCNN_SHARED=1 "
                                "(experimental; probe staging cost drops to ~2ms)");
                } else {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC-NCNN] phase-B.4c: shared path opt-in failed (compile/alloc/selftest);"
                                " falling back to CPU staging path");
                }
            } else {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC-NCNN] phase-B.4 disabled (default). "
                            "Set VIPLE_FRUC_NCNN_SHARED=1 to opt into the experimental GPU compute path");
            }
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

    // v1.3.39: per-step trace so future crash logs pinpoint location.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 1/6 new ncnn::Net");
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 2/6 set_vulkan_device(%d)", m_GpuIndex);
    m_Net->set_vulkan_device(m_GpuIndex);

    // Register the custom rife.Warp layer extracted from
    // rife-ncnn-vulkan source.  Stock ncnn doesn't ship this layer,
    // so without registration load_param() fails with rc=-1 the
    // moment the parser hits "rife.Warp" in the network.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 3/6 register_custom_layer(rife.Warp)");
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

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 4/6 load_param");
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

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 5/6 load_model");
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

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] loadModel: step 6/6 done");
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

    // v1.3.39: explicit raw Option — ncnn::compile_spirv_module
    // injects #define NCNN_fp16_packed / NCNN_fp16_storage / etc. into
    // the GLSL preprocessor based on opt flags, and rewrites
    // afp/sfp/buffer_ld* macros accordingly.  Our shaders use plain
    // float/uint with no afp pretense, so we want every ncnn knob OFF
    // to get a verbatim compile of the source we wrote.
    ncnn::Option opt;
    opt.use_vulkan_compute    = true;
    opt.use_fp16_packed       = false;
    opt.use_fp16_storage      = false;
    opt.use_fp16_arithmetic   = false;
    opt.use_int8_storage      = false;
    opt.use_int8_arithmetic   = false;
    opt.use_packing_layout    = false;
    opt.use_shader_pack8      = false;

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

// ---- Phase B.4b: command pool, staging VkMats, D3D11 sync query ----
//
// Persistent resources owned by us across the whole streaming session.
// VkCommandPool + 2× VkCommandBuffer are needed because we record the
// imported-image ↔ packed-buffer copies via raw Vulkan (ncnn doesn't
// expose its own VkCommandBuffer).  ncnn::VkMat instances for the
// staging buffers are allocated through the device's blob allocator
// so they integrate with the rest of ncnn's pipeline machinery.

bool NcnnFRUC::initSharedPathResources()
{
    if (!m_Net) return false;

    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
    if (!vkdev) return false;

    // ---- D3D11 sync query ----
    // D3D11_QUERY_EVENT End()/GetData() pair gives us a CPU-side
    // fence: GetData returns S_OK only after every D3D11 command
    // submitted before End() has reached the GPU.  We poll-spin in
    // submitFrameShared until that point so Vulkan never reads the
    // imported texture mid-D3D11-write.  Adds ~0.2-1 ms CPU per
    // frame; future B.5 optimisation: switch to ID3D11Fence +
    // VkSemaphore via VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT.
    D3D11_QUERY_DESC qDesc = {};
    qDesc.Query = D3D11_QUERY_EVENT;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    if (FAILED(m_Device->CreateQuery(&qDesc, query.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: D3D11 CreateQuery(EVENT) failed");
        return false;
    }
    m_D3D11SyncQuery = query.Detach();

    // ---- Vulkan resources ----
    HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) vk = ::LoadLibraryW(L"vulkan-1.dll");
    if (!vk) return false;

    auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
    if (!pfnGDPA) return false;

    const VkDevice dev = vkdev->vkdevice();
    auto pfnGetQueue       = (PFN_vkGetDeviceQueue)        pfnGDPA(dev, "vkGetDeviceQueue");
    auto pfnCreatePool     = (PFN_vkCreateCommandPool)     pfnGDPA(dev, "vkCreateCommandPool");
    auto pfnAllocCmdBufs   = (PFN_vkAllocateCommandBuffers)pfnGDPA(dev, "vkAllocateCommandBuffers");
    if (!pfnGetQueue || !pfnCreatePool || !pfnAllocCmdBufs) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: required Vulkan entry points missing");
        return false;
    }

    // ncnn's compute queue family — same one its layers run on.  The
    // imported VkImages are owned by the same VulkanDevice so any
    // queue from this family can access them without ownership transfer.
    m_VkComputeQueueFamily = vkdev->info.compute_queue_family_index();
    VkQueue queue = VK_NULL_HANDLE;
    pfnGetQueue(dev, m_VkComputeQueueFamily, 0, &queue);
    if (!queue) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: vkGetDeviceQueue returned NULL");
        return false;
    }
    m_VkComputeQueue = (void*)queue;

    // Command pool with TRANSIENT bit (we re-record every frame, so
    // RESET_COMMAND_BUFFER_BIT is needed too).
    VkCommandPoolCreateInfo cpci = {};
    cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                          | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = m_VkComputeQueueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (pfnCreatePool(dev, &cpci, nullptr, &pool) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: vkCreateCommandPool failed");
        return false;
    }
    m_VkCmdPool = (void*)pool;

    // Two primary command buffers, one for each direction.  Keeping
    // them separate simplifies recording — we don't need to track
    // intermediate fence states.
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cbIn = VK_NULL_HANDLE, cbOut = VK_NULL_HANDLE;
    if (pfnAllocCmdBufs(dev, &cbai, &cbIn) != VK_SUCCESS ||
        pfnAllocCmdBufs(dev, &cbai, &cbOut) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: vkAllocateCommandBuffers failed");
        return false;
    }
    m_VkCmdBufImgIn  = (void*)cbIn;
    m_VkCmdBufImgOut = (void*)cbOut;

    // ---- ncnn-allocated staging VkMats ----
    // Packed RGBA8 buffers: shape (W*H, 1) elemsize=4 = W*H*4 bytes.
    // Planar fp32 prev/timestep: shape (W, H, 3) and (W, H, 1) elemsize=4.
    // v1.3.39: stash allocator in m_BlobAllocator for reclaim in destroy().
    m_BlobAllocator = vkdev->acquire_blob_allocator();
    const int W = static_cast<int>(m_Width);
    const int H = static_cast<int>(m_Height);

    m_PackedInVkMat.create(W * H, (size_t)4, m_BlobAllocator);
    m_PackedOutVkMat.create(W * H, (size_t)4, m_BlobAllocator);
    m_PrevVkMat.create(W, H, 3, (size_t)4, m_BlobAllocator);
    m_TimestepVkMat.create(W, H, 1, (size_t)4, m_BlobAllocator);
    if (m_PackedInVkMat.empty() || m_PackedOutVkMat.empty() ||
        m_PrevVkMat.empty() || m_TimestepVkMat.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: VkMat allocation failed via blob allocator");
        return false;
    }

    // Upload constant 0.5 timestep into m_TimestepVkMat.  This is the
    // RIFE midpoint indicator; it never changes after init.  We use a
    // VkTransfer (one-shot CPU→GPU upload) since it's a single tensor.
    //
    // v1.3.39: explicit raw Option — default ncnn::Option carries
    // use_fp16_packed/storage/arithmetic + use_packing_layout = true,
    // which makes record_upload reformat the bytes (fp32→fp16 +
    // elempack alignment).  RIFE's in2 input expects raw fp32 (3, H, W)
    // so we want a verbatim byte copy.
    {
        ncnn::Mat cpu_timestep(W, H, 1);
        cpu_timestep.fill(0.5f);
        ncnn::Option opt;
        opt.use_vulkan_compute    = true;
        opt.use_fp16_packed       = false;
        opt.use_fp16_storage      = false;
        opt.use_fp16_arithmetic   = false;
        opt.use_int8_storage      = false;
        opt.use_int8_arithmetic   = false;
        opt.use_packing_layout    = false;
        opt.use_shader_pack8      = false;
        opt.blob_vkallocator      = m_BlobAllocator;
        opt.staging_vkallocator   = vkdev->acquire_staging_allocator();
        ncnn::VkTransfer xfer(vkdev);
        xfer.record_upload(cpu_timestep, m_TimestepVkMat, opt);
        xfer.submit_and_wait();
        vkdev->reclaim_staging_allocator(opt.staging_vkallocator);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] phase-B.4b SUCCESS — command pool, command buffers, "
                "packed VkMats (%dx%d), and timestep tensor allocated. "
                "submitFrameShared is wired but gated behind m_SharedPathReady (still false)",
                W, H);
    return true;
}

// One-shot UNDEFINED→GENERAL transition for the imported VkImages.
// Called lazily on first submitFrameShared invocation because
// vkQueueSubmit is only valid once the queue exists and cmdBuf can be
// recorded.  Returns false on Vulkan error; caller treats as fatal
// for the shared path (falls back to CPU staging path).

bool NcnnFRUC::transitionSharedImagesToGeneral()
{
    HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) return false;
    auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
    if (!pfnGDPA) return false;

    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
    const VkDevice dev = vkdev->vkdevice();
    auto pfnBegin   = (PFN_vkBeginCommandBuffer) pfnGDPA(dev, "vkBeginCommandBuffer");
    auto pfnEnd     = (PFN_vkEndCommandBuffer)   pfnGDPA(dev, "vkEndCommandBuffer");
    auto pfnBarrier = (PFN_vkCmdPipelineBarrier) pfnGDPA(dev, "vkCmdPipelineBarrier");
    auto pfnSubmit  = (PFN_vkQueueSubmit)        pfnGDPA(dev, "vkQueueSubmit");
    auto pfnWait    = (PFN_vkQueueWaitIdle)      pfnGDPA(dev, "vkQueueWaitIdle");
    auto pfnReset   = (PFN_vkResetCommandBuffer) pfnGDPA(dev, "vkResetCommandBuffer");
    if (!pfnBegin || !pfnEnd || !pfnBarrier || !pfnSubmit || !pfnWait || !pfnReset)
        return false;

    auto cb = (VkCommandBuffer)m_VkCmdBufImgIn;
    pfnReset(cb, 0);

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pfnBegin(cb, &bi);

    auto makeBarrier = [](VkImage img) {
        VkImageMemoryBarrier b = {};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
        b.srcAccessMask    = 0;
        b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image            = img;
        b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel   = 0;
        b.subresourceRange.levelCount     = 1;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount     = 1;
        return b;
    };

    VkImageMemoryBarrier barriers[2] = {
        makeBarrier((VkImage)m_VkRenderImage),
        makeBarrier((VkImage)m_VkOutputImage),
    };
    pfnBarrier(cb,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               0, 0, nullptr, 0, nullptr, 2, barriers);
    pfnEnd(cb);

    VkSubmitInfo si = {};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    if (pfnSubmit((VkQueue)m_VkComputeQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: layout transition vkQueueSubmit failed");
        return false;
    }
    pfnWait((VkQueue)m_VkComputeQueue);

    m_VkImagesInGeneralLayout = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] phase-B.4b: imported VkImages transitioned to GENERAL layout");
    return true;
}

bool NcnnFRUC::submitFrameShared(ID3D11DeviceContext* ctx)
{
    if (!m_SharedPathReady || !m_PipelinePre || !m_PipelinePost) return false;
    if (!m_VkCmdPool || !m_VkComputeQueue) return false;
    if (!m_VkRenderImage || !m_VkOutputImage) return false;

    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
    if (!vkdev) return false;

    // Lazy first-use: transition imported images to GENERAL layout.
    if (!m_VkImagesInGeneralLayout) {
        if (!transitionSharedImagesToGeneral()) return false;
    }

    HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!vk) return false;
    auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
    if (!pfnGDPA) return false;

    const VkDevice dev = vkdev->vkdevice();
    auto pfnBegin    = (PFN_vkBeginCommandBuffer) pfnGDPA(dev, "vkBeginCommandBuffer");
    auto pfnEnd      = (PFN_vkEndCommandBuffer)   pfnGDPA(dev, "vkEndCommandBuffer");
    auto pfnBarrier  = (PFN_vkCmdPipelineBarrier) pfnGDPA(dev, "vkCmdPipelineBarrier");
    auto pfnImg2Buf  = (PFN_vkCmdCopyImageToBuffer) pfnGDPA(dev, "vkCmdCopyImageToBuffer");
    auto pfnBuf2Img  = (PFN_vkCmdCopyBufferToImage) pfnGDPA(dev, "vkCmdCopyBufferToImage");
    auto pfnSubmit   = (PFN_vkQueueSubmit)        pfnGDPA(dev, "vkQueueSubmit");
    auto pfnWait     = (PFN_vkQueueWaitIdle)      pfnGDPA(dev, "vkQueueWaitIdle");
    auto pfnReset    = (PFN_vkResetCommandBuffer) pfnGDPA(dev, "vkResetCommandBuffer");

    // Step 1: Unbind RTV (D3D11 silently drops SRV bind otherwise; same dance as GenericFRUC).
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    // Step 2: D3D11 sync — End() the event query and spin until GetData returns S_OK.
    auto* sync = (ID3D11Query*)m_D3D11SyncQuery;
    ctx->End(sync);
    BOOL d3d11Done = FALSE;
    while (true) {
        HRESULT hr = ctx->GetData(sync, &d3d11Done, sizeof(d3d11Done), 0);
        if (hr == S_OK && d3d11Done) break;
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.4b: D3D11 sync GetData failed 0x%08lx", hr);
            return false;
        }
        // S_FALSE: not yet ready, spin.
    }

    // Step 3: Vulkan submission — vkCmdCopyImageToBuffer (imported render → packed-in buf)
    {
        auto cb = (VkCommandBuffer)m_VkCmdBufImgIn;
        pfnReset(cb, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pfnBegin(cb, &bi);

        VkBufferImageCopy region = {};
        region.bufferOffset      = m_PackedInVkMat.buffer_offset();
        region.bufferRowLength   = 0;  // tightly packed
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { m_Width, m_Height, 1 };
        pfnImg2Buf(cb, (VkImage)m_VkRenderImage, VK_IMAGE_LAYOUT_GENERAL,
                   m_PackedInVkMat.buffer(), 1, &region);

        // Buffer barrier so the compute shader read sees the copy.
        VkBufferMemoryBarrier bb = {};
        bb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer              = m_PackedInVkMat.buffer();
        bb.offset              = m_PackedInVkMat.buffer_offset();
        bb.size                = VK_WHOLE_SIZE;
        pfnBarrier(cb,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   0, 0, nullptr, 1, &bb, 0, nullptr);
        pfnEnd(cb);

        VkSubmitInfo si = {};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        if (pfnSubmit((VkQueue)m_VkComputeQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        pfnWait((VkQueue)m_VkComputeQueue);
    }

    // Step 4: ncnn::VkCompute — pre-shader, then RIFE forward, then post-shader.
    auto t0 = std::chrono::steady_clock::now();
    ncnn::VkCompute cmd(vkdev);

    const int W = static_cast<int>(m_Width);
    const int H = static_cast<int>(m_Height);
    ncnn::VkMat curr_vk(W, H, 3, (size_t)4, vkdev->acquire_blob_allocator());

    // Pre-shader push constants: w, h.
    std::vector<ncnn::vk_constant_type> preConsts(2);
    preConsts[0].i = W;
    preConsts[1].i = H;
    cmd.record_pipeline(m_PipelinePre,
                        std::vector<ncnn::VkMat>{ m_PackedInVkMat, curr_vk },
                        preConsts, curr_vk);

    // First frame: stash and skip — no prev yet.
    if (m_FrameCount == 0) {
        cmd.record_clone(curr_vk, m_PrevVkMat, ncnn::Option());
        cmd.submit_and_wait();
        m_FrameCount = 1;
        return false;
    }

    // RIFE forward, VkMat in/out.
    ncnn::Extractor ex = m_Net->create_extractor();
    ex.input("in0", m_PrevVkMat);
    ex.input("in1", curr_vk);
    ex.input("in2", m_TimestepVkMat);
    ncnn::VkMat out_vk;
    if (ex.extract("out0", out_vk, cmd) != 0) {
        cmd.submit_and_wait();
        cmd.record_clone(curr_vk, m_PrevVkMat, ncnn::Option());
        m_FrameCount++;
        return false;
    }

    // Post-shader: out_vk (planar fp32) → m_PackedOutVkMat (RGBA8).
    std::vector<ncnn::vk_constant_type> postConsts(2);
    postConsts[0].i = W;
    postConsts[1].i = H;
    cmd.record_pipeline(m_PipelinePost,
                        std::vector<ncnn::VkMat>{ out_vk, m_PackedOutVkMat },
                        postConsts, m_PackedOutVkMat);

    // Save curr → prev for next frame.
    cmd.record_clone(curr_vk, m_PrevVkMat, ncnn::Option());

    if (cmd.submit_and_wait() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4b: VkCompute submit_and_wait failed");
        return false;
    }
    auto t1 = std::chrono::steady_clock::now();
    m_LastInferenceMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Step 5: vkCmdCopyBufferToImage (m_PackedOutVkMat → imported output VkImage).
    {
        auto cb = (VkCommandBuffer)m_VkCmdBufImgOut;
        pfnReset(cb, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pfnBegin(cb, &bi);

        // Buffer must be readable for transfer.
        VkBufferMemoryBarrier bb = {};
        bb.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        bb.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer              = m_PackedOutVkMat.buffer();
        bb.offset              = m_PackedOutVkMat.buffer_offset();
        bb.size                = VK_WHOLE_SIZE;
        pfnBarrier(cb,
                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                   0, 0, nullptr, 1, &bb, 0, nullptr);

        VkBufferImageCopy region = {};
        region.bufferOffset      = m_PackedOutVkMat.buffer_offset();
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { m_Width, m_Height, 1 };
        pfnBuf2Img(cb, m_PackedOutVkMat.buffer(),
                   (VkImage)m_VkOutputImage, VK_IMAGE_LAYOUT_GENERAL,
                   1, &region);
        pfnEnd(cb);

        VkSubmitInfo si = {};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        if (pfnSubmit((VkQueue)m_VkComputeQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
        pfnWait((VkQueue)m_VkComputeQueue);
    }

    m_FrameCount++;
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

} // namespace

// ---- Phase B.4c: shader correctness selftest ----
// 4×4 packed RGBA8 → planar fp32 round-trip via record_upload + pre-shader
// + record_download.  Validates pipeline reflection / dispatch / memory
// barriers without touching the imported VkImages or D3D11 sync, so a
// failure here pinpoints shader / pipeline issues vs interop issues.

bool NcnnFRUC::selftestSharedPath()
{
    if (!m_PipelinePre || !m_BlobAllocator) return false;

    ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
    if (!vkdev) return false;

    // v1.3.39: reuse m_BlobAllocator instead of acquiring a fresh one
    // (would leak the allocator since selftest goes out of scope and
    // the local pointer is dropped).  Staging allocator IS local —
    // selftest reclaim is straightforward via RAII.
    ncnn::VkAllocator* blob    = m_BlobAllocator;
    ncnn::VkAllocator* staging = vkdev->acquire_staging_allocator();

    constexpr int W = 4, H = 4;

    // Build CPU input: pixel(x,y).R = (x*16+y*4)&0xFF (deterministic)
    // We pack into a 1D Mat of W*H uint32s.  ncnn doesn't have a "uint"
    // Mat type so we treat it as W*H elements with elemsize=4 — the
    // bytes flow through unchanged.
    ncnn::Mat cpu_packed(W * H, (size_t)4);
    auto* packed_u32 = static_cast<uint32_t*>(cpu_packed.data);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t r = static_cast<uint32_t>(x * 32 + y * 8) & 0xFFu;
            uint32_t g = static_cast<uint32_t>(x * 16 + y * 4) & 0xFFu;
            uint32_t b = static_cast<uint32_t>(x *  8 + y * 2) & 0xFFu;
            packed_u32[y * W + x] = r | (g << 8) | (b << 16) | (255u << 24);
        }
    }

    // v1.3.39: explicit raw Option — same reasoning as the timestep
    // upload (default Option's fp16 flags would cause record_upload to
    // reformat our uint32 packed buffer as fp16).
    ncnn::Option opt;
    opt.use_vulkan_compute    = true;
    opt.use_fp16_packed       = false;
    opt.use_fp16_storage      = false;
    opt.use_fp16_arithmetic   = false;
    opt.use_int8_storage      = false;
    opt.use_int8_arithmetic   = false;
    opt.use_packing_layout    = false;
    opt.use_shader_pack8      = false;
    opt.blob_vkallocator      = blob;
    opt.staging_vkallocator   = staging;

    // Allocate dedicated test mats (don't touch m_PackedInVkMat which is
    // sized for the full frame).
    ncnn::VkMat gpu_packed;
    ncnn::VkMat gpu_planar(W, H, 3, (size_t)4, blob);

    ncnn::VkTransfer xfer(vkdev);
    xfer.record_upload(cpu_packed, gpu_packed, opt);
    xfer.submit_and_wait();

    ncnn::VkCompute cmd(vkdev);
    std::vector<ncnn::vk_constant_type> consts(2);
    consts[0].i = W;
    consts[1].i = H;
    cmd.record_pipeline(m_PipelinePre,
                        std::vector<ncnn::VkMat>{ gpu_packed, gpu_planar },
                        consts, gpu_planar);

    ncnn::Mat result;
    cmd.record_download(gpu_planar, result, opt);
    if (cmd.submit_and_wait() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4c selftest: VkCompute submit failed");
        vkdev->reclaim_staging_allocator(staging);
        return false;
    }

    // Verify: 4 sample pixels.  fp32 should be roughly src_byte/255.
    // Tolerance 1/255 is enough — shader does exact divide.
    auto check = [&](int x, int y) -> bool {
        const uint32_t pix = packed_u32[y * W + x];
        const float er = float(pix         & 0xFFu) / 255.0f;
        const float eg = float((pix >>  8) & 0xFFu) / 255.0f;
        const float eb = float((pix >> 16) & 0xFFu) / 255.0f;
        const float ar = result.channel(0).row(y)[x];
        const float ag = result.channel(1).row(y)[x];
        const float ab = result.channel(2).row(y)[x];
        const bool ok =
            std::fabs(ar - er) < 0.01f &&
            std::fabs(ag - eg) < 0.01f &&
            std::fabs(ab - eb) < 0.01f;
        if (!ok) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-NCNN] phase-B.4c selftest pixel(%d,%d): "
                        "got (%.4f, %.4f, %.4f) expected (%.4f, %.4f, %.4f)",
                        x, y, ar, ag, ab, er, eg, eb);
        }
        return ok;
    };
    const bool pass = check(0, 0) && check(3, 0) && check(0, 3) && check(3, 3);

    vkdev->reclaim_staging_allocator(staging);

    if (pass) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] phase-B.4c selftest PASS — pre-shader produces "
                    "correct planar fp32 from packed RGBA8 (4 sample points within 1/255 tolerance)");
    }
    return pass;
}

namespace {

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

bool NcnnFRUC::submitFrame(ID3D11DeviceContext* ctx, double timestamp)
{
    if (!ctx || !m_Net) return false;

    // Phase B.4b: when the shared path is ready (B.4c flips this on
    // after empirical selftest), bypass the CPU staging round-trip
    // entirely.  Failure inside submitFrameShared falls through to
    // the CPU path so a transient Vulkan error doesn't black out the
    // stream — the user might see one frame of stale interp but the
    // next frame recovers.
    if (m_SharedPathReady) {
        if (submitFrameShared(ctx)) return true;
        // Shared path declined or failed.  Fall through to CPU staging
        // — m_PrevMat is independent of m_PrevVkMat so this is safe.
    }

    // CRITICAL: same dance as GenericFRUC — caller wrote into RTV;
    // we must unbind it before lifting the texture as SRV / staging
    // source, otherwise D3D11 silently drops the SRV bind.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    (void)timestamp;

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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC-NCNN] destroy() called (initialized=%d, m_Net=%p)",
                m_Initialized.load(std::memory_order_acquire) ? 1 : 0, (void*)m_Net.get());
    // Phase B.4b: tear down GPU shared-path resources BEFORE pipelines
    // and m_Net.  Order: query → cmdbufs → cmdpool → VkMats (auto via
    // dtor) → pipelines → m_Net → ncnn instance refcount.
    if (m_D3D11SyncQuery) {
        ((ID3D11Query*)m_D3D11SyncQuery)->Release();
        m_D3D11SyncQuery = nullptr;
    }
    if (m_VkCmdPool) {
        HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (vk) {
            auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
            if (pfnGDPA && m_Net) {
                ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
                if (vkdev) {
                    const VkDevice dev = vkdev->vkdevice();
                    auto pfnDestroyPool = (PFN_vkDestroyCommandPool)pfnGDPA(dev, "vkDestroyCommandPool");
                    if (pfnDestroyPool) {
                        // FreeCommandBuffers is implicit when the pool is destroyed.
                        pfnDestroyPool(dev, (VkCommandPool)m_VkCmdPool, nullptr);
                    }
                }
            }
        }
        m_VkCmdPool = nullptr;
        m_VkCmdBufImgIn = nullptr;
        m_VkCmdBufImgOut = nullptr;
    }
    // ncnn::VkMat dtors release their VkBufferMemory back to ncnn's
    // blob allocator.  The VulkanDevice/blob allocator outlives them
    // because the m_Net.reset() below is what triggers VulkanDevice
    // teardown via destroy_gpu_instance refcount.
    m_PackedInVkMat.release();
    m_PackedOutVkMat.release();
    m_PrevVkMat.release();
    m_TimestepVkMat.release();

    // v1.3.39: release imported VkImage + VkDeviceMemory (B.3 leak).
    // Without this, every destroy/init cycle leaks ~16 MB GPU memory
    // and 2 kernel allocations.  Pre-existing in v1.3.34.  Vulkan
    // entry points still need to be loaded dynamically.
    if (m_VkRenderImage || m_VkOutputImage || m_VkRenderMem || m_VkOutputMem) {
        HMODULE vk = ::GetModuleHandleW(L"vulkan-1.dll");
        if (vk && m_Net) {
            auto pfnGDPA = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                ::GetProcAddress(vk, "vkGetDeviceProcAddr"));
            ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
            if (pfnGDPA && vkdev) {
                const VkDevice dev = vkdev->vkdevice();
                auto pfnDestroyImage = (PFN_vkDestroyImage)pfnGDPA(dev, "vkDestroyImage");
                auto pfnFreeMemory   = (PFN_vkFreeMemory)  pfnGDPA(dev, "vkFreeMemory");
                if (pfnDestroyImage && pfnFreeMemory) {
                    if (m_VkRenderImage) pfnDestroyImage(dev, (VkImage)m_VkRenderImage, nullptr);
                    if (m_VkOutputImage) pfnDestroyImage(dev, (VkImage)m_VkOutputImage, nullptr);
                    if (m_VkRenderMem)   pfnFreeMemory  (dev, (VkDeviceMemory)m_VkRenderMem, nullptr);
                    if (m_VkOutputMem)   pfnFreeMemory  (dev, (VkDeviceMemory)m_VkOutputMem, nullptr);
                }
            }
        }
        m_VkRenderImage = nullptr;
        m_VkOutputImage = nullptr;
        m_VkRenderMem   = nullptr;
        m_VkOutputMem   = nullptr;
    }

    // v1.3.39: reclaim the blob allocator we acquired in
    // initSharedPathResources (and that selftest reused).  Must happen
    // AFTER all VkMats above have released their VkBufferMemory back
    // into the allocator's internal pool but BEFORE m_Net.reset()
    // triggers destroy_gpu_instance, which would leave the allocator
    // dangling on the device's outstanding-acquired list.
    if (m_BlobAllocator) {
        ncnn::VulkanDevice* vkdev = ncnn::get_gpu_device(m_GpuIndex);
        if (vkdev) vkdev->reclaim_blob_allocator(m_BlobAllocator);
        m_BlobAllocator = nullptr;
    }

    m_VkImagesInGeneralLayout = false;
    m_VkComputeQueue = nullptr;

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
