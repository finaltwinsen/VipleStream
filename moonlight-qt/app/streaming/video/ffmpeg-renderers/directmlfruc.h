// VipleStream: DirectML-backed frame interpolation backend.
//
// Alternative to GenericFRUC's compute-shader pipeline. Runs a
// DirectML graph on a D3D12 device that shares the same adapter as
// the renderer's D3D11 device — interp output lands back in an
// ID3D11Texture2D so the rest of the render path is unchanged.
//
// The initial graph is intentionally small (bidirectional mean) — it
// exercises the full DirectML plumbing (D3D12 device, DML device,
// tensor layout, compiled operator, command recorder, fence signal)
// without needing external model weights. The real target is to
// replace the single op with a compiled ONNX FRUC model (RIFE /
// FLAVR / custom) once the interop path has been validated on user
// hardware; search for `TODO(directml:model)` below for the hook.
//
// Requires: Windows 10 1903+ for system DirectML.dll and D3D12 on
// the render adapter (every discrete GPU since Kepler/GCN and every
// Intel iGPU since Gen9 satisfy this — including the UHD 630 /
// RTX A1000 hybrid on the dev box).

#pragma once

#ifdef _WIN32

#include <d3d11.h>
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

    // DirectML backend has a single fused "interp" dispatch rather
    // than the ME/median/warp triptych of GenericFRUC. We map it
    // onto getLastWarpTimeMs() so the existing [VIPLE-FRUC-Stats]
    // log line still surfaces a useful number; ME/median stay at 0.
    double getLastMeTimeMs()     const override { return 0.0; }
    double getLastMedianTimeMs() const override { return 0.0; }
    double getLastWarpTimeMs()   const override { return m_LastInterpMs; }

    const char* backendName() const override { return "DirectML"; }

private:
    using MsComPtr = Microsoft::WRL::ComPtr<IUnknown>;
    template<class T> using CP = Microsoft::WRL::ComPtr<T>;

    // --- init helpers ---
    bool createD3D12Device();
    bool createDMLDevice();
    bool createD3D11Textures();
    bool createD3D12TensorResources();
    bool compileDMLGraph();
    bool createCommandInfra();

    // --- per-frame helpers ---
    // Upload the current render texture into the "curr" tensor
    // slot. The prev slot is satisfied by ping-ponging the two
    // tensors between frames (D4 iter 3) — no re-upload needed.
    bool uploadCurrToTensor(ID3D11DeviceContext* ctx);
    bool executeDMLGraph();
    bool downloadTensorToOutput(ID3D11DeviceContext* ctx);

    // Rebuild the binding table so the "prev" input points at the
    // tensor that held last frame's "curr" data, and the "curr"
    // input points at the empty slot that uploadCurrToTensor fills.
    // Skips re-uploading data that the GPU already has.
    bool rebindPingPongInputs();

    // --- members ---
    bool m_Initialized = false;
    int  m_QualityLevel = 1;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    int  m_FrameCount = 0;
    double m_LastInterpMs = 0.0;

    // D3D11 side — matches GenericFRUC's shape so the renderer can
    // swap backends without changing the draw path.
    ID3D11Device* m_Device11 = nullptr;
    CP<ID3D11Texture2D>          m_RenderTexture;
    CP<ID3D11RenderTargetView>   m_RenderRTV;
    CP<ID3D11ShaderResourceView> m_RenderSRV;
    CP<ID3D11Texture2D>          m_PrevTexture;       // snapshot of previous render
    CP<ID3D11Texture2D>          m_OutputTexture;     // interpolated frame (caller reads this)
    CP<ID3D11ShaderResourceView> m_OutputSRV;

    // Staging textures for D3D11 -> CPU -> D3D12 roundtrip. First
    // ship uses the CPU path for simplicity; a shared-heap zero-copy
    // path is TODO(directml:zerocopy).
    CP<ID3D11Texture2D> m_StagingRead;    // GPU -> CPU
    CP<ID3D11Texture2D> m_StagingWrite;   // CPU -> GPU

    // D3D12 + DML
    CP<ID3D12Device>          m_Device12;
    CP<ID3D12CommandQueue>    m_Queue12;
    CP<ID3D12CommandAllocator> m_CmdAlloc12;
    CP<ID3D12GraphicsCommandList> m_CmdList12;
    CP<ID3D12Fence>           m_Fence12;
    HANDLE                    m_FenceEvent = nullptr;
    uint64_t                  m_FenceValue = 0;

    CP<IDMLDevice>                 m_DMLDevice;
    CP<IDMLCommandRecorder>        m_DMLRecorder;
    CP<IDMLCompiledOperator>       m_DMLCompiledOp;
    CP<IDMLBindingTable>           m_DMLBindingTable;
    CP<ID3D12DescriptorHeap>       m_DMLDescHeap;

    // Tensor buffers — two "frame" slots that ping-pong between
    // "prev" and "curr" bindings every frame (D4 iter 3). Saves a
    // full-frame CPU upload per submit: what was "curr" last frame
    // is already sitting in VRAM, we just flip the binding indices.
    CP<ID3D12Resource> m_FrameTensor[2];  // [m_WriteSlot] = curr, [1 - m_WriteSlot] = prev
    CP<ID3D12Resource> m_OutputTensor;
    CP<ID3D12Resource> m_TempResource;
    CP<ID3D12Resource> m_PersistentResource;

    CP<ID3D12Resource> m_UploadCurr;       // UPLOAD heap; only one
                                           // upload per frame now.
    CP<ID3D12Resource> m_ReadbackOutput;

    // D4 iter 2: UPLOAD/READBACK resources stay persistently mapped
    // between frames. Map/Unmap is non-trivial on write-combined
    // memory; doing it per-frame shows up as measurable overhead in
    // the CPU path. The pointers are valid until destroy() calls
    // Unmap.
    void*  m_UploadCurrPtr    = nullptr;
    void*  m_ReadbackOutputPtr = nullptr;

    int m_WriteSlot = 0;  // which index of m_FrameTensor holds the
                          // "curr" (newest) upload.

    // Tensor sizing — RGBA FP16, NCHW layout ([1, 4, H, W]). Using
    // FP16 halves the bandwidth and matches how most shipped FRUC
    // ONNX models expect their inputs.
    static constexpr DML_TENSOR_DATA_TYPE kDmlDtype = DML_TENSOR_DATA_TYPE_FLOAT16;
    uint32_t m_TensorElements = 0;
    uint32_t m_TensorBytes    = 0;
};

#endif // _WIN32
