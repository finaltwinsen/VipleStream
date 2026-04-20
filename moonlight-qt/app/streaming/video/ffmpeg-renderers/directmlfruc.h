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

#include "ifrucbackend.h"

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

    bool rebindPingPongInputs();  // DML input binding swap each frame.
    void runPackCS(ID3D11DeviceContext4* ctx4, int slot);
    bool runDMLDispatch();
    void runUnpackCS(ID3D11DeviceContext4* ctx4);

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

    static constexpr DML_TENSOR_DATA_TYPE kDmlDtype = DML_TENSOR_DATA_TYPE_FLOAT16;
    uint32_t m_TensorElements = 0;  // = 4 * W * H
    uint32_t m_TensorBytes    = 0;  // = elements * 2
};

#endif // _WIN32
