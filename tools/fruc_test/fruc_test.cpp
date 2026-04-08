// Standalone NvOFFRUC test — mirrors SDK sample exactly
// Build: cl /EHsc /O2 fruc_test.cpp /link d3d11.lib dxgi.lib
#include <stdio.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <NvOFFRUC.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

int main()
{
    printf("=== NvOFFRUC Standalone Test ===\n\n");

    // 1. Create D3D11 device — match Moonlight's separate device mode exactly
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);  // GPU 0 = NVIDIA
    DXGI_ADAPTER_DESC1 adapterDesc;
    adapter->GetDesc1(&adapterDesc);
    printf("[INFO] Adapter 0: %S (VID=%x DID=%x)\n", adapterDesc.Description, adapterDesc.VendorId, adapterDesc.DeviceId);

    D3D_FEATURE_LEVEL supportedLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL featureLevel;
    // Match Moonlight: D3D_DRIVER_TYPE_UNKNOWN + specific adapter + VIDEO_SUPPORT
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                    supportedLevels, ARRAYSIZE(supportedLevels),
                                    D3D11_SDK_VERSION,
                                    &device, &featureLevel, &ctx);
    if (FAILED(hr)) {
        // Fallback without VIDEO_SUPPORT
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               0, supportedLevels, ARRAYSIZE(supportedLevels),
                               D3D11_SDK_VERSION, &device, &featureLevel, &ctx);
    }
    if (FAILED(hr)) { printf("D3D11CreateDevice FAILED: 0x%x\n", hr); return 1; }
    printf("[OK] Render device created (FL %x, DRIVER_TYPE_UNKNOWN + adapter)\n", featureLevel);

    // Create second device (decode device) + shared fence — simulating Moonlight's separate device mode
    ComPtr<ID3D11Device> decodeDevice;
    ComPtr<ID3D11DeviceContext> decodeCtx;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                           supportedLevels, ARRAYSIZE(supportedLevels),
                           D3D11_SDK_VERSION, &decodeDevice, nullptr, &decodeCtx);
    if (SUCCEEDED(hr)) {
        printf("[OK] Decode device created (separate device mode)\n");
        // Create shared fence between devices (like Moonlight's D2R fence)
        ComPtr<ID3D11Device5> decodeDev5;
        if (SUCCEEDED(decodeDevice->QueryInterface(IID_PPV_ARGS(&decodeDev5)))) {
            ComPtr<ID3D11Fence> d2rFence;
            if (SUCCEEDED(decodeDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d2rFence)))) {
                printf("[OK] D2R shared fence created\n");
            }
        }
    }

    // 2. Check fence support
    ComPtr<ID3D11Device5> dev5;
    bool fenceSupported = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev5)));
    printf("[INFO] Fence support: %s\n", fenceSupported ? "YES" : "NO");

    // 3. Load NvOFFRUC.dll
    HMODULE hDll = LoadLibraryW(L"NvOFFRUC.dll");
    if (!hDll) { printf("LoadLibrary(NvOFFRUC.dll) FAILED: %lu\n", GetLastError()); return 1; }
    printf("[OK] NvOFFRUC.dll loaded\n");

    auto fnCreate = (PtrToFuncNvOFFRUCCreate)GetProcAddress(hDll, CreateProcName);
    auto fnRegister = (PtrToFuncNvOFFRUCRegisterResource)GetProcAddress(hDll, RegisterResourceProcName);
    auto fnUnregister = (PtrToFuncNvOFFRUCUnregisterResource)GetProcAddress(hDll, UnregisterResourceProcName);
    auto fnProcess = (PtrToFuncNvOFFRUCProcess)GetProcAddress(hDll, ProcessProcName);
    auto fnDestroy = (PtrToFuncNvOFFRUCDestroy)GetProcAddress(hDll, DestroyProcName);
    if (!fnCreate || !fnRegister || !fnProcess || !fnDestroy) {
        printf("GetProcAddress FAILED\n"); return 1;
    }
    printf("[OK] All function pointers loaded\n");

    // 4. Create FRUC instance
    const uint32_t W = 1920, H = 1080;
    NvOFFRUC_CREATE_PARAM createParams = {};
    createParams.uiWidth = W;
    createParams.uiHeight = H;
    createParams.pDevice = device.Get();
    createParams.eResourceType = DirectX11Resource;
    createParams.eSurfaceFormat = ARGBSurface;

    NvOFFRUCHandle hFRUC = nullptr;
    NvOFFRUC_STATUS status = fnCreate(&createParams, &hFRUC);
    if (status != NvOFFRUC_SUCCESS) { printf("NvOFFRUCCreate FAILED: %d\n", status); return 1; }
    printf("[OK] NvOFFRUC created (%dx%d ARGB)\n", W, H);

    // 5. Create D3D11 Fence (if supported)
    ComPtr<ID3D11Fence> fence;
    uint64_t fenceValue = 0;
    if (fenceSupported) {
        hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        if (SUCCEEDED(hr)) {
            printf("[OK] D3D11 Fence created\n");
        } else {
            printf("[WARN] CreateFence failed: 0x%x, using keyed mutex\n", hr);
            fenceSupported = false;
        }
    }

    // 6. Create textures — match SDK sample exactly
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = W;
    desc.Height = H;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // SDK sample uses this for ARGB
    desc.SampleDesc.Count = 1;

    if (fenceSupported) {
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    } else {
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    }

    const int NUM_RENDER = 2, NUM_INTERP = 1;
    ID3D11Texture2D* renderTex[NUM_RENDER] = {};
    ID3D11Texture2D* interpTex[NUM_INTERP] = {};

    for (int i = 0; i < NUM_RENDER; i++) {
        hr = device->CreateTexture2D(&desc, nullptr, &renderTex[i]);
        if (FAILED(hr)) { printf("CreateTexture2D(render %d) FAILED: 0x%x\n", i, hr); return 1; }
    }
    for (int i = 0; i < NUM_INTERP; i++) {
        hr = device->CreateTexture2D(&desc, nullptr, &interpTex[i]);
        if (FAILED(hr)) { printf("CreateTexture2D(interp %d) FAILED: 0x%x\n", i, hr); return 1; }
    }
    printf("[OK] Created %d render + %d interp textures (%s)\n",
           NUM_RENDER, NUM_INTERP, fenceSupported ? "SHARED|NTHANDLE" : "KEYEDMUTEX|NTHANDLE");

    // 7. Register resources
    NvOFFRUC_REGISTER_RESOURCE_PARAM regParam = {};
    uint32_t idx = 0;
    for (int i = 0; i < NUM_INTERP; i++) regParam.pArrResource[idx++] = interpTex[i];
    for (int i = 0; i < NUM_RENDER; i++) regParam.pArrResource[idx++] = renderTex[i];
    regParam.uiCount = idx;
    regParam.pD3D11FenceObj = fenceSupported ? fence.Get() : nullptr;

    status = fnRegister(hFRUC, &regParam);
    if (status != NvOFFRUC_SUCCESS) { printf("NvOFFRUCRegisterResource FAILED: %d\n", status); return 1; }
    printf("[OK] Registered %d resources (fence=%s)\n", idx, fenceSupported ? "yes" : "null/keyedmutex");

    // 8. Create staging texture for filling render textures
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;
    ComPtr<ID3D11Texture2D> stagingTex;
    hr = device->CreateTexture2D(&desc, nullptr, &stagingTex);
    if (FAILED(hr)) { printf("CreateTexture2D(staging) FAILED: 0x%x\n", hr); return 1; }

    // 9. Process frames
    printf("\n=== Processing frames ===\n");
    bool bRepeat = false;
    int renderIdx = 0;

    ComPtr<ID3D11DeviceContext4> ctx4;
    if (fenceSupported) ctx->QueryInterface(IID_PPV_ARGS(&ctx4));

    for (int frame = 0; frame < 10; frame++) {
        int rIdx = renderIdx;
        renderIdx = (renderIdx + 1) % NUM_RENDER;

        // Fill staging with solid color, copy to render texture
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = ctx->Map(stagingTex.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
        if (SUCCEEDED(hr)) {
            uint32_t color = (frame & 1) ? 0xFF00FF00 : 0xFFFF0000;  // green / red
            uint32_t* pixels = (uint32_t*)mapped.pData;
            for (uint32_t y = 0; y < H; y++) {
                uint32_t* row = (uint32_t*)((uint8_t*)mapped.pData + y * mapped.RowPitch);
                for (uint32_t x = 0; x < W; x++) row[x] = color;
            }
            ctx->Unmap(stagingTex.Get(), 0);
        }
        ctx->CopyResource(renderTex[rIdx], stagingTex.Get());

        // Signal fence / flush
        if (fenceSupported && ctx4) {
            ctx4->Signal(fence.Get(), ++fenceValue);
        } else {
            ctx->Flush();
        }

        // Setup Process params
        double inputTs = frame * 16.667;
        double prevTs = (frame > 0) ? (frame - 1) * 16.667 : 0;
        double midTs = (frame > 0) ? (prevTs + inputTs) / 2.0 : inputTs;

        NvOFFRUC_PROCESS_IN_PARAMS inParams = {};
        inParams.stFrameDataInput.pFrame = renderTex[rIdx];
        inParams.stFrameDataInput.nTimeStamp = inputTs;
        inParams.bSkipWarp = 0;

        if (fenceSupported) {
            inParams.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = fenceValue;
        }

        NvOFFRUC_PROCESS_OUT_PARAMS outParams = {};
        outParams.stFrameDataOutput.pFrame = interpTex[0];
        outParams.stFrameDataOutput.nTimeStamp = midTs;
        outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &bRepeat;

        if (fenceSupported) {
            outParams.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = ++fenceValue;
        }

        status = fnProcess(hFRUC, &inParams, &outParams);

        printf("  frame %d: status=%d repeat=%d ts=%.1f midTs=%.1f\n",
               frame, (int)status, (int)bRepeat, inputTs, midTs);

        if (status == NvOFFRUC_SUCCESS && fenceSupported && ctx4) {
            // Wait for CUDA to finish before next frame
            ctx4->Wait(fence.Get(), fenceValue);
        }
    }

    // 10. Cleanup
    NvOFFRUC_UNREGISTER_RESOURCE_PARAM unregParam = {};
    memcpy(unregParam.pArrResource, regParam.pArrResource, regParam.uiCount * sizeof(void*));
    unregParam.uiCount = regParam.uiCount;
    fnUnregister(hFRUC, &unregParam);
    fnDestroy(hFRUC);

    for (int i = 0; i < NUM_RENDER; i++) renderTex[i]->Release();
    for (int i = 0; i < NUM_INTERP; i++) interpTex[i]->Release();
    FreeLibrary(hDll);

    printf("\n=== Test complete ===\n");
    return 0;
}
