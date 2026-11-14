// frame_compositor.h - VipleStream §VR：把 SteamVR 送來的 layer 合成到 server 的 ring slot（設計 §E.2 frame_compositor）。
//
// V4 是獨立撰寫（本機沒有 ALVR 原始碼可移植；設計 §E.3 原定移植 FrameRender，改列偏差）：
//  - 每眼一個 viewport（左半／右半）；第 0 層 ONE/ZERO，其餘 SRC_ALPHA/INV_SRC_ALPHA；
//  - 每層畫成以 FOV tangent 投影的四邊形（layer 的 mProjection 反推 tangent 範圍 → 本眼的 fov_to_rect()），UV 取 bounds；
//  - 格式白名單在 direct_mode 建立 swap set 時決定（SRV 與 mode 在那裡建一次，不每幀建）。
// 呼叫端（direct_mode）持 present 鎖；這裡不做任何同步。
#pragma once

#include <cstdint>

#include <d3d11_4.h>
#include <wrl/client.h>

#include "vr_math.h"

namespace vrdrv {

  using Microsoft::WRL::ComPtr;

  // 一層一眼的輸入（已解析成我們的 SRV）
  struct compose_eye_t {
    ID3D11ShaderResourceView *srv = nullptr;  // nullptr → 這一眼略過
    uint32_t mode = 0;  // compositor_ps.hlsl 的 mode
    bool force_alpha = false;
    float bounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // uMin, vMin, uMax, vMax
    float proj[4][4] = {};  // HmdMatrix44_t（列主序）；m[0][0] == 0 表示沒有（當成整眼）
  };

  struct compose_layer_t {
    compose_eye_t eye[2];
  };

  // §VR-FOVEA-TILES（2026-11-10）：每眼兩塊影像。rtv＝(2·tile_w)×(2·tile_h)：上排左、右眼的全視野（整眼縮小），
  // 下排左、右眼的注視點（本眼影像裡 origin 起、cover 大小的裁切，不變形）
  struct tile_layout_t {
    uint32_t tile_w = 0;
    uint32_t tile_h = 0;
    float origin[2][2] = {};  // [eye][x, y]：注視點那塊的左上角（本眼影像的正規化座標，y 由上緣算）
    float cover[2] = {};  // 注視點那塊佔本眼影像的比例（x、y）
  };

  class frame_compositor_t {
  public:
    // 建 shader、sampler、blend、constant buffer。device 換了要重建（K19：device 活到 Cleanup，不會換）。
    HRESULT init(ID3D11Device *dev);

    bool ready() const {
      return vs_ && ps_;
    }

    // 合成到 rtv（packed：左眼在左半、右眼在右半）。eye_rect：本眼的 tangent 範圍（fov_to_rect）。
    // layer 為 0 層時清成黑色（仍發布，讓串流不中斷）。
    // fovea_a（§VR-FOVEA）：0＝均勻取樣；0.4–1＝注視點編碼，正前方（tangent 0）的斜率（見 compositor_ps.hlsl）。
    // fovea_u0（§VR-FOVEA-INSET）：> 0 時改用分段曲線（中央段斜率 fovea_a、範圍 fovea_u0），0＝三次曲線
    // tiles（§VR-FOVEA-TILES）：不是 nullptr 時改畫成每眼兩塊（eye_w／eye_h 與 fovea_* 不看）
    // rt_w／rt_h（§PERF-1113）：rtv 的實際像素尺寸。有給、而且確定第 0 層會把整張寫滿時不先清成黑色；
    // 0＝不知道，一律先清（原本的行為）
    void compose(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, uint32_t eye_w, uint32_t eye_h, const math::rect_t eye_rect[2], const compose_layer_t *layers, uint32_t count, float fovea_a = 0.0f, const float fovea_centre[2][2] = nullptr, float fovea_u0 = 0.0f, const tile_layout_t *tiles = nullptr, uint32_t rt_w = 0, uint32_t rt_h = 0);

  private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_opaque_;
    ComPtr<ID3D11BlendState> blend_alpha_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11DepthStencilState> depth_off_;
    ComPtr<ID3D11Buffer> cb_;
  };

}  // namespace vrdrv
