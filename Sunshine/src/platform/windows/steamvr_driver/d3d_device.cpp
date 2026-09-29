// d3d_device.cpp - 見 d3d_device.h。
#include "d3d_device.h"

#include "vr_ipc_abi.h"

namespace vrdrv {

  HRESULT d3d_device_t::ensure(const LUID &luid) {
    if (valid()) {
      return same_luid(luid) ? S_OK : DXGI_ERROR_INVALID_CALL;
    }
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
      return hr;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1};
    D3D_FEATURE_LEVEL got {};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) {
      return hr;
    }
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    hr = dev.As(&dev1);
    if (SUCCEEDED(hr)) {
      hr = dev.As(&dev5);
    }
    if (SUCCEEDED(hr)) {
      hr = ctx.As(&ctx4);
    }
    if (FAILED(hr)) {
      return hr;
    }
    // vrcompositor 與遊戲同卡：driver 的合成走一般優先權（不設 REALTIME）
    dev_ = dev;
    dev1_ = dev1;
    dev5_ = dev5;
    ctx_ = ctx;
    ctx4_ = ctx4;
    luid_ = luid;
    return S_OK;
  }

  HRESULT d3d_device_t::open_ring(const HANDLE tex[3], HANDLE shared_fence, HANDLE consumed_fence, uint32_t width, uint32_t height, ring_t &out, const char *&why) {
    out = ring_t {};
    if (!valid()) {
      why = "no-device";
      return E_POINTER;
    }
    for (int i = 0; i < 3; ++i) {
      HRESULT hr = dev1_->OpenSharedResource1(tex[i], IID_PPV_ARGS(&out.tex[i]));
      if (FAILED(hr)) {
        why = "open-texture";
        return hr;
      }
      D3D11_TEXTURE2D_DESC d {};
      out.tex[i]->GetDesc(&d);
      if (d.Width != width || d.Height != height || d.Format != (DXGI_FORMAT) VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM || d.MipLevels != 1 || d.ArraySize != 1 ||
          d.SampleDesc.Count != 1 || (d.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
        why = "texture-desc";
        return E_INVALIDARG;
      }
      hr = dev_->CreateRenderTargetView(out.tex[i].Get(), nullptr, &out.rtv[i]);
      if (FAILED(hr)) {
        why = "rtv";
        return hr;
      }
    }
    HRESULT hr = dev5_->OpenSharedFence(shared_fence, IID_PPV_ARGS(&out.shared_fence));
    if (FAILED(hr)) {
      why = "open-shared-fence";
      return hr;
    }
    hr = dev5_->OpenSharedFence(consumed_fence, IID_PPV_ARGS(&out.consumed_fence));
    if (FAILED(hr)) {
      why = "open-consumed-fence";
      return hr;
    }
    out.width = width;
    out.height = height;
    why = "ok";
    return S_OK;
  }

  HRESULT d3d_device_t::open_legacy(uint64_t handle, ComPtr<ID3D11Texture2D> &out) {
    out.Reset();
    if (!valid() || handle == 0) {
      return E_POINTER;
    }
    return dev_->OpenSharedResource((HANDLE) (uintptr_t) handle, IID_PPV_ARGS(&out));
  }

}  // namespace vrdrv
