// VipleStream: NVIDIA Optical Flow Frame Rate Up Conversion (FRUC) wrapper
// Provides 2x frame interpolation using NVIDIA's hardware optical flow engine.
// Requires RTX 2000+ (Turing) GPU and NvOFFRUC.dll.

#pragma once

#ifdef _WIN32

#include <d3d11_4.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>

class NvOFRUCWrapper {
public:
    NvOFRUCWrapper();
    ~NvOFRUCWrapper();

    // Initialize FRUC with the D3D11 device used for rendering.
    // width/height = video frame dimensions (not display dimensions).
    bool initialize(ID3D11Device* device, uint32_t width, uint32_t height);

    // Destroy FRUC instance and free resources.
    void destroy();

    // Get the interpolated frame (valid only when submitFrame returns true).
    ID3D11Texture2D* getOutputTexture() const { return m_OutputTextures[m_CurrentOutputIndex]; }

    bool isInitialized() const { return m_Handle != nullptr; }

    // Get the SRV for the current interpolated output (valid when processFrame returns true).
    ID3D11ShaderResourceView* getOutputSRV() const { return m_OutputSRVs[m_CurrentOutputIndex]; }

    // Get the RTV for the current render texture (caller renders directly into it).
    ID3D11RenderTargetView* getCurrentRenderRTV() const { return m_RenderRTVs[m_CurrentRenderIndex]; }

    // Get the SRV for the last submitted render texture (valid after submitFrame).
    // Use this to blit the real frame without re-rendering.
    ID3D11ShaderResourceView* getLastRenderSRV() const { return m_RenderSRVs[m_LastRenderIndex]; }

    // Submit the current render texture (already rendered into via RTV) for interpolation.
    // Call getCurrentRenderRTV(), render into it, then call this.
    // deviceContext: the SAME context used for rendering (for fence Signal).
    bool submitFrame(ID3D11DeviceContext* deviceContext, double timestamp);

    // Keyed mutex for FRUC output texture — caller must bracket blit with these.
    bool acquireOutputMutex(DWORD timeoutMs = 1000);
    void releaseOutputMutex();

private:
    static const int NUM_RENDER_TEXTURES = 2;
    static const int NUM_INTERP_TEXTURES = 2;

    // FRUC handle
    void* m_Handle = nullptr;

    // DLL handle and function pointers (loaded dynamically)
    void* m_DLL = nullptr;
    void* m_fnCreate = nullptr;
    void* m_fnRegisterResource = nullptr;
    void* m_fnUnregisterResource = nullptr;
    void* m_fnProcess = nullptr;
    void* m_fnDestroy = nullptr;

    // D3D11 resources
    ID3D11Device* m_Device = nullptr;
    ID3D11Texture2D* m_RenderTextures[NUM_RENDER_TEXTURES] = {};
    ID3D11RenderTargetView* m_RenderRTVs[NUM_RENDER_TEXTURES] = {};
    ID3D11ShaderResourceView* m_RenderSRVs[NUM_RENDER_TEXTURES] = {};
    ID3D11Texture2D* m_OutputTextures[NUM_INTERP_TEXTURES] = {};
    ID3D11ShaderResourceView* m_OutputSRVs[NUM_INTERP_TEXTURES] = {};

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    int m_CurrentRenderIndex = 0;
    int m_LastRenderIndex = 0;
    int m_CurrentOutputIndex = 0;
    int m_FrameCount = 0;
    // VipleStream v1.2.56: running count of NvOFFRUC bRepeat=true
    // responses — diagnostic for "bRepeat on every frame" behaviour.
    int m_RepeatCount = 0;
    double m_PrevTimestamp = 0.0;
    bool m_HasPrevTimestamp = false;

    // D3D11 Fence for D3D11↔CUDA synchronization
    ID3D11Fence* m_Fence = nullptr;
    uint64_t m_FenceValue = 0;

    bool loadLibrary();
    bool createTextures();
};

#endif // Q_OS_WIN32
