// VipleStream §I.F — NCNN-Vulkan FRUC backend (phase 1: probe-only).

#ifdef _WIN32

#include "ncnnfruc.h"
#include "ncnn_rife_warp.h"
#include "path.h"

#include <SDL.h>
#include <QDir>

#include <ncnn/net.h>
#include <ncnn/gpu.h>
#include <ncnn/mat.h>

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

    // Phase-1 placeholder textures.  Backend contract requires
    // non-null RTV/SRV/OutputTexture so callers don't crash on read;
    // submitFrame is a no-op until the shared-texture bridge lands.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_Width;
    td.Height = m_Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_Device->CreateTexture2D(&td, nullptr, m_RenderTex.GetAddressOf())) ||
        FAILED(m_Device->CreateTexture2D(&td, nullptr, m_OutputTex.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] D3D11 placeholder texture creation failed");
        destroy();
        return false;
    }
    if (FAILED(m_Device->CreateRenderTargetView(m_RenderTex.Get(), nullptr,
                                                m_RenderRTV.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_RenderTex.Get(), nullptr,
                                                  m_LastSRV.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_OutputTex.Get(), nullptr,
                                                  m_OutputSRV.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] D3D11 placeholder RTV/SRV creation failed");
        destroy();
        return false;
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
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-NCNN] probe @ %ux%u: median=%.2fms (min=%.2f max=%.2f n=%d, warmup=%d)",
                m_Width, m_Height, med, *mn, *mx, iterations, warmup);

    m_LastInferenceMs = med;
    return med;
}

bool NcnnFRUC::submitFrame(ID3D11DeviceContext* /*ctx*/, double /*timestamp*/)
{
    // Phase 1: shared-texture bridge not yet implemented.  Returning
    // false makes the caller treat us as "no interp emitted this
    // frame" — the real frame still presents normally.  Phase 2 will
    // record the actual ncnn Vulkan compute submission here.
    return false;
}

void NcnnFRUC::skipFrame(ID3D11DeviceContext* /*ctx*/)
{
    // Stateful caches (prev frame) live in phase 2.  Today nothing to
    // skip — the no-op submitFrame already did nothing.
}

void NcnnFRUC::destroy()
{
    if (m_Net) {
        m_Net.reset();
        releaseNcnnGpu();
    }
    m_OutputSRV.Reset();
    m_OutputTex.Reset();
    m_LastSRV.Reset();
    m_RenderRTV.Reset();
    m_RenderTex.Reset();
    m_Device.Reset();
    m_Initialized.store(false, std::memory_order_release);
}

#endif // _WIN32
