/*
* Copyright 2022 NVIDIA Corporation.  All rights reserved.
*
* Please refer to the NVIDIA end user license agreement (EULA) associated
* with this source code for terms and conditions that govern your use of
* this software. Any use, reproduction, disclosure, or distribution of
* this software and related documentation outside the terms of the EULA
* is strictly prohibited.
*
*/

#pragma once
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
using namespace std;

#ifdef __linux__ 
    #define OS_LINUX 1
#endif

#ifdef OS_LINUX
    #include <inttypes.h>
#else
    #include <Windows.h>
#endif

#define VALID_LOADLIBRARYEX_FLAGS ~(LOAD_LIBRARY_SEARCH_APPLICATION_DIR |\
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |\
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |\
                                    LOAD_LIBRARY_SEARCH_USER_DIRS |\
                                    LOAD_WITH_ALTERED_SEARCH_PATH)

#define CreateProcName "NvOFFRUCCreate"
#define RegisterResourceProcName "NvOFFRUCRegisterResource"
#define UnregisterResourceProcName "NvOFFRUCUnregisterResource"
#define ProcessProcName "NvOFFRUCProcess"
#define DestroyProcName "NvOFFRUCDestroy"

#if defined(__cplusplus)
extern "C" {
#endif /* __cplusplus */

#ifdef OS_LINUX
    typedef int64_t __int64;
    #define _countof(a) std::extent< decltype(a) >::value
        #ifndef CALLBACK
            #define CALLBACK
        #endif
    #define RtlEqualMemory(Destination,Source,Length) (!memcmp((Destination),(Source),(Length)))
    #define RtlMoveMemory(Destination,Source,Length) memmove((Destination),(Source),(Length))
    #define RtlCopyMemory(Destination,Source,Length) memcpy((Destination),(Source),(Length))
    #define RtlFillMemory(Destination,Length,Fill) memset((Destination),(Fill),(Length))
    #define RtlZeroMemory(Destination,Length) memset((Destination),0,(Length))
    #define MoveMemory RtlMoveMemory
    #define CopyMemory RtlCopyMemory
    #define FillMemory RtlFillMemory
    #define ZeroMemory RtlZeroMemory

    #define far
    #define near
    #define CONST               const
    #define _TRUNCATE           ((size_t)-1)

    typedef int                 BOOL;
    typedef unsigned char       BYTE;
    typedef unsigned short      WORD;
    typedef float               FLOAT;
    typedef FLOAT               *PFLOAT;
    typedef BOOL near           *PBOOL;
    typedef BOOL far            *LPBOOL;
    typedef BYTE near           *PBYTE;
    typedef BYTE far            *LPBYTE;
    typedef int near            *PINT;
    typedef int far             *LPINT;
    typedef WORD near           *PWORD;
    typedef WORD far            *LPWORD;
    typedef long far            *LPLONG;
    typedef void far            *LPVOID;
    typedef CONST void far      *LPCVOID;

    typedef int                 INT;
    typedef unsigned int        UINT;
    typedef unsigned int        *PUINT;
    typedef int64_t             LONGLONG;
    typedef BYTE                BOOLEAN;
    typedef BOOLEAN             *PBOOLEAN;

    #ifndef _HRESULT_DEFINED
        #define _HRESULT_DEFINED
        typedef long                HRESULT;
        #define FAILED(hr) (((HRESULT)(hr)) < 0)
    #endif

    #define S_OK                                   ((HRESULT)0L)
    #define S_FALSE                                ((HRESULT)1L)

    #define _HRESULT_TYPEDEF_(_sc) ((HRESULT)_sc)
    #define E_NOTIMPL                        _HRESULT_TYPEDEF_(0x80000001L)
    #define E_OUTOFMEMORY                    _HRESULT_TYPEDEF_(0x80000002L)
    #define E_INVALIDARG                     _HRESULT_TYPEDEF_(0x80000003L)
    #define E_NOINTERFACE                    _HRESULT_TYPEDEF_(0x80000004L)
    #define E_POINTER                        _HRESULT_TYPEDEF_(0x80000005L)
    #define E_HANDLE                         _HRESULT_TYPEDEF_(0x80000006L)
    #define E_ABORT                          _HRESULT_TYPEDEF_(0x80000007L)
    #define E_FAIL                           _HRESULT_TYPEDEF_(0x80000008L)
    #define E_ACCESSDENIED                   _HRESULT_TYPEDEF_(0x80000009L)


#endif

/**
* Maximum number of resource NvOFFRUC can be registered with
*/
#define NvOFFRUC_MAX_RESOURCE 10

enum NvOFFRUCCUDAResourceType
{
    CudaResourceTypeUndefined = -1,
    CudaResourceCuDevicePtr,
    CudaResourceCuArray
};

/**
 * @brief Type of resource created and shared by client
 */
enum NvOFFRUCResourceType
{
    UndefinedResourceType = -1,
    CudaResource = 0,
    DirectX11Resource = 1,
};

/**
 * @brief format of resource created and shared by client
 */
enum NvOFFRUCSurfaceFormat
{
    UndefinedSurfaceType = -1,
    NV12Surface = 0,
    ARGBSurface = 1,
};

enum NvOFFRUCInputFileType
{
    InputFileTypeUndefined = -1,
    InputFileTypeYUV = 0,
    InputFileTypePNG = 1,
};

typedef union
{
    struct
    {
        uint64_t    uiFenceValueToWaitOn;
    }FenceWaitValue;
    struct
    {
        uint64_t    uiKeyForRenderTextureAcquire;
        uint64_t    uiKeyForInterpTextureAcquire;
    }MutexAcquireKey;
}SyncWait;

typedef union
{
    struct
    {
        uint64_t    uiFenceValueToSignalOn;
    }FenceSignalValue;
    struct
    {
        uint64_t    uiKeyForRenderTextureRelease;
        uint64_t    uiKeyForInterpolateRelease;
    }MutexReleaseKey;
}SyncSignal;


/**
* Minimun number of resource NvOFFRUC should get regsitered with before NvOFFRUCProcess call occurs
*/
#define NvOFFRUC_MIN_RESOURCE 3

    typedef struct _NvOFFRUC* NvOFFRUCHandle;

    /**
    *  Supported error codes
    */
    typedef enum _NvOFFRUC_STATUS
    {
        NvOFFRUC_SUCCESS = 0,
        NvOFFRUC_ERR_NvOFFRUC_NOT_SUPPORTED,
        NvOFFRUC_ERR_INVALID_PTR,
        NvOFFRUC_ERR_INVALID_PARAM,
        NvOFFRUC_ERR_INVALID_HANDLE,
        NvOFFRUC_ERR_OUT_OF_SYSTEM_MEMORY,
        NvOFFRUC_ERR_OUT_OF_VIDEO_MEMORY,
        NvOFFRUC_ERR_OPENCV_NOT_AVAILABLE,
        NvOFFRUC_ERR_UNIMPLEMENTED,
        NvOFFRUC_ERR_OF_FAILURE,
        NvOFFRUC_ERR_DUPLICATE_RESOURCE,
        NvOFFRUC_ERR_UNREGISTERED_RESOURCE,
        NvOFFRUC_ERR_INCORRECT_API_SEQUENCE,
        NvOFFRUC_ERR_WRITE_TODISK_FAILED,
        NvOFFRUC_ERR_PIPELINE_EXECUTION_FAILURE,
        NvOFFRUC_ERR_SYNC_WRITE_FAILED,
        NvOFFRUC_ERR_GENERIC,
        NvOFFRUC_ERR_MAX_ERROR
    }NvOFFRUC_STATUS;

    /**
    *  Supported error strings
    */

    static const char *NvOFFRUCErrorString[]
    {
        "This indicates that API call returned with no errors."
        "This indicates that HW Optical flow functionality is not supported",
        "This indicates that one or more of the pointers passed to the API call is invalid.",
        "This indicates that one or more of the parameter passed to the API call is invalid.",
        "This indicates that an API call was with an invalid handle.",
        "This indicates that the API call failed because it was unable to allocate enough system memory to perform the requested operation.",
        "This indicates that the API call failed because it was unable to allocate enough video memory to perform the requested operation.",
        "This indicates that the API call failed because openCV is not available on system.",
        "This indicates API failed due to an unimplemented feature",
        "This indicated NvOFFRUC unable to generate optical flow",
        "This indicates the resouce passed for NvOFFRUCRegisterResource is already registered",
        "This indicates the resource passed to NvOFFRUCProcess is not registered with NvOFFRUC, Register the resource using NvOFFRUCRegisterResource() before calling NvOFFRUCProcess",
        "This indicates that the API sequence is incorrect i.e, the order of calls is incorrect",
        "This indicates that an interpolated frame could not be written to disk",
        "This indicates that one of the FRUC pipeline stages returned error",
        "This indicates that write synchronization failed"
        "This indicates that an unknown internal error has occurred"
    };

    /**
    * \struct NvOFFRUC_CREATE_PARAM
    * NvOFFRUC creation parameters.
    */
    typedef struct _NvOFFRUC_CREATE_PARAM
    {
        uint32_t                    uiWidth;            /**< [in]: Specifies input/output video surface width. */
        uint32_t                    uiHeight;           /**< [in]: Specifies input/output video surface height. */
        void*                       pDevice;            /**< [in]: Specifies d3d device created by client. */
        NvOFFRUCResourceType        eResourceType;      /**< [in]: Specifies whether resource created by client is DX or CUDA. */
        NvOFFRUCSurfaceFormat       eSurfaceFormat;     /**< [in]: Specifies surface format i.e. NV12 or ARGB. */
        NvOFFRUCCUDAResourceType    eCUDAResourceType;  /**< [in]: Specifies whether CUDA resource is cuDevicePtr or cuArray. */
        uint32_t                    uiReserved[32];
    }NvOFFRUC_CREATE_PARAM;

    /**
    * \struct NvOFFRUC_FrameData
    * Data about input/output framers to FRUC APIs
    */
    typedef struct _NvOFFRUC_FRAMEDATA
    {
        void*                   pFrame;             /**< Frame as D3D Resource ID3D11Texture2D* etc:.*/
        double                  nTimeStamp;         /**< Frame time stamp. */
        size_t                  nCuSurfacePitch;    /**< Pitch for CUDA Pitch Allocations*/
        bool*                   bHasFrameRepetitionOccurred;  /**< [out]: flag to indicate whether frame repeatition has occured */
        uint32_t                uiReserved[32];
    }NvOFFRUC_FRAMEDATA;

    /**
    * \struct NvOFFRUC_PROCESS_IN_PARAMS
    * Parameters for interpolating.extrapolating frame
    * See NvOFFRUCProcess
    */
    typedef struct _NvOFFRUC_PROCESS_IN_PARAMS
    {
        NvOFFRUC_FRAMEDATA          stFrameDataInput;        /**< [in]: Input frame data. */
        uint32_t                    bSkipWarp : 1;           /**< [in]: API will skip warping and updates only state data*/
        SyncWait                    uSyncWait;
        uint32_t                    uiReserved[32];
    }NvOFFRUC_PROCESS_IN_PARAMS;

    /**
    * \struct NvOFFRUC_PROCESS_OUT_PARAMS
    * Parameters for interpolating.extrapolating frame
    * See NvOFFRUCProcess
    */
    typedef struct _NvOFFRUC_PROCESS_OUT_PARAMS
    {
        NvOFFRUC_FRAMEDATA          stFrameDataOutput;          /**< [out]: Output frame data. Resource should be managed by caller */
        SyncSignal                  uSyncSignal;
        uint32_t                    uiReserved[32];
    }NvOFFRUC_PROCESS_OUT_PARAMS;

    /**
    * \struct NvOFFRUC_REGISTER_RESOURCE_PARAM
    * Parameters for registering a resource with NvOFFRUC
    * See NvOFFRUCRegisterResource
    */
    typedef struct _NvOFFRUC_REGISTER_RESOURCE_PARAM
    {
        void*                 pArrResource[NvOFFRUC_MAX_RESOURCE];      /**<[in]: Array of resources created with same device NvOFFRUC_CREATE_PARAM::pDevice*/
        void*                 pD3D11FenceObj;
        uint32_t              uiCount;                                  /**<[out]: count of resources present in pAddDirectXResource*/
    }NvOFFRUC_REGISTER_RESOURCE_PARAM;

    /**
    * \struc NvOFFRUC_UNREGISTER_RESOURCE_PARAM
    * Parameters for unregistering a reource with NvOFFRUC
    * See NvOFFRUCUnregisterResource
    */
    typedef struct _NvOFFRUC_UNREGISTER_RESOURCE_PARAM
    {
        void*                 pArrResource[NvOFFRUC_MAX_RESOURCE];      /**<[in]: Array of resources created with same device NvOFFRUC_CREATE_PARAM::pDevice*/
        uint32_t              uiCount;                                  /**<[in]: Number of resources in pArrDirectXResource */
    }NvOFFRUC_UNREGISTER_RESOURCE_PARAM;

    /**
    * \brief Create an instance of NvOFFRUCHandle object.
    *
    * This function creates an instance of NvOFFRUCHandle object and returns status.
    * Client is expected to release NvOFFRUCHandle resource using NvOFFRUCDestroy function call.
    *
    * \param [in] createParams
    *   Pointer of ::NvOFFRUC_CREATE_PARAMS structure
    * \param [out] hFRUC
    *   Pointer of ::NvOFFRUCHandle object.
    *
    * \return
    * ::NvOFFRUC_SUCCESS \n
    * ::NvOFFRUC_ERR_NvOFFRUC_NOT_SUPPORTED \n
    * ::NvOFFRUC_ERR_INVALID_PTR \n
    * ::NvOFFRUC_ERR_INVALID_PARAM \n
    * ::NvOFFRUC_ERR_OUT_OF_SYSTEM_MEMORY \n
    * ::NvOFFRUC_ERR_OUT_OF_VIDEO_MEMORY \n
    * ::NvOFFRUC_ERR_OPENCV_NOT_AVAILABLE \n
    * ::NvOFFRUC_ERR_GENERIC \n
    */
    typedef NvOFFRUC_STATUS(CALLBACK* PtrToFuncNvOFFRUCCreate)(const NvOFFRUC_CREATE_PARAM*, NvOFFRUCHandle*);

    /**
    * \brief Register resource for sharing with FRUC. Any input frame that is passed to NvOFFRUC via NvOFFRUCProcess
    should be registered with NvOFFRUCRegisterResource
    * \return
    * ::NvOFFRUC_ERR_INVALID_PARAM : Number of resource to register is above valid range,
    */
    typedef NvOFFRUC_STATUS(CALLBACK* PtrToFuncNvOFFRUCRegisterResource)(NvOFFRUCHandle, const NvOFFRUC_REGISTER_RESOURCE_PARAM*);


    /**
    * \brief Unregister resource registered with FRUC. 
    * \return
    * ::NvOFFRUC_ERR_UNREGISTERED_RESOURCE : resource is not recognized by NvOFFRUC\n
    */
    typedef NvOFFRUC_STATUS(CALLBACK* PtrToFuncNvOFFRUCUnregisterResource)(NvOFFRUCHandle, const NvOFFRUC_UNREGISTER_RESOURCE_PARAM*);

    /**
    * \brief Process frames in input buffer to interpolate/extrapolate output frame.
    * Must be called for every frame rendered by caller. Use NvOFFRUC_PROCESS_PARAMS::bSkipWarping if 
    * frame extrapolation/ interpolation is not required.
    *
    * \param [in] hFRUC
    *   ::NvOFFRUCHandle object.
    * \param [in] inParams
    *   Pointer of ::NvOFFRUC_PROCESS_PARAMS structure
    *
    * \return
    * ::NvOFFRUC_SUCCESS \n
    * ::NvOFFRUC_ERR_INVALID_HANDLE \n
    * ::NvOFFRUC_ERR_INVALID_PTR \n
    * ::NvOFFRUC_ERR_INVALID_PARAM \n
    * ::NvOFFRUC_ERR_OUT_OF_SYSTEM_MEMORY \n
    * ::NvOFFRUC_ERR_OUT_OF_VIDEO_MEMORY \n
    * ::NvOFFRUC_ERR_GENERIC \n
    */
    typedef NvOFFRUC_STATUS(CALLBACK* PtrToFuncNvOFFRUCProcess)(NvOFFRUCHandle, const NvOFFRUC_PROCESS_IN_PARAMS*, const NvOFFRUC_PROCESS_OUT_PARAMS*);

    /**
    * \brief Release NvOFFRUC API  and resoures.
    *
    * Releases resources and waits until all resources are gracefully released.
    *
    *  \param [in] hFRUC
    *   ::NvOFFRUCHandle object.
    *  \param [in] hFRUC
    *   ::Pointer to NvOFFRUC_DEBUG_PARAMS object.
    *
    * \return
    * ::NvOFFRUC_SUCCESS \n
    * ::NvOFFRUC_ERR_INVALID_HANDLE \n
    * ::NvOFFRUC_ERR_GENERIC \n
    */
    typedef NvOFFRUC_STATUS(CALLBACK* PtrToFuncNvOFFRUCDestroy)(NvOFFRUCHandle);

#if defined(__cplusplus)
} // extern "C"
#endif/* __cplusplus */