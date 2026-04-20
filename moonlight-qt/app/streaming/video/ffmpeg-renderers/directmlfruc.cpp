// VipleStream: DirectMLFRUC — zero-copy D5 revision.
//
// The D3D12/DML tensor buffers are created with HEAP_FLAG_SHARED,
// forwarded to the renderer's D3D11 device via NT shared handles,
// and driven end-to-end on the GPU with two small D3D11 compute
// shaders (pack / unpack) plus a cross-API fence. No CPU memory
// traffic in the hot path, no staging textures, no SIMD quantise
// loops, no Map/Unmap per frame.
//
// Per-frame sequence on the D3D11 immediate context:
//
//   1. Unbind RTV                     (D3D11)
//   2. Pack CS                        (D3D11): RGBA8 -> shared FP16 buf [slot]
//   3. Signal fence=N                 (D3D11)
//   4. Wait fence=N, DML dispatch,    (D3D12 queue)
//      Signal fence=N+1
//   5. Wait fence=N+1                 (D3D11 ctx)
//   6. Unpack CS                      (D3D11): shared FP16 buf -> RGBA8 texture
//
// The D3D11 Signal/Wait are non-blocking CPU-side — they enqueue
// GPU-side waits on the graphics queue. CPU returns immediately
// after queueing the whole chain.
//
// TODO(directml:model): swap the inline DML_ELEMENT_WISE_ADD1 for
// a compiled ONNX FRUC model — the zero-copy interop is now cheap
// enough to make a real model pay off visually.

#include "directmlfruc.h"
#include "path.h"

#include <SDL.h>

#include <algorithm>
#include <cstring>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using Microsoft::WRL::ComPtr;

namespace {

inline uint64_t alignUp(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

struct PackCBData {
    uint32_t width;
    uint32_t height;
    uint32_t _pad0;
    uint32_t _pad1;
};

} // namespace


DirectMLFRUC::DirectMLFRUC() = default;
DirectMLFRUC::~DirectMLFRUC() { destroy(); }


bool DirectMLFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_Device11 = device;
    m_Width    = width;
    m_Height   = height;
    m_TensorElements = 4u * width * height;
    m_TensorBytes    = m_TensorElements * sizeof(uint16_t);

    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&m_Device11_5)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: renderer lacks ID3D11Device5 (Win10 1703+)");
        return false;
    }

    if (!createD3D12Device())          return false;
    if (!createDMLDevice())            return false;
    if (!createSharedFence())          return false;
    if (!createSharedTensorBuffers())  return false;
    if (!createD3D11Textures())        return false;
    if (!createD3D11Views())           return false;
    if (!loadComputeShaders())         return false;
    if (!compileDMLGraph())            return false;

    m_Initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML (zero-copy) initialized: %ux%u, FP16 tensor %.2f MB/buffer",
                width, height, (double)m_TensorBytes / (1024.0 * 1024.0));
    return true;
}


void DirectMLFRUC::destroy()
{
    // Drain the queue so no in-flight command list references
    // resources we're about to drop.
    if (m_Queue12 && m_Fence12) {
        m_FenceValue++;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
                WaitForSingleObject(ev, 500);
            }
            CloseHandle(ev);
        }
    }

    m_Fence11.Reset();
    m_DMLBindingTable.Reset();
    m_DMLCompiledOp.Reset();
    m_DMLDescHeap.Reset();
    m_DMLRecorder.Reset();
    m_DMLDevice.Reset();
    m_TempResource.Reset();
    m_PersistentResource.Reset();
    m_OutputTensor.Reset();
    m_FrameTensor[0].Reset();
    m_FrameTensor[1].Reset();

    m_FrameBufferUAV11[0].Reset();
    m_FrameBufferUAV11[1].Reset();
    m_FrameBuffer11[0].Reset();
    m_FrameBuffer11[1].Reset();
    m_OutputBufferSRV11.Reset();
    m_OutputBuffer11.Reset();

    m_PackConstBuf.Reset();
    m_PackCS.Reset();
    m_UnpackCS.Reset();

    m_OutputUAV.Reset();
    m_OutputSRV.Reset();
    m_OutputTexture.Reset();
    m_RenderSRV.Reset();
    m_RenderRTV.Reset();
    m_RenderTexture.Reset();

    m_CmdList12.Reset();
    m_CmdAlloc12.Reset();
    m_Fence12.Reset();
    m_Queue12.Reset();
    m_Device12.Reset();

    m_Device11_5.Reset();
    m_Device11 = nullptr;
    m_Initialized = false;
}


bool DirectMLFRUC::createD3D12Device()
{
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;

    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: D3D12CreateDevice failed 0x%08lx", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_Device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_Queue12)))) return false;
    if (FAILED(m_Device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&m_CmdAlloc12)))) return false;
    if (FAILED(m_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             m_CmdAlloc12.Get(), nullptr,
                                             IID_PPV_ARGS(&m_CmdList12)))) return false;
    m_CmdList12->Close();
    return true;
}


bool DirectMLFRUC::createDMLDevice()
{
    HRESULT hr = DMLCreateDevice(m_Device12.Get(), DML_CREATE_DEVICE_FLAG_NONE,
                                 IID_PPV_ARGS(&m_DMLDevice));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: DMLCreateDevice failed 0x%08lx", hr);
        return false;
    }
    return SUCCEEDED(m_DMLDevice->CreateCommandRecorder(IID_PPV_ARGS(&m_DMLRecorder)));
}


bool DirectMLFRUC::createSharedFence()
{
    // Fence must be shared (D3D12) AND cross-adapter=false (single
    // adapter). ID3D11Device5::OpenSharedFence will then hand back
    // an ID3D11Fence that is the same underlying kernel object.
    if (FAILED(m_Device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence12)))) return false;

    HANDLE shared = nullptr;
    if (FAILED(m_Device12->CreateSharedHandle(m_Fence12.Get(), nullptr, GENERIC_ALL, nullptr, &shared))) return false;
    HRESULT hr = m_Device11_5->OpenSharedFence(shared, IID_PPV_ARGS(&m_Fence11));
    CloseHandle(shared);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: OpenSharedFence failed 0x%08lx", hr);
        return false;
    }
    return true;
}


bool DirectMLFRUC::createSharedTensorBuffers()
{
    ComPtr<ID3D11Device1> dev11_1;
    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&dev11_1)))) return false;

    auto makeShared = [&](uint64_t bytes, ComPtr<ID3D12Resource>& d12,
                          ComPtr<ID3D11Buffer>& d11) -> bool
    {
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
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        if (FAILED(m_Device12->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_SHARED, &rd,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&d12)))) return false;

        HANDLE handle = nullptr;
        if (FAILED(m_Device12->CreateSharedHandle(d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) return false;
        HRESULT hr = dev11_1->OpenSharedResource1(handle, IID_PPV_ARGS(&d11));
        CloseHandle(handle);
        return SUCCEEDED(hr);
    };

    if (!makeShared(m_TensorBytes, m_FrameTensor[0], m_FrameBuffer11[0])) return false;
    if (!makeShared(m_TensorBytes, m_FrameTensor[1], m_FrameBuffer11[1])) return false;
    if (!makeShared(m_TensorBytes, m_OutputTensor,   m_OutputBuffer11))   return false;
    return true;
}


bool DirectMLFRUC::createD3D11Textures()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width  = m_Width;
    td.Height = m_Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_RenderTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateRenderTargetView(m_RenderTexture.Get(), nullptr, m_RenderRTV.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_RenderTexture.Get(), nullptr, m_RenderSRV.GetAddressOf()))) return false;

    // Output texture — the unpack CS writes it via UAV; the blit
    // path reads it via SRV. Must have both bind flags.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_OutputTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_OutputTexture.Get(), nullptr, m_OutputSRV.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateUnorderedAccessView(m_OutputTexture.Get(), nullptr, m_OutputUAV.GetAddressOf()))) return false;

    // Constant buffer for the pack/unpack shaders — only width /
    // height need to reach the GPU. Filled once; WRITE_DISCARD
    // update per frame would work too but sizes don't change.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PackCBData);
    cbd.Usage     = D3D11_USAGE_IMMUTABLE;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    PackCBData cbInit = { m_Width, m_Height, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd = { &cbInit, 0, 0 };
    if (FAILED(m_Device11->CreateBuffer(&cbd, &sd, m_PackConstBuf.GetAddressOf()))) return false;
    return true;
}


bool DirectMLFRUC::createD3D11Views()
{
    // Typed UAV on the shared buffer as R16_FLOAT — every element
    // is one FP16, addressing is element index, so the pack
    // shader's `output[idx] = float_value` lowers to a single
    // typed store per write.
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format        = DXGI_FORMAT_R16_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 0;
    ud.Buffer.NumElements  = m_TensorElements;

    if (FAILED(m_Device11->CreateUnorderedAccessView(m_FrameBuffer11[0].Get(), &ud, m_FrameBufferUAV11[0].GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateUnorderedAccessView(m_FrameBuffer11[1].Get(), &ud, m_FrameBufferUAV11[1].GetAddressOf()))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format        = DXGI_FORMAT_R16_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements  = m_TensorElements;
    if (FAILED(m_Device11->CreateShaderResourceView(m_OutputBuffer11.Get(), &sd, m_OutputBufferSRV11.GetAddressOf()))) return false;
    return true;
}


bool DirectMLFRUC::loadComputeShaders()
{
    QByteArray pack = Path::readDataFile("d3d11_dml_pack_rgba8_fp16.fxc");
    if (pack.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: d3d11_dml_pack_rgba8_fp16.fxc missing");
        return false;
    }
    if (FAILED(m_Device11->CreateComputeShader(pack.constData(), pack.size(), nullptr, m_PackCS.GetAddressOf()))) return false;

    QByteArray unpack = Path::readDataFile("d3d11_dml_unpack_fp16_rgba8.fxc");
    if (unpack.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: d3d11_dml_unpack_fp16_rgba8.fxc missing");
        return false;
    }
    if (FAILED(m_Device11->CreateComputeShader(unpack.constData(), unpack.size(), nullptr, m_UnpackCS.GetAddressOf()))) return false;
    return true;
}


bool DirectMLFRUC::compileDMLGraph()
{
    const uint32_t sizes[4]   = { 1u, 4u, m_Height, m_Width };
    const uint32_t strides[4] = { 4u * m_Height * m_Width, m_Height * m_Width, m_Width, 1u };

    DML_BUFFER_TENSOR_DESC bufDesc = {};
    bufDesc.DataType       = kDmlDtype;
    bufDesc.DimensionCount = 4;
    bufDesc.Sizes          = sizes;
    bufDesc.Strides        = strides;
    bufDesc.TotalTensorSizeInBytes = m_TensorBytes;

    DML_TENSOR_DESC td = { DML_TENSOR_TYPE_BUFFER, &bufDesc };
    DML_ELEMENT_WISE_ADD1_OPERATOR_DESC addDesc = {};
    addDesc.ATensor      = &td;
    addDesc.BTensor      = &td;
    addDesc.OutputTensor = &td;
    DML_OPERATOR_DESC opDesc = { DML_OPERATOR_ELEMENT_WISE_ADD1, &addDesc };

    ComPtr<IDMLOperator> op;
    if (FAILED(m_DMLDevice->CreateOperator(&opDesc, IID_PPV_ARGS(&op)))) return false;
    if (FAILED(m_DMLDevice->CompileOperator(op.Get(),
            DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
            IID_PPV_ARGS(&m_DMLCompiledOp)))) return false;

    DML_BINDING_PROPERTIES execProps = m_DMLCompiledOp->GetBindingProperties();

    ComPtr<IDMLOperatorInitializer> initializer;
    IDMLCompiledOperator* opsToInit[] = { m_DMLCompiledOp.Get() };
    if (FAILED(m_DMLDevice->CreateOperatorInitializer(1, opsToInit, IID_PPV_ARGS(&initializer)))) return false;
    DML_BINDING_PROPERTIES initProps = initializer->GetBindingProperties();

    uint32_t descCount = std::max(initProps.RequiredDescriptorCount, execProps.RequiredDescriptorCount);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = descCount;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_Device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DMLDescHeap)))) return false;

    auto allocBuffer = [&](uint64_t bytes, ComPtr<ID3D12Resource>& out) -> bool {
        if (bytes == 0) { out.Reset(); return true; }
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
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
    uint64_t tempBytes = std::max(initProps.TemporaryResourceSize, execProps.TemporaryResourceSize);
    if (!allocBuffer(tempBytes, m_TempResource)) return false;
    if (!allocBuffer(execProps.PersistentResourceSize, m_PersistentResource)) return false;

    // --- Initializer dispatch ---
    DML_BINDING_TABLE_DESC btd = {};
    btd.Dispatchable        = initializer.Get();
    btd.CPUDescriptorHandle = m_DMLDescHeap->GetCPUDescriptorHandleForHeapStart();
    btd.GPUDescriptorHandle = m_DMLDescHeap->GetGPUDescriptorHandleForHeapStart();
    btd.SizeInDescriptors   = descCount;
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
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }

    // --- Exec binding (inputs rebound per frame) ---
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
    DML_BUFFER_BINDING out = { m_OutputTensor.Get(), 0, m_TensorBytes };
    DML_BINDING_DESC outDesc = { DML_BINDING_TYPE_BUFFER, &out };
    m_DMLBindingTable->BindOutputs(1, &outDesc);
    return true;
}


bool DirectMLFRUC::rebindPingPongInputs()
{
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


void DirectMLFRUC::runPackCS(ID3D11DeviceContext4* ctx4, int slot)
{
    ID3D11ShaderResourceView*   nullSRVs[1] = { nullptr };
    ID3D11UnorderedAccessView*  nullUAVs[1] = { nullptr };

    // Bind pack CS: render texture as input SRV, shared tensor
    // slot as output UAV, const buffer for {width, height}.
    ctx4->CSSetShader(m_PackCS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = m_RenderSRV.Get();
    ctx4->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uav = m_FrameBufferUAV11[slot].Get();
    UINT initialCounts = 0;
    ctx4->CSSetUnorderedAccessViews(0, 1, &uav, &initialCounts);
    ID3D11Buffer* cb = m_PackConstBuf.Get();
    ctx4->CSSetConstantBuffers(0, 1, &cb);

    UINT gx = (m_Width  + 15) / 16;
    UINT gy = (m_Height + 15) / 16;
    ctx4->Dispatch(gx, gy, 1);

    // Unbind the UAV so D3D12 can see the writes. D3D11 flushes
    // pending UAV writes to VRAM on the next GPU-side barrier; the
    // fence signal that follows enforces that ordering.
    ctx4->CSSetUnorderedAccessViews(0, 1, nullUAVs, &initialCounts);
    ctx4->CSSetShaderResources(0, 1, nullSRVs);
}


bool DirectMLFRUC::runDMLDispatch()
{
    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = { m_DMLDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, heaps);
    m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), m_DMLCompiledOp.Get(), m_DMLBindingTable.Get());
    m_CmdList12->Close();
    ID3D12CommandList* cmds[] = { m_CmdList12.Get() };
    m_Queue12->ExecuteCommandLists(1, cmds);
    return true;
}


void DirectMLFRUC::runUnpackCS(ID3D11DeviceContext4* ctx4)
{
    ID3D11ShaderResourceView*   nullSRVs[1] = { nullptr };
    ID3D11UnorderedAccessView*  nullUAVs[1] = { nullptr };
    UINT initialCounts = 0;

    ctx4->CSSetShader(m_UnpackCS.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = m_OutputBufferSRV11.Get();
    ctx4->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uav = m_OutputUAV.Get();
    ctx4->CSSetUnorderedAccessViews(0, 1, &uav, &initialCounts);
    ID3D11Buffer* cb = m_PackConstBuf.Get();
    ctx4->CSSetConstantBuffers(0, 1, &cb);

    UINT gx = (m_Width  + 15) / 16;
    UINT gy = (m_Height + 15) / 16;
    ctx4->Dispatch(gx, gy, 1);

    ctx4->CSSetUnorderedAccessViews(0, 1, nullUAVs, &initialCounts);
    ctx4->CSSetShaderResources(0, 1, nullSRVs);
}


bool DirectMLFRUC::submitFrame(ID3D11DeviceContext* ctx, double /*timestamp*/)
{
    if (!m_Initialized) return false;

    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

    // Pack/unpack require ID3D11DeviceContext4 for Signal/Wait.
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return false;

    // Unbind RTV — same reason as v1.1.143; the render path leaves
    // the FRUC RTV attached when it calls us.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx4->OMSetRenderTargets(1, &nullRTV, nullptr);

    // First frame: pack into slot 0 and bail. The write becomes
    // visible to D3D12 (for next frame's DML read) via the fence
    // signal that we emit here.
    if (m_FrameCount == 0) {
        runPackCS(ctx4.Get(), 0);
        m_FenceValue++;
        ctx4->Signal(m_Fence11.Get(), m_FenceValue);
        m_Queue12->Wait(m_Fence12.Get(), m_FenceValue);
        m_WriteSlot = 1;
        m_FrameCount++;
        return false;
    }

    // Pack current render into the write slot and make D3D12 wait
    // for it before dispatching DML.
    runPackCS(ctx4.Get(), m_WriteSlot);
    m_FenceValue++;
    ctx4->Signal(m_Fence11.Get(), m_FenceValue);
    m_Queue12->Wait(m_Fence12.Get(), m_FenceValue);

    rebindPingPongInputs();
    runDMLDispatch();
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    ctx4->Wait(m_Fence11.Get(), m_FenceValue);

    runUnpackCS(ctx4.Get());

    m_WriteSlot = 1 - m_WriteSlot;
    m_FrameCount++;

    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    double dtMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    m_LastInterpMs = (m_LastInterpMs == 0.0) ? dtMs : (m_LastInterpMs * 0.9375 + dtMs * 0.0625);
    return true;
}


void DirectMLFRUC::skipFrame(ID3D11DeviceContext* ctx)
{
    if (!m_Initialized) return;
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    m_FrameCount++;
}
