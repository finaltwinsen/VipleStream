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
#include "modelfetcher.h"
#include "path.h"

#include <SDL.h>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <vector>

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
            // VipleStream: no Tier-3 fallback.
            //
            // Previously: if compileDMLGraph() failed we fell back to a
            // "Tier 3 D3D12 native blend" path that just produced
            // 0.5*prev + 0.5*curr via a compute shader. That path had
            // two user-visible problems:
            //   1. Visually it's a crossfade, not motion-compensated
            //      interpolation → shows as ghosting/double-image,
            //      which users perceive as "no FRUC effect".
            //   2. Frame-time drop after ~5–20s — likely CPU-side fence
            //      not progressing, command-allocator resets outpacing
            //      GPU completion, positive-feedback backpressure that
            //      lets skip_ratio climb from 5 % → 70 % within 16 s.
            //
            // Rather than ship a surprise second-best path under the
            // "DirectML" label, we now fail the DirectMLFRUC init
            // cleanly. The caller (d3d11va.cpp) then naturally falls
            // through to GenericFRUC which is working and motion-
            // compensated. Re-enable the Tier-3 path (still in this
            // file: recordBlendCS, m_BlendPSO12, etc.) behind an
            // explicit debug flag if it ever becomes useful again.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: compileDMLGraph failed — "
                        "DirectML unavailable on this hardware/model; "
                        "caller will fall back to Generic compute shader.");
            return false;
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

// VipleStream v1.2.58: Check env-var `VIPLE_DIRECTML_DEBUG` once per
// process. When set to "1" we enable the D3D12 debug layer and DML
// debug flag during DirectMLFRUC::initialize — costs ~0 unless DML
// path is actually hit. Used to dig into 0x887a0005 on RTX A1000.
bool DirectMLFRUC::isDmlDebugEnabled()
{
    static const bool enabled = []() {
        char buf[8] = {};
        size_t sz = 0;
        getenv_s(&sz, buf, sizeof(buf), "VIPLE_DIRECTML_DEBUG");
        return sz > 0 && buf[0] == '1';
    }();
    return enabled;
}

// Drain ID3D12InfoQueue and log every stored message. Called right
// after any suspicious DML / D3D12 call. Returns silently if debug
// layer never attached (InfoQueue QI will fail on release device).
void DirectMLFRUC::logD3D12DebugMessages(const char* where)
{
    if (!m_Device12) return;
    ComPtr<ID3D12InfoQueue> iq;
    if (FAILED(m_Device12->QueryInterface(IID_PPV_ARGS(&iq)))) return;
    const UINT64 n = iq->GetNumStoredMessages();
    if (n == 0) return;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] D3D12 InfoQueue at %s: %llu messages",
                where, (unsigned long long)n);
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T sz = 0;
        iq->GetMessage(i, nullptr, &sz);
        if (sz == 0) continue;
        std::vector<char> buf(sz);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(iq->GetMessage(i, msg, &sz))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC]   [%u/%u] sev=%d cat=%d id=%d : %s",
                        (unsigned)(i + 1), (unsigned)n,
                        (int)msg->Severity, (int)msg->Category, (int)msg->ID,
                        msg->pDescription ? msg->pDescription : "(no desc)");
        }
    }
    iq->ClearStoredMessages();
}

bool DirectMLFRUC::createD3D12Device()
{
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_Device11->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(&adapter))) return false;

    // VipleStream v1.2.58: opt-in D3D12 debug layer for digging into
    // 0x887a0005 (fake DEVICE_REMOVED) on DirectMLFRUC init. Debug
    // layer has measurable cost so only turn on via env var.
    if (isDmlDebugEnabled()) {
        ComPtr<ID3D12Debug> d12Debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&d12Debug)))) {
            d12Debug->EnableDebugLayer();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: D3D12 debug layer ENABLED via VIPLE_DIRECTML_DEBUG=1");
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: D3D12 debug interface unavailable "
                        "(install Windows 10 SDK Graphics Tools for /debug to work)");
        }
    }

    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_Device12));
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: D3D12CreateDevice failed 0x%08lx", hr);
        return false;
    }

    // VipleStream v1.2.58: retain ALL severities in InfoQueue so a
    // post-failure drain surfaces the driver-side validation reason
    // behind 0x887a0005. No-op if debug layer isn't on — QI fails and
    // we silently skip.
    if (isDmlDebugEnabled()) {
        ComPtr<ID3D12InfoQueue> iq;
        if (SUCCEEDED(m_Device12->QueryInterface(IID_PPV_ARGS(&iq)))) {
            // Break on corruption/error for easier debugging when
            // run under a debugger. In release, just collect.
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            iq->SetMuteDebugOutput(FALSE);
        }
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

    // VipleStream v1.2.62: Create ring of command allocators for the
    // hot submitFrame path. Init time only — destroy happens implicitly
    // via ComPtr at device teardown.
    for (uint32_t i = 0; i < kAllocRingSize; ++i) {
        if (FAILED(m_Device12->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_CmdAllocRing[i])))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: ring allocator %u create failed", i);
            return false;
        }
        m_CmdAllocFenceVal[i] = 0;  // 0 = never used, Reset not needed
    }

    // Second allocator/list for ORT path post-dispatch unpack CL.
    if (FAILED(m_Device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&m_PostCmdAlloc12)))) return false;
    if (FAILED(m_Device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             m_PostCmdAlloc12.Get(), nullptr,
                                             IID_PPV_ARGS(&m_PostCmdList12)))) return false;
    m_PostCmdList12->Close();

    // VipleStream v1.2.86: 2-slot rings for post-unpack and concat
    // allocators. Frames alternate slots so by the time a slot is
    // re-used, its previous frame's GPU work has had a full source-
    // frame period (~33 ms at 30 fps source) to drain — long enough
    // for the ~25 ms 540p inference, eliminating the wait_post stall
    // that's been capping real fps at 26 (vs the 30 fps source).
    for (uint32_t i = 0; i < kPostAllocRingSize; ++i) {
        if (FAILED(m_Device12->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_PostUnpackAllocRing[i])))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: post-unpack ring %u create failed", i);
            return false;
        }
        m_PostUnpackAllocFenceVal[i] = 0;

        if (FAILED(m_Device12->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&m_ConcatAllocRing[i])))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: concat ring %u create failed", i);
            return false;
        }
        m_ConcatAllocFenceVal[i] = 0;
    }
    return true;
}


// ── createDMLDevice ───────────────────────────────────────────────────────

bool DirectMLFRUC::createDMLDevice()
{
    // VipleStream v1.2.58: try DML_CREATE_DEVICE_FLAG_DEBUG when the
    // user asks via VIPLE_DIRECTML_DEBUG=1, but fall back to _NONE on
    // 0x887a002d (DXGI_ERROR_SDK_COMPONENT_MISSING) — that error means
    // Graphics Tools / DirectML debug layer isn't installed on the
    // system. Without this fallback, setting the debug env var would
    // kill DML completely on default Windows installs rather than
    // just silently skipping the extra validation.
    HRESULT hr = S_OK;
    DML_CREATE_DEVICE_FLAGS flags = DML_CREATE_DEVICE_FLAG_NONE;
    if (isDmlDebugEnabled()) {
        flags = DML_CREATE_DEVICE_FLAG_DEBUG;
        hr = DMLCreateDevice(m_Device12.Get(), flags, IID_PPV_ARGS(&m_DMLDevice));
        if (hr == 0x887a002d /* DXGI_ERROR_SDK_COMPONENT_MISSING */ || FAILED(hr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: DMLCreateDevice(DEBUG) returned 0x%08lx, "
                        "falling back to non-debug (install Graphics Tools "
                        "optional feature for /debug to work)", hr);
            flags = DML_CREATE_DEVICE_FLAG_NONE;
            m_DMLDevice.Reset();
            hr = DMLCreateDevice(m_Device12.Get(), flags, IID_PPV_ARGS(&m_DMLDevice));
        }
    } else {
        hr = DMLCreateDevice(m_Device12.Get(), flags, IID_PPV_ARGS(&m_DMLDevice));
    }
    if (FAILED(hr)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: DMLCreateDevice failed 0x%08lx "
                    "(flags=0x%x)", hr, (unsigned)flags);
        logD3D12DebugMessages("DMLCreateDevice-failure");
        return false;
    }
    if (flags & DML_CREATE_DEVICE_FLAG_DEBUG) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: DMLDevice created with DEBUG flag");
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
        logD3D12DebugMessages("CompileGraph-failure");
        return false;
    }
    logD3D12DebugMessages("post-CompileGraph");

    DML_BINDING_PROPERTIES execProps = m_DMLCompiledOp->GetBindingProperties();

    ComPtr<IDMLOperatorInitializer> initializer;
    IDMLCompiledOperator* opsToInit[] = { m_DMLCompiledOp.Get() };
    if (FAILED(hr = m_DMLDevice->CreateOperatorInitializer(1, opsToInit, IID_PPV_ARGS(&initializer)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateOperatorInitializer failed 0x%08lx", hr);
        logD3D12DebugMessages("CreateOperatorInitializer-failure");
        return false;
    }
    DML_BINDING_PROPERTIES initProps = initializer->GetBindingProperties();

    // VipleStream v1.2.58: separate descriptor ranges for init/exec
    // binding tables. Previous "one table + Reset() between phases"
    // approach was failing with 0x887a0005 on RTX A1000 even though
    // GetDeviceRemovedReason == S_OK (i.e. DML-internal validation
    // error, not actual device loss). Two distinct tables on distinct
    // slices of the same heap avoids any binding-table-reuse path
    // inside the DML runtime that the A1000 driver's validator trips on.
    //
    // Heap layout:
    //   [0 .. initDescCount)              → init binding table
    //   [initDescCount .. initDescCount + execDescCount) → exec binding table
    //   + a small padding region          → safety margin for driver quirks
    const uint32_t initDescCount = initProps.RequiredDescriptorCount;
    const uint32_t execDescCount = execProps.RequiredDescriptorCount;
    const uint32_t descPad       = 8u;
    const uint32_t totalDescs    = initDescCount + execDescCount + descPad;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML: inline graph props — initDesc=%u, "
                "execDesc=%u, pad=%u, totalHeap=%u, initTemp=%llu, "
                "execTemp=%llu, persist=%llu",
                initDescCount, execDescCount, descPad, totalDescs,
                (unsigned long long)initProps.TemporaryResourceSize,
                (unsigned long long)execProps.TemporaryResourceSize,
                (unsigned long long)execProps.PersistentResourceSize);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = totalDescs;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(hr = m_Device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_DMLDescHeap)))) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: CreateDescriptorHeap(DML %u) failed 0x%08lx",
                    totalDescs, hr);
        logD3D12DebugMessages("CreateDescriptorHeap-failure");
        return false;
    }
    const UINT descIncr = m_Device12->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpuAt = [&](uint32_t i) -> D3D12_CPU_DESCRIPTOR_HANDLE {
        auto h = m_DMLDescHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * descIncr;
        return h;
    };
    auto gpuAt = [&](uint32_t i) -> D3D12_GPU_DESCRIPTOR_HANDLE {
        auto h = m_DMLDescHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)i * descIncr;
        return h;
    };

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

    // ── Init binding table — range [0, initDescCount) ──────────────────────
    ComPtr<IDMLBindingTable> initBindingTable;
    {
        DML_BINDING_TABLE_DESC btd = {};
        btd.Dispatchable        = initializer.Get();
        btd.CPUDescriptorHandle = cpuAt(0);
        btd.GPUDescriptorHandle = gpuAt(0);
        btd.SizeInDescriptors   = initDescCount;
        if (FAILED(hr = m_DMLDevice->CreateBindingTable(&btd, IID_PPV_ARGS(&initBindingTable)))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: CreateBindingTable(init) failed 0x%08lx", hr);
            logD3D12DebugMessages("CreateBindingTable-init-failure");
            return false;
        }
        // Init's temp binding (if the initializer needs scratch).
        if (initProps.TemporaryResourceSize > 0 && m_TempResource) {
            DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, initProps.TemporaryResourceSize };
            DML_BINDING_DESC   tbd = { DML_BINDING_TYPE_BUFFER, &tb };
            initBindingTable->BindTemporaryResource(&tbd);
        }
        // Init's outputs are the compiled op's persistent state.
        // RequiredDescriptorCount covers this slot — always bind it
        // if the compiled op has any persistent state.
        if (m_PersistentResource) {
            DML_BUFFER_BINDING pb = { m_PersistentResource.Get(), 0,
                                      m_PersistentResource->GetDesc().Width };
            DML_BINDING_DESC   pbd = { DML_BINDING_TYPE_BUFFER, &pb };
            initBindingTable->BindOutputs(1, &pbd);
        } else {
            // No persistent state — bind a "none" output anyway so
            // the init dispatcher doesn't read an unbound slot.
            DML_BINDING_DESC none = { DML_BINDING_TYPE_NONE, nullptr };
            initBindingTable->BindOutputs(1, &none);
        }
    }

    // Record + submit the init dispatch on cmd list 12.
    m_CmdAlloc12->Reset();
    m_CmdList12->Reset(m_CmdAlloc12.Get(), nullptr);
    ID3D12DescriptorHeap* initHeaps[] = { m_DMLDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, initHeaps);
    m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), initializer.Get(), initBindingTable.Get());
    m_CmdList12->Close();
    { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
    logD3D12DebugMessages("post-init-dispatch-submit");

    // VipleStream v1.2.58: hard CPU-side fence wait BEFORE any further
    // allocator/table work. Previous code only waited if
    // GetCompletedValue() < FenceValue at query time; but on fast
    // drivers (or when the init dispatch crashed silently) the fence
    // could signal early with the device in a bad state. An explicit
    // WaitForSingleObject ensures any queued init work is fully
    // retired on GPU before we touch the command allocator for the
    // first frame's work.
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: CreateEventW failed during init wait");
            return false;
        }
        m_Fence12->SetEventOnCompletion(m_FenceValue, ev);
        DWORD waitResult = WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
        if (waitResult != WAIT_OBJECT_0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: init dispatch fence wait timed out (%lu)",
                        waitResult);
            logD3D12DebugMessages("init-fence-timeout");
            return false;
        }
    }

    // Did the init dispatch crash the device?
    HRESULT devReason = m_Device12->GetDeviceRemovedReason();
    if (devReason != S_OK) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: device removed after init dispatch, reason 0x%08lx "
                    "(driver-side DML graph validation failure — try updating GPU driver)",
                    devReason);
        logD3D12DebugMessages("device-removed-after-init");
        return false;
    }

    // ── Exec binding table — range [initDescCount, initDescCount+execDescCount)
    //     SEPARATE IDMLBindingTable instance on a SEPARATE descriptor
    //     slice. This is the key divergence from prior versions that
    //     reused one table with Reset() — some driver combos (RTX
    //     A1000 with current driver) returned 0x887a0005 on Reset()
    //     even with device in S_OK, meaning the DML runtime's own
    //     validator rejected the swap. Fresh table on distinct
    //     descriptor range avoids that path entirely.
    {
        DML_BINDING_TABLE_DESC btd = {};
        btd.Dispatchable        = m_DMLCompiledOp.Get();
        btd.CPUDescriptorHandle = cpuAt(initDescCount);
        btd.GPUDescriptorHandle = gpuAt(initDescCount);
        btd.SizeInDescriptors   = execDescCount;
        if (FAILED(hr = m_DMLDevice->CreateBindingTable(&btd, IID_PPV_ARGS(&m_DMLBindingTable)))) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML: CreateBindingTable(exec) failed 0x%08lx",
                        hr);
            logD3D12DebugMessages("CreateBindingTable-exec-failure");
            return false;
        }
    }
    if (m_TempResource && execProps.TemporaryResourceSize > 0) {
        DML_BUFFER_BINDING tb = { m_TempResource.Get(), 0, execProps.TemporaryResourceSize };
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
    logD3D12DebugMessages("post-exec-table-bound");
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


// VipleStream v1.2.82: gate an allocator's Reset() on its last
// recorded fence value having been reached by the GPU.
//
// In steady state this is a single fast atomic load (GetCompletedValue)
// followed by a no-op compare — sub-µs cost. When the GPU has fallen
// behind (e.g. heavy game or low-end GPU) we block on an event for up
// to 500 ms. That's still better than calling Reset() on an in-use
// allocator, which is undefined behaviour and on most drivers
// manifests as an *implicit* (un-instrumented) CPU stall — exactly
// the regression that made DirectML "drop FPS instead of doubling it"
// after Option C (v1.2.64) added the ring allocator for the *pack*
// CL only, leaving m_CmdAlloc12 / m_PostCmdAlloc12 unprotected for
// their post-dispatch CLs.
//
// v1.2.83: instrumented to measure how often / how long we wait —
// the result feeds the per-stage [VIPLE-FRUC-DML] log so we can tell
// whether the bottleneck is GPU inference or the wait.  Returns the
// number of microseconds spent waiting (0 in the fast path).
static double waitFenceBeforeAllocReset(ID3D12Fence* fence, uint64_t fenceVal)
{
    if (fenceVal == 0) return 0.0;  // never used, no work to wait for
    if (fence->GetCompletedValue() >= fenceVal) return 0.0;  // GPU already done

    LARGE_INTEGER tStart, tEnd, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) return 0.0;
    if (SUCCEEDED(fence->SetEventOnCompletion(fenceVal, ev))) {
        WaitForSingleObject(ev, 500);
    }
    CloseHandle(ev);

    QueryPerformanceCounter(&tEnd);
    return (double)(tEnd.QuadPart - tStart.QuadPart) * 1000000.0 / (double)freq.QuadPart;
}

// ── runDMLDispatch ────────────────────────────────────────────────────────
//
// D6 flow:
//   1. Pack current frame into FrameTensor[m_WriteSlot]  (CL on m_CsDescHeap)
//   2a. ORT path: submit pre-CL, call runOrtInference() (which submits post-CL)
//   2b. DML path: switch heap, DML dispatch, switch back, unpack CS — all one CL

bool DirectMLFRUC::runDMLDispatch()
{
    // VipleStream v1.2.62: instrument for Option C interop tuning
    LARGE_INTEGER qpfFreq; QueryPerformanceFrequency(&qpfFreq);
    auto usSince = [&](LARGE_INTEGER a, LARGE_INTEGER b) -> double {
        return (double)(b.QuadPart - a.QuadPart) * 1000000.0 / (double)qpfFreq.QuadPart;
    };
    auto emaAcc = [](double& acc, double sample) {
        acc = (acc == 0.0) ? sample : (acc * 0.9375 + sample * 0.0625);
    };

    LARGE_INTEGER tR0; QueryPerformanceCounter(&tR0);
    // ── 1. Build the pre-dispatch CL: pack CS + UAV barrier ───────────────
    //
    // VipleStream v1.2.62 (Option C): pick next allocator from the
    // ring. Wait if its last-known fence value hasn't been reached by
    // the GPU yet. v1.2.83 routes through waitFenceBeforeAllocReset
    // so the wait time gets accumulated into wait_pack_us — making
    // ring-allocator stalls visible in the [VIPLE-FRUC-DML] log too.
    const uint32_t slot = m_CmdAllocIdx;
    double waitPackUs = waitFenceBeforeAllocReset(m_Fence12.Get(), m_CmdAllocFenceVal[slot]);
    emaAcc(m_Stage.wait_pack_us, waitPackUs);
    auto& alloc = m_CmdAllocRing[slot];
    alloc->Reset();
    m_CmdList12->Reset(alloc.Get(), nullptr);
    LARGE_INTEGER tR_afterReset; QueryPerformanceCounter(&tR_afterReset);

    ID3D12DescriptorHeap* csHeaps[] = { m_CsDescHeap.Get() };
    m_CmdList12->SetDescriptorHeaps(1, csHeaps);
    recordPackCS(m_CmdList12.Get(), m_WriteSlot);

    // UAV barrier: pack wrote FrameTensor[m_WriteSlot], DML/ORT will read it.
    D3D12_RESOURCE_BARRIER uavPack = {};
    uavPack.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavPack.UAV.pResource = m_FrameTensor[m_WriteSlot].Get();
    m_CmdList12->ResourceBarrier(1, &uavPack);
    LARGE_INTEGER tR_afterPack; QueryPerformanceCounter(&tR_afterPack);
    emaAcc(m_Stage.alloc_reset_us, usSince(tR0,          tR_afterReset));
    emaAcc(m_Stage.pack_record_us, usSince(tR_afterReset, tR_afterPack));

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
        // v1.2.82: gate Reset on the previous frame's post-dispatch
        // CL having actually completed on the GPU.
        waitFenceBeforeAllocReset(m_Fence12.Get(), m_CmdAlloc12FenceVal);
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
        LARGE_INTEGER tD0; QueryPerformanceCounter(&tD0);
        ID3D12DescriptorHeap* dmlHeaps[] = { m_DMLDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, dmlHeaps);
        m_DMLRecorder->RecordDispatch(m_CmdList12.Get(), m_DMLCompiledOp.Get(),
                                      m_DMLBindingTable.Get());

        // UAV barrier: DML wrote OutputTensor, unpack CS will read it.
        D3D12_RESOURCE_BARRIER uavOut = {};
        uavOut.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavOut.UAV.pResource = m_OutputTensor.Get();
        m_CmdList12->ResourceBarrier(1, &uavOut);
        LARGE_INTEGER tD_afterDML; QueryPerformanceCounter(&tD_afterDML);

        // Switch back to CS heap, record unpack.
        m_CmdList12->SetDescriptorHeaps(1, csHeaps);
        recordUnpackCS(m_CmdList12.Get());
        LARGE_INTEGER tD_afterUnpack; QueryPerformanceCounter(&tD_afterUnpack);

        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }
        LARGE_INTEGER tD_afterExec; QueryPerformanceCounter(&tD_afterExec);

        emaAcc(m_Stage.dml_record_us,    usSince(tD0,             tD_afterDML));
        emaAcc(m_Stage.unpack_record_us, usSince(tD_afterDML,     tD_afterUnpack));
        emaAcc(m_Stage.close_exec_us,    usSince(tD_afterUnpack,  tD_afterExec));
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
    // §G.4 — model lookup order (release zip no longer ships the .onnx files):
    //   1. Path::getDataFilePath  — exe dir / install / portable / Qt resource;
    //                                covers dev builds where the .onnx still
    //                                sits next to the binary.
    //   2. ModelFetcher cache      — %LOCALAPPDATA%\VipleStream\fruc_models\;
    //                                blocking download from GitHub release on
    //                                first use, sha-256 verified, retried once.
    // If both miss, fall through to the inline DML blend graph (existing
    // behaviour pre-§G.4) so the user still gets a working — if non-RIFE —
    // FRUC chain rather than a startup error.
    QString modelPath = Path::getDataFilePath(QString::fromStdString(m_ModelFilename));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] %s not found in install/data dirs; "
                    "checking on-demand model cache (will download if absent)",
                    m_ModelFilename.c_str());
        modelPath = ModelFetcher::ensureModelPath(QString::fromStdString(m_ModelFilename));
        if (modelPath.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] %s unavailable (cache + download both failed) "
                        "— using inline DML graph.",
                        m_ModelFilename.c_str());
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] %s resolved via cache/download → %s",
                    m_ModelFilename.c_str(), qPrintable(modelPath));
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] Loading ONNX model: %s", m_ModelFilename.c_str());

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
        // v1.3.3 — VIPLE_DIRECTML_VERBOSE=1 flips this session's log
        // severity to VERBOSE (level 0).  ORT then dumps the EP each
        // node ends up assigned to as part of the partitioner output.
        // We need this to find which ops are falling back to CPU EP
        // and causing the 60-70ms inference on Tensor-Core-class GPUs.
        // Default (env unset) keeps the global Env's WARNING level so
        // production logs aren't flooded.
        {
            char vbuf[8] = {}; size_t vsz = 0;
            getenv_s(&vsz, vbuf, sizeof(vbuf), "VIPLE_DIRECTML_VERBOSE");
            if (vsz > 0 && (vbuf[0] == '1' || _stricmp(vbuf, "true") == 0)) {
                so.SetLogSeverityLevel(0);  // 0 = VERBOSE
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: VIPLE_DIRECTML_VERBOSE=1 → "
                            "ORT session log = VERBOSE (expect node-by-node EP assignment dump)");
            }
        }
        // v1.3.5 — Pin the model's symbolic input dims (batch=1,
        // height=m_Height, width=m_Width) up-front so ORT's graph
        // optimiser can constant-fold the entire shape-arithmetic
        // subgraph. Both fruc.onnx and fruc_fp16.onnx ship with input
        // shape [batch, 7, height, width] — when these dims are
        // dynamic, ~84 shape-domain ops (Cast/Gather/Sub/Div/
        // Unsqueeze/Concat ...) get assigned to CPU EP, which inserts
        // 8+ MemcpyFromHost boundaries that dominate runtime even on
        // a Tensor-Core GPU (RTX 3060 Laptop measured 60-70 ms before
        // this change, vs 28.3 ms half-rate budget).  The dim names
        // match what `python -c "import onnx; ..."` reports for both
        // models in the cascade; mismatched names are ignored
        // silently by ORT (no harm if a future model uses different
        // names, just no acceleration).
        // ORT 1.20 C++ wrapper doesn't expose AddFreeDimensionOverrideByName
        // — call through the C API.  Ort::SessionOptions has an implicit
        // OrtSessionOptions* cast operator (used same pattern at the DML1
        // append call below).  Mismatched dim names return non-fatal status
        // we just log + ignore.
        auto applyDimOverride = [&](const char* name, int64_t value) {
            if (auto* st = ortApi.AddFreeDimensionOverrideByName(so, name, value); st != nullptr) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: free-dim '%s' override skipped (%s)",
                            name, ortApi.GetErrorMessage(st));
                ortApi.ReleaseStatus(st);
            }
        };
        applyDimOverride("batch",  1);
        applyDimOverride("height", static_cast<int64_t>(m_Height));
        applyDimOverride("width",  static_cast<int64_t>(m_Width));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML: free-dim overrides set "
                    "(batch=1, height=%u, width=%u) → ORT can constant-fold shape subgraph",
                    m_Height, m_Width);

        // VIPLE_DIRECTML_NO_CPU_FALLBACK=1 escape hatch — forces DML
        // to claim every op or reject the model entirely. v1.3.4
        // experiment confirmed this just makes ORT bail out and the
        // cascade falls through to the inline DML blend graph (a
        // simple 0.5×prev + 0.5×curr, NOT RIFE), so it's only useful
        // for diagnostic confirmation that CPU-EP fallback is the
        // limiting factor.
        {
            char nbuf[8] = {}; size_t nsz = 0;
            getenv_s(&nsz, nbuf, sizeof(nbuf), "VIPLE_DIRECTML_NO_CPU_FALLBACK");
            if (nsz > 0 && (nbuf[0] == '1' || _stricmp(nbuf, "true") == 0)) {
                so.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] DirectML: VIPLE_DIRECTML_NO_CPU_FALLBACK=1 → "
                            "session.disable_cpu_ep_fallback=1 (diagnostic only — "
                            "expected to fail loading and fall through to inline blend)");
            }
        }
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

        // VipleStream v1.2.83: ORT-path stage timers.
        LARGE_INTEGER qpfFreq; QueryPerformanceFrequency(&qpfFreq);
        auto usSince = [&](LARGE_INTEGER a, LARGE_INTEGER b) -> double {
            return (double)(b.QuadPart - a.QuadPart) * 1000000.0 / (double)qpfFreq.QuadPart;
        };
        auto emaAcc = [](double& acc, double sample) {
            acc = (acc == 0.0) ? sample : (acc * 0.9375 + sample * 0.0625);
        };

        // ── Concat-input path: build the concatenated input tensor ───────
        // Reuses m_PostCmdAlloc12 so m_CmdAlloc12 (still tracking the
        // pack CL submitted in runDMLDispatch) is free to be reused for
        // the post-unpack CL.
        if (m_OrtConcatInput) {
            const uint64_t planeBytes  = (uint64_t)m_Height * m_Width * sizeof(float);
            const uint64_t imgBlockBytes = (uint64_t)m_ConcatImageChannels * planeBytes;

            // v1.2.86: pick next concat-allocator ring slot.  The
            // 2-slot ring eliminates per-frame wait at 30 fps source
            // because slot N's frame is two source-frame-periods (~66 ms)
            // old by the time it's reused — well past the 25 ms
            // inference window.  Wait gate stays as a safety net for
            // transients (e.g. stutter caused by external GPU load).
            LARGE_INTEGER tConcat0; QueryPerformanceCounter(&tConcat0);
            const uint32_t concatSlot = m_ConcatAllocIdx;
            emaAcc(m_Stage.wait_concat_us,
                   waitFenceBeforeAllocReset(m_Fence12.Get(),
                                             m_ConcatAllocFenceVal[concatSlot]));
            auto& concatAlloc = m_ConcatAllocRing[concatSlot];
            concatAlloc->Reset();
            m_PostCmdList12->Reset(concatAlloc.Get(), nullptr);

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

            // VipleStream v1.2.83: signal IMMEDIATELY after the concat
            // CL is submitted so we have a fence value that completes
            // when (just) the concat CL is done.
            //
            // v1.2.86: stamp the active ring slot, then advance.
            m_FenceValue++;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            m_ConcatAllocFenceVal[concatSlot] = m_FenceValue;
            m_ConcatAllocIdx = (m_ConcatAllocIdx + 1) % kPostAllocRingSize;

            LARGE_INTEGER tConcatEnd; QueryPerformanceCounter(&tConcatEnd);
            emaAcc(m_Stage.ort_concat_us, usSince(tConcat0, tConcatEnd));
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

        // v1.2.83: time the ORT::Run() call separately. With GPU-resident
        // input/output tensors and the DML EP sharing our m_Queue12,
        // this should just enqueue ops and return — but every prior
        // [VIPLE-FRUC-DML] log shows ~44ms hidden in this region, so
        // we need a dedicated number to know whether ORT is blocking.
        LARGE_INTEGER tRun0; QueryPerformanceCounter(&tRun0);
        Ort::RunOptions ro;
        m_OrtSession->Run(ro,
            m_OrtInputNamesCStr.data(),  inputs.data(), inputs.size(),
            m_OrtOutputNamesCStr.data(), &out, 1);
        LARGE_INTEGER tRun1; QueryPerformanceCounter(&tRun1);
        emaAcc(m_Stage.ort_run_us, usSince(tRun0, tRun1));

        // ── Post-ORT CL: UAV barrier on OutputTensor + unpack CS ─────────
        //
        // v1.2.86: 2-slot ring for post-unpack allocators.  Slot N is
        // re-used 2 source-frame-periods (~66 ms at 30 fps source)
        // after submission — well past the ~25 ms inference window —
        // so the wait fence gate steady-state cost drops to zero.
        // (The fence still gates on a slow-path stutter as a safety
        // net.)  This is what brings DirectML real fps from 26 to ~30,
        // matching the source rate.
        const uint32_t postSlot = m_PostUnpackAllocIdx;
        emaAcc(m_Stage.wait_post_us,
               waitFenceBeforeAllocReset(m_Fence12.Get(),
                                         m_PostUnpackAllocFenceVal[postSlot]));
        LARGE_INTEGER tPost0; QueryPerformanceCounter(&tPost0);
        auto& postAlloc = m_PostUnpackAllocRing[postSlot];
        postAlloc->Reset();
        m_CmdList12->Reset(postAlloc.Get(), nullptr);
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
        LARGE_INTEGER tPost1; QueryPerformanceCounter(&tPost1);
        emaAcc(m_Stage.ort_post_us, usSince(tPost0, tPost1));

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

    // VipleStream v1.2.62: instrumented per-stage timing for interop
    // overhead tuning (Option C). Each sub-step gets a QPC counter;
    // EMA of all timings printed every 120 frames via `[VIPLE-FRUC-DML]`.
    LARGE_INTEGER qpfFreq; QueryPerformanceFrequency(&qpfFreq);
    auto usSince = [&](LARGE_INTEGER a, LARGE_INTEGER b) -> double {
        return (double)(b.QuadPart - a.QuadPart) * 1000000.0 / (double)qpfFreq.QuadPart;
    };
    auto emaAcc = [](double& acc, double sample) {
        acc = (acc == 0.0) ? sample : (acc * 0.9375 + sample * 0.0625);
    };

    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);

    ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) return false;

    // Unbind RTV: ensures D3D12 can safely read the render texture.
    ID3D11RenderTargetView* nullRTV = nullptr;
    ctx4->OMSetRenderTargets(1, &nullRTV, nullptr);
    LARGE_INTEGER t_afterRtv; QueryPerformanceCounter(&t_afterRtv);

    // D3D11 signals N: "render texture is ready." (forces D3D11 to
    // flush pending command buffer to GPU first)
    m_FenceValue++;
    ctx4->Signal(m_Fence11.Get(), m_FenceValue);
    LARGE_INTEGER t_afterSignal; QueryPerformanceCounter(&t_afterSignal);

    // D3D12 queue waits before touching the render texture (GPU-side
    // wait — just queues a wait command, doesn't block CPU).
    m_Queue12->Wait(m_Fence12.Get(), m_FenceValue);
    LARGE_INTEGER t_syncDone; QueryPerformanceCounter(&t_syncDone);

    // Diagnostic sub-breakdown of d3d11_sync
    static double ema_rtv = 0, ema_sig = 0, ema_wait = 0;
    auto emaAccDiag = [](double& a, double s) { a = (a == 0.0) ? s : (a * 0.9375 + s * 0.0625); };
    emaAccDiag(ema_rtv,  usSince(t0,              t_afterRtv));
    emaAccDiag(ema_sig,  usSince(t_afterRtv,      t_afterSignal));
    emaAccDiag(ema_wait, usSince(t_afterSignal,   t_syncDone));
    if (m_Stage.sample_count > 0 && (m_Stage.sample_count % 120) == 119) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-DML-SUB] rtv=%.0f sig=%.0f wait=%.0f μs",
                    ema_rtv, ema_sig, ema_wait);
    }

    // First frame: pack slot 0 only; no interpolation output yet.
    if (m_FrameCount == 0) {
        // Use ring slot 0 (never used, fence_val==0 → no wait).
        auto& firstAlloc = m_CmdAllocRing[m_CmdAllocIdx];
        firstAlloc->Reset();
        m_CmdList12->Reset(firstAlloc.Get(), nullptr);
        ID3D12DescriptorHeap* heaps[] = { m_CsDescHeap.Get() };
        m_CmdList12->SetDescriptorHeaps(1, heaps);
        recordPackCS(m_CmdList12.Get(), 0);
        m_CmdList12->Close();
        { ID3D12CommandList* c[] = { m_CmdList12.Get() }; m_Queue12->ExecuteCommandLists(1, c); }

        // Signal D3D11 back so it can proceed to render the next frame.
        m_FenceValue++;
        m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
        ctx4->Wait(m_Fence11.Get(), m_FenceValue);

        // Record the fence value against current slot + advance.
        m_CmdAllocFenceVal[m_CmdAllocIdx] = m_FenceValue;
        m_CmdAllocIdx = (m_CmdAllocIdx + 1) % kAllocRingSize;

        m_WriteSlot = 1;
        m_FrameCount++;
        return false;
    }

    // Frames 1+: pack + FRUC + unpack.
    // rebindPingPongInputs() touches the DML binding table; skip it on
    // the ORT path (ORT owns its own bindings) and on the Tier 3 blend
    // path (no binding table exists).
    if (!m_UseOrt && m_DMLCompiledOp) rebindPingPongInputs();

    bool ok = runDMLDispatch();  // timing accumulated inside runDMLDispatch
    LARGE_INTEGER t_dispatchDone; QueryPerformanceCounter(&t_dispatchDone);

    // D3D12 signals N+1: "output texture is written."
    m_FenceValue++;
    m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
    // D3D11 waits before blitting the output texture.
    ctx4->Wait(m_Fence11.Get(), m_FenceValue);
    LARGE_INTEGER t_postSignal; QueryPerformanceCounter(&t_postSignal);

    // VipleStream v1.2.62 (Option C): record the post-signal fence
    // value against the allocator slot we just submitted from, then
    // advance the ring. Next time we land on this slot, we gate on
    // this fence value having been reached by GPU.
    m_CmdAllocFenceVal[m_CmdAllocIdx] = m_FenceValue;
    m_CmdAllocIdx = (m_CmdAllocIdx + 1) % kAllocRingSize;

    // VipleStream v1.2.86: stamp the just-used post-unpack ring slot
    // with the current end-of-frame fence value, then advance the
    // index.  When the ring wraps back to this slot two source-frame
    // periods later, the fence-gate is satisfied immediately (GPU
    // long since past this fence value).
    //
    // m_CmdAlloc12FenceVal kept in sync as a defensive backstop for
    // the few non-hot paths that still touch m_CmdAlloc12 (e.g. ORT
    // failure -> inline DML fallback).
    m_PostUnpackAllocFenceVal[m_PostUnpackAllocIdx] = m_FenceValue;
    m_PostUnpackAllocIdx = (m_PostUnpackAllocIdx + 1) % kPostAllocRingSize;
    m_CmdAlloc12FenceVal = m_FenceValue;

    m_WriteSlot = 1 - m_WriteSlot;
    m_FrameCount++;

    if (!ok) return false;

    emaAcc(m_Stage.d3d11_sync_us,        usSince(t0,             t_syncDone));
    emaAcc(m_Stage.d3d12_signal_wait_us, usSince(t_dispatchDone, t_postSignal));
    double totalUs = usSince(t0, t_postSignal);
    emaAcc(m_Stage.submit_total_us, totalUs);
    m_Stage.sample_count++;

    // Periodic per-stage breakdown (every 120 frames ≈ 2s at 60fps real).
    if ((m_Stage.sample_count % 120) == 0) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-DML] n=%llu ema μs: d3d11_sync=%.0f alloc_reset=%.0f "
                    "pack_record=%.0f dml_record=%.0f unpack_record=%.0f "
                    "close_exec=%.0f d3d12_signal=%.0f | total=%.0f",
                    (unsigned long long)m_Stage.sample_count,
                    m_Stage.d3d11_sync_us, m_Stage.alloc_reset_us,
                    m_Stage.pack_record_us, m_Stage.dml_record_us,
                    m_Stage.unpack_record_us, m_Stage.close_exec_us,
                    m_Stage.d3d12_signal_wait_us, m_Stage.submit_total_us);
        // v1.2.83: ORT-path-specific breakdown — prints zeros for
        // the inline DML path which doesn't go through these stages.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC-DML-ORT] ort_concat=%.0f ort_run=%.0f ort_post=%.0f | "
                    "wait_pack=%.0f wait_post=%.0f wait_concat=%.0f μs",
                    m_Stage.ort_concat_us, m_Stage.ort_run_us, m_Stage.ort_post_us,
                    m_Stage.wait_pack_us, m_Stage.wait_post_us, m_Stage.wait_concat_us);
    }

    double dtMs = totalUs / 1000.0;
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

// ── probeInferenceCost ────────────────────────────────────────────────────
//
// VipleStream v1.2.87: measure how long one DML/ORT inference takes
// at this backend's current resolution.  Drives the auto-resolution
// cascade in D3D11VARenderer::initFRUC().
//
// Strategy:
//   1. Run `warmup` inferences with no timing — first ORT::Run on a
//      DML EP session is always slower (kernel selection / JIT) and
//      doesn't reflect steady-state cost.
//   2. Run `iterations` timed inferences. Each one Signals a fence
//      after the unpack CL submission and CPU-waits for the fence
//      to reach that value — that's how we measure end-to-end GPU
//      time per frame instead of just CPU enqueue time.
//   3. Return the median timed result (resilient to one spike).
//
// All ops run on uninitialised GPU tensor data — DML inference cost
// is shape-dependent, not data-dependent.

double DirectMLFRUC::probeInferenceCost(int warmup, int iterations)
{
    if (!m_Initialized) return -1.0;
    if (!m_UseOrt && !m_DMLCompiledOp) return -1.0;
    if (warmup < 0)     warmup = 0;
    if (iterations < 1) iterations = 1;

    // v1.2.124: detect "ORT silently disabled mid-probe" path.  When
    // runOrtInference() catches an Ort::Exception (e.g. a DML kernel
    // rejecting a Concat at runtime that didn't trip during init), it
    // sets m_UseOrt=false and falls through to the inline DML graph
    // for subsequent calls.  Without this guard the probe would
    // average MIXED ORT+inline timings, return a low median, and the
    // cascade would think the ONNX path "passed" -- but production
    // would silently use the inline crossfade graph (= Generic-quality
    // output).  By snapshotting m_UseOrt entry-state and re-checking
    // at exit, we report -1 to the cascade so it tries the next model
    // (fp16 -> fp32 -> Generic).
    const bool wantedOrtAtEntry = m_UseOrt;

    // Helper that fully drains the GPU queue up to a given fence value.
    auto cpuWaitFence = [&](uint64_t fenceVal) {
        if (m_Fence12->GetCompletedValue() >= fenceVal) return;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) return;
        if (SUCCEEDED(m_Fence12->SetEventOnCompletion(fenceVal, ev))) {
            // 2 s cap — 100 ms × 20 iterations is plenty even for the
            // slowest GPUs we'd consider; longer means something
            // is wrong and we should bail rather than hang init.
            WaitForSingleObject(ev, 2000);
        }
        CloseHandle(ev);
    };

    // Run a single inference path that mirrors runDMLDispatch+post-
    // unpack, but skips the pack CS (we don't have a real source
    // texture here). DML reads m_FrameTensor[0]/[1] directly — for
    // timing they can be uninitialised garbage.
    auto runOne = [&]() -> uint64_t {
        // Bind ping-pong inputs every probe iteration so DML reads
        // valid descriptors (the binding table was set up at init
        // for slot 0 → slot 1).
        if (!m_UseOrt && m_DMLCompiledOp) rebindPingPongInputs();

        if (m_UseOrt) {
            runOrtInference();
        } else {
            // Inline DML: record + submit dispatch only.
            const uint32_t slot = m_CmdAllocIdx;
            waitFenceBeforeAllocReset(m_Fence12.Get(), m_CmdAllocFenceVal[slot]);
            auto& alloc = m_CmdAllocRing[slot];
            alloc->Reset();
            m_CmdList12->Reset(alloc.Get(), nullptr);
            ID3D12DescriptorHeap* dmlHeaps[] = { m_DMLDescHeap.Get() };
            m_CmdList12->SetDescriptorHeaps(1, dmlHeaps);
            m_DMLRecorder->RecordDispatch(m_CmdList12.Get(),
                                          m_DMLCompiledOp.Get(),
                                          m_DMLBindingTable.Get());
            m_CmdList12->Close();
            ID3D12CommandList* c[] = { m_CmdList12.Get() };
            m_Queue12->ExecuteCommandLists(1, c);
            m_CmdAllocFenceVal[slot] = ++m_FenceValue;
            m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
            m_CmdAllocIdx = (m_CmdAllocIdx + 1) % kAllocRingSize;
        }
        // Always Signal at the end so we have a fence to wait on.
        // ORT path's runOrtInference doesn't bump m_FenceValue itself
        // (its own concat-Signal aside), so we add a closing one here.
        ++m_FenceValue;
        m_Queue12->Signal(m_Fence12.Get(), m_FenceValue);
        return m_FenceValue;
    };

    LARGE_INTEGER qpfFreq; QueryPerformanceFrequency(&qpfFreq);
    auto msNow = [&]() {
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return (double)t.QuadPart * 1000.0 / (double)qpfFreq.QuadPart;
    };

    // Warmup — discard timings.
    for (int i = 0; i < warmup; ++i) {
        uint64_t fv = runOne();
        cpuWaitFence(fv);
        // v1.2.125: bail immediately if ORT got disabled mid-warmup.
        // The else-branch in runOne() expects m_DMLCompiledOp, which
        // may be null when init went down the ORT-only path -- letting
        // the loop continue would crash with a null deref / access
        // violation when RecordDispatch dereferences m_DMLCompiledOp.
        if (wantedOrtAtEntry && !m_UseOrt) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML probe @ %ux%u: ORT disabled in warmup -- "
                        "treating as probe failure (cascade will try next model)",
                        m_Width, m_Height);
            return -1.0;
        }
    }

    // Timed runs.
    std::vector<double> samples;
    samples.reserve(iterations);
    for (int i = 0; i < iterations; ++i) {
        double t0 = msNow();
        uint64_t fv = runOne();
        cpuWaitFence(fv);
        samples.push_back(msNow() - t0);
        if (wantedOrtAtEntry && !m_UseOrt) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] DirectML probe @ %ux%u: ORT disabled mid-iteration -- "
                        "treating as probe failure (cascade will try next model)",
                        m_Width, m_Height);
            return -1.0;
        }
    }

    std::sort(samples.begin(), samples.end());
    double median = samples[samples.size() / 2];

    // v1.2.124: ORT was disabled mid-probe -> probe is invalid (mixed
    // path timings).  Tell the cascade to try the next model variant.
    if (wantedOrtAtEntry && !m_UseOrt) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] DirectML probe @ %ux%u: ORT disabled mid-probe "
                    "(model has runtime DML kernel issue) -- treating as probe failure",
                    m_Width, m_Height);
        return -1.0;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] DirectML probe @ %ux%u: median=%.2fms "
                "(min=%.2f max=%.2f n=%d, warmup=%d)",
                m_Width, m_Height, median,
                samples.front(), samples.back(), iterations, warmup);
    return median;
}
