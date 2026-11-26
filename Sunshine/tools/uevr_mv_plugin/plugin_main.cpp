// ============================================================================
// plugin_main.cpp
// VipleStream §VR-GAME-MV (M-MV0 / M-MV1) — UEVR 遊戲內動態向量與深度擷取／降採樣／傾印／IPC 發布外掛
//
// 功能摘要：
//   1. 僅依賴 third_party/uevr/ 下的三個 MIT 標頭（API.h、API.hpp、Plugin.hpp），
//      匯出 uevr_plugin_required_version 與 uevr_plugin_initialize。
//   2. 透過 UEVR 的 register_inline_hook（或動態模組解析）攔截 _nvngx.dll / nvngx.dll 的
//      NVSDK_NGX_D3D12_CreateFeature、ReleaseFeature、EvaluateFeature（含 _C），
//      僅處理 FeatureID 1 (SuperSampling) 與 13 (RayReconstruction)。
//   3. 於 EvaluateFeature 透過 NVSDK_NGX_Parameter 虛擬函式表讀取 MotionVectors、Depth、
//      Color、MV.Scale.X/Y、Jitter.Offset.X/Y、Reset、DLSS.Render.Subrect.Dimensions.Width/Height。
//   4. 分眼判定：以 NGX Handle 為主鍵（追蹤中的目標 Handle 依建立順序配對：同時存活的兩個中
//      先建的＝左眼、後建的＝右眼；Handle 釋放或重建時重新配對）。
//      on_post_calculate_stereo_view_offset 僅作交叉驗證並統計不一致次數至 log，不再使用 Present 序號。
//   5. 幀分組：一組＝左右眼各一次評估（以遊戲幀為單位，不以 Present 為單位）。
//      發布與傾印的每一筆必定是雙眼齊備的一組；不齊備則跳過並計入 frames_dropped_incomplete。
//   6. 在遊戲傳入的 ID3D12GraphicsCommandList 上派發 cs_downsample_mv_8x8（前景優先池化），
//      以單眼正規化 UV 對齊高解析度 MV（如 2152x2152）與算圖解析度 Depth/Color（如 1436x1436），
//      並根據 UEVR 投影矩陣近平面與 world_to_meters 將 Reverse-Z 極小硬體逆深度正規化至 [0, 1] float16。
//   7. M-MV1 跨行程交接：背景連線執行緒連 \\.\pipe\VipleStreamVRMV-<session_id>（message mode），
//      驗證 Server 行程為 S-1-5-18 (LocalSystem)，完成 HELLO -> WELCOME 握手與 MapViewOfFile；
//      雙眼齊備時於右眼 EvaluateFeature 將 m_out_mv8_tex 複製至 VRMV_SLOTS 個 READBACK 環狀緩衝，
//      於第一次 on_present Signal fence 並記 game_present_qpc，由 publish 執行緒等待完成（上限 50 ms）、
//      透過 per-slot seqlock 寫入 payload 與 vrmv_desc_t、每 100 ms 更新 writer heartbeat，
//      並支援 viple_mv_delay.txt 延遲注入與 [VIPLE-GAME-MV-IPC] 每 600 遊戲幀統計 log。
//   8. 保留 M-MV0 磁碟傾印（viple_mv_dump.txt），可與 M-MV1 IPC 同時開啟。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <aclapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "third_party/uevr/Plugin.hpp"
#include "ngx_minimal.h"
#include "vr_mv_ipc_abi.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

// ============================================================================
// 內嵌備援 HLSL 原始碼（當外掛目錄下找不到 cs_downsample_mv_8x8.hlsl 時自動使用）
// 內容與同目錄下的 cs_downsample_mv_8x8.hlsl 完全一致
// ============================================================================
constexpr const char* kEmbeddedDownsampleHlsl = R"HLSL(
cbuffer DownsampleCB : register(b0)
{
    uint2  g_MvValidDim;
    uint2  g_DepthValidDim;
    uint2  g_ColorValidDim;
    uint2  g_EyeGridDim;
    uint   g_EyeIndex;
    uint   g_Flags;
    float2 g_MvScaleToUv;
    float2 g_JitterDeltaUv;
    float  g_DepthEpsilon;
    float  g_DepthRef;
    uint2  g_MvBase;
    uint2  g_DepthBase;
    uint2  g_ColorBase;
    uint2  g_RawMv4GridDim;
};

Texture2D<float2>         g_MvTex     : register(t0);
Texture2D<float>          g_DepthTex  : register(t1);
Texture2D<float4>         g_ColorTex  : register(t2);

RWTexture2D<float4>       g_OutMv8    : register(u0);
RWTexture2D<unorm float4> g_OutColor8 : register(u1);
RWTexture2D<float2>       g_OutRawMv4 : register(u2);

groupshared float2 s_Mv[64];
groupshared float  s_InvZ[64];
groupshared uint   s_Valid[64];
groupshared float3 s_Color[64];

[numthreads(8, 8, 1)]
void cs_downsample_mv_8x8(
    uint3 gid  : SV_GroupID,
    uint3 gtid : SV_GroupThreadID,
    uint  gi   : SV_GroupIndex)
{
    uint2 renderDim = max(g_ColorValidDim, uint2(1u, 1u));
    uint2 renderPx  = gid.xy * 8u + gtid.xy;
    bool inBounds   = (renderPx.x < renderDim.x) && (renderPx.y < renderDim.y);

    uint2 clampedRenderPx = min(renderPx, renderDim - 1u);
    float2 normUv = (float2(clampedRenderPx) + 0.5f) / float2(renderDim);

    uint2 mvDim = max(g_MvValidDim, uint2(1u, 1u));
    uint2 mvPx  = (mvDim.x == renderDim.x && mvDim.y == renderDim.y)
                      ? clampedRenderPx
                      : min(uint2(normUv * float2(mvDim)), mvDim - 1u);
    float2 rawMv = g_MvTex.Load(int3(g_MvBase + mvPx, 0)).xy;
    float2 uvMv = rawMv * g_MvScaleToUv - g_JitterDeltaUv;

    bool isFiniteMv = !any(isnan(uvMv)) && !any(isinf(uvMv)) &&
                      (abs(uvMv.x) <= 2.0f) && (abs(uvMv.y) <= 2.0f);

    float invZ = 0.0f;
    bool hasDepth = (g_Flags & 1u) != 0u;
    if (hasDepth)
    {
        uint2 depthDim = max(g_DepthValidDim, uint2(1u, 1u));
        uint2 depthPx  = (depthDim.x == renderDim.x && depthDim.y == renderDim.y)
                             ? clampedRenderPx
                             : min(uint2(normUv * float2(depthDim)), depthDim - 1u);
        float rawD = g_DepthTex.Load(int3(g_DepthBase + depthPx, 0)).x;
        if (!isnan(rawD) && !isinf(rawD))
        {
            bool depthInverted = (g_Flags & 2u) != 0u;
            float rawInvZ = depthInverted ? max(rawD, 0.0f) : max(1.0f - rawD, 0.0f);
            float depthRef = max(g_DepthRef, 1e-9f);
            invZ = (rawInvZ > 0.0f) ? (rawInvZ / (rawInvZ + depthRef)) : 0.0f;
        }
    }

    s_Mv[gi]    = (inBounds && isFiniteMv) ? uvMv : float2(0.0f, 0.0f);
    s_InvZ[gi]  = (inBounds && isFiniteMv) ? invZ : -1.0f;
    s_Valid[gi] = (inBounds && isFiniteMv) ? 1u : 0u;

    bool dumpColor = (g_Flags & 8u) != 0u;
    if (dumpColor)
    {
        float3 col = g_ColorTex.Load(int3(g_ColorBase + clampedRenderPx, 0)).rgb;
        if (any(isnan(col)) || any(isinf(col)))
        {
            col = float3(0.0f, 0.0f, 0.0f);
        }
        col = max(col, float3(0.0f, 0.0f, 0.0f));
        col = col / (1.0f + max(col.r, max(col.g, col.b)) * 0.25f);
        s_Color[gi] = inBounds ? saturate(col) : float3(0.0f, 0.0f, 0.0f);
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi != 0u)
    {
        return;
    }

    if (gid.x >= g_EyeGridDim.x || gid.y >= g_EyeGridDim.y)
    {
        return;
    }

    float zMax = -1.0f;
    uint totalValid = 0u;
    [unroll]
    for (uint i = 0u; i < 64u; ++i)
    {
        if (s_Valid[i] != 0u)
        {
            zMax = max(zMax, s_InvZ[i]);
            totalValid += 1u;
        }
    }

    uint2 outCoord = uint2(gid.x + g_EyeIndex * g_EyeGridDim.x, gid.y);

    if (totalValid == 0u)
    {
        g_OutMv8[outCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        float relEps = clamp(g_DepthEpsilon, 0.005f, 0.5f);
        float eps = max(0.002f, zMax * relEps);
        float threshold = (hasDepth && zMax > 0.0f) ? max(0.0f, zMax - eps) : -1.0f;

        float2 sumMv = float2(0.0f, 0.0f);
        float  sumZ  = 0.0f;
        uint   fgCount = 0u;

        for (uint j = 0u; j < 64u; ++j)
        {
            if (s_Valid[j] != 0u && s_InvZ[j] >= threshold)
            {
                sumMv += s_Mv[j];
                sumZ  += s_InvZ[j];
                fgCount += 1u;
            }
        }

        float invCount = (fgCount > 0u) ? (1.0f / float(fgCount)) : 0.0f;
        float2 avgMv = sumMv * invCount;
        float  avgZ  = sumZ * invCount;

        float varSum = 0.0f;
        for (uint k = 0u; k < 64u; ++k)
        {
            if (s_Valid[k] != 0u && s_InvZ[k] >= threshold)
            {
                float2 diff = s_Mv[k] - avgMv;
                varSum += dot(diff, diff);
            }
        }
        float avgVar = varSum * invCount;
        float validity = clamp(1.0f / (1.0f + avgVar * 256.0f), 0.05f, 1.0f);

        g_OutMv8[outCoord] = float4(avgMv.x, avgMv.y, saturate(avgZ), validity);
    }

    if (dumpColor)
    {
        float3 sumCol = float3(0.0f, 0.0f, 0.0f);
        uint colCount = 0u;
        for (uint c = 0u; c < 64u; ++c)
        {
            uint2 cPx = gid.xy * 8u + uint2(c & 7u, c >> 3u);
            if (cPx.x < renderDim.x && cPx.y < renderDim.y)
            {
                sumCol += s_Color[c];
                colCount += 1u;
            }
        }
        float3 avgCol = (colCount > 0u) ? (sumCol / float(colCount)) : float3(0.0f, 0.0f, 0.0f);
        g_OutColor8[outCoord] = float4(saturate(avgCol), 1.0f);
    }
}

[numthreads(8, 8, 1)]
void cs_downsample_raw_mv_4x4(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_RawMv4GridDim.x || dtid.y >= g_RawMv4GridDim.y)
    {
        return;
    }

    uint2 mvDim = max(g_MvValidDim, uint2(1u, 1u));
    uint2 basePx = dtid.xy * 4u;
    float2 sumRaw = float2(0.0f, 0.0f);
    uint validCount = 0u;

    [unroll]
    for (uint dy = 0u; dy < 4u; ++dy)
    {
        [unroll]
        for (uint dx = 0u; dx < 4u; ++dx)
        {
            uint2 px = basePx + uint2(dx, dy);
            if (px.x < mvDim.x && px.y < mvDim.y)
            {
                float2 v = g_MvTex.Load(int3(g_MvBase + px, 0)).xy;
                if (!any(isnan(v)) && !any(isinf(v)))
                {
                    sumRaw += v;
                    validCount += 1u;
                }
            }
        }
    }

    float2 avgRaw = (validCount > 0u) ? (sumRaw / float(validCount)) : float2(0.0f, 0.0f);
    uint2 outCoord = uint2(dtid.x + g_EyeIndex * g_RawMv4GridDim.x, dtid.y);
    g_OutRawMv4[outCoord] = avgRaw;
}
)HLSL";

// ============================================================================
// 極簡零外部依賴 PNG 寫入器（使用 zlib 無壓縮 stored block + Adler32 + CRC32）
// 用於將 RGBA8 Color 縮圖直接輸出為標準 .png 檔案
// ============================================================================
uint32_t CalcCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t CalcAdler32(const uint8_t* data, size_t len) {
    uint32_t a = 1u;
    uint32_t b = 0u;
    constexpr uint32_t kModAdler = 65521u;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % kModAdler;
        b = (b + a) % kModAdler;
    }
    return (b << 16) | a;
}

void AppendBe32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    buf.push_back(static_cast<uint8_t>(val & 0xFFu));
}

void AppendPngChunk(std::vector<uint8_t>& png, const char type[4], const uint8_t* data, size_t len) {
    AppendBe32(png, static_cast<uint32_t>(len));
    size_t type_offset = png.size();
    png.push_back(static_cast<uint8_t>(type[0]));
    png.push_back(static_cast<uint8_t>(type[1]));
    png.push_back(static_cast<uint8_t>(type[2]));
    png.push_back(static_cast<uint8_t>(type[3]));
    if (len > 0 && data != nullptr) {
        png.insert(png.end(), data, data + len);
    }
    uint32_t crc = CalcCrc32(png.data() + type_offset, 4 + len);
    AppendBe32(png, crc);
}

bool WriteRgba8Png(const fs::path& filepath, uint32_t width, uint32_t height, const uint8_t* rgba_pixels) {
    if (width == 0 || height == 0 || rgba_pixels == nullptr) {
        return false;
    }

    // 準備每列前綴 1 byte filter=0 的原始掃描線資料
    const size_t row_bytes = static_cast<size_t>(width) * 4u;
    std::vector<uint8_t> raw_scanlines;
    raw_scanlines.reserve((row_bytes + 1u) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw_scanlines.push_back(0u); // Filter type 0 (None)
        const uint8_t* row_ptr = rgba_pixels + static_cast<size_t>(y) * row_bytes;
        raw_scanlines.insert(raw_scanlines.end(), row_ptr, row_ptr + row_bytes);
    }

    // 封裝進 zlib stream（使用 deflate stored blocks，每塊最多 65535 bytes）
    std::vector<uint8_t> zlib_stream;
    zlib_stream.push_back(0x78u); // CMF: deflate, 32K window
    zlib_stream.push_back(0x01u); // FLG: no preset dict, check bits

    size_t remaining = raw_scanlines.size();
    size_t offset = 0;
    while (remaining > 0) {
        uint16_t chunk_len = static_cast<uint16_t>(std::min<size_t>(remaining, 65535u));
        bool is_final = (remaining - chunk_len == 0);
        zlib_stream.push_back(is_final ? 0x01u : 0x00u); // BFINAL + BTYPE=00 (stored)
        zlib_stream.push_back(static_cast<uint8_t>(chunk_len & 0xFFu));
        zlib_stream.push_back(static_cast<uint8_t>((chunk_len >> 8) & 0xFFu));
        uint16_t nlen = ~chunk_len;
        zlib_stream.push_back(static_cast<uint8_t>(nlen & 0xFFu));
        zlib_stream.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
        zlib_stream.insert(zlib_stream.end(),
                           raw_scanlines.data() + offset,
                           raw_scanlines.data() + offset + chunk_len);
        offset += chunk_len;
        remaining -= chunk_len;
    }

    uint32_t adler = CalcAdler32(raw_scanlines.data(), raw_scanlines.size());
    AppendBe32(zlib_stream, adler);

    std::vector<uint8_t> png;
    const uint8_t kPngSig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    png.insert(png.end(), kPngSig, kPngSig + 8);

    std::vector<uint8_t> ihdr;
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8u); // bit depth = 8
    ihdr.push_back(6u); // color type = 6 (RGBA)
    ihdr.push_back(0u); // compression = 0
    ihdr.push_back(0u); // filter = 0
    ihdr.push_back(0u); // interlace = 0
    AppendPngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    AppendPngChunk(png, "IDAT", zlib_stream.data(), zlib_stream.size());
    AppendPngChunk(png, "IEND", nullptr, 0);

    std::ofstream ofs(filepath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return ofs.good();
}

// ============================================================================
// 將 D3D12 貼圖格式（含 Typeless 與 Depth 格式）轉為可供 SRV 讀取的型別格式
// ============================================================================
DXGI_FORMAT GetSrvFormatForResource(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:
            return fmt;
    }
}

uint32_t AlignUp(uint32_t val, uint32_t alignment) {
    return (val + alignment - 1u) & ~(alignment - 1u);
}

// IEEE 754 binary16 (float16) 轉 float32，用於傾印時統計 1/8 逆深度 min/max/mean
float HalfToFloat(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp        = (h >> 10) & 0x1Fu;
    uint32_t mant       = h & 0x03FFu;
    uint32_t bits       = 0;

    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03FFu;
            bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }

    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

struct DepthGridStats {
    float min_val{0.0f};
    float max_val{0.0f};
    float mean_val{0.0f};
    uint32_t valid_cells{0};
    uint32_t nonzero_cells{0};
};

// ============================================================================
// 派發前後資源狀態對稱轉換守衛（Scoped Resource State Restorer）
// 確保若在派發前將輸入資源從 assumed_state 轉為 NON_PIXEL_SHADER_RESOURCE，
// 派發後必定以反向 Barrier 還原回遊戲傳入時的狀態。
// ============================================================================
class ScopedInputStateGuard {
public:
    ScopedInputStateGuard(ID3D12GraphicsCommandList* cmd_list)
        : m_cmd_list(cmd_list) {}

    void AddIfNeeded(ID3D12Resource* res, D3D12_RESOURCE_STATES assumed_incoming_state) {
        if (res == nullptr || m_cmd_list == nullptr) {
            return;
        }
        constexpr D3D12_RESOURCE_STATES kRequiredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // 若遊戲傳入狀態已包含 NON_PIXEL_SHADER_RESOURCE，不發多餘 barrier 以免觸發 D3D12 mismatch
        if ((assumed_incoming_state & kRequiredState) == kRequiredState) {
            return;
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = assumed_incoming_state;
        b.Transition.StateAfter = kRequiredState;
        m_pre_barriers.push_back(b);
    }

    void ApplyPreBarriers() {
        if (!m_pre_barriers.empty() && m_cmd_list != nullptr) {
            m_cmd_list->ResourceBarrier(static_cast<UINT>(m_pre_barriers.size()), m_pre_barriers.data());
        }
    }

    void RestorePostBarriers() {
        if (!m_pre_barriers.empty() && m_cmd_list != nullptr) {
            std::vector<D3D12_RESOURCE_BARRIER> post = m_pre_barriers;
            for (auto& b : post) {
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            }
            m_cmd_list->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
            m_pre_barriers.clear();
        }
    }

    ~ScopedInputStateGuard() {
        RestorePostBarriers();
    }

private:
    ID3D12GraphicsCommandList* m_cmd_list{nullptr};
    std::vector<D3D12_RESOURCE_BARRIER> m_pre_barriers;
};

// ============================================================================
// 資料結構定義
// ============================================================================
struct TrackedFeatureInfo {
    const NVSDK_NGX_Handle* handle{nullptr};
    unsigned int handle_id{0};
    NVSDK_NGX_Feature feature_id{NVSDK_NGX_Feature_Reserved0};
    int create_flags{0};
    unsigned int width{0};
    unsigned int height{0};
    unsigned int out_width{0};
    unsigned int out_height{0};
    float prev_jitter_x{0.0f};
    float prev_jitter_y{0.0f};
    bool has_prev_jitter{false};
    uint64_t create_order{0};
    int assigned_eye{-1}; // 0=Left, 1=Right（依同時存活的建立順序配對：先建＝左、後建＝右）
};

struct EyeCameraPose {
    float view_offset_pos[3]{0.0f, 0.0f, 0.0f};
    float view_offset_rot[3]{0.0f, 0.0f, 0.0f}; // pitch, yaw, roll
    float world_to_meters{100.0f};
    bool has_view_offset{false};

    float eye_offset[3]{0.0f, 0.0f, 0.0f};
    float ue_proj[4][4]{};
    bool has_vr_proj{false};
};

struct EyeSubrectMetadata {
    uint32_t color_base_x{0};
    uint32_t color_base_y{0};
    uint32_t depth_base_x{0};
    uint32_t depth_base_y{0};
    uint32_t mv_base_x{0};
    uint32_t mv_base_y{0};
    uint32_t output_base_x{0};
    uint32_t output_base_y{0};
    uint32_t render_w{0};
    uint32_t render_h{0};
    bool has_color_base_x{false};
    bool has_color_base_y{false};
    bool has_depth_base_x{false};
    bool has_depth_base_y{false};
    bool has_mv_base_x{false};
    bool has_mv_base_y{false};
    bool has_output_base_x{false};
    bool has_output_base_y{false};
    bool has_render_w{false};
    bool has_render_h{false};
};

struct EyeEvalMetadata {
    bool valid{false};
    int eye_index{0}; // 0=Left, 1=Right
    std::string eye_source{"none"};
    int cross_check_offset_eye{-1}; // stereo_view_offset 交叉驗證眼別 (-1=無, 0=左, 1=右)
    int feature_id{0};
    unsigned int handle_id{0};
    int create_flags{0};
    uint32_t render_w{0};
    uint32_t render_h{0};
    uint32_t mv_valid_w{0};
    uint32_t mv_valid_h{0};
    uint32_t base_w{0};
    uint32_t base_h{0};
    uint32_t mv_tex_w{0};
    uint32_t mv_tex_h{0};
    uint32_t mv_format{0};
    uint32_t depth_tex_w{0};
    uint32_t depth_tex_h{0};
    uint32_t depth_format{0};
    uint32_t color_tex_w{0};
    uint32_t color_tex_h{0};
    uint32_t color_format{0};
    uintptr_t mv_res_ptr{0};
    uintptr_t depth_res_ptr{0};
    uintptr_t color_res_ptr{0};
    float mv_scale_x{1.0f};
    float mv_scale_y{1.0f};
    float mv_scale_to_uv_x{0.0f};
    float mv_scale_to_uv_y{0.0f};
    float jitter_x{0.0f};
    float jitter_y{0.0f};
    float jitter_delta_uv_x{0.0f};
    float jitter_delta_uv_y{0.0f};
    float depth_ref{1e-5f};
    int reset{0};
    EyeSubrectMetadata subrect{};
    EyeCameraPose cam;
};

struct FrameDumpMetadata {
    uint64_t game_frame_index{0};
    uint64_t present_frame_index{0};
    uint32_t dump_seq_index{0};
    uint32_t dump_skipped_incomplete{0};
    bool is_last_in_batch{false};
    uint32_t eye_grid_w{0};
    uint32_t eye_grid_h{0};
    uint32_t sbs_grid_w{0};
    uint32_t sbs_grid_h{0};
    uint32_t mv8_row_pitch{0};
    uint32_t color8_row_pitch{0};
    bool     dump_raw_mv4{false};
    uint32_t raw_mv4_eye_w{0};
    uint32_t raw_mv4_eye_h{0};
    uint32_t raw_mv4_sbs_w{0};
    uint32_t raw_mv4_sbs_h{0};
    uint32_t raw_mv4_row_pitch{0};
    uint32_t readback_slot{0};
    uint64_t fence_value{0};

    EyeEvalMetadata eyes[2];

    bool hmd_pose_valid{false};
    float hmd_pos[3]{0.0f, 0.0f, 0.0f};
    float hmd_quat[4]{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    float hmd_transform[4][4]{};
};

// Compute Shader 根常數結構（24 個 32-bit DWORD = 96 bytes）
struct alignas(16) DownsampleRootConstants {
    uint32_t mv_valid_dim[2];
    uint32_t depth_valid_dim[2];
    uint32_t color_valid_dim[2];
    uint32_t eye_grid_dim[2];
    uint32_t eye_index;
    uint32_t flags;
    float    mv_scale_to_uv[2];
    float    jitter_delta_uv[2];
    float    depth_epsilon;
    float    depth_ref;
    uint32_t mv_base[2];
    uint32_t depth_base[2];
    uint32_t color_base[2];
    uint32_t raw_mv4_grid_dim[2];
};
static_assert(sizeof(DownsampleRootConstants) == 96, "DownsampleRootConstants must be 96 bytes (24 DWORDs)");

constexpr uint32_t kDescRingSlots = 64u;
constexpr uint32_t kDescsPerSlot  = 8u; // 0..2: SRV (t0..t2), 3..5: UAV (u0..u2)
constexpr uint32_t kReadbackSlots = 8u;

struct ReadbackSlotResources {
    ComPtr<ID3D12Resource> mv8_buf;
    ComPtr<ID3D12Resource> color8_buf;
    ComPtr<ID3D12Resource> raw_mv4_buf;
    uint64_t allocated_mv8_bytes{0};
    uint64_t allocated_color8_bytes{0};
    uint64_t allocated_raw_mv4_bytes{0};
    std::atomic<bool> in_use{false};
};

// ============================================================================
// M-MV1 跨行程 IPC、QPC 與 Server 身分驗證輔助函式
// 遵守 vr_mv_ipc_abi.h 規則 A～D 與 vr_ipc_abi.h 規則 1～8
// ============================================================================
int64_t QueryQpcNow() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

int64_t QueryQpcFreq() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return (f.QuadPart > 0) ? f.QuadPart : 10000000ll;
}

int64_t MsToQpc(int64_t qpf, int64_t ms) {
    return (qpf / 1000ll) * ms + ((qpf % 1000ll) * ms) / 1000ll;
}

// 驗證 server 透過 DuplicateHandle 傳入本行程的 handle 值合理性（非 0、4 的倍數、32-bit 範圍）
constexpr bool IsPlausibleHandleValue(uint64_t v) {
    return v != 0ull && (v & 3ull) == 0ull && v <= 0xFFFFFFFFull;
}

HANDLE ToWinHandle(uint64_t v) {
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v));
}

void CloseIfPlausibleHandle(uint64_t v) {
    if (IsPlausibleHandleValue(v)) {
        CloseHandle(ToWinHandle(v));
    }
}

const char* VrmvRejectReasonName(uint32_t reason) {
    switch (reason) {
        case VRMV_REJ_ABI_MISMATCH: return "ABI_MISMATCH";
        case VRMV_REJ_IDENTITY:     return "IDENTITY";
        case VRMV_REJ_BAD_MESSAGE:  return "BAD_MESSAGE";
        case VRMV_REJ_BUSY:         return "BUSY";
        case VRMV_REJ_DISABLED:     return "DISABLED";
        default:                    return "UNKNOWN";
    }
}

constexpr uint32_t kVrmvMinMsgBytes       = 24u;     // vrmv_reject_t / vrmv_bye_t / vrmv_hello_t 凍結前綴皆為 24 B
constexpr uint32_t kIpcWriteTimeoutMs     = 200u;    // Pipe 寫入逾時上限
constexpr uint32_t kIpcWelcomeTimeoutMs   = 2000u;   // HELLO -> WELCOME/REJECT 等待上限
constexpr uint32_t kIpcFenceWaitTimeoutMs = 50u;     // publish 執行緒等待 GPU readback 完成上限 (50 ms)
constexpr uint32_t kIpcHeartbeatIntervalMs = 100u;   // writer heartbeat 更新週期 (100 ms)
constexpr uint32_t kIpcDelayReloadMs      = 2000u;   // viple_mv_delay.txt 重讀週期 (2 s)
constexpr uint32_t kIpcBackoffInitMs      = 1000u;   // 一般退避重試起始值 (1 s)
constexpr uint32_t kIpcBackoffMaxMs       = 10000u;  // 一般退避重試上限 (10 s)
constexpr uint32_t kIpcBackoffLongMs      = 30000u;  // VRMV_REJ_DISABLED / VRMV_REJ_IDENTITY 重試間隔 (30 s)

// 計算一般連線失敗或被拒時的退避時間（1 s 起，每次 ×2，上限 10 s）
uint32_t CalcIpcBackoffMs(uint32_t consecutive_failures) {
    if (consecutive_failures <= 1u) {
        return kIpcBackoffInitMs;
    }
    uint32_t ms = kIpcBackoffInitMs;
    for (uint32_t i = 1u; i < consecutive_failures && ms < kIpcBackoffMaxMs; ++i) {
        ms = std::min<uint32_t>(ms * 2u, kIpcBackoffMaxMs);
    }
    return std::min<uint32_t>(ms, kIpcBackoffMaxMs);
}

// 先驗凍結的 pipe 訊息 header（vr_ipc_abi.h 規則 7）：bytes >= 24、<= VRMV_PIPE_MAX_MSG、magic、hdr.size == bytes、flags == 0
bool CheckVrmvMsgHeader(const void* buf, uint32_t bytes, vrmv_msg_hdr_t& out_hdr, const char*& out_why) {
    if (bytes < kVrmvMinMsgBytes) {
        out_why = "short-msg";
        return false;
    }
    if (bytes > VRMV_PIPE_MAX_MSG) {
        out_why = "oversize-msg";
        return false;
    }
    std::memcpy(&out_hdr, buf, sizeof(out_hdr));
    if (out_hdr.magic != VRMV_MAGIC_PIPE) {
        out_why = "bad-pipe-magic";
        return false;
    }
    if (out_hdr.size != bytes) {
        out_why = "hdr-size-mismatch";
        return false;
    }
    if (out_hdr.flags != 0u) {
        out_why = "bad-hdr-flags";
        return false;
    }
    out_why = "ok";
    return true;
}

// 驗證 Named Pipe Server 身分（對照需求 1 與 ipc_client.cpp）：
//   1. GetNamedPipeServerProcessId 取得 server PID，透過 OpenProcess + OpenProcessToken(TokenUser)
//      驗證使用者 SID 必須是 S-1-5-18 (WinLocalSystemSid / LocalSystem)。
//   2. 同時照 ipc_client.cpp 以 GetSecurityInfo(SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)
//      驗證 pipe 擁有者 SID 為 S-1-5-18 且 DACL 為 protected，並確認同 session。
//      （若同 session 一般使用者假冒 server，步驟 1 必定能開啟其 Token 並檢出 SID != S-1-5-18 而拒絕；
//       若 Medium Integrity 遊戲行程對 LocalSystem 服務行程呼叫 OpenProcess/OpenProcessToken 遭遇 ERROR_ACCESS_DENIED，
//       則由步驟 2 確認 pipe 核心物件擁有者確為 S-1-5-18 且具 protected DACL，雙重確保只連真正的 LocalSystem server。）
bool VerifyVrmvPipeServerIdentity(
    HANDLE pipe,
    DWORD own_session,
    ULONG& out_server_pid,
    const char*& out_why,
    DWORD& out_err) {
    out_server_pid = 0;
    out_why = "ok";
    out_err = 0;

    ULONG server_session = 0;
    if (!GetNamedPipeServerSessionId(pipe, &server_session)) {
        out_why = "server-session-query";
        out_err = GetLastError();
        return false;
    }
    if (server_session != own_session) {
        out_why = "session-mismatch";
        out_err = server_session;
        return false;
    }

    if (!GetNamedPipeServerProcessId(pipe, &out_server_pid) || out_server_pid == 0) {
        out_why = "server-pid-query";
        out_err = GetLastError();
        return false;
    }

    bool token_verified_system = false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out_server_pid);
    if (proc != nullptr) {
        HANDLE tok = nullptr;
        if (OpenProcessToken(proc, TOKEN_QUERY, &tok) && tok != nullptr) {
            DWORD needed = 0;
            GetTokenInformation(tok, TokenUser, nullptr, 0, &needed);
            if (needed == 0 || needed > 4096u) {
                out_err = GetLastError();
                CloseHandle(tok);
                CloseHandle(proc);
                out_why = "token-user-size";
                return false;
            }
            std::vector<uint8_t> tok_buf(needed, 0);
            if (!GetTokenInformation(tok, TokenUser, tok_buf.data(), needed, &needed)) {
                out_err = GetLastError();
                CloseHandle(tok);
                CloseHandle(proc);
                out_why = "token-user-query";
                return false;
            }
            const auto* tu = reinterpret_cast<const TOKEN_USER*>(tok_buf.data());
            const bool is_local_system =
                (tu->User.Sid != nullptr) &&
                (IsValidSid(tu->User.Sid) != FALSE) &&
                (IsWellKnownSid(tu->User.Sid, WinLocalSystemSid) != FALSE); // S-1-5-18
            CloseHandle(tok);
            CloseHandle(proc);
            if (!is_local_system) {
                out_why = "token-sid-not-S-1-5-18";
                return false;
            }
            token_verified_system = true;
        } else {
            const DWORD tok_err = GetLastError();
            CloseHandle(proc);
            if (tok_err != ERROR_ACCESS_DENIED) {
                out_why = "open-process-token";
                out_err = tok_err;
                return false;
            }
        }
    } else {
        const DWORD proc_err = GetLastError();
        if (proc_err != ERROR_ACCESS_DENIED) {
            out_why = "open-server-process";
            out_err = proc_err;
            return false;
        }
    }

    // 照 ipc_client.cpp:653-680 驗證 pipe 物件擁有者為 S-1-5-18 (WinLocalSystemSid) 且 DACL protected
    bool pipe_owner_verified_system = false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetSecurityInfo(
        pipe,
        SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner,
        nullptr,
        &dacl,
        nullptr,
        &sd);
    if (rc == ERROR_SUCCESS) {
        const bool owner_system =
            (owner != nullptr) &&
            (IsValidSid(owner) != FALSE) &&
            (IsWellKnownSid(owner, WinLocalSystemSid) != FALSE); // S-1-5-18
        SECURITY_DESCRIPTOR_CONTROL ctl = 0;
        DWORD rev = 0;
        const bool ctl_ok = (GetSecurityDescriptorControl(sd, &ctl, &rev) != FALSE);
        const bool has_dacl = (dacl != nullptr);
        LocalFree(sd);

        if (!owner_system) {
            out_why = "pipe-owner-not-S-1-5-18";
            return false;
        }
        if (!ctl_ok || (ctl & SE_DACL_PROTECTED) == 0) {
            out_why = "pipe-dacl-not-protected";
            return false;
        }
        if (!has_dacl) {
            out_why = "pipe-dacl-null";
            return false;
        }
        pipe_owner_verified_system = true;
    } else if (!token_verified_system) {
        out_why = "secinfo-and-token-access-denied";
        out_err = rc;
        return false;
    }

    if (!token_verified_system && !pipe_owner_verified_system) {
        out_why = "unverified-S-1-5-18";
        return false;
    }
    return true;
}

// Overlapped Pipe 寫入（附逾時與 CancelIoEx，絕不呼叫 FlushFileBuffers）
bool WriteVrmvPipeMsg(HANDLE pipe, OVERLAPPED& wov, void* msg, uint32_t size, uint32_t& io_seq_out) {
    auto* hdr = static_cast<vrmv_msg_hdr_t*>(msg);
    hdr->seq = ++io_seq_out;
    ResetEvent(wov.hEvent);
    if (!WriteFile(pipe, msg, size, nullptr, &wov)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            return false;
        }
    }
    if (WaitForSingleObject(wov.hEvent, kIpcWriteTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &wov);
        DWORD transferred = 0;
        GetOverlappedResult(pipe, &wov, &transferred, TRUE);
        return false;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &wov, &transferred, FALSE) || transferred != size) {
        return false;
    }
    return true;
}

// 一次成功握手並映射的 SHM Session（以 shared_ptr 管理生命週期，最後一個參照釋放時才 UnmapViewOfFile）
struct IpcMappedSession {
    vrmv_shm_t* shm{nullptr};
    uint64_t generation{0};
    uint64_t section_size{0};
    int64_t qpc_frequency{0};
    uint32_t server_pid{0};
    std::atomic<bool> active{true};

    // 僅由單一 publish 執行緒存取的本地計數器（遵守 ABI 規則 6：絕不從 shm 讀回自己寫過的值）
    uint64_t local_mv_frame_id{0};
    uint64_t local_slot_write_count[VRMV_SLOTS]{0, 0, 0, 0};
    uint64_t last_published_game_frame{0};
    int64_t next_heartbeat_qpc{0};

    ~IpcMappedSession() {
        if (shm != nullptr) {
            UnmapViewOfFile(shm);
            shm = nullptr;
        }
    }
};

struct IpcReadbackSlot {
    ComPtr<ID3D12Resource> mv8_buf;
    uint64_t allocated_bytes{0};
    uint64_t pending_fence_value{0};
    std::atomic<bool> in_use{false};
};

struct IpcPublishTask {
    uint64_t game_frame_index{0};
    uint32_t rb_slot_index{0};
    uint32_t sbs_grid_w{0};
    uint32_t sbs_grid_h{0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t footprint_total_bytes{0};
    uint64_t fence_value{0};
    int64_t game_present_qpc{0};
    EyeEvalMetadata eyes[2]{};
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> mv8_buf;
};

} // namespace

// ============================================================================
// VipleUevrMvPlugin 主類別
// ============================================================================
class VipleUevrMvPlugin final : public uevr::Plugin {
public:
    VipleUevrMvPlugin() = default;

    ~VipleUevrMvPlugin() override {
        ShutdownWorker();
    }

    void on_dllmain() override {
        ResolvePluginDirectory();
    }

    void on_initialize() override {
        ResolvePluginDirectory();
        m_qpc_freq = QueryQpcFreq();
        m_self_pid = GetCurrentProcessId();
        LogInfo("[VIPLE-GAME-MV] Initializing viple_uevr_mv_plugin (M-MV0/M-MV1) in dir: %s",
                m_plugin_dir.u8string().c_str());

        StartDumpWorker();
        StartIpcWorkers();
        TryInstallNgxHooks();
    }

    void on_device_reset() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogWarn("[VIPLE-GAME-MV] on_device_reset triggered, releasing D3D12 pipeline resources");
        ReleaseD3D12ResourcesLocked();
    }

    // ------------------------------------------------------------------------
    // 分眼首選訊號：UE 引擎在準備某一眼的 View 時呼叫 CalculateStereoViewOffset
    // 注意 API.h:175：當 is_double == true 時（UE5 Large World Coordinates），
    // position 與 rotation 實際指向 UEVR_Vector3d* 與 UEVR_Rotatord*！
    // ------------------------------------------------------------------------
    void on_post_calculate_stereo_view_offset(
        UEVR_StereoRenderingDeviceHandle /*device*/,
        int view_index,
        float world_to_meters,
        UEVR_Vector3f* position,
        UEVR_Rotatorf* rotation,
        bool is_double) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 對照 UEVR FFakeStereoRenderingHook.cpp:5215-5230：
        // UE EStereoscopicPass 在雙眼立體算繪時通常為 eSSP_PRIMARY=1 (左眼)、eSSP_SECONDARY=2 (右眼)，
        // 而 eSSP_FULL=0 為單眼（如 UI WorldToScreen）。一旦觀察過 view_index == 2，
        // 即確認立體視圖從 1 起算 (1=Left, 2=Right)，此時 view_index == 0 應忽略。
        if (view_index == 2) {
            m_view_index_observed_two = true;
        } else if (view_index == 0 && !m_view_index_observed_two) {
            m_view_index_observed_zero = true;
        }

        if (m_view_index_observed_two && view_index == 0) {
            return;
        }

        int eye = 0;
        if (m_view_index_observed_two || !m_view_index_observed_zero) {
            eye = (view_index >= 1) ? ((view_index - 1) % 2) : 0;
        } else {
            eye = (view_index >= 0) ? (view_index % 2) : 0;
        }
        eye = std::clamp(eye, 0, 1);

        EyeCameraPose& cam = m_latest_eye_cam[eye];
        cam.world_to_meters = world_to_meters;
        cam.has_view_offset = true;

        if (position != nullptr) {
            if (is_double) {
                const auto* pos_d = reinterpret_cast<const UEVR_Vector3d*>(position);
                cam.view_offset_pos[0] = static_cast<float>(pos_d->x);
                cam.view_offset_pos[1] = static_cast<float>(pos_d->y);
                cam.view_offset_pos[2] = static_cast<float>(pos_d->z);
            } else {
                cam.view_offset_pos[0] = position->x;
                cam.view_offset_pos[1] = position->y;
                cam.view_offset_pos[2] = position->z;
            }
        }

        if (rotation != nullptr) {
            if (is_double) {
                const auto* rot_d = reinterpret_cast<const UEVR_Rotatord*>(rotation);
                cam.view_offset_rot[0] = static_cast<float>(rot_d->pitch);
                cam.view_offset_rot[1] = static_cast<float>(rot_d->yaw);
                cam.view_offset_rot[2] = static_cast<float>(rot_d->roll);
            } else {
                cam.view_offset_rot[0] = rotation->pitch;
                cam.view_offset_rot[1] = rotation->yaw;
                cam.view_offset_rot[2] = rotation->roll;
            }
        }

        // 將眼別推入 FIFO 佇列（上限 4 筆），供隨後在 Render Thread 執行的 EvaluateFeature 消費
        if (m_stereo_view_queue.size() >= 4) {
            m_stereo_view_queue.pop_front();
        }
        m_stereo_view_queue.push_back(eye);
    }

    // ------------------------------------------------------------------------
    // on_present：觸發已湊齊雙眼之遊戲幀的非同步傾印 Fence Signal、每 600 個遊戲幀輸出統計 log
    // 注意：UEVR 每個遊戲幀可能呼叫 Present 兩次，故不以 on_present 重置左右眼分組狀態。
    // ------------------------------------------------------------------------
    void on_present() override {
        if (!m_ngx_hooked.load(std::memory_order_acquire)) {
            TryInstallNgxHooks();
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_present_count++;
        m_stats_window_presents++;

        // M-MV1：雙眼齊備的那個遊戲幀第一次 Present：在遊戲的 command queue 上 Signal 外掛自己的 IPC fence，
        // 記下 game_present_qpc，交由 publish 執行緒等待與寫入 SHM
        if (!m_staged_ipc_tasks.empty()) {
            SubmitAsyncIpcOnPresentLocked();
        }

        // M-MV0：若有已經湊齊雙眼並完成 CopyTextureRegion 至傾印 Readback Buffer 的遊戲幀組，
        // 在 CommandQueue 上 Signal Fence 交由傾印背景執行緒等待與寫檔
        if (!m_staged_dump_tasks.empty()) {
            SubmitAsyncDumpOnPresentLocked();
        }

        // 每滿 600 個遊戲幀輸出一次統計 log（涵蓋 [VIPLE-GAME-MV] 與 [VIPLE-GAME-MV-IPC]）
        if (m_stats_window_game_frames >= 600) {
            const double inv_gf = 1.0 / static_cast<double>(m_stats_window_game_frames);
            LogInfo(
                "[VIPLE-GAME-MV] 600-game-frame stats (total_game_frames=%llu, total_presents=%llu, window_presents=%u): "
                "calls_per_game_frame=(L:%.2f, R:%.2f), stereo_game_frames=%u/%u (incomplete_skipped=%u), "
                "handles=(L:%u, R:%u), eye_mismatch=%u (offset_match=%u, offset_mismatch=%u, offset_missing=%u), "
                "mvScale=(%.4f, %.4f), render=(%ux%u)->sbs_grid=(%ux%u)",
                static_cast<unsigned long long>(m_game_frame_index),
                static_cast<unsigned long long>(m_present_count),
                m_stats_window_presents,
                static_cast<double>(m_stats_left_calls) * inv_gf,
                static_cast<double>(m_stats_right_calls) * inv_gf,
                m_stats_complete_game_frames,
                m_stats_window_game_frames,
                m_stats_incomplete_game_frames,
                m_paired_handle_ids[0],
                m_paired_handle_ids[1],
                m_stats_offset_mismatch,
                m_stats_offset_match,
                m_stats_offset_mismatch,
                m_stats_offset_missing,
                m_last_mv_scale_x,
                m_last_mv_scale_y,
                m_last_render_w,
                m_last_render_h,
                m_eye_grid_w * 2u,
                m_eye_grid_h);
            for (int e = 0; e < 2; ++e) {
                const EyeSubrectMetadata& sr = m_last_subrect[e];
                LogInfo(
                    "[VIPLE-GAME-MV] 600-game-frame subrect[%s]: "
                    "Color.Base=(%u,%u)[set:%d,%d] Depth.Base=(%u,%u)[set:%d,%d] "
                    "MV.Subrect.Base=(%u,%u)[set:%d,%d] Output.Base=(%u,%u)[set:%d,%d] "
                    "Render.Subrect.Dim=(%ux%u)[set:%d,%d]",
                    (e == 0) ? "Left" : "Right",
                    sr.color_base_x, sr.color_base_y, sr.has_color_base_x ? 1 : 0, sr.has_color_base_y ? 1 : 0,
                    sr.depth_base_x, sr.depth_base_y, sr.has_depth_base_x ? 1 : 0, sr.has_depth_base_y ? 1 : 0,
                    sr.mv_base_x, sr.mv_base_y, sr.has_mv_base_x ? 1 : 0, sr.has_mv_base_y ? 1 : 0,
                    sr.output_base_x, sr.output_base_y, sr.has_output_base_x ? 1 : 0, sr.has_output_base_y ? 1 : 0,
                    sr.render_w, sr.render_h, sr.has_render_w ? 1 : 0, sr.has_render_h ? 1 : 0);
            }
            LogInfo("[VIPLE-GAME-MV] eval entry calls (cumulative): plain=%llu c=%llu",
                    static_cast<unsigned long long>(m_eval_calls_plain.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(m_eval_calls_c.load(std::memory_order_relaxed)));

            LogIpc600GameFrameStatsLocked(m_stats_window_game_frames);

            m_stats_window_game_frames     = 0;
            m_stats_window_presents        = 0;
            m_stats_left_calls             = 0;
            m_stats_right_calls            = 0;
            m_stats_complete_game_frames   = 0;
            m_stats_incomplete_game_frames = 0;
            m_stats_offset_match           = 0;
            m_stats_offset_mismatch        = 0;
            m_stats_offset_missing         = 0;
        }

        // 檢查外掛目錄下是否出現 viple_mv_dump.txt 觸發檔
        CheckDumpTriggerFileLocked();
    }

    // ========================================================================
    // NGX Hook 處理常式
    // ========================================================================
    NVSDK_NGX_Result HandleCreateFeature(
        ID3D12GraphicsCommandList* cmd_list,
        NVSDK_NGX_Feature feature_id,
        NVSDK_NGX_Parameter* params,
        NVSDK_NGX_Handle** out_handle) {
        NVSDK_NGX_Result res = m_orig_create_feature(cmd_list, feature_id, params, out_handle);

        if (!NVSDK_NGX_SUCCEED(res) || out_handle == nullptr || *out_handle == nullptr) {
            return res;
        }

        // 只處理 FeatureID 1 (SuperSampling) 與 13 (RayReconstruction)
        if (feature_id != NVSDK_NGX_Feature_SuperSampling &&
            feature_id != NVSDK_NGX_Feature_RayReconstruction) {
            LogInfo("[VIPLE-GAME-MV] Ignoring non-target NGX FeatureID=%d (Handle=%p)",
                    static_cast<int>(feature_id),
                    static_cast<const void*>(*out_handle));
            return res;
        }

        TrackedFeatureInfo info{};
        info.handle = *out_handle;
        info.handle_id = (*out_handle)->Id;
        info.feature_id = feature_id;

        if (params != nullptr) {
            params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &info.create_flags);
            params->Get(NVSDK_NGX_Parameter_Width, &info.width);
            params->Get(NVSDK_NGX_Parameter_Height, &info.height);
            params->Get(NVSDK_NGX_Parameter_OutWidth, &info.out_width);
            params->Get(NVSDK_NGX_Parameter_OutHeight, &info.out_height);
        }

        int assigned_eye = -1;
        uint64_t create_order = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            info.create_order = ++m_feature_create_seq;
            create_order = info.create_order;
            m_tracked_features[*out_handle] = info;
            ReassignTrackedHandleEyesLocked();
            assigned_eye = m_tracked_features[*out_handle].assigned_eye;

            // 注意：UE5 首幀建立 DLSS Feature 的順序為：
            //   CreateFeature(Left) -> EvaluateFeature(Left) -> CreateFeature(Right) -> EvaluateFeature(Right)
            // 因此當新建立的是右眼 (assigned_eye == 1) 且左眼剛剛在同幀完成評估 (m_group_eye_ready[0] == true) 時，
            // 絕不可清空 m_group_eye_ready[0]，否則會吃掉同幀左眼並導致後續所有幀變成 (T-1 右眼 + T 左眼) 跨幀錯位！
            if (assigned_eye != 1 || m_group_eye_ready[1]) {
                if (m_group_eye_ready[0] || m_group_eye_ready[1]) {
                    RecordIncompleteGameFrameLocked();
                }
            }
        }

        LogInfo(
            "[VIPLE-GAME-MV] Tracked NGX CreateFeature: FeatureID=%d, Handle=%p (Id=%u, order=%llu, eye=%s), "
            "Flags=0x%x, In=(%ux%u), Out=(%ux%u)",
            static_cast<int>(feature_id),
            static_cast<const void*>(*out_handle),
            info.handle_id,
            static_cast<unsigned long long>(create_order),
            (assigned_eye == 0) ? "Left" : ((assigned_eye == 1) ? "Right" : "Unpaired"),
            info.create_flags,
            info.width,
            info.height,
            info.out_width,
            info.out_height);

        return res;
    }

    NVSDK_NGX_Result HandleReleaseFeature(NVSDK_NGX_Handle* handle) {
        if (handle != nullptr) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tracked_features.find(handle);
            if (it != m_tracked_features.end()) {
                LogInfo("[VIPLE-GAME-MV] Releasing tracked NGX Handle=%p (Id=%u, FeatureID=%d, order=%llu)",
                        static_cast<const void*>(handle),
                        it->second.handle_id,
                        static_cast<int>(it->second.feature_id),
                        static_cast<unsigned long long>(it->second.create_order));
                m_tracked_features.erase(it);
                ReassignTrackedHandleEyesLocked();

                if (m_group_eye_ready[0] || m_group_eye_ready[1]) {
                    RecordIncompleteGameFrameLocked();
                }
            }
        }
        return m_orig_release_feature(handle);
    }

    NVSDK_NGX_Result HandleEvaluateFeature(
        ID3D12GraphicsCommandList* cmd_list,
        const NVSDK_NGX_Handle* handle,
        const NVSDK_NGX_Parameter* params,
        PFN_NVSDK_NGX_ProgressCallback callback,
        bool c_entry = false) {
        // 兩個入口（EvaluateFeature 與 EvaluateFeature_C）共用擷取邏輯。_C 內部若再呼叫非 _C 版，
        // 同一次評估只處理一次：外層設旗標，內層看到旗標就直接轉呼叫原函式。
        static thread_local int s_depth = 0;
        const bool outer = (s_depth == 0);
        if (outer && cmd_list != nullptr && handle != nullptr && params != nullptr) {
            (c_entry ? m_eval_calls_c : m_eval_calls_plain).fetch_add(1, std::memory_order_relaxed);
            ProcessEvaluateFeaturePre(cmd_list, handle, params);
        }
        ++s_depth;
        NVSDK_NGX_Result r = c_entry ? m_orig_evaluate_feature_c(cmd_list, handle, params, callback)
                                     : m_orig_evaluate_feature(cmd_list, handle, params, callback);
        --s_depth;
        return r;
    }

    // 每個入口實際被呼叫的次數（只算外層），給統計 log 判斷遊戲走哪一條
    std::atomic<uint64_t> m_eval_calls_plain{0};
    std::atomic<uint64_t> m_eval_calls_c{0};
    PFN_NVSDK_NGX_D3D12_EvaluateFeature_C m_orig_evaluate_feature_c{nullptr};
    int m_hook_id_evaluate_c{-1};

private:
    // ========================================================================
    // 依存活 NGX Handle 建立順序配對左右眼（同時存活的兩個中：先建＝左眼 0、後建＝右眼 1）
    // ========================================================================
    void ReassignTrackedHandleEyesLocked() {
        std::vector<TrackedFeatureInfo*> active;
        active.reserve(m_tracked_features.size());
        for (auto& kv : m_tracked_features) {
            active.push_back(&kv.second);
        }
        std::sort(active.begin(), active.end(),
                  [](const TrackedFeatureInfo* a, const TrackedFeatureInfo* b) {
                      if (a->create_order != b->create_order) {
                          return a->create_order < b->create_order;
                      }
                      return a->handle_id < b->handle_id;
                  });

        m_paired_handle_ids[0] = 0;
        m_paired_handle_ids[1] = 0;

        if (active.empty()) {
            return;
        }
        if (active.size() == 1) {
            active[0]->assigned_eye = 0;
            m_paired_handle_ids[0]  = active[0]->handle_id;
        } else {
            for (size_t i = 0; i < active.size(); ++i) {
                active[i]->assigned_eye = static_cast<int>(i % 2);
            }
            // 以最新同時存活的一對依建立順序配對：先建＝左眼 (0)、後建＝右眼 (1)
            TrackedFeatureInfo* left_feat  = active[active.size() - 2];
            TrackedFeatureInfo* right_feat = active[active.size() - 1];
            left_feat->assigned_eye  = 0;
            right_feat->assigned_eye = 1;
            m_paired_handle_ids[0]   = left_feat->handle_id;
            m_paired_handle_ids[1]   = right_feat->handle_id;
        }

        LogInfo("[VIPLE-GAME-MV] Paired NGX handles by creation order (active=%zu): Left=Id:%u, Right=Id:%u",
                active.size(),
                m_paired_handle_ids[0],
                m_paired_handle_ids[1]);
    }

    // ========================================================================
    // EvaluateFeature 擷取、NGX Handle 分眼、遊戲幀雙眼分組與 D3D12 Compute Shader 派發
    // ========================================================================
    void ProcessEvaluateFeaturePre(
        ID3D12GraphicsCommandList* cmd_list,
        const NVSDK_NGX_Handle* handle,
        const NVSDK_NGX_Parameter* params) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_tracked_features.find(handle);
        if (it == m_tracked_features.end()) {
            return;
        }
        TrackedFeatureInfo& feat = it->second;

        // 1. 透過 NVSDK_NGX_Parameter 虛擬函式表讀取公開參數與子區域（Subrect）參數
        ID3D12Resource* mv_res    = nullptr;
        ID3D12Resource* depth_res = nullptr;
        ID3D12Resource* color_res = nullptr;
        float mv_scale_x = 1.0f;
        float mv_scale_y = 1.0f;
        float jitter_x   = 0.0f;
        float jitter_y   = 0.0f;
        int   reset      = 0;

        EyeSubrectMetadata subrect{};
        unsigned int tmp_u = 0;

        params->Get(NVSDK_NGX_Parameter_MotionVectors, &mv_res);
        params->Get(NVSDK_NGX_Parameter_Depth, &depth_res);
        params->Get(NVSDK_NGX_Parameter_Color, &color_res);

        if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale_x))) {
            mv_scale_x = 1.0f;
        }
        if (NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale_y))) {
            mv_scale_y = 1.0f;
        }
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitter_x);
        params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitter_y);
        params->Get(NVSDK_NGX_Parameter_Reset, &reset);

        // 讀取官方 nvsdk_ngx_defs.h 定義之每次評估子區域參數
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &tmp_u))) {
            subrect.color_base_x = tmp_u;
            subrect.has_color_base_x = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &tmp_u))) {
            subrect.color_base_y = tmp_u;
            subrect.has_color_base_y = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &tmp_u))) {
            subrect.depth_base_x = tmp_u;
            subrect.has_depth_base_x = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &tmp_u))) {
            subrect.depth_base_y = tmp_u;
            subrect.has_depth_base_y = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &tmp_u)) ||
            NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_NoDot_X, &tmp_u))) {
            subrect.mv_base_x = tmp_u;
            subrect.has_mv_base_x = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &tmp_u)) ||
            NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_NoDot_Y, &tmp_u))) {
            subrect.mv_base_y = tmp_u;
            subrect.has_mv_base_y = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &tmp_u))) {
            subrect.output_base_x = tmp_u;
            subrect.has_output_base_x = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &tmp_u))) {
            subrect.output_base_y = tmp_u;
            subrect.has_output_base_y = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &tmp_u))) {
            subrect.render_w = tmp_u;
            subrect.has_render_w = true;
        }
        tmp_u = 0;
        if (NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &tmp_u))) {
            subrect.render_h = tmp_u;
            subrect.has_render_h = true;
        }
        const unsigned int subrect_w = subrect.render_w;
        const unsigned int subrect_h = subrect.render_h;

        if (mv_res == nullptr) {
            if (m_group_eye_ready[0] || m_group_eye_ready[1]) {
                RecordIncompleteGameFrameLocked();
            }
            return;
        }

        // 2. 分眼判定：以 NGX Handle 建立順序配對為主鍵（先建＝左眼 0、後建＝右眼 1）
        //    stereo_view_offset 僅作交叉驗證並計數不一致次數到 log，不再使用 Present 序號
        if (feat.assigned_eye < 0 || feat.assigned_eye > 1) {
            ReassignTrackedHandleEyesLocked();
        }
        const int eye = std::clamp(feat.assigned_eye, 0, 1);
        const std::string eye_source = "ngx_handle";

        // 記錄每眼最新 subrect 參數；於首次評估或子區域參數發生變動時寫入 log
        const EyeSubrectMetadata& prev_sr = m_last_subrect[eye];
        const bool sr_changed =
            !m_subrect_logged[eye] ||
            prev_sr.color_base_x != subrect.color_base_x || prev_sr.color_base_y != subrect.color_base_y ||
            prev_sr.depth_base_x != subrect.depth_base_x || prev_sr.depth_base_y != subrect.depth_base_y ||
            prev_sr.mv_base_x != subrect.mv_base_x || prev_sr.mv_base_y != subrect.mv_base_y ||
            prev_sr.output_base_x != subrect.output_base_x || prev_sr.output_base_y != subrect.output_base_y ||
            prev_sr.render_w != subrect.render_w || prev_sr.render_h != subrect.render_h;
        m_last_subrect[eye] = subrect;
        if (sr_changed) {
            m_subrect_logged[eye] = true;
            LogInfo(
                "[VIPLE-GAME-MV] Eye=%s (handle=%u) NGX Subrect params: "
                "Color.Base=(%u,%u)[set:%d,%d] Depth.Base=(%u,%u)[set:%d,%d] "
                "MV.Subrect.Base=(%u,%u)[set:%d,%d] Output.Base=(%u,%u)[set:%d,%d] "
                "Render.Subrect.Dim=(%ux%u)[set:%d,%d]",
                (eye == 0) ? "Left" : "Right",
                feat.handle_id,
                subrect.color_base_x, subrect.color_base_y, subrect.has_color_base_x ? 1 : 0, subrect.has_color_base_y ? 1 : 0,
                subrect.depth_base_x, subrect.depth_base_y, subrect.has_depth_base_x ? 1 : 0, subrect.has_depth_base_y ? 1 : 0,
                subrect.mv_base_x, subrect.mv_base_y, subrect.has_mv_base_x ? 1 : 0, subrect.has_mv_base_y ? 1 : 0,
                subrect.output_base_x, subrect.output_base_y, subrect.has_output_base_x ? 1 : 0, subrect.has_output_base_y ? 1 : 0,
                subrect.render_w, subrect.render_h, subrect.has_render_w ? 1 : 0, subrect.has_render_h ? 1 : 0);
        }

        int cross_check_offset_eye = -1;
        if (!m_stereo_view_queue.empty()) {
            cross_check_offset_eye = m_stereo_view_queue.front();
            m_stereo_view_queue.pop_front();
            if (cross_check_offset_eye == eye) {
                m_stats_offset_match++;
            } else {
                m_stats_offset_mismatch++;
            }
        } else {
            m_stats_offset_missing++;
        }

        if (eye == 0) {
            m_stats_left_calls++;
        } else {
            m_stats_right_calls++;
        }

        // 雙眼遊戲幀組必須嚴格由同幀的「左眼 (eye==0) 開啟、右眼 (eye==1) 結算」，防止 (T-1 右眼 + T 左眼) 跨幀配對：
        // - 若左眼 (eye==0) 到來時，上一組還有殘留未結算的單眼（m_group_eye_ready[0] 或 [1]），將上一組計為不齊備並清空；
        // - 若右眼 (eye==1) 到來時，m_group_eye_ready[0] 為 false（沒有同幀左眼，例如剛啟動傾印時殘留的上一幀右眼），
        //   則此孤立右眼計為不齊備並直接丟棄（不留存至下一幀與下一幀左眼湊對）。
        if (eye == 0) {
            if (m_group_eye_ready[0] || m_group_eye_ready[1]) {
                RecordIncompleteGameFrameLocked();
            }
        } else {
            if (!m_group_eye_ready[0] || m_group_eye_ready[1]) {
                RecordIncompleteGameFrameLocked();
                return;
            }
        }

        // 3. 解析貼圖尺寸、子區域偏移、MV 有效範圍與基準解析度（設計 2.1 節）
        const D3D12_RESOURCE_DESC mv_desc = mv_res->GetDesc();
        const D3D12_RESOURCE_DESC depth_desc =
            (depth_res != nullptr) ? depth_res->GetDesc() : D3D12_RESOURCE_DESC{};
        const D3D12_RESOURCE_DESC color_desc =
            (color_res != nullptr) ? color_res->GetDesc() : D3D12_RESOURCE_DESC{};

        const uint32_t mv_base_x = (subrect.mv_base_x < mv_desc.Width) ? subrect.mv_base_x : 0u;
        const uint32_t mv_base_y = (subrect.mv_base_y < mv_desc.Height) ? subrect.mv_base_y : 0u;
        const uint32_t depth_base_x = (depth_res != nullptr && subrect.depth_base_x < depth_desc.Width) ? subrect.depth_base_x : 0u;
        const uint32_t depth_base_y = (depth_res != nullptr && subrect.depth_base_y < depth_desc.Height) ? subrect.depth_base_y : 0u;
        const uint32_t color_base_x = (color_res != nullptr && subrect.color_base_x < color_desc.Width) ? subrect.color_base_x : 0u;
        const uint32_t color_base_y = (color_res != nullptr && subrect.color_base_y < color_desc.Height) ? subrect.color_base_y : 0u;

        const uint32_t mv_avail_w = static_cast<uint32_t>(mv_desc.Width) - mv_base_x;
        const uint32_t mv_avail_h = static_cast<uint32_t>(mv_desc.Height) - mv_base_y;
        const uint32_t depth_avail_w = (depth_res != nullptr) ? (static_cast<uint32_t>(depth_desc.Width) - depth_base_x) : 0u;
        const uint32_t depth_avail_h = (depth_res != nullptr) ? (static_cast<uint32_t>(depth_desc.Height) - depth_base_y) : 0u;
        const uint32_t color_avail_w = (color_res != nullptr) ? (static_cast<uint32_t>(color_desc.Width) - color_base_x) : 0u;
        const uint32_t color_avail_h = (color_res != nullptr) ? (static_cast<uint32_t>(color_desc.Height) - color_base_y) : 0u;

        uint32_t render_w = mv_avail_w;
        uint32_t render_h = mv_avail_h;
        if (subrect_w > 0 && subrect_h > 0 &&
            subrect_w <= mv_avail_w && subrect_h <= mv_avail_h) {
            render_w = subrect_w;
            render_h = subrect_h;
        } else if (feat.width > 0 && feat.height > 0 &&
                   feat.width <= mv_avail_w && feat.height <= mv_avail_h) {
            render_w = feat.width;
            render_h = feat.height;
        } else if (color_res != nullptr && color_avail_w > 0 && color_avail_h > 0 &&
                   color_avail_w <= mv_avail_w && color_avail_h <= mv_avail_h) {
            render_w = color_avail_w;
            render_h = color_avail_h;
        }

        if (render_w == 0 || render_h == 0) {
            if (eye == 1 && m_group_eye_ready[0]) {
                RecordIncompleteGameFrameLocked();
            }
            return;
        }

        // 若未設 MVLowRes (bit 1) 且 MV 貼圖本身是放大後尺寸（如 Palworld create_flags=0x9, MV=2152x2152, Input=1436x1436），
        // 則 MV 的基準分母與有效取樣範圍均為放大後尺寸 (base_w x base_h)，而 Depth/Color 為算圖尺寸 (render_w x render_h)
        uint32_t base_w = render_w;
        uint32_t base_h = render_h;
        uint32_t mv_valid_w = std::min<uint32_t>(render_w, mv_avail_w);
        uint32_t mv_valid_h = std::min<uint32_t>(render_h, mv_avail_h);
        if ((feat.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == 0 &&
            feat.out_width > render_w &&
            mv_avail_w >= feat.out_width && mv_avail_h >= feat.out_height) {
            base_w     = feat.out_width;
            base_h     = feat.out_height;
            mv_valid_w = feat.out_width;
            mv_valid_h = feat.out_height;
        }

        const uint32_t raw_mv4_eye_w = std::max(1u, (mv_valid_w + 3u) / 4u);
        const uint32_t raw_mv4_eye_h = std::max(1u, (mv_valid_h + 3u) / 4u);

        const float mv_scale_to_uv_x = mv_scale_x / static_cast<float>(base_w);
        const float mv_scale_to_uv_y = mv_scale_y / static_cast<float>(base_h);

        // 若建立旗標帶有 MVJittered (0x04)，扣除前後幀 jitter 差
        float jitter_delta_uv_x = 0.0f;
        float jitter_delta_uv_y = 0.0f;
        const bool mv_jittered = (feat.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        if (mv_jittered && feat.has_prev_jitter && reset == 0) {
            jitter_delta_uv_x = (jitter_x - feat.prev_jitter_x) / static_cast<float>(render_w);
            jitter_delta_uv_y = (jitter_y - feat.prev_jitter_y) / static_cast<float>(render_h);
        }
        feat.prev_jitter_x = jitter_x;
        feat.prev_jitter_y = jitter_y;
        feat.has_prev_jitter = true;

        m_last_mv_scale_x = mv_scale_x;
        m_last_mv_scale_y = mv_scale_y;
        m_last_render_w   = render_w;
        m_last_render_h   = render_h;

        const uint32_t eye_grid_w = std::max(1u, (render_w + 7u) / 8u);
        const uint32_t eye_grid_h = std::max(1u, (render_h + 7u) / 8u);

        const bool want_dump     = (m_dump_remaining_frames > 0);
        const bool want_raw_mv4  = (want_dump && m_dump_raw_mv4);

        // 若中途輸出網格尺寸變動導致貼圖重建，而另一眼正處於等待配對狀態，放棄半組
        if ((m_eye_grid_w != 0 && m_eye_grid_w != eye_grid_w) ||
            (m_eye_grid_h != 0 && m_eye_grid_h != eye_grid_h) ||
            (want_raw_mv4 && ((m_raw_mv4_eye_w != 0 && m_raw_mv4_eye_w != raw_mv4_eye_w) ||
                              (m_raw_mv4_eye_h != 0 && m_raw_mv4_eye_h != raw_mv4_eye_h)))) {
            if (m_group_eye_ready[0] || m_group_eye_ready[1]) {
                RecordIncompleteGameFrameLocked();
                if (eye == 1) {
                    return;
                }
            }
        }

        // 4. 確保 D3D12 降採樣管線與輸出貼圖已就緒
        if (!EnsureD3D12PipelineLocked(cmd_list, eye_grid_w, eye_grid_h, want_raw_mv4, raw_mv4_eye_w, raw_mv4_eye_h)) {
            if (eye == 1 && m_group_eye_ready[0]) {
                RecordIncompleteGameFrameLocked();
            }
            return;
        }

        // 先查詢本眼 UEVR 投影矩陣與相機姿態，供計算 Reverse-Z 近平面深度正規化基準值 depth_ref
        QueryUevrEyeProjectionLocked(eye, m_latest_eye_cam[eye]);
        float z_near = 0.001f;
        if (m_latest_eye_cam[eye].has_vr_proj) {
            const float proj_z = std::fabs(m_latest_eye_cam[eye].ue_proj[3][2]);
            if (proj_z > 1e-7f) {
                z_near = proj_z;
            }
        }
        const float w2m = (m_latest_eye_cam[eye].world_to_meters > 1e-3f)
                              ? m_latest_eye_cam[eye].world_to_meters
                              : 100.0f;
        // 1 公尺處對應的硬體逆深度基準值（Palworld+UEVR 下 z_near=0.001, w2m=100 -> depth_ref=1e-5）
        const float depth_ref = std::clamp(z_near / w2m, 1e-8f, 1.0f);

        // 5. 配置環狀 Descriptor Slot（避免同一幀左右眼或跨幀指令覆蓋彼此的 SRV/UAV）
        const uint32_t slot_idx = (m_desc_ring_cursor++) % kDescRingSlots;
        const uint32_t base_desc_idx = slot_idx * kDescsPerSlot;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu_base = m_srv_uav_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_base = m_srv_uav_heap->GetGPUDescriptorHandleForHeapStart();
        cpu_base.ptr += static_cast<SIZE_T>(base_desc_idx) * m_srv_uav_desc_size;
        gpu_base.ptr += static_cast<UINT64>(base_desc_idx) * m_srv_uav_desc_size;

        // 建立 t0 (MV), t1 (Depth), t2 (Color) SRV
        CreateTexture2DSrvLocked(mv_res, mv_desc.Format, cpu_base, 0);
        if (depth_res != nullptr) {
            CreateTexture2DSrvLocked(depth_res, depth_desc.Format, cpu_base, 1);
        } else {
            CreateTexture2DSrvLocked(m_dummy_tex.Get(), DXGI_FORMAT_R32_FLOAT, cpu_base, 1);
        }
        if (color_res != nullptr && want_dump) {
            CreateTexture2DSrvLocked(color_res, color_desc.Format, cpu_base, 2);
        } else {
            CreateTexture2DSrvLocked(m_dummy_tex.Get(), DXGI_FORMAT_R32_FLOAT, cpu_base, 2);
        }

        // 建立 u0 (OutMv8), u1 (OutColor8), u2 (OutRawMv4) UAV
        CreateTexture2DUavLocked(m_out_mv8_tex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, cpu_base, 3);
        CreateTexture2DUavLocked(m_out_color8_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, cpu_base, 4);
        if (want_raw_mv4 && m_out_raw_mv4_tex) {
            CreateTexture2DUavLocked(m_out_raw_mv4_tex.Get(), DXGI_FORMAT_R32G32_FLOAT, cpu_base, 5);
        } else {
            CreateTexture2DUavLocked(m_out_mv8_tex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, cpu_base, 5);
        }

        // 6. 資源狀態與快取同步保護：
        //    - 若左右眼共用同一張 mv_res 且上游可能經由 Compute Shader (UAV) 覆寫，先發出 UAV Barrier 確保快取同步；
        //    - 若設定了非 NON_PIXEL_SHADER_RESOURCE 的傳入狀態，派發前轉為 SRV、派發後嚴格還原。
        if ((mv_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
            D3D12_RESOURCE_BARRIER mv_uav_b{};
            mv_uav_b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            mv_uav_b.UAV.pResource = mv_res;
            cmd_list->ResourceBarrier(1, &mv_uav_b);
        }

        ScopedInputStateGuard state_guard(cmd_list);
        state_guard.AddIfNeeded(mv_res, m_assumed_mv_state);
        if (depth_res != nullptr) {
            state_guard.AddIfNeeded(depth_res, m_assumed_depth_state);
        }
        if (color_res != nullptr && want_dump) {
            state_guard.AddIfNeeded(color_res, m_assumed_color_state);
        }
        state_guard.ApplyPreBarriers();

        // 7. 設定 Compute Pipeline 並派發 cs_downsample_mv_8x8（以及啟用 raw 時派發 cs_downsample_raw_mv_4x4）
        DownsampleRootConstants rc{};
        rc.mv_valid_dim[0]     = mv_valid_w;
        rc.mv_valid_dim[1]     = mv_valid_h;
        rc.depth_valid_dim[0]  = (depth_res != nullptr) ? std::min<uint32_t>(render_w, depth_avail_w) : render_w;
        rc.depth_valid_dim[1]  = (depth_res != nullptr) ? std::min<uint32_t>(render_h, depth_avail_h) : render_h;
        rc.color_valid_dim[0]  = (color_res != nullptr) ? std::min<uint32_t>(render_w, color_avail_w) : render_w;
        rc.color_valid_dim[1]  = (color_res != nullptr) ? std::min<uint32_t>(render_h, color_avail_h) : render_h;
        rc.eye_grid_dim[0]     = eye_grid_w;
        rc.eye_grid_dim[1]     = eye_grid_h;
        rc.eye_index           = static_cast<uint32_t>(eye);

        // UE5 預設使用 Reverse-Z；若 create_flags 明確帶 DepthInverted 或預設皆啟用 Reverse-Z
        const bool depth_inverted =
            ((feat.create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0) || m_default_reverse_z;
        rc.flags = 0u;
        if (depth_res != nullptr) {
            rc.flags |= 1u; // b0: hasDepth
        }
        if (depth_inverted) {
            rc.flags |= 2u; // b1: depthInverted (Reverse-Z)
        }
        if (mv_jittered) {
            rc.flags |= 4u; // b2: mvJittered
        }
        if (want_dump && color_res != nullptr) {
            rc.flags |= 8u; // b3: dumpColor
        }

        rc.mv_scale_to_uv[0]   = mv_scale_to_uv_x;
        rc.mv_scale_to_uv[1]   = mv_scale_to_uv_y;
        rc.jitter_delta_uv[0]  = jitter_delta_uv_x;
        rc.jitter_delta_uv[1]  = jitter_delta_uv_y;
        rc.depth_epsilon       = 0.05f;
        rc.depth_ref           = depth_ref;
        rc.mv_base[0]          = mv_base_x;
        rc.mv_base[1]          = mv_base_y;
        rc.depth_base[0]       = depth_base_x;
        rc.depth_base[1]       = depth_base_y;
        rc.color_base[0]       = color_base_x;
        rc.color_base[1]       = color_base_y;
        rc.raw_mv4_grid_dim[0] = raw_mv4_eye_w;
        rc.raw_mv4_grid_dim[1] = raw_mv4_eye_h;

        ID3D12DescriptorHeap* heaps[] = {m_srv_uav_heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(m_root_sig.Get());
        cmd_list->SetPipelineState(m_pso.Get());
        cmd_list->SetComputeRoot32BitConstants(0, 24, &rc, 0);

        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu_base;
        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu_base;
        uav_gpu.ptr += 3ull * m_srv_uav_desc_size;

        cmd_list->SetComputeRootDescriptorTable(1, srv_gpu);
        cmd_list->SetComputeRootDescriptorTable(2, uav_gpu);
        cmd_list->Dispatch(eye_grid_w, eye_grid_h, 1);

        if (want_raw_mv4 && m_pso_raw_mv4 && m_out_raw_mv4_tex) {
            cmd_list->SetPipelineState(m_pso_raw_mv4.Get());
            cmd_list->Dispatch((raw_mv4_eye_w + 7u) / 8u, (raw_mv4_eye_h + 7u) / 8u, 1);
        }

        // 插入 UAV barrier，確保寫入完成且左右眼寫入同一張貼圖的左右半邊不發生競爭
        D3D12_RESOURCE_BARRIER uav_barriers[3]{};
        uav_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barriers[0].UAV.pResource = m_out_mv8_tex.Get();
        uav_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barriers[1].UAV.pResource = m_out_color8_tex.Get();
        uav_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barriers[2].UAV.pResource = m_out_raw_mv4_tex.Get();
        const UINT num_uav_barriers = (want_raw_mv4 && m_out_raw_mv4_tex) ? 3u : (want_dump ? 2u : 1u);
        cmd_list->ResourceBarrier(num_uav_barriers, uav_barriers);

        // 還原輸入資源（MV / Depth / Color）至遊戲傳入時的狀態
        state_guard.RestorePostBarriers();

        // 8. 記錄本眼元資料至當前遊戲幀組
        EyeEvalMetadata& meta       = m_group_eye_meta[eye];
        meta.valid                  = true;
        meta.eye_index              = eye;
        meta.eye_source             = eye_source;
        meta.cross_check_offset_eye = cross_check_offset_eye;
        meta.feature_id             = static_cast<int>(feat.feature_id);
        meta.handle_id              = feat.handle_id;
        meta.create_flags           = feat.create_flags;
        meta.render_w               = render_w;
        meta.render_h               = render_h;
        meta.mv_valid_w             = mv_valid_w;
        meta.mv_valid_h             = mv_valid_h;
        meta.base_w                 = base_w;
        meta.base_h                 = base_h;
        meta.mv_tex_w               = static_cast<uint32_t>(mv_desc.Width);
        meta.mv_tex_h               = static_cast<uint32_t>(mv_desc.Height);
        meta.mv_format              = static_cast<uint32_t>(mv_desc.Format);
        meta.depth_tex_w            = static_cast<uint32_t>(depth_desc.Width);
        meta.depth_tex_h            = static_cast<uint32_t>(depth_desc.Height);
        meta.depth_format           = static_cast<uint32_t>(depth_desc.Format);
        meta.color_tex_w            = static_cast<uint32_t>(color_desc.Width);
        meta.color_tex_h            = static_cast<uint32_t>(color_desc.Height);
        meta.color_format           = static_cast<uint32_t>(color_desc.Format);
        meta.mv_res_ptr             = reinterpret_cast<uintptr_t>(mv_res);
        meta.depth_res_ptr          = reinterpret_cast<uintptr_t>(depth_res);
        meta.color_res_ptr          = reinterpret_cast<uintptr_t>(color_res);
        meta.mv_scale_x             = mv_scale_x;
        meta.mv_scale_y             = mv_scale_y;
        meta.mv_scale_to_uv_x       = mv_scale_to_uv_x;
        meta.mv_scale_to_uv_y       = mv_scale_to_uv_y;
        meta.jitter_x               = jitter_x;
        meta.jitter_y               = jitter_y;
        meta.jitter_delta_uv_x      = jitter_delta_uv_x;
        meta.jitter_delta_uv_y      = jitter_delta_uv_y;
        meta.depth_ref              = depth_ref;
        meta.reset                  = reset;
        meta.subrect                = subrect;
        meta.cam                    = m_latest_eye_cam[eye];
        m_group_eye_ready[eye]      = true;

        // 9. 幀分組結算：當左右眼各完成一次評估（雙眼齊備的一組遊戲幀）時才算完成一幀；
        //    此時處於右眼 EvaluateFeature 的 dispatch 與 UAV barrier 之後（同一張 command list）：
        //    - M-MV1：若已連上 server 且有 section，將 m_out_mv8_tex 轉成 COPY_SOURCE，
        //             CopyTextureRegion 到 READBACK heap 的環狀緩衝（VRMV_SLOTS 個，GetCopyableFootprints），再轉回原狀態。
        //    - M-MV0：若處於磁碟傾印模式，同樣複製至傾印專用的 Readback Slot（兩者可同時開啟）。
        if (m_group_eye_ready[0] && m_group_eye_ready[1]) {
            m_game_frame_index++;
            m_stats_window_game_frames++;
            m_stats_complete_game_frames++;

            if (m_ipc_connected.load(std::memory_order_acquire)) {
                RecordCompleteStereoIpcToReadbackLocked(cmd_list);
            }

            if (want_dump) {
                RecordCompleteStereoDumpToReadbackLocked(cmd_list);
            }

            m_group_eye_ready[0] = false;
            m_group_eye_ready[1] = false;
            m_group_eye_meta[0]  = EyeEvalMetadata{};
            m_group_eye_meta[1]  = EyeEvalMetadata{};
        }
    }

    void RecordIncompleteGameFrameLocked() {
        m_game_frame_index++;
        m_stats_window_game_frames++;
        m_stats_incomplete_game_frames++;
        m_ipc_frames_dropped_incomplete.fetch_add(1, std::memory_order_relaxed);
        m_ipc_window_dropped_incomplete.fetch_add(1, std::memory_order_relaxed);
        if (m_dump_remaining_frames > 0) {
            m_dump_skipped_incomplete++;
        }
        m_group_eye_ready[0] = false;
        m_group_eye_ready[1] = false;
        m_group_eye_meta[0]  = EyeEvalMetadata{};
        m_group_eye_meta[1]  = EyeEvalMetadata{};
    }

    // ========================================================================
    // UEVR 相機姿態與投影矩陣查詢
    // ========================================================================
    void QueryUevrEyeProjectionLocked(int eye, EyeCameraPose& out_cam) {
        const auto* param = uevr::API::get()->param();
        if (param == nullptr || param->vr == nullptr) {
            return;
        }
        const auto* vr = param->vr;
        if (vr->is_runtime_ready != nullptr && !vr->is_runtime_ready()) {
            return;
        }

        const UEVR_Eye uevr_eye = (eye == 0) ? UEVR_LEFT_EYE : UEVR_RIGHT_EYE;
        if (vr->get_eye_offset != nullptr) {
            UEVR_Vector3f eye_off{};
            vr->get_eye_offset(uevr_eye, &eye_off);
            out_cam.eye_offset[0] = eye_off.x;
            out_cam.eye_offset[1] = eye_off.y;
            out_cam.eye_offset[2] = eye_off.z;
        }
        if (vr->get_ue_projection_matrix != nullptr) {
            UEVR_Matrix4x4f proj{};
            vr->get_ue_projection_matrix(uevr_eye, &proj);
            std::memcpy(out_cam.ue_proj, proj.m, sizeof(out_cam.ue_proj));
            out_cam.has_vr_proj = true;
        }
    }

    void QueryUevrHmdPoseLocked(FrameDumpMetadata& meta) {
        meta.hmd_pose_valid = false;
        const auto* param = uevr::API::get()->param();
        if (param == nullptr || param->vr == nullptr) {
            return;
        }
        const auto* vr = param->vr;
        if (vr->is_runtime_ready != nullptr && !vr->is_runtime_ready()) {
            return;
        }

        UEVR_TrackedDeviceIndex hmd_idx = 0;
        if (vr->get_hmd_index != nullptr) {
            hmd_idx = vr->get_hmd_index();
        }
        if (vr->get_pose != nullptr) {
            UEVR_Vector3f pos{};
            UEVR_Quaternionf rot{};
            vr->get_pose(hmd_idx, &pos, &rot);
            meta.hmd_pos[0]  = pos.x;
            meta.hmd_pos[1]  = pos.y;
            meta.hmd_pos[2]  = pos.z;
            meta.hmd_quat[0] = rot.w;
            meta.hmd_quat[1] = rot.x;
            meta.hmd_quat[2] = rot.y;
            meta.hmd_quat[3] = rot.z;
            meta.hmd_pose_valid = true;
        }
        if (vr->get_transform != nullptr) {
            UEVR_Matrix4x4f tf{};
            vr->get_transform(hmd_idx, &tf);
            std::memcpy(meta.hmd_transform, tf.m, sizeof(meta.hmd_transform));
        }
    }

    // ========================================================================
    // D3D12 管線與資源管理
    // ========================================================================
    bool EnsureD3D12PipelineLocked(
        ID3D12GraphicsCommandList* cmd_list,
        uint32_t eye_grid_w,
        uint32_t eye_grid_h,
        bool want_raw_mv4,
        uint32_t raw_mv4_eye_w,
        uint32_t raw_mv4_eye_h) {
        ComPtr<ID3D12Device> dev;
        if (FAILED(cmd_list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
            return false;
        }

        if (m_device.Get() != dev.Get()) {
            ReleaseD3D12ResourcesLocked();
            m_device = dev;
        }

        // 1. 建立 Root Signature 與 Compute PSO (含 cs_downsample_mv_8x8 與 cs_downsample_raw_mv_4x4)
        if (!m_root_sig || !m_pso || !m_pso_raw_mv4) {
            if (!CreateComputePipelineLocked()) {
                return false;
            }
        }

        // 2. 建立環狀 Shader-Visible Descriptor Heap 與 Dummy 1x1 貼圖
        if (!m_srv_uav_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
            heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap_desc.NumDescriptors = kDescRingSlots * kDescsPerSlot;
            heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            heap_desc.NodeMask       = 0;
            if (FAILED(m_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_srv_uav_heap)))) {
                LogError("[VIPLE-GAME-MV] Failed to create SRV/UAV descriptor heap");
                return false;
            }
            m_srv_uav_desc_size =
                m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }

        if (!m_dummy_tex) {
            D3D12_HEAP_PROPERTIES default_heap{};
            default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC dummy_desc{};
            dummy_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            dummy_desc.Width            = 1;
            dummy_desc.Height           = 1;
            dummy_desc.DepthOrArraySize = 1;
            dummy_desc.MipLevels        = 1;
            dummy_desc.Format           = DXGI_FORMAT_R32_FLOAT;
            dummy_desc.SampleDesc.Count = 1;
            dummy_desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            dummy_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(m_device->CreateCommittedResource(
                    &default_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &dummy_desc,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr,
                    IID_PPV_ARGS(&m_dummy_tex)))) {
                LogError("[VIPLE-GAME-MV] Failed to create dummy 1x1 SRV texture");
                return false;
            }
        }

        // 3. 建立或調整 1/8 雙眼並排輸出貼圖 (OutMv8: RGBA16F, OutColor8: RGBA8_UNORM)
        if (!m_out_mv8_tex || !m_out_color8_tex ||
            m_eye_grid_w != eye_grid_w || m_eye_grid_h != eye_grid_h) {
            m_out_mv8_tex.Reset();
            m_out_color8_tex.Reset();

            const uint32_t sbs_w = eye_grid_w * 2u;
            const uint32_t sbs_h = eye_grid_h;

            D3D12_HEAP_PROPERTIES default_heap{};
            default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC tex_desc{};
            tex_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tex_desc.Width            = sbs_w;
            tex_desc.Height           = sbs_h;
            tex_desc.DepthOrArraySize = 1;
            tex_desc.MipLevels        = 1;
            tex_desc.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
            tex_desc.SampleDesc.Count = 1;
            tex_desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            tex_desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            if (FAILED(m_device->CreateCommittedResource(
                    &default_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &tex_desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr,
                    IID_PPV_ARGS(&m_out_mv8_tex)))) {
                LogError("[VIPLE-GAME-MV] Failed to create OutMv8 texture (%ux%u)", sbs_w, sbs_h);
                return false;
            }

            tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (FAILED(m_device->CreateCommittedResource(
                    &default_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &tex_desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr,
                    IID_PPV_ARGS(&m_out_color8_tex)))) {
                LogError("[VIPLE-GAME-MV] Failed to create OutColor8 texture (%ux%u)", sbs_w, sbs_h);
                return false;
            }

            m_eye_grid_w = eye_grid_w;
            m_eye_grid_h = eye_grid_h;

            LogInfo("[VIPLE-GAME-MV] Allocated 1/8 SBS output textures: eye_grid=(%ux%u), sbs_grid=(%ux%u)",
                    eye_grid_w, eye_grid_h, sbs_w, sbs_h);
        }

        // 3b. 若啟用 raw 除錯傾印，建立或調整 1/4 原始 MV 雙眼並排輸出貼圖 (OutRawMv4: RG32F)
        if (want_raw_mv4 && raw_mv4_eye_w > 0 && raw_mv4_eye_h > 0) {
            if (!m_out_raw_mv4_tex ||
                m_raw_mv4_eye_w != raw_mv4_eye_w ||
                m_raw_mv4_eye_h != raw_mv4_eye_h) {
                m_out_raw_mv4_tex.Reset();

                const uint32_t raw_sbs_w = raw_mv4_eye_w * 2u;
                const uint32_t raw_sbs_h = raw_mv4_eye_h;

                D3D12_HEAP_PROPERTIES default_heap{};
                default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

                D3D12_RESOURCE_DESC raw_desc{};
                raw_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                raw_desc.Width            = raw_sbs_w;
                raw_desc.Height           = raw_sbs_h;
                raw_desc.DepthOrArraySize = 1;
                raw_desc.MipLevels        = 1;
                raw_desc.Format           = DXGI_FORMAT_R32G32_FLOAT;
                raw_desc.SampleDesc.Count = 1;
                raw_desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                raw_desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

                if (FAILED(m_device->CreateCommittedResource(
                        &default_heap,
                        D3D12_HEAP_FLAG_NONE,
                        &raw_desc,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        nullptr,
                        IID_PPV_ARGS(&m_out_raw_mv4_tex)))) {
                    LogError("[VIPLE-GAME-MV] Failed to create OutRawMv4 texture (%ux%u)", raw_sbs_w, raw_sbs_h);
                    return false;
                }

                m_raw_mv4_eye_w = raw_mv4_eye_w;
                m_raw_mv4_eye_h = raw_mv4_eye_h;

                LogInfo("[VIPLE-GAME-MV] Allocated 1/4 raw MV SBS output texture (RG32F): eye_grid=(%ux%u), sbs_grid=(%ux%u)",
                        raw_mv4_eye_w, raw_mv4_eye_h, raw_sbs_w, raw_sbs_h);
            }
        }

        // 4. 建立傾印與 IPC 發布用的 Fence
        if (!m_readback_fence) {
            if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_readback_fence)))) {
                LogError("[VIPLE-GAME-MV] Failed to create D3D12 readback fence");
                return false;
            }
            m_fence_counter = 0;
        }
        if (!m_ipc_fence) {
            if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_ipc_fence)))) {
                LogError("[VIPLE-GAME-MV-IPC] Failed to create D3D12 IPC readback fence");
                return false;
            }
            m_ipc_fence_counter = 0;
        }

        return true;
    }

    bool CompileCsEntryLocked(const char* entry_point, ComPtr<ID3DBlob>& out_blob) {
        ComPtr<ID3DBlob> err_blob;
        const fs::path hlsl_path = m_plugin_dir / L"cs_downsample_mv_8x8.hlsl";
        UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

        HRESULT hr = E_FAIL;
        if (fs::exists(hlsl_path)) {
            hr = D3DCompileFromFile(
                hlsl_path.c_str(),
                nullptr,
                nullptr,
                entry_point,
                "cs_5_0",
                compile_flags,
                0,
                &out_blob,
                &err_blob);
            if (SUCCEEDED(hr) && out_blob) {
                LogInfo("[VIPLE-GAME-MV] Compiled %s from file: %s",
                        entry_point,
                        hlsl_path.u8string().c_str());
                return true;
            }
            LogWarn("[VIPLE-GAME-MV] D3DCompileFromFile(%s) failed (%s), falling back to embedded HLSL",
                    entry_point,
                    err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown");
            err_blob.Reset();
        }

        hr = D3DCompile(
            kEmbeddedDownsampleHlsl,
            std::strlen(kEmbeddedDownsampleHlsl),
            "cs_downsample_mv_8x8_embedded.hlsl",
            nullptr,
            nullptr,
            entry_point,
            "cs_5_0",
            compile_flags,
            0,
            &out_blob,
            &err_blob);
        if (FAILED(hr) || !out_blob) {
            LogError("[VIPLE-GAME-MV] Failed to compile embedded %s: %s",
                     entry_point,
                     err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown");
            return false;
        }
        LogInfo("[VIPLE-GAME-MV] Compiled %s from embedded HLSL source", entry_point);
        return true;
    }

    bool CreateComputePipelineLocked() {
        ComPtr<ID3DBlob> cs_blob;
        ComPtr<ID3DBlob> cs_raw4_blob;
        if (!CompileCsEntryLocked("cs_downsample_mv_8x8", cs_blob) ||
            !CompileCsEntryLocked("cs_downsample_raw_mv_4x4", cs_raw4_blob)) {
            return false;
        }

        // 建立 Root Signature：
        //   [0] Root Constants (b0, 24 x 32-bit values)
        //   [1] Descriptor Table (SRV t0..t2)
        //   [2] Descriptor Table (UAV u0..u2)
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors                    = 3;
        srv_range.BaseShaderRegister                = 0;
        srv_range.RegisterSpace                     = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors                    = 3;
        uav_range.BaseShaderRegister                = 0;
        uav_range.RegisterSpace                     = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[3]{};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 24;
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        root_params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        root_params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[2].DescriptorTable.NumDescriptorRanges = 1;
        root_params[2].DescriptorTable.pDescriptorRanges   = &uav_range;
        root_params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters     = 3;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 0;
        rs_desc.pStaticSamplers   = nullptr;
        rs_desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig_blob;
        ComPtr<ID3DBlob> rs_err_blob;
        HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &rs_err_blob);
        if (FAILED(hr) || !sig_blob) {
            LogError("[VIPLE-GAME-MV] D3D12SerializeRootSignature failed: %s",
                     rs_err_blob ? static_cast<const char*>(rs_err_blob->GetBufferPointer()) : "unknown");
            return false;
        }

        hr = m_device->CreateRootSignature(
            0,
            sig_blob->GetBufferPointer(),
            sig_blob->GetBufferSize(),
            IID_PPV_ARGS(&m_root_sig));
        if (FAILED(hr) || !m_root_sig) {
            LogError("[VIPLE-GAME-MV] CreateRootSignature failed (hr=0x%08x)", static_cast<unsigned>(hr));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature     = m_root_sig.Get();
        pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
        pso_desc.CS.BytecodeLength  = cs_blob->GetBufferSize();

        hr = m_device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso));
        if (FAILED(hr) || !m_pso) {
            LogError("[VIPLE-GAME-MV] CreateComputePipelineState(cs_downsample_mv_8x8) failed (hr=0x%08x)",
                     static_cast<unsigned>(hr));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_raw4_desc{};
        pso_raw4_desc.pRootSignature     = m_root_sig.Get();
        pso_raw4_desc.CS.pShaderBytecode = cs_raw4_blob->GetBufferPointer();
        pso_raw4_desc.CS.BytecodeLength  = cs_raw4_blob->GetBufferSize();

        hr = m_device->CreateComputePipelineState(&pso_raw4_desc, IID_PPV_ARGS(&m_pso_raw_mv4));
        if (FAILED(hr) || !m_pso_raw_mv4) {
            LogError("[VIPLE-GAME-MV] CreateComputePipelineState(cs_downsample_raw_mv_4x4) failed (hr=0x%08x)",
                     static_cast<unsigned>(hr));
            return false;
        }

        return true;
    }

    void CreateTexture2DSrvLocked(
        ID3D12Resource* res,
        DXGI_FORMAT raw_fmt,
        D3D12_CPU_DESCRIPTOR_HANDLE base_cpu,
        uint32_t offset_slots) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format                  = GetSrvFormatForResource(raw_fmt);
        srv_desc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels       = 1;
        srv_desc.Texture2D.PlaneSlice      = 0;
        srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE dst = base_cpu;
        dst.ptr += static_cast<SIZE_T>(offset_slots) * m_srv_uav_desc_size;
        m_device->CreateShaderResourceView(res, &srv_desc, dst);
    }

    void CreateTexture2DUavLocked(
        ID3D12Resource* res,
        DXGI_FORMAT fmt,
        D3D12_CPU_DESCRIPTOR_HANDLE base_cpu,
        uint32_t offset_slots) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format             = fmt;
        uav_desc.ViewDimension      = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav_desc.Texture2D.MipSlice = 0;
        uav_desc.Texture2D.PlaneSlice = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE dst = base_cpu;
        dst.ptr += static_cast<SIZE_T>(offset_slots) * m_srv_uav_desc_size;
        m_device->CreateUnorderedAccessView(res, nullptr, &uav_desc, dst);
    }

    void ReleaseD3D12ResourcesLocked() {
        m_pso.Reset();
        m_pso_raw_mv4.Reset();
        m_root_sig.Reset();
        m_srv_uav_heap.Reset();
        m_dummy_tex.Reset();
        m_out_mv8_tex.Reset();
        m_out_color8_tex.Reset();
        m_out_raw_mv4_tex.Reset();
        m_readback_fence.Reset();
        m_ipc_fence.Reset();
        for (auto& slot : m_readback_slots) {
            slot.mv8_buf.Reset();
            slot.color8_buf.Reset();
            slot.raw_mv4_buf.Reset();
            slot.allocated_mv8_bytes = 0;
            slot.allocated_color8_bytes = 0;
            slot.allocated_raw_mv4_bytes = 0;
            slot.in_use.store(false, std::memory_order_release);
        }
        for (auto& slot : m_ipc_rb_slots) {
            slot.mv8_buf.Reset();
            slot.allocated_bytes = 0;
            slot.pending_fence_value = 0;
            slot.in_use.store(false, std::memory_order_release);
        }
        m_staged_dump_tasks.clear();
        m_staged_ipc_tasks.clear();
        {
            std::lock_guard<std::mutex> qlock(m_ipc_queue_mutex);
            m_ipc_publish_queue.clear();
        }
        m_group_eye_ready[0] = false;
        m_group_eye_ready[1] = false;
        m_group_eye_meta[0]  = EyeEvalMetadata{};
        m_group_eye_meta[1]  = EyeEvalMetadata{};
        m_device.Reset();
        m_eye_grid_w = 0;
        m_eye_grid_h = 0;
        m_raw_mv4_eye_w = 0;
        m_raw_mv4_eye_h = 0;
    }

    // ========================================================================
    // Readback 傾印（D3D12 Readback Heap + Fence + 背景執行緒）
    // 每一筆傾印保證為「左右眼各完成一次評估」的雙眼齊備遊戲幀組
    // ========================================================================
    void CheckDumpTriggerFileLocked() {
        if (m_dump_remaining_frames > 0 || m_dump_batch_in_flight.load(std::memory_order_acquire)) {
            return;
        }
        // 每 15 次 Present 檢查一次外掛目錄下的 viple_mv_dump.txt
        if ((m_present_count % 15ull) != 0ull && m_present_count > 1ull) {
            return;
        }

        const fs::path trigger_path = m_plugin_dir / L"viple_mv_dump.txt";
        std::error_code ec;
        if (!fs::exists(trigger_path, ec)) {
            return;
        }

        uint32_t requested_frames = 10u;
        bool want_raw = false;
        {
            std::ifstream ifs(trigger_path);
            std::string tok;
            bool parsed_count = false;
            while (ifs >> tok) {
                std::string lower_tok = tok;
                std::transform(lower_tok.begin(), lower_tok.end(), lower_tok.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower_tok == "raw") {
                    want_raw = true;
                } else if (!parsed_count) {
                    char* end_ptr = nullptr;
                    unsigned long val = std::strtoul(tok.c_str(), &end_ptr, 10);
                    if (end_ptr != tok.c_str() && val > 0) {
                        requested_frames = std::clamp(static_cast<uint32_t>(val), 1u, 600u);
                        parsed_count = true;
                    }
                }
            }
        }

        const fs::path dump_dir = m_plugin_dir / L"dump";
        fs::create_directories(dump_dir, ec);

        m_dump_remaining_frames   = requested_frames;
        m_dump_raw_mv4            = want_raw;
        m_dump_seq_counter        = 0;
        m_dump_skipped_incomplete = 0;
        m_staged_dump_tasks.clear();

        // 重置尚未結算的半組遊戲幀狀態，確保第 0 筆傾印一定從全新的完整雙眼遊戲幀（左眼 -> 右眼）起算，
        // 避免把觸發前未開 dumpColor 的殘留單眼併入第 0 筆傾印。
        m_group_eye_ready[0] = false;
        m_group_eye_ready[1] = false;
        m_group_eye_meta[0]  = EyeEvalMetadata{};
        m_group_eye_meta[1]  = EyeEvalMetadata{};

        m_dump_batch_in_flight.store(true, std::memory_order_release);

        LogInfo("[VIPLE-GAME-MV] Detected viple_mv_dump.txt: scheduling %u complete stereo game frames (raw_mv4=%s) to %s",
                requested_frames,
                want_raw ? "true" : "false",
                dump_dir.u8string().c_str());
    }

    bool EnsureReadbackSlotBuffersLocked(
        uint32_t slot_idx,
        uint32_t sbs_w,
        uint32_t sbs_h,
        uint32_t& out_mv8_pitch,
        uint32_t& out_color8_pitch,
        bool want_raw_mv4,
        uint32_t raw_sbs_w,
        uint32_t raw_sbs_h,
        uint32_t& out_raw_mv4_pitch) {
        out_mv8_pitch     = AlignUp(sbs_w * 8u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); // RGBA16F = 8 B/px
        out_color8_pitch  = AlignUp(sbs_w * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); // RGBA8   = 4 B/px
        out_raw_mv4_pitch = want_raw_mv4
                                ? AlignUp(raw_sbs_w * 8u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) // RG32F = 8 B/px
                                : 0u;

        const uint64_t mv8_total_bytes     = static_cast<uint64_t>(out_mv8_pitch) * sbs_h;
        const uint64_t color8_total_bytes  = static_cast<uint64_t>(out_color8_pitch) * sbs_h;
        const uint64_t raw_mv4_total_bytes = static_cast<uint64_t>(out_raw_mv4_pitch) * raw_sbs_h;

        ReadbackSlotResources& slot = m_readback_slots[slot_idx];

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

        if (!slot.mv8_buf || slot.allocated_mv8_bytes < mv8_total_bytes) {
            slot.mv8_buf.Reset();
            D3D12_RESOURCE_DESC buf_desc{};
            buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            buf_desc.Width            = mv8_total_bytes;
            buf_desc.Height           = 1;
            buf_desc.DepthOrArraySize = 1;
            buf_desc.MipLevels        = 1;
            buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
            buf_desc.SampleDesc.Count = 1;
            buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            buf_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(m_device->CreateCommittedResource(
                    &rb_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buf_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&slot.mv8_buf)))) {
                LogError("[VIPLE-GAME-MV] Failed to create mv8 readback buffer");
                return false;
            }
            slot.allocated_mv8_bytes = mv8_total_bytes;
        }

        if (!slot.color8_buf || slot.allocated_color8_bytes < color8_total_bytes) {
            slot.color8_buf.Reset();
            D3D12_RESOURCE_DESC buf_desc{};
            buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            buf_desc.Width            = color8_total_bytes;
            buf_desc.Height           = 1;
            buf_desc.DepthOrArraySize = 1;
            buf_desc.MipLevels        = 1;
            buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
            buf_desc.SampleDesc.Count = 1;
            buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            buf_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

            if (FAILED(m_device->CreateCommittedResource(
                    &rb_heap,
                    D3D12_HEAP_FLAG_NONE,
                    &buf_desc,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,
                    IID_PPV_ARGS(&slot.color8_buf)))) {
                LogError("[VIPLE-GAME-MV] Failed to create color8 readback buffer");
                return false;
            }
            slot.allocated_color8_bytes = color8_total_bytes;
        }

        if (want_raw_mv4 && raw_mv4_total_bytes > 0) {
            if (!slot.raw_mv4_buf || slot.allocated_raw_mv4_bytes < raw_mv4_total_bytes) {
                slot.raw_mv4_buf.Reset();
                D3D12_RESOURCE_DESC buf_desc{};
                buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                buf_desc.Width            = raw_mv4_total_bytes;
                buf_desc.Height           = 1;
                buf_desc.DepthOrArraySize = 1;
                buf_desc.MipLevels        = 1;
                buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
                buf_desc.SampleDesc.Count = 1;
                buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                buf_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

                if (FAILED(m_device->CreateCommittedResource(
                        &rb_heap,
                        D3D12_HEAP_FLAG_NONE,
                        &buf_desc,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,
                        IID_PPV_ARGS(&slot.raw_mv4_buf)))) {
                    LogError("[VIPLE-GAME-MV] Failed to create raw_mv4 readback buffer");
                    return false;
                }
                slot.allocated_raw_mv4_bytes = raw_mv4_total_bytes;
            }
        }

        return true;
    }

    void RecordCompleteStereoDumpToReadbackLocked(ID3D12GraphicsCommandList* cmd_list) {
        if (!m_out_mv8_tex || !m_out_color8_tex || m_eye_grid_w == 0 || m_eye_grid_h == 0) {
            return;
        }
        if (!m_group_eye_ready[0] || !m_group_eye_ready[1] || m_dump_remaining_frames == 0) {
            return;
        }

        const bool copy_raw_mv4 = (m_dump_raw_mv4 && m_out_raw_mv4_tex && m_raw_mv4_eye_w > 0 && m_raw_mv4_eye_h > 0);

        // 挑一個未在使用中的獨立 Readback Slot
        uint32_t chosen = UINT32_MAX;
        for (uint32_t i = 0; i < kReadbackSlots; ++i) {
            uint32_t candidate = (m_readback_cursor + i) % kReadbackSlots;
            if (!m_readback_slots[candidate].in_use.load(std::memory_order_acquire)) {
                chosen = candidate;
                m_readback_cursor = (candidate + 1u) % kReadbackSlots;
                break;
            }
        }
        if (chosen == UINT32_MAX) {
            LogWarn("[VIPLE-GAME-MV] All readback slots busy, skipping dump copy for this game frame");
            return;
        }

        const uint32_t sbs_w     = m_eye_grid_w * 2u;
        const uint32_t sbs_h     = m_eye_grid_h;
        const uint32_t raw_sbs_w = copy_raw_mv4 ? (m_raw_mv4_eye_w * 2u) : 0u;
        const uint32_t raw_sbs_h = copy_raw_mv4 ? m_raw_mv4_eye_h : 0u;
        uint32_t mv8_pitch     = 0;
        uint32_t color8_pitch  = 0;
        uint32_t raw_mv4_pitch = 0;
        if (!EnsureReadbackSlotBuffersLocked(
                chosen, sbs_w, sbs_h, mv8_pitch, color8_pitch,
                copy_raw_mv4, raw_sbs_w, raw_sbs_h, raw_mv4_pitch)) {
            return;
        }

        ReadbackSlotResources& slot = m_readback_slots[chosen];
        slot.in_use.store(true, std::memory_order_release);

        // 將 OutMv8、OutColor8（及 OutRawMv4）從 UNORDERED_ACCESS 轉為 COPY_SOURCE
        D3D12_RESOURCE_BARRIER to_copy[3]{};
        to_copy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy[0].Transition.pResource   = m_out_mv8_tex.Get();
        to_copy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

        to_copy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy[1].Transition.pResource   = m_out_color8_tex.Get();
        to_copy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

        to_copy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy[2].Transition.pResource   = m_out_raw_mv4_tex.Get();
        to_copy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy[2].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd_list->ResourceBarrier(copy_raw_mv4 ? 3u : 2u, to_copy);

        // CopyTextureRegion: OutMv8 -> slot.mv8_buf
        D3D12_TEXTURE_COPY_LOCATION src_mv{};
        src_mv.pResource        = m_out_mv8_tex.Get();
        src_mv.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_mv.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_mv{};
        dst_mv.pResource       = slot.mv8_buf.Get();
        dst_mv.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_mv.PlacedFootprint.Offset             = 0;
        dst_mv.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst_mv.PlacedFootprint.Footprint.Width    = sbs_w;
        dst_mv.PlacedFootprint.Footprint.Height   = sbs_h;
        dst_mv.PlacedFootprint.Footprint.Depth    = 1;
        dst_mv.PlacedFootprint.Footprint.RowPitch = mv8_pitch;

        cmd_list->CopyTextureRegion(&dst_mv, 0, 0, 0, &src_mv, nullptr);

        // CopyTextureRegion: OutColor8 -> slot.color8_buf
        D3D12_TEXTURE_COPY_LOCATION src_col{};
        src_col.pResource        = m_out_color8_tex.Get();
        src_col.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_col.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_col{};
        dst_col.pResource       = slot.color8_buf.Get();
        dst_col.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_col.PlacedFootprint.Offset             = 0;
        dst_col.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst_col.PlacedFootprint.Footprint.Width    = sbs_w;
        dst_col.PlacedFootprint.Footprint.Height   = sbs_h;
        dst_col.PlacedFootprint.Footprint.Depth    = 1;
        dst_col.PlacedFootprint.Footprint.RowPitch = color8_pitch;

        cmd_list->CopyTextureRegion(&dst_col, 0, 0, 0, &src_col, nullptr);

        // CopyTextureRegion: OutRawMv4 -> slot.raw_mv4_buf (當啟用 raw 除錯傾印時)
        if (copy_raw_mv4 && slot.raw_mv4_buf) {
            D3D12_TEXTURE_COPY_LOCATION src_raw{};
            src_raw.pResource        = m_out_raw_mv4_tex.Get();
            src_raw.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_raw.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION dst_raw{};
            dst_raw.pResource       = slot.raw_mv4_buf.Get();
            dst_raw.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_raw.PlacedFootprint.Offset             = 0;
            dst_raw.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R32G32_FLOAT;
            dst_raw.PlacedFootprint.Footprint.Width    = raw_sbs_w;
            dst_raw.PlacedFootprint.Footprint.Height   = raw_sbs_h;
            dst_raw.PlacedFootprint.Footprint.Depth    = 1;
            dst_raw.PlacedFootprint.Footprint.RowPitch = raw_mv4_pitch;

            cmd_list->CopyTextureRegion(&dst_raw, 0, 0, 0, &src_raw, nullptr);
        }

        // 還原 OutMv8、OutColor8（及 OutRawMv4）回 UNORDERED_ACCESS
        D3D12_RESOURCE_BARRIER to_uav[3] = {to_copy[0], to_copy[1], to_copy[2]};
        std::swap(to_uav[0].Transition.StateBefore, to_uav[0].Transition.StateAfter);
        std::swap(to_uav[1].Transition.StateBefore, to_uav[1].Transition.StateAfter);
        std::swap(to_uav[2].Transition.StateBefore, to_uav[2].Transition.StateAfter);
        cmd_list->ResourceBarrier(copy_raw_mv4 ? 3u : 2u, to_uav);

        FrameDumpMetadata task{};
        task.game_frame_index        = m_game_frame_index;
        task.present_frame_index     = m_present_count;
        task.dump_seq_index          = m_dump_seq_counter++;
        task.dump_skipped_incomplete = m_dump_skipped_incomplete;
        task.eye_grid_w              = m_eye_grid_w;
        task.eye_grid_h              = m_eye_grid_h;
        task.sbs_grid_w              = sbs_w;
        task.sbs_grid_h              = sbs_h;
        task.mv8_row_pitch           = mv8_pitch;
        task.color8_row_pitch        = color8_pitch;
        task.dump_raw_mv4            = copy_raw_mv4;
        task.raw_mv4_eye_w           = copy_raw_mv4 ? m_raw_mv4_eye_w : 0u;
        task.raw_mv4_eye_h           = copy_raw_mv4 ? m_raw_mv4_eye_h : 0u;
        task.raw_mv4_sbs_w           = raw_sbs_w;
        task.raw_mv4_sbs_h           = raw_sbs_h;
        task.raw_mv4_row_pitch       = raw_mv4_pitch;
        task.readback_slot           = chosen;
        task.fence_value             = 0;
        task.eyes[0]                 = m_group_eye_meta[0];
        task.eyes[1]                 = m_group_eye_meta[1];
        QueryUevrHmdPoseLocked(task);

        m_dump_remaining_frames--;
        task.is_last_in_batch = (m_dump_remaining_frames == 0);

        m_staged_dump_tasks.push_back(std::move(task));
    }

    void SubmitAsyncDumpOnPresentLocked() {
        if (m_staged_dump_tasks.empty()) {
            return;
        }

        const auto* param = uevr::API::get()->param();
        if (param == nullptr || param->renderer == nullptr ||
            param->renderer->command_queue == nullptr || !m_readback_fence) {
            return;
        }

        auto* cmd_queue = static_cast<ID3D12CommandQueue*>(param->renderer->command_queue);
        const uint64_t fv = ++m_fence_counter;
        if (FAILED(cmd_queue->Signal(m_readback_fence.Get(), fv))) {
            LogError("[VIPLE-GAME-MV] CommandQueue::Signal failed on present");
            return;
        }

        {
            std::lock_guard<std::mutex> qlock(m_dump_queue_mutex);
            while (!m_staged_dump_tasks.empty()) {
                FrameDumpMetadata task = std::move(m_staged_dump_tasks.front());
                m_staged_dump_tasks.pop_front();
                task.fence_value         = fv;
                task.present_frame_index = m_present_count;
                m_dump_queue.push_back(std::move(task));
            }
        }
        m_dump_cv.notify_one();
    }

    void StartDumpWorker() {
        if (m_worker_running.exchange(true)) {
            return;
        }
        m_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_dump_worker = std::thread([this]() { DumpWorkerLoop(); });
    }

    void ShutdownWorker() {
        // 先停止 M-MV1 IPC 連線與 publish 執行緒
        if (m_ipc_running.exchange(false)) {
            if (m_ipc_stop_event != nullptr) {
                SetEvent(m_ipc_stop_event);
            }
            m_ipc_publish_cv.notify_all();
            if (m_ipc_conn_thread.joinable()) {
                m_ipc_conn_thread.join();
            }
            if (m_ipc_publish_thread.joinable()) {
                m_ipc_publish_thread.join();
            }
            {
                std::lock_guard<std::mutex> slock(m_ipc_session_mutex);
                if (m_ipc_session) {
                    m_ipc_session->active.store(false, std::memory_order_release);
                    m_ipc_session.reset();
                }
                m_ipc_connected.store(false, std::memory_order_release);
            }
            if (m_ipc_stop_event != nullptr) {
                CloseHandle(m_ipc_stop_event);
                m_ipc_stop_event = nullptr;
            }
            if (m_ipc_fence_event != nullptr) {
                CloseHandle(m_ipc_fence_event);
                m_ipc_fence_event = nullptr;
            }
        }

        // 停止 M-MV0 磁碟傾印執行緒
        if (!m_worker_running.exchange(false)) {
            return;
        }
        m_dump_cv.notify_all();
        if (m_dump_worker.joinable()) {
            m_dump_worker.join();
        }
        if (m_fence_event != nullptr) {
            CloseHandle(m_fence_event);
            m_fence_event = nullptr;
        }
    }

    // ========================================================================
    // M-MV1：Readback（VRMV_SLOTS 環狀緩衝）、Fence Signal、Publish 執行緒與 Named Pipe IPC
    // ========================================================================
    bool EnsureIpcReadbackSlotBufferLocked(uint32_t slot_idx, uint64_t required_bytes) {
        if (slot_idx >= VRMV_SLOTS || !m_device || required_bytes == 0) {
            return false;
        }
        IpcReadbackSlot& slot = m_ipc_rb_slots[slot_idx];
        if (slot.mv8_buf && slot.allocated_bytes >= required_bytes) {
            return true;
        }

        slot.mv8_buf.Reset();
        slot.allocated_bytes = 0;

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width            = required_bytes;
        buf_desc.Height           = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels        = 1;
        buf_desc.Format           = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Flags            = D3D12_RESOURCE_FLAG_NONE;

        if (FAILED(m_device->CreateCommittedResource(
                &rb_heap,
                D3D12_HEAP_FLAG_NONE,
                &buf_desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&slot.mv8_buf)))) {
            LogError("[VIPLE-GAME-MV-IPC] Failed to create IPC mv8 readback buffer (slot=%u, bytes=%llu)",
                     slot_idx,
                     static_cast<unsigned long long>(required_bytes));
            return false;
        }
        slot.allocated_bytes = required_bytes;
        return true;
    }

    // 需求 2：連線成功且有 section 時，每個雙眼齊備的遊戲幀，在右眼那次 EvaluateFeature 的 dispatch 之後
    // （同一張 command list、UAV barrier 之後），把 m_out_mv8_tex 轉成 COPY_SOURCE、CopyTextureRegion 到
    // READBACK heap 的環狀緩衝（VRMV_SLOTS 個，footprint 用 GetCopyableFootprints），再轉回原狀態。
    // 出口原因的節流 log（每種最多 3 次）：M-MV1 第一次實測 published=0 卻沒有任何訊息，看不出卡在哪
    void IpcSkipLog(uint32_t code, const char* what) {
        if (code < kIpcSkipCodes && m_ipc_skip_logged[code] < 3u) {
            m_ipc_skip_logged[code]++;
            LogWarn("[VIPLE-GAME-MV-IPC] skip code=%u (%s) game_frame=%llu", code, what,
                    static_cast<unsigned long long>(m_game_frame_index));
        }
    }

    void RecordCompleteStereoIpcToReadbackLocked(ID3D12GraphicsCommandList* cmd_list) {
        if (!m_device || !m_out_mv8_tex || !m_ipc_fence || m_eye_grid_w == 0 || m_eye_grid_h == 0) {
            IpcSkipLog(0, !m_device ? "no device" : !m_out_mv8_tex ? "no mv8 tex" : !m_ipc_fence ? "no ipc fence" : "grid 0");
            return;
        }
        // 只在左眼已就緒且右眼剛完成（同一遊戲幀雙眼齊備）時才複製
        if (!m_group_eye_ready[0] || !m_group_eye_ready[1]) {
            IpcSkipLog(1, "eye not ready");
            return;
        }

        const uint32_t sbs_w = m_eye_grid_w * 2u;
        const uint32_t sbs_h = m_eye_grid_h;
        if (sbs_w == 0 || sbs_w > VRMV_MAX_GRID_W || sbs_h == 0 || sbs_h > VRMV_MAX_GRID_H) {
            IpcSkipLog(2, "grid over VRMV_MAX");
            return;
        }

        const D3D12_RESOURCE_DESC mv8_desc = m_out_mv8_tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0;
        UINT64 row_size_in_bytes = 0;
        UINT64 total_bytes = 0;
        m_device->GetCopyableFootprints(&mv8_desc, 0, 1, 0, &footprint, &num_rows, &row_size_in_bytes, &total_bytes);
        if (total_bytes == 0 || footprint.Footprint.RowPitch < static_cast<UINT>(sbs_w * VRMV_CELL_BYTES)) {
            IpcSkipLog(3, "footprint");
            return;
        }

        // 挑選一個 publish 執行緒已讀完且 GPU 已完成前次寫入的 Readback Slot（不可被下一幀覆寫）
        const uint64_t completed_fv = m_ipc_fence->GetCompletedValue();
        uint32_t chosen = UINT32_MAX;
        for (uint32_t i = 0; i < VRMV_SLOTS; ++i) {
            const uint32_t candidate = (m_ipc_rb_cursor + i) % VRMV_SLOTS;
            if (!m_ipc_rb_slots[candidate].in_use.load(std::memory_order_acquire) &&
                completed_fv >= m_ipc_rb_slots[candidate].pending_fence_value) {
                chosen = candidate;
                m_ipc_rb_cursor = (candidate + 1u) % VRMV_SLOTS;
                break;
            }
        }
        if (chosen == UINT32_MAX) {
            // 環數不夠（前 VRMV_SLOTS 幀尚未被 publish 讀完）：丟棄新的那筆並計數
            m_ipc_frames_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
            m_ipc_window_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
            m_ipc_window_dropped_rb_full.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!EnsureIpcReadbackSlotBufferLocked(chosen, total_bytes)) {
            IpcSkipLog(4, "readback alloc");
            return;
        }

        IpcReadbackSlot& rb_slot = m_ipc_rb_slots[chosen];
        rb_slot.in_use.store(true, std::memory_order_release);

        // m_out_mv8_tex: UNORDERED_ACCESS -> COPY_SOURCE
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        to_copy.Transition.pResource   = m_out_mv8_tex.Get();
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        to_copy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd_list->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource        = m_out_mv8_tex.Get();
        src_loc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_loc{};
        dst_loc.pResource       = rb_slot.mv8_buf.Get();
        dst_loc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = footprint;

        cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

        // m_out_mv8_tex: COPY_SOURCE -> UNORDERED_ACCESS（轉回原狀態）
        D3D12_RESOURCE_BARRIER to_uav = to_copy;
        std::swap(to_uav.Transition.StateBefore, to_uav.Transition.StateAfter);
        cmd_list->ResourceBarrier(1, &to_uav);

        IpcPublishTask task{};
        task.game_frame_index      = m_game_frame_index;
        task.rb_slot_index         = chosen;
        task.sbs_grid_w            = sbs_w;
        task.sbs_grid_h            = sbs_h;
        task.footprint             = footprint;
        task.footprint_total_bytes = total_bytes;
        task.fence_value           = 0;
        task.game_present_qpc      = 0;
        task.eyes[0]               = m_group_eye_meta[0];
        task.eyes[1]               = m_group_eye_meta[1];
        task.fence                 = m_ipc_fence;
        task.mv8_buf               = rb_slot.mv8_buf;

        m_staged_ipc_tasks.push_back(std::move(task));
    }

    // 需求 3：on_present（雙眼齊備的那個遊戲幀第一次 Present）在遊戲的 command queue 上
    // Signal 外掛自己的 fence（值遞增），記下 game_present_qpc，推入 publish 佇列。
    void SubmitAsyncIpcOnPresentLocked() {
        if (m_staged_ipc_tasks.empty()) {
            return;
        }

        auto release_staged_slots = [this]() {
            while (!m_staged_ipc_tasks.empty()) {
                const uint32_t idx = m_staged_ipc_tasks.front().rb_slot_index;
                if (idx < VRMV_SLOTS) {
                    m_ipc_rb_slots[idx].in_use.store(false, std::memory_order_release);
                }
                m_staged_ipc_tasks.pop_front();
            }
        };

        const auto* param = uevr::API::get()->param();
        if (param == nullptr || param->renderer == nullptr ||
            param->renderer->command_queue == nullptr || !m_ipc_fence) {
            IpcSkipLog(5, "no command queue at present");
            release_staged_slots();
            return;
        }

        auto* cmd_queue = static_cast<ID3D12CommandQueue*>(param->renderer->command_queue);
        const uint64_t fv = ++m_ipc_fence_counter;
        if (FAILED(cmd_queue->Signal(m_ipc_fence.Get(), fv))) {
            LogError("[VIPLE-GAME-MV-IPC] CommandQueue::Signal failed on present");
            release_staged_slots();
            return;
        }
        const int64_t present_qpc = QueryQpcNow();

        {
            std::lock_guard<std::mutex> qlock(m_ipc_queue_mutex);
            while (!m_staged_ipc_tasks.empty()) {
                IpcPublishTask task = std::move(m_staged_ipc_tasks.front());
                m_staged_ipc_tasks.pop_front();
                task.fence_value      = fv;
                task.game_present_qpc = present_qpc;
                if (task.rb_slot_index < VRMV_SLOTS) {
                    m_ipc_rb_slots[task.rb_slot_index].pending_fence_value = fv;
                }
                m_ipc_publish_queue.push_back(std::move(task));
            }
        }
        m_ipc_publish_cv.notify_one();
    }

    // 需求 7：每 600 個遊戲幀一行 [VIPLE-GAME-MV-IPC] 統計
    void LogIpc600GameFrameStatsLocked(uint32_t window_game_frames) {
        std::vector<double> gpu_to_pub_samples;
        std::vector<double> present_to_pub_samples;
        {
            std::lock_guard<std::mutex> slock(m_ipc_stats_mutex);
            gpu_to_pub_samples.swap(m_win_gpu_to_publish_us);
            present_to_pub_samples.swap(m_win_present_to_publish_us);
        }

        auto calc_percentile = [](std::vector<double>& v, uint32_t pct) -> double {
            if (v.empty()) {
                return 0.0;
            }
            std::sort(v.begin(), v.end());
            const size_t idx = ((v.size() - 1u) * static_cast<size_t>(pct)) / 100u;
            return v[idx];
        };

        const double gpu_to_pub_p50     = calc_percentile(gpu_to_pub_samples, 50u);
        const double gpu_to_pub_p95     = calc_percentile(gpu_to_pub_samples, 95u);
        const double present_to_pub_p50 = calc_percentile(present_to_pub_samples, 50u);
        const double present_to_pub_p95 = calc_percentile(present_to_pub_samples, 95u);

        const uint64_t total_pub      = m_ipc_frames_published.load(std::memory_order_relaxed);
        const uint64_t total_inc      = m_ipc_frames_dropped_incomplete.load(std::memory_order_relaxed);
        const uint64_t total_late     = m_ipc_frames_dropped_gpu_late.load(std::memory_order_relaxed);
        const uint32_t win_pub        = m_ipc_window_published.exchange(0u, std::memory_order_relaxed);
        const uint32_t win_inc        = m_ipc_window_dropped_incomplete.exchange(0u, std::memory_order_relaxed);
        const uint32_t win_late       = m_ipc_window_dropped_gpu_late.exchange(0u, std::memory_order_relaxed);
        const uint32_t win_rb_full    = m_ipc_window_dropped_rb_full.exchange(0u, std::memory_order_relaxed);
        const bool     connected      = m_ipc_connected.load(std::memory_order_relaxed);
        const uint64_t cur_gen        = m_ipc_current_gen.load(std::memory_order_relaxed);
        const uint32_t delay_ms       = m_ipc_injected_delay_ms.load(std::memory_order_relaxed);

        LogInfo(
            "[VIPLE-GAME-MV-IPC] 600-game-frame stats (window_gf=%u, connected=%d, gen=%llu, delay_ms=%u): "
            "published=%llu (win=%u), dropped_incomplete=%llu (win=%u), dropped_gpu_late=%llu (win=%u, rb_full=%u), "
            "gpu_done->publish_us=(p50:%.1f, p95:%.1f), game_present->publish_us=(p50:%.1f, p95:%.1f)",
            window_game_frames,
            connected ? 1 : 0,
            static_cast<unsigned long long>(cur_gen),
            delay_ms,
            static_cast<unsigned long long>(total_pub),
            win_pub,
            static_cast<unsigned long long>(total_inc),
            win_inc,
            static_cast<unsigned long long>(total_late),
            win_late,
            win_rb_full,
            gpu_to_pub_p50,
            gpu_to_pub_p95,
            present_to_pub_p50,
            present_to_pub_p95);
    }

    void StartIpcWorkers() {
        if (m_ipc_running.exchange(true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> slock(m_ipc_stats_mutex);
            m_win_gpu_to_publish_us.reserve(1024);
            m_win_present_to_publish_us.reserve(1024);
        }
        m_ipc_stop_event  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_ipc_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_ipc_publish_thread = std::thread([this]() { IpcPublishWorkerLoop(); });
        m_ipc_conn_thread    = std::thread([this]() { IpcConnWorkerLoop(); });
    }

    std::shared_ptr<IpcMappedSession> GetActiveIpcSession() {
        std::lock_guard<std::mutex> slock(m_ipc_session_mutex);
        return m_ipc_session;
    }

    void SetActiveIpcSession(std::shared_ptr<IpcMappedSession> sess) {
        {
            std::lock_guard<std::mutex> slock(m_ipc_session_mutex);
            if (m_ipc_session && m_ipc_session != sess) {
                m_ipc_session->active.store(false, std::memory_order_release);
            }
            m_ipc_session = std::move(sess);
            const bool active = (m_ipc_session != nullptr);
            m_ipc_connected.store(active, std::memory_order_release);
            m_ipc_current_gen.store(active ? m_ipc_session->generation : 0ull, std::memory_order_relaxed);
        }
        m_ipc_publish_cv.notify_one();
    }

    // ------------------------------------------------------------------------
    // M-MV1 背景連線執行緒：Named Pipe 連線、S-1-5-18 身分驗證、HELLO/WELCOME 握手、
    // MapViewOfFile、VirtualQuery、server block 驗證、斷線/BYE 監聽與退避重試
    // ------------------------------------------------------------------------
    void IpcConnWorkerLoop() {
        DWORD own_session = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session)) {
            const DWORD err = GetLastError();
            LogError("[VIPLE-GAME-MV-IPC] ProcessIdToSessionId failed (err=%lu)", static_cast<unsigned long>(err));
            return;
        }

        const std::wstring pipe_name = std::wstring(VRMV_PIPE_NAME_PREFIX) + std::to_wstring(own_session);
        LogInfo("[VIPLE-GAME-MV-IPC] IPC client worker started (pipe=%ls, session=%lu, pid=%lu)",
                pipe_name.c_str(),
                static_cast<unsigned long>(own_session),
                static_cast<unsigned long>(m_self_pid));

        uint32_t consecutive_failures = 0;
        uint32_t retry_delay_ms       = 0;

        while (m_ipc_running.load(std::memory_order_acquire)) {
            if (retry_delay_ms > 0) {
                if (WaitForSingleObject(m_ipc_stop_event, retry_delay_ms) == WAIT_OBJECT_0) {
                    break;
                }
            }
            if (!m_ipc_running.load(std::memory_order_acquire)) {
                break;
            }

            HANDLE pipe = CreateFileW(
                pipe_name.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                nullptr);

            if (pipe == INVALID_HANDLE_VALUE) {
                const DWORD err = GetLastError();
                if (err == ERROR_PIPE_BUSY) {
                    WaitNamedPipeW(pipe_name.c_str(), 100);
                }
                consecutive_failures++;
                retry_delay_ms = CalcIpcBackoffMs(consecutive_failures);
                LogWarn("[VIPLE-GAME-MV-IPC] Connect failed: CreateFileW(%ls) err=%lu, retry in %u ms",
                        pipe_name.c_str(),
                        static_cast<unsigned long>(err),
                        retry_delay_ms);
                continue;
            }

            DWORD pipe_mode = PIPE_READMODE_MESSAGE;
            if (!SetNamedPipeHandleState(pipe, &pipe_mode, nullptr, nullptr)) {
                const DWORD err = GetLastError();
                CloseHandle(pipe);
                consecutive_failures++;
                retry_delay_ms = CalcIpcBackoffMs(consecutive_failures);
                LogWarn("[VIPLE-GAME-MV-IPC] SetNamedPipeHandleState(PIPE_READMODE_MESSAGE) failed err=%lu, retry in %u ms",
                        static_cast<unsigned long>(err),
                        retry_delay_ms);
                continue;
            }

            ULONG server_pid = 0;
            const char* id_why = "ok";
            DWORD id_err = 0;
            if (!VerifyVrmvPipeServerIdentity(pipe, own_session, server_pid, id_why, id_err)) {
                CloseHandle(pipe);
                consecutive_failures++;
                retry_delay_ms = CalcIpcBackoffMs(consecutive_failures);
                LogWarn("[VIPLE-GAME-MV-IPC] Server identity verification failed (reason=%s, server_pid=%lu, err=%lu), disconnecting and retrying in %u ms",
                        id_why,
                        static_cast<unsigned long>(server_pid),
                        static_cast<unsigned long>(id_err),
                        retry_delay_ms);
                continue;
            }

            uint32_t special_backoff_ms = 0;
            bool handshake_succeeded    = false;
            RunOneIpcSession(pipe, server_pid, handshake_succeeded, special_backoff_ms);
            CloseHandle(pipe);

            if (!m_ipc_running.load(std::memory_order_acquire)) {
                break;
            }

            if (handshake_succeeded) {
                consecutive_failures = 0;
            }
            if (special_backoff_ms > 0) {
                retry_delay_ms = special_backoff_ms;
            } else {
                consecutive_failures++;
                retry_delay_ms = CalcIpcBackoffMs(consecutive_failures);
            }
        }

        LogInfo("[VIPLE-GAME-MV-IPC] IPC client worker stopped");
    }

    void RunOneIpcSession(
        HANDLE pipe,
        ULONG verified_server_pid,
        bool& out_handshake_succeeded,
        uint32_t& out_special_backoff_ms) {
        out_handshake_succeeded = false;
        out_special_backoff_ms  = 0;

        OVERLAPPED rov{};
        OVERLAPPED wov{};
        rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (rov.hEvent == nullptr || wov.hEvent == nullptr) {
            if (rov.hEvent != nullptr) {
                CloseHandle(rov.hEvent);
            }
            if (wov.hEvent != nullptr) {
                CloseHandle(wov.hEvent);
            }
            LogError("[VIPLE-GAME-MV-IPC] Failed to create overlapped events for pipe session");
            return;
        }

        auto cleanup_events = [&rov, &wov]() {
            if (rov.hEvent != nullptr) {
                CloseHandle(rov.hEvent);
                rov.hEvent = nullptr;
            }
            if (wov.hEvent != nullptr) {
                CloseHandle(wov.hEvent);
                wov.hEvent = nullptr;
            }
        };

        uint32_t seq_out     = 0;
        uint32_t last_seq_in = 0;

        // 1. 送出 vrmv_hello_t
        vrmv_hello_t hello{};
        hello.hdr.magic             = VRMV_MAGIC_PIPE;
        hello.hdr.type              = VRMV_MSG_HELLO;
        hello.hdr.flags             = 0;
        hello.hdr.size              = static_cast<uint32_t>(sizeof(vrmv_hello_t));
        hello.hdr.seq               = 0;
        hello.abi_version           = VRMV_ABI_VERSION;
        hello.plugin_pid            = m_self_pid;
        hello.plugin_version_packed = (0u << 24) | (1u << 16) | 1u; // 0.1.1
        hello.upscaler_kind         = VRMV_UPSCALER_DLSS;

        if (!WriteVrmvPipeMsg(pipe, wov, &hello, static_cast<uint32_t>(sizeof(hello)), seq_out)) {
            LogWarn("[VIPLE-GAME-MV-IPC] Failed to write HELLO to server (server_pid=%lu)",
                    static_cast<unsigned long>(verified_server_pid));
            cleanup_events();
            return;
        }

        // 2. 接收 vrmv_welcome_t 或 vrmv_reject_t（緩衝一律 VRMV_PIPE_MAX_MSG，先驗 hdr 再驗 sizeof）
        alignas(8) uint8_t rbuf[VRMV_PIPE_MAX_MSG]{};
        ResetEvent(rov.hEvent);
        bool read_pending = false;
        if (!ReadFile(pipe, rbuf, sizeof(rbuf), nullptr, &rov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                LogWarn("[VIPLE-GAME-MV-IPC] ReadFile(WELCOME/REJECT) failed immediately (err=%lu)",
                        static_cast<unsigned long>(err));
                cleanup_events();
                return;
            }
            read_pending = true;
        }

        if (read_pending) {
            HANDLE wait_handles[2] = {m_ipc_stop_event, rov.hEvent};
            const DWORD w = WaitForMultipleObjects(2, wait_handles, FALSE, kIpcWelcomeTimeoutMs);
            if (w != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &rov);
                DWORD dummy = 0;
                GetOverlappedResult(pipe, &rov, &dummy, TRUE);
                if (w == WAIT_TIMEOUT) {
                    LogWarn("[VIPLE-GAME-MV-IPC] Timed out waiting for WELCOME/REJECT from server");
                }
                cleanup_events();
                return;
            }
        }

        DWORD bytes_read = 0;
        if (!GetOverlappedResult(pipe, &rov, &bytes_read, FALSE)) {
            const DWORD err = GetLastError();
            LogWarn("[VIPLE-GAME-MV-IPC] GetOverlappedResult(WELCOME/REJECT) failed (err=%lu)",
                    static_cast<unsigned long>(err));
            cleanup_events();
            return;
        }

        vrmv_msg_hdr_t hdr{};
        const char* hdr_why = "ok";
        if (!CheckVrmvMsgHeader(rbuf, bytes_read, hdr, hdr_why)) {
            LogWarn("[VIPLE-GAME-MV-IPC] Invalid message header from server: reason=%s (bytes=%lu)",
                    hdr_why,
                    static_cast<unsigned long>(bytes_read));
            cleanup_events();
            return;
        }
        if (hdr.seq <= last_seq_in) {
            LogWarn("[VIPLE-GAME-MV-IPC] Invalid message seq from server (seq=%u, last=%u)",
                    hdr.seq,
                    last_seq_in);
            cleanup_events();
            return;
        }
        last_seq_in = hdr.seq;

        if (hdr.type == VRMV_MSG_REJECT) {
            if (bytes_read < sizeof(vrmv_reject_t)) {
                LogWarn("[VIPLE-GAME-MV-IPC] REJECT message too short (%lu < %zu)",
                        static_cast<unsigned long>(bytes_read),
                        sizeof(vrmv_reject_t));
                cleanup_events();
                return;
            }
            vrmv_reject_t rj{};
            std::memcpy(&rj, rbuf, sizeof(rj));
            if (rj.reason == VRMV_REJ_DISABLED || rj.reason == VRMV_REJ_IDENTITY) {
                out_special_backoff_ms = kIpcBackoffLongMs; // 30 s 重試
            }
            const uint32_t next_ms = (out_special_backoff_ms > 0) ? out_special_backoff_ms : kIpcBackoffInitMs;
            LogWarn("[VIPLE-GAME-MV-IPC] Connection rejected by server: reason=%s (%u), retry in %u ms",
                    VrmvRejectReasonName(rj.reason),
                    rj.reason,
                    next_ms);
            cleanup_events();
            return;
        }

        if (hdr.type != VRMV_MSG_WELCOME) {
            LogWarn("[VIPLE-GAME-MV-IPC] Unexpected message type=%u while waiting for WELCOME", hdr.type);
            cleanup_events();
            return;
        }

        // 先比對 abi_version，再驗 bytes == sizeof(vrmv_welcome_t)（遵守 vr_ipc_abi.h 規則 7）
        uint32_t welcome_abi = 0;
        std::memcpy(&welcome_abi, rbuf + sizeof(vrmv_msg_hdr_t), sizeof(uint32_t));
        if (welcome_abi != VRMV_ABI_VERSION) {
            LogWarn("[VIPLE-GAME-MV-IPC] WELCOME abi_version mismatch (server=%u, plugin=%u)",
                    welcome_abi,
                    static_cast<unsigned>(VRMV_ABI_VERSION));
            cleanup_events();
            return;
        }
        if (bytes_read != sizeof(vrmv_welcome_t)) {
            LogWarn("[VIPLE-GAME-MV-IPC] WELCOME size mismatch (bytes=%lu, expected=%zu)",
                    static_cast<unsigned long>(bytes_read),
                    sizeof(vrmv_welcome_t));
            cleanup_events();
            return;
        }

        vrmv_welcome_t wl{};
        std::memcpy(&wl, rbuf, sizeof(wl));
        if (wl.generation == 0 ||
            wl.section_size < VRMV_SECTION_SIZE ||
            wl.qpc_frequency != m_qpc_freq ||
            !IsPlausibleHandleValue(wl.dup_section_handle)) {
            CloseIfPlausibleHandle(wl.dup_section_handle);
            LogWarn("[VIPLE-GAME-MV-IPC] WELCOME validation failed (gen=%llu, section_size=%llu, qpf=%lld)",
                    static_cast<unsigned long long>(wl.generation),
                    static_cast<unsigned long long>(wl.section_size),
                    static_cast<long long>(wl.qpc_frequency));
            cleanup_events();
            return;
        }

        // 3. MapViewOfFile 整個 VRMV_SECTION_SIZE（handle 已在本行程內，開完或失敗皆由本行程唯一負責關閉；絕不把 handle 寫進 log）
        HANDLE sec_handle = ToWinHandle(wl.dup_section_handle);
        void* view = MapViewOfFile(sec_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VRMV_SECTION_SIZE);
        const DWORD map_err = (view != nullptr) ? 0u : GetLastError();
        CloseHandle(sec_handle); // view 映射成功後會保住 section 物件
        if (view == nullptr) {
            LogWarn("[VIPLE-GAME-MV-IPC] MapViewOfFile failed (err=%lu)", static_cast<unsigned long>(map_err));
            cleanup_events();
            return;
        }

        auto sess = std::make_shared<IpcMappedSession>();
        sess->shm           = static_cast<vrmv_shm_t*>(view);
        sess->generation    = wl.generation;
        sess->section_size  = wl.section_size;
        sess->qpc_frequency = wl.qpc_frequency;
        sess->server_pid    = wl.server_pid;

        // 用 VirtualQuery 確認大小 >= section_size（且 >= VRMV_SECTION_SIZE）
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 ||
            mbi.RegionSize < VRMV_SECTION_SIZE ||
            mbi.RegionSize < wl.section_size) {
            LogWarn("[VIPLE-GAME-MV-IPC] VirtualQuery region size check failed (region=%llu, expected=%llu)",
                    static_cast<unsigned long long>(mbi.RegionSize),
                    static_cast<unsigned long long>(wl.section_size));
            cleanup_events();
            return;
        }

        // 整份複製 server block 到本地再驗證 magic / abi / generation（遵守 ABI 規則 6）
        vrmv_server_block_t srv_blk{};
        VRMV_COMPILER_BARRIER();
        std::memcpy(&srv_blk, &sess->shm->server, sizeof(srv_blk));
        VRMV_COMPILER_BARRIER();

        if (srv_blk.magic != VRMV_MAGIC_SHM ||
            srv_blk.abi_version != VRMV_ABI_VERSION ||
            srv_blk.section_size < VRMV_SECTION_SIZE ||
            srv_blk.section_size != wl.section_size ||
            srv_blk.generation != wl.generation ||
            srv_blk.qpc_frequency != wl.qpc_frequency) {
            LogWarn("[VIPLE-GAME-MV-IPC] SHM server block validation failed (magic=0x%08x, abi=%u, gen=%llu/%llu, size=%llu)",
                    srv_blk.magic,
                    srv_blk.abi_version,
                    static_cast<unsigned long long>(srv_blk.generation),
                    static_cast<unsigned long long>(wl.generation),
                    static_cast<unsigned long long>(srv_blk.section_size));
            cleanup_events();
            return;
        }

        out_handshake_succeeded = true;
        SetActiveIpcSession(sess);
        LogInfo("[VIPLE-GAME-MV-IPC] Connected to server and mapped SHM (server_pid=%u, gen=%llu, section_size=%llu)",
                wl.server_pid,
                static_cast<unsigned long long>(wl.generation),
                static_cast<unsigned long long>(wl.section_size));

        // 4. 持續監聽 Pipe：直到 server 送 BYE、pipe 斷線或外掛卸載
        const char* disconnect_reason = "pipe-broken";
        for (;;) {
            ResetEvent(rov.hEvent);
            bool pending = false;
            if (!ReadFile(pipe, rbuf, sizeof(rbuf), nullptr, &rov)) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    disconnect_reason = "pipe-disconnected";
                    break;
                }
                pending = true;
            }

            if (pending) {
                HANDLE wait_handles[2] = {m_ipc_stop_event, rov.hEvent};
                const DWORD w = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {
                    // 外掛正在卸載：取消讀取並禮貌送出 BYE
                    CancelIoEx(pipe, &rov);
                    DWORD dummy = 0;
                    GetOverlappedResult(pipe, &rov, &dummy, TRUE);

                    vrmv_bye_t bye{};
                    bye.hdr.magic = VRMV_MAGIC_PIPE;
                    bye.hdr.type  = VRMV_MSG_BYE;
                    bye.hdr.flags = 0;
                    bye.hdr.size  = static_cast<uint32_t>(sizeof(vrmv_bye_t));
                    bye.reason    = 0;
                    WriteVrmvPipeMsg(pipe, wov, &bye, static_cast<uint32_t>(sizeof(bye)), seq_out);
                    disconnect_reason = "plugin-shutdown";
                    break;
                }
            }

            DWORD n = 0;
            if (!GetOverlappedResult(pipe, &rov, &n, FALSE)) {
                disconnect_reason = "pipe-disconnected";
                break;
            }

            vrmv_msg_hdr_t in_hdr{};
            const char* in_why = "ok";
            if (!CheckVrmvMsgHeader(rbuf, n, in_hdr, in_why) || in_hdr.seq <= last_seq_in) {
                disconnect_reason = "protocol-error";
                break;
            }
            last_seq_in = in_hdr.seq;

            if (in_hdr.type == VRMV_MSG_BYE) {
                if (n != sizeof(vrmv_bye_t)) {
                    disconnect_reason = "bye-bad-size";
                    break;
                }
                vrmv_bye_t bye{};
                std::memcpy(&bye, rbuf, sizeof(bye));
                LogInfo("[VIPLE-GAME-MV-IPC] Received BYE from server (gen=%llu, reason=%u)",
                        static_cast<unsigned long long>(wl.generation),
                        bye.reason);
                disconnect_reason = "server-bye";
                break;
            } else {
                disconnect_reason = "unexpected-msg";
                break;
            }
        }

        // 斷線或收到 BYE：停用 session 並清空（待 publish 執行緒放掉最後參照即自動 unmap）
        sess->active.store(false, std::memory_order_release);
        SetActiveIpcSession(nullptr);
        LogInfo("[VIPLE-GAME-MV-IPC] Session ended (gen=%llu, reason=%s), unmapping SHM and returning to reconnect",
                static_cast<unsigned long long>(wl.generation),
                disconnect_reason);
        cleanup_events();
    }

    // ------------------------------------------------------------------------
    // M-MV1 Publish 執行緒：
    //   - 每 100 ms 寫一次 writer.writer_heartbeat_qpc 與 frames_* 計數、writer_pid
    //   - 每 2 秒重讀一次 viple_mv_delay.txt（延遲注入毫秒數）
    //   - 用 SetEventOnCompletion 等待 GPU readback fence（上限 50 ms；逾時放棄並計入 frames_dropped_gpu_late）
    //   - 完成後記 gpu_done_qpc ->（選配延遲注入 Sleep）-> 逐列緊密複製 payload 至 vrmv_payload_offset(slot)
    //     -> per-slot seqlock 寫入 vrmv_desc_t -> store_release 更新 write_index = mv_frame_id
    // ------------------------------------------------------------------------
    void WriteIpcWriterHeartbeatOnPublishThread(IpcMappedSession& sess, int64_t now_qpc) {
        if (!sess.shm || !sess.active.load(std::memory_order_acquire)) {
            return;
        }
        vrmv_writer_block_t& wb = sess.shm->writer;
        wb.frames_published          = m_ipc_frames_published.load(std::memory_order_relaxed);
        wb.frames_dropped_gpu_late   = m_ipc_frames_dropped_gpu_late.load(std::memory_order_relaxed);
        wb.frames_dropped_incomplete = m_ipc_frames_dropped_incomplete.load(std::memory_order_relaxed);
        wb.writer_pid                = m_self_pid;
        wb.reserved0                 = 0;
        wb.reserved1                 = 0;  // pending_* 由 publish 流程寫，這裡不碰
        vrmv_store_release_u64(&wb.writer_heartbeat_qpc, static_cast<uint64_t>(now_qpc));
        sess.next_heartbeat_qpc = now_qpc + MsToQpc(m_qpc_freq, kIpcHeartbeatIntervalMs);
    }

    void RefreshDelayInjectionOnPublishThread(int64_t now_qpc) {
        if (m_next_delay_check_qpc != 0 && now_qpc < m_next_delay_check_qpc) {
            return;
        }
        m_next_delay_check_qpc = now_qpc + MsToQpc(m_qpc_freq, kIpcDelayReloadMs);

        uint32_t new_delay_ms = 0;
        const fs::path delay_path = m_plugin_dir / L"viple_mv_delay.txt";
        std::error_code ec;
        if (fs::exists(delay_path, ec)) {
            std::ifstream ifs(delay_path);
            long long val = 0;
            if ((ifs >> val) && val > 0) {
                new_delay_ms = static_cast<uint32_t>(std::clamp<long long>(val, 0ll, 5000ll));
            }
        }

        const uint32_t prev_delay = m_ipc_injected_delay_ms.exchange(new_delay_ms, std::memory_order_relaxed);
        if (prev_delay != new_delay_ms) {
            LogInfo("[VIPLE-GAME-MV-IPC] Delay injection updated: %u ms -> %u ms (file: %s)",
                    prev_delay,
                    new_delay_ms,
                    delay_path.u8string().c_str());
        }
    }

    void IpcPublishWorkerLoop() {
        while (m_ipc_running.load(std::memory_order_acquire)) {
            const int64_t now = QueryQpcNow();
            RefreshDelayInjectionOnPublishThread(now);

            std::shared_ptr<IpcMappedSession> sess = GetActiveIpcSession();
            if (sess && sess->active.load(std::memory_order_acquire)) {
                if (sess->next_heartbeat_qpc == 0 || now >= sess->next_heartbeat_qpc) {
                    WriteIpcWriterHeartbeatOnPublishThread(*sess, now);
                }
            }

            IpcPublishTask task{};
            bool has_task = false;
            {
                std::unique_lock<std::mutex> qlock(m_ipc_queue_mutex);
                if (m_ipc_publish_queue.empty()) {
                    m_ipc_publish_cv.wait_for(
                        qlock,
                        std::chrono::milliseconds(25),
                        [this]() {
                            return !m_ipc_publish_queue.empty() ||
                                   !m_ipc_running.load(std::memory_order_acquire);
                        });
                }
                if (!m_ipc_running.load(std::memory_order_acquire) && m_ipc_publish_queue.empty()) {
                    break;
                }
                if (!m_ipc_publish_queue.empty()) {
                    task = std::move(m_ipc_publish_queue.front());
                    m_ipc_publish_queue.pop_front();
                    has_task = true;
                }
            }

            if (has_task) {
                ProcessIpcPublishTaskOnWorker(task);
            }
        }
    }

    void ProcessIpcPublishTaskOnWorker(IpcPublishTask& task) {
        auto release_rb_slot = [this, &task]() {
            if (task.rb_slot_index < VRMV_SLOTS) {
                m_ipc_rb_slots[task.rb_slot_index].in_use.store(false, std::memory_order_release);
            }
        };

        if (!task.fence || !task.mv8_buf || task.sbs_grid_w == 0 || task.sbs_grid_h == 0) {
            IpcSkipLog(6, "bad task");
            release_rb_slot();
            return;
        }

        // 1. 用 SetEventOnCompletion 等待該 fence 值（等待上限 50 ms；逾時＝放棄該筆、frames_dropped_gpu_late++）
        if (task.fence->GetCompletedValue() < task.fence_value) {
            if (FAILED(task.fence->SetEventOnCompletion(task.fence_value, m_ipc_fence_event))) {
                m_ipc_frames_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
                m_ipc_window_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
                release_rb_slot();
                return;
            }
            const DWORD w = WaitForSingleObject(m_ipc_fence_event, kIpcFenceWaitTimeoutMs);
            if (w != WAIT_OBJECT_0 || task.fence->GetCompletedValue() < task.fence_value) {
                m_ipc_frames_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
                m_ipc_window_dropped_gpu_late.fetch_add(1, std::memory_order_relaxed);
                release_rb_slot();
                return;
            }
        }

        // 2. GPU 完成後立刻記下 gpu_done_qpc
        const int64_t gpu_done_qpc = QueryQpcNow();

        std::shared_ptr<IpcMappedSession> sess = GetActiveIpcSession();
        if (!sess || !sess->shm || !sess->active.load(std::memory_order_acquire)) {
            IpcSkipLog(7, "no active session at publish");
            release_rb_slot();
            return;
        }

        // 2b. 先公告「這一幀 GPU 做完了」（還沒複製、還沒發布）：server 看到比手上更新的 pending 幀
        //     就知道被合成的是它、這張不配 MV，不會拿上一幀頂替（延遲注入與執行緒被搶占時會發生）
        const uint64_t mv_frame_id = ++sess->local_mv_frame_id;
        sess->shm->writer.pending_gpu_done_qpc = gpu_done_qpc;
        vrmv_store_release_u64(&sess->shm->writer.pending_mv_frame_id, mv_frame_id);

        // 3. 需求 6：延遲注入（viple_mv_delay.txt，內容為毫秒數；publish 執行緒在 GPU 完成後多 Sleep 那麼久再寫）
        RefreshDelayInjectionOnPublishThread(gpu_done_qpc);
        const uint32_t delay_ms = m_ipc_injected_delay_ms.load(std::memory_order_relaxed);
        if (delay_ms > 0 && m_ipc_stop_event != nullptr) {
            if (WaitForSingleObject(m_ipc_stop_event, delay_ms) == WAIT_OBJECT_0) {
                release_rb_slot();
                return;
            }
        }

        if (!sess->active.load(std::memory_order_acquire)) {
            IpcSkipLog(7, "no active session at publish");
            release_rb_slot();
            return;
        }

        const uint32_t tight_row_bytes = task.sbs_grid_w * VRMV_CELL_BYTES;
        const uint32_t payload_bytes   = tight_row_bytes * task.sbs_grid_h;
        const uint32_t src_row_pitch   = task.footprint.Footprint.RowPitch;
        const uint64_t src_base_offset = task.footprint.Offset;

        if (payload_bytes == 0 ||
            payload_bytes > VRMV_SLOT_PAYLOAD_BYTES ||
            src_row_pitch < tight_row_bytes ||
            src_base_offset + static_cast<uint64_t>(src_row_pitch) * (task.sbs_grid_h - 1u) + tight_row_bytes >
                task.footprint_total_bytes) {
            IpcSkipLog(8, "payload size");
            release_rb_slot();
            return;
        }

        // 4. Map readback buffer
        D3D12_RANGE read_range{
            static_cast<SIZE_T>(src_base_offset),
            static_cast<SIZE_T>(task.footprint_total_bytes)};  // 最後一列沒有補到 RowPitch：範圍超出緩衝區 Map 會失敗
        void* mapped = nullptr;
        if (FAILED(task.mv8_buf->Map(0, &read_range, &mapped)) || mapped == nullptr) {
            IpcSkipLog(9, "map");
            release_rb_slot();
            return;
        }

        // 5. 計算 mv_frame_id、slot = mv_frame_id % VRMV_SLOTS、本地計數 n 與 game_frame_step (1..255)
        const uint32_t slot        = static_cast<uint32_t>(mv_frame_id % VRMV_SLOTS);
        const uint64_t n           = sess->local_slot_write_count[slot]++;

        uint8_t game_frame_step = 1u;
        if (sess->last_published_game_frame != 0 && task.game_frame_index > sess->last_published_game_frame) {
            const uint64_t diff = task.game_frame_index - sess->last_published_game_frame;
            game_frame_step = static_cast<uint8_t>(std::clamp<uint64_t>(diff, 1ull, 255ull));
        }
        sess->last_published_game_frame = task.game_frame_index;

        // 6. 組裝 vrmv_desc_t 欄位（需求 4）
        const EyeEvalMetadata& left_eye  = task.eyes[0];
        const EyeEvalMetadata& right_eye = task.eyes[1];

        uint32_t desc_flags = 0u;
        if (left_eye.valid) {
            desc_flags |= VRMV_F_LEFT_VALID;
        }
        if (right_eye.valid) {
            desc_flags |= VRMV_F_RIGHT_VALID;
        }
        if (left_eye.depth_res_ptr != 0 && right_eye.depth_res_ptr != 0) {
            desc_flags |= VRMV_F_HAS_DEPTH;
        }
        if (((left_eye.create_flags | right_eye.create_flags) & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0) {
            desc_flags |= VRMV_F_DEPTH_INVERTED;
        }
        if (left_eye.reset != 0 || right_eye.reset != 0) {
            desc_flags |= VRMV_F_RESET;
        }

        const uint32_t mv_tex_w = (left_eye.mv_valid_w > 0) ? left_eye.mv_valid_w : left_eye.mv_tex_w;
        const uint32_t mv_tex_h = (left_eye.mv_valid_h > 0) ? left_eye.mv_valid_h : left_eye.mv_tex_h;

        vrmv_desc_t desc{};
        desc.seq              = 0;
        desc.mv_frame_id      = mv_frame_id;
        desc.generation       = sess->generation;
        desc.game_present_qpc = task.game_present_qpc;
        desc.gpu_done_qpc     = gpu_done_qpc;
        desc.publish_qpc      = 0;
        desc.flags            = desc_flags;
        desc.payload_bytes    = payload_bytes;
        desc.grid_w           = static_cast<uint16_t>(task.sbs_grid_w);
        desc.grid_h           = static_cast<uint16_t>(task.sbs_grid_h);
        desc.render_eye_w     = static_cast<uint16_t>(left_eye.render_w);
        desc.render_eye_h     = static_cast<uint16_t>(left_eye.render_h);
        desc.mv_tex_w         = static_cast<uint16_t>(mv_tex_w);
        desc.mv_tex_h         = static_cast<uint16_t>(mv_tex_h);
        desc.mv_units         = VRMV_UNITS_EYE_UV;
        desc.upscaler_kind    = VRMV_UPSCALER_DLSS;
        desc.game_frame_step  = game_frame_step;
        desc.mv_scale[0]      = left_eye.mv_scale_x;
        desc.mv_scale[1]      = left_eye.mv_scale_y;
        desc.depth_ref        = left_eye.depth_ref;

        // 7. Seqlock 寫入：vrmv_seq_write_begin(seq, 2n+1) -> payload + desc 欄位 -> vrmv_store_release_u64(seq, 2n+2)
        //    -> vrmv_store_release_u64(&write_index, mv_frame_id)
        vrmv_shm_t* shm          = sess->shm;
        vrmv_desc_t* dst_desc    = &shm->slot[slot];
        uint8_t* dst_payload     = reinterpret_cast<uint8_t*>(shm) + vrmv_payload_offset(slot);
        const uint8_t* src_bytes = static_cast<const uint8_t*>(mapped) + src_base_offset;

        vrmv_seq_write_begin(&dst_desc->seq, 2ull * n + 1ull);

        for (uint32_t y = 0; y < task.sbs_grid_h; ++y) {
            std::memcpy(
                dst_payload + static_cast<size_t>(y) * tight_row_bytes,
                src_bytes + static_cast<size_t>(y) * src_row_pitch,
                tight_row_bytes);
        }

        // Payload 已複製完畢，立即 Unmap 並釋放 readback slot 供後續遊戲幀使用
        D3D12_RANGE write_range{0, 0};
        task.mv8_buf->Unmap(0, &write_range);
        release_rb_slot();

        const int64_t publish_qpc = QueryQpcNow();
        desc.publish_qpc = publish_qpc;

        std::memcpy(
            reinterpret_cast<char*>(dst_desc) + sizeof(uint64_t),
            reinterpret_cast<const char*>(&desc) + sizeof(uint64_t),
            sizeof(vrmv_desc_t) - sizeof(uint64_t));

        vrmv_store_release_u64(&dst_desc->seq, 2ull * n + 2ull);
        vrmv_store_release_u64(&shm->write_index, mv_frame_id);

        m_ipc_frames_published.fetch_add(1, std::memory_order_relaxed);
        m_ipc_window_published.fetch_add(1, std::memory_order_relaxed);

        // 記錄延遲統計樣本（微秒 µs）
        const double inv_qpf_us = 1000000.0 / static_cast<double>(m_qpc_freq);
        const double gpu_to_pub_us =
            static_cast<double>(std::max<int64_t>(0ll, publish_qpc - gpu_done_qpc)) * inv_qpf_us;
        const double present_to_pub_us =
            static_cast<double>(std::max<int64_t>(0ll, publish_qpc - task.game_present_qpc)) * inv_qpf_us;
        {
            std::lock_guard<std::mutex> slock(m_ipc_stats_mutex);
            if (m_win_gpu_to_publish_us.size() < 4096u) {
                m_win_gpu_to_publish_us.push_back(gpu_to_pub_us);
                m_win_present_to_publish_us.push_back(present_to_pub_us);
            }
        }

        // 若已達 100 ms heartbeat 週期，順便更新 writer block
        if (sess->next_heartbeat_qpc == 0 || publish_qpc >= sess->next_heartbeat_qpc) {
            WriteIpcWriterHeartbeatOnPublishThread(*sess, publish_qpc);
        }
    }

    void DumpWorkerLoop() {
        while (m_worker_running.load(std::memory_order_acquire)) {
            FrameDumpMetadata task{};
            {
                std::unique_lock<std::mutex> qlock(m_dump_queue_mutex);
                m_dump_cv.wait(qlock, [this]() {
                    return !m_dump_queue.empty() || !m_worker_running.load(std::memory_order_acquire);
                });
                if (!m_worker_running.load(std::memory_order_acquire) && m_dump_queue.empty()) {
                    break;
                }
                task = std::move(m_dump_queue.front());
                m_dump_queue.pop_front();
            }

            ProcessDumpTaskOnWorker(task);
        }
    }

    void ProcessDumpTaskOnWorker(const FrameDumpMetadata& task) {
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12Resource> mv8_buf;
        ComPtr<ID3D12Resource> color8_buf;
        ComPtr<ID3D12Resource> raw_mv4_buf;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            fence = m_readback_fence;
            if (task.readback_slot < kReadbackSlots) {
                mv8_buf     = m_readback_slots[task.readback_slot].mv8_buf;
                color8_buf  = m_readback_slots[task.readback_slot].color8_buf;
                raw_mv4_buf = m_readback_slots[task.readback_slot].raw_mv4_buf;
            }
        }

        auto release_slot = [this, &task]() {
            if (task.readback_slot < kReadbackSlots) {
                m_readback_slots[task.readback_slot].in_use.store(false, std::memory_order_release);
            }
            if (task.is_last_in_batch) {
                std::error_code ec;
                const fs::path trigger_path = m_plugin_dir / L"viple_mv_dump.txt";
                fs::remove(trigger_path, ec);
                m_dump_batch_in_flight.store(false, std::memory_order_release);
                LogInfo(
                    "[VIPLE-GAME-MV] Finished dumping batch (%u stereo game frames, skipped_incomplete=%u), "
                    "removed trigger file viple_mv_dump.txt",
                    task.dump_seq_index + 1u,
                    task.dump_skipped_incomplete);
            }
        };

        if (!fence || !mv8_buf || !color8_buf) {
            release_slot();
            return;
        }

        // 在背景執行緒等待 GPU 完成該幀的 Readback DMA
        if (fence->GetCompletedValue() < task.fence_value) {
            if (SUCCEEDED(fence->SetEventOnCompletion(task.fence_value, m_fence_event))) {
                WaitForSingleObject(m_fence_event, 2000);
            }
        }

        const fs::path dump_dir = m_plugin_dir / L"dump";
        std::error_code ec;
        fs::create_directories(dump_dir, ec);

        char base_name[64]{};
        std::snprintf(base_name, sizeof(base_name), "%04u", task.dump_seq_index);
        const std::string mv_bin_filename     = std::string("mv8_") + base_name + ".bin";
        const std::string mv_json_filename    = std::string("mv8_") + base_name + ".json";
        const std::string col_png_filename    = std::string("color8_") + base_name + ".png";
        const std::string raw_mv_bin_filename = std::string("mv4_raw_") + base_name + ".bin";

        const fs::path mv_bin_path     = dump_dir / mv_bin_filename;
        const fs::path mv_json_path    = dump_dir / mv_json_filename;
        const fs::path col_png_path    = dump_dir / col_png_filename;
        const fs::path raw_mv_bin_path = dump_dir / raw_mv_bin_filename;

        DepthGridStats sbs_depth_stats{};
        DepthGridStats eye_depth_stats[2]{};

        // 1. Map 並寫出 mv8_<frame>.bin（緊密排列 RGBA16F，每格 8 bytes），同時統計 1/8 深度 min/max/mean
        {
            const size_t row_bytes = static_cast<size_t>(task.sbs_grid_w) * 8u;
            const size_t total_mapped = static_cast<size_t>(task.mv8_row_pitch) * task.sbs_grid_h;
            D3D12_RANGE read_range{0, total_mapped};
            void* mapped = nullptr;
            if (SUCCEEDED(mv8_buf->Map(0, &read_range, &mapped)) && mapped != nullptr) {
                std::ofstream ofs(mv_bin_path, std::ios::binary | std::ios::trunc);
                const auto* src_bytes = static_cast<const uint8_t*>(mapped);

                float sbs_min = 1e30f;
                float sbs_max = -1e30f;
                double sbs_sum = 0.0;
                uint32_t sbs_valid = 0;
                uint32_t sbs_nonzero = 0;

                float eye_min[2] = {1e30f, 1e30f};
                float eye_max[2] = {-1e30f, -1e30f};
                double eye_sum[2] = {0.0, 0.0};
                uint32_t eye_valid[2] = {0, 0};
                uint32_t eye_nonzero[2] = {0, 0};

                for (uint32_t y = 0; y < task.sbs_grid_h; ++y) {
                    const uint8_t* row_ptr = src_bytes + static_cast<size_t>(y) * task.mv8_row_pitch;
                    ofs.write(
                        reinterpret_cast<const char*>(row_ptr),
                        static_cast<std::streamsize>(row_bytes));

                    for (uint32_t x = 0; x < task.sbs_grid_w; ++x) {
                        const auto* cell_u16 = reinterpret_cast<const uint16_t*>(row_ptr + static_cast<size_t>(x) * 8u);
                        const float inv_z = HalfToFloat(cell_u16[2]);
                        const float val   = HalfToFloat(cell_u16[3]);
                        if (val > 0.01f && std::isfinite(inv_z)) {
                            const int e = (x < task.eye_grid_w) ? 0 : 1;
                            sbs_min = std::min(sbs_min, inv_z);
                            sbs_max = std::max(sbs_max, inv_z);
                            sbs_sum += static_cast<double>(inv_z);
                            sbs_valid++;
                            if (inv_z > 1e-7f) {
                                sbs_nonzero++;
                            }

                            eye_min[e] = std::min(eye_min[e], inv_z);
                            eye_max[e] = std::max(eye_max[e], inv_z);
                            eye_sum[e] += static_cast<double>(inv_z);
                            eye_valid[e]++;
                            if (inv_z > 1e-7f) {
                                eye_nonzero[e]++;
                            }
                        }
                    }
                }
                D3D12_RANGE write_range{0, 0};
                mv8_buf->Unmap(0, &write_range);

                if (sbs_valid > 0) {
                    sbs_depth_stats.min_val       = sbs_min;
                    sbs_depth_stats.max_val       = sbs_max;
                    sbs_depth_stats.mean_val      = static_cast<float>(sbs_sum / static_cast<double>(sbs_valid));
                    sbs_depth_stats.valid_cells   = sbs_valid;
                    sbs_depth_stats.nonzero_cells = sbs_nonzero;
                }
                for (int e = 0; e < 2; ++e) {
                    if (eye_valid[e] > 0) {
                        eye_depth_stats[e].min_val       = eye_min[e];
                        eye_depth_stats[e].max_val       = eye_max[e];
                        eye_depth_stats[e].mean_val      = static_cast<float>(eye_sum[e] / static_cast<double>(eye_valid[e]));
                        eye_depth_stats[e].valid_cells   = eye_valid[e];
                        eye_depth_stats[e].nonzero_cells = eye_nonzero[e];
                    }
                }
            }
        }

        // 2. Map 並寫出 color8_<frame>.png（雙眼並排 1/8 RGBA8 Color 縮圖）
        {
            const size_t row_bytes = static_cast<size_t>(task.sbs_grid_w) * 4u;
            const size_t total_mapped = static_cast<size_t>(task.color8_row_pitch) * task.sbs_grid_h;
            D3D12_RANGE read_range{0, total_mapped};
            void* mapped = nullptr;
            if (SUCCEEDED(color8_buf->Map(0, &read_range, &mapped)) && mapped != nullptr) {
                std::vector<uint8_t> tight_rgba(row_bytes * task.sbs_grid_h);
                const auto* src_bytes = static_cast<const uint8_t*>(mapped);
                for (uint32_t y = 0; y < task.sbs_grid_h; ++y) {
                    std::memcpy(
                        tight_rgba.data() + static_cast<size_t>(y) * row_bytes,
                        src_bytes + static_cast<size_t>(y) * task.color8_row_pitch,
                        row_bytes);
                }
                D3D12_RANGE write_range{0, 0};
                color8_buf->Unmap(0, &write_range);

                WriteRgba8Png(col_png_path, task.sbs_grid_w, task.sbs_grid_h, tight_rgba.data());
            }
        }

        // 2b. 若啟用 raw 除錯傾印，Map 並寫出 mv4_raw_<frame>.bin（雙眼並排 1/4 原始 MV，RG32F，每格 8 bytes）
        bool raw_mv4_written = false;
        if (task.dump_raw_mv4 && raw_mv4_buf && task.raw_mv4_sbs_w > 0 && task.raw_mv4_sbs_h > 0) {
            const size_t row_bytes = static_cast<size_t>(task.raw_mv4_sbs_w) * 8u;
            const size_t total_mapped = static_cast<size_t>(task.raw_mv4_row_pitch) * task.raw_mv4_sbs_h;
            D3D12_RANGE read_range{0, total_mapped};
            void* mapped = nullptr;
            if (SUCCEEDED(raw_mv4_buf->Map(0, &read_range, &mapped)) && mapped != nullptr) {
                std::ofstream ofs(raw_mv_bin_path, std::ios::binary | std::ios::trunc);
                const auto* src_bytes = static_cast<const uint8_t*>(mapped);
                for (uint32_t y = 0; y < task.raw_mv4_sbs_h; ++y) {
                    const uint8_t* row_ptr = src_bytes + static_cast<size_t>(y) * task.raw_mv4_row_pitch;
                    ofs.write(
                        reinterpret_cast<const char*>(row_ptr),
                        static_cast<std::streamsize>(row_bytes));
                }
                D3D12_RANGE write_range{0, 0};
                raw_mv4_buf->Unmap(0, &write_range);
                raw_mv4_written = ofs.good();
            }
        }

        // 3. 寫出 mv8_<frame>.json（含尺寸、格式、mvScale、jitter、reset、subrect、raw_mv4、1/8 深度統計、分眼來源與相機矩陣）
        WriteDumpJson(
            mv_json_path,
            task,
            mv_bin_filename,
            col_png_filename,
            raw_mv4_written ? raw_mv_bin_filename : std::string(),
            sbs_depth_stats,
            eye_depth_stats);

        release_slot();
    }

    static void WriteMatrix4x4Json(std::ostream& os, const float m[4][4], const char* indent) {
        os << "[\n";
        for (int r = 0; r < 4; ++r) {
            os << indent << "  ["
               << m[r][0] << ", " << m[r][1] << ", " << m[r][2] << ", " << m[r][3]
               << "]" << (r < 3 ? "," : "") << "\n";
        }
        os << indent << "]";
    }

    static void WriteDepthStatsJson(std::ostream& os, const DepthGridStats& ds, const char* indent) {
        os << "{\n";
        os << indent << "  \"min\": " << ds.min_val << ",\n";
        os << indent << "  \"max\": " << ds.max_val << ",\n";
        os << indent << "  \"mean\": " << ds.mean_val << ",\n";
        os << indent << "  \"valid_cells\": " << ds.valid_cells << ",\n";
        os << indent << "  \"nonzero_cells\": " << ds.nonzero_cells << "\n";
        os << indent << "}";
    }

    static std::string FormatPtrHex(uintptr_t ptr) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(ptr));
        return std::string(buf);
    }

    static void WriteDumpJson(
        const fs::path& json_path,
        const FrameDumpMetadata& task,
        const std::string& mv_bin_filename,
        const std::string& col_png_filename,
        const std::string& raw_mv_bin_filename,
        const DepthGridStats& sbs_depth_stats,
        const DepthGridStats eye_depth_stats[2]) {
        std::ofstream ofs(json_path, std::ios::trunc);
        if (!ofs) {
            return;
        }
        ofs << "{\n";
        ofs << "  \"frame_seq\": " << task.dump_seq_index << ",\n";
        ofs << "  \"game_frame_index\": " << task.game_frame_index << ",\n";
        ofs << "  \"present_frame_index\": " << task.present_frame_index << ",\n";
        ofs << "  \"dump_skipped_incomplete_frames\": " << task.dump_skipped_incomplete << ",\n";
        ofs << "  \"format\": \"RGBA16F\",\n";
        ofs << "  \"channel_layout\": \"xy=eye_uv_mv(uv_prev-uv_cur), z=inv_depth, w=validity\",\n";
        ofs << "  \"eye_layout\": \"left_half=left_eye, right_half=right_eye\",\n";
        ofs << "  \"sbs_grid_w\": " << task.sbs_grid_w << ",\n";
        ofs << "  \"sbs_grid_h\": " << task.sbs_grid_h << ",\n";
        ofs << "  \"eye_grid_w\": " << task.eye_grid_w << ",\n";
        ofs << "  \"eye_grid_h\": " << task.eye_grid_h << ",\n";
        ofs << "  \"mv_bin_file\": \"" << mv_bin_filename << "\",\n";
        ofs << "  \"color_file\": \"" << col_png_filename << "\",\n";
        ofs << "  \"raw_mv4_enabled\": " << (!raw_mv_bin_filename.empty() ? "true" : "false") << ",\n";
        if (!raw_mv_bin_filename.empty()) {
            ofs << "  \"raw_mv4_bin_file\": \"" << raw_mv_bin_filename << "\",\n";
            ofs << "  \"raw_mv4_format\": \"RG32F\",\n";
            ofs << "  \"raw_mv4_channel_layout\": \"xy=raw_mv_tex_value_4x4_box_avg\",\n";
            ofs << "  \"raw_mv4_eye_grid_w\": " << task.raw_mv4_eye_w << ",\n";
            ofs << "  \"raw_mv4_eye_grid_h\": " << task.raw_mv4_eye_h << ",\n";
            ofs << "  \"raw_mv4_sbs_grid_w\": " << task.raw_mv4_sbs_w << ",\n";
            ofs << "  \"raw_mv4_sbs_grid_h\": " << task.raw_mv4_sbs_h << ",\n";
        }
        ofs << "  \"depth_stats\": ";
        WriteDepthStatsJson(ofs, sbs_depth_stats, "  ");
        ofs << ",\n";

        ofs << "  \"hmd_pose\": {\n";
        ofs << "    \"valid\": " << (task.hmd_pose_valid ? "true" : "false") << ",\n";
        ofs << "    \"position\": [" << task.hmd_pos[0] << ", " << task.hmd_pos[1] << ", " << task.hmd_pos[2] << "],\n";
        ofs << "    \"rotation_quat_wxyz\": ["
            << task.hmd_quat[0] << ", " << task.hmd_quat[1] << ", "
            << task.hmd_quat[2] << ", " << task.hmd_quat[3] << "],\n";
        ofs << "    \"transform\": ";
        WriteMatrix4x4Json(ofs, task.hmd_transform, "    ");
        ofs << "\n  },\n";

        ofs << "  \"eyes\": {\n";
        const char* eye_names[2] = {"left", "right"};
        for (int e = 0; e < 2; ++e) {
            const EyeEvalMetadata& em = task.eyes[e];
            const EyeSubrectMetadata& sr = em.subrect;
            const uint32_t eff_subrect_w = (sr.render_w > 0) ? sr.render_w : em.render_w;
            const uint32_t eff_subrect_h = (sr.render_h > 0) ? sr.render_h : em.render_h;
            ofs << "    \"" << eye_names[e] << "\": {\n";
            ofs << "      \"valid\": " << (em.valid ? "true" : "false") << ",\n";
            ofs << "      \"eye_index\": " << em.eye_index << ",\n";
            ofs << "      \"eye_source\": \"" << em.eye_source << "\",\n";
            ofs << "      \"cross_check_offset_eye\": " << em.cross_check_offset_eye << ",\n";
            ofs << "      \"feature_id\": " << em.feature_id << ",\n";
            ofs << "      \"handle_id\": " << em.handle_id << ",\n";
            ofs << "      \"create_flags\": " << em.create_flags << ",\n";
            ofs << "      \"render_dim\": [" << em.render_w << ", " << em.render_h << "],\n";
            ofs << "      \"render_subrect\": [" << eff_subrect_w << ", " << eff_subrect_h << "],\n";
            ofs << "      \"subrect\": {\n";
            ofs << "        \"DLSS.Input.Color.Subrect.Base.X\": " << sr.color_base_x << ",\n";
            ofs << "        \"DLSS.Input.Color.Subrect.Base.Y\": " << sr.color_base_y << ",\n";
            ofs << "        \"DLSS.Input.Depth.Subrect.Base.X\": " << sr.depth_base_x << ",\n";
            ofs << "        \"DLSS.Input.Depth.Subrect.Base.Y\": " << sr.depth_base_y << ",\n";
            ofs << "        \"DLSS.Input.MV.Subrect.Base.X\": " << sr.mv_base_x << ",\n";
            ofs << "        \"DLSS.Input.MV.Subrect.Base.Y\": " << sr.mv_base_y << ",\n";
            ofs << "        \"DLSS.Output.Subrect.Base.X\": " << sr.output_base_x << ",\n";
            ofs << "        \"DLSS.Output.Subrect.Base.Y\": " << sr.output_base_y << ",\n";
            ofs << "        \"DLSS.Render.Subrect.Dimensions.Width\": " << sr.render_w << ",\n";
            ofs << "        \"DLSS.Render.Subrect.Dimensions.Height\": " << sr.render_h << ",\n";
            ofs << "        \"color_base\": [" << sr.color_base_x << ", " << sr.color_base_y << "],\n";
            ofs << "        \"depth_base\": [" << sr.depth_base_x << ", " << sr.depth_base_y << "],\n";
            ofs << "        \"mv_base\": [" << sr.mv_base_x << ", " << sr.mv_base_y << "],\n";
            ofs << "        \"output_base\": [" << sr.output_base_x << ", " << sr.output_base_y << "],\n";
            ofs << "        \"render_dimensions\": [" << eff_subrect_w << ", " << eff_subrect_h << "],\n";
            ofs << "        \"present\": {\n";
            ofs << "          \"color_base_x\": " << (sr.has_color_base_x ? "true" : "false") << ",\n";
            ofs << "          \"color_base_y\": " << (sr.has_color_base_y ? "true" : "false") << ",\n";
            ofs << "          \"depth_base_x\": " << (sr.has_depth_base_x ? "true" : "false") << ",\n";
            ofs << "          \"depth_base_y\": " << (sr.has_depth_base_y ? "true" : "false") << ",\n";
            ofs << "          \"mv_base_x\": " << (sr.has_mv_base_x ? "true" : "false") << ",\n";
            ofs << "          \"mv_base_y\": " << (sr.has_mv_base_y ? "true" : "false") << ",\n";
            ofs << "          \"output_base_x\": " << (sr.has_output_base_x ? "true" : "false") << ",\n";
            ofs << "          \"output_base_y\": " << (sr.has_output_base_y ? "true" : "false") << ",\n";
            ofs << "          \"render_w\": " << (sr.has_render_w ? "true" : "false") << ",\n";
            ofs << "          \"render_h\": " << (sr.has_render_h ? "true" : "false") << "\n";
            ofs << "        }\n";
            ofs << "      },\n";
            ofs << "      \"mv_valid_dim\": [" << em.mv_valid_w << ", " << em.mv_valid_h << "],\n";
            ofs << "      \"base_dim\": [" << em.base_w << ", " << em.base_h << "],\n";
            ofs << "      \"mv_tex_dim\": [" << em.mv_tex_w << ", " << em.mv_tex_h << "],\n";
            ofs << "      \"mv_format\": " << em.mv_format << ",\n";
            ofs << "      \"depth_tex_dim\": [" << em.depth_tex_w << ", " << em.depth_tex_h << "],\n";
            ofs << "      \"depth_format\": " << em.depth_format << ",\n";
            ofs << "      \"color_tex_dim\": [" << em.color_tex_w << ", " << em.color_tex_h << "],\n";
            ofs << "      \"color_format\": " << em.color_format << ",\n";
            ofs << "      \"mv_res_ptr\": \"" << FormatPtrHex(em.mv_res_ptr) << "\",\n";
            ofs << "      \"depth_res_ptr\": \"" << FormatPtrHex(em.depth_res_ptr) << "\",\n";
            ofs << "      \"color_res_ptr\": \"" << FormatPtrHex(em.color_res_ptr) << "\",\n";
            ofs << "      \"mv_scale\": [" << em.mv_scale_x << ", " << em.mv_scale_y << "],\n";
            ofs << "      \"mv_scale_to_uv\": [" << em.mv_scale_to_uv_x << ", " << em.mv_scale_to_uv_y << "],\n";
            ofs << "      \"jitter_offset\": [" << em.jitter_x << ", " << em.jitter_y << "],\n";
            ofs << "      \"jitter_delta_uv\": [" << em.jitter_delta_uv_x << ", " << em.jitter_delta_uv_y << "],\n";
            ofs << "      \"depth_ref\": " << em.depth_ref << ",\n";
            ofs << "      \"reset\": " << em.reset << ",\n";
            ofs << "      \"depth_stats\": ";
            WriteDepthStatsJson(ofs, eye_depth_stats[e], "      ");
            ofs << ",\n";
            ofs << "      \"camera\": {\n";
            ofs << "        \"has_view_offset\": " << (em.cam.has_view_offset ? "true" : "false") << ",\n";
            ofs << "        \"world_to_meters\": " << em.cam.world_to_meters << ",\n";
            ofs << "        \"view_offset_pos\": ["
                << em.cam.view_offset_pos[0] << ", "
                << em.cam.view_offset_pos[1] << ", "
                << em.cam.view_offset_pos[2] << "],\n";
            ofs << "        \"view_offset_rot_pyr\": ["
                << em.cam.view_offset_rot[0] << ", "
                << em.cam.view_offset_rot[1] << ", "
                << em.cam.view_offset_rot[2] << "],\n";
            ofs << "        \"vr_eye_offset\": ["
                << em.cam.eye_offset[0] << ", "
                << em.cam.eye_offset[1] << ", "
                << em.cam.eye_offset[2] << "],\n";
            ofs << "        \"has_vr_proj\": " << (em.cam.has_vr_proj ? "true" : "false") << ",\n";
            ofs << "        \"ue_projection_matrix\": ";
            WriteMatrix4x4Json(ofs, em.cam.ue_proj, "        ");
            ofs << "\n      }\n";
            ofs << "    }" << (e == 0 ? "," : "") << "\n";
        }
        ofs << "  }\n";
        ofs << "}\n";
    }

    // ========================================================================
    // NGX 模組偵測與 UEVR register_inline_hook 掛載
    // ========================================================================
    void TryInstallNgxHooks();

    void ResolvePluginDirectory() {
        if (!m_plugin_dir.empty()) {
            return;
        }
        HMODULE self_mod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&uevr_plugin_initialize),
                &self_mod) &&
            self_mod != nullptr) {
            wchar_t path_buf[MAX_PATH * 2]{};
            DWORD len = GetModuleFileNameW(self_mod, path_buf, static_cast<DWORD>(std::size(path_buf)));
            if (len > 0) {
                m_plugin_dir = fs::path(path_buf).parent_path();
            }
        }
        if (m_plugin_dir.empty()) {
            m_plugin_dir = fs::current_path();
        }
    }

    template <typename... Args>
    static void LogInfo(const char* fmt, Args... args) {
        try {
            uevr::API::get()->log_info(fmt, args...);
        } catch (...) {
        }
    }

    template <typename... Args>
    static void LogWarn(const char* fmt, Args... args) {
        try {
            uevr::API::get()->log_warn(fmt, args...);
        } catch (...) {
        }
    }

    template <typename... Args>
    static void LogError(const char* fmt, Args... args) {
        try {
            uevr::API::get()->log_error(fmt, args...);
        } catch (...) {
        }
    }

private:
    std::mutex m_mutex;
    fs::path m_plugin_dir;

    std::atomic<bool> m_ngx_hooked{false};
    int m_hook_id_create{-1};
    int m_hook_id_release{-1};
    int m_hook_id_evaluate{-1};

    PFN_NVSDK_NGX_D3D12_CreateFeature   m_orig_create_feature{nullptr};
    PFN_NVSDK_NGX_D3D12_ReleaseFeature  m_orig_release_feature{nullptr};
    PFN_NVSDK_NGX_D3D12_EvaluateFeature m_orig_evaluate_feature{nullptr};

    std::unordered_map<const NVSDK_NGX_Handle*, TrackedFeatureInfo> m_tracked_features;
    uint64_t m_feature_create_seq{0};
    unsigned int m_paired_handle_ids[2]{0, 0};

    // 分眼判定、遊戲幀雙眼分組與相機狀態
    bool m_view_index_observed_zero{false};
    bool m_view_index_observed_two{false};
    std::deque<int> m_stereo_view_queue;
    EyeCameraPose m_latest_eye_cam[2]{};
    EyeSubrectMetadata m_last_subrect[2]{};
    bool m_subrect_logged[2]{false, false};
    bool m_group_eye_ready[2]{false, false};
    EyeEvalMetadata m_group_eye_meta[2]{};

    // 600 遊戲幀統計計數器
    uint64_t m_game_frame_index{0};
    uint64_t m_present_count{0};
    uint32_t m_stats_window_game_frames{0};
    uint32_t m_stats_window_presents{0};
    uint32_t m_stats_left_calls{0};
    uint32_t m_stats_right_calls{0};
    uint32_t m_stats_complete_game_frames{0};
    uint32_t m_stats_incomplete_game_frames{0};
    uint32_t m_stats_offset_match{0};
    uint32_t m_stats_offset_mismatch{0};
    uint32_t m_stats_offset_missing{0};
    float    m_last_mv_scale_x{1.0f};
    float    m_last_mv_scale_y{1.0f};
    uint32_t m_last_render_w{0};
    uint32_t m_last_render_h{0};

    // D3D12 Compute Pipeline 與輸出貼圖
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_root_sig;
    ComPtr<ID3D12PipelineState>       m_pso;
    ComPtr<ID3D12PipelineState>       m_pso_raw_mv4;
    ComPtr<ID3D12DescriptorHeap>      m_srv_uav_heap;
    uint32_t                          m_srv_uav_desc_size{0};
    uint32_t                          m_desc_ring_cursor{0};
    ComPtr<ID3D12Resource>            m_dummy_tex;
    ComPtr<ID3D12Resource>            m_out_mv8_tex;
    ComPtr<ID3D12Resource>            m_out_color8_tex;
    ComPtr<ID3D12Resource>            m_out_raw_mv4_tex;
    uint32_t                          m_eye_grid_w{0};
    uint32_t                          m_eye_grid_h{0};
    uint32_t                          m_raw_mv4_eye_w{0};
    uint32_t                          m_raw_mv4_eye_h{0};
    bool                              m_default_reverse_z{true};

    // 輸入資源預期狀態（依 NGX D3D12 規範預設為 NON_PIXEL_SHADER_RESOURCE）
    D3D12_RESOURCE_STATES m_assumed_mv_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES m_assumed_depth_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES m_assumed_color_state{D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};

    // 非同步 Readback 傾印資源與背景執行緒
    ComPtr<ID3D12Fence>               m_readback_fence;
    uint64_t                          m_fence_counter{0};
    std::array<ReadbackSlotResources, kReadbackSlots> m_readback_slots;
    uint32_t                          m_readback_cursor{0};
    std::deque<FrameDumpMetadata>     m_staged_dump_tasks;

    uint32_t                          m_dump_remaining_frames{0};
    bool                              m_dump_raw_mv4{false};
    uint32_t                          m_dump_seq_counter{0};
    uint32_t                          m_dump_skipped_incomplete{0};
    std::atomic<bool>                 m_dump_batch_in_flight{false};

    std::atomic<bool>                 m_worker_running{false};
    HANDLE                            m_fence_event{nullptr};
    std::mutex                        m_dump_queue_mutex;
    std::condition_variable           m_dump_cv;
    std::deque<FrameDumpMetadata>     m_dump_queue;
    std::thread                       m_dump_worker;

    // M-MV1 跨行程 IPC、Readback 環狀緩衝（VRMV_SLOTS 個）、Publish 執行緒與統計狀態
    int64_t                           m_qpc_freq{10000000ll};
    uint32_t                          m_self_pid{0};
    ComPtr<ID3D12Fence>               m_ipc_fence;
    static constexpr uint32_t kIpcSkipCodes = 10;
    std::atomic<uint32_t> m_ipc_skip_logged[kIpcSkipCodes]{};
    uint64_t                          m_ipc_fence_counter{0};
    std::array<IpcReadbackSlot, VRMV_SLOTS> m_ipc_rb_slots;
    uint32_t                          m_ipc_rb_cursor{0};
    std::deque<IpcPublishTask>        m_staged_ipc_tasks;

    std::atomic<bool>                 m_ipc_running{false};
    std::atomic<bool>                 m_ipc_connected{false};
    std::atomic<uint64_t>             m_ipc_current_gen{0};
    HANDLE                            m_ipc_stop_event{nullptr};
    HANDLE                            m_ipc_fence_event{nullptr};
    std::mutex                        m_ipc_session_mutex;
    std::shared_ptr<IpcMappedSession> m_ipc_session;
    std::mutex                        m_ipc_queue_mutex;
    std::condition_variable           m_ipc_publish_cv;
    std::deque<IpcPublishTask>        m_ipc_publish_queue;
    std::thread                       m_ipc_conn_thread;
    std::thread                       m_ipc_publish_thread;

    // M-MV1 延遲注入（viple_mv_delay.txt）與計數器
    int64_t                           m_next_delay_check_qpc{0};
    std::atomic<uint32_t>             m_ipc_injected_delay_ms{0};
    std::atomic<uint64_t>             m_ipc_frames_published{0};
    std::atomic<uint64_t>             m_ipc_frames_dropped_gpu_late{0};
    std::atomic<uint64_t>             m_ipc_frames_dropped_incomplete{0};
    std::atomic<uint32_t>             m_ipc_window_published{0};
    std::atomic<uint32_t>             m_ipc_window_dropped_gpu_late{0};
    std::atomic<uint32_t>             m_ipc_window_dropped_incomplete{0};
    std::atomic<uint32_t>             m_ipc_window_dropped_rb_full{0};
    std::mutex                        m_ipc_stats_mutex;
    std::vector<double>               m_win_gpu_to_publish_us;
    std::vector<double>               m_win_present_to_publish_us;
};

// 全域外掛實例（建構時自動註冊至 uevr::detail::g_plugin）
static VipleUevrMvPlugin g_viple_mv_plugin;

// ============================================================================
// NGX C-ABI Hook Detour 函式
// ============================================================================
namespace {

NVSDK_NGX_Result NVSDK_CONV Hook_NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    NVSDK_NGX_Feature InFeatureID,
    NVSDK_NGX_Parameter* InParameters,
    NVSDK_NGX_Handle** OutHandle) {
    return g_viple_mv_plugin.HandleCreateFeature(InCmdList, InFeatureID, InParameters, OutHandle);
}

NVSDK_NGX_Result NVSDK_CONV Hook_NVSDK_NGX_D3D12_ReleaseFeature(
    NVSDK_NGX_Handle* InHandle) {
    return g_viple_mv_plugin.HandleReleaseFeature(InHandle);
}

NVSDK_NGX_Result NVSDK_CONV Hook_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    const NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback) {
    return g_viple_mv_plugin.HandleEvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback);
}

NVSDK_NGX_Result NVSDK_CONV Hook_NVSDK_NGX_D3D12_EvaluateFeature_C(
    ID3D12GraphicsCommandList* InCmdList,
    const NVSDK_NGX_Handle* InFeatureHandle,
    const NVSDK_NGX_Parameter* InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback) {
    return g_viple_mv_plugin.HandleEvaluateFeature(InCmdList, InFeatureHandle, InParameters, InCallback, true);
}

} // namespace

void VipleUevrMvPlugin::TryInstallNgxHooks() {
    if (m_ngx_hooked.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ngx_hooked.load(std::memory_order_relaxed)) {
        return;
    }

    const auto* param = uevr::API::get()->param();
    if (param == nullptr || param->functions == nullptr ||
        param->functions->register_inline_hook == nullptr) {
        return;
    }

    // 確認目前是否為 D3D12 算繪器
    if (param->renderer != nullptr &&
        param->renderer->renderer_type != UEVR_RENDERER_D3D12) {
        return;
    }

    // 依序探測已載入的 _nvngx.dll 或 nvngx.dll
    const wchar_t* kCandidateModules[] = {
        L"_nvngx.dll",
        L"nvngx.dll",
    };

    HMODULE ngx_mod = nullptr;
    const wchar_t* loaded_mod_name = nullptr;
    for (const wchar_t* mod_name : kCandidateModules) {
        ngx_mod = GetModuleHandleW(mod_name);
        if (ngx_mod != nullptr) {
            loaded_mod_name = mod_name;
            break;
        }
    }

    if (ngx_mod == nullptr) {
        return;
    }

    void* fn_create   = reinterpret_cast<void*>(GetProcAddress(ngx_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    void* fn_release  = reinterpret_cast<void*>(GetProcAddress(ngx_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    void* fn_evaluate = reinterpret_cast<void*>(GetProcAddress(ngx_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));

    if (fn_create == nullptr || fn_release == nullptr || fn_evaluate == nullptr) {
        LogWarn("[VIPLE-GAME-MV] Found NGX module but D3D12 exports are missing (Create=%p, Release=%p, Evaluate=%p)",
                fn_create, fn_release, fn_evaluate);
        return;
    }

    auto reg_hook = param->functions->register_inline_hook;
    m_hook_id_create = reg_hook(
        fn_create,
        reinterpret_cast<void*>(&Hook_NVSDK_NGX_D3D12_CreateFeature),
        reinterpret_cast<void**>(&m_orig_create_feature));

    m_hook_id_release = reg_hook(
        fn_release,
        reinterpret_cast<void*>(&Hook_NVSDK_NGX_D3D12_ReleaseFeature),
        reinterpret_cast<void**>(&m_orig_release_feature));

    m_hook_id_evaluate = reg_hook(
        fn_evaluate,
        reinterpret_cast<void*>(&Hook_NVSDK_NGX_D3D12_EvaluateFeature),
        reinterpret_cast<void**>(&m_orig_evaluate_feature));

    if (m_orig_create_feature == nullptr ||
        m_orig_release_feature == nullptr ||
        m_orig_evaluate_feature == nullptr) {
        LogError("[VIPLE-GAME-MV] register_inline_hook failed (ids: %d, %d, %d)",
                 m_hook_id_create, m_hook_id_release, m_hook_id_evaluate);
        return;
    }

    // _C 版是可選的：模組有匯出才掛，掛不上不影響非 _C 版
    if (void* fn_evaluate_c = reinterpret_cast<void*>(GetProcAddress(ngx_mod, "NVSDK_NGX_D3D12_EvaluateFeature_C"))) {
        m_hook_id_evaluate_c = reg_hook(
            fn_evaluate_c,
            reinterpret_cast<void*>(&Hook_NVSDK_NGX_D3D12_EvaluateFeature_C),
            reinterpret_cast<void**>(&m_orig_evaluate_feature_c));
        if (m_orig_evaluate_feature_c == nullptr) {
            LogWarn("[VIPLE-GAME-MV] EvaluateFeature_C exported but hook failed (id=%d)", m_hook_id_evaluate_c);
        }
        else {
            LogInfo("[VIPLE-GAME-MV] hooked EvaluateFeature_C=%p (id=%d)", fn_evaluate_c, m_hook_id_evaluate_c);
        }
    }
    else {
        LogInfo("[VIPLE-GAME-MV] %ls does not export EvaluateFeature_C", loaded_mod_name);
    }

    m_ngx_hooked.store(true, std::memory_order_release);
    LogInfo(
        "[VIPLE-GAME-MV] Successfully hooked NGX D3D12 (%ls): "
        "CreateFeature=%p (id=%d), ReleaseFeature=%p (id=%d), EvaluateFeature=%p (id=%d)",
        loaded_mod_name,
        fn_create, m_hook_id_create,
        fn_release, m_hook_id_release,
        fn_evaluate, m_hook_id_evaluate);
}
