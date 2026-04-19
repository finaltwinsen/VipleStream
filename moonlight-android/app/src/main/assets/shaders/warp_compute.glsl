#version 310 es
// VipleStream v18: Multi-quality Warp + Blend (GLES 3.1)
// QUALITY_LEVEL injected at compile time: 0=Quality, 1=Balanced, 2=Performance

#ifndef QUALITY_LEVEL
#define QUALITY_LEVEL 0
#endif

#if QUALITY_LEVEL == 0
  #define ENABLE_ADAPTIVE_BLEND 1
#else
  #define ENABLE_ADAPTIVE_BLEND 0
#endif

precision highp float;
precision highp int;
precision highp image2D;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D prevFrame;
uniform sampler2D currFrame;
uniform highp isampler2D motionField;
layout(rgba8, binding = 0) writeonly uniform highp image2D interpFrame;

uniform uint frameWidth;
uniform uint frameHeight;
uniform uint mvBlockSize;
uniform float blendFactor;

// Threshold (pixels): if the largest MV delta among the 4 neighbor
// blocks exceeds this, we consider the pixel to be on a motion
// boundary and fall back to nearest-neighbor sampling instead of
// bilinear. Mirrors the desktop v1.1.135 change.
#define EDGE_AWARE_MV_THRESHOLD 4.0

// Edge-aware bilinear MV interpolation (Q1 -> pixel units).
// At motion boundaries (fast object next to static bg) a blind
// bilinear blend of the 4 neighbor-block MVs drags static pixels
// along the partial motion -> visible shimmer. When the MVs
// disagree strongly enough, sample the *nearest* block's MV.
vec2 sampleMV(vec2 pixelPos) {
    float blockSizeF = float(mvBlockSize);
    vec2 mvPos = pixelPos / blockSizeF - 0.5;
    ivec2 mv00 = ivec2(floor(mvPos));
    ivec2 mv10 = mv00 + ivec2(1, 0);
    ivec2 mv01 = mv00 + ivec2(0, 1);
    ivec2 mv11 = mv00 + ivec2(1, 1);

    ivec2 mvDims = textureSize(motionField, 0);
    mv00 = clamp(mv00, ivec2(0), mvDims - 1);
    mv10 = clamp(mv10, ivec2(0), mvDims - 1);
    mv01 = clamp(mv01, ivec2(0), mvDims - 1);
    mv11 = clamp(mv11, ivec2(0), mvDims - 1);

    vec2 frac_val = mvPos - floor(mvPos);

    // Unpack r32i Q1 -> pixel units (* 0.5)
    int p00 = texelFetch(motionField, mv00, 0).r;
    int p10 = texelFetch(motionField, mv10, 0).r;
    int p01 = texelFetch(motionField, mv01, 0).r;
    int p11 = texelFetch(motionField, mv11, 0).r;
    vec2 v00 = vec2(float(p00 >> 16), float((p00 << 16) >> 16)) * 0.5;
    vec2 v10 = vec2(float(p10 >> 16), float((p10 << 16) >> 16)) * 0.5;
    vec2 v01 = vec2(float(p01 >> 16), float((p01 << 16) >> 16)) * 0.5;
    vec2 v11 = vec2(float(p11 >> 16), float((p11 << 16) >> 16)) * 0.5;

    // Max pairwise MV delta (squared distances — avoids sqrt).
    vec2 d01 = v00 - v10;
    vec2 d02 = v00 - v01;
    vec2 d13 = v10 - v11;
    vec2 d23 = v01 - v11;
    float maxDiffSq = max(max(dot(d01, d01), dot(d02, d02)),
                          max(dot(d13, d13), dot(d23, d23)));

    if (maxDiffSq > EDGE_AWARE_MV_THRESHOLD * EDGE_AWARE_MV_THRESHOLD) {
        // Nearest-neighbor within the 2x2 cell.
        vec2 pick = (frac_val.x < 0.5)
            ? ((frac_val.y < 0.5) ? v00 : v01)
            : ((frac_val.y < 0.5) ? v10 : v11);
        return pick;
    }

    vec2 top = mix(v00, v10, frac_val.x);
    vec2 bot = mix(v01, v11, frac_val.x);
    return mix(top, bot, frac_val.y);
}

#if ENABLE_ADAPTIVE_BLEND
float computeAdaptiveWeight(vec2 pixelPos, vec2 mv) {
    vec2 prevPos = pixelPos - mv * 0.5;
    vec2 mvAtPrev = sampleMV(prevPos);
    vec2 backFromPrev = prevPos + mvAtPrev * 0.5;
    float fwdError = length(backFromPrev - pixelPos);

    vec2 currPos = pixelPos + mv * 0.5;
    vec2 mvAtCurr = sampleMV(currPos);
    vec2 backFromCurr = currPos - mvAtCurr * 0.5;
    float bwdError = length(backFromCurr - pixelPos);

    float prevConf = 1.0 - smoothstep(2.0, 8.0, fwdError);
    float currConf = 1.0 - smoothstep(2.0, 8.0, bwdError);

    float totalConf = prevConf + currConf;
    if (totalConf < 0.001) return 0.5;
    return currConf / totalConf;
}
#endif

void main() {
    uint px = gl_GlobalInvocationID.x;
    uint py = gl_GlobalInvocationID.y;
    if (px >= frameWidth || py >= frameHeight)
        return;

    vec2 mv = sampleMV(vec2(float(px), float(py)));
    mv = clamp(mv, vec2(-64.0), vec2(64.0));

    vec2 dims = vec2(float(frameWidth), float(frameHeight));
    vec2 prevUV = (vec2(float(px), float(py)) - mv * 0.5 + 0.5) / dims;
    vec2 currUV = (vec2(float(px), float(py)) + mv * 0.5 + 0.5) / dims;
    prevUV = clamp(prevUV, vec2(0.0), vec2(1.0));
    currUV = clamp(currUV, vec2(0.0), vec2(1.0));

    vec4 prevSample = texture(prevFrame, prevUV);
    vec4 currSample = texture(currFrame, currUV);

    vec2 prevPos = vec2(float(px), float(py)) - mv * 0.5;
    vec2 currPos = vec2(float(px), float(py)) + mv * 0.5;
    bool prevValid = all(greaterThanEqual(prevPos, vec2(0.0))) && all(lessThan(prevPos, dims));
    bool currValid = all(greaterThanEqual(currPos, vec2(0.0))) && all(lessThan(currPos, dims));

    vec4 result;
    if (prevValid && currValid) {
#if ENABLE_ADAPTIVE_BLEND
        float weight = computeAdaptiveWeight(vec2(float(px), float(py)), mv);
        result = mix(prevSample, currSample, weight);
#else
        result = mix(prevSample, currSample, 0.5);
#endif
    } else if (prevValid) {
        result = prevSample;
    } else if (currValid) {
        result = currSample;
    } else {
        result = texelFetch(currFrame, ivec2(px, py), 0);
    }

    imageStore(interpFrame, ivec2(px, py), result);
}
