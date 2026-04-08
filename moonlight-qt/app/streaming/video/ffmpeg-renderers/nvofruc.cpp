// VipleStream: NVIDIA Optical Flow FRUC wrapper implementation

#ifdef _WIN32

#include "nvofruc.h"
#include <NvOFFRUC.h>
#include <SDL_log.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

NvOFRUCWrapper::NvOFRUCWrapper() {}

NvOFRUCWrapper::~NvOFRUCWrapper() {
    destroy();
}

bool NvOFRUCWrapper::loadLibrary() {
    // Try loading from same directory as the executable
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] Exe path: %S", exePath);
    WCHAR* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        WCHAR dllPath[MAX_PATH];
        wcscpy_s(dllPath, MAX_PATH, exePath);
        wcscat_s(dllPath, MAX_PATH, L"NvOFFRUC.dll");
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] Trying: %S", dllPath);
        m_DLL = (void*)LoadLibraryW(dllPath);
    }
    if (!m_DLL) {
        // Try current working directory
        m_DLL = (void*)LoadLibraryW(L"NvOFFRUC.dll");
    }
    if (!m_DLL) {
        // Try next to the DLL itself (for portable installations)
        WCHAR modulePath[MAX_PATH] = {};
        GetModuleFileNameW((HMODULE)GetModuleHandleW(L"Moonlight.exe"), modulePath, MAX_PATH);
        lastSlash = wcsrchr(modulePath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            wcscat_s(modulePath, MAX_PATH, L"NvOFFRUC.dll");
            m_DLL = (void*)LoadLibraryW(modulePath);
        }
    }
    if (!m_DLL) {
        DWORD err = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC] NvOFFRUC.dll not found (GetLastError=%lu)", err);
        return false;
    }

    m_fnCreate = (void*)GetProcAddress((HMODULE)m_DLL, CreateProcName);
    m_fnRegisterResource = (void*)GetProcAddress((HMODULE)m_DLL, RegisterResourceProcName);
    m_fnUnregisterResource = (void*)GetProcAddress((HMODULE)m_DLL, UnregisterResourceProcName);
    m_fnProcess = (void*)GetProcAddress((HMODULE)m_DLL, ProcessProcName);
    m_fnDestroy = (void*)GetProcAddress((HMODULE)m_DLL, DestroyProcName);

    if (!m_fnCreate || !m_fnRegisterResource || !m_fnUnregisterResource ||
        !m_fnProcess || !m_fnDestroy) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC] Failed to load NvOFFRUC functions");
        FreeLibrary((HMODULE)m_DLL);
        m_DLL = nullptr;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] NvOFFRUC.dll loaded successfully");
    return true;
}

bool NvOFRUCWrapper::createTextures() {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_Width;
    desc.Height = m_Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // NvOFFRUC ARGBSurface = RGBA (NOT BGRA!)
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Fence mode (proven working in standalone test): SHARED + SHARED_NTHANDLE.
    // Requires CUDA 11.8+ runtime and D3D11 Fence for synchronization.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    // Create render input textures + RTVs (so d3d11va can render directly into them)
    for (int i = 0; i < NUM_RENDER_TEXTURES; i++) {
        if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, &m_RenderTextures[i]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC] Failed to create render texture %d", i);
            return false;
        }
        if (FAILED(m_Device->CreateRenderTargetView(m_RenderTextures[i], nullptr, &m_RenderRTVs[i]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC] Failed to create render RTV %d", i);
            return false;
        }
    }

    // Create interpolation output textures and their SRVs
    for (int i = 0; i < NUM_INTERP_TEXTURES; i++) {
        if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, &m_OutputTextures[i]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC] Failed to create output texture %d", i);
            return false;
        }
        if (FAILED(m_Device->CreateShaderResourceView(m_OutputTextures[i], nullptr, &m_OutputSRVs[i]))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-FRUC] Failed to create output SRV %d", i);
            return false;
        }
    }

    // KEYEDMUTEX init: Release key=0 on all textures so first AcquireSync(0) succeeds.
    // DXGI creates keyed mutex textures in "acquired, key=0" state — must release first.
    for (int i = 0; i < NUM_RENDER_TEXTURES; i++) {
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(m_RenderTextures[i]->QueryInterface(IID_PPV_ARGS(&km)))) {
            km->ReleaseSync(0);
        }
    }
    for (int i = 0; i < NUM_INTERP_TEXTURES; i++) {
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(m_OutputTextures[i]->QueryInterface(IID_PPV_ARGS(&km)))) {
            km->ReleaseSync(0);
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] Created %d render + %d output textures (%dx%d)",
                NUM_RENDER_TEXTURES, NUM_INTERP_TEXTURES, m_Width, m_Height);
    return true;
}

bool NvOFRUCWrapper::initialize(ID3D11Device* device, uint32_t width, uint32_t height) {
    m_Device = device;
    m_Width = width;
    m_Height = height;

    if (!loadLibrary()) return false;
    if (!createTextures()) return false;

    // Create FRUC instance
    NvOFFRUC_CREATE_PARAM createParams = {};
    createParams.uiWidth = width;
    createParams.uiHeight = height;
    createParams.pDevice = device;
    createParams.eResourceType = DirectX11Resource;
    createParams.eSurfaceFormat = ARGBSurface;

    auto fnCreate = (PtrToFuncNvOFFRUCCreate)m_fnCreate;
    NvOFFRUCHandle handle = nullptr;
    NvOFFRUC_STATUS status = fnCreate(&createParams, &handle);
    if (status != NvOFFRUC_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC] NvOFFRUCCreate failed: %d", (int)status);
        return false;
    }
    m_Handle = handle;

    // Create D3D11 Fence for synchronization (proven working in standalone test)
    {
        ComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(m_Device->QueryInterface(IID_PPV_ARGS(&dev5)))) {
            if (SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence)))) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-FRUC] D3D11 Fence created for CUDA sync");
            }
        }
    }

    // Register all textures with FRUC
    NvOFFRUC_REGISTER_RESOURCE_PARAM regParam = {};
    uint32_t idx = 0;
    for (int i = 0; i < NUM_INTERP_TEXTURES; i++) {
        regParam.pArrResource[idx++] = m_OutputTextures[i];
    }
    for (int i = 0; i < NUM_RENDER_TEXTURES; i++) {
        regParam.pArrResource[idx++] = m_RenderTextures[i];
    }
    regParam.uiCount = idx;
    regParam.pD3D11FenceObj = m_Fence; // D3D11 Fence for D3D11↔CUDA sync

    auto fnRegister = (PtrToFuncNvOFFRUCRegisterResource)m_fnRegisterResource;
    status = fnRegister((NvOFFRUCHandle)m_Handle, &regParam);
    if (status != NvOFFRUC_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-FRUC] NvOFFRUCRegisterResource failed: %d", (int)status);
        auto fnDel = (PtrToFuncNvOFFRUCDestroy)m_fnDestroy;
        fnDel((NvOFFRUCHandle)m_Handle);
        m_Handle = nullptr;
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-FRUC] Initialized: %dx%d, %d textures registered",
                width, height, idx);

    // Self-test removed: confirmed NvOFFRUC Process works (status=0) with CUDA 11.8 +
    // SHARED_KEYEDMUTEX | SHARED_NTHANDLE. Self-test was corrupting keyed mutex state.
    return true;
}

void NvOFRUCWrapper::destroy() {
    if (m_Handle) {
        // Unregister resources
        NvOFFRUC_UNREGISTER_RESOURCE_PARAM unregParam = {};
        uint32_t idx = 0;
        for (int i = 0; i < NUM_INTERP_TEXTURES; i++) {
            if (m_OutputTextures[i]) unregParam.pArrResource[idx++] = m_OutputTextures[i];
        }
        for (int i = 0; i < NUM_RENDER_TEXTURES; i++) {
            if (m_RenderTextures[i]) unregParam.pArrResource[idx++] = m_RenderTextures[i];
        }
        unregParam.uiCount = idx;

        auto fnUnreg = (PtrToFuncNvOFFRUCUnregisterResource)m_fnUnregisterResource;
        fnUnreg((NvOFFRUCHandle)m_Handle, &unregParam);

        auto fnDel = (PtrToFuncNvOFFRUCDestroy)m_fnDestroy;
        fnDel((NvOFFRUCHandle)m_Handle);
        m_Handle = nullptr;
    }

    for (int i = 0; i < NUM_RENDER_TEXTURES; i++) {
        if (m_RenderRTVs[i]) { m_RenderRTVs[i]->Release(); m_RenderRTVs[i] = nullptr; }
        if (m_RenderTextures[i]) { m_RenderTextures[i]->Release(); m_RenderTextures[i] = nullptr; }
    }
    for (int i = 0; i < NUM_INTERP_TEXTURES; i++) {
        if (m_OutputSRVs[i]) { m_OutputSRVs[i]->Release(); m_OutputSRVs[i] = nullptr; }
        if (m_OutputTextures[i]) { m_OutputTextures[i]->Release(); m_OutputTextures[i] = nullptr; }
    }

    if (m_Fence) { m_Fence->Release(); m_Fence = nullptr; }
    if (m_DLL) { FreeLibrary((HMODULE)m_DLL); m_DLL = nullptr; }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-FRUC] Destroyed");
}

bool NvOFRUCWrapper::submitFrame(ID3D11DeviceContext* deviceContext, double timestamp) {
    if (!m_Handle) return false;

    int renderIdx = m_CurrentRenderIndex;
    m_CurrentRenderIndex = (m_CurrentRenderIndex + 1) % NUM_RENDER_TEXTURES;
    int outputIdx = m_CurrentOutputIndex;

    // Signal fence using the SAME context that did the rendering
    ComPtr<ID3D11DeviceContext4> ctx4;
    if (m_Fence && SUCCEEDED(deviceContext->QueryInterface(IID_PPV_ARGS(&ctx4)))) {
        ctx4->Signal(m_Fence, ++m_FenceValue);
    } else {
        deviceContext->Flush();
    }

    NvOFFRUC_PROCESS_IN_PARAMS inParams = {};
    inParams.stFrameDataInput.pFrame = m_RenderTextures[renderIdx];
    inParams.stFrameDataInput.nTimeStamp = timestamp;
    inParams.bSkipWarp = 0;
    if (m_Fence) {
        inParams.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = m_FenceValue;
    }

    bool bRepeat = false;
    NvOFFRUC_PROCESS_OUT_PARAMS outParams = {};
    outParams.stFrameDataOutput.pFrame = m_OutputTextures[outputIdx];
    outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &bRepeat;
    if (m_Fence) {
        outParams.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = ++m_FenceValue;
    }
    if (m_HasPrevTimestamp) {
        outParams.stFrameDataOutput.nTimeStamp = (m_PrevTimestamp + timestamp) / 2.0;
    } else {
        outParams.stFrameDataOutput.nTimeStamp = timestamp;
    }

    auto fnProcess = (PtrToFuncNvOFFRUCProcess)m_fnProcess;
    NvOFFRUC_STATUS status = fnProcess((NvOFFRUCHandle)m_Handle, &inParams, &outParams);

    m_FrameCount++;
    m_PrevTimestamp = timestamp;
    m_HasPrevTimestamp = true;

    if (status != NvOFFRUC_SUCCESS) {
        if (m_FrameCount <= 3) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-FRUC] Warming up (frame %d, status %d)", m_FrameCount, (int)status);
            return false;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] NvOFFRUCProcess failed: %d (frame %d)",
                    (int)status, m_FrameCount);
        return false;
    }

    if (m_FrameCount <= 5 || (m_FrameCount % 300 == 0)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-FRUC] frame=%d status=OK repeat=%d outIdx=%d",
                    m_FrameCount, (int)bRepeat, outputIdx);
    }

    if (bRepeat) return false;
    m_CurrentOutputIndex = outputIdx;
    return true;
}

// ── Keyed mutex helpers for output texture (called from d3d11va.cpp) ──

bool NvOFRUCWrapper::acquireOutputMutex(DWORD timeoutMs) {
    // Wait for CUDA to finish writing the interpolated frame
    if (m_Fence) {
        ComPtr<ID3D11DeviceContext> ctx;
        m_Device->GetImmediateContext(&ctx);
        ComPtr<ID3D11DeviceContext4> ctx4;
        if (SUCCEEDED(ctx.As(&ctx4))) {
            ctx4->Wait(m_Fence, m_FenceValue);
        }
    }
    return true;
}

void NvOFRUCWrapper::releaseOutputMutex() {
    // No-op in fence mode
}

#endif // Q_OS_WIN32
