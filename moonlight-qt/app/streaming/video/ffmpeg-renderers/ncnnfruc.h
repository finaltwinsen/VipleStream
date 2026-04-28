// VipleStream §I.F (v1.3.x) — NCNN-Vulkan FRUC backend.
//
// Cross-vendor (NV/AMD/Intel) RIFE inference via Tencent ncnn's
// Vulkan compute path.  Sits between DirectMLFRUC and GenericFRUC
// in d3d11va.cpp's cascade — preferred over DML on platforms where
// ORT's DML EP partitioner is suboptimal (e.g. RTX 3060 Laptop:
// DML 70-80 ms vs NCNN 30-45 ms for RIFE 4.25-lite at 1080p).
//
// Phase 1 (this file): minimal skeleton — model load + Vulkan
// extractor + probe-only path with dummy ncnn::Mat tensors.  Owns
// D3D11 placeholder textures so the IFRUCBackend interface contract
// is honoured but submitFrame is currently a no-op (returns false).
// Phase 2 will land the D3D11→D3D12→Vulkan shared-texture bridge.

#pragma once

#ifdef _WIN32

#include "ifrucbackend.h"

#include <wrl/client.h>
#include <d3d11.h>

#include <atomic>
#include <memory>
#include <string>

#include <ncnn/mat.h>

namespace ncnn {
class Net;
class VulkanDevice;
class Pipeline;
}

class NcnnFRUC : public IFRUCBackend {
public:
    NcnnFRUC();
    ~NcnnFRUC() override;

    // IFRUCBackend
    bool initialize(ID3D11Device* device, uint32_t width, uint32_t height) override;
    void destroy() override;

    // Phase 1: submitFrame is a no-op (returns false).  The cascade
    // probe runs first and only commits to this backend if probe
    // passes the budget; until phase 2 wires the shared-texture
    // bridge, we'd rather decline than emit garbage frames.
    bool submitFrame(ID3D11DeviceContext* ctx, double timestamp) override;
    void skipFrame(ID3D11DeviceContext* ctx) override;

    ID3D11RenderTargetView*    getRenderRTV()      const override { return m_RenderRTV.Get(); }
    ID3D11ShaderResourceView*  getLastRenderSRV()  const override { return m_LastSRV.Get(); }
    ID3D11ShaderResourceView*  getOutputSRV()      const override { return m_OutputSRV.Get(); }
    ID3D11Texture2D*           getOutputTexture()  const override { return m_OutputTex.Get(); }

    bool isInitialized() const override { return m_Initialized.load(std::memory_order_acquire); }
    void setQualityLevel(int level) override { m_Quality = level; }

    double getLastMeTimeMs()     const override { return 0.0; }
    double getLastMedianTimeMs() const override { return m_LastInferenceMs; }
    double getLastWarpTimeMs()   const override { return 0.0; }

    const char* backendName() const override { return "NCNN-Vulkan"; }

    // ---- Cascade-specific (mirrors DirectMLFRUC's API surface) ----

    // Caller picks the model directory under Path::getDataFilePath().
    // Default "rife-v4.25-lite" matches the bundle in
    // build_moonlight_package.cmd.
    void setModelDir(const std::string& dir) { m_ModelDir = dir; }

    // Run the loaded RIFE forward N times on dummy NCHW inputs sized
    // to (1, 3, m_Height, m_Width); returns median wall-clock ms or
    // negative on failure.  Used by d3d11va.cpp to decide whether
    // this backend fits the half-rate frame budget at this resolution
    // on this GPU.
    double probeInferenceCost(int warmup, int iterations);

private:
    bool loadModel();
    // Phase B.3: open the DXGI shared handles as Vulkan VkImage +
    // VkDeviceMemory pointing at the same physical memory.  Returns
    // false (and logs reason) if any step fails — caller leaves
    // m_SharedPathReady=false and the CPU-staging Phase A.5 path
    // continues to operate as before.  Idempotent: nullable input
    // handles are tolerated.
    bool importSharedTexturesIntoVulkan();

    // Phase B.4a: compile the two compute shaders that turn packed
    // RGBA8 buffers ↔ planar fp32 buffers (ncnn-compatible).  These
    // are the GPU replacement for rgba8RowToPlanarFp32 /
    // planarFp32RowToRgba8 — running them on the same Vulkan device
    // as the RIFE inference avoids the ~30 ms CPU staging round-trip
    // per submitFrame.  Returns false on GLSL compile failure or
    // pipeline create failure; failure is non-fatal — m_SharedPathReady
    // stays false and CPU staging path continues to operate.
    bool compileSharedPathPipelines();

    // Phase B.4b: own command pool, command buffers, packed-RGBA8
    // staging VkMats, the D3D11 event query, and pre-uploaded
    // timestep tensor.  Called from initialize() after the pipelines
    // build successfully.  Returns false on any allocation failure
    // (logs reason); failure is non-fatal — m_SharedPathReady stays
    // false.  Resources are torn down in destroy().
    bool initSharedPathResources();

    // Phase B.4b: GPU-only path equivalent to submitFrame's CPU
    // staging body.  Only invoked when m_SharedPathReady is true.
    // Steps: D3D11 event-query sync → vkCmdCopyImageToBuffer
    // (imported render VkImage → m_PackedInVkMat) → pre-shader
    // dispatch → RIFE forward → post-shader dispatch →
    // vkCmdCopyBufferToImage (m_PackedOutVkMat → imported output
    // VkImage).  Returns true if interp output landed in m_OutputTex,
    // false on any failure (caller falls back to last frame as in the
    // CPU-staging path).
    bool submitFrameShared(ID3D11DeviceContext* ctx);

    // Phase B.4b: one-shot transition the imported render + output
    // VkImages from UNDEFINED to GENERAL on first use.  GENERAL is
    // the recommended layout for D3D11/Vulkan shared images (no
    // transitions needed afterwards, just memory barriers).
    bool transitionSharedImagesToGeneral();

    // Phase B.4c: end-to-end correctness check on the conversion
    // shaders.  Builds a 4×4 packed RGBA8 input with known pixel
    // values, dispatches pre → post → reads back, asserts
    // bit-pattern-close-to-original (loss is fp32→u8 round-trip
    // quantisation only).  Runs at init time; if it fails we never
    // flip m_SharedPathReady regardless of env var settings.
    bool selftestSharedPath();

    std::atomic<bool> m_Initialized { false };
    int               m_Quality     = 0;
    uint32_t          m_Width       = 0;
    uint32_t          m_Height      = 0;
    std::string       m_ModelDir    = "rife-v4.25-lite";

    // D3D11 layout (mirrors GenericFRUC):
    //   m_RenderTex (DEFAULT, RTV+SRV, SHARED_NTHANDLE in B.2+) — caller
    //   m_OutputTex (DEFAULT, SRV, SHARED_NTHANDLE in B.2+) — output
    //   m_StagingCurrTex / m_StagingOutTex — Phase A.5 CPU staging
    //                       fallback path (kept until shared-texture
    //                       wiring in B.4-5 is verified end-to-end)
    //   m_SharedRenderHandle / m_SharedOutputHandle — DXGI NT handles
    //                       exported from D3D11 textures, opened by
    //                       Vulkan in B.3 as imported VkImage backing
    //                       memory.
    Microsoft::WRL::ComPtr<ID3D11Device>             m_Device;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_RenderTex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   m_RenderRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_LastSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_StagingCurrTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_OutputTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_OutputSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_StagingOutTex;
    HANDLE                                           m_SharedRenderHandle = nullptr;
    HANDLE                                           m_SharedOutputHandle = nullptr;
    // Imported Vulkan resources (B.3+): same physical memory as the
    // D3D11 textures above, accessed via VK_KHR_external_memory_win32.
    void*                                            m_VkRenderImage      = nullptr;  // VkImage (opaque)
    void*                                            m_VkOutputImage      = nullptr;  // VkImage
    void*                                            m_VkRenderMem        = nullptr;  // VkDeviceMemory
    void*                                            m_VkOutputMem        = nullptr;  // VkDeviceMemory
    bool                                             m_SharedPathReady    = false;
    // Phase B.4a: GPU compute pipelines for RGBA8 ↔ planar fp32
    // conversion.  ncnn::Pipeline is the framework's wrapper around
    // VkPipeline + VkDescriptorSetLayout + VkPipelineLayout, with
    // shader-reflection done via SPIRV-Reflect during create().
    // Stored as opaque pointers to keep the header free of ncnn
    // private types — see ncnnfruc.cpp for the cast.
    ncnn::Pipeline*                                  m_PipelinePre        = nullptr;
    ncnn::Pipeline*                                  m_PipelinePost       = nullptr;
    // Phase B.4b: shared-path GPU resources.  All Vulkan handles
    // are stored as void* so the header doesn't need to drag in
    // <vulkan/vulkan.h> — the .cpp casts back to the typed handles
    // for use.  ncnn::VkMat instances embed VkBufferMemory* with
    // refcount; they're owned by ncnn's blob allocator and freed
    // when the allocator is reclaimed in destroy().
    void*                                            m_VkComputeQueue     = nullptr;  // VkQueue
    uint32_t                                         m_VkComputeQueueFamily = 0;
    void*                                            m_VkCmdPool          = nullptr;  // VkCommandPool
    void*                                            m_VkCmdBufImgIn      = nullptr;  // VkCommandBuffer (img→buf)
    void*                                            m_VkCmdBufImgOut     = nullptr;  // VkCommandBuffer (buf→img)
    ncnn::VkMat                                      m_PackedInVkMat;                 // RGBA8 staging (W*H uint32, c=1)
    ncnn::VkMat                                      m_PackedOutVkMat;                // RGBA8 staging output
    ncnn::VkMat                                      m_PrevVkMat;                     // planar fp32 prev frame (W,H,3)
    ncnn::VkMat                                      m_TimestepVkMat;                 // constant 0.5 (W,H,1)
    // v1.3.39: track the blob allocator we acquired so destroy() can
    // reclaim it.  Without this every destroy/init cycle (e.g. AV1
    // codec re-init) would leak one allocator off the device pool;
    // pool exhaustion eventually crashes vkCreateBuffer.
    ncnn::VkAllocator*                               m_BlobAllocator      = nullptr;
    bool                                             m_VkImagesInGeneralLayout = false;
    void*                                            m_D3D11SyncQuery     = nullptr;  // ID3D11Query (event)
    // v1.3.41: count consecutive shared-path failures; after 3 we
    // permanently flip m_SharedPathReady=false and stay on CPU staging
    // for the rest of this NcnnFRUC instance's lifetime.  Avoids
    // 785-line warn spam and per-frame retry cost when the shared path
    // is fundamentally broken on this GPU/driver.
    int                                              m_SharedPathFailCount = 0;

    // NCNN bits.  m_Net is the loaded RIFE 4.25-lite flownet
    // (3-input: in0/in1/in2 = prev RGB, curr RGB, timestep).
    // m_PrevMat is the previous-frame fp32 normalized RGB tensor we
    // feed into in0 on each forward; m_FrameCount==0 means "no prev
    // yet, just stash and skip".  m_TimestepMat is constant-fill 0.5
    // for midpoint interpolation.
    std::unique_ptr<ncnn::Net> m_Net;
    int                        m_GpuIndex = 0;
    double                     m_LastInferenceMs = 0.0;
    ncnn::Mat                  m_PrevMat;
    ncnn::Mat                  m_TimestepMat;
    int                        m_FrameCount = 0;
};

#endif // _WIN32
