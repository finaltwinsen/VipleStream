// compositor_vs.hlsl - VipleStream §VR：frame_compositor 的四邊形頂點著色器（設計 §E.2 frame_compositor）。
// 不需要 vertex buffer：以 SV_VertexID 0..3 產生 triangle strip；四邊形的 NDC 範圍與 UV 範圍由 constant buffer 給
// （frame_compositor.cpp 以 fov_to_rect() 算好：layer 的 tangent 範圍投影到本眼的 viewport）。
// fxc /T vs_5_0 /E main /Vn g_compositor_vs（Build-SteamVRDriver.ps1）。

cbuffer layer_cb : register(b0) {
  float4 dst_rect;  // NDC：x0, y0（下）, x1, y1（上）
  float4 uv_rect;  // u0, v0（上）, u1, v1（下）
  uint mode;  // 0 SRGB 視圖（取樣已是 linear）、1 UNORM 視為 sRGB 編碼（原樣）、2 float linear
  uint force_alpha;  // B8G8R8X8：alpha 未定義 → 1
  uint2 pad;
  float4 fovea;  // §VR-FOVEA：x＝a（0＝關）。開的時候四邊形蓋滿本眼的 viewport，uv 改成本眼影像的座標（左上 0,0）
};

struct vs_out {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

vs_out main(uint id : SV_VertexID) {
  // 0 左上、1 右上、2 左下、3 右下
  const float fx = (id & 1) ? 1.0 : 0.0;
  const float fy = (id & 2) ? 1.0 : 0.0;
  vs_out o;
  if (fovea.x > 0.0) {
    o.pos = float4(lerp(-1.0, 1.0, fx), lerp(1.0, -1.0, fy), 0.0, 1.0);
    o.uv = float2(fx, fy);
    return o;
  }
  o.pos = float4(lerp(dst_rect.x, dst_rect.z, fx), lerp(dst_rect.w, dst_rect.y, fy), 0.0, 1.0);
  o.uv = float2(lerp(uv_rect.x, uv_rect.z, fx), lerp(uv_rect.y, uv_rect.w, fy));
  return o;
}
