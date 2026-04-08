// VipleStream: Bicubic (Catmull-Rom) YUV420 -> RGB pixel shader
// Uses screen-space derivatives to compute texel size, avoiding GetDimensions issues
// with DXVA texture arrays that have alignment padding.

Texture2D<min16float> luminancePlane : register(t0);
Texture2D<min16float2> chrominancePlane : register(t1);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer CSC_CONST_BUF : register(b0)
{
    min16float3x3 cscMatrix;
    min16float3 offsets;
    min16float2 chromaOffset;
    min16float2 chromaTexMax;
};

// Catmull-Rom spline weights
float4 cubicWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(
        -0.5*t3 + t2 - 0.5*t,
         1.5*t3 - 2.5*t2 + 1.0,
        -1.5*t3 + 2.0*t2 + 0.5*t,
         0.5*t3 - 0.5*t2
    );
}

// Bicubic sample: 4 bilinear taps combined with Catmull-Rom weights
// texelSize = 1.0 / actual_texture_resolution (derived from UV derivatives)
float bicubicLuma(Texture2D<min16float> tex, SamplerState samp, float2 uv, float2 texelSize) {
    float2 iTc = uv / texelSize - 0.5;
    float2 f = frac(iTc);
    float2 tc = (floor(iTc) + 0.5) * texelSize;

    float4 wx = cubicWeights(f.x);
    float4 wy = cubicWeights(f.y);

    float2 s0 = float2(wx.x + wx.y, wy.x + wy.y);
    float2 s1 = float2(wx.z + wx.w, wy.z + wy.w);
    float2 f0 = float2(wx.y, wy.y) / max(s0, 0.001);
    float2 f1 = float2(wx.w, wy.w) / max(s1, 0.001);

    float2 t0 = tc + (f0 - 1.0) * texelSize;
    float2 t1 = tc + (f1 + 1.0) * texelSize;

    return (float)tex.SampleLevel(samp, float2(t0.x, t0.y), 0) * s0.x * s0.y
         + (float)tex.SampleLevel(samp, float2(t1.x, t0.y), 0) * s1.x * s0.y
         + (float)tex.SampleLevel(samp, float2(t0.x, t1.y), 0) * s0.x * s1.y
         + (float)tex.SampleLevel(samp, float2(t1.x, t1.y), 0) * s1.x * s1.y;
}

float2 bicubicChroma(Texture2D<min16float2> tex, SamplerState samp, float2 uv, float2 texelSize) {
    float2 iTc = uv / texelSize - 0.5;
    float2 f = frac(iTc);
    float2 tc = (floor(iTc) + 0.5) * texelSize;

    float4 wx = cubicWeights(f.x);
    float4 wy = cubicWeights(f.y);

    float2 s0 = float2(wx.x + wx.y, wy.x + wy.y);
    float2 s1 = float2(wx.z + wx.w, wy.z + wy.w);
    float2 f0 = float2(wx.y, wy.y) / max(s0, 0.001);
    float2 f1 = float2(wx.w, wy.w) / max(s1, 0.001);

    float2 t0 = tc + (f0 - 1.0) * texelSize;
    float2 t1 = tc + (f1 + 1.0) * texelSize;

    return (float2)tex.SampleLevel(samp, float2(t0.x, t0.y), 0) * s0.x * s0.y
         + (float2)tex.SampleLevel(samp, float2(t1.x, t0.y), 0) * s1.x * s0.y
         + (float2)tex.SampleLevel(samp, float2(t0.x, t1.y), 0) * s0.x * s1.y
         + (float2)tex.SampleLevel(samp, float2(t1.x, t1.y), 0) * s1.x * s1.y;
}

min16float4 main(ShaderInput input) : SV_TARGET
{
    // Derive texel size from screen-space UV derivatives.
    // ddx/ddy of texcoord gives us the UV change per screen pixel,
    // which equals 1/source_resolution when the texture fills the quad.
    float2 lumaTexelSize = abs(float2(ddx(input.tex.x), ddy(input.tex.y)));
    // Chroma plane is half resolution for YUV420
    float2 chromaTexelSize = lumaTexelSize * 2.0;

    float lumaY = bicubicLuma(luminancePlane, theSampler, input.tex, lumaTexelSize);
    float2 chromaUV = bicubicChroma(chrominancePlane, theSampler,
                                     min(input.tex + chromaOffset, chromaTexMax.rg),
                                     chromaTexelSize);

    min16float3 yuv = min16float3(lumaY, chromaUV);
    yuv -= offsets;
    yuv = mul(yuv, cscMatrix);

    return min16float4(yuv, 1.0);
}
