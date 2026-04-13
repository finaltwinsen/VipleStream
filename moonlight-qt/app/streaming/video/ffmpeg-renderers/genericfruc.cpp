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

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC-Generic] All shader variants loaded (Quality/Balanced/Performance)");
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
    m_ConstantBuffer.Reset();
    m_PointSampler.Reset();
    m_LinearSampler.Reset();
    m_PrevFrame.Reset(); m_PrevFrameSRV.Reset();
    m_PrevFrameQuarter.Reset(); m_PrevFrameQuarterSRV.Reset();
    m_CurrFrameQuarter.Reset(); m_CurrFrameQuarterSRV.Reset();
    m_MotionField.Reset(); m_MotionFieldUAV.Reset(); m_MotionFieldSRV.Reset();
    m_PrevMotionField.Reset(); m_PrevMotionFieldSRV.Reset();
    m_InterpTexture.Reset(); m_InterpUAV.Reset(); m_InterpSRV.Reset();
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

        ctx->CSSetShader(m_MotionEstCS[m_QualityLevel].Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = { m_PrevFrameSRV.Get(), m_RenderSRV.Get(), m_PrevMotionFieldSRV.Get() };
        ctx->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_MotionFieldUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, m_ConstantBuffer.GetAddressOf());
        ctx->CSSetSamplers(0, 1, m_PointSampler.GetAddressOf());

        uint32_t groupsX = (m_Width / (BLOCK_SIZE * DOWNSCALE) + 7) / 8;
        uint32_t groupsY = (m_Height / (BLOCK_SIZE * DOWNSCALE) + 7) / 8;
        ctx->Dispatch(groupsX, groupsY, 1);

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[3] = {};
        ctx->CSSetShaderResources(0, 3, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);

        // Copy current MV field to previous for next frame's temporal smoothing
        ctx->CopyResource(m_PrevMotionField.Get(), m_MotionField.Get());
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
        ID3D11ShaderResourceView* srvs[] = { m_PrevFrameSRV.Get(), m_RenderSRV.Get(), m_MotionFieldSRV.Get() };
        ctx->CSSetShaderResources(0, 3, srvs);
        ID3D11UnorderedAccessView* uavs[] = { m_InterpUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, m_ConstantBuffer.GetAddressOf());
        ctx->CSSetSamplers(0, 1, m_LinearSampler.GetAddressOf());

        uint32_t groupsX = (m_Width + 7) / 8;
        uint32_t groupsY = (m_Height + 7) / 8;
        ctx->Dispatch(groupsX, groupsY, 1);

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[3] = {};
        ctx->CSSetShaderResources(0, 3, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[1] = {};
        ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }

    // --- Save current frame as previous for next iteration ---
    ctx->CopyResource(m_PrevFrame.Get(), m_RenderTexture.Get());

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

#endif // _WIN32
