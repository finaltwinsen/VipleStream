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
  uint fovea_mode;  // §VR-FOVEA-TILES：1＝注視點那塊，fovea＝(裁切左上角 x, y, 寬, 高)，本眼影像的正規化座標；0＝看 fovea.x
  uint pad;
  float4 fovea;  // §VR-FOVEA：x＝a（0＝關）、y＝cx、z＝cy：正前方在本眼影像裡的位置；w＝u0（§VR-FOVEA-INSET，> 0＝分段曲線）
};

Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);

float3 linear_to_srgb(float3 c) {
  c = saturate(c);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// §VR-FOVEA：編碼影像的座標 p（0–1）→ 均勻取樣時的座標。以正前方 c 為中心、每一側各自正規化成 e ∈ [-1,1]，
// t = a·e + (1−a)·e³：正前方的斜率是 a（< 1：同樣的視角佔比較多像素），邊緣是 3 − 2a。頭盔端解這條三次式還原。
float fovea_warp(float p, float c, float a) {
  const float ext = p < c ? c : 1.0 - c;
  const float e = (p - c) / ext;
  return c + (a * e + (1.0 - a) * e * e * e) * ext;
}

// §VR-FOVEA-INSET（2026-11-09）：分段曲線。|e| ≤ u0 的中央段斜率是 a（原生細節），外面線性壓縮到邊緣；
// 頭盔端（xrvideo.cpp 的 kFovHeader）照同一組 a、u0 精確反解。參數由 host 協商時算好（vr_fovea_profile.h）
float fovea_warp_inset(float p, float c, float a, float u0) {
  const float ext = p < c ? c : 1.0 - c;
  const float e = (p - c) / ext;
  const float ae = abs(e);
  const float k2 = (1.0 - a * u0) / (1.0 - u0);
  const float t = ae <= u0 ? a * ae : a * u0 + k2 * (ae - u0);
  return c + sign(e) * t * ext;
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 suv = uv;
  bool inside = true;
  if (fovea_mode == 1 || fovea.x > 0.0) {
    // uv 是本眼影像的座標：換成均勻取樣的座標 → NDC → 這一層四邊形裡的比例 → 這一層貼圖的 uv
    float2 q;
    if (fovea_mode == 1) {
      // §VR-FOVEA-TILES：注視點那塊＝本眼影像的一塊裁切，不變形。左上角對齊算圖像素、大小＝塊的像素數／算圖尺寸，
      // 遊戲照建議尺寸算圖時每個輸出像素正好落在一個來源像素的中心（原樣搬）
      q = fovea.xy + uv * fovea.zw;
    } else if (fovea.w > 0.0) {
      q = float2(fovea_warp_inset(uv.x, fovea.y, fovea.x, fovea.w), fovea_warp_inset(uv.y, fovea.z, fovea.x, fovea.w));
    } else {
      q = float2(fovea_warp(uv.x, fovea.y, fovea.x), fovea_warp(uv.y, fovea.z, fovea.x));
    }
    const float2 ndc = float2(2.0 * q.x - 1.0, 1.0 - 2.0 * q.y);
    const float2 f = float2((ndc.x - dst_rect.x) / (dst_rect.z - dst_rect.x), (dst_rect.w - ndc.y) / (dst_rect.w - dst_rect.y));
    suv = lerp(uv_rect.xy, uv_rect.zw, f);
    inside = all(f >= 0.0) && all(f <= 1.0);
  }
  // 縮小取樣（算圖尺寸大於串流尺寸、或注視點編碼的邊緣）：一個輸出像素涵蓋超過一個來源像素時，在它涵蓋的範圍內取
  // 四點平均（各自雙線性），不然細線會隨畫面移動閃爍。涵蓋不到一個來源像素時四點重合，等於原本的單點取樣。
  float tw, th;
  src.GetDimensions(tw, th);
  const float2 dx = ddx(suv);
  const float2 dy = ddy(suv);
  // 導數已在上面算好（quad 內一致），之後才 discard；SampleLevel 是明確 LOD，不用隱含導數
  if (!inside) {
    discard;
  }
  const float fp = max(length(dx * float2(tw, th)), length(dy * float2(tw, th)));
  const float k = 0.25 * saturate(fp - 1.0);
  const float2 lo = min(uv_rect.xy, uv_rect.zw);
  const float2 hi = max(uv_rect.xy, uv_rect.zw);
  float4 c;
  [branch]
  if (k > 0.0) {
    c = src.SampleLevel(samp, clamp(suv - k * dx - k * dy, lo, hi), 0.0);
    c += src.SampleLevel(samp, clamp(suv + k * dx - k * dy, lo, hi), 0.0);
    c += src.SampleLevel(samp, clamp(suv - k * dx + k * dy, lo, hi), 0.0);
    c += src.SampleLevel(samp, clamp(suv + k * dx + k * dy, lo, hi), 0.0);
    c *= 0.25;
  } else {
    // k == 0：四點重合，四個相同值相加再乘 0.25 與單點完全相等，只取一次（opt B036）
    c = src.SampleLevel(samp, clamp(suv, lo, hi), 0.0);
  }
  if (force_alpha != 0) {
    c.a = 1.0;
  }
  if (mode == 1) {
    return float4(saturate(c.rgb), saturate(c.a));
  }
  return float4(linear_to_srgb(c.rgb), saturate(c.a));
}
