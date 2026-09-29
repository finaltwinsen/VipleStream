// compositor_ps.hlsl - VipleStream §VR：frame_compositor 的像素著色器（設計 §E.2 frame_compositor）。
// 輸出寫進 server 建的 B8G8R8A8_UNORM ring（非 SRGB 視圖）：一律輸出 sRGB 編碼值。
//  - mode 0：app 貼圖以 *_SRGB 視圖取樣（硬體已解成 linear）→ linear_to_srgb
//  - mode 1：*_UNORM（非 SRGB）視為 sRGB 編碼內容 → 原樣（等價於 srgb_to_linear 再 linear_to_srgb，省掉精度損失）
//  - mode 2：R16G16B16A16_FLOAT 視為 linear，M1b 夾到 [0,1]（tonemap 延到 M4a）→ linear_to_srgb
// overlay 在 gamma 空間混合（M1b 註明，視覺差異可接受）。
// fxc /T ps_5_0 /E main /Vn g_compositor_ps（Build-SteamVRDriver.ps1）。

cbuffer layer_cb : register(b0) {
  float4 dst_rect;
  float4 uv_rect;
  uint mode;
  uint force_alpha;
  uint2 pad;
};

Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);

float3 linear_to_srgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 c = src.Sample(samp, uv);
  if (force_alpha != 0) {
    c.a = 1.0;
  }
  if (mode == 1) {
    return float4(saturate(c.rgb), saturate(c.a));
  }
  return float4(linear_to_srgb(c.rgb), saturate(c.a));
}
