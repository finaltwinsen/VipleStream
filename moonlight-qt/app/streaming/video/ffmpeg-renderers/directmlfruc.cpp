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
// FP16 helpers. Windows has a _mm_cvt* intrinsic path via F16C but
// not every target CPU exposes it; the scalar version below matches
// IEEE 754 binary16 and is plenty for 1080p @ 60 fps (a few ms on
// the CPU side while the GPU is doing heavier work).
// ---------------------------------------------------------------

inline uint16_t floatToHalf(float f)
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

inline float halfToFloat(uint16_t h)
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

// Align x up to multiple of a (a must be power of two).
inline uint64_t alignUp(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

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

    m_PrevTensor.Reset();
    m_CurrTensor.Reset();
    m_OutputTensor.Reset();
    m_TempResource.Reset();
    m_PersistentResource.Reset();
    m_UploadPrev.Reset();
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
    m_PrevTexture.Reset();
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

    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_PrevTexture.GetAddressOf()))) return false;

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

    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_PrevTensor)) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_CurrTensor)) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_DEFAULT,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_OutputTensor)) return false;

    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ,
                      D3D12_RESOURCE_FLAG_NONE, m_UploadPrev)) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ,
                      D3D12_RESOURCE_FLAG_NONE, m_UploadCurr)) return false;
    if (!makeResource(m_TensorBytes, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_FLAG_NONE, m_ReadbackOutput)) return false;

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
    DML_BUFFER_BINDING ins[2] = {
        { m_PrevTensor.Get(), 0, m_TensorBytes },
        { m_CurrTensor.Get(), 0, m_TensorBytes },
    };
    DML_BINDING_DESC inDescs[2] = {
        { DML_BINDING_TYPE_BUFFER, &ins[0] },
        { DML_BINDING_TYPE_BUFFER, &ins[1] },
    };
    m_DMLBindingTable->BindInputs(2, inDescs);
    DML_BUFFER_BINDING out = { m_OutputTensor.Get(), 0, m_TensorBytes };
    DML_BINDING_DESC outDesc = { DML_BINDING_TYPE_BUFFER, &out };
    m_DMLBindingTable->BindOutputs(1, &outDesc);

    return true;
}


bool DirectMLFRUC::createCommandInfra()
{
    // Nothing else to stand up — command list / allocator / queue /
    // fence were already created in createD3D12Device().
    return true;
}


bool DirectMLFRUC::uploadRenderToTensor(ID3D11DeviceContext* ctx, bool isPrev)
{
    // D3D11 GPU -> staging read -> CPU map -> FP16 quantize -> D3D12 upload buffer.
    ID3D11Texture2D* src = isPrev ? m_PrevTexture.Get() : m_RenderTexture.Get();
    ctx->CopyResource(m_StagingRead.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_StagingRead.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    // Upload buffer is a flat FP16 NCHW layout ([1,4,H,W]) — plane
    // 0 = R, 1 = G, 2 = B, 3 = A. Row pitch inside an ID3D11Texture2D
    // staging copy may be > 4*Width so we walk row by row.
    ComPtr<ID3D12Resource> upload = isPrev ? m_UploadPrev : m_UploadCurr;
    void* uploadPtr = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(upload->Map(0, &readRange, &uploadPtr))) {
        ctx->Unmap(m_StagingRead.Get(), 0);
        return false;
    }
    uint16_t* dst = reinterpret_cast<uint16_t*>(uploadPtr);
    const uint32_t plane = m_Width * m_Height;
    const float inv255 = 1.0f / 255.0f;
    for (uint32_t y = 0; y < m_Height; ++y) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (uint32_t x = 0; x < m_Width; ++x) {
            float r = row[x*4 + 0] * inv255;
            float g = row[x*4 + 1] * inv255;
            float b = row[x*4 + 2] * inv255;
            float a = row[x*4 + 3] * inv255;
            uint32_t idx = y * m_Width + x;
            dst[0 * plane + idx] = floatToHalf(r);
            dst[1 * plane + idx] = floatToHalf(g);
            dst[2 * plane + idx] = floatToHalf(b);
            dst[3 * plane + idx] = floatToHalf(a);
        }
    }
    D3D12_RANGE writeRange = { 0, m_TensorBytes };
    upload->Unmap(0, &writeRange);
    ctx->Unmap(m_StagingRead.Get(), 0);

    // Copy upload -> device-local tensor on the D3D12 queue. We fuse
    // this with the DML dispatch in executeDMLGraph so there is a
    // single ExecuteCommandLists + Signal per frame.
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

    // Upload -> device-local copy for both input tensors.
    transition(m_PrevTensor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(m_CurrTensor.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    m_CmdList12->CopyBufferRegion(m_PrevTensor.Get(), 0, m_UploadPrev.Get(), 0, m_TensorBytes);
    m_CmdList12->CopyBufferRegion(m_CurrTensor.Get(), 0, m_UploadCurr.Get(), 0, m_TensorBytes);
    transition(m_PrevTensor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(m_CurrTensor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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
    // Readback -> CPU -> dequantize -> D3D11 staging -> m_OutputTexture.
    void* readPtr = nullptr;
    D3D12_RANGE readRange = { 0, m_TensorBytes };
    if (FAILED(m_ReadbackOutput->Map(0, &readRange, &readPtr))) return false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_StagingWrite.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
        D3D12_RANGE noWrite = { 0, 0 };
        m_ReadbackOutput->Unmap(0, &noWrite);
        return false;
    }

    const uint16_t* src = reinterpret_cast<const uint16_t*>(readPtr);
    const uint32_t plane = m_Width * m_Height;
    for (uint32_t y = 0; y < m_Height; ++y) {
        uint8_t* row = reinterpret_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (uint32_t x = 0; x < m_Width; ++x) {
            uint32_t idx = y * m_Width + x;
            float r = halfToFloat(src[0 * plane + idx]);
            float g = halfToFloat(src[1 * plane + idx]);
            float b = halfToFloat(src[2 * plane + idx]);
            float a = halfToFloat(src[3 * plane + idx]);
            auto q = [](float v) {
                int i = (int)(v * 255.0f + 0.5f);
                if (i < 0) i = 0; if (i > 255) i = 255;
                return (uint8_t)i;
            };
            row[x*4 + 0] = q(r);
            row[x*4 + 1] = q(g);
            row[x*4 + 2] = q(b);
            row[x*4 + 3] = q(a);
        }
    }

    ctx->Unmap(m_StagingWrite.Get(), 0);
    D3D12_RANGE noWrite = { 0, 0 };
    m_ReadbackOutput->Unmap(0, &noWrite);

    ctx->CopyResource(m_OutputTexture.Get(), m_StagingWrite.Get());
    return true;
}


bool DirectMLFRUC::submitFrame(ID3D11DeviceContext* ctx, double /*timestamp*/)
{
    if (!m_Initialized) return false;

    const LARGE_INTEGER t0 = [] { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x; }();

    // First frame: just snapshot into prev and bail — we need at
    // least two frames before we can interpolate.
    if (m_FrameCount == 0) {
        ctx->CopyResource(m_PrevTexture.Get(), m_RenderTexture.Get());
        m_FrameCount++;
        return false;
    }

    bool ok = true;
    ok = ok && uploadRenderToTensor(ctx, /*isPrev=*/true);   // m_PrevTexture -> prev tensor
    ok = ok && uploadRenderToTensor(ctx, /*isPrev=*/false);  // m_RenderTexture -> curr tensor
    ok = ok && executeDMLGraph();
    ok = ok && downloadTensorToOutput(ctx);

    // Advance prev <- curr for next frame.
    ctx->CopyResource(m_PrevTexture.Get(), m_RenderTexture.Get());
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
    // Match GenericFRUC semantics: maintain prev in sync so the next
    // interpolated frame has a correct reference, but do no DML
    // work.
    if (!m_Initialized) return;
    ctx->CopyResource(m_PrevTexture.Get(), m_RenderTexture.Get());
    m_FrameCount++;
}
