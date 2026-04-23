// VipleStream: Generic FRUC implementation — compute shader optical flow + warping

#ifdef _WIN32

#include "genericfruc.h"
#include "path.h"
#include <SDL_log.h>

using Microsoft::WRL::ComPtr;

GenericFRUC::GenericFRUC() {}

GenericFRUC::~GenericFRUC() {
    destroy();
}

bool GenericFRUC::loadShaders() {
    // Load 3 quality variants of each compute shader
    static const char* meNames[] = {
        "d3d11_motionest_quality.fxc",
        "d3d11_motionest_balanced.fxc",
        "d3d11_motionest_performance.fxc"
    };
    static const char* warpNames[] = {
        "d3d11_warp_quality.fxc",
        "d3d11_warp_balanced.fxc",
        "d3d11_warp_performance.fxc"
    };
    static const char* qualityLabels[] = { "Quality", "Balanced", "Performance" };

    for (int q = 0; q < 3; q++) {
        QByteArray meData = Path::readDataFile(meNames[q]);
        if (meData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC-Generic] %s not found", meNames[q]);
            return false;
        }
        if (FAILED(m_Device->CreateComputeShader(meData.constData(), meData.size(),
                                                 nullptr, &m_MotionEstCS[q]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC-Generic] CreateComputeShader(%s) failed", meNames[q]);
            return false;
        }

        QByteArray warpData = Path::readDataFile(warpNames[q]);
        if (warpData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC-Generic] %s not found", warpNames[q]);
            return false;
        }
        if (FAILED(m_Device->CreateComputeShader(warpData.constData(), warpData.size(),
                                                 nullptr, &m_WarpBlendCS[q]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC-Generic] CreateComputeShader(%s) failed", warpNames[q]);
            return false;
        }
    }

    // VipleStream: 3x3 median-filter shader for the MV field. Single
    // variant (no quality tier — deterministic filter).
    QByteArray medData = Path::readDataFile("d3d11_mv_median.fxc");
    if (medData.isEmpty()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC-Generic] d3d11_mv_median.fxc not found");
        return false;
    }
    if (FAILED(m_Device->CreateComputeShader(medData.constData(), medData.size(),
                                             nullptr, &m_MvMedianCS))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC-Generic] CreateComputeShader(mv_median) failed");
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] All shader variants loaded (Quality/Balanced/Performance + MV median)");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] Active quality: %s", qualityLabels[m_QualityLevel]);
    return true;
}

bool GenericFRUC::createTextures() {
    HRESULT hr;

    // --- Full-resolution previous frame (R8G8B8A8) ---
    D3D11_TEXTURE2D_DESC fullDesc = {};
    fullDesc.Width = m_Width;
    fullDesc.Height = m_Height;
    fullDesc.MipLevels = 1;
    fullDesc.ArraySize = 1;
    fullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    fullDesc.SampleDesc.Count = 1;
    fullDesc.Usage = D3D11_USAGE_DEFAULT;
    fullDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = m_Device->CreateTexture2D(&fullDesc, nullptr, &m_PrevFrame);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_PrevFrame.Get(), nullptr, &m_PrevFrameSRV);
    if (FAILED(hr)) return false;

    // --- Render texture (caller renders video into this via RTV) ---
    D3D11_TEXTURE2D_DESC renderDesc = fullDesc;
    renderDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = m_Device->CreateTexture2D(&renderDesc, nullptr, &m_RenderTexture);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateRenderTargetView(m_RenderTexture.Get(), nullptr, &m_RenderRTV);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_RenderTexture.Get(), nullptr, &m_RenderSRV);
    if (FAILED(hr)) return false;

    // --- 1/4 resolution textures for motion estimation ---
    D3D11_TEXTURE2D_DESC quarterDesc = fullDesc;
    quarterDesc.Width = m_QuarterWidth;
    quarterDesc.Height = m_QuarterHeight;
    quarterDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = m_Device->CreateTexture2D(&quarterDesc, nullptr, &m_PrevFrameQuarter);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_PrevFrameQuarter.Get(), nullptr, &m_PrevFrameQuarterSRV);
    if (FAILED(hr)) return false;

    hr = m_Device->CreateTexture2D(&quarterDesc, nullptr, &m_CurrFrameQuarter);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_CurrFrameQuarter.Get(), nullptr, &m_CurrFrameQuarterSRV);
    if (FAILED(hr)) return false;

    // --- Motion vector field (R16G16_SINT, block-level at 1/4 res) ---
    uint32_t mvWidth = m_QuarterWidth / BLOCK_SIZE;
    uint32_t mvHeight = m_QuarterHeight / BLOCK_SIZE;

    D3D11_TEXTURE2D_DESC mvDesc = {};
    mvDesc.Width = mvWidth;
    mvDesc.Height = mvHeight;
    mvDesc.MipLevels = 1;
    mvDesc.ArraySize = 1;
    mvDesc.Format = DXGI_FORMAT_R16G16_SINT;
    mvDesc.SampleDesc.Count = 1;
    mvDesc.Usage = D3D11_USAGE_DEFAULT;
    mvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    hr = m_Device->CreateTexture2D(&mvDesc, nullptr, &m_MotionField);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateUnorderedAccessView(m_MotionField.Get(), nullptr, &m_MotionFieldUAV);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_MotionField.Get(), nullptr, &m_MotionFieldSRV);
    if (FAILED(hr)) return false;

    // --- Previous MV field for temporal smoothing ---
    {
        D3D11_TEXTURE2D_DESC prevMvDesc = mvDesc;
        prevMvDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_Device->CreateTexture2D(&prevMvDesc, nullptr, &m_PrevMotionField);
        if (FAILED(hr)) return false;
        hr = m_Device->CreateShaderResourceView(m_PrevMotionField.Get(), nullptr, &m_PrevMotionFieldSRV);
        if (FAILED(hr)) return false;
    }

    // --- Median-filtered MV field (same format as m_MotionField) ---
    // warp reads this SRV; the 3x3 median pass writes to it via UAV.
    hr = m_Device->CreateTexture2D(&mvDesc, nullptr, &m_MotionFieldFiltered);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateUnorderedAccessView(m_MotionFieldFiltered.Get(), nullptr,
                                             &m_MotionFieldFilteredUAV);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_MotionFieldFiltered.Get(), nullptr,
                                            &m_MotionFieldFilteredSRV);
    if (FAILED(hr)) return false;

    // --- MV staging texture for diagnostic readback ---
    {
        D3D11_TEXTURE2D_DESC stagingDesc = mvDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_Device->CreateTexture2D(&stagingDesc, nullptr, &m_MotionFieldStaging);
        // Non-critical: if this fails, diagnostics just won't log MV stats
    }

    // --- Interpolated output frame (R8G8B8A8, full res) ---
    D3D11_TEXTURE2D_DESC interpDesc = fullDesc;
    interpDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    hr = m_Device->CreateTexture2D(&interpDesc, nullptr, &m_InterpTexture);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateUnorderedAccessView(m_InterpTexture.Get(), nullptr, &m_InterpUAV);
    if (FAILED(hr)) return false;
    hr = m_Device->CreateShaderResourceView(m_InterpTexture.Get(), nullptr, &m_InterpSRV);
    if (FAILED(hr)) return false;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] Textures: full %dx%d, MV %dx%d (blockSize=%d, dispatch=%dx%d)",
                m_Width, m_Height, mvWidth, mvHeight,
                BLOCK_SIZE * DOWNSCALE,
                (m_Width / (BLOCK_SIZE * DOWNSCALE) + 7) / 8,
                (m_Height / (BLOCK_SIZE * DOWNSCALE) + 7) / 8);
    return true;
}

bool GenericFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;
    m_QuarterWidth = width / DOWNSCALE;
    m_QuarterHeight = height / DOWNSCALE;

    if (!loadShaders()) return false;
    if (!createTextures()) return false;
    createTimestampQueries();  // non-critical: perf log only, continue on failure

    // Constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CBData);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_Device->CreateBuffer(&cbDesc, nullptr, &m_ConstantBuffer))) {
        return false;
    }

    // Samplers
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    m_Device->CreateSamplerState(&sampDesc, &m_PointSampler);

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    m_Device->CreateSamplerState(&sampDesc, &m_LinearSampler);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] Initialized: %dx%d (Generic compute shader FRUC)",
                width, height);
    return true;
}

void GenericFRUC::destroy() {
    for (int q = 0; q < 3; q++) {
        m_MotionEstCS[q].Reset();
        m_WarpBlendCS[q].Reset();
    }
    m_MvMedianCS.Reset();
    m_ConstantBuffer.Reset();
    m_PointSampler.Reset();
    m_LinearSampler.Reset();
    m_PrevFrame.Reset(); m_PrevFrameSRV.Reset();
    m_PrevFrameQuarter.Reset(); m_PrevFrameQuarterSRV.Reset();
    m_CurrFrameQuarter.Reset(); m_CurrFrameQuarterSRV.Reset();
    m_MotionField.Reset(); m_MotionFieldUAV.Reset(); m_MotionFieldSRV.Reset();
    m_PrevMotionField.Reset(); m_PrevMotionFieldSRV.Reset();
    m_MotionFieldFiltered.Reset();
    m_MotionFieldFilteredUAV.Reset();
    m_MotionFieldFilteredSRV.Reset();
    m_InterpTexture.Reset(); m_InterpUAV.Reset(); m_InterpSRV.Reset();
    for (int i = 0; i < TS_RING; i++) {
        m_TsDisjoint[i].Reset();
        m_TsMeBegin[i].Reset();     m_TsMeEnd[i].Reset();
        m_TsMedianBegin[i].Reset(); m_TsMedianEnd[i].Reset();
        m_TsWarpBegin[i].Reset();   m_TsWarpEnd[i].Reset();
    }
    m_TsQueriesValid = false;
    m_LastMeMs = m_LastMedianMs = m_LastWarpMs = 0.0;
}

bool GenericFRUC::submitFrame(ID3D11DeviceContext* ctx, double timestamp) {
    m_FrameCount++;

    // CRITICAL: Unbind render target before using m_RenderTexture as SRV input.
    // D3D11 silently unbinds SRVs that are also bound as render targets.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    if (m_FrameCount <= 1) {
        ctx->CopyResource(m_PrevFrame.Get(), m_RenderTexture.Get());
        return false;
    }

    // VipleStream: bracket the whole FRUC work in a disjoint query
    // so GetData() can tell us whether the GPU's timestamp clock was
    // stable over the window. Per-stage begin/end timestamps get
    // emitted around the two Dispatch() calls below.
    if (m_TsQueriesValid) {
        ctx->Begin(m_TsDisjoint[m_TsSlot].Get());
    }

    // --- Stage 2: Motion Estimation (full-res, strided block matching) ---
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_ConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        CBData* cb = (CBData*)mapped.pData;
        cb->frameWidth = m_Width;
        cb->frameHeight = m_Height;
        cb->blockSize = BLOCK_SIZE * DOWNSCALE;  // 32 pixels at full res
        cb->searchRadius = SEARCH_RADIUS;
        ctx->Unmap(m_ConstantBuffer.Get(), 0);

        // AV1 vs HEVC observation (measured 2026-04-23, A1000 @ 4K120):
        // When the host streams AV1, me_gpu and warp_gpu timestamp
        // queries report ~4× and ~2.6× longer dispatches than the
        // exact same shader bytecode reports under HEVC (1.67ms vs
        // 0.38ms ME, 1.74ms vs 0.68ms warp). It's NOT a correctness
        // problem — FRUC output is identical. The extra time is
        // GPU-hardware contention between NVDEC (busier for AV1 —
        // AV1 decode costs 2.3ms/frame vs HEVC's ~0ms overhead on
        // A1000) and the compute units this shader wants. D3D11
        // has no async-compute queue (that's a D3D12 feature), so
        // the dispatch is effectively serialized behind NVDEC's
        // outstanding work. The net delta in SwapChain GPU busy is
        // only +1.44ms/frame (vs +0.78ms with HEVC) because the
        // dispatch still overlaps with the decode tail.
        //
        // Nothing to optimize at the shader level. Recommend HEVC
        // for 4K+FRUC streaming on consumer/pro NVDEC SKUs; AV1
        // becomes net-cheaper only on GPUs with independent VDE
        // (decode engine) + compute resources or under D3D12.
        ctx->CSSetShader(m_MotionEstCS[m_QualityLevel].Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_PrevFrameSRV.Get(), m_RenderSRV.Get(), m_PrevMotionFieldSRV.Get() };
        ctx->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_MotionFieldUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, m_ConstantBuffer.GetAddressOf());
        ctx->CSSetSamplers(0, 1, m_PointSampler.GetAddressOf());

        uint32_t groupsX = (m_Width / (BLOCK_SIZE * DOWNSCALE) + 7) / 8;
        uint32_t groupsY = (m_Height / (BLOCK_SIZE * DOWNSCALE) + 7) / 8;
        if (m_TsQueriesValid) ctx->End(m_TsMeBegin[m_TsSlot].Get());
        ctx->Dispatch(groupsX, groupsY, 1);
        if (m_TsQueriesValid) ctx->End(m_TsMeEnd[m_TsSlot].Get());

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[3] = {};
        ctx->CSSetShaderResources(0, 3, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

        // VipleStream: MV field copy was originally done here,
        // immediately after the ME dispatch. That puts the copy in the
        // middle of the ME→Median→Warp chain, where the D3D11 driver
        // can serialize it against Median's read of m_MotionField.
        // Moved to the end of submitFrame (just after warp completes)
        // so the pipeline is uninterrupted. Expected win: 0.1-0.3 ms
        // on most workloads (too small to show against frame-time
        // noise, but correctness is unchanged and the code flow
        // reads more naturally).
    }

    // --- Stage 2.5: 3x3 median filter on the MV field ---
    // Kills single-block outlier MVs that would otherwise get
    // blended into neighboring static regions by warp's bilinear
    // MV sampling. Trivially cheap — 30x17 grid at 1080p.
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_ConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        CBData* cb = (CBData*)mapped.pData;
        uint32_t mvW = m_Width / (BLOCK_SIZE * DOWNSCALE);
        uint32_t mvH = m_Height / (BLOCK_SIZE * DOWNSCALE);
        cb->frameWidth = mvW;          // median shader reads as mvWidth
        cb->frameHeight = mvH;         // ...           mvHeight
        cb->blockSize = 0;             // unused by median shader
        cb->searchRadius = 0;
        ctx->Unmap(m_ConstantBuffer.Get(), 0);

        ctx->CSSetShader(m_MvMedianCS.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_MotionFieldSRV.Get() };
        ctx->CSSetShaderResources(0, 1, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_MotionFieldFilteredUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, m_ConstantBuffer.GetAddressOf());

        uint32_t groupsX = (mvW + 7) / 8;
        uint32_t groupsY = (mvH + 7) / 8;
        if (m_TsQueriesValid) ctx->End(m_TsMedianBegin[m_TsSlot].Get());
        ctx->Dispatch(groupsX, groupsY, 1);
        if (m_TsQueriesValid) ctx->End(m_TsMedianEnd[m_TsSlot].Get());

        ID3D11ShaderResourceView* nullSRVs[1] = {};
        ctx->CSSetShaderResources(0, 1, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
    }

    // --- Stage 3: Warp + Blend ---
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        ctx->Map(m_ConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        CBData* cb = (CBData*)mapped.pData;
        cb->frameWidth = m_Width;
        cb->frameHeight = m_Height;
        cb->blockSize = BLOCK_SIZE * DOWNSCALE;  // full-res block size for MV lookup
        cb->blendFactor = 0.5f;
        ctx->Unmap(m_ConstantBuffer.Get(), 0);

        ctx->CSSetShader(m_WarpBlendCS[m_QualityLevel].Get(), nullptr, 0);
        // VipleStream: warp reads the median-filtered MV field, not
        // the raw ME output. The filter kills 1-block outliers that
        // would otherwise bleed into static neighbors via bilinear
        // MV sampling.
        ID3D11ShaderResourceView* srvs[] = { m_PrevFrameSRV.Get(), m_RenderSRV.Get(), m_MotionFieldFilteredSRV.Get() };
        ctx->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_InterpUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, m_ConstantBuffer.GetAddressOf());
        ctx->CSSetSamplers(0, 1, m_LinearSampler.GetAddressOf());

        uint32_t groupsX = (m_Width + 7) / 8;
        uint32_t groupsY = (m_Height + 7) / 8;
        if (m_TsQueriesValid) ctx->End(m_TsWarpBegin[m_TsSlot].Get());
        ctx->Dispatch(groupsX, groupsY, 1);
        if (m_TsQueriesValid) ctx->End(m_TsWarpEnd[m_TsSlot].Get());

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[3] = {};
        ctx->CSSetShaderResources(0, 3, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    // End the disjoint query and consume whichever frame's queries
    // are now ready on the GPU (typically N-2). This updates
    // m_LastMeMs / m_LastWarpMs for the next periodic stats log.
    if (m_TsQueriesValid) {
        ctx->End(m_TsDisjoint[m_TsSlot].Get());
        m_TsSlot = (m_TsSlot + 1) % TS_RING;
        readTimestamps(ctx);
    }

    // --- Save current frame as previous for next iteration ---
    ctx->CopyResource(m_PrevFrame.Get(), m_RenderTexture.Get());
    // VipleStream: MV field copy moved here from inside the ME stage
    // (was the line immediately after the ME Dispatch(), which forced
    // the D3D11 driver to serialize the copy against Median's upcoming
    // read of m_MotionField). Doing it here, after warp has finished
    // reading from m_MotionFieldFiltered, means ME→Median→Warp run
    // back-to-back with no copy insertion, and the copy overlaps with
    // whatever comes after submitFrame on the GPU queue.
    ctx->CopyResource(m_PrevMotionField.Get(), m_MotionField.Get());

    // --- Diagnostic: read back MV field and log statistics ---
    if (m_FrameCount <= 5 || (m_FrameCount % 300 == 0)) {
        uint32_t mvW = m_Width / (BLOCK_SIZE * DOWNSCALE);
        uint32_t mvH = m_Height / (BLOCK_SIZE * DOWNSCALE);

        if (m_MotionFieldStaging) {
            ctx->CopyResource(m_MotionFieldStaging.Get(), m_MotionField.Get());

            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(m_MotionFieldStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                // MV values are Q1 fixed-point (actual_pixels × 2)
                float minMvX = 9999, maxMvX = -9999, minMvY = 9999, maxMvY = -9999;
                double sumAbsMv = 0;
                int nonZero = 0;

                for (uint32_t y = 0; y < mvH; y++) {
                    int16_t* row = (int16_t*)((uint8_t*)mapped.pData + y * mapped.RowPitch);
                    for (uint32_t x = 0; x < mvW; x++) {
                        float mvx = row[x * 2] * 0.5f;   // Q1 → pixels
                        float mvy = row[x * 2 + 1] * 0.5f;
                        if (mvx < minMvX) minMvX = mvx;
                        if (mvx > maxMvX) maxMvX = mvx;
                        if (mvy < minMvY) minMvY = mvy;
                        if (mvy > maxMvY) maxMvY = mvy;
                        sumAbsMv += fabs(mvx) + fabs(mvy);
                        if (mvx != 0 || mvy != 0) nonZero++;
                    }
                }
                ctx->Unmap(m_MotionFieldStaging.Get(), 0);

                int total = mvW * mvH;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC-Generic] frame=%d MV[%dx%d]: X[%.1f..%.1f] Y[%.1f..%.1f] avgAbs=%.1f nonZero=%d/%d",
                            m_FrameCount, mvW, mvH,
                            minMvX, maxMvX, minMvY, maxMvY,
                            sumAbsMv / total, nonZero, total);
            }
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-Generic] frame=%d interpolated (no staging for MV diag)", m_FrameCount);
        }
    }

    return true;
}

void GenericFRUC::skipFrame(ID3D11DeviceContext* ctx) {
    // VipleStream: Frame arrived late — skip expensive ME/warp,
    // just update prev frame so the next frame's ME has correct reference.
    m_FrameCount++;

    // Unbind render target (same safety as submitFrame)
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    ctx->CopyResource(m_PrevFrame.Get(), m_RenderTexture.Get());

    // Also copy MV field to prev MV (temporal smoothing expects it)
    if (m_PrevMotionField) {
        ctx->CopyResource(m_PrevMotionField.Get(), m_MotionField.Get());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] frame=%d SKIPPED (late arrival)", m_FrameCount);
}

// VipleStream: allocate a 3-slot ring of TIMESTAMP_DISJOINT +
// begin/end TIMESTAMP queries so submitFrame() can bracket the
// motion-est and warp dispatches with GPU-side timestamps.
// Non-critical: if any Create fails, m_TsQueriesValid stays false
// and the renderer just reports 0 ms for the two stages.
bool GenericFRUC::createTimestampQueries()
{
    D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC ts = { D3D11_QUERY_TIMESTAMP, 0 };
    for (int i = 0; i < TS_RING; i++) {
        if (FAILED(m_Device->CreateQuery(&dj, &m_TsDisjoint[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsMeBegin[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsMeEnd[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsMedianBegin[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsMedianEnd[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsWarpBegin[i])) ||
            FAILED(m_Device->CreateQuery(&ts, &m_TsWarpEnd[i]))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC-Generic] CreateQuery failed; "
                        "per-stage GPU timing will be unavailable");
            for (int j = 0; j < TS_RING; j++) {
                m_TsDisjoint[j].Reset();
                m_TsMeBegin[j].Reset();     m_TsMeEnd[j].Reset();
                m_TsMedianBegin[j].Reset(); m_TsMedianEnd[j].Reset();
                m_TsWarpBegin[j].Reset();   m_TsWarpEnd[j].Reset();
            }
            m_TsQueriesValid = false;
            return false;
        }
    }
    m_TsQueriesValid = true;
    return true;
}

// VipleStream: pick the oldest slot in the ring (after advancing
// the write cursor in submitFrame) and poll its GetData without
// blocking. If the frame has retired its queries, convert to ms
// via the disjoint frequency and update an EMA.
void GenericFRUC::readTimestamps(ID3D11DeviceContext* ctx)
{
    const int read = m_TsSlot;  // this is the slot we JUST advanced PAST
    // GetData(..., 0) returns S_FALSE if the data isn't ready yet —
    // we tolerate that silently, the next frame's read will catch up.
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    if (ctx->GetData(m_TsDisjoint[read].Get(), &dj, sizeof(dj), 0) != S_OK) return;
    if (dj.Disjoint || dj.Frequency == 0) return;
    UINT64 meBegin = 0, meEnd = 0;
    UINT64 medianBegin = 0, medianEnd = 0;
    UINT64 warpBegin = 0, warpEnd = 0;
    if (ctx->GetData(m_TsMeBegin[read].Get(),     &meBegin,     sizeof(meBegin), 0) != S_OK) return;
    if (ctx->GetData(m_TsMeEnd[read].Get(),       &meEnd,       sizeof(meEnd), 0) != S_OK) return;
    if (ctx->GetData(m_TsMedianBegin[read].Get(), &medianBegin, sizeof(medianBegin), 0) != S_OK) return;
    if (ctx->GetData(m_TsMedianEnd[read].Get(),   &medianEnd,   sizeof(medianEnd), 0) != S_OK) return;
    if (ctx->GetData(m_TsWarpBegin[read].Get(),   &warpBegin,   sizeof(warpBegin), 0) != S_OK) return;
    if (ctx->GetData(m_TsWarpEnd[read].Get(),     &warpEnd,     sizeof(warpEnd), 0) != S_OK) return;

    double toMs = 1000.0 / (double)dj.Frequency;
    double meMs     = (meEnd     > meBegin)     ? (meEnd     - meBegin)     * toMs : 0.0;
    double medianMs = (medianEnd > medianBegin) ? (medianEnd - medianBegin) * toMs : 0.0;
    double warpMs   = (warpEnd   > warpBegin)   ? (warpEnd   - warpBegin)   * toMs : 0.0;

    // EMA (0.2 weight on new sample) keeps the log line stable
    // between 5-second reports even though individual-frame times
    // wobble ~20% because of scheduler / clock-scaling noise.
    const double a = 0.2;
    m_LastMeMs     = m_LastMeMs     == 0.0 ? meMs     : a * meMs     + (1 - a) * m_LastMeMs;
    m_LastMedianMs = m_LastMedianMs == 0.0 ? medianMs : a * medianMs + (1 - a) * m_LastMedianMs;
    m_LastWarpMs   = m_LastWarpMs   == 0.0 ? warpMs   : a * warpMs   + (1 - a) * m_LastWarpMs;
}

#endif // _WIN32
