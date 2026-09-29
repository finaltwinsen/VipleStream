// d3d_device.h - VipleStream §VR：direct-mode D3D11 device（設計 §E.1、K19）與 server ring 的開啟。
//
// - device 在第一次需要時（TEXTURES 或 CreateSwapTextureSet）以 LUID 建立，活到 Cleanup；LUID 在本 vrserver
//   生命週期內不變（K20），之後 TEXTURES 帶不同 LUID → REJECT(ADAPTER_MISMATCH)。
// - ring（server 建立的 3 張 B8G8R8A8 NT handle 貼圖＋2 個 fence）是 per-generation 資源，由 direct_mode 在
//   present 鎖內換上／換下（§B.6 TEARDOWN）。
// - 這個模組只做 D3D 物件；呼叫端負責鎖（所有呼叫都在 direct_mode 的 present 鎖內，或在建立 device 的單一路徑上）。
#pragma once

#include <cstdint>

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

namespace vrdrv {

  using Microsoft::WRL::ComPtr;

  struct ring_t {
    ComPtr<ID3D11Texture2D> tex[3];
    ComPtr<ID3D11RenderTargetView> rtv[3];
    ComPtr<ID3D11Fence> shared_fence;  // driver GPU Signal、server CPU 觀察
    ComPtr<ID3D11Fence> consumed_fence;  // server GPU Signal、driver CPU 觀察
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    uint16_t space_epoch = 0;
    uint8_t layout_epoch = 0;
    // §B.7：每個 slot 最後一次 Signal 的值；consumedFence ≥ 它才可重用
    uint64_t last_fence_of[3] = {0, 0, 0};
    uint64_t fence_n = 0;
  };

  class d3d_device_t {
  public:
    // FL 11_1、需要 ID3D11Device5／ID3D11DeviceContext4（fence）。已建立而且 LUID 相同時直接回 S_OK。
    HRESULT ensure(const LUID &luid);

    bool valid() const {
      return dev5_ && ctx4_;
    }

    const LUID &luid() const {
      return luid_;
    }

    bool same_luid(const LUID &l) const {
      return valid() && l.LowPart == luid_.LowPart && l.HighPart == luid_.HighPart;
    }

    ID3D11Device *dev() const {
      return dev_.Get();
    }

    ID3D11Device1 *dev1() const {
      return dev1_.Get();
    }

    ID3D11DeviceContext *ctx() const {
      return ctx_.Get();
    }

    ID3D11DeviceContext4 *ctx4() const {
      return ctx4_.Get();
    }

    // 以 NT handle 開 server 建的 ring 與 fence（handle 由 ipc_client 關；COM 參照保住物件）。
    // GetDesc 核對尺寸／格式／bind（§B.4 D5 的防呆）。why：失敗原因（靜態字串）。
    HRESULT open_ring(const HANDLE tex[3], HANDLE shared_fence, HANDLE consumed_fence, uint32_t width, uint32_t height, ring_t &out, const char *&why);

    // legacy（KMT）共享 handle → 貼圖（vrcompositor 的 sync texture）。呼叫端自己快取結果。
    HRESULT open_legacy(uint64_t handle, ComPtr<ID3D11Texture2D> &out);

    // device removed（TDR）時回 FAILED 的 HRESULT
    HRESULT removed_reason() const {
      return dev_ ? dev_->GetDeviceRemovedReason() : E_POINTER;
    }

  private:
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11Device1> dev1_;
    ComPtr<ID3D11Device5> dev5_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext4> ctx4_;
    LUID luid_ {};
  };

}  // namespace vrdrv
