// VipleStream: DirectML-backed frame interpolation backend.
//
// Architecture (D5 — zero-copy):
//
// Tensor buffers live in shared VRAM heaps created on D3D12 with
// D3D12_HEAP_FLAG_SHARED; the same resources are opened on the
// renderer's D3D11 device via ID3D11Device1::OpenSharedResource1.
// Pack / unpack between the RGBA8 render texture and the planar
// FP16 tensor happens entirely on the GPU via a pair of D3D11
// compute shaders (d3d11_dml_pack_rgba8_fp16 / _unpack). A single
// shared ID3D12Fence is signalled/waited across both APIs so the
// D3D11 immediate context and the D3D12 DML queue serialise
// correctly without a CPU-side WaitForSingleObject each frame.
//
// The whole per-frame CPU path is now "bind, dispatch, signal,
// wait, dispatch, signal" — no Map, no staging textures, no SIMD
// quantise loops, no pageable memory in the hot path.
//
// TODO(directml:model): the DML graph itself remains the trivial
// DML_ELEMENT_WISE_ADD1 bidirectional mean; with interop cost now
// close to zero, the next round swaps this for a compiled ONNX
// FRUC model (RIFE v4 lite is the first candidate).
//
// Requires Windows 10 1903+ for system DirectML.dll, D3D12 on the
// render adapter, and D3D11.3+ interfaces (ID3D11Device5,
// ID3D11DeviceContext4) which ship with Windows 10 1703+.

#pragma once

#ifdef _WIN32

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <DirectML.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include "ifrucbackend.h"

// ONNX Runtime forward-declare wrappers (full includes live only in
// directmlfruc.cpp so the rest of the project doesn't have to
// ingest the ORT headers). The pointer-to-impl keeps the TU count
// and rebuild cost down.
namespace Ort { class Env; class Session; }
struct OrtDmlApi;

class DirectMLFRUC : public IFRUCBackend {
public:
    DirectMLFRUC();
    ~DirectMLFRUC() override;

    bool initialize(ID3D11Device* device, uint32_t width, uint32_t height) override;
    void destroy() override;

    bool submitFrame(ID3D11DeviceContext* ctx, double timestamp) override;
    void skipFrame(ID3D11DeviceContext* ctx) override;

    ID3D11RenderTargetView*   getRenderRTV()     const override { return m_RenderRTV.Get(); }
    ID3D11ShaderResourceView* getLastRenderSRV() const override { return m_RenderSRV.Get(); }
    ID3D11ShaderResourceView* getOutputSRV()     const override { return m_OutputSRV.Get(); }
    ID3D11Texture2D*          getOutputTexture() const override { return m_OutputTexture.Get(); }

    bool isInitialized() const override { return m_Initialized; }
    void setQualityLevel(int level) override { m_QualityLevel = (level >= 0 && level <= 2) ? level : 1; }

    double getLastMeTimeMs()     const override { return 0.0; }
    double getLastMedianTimeMs() const override { return 0.0; }
    double getLastWarpTimeMs()   const override { return m_LastInterpMs; }

    const char* backendName() const override { return "DirectML"; }

private:
    template<class T> using CP = Microsoft::WRL::ComPtr<T>;

    bool createD3D12Device();
    bool createDMLDevice();
    bool createSharedFence();
    bool createD3D11Textures();
    bool createSharedTensorBuffers();
    bool createD3D11Views();
    bool loadComputeShaders();
    bool compileDMLGraph();
    // Optional: try to load an ONNX FRUC model from the data dir.
    // Returns true if a model was loaded and m_UseOrt is now true;
    // false (without logging an error) if no model file exists —
    // that's the normal path for users who didn't opt in.
    bool tryLoadOnnxModel();

    bool rebindPingPongInputs();  // DML input binding swap each frame.
    void runPackCS(ID3D11DeviceContext4* ctx4, int slot);
    bool runDMLDispatch();
    bool runOrtInference();       // ORT path of runDMLDispatch().
    void runUnpackCS(ID3D11DeviceContext4* ctx4);

    // Record resource-state transition barriers for the three shared
    // tensor buffers (both frame tensors + output tensor) into cl.
    // Call with before=COMMON / after=UAV before DML/ORT dispatch and
    // before=UAV / after=COMMON afterwards so D3D11 can safely use
    // the resources again.
    void recordSharedTensorBarriers(ID3D12GraphicsCommandList* cl,
                                    D3D12_RESOURCE_STATES before,
                                    D3D12_RESOURCE_STATES after);

    // --- common state ---
    bool m_Initialized = false;
    int  m_QualityLevel = 1;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    int  m_FrameCount = 0;
    double m_LastInterpMs = 0.0;

    // --- D3D11 side ---
    ID3D11Device* m_Device11 = nullptr;
    CP<ID3D11Device5>             m_Device11_5;      // for OpenSharedFence
    CP<ID3D11Texture2D>           m_RenderTexture;
    CP<ID3D11RenderTargetView>    m_RenderRTV;
    CP<ID3D11ShaderResourceView>  m_RenderSRV;
    CP<ID3D11Texture2D>           m_OutputTexture;
    CP<ID3D11ShaderResourceView>  m_OutputSRV;
    CP<ID3D11UnorderedAccessView> m_OutputUAV;       // target of unpack CS

    // Shared tensor resources opened as D3D11 buffers. UAVs drive
    // the pack CS (D3D11 writer); SRVs drive the unpack CS (D3D11
    // reader from the DML output buffer).
    CP<ID3D11Buffer>              m_FrameBuffer11[2];
    CP<ID3D11UnorderedAccessView> m_FrameBufferUAV11[2];
    CP<ID3D11Buffer>              m_OutputBuffer11;
    CP<ID3D11ShaderResourceView>  m_OutputBufferSRV11;

    CP<ID3D11ComputeShader>       m_PackCS;
    CP<ID3D11ComputeShader>       m_UnpackCS;
    CP<ID3D11Buffer>              m_PackConstBuf;    // width/height uniforms

    CP<ID3D11Fence>               m_Fence11;         // opened from shared D3D12 fence

    // --- D3D12 + DML ---
    CP<ID3D12Device>              m_Device12;
    CP<ID3D12CommandQueue>        m_Queue12;
    CP<ID3D12CommandAllocator>    m_CmdAlloc12;
    CP<ID3D12GraphicsCommandList> m_CmdList12;
    // Second allocator/list used exclusively for cross-API barrier
    // submissions in the ORT path. Kept separate from m_CmdAlloc12
    // because both pre-dispatch and post-dispatch barriers must be
    // submitted to the queue without a CPU stall between them; reusing
    // a single allocator would require waiting for the GPU to drain.
    CP<ID3D12CommandAllocator>    m_BarrierCmdAlloc12;
    CP<ID3D12GraphicsCommandList> m_BarrierCmdList12;
    CP<ID3D12Fence>               m_Fence12;         // SHARED, cross-API

    CP<IDMLDevice>                m_DMLDevice;
    CP<IDMLCommandRecorder>       m_DMLRecorder;
    CP<IDMLCompiledOperator>      m_DMLCompiledOp;
    CP<IDMLBindingTable>          m_DMLBindingTable;
    CP<ID3D12DescriptorHeap>      m_DMLDescHeap;

    // Shared tensor resources on D3D12 (ping-pong input slots +
    // output). Created with D3D12_HEAP_FLAG_SHARED; NT handles
    // forwarded to D3D11 at init time.
    CP<ID3D12Resource>            m_FrameTensor[2];
    CP<ID3D12Resource>            m_OutputTensor;
    CP<ID3D12Resource>            m_TempResource;
    CP<ID3D12Resource>            m_PersistentResource;

    // Cross-API fence counter. Each submit advances it by 2 — one
    // signal after pack, one after DML. Separate counters per API
    // aren't needed: the fence is strictly monotonic.
    uint64_t m_FenceValue = 0;
    int      m_WriteSlot  = 0;

    // FP32 internally: matches the dtype of every public RIFE / FLAVR /
    // IFRNet ONNX export in the wild, so models drop in without an
    // offline conversion step. Costs ~2x the tensor VRAM vs FP16 but
    // still well under 100 MB on 1080p which every target GPU handles.
    static constexpr DML_TENSOR_DATA_TYPE kDmlDtype = DML_TENSOR_DATA_TYPE_FLOAT32;
    uint32_t m_TensorElements = 0;  // = 4 * W * H
    uint32_t m_TensorBytes    = 0;  // = elements * 4

    // --- ONNX Runtime (optional; activated when fruc.onnx exists) ---
    bool                           m_UseOrt = false;
    std::unique_ptr<Ort::Session>  m_OrtSession;
    const OrtDmlApi*               m_OrtDmlApi = nullptr;
    // Opaque ORT-owned wrappers for our shared D3D12 resources. 1:1
    // with m_FrameTensor[0], m_FrameTensor[1], m_OutputTensor so
    // ORT can bind them with zero copies.
    void*                          m_OrtAllocFrame[2] = { nullptr, nullptr };
    void*                          m_OrtAllocOutput   = nullptr;
    // Model-contract metadata learnt at load time. Drives per-frame
    // tensor binding (channel count, extra timestep input, alpha
    // passthrough path in the unpack shader).
    uint32_t                       m_ModelChannels = 4;     // 3 or 4
    bool                           m_HasTimestep = false;   // RIFE v4+ has a scalar 3rd input
    float                          m_TimestepValue = 0.5f;  // midpoint for 2x FRUC
    Microsoft::WRL::ComPtr<ID3D12Resource> m_TimestepResource;
    void*                          m_OrtAllocTimestep = nullptr;
    // Cached session metadata — names and shape we validated at
    // model-load time. Session::Run wants the names every call.
    std::vector<std::string>       m_OrtInputNames;
    std::vector<std::string>       m_OrtOutputNames;
    std::vector<const char*>       m_OrtInputNamesCStr;
    std::vector<const char*>       m_OrtOutputNamesCStr;
    // Actual shape of the timestep input as exported by the model.
    // Stored at load time so runOrtInference can create a tensor with
    // the exact shape the model declares (e.g. [1] vs [1,1,1,1]).
    // All dynamic dims (-1) are folded to 1 since we always pass a
    // single 0.5 value regardless of broadcast shape.
    std::vector<int64_t>           m_TimestepShape;
};

#endif // _WIN32
