// probe_d3d.cpp - 見 probe_d3d.h。
#include "probe_d3d.h"

#include "vr_ipc_abi.h"

namespace probe::d3d {

  HRESULT create_device(const LUID *luid, device_t &out) {
    out = device_t {};
    ComPtr<IDXGIFactory1> f1;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f1));
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (luid) {
      ComPtr<IDXGIFactory4> f4;
      hr = f1.As(&f4);
      if (FAILED(hr)) {
        return hr;
      }
      hr = f4->EnumAdapterByLuid(*luid, IID_PPV_ARGS(&adapter));
      if (FAILED(hr)) {
        return hr;
      }
    } else {
      for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (f1->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(a->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
          adapter = a;
          break;
        }
      }
      if (!adapter) {
        return DXGI_ERROR_NOT_FOUND;
      }
    }
    DXGI_ADAPTER_DESC1 desc {};
    adapter->GetDesc1(&desc);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    D3D_FEATURE_LEVEL got {};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &out.dev, &got, &out.ctx);
    if (FAILED(hr)) {
      return hr;
    }
    hr = out.dev.As(&out.dev1);
    if (SUCCEEDED(hr)) {
      hr = out.dev.As(&out.dev5);
    }
    if (SUCCEEDED(hr)) {
      hr = out.ctx.As(&out.ctx1);
    }
    if (SUCCEEDED(hr)) {
      hr = out.ctx.As(&out.ctx4);
    }
    if (FAILED(hr)) {
      out = device_t {};
      return hr;
    }
    out.luid = desc.AdapterLuid;
    return S_OK;
  }

  server_ring_t::~server_ring_t() {
    for (HANDLE &h : tex_nt) {
      if (h) {
        CloseHandle(h);
        h = nullptr;
      }
    }
    if (shared_fence_nt) {
      CloseHandle(shared_fence_nt);
    }
    if (consumed_fence_nt) {
      CloseHandle(consumed_fence_nt);
    }
  }

  HRESULT create_server_ring(device_t &d, uint32_t width, uint32_t height, server_ring_t &out) {
    D3D11_TEXTURE2D_DESC td {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (int i = 0; i < 3; ++i) {
      HRESULT hr = d.dev->CreateTexture2D(&td, nullptr, &out.tex[i]);
      if (FAILED(hr)) {
        return hr;
      }
      ComPtr<IDXGIResource1> r1;
      hr = out.tex[i].As(&r1);
      if (FAILED(hr)) {
        return hr;
      }
      hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &out.tex_nt[i]);
      if (FAILED(hr)) {
        return hr;
      }
    }
    HRESULT hr = d.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out.shared_fence));
    if (SUCCEEDED(hr)) {
      hr = out.shared_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &out.shared_fence_nt);
    }
    if (SUCCEEDED(hr)) {
      hr = d.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out.consumed_fence));
    }
    if (SUCCEEDED(hr)) {
      hr = out.consumed_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &out.consumed_fence_nt);
    }
    return hr;
  }

  HRESULT open_ring(device_t &d, const HANDLE tex[3], HANDLE shared_fence, HANDLE consumed_fence, uint32_t width, uint32_t height, opened_ring_t &out, const char *&why) {
    out = opened_ring_t {};
    for (int i = 0; i < 3; ++i) {
      HRESULT hr = d.dev1->OpenSharedResource1(tex[i], IID_PPV_ARGS(&out.tex[i]));
      if (FAILED(hr)) {
        why = "open-texture";
        return hr;
      }
      D3D11_TEXTURE2D_DESC desc {};
      out.tex[i]->GetDesc(&desc);
      if (desc.Width != width || desc.Height != height || desc.Format != (DXGI_FORMAT) VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM || desc.MipLevels != 1 ||
          desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
        why = "texture-desc";
        return E_INVALIDARG;
      }
      hr = d.dev->CreateRenderTargetView(out.tex[i].Get(), nullptr, &out.rtv[i]);
      if (FAILED(hr)) {
        why = "rtv";
        return hr;
      }
    }
    HRESULT hr = d.dev5->OpenSharedFence(shared_fence, IID_PPV_ARGS(&out.shared_fence));
    if (FAILED(hr)) {
      why = "open-shared-fence";
      return hr;
    }
    hr = d.dev5->OpenSharedFence(consumed_fence, IID_PPV_ARGS(&out.consumed_fence));
    if (FAILED(hr)) {
      why = "open-consumed-fence";
      return hr;
    }
    out.width = width;
    out.height = height;
    why = "ok";
    return S_OK;
  }

}  // namespace probe::d3d
