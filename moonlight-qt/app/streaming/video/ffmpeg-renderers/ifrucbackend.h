// VipleStream: abstract FRUC backend interface.
//
// Shared surface implemented by every frame-interpolation backend
// (GenericFRUC, DirectMLFRUC, ...), so d3d11va.cpp can hold a single
// pointer regardless of which backend is active. The interface
// mirrors GenericFRUC's original public API 1:1 — changing the
// backend slot to this interface is source-compatible for every
// existing call site in the renderer.

#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <cstdint>

class IFRUCBackend {
public:
    virtual ~IFRUCBackend() = default;

    virtual bool initialize(ID3D11Device* device, uint32_t width, uint32_t height) = 0;
    virtual void destroy() = 0;

    virtual bool submitFrame(ID3D11DeviceContext* ctx, double timestamp) = 0;
    virtual void skipFrame(ID3D11DeviceContext* ctx) = 0;

    virtual ID3D11RenderTargetView*    getRenderRTV()      const = 0;
    virtual ID3D11ShaderResourceView*  getLastRenderSRV()  const = 0;
    virtual ID3D11ShaderResourceView*  getOutputSRV()      const = 0;
    virtual ID3D11Texture2D*           getOutputTexture()  const = 0;

    virtual bool isInitialized() const = 0;
    virtual void setQualityLevel(int level) = 0;

    // Rolling-average GPU time for each dispatch (ms). Zero when the
    // backend does not have a corresponding stage; d3d11va's
    // [VIPLE-FRUC-Stats] log line degrades gracefully.
    virtual double getLastMeTimeMs()     const = 0;
    virtual double getLastMedianTimeMs() const = 0;
    virtual double getLastWarpTimeMs()   const = 0;

    // Display name for the [VIPLE-FRUC] init log.
    virtual const char* backendName() const = 0;
};

#endif // _WIN32
