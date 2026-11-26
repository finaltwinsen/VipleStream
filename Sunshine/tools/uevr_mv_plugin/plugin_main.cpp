// ============================================================================
// plugin_main.cpp
// VipleStream §VR-GAME-MV (M-MV0) — UEVR 遊戲內動態向量與深度擷取／降採樣／傾印外掛
//
// 功能摘要：
//   1. 僅依賴 third_party/uevr/ 下的三個 MIT 標頭（API.h、API.hpp、Plugin.hpp），
//      匯出 uevr_plugin_required_version 與 uevr_plugin_initialize。
//   2. 透過 UEVR 的 register_inline_hook（或動態模組解析）攔截 _nvngx.dll / nvngx.dll 的
//      NVSDK_NGX_D3D12_CreateFeature、ReleaseFeature、EvaluateFeature，
//      僅處理 FeatureID 1 (SuperSampling) 與 13 (RayReconstruction)。
//   3. 於 EvaluateFeature 透過 NVSDK_NGX_Parameter 虛擬函式表讀取 MotionVectors、Depth、
//      Color、MV.Scale.X/Y、Jitter.Offset.X/Y、Reset、DLSS.Render.Subrect.Dimensions.Width/Height。
//   4. 分眼判定：首選 on_post_calculate_stereo_view_offset 的 view_index 佇列，
//      次選每次 on_present 歸零的幀內呼叫序號（絕不使用 render_frame_count % 2）。
//   5. 在遊戲傳入的 ID3D12GraphicsCommandList 上派發 cs_downsample_mv_8x8（前景優先池化），
//      輸出 1/8 解析度雙眼並排 RGBA16F（xy=單眼 UV 位移、z=逆深度、w=有效度），並確保派發前後
//      將輸入資源狀態還原。
//   6. 當外掛目錄存在 viple_mv_dump.txt（內容為幀數 N）時，透過 D3D12 Readback Heap + Fence +
//      背景執行緒非同步傾印連續 N 幀的 mv8_<frame>.bin、mv8_<frame>.json、color8_<frame>.png，
//      全部寫完後刪除 viple_mv_dump.txt。
//   7. 使用 UEVR log 函式（標籤 [VIPLE-GAME-MV]）記錄攔截狀態與每 600 幀統計。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
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
    uint   g_Reserved;
};

Texture2D<float2>         g_MvTex     : register(t0);
Texture2D<float>          g_DepthTex  : register(t1);
Texture2D<float4>         g_ColorTex  : register(t2);

RWTexture2D<float4>       g_OutMv8    : register(u0);
RWTexture2D<unorm float4> g_OutColor8 : register(u1);

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
    uint2 mvPx = gid.xy * 8u + gtid.xy;
    bool inBounds = (mvPx.x < g_MvValidDim.x) && (mvPx.y < g_MvValidDim.y);

    uint2 clampedMvPx = min(mvPx, max(g_MvValidDim, uint2(1u, 1u)) - 1u);
    float2 rawMv = g_MvTex.Load(int3(clampedMvPx, 0)).xy;
    float2 uvMv = rawMv * g_MvScaleToUv - g_JitterDeltaUv;

    bool isFiniteMv = !any(isnan(uvMv)) && !any(isinf(uvMv)) &&
                      (abs(uvMv.x) <= 2.0f) && (abs(uvMv.y) <= 2.0f);

    float invZ = 0.5f;
    bool hasDepth = (g_Flags & 1u) != 0u;
    if (hasDepth)
    {
        uint2 depthPx = min(mvPx, max(g_DepthValidDim, uint2(1u, 1u)) - 1u);
        float rawD = g_DepthTex.Load(int3(depthPx, 0)).x;
        if (!isnan(rawD) && !isinf(rawD))
        {
            bool depthInverted = (g_Flags & 2u) != 0u;
            invZ = depthInverted ? saturate(rawD) : saturate(1.0f - rawD);
        }
    }

    s_Mv[gi]    = (inBounds && isFiniteMv) ? uvMv : float2(0.0f, 0.0f);
    s_InvZ[gi]  = (inBounds && isFiniteMv) ? invZ : -1.0f;
    s_Valid[gi] = (inBounds && isFiniteMv) ? 1u : 0u;

    bool dumpColor = (g_Flags & 8u) != 0u;
    if (dumpColor)
    {
        uint2 colPx = min(mvPx, max(g_ColorValidDim, uint2(1u, 1u)) - 1u);
        float3 col = g_ColorTex.Load(int3(colPx, 0)).rgb;
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
        float eps = max(g_DepthEpsilon, zMax * 0.05f);
        float threshold = hasDepth ? (zMax - eps) : -1.0f;

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
            if (s_Valid[c] != 0u)
            {
                sumCol += s_Color[c];
                colCount += 1u;
            }
        }
        float3 avgCol = (colCount > 0u) ? (sumCol / float(colCount)) : float3(0.0f, 0.0f, 0.0f);
        g_OutColor8[outCoord] = float4(saturate(avgCol), 1.0f);
    }
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

struct EyeEvalMetadata {
    bool valid{false};
    int eye_index{0}; // 0=Left, 1=Right
    std::string eye_source{"none"};
    int feature_id{0};
    unsigned int handle_id{0};
    int create_flags{0};
    uint32_t render_w{0};
    uint32_t render_h{0};
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
    float mv_scale_x{1.0f};
    float mv_scale_y{1.0f};
    float mv_scale_to_uv_x{0.0f};
    float mv_scale_to_uv_y{0.0f};
    float jitter_x{0.0f};
    float jitter_y{0.0f};
    float jitter_delta_uv_x{0.0f};
    float jitter_delta_uv_y{0.0f};
    int reset{0};
    EyeCameraPose cam;
};

struct FrameDumpMetadata {
    uint64_t present_frame_index{0};
    uint32_t dump_seq_index{0};
    bool is_last_in_batch{false};
    uint32_t eye_grid_w{0};
    uint32_t eye_grid_h{0};
    uint32_t sbs_grid_w{0};
    uint32_t sbs_grid_h{0};
    uint32_t mv8_row_pitch{0};
    uint32_t color8_row_pitch{0};
    uint32_t readback_slot{0};
    uint64_t fence_value{0};

    EyeEvalMetadata eyes[2];

    bool hmd_pose_valid{false};
    float hmd_pos[3]{0.0f, 0.0f, 0.0f};
    float hmd_quat[4]{1.0f, 0.0f, 0.0f, 0.0f}; // w, x, y, z
    float hmd_transform[4][4]{};
};

// Compute Shader 根常數結構（16 個 32-bit DWORD = 64 bytes）
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
    uint32_t reserved;
};
static_assert(sizeof(DownsampleRootConstants) == 64, "DownsampleRootConstants must be 64 bytes (16 DWORDs)");

constexpr uint32_t kDescRingSlots = 64u;
constexpr uint32_t kDescsPerSlot  = 8u; // 0..2: SRV (t0..t2), 3..4: UAV (u0..u1)
constexpr uint32_t kReadbackSlots = 8u;

struct ReadbackSlotResources {
    ComPtr<ID3D12Resource> mv8_buf;
    ComPtr<ID3D12Resource> color8_buf;
    uint64_t allocated_mv8_bytes{0};
    uint64_t allocated_color8_bytes{0};
    std::atomic<bool> in_use{false};
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
        LogInfo("[VIPLE-GAME-MV] Initializing viple_uevr_mv_plugin (M-MV0) in dir: %s",
                m_plugin_dir.u8string().c_str());

        StartDumpWorker();
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

        if (view_index == 0) {
            m_view_index_observed_zero = true;
        }
        int eye = 0;
        if (m_view_index_observed_zero) {
            eye = (view_index >= 0) ? (view_index % 2) : 0;
        } else {
            // 若引擎從 1 起算 (1=Left, 2=Right)
            eye = (view_index >= 1) ? ((view_index - 1) % 2) : 0;
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
    // on_present：每幀結算、觸發非同步傾印 Fence Signal、每 600 幀輸出統計 log
    // ------------------------------------------------------------------------
    void on_present() override {
        if (!m_ngx_hooked.load(std::memory_order_acquire)) {
            TryInstallNgxHooks();
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // 若本幀有實際進行過 EvaluateFeature 且處於傾印模式，在 CommandQueue 上 Signal Fence
        // 交由背景執行緒等待與寫檔，絕不在遊戲執行緒上阻塞超過一幀
        if (m_frame_dump_copied && m_dump_remaining_frames > 0) {
            SubmitAsyncDumpOnPresentLocked();
        }

        // 更新 600 幀統計數據
        m_present_count++;
        m_stats_window_presents++;
        if (m_frame_eye_calls[0] > 0 && m_frame_eye_calls[1] > 0) {
            m_stats_both_eyes_presents++;
        }

        if (m_stats_window_presents >= 600) {
            const double inv_p = 1.0 / static_cast<double>(m_stats_window_presents);
            const char* dominant_src =
                (m_stats_src_offset >= m_stats_src_ordinal)
                    ? ((m_stats_src_offset > 0) ? "stereo_view_offset" : "none")
                    : "frame_call_ordinal";

            LogInfo(
                "[VIPLE-GAME-MV] 600-frame stats (total_presents=%llu): "
                "calls_per_present=(L:%.2f, R:%.2f), stereo_frames=%u/%u, "
                "eye_source=%s (offset=%u, ordinal=%u), mvScale=(%.4f, %.4f), "
                "render=(%ux%u)->sbs_grid=(%ux%u)",
                static_cast<unsigned long long>(m_present_count),
                static_cast<double>(m_stats_left_calls) * inv_p,
                static_cast<double>(m_stats_right_calls) * inv_p,
                m_stats_both_eyes_presents,
                m_stats_window_presents,
                dominant_src,
                m_stats_src_offset,
                m_stats_src_ordinal,
                m_last_mv_scale_x,
                m_last_mv_scale_y,
                m_last_render_w,
                m_last_render_h,
                m_eye_grid_w * 2u,
                m_eye_grid_h);
            LogInfo("[VIPLE-GAME-MV] eval entry calls (cumulative): plain=%llu c=%llu",
                    static_cast<unsigned long long>(m_eval_calls_plain.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(m_eval_calls_c.load(std::memory_order_relaxed)));

            m_stats_window_presents = 0;
            m_stats_left_calls = 0;
            m_stats_right_calls = 0;
            m_stats_both_eyes_presents = 0;
            m_stats_src_offset = 0;
            m_stats_src_ordinal = 0;
        }

        // 重置幀內呼叫序號與單幀暫存狀態（設計 2.2 節：每次 on_present 歸零）
        m_frame_eval_ordinal = 0;
        m_frame_eye_calls[0] = 0;
        m_frame_eye_calls[1] = 0;
        m_frame_eye_meta[0] = EyeEvalMetadata{};
        m_frame_eye_meta[1] = EyeEvalMetadata{};
        m_frame_dump_copied = false;
        m_stereo_view_queue.clear();

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

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tracked_features[*out_handle] = info;
        }

        LogInfo(
            "[VIPLE-GAME-MV] Tracked NGX CreateFeature: FeatureID=%d, Handle=%p (Id=%u), "
            "Flags=0x%x, In=(%ux%u), Out=(%ux%u)",
            static_cast<int>(feature_id),
            static_cast<const void*>(*out_handle),
            info.handle_id,
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
                LogInfo("[VIPLE-GAME-MV] Releasing tracked NGX Handle=%p (Id=%u, FeatureID=%d)",
                        static_cast<const void*>(handle),
                        it->second.handle_id,
                        static_cast<int>(it->second.feature_id));
                m_tracked_features.erase(it);
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
    // EvaluateFeature 擷取、分眼判定與 D3D12 Compute Shader 派發
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

        // 1. 透過 NVSDK_NGX_Parameter 虛擬函式表讀取公開參數
        ID3D12Resource* mv_res    = nullptr;
        ID3D12Resource* depth_res = nullptr;
        ID3D12Resource* color_res = nullptr;
        float mv_scale_x = 1.0f;
        float mv_scale_y = 1.0f;
        float jitter_x   = 0.0f;
        float jitter_y   = 0.0f;
        int   reset      = 0;
        unsigned int subrect_w = 0;
        unsigned int subrect_h = 0;

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
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subrect_w);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &subrect_h);

        if (mv_res == nullptr) {
            return;
        }

        // 2. 分眼判定（設計 2.2 節）
        //    首選：on_post_calculate_stereo_view_offset 推入的 view_index 佇列
        //    次選：每次 on_present 歸零的幀內呼叫序號 m_frame_eval_ordinal
        int eye = 0;
        std::string eye_source;
        const int ordinal_eye = std::clamp(m_frame_eval_ordinal, 0, 1);

        if (!m_stereo_view_queue.empty()) {
            int queued_eye = m_stereo_view_queue.front();
            m_stereo_view_queue.pop_front();
            // 若同一幀內該眼尚未被分配，則直接採用；若已被分配則以幀內序號修正避免雙眼撞同一側
            if (m_frame_eye_calls[queued_eye] == 0) {
                eye = queued_eye;
                eye_source = "stereo_view_offset";
                m_stats_src_offset++;
            } else {
                eye = ordinal_eye;
                eye_source = "frame_call_ordinal";
                m_stats_src_ordinal++;
            }
        } else {
            eye = ordinal_eye;
            eye_source = "frame_call_ordinal";
            m_stats_src_ordinal++;
        }

        m_frame_eval_ordinal++;
        m_frame_eye_calls[eye]++;
        if (eye == 0) {
            m_stats_left_calls++;
        } else {
            m_stats_right_calls++;
        }

        // 3. 解析貼圖尺寸與基準解析度（設計 2.1 節）
        const D3D12_RESOURCE_DESC mv_desc = mv_res->GetDesc();
        const D3D12_RESOURCE_DESC depth_desc =
            (depth_res != nullptr) ? depth_res->GetDesc() : D3D12_RESOURCE_DESC{};
        const D3D12_RESOURCE_DESC color_desc =
            (color_res != nullptr) ? color_res->GetDesc() : D3D12_RESOURCE_DESC{};

        uint32_t render_w = static_cast<uint32_t>(mv_desc.Width);
        uint32_t render_h = static_cast<uint32_t>(mv_desc.Height);
        if (subrect_w > 0 && subrect_h > 0 &&
            subrect_w <= mv_desc.Width && subrect_h <= mv_desc.Height) {
            render_w = subrect_w;
            render_h = subrect_h;
        } else if (feat.width > 0 && feat.height > 0 &&
                   feat.width <= mv_desc.Width && feat.height <= mv_desc.Height) {
            render_w = feat.width;
            render_h = feat.height;
        }

        if (render_w == 0 || render_h == 0) {
            return;
        }

        // 若未設 MVLowRes (bit 1) 且 MV 貼圖本身是放大後尺寸，則基準分母為 MV 貼圖尺寸
        uint32_t base_w = render_w;
        uint32_t base_h = render_h;
        if ((feat.create_flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) == 0 &&
            feat.out_width > render_w &&
            mv_desc.Width >= feat.out_width && mv_desc.Height >= feat.out_height) {
            base_w = feat.out_width;
            base_h = feat.out_height;
        }

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

        // 4. 確保 D3D12 降採樣管線與輸出貼圖已就緒
        if (!EnsureD3D12PipelineLocked(cmd_list, eye_grid_w, eye_grid_h)) {
            return;
        }

        const bool want_dump = (m_dump_remaining_frames > 0);

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

        // 建立 u0 (OutMv8), u1 (OutColor8) UAV
        CreateTexture2DUavLocked(m_out_mv8_tex.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, cpu_base, 3);
        CreateTexture2DUavLocked(m_out_color8_tex.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, cpu_base, 4);

        // 6. 資源狀態保護：若設定了非 NON_PIXEL_SHADER_RESOURCE 的傳入狀態，派發前轉為 SRV、派發後嚴格還原
        ScopedInputStateGuard state_guard(cmd_list);
        state_guard.AddIfNeeded(mv_res, m_assumed_mv_state);
        if (depth_res != nullptr) {
            state_guard.AddIfNeeded(depth_res, m_assumed_depth_state);
        }
        if (color_res != nullptr && want_dump) {
            state_guard.AddIfNeeded(color_res, m_assumed_color_state);
        }
        state_guard.ApplyPreBarriers();

        // 7. 設定 Compute Pipeline 並派發 cs_downsample_mv_8x8
        DownsampleRootConstants rc{};
        rc.mv_valid_dim[0]    = render_w;
        rc.mv_valid_dim[1]    = render_h;
        rc.depth_valid_dim[0] = (depth_res != nullptr) ? std::min<uint32_t>(render_w, static_cast<uint32_t>(depth_desc.Width)) : 1u;
        rc.depth_valid_dim[1] = (depth_res != nullptr) ? std::min<uint32_t>(render_h, static_cast<uint32_t>(depth_desc.Height)) : 1u;
        rc.color_valid_dim[0] = (color_res != nullptr) ? std::min<uint32_t>(render_w, static_cast<uint32_t>(color_desc.Width)) : 1u;
        rc.color_valid_dim[1] = (color_res != nullptr) ? std::min<uint32_t>(render_h, static_cast<uint32_t>(color_desc.Height)) : 1u;
        rc.eye_grid_dim[0]    = eye_grid_w;
        rc.eye_grid_dim[1]    = eye_grid_h;
        rc.eye_index          = static_cast<uint32_t>(eye);

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

        rc.mv_scale_to_uv[0]  = mv_scale_to_uv_x;
        rc.mv_scale_to_uv[1]  = mv_scale_to_uv_y;
        rc.jitter_delta_uv[0] = jitter_delta_uv_x;
        rc.jitter_delta_uv[1] = jitter_delta_uv_y;
        rc.depth_epsilon      = 0.02f;
        rc.reserved           = 0u;

        ID3D12DescriptorHeap* heaps[] = {m_srv_uav_heap.Get()};
        cmd_list->SetDescriptorHeaps(1, heaps);
        cmd_list->SetComputeRootSignature(m_root_sig.Get());
        cmd_list->SetPipelineState(m_pso.Get());
        cmd_list->SetComputeRoot32BitConstants(0, 16, &rc, 0);

        D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu_base;
        D3D12_GPU_DESCRIPTOR_HANDLE uav_gpu = gpu_base;
        uav_gpu.ptr += 3ull * m_srv_uav_desc_size;

        cmd_list->SetComputeRootDescriptorTable(1, srv_gpu);
        cmd_list->SetComputeRootDescriptorTable(2, uav_gpu);
        cmd_list->Dispatch(eye_grid_w, eye_grid_h, 1);

        // 插入 UAV barrier，確保寫入完成且左右眼寫入同一張貼圖的左右半邊不發生競爭
        D3D12_RESOURCE_BARRIER uav_barriers[2]{};
        uav_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barriers[0].UAV.pResource = m_out_mv8_tex.Get();
        uav_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_barriers[1].UAV.pResource = m_out_color8_tex.Get();
        cmd_list->ResourceBarrier(want_dump ? 2 : 1, uav_barriers);

        // 還原輸入資源（MV / Depth / Color）至遊戲傳入時的狀態
        state_guard.RestorePostBarriers();

        // 8. 記錄本眼元資料與 UEVR 相機矩陣
        QueryUevrEyeProjectionLocked(eye, m_latest_eye_cam[eye]);

        EyeEvalMetadata& meta = m_frame_eye_meta[eye];
        meta.valid              = true;
        meta.eye_index          = eye;
        meta.eye_source         = eye_source;
        meta.feature_id         = static_cast<int>(feat.feature_id);
        meta.handle_id          = feat.handle_id;
        meta.create_flags       = feat.create_flags;
        meta.render_w           = render_w;
        meta.render_h           = render_h;
        meta.base_w             = base_w;
        meta.base_h             = base_h;
        meta.mv_tex_w           = static_cast<uint32_t>(mv_desc.Width);
        meta.mv_tex_h           = static_cast<uint32_t>(mv_desc.Height);
        meta.mv_format          = static_cast<uint32_t>(mv_desc.Format);
        meta.depth_tex_w        = static_cast<uint32_t>(depth_desc.Width);
        meta.depth_tex_h        = static_cast<uint32_t>(depth_desc.Height);
        meta.depth_format       = static_cast<uint32_t>(depth_desc.Format);
        meta.color_tex_w        = static_cast<uint32_t>(color_desc.Width);
        meta.color_tex_h        = static_cast<uint32_t>(color_desc.Height);
        meta.color_format       = static_cast<uint32_t>(color_desc.Format);
        meta.mv_scale_x         = mv_scale_x;
        meta.mv_scale_y         = mv_scale_y;
        meta.mv_scale_to_uv_x   = mv_scale_to_uv_x;
        meta.mv_scale_to_uv_y   = mv_scale_to_uv_y;
        meta.jitter_x           = jitter_x;
        meta.jitter_y           = jitter_y;
        meta.jitter_delta_uv_x  = jitter_delta_uv_x;
        meta.jitter_delta_uv_y  = jitter_delta_uv_y;
        meta.reset              = reset;
        meta.cam                = m_latest_eye_cam[eye];

        // 9. 若處於傾印模式，將最新寫入的 1/8 MV 與 Color 縮圖在同一個 Command List 上
        //    CopyTextureRegion 到 Readback Heap（並立即把輸出貼圖狀態還原回 UNORDERED_ACCESS）
        if (want_dump) {
            RecordDumpCopyToReadbackLocked(cmd_list);
        }
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
        uint32_t eye_grid_h) {
        ComPtr<ID3D12Device> dev;
        if (FAILED(cmd_list->GetDevice(IID_PPV_ARGS(&dev))) || !dev) {
            return false;
        }

        if (m_device.Get() != dev.Get()) {
            ReleaseD3D12ResourcesLocked();
            m_device = dev;
        }

        // 1. 建立 Root Signature 與 Compute PSO
        if (!m_root_sig || !m_pso) {
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

        // 4. 建立傾印用的 Fence
        if (!m_readback_fence) {
            if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_readback_fence)))) {
                LogError("[VIPLE-GAME-MV] Failed to create D3D12 readback fence");
                return false;
            }
            m_fence_counter = 0;
        }

        return true;
    }

    bool CreateComputePipelineLocked() {
        // 優先從外掛目錄讀取 cs_downsample_mv_8x8.hlsl 編譯；若檔案不存在則使用內嵌字串
        ComPtr<ID3DBlob> cs_blob;
        ComPtr<ID3DBlob> err_blob;
        const fs::path hlsl_path = m_plugin_dir / L"cs_downsample_mv_8x8.hlsl";
        UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

        HRESULT hr = E_FAIL;
        if (fs::exists(hlsl_path)) {
            hr = D3DCompileFromFile(
                hlsl_path.c_str(),
                nullptr,
                nullptr,
                "cs_downsample_mv_8x8",
                "cs_5_0",
                compile_flags,
                0,
                &cs_blob,
                &err_blob);
            if (SUCCEEDED(hr)) {
                LogInfo("[VIPLE-GAME-MV] Compiled cs_downsample_mv_8x8 from file: %s",
                        hlsl_path.u8string().c_str());
            } else {
                LogWarn("[VIPLE-GAME-MV] D3DCompileFromFile failed (%s), falling back to embedded HLSL",
                        err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown");
                err_blob.Reset();
            }
        }

        if (FAILED(hr) || !cs_blob) {
            hr = D3DCompile(
                kEmbeddedDownsampleHlsl,
                std::strlen(kEmbeddedDownsampleHlsl),
                "cs_downsample_mv_8x8_embedded.hlsl",
                nullptr,
                nullptr,
                "cs_downsample_mv_8x8",
                "cs_5_0",
                compile_flags,
                0,
                &cs_blob,
                &err_blob);
            if (FAILED(hr) || !cs_blob) {
                LogError("[VIPLE-GAME-MV] Failed to compile embedded cs_downsample_mv_8x8: %s",
                         err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown");
                return false;
            }
            LogInfo("[VIPLE-GAME-MV] Compiled cs_downsample_mv_8x8 from embedded HLSL source");
        }

        // 建立 Root Signature：
        //   [0] Root Constants (b0, 16 x 32-bit values)
        //   [1] Descriptor Table (SRV t0..t2)
        //   [2] Descriptor Table (UAV u0..u1)
        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors                    = 3;
        srv_range.BaseShaderRegister                = 0;
        srv_range.RegisterSpace                     = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors                    = 2;
        uav_range.BaseShaderRegister                = 0;
        uav_range.RegisterSpace                     = 0;
        uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[3]{};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;
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
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &rs_err_blob);
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
        pso_desc.pRootSignature = m_root_sig.Get();
        pso_desc.CS.pShaderBytecode = cs_blob->GetBufferPointer();
        pso_desc.CS.BytecodeLength  = cs_blob->GetBufferSize();

        hr = m_device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&m_pso));
        if (FAILED(hr) || !m_pso) {
            LogError("[VIPLE-GAME-MV] CreateComputePipelineState failed (hr=0x%08x)", static_cast<unsigned>(hr));
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
        m_root_sig.Reset();
        m_srv_uav_heap.Reset();
        m_dummy_tex.Reset();
        m_out_mv8_tex.Reset();
        m_out_color8_tex.Reset();
        m_readback_fence.Reset();
        for (auto& slot : m_readback_slots) {
            slot.mv8_buf.Reset();
            slot.color8_buf.Reset();
            slot.allocated_mv8_bytes = 0;
            slot.allocated_color8_bytes = 0;
            slot.in_use.store(false, std::memory_order_release);
        }
        m_device.Reset();
        m_eye_grid_w = 0;
        m_eye_grid_h = 0;
    }

    // ========================================================================
    // Readback 傾印（D3D12 Readback Heap + Fence + 背景執行緒）
    // ========================================================================
    void CheckDumpTriggerFileLocked() {
        if (m_dump_remaining_frames > 0 || m_dump_batch_in_flight.load(std::memory_order_acquire)) {
            return;
        }
        // 每 15 幀檢查一次外掛目錄下的 viple_mv_dump.txt
        if ((m_present_count % 15ull) != 0ull && m_present_count > 1ull) {
            return;
        }

        const fs::path trigger_path = m_plugin_dir / L"viple_mv_dump.txt";
        std::error_code ec;
        if (!fs::exists(trigger_path, ec)) {
            return;
        }

        uint32_t requested_frames = 10u;
        {
            std::ifstream ifs(trigger_path);
            if (ifs) {
                uint32_t val = 0;
                if (ifs >> val && val > 0) {
                    requested_frames = std::clamp(val, 1u, 600u);
                }
            }
        }

        const fs::path dump_dir = m_plugin_dir / L"dump";
        fs::create_directories(dump_dir, ec);

        m_dump_remaining_frames = requested_frames;
        m_dump_seq_counter = 0;
        m_dump_batch_in_flight.store(true, std::memory_order_release);

        LogInfo("[VIPLE-GAME-MV] Detected viple_mv_dump.txt: scheduling %u frames to %s",
                requested_frames,
                dump_dir.u8string().c_str());
    }

    bool EnsureReadbackSlotBuffersLocked(
        uint32_t slot_idx,
        uint32_t sbs_w,
        uint32_t sbs_h,
        uint32_t& out_mv8_pitch,
        uint32_t& out_color8_pitch) {
        out_mv8_pitch    = AlignUp(sbs_w * 8u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); // RGBA16F = 8 B/px
        out_color8_pitch = AlignUp(sbs_w * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT); // RGBA8   = 4 B/px

        const uint64_t mv8_total_bytes    = static_cast<uint64_t>(out_mv8_pitch) * sbs_h;
        const uint64_t color8_total_bytes = static_cast<uint64_t>(out_color8_pitch) * sbs_h;

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

        return true;
    }

    void RecordDumpCopyToReadbackLocked(ID3D12GraphicsCommandList* cmd_list) {
        if (!m_out_mv8_tex || !m_out_color8_tex || m_eye_grid_w == 0 || m_eye_grid_h == 0) {
            return;
        }

        // 若本幀尚未選定空閒的 Readback Slot，挑一個未在使用中的 Slot
        if (!m_frame_dump_copied) {
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
                // 所有 Readback Slots 都在背景寫檔中，跳過此幀避免阻塞遊戲 GPU/CPU
                LogWarn("[VIPLE-GAME-MV] All readback slots busy, skipping dump copy for this frame");
                return;
            }
            m_current_frame_rb_slot = chosen;
        }

        const uint32_t sbs_w = m_eye_grid_w * 2u;
        const uint32_t sbs_h = m_eye_grid_h;
        uint32_t mv8_pitch = 0;
        uint32_t color8_pitch = 0;
        if (!EnsureReadbackSlotBuffersLocked(
                m_current_frame_rb_slot, sbs_w, sbs_h, mv8_pitch, color8_pitch)) {
            return;
        }

        ReadbackSlotResources& slot = m_readback_slots[m_current_frame_rb_slot];

        // 將 OutMv8 與 OutColor8 從 UNORDERED_ACCESS 轉為 COPY_SOURCE
        D3D12_RESOURCE_BARRIER to_copy[2]{};
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
        cmd_list->ResourceBarrier(2, to_copy);

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

        // 還原 OutMv8 與 OutColor8 回 UNORDERED_ACCESS
        D3D12_RESOURCE_BARRIER to_uav[2] = {to_copy[0], to_copy[1]};
        std::swap(to_uav[0].Transition.StateBefore, to_uav[0].Transition.StateAfter);
        std::swap(to_uav[1].Transition.StateBefore, to_uav[1].Transition.StateAfter);
        cmd_list->ResourceBarrier(2, to_uav);

        m_current_frame_mv8_pitch    = mv8_pitch;
        m_current_frame_color8_pitch = color8_pitch;
        m_frame_dump_copied          = true;
    }

    void SubmitAsyncDumpOnPresentLocked() {
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

        const uint32_t slot_idx = m_current_frame_rb_slot;
        m_readback_slots[slot_idx].in_use.store(true, std::memory_order_release);

        FrameDumpMetadata task{};
        task.present_frame_index = m_present_count;
        task.dump_seq_index      = m_dump_seq_counter++;
        task.eye_grid_w          = m_eye_grid_w;
        task.eye_grid_h          = m_eye_grid_h;
        task.sbs_grid_w          = m_eye_grid_w * 2u;
        task.sbs_grid_h          = m_eye_grid_h;
        task.mv8_row_pitch       = m_current_frame_mv8_pitch;
        task.color8_row_pitch    = m_current_frame_color8_pitch;
        task.readback_slot       = slot_idx;
        task.fence_value         = fv;
        task.eyes[0]             = m_frame_eye_meta[0];
        task.eyes[1]             = m_frame_eye_meta[1];
        QueryUevrHmdPoseLocked(task);

        m_dump_remaining_frames--;
        task.is_last_in_batch = (m_dump_remaining_frames == 0);

        {
            std::lock_guard<std::mutex> qlock(m_dump_queue_mutex);
            m_dump_queue.push_back(std::move(task));
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
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            fence = m_readback_fence;
            if (task.readback_slot < kReadbackSlots) {
                mv8_buf    = m_readback_slots[task.readback_slot].mv8_buf;
                color8_buf = m_readback_slots[task.readback_slot].color8_buf;
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
                LogInfo("[VIPLE-GAME-MV] Finished dumping batch (%u frames), removed trigger file viple_mv_dump.txt",
                        task.dump_seq_index + 1u);
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
        const std::string mv_bin_filename   = std::string("mv8_") + base_name + ".bin";
        const std::string mv_json_filename  = std::string("mv8_") + base_name + ".json";
        const std::string col_png_filename  = std::string("color8_") + base_name + ".png";

        const fs::path mv_bin_path  = dump_dir / mv_bin_filename;
        const fs::path mv_json_path = dump_dir / mv_json_filename;
        const fs::path col_png_path = dump_dir / col_png_filename;

        // 1. Map 並寫出 mv8_<frame>.bin（緊密排列 RGBA16F，每格 8 bytes）
        {
            const size_t row_bytes = static_cast<size_t>(task.sbs_grid_w) * 8u;
            const size_t total_mapped = static_cast<size_t>(task.mv8_row_pitch) * task.sbs_grid_h;
            D3D12_RANGE read_range{0, total_mapped};
            void* mapped = nullptr;
            if (SUCCEEDED(mv8_buf->Map(0, &read_range, &mapped)) && mapped != nullptr) {
                std::ofstream ofs(mv_bin_path, std::ios::binary | std::ios::trunc);
                const auto* src_bytes = static_cast<const uint8_t*>(mapped);
                for (uint32_t y = 0; y < task.sbs_grid_h; ++y) {
                    ofs.write(
                        reinterpret_cast<const char*>(src_bytes + static_cast<size_t>(y) * task.mv8_row_pitch),
                        static_cast<std::streamsize>(row_bytes));
                }
                D3D12_RANGE write_range{0, 0};
                mv8_buf->Unmap(0, &write_range);
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

        // 3. 寫出 mv8_<frame>.json（含尺寸、格式、mvScale、jitter、reset、分眼判定來源與相機矩陣）
        WriteDumpJson(mv_json_path, task, mv_bin_filename, col_png_filename);

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

    static void WriteDumpJson(
        const fs::path& json_path,
        const FrameDumpMetadata& task,
        const std::string& mv_bin_filename,
        const std::string& col_png_filename) {
        std::ofstream ofs(json_path, std::ios::trunc);
        if (!ofs) {
            return;
        }
        ofs << "{\n";
        ofs << "  \"frame_seq\": " << task.dump_seq_index << ",\n";
        ofs << "  \"present_frame_index\": " << task.present_frame_index << ",\n";
        ofs << "  \"format\": \"RGBA16F\",\n";
        ofs << "  \"channel_layout\": \"xy=eye_uv_mv(uv_prev-uv_cur), z=inv_depth, w=validity\",\n";
        ofs << "  \"eye_layout\": \"left_half=left_eye, right_half=right_eye\",\n";
        ofs << "  \"sbs_grid_w\": " << task.sbs_grid_w << ",\n";
        ofs << "  \"sbs_grid_h\": " << task.sbs_grid_h << ",\n";
        ofs << "  \"eye_grid_w\": " << task.eye_grid_w << ",\n";
        ofs << "  \"eye_grid_h\": " << task.eye_grid_h << ",\n";
        ofs << "  \"mv_bin_file\": \"" << mv_bin_filename << "\",\n";
        ofs << "  \"color_file\": \"" << col_png_filename << "\",\n";

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
            ofs << "    \"" << eye_names[e] << "\": {\n";
            ofs << "      \"valid\": " << (em.valid ? "true" : "false") << ",\n";
            ofs << "      \"eye_index\": " << em.eye_index << ",\n";
            ofs << "      \"eye_source\": \"" << em.eye_source << "\",\n";
            ofs << "      \"feature_id\": " << em.feature_id << ",\n";
            ofs << "      \"handle_id\": " << em.handle_id << ",\n";
            ofs << "      \"create_flags\": " << em.create_flags << ",\n";
            ofs << "      \"render_dim\": [" << em.render_w << ", " << em.render_h << "],\n";
            ofs << "      \"base_dim\": [" << em.base_w << ", " << em.base_h << "],\n";
            ofs << "      \"mv_tex_dim\": [" << em.mv_tex_w << ", " << em.mv_tex_h << "],\n";
            ofs << "      \"mv_format\": " << em.mv_format << ",\n";
            ofs << "      \"depth_tex_dim\": [" << em.depth_tex_w << ", " << em.depth_tex_h << "],\n";
            ofs << "      \"depth_format\": " << em.depth_format << ",\n";
            ofs << "      \"color_tex_dim\": [" << em.color_tex_w << ", " << em.color_tex_h << "],\n";
            ofs << "      \"color_format\": " << em.color_format << ",\n";
            ofs << "      \"mv_scale\": [" << em.mv_scale_x << ", " << em.mv_scale_y << "],\n";
            ofs << "      \"mv_scale_to_uv\": [" << em.mv_scale_to_uv_x << ", " << em.mv_scale_to_uv_y << "],\n";
            ofs << "      \"jitter_offset\": [" << em.jitter_x << ", " << em.jitter_y << "],\n";
            ofs << "      \"jitter_delta_uv\": [" << em.jitter_delta_uv_x << ", " << em.jitter_delta_uv_y << "],\n";
            ofs << "      \"reset\": " << em.reset << ",\n";
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

    // 分眼判定與相機狀態
    bool m_view_index_observed_zero{false};
    std::deque<int> m_stereo_view_queue;
    EyeCameraPose m_latest_eye_cam[2]{};
    int m_frame_eval_ordinal{0};
    uint32_t m_frame_eye_calls[2]{0, 0};
    EyeEvalMetadata m_frame_eye_meta[2]{};

    // 600 幀統計計數器
    uint64_t m_present_count{0};
    uint32_t m_stats_window_presents{0};
    uint32_t m_stats_left_calls{0};
    uint32_t m_stats_right_calls{0};
    uint32_t m_stats_both_eyes_presents{0};
    uint32_t m_stats_src_offset{0};
    uint32_t m_stats_src_ordinal{0};
    float    m_last_mv_scale_x{1.0f};
    float    m_last_mv_scale_y{1.0f};
    uint32_t m_last_render_w{0};
    uint32_t m_last_render_h{0};

    // D3D12 Compute Pipeline 與輸出貼圖
    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12RootSignature>       m_root_sig;
    ComPtr<ID3D12PipelineState>       m_pso;
    ComPtr<ID3D12DescriptorHeap>      m_srv_uav_heap;
    uint32_t                          m_srv_uav_desc_size{0};
    uint32_t                          m_desc_ring_cursor{0};
    ComPtr<ID3D12Resource>            m_dummy_tex;
    ComPtr<ID3D12Resource>            m_out_mv8_tex;
    ComPtr<ID3D12Resource>            m_out_color8_tex;
    uint32_t                          m_eye_grid_w{0};
    uint32_t                          m_eye_grid_h{0};
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
    uint32_t                          m_current_frame_rb_slot{0};
    uint32_t                          m_current_frame_mv8_pitch{0};
    uint32_t                          m_current_frame_color8_pitch{0};
    bool                              m_frame_dump_copied{false};

    uint32_t                          m_dump_remaining_frames{0};
    uint32_t                          m_dump_seq_counter{0};
    std::atomic<bool>                 m_dump_batch_in_flight{false};

    std::atomic<bool>                 m_worker_running{false};
    HANDLE                            m_fence_event{nullptr};
    std::mutex                        m_dump_queue_mutex;
    std::condition_variable           m_dump_cv;
    std::deque<FrameDumpMetadata>     m_dump_queue;
    std::thread                       m_dump_worker;
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
