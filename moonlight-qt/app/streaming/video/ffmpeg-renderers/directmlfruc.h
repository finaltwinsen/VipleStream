// VipleStream: DirectML-backed frame interpolation backend.
//
// Architecture (D6 — D3D11-texture-share):
//
// D3D11 owns the render and output textures (created with
// D3D11_RESOURCE_MISC_SHARED_NTHANDLE).  D3D12 opens them via
// IDXGIResource1::CreateSharedHandle + ID3D12Device::OpenSharedHandle —
// the well-supported direction for cross-API texture sharing.
// D3D12 runs pack and unpack compute shaders on its own queue using
// those shared textures and private (non-shared) tensor buffers.
// A cross-API fence serialises D3D11 rendering with D3D12 FRUC work.
//
// Per-frame sequence (frame > 0):
//
//   D3D11:  Unbind RTV, Signal fence=N
//   D3D12:  Wait fence=N
//              Pack CS     : shared render tex  → FrameTensor[slot]
//              DML / ORT   : FrameTensor        → OutputTensor
//              Unpack CS   : OutputTensor       → shared output tex
//            Signal fence=N+1
//   D3D11:  Wait fence=N+1  (GPU-side, CPU not blocked)
//           Blit shared output texture to screen
//
// No D3D12→D3D11 buffer sharing (unsupported on many drivers),
// no CPU staging, no SIMD loops, no Map/Unmap per frame.
//
// Requires Windows 10 1703+ (ID3D11Device5 / ID3D11DeviceContext4
// for the cross-API fence) and a D3D12-capable adapter.

#pragma once

#ifdef _WIN32

#include <d3d11_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>   // ID3D12Debug, ID3D12InfoQueue
#include <dxgi1_4.h>
#include <DirectML.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include "ifrucbackend.h"

// ONNX Runtime declares Env/Session as `struct` in its own public
// header; match that tag so MSVC doesn't emit C4099 warnings when
// onnxruntime_cxx_api.h is later included in the .cpp.
namespace Ort { struct Env; struct Session; }
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

    // VipleStream v1.2.58: diagnostic helpers for DirectML init
    // failures on RTX A1000 (0x887a0005 with reason=0x0). Enabled
    // only when VIPLE_DIRECTML_DEBUG=1 is set in the environment.
    static bool isDmlDebugEnabled();
    void        logD3D12DebugMessages(const char* where);

    // Init helpers — called in order from initialize().
    bool createD3D12Device();
    bool createDMLDevice();
    bool createSharedFence();
    bool createTensorBuffers();           // private D3D12 buffers (no sharing)
    bool createD3D11Textures();           // D3D11 textures with SHARED_NTHANDLE
    bool openD3D11TexturesInD3D12();      // D3D12 views of the D3D11 textures
    bool createD3D12CsPipeline();         // root-sig + PSOs + descriptor heap
    bool compileDMLGraph();               // fallback inline DML ADD1+CLIP graph
    bool tryLoadOnnxModel();              // optional RIFE/FLAVR/IFRNet model

    // Tier 3: when compileDMLGraph() fails on the user's hardware
    // (DML runtime bug returning 0x887A0005 on binding-table reset)
    // we still want frame interpolation to work. The D3D12 blend
    // compute shader replicates the inline DML graph's semantics
    // — 0.5*(prev + curr) clipped to [0,1] — without touching any
    // IDMLOperator / IDMLBindingTable machinery at all.
    void recordBlendCS(ID3D12GraphicsCommandList* cl);

    // Per-frame helpers.
    bool runDMLDispatch();
    bool runOrtInference();
    bool rebindPingPongInputs();

    // Record the D3D12 pack CS (render tex → tensor[slot]) into cl.
    void recordPackCS(ID3D12GraphicsCommandList* cl, int slot);
    // Record the D3D12 unpack CS (tensor → output tex) into cl,
    // including the COMMON↔UAV barriers for the shared output texture.
    void recordUnpackCS(ID3D12GraphicsCommandList* cl);

    // --- common state ---
    bool     m_Initialized = false;
    int      m_QualityLevel = 1;
    uint32_t m_Width  = 0;
    uint32_t m_Height = 0;
    int      m_FrameCount = 0;
    double   m_LastInterpMs = 0.0;

    // --- D3D11 side ---
    ID3D11Device*                  m_Device11 = nullptr;
    CP<ID3D11Device5>              m_Device11_5;   // for OpenSharedFence
    // Render texture: D3D11 writes (RTV), D3D12 reads (pack CS SRV).
    // Created with SHARED_NTHANDLE so D3D12 can open it.
    CP<ID3D11Texture2D>            m_RenderTexture;
    CP<ID3D11RenderTargetView>     m_RenderRTV;
    CP<ID3D11ShaderResourceView>   m_RenderSRV;    // for getLastRenderSRV()
    // Output texture: D3D12 writes (unpack CS UAV), D3D11 reads (blit SRV).
    // Also created with SHARED_NTHANDLE.
    CP<ID3D11Texture2D>            m_OutputTexture;
    CP<ID3D11ShaderResourceView>   m_OutputSRV;    // for final blit
    CP<ID3D11Fence>                m_Fence11;       // opened from shared D3D12 fence

    // --- D3D12 + cross-API ---
    CP<ID3D12Device>               m_Device12;
    CP<ID3D12CommandQueue>         m_Queue12;
    CP<ID3D12CommandAllocator>     m_CmdAlloc12;
    CP<ID3D12GraphicsCommandList>  m_CmdList12;
    // Second pair used exclusively for the ORT path post-dispatch
    // command list (unpack CS + barriers).  Kept separate so both
    // pre- and post-ORT command lists can be in-flight simultaneously
    // without a CPU stall between them.
    CP<ID3D12CommandAllocator>     m_PostCmdAlloc12;
    CP<ID3D12GraphicsCommandList>  m_PostCmdList12;
    CP<ID3D12Fence>                m_Fence12;       // SHARED, cross-API

    // D3D12 views of the shared D3D11 render / output textures.
    // Opened via ID3D12Device::OpenSharedHandle from IDXGIResource1 handles.
    CP<ID3D12Resource>             m_SharedRenderTex12;
    CP<ID3D12Resource>             m_SharedOutputTex12;

    // D3D12 pack/unpack/blend compute pipeline.
    // All three PSOs share one root signature with 4 descriptor-table
    // parameters. Pack/unpack use params 0/1/2 (and ignore 3); blend
    // uses 0/1/2/3 so it can bind two SRVs (t0 + t1) simultaneously.
    CP<ID3D12RootSignature>        m_CsRS12;
    CP<ID3D12PipelineState>        m_PackPSO12;
    CP<ID3D12PipelineState>        m_UnpackPSO12;
    CP<ID3D12PipelineState>        m_BlendPSO12;    // Tier 3 fallback
    // Descriptor heap for CS bindings (9 descriptors, shader-visible):
    //   [0] CBV — pack/unpack/blend constant buffer
    //   [1] SRV — render texture         (pack input)
    //   [2] UAV — FrameTensor[0]         (pack slot-0 output)
    //   [3] UAV — FrameTensor[1]         (pack slot-1 output)
    //   [4] SRV — OutputTensor           (unpack input)
    //   [5] UAV — output texture         (unpack output)
    //   [6] SRV — FrameTensor[0] buffer  (blend t0 — Tier 3)
    //   [7] SRV — FrameTensor[1] buffer  (blend t1 — Tier 3)
    //   [8] UAV — OutputTensor buffer    (blend u0 — Tier 3)
    CP<ID3D12DescriptorHeap>       m_CsDescHeap;
    uint32_t                       m_CsDescIncrSize = 0;
    // Constant buffer on UPLOAD heap, persistently mapped.
    CP<ID3D12Resource>             m_CsCB12;
    void*                          m_CsCBMapped = nullptr;

    // --- Tier 3 D3D12-native blend fallback ---
    // Set when compileDMLGraph() fails but createD3D12CsPipeline()
    // succeeded (i.e. m_BlendPSO12 is live). In that case runDMLDispatch()
    // dispatches m_BlendPSO12 instead of invoking the DML graph.
    bool                           m_BlendAvailable = false;

    // --- DML fallback graph ---
    CP<IDMLDevice>                 m_DMLDevice;
    CP<IDMLCommandRecorder>        m_DMLRecorder;
    CP<IDMLCompiledOperator>       m_DMLCompiledOp;
    CP<IDMLBindingTable>           m_DMLBindingTable;
    CP<ID3D12DescriptorHeap>       m_DMLDescHeap;
    CP<ID3D12Resource>             m_TempResource;
    CP<ID3D12Resource>             m_PersistentResource;

    // Tensor buffers — private D3D12 DEFAULT resources (no sharing).
    // Pack CS writes here, DML/ORT reads and writes here, Unpack CS reads.
    CP<ID3D12Resource>             m_FrameTensor[2];
    CP<ID3D12Resource>             m_OutputTensor;

    uint64_t m_FenceValue = 0;
    int      m_WriteSlot  = 0;

    static constexpr DML_TENSOR_DATA_TYPE kDmlDtype = DML_TENSOR_DATA_TYPE_FLOAT32;
    uint32_t m_TensorElements = 0;  // 4 * W * H
    uint32_t m_TensorBytes    = 0;  // elements * sizeof(float)

    // --- ONNX Runtime (optional) ---
    bool                           m_UseOrt = false;
    std::unique_ptr<Ort::Session>  m_OrtSession;
    const OrtDmlApi*               m_OrtDmlApi = nullptr;
    void*                          m_OrtAllocFrame[2] = {};
    void*                          m_OrtAllocOutput   = nullptr;
    uint32_t                       m_ModelChannels = 4;
    bool                           m_HasTimestep  = false;
    float                          m_TimestepValue = 0.5f;
    CP<ID3D12Resource>             m_TimestepResource;
    void*                          m_OrtAllocTimestep = nullptr;
    std::vector<std::string>       m_OrtInputNames;
    std::vector<std::string>       m_OrtOutputNames;
    std::vector<const char*>       m_OrtInputNamesCStr;
    std::vector<const char*>       m_OrtOutputNamesCStr;
    std::vector<int64_t>           m_TimestepShape;

    // 1-input concatenated layout (common for RIFE v4.x lite exports):
    //   Input 0: [1, C, H, W] where C ∈ {6, 7, 8, 9}
    //     C=6: prev RGB  + curr RGB           (no timestep)
    //     C=7: prev RGB  + curr RGB  + ts ch  (timestep = 1 plane of 0.5)
    //     C=8: prev RGBA + curr RGBA          (no timestep)
    //     C=9: prev RGBA + curr RGBA + ts ch  (timestep = 1 plane of 0.5)
    bool                           m_OrtConcatInput      = false;
    uint32_t                       m_ConcatChannels      = 0;    // total C of concat input
    uint32_t                       m_ConcatImageChannels = 0;    // 3 or 4
    bool                           m_ConcatHasTimestep   = false;
    CP<ID3D12Resource>             m_ConcatTensor;
    void*                          m_OrtAllocConcat      = nullptr;
    std::vector<int64_t>           m_ConcatShape;
};

#endif // _WIN32
