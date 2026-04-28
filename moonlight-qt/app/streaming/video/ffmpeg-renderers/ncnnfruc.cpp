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

    // §I.F Phase A.5 (v1.3.21) — full D3D11 texture set for CPU-staged
    // bridge between D3D11 (caller) and ncnn::Mat (NCNN Vulkan).
    //
    // Render → caller writes decoded RGBA8 frame here (RTV).
    // Staging-curr → CopyResource() target, CPU-readable, used to lift
    //                pixels into m_PrevMat for the next inference.
    // Output → Vulkan-side interp result lands here (after staging-out
    //          → CopyResource), caller blits to swapchain via SRV.
    // Staging-out → CPU-writable, holds ncnn output until CopyResource.
    D3D11_TEXTURE2D_DESC renderDesc = {};
    renderDesc.Width = m_Width;
    renderDesc.Height = m_Height;
    renderDesc.MipLevels = 1;
    renderDesc.ArraySize = 1;
    renderDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderDesc.SampleDesc.Count = 1;
    renderDesc.Usage = D3D11_USAGE_DEFAULT;
    renderDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device->CreateTexture2D(&renderDesc, nullptr, m_RenderTex.GetAddressOf())) ||
        FAILED(m_Device->CreateRenderTargetView(m_RenderTex.Get(), nullptr, m_RenderRTV.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_RenderTex.Get(), nullptr, m_LastSRV.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] render-tex creation failed");
        destroy();
        return false;
    }

    D3D11_TEXTURE2D_DESC outDesc = renderDesc;
    outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device->CreateTexture2D(&outDesc, nullptr, m_OutputTex.GetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_OutputTex.Get(), nullptr, m_OutputSRV.GetAddressOf()))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-NCNN] output-tex creation failed");
        destroy();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingCurrDesc = renderDesc;
    stagingCurrDesc.Usage = D3D11_USAGE_STAGING;
    stagingCurrDesc.BindFlags = 0;
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
    if (m_Net) {
        m_Net.reset();
        releaseNcnnGpu();
    }
    m_PrevMat.release();
    m_TimestepMat.release();
    m_FrameCount = 0;
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
