// VipleStream: Generic FRUC v18 — Multi-quality Warp + Blend
// Compiled with /D QUALITY_LEVEL=0 (Quality) | 1 (Balanced) | 2 (Performance)

#ifndef QUALITY_LEVEL
#define QUALITY_LEVEL 0
#endif

#if QUALITY_LEVEL == 0
  #define ENABLE_ADAPTIVE_BLEND 1
#else
  #define ENABLE_ADAPTIVE_BLEND 0
#endif

Texture2D<float4>  prevFrame   : register(t0);
Texture2D<float4>  currFrame   : register(t1);
Texture2D<int2>    motionField : register(t2);

RWTexture2D<float4> interpFrame : register(u0);

SamplerState linearSampler : register(s0);

cbuffer Constants : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint mvBlockSize;
    float blendFactor;
};

// Bilinear MV interpolation (Q1 → pixel units)
float2 sampleMV(float2 pixelPos)
{
    float blockSizeF = float(mvBlockSize);
    float2 mvPos = pixelPos / blockSizeF - 0.5;
    int2 mv00 = int2(floor(mvPos));
    int2 mv10 = mv00 + int2(1, 0);
    int2 mv01 = mv00 + int2(0, 1);
    int2 mv11 = mv00 + int2(1, 1);

    int2 mvDims;
    motionField.GetDimensions(mvDims.x, mvDims.y);
    mv00 = clamp(mv00, int2(0, 0), mvDims - 1);
    mv10 = clamp(mv10, int2(0, 0), mvDims - 1);
    mv01 = clamp(mv01, int2(0, 0), mvDims - 1);
    mv11 = clamp(mv11, int2(0, 0), mvDims - 1);

    float2 frac = mvPos - floor(mvPos);

    float2 v00 = float2(motionField.Load(int3(mv00, 0))) * 0.5;
    float2 v10 = float2(motionField.Load(int3(mv10, 0))) * 0.5;
    float2 v01 = float2(motionField.Load(int3(mv01, 0))) * 0.5;
    float2 v11 = float2(motionField.Load(int3(mv11, 0))) * 0.5;

    float2 top = lerp(v00, v10, frac.x);
    float2 bot = lerp(v01, v11, frac.x);
    return lerp(top, bot, frac.y);
}

#if ENABLE_ADAPTIVE_BLEND
float computeAdaptiveWeight(float2 pixelPos, float2 mv)
{
    float2 prevPos = pixelPos - mv * 0.5;
    float2 mvAtPrev = sampleMV(prevPos);
    float2 backFromPrev = prevPos + mvAtPrev * 0.5;
    float fwdError = length(backFromPrev - pixelPos);

    float2 currPos = pixelPos + mv * 0.5;
    float2 mvAtCurr = sampleMV(currPos);
    float2 backFromCurr = currPos - mvAtCurr * 0.5;
    float bwdError = length(backFromCurr - pixelPos);

    float prevConf = 1.0 - smoothstep(2.0, 8.0, fwdError);
    float currConf = 1.0 - smoothstep(2.0, 8.0, bwdError);

    float totalConf = prevConf + currConf;
    if (totalConf < 0.001) return 0.5;
    return currConf / totalConf;
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    uint px = dispatchID.x;
    uint py = dispatchID.y;
    if (px >= frameWidth || py >= frameHeight)
        return;

    float2 mv = sampleMV(float2(px, py));
    mv = clamp(mv, float2(-64, -64), float2(64, 64));

    float2 prevUV = (float2(px, py) - mv * 0.5 + 0.5) / float2(frameWidth, frameHeight);
    float2 currUV = (float2(px, py) + mv * 0.5 + 0.5) / float2(frameWidth, frameHeight);
    prevUV = saturate(prevUV);
    currUV = saturate(currUV);

    float4 prevSample = prevFrame.SampleLevel(linearSampler, prevUV, 0);
    float4 currSample = currFrame.SampleLevel(linearSampler, currUV, 0);

    float2 prevPos = float2(px, py) - mv * 0.5;
    float2 currPos = float2(px, py) + mv * 0.5;
    bool prevValid = all(prevPos >= 0) && all(prevPos < float2(frameWidth, frameHeight));
    bool currValid = all(currPos >= 0) && all(currPos < float2(frameWidth, frameHeight));

    float4 result;
    if (prevValid && currValid) {
#if ENABLE_ADAPTIVE_BLEND
        float weight = computeAdaptiveWeight(float2(px, py), mv);
        result = lerp(prevSample, currSample, weight);
#else
        result = lerp(prevSample, currSample, 0.5);
#endif
    } else if (prevValid) {
        result = prevSample;
    } else if (currValid) {
        result = currSample;
    } else {
        result = currFrame.Load(int3(px, py, 0));
    }

    interpFrame[int2(px, py)] = result;
}
