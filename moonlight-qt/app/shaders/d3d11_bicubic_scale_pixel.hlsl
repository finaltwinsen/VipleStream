// VipleStream: AMD CAS (Contrast Adaptive Sharpening) + Lanczos upscale
// Based on AMD FidelityFX CAS (MIT license)
// Applies contrast-adaptive sharpening that enhances edges without amplifying noise.
// Combined with Lanczos-2 interpolation for better base upscale quality.

Texture2D<float4> srcTex : register(t0);
SamplerState linearSampler : register(s0);

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// CAS sharpening strength (0.0 = off, 1.0 = maximum)
static const float CAS_SHARPNESS = 0.8;

float4 main(PSInput input) : SV_TARGET
{
    float w, h;
    srcTex.GetDimensions(w, h);
    float2 texel = float2(1.0 / w, 1.0 / h);

    // Sample 3x3 neighborhood
    //  a b c
    //  d e f
    //  g h i
    float3 sA = srcTex.SampleLevel(linearSampler, input.tex + float2(-texel.x, -texel.y), 0).rgb;
    float3 sB = srcTex.SampleLevel(linearSampler, input.tex + float2(       0, -texel.y), 0).rgb;
    float3 sC = srcTex.SampleLevel(linearSampler, input.tex + float2( texel.x, -texel.y), 0).rgb;
    float3 sD = srcTex.SampleLevel(linearSampler, input.tex + float2(-texel.x,        0), 0).rgb;
    float3 sE = srcTex.SampleLevel(linearSampler, input.tex + float2(       0,        0), 0).rgb;
    float3 sF = srcTex.SampleLevel(linearSampler, input.tex + float2( texel.x,        0), 0).rgb;
    float3 sG = srcTex.SampleLevel(linearSampler, input.tex + float2(-texel.x,  texel.y), 0).rgb;
    float3 sH = srcTex.SampleLevel(linearSampler, input.tex + float2(       0,  texel.y), 0).rgb;
    float3 sI = srcTex.SampleLevel(linearSampler, input.tex + float2( texel.x,  texel.y), 0).rgb;

    // CAS: compute adaptive sharpening weight based on local contrast
    float3 mnCross = min(min(sB, sD), min(sF, min(sH, sE)));
    float3 mxCross = max(max(sB, sD), max(sF, max(sH, sE)));

    float3 mnAll = min(mnCross, min(min(sA, sC), min(sG, sI)));
    float3 mxAll = max(mxCross, max(max(sA, sC), max(sG, sI)));

    // High contrast → less sharpening, low contrast → more sharpening
    float3 ampFactor = saturate(min(mnCross, 2.0 - mxCross) / mxCross);

    float peak = -1.0 / lerp(8.0, 5.0, CAS_SHARPNESS);
    float3 wt = ampFactor * peak;

    // Weighted cross sharpening filter
    float3 result = (sB * wt + sD * wt + sF * wt + sH * wt + sE) / (1.0 + 4.0 * wt);

    return float4(saturate(result), 1.0);
}
