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
#include <QFileInfo>

#include <algorithm>
#include <cstring>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

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
    uint32_t useAlpha;  // unpack: 0 = force 1.0 (3-ch models), 1 = read plane 3
    uint32_t _pad1;
};

// ORT env is process-global and expensive to construct (spins up a
// thread pool, parses schema). Share across all DirectMLFRUC
// instances; first call lazy-initialises. Never torn down —
// ONNX Runtime's atexit handlers manage the lifetime. Constructing
// more than one Env in a single process is explicitly supported but
// wastes memory.
Ort::Env& sharedOrtEnv()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "VipleStream.DirectML");
    return env;
}

} // namespace


DirectMLFRUC::DirectMLFRUC() = default;
DirectMLFRUC::~DirectMLFRUC() { destroy(); }


bool DirectMLFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_Device11 = device;
    m_Width    = width;
    m_Height   = height;
    m_TensorElements = 4u * width * height;
    m_TensorBytes    = m_TensorElements * sizeof(float);  // FP32 internally

    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&m_Device11_5)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: renderer lacks ID3D11Device5 (Win10 1703+)");
        return false;
    }

#define DML_INIT_STEP(name, expr) \
    do { if (!(expr)) { SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, \
         "[VIPLE-FRUC] DirectML: init step '" name "' failed"); return false; } } while(0)

    DML_INIT_STEP("createD3D12Device",         createD3D12Device());
    DML_INIT_STEP("createDMLDevice",           createDMLDevice());
    DML_INIT_STEP("createSharedFence",         createSharedFence());
    DML_INIT_STEP("createSharedTensorBuffers", createSharedTensorBuffers());
    DML_INIT_STEP("createD3D11Textures",       createD3D11Textures());
    DML_INIT_STEP("createD3D11Views",          createD3D11Views());
    DML_INIT_STEP("loadComputeShaders",        loadComputeShaders());

#undef DML_INIT_STEP

    // Try the ONNX model first. Falls back to the inline DML graph
    // silently if no fruc.onnx is on disk — that's the default
    // path for users who haven't dropped in a model.
    bool ortLoaded = tryLoadOnnxModel();
    if (!ortLoaded) {
        // ORT session creation/teardown may have submitted work to
        // our D3D12 queue or left the command allocator in a non-
        // reset state. Drain the queue and recreate the command
        // infra before compiling the fallback inline DML graph.
        {
            m_FenceValue++;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
                    WaitForSingleObject(ev, 500);
                    CloseHandle(ev);
                }
            }
            // Recreate the command allocator + list to guarantee a
            // clean slate regardless of what ORT may have done.
            m_CmdList12.Reset();
            m_CmdAlloc12.Reset();
            if (FAILED(m_Device12->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CmdAlloc12)))) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: cmd allocator recreate failed");
                return false;
            }
            if (FAILED(m_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    m_CmdAlloc12.Get(), nullptr, IID_PPV_ARGS(&m_CmdList12)))) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: cmd list recreate failed");
                return false;
            }
            m_CmdList12->Close();
        }
        if (!compileDMLGraph()) return false;
    }

    m_Initialized = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML (zero-copy, %s) initialized: %ux%u, FP32 tensor %.2f MB/buffer",
                m_UseOrt ? "ONNX model" : "inline graph",
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

    // ORT allocations + session must tear down BEFORE we release
    // the D3D12 resources / DML device they reference. FreeGPUAllocation
    // decrements the ref count on the wrapped ID3D12Resource.
    if (m_OrtDmlApi) {
        if (m_OrtAllocFrame[0]) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocFrame[0]);
        if (m_OrtAllocFrame[1]) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocFrame[1]);
        if (m_OrtAllocOutput)   m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocOutput);
        if (m_OrtAllocTimestep) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocTimestep);
        m_OrtAllocFrame[0] = m_OrtAllocFrame[1] = m_OrtAllocOutput = m_OrtAllocTimestep = nullptr;
    }
    m_TimestepResource.Reset();
    m_HasTimestep = false;
    m_TimestepShape.clear();
    m_ModelChannels = 4;
    m_OrtSession.reset();
    m_OrtInputNames.clear();
    m_OrtOutputNames.clear();
    m_OrtInputNamesCStr.clear();
    m_OrtOutputNamesCStr.clear();
    m_OrtDmlApi = nullptr;
    m_UseOrt = false;

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

    // Constant buffer for the pack/unpack shaders. useAlpha
    // defaults to 1 (4-channel I/O: inline graph or 4-ch model).
    // tryLoadOnnxModel() flips it to 0 and re-uploads when it
    // detects a 3-channel model output.
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(PackCBData);
    cbd.Usage     = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    PackCBData cbInit = { m_Width, m_Height, /*useAlpha=*/1, 0 };
    D3D11_SUBRESOURCE_DATA sd = { &cbInit, 0, 0 };
    if (FAILED(m_Device11->CreateBuffer(&cbd, &sd, m_PackConstBuf.GetAddressOf()))) return false;
    return true;
}


bool DirectMLFRUC::createD3D11Views()
{
    // Typed UAV on the shared buffer as R32_FLOAT — FP32 storage
    // so ORT can bind our buffer directly to any public RIFE /
    // FLAVR / IFRNet model (all of which ship as FP32 ONNX). The
    // HLSL pack/unpack shaders use `RWBuffer<float>` / `Buffer<float>`
    // which is format-agnostic; swapping the view format is all
    // that's required to flip the on-disk dtype.
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format        = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 0;
    ud.Buffer.NumElements  = m_TensorElements;

    if (FAILED(m_Device11->CreateUnorderedAccessView(m_FrameBuffer11[0].Get(), &ud, m_FrameBufferUAV11[0].GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateUnorderedAccessView(m_FrameBuffer11[1].Get(), &ud, m_FrameBufferUAV11[1].GetAddressOf()))) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format        = DXGI_FORMAT_R32_FLOAT;
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

    // The pack shader now writes pixel values in [0, 1] (matching
    // what ONNX models expect). The inline 4-op graph below re-
    // introduces the 0.5 scale via IDENTITY nodes so ADD1 still
    // produces a bidirectional mean.
    DML_SCALE_BIAS half = { 0.5f, 0.0f };

    // Node 0 / 1: IDENTITY with Scale=0.5 on each input.
    DML_ELEMENT_WISE_IDENTITY_OPERATOR_DESC idDesc = {};
    idDesc.InputTensor  = &td;
    idDesc.OutputTensor = &td;
    idDesc.ScaleBias    = &half;
    DML_OPERATOR_DESC idOpDesc = { DML_OPERATOR_ELEMENT_WISE_IDENTITY, &idDesc };
    ComPtr<IDMLOperator> idA, idB;
    if (FAILED(m_DMLDevice->CreateOperator(&idOpDesc, IID_PPV_ARGS(&idA)))) return false;
    if (FAILED(m_DMLDevice->CreateOperator(&idOpDesc, IID_PPV_ARGS(&idB)))) return false;

    // Node 2: ADD1(halfA, halfB) — bidirectional mean.
    DML_ELEMENT_WISE_ADD1_OPERATOR_DESC addDesc = {};
    addDesc.ATensor      = &td;
    addDesc.BTensor      = &td;
    addDesc.OutputTensor = &td;
    DML_OPERATOR_DESC addOpDesc = { DML_OPERATOR_ELEMENT_WISE_ADD1, &addDesc };
    ComPtr<IDMLOperator> addOp;
    if (FAILED(m_DMLDevice->CreateOperator(&addOpDesc, IID_PPV_ARGS(&addOp)))) return false;

    // Node 3: CLIP(x, 0, 1) — clamps rounding drift at the tails.
    DML_ELEMENT_WISE_CLIP_OPERATOR_DESC clipDesc = {};
    clipDesc.InputTensor  = &td;
    clipDesc.OutputTensor = &td;
    clipDesc.Min          = 0.0f;
    clipDesc.Max          = 1.0f;
    DML_OPERATOR_DESC clipOpDesc = { DML_OPERATOR_ELEMENT_WISE_CLIP, &clipDesc };
    ComPtr<IDMLOperator> clipOp;
    if (FAILED(m_DMLDevice->CreateOperator(&clipOpDesc, IID_PPV_ARGS(&clipOp)))) return false;

    DML_OPERATOR_GRAPH_NODE_DESC idANode = { idA.Get(),   "idA"  };
    DML_OPERATOR_GRAPH_NODE_DESC idBNode = { idB.Get(),   "idB"  };
    DML_OPERATOR_GRAPH_NODE_DESC addNode = { addOp.Get(), "add"  };
    DML_OPERATOR_GRAPH_NODE_DESC clipNode= { clipOp.Get(),"clip" };
    DML_GRAPH_NODE_DESC nodes[4] = {
        { DML_GRAPH_NODE_TYPE_OPERATOR, &idANode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &idBNode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &addNode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &clipNode },
    };

    DML_INPUT_GRAPH_EDGE_DESC inEdges[2] = {
        { 0u, 0u, 0u, "A" },   // graph input 0 -> idA.in
        { 1u, 1u, 0u, "B" },   // graph input 1 -> idB.in
    };
    DML_GRAPH_EDGE_DESC inEdgeDescs[2] = {
        { DML_GRAPH_EDGE_TYPE_INPUT, &inEdges[0] },
        { DML_GRAPH_EDGE_TYPE_INPUT, &inEdges[1] },
    };
    DML_INTERMEDIATE_GRAPH_EDGE_DESC midEdges[3] = {
        { 0u, 0u, 2u, 0u, "idA->add.A" },
        { 1u, 0u, 2u, 1u, "idB->add.B" },
        { 2u, 0u, 3u, 0u, "add->clip"  },
    };
    DML_GRAPH_EDGE_DESC midEdgeDescs[3] = {
        { DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &midEdges[0] },
        { DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &midEdges[1] },
        { DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &midEdges[2] },
    };
    DML_OUTPUT_GRAPH_EDGE_DESC outEdge = { 3u, 0u, 0u, "clip->out" };
    DML_GRAPH_EDGE_DESC outEdgeDesc = { DML_GRAPH_EDGE_TYPE_OUTPUT, &outEdge };

    DML_GRAPH_DESC gd = {};
    gd.InputCount            = 2;
    gd.OutputCount           = 1;
    gd.NodeCount             = 4;
    gd.Nodes                 = nodes;
    gd.InputEdgeCount        = 2;
    gd.InputEdges            = inEdgeDescs;
    gd.OutputEdgeCount       = 1;
    gd.OutputEdges           = &outEdgeDesc;
    gd.IntermediateEdgeCount = 3;
    gd.IntermediateEdges     = midEdgeDescs;

    ComPtr<IDMLDevice1> device1;
    if (FAILED(m_DMLDevice.As(&device1))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: IDMLDevice1 not available (need DML 1.1 / Win10 2004+)");
        return false;
    }
    if (FAILED(device1->CompileGraph(&gd,
            DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION,
            IID_PPV_ARGS(&m_DMLCompiledOp)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CompileGraph failed (add1 + clip)");
        return false;
    }

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
    if (m_UseOrt) {
        bool ok = runOrtInference();
        if (!ok) {
            // ORT inference failed (e.g. shape mismatch, driver error).
            // m_UseOrt was already cleared inside runOrtInference().
            // If we have no inline graph to fall back to, return false
            // so the frame is silently dropped rather than crashing.
            if (!m_DMLCompiledOp) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ORT inference failed and no inline DML graph "
                            "is available — frame dropped. Check model resolution alignment.");
                return false;
            }
            // Inline graph available (compiled on init before ORT loaded
            // or after ORT failed). Fall through to the inline path.
        } else {
            return true;
        }
    }

    if (!m_DMLCompiledOp) return false;  // should never happen; guard anyway

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


bool DirectMLFRUC::tryLoadOnnxModel()
{
    // Users opt in to the ONNX path by dropping fruc.onnx into the
    // app data directory (where the shader .fxc files live). No
    // model present = silently use the inline DML graph.
    QString modelPath = Path::getDataFilePath(QStringLiteral("fruc.onnx"));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] No fruc.onnx at '%s' — using inline DML graph. "
                    "Drop a 3-or-4-channel FP32 ONNX FRUC model there to enable the ORT path.",
                    modelPath.isEmpty() ? "(data dir)" : qPrintable(modelPath));
        return false;
    }

    try {
        // Grab the DirectML EP API table.
        const OrtApi& ortApi = Ort::GetApi();
        if (auto* st = ortApi.GetExecutionProviderApi("DML", ORT_API_VERSION,
                                                      reinterpret_cast<const void**>(&m_OrtDmlApi));
            st != nullptr) {
            ortApi.ReleaseStatus(st);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: ORT GetExecutionProviderApi(DML) failed");
            return false;
        }

        Ort::SessionOptions so;
        so.DisableMemPattern();
        so.SetExecutionMode(ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Route ORT onto our existing D3D12 queue and DML device so
        // everything shares the same fence timeline.
        if (auto* st = m_OrtDmlApi->SessionOptionsAppendExecutionProvider_DML1(
                so, m_DMLDevice.Get(), m_Queue12.Get());
            st != nullptr) {
            ortApi.ReleaseStatus(st);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: ORT SessionOptionsAppendExecutionProvider_DML1 failed");
            return false;
        }

#ifdef _WIN32
        std::wstring wpath = modelPath.toStdWString();
        m_OrtSession = std::make_unique<Ort::Session>(sharedOrtEnv(), wpath.c_str(), so);
#else
        std::string apath = modelPath.toStdString();
        m_OrtSession = std::make_unique<Ort::Session>(sharedOrtEnv(), apath.c_str(), so);
#endif

        // Validate shapes. Accepted contract for v1.2.4:
        //   Inputs: 2 or 3 FP32 tensors. First two are NCHW images
        //           [1, 3 or 4, H, W]. Third (if present) is a
        //           timestep — any FP32 tensor whose element count
        //           is 1 qualifies. RIFE v4+ exports this as [1],
        //           FLAVR variants as [1,1,1,1], some as [1,1,H/k,W/k].
        //   Output: 1 FP32 tensor [1, 3 or 4, H, W] with matching
        //           channel count + spatial dims.
        const size_t inCount  = m_OrtSession->GetInputCount();
        const size_t outCount = m_OrtSession->GetOutputCount();
        // Accept ≥1 outputs so models that export auxiliary tensors
        // (optical-flow maps, confidence maps) aren't rejected — we
        // only use the first output regardless.
        if ((inCount != 2 && inCount != 3) || outCount < 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] ONNX: want 2 or 3 inputs + ≥1 output (got %zu/%zu)",
                        inCount, outCount);
            m_OrtSession.reset();
            return false;
        }
        if (outCount > 1) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] ONNX: model has %zu outputs — using only the first",
                        outCount);
        }

        Ort::AllocatorWithDefaultOptions alloc;
        uint32_t detectedChannels = 0;
        auto validateImage = [&](Ort::TypeInfo ti, const char* kind) -> bool {
            auto info  = ti.GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            auto dtype = info.GetElementType();
            if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: expected FP32 (got dtype=%d). "
                            "Convert FP16 models with onnxconverter_common.convert_float_to_float16.",
                            kind, dtype);
                return false;
            }
            if (shape.size() != 4) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: expected 4D NCHW (got %zu)", kind, shape.size());
                return false;
            }
            // Spatial dims must match stream resolution exactly (no
            // padding handling yet — that's a follow-up round).
            if ((shape[2] > 0 && shape[2] != (int64_t)m_Height) ||
                (shape[3] > 0 && shape[3] != (int64_t)m_Width)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: H/W mismatch (need [*,C,%u,%u])", kind,
                            m_Height, m_Width);
                return false;
            }
            // Channel count: 3 or 4. First image input sets the
            // detected count; everything else must agree.
            int64_t c = shape[1];
            if (c > 0 && c != 3 && c != 4) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: only 3 or 4 channels supported (got %lld)",
                            kind, (long long)c);
                return false;
            }
            if (c > 0) {
                if (detectedChannels == 0) {
                    detectedChannels = (uint32_t)c;
                } else if ((uint32_t)c != detectedChannels) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC] ONNX %s: channel mismatch (was %u, got %lld)",
                                kind, detectedChannels, (long long)c);
                    return false;
                }
            }
            return true;
        };
        for (size_t i = 0; i < 2; ++i) {
            if (!validateImage(m_OrtSession->GetInputTypeInfo(i), "image input")) {
                m_OrtSession.reset(); return false;
            }
            Ort::AllocatedStringPtr n = m_OrtSession->GetInputNameAllocated(i, alloc);
            m_OrtInputNames.emplace_back(n.get());
        }
        // Third input = timestep scalar (RIFE v4+). We accept any
        // FP32 tensor whose total element count resolves to 1 at
        // runtime (shape [1], [1,1,1,1], etc.). Dynamic dims (-1)
        // are treated as 1 for storage purposes. We feed 0.5.
        if (inCount == 3) {
            auto ti   = m_OrtSession->GetInputTypeInfo(2);
            auto info = ti.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX timestep input: expected FP32 (got %d)",
                            (int)info.GetElementType());
                m_OrtSession.reset(); return false;
            }
            // Materialise the shape: fold dynamic (-1) dims to 1.
            auto rawShape = info.GetShape();
            m_TimestepShape.clear();
            int64_t tsElems = 1;
            for (int64_t d : rawShape) {
                int64_t concrete = (d > 0) ? d : 1;
                m_TimestepShape.push_back(concrete);
                tsElems *= concrete;
            }
            if (m_TimestepShape.empty()) {
                // Scalar output with no shape dims — treat as [1].
                m_TimestepShape.push_back(1);
                tsElems = 1;
            }
            if (tsElems != 1) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX timestep: expected single-element tensor "
                            "(got %lld elements) — use a model with a scalar timestep input",
                            (long long)tsElems);
                m_OrtSession.reset(); return false;
            }
            m_HasTimestep = true;
            Ort::AllocatedStringPtr n = m_OrtSession->GetInputNameAllocated(2, alloc);
            m_OrtInputNames.emplace_back(n.get());
        }
        if (!validateImage(m_OrtSession->GetOutputTypeInfo(0), "output")) {
            m_OrtSession.reset(); return false;
        }
        {
            Ort::AllocatedStringPtr n = m_OrtSession->GetOutputNameAllocated(0, alloc);
            m_OrtOutputNames.emplace_back(n.get());
        }
        for (auto& s : m_OrtInputNames)  m_OrtInputNamesCStr.push_back(s.c_str());
        for (auto& s : m_OrtOutputNames) m_OrtOutputNamesCStr.push_back(s.c_str());

        // Default detectedChannels to 4 if every dim was dynamic
        // (model didn't fix channel at export). That preserves the
        // 4-channel output path for RGBA pipelines.
        m_ModelChannels = detectedChannels ? detectedChannels : 4;

        // 3-channel models leave plane 3 of our output buffer
        // untouched; flip useAlpha=0 so the unpack shader writes
        // alpha=1 instead of reading stale data.
        if (m_ModelChannels == 3) {
            PackCBData cbData = { m_Width, m_Height, 0, 0 };
            ComPtr<ID3D11DeviceContext> ctx;
            m_Device11->GetImmediateContext(&ctx);
            ctx->UpdateSubresource(m_PackConstBuf.Get(), 0, nullptr, &cbData, 0, 0);
        }

        // Allocate the timestep resource (tiny FP32 buffer, 16 B
        // for alignment) and wrap it as an ORT allocation.
        if (m_HasTimestep) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 256;  // min alignment for buffer
            rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags  = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(m_Device12->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&m_TimestepResource)))) {
                m_OrtSession.reset(); return false;
            }
            // Upload 0.5f into the first 4 bytes via a transient
            // UPLOAD heap + CopyBufferRegion on the queue.
            D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            ComPtr<ID3D12Resource> up;
            D3D12_RESOURCE_DESC urd = rd;
            urd.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(m_Device12->CreateCommittedResource(
                    &uhp, D3D12_HEAP_FLAG_NONE, &urd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&up)))) {
                m_OrtSession.reset(); return false;
            }
            void* mapped = nullptr;
            D3D12_RANGE noRead = { 0, 0 };
            up->Map(0, &noRead, &mapped);
            *reinterpret_cast<float*>(mapped) = m_TimestepValue;
            D3D12_RANGE written = { 0, sizeof(float) };
            up->Unmap(0, &written);

            m_CmdAlloc12->Reset();
            m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
            m_CmdList12->CopyBufferRegion(m_TimestepResource.Get(), 0, up.Get(), 0, sizeof(float));
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = m_TimestepResource.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_CmdList12->ResourceBarrier(1, &b);
            m_CmdList12->Close();
            ID3D12CommandList* cmds[] = { m_CmdList12.Get() };
            m_Queue12->ExecuteCommandLists(1, cmds);
            m_FenceValue++;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            // Small sync (timestep is persistent, only uploaded once).
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
                WaitForSingleObject(ev, 500);
            }
            CloseHandle(ev);

            if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                    m_TimestepResource.Get(), &m_OrtAllocTimestep);
                st != nullptr) {
                ortApi.ReleaseStatus(st);
                m_OrtSession.reset(); return false;
            }
        }

        // Wrap our shared D3D12 tensors as DML allocations. These
        // don't copy memory; ORT holds a reference to the resource
        // and binds it directly as tensor memory.
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(m_FrameTensor[0].Get(), &m_OrtAllocFrame[0]);
            st != nullptr) { ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false; }
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(m_FrameTensor[1].Get(), &m_OrtAllocFrame[1]);
            st != nullptr) { ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false; }
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(m_OutputTensor.Get(), &m_OrtAllocOutput);
            st != nullptr) { ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false; }

        m_UseOrt = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model loaded: %s",
                    qPrintable(modelPath));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC]   contract: %u-ch FP32%s (inputs: %s, %s%s%s; output: %s)",
                    m_ModelChannels,
                    m_HasTimestep ? " + timestep" : "",
                    m_OrtInputNamesCStr[0], m_OrtInputNamesCStr[1],
                    m_HasTimestep ? ", " : "",
                    m_HasTimestep ? m_OrtInputNamesCStr[2] : "",
                    m_OrtOutputNamesCStr[0]);
        return true;
    } catch (const Ort::Exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (OrtException): %s", e.what());
        m_OrtSession.reset();
        m_TimestepShape.clear();
        m_HasTimestep = false;
        return false;
    } catch (const std::exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (std::exception): %s", e.what());
        m_OrtSession.reset();
        m_TimestepShape.clear();
        m_HasTimestep = false;
        return false;
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (unknown exception)");
        m_OrtSession.reset();
        m_TimestepShape.clear();
        m_HasTimestep = false;
        return false;
    }
}


bool DirectMLFRUC::runOrtInference()
{
    if (!m_UseOrt || !m_OrtSession) return false;
    try {
        int prevSlot = 1 - m_WriteSlot;
        int currSlot = m_WriteSlot;

        Ort::MemoryInfo mem("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);

        // The shared buffer is always laid out 4-plane RGBA; a 3-
        // channel model simply reads/writes planes 0-2 and leaves
        // plane 3 alone. Byte count shrinks to 3/4 of the full
        // tensor so ORT's bounds check stays happy.
        const uint32_t ch = m_ModelChannels;
        const uint64_t imgBytes = (uint64_t)ch * m_Height * m_Width * sizeof(float);
        std::array<int64_t, 4> shape = { 1, (int64_t)ch, (int64_t)m_Height, (int64_t)m_Width };

        Ort::Value prev = Ort::Value::CreateTensor(
            mem, m_OrtAllocFrame[prevSlot], imgBytes,
            shape.data(), shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        Ort::Value curr = Ort::Value::CreateTensor(
            mem, m_OrtAllocFrame[currSlot], imgBytes,
            shape.data(), shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        Ort::Value out = Ort::Value::CreateTensor(
            mem, m_OrtAllocOutput, imgBytes,
            shape.data(), shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);

        // Timestep tensor: single-element FP32 = 0.5, persistent.
        // Use the shape as declared by the model (m_TimestepShape,
        // resolved at load time). Common cases: [1], [1,1,1,1].
        std::vector<Ort::Value> inputs;
        inputs.reserve(3);
        inputs.emplace_back(std::move(prev));
        inputs.emplace_back(std::move(curr));
        if (m_HasTimestep) {
            const auto& ts = m_TimestepShape.empty()
                             ? std::vector<int64_t>{ 1 } : m_TimestepShape;
            Ort::Value t = Ort::Value::CreateTensor(
                mem, m_OrtAllocTimestep, sizeof(float),
                ts.data(), ts.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
            inputs.emplace_back(std::move(t));
        }

        Ort::RunOptions ro;
        m_OrtSession->Run(
            ro,
            m_OrtInputNamesCStr.data(), inputs.data(), inputs.size(),
            m_OrtOutputNamesCStr.data(), &out, 1);
        return true;
    } catch (const Ort::Exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX Run() failed, disabling ORT path: %s", e.what());
        m_UseOrt = false;
        return false;
    }
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

    // ORT path uses its own input binding built per-frame inside
    // runOrtInference; the hand-authored DML graph uses our binding
    // table and needs the ping-pong rebind each frame.
    if (!m_UseOrt) rebindPingPongInputs();
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
