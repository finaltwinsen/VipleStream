#pragma once

#include "renderer.h"

#include <atomic>
#include <vector>
#include <d3d11_4.h>
#include <dxgi1_6.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

class D3D11VARenderer : public IFFmpegRenderer
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual void waitToRender() override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderCapabilities() override;
    virtual InitFailureReason getInitFailureReason() override;

    // VipleStream: FRUC stats & control
    virtual bool isFRUCActive() const override;
    virtual bool lastFrameHadFRUCInterp() const override;
    virtual const char* getFRUCBackendName() const override;
    virtual void toggleFRUC() override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
        _COUNT
    };

private:
    static void lockContext(void* lock_ctx);
    static void unlockContext(void* lock_ctx);

    bool setupRenderingResources();
    std::vector<DXGI_FORMAT> getVideoTextureSRVFormats();
    bool setupFrameRenderingResources(AVHWFramesContext* framesContext);
    bool setupSwapchainDependentResources();
    bool setupVideoTexture(AVHWFramesContext* framesContext); // for !m_BindDecoderOutputTextures
    bool setupTexturePoolViews(AVHWFramesContext* framesContext); // for m_BindDecoderOutputTextures
    void renderOverlay(Overlay::OverlayType type);
    bool createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, Microsoft::WRL::ComPtr<ID3D11Buffer>& newVertexBuffer);
    void bindColorConversion(bool frameChanged, AVFrame* frame);
    void bindVideoVertexBuffer(bool frameChanged, AVFrame* frame);
    void renderVideo(AVFrame* frame);
    // VipleStream: resize the D3D11 viewport. Used to switch between
    // FRUC-RT-sized (stream res) and swap-chain-sized (display res)
    // rendering within a single frame.
    void setViewport(int width, int height);
    bool checkDecoderSupport(IDXGIAdapter* adapter);
    bool createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound = nullptr);
    bool setupSharedDevice(IDXGIAdapter1* adapter);
    bool createSharedFencePair(UINT64 initialValue,
                               ID3D11Device5* dev1, ID3D11Device5* dev2,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev1Fence,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev2Fence);

    int m_DecoderSelectionPass;
    int m_DevicesWithFL11Support;
    int m_DevicesWithCodecSupport;

    enum class SupportedFenceType {
        None,
        NonMonitored,
        Monitored,
    };

    Microsoft::WRL::ComPtr<IDXGIFactory5> m_Factory;
    int m_AdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    bool m_BindDecoderOutputTextures;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    // VipleStream: FRUC render targets live at stream resolution (fruW x fruH).
    // When rendering video into them, the D3D viewport must match the RT size —
    // otherwise D3D clips the vertex-shader output to the RT bounds and only
    // the top-left stream_res/display_res fraction of the video ends up in the
    // FRUC texture, which blitFRUCTexture then stretches to full-screen giving
    // the visible "zoomed upper-left corner" aspect bug.
    int m_FrucTextureWidth = 0;
    int m_FrucTextureHeight = 0;
    AVColorTransferCharacteristic m_LastColorTrc;

    bool m_AllowTearing;
    HANDLE m_FrameLatencyWaitableObject;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    // VipleStream: Frame interpolation (lazy-init in renderFrame)
    // Tier 0: NvOFFRUC (NVIDIA hardware optical flow)
    class NvOFRUCWrapper* m_FRUC = nullptr;
    // Tier 1: any IFRUCBackend implementation (Generic compute shader
    // or DirectML). Name kept as m_GenericFRUC for minimal diff — the
    // backend concrete type is chosen at initFRUC() time per
    // prefs->frucBackend.
    class IFRUCBackend* m_GenericFRUC = nullptr;

    bool m_FRUCLastFrameInterpolated = false;
    bool m_FRUCInitFailed = false;

    // Deferred swap chain latency update (set by keyboard thread, applied by render thread)
    std::atomic<int> m_PendingSwapChainLatency{0};  // 0 = no change pending

    // VipleStream: frame gap detection for FRUC drop concealment.
    // We use a rolling-window average of recent gaps rather than the last gap
    // alone so isolated spikes don't latch skipFrame on permanently.
    uint64_t m_LastRenderTimeMs = 0;
    int m_RenderFrameCount = 0;
    static constexpr int FRUC_GAP_WINDOW = 8;
    uint64_t m_RecentGapsMs[FRUC_GAP_WINDOW] = {};
    int m_RecentGapsIdx = 0;
    // Counters to log how often FRUC was skipped vs interpolated
    uint32_t m_FrucSubmitCount = 0;
    uint32_t m_FrucSkipCount = 0;
    uint64_t m_FrucLastStatLogMs = 0;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_FRUCBlitVertexBuffer;
    void blitFRUCTexture(ID3D11ShaderResourceView* srv);
    bool initFRUC(); // Lazy-init: try NvOFFRUC, fall back to GenericFRUC

    // VipleStream: in-process Present timing, independent of ETW/
    // PresentMon. Benchmark target displays may be indirect-display
    // drivers (e.g. a Virtual Display Driver at 4K@120Hz that the
    // user pushes Moonlight onto to bypass their physical 60Hz VBlank
    // ceiling); the DXGI ETW provider doesn't always emit flip events
    // for IDD flips, so PresentMon CSV comes out empty. Measuring
    // Present() return time inside renderFrame() is direct and
    // immune to that whole category of tooling issue.
    //
    // Tracked separately for real vs interpolated frames because
    // the FRUC path calls SwapChain->Present() twice per incoming
    // host frame; the gap between real and interp has different
    // pacing semantics than real-to-real (the latter follows host
    // frame arrival, the former follows the DXGI latency waitable).
    struct PresentBucket {
        LARGE_INTEGER lastPresent {};
        std::vector<double> intervalMs;  // inter-Present intervals (ms) in current stat window
        std::vector<double> callLatMs;   // Present() wall-clock (ms) in current stat window
    };
    PresentBucket m_PresentReal;
    PresentBucket m_PresentInterp;
    LARGE_INTEGER m_PresentQpcFreq {};
    uint32_t      m_PresentRealTotal = 0;    // cumulative, never cleared
    uint32_t      m_PresentInterpTotal = 0;  // cumulative, never cleared
    uint64_t      m_PresentLastLogMs = 0;    // SDL_GetTicks() of last flush
    void recordPresent(PresentBucket& b,
                       LARGE_INTEGER callBegin,
                       LARGE_INTEGER callEnd);

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;
};

