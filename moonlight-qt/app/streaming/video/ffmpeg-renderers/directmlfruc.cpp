// VipleStream: DirectMLFRUC — D3D11-texture-share revision (D6).
//
// D3D11 owns render + output textures (SHARED_NTHANDLE).
// D3D12 opens them via IDXGIResource1::CreateSharedHandle +
// ID3D12Device::OpenSharedHandle. Pack/unpack compute shaders run
// entirely on the D3D12 queue using private (non-shared) FP32
// tensor buffers. A cross-API fence serialises D3D11 rendering with
// D3D12 FRUC work.
//
// Per-frame sequence (frame > 0):
//
//   D3D11:  Unbind RTV, Signal fence=N
//   D3D12:  Wait fence=N
//              Pack CS     : shared render tex  → FrameTensor[slot]
//              DML / ORT   : FrameTensor        → OutputTensor
//              Unpack CS   : OutputTensor       → shared output tex
//            Signal fence=N+1
//   D3D11:  Wait fence=N+1
//           Blit shared output texture to screen
//
// No CPU staging, no Map/Unmap per frame, no D3D12→D3D11 buffer
// sharing (fundamentally unsupported on most drivers).

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
    uint32_t useAlpha;  // unpack: 0 = force alpha=1 (3-ch model), 1 = read plane 3
    uint32_t _pad1;
};

// Process-global ORT env — expensive to construct, never torn down.
Ort::Env& sharedOrtEnv()
{
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "VipleStream.DirectML");
    return env;
}

} // namespace


// ── Constructor / destructor ───────────────────────────────────────────────

DirectMLFRUC::DirectMLFRUC() = default;
DirectMLFRUC::~DirectMLFRUC() { destroy(); }


// ── initialize ────────────────────────────────────────────────────────────

bool DirectMLFRUC::initialize(ID3D11Device* device, uint32_t width, uint32_t height)
{
    m_Device11 = device;
    m_Width    = width;
    m_Height   = height;
    m_TensorElements = 4u * width * height;
    m_TensorBytes    = m_TensorElements * sizeof(float);  // FP32 tensor buffers

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
    DML_INIT_STEP("createTensorBuffers",       createTensorBuffers());
    DML_INIT_STEP("createD3D11Textures",       createD3D11Textures());
    DML_INIT_STEP("openD3D11TexturesInD3D12",  openD3D11TexturesInD3D12());
    DML_INIT_STEP("createD3D12CsPipeline",     createD3D12CsPipeline());

#undef DML_INIT_STEP

    // ORT model is optional; if missing / invalid, use inline DML graph.
    bool ortLoaded = tryLoadOnnxModel();
    if (!ortLoaded) {
        // Drain queue + recreate CL infra so the DML initializer gets a
        // clean slate regardless of any partial ORT work.
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

        if (!compileDMLGraph()) {
            // Tier 3 fallback: DML graph refused to compile / init on
            // this hardware, but the D3D12 CS pipeline is ready and the
            // blend PSO exists. Fall through to native blend — this keeps
            // DirectMLFRUC alive instead of surrendering the whole
            // backend to GenericFRUC.
            if (m_BlendPSO12) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: compileDMLGraph failed — "
                            "using Tier 3 D3D12 native blend fallback "
                            "(0.5*prev + 0.5*curr via compute shader, "
                            "no DML runtime involvement)");
                // Drop any partially-created DML graph resources so
                // runDMLDispatch() recognises the blend-only state.
                m_DMLBindingTable.Reset();
                m_DMLCompiledOp.Reset();
                m_DMLDescHeap.Reset();
                m_TempResource.Reset();
                m_PersistentResource.Reset();
                // Also drain the queue once more — compileDMLGraph may
                // have left the init dispatch in flight before failing.
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
                m_BlendAvailable = true;
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: compileDMLGraph failed "
                            "AND blend PSO unavailable — bailing out");
                return false;
            }
        }
    }

    m_Initialized = true;
    const char* modeTag = m_UseOrt          ? "ONNX model"
                        : m_DMLCompiledOp   ? "inline DML graph"
                        : m_BlendAvailable  ? "Tier 3 D3D12 blend"
                        : "?";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML (D6 tex-share, %s) initialized: %ux%u, "
                "FP32 tensor %.2f MB/buf",
                modeTag, width, height, (double)m_TensorBytes / (1024.0 * 1024.0));
    return true;
}


// ── destroy ───────────────────────────────────────────────────────────────

void DirectMLFRUC::destroy()
{
    // Drain queue so no in-flight CL touches resources we're releasing.
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

    // ORT allocations + session must tear down before D3D12 resources.
    if (m_OrtDmlApi) {
        if (m_OrtAllocFrame[0]) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocFrame[0]);
        if (m_OrtAllocFrame[1]) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocFrame[1]);
        if (m_OrtAllocOutput)   m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocOutput);
        if (m_OrtAllocTimestep) m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocTimestep);
        if (m_OrtAllocConcat)   m_OrtDmlApi->FreeGPUAllocation(m_OrtAllocConcat);
        m_OrtAllocFrame[0] = m_OrtAllocFrame[1] = m_OrtAllocOutput
                            = m_OrtAllocTimestep = m_OrtAllocConcat = nullptr;
    }
    m_ConcatTensor.Reset();
    m_ConcatShape.clear();
    m_OrtConcatInput      = false;
    m_ConcatChannels      = 0;
    m_ConcatImageChannels = 0;
    m_ConcatHasTimestep   = false;

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

    // DML graph
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

    // D6: CS pipeline, descriptor heap, constant buffer
    m_BlendPSO12.Reset();   // Tier 3 fallback
    m_BlendAvailable = false;
    m_UnpackPSO12.Reset();
    m_PackPSO12.Reset();
    m_CsRS12.Reset();
    m_CsDescHeap.Reset();
    if (m_CsCBMapped) {
        m_CsCB12->Unmap(0, nullptr);
        m_CsCBMapped = nullptr;
    }
    m_CsCB12.Reset();

    // D6: D3D12 views of shared D3D11 textures
    m_SharedOutputTex12.Reset();
    m_SharedRenderTex12.Reset();

    // D3D11 textures + views
    m_OutputSRV.Reset();
    m_OutputTexture.Reset();
    m_RenderSRV.Reset();
    m_RenderRTV.Reset();
    m_RenderTexture.Reset();

    // D3D12 infrastructure
    m_PostCmdList12.Reset();
    m_PostCmdAlloc12.Reset();
    m_CmdList12.Reset();
    m_CmdAlloc12.Reset();
    m_Fence12.Reset();
    m_Queue12.Reset();
    m_Device12.Reset();

    m_Device11_5.Reset();
    m_Device11    = nullptr;
    m_Initialized = false;
}


// ── createD3D12Device ─────────────────────────────────────────────────────

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

    // Second allocator/list for ORT path post-dispatch unpack CL.
    if (FAILED(m_Device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&m_PostCmdAlloc12)))) return false;
    if (FAILED(m_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             m_PostCmdAlloc12.Get(), nullptr,
                                             IID_PPV_ARGS(&m_PostCmdList12)))) return false;
    m_PostCmdList12->Close();
    return true;
}


// ── createDMLDevice ───────────────────────────────────────────────────────

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


// ── createSharedFence ─────────────────────────────────────────────────────

bool DirectMLFRUC::createSharedFence()
{
    if (FAILED(m_Device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                       IID_PPV_ARGS(&m_Fence12)))) return false;

    HANDLE shared = nullptr;
    if (FAILED(m_Device12->CreateSharedHandle(m_Fence12.Get(), nullptr,
                                              GENERIC_ALL, nullptr, &shared))) return false;
    HRESULT hr = m_Device11_5->OpenSharedFence(shared, IID_PPV_ARGS(&m_Fence11));
    CloseHandle(shared);
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: OpenSharedFence failed 0x%08lx", hr);
        return false;
    }
    return true;
}


// ── createTensorBuffers ───────────────────────────────────────────────────
//
// Private D3D12 DEFAULT buffers for the FRUC tensors.  No sharing flag —
// only the D3D12 queue accesses them, so they stay in
// D3D12_RESOURCE_STATE_UNORDERED_ACCESS for their entire lifetime.

bool DirectMLFRUC::createTensorBuffers()
{
    auto makeBuf = [&](ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = alignUp(m_TensorBytes, 256);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = m_Device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: tensor buffer alloc (%llu B) failed 0x%08lx",
                        (unsigned long long)rd.Width, hr);
        }
        return SUCCEEDED(hr);
    };
    return makeBuf(m_FrameTensor[0]) && makeBuf(m_FrameTensor[1]) && makeBuf(m_OutputTensor);
}


// ── createD3D11Textures ───────────────────────────────────────────────────
//
// Both textures are created with SHARED_NTHANDLE | SHARED so D3D12 can
// open them via IDXGIResource1::CreateSharedHandle.
//
// Output texture also needs D3D11_BIND_UNORDERED_ACCESS so the D3D12
// resource that wraps it gets D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
// (bind flags are reflected in the D3D12 resource description).

bool DirectMLFRUC::createD3D11Textures()
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = m_Width;
    td.Height           = m_Height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    // Render texture: D3D11 renders into (RTV), D3D12 pack CS reads (SRV).
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_RenderTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateRenderTargetView(m_RenderTexture.Get(), nullptr,
                                                  m_RenderRTV.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_RenderTexture.Get(), nullptr,
                                                    m_RenderSRV.GetAddressOf()))) return false;

    // Output texture: D3D12 unpack CS writes (UAV), D3D11 blits via SRV.
    // BIND_UNORDERED_ACCESS is required so D3D12 gets ALLOW_UNORDERED_ACCESS.
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(m_Device11->CreateTexture2D(&td, nullptr, m_OutputTexture.GetAddressOf()))) return false;
    if (FAILED(m_Device11->CreateShaderResourceView(m_OutputTexture.Get(), nullptr,
                                                    m_OutputSRV.GetAddressOf()))) return false;
    return true;
}


// ── openD3D11TexturesInD3D12 ──────────────────────────────────────────────
//
// Export NT handles from the D3D11 textures and open them as D3D12
// resources.  D3D11→D3D12 is the universally supported sharing direction.

bool DirectMLFRUC::openD3D11TexturesInD3D12()
{
    auto openTex = [&](ID3D11Texture2D* tex11, ComPtr<ID3D12Resource>& d12,
                       const char* label) -> bool
    {
        ComPtr<IDXGIResource1> dxgi;
        if (FAILED(tex11->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: QueryInterface(IDXGIResource1) failed for %s", label);
            return false;
        }
        HANDLE h = nullptr;
        HRESULT hr = dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: CreateSharedHandle(%s) failed 0x%08lx", label, hr);
            return false;
        }
        hr = m_Device12->OpenSharedHandle(h, IID_PPV_ARGS(&d12));
        CloseHandle(h);
        if (FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: OpenSharedHandle(%s) failed 0x%08lx", label, hr);
            return false;
        }
        return true;
    };

    if (!openTex(m_RenderTexture.Get(), m_SharedRenderTex12, "render"))  return false;
    if (!openTex(m_OutputTexture.Get(), m_SharedOutputTex12, "output"))  return false;
    return true;
}


// ── createD3D12CsPipeline ─────────────────────────────────────────────────
//
// Root signature, pack/unpack/blend PSOs (reusing the same cs_5_0
// .fxc bytecode compiled for D3D11), descriptor heap (9 entries), and
// the persistently-mapped UPLOAD-heap constant buffer.
//
// Descriptor heap layout:
//   [0] CBV — constant buffer (width, height, useAlpha)
//   [1] SRV — render texture           (pack t0)
//   [2] UAV — FrameTensor[0]           (pack u0, slot 0)
//   [3] UAV — FrameTensor[1]           (pack u0, slot 1)
//   [4] SRV — OutputTensor             (unpack t0)
//   [5] UAV — output texture           (unpack u0)
//   [6] SRV — FrameTensor[0] buffer    (blend  t0 — Tier 3)
//   [7] SRV — FrameTensor[1] buffer    (blend  t1 — Tier 3)
//   [8] UAV — OutputTensor  buffer     (blend  u0 — Tier 3)
//
// Root signature: 4 descriptor-table parameters.
//   param[0]: CBV @ b0           (all three PSOs)
//   param[1]: SRV @ t0           (all three PSOs)
//   param[2]: UAV @ u0           (all three PSOs)
//   param[3]: SRV @ t1           (blend only; pack/unpack leave unbound
//                                 — legal in D3D12 as long as the shader
//                                 doesn't reference the slot)

bool DirectMLFRUC::createD3D12CsPipeline()
{
    // ── Root signature: 4 params, each a 1-range descriptor table ─────────
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;   // b0
    ranges[0].RegisterSpace                     = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0;   // t0
    ranges[1].RegisterSpace                     = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[2].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[2].NumDescriptors                    = 1;
    ranges[2].BaseShaderRegister                = 0;   // u0
    ranges[2].RegisterSpace                     = 0;
    ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[3].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[3].NumDescriptors                    = 1;
    ranges[3].BaseShaderRegister                = 1;   // t1 — blend only
    ranges[3].RegisterSpace                     = 0;
    ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4] = {};
    for (int i = 0; i < 4; ++i) {
        params[i].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges   = &ranges[i];
        params[i].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 4;
    rsd.pParameters   = params;
    rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &rsBlob, &rsErr);
    if (FAILED(hr)) {
        // rsErr carries the D3D12 parser's human-readable diagnostic —
        // always more useful than just the HRESULT.
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: D3D12SerializeRootSignature failed 0x%08lx%s%s",
                    hr,
                    rsErr ? ": "   : "",
                    rsErr ? static_cast<const char*>(rsErr->GetBufferPointer()) : "");
        return false;
    }
    hr = m_Device12->CreateRootSignature(0, rsBlob->GetBufferPointer(),
                                         rsBlob->GetBufferSize(),
                                         IID_PPV_ARGS(&m_CsRS12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateRootSignature failed 0x%08lx", hr);
        return false;
    }

    // ── PSOs — reuse D3D11 cs_5_0 bytecode ────────────────────────────────
    QByteArray packBytes = Path::readDataFile("d3d11_dml_pack_rgba8_fp16.fxc");
    if (packBytes.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: d3d11_dml_pack_rgba8_fp16.fxc missing");
        return false;
    }
    QByteArray unpackBytes = Path::readDataFile("d3d11_dml_unpack_fp16_rgba8.fxc");
    if (unpackBytes.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: d3d11_dml_unpack_fp16_rgba8.fxc missing");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psd = {};
    psd.pRootSignature = m_CsRS12.Get();
    psd.CS             = { packBytes.constData(), (SIZE_T)packBytes.size() };
    hr = m_Device12->CreateComputePipelineState(&psd, IID_PPV_ARGS(&m_PackPSO12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateComputePipelineState(pack) failed 0x%08lx", hr);
        return false;
    }
    psd.CS = { unpackBytes.constData(), (SIZE_T)unpackBytes.size() };
    hr = m_Device12->CreateComputePipelineState(&psd, IID_PPV_ARGS(&m_UnpackPSO12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateComputePipelineState(unpack) failed 0x%08lx", hr);
        return false;
    }

    // ── Tier 3 blend PSO (optional — if the .fxc is missing the
    //    Tier-3 fallback simply isn't available; we still try DML). ───────
    QByteArray blendBytes = Path::readDataFile("d3d11_fruc_blend_fp32.fxc");
    if (blendBytes.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: d3d11_fruc_blend_fp32.fxc missing — "
                    "Tier 3 blend fallback unavailable; DirectML init will bail "
                    "to GenericFRUC if DML graph compilation fails.");
    } else {
        psd.CS = { blendBytes.constData(), (SIZE_T)blendBytes.size() };
        HRESULT bhr = m_Device12->CreateComputePipelineState(&psd, IID_PPV_ARGS(&m_BlendPSO12));
        if (FAILED(bhr)) {
            // Non-fatal — we can still try DML path. Just leave m_BlendPSO12 null.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: CreateComputePipelineState(blend) failed 0x%08lx "
                        "— Tier 3 blend fallback unavailable", bhr);
            m_BlendPSO12.Reset();
        }
    }

    // ── Descriptor heap (9 entries, shader-visible) ────────────────────────
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 9;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_Device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_CsDescHeap));
    if (FAILED(hr)) return false;
    m_CsDescIncrSize = m_Device12->GetDescriptorHandleIncrementSize(
                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // ── Constant buffer on UPLOAD heap, persistently mapped ───────────────
    D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbd = {};
    cbd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbd.Width            = 256; // CBV requires 256-byte alignment
    cbd.Height           = 1;
    cbd.DepthOrArraySize = 1;
    cbd.MipLevels        = 1;
    cbd.SampleDesc.Count = 1;
    cbd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    cbd.Flags            = D3D12_RESOURCE_FLAG_NONE;
    hr = m_Device12->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &cbd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&m_CsCB12));
    if (FAILED(hr)) return false;
    D3D12_RANGE noRead = { 0, 0 };
    hr = m_CsCB12->Map(0, &noRead, &m_CsCBMapped);
    if (FAILED(hr)) return false;
    PackCBData cbInit = { m_Width, m_Height, /*useAlpha=*/1, 0 };
    std::memcpy(m_CsCBMapped, &cbInit, sizeof(cbInit));

    // ── Populate descriptors ───────────────────────────────────────────────
    auto cpuBase = m_CsDescHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT inc = m_CsDescIncrSize;
    auto cpuAt = [&](UINT i) -> D3D12_CPU_DESCRIPTOR_HANDLE {
        return { cpuBase.ptr + i * inc };
    };

    // [0] CBV
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_CsCB12->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes    = 256;
    m_Device12->CreateConstantBufferView(&cbvDesc, cpuAt(0));

    // [1] SRV — render texture (D3D12 view of shared D3D11 render tex)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvTex = {};
    srvTex.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvTex.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvTex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvTex.Texture2D.MipLevels     = 1;
    m_Device12->CreateShaderResourceView(m_SharedRenderTex12.Get(), &srvTex, cpuAt(1));

    // [2] UAV — FrameTensor[0]  (FP32 flat buffer)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavBuf = {};
    uavBuf.Format              = DXGI_FORMAT_R32_FLOAT;
    uavBuf.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
    uavBuf.Buffer.NumElements  = m_TensorElements;
    m_Device12->CreateUnorderedAccessView(m_FrameTensor[0].Get(), nullptr, &uavBuf, cpuAt(2));

    // [3] UAV — FrameTensor[1]
    m_Device12->CreateUnorderedAccessView(m_FrameTensor[1].Get(), nullptr, &uavBuf, cpuAt(3));

    // [4] SRV — OutputTensor
    D3D12_SHADER_RESOURCE_VIEW_DESC srvBuf = {};
    srvBuf.Format                  = DXGI_FORMAT_R32_FLOAT;
    srvBuf.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srvBuf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvBuf.Buffer.NumElements      = m_TensorElements;
    m_Device12->CreateShaderResourceView(m_OutputTensor.Get(), &srvBuf, cpuAt(4));

    // [5] UAV — output texture (D3D12 view of shared D3D11 output tex)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavTex = {};
    uavTex.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavTex.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_Device12->CreateUnorderedAccessView(m_SharedOutputTex12.Get(), nullptr, &uavTex, cpuAt(5));

    // ── Tier 3 blend-path views ───────────────────────────────────────────
    // These alias the same underlying FrameTensor / OutputTensor buffers
    // as slots [2]/[3]/[4] but with the roles the blend shader needs:
    //   t0 ← FrameTensor[0] as Buffer<float>  (slot 6)
    //   t1 ← FrameTensor[1] as Buffer<float>  (slot 7)
    //   u0 → OutputTensor   as RWBuffer<float>(slot 8)
    // We use the same FP32 typed-buffer layout as the pack/unpack paths.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvFrame = srvBuf;  // inherits R32_FLOAT, Buffer, NumElements
    m_Device12->CreateShaderResourceView(m_FrameTensor[0].Get(), &srvFrame, cpuAt(6));
    m_Device12->CreateShaderResourceView(m_FrameTensor[1].Get(), &srvFrame, cpuAt(7));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavOut = uavBuf;   // inherits R32_FLOAT, Buffer, NumElements
    m_Device12->CreateUnorderedAccessView(m_OutputTensor.Get(), nullptr, &uavOut, cpuAt(8));

    return true;
}


// ── compileDMLGraph ───────────────────────────────────────────────────────
//
// Inline 4-node DML graph: idA(0.5·prev) + idB(0.5·curr) → ADD1 → CLIP.
// Produces a bidirectional temporal blend in [0,1].

bool DirectMLFRUC::compileDMLGraph()
{
    const uint32_t sizes[4]   = { 1u, 4u, m_Height, m_Width };

    DML_BUFFER_TENSOR_DESC bufDesc = {};
    bufDesc.DataType               = kDmlDtype;
    bufDesc.DimensionCount         = 4;
    bufDesc.Sizes                  = sizes;
    // Strides intentionally NULL — DML treats null strides as
    // "tightly packed NCHW" which matches our actual memory layout.
    // Passing explicit strides that *equal* the packed layout is
    // logically correct but some DirectML driver revisions hit bugs
    // on the explicit path; null is the canonical hint that the
    // tensor is packed.
    bufDesc.Strides                = nullptr;
    bufDesc.TotalTensorSizeInBytes = m_TensorBytes;
    DML_TENSOR_DESC td = { DML_TENSOR_TYPE_BUFFER, &bufDesc };

    DML_SCALE_BIAS half = { 0.5f, 0.0f };

    DML_ELEMENT_WISE_IDENTITY_OPERATOR_DESC idDesc = {};
    idDesc.InputTensor  = &td;
    idDesc.OutputTensor = &td;
    idDesc.ScaleBias    = &half;
    DML_OPERATOR_DESC idOpDesc = { DML_OPERATOR_ELEMENT_WISE_IDENTITY, &idDesc };
    ComPtr<IDMLOperator> idA, idB;
    HRESULT hr;
    if (FAILED(hr = m_DMLDevice->CreateOperator(&idOpDesc, IID_PPV_ARGS(&idA)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperator(idA) failed 0x%08lx", hr);
        return false;
    }
    if (FAILED(hr = m_DMLDevice->CreateOperator(&idOpDesc, IID_PPV_ARGS(&idB)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperator(idB) failed 0x%08lx", hr);
        return false;
    }

    DML_ELEMENT_WISE_ADD1_OPERATOR_DESC addDesc = {};
    addDesc.ATensor      = &td;
    addDesc.BTensor      = &td;
    addDesc.OutputTensor = &td;
    DML_OPERATOR_DESC addOpDesc = { DML_OPERATOR_ELEMENT_WISE_ADD1, &addDesc };
    ComPtr<IDMLOperator> addOp;
    if (FAILED(hr = m_DMLDevice->CreateOperator(&addOpDesc, IID_PPV_ARGS(&addOp)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperator(add) failed 0x%08lx", hr);
        return false;
    }

    DML_ELEMENT_WISE_CLIP_OPERATOR_DESC clipDesc = {};
    clipDesc.InputTensor  = &td;
    clipDesc.OutputTensor = &td;
    clipDesc.Min          = 0.0f;
    clipDesc.Max          = 1.0f;
    DML_OPERATOR_DESC clipOpDesc = { DML_OPERATOR_ELEMENT_WISE_CLIP, &clipDesc };
    ComPtr<IDMLOperator> clipOp;
    if (FAILED(hr = m_DMLDevice->CreateOperator(&clipOpDesc, IID_PPV_ARGS(&clipOp)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperator(clip) failed 0x%08lx", hr);
        return false;
    }

    DML_OPERATOR_GRAPH_NODE_DESC idANode  = { idA.Get(),    "idA"  };
    DML_OPERATOR_GRAPH_NODE_DESC idBNode  = { idB.Get(),    "idB"  };
    DML_OPERATOR_GRAPH_NODE_DESC addNode  = { addOp.Get(),  "add"  };
    DML_OPERATOR_GRAPH_NODE_DESC clipNode = { clipOp.Get(), "clip" };
    DML_GRAPH_NODE_DESC nodes[4] = {
        { DML_GRAPH_NODE_TYPE_OPERATOR, &idANode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &idBNode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &addNode  },
        { DML_GRAPH_NODE_TYPE_OPERATOR, &clipNode },
    };

    DML_INPUT_GRAPH_EDGE_DESC inEdges[2] = {
        { 0u, 0u, 0u, "A" },
        { 1u, 1u, 0u, "B" },
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
    DML_OUTPUT_GRAPH_EDGE_DESC outEdge   = { 3u, 0u, 0u, "clip->out" };
    DML_GRAPH_EDGE_DESC        outEdgeDesc = { DML_GRAPH_EDGE_TYPE_OUTPUT, &outEdge };

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
    if (FAILED(hr = m_DMLDevice.As(&device1))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: IDMLDevice1 not available (DML 1.1 / Win10 2004+) 0x%08lx", hr);
        return false;
    }
    // Drop DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION — older
    // driver revisions hit DEVICE_REMOVED on FP32 tensors with the
    // half-precision hint because the graph's intermediate buffers
    // end up mis-sized.  Keep strict FP32 compilation for the fallback
    // graph; the perf delta on 4 trivial ops is noise.
    if (FAILED(hr = device1->CompileGraph(&gd,
            DML_EXECUTION_FLAG_NONE,
            IID_PPV_ARGS(&m_DMLCompiledOp)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CompileGraph failed 0x%08lx", hr);
        return false;
    }

    DML_BINDING_PROPERTIES execProps = m_DMLCompiledOp->GetBindingProperties();

    ComPtr<IDMLOperatorInitializer> initializer;
    IDMLCompiledOperator* opsToInit[] = { m_DMLCompiledOp.Get() };
    if (FAILED(hr = m_DMLDevice->CreateOperatorInitializer(1, opsToInit, IID_PPV_ARGS(&initializer)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperatorInitializer failed 0x%08lx", hr);
        return false;
    }
    DML_BINDING_PROPERTIES initProps = initializer->GetBindingProperties();

    uint32_t descCount = std::max(initProps.RequiredDescriptorCount,
                                  execProps.RequiredDescriptorCount);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML: inline graph props — descCount=%u, "
                "initTemp=%llu, execTemp=%llu, persist=%llu",
                descCount,
                (unsigned long long)initProps.TemporaryResourceSize,
                (unsigned long long)execProps.TemporaryResourceSize,
                (unsigned long long)execProps.PersistentResourceSize);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = descCount;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = m_Device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DMLDescHeap)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateDescriptorHeap(DML %u) failed 0x%08lx",
                    descCount, hr);
        return false;
    }

    auto allocBuf = [&](uint64_t bytes, ComPtr<ID3D12Resource>& out, const char* label) -> bool {
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
        HRESULT lhr = m_Device12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(lhr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: %s alloc (%llu B) failed 0x%08lx",
                        label, (unsigned long long)rd.Width, lhr);
        }
        return SUCCEEDED(lhr);
    };
    uint64_t tempBytes = std::max(initProps.TemporaryResourceSize,
                                  execProps.TemporaryResourceSize);
    if (!allocBuf(tempBytes, m_TempResource, "DML Temp"))                              return false;
    if (!allocBuf(execProps.PersistentResourceSize, m_PersistentResource, "DML Pers")) return false;

    // ── Binding table — single instance, Reset() between init and
    //    exec phases.  Matches the Microsoft HelloDirectML reference
    //    pattern.  Previous revision created a second binding table
    //    on the same descriptor heap range for the exec phase; on
    //    some drivers that returned DXGI_ERROR_DEVICE_REMOVED (even
    //    though GetDeviceRemovedReason still returned S_OK, i.e. the
    //    device was fine — it was a DML-level validation error).
    //    Reusing one table with Reset() sidesteps the collision.
    DML_BINDING_TABLE_DESC btd = {};
    btd.Dispatchable        = initializer.Get();
    btd.CPUDescriptorHandle = m_DMLDescHeap->GetCPUDescriptorHandleForHeapStart();
    btd.GPUDescriptorHandle = m_DMLDescHeap->GetGPUDescriptorHandleForHeapStart();
    btd.SizeInDescriptors   = descCount;
    if (FAILED(hr = m_DMLDevice->CreateBindingTable(&btd, IID_PPV_ARGS(&m_DMLBindingTable)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateBindingTable(init) failed 0x%08lx", hr);
        return false;
    }
    if (m_TempResource) {
        DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, m_TempResource->GetDesc().Width };
        DML_BINDING_DESC   tbd = { DML_BINDING_TYPE_BUFFER, &tb };
        m_DMLBindingTable->BindTemporaryResource(&tbd);
    }
    if (m_PersistentResource) {
        DML_BUFFER_BINDING pb = { m_PersistentResource.Get(), 0,
                                  m_PersistentResource->GetDesc().Width };
        DML_BINDING_DESC   pbd = { DML_BINDING_TYPE_BUFFER, &pb };
        m_DMLBindingTable->BindOutputs(1, &pbd);
    }

    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
    ID3D12DescriptorHeap* initHeaps[] = { m_DMLDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, initHeaps);
    m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), initializer.Get(), m_DMLBindingTable.Get());
    m_CmdList12->Close();
    { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    if (m_Fence12->GetCompletedValue() < m_FenceValue) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
        // Bounded wait — if the init dispatch crashes the device the
        // fence still signals (device-removed unblocks all waits), but
        // we want to fail fast rather than hang if anything else is off.
        DWORD waitResult = WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
        if (waitResult != WAIT_OBJECT_0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: init dispatch fence wait timed out (%lu)",
                        waitResult);
            return false;
        }
    }

    // Did the init dispatch crash the device?  A successful fence wait
    // after device-removed looks normal from CPU's perspective but all
    // subsequent DML/D3D12 calls will return DEVICE_REMOVED.
    HRESULT devReason = m_Device12->GetDeviceRemovedReason();
    if (devReason != S_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: device removed after init dispatch, reason 0x%08lx "
                    "(likely a driver-side DML graph validation failure — try updating GPU driver)",
                    devReason);
        return false;
    }

    // ── Exec phase: Reset() swaps the dispatchable without reopening
    //    a second binding table on top of the same descriptor heap.
    btd.Dispatchable = m_DMLCompiledOp.Get();
    if (FAILED(hr = m_DMLBindingTable->Reset(&btd))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: BindingTable->Reset(exec) failed 0x%08lx "
                    "(%s)",
                    hr,
                    (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG)
                        ? "actual device removed"
                        : "DML-internal error; device still healthy");
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: GetDeviceRemovedReason = 0x%08lx",
                        m_Device12->GetDeviceRemovedReason());
        }
        return false;
    }
    if (m_TempResource) {
        DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, m_TempResource->GetDesc().Width };
        DML_BINDING_DESC   tbd = { DML_BINDING_TYPE_BUFFER, &tb };
        m_DMLBindingTable->BindTemporaryResource(&tbd);
    }
    if (m_PersistentResource) {
        DML_BUFFER_BINDING pb = { m_PersistentResource.Get(), 0,
                                  m_PersistentResource->GetDesc().Width };
        DML_BINDING_DESC   pbd = { DML_BINDING_TYPE_BUFFER, &pb };
        m_DMLBindingTable->BindPersistentResource(&pbd);
    }
    DML_BUFFER_BINDING out = { m_OutputTensor.Get(), 0, m_TensorBytes };
    DML_BINDING_DESC   outDesc = { DML_BINDING_TYPE_BUFFER, &out };
    m_DMLBindingTable->BindOutputs(1, &outDesc);
    return true;
}


// ── rebindPingPongInputs ──────────────────────────────────────────────────

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


// ── recordPackCS ──────────────────────────────────────────────────────────
//
// Record pack compute shader into cl.  Assumes m_CsDescHeap is already
// bound via SetDescriptorHeaps before this call.

void DirectMLFRUC::recordPackCS(ID3D12GraphicsCommandList* cl, int slot)
{
    auto gpuBase = m_CsDescHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = m_CsDescIncrSize;
    auto gpuAt = [&](UINT i) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        return { gpuBase.ptr + i * inc };
    };

    cl->SetComputeRootSignature(m_CsRS12.Get());
    cl->SetPipelineState(m_PackPSO12.Get());
    cl->SetComputeRootDescriptorTable(0, gpuAt(0));          // CBV
    cl->SetComputeRootDescriptorTable(1, gpuAt(1));          // SRV render tex
    cl->SetComputeRootDescriptorTable(2, gpuAt(2 + (UINT)slot)); // UAV FrameTensor[slot]
    cl->Dispatch((m_Width + 15) / 16, (m_Height + 15) / 16, 1);
}


// ── recordBlendCS ─────────────────────────────────────────────────────────
//
// Tier 3 fallback: native D3D12 blend. Reads FrameTensor[0] at t0,
// FrameTensor[1] at t1, writes saturated 0.5*(a+b) into OutputTensor
// at u0. Assumes m_CsDescHeap is already bound via SetDescriptorHeaps
// before this call. Safe to invoke even while the same heap is bound
// to the pack pipeline — we just flip root-sig bindings + PSO.

void DirectMLFRUC::recordBlendCS(ID3D12GraphicsCommandList* cl)
{
    auto gpuBase = m_CsDescHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = m_CsDescIncrSize;
    auto gpuAt = [&](UINT i) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        return { gpuBase.ptr + i * inc };
    };

    cl->SetComputeRootSignature(m_CsRS12.Get());
    cl->SetPipelineState(m_BlendPSO12.Get());
    cl->SetComputeRootDescriptorTable(0, gpuAt(0));  // b0: CBV (width/height)
    cl->SetComputeRootDescriptorTable(1, gpuAt(6));  // t0: FrameTensor[0] SRV
    cl->SetComputeRootDescriptorTable(2, gpuAt(8));  // u0: OutputTensor  UAV
    cl->SetComputeRootDescriptorTable(3, gpuAt(7));  // t1: FrameTensor[1] SRV
    // The blend is commutative — (a+b)/2 == (b+a)/2 — so whichever of
    // FrameTensor[0]/[1] is "prev" vs "curr" makes no visual difference.
    cl->Dispatch((m_Width + 15) / 16, (m_Height + 15) / 16, 1);
}


// ── recordUnpackCS ────────────────────────────────────────────────────────
//
// Record unpack compute shader into cl, including the COMMON↔UAV
// barriers for the shared output texture.  Assumes m_CsDescHeap is
// already bound before this call.

void DirectMLFRUC::recordUnpackCS(ID3D12GraphicsCommandList* cl)
{
    // COMMON → UAV: D3D12 will write the shared output texture.
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = m_SharedOutputTex12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);

    auto gpuBase = m_CsDescHeap->GetGPUDescriptorHandleForHeapStart();
    const UINT inc = m_CsDescIncrSize;
    auto gpuAt = [&](UINT i) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        return { gpuBase.ptr + i * inc };
    };

    cl->SetComputeRootSignature(m_CsRS12.Get());
    cl->SetPipelineState(m_UnpackPSO12.Get());
    cl->SetComputeRootDescriptorTable(0, gpuAt(0)); // CBV
    cl->SetComputeRootDescriptorTable(1, gpuAt(4)); // SRV OutputTensor
    cl->SetComputeRootDescriptorTable(2, gpuAt(5)); // UAV output texture
    cl->Dispatch((m_Width + 15) / 16, (m_Height + 15) / 16, 1);

    // UAV → COMMON: hand the texture back so D3D11 can read it as SRV.
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    cl->ResourceBarrier(1, &b);
}


// ── runDMLDispatch ────────────────────────────────────────────────────────
//
// D6 flow:
//   1. Pack current frame into FrameTensor[m_WriteSlot]  (CL on m_CsDescHeap)
//   2a. ORT path: submit pre-CL, call runOrtInference() (which submits post-CL)
//   2b. DML path: switch heap, DML dispatch, switch back, unpack CS — all one CL

bool DirectMLFRUC::runDMLDispatch()
{
    // ── 1. Build the pre-dispatch CL: pack CS + UAV barrier ───────────────
    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
    ID3D12DescriptorHeap* csHeaps[] = { m_CsDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, csHeaps);
    recordPackCS(m_CmdList12.Get(), m_WriteSlot);

    // UAV barrier: pack wrote FrameTensor[m_WriteSlot], DML/ORT will read it.
    D3D12_RESOURCE_BARRIER uavPack = {};
    uavPack.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavPack.UAV.pResource = m_FrameTensor[m_WriteSlot].Get();
    m_CmdList12->ResourceBarrier(1, &uavPack);

    // ── 2a. ORT path ──────────────────────────────────────────────────────
    if (m_UseOrt) {
        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }

        bool ok = runOrtInference();  // submits ORT work + post-unpack CL
        if (ok) return true;

        // ORT failed — fall through to inline DML if available.
        if (!m_DMLCompiledOp) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] ORT inference failed and no inline DML graph "
                        "available — frame dropped");
            return false;
        }
        // The caller (submitFrame) skipped rebindPingPongInputs() because
        // m_UseOrt was still true at frame start.  Now that we've switched
        // to inline DML mid-frame, the binding table has no inputs for
        // this frame — rebind before RecordDispatch, otherwise DML reads
        // stale/empty inputs.
        rebindPingPongInputs();

        // Pack CL already executed; dispatch DML + unpack in a new CL.
        m_CmdAlloc12->Reset();
        m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
        ID3D12DescriptorHeap* dmlHeaps[] = { m_DMLDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, dmlHeaps);
        m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), m_DMLCompiledOp.Get(),
                                      m_DMLBindingTable.Get());
        D3D12_RESOURCE_BARRIER uavOut = {};
        uavOut.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavOut.UAV.pResource = m_OutputTensor.Get();
        m_CmdList12->ResourceBarrier(1, &uavOut);
        m_CmdList12->SetDescriptorHeaps(1, csHeaps);
        recordUnpackCS(m_CmdList12.Get());
        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
        return true;
    }

    // ── 2b. Inline DML path — continue in the same CL ─────────────────────
    if (m_DMLCompiledOp) {
        ID3D12DescriptorHeap* dmlHeaps[] = { m_DMLDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, dmlHeaps);
        m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), m_DMLCompiledOp.Get(),
                                      m_DMLBindingTable.Get());

        // UAV barrier: DML wrote OutputTensor, unpack CS will read it.
        D3D12_RESOURCE_BARRIER uavOut = {};
        uavOut.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavOut.UAV.pResource = m_OutputTensor.Get();
        m_CmdList12->ResourceBarrier(1, &uavOut);

        // Switch back to CS heap, record unpack.
        m_CmdList12->SetDescriptorHeaps(1, csHeaps);
        recordUnpackCS(m_CmdList12.Get());

        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
        return true;
    }

    // ── 2c. Tier 3 fallback: native D3D12 blend CS ───────────────────────
    //       No DML runtime involvement — pack → blend → unpack all on the
    //       same m_CsDescHeap, zero descriptor-heap switches.
    if (m_BlendAvailable && m_BlendPSO12) {
        // FrameTensor[curr] was just written by pack; FrameTensor[prev]
        // was written by the previous frame's pack. Both are already in
        // UNORDERED_ACCESS; the UAV barrier recorded above guarantees the
        // write from this frame's pack is visible to the blend read.
        //
        // Unlike the DML path which reads FrameTensor[prev/curr] as UAVs
        // (DML's binding layer handles state silently), the blend shader
        // reads them as typed SRVs aliased onto the same buffers. On
        // simultaneous-access buffers this is fine without a transition
        // because UAV-state buffers accept SRV reads implicitly — but
        // the UAV barrier is still required to serialise the pack write
        // against the blend read, which we already have from uavPack
        // above.
        recordBlendCS(m_CmdList12.Get());

        // UAV barrier on OutputTensor before unpack reads it as SRV.
        D3D12_RESOURCE_BARRIER uavOut = {};
        uavOut.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavOut.UAV.pResource = m_OutputTensor.Get();
        m_CmdList12->ResourceBarrier(1, &uavOut);

        recordUnpackCS(m_CmdList12.Get());

        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
        return true;
    }

    // Neither DML nor blend available — should never happen once
    // initialize() succeeds; bail out safely.
    m_CmdList12->Close();
    return false;
}


// ── tryLoadOnnxModel ──────────────────────────────────────────────────────

bool DirectMLFRUC::tryLoadOnnxModel()
{
    QString modelPath = Path::getDataFilePath(QStringLiteral("fruc.onnx"));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] No fruc.onnx at '%s' — using inline DML graph.",
                    modelPath.isEmpty() ? "(data dir)" : qPrintable(modelPath));
        return false;
    }

    try {
        const OrtApi& ortApi = Ort::GetApi();
        if (auto* st = ortApi.GetExecutionProviderApi("DML", ORT_API_VERSION,
                reinterpret_cast<const void**>(&m_OrtDmlApi)); st != nullptr) {
            ortApi.ReleaseStatus(st);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: ORT GetExecutionProviderApi(DML) failed");
            return false;
        }

        Ort::SessionOptions so;
        so.DisableMemPattern();
        so.SetExecutionMode(ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (auto* st = m_OrtDmlApi->SessionOptionsAppendExecutionProvider_DML1(
                so, m_DMLDevice.Get(), m_Queue12.Get()); st != nullptr) {
            ortApi.ReleaseStatus(st);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: SessionOptionsAppendExecutionProvider_DML1 failed");
            return false;
        }

#ifdef _WIN32
        m_OrtSession = std::make_unique<Ort::Session>(sharedOrtEnv(),
                           modelPath.toStdWString().c_str(), so);
#else
        m_OrtSession = std::make_unique<Ort::Session>(sharedOrtEnv(),
                           modelPath.toStdString().c_str(), so);
#endif

        const size_t inCount  = m_OrtSession->GetInputCount();
        const size_t outCount = m_OrtSession->GetOutputCount();
        // Supported layouts:
        //   • 1 input  — concatenated [1, C, H, W] where C ∈ {6,7,8,9}
        //                (prev + curr [+ timestep plane]).  RIFE v4.x lite.
        //   • 2 inputs — separate prev + curr image tensors.
        //   • 3 inputs — separate prev + curr + scalar timestep.
        if ((inCount < 1 || inCount > 3) || outCount < 1) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] ONNX: want 1-3 inputs + ≥1 output (got %zu/%zu)",
                        inCount, outCount);
            m_OrtSession.reset(); return false;
        }
        if (outCount > 1)
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] ONNX: model has %zu outputs — using first only", outCount);

        Ort::AllocatorWithDefaultOptions alloc;

        // Helper: validate common image-tensor attrs (FP32, rank-4, H/W match).
        // Sets *outChannels to shape[1] if known (> 0), else 0 for dynamic.
        auto checkImageTensor = [&](Ort::TypeInfo ti, const char* kind,
                                    int64_t* outChannels) -> bool {
            auto info  = ti.GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: expected FP32 (got %d)", kind,
                            (int)info.GetElementType());
                return false;
            }
            if (shape.size() != 4) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: expected 4D NCHW (got %zu)", kind, shape.size());
                return false;
            }
            if ((shape[2] > 0 && shape[2] != (int64_t)m_Height) ||
                (shape[3] > 0 && shape[3] != (int64_t)m_Width)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX %s: H/W mismatch (need [*,C,%u,%u])",
                            kind, m_Height, m_Width);
                return false;
            }
            if (outChannels) *outChannels = shape[1];
            return true;
        };

        // ── 1-input concatenated layout ───────────────────────────────────
        if (inCount == 1) {
            int64_t inC = 0;
            if (!checkImageTensor(m_OrtSession->GetInputTypeInfo(0), "input", &inC)) {
                m_OrtSession.reset(); return false;
            }
            if (inC < 6 || inC > 9) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX 1-input: concat channel count must be 6-9 "
                            "(prev+curr, optionally +1 timestep plane) — got %lld",
                            (long long)inC);
                m_OrtSession.reset(); return false;
            }
            // Derive image channel count + timestep presence from concat C.
            uint32_t imageCh = 0;
            bool hasTs = false;
            switch ((int)inC) {
                case 6: imageCh = 3; hasTs = false; break;
                case 7: imageCh = 3; hasTs = true;  break;
                case 8: imageCh = 4; hasTs = false; break;
                case 9: imageCh = 4; hasTs = true;  break;
            }
            m_OrtConcatInput      = true;
            m_ConcatChannels      = (uint32_t)inC;
            m_ConcatImageChannels = imageCh;
            m_ConcatHasTimestep   = hasTs;
            m_ConcatShape         = { 1, inC, (int64_t)m_Height, (int64_t)m_Width };

            // Output: accept 3 or 4 channels (auto-detect) with matching H/W.
            int64_t outC = 0;
            if (!checkImageTensor(m_OrtSession->GetOutputTypeInfo(0), "output", &outC)) {
                m_OrtSession.reset(); return false;
            }
            if (outC > 0 && outC != 3 && outC != 4) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] ONNX output: only 3 or 4 channels (got %lld)",
                            (long long)outC);
                m_OrtSession.reset(); return false;
            }
            m_ModelChannels = (outC > 0) ? (uint32_t)outC : imageCh;

            m_OrtInputNames.emplace_back(m_OrtSession->GetInputNameAllocated(0, alloc).get());
            m_OrtOutputNames.emplace_back(m_OrtSession->GetOutputNameAllocated(0, alloc).get());
        }
        // ── 2/3-input separate layout ─────────────────────────────────────
        else {
            uint32_t detectedChannels = 0;
            auto validateSeparate = [&](Ort::TypeInfo ti, const char* kind) -> bool {
                int64_t c = 0;
                if (!checkImageTensor(std::move(ti), kind, &c)) return false;
                if (c > 0 && c != 3 && c != 4) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC] ONNX %s: only 3 or 4 channels (got %lld)",
                                kind, (long long)c);
                    return false;
                }
                if (c > 0) {
                    if (detectedChannels == 0) detectedChannels = (uint32_t)c;
                    else if ((uint32_t)c != detectedChannels) {
                        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                    "[VIPLE-FRUC] ONNX %s: channel mismatch (was %u, got %lld)",
                                    kind, detectedChannels, (long long)c);
                        return false;
                    }
                }
                return true;
            };
            for (size_t i = 0; i < 2; ++i) {
                if (!validateSeparate(m_OrtSession->GetInputTypeInfo(i), "image input")) {
                    m_OrtSession.reset(); return false;
                }
                m_OrtInputNames.emplace_back(m_OrtSession->GetInputNameAllocated(i, alloc).get());
            }
            if (inCount == 3) {
                auto ti   = m_OrtSession->GetInputTypeInfo(2);
                auto info = ti.GetTensorTypeAndShapeInfo();
                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC] ONNX timestep: expected FP32 (got %d)",
                                (int)info.GetElementType());
                    m_OrtSession.reset(); return false;
                }
                m_TimestepShape.clear();
                int64_t tsElems = 1;
                for (int64_t d : info.GetShape()) {
                    int64_t c = (d > 0) ? d : 1;
                    m_TimestepShape.push_back(c);
                    tsElems *= c;
                }
                if (m_TimestepShape.empty()) { m_TimestepShape.push_back(1); tsElems = 1; }
                if (tsElems != 1) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "[VIPLE-FRUC] ONNX timestep: expected scalar (got %lld elements)",
                                (long long)tsElems);
                    m_OrtSession.reset(); return false;
                }
                m_HasTimestep = true;
                m_OrtInputNames.emplace_back(m_OrtSession->GetInputNameAllocated(2, alloc).get());
            }
            if (!validateSeparate(m_OrtSession->GetOutputTypeInfo(0), "output")) {
                m_OrtSession.reset(); return false;
            }
            m_OrtOutputNames.emplace_back(m_OrtSession->GetOutputNameAllocated(0, alloc).get());
            m_ModelChannels = detectedChannels ? detectedChannels : 4;
        }

        for (auto& s : m_OrtInputNames)  m_OrtInputNamesCStr.push_back(s.c_str());
        for (auto& s : m_OrtOutputNames) m_OrtOutputNamesCStr.push_back(s.c_str());

        // 3-channel model: flip useAlpha=0 in the D3D12 constant buffer.
        if (m_ModelChannels == 3 && m_CsCBMapped) {
            PackCBData cbData = { m_Width, m_Height, 0, 0 };
            std::memcpy(m_CsCBMapped, &cbData, sizeof(cbData));
        }

        // Timestep resource: tiny D3D12 buffer, constant 0.5f.
        if (m_HasTimestep) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 256; rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags  = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(m_Device12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&m_TimestepResource)))) {
                m_OrtSession.reset(); return false;
            }
            D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            ComPtr<ID3D12Resource> up;
            D3D12_RESOURCE_DESC urd = rd; urd.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(m_Device12->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) {
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
            { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
            m_FenceValue++;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
                WaitForSingleObject(ev, 500);
            }
            CloseHandle(ev);

            if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                    m_TimestepResource.Get(), &m_OrtAllocTimestep); st != nullptr) {
                ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false;
            }
        }

        // ── ConcatTensor for 1-input concat models ────────────────────────
        if (m_OrtConcatInput) {
            const uint64_t concatBytes =
                (uint64_t)m_ConcatChannels * m_Height * m_Width * sizeof(float);
            const uint64_t planeBytes  = (uint64_t)m_Height * m_Width * sizeof(float);

            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = alignUp(concatBytes, 256);
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            // Land in UAV state to match every other tensor buffer — the
            // per-frame runOrtInference() flow issues UAV→COPY_DEST before
            // its copies and COPY_DEST→UAV after.
            if (FAILED(m_Device12->CreateCommittedResource(
                    &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&m_ConcatTensor)))) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: ConcatTensor alloc (%llu B) failed",
                            (unsigned long long)rd.Width);
                m_OrtSession.reset(); return false;
            }

            // Initialize timestep plane (plane 2*imageCh) to all-0.5 floats,
            // if the model has one.  The plane is written once at load-time
            // and left untouched by per-frame copies.
            if (m_ConcatHasTimestep) {
                const uint64_t tsOffset = 2ull * m_ConcatImageChannels * planeBytes;

                D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC urd = {};
                urd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
                urd.Width            = alignUp(planeBytes, 256);
                urd.Height           = 1;
                urd.DepthOrArraySize = 1;
                urd.MipLevels        = 1;
                urd.SampleDesc.Count = 1;
                urd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                ComPtr<ID3D12Resource> up;
                if (FAILED(m_Device12->CreateCommittedResource(
                        &uhp, D3D12_HEAP_FLAG_NONE, &urd,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) {
                    m_OrtSession.reset(); return false;
                }
                void* mapped = nullptr;
                D3D12_RANGE noRead = { 0, 0 };
                up->Map(0, &noRead, &mapped);
                const uint32_t planeElems = m_Height * m_Width;
                std::fill_n(reinterpret_cast<float*>(mapped), planeElems, m_TimestepValue);
                D3D12_RANGE written = { 0, (SIZE_T)planeBytes };
                up->Unmap(0, &written);

                m_CmdAlloc12->Reset();
                m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
                D3D12_RESOURCE_BARRIER pre = {};
                pre.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                pre.Transition.pResource   = m_ConcatTensor.Get();
                pre.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                pre.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                pre.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                m_CmdList12->ResourceBarrier(1, &pre);
                m_CmdList12->CopyBufferRegion(m_ConcatTensor.Get(), tsOffset,
                                              up.Get(), 0, planeBytes);
                D3D12_RESOURCE_BARRIER post = pre;
                post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                post.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                m_CmdList12->ResourceBarrier(1, &post);
                m_CmdList12->Close();
                { ID3D12CommandList* c[] = { m_CmdList12.Get() };
                  m_Queue12->ExecuteCommandLists(1, c); }
                m_FenceValue++;
                m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (m_Fence12->GetCompletedValue() < m_FenceValue) {
                    m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
                    WaitForSingleObject(ev, 500);
                }
                CloseHandle(ev);
            }

            if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                    m_ConcatTensor.Get(), &m_OrtAllocConcat); st != nullptr) {
                ortApi.ReleaseStatus(st);
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: CreateGPUAllocationFromD3DResource(concat) failed");
                m_OrtSession.reset(); return false;
            }
        }

        // Wrap tensor resources as ORT DML allocations (no copy).
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                m_FrameTensor[0].Get(), &m_OrtAllocFrame[0]); st != nullptr) {
            ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false;
        }
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                m_FrameTensor[1].Get(), &m_OrtAllocFrame[1]); st != nullptr) {
            ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false;
        }
        if (auto* st = m_OrtDmlApi->CreateGPUAllocationFromD3DResource(
                m_OutputTensor.Get(), &m_OrtAllocOutput); st != nullptr) {
            ortApi.ReleaseStatus(st); m_OrtSession.reset(); return false;
        }

        m_UseOrt = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model loaded: %s", qPrintable(modelPath));
        if (m_OrtConcatInput) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC]   contract: concat [%u ch = 2×%u%s] → %u-ch (input: %s, output: %s)",
                        m_ConcatChannels, m_ConcatImageChannels,
                        m_ConcatHasTimestep ? " + timestep plane" : "",
                        m_ModelChannels,
                        m_OrtInputNamesCStr[0], m_OrtOutputNamesCStr[0]);
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC]   contract: %u-ch FP32%s (inputs: %s, %s%s%s; output: %s)",
                        m_ModelChannels,
                        m_HasTimestep ? " + timestep" : "",
                        m_OrtInputNamesCStr[0], m_OrtInputNamesCStr[1],
                        m_HasTimestep ? ", " : "",
                        m_HasTimestep ? m_OrtInputNamesCStr[2] : "",
                        m_OrtOutputNamesCStr[0]);
        }
        return true;

    } catch (const Ort::Exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (OrtException): %s", e.what());
    } catch (const std::exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (std::exception): %s", e.what());
    } catch (...) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX model load failed (unknown exception)");
    }
    m_OrtSession.reset();
    m_TimestepShape.clear();
    m_HasTimestep         = false;
    m_OrtConcatInput      = false;
    m_ConcatChannels      = 0;
    m_ConcatImageChannels = 0;
    m_ConcatHasTimestep   = false;
    m_ConcatShape.clear();
    return false;
}


// ── runOrtInference ───────────────────────────────────────────────────────
//
// Called from runDMLDispatch() AFTER the pre-CL (pack CS + UAV barrier)
// has already been submitted.  Runs ORT inference on the DML EP, then
// submits the post-unpack CL on m_PostCmdList12.

bool DirectMLFRUC::runOrtInference()
{
    if (!m_UseOrt || !m_OrtSession) return false;
    try {
        int prevSlot = 1 - m_WriteSlot;
        int currSlot = m_WriteSlot;

        // ── Concat-input path: build the concatenated input tensor ───────
        // Reuses m_PostCmdAlloc12 so m_CmdAlloc12 (still tracking the
        // pack CL submitted in runDMLDispatch) is free to be reused for
        // the post-unpack CL.
        if (m_OrtConcatInput) {
            const uint64_t planeBytes  = (uint64_t)m_Height * m_Width * sizeof(float);
            const uint64_t imgBlockBytes = (uint64_t)m_ConcatImageChannels * planeBytes;

            m_PostCmdAlloc12->Reset();
            m_PostCmdList12->Reset(m_PostCmdAlloc12.Get(), nullptr);

            // Pre-transitions:
            //   FrameTensor[prev,curr]: UAV     -> COPY_SOURCE
            //   ConcatTensor:           (init=COPY_DEST on first frame,
            //                            UAV on subsequent) -> COPY_DEST
            // We always issue UAV -> COPY_DEST for ConcatTensor; on the
            // first frame D3D12 accepts UAV↔COPY_DEST as a promotion/decay
            // since the resource was created in COPY_DEST.  To keep the
            // transition strictly valid, we bump state to UAV once at
            // load time (inside tryLoadOnnxModel) via an implicit
            // promotion on the first ORT binding.  In practice ConcatTensor
            // lives in UAV by the time we get here.
            D3D12_RESOURCE_BARRIER preBars[3] = {};
            for (int i = 0; i < 3; ++i) {
                preBars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                preBars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                preBars[i].Transition.StateAfter  = (i < 2)
                    ? D3D12_RESOURCE_STATE_COPY_SOURCE
                    : D3D12_RESOURCE_STATE_COPY_DEST;
                preBars[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            preBars[0].Transition.pResource = m_FrameTensor[prevSlot].Get();
            preBars[1].Transition.pResource = m_FrameTensor[currSlot].Get();
            preBars[2].Transition.pResource = m_ConcatTensor.Get();
            m_PostCmdList12->ResourceBarrier(3, preBars);

            // Copy prev image planes (planes 0..imageCh-1 of FrameTensor[prev])
            // to planes 0..imageCh-1 of ConcatTensor.
            m_PostCmdList12->CopyBufferRegion(m_ConcatTensor.Get(), 0,
                                              m_FrameTensor[prevSlot].Get(), 0,
                                              imgBlockBytes);
            // Copy curr image planes to planes imageCh..2*imageCh-1.
            m_PostCmdList12->CopyBufferRegion(m_ConcatTensor.Get(), imgBlockBytes,
                                              m_FrameTensor[currSlot].Get(), 0,
                                              imgBlockBytes);
            // Timestep plane (if present) was filled at load time and
            // is left untouched here.

            // Post-transitions: back to UAV for ORT to read.
            D3D12_RESOURCE_BARRIER postBars[3] = {};
            for (int i = 0; i < 3; ++i) {
                postBars[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                postBars[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                postBars[i].Transition.StateBefore = (i < 2)
                    ? D3D12_RESOURCE_STATE_COPY_SOURCE
                    : D3D12_RESOURCE_STATE_COPY_DEST;
                postBars[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            postBars[0].Transition.pResource = m_FrameTensor[prevSlot].Get();
            postBars[1].Transition.pResource = m_FrameTensor[currSlot].Get();
            postBars[2].Transition.pResource = m_ConcatTensor.Get();
            m_PostCmdList12->ResourceBarrier(3, postBars);

            m_PostCmdList12->Close();
            { ID3D12CommandList* c[] = { m_PostCmdList12.Get() };
              m_Queue12->ExecuteCommandLists(1, c); }
        }

        Ort::MemoryInfo mem("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);

        // ── Build ORT input list + run ───────────────────────────────────
        // Ort::Value has no default constructor — initialise with
        // nullptr and std::move an actual value in below.
        std::vector<Ort::Value> inputs;
        Ort::Value out{nullptr};

        if (m_OrtConcatInput) {
            const uint64_t concatBytes =
                (uint64_t)m_ConcatChannels * m_Height * m_Width * sizeof(float);
            inputs.reserve(1);
            inputs.emplace_back(Ort::Value::CreateTensor(
                mem, m_OrtAllocConcat, concatBytes,
                m_ConcatShape.data(), m_ConcatShape.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));

            const uint32_t outCh = m_ModelChannels;
            const uint64_t outBytes = (uint64_t)outCh * m_Height * m_Width * sizeof(float);
            std::array<int64_t, 4> outShape = { 1, (int64_t)outCh,
                                                (int64_t)m_Height, (int64_t)m_Width };
            out = Ort::Value::CreateTensor(mem, m_OrtAllocOutput, outBytes,
                                           outShape.data(), outShape.size(),
                                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        } else {
            const uint32_t ch       = m_ModelChannels;
            const uint64_t imgBytes = (uint64_t)ch * m_Height * m_Width * sizeof(float);
            std::array<int64_t, 4> shape = { 1, (int64_t)ch,
                                             (int64_t)m_Height, (int64_t)m_Width };

            inputs.reserve(3);
            inputs.emplace_back(Ort::Value::CreateTensor(
                mem, m_OrtAllocFrame[prevSlot], imgBytes,
                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
            inputs.emplace_back(Ort::Value::CreateTensor(
                mem, m_OrtAllocFrame[currSlot], imgBytes,
                shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
            if (m_HasTimestep) {
                const auto& ts = m_TimestepShape.empty()
                                 ? std::vector<int64_t>{ 1 } : m_TimestepShape;
                inputs.emplace_back(Ort::Value::CreateTensor(
                    mem, m_OrtAllocTimestep, sizeof(float),
                    ts.data(), ts.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT));
            }
            out = Ort::Value::CreateTensor(mem, m_OrtAllocOutput, imgBytes,
                                           shape.data(), shape.size(),
                                           ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        }

        Ort::RunOptions ro;
        m_OrtSession->Run(ro,
            m_OrtInputNamesCStr.data(),  inputs.data(), inputs.size(),
            m_OrtOutputNamesCStr.data(), &out, 1);

        // ── Post-ORT CL: UAV barrier on OutputTensor + unpack CS ─────────
        // Uses m_CmdAlloc12; by this point the pack pre-CL submitted in
        // runDMLDispatch has had plenty of GPU time.
        m_CmdAlloc12->Reset();
        m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
        ID3D12DescriptorHeap* csHeaps[] = { m_CsDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, csHeaps);

        D3D12_RESOURCE_BARRIER uavOut = {};
        uavOut.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavOut.UAV.pResource = m_OutputTensor.Get();
        m_CmdList12->ResourceBarrier(1, &uavOut);

        recordUnpackCS(m_CmdList12.Get());
        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() };
          m_Queue12->ExecuteCommandLists(1, c); }

        return true;

    } catch (const Ort::Exception& e) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] ONNX Run() failed, disabling ORT path: %s", e.what());
        m_UseOrt = false;
        return false;
    }
}


// ── submitFrame ───────────────────────────────────────────────────────────

bool DirectMLFRUC::submitFrame(ID3D11DeviceContext* ctx, double /*timestamp*/)
{
    if (!m_Initialized) return false;

    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return false;

    // Unbind RTV: ensures D3D12 can safely read the render texture.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx4->OMSetRenderTargets(1, &nullRTV, nullptr);

    // D3D11 signals N: "render texture is ready."
    m_FenceValue++;
    ctx4->Signal(m_Fence11.Get(), m_FenceValue);
    // D3D12 queue waits before touching the render texture.
    m_Queue12->Wait(m_Fence12.Get(), m_FenceValue);

    // First frame: pack slot 0 only; no interpolation output yet.
    if (m_FrameCount == 0) {
        m_CmdAlloc12->Reset();
        m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
        ID3D12DescriptorHeap* heaps[] = { m_CsDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, heaps);
        recordPackCS(m_CmdList12.Get(), 0);
        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }

        // Signal D3D11 back so it can proceed to render the next frame.
        m_FenceValue++;
        m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
        ctx4->Wait(m_Fence11.Get(), m_FenceValue);

        m_WriteSlot = 1;
        m_FrameCount++;
        return false;
    }

    // Frames 1+: pack + FRUC + unpack.
    // rebindPingPongInputs() touches the DML binding table; skip it on
    // the ORT path (ORT owns its own bindings) and on the Tier 3 blend
    // path (no binding table exists).
    if (!m_UseOrt && m_DMLCompiledOp) rebindPingPongInputs();

    bool ok = runDMLDispatch();  // internally: pack CS + DML/ORT + unpack CS

    // D3D12 signals N+1: "output texture is written."
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    // D3D11 waits before blitting the output texture.
    ctx4->Wait(m_Fence11.Get(), m_FenceValue);

    m_WriteSlot = 1 - m_WriteSlot;
    m_FrameCount++;

    if (!ok) return false;

    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    double dtMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    m_LastInterpMs = (m_LastInterpMs == 0.0) ? dtMs
                   : (m_LastInterpMs * 0.9375 + dtMs * 0.0625);
    return true;
}


// ── skipFrame ─────────────────────────────────────────────────────────────

void DirectMLFRUC::skipFrame(ID3D11DeviceContext* ctx)
{
    if (!m_Initialized) return;
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
    m_FrameCount++;
}
