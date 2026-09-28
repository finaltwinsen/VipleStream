// probe_d3d.h - VipleStream §VR：vr_probe 的 D3D11 輔助（ipcpeer 開 server 建的 ring／fence；unit loopback 扮 server 建它們）。
#pragma once

#include <cstdint>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

namespace probe::d3d {

  using Microsoft::WRL::ComPtr;

  struct device_t {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11DeviceContext4> ctx4;
    LUID luid {};

    bool valid() const {
      return dev5 && ctx4;
    }
  };

  // luid == nullptr：第一張硬體 adapter（跳過 Microsoft Basic Render Driver）。FL 11_1、需要 ID3D11Device5。
  HRESULT create_device(const LUID *luid, device_t &out);

  // server 端（unit loopback 用）：在 dev 上建 ring ×3（B8G8R8A8_UNORM、RT|SRV、SHARED|SHARED_NTHANDLE）與 fence ×2（SHARED）。
  // NT handle 由這個物件持有（解構時關）。與 §B.4 S4 的建立參數相同。
  struct server_ring_t {
    ComPtr<ID3D11Texture2D> tex[3];
    ComPtr<ID3D11Fence> shared_fence;  // driver GPU Signal、server CPU 觀察
    ComPtr<ID3D11Fence> consumed_fence;  // server GPU Signal、driver CPU 觀察
    HANDLE tex_nt[3] = {nullptr, nullptr, nullptr};
    HANDLE shared_fence_nt = nullptr;
    HANDLE consumed_fence_nt = nullptr;

    server_ring_t() = default;
    server_ring_t(const server_ring_t &) = delete;
    server_ring_t &operator=(const server_ring_t &) = delete;
    ~server_ring_t();
  };

  HRESULT create_server_ring(device_t &d, uint32_t width, uint32_t height, server_ring_t &out);

  // driver 端：以 NT handle 開 server 建的 ring 與 fence（handle 由呼叫端關；COM 參照保住物件）。
  struct opened_ring_t {
    ComPtr<ID3D11Texture2D> tex[3];
    ComPtr<ID3D11RenderTargetView> rtv[3];
    ComPtr<ID3D11Fence> shared_fence;
    ComPtr<ID3D11Fence> consumed_fence;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  // why：失敗原因（靜態字串）。GetDesc 核對尺寸／格式／bind／MiscFlags（§B.4 D5 的防呆）。
  HRESULT open_ring(device_t &d, const HANDLE tex[3], HANDLE shared_fence, HANDLE consumed_fence, uint32_t width, uint32_t height, opened_ring_t &out, const char *&why);

}  // namespace probe::d3d
