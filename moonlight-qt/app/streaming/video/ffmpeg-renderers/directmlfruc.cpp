// VipleStream: DirectMLFRUC — see directmlfruc.h for the rationale.
//
// Implementation strategy for the first ship:
//   * Hold three D3D12 committed resources for the input / output
//     tensors (prev, curr, output) in FP16 NCHW RGBA.
//   * The renderer still draws into an ID3D11Texture2D (m_RenderTexture),
//     which we snapshot into a D3D11 staging read before executing the
//     DML graph; staging read -> CPU buffer -> FP16 quantize -> upload.
//   * After the graph runs we readback output -> CPU -> dequantize ->
//     upload into m_OutputTexture (D3D11) via a write-staging texture.
//
// The CPU roundtrip is obvious and deliberately simple — this is the
// scaffolding backend that will get replaced by shared-heap zero-copy
// once the DML graph is doing meaningful work. For "bidirectional
// mean" (the initial graph) the CPU path is slower than GenericFRUC,
// but it validates the full DirectML plumbing on real decoded frames.
//
// TODO(directml:zerocopy): replace staging roundtrip with shared NT
// handles (D3D12 committed resource with SHARED flag, opened via
// ID3D11Device1::OpenSharedResource1 on the renderer's device) and a
// cross-API fence for sync. Saves two ~8 MB memcpys per frame on
// 1080p RGBA8.
// TODO(directml:model): replace the inline DML_ELEMENT_WISE_ADD
// graph with a compiled ONNX model (e.g. RIFE v4 lite). The graph
// compilation happens in compileDMLGraph(); the per-frame dispatch
// in executeDMLGraph() already passes bound inputs/outputs through
// IDMLCommandRecorder::RecordDispatch so only the operator itself
// needs to change.

#include "directmlfruc.h"

#include <SDL.h>

#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <immintrin.h>   // D4 iter 1: F16C + SSE intrinsics for tensor conversion.

// MSVC's windows.h can leak min/max macros which shadow std::min/max.
// Undefine here so the std calls below compile regardless of include
// order.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using Microsoft::WRL::ComPtr;

namespace {

// ---------------------------------------------------------------
// D4 iter 1: F16C (Ivy Bridge / 2012+) delivers the FP16 <-> FP32
// conversion in a single vector instruction. Every Windows 10+
// target CPU we actually ship to has F16C (Intel Ivy Bridge, AMD
// Piledriver / Zen1+). The scalar path below is only a runtime
// safety net in case something regresses.
// ---------------------------------------------------------------

inline uint16_t scalarFloatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t m = mant >> shift;
        if ((mant >> (shift - 1)) & 1u) m += 1;
        return (uint16_t)(sign | m);
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7C00u | (mant ? 0x200u : 0u));
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

inline float scalarHalfToFloat(uint16_t h)
{
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp  = ((uint32_t)h >> 10) & 0x1Fu;
    uint32_t mant = (uint32_t)h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) { f = sign; }
        else {
            while ((mant & 0x400u) == 0) { mant <<= 1; exp--; }
            exp++;
            mant &= ~0x400u;
            f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13);
    } else {
        f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out; std::memcpy(&out, &f, sizeof(out));
    return out;
}

// Cached CPU feature probe — F16C bit in CPUID leaf 1 ECX (bit 29).
// Checked once at first use; every subsequent convert path reads
// this flag without any CPUID cost.
inline bool hasF16C()
{
    static const bool cached = []{
        int r[4] = {};
#ifdef _MSC_VER
        __cpuid(r, 1);
#endif
        return (r[2] & (1 << 29)) != 0;
    }();
    return cached;
}

// Align x up to multiple of a (a must be power of two).
inline uint64_t alignUp(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------
// D4 iter 4: SIMD-tight RGBA8 row -> planar FP16 NCHW conversion
// with the 0.5 scale folded in (see DML op notes — we need the
// pre-scale so ADD1 is a true mean). One pass processes 8 pixels:
//   * load 32 bytes (two __m128i vecs)
//   * unpack to 4x __m128i int32
//   * convert to fp32 and multiply by 1/510
//   * F16C pack to 8x fp16 per lane
//   * scatter into the four destination planes.
// ~6-8 CPU cycles/pixel vs ~60 for the scalar loop. The tail
// (rows that aren't a multiple of 8 pixels wide) falls back to
// scalar so any resolution still works.
// ---------------------------------------------------------------
inline void packRowRgba8ToPlanarFp16(const uint8_t* src, uint16_t* dstR, uint16_t* dstG,
                                     uint16_t* dstB, uint16_t* dstA, uint32_t width)
{
    const float inv510 = 1.0f / 510.0f;
    uint32_t x = 0;
    if (hasF16C()) {
        const __m128 vScale = _mm_set1_ps(inv510);
        for (; x + 8 <= width; x += 8) {
            // 8 RGBA pixels = 32 bytes
            __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4 + 16));
            // Gather channels by shuffling the 4-byte groups. Uses
            // _mm_shuffle_epi8 with per-channel index vectors.
            alignas(16) static const uint8_t kShufR[16] = {0,4,8,12, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80};
            alignas(16) static const uint8_t kShufG[16] = {1,5,9,13, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80};
            alignas(16) static const uint8_t kShufB[16] = {2,6,10,14,0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80};
            alignas(16) static const uint8_t kShufA[16] = {3,7,11,15,0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80, 0x80,0x80,0x80,0x80};
            __m128i mR = _mm_load_si128(reinterpret_cast<const __m128i*>(kShufR));
            __m128i mG = _mm_load_si128(reinterpret_cast<const __m128i*>(kShufG));
            __m128i mB = _mm_load_si128(reinterpret_cast<const __m128i*>(kShufB));
            __m128i mA = _mm_load_si128(reinterpret_cast<const __m128i*>(kShufA));

            auto process = [&](__m128i chan0, __m128i chan1, uint16_t* dst) {
                __m128i c = _mm_unpacklo_epi32(chan0, chan1);         // 8 bytes in low lane
                __m128i w0 = _mm_cvtepu8_epi32(c);                     // first 4 -> int32
                __m128i w1 = _mm_cvtepu8_epi32(_mm_srli_si128(c, 4));  // last 4 -> int32
                __m128  f0 = _mm_mul_ps(_mm_cvtepi32_ps(w0), vScale);
                __m128  f1 = _mm_mul_ps(_mm_cvtepi32_ps(w1), vScale);
                __m128i h0 = _mm_cvtps_ph(f0, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
                __m128i h1 = _mm_cvtps_ph(f1, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(dst),     h0);
                _mm_storel_epi64(reinterpret_cast<__m128i*>(dst + 4), h1);
            };
            process(_mm_shuffle_epi8(v0, mR), _mm_shuffle_epi8(v1, mR), dstR + x);
            process(_mm_shuffle_epi8(v0, mG), _mm_shuffle_epi8(v1, mG), dstG + x);
            process(_mm_shuffle_epi8(v0, mB), _mm_shuffle_epi8(v1, mB), dstB + x);
            process(_mm_shuffle_epi8(v0, mA), _mm_shuffle_epi8(v1, mA), dstA + x);
        }
    }
    // Scalar tail (and full fallback when F16C is absent).
    for (; x < width; ++x) {
        float r = src[x*4 + 0] * inv510;
        float g = src[x*4 + 1] * inv510;
        float b = src[x*4 + 2] * inv510;
        float a = src[x*4 + 3] * inv510;
        dstR[x] = scalarFloatToHalf(r);
        dstG[x] = scalarFloatToHalf(g);
        dstB[x] = scalarFloatToHalf(b);
        dstA[x] = scalarFloatToHalf(a);
    }
}

// Inverse: planar FP16 -> interleaved RGBA8 row. 0-1 clamp + 255x
// fused into a single multiply since the DML output of ADD1 is
// already in [0, 1]. F16C path processes 8 pixels per iteration.
inline void unpackRowPlanarFp16ToRgba8(const uint16_t* srcR, const uint16_t* srcG,
                                       const uint16_t* srcB, const uint16_t* srcA,
                                       uint8_t* dst, uint32_t width)
{
    uint32_t x = 0;
    if (hasF16C()) {
        const __m128 v255 = _mm_set1_ps(255.0f);
        const __m128 vLo  = _mm_set1_ps(0.0f);
        const __m128 vHi  = _mm_set1_ps(255.0f);
        for (; x + 8 <= width; x += 8) {
            auto load8 = [&](const uint16_t* p) -> std::pair<__m128i, __m128i> {
                __m128i h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                __m128  f0 = _mm_cvtph_ps(h);
                __m128  f1 = _mm_cvtph_ps(_mm_unpackhi_epi64(h, h));
                __m128  p0 = _mm_min_ps(vHi, _mm_max_ps(vLo, _mm_mul_ps(f0, v255)));
                __m128  p1 = _mm_min_ps(vHi, _mm_max_ps(vLo, _mm_mul_ps(f1, v255)));
                __m128i i0 = _mm_cvtps_epi32(p0);
                __m128i i1 = _mm_cvtps_epi32(p1);
                return {i0, i1};
            };
            auto [r0, r1] = load8(srcR + x);
            auto [g0, g1] = load8(srcG + x);
            auto [b0, b1] = load8(srcB + x);
            auto [a0, a1] = load8(srcA + x);
            __m128i rg0 = _mm_packus_epi32(r0, g0);   // r0..r3, g0..g3 as u16
            __m128i rg1 = _mm_packus_epi32(r1, g1);
            __m128i ba0 = _mm_packus_epi32(b0, a0);
            __m128i ba1 = _mm_packus_epi32(b1, a1);
            // Re-interleave to u8 RGBA pixels. Build two halves separately.
            __m128i rgba0 = _mm_packus_epi16(
                _mm_unpacklo_epi16(rg0, ba0),
                _mm_unpackhi_epi16(rg0, ba0));
            __m128i rgba1 = _mm_packus_epi16(
                _mm_unpacklo_epi16(rg1, ba1),
                _mm_unpackhi_epi16(rg1, ba1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x * 4),      rgba0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + x * 4 + 16), rgba1);
        }
    }
    for (; x < width; ++x) {
        auto q = [](float v) { int i = (int)(v * 255.0f + 0.5f); if (i < 0) i = 0; if (i > 255) i = 255; return (uint8_t)i; };
        dst[x*4 + 0] = q(scalarHalfToFloat(srcR[x]));
        dst[x*4 + 1] = q(scalarHalfToFloat(srcG[x]));
        dst[x*4 + 2] = q(scalarHalfToFloat(srcB[x]));
        dst[x*4 + 3] = q(scalarHalfToFloat(srcA[x]));
    }
}

} // namespace


DirectMLFRUC::DirectMLFRUC() = default;

DirectMLFRUC::~DirectMLFRUC() { destroy(); }


bool DirectMLFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_Device11 = device;
    m_Width    = width;
    m_Height   = height;
    m_TensorElements = 1u * 4u * width * height;
    m_TensorBytes    = m_TensorElements * sizeof(uint16_t);

    if (!createD3D12Device())       return false;
    if (!createDMLDevice())         return false;
    if (!createD3D11Textures())     return false;
    if (!createD3D12TensorResources()) return false;
    if (!compileDMLGraph())         return false;
    if (!createCommandInfra())      return false;

    m_Initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML backend initialized: %ux%u, FP16 tensor, %.2f MB/buffer",
                width, height, (double)m_TensorBytes / (1024.0 * 1024.0));
    return true;
}


void DirectMLFRUC::destroy()
{
    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }
    m_DMLBindingTable.Reset();
    m_DMLCompiledOp.Reset();
    m_DMLDescHeap.Reset();
    m_DMLRecorder.Reset();
    m_DMLDevice.Reset();

    // D4 iter 2: release the persistent CPU-visible maps before the
    // underlying UPLOAD / READBACK resources release.
    if (m_UploadCurr && m_UploadCurrPtr) {
        D3D12_RANGE written = { 0, m_TensorBytes };
        m_UploadCurr->Unmap(0, &written);
        m_UploadCurrPtr = nullptr;
    }
    if (m_ReadbackOutput && m_ReadbackOutputPtr) {
        D3D12_RANGE none = { 0, 0 };
        m_ReadbackOutput->Unmap(0, &none);
        m_ReadbackOutputPtr = nullptr;
    }

    m_FrameTensor[0].Reset();
    m_FrameTensor[1].Reset();
    m_OutputTensor.Reset();
    m_TempResource.Reset();
    m_PersistentResource.Reset();
    m_UploadCurr.Reset();
    m_ReadbackOutput.Reset();

    m_CmdList12.Reset();
    m_CmdAlloc12.Reset();
    m_Fence12.Reset();
    m_Queue12.Reset();
    m_Device12.Reset();

    m_StagingRead.Reset();
    m_StagingWrite.Reset();
    m_RenderSRV.Reset();
    m_RenderRTV.Reset();
    m_RenderTexture.Reset();
    m_OutputSRV.Reset();
    m_OutputTexture.Reset();

    m_Device11 = nullptr;
    m_Initialized = false;
}


bool DirectMLFRUC::createD3D12Device()
{
    // D3D12 must run on the same IDXGIAdapter as the renderer's
    // D3D11 device, otherwise the renderer won't be able to use the
    // staging textures we hand back (on Windows 11 Multi-GPU).
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;

    HRESULT hr = D3D12CreateDevice(adapter.Get(),
                                   D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&m_Device12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: D3D12CreateDevice failed 0x%08lx", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_Device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_Queue12)))) return false;

    if (FAILED(m_Device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&m_CmdAlloc12)))) return false;
    if (FAILED(m_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             m_CmdAlloc12.Get(), nullptr,
                                             IID_PPV_ARGS(&m_CmdList12)))) return false;
    m_CmdList12->Close();

    if (FAILED(m_Device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence12)))) return false;
    m_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) return false;

    return true;
}


bool DirectMLFRUC::createDMLDevice()
{
    // DML_CREATE_DEVICE_FLAG_NONE is the right default on release
    // builds. Debug layer can be toggled via the
    // Microsoft.AI.DirectML redistributable; system DirectML.dll
    // doesn't carry the debug layer.
    HRESULT hr = DMLCreateDevice(m_Device12.Get(), DML_CREATE_DEVICE_FLAG_NONE,
                                 IID_PPV_ARGS(&m_DMLDevice));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: DMLCreateDevice failed 0x%08lx", hr);
        return false;
    }
    if (FAILED(m_DMLDevice->CreateCommandRecorder(IID_PPV_ARGS(&m_DMLRecorder)))) return false;
    return true;
}


bool DirectMLFRUC::createD3D11Textures()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width          = m_Width;
    td.Height         = m_Height;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage          = D3D11_USAGE_DEFAULT;
    td.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0;

    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_RenderTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateRenderTargetView(m_RenderTexture.Get(), nullptr, m_RenderRTV.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_RenderTexture.Get(), nullptr, m_RenderSRV.GetAddressOf()))) return false;

    // D4 iter 3: no m_PrevTexture on the D3D11 side any more — the
    // "prev" frame lives in whichever D3D12 tensor we're not writing
    // into this frame, so the staging roundtrip for prev is gone.

    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_OutputTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_OutputTexture.Get(), nullptr, m_OutputSRV.GetAddressOf()))) return false;

    // CPU-visible staging textures for the D3D11<->D3D12 roundtrip.
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_Device11->CreateTexture2D(&sd, nullptr, m_StagingRead.GetAddressOf()))) return false;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_Device11->CreateTexture2D(&sd, nullptr, m_StagingWrite.GetAddressOf()))) return false;

    return true;
}


bool DirectMLFRUC::createD3D12TensorResources()
{
    auto makeResource = [&](uint64_t bytes, D3D12_HEAP_TYPE heap,
                            D3D12_RESOURCE_STATES state,
                            D3D12_RESOURCE_FLAGS flags,
                            ComPtr<ID3D12Resource>& out) -> bool
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = heap;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = alignUp(bytes, 256);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = flags;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        return SUCCEEDED(m_Device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&out)));
    };

    // D4 iter 3: two tensor slots that ping-pong between "prev" and
    // "curr" each frame. This is the single biggest win in the CPU
    // path — we never re-upload last frame's data, since it is
    // already in VRAM.
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_FrameTensor[0])) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_FrameTensor[1])) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_OutputTensor)) return false;

    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ,
                      D3D12_RESOURCE_FLAG_NONE, m_UploadCurr)) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_NONE, m_ReadbackOutput)) return false;

    // D4 iter 2: keep UPLOAD and READBACK persistently mapped. D3D12
    // explicitly supports long-lived Map on upload/readback heaps;
    // Map/Unmap per frame costs us a trip through the kernel-mode
    // resource tracker that's measurable in the CPU path.
    D3D12_RANGE noRead = { 0, 0 };
    if (FAILED(m_UploadCurr->Map(0, &noRead, &m_UploadCurrPtr))) return false;
    D3D12_RANGE fullRead = { 0, m_TensorBytes };
    if (FAILED(m_ReadbackOutput->Map(0, &fullRead, &m_ReadbackOutputPtr))) return false;

    return true;
}


bool DirectMLFRUC::compileDMLGraph()
{
    // Tensor descriptors: [1, 4, H, W] FP16 NCHW. DirectML requires
    // Sizes and Strides to match the on-disk layout; for contiguous
    // NCHW the strides are [C*H*W, H*W, W, 1].
    const uint32_t sizes[4]   = { 1u, 4u, m_Height, m_Width };
    const uint32_t strides[4] = { 4u * m_Height * m_Width,
                                  m_Height * m_Width,
                                  m_Width,
                                  1u };

    DML_BUFFER_TENSOR_DESC bufDesc = {};
    bufDesc.DataType     = kDmlDtype;
    bufDesc.Flags        = DML_TENSOR_FLAG_NONE;
    bufDesc.DimensionCount = 4;
    bufDesc.Sizes        = sizes;
    bufDesc.Strides      = strides;
    bufDesc.TotalTensorSizeInBytes = m_TensorBytes;

    DML_TENSOR_DESC tensorDesc = {};
    tensorDesc.Type = DML_TENSOR_TYPE_BUFFER;
    tensorDesc.Desc = &bufDesc;

    // Bidirectional mean: output = 0.5 * prev + 0.5 * curr. We
    // express this with a single DML_ELEMENT_WISE_ADD and a scale
    // factor applied to the result (Scale=0.5, Bias=0). Both
    // operands are the same shape/type, no broadcasting required.
    //
    // TODO(directml:model): replace this block with DMLX graph
    // construction that wraps a compiled ONNX FRUC model. The
    // binding shape below (two inputs, one output) matches a
    // standard 2-input interp model, so most of the surrounding
    // plumbing stays as-is when we swap the operator.
    DML_SCALE_BIAS scaleBias = { 0.5f, 0.0f };

    DML_ELEMENT_WISE_ADD1_OPERATOR_DESC addDesc = {};
    addDesc.ATensor       = &tensorDesc;
    addDesc.BTensor       = &tensorDesc;
    addDesc.OutputTensor  = &tensorDesc;
    addDesc.FusedActivation = nullptr;

    DML_OPERATOR_DESC opDesc = {};
    opDesc.Type = DML_OPERATOR_ELEMENT_WISE_ADD1;
    opDesc.Desc = &addDesc;

    ComPtr<IDMLOperator> op;
    if (FAILED(m_DMLDevice->CreateOperator(&opDesc, IID_PPV_ARGS(&op)))) return false;

    if (FAILED(m_DMLDevice->CompileOperator(op.Get(),
                                            DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
                                            IID_PPV_ARGS(&m_DMLCompiledOp)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CompileOperator failed (half-precision add)");
        return false;
    }

    // Allocate the descriptor heap + init/exec binding tables. We
    // keep the binding table around between frames — for a fused
    // single-op graph like this one, nothing in the binding changes
    // frame to frame.
    DML_BINDING_PROPERTIES execProps = m_DMLCompiledOp->GetBindingProperties();

    ComPtr<IDMLOperatorInitializer> initializer;
    IDMLCompiledOperator* opsToInit[] = { m_DMLCompiledOp.Get() };
    if (FAILED(m_DMLDevice->CreateOperatorInitializer(1, opsToInit, IID_PPV_ARGS(&initializer)))) return false;
    DML_BINDING_PROPERTIES initProps = initializer->GetBindingProperties();

    uint32_t descCount = std::max(initProps.RequiredDescriptorCount,
                                  execProps.RequiredDescriptorCount);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type          = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = descCount;
    hd.Flags         = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_Device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DMLDescHeap)))) return false;

    // Allocate temporary + persistent resources per DML's request.
    auto allocBuffer = [&](uint64_t bytes, ComPtr<ID3D12Resource>& out) -> bool {
        if (bytes == 0) { out.Reset(); return true; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = alignUp(bytes, 256);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(m_Device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out)));
    };

    uint64_t tempBytes = std::max(initProps.TemporaryResourceSize,
                                  execProps.TemporaryResourceSize);
    if (!allocBuffer(tempBytes, m_TempResource)) return false;
    if (!allocBuffer(execProps.PersistentResourceSize, m_PersistentResource)) return false;

    // Initialize the compiled operator: run the initializer once
    // with no inputs (this graph has none — weights are embedded in
    // the op desc) but with the persistent binding where DML will
    // stash any internal state it wants to reuse between dispatches.
    DML_BINDING_TABLE_DESC btd = {};
    btd.Dispatchable            = initializer.Get();
    btd.CPUDescriptorHandle     = m_DMLDescHeap->GetCPUDescriptorHandleForHeapStart();
    btd.GPUDescriptorHandle     = m_DMLDescHeap->GetGPUDescriptorHandleForHeapStart();
    btd.SizeInDescriptors       = descCount;
    ComPtr<IDMLBindingTable> initBinding;
    if (FAILED(m_DMLDevice->CreateBindingTable(&btd, IID_PPV_ARGS(&initBinding)))) return false;

    if (m_TempResource) {
        DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, m_TempResource->GetDesc().Width };
        DML_BINDING_DESC tbd  = { DML_BINDING_TYPE_BUFFER, &tb };
        initBinding->BindTemporaryResource(&tbd);
    }
    if (m_PersistentResource) {
        DML_BUFFER_BINDING pb = { m_PersistentResource.Get(), 0, m_PersistentResource->GetDesc().Width };
        DML_BINDING_DESC pbd  = { DML_BINDING_TYPE_BUFFER, &pb };
        initBinding->BindOutputs(1, &pbd);
    }

    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = { m_DMLDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, heaps);
    m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), initializer.Get(), initBinding.Get());
    m_CmdList12->Close();
    ID3D12CommandList* cmds[] = { m_CmdList12.Get() };
    m_Queue12->ExecuteCommandLists(1, cmds);
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    if (m_Fence12->GetCompletedValue() < m_FenceValue) {
        m_Fence12->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }

    // Build the exec binding — reused for every frame's dispatch.
    btd.Dispatchable = m_DMLCompiledOp.Get();
    if (FAILED(m_DMLDevice->CreateBindingTable(&btd, IID_PPV_ARGS(&m_DMLBindingTable)))) return false;
    if (m_TempResource) {
        DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, m_TempResource->GetDesc().Width };
        DML_BINDING_DESC tbd  = { DML_BINDING_TYPE_BUFFER, &tb };
        m_DMLBindingTable->BindTemporaryResource(&tbd);
    }
    if (m_PersistentResource) {
        DML_BUFFER_BINDING pb = { m_PersistentResource.Get(), 0, m_PersistentResource->GetDesc().Width };
        DML_BINDING_DESC pbd  = { DML_BINDING_TYPE_BUFFER, &pb };
        m_DMLBindingTable->BindPersistentResource(&pbd);
    }
    // Input bindings are set per-frame by rebindPingPongInputs().
    // Output is static (single tensor), bind it once.
    DML_BUFFER_BINDING out = { m_OutputTensor.Get(), 0, m_TensorBytes };
    DML_BINDING_DESC outDesc = { DML_BINDING_TYPE_BUFFER, &out };
    m_DMLBindingTable->BindOutputs(1, &outDesc);

    return true;
}


bool DirectMLFRUC::rebindPingPongInputs()
{
    // m_WriteSlot == slot holding the newest (curr) upload. The
    // opposite slot still holds the previous frame's upload, so we
    // bind it as "prev" for free.
    int prevSlot = 1 - m_WriteSlot;
    int currSlot = m_WriteSlot;
    DML_BUFFER_BINDING ins[2] = {
        { m_FrameTensor[prevSlot].Get(), 0, m_TensorBytes },
        { m_FrameTensor[currSlot].Get(), 0, m_TensorBytes },
    };
    DML_BINDING_DESC inDescs[2] = {
        { DML_BINDING_TYPE_BUFFER, &ins[0] },
        { DML_BINDING_TYPE_BUFFER, &ins[1] },
    };
    m_DMLBindingTable->BindInputs(2, inDescs);
    return true;
}


bool DirectMLFRUC::createCommandInfra()
{
    // Nothing else to stand up — command list / allocator / queue /
    // fence were already created in createD3D12Device().
    return true;
}


bool DirectMLFRUC::uploadCurrToTensor(ID3D11DeviceContext* ctx)
{
    // D3D11 GPU -> staging read -> CPU map -> FP16 quantize -> D3D12 UPLOAD heap.
    // Only the *curr* frame is ever re-uploaded — prev is the
    // previous frame's curr, sitting in VRAM already (ping-pong).
    ctx->CopyResource(m_StagingRead.Get(), m_RenderTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_StagingRead.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    // UPLOAD heap is persistently mapped (D4 iter 2) — m_UploadCurrPtr
    // is a pointer into write-combined CPU memory that stays valid
    // across frames. Each tensor plane is a contiguous W*H block in
    // NCHW order.
    uint16_t* dst = reinterpret_cast<uint16_t*>(m_UploadCurrPtr);
    const uint32_t plane = m_Width * m_Height;
    for (uint32_t y = 0; y < m_Height; ++y) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        uint32_t off = y * m_Width;
        // D4 iter 4: SIMD path (F16C + SSE4.1) — packs 8 pixels per
        // iteration and folds the 0.5 pre-scale so ADD1 yields the
        // mean directly.
        packRowRgba8ToPlanarFp16(row,
                                 dst + 0 * plane + off,
                                 dst + 1 * plane + off,
                                 dst + 2 * plane + off,
                                 dst + 3 * plane + off,
                                 m_Width);
    }
    ctx->Unmap(m_StagingRead.Get(), 0);
    return true;
}


bool DirectMLFRUC::executeDMLGraph()
{
    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);

    auto transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource   = r;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter  = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList12->ResourceBarrier(1, &b);
    };

    // Only the *curr* slot needs an UPLOAD->DEFAULT copy this frame;
    // the *prev* slot is last frame's upload still sitting in VRAM
    // (D4 iter 3 ping-pong).
    ID3D12Resource* currSlot = m_FrameTensor[m_WriteSlot].Get();
    transition(currSlot, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    m_CmdList12->CopyBufferRegion(currSlot, 0, m_UploadCurr.Get(), 0, m_TensorBytes);
    transition(currSlot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Dispatch the DML operator.
    ID3D12DescriptorHeap* heaps[] = { m_DMLDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, heaps);
    m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), m_DMLCompiledOp.Get(), m_DMLBindingTable.Get());

    // Readback output -> staging for CPU.
    transition(m_OutputTensor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_CmdList12->CopyBufferRegion(m_ReadbackOutput.Get(), 0, m_OutputTensor.Get(), 0, m_TensorBytes);
    transition(m_OutputTensor.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    m_CmdList12->Close();
    ID3D12CommandList* cmds[] = { m_CmdList12.Get() };
    m_Queue12->ExecuteCommandLists(1, cmds);
    m_FenceValue++;
    if (FAILED(m_Queue12->Signal(m_Fence12.Get(), m_FenceValue))) return false;
    if (m_Fence12->GetCompletedValue() < m_FenceValue) {
        m_Fence12->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }
    return true;
}


bool DirectMLFRUC::downloadTensorToOutput(ID3D11DeviceContext* ctx)
{
    // D4 iter 2: READBACK heap is persistently mapped; just read
    // through m_ReadbackOutputPtr, no Map/Unmap trip per frame.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_StagingWrite.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) return false;

    const uint16_t* src = reinterpret_cast<const uint16_t*>(m_ReadbackOutputPtr);
    const uint32_t plane = m_Width * m_Height;
    for (uint32_t y = 0; y < m_Height; ++y) {
        uint8_t* row = reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        uint32_t off = y * m_Width;
        // D4 iter 4: SIMD dequantize + pack to RGBA8.
        unpackRowPlanarFp16ToRgba8(src + 0 * plane + off,
                                   src + 1 * plane + off,
                                   src + 2 * plane + off,
                                   src + 3 * plane + off,
                                   row, m_Width);
    }

    ctx->Unmap(m_StagingWrite.Get(), 0);
    ctx->CopyResource(m_OutputTexture.Get(), m_StagingWrite.Get());
    return true;
}


bool DirectMLFRUC::submitFrame(ID3D11DeviceContext* ctx, double /*timestamp*/)
{
    if (!m_Initialized) return false;

    const LARGE_INTEGER t0 = [] { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x; }();

    // CRITICAL: the renderer has m_RenderRTV still bound as output
    // when it calls submitFrame. D3D11 silently turns any SRV /
    // CopyResource access on a resource that is *also* currently
    // bound as an RTV into a no-op, which shows as flicker (black
    // interp frame alternating with the real frame). Mirrors the
    // same unbind GenericFRUC does at the top of its submitFrame.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);

    // First frame: upload into slot 0 and bail — we need at least
    // two frames before we can interpolate. From frame 1 onward the
    // previous upload is still sitting in the other slot, so each
    // submit pays for only ONE CPU->VRAM tensor upload.
    if (m_FrameCount == 0) {
        m_WriteSlot = 0;
        bool ok = uploadCurrToTensor(ctx);
        if (ok) {
            // Copy UPLOAD -> DEFAULT so slot 0 is populated for the
            // next frame's "prev" binding.
            m_CmdAlloc12->Reset();
            m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
            ID3D12Resource* slot = m_FrameTensor[m_WriteSlot].Get();
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = slot;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_CmdList12->ResourceBarrier(1, &b);
            m_CmdList12->CopyBufferRegion(slot, 0, m_UploadCurr.Get(), 0, m_TensorBytes);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            m_CmdList12->ResourceBarrier(1, &b);
            m_CmdList12->Close();
            ID3D12CommandList* cmds[] = { m_CmdList12.Get() };
            m_Queue12->ExecuteCommandLists(1, cmds);
            m_FenceValue++;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                m_Fence12->SetEventOnCompletion(m_FenceValue, m_FenceEvent);
                WaitForSingleObject(m_FenceEvent, INFINITE);
            }
        }
        m_WriteSlot = 1;  // next frame writes to the other slot
        m_FrameCount++;
        return false;
    }

    bool ok = true;
    ok = ok && uploadCurrToTensor(ctx);   // RGBA8 -> UPLOAD (ping-pong slot)
    ok = ok && rebindPingPongInputs();    // rebind DML inputs prev<->curr
    ok = ok && executeDMLGraph();         // UPLOAD -> curr slot, DML, readback
    ok = ok && downloadTensorToOutput(ctx);

    m_WriteSlot = 1 - m_WriteSlot;  // flip for next frame
    m_FrameCount++;

    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    double dtMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    // EMA over ~16 frames so the stats line stays stable.
    m_LastInterpMs = m_LastInterpMs == 0.0 ? dtMs : (m_LastInterpMs * 0.9375 + dtMs * 0.0625);

    return ok;
}


void DirectMLFRUC::skipFrame(ID3D11DeviceContext* ctx)
{
    // Match GenericFRUC semantics: keep prev in sync with the
    // renderer but do no DML work. With ping-pong we advance by
    // just treating this frame as a curr-upload that never gets
    // consumed — ignore it and leave the last-good tensor pair in
    // place. Bumping m_FrameCount alone is not enough: m_WriteSlot
    // tracks which slot is "newest", so we keep it pointing at the
    // slot that actually has the freshest uploaded tensor.
    if (!m_Initialized) return;
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    m_FrameCount++;
}
