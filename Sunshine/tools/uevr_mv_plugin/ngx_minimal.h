// ============================================================================
// ngx_minimal.h
// NVIDIA DLSS (NGX) D3D12 最小介面宣告（Clean-room 自寫，零第三方二進位依賴）
//
// 對照官方標頭（MIT License，https://github.com/NVIDIA/DLSS）：
//   1. include/nvsdk_ngx_defs.h   — 型別、Feature ID、Result 代碼、參數名稱字串常數
//   2. include/nvsdk_ngx_params.h — NVSDK_NGX_Parameter 虛擬函式表（vtable）結構
//   3. include/nvsdk_ngx.h        — NVSDK_NGX_D3D12_CreateFeature / ReleaseFeature / EvaluateFeature
//
// 重要 ABI 說明（MSVC x64 C++ ABI）：
//   NVSDK_NGX_Parameter 使用同名多載（overloaded）的虛擬函式 Set(...) 與 Get(...)。
//   在 MSVC C++ ABI 中，同名多載的虛擬函式在 vtable 內的排列順序取決於宣告順序
//   （MSVC 會將同一名稱的多載群組依宣告順序反向排列於該群組的 vtable 區段）。
//   因此下方 struct NVSDK_NGX_Parameter 的方法名稱、參數型別、const 修飾詞與宣告順序
//   必須與官方 include/nvsdk_ngx_params.h 完全一致，切勿改名或調換順序。
// ============================================================================

#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_defs.h
// 呼叫慣例與回傳碼定義
// ----------------------------------------------------------------------------
#ifndef NVSDK_CONV
#define NVSDK_CONV __cdecl
#endif

typedef enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_Fail    = 0xBAD00000,
} NVSDK_NGX_Result;

#define NVSDK_NGX_SUCCEED(value) (((value) & 0xFFF00000) != NVSDK_NGX_Result_Fail)
#define NVSDK_NGX_FAILED(value)  (((value) & 0xFFF00000) == NVSDK_NGX_Result_Fail)

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_defs.h
// NVSDK_NGX_Feature 列舉（本外掛僅攔截處理 1=SuperSampling 與 13=RayReconstruction）
// ----------------------------------------------------------------------------
typedef enum NVSDK_NGX_Feature {
    NVSDK_NGX_Feature_Reserved0             = 0,
    NVSDK_NGX_Feature_SuperSampling         = 1,  // DLSS Super Resolution
    NVSDK_NGX_Feature_InPainting            = 2,
    NVSDK_NGX_Feature_ImageSuperResolution  = 3,
    NVSDK_NGX_Feature_SlowMotion            = 4,
    NVSDK_NGX_Feature_VideoSuperResolution  = 5,
    NVSDK_NGX_Feature_Reserved1             = 6,
    NVSDK_NGX_Feature_Reserved2             = 7,
    NVSDK_NGX_Feature_Reserved3             = 8,
    NVSDK_NGX_Feature_ImageSignalProcessing = 9,
    NVSDK_NGX_Feature_DeepResolve           = 10,
    NVSDK_NGX_Feature_FrameGeneration       = 11, // DLSS-G Frame Generation
    NVSDK_NGX_Feature_DeepDVC               = 12,
    NVSDK_NGX_Feature_RayReconstruction     = 13, // DLSS-RR Ray Reconstruction
    NVSDK_NGX_Feature_Count
} NVSDK_NGX_Feature;

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_defs.h
// NVSDK_NGX_DLSS_Feature_Flags（對應 "DLSS.Feature.Create.Flags"）
// ----------------------------------------------------------------------------
typedef enum NVSDK_NGX_DLSS_Feature_Flags {
    NVSDK_NGX_DLSS_Feature_Flags_IsInvalid     = (1 << 31),
    NVSDK_NGX_DLSS_Feature_Flags_None          = 0,
    NVSDK_NGX_DLSS_Feature_Flags_IsLowRes      = (1 << 0),
    NVSDK_NGX_DLSS_Feature_Flags_MVLowRes      = (1 << 1),
    NVSDK_NGX_DLSS_Feature_Flags_MVJittered    = (1 << 2),
    NVSDK_NGX_DLSS_Feature_Flags_DepthInverted = (1 << 3),
    NVSDK_NGX_DLSS_Feature_Flags_Reserved_0    = (1 << 4),
    NVSDK_NGX_DLSS_Feature_Flags_DoSharpening  = (1 << 5),
    NVSDK_NGX_DLSS_Feature_Flags_AutoExposure  = (1 << 6),
    NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling = (1 << 7),
} NVSDK_NGX_DLSS_Feature_Flags;

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_defs.h
// NVSDK_NGX_Handle 與進度回呼型別
// ----------------------------------------------------------------------------
typedef struct NVSDK_NGX_Handle {
    unsigned int Id;
} NVSDK_NGX_Handle;

typedef void (NVSDK_CONV *PFN_NVSDK_NGX_ProgressCallback)(float InCurrentProgress, bool *OutShouldCancel);

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_defs.h
// 公開參數名稱常數
// ----------------------------------------------------------------------------
#define NVSDK_NGX_Parameter_Color                                "Color"
#define NVSDK_NGX_Parameter_Output                               "Output"
#define NVSDK_NGX_Parameter_Depth                                "Depth"
#define NVSDK_NGX_Parameter_MotionVectors                        "MotionVectors"
#define NVSDK_NGX_Parameter_MV_Scale_X                           "MV.Scale.X"
#define NVSDK_NGX_Parameter_MV_Scale_Y                           "MV.Scale.Y"
#define NVSDK_NGX_Parameter_Jitter_Offset_X                      "Jitter.Offset.X"
#define NVSDK_NGX_Parameter_Jitter_Offset_Y                      "Jitter.Offset.Y"
#define NVSDK_NGX_Parameter_Reset                                "Reset"
#define NVSDK_NGX_Parameter_Width                                "Width"
#define NVSDK_NGX_Parameter_Height                               "Height"
#define NVSDK_NGX_Parameter_OutWidth                             "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight                            "OutHeight"
#define NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags            "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width  "DLSS.Render.Subrect.Dimensions.Width"
#define NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height "DLSS.Render.Subrect.Dimensions.Height"

#ifdef __cplusplus
} // extern "C"
#endif

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx_params.h
// NVSDK_NGX_Parameter C++ 虛擬函式表介面
// 宣告順序必須與 NVIDIA 官方 include/nvsdk_ngx_params.h 100% 一致：
//   Set (8 個多載) -> Get (8 個 const 多載) -> Reset (1 個) -> ~NVSDK_NGX_Parameter()
// ----------------------------------------------------------------------------
#ifdef __cplusplus
typedef struct NVSDK_NGX_Parameter {
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;

    virtual NVSDK_NGX_Result Reset(const char *InName) = 0;
    virtual ~NVSDK_NGX_Parameter() {}
} NVSDK_NGX_Parameter;
#endif

// ----------------------------------------------------------------------------
// 對照官方檔名：include/nvsdk_ngx.h
// D3D12 Feature 建立、釋放與執行函式指標型別
// ----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_D3D12_CreateFeature)(
    ID3D12GraphicsCommandList *InCmdList,
    NVSDK_NGX_Feature          InFeatureID,
    NVSDK_NGX_Parameter       *InParameters,
    NVSDK_NGX_Handle         **OutHandle
);

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_D3D12_ReleaseFeature)(
    NVSDK_NGX_Handle *InHandle
);

// 官方 nvsdk_ngx.h 的 NVSDK_NGX_D3D12_EvaluateFeature_C：nvsdk_ngx_helpers.h 的 NGX_D3D12_EVALUATE_DLSS_EXT 走這條。
// 參數與非 _C 版相同（C 版回呼 PFN_NVSDK_NGX_ProgressCallback_C 的簽名也一樣：float, bool*）。
typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_D3D12_EvaluateFeature_C)(
    ID3D12GraphicsCommandList *InCmdList,
    const NVSDK_NGX_Handle *InFeatureHandle,
    const NVSDK_NGX_Parameter *InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback);

typedef NVSDK_NGX_Result (NVSDK_CONV *PFN_NVSDK_NGX_D3D12_EvaluateFeature)(
    ID3D12GraphicsCommandList     *InCmdList,
    const NVSDK_NGX_Handle        *InFeatureHandle,
    const NVSDK_NGX_Parameter     *InParameters,
    PFN_NVSDK_NGX_ProgressCallback InCallback
);

#ifdef __cplusplus
}
#endif
