// VipleStream: Generic FRUC — cross-platform frame interpolation via D3D11 compute shaders.
// Uses SAD block matching at 1/4 resolution + full-res bidirectional warping.
// No vendor-specific SDK dependency — works on any D3D11 Feature Level 11.0+ GPU.

#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "ifrucbackend.h"

class GenericFRUC : public IFRUCBackend {
public:
    GenericFRUC();
    ~GenericFRUC() override;

    // Initialize with the D3D11 device used for rendering.
    // width/height = display dimensions (full resolution).
    bool initialize(ID3D11Device* device, uint32_t width, uint32_t height) override;

    void destroy() override;

    // Get the RTV for the render texture. Caller renders video into this,
    // then calls submitFrame(). Same pattern as NvOFFRUC.
    // §PERF-1113：兩張全解析度貼圖輪替，這裡回傳「下一張要畫進去」的那張。
    ID3D11RenderTargetView* getRenderRTV() const override { return m_FrameRTV[m_RenderIdx].Get(); }

    // Get the SRV for the last rendered frame (for blitting real frame to swapchain).
    // §PERF-1113：submitFrame／skipFrame 之後仍指向「這一幀」那張（m_LastIdx），不受輪替影響。
    ID3D11ShaderResourceView* getLastRenderSRV() const override { return m_FrameSRV[m_LastIdx].Get(); }

    // Submit the already-rendered frame for interpolation.
    // Returns true if an interpolated frame is available.
    bool submitFrame(ID3D11DeviceContext* ctx, double timestamp) override;

    // VipleStream: Skip ME/warp but update internal state (for late frames).
    // Keeps prev frame in sync without the cost of full interpolation.
    void skipFrame(ID3D11DeviceContext* ctx) override;

    // Get the interpolated frame (valid only when submitFrame returns true).
    ID3D11Texture2D* getOutputTexture() const override { return m_InterpTexture.Get(); }
    ID3D11ShaderResourceView* getOutputSRV() const override { return m_InterpSRV.Get(); }

    bool isInitialized() const override { return m_MotionEstCS[m_QualityLevel] != nullptr; }
    void setQualityLevel(int level) override { m_QualityLevel = (level >= 0 && level <= 2) ? level : 1; }

    // VipleStream: rolling-average GPU time for each dispatch (ms).
    // Zero until enough frames have retired that timestamp queries are
    // ready. Updated inside submitFrame(); read by the renderer for
    // the periodic [VIPLE-FRUC-Stats] line so we can see per-stage
    // budget without an external profiler.
    double getLastMeTimeMs()     const override { return m_LastMeMs; }
    double getLastMedianTimeMs() const override { return m_LastMedianMs; }
    double getLastWarpTimeMs()   const override { return m_LastWarpMs; }

    const char* backendName() const override { return "Generic Compute"; }

private:
    static const uint32_t BLOCK_SIZE = 8;      // Conceptual block size at 1/4 resolution
    static const uint32_t SEARCH_RADIUS = 12;  // Search radius in pixels
    static const uint32_t DOWNSCALE = 8;       // 64px full-res blocks (was 4→32px). Fewer blocks = faster ME.

    bool loadShaders();
    bool createTextures();

    ID3D11Device* m_Device = nullptr;

    // Compute shaders — 3 quality variants each
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_MotionEstCS[3];  // [0]=quality [1]=balanced [2]=performance
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_WarpBlendCS[3];
    // VipleStream: 3x3 median filter over the MV field. One shader
    // (no quality variants — median is deterministic). Runs between
    // ME and warp; kills single-block MV outliers that otherwise
    // show up as shimmer near motion boundaries.
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_MvMedianCS;
    int m_QualityLevel = 1;  // default: balanced

    // Constant buffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ConstantBuffer;

    // Sampler
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_PointSampler;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_LinearSampler;

    // §PERF-1113：原本是 m_RenderTexture（呼叫端畫進去）＋m_PrevFrame（上一幀參考），每幀結尾整張
    // CopyResource 過去。改成兩張同規格貼圖輪替：這一幀畫的那張，下一幀直接當上一幀參考，省掉複製。
    //   m_RenderIdx＝下一次 getRenderRTV() 給出去的那張（也是 submitFrame 進來時的「這一幀」）
    //   m_LastIdx  ＝最近一次 submitFrame／skipFrame 處理的那張（getLastRenderSRV() 用）
    //   上一幀參考＝1 - m_RenderIdx
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_FrameTex[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_FrameRTV[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_FrameSRV[2];
    int m_RenderIdx = 0;
    int m_LastIdx = 0;

    // 1/4-resolution textures for motion estimation
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_PrevFrameQuarter;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_PrevFrameQuarterSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_CurrFrameQuarter;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_CurrFrameQuarterSRV;

    // Motion vector field (R16G16_SINT, block-level, Q1 fixed-point)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_MotionField;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_MotionFieldUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_MotionFieldSRV;

    // Previous frame's MV field for temporal smoothing
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_PrevMotionField;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_PrevMotionFieldSRV;

    // VipleStream: post-median-filtered MV field, same format as the
    // raw m_MotionField. warp reads from the filtered SRV.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_MotionFieldFiltered;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_MotionFieldFilteredUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_MotionFieldFilteredSRV;

    // MV readback for diagnostics
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_MotionFieldStaging;

    // Interpolated output frame
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_InterpTexture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_InterpUAV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_InterpSRV;

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_QuarterWidth = 0;
    uint32_t m_QuarterHeight = 0;
    int m_FrameCount = 0;
    bool m_MvDiagPending = false;  // §REVIEW-1103：MV 診斷讀回已排 copy、等 GPU 做完再讀（不阻塞）
    int m_MvDiagFrame = 0;         // §REVIEW-1103：排 copy 的那一幀編號（log 用）

    // VipleStream: GPU-side timestamp queries for the ME + warp
    // dispatches. Double-buffered so the CPU reads frame N-2's
    // results while frame N is issuing new queries — avoids the
    // stall that GetData(..., 0) would cause on the current frame.
    static const int TS_RING = 3;
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsDisjoint[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsMeBegin[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsMeEnd[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsMedianBegin[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsMedianEnd[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsWarpBegin[TS_RING];
    Microsoft::WRL::ComPtr<ID3D11Query> m_TsWarpEnd[TS_RING];
    int m_TsSlot = 0;
    bool m_TsQueriesValid = false;
    // Exponential moving average so the reported number is stable
    // between log lines (raw per-frame numbers bounce 20%+).
    double m_LastMeMs = 0.0;
    double m_LastMedianMs = 0.0;
    double m_LastWarpMs = 0.0;
    bool createTimestampQueries();
    void readTimestamps(ID3D11DeviceContext* ctx);

    struct alignas(16) CBData {
        uint32_t frameWidth;
        uint32_t frameHeight;
        uint32_t blockSize;
        union {
            uint32_t searchRadius;  // motion est
            float    blendFactor;   // warp
        };
    };
};

#endif // _WIN32
