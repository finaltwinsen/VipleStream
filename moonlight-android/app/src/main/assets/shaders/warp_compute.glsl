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

// Threshold (pixels): tightened 4.0 -> 3.0 to match desktop,
// catches subtle 3-px discontinuities on thin-line / UI content.
#define EDGE_AWARE_MV_THRESHOLD 3.0

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

    // Gradient-magnitude gate: distinguish a real motion boundary
    // (one block moves while neighbour is static) from uniformly-
    // high bulk motion (all 4 blocks moving ~20 px the same way).
    // Bilinear is fine for the latter.
    vec2 avgMv = 0.25 * (v00 + v10 + v01 + v11);
    float avgMagSq = dot(avgMv, avgMv);

    if (maxDiffSq > EDGE_AWARE_MV_THRESHOLD * EDGE_AWARE_MV_THRESHOLD
        && maxDiffSq > avgMagSq * 0.25) {
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

    // Wider confidence falloff 1..10 — smoother transition than
    // 2..8 px, reduces pixel-level "popping" on motion boundaries.
    float prevConf = 1.0 - smoothstep(1.0, 10.0, fwdError);
    float currConf = 1.0 - smoothstep(1.0, 10.0, bwdError);

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
    // ±48 px clamp matches the ME shader's actual search radius;
    // tighter than ±64 avoids bilinear MV overshoots on motion
    // boundaries pulling stray pixels from far across the frame.
    mv = clamp(mv, vec2(-48.0), vec2(48.0));

    // Static early-skip: sub-half-pixel MVs give an output identical
    // to curr frame. Saves ALU + two texture samples on a large
    // fraction of pixels (UI chrome, backgrounds, idle frames).
    if (dot(mv, mv) < 0.25) {
        imageStore(interpFrame, ivec2(px, py),
                   texelFetch(currFrame, ivec2(px, py), 0));
        return;
    }

    vec2 dims = vec2(float(frameWidth), float(frameHeight));
    vec2 prevUV = (vec2(float(px), float(py)) - mv * 0.5 + 0.5) / dims;
    vec2 currUV = (vec2(float(px), float(py)) + mv * 0.5 + 0.5) / dims;
    // GL CLAMP_TO_EDGE on the samplers does this for free — drop
    // two ALU ops per pixel.

    vec4 prevSample = texture(prevFrame, prevUV);
    vec4 currSample = texture(currFrame, currUV);

    vec2 prevPos = vec2(float(px), float(py)) - mv * 0.5;
    vec2 currPos = vec2(float(px), float(py)) + mv * 0.5;
    bool prevValid = all(greaterThanEqual(prevPos, vec2(0.0))) && all(lessThan(prevPos, dims));
    bool currValid = all(greaterThanEqual(currPos, vec2(0.0))) && all(lessThan(currPos, dims));

    vec4 result;
    if (prevValid && currValid) {
#if ENABLE_ADAPTIVE_BLEND
        // Short-circuit adaptive weight when MV is small — saves
        // the two extra sampleMV() calls that computeAdaptiveWeight
        // does, on pixels where both warps land ~identically.
        float weight = (dot(mv, mv) < 4.0)
            ? 0.5
            : computeAdaptiveWeight(vec2(float(px), float(py)), mv);
        result = mix(prevSample, currSample, weight);
#else
        // Catastrophic-disagreement fallback: if prev/curr warps
        // land on wildly different colours (occlusion edge), the
        // blend produces a ghost. Bias toward currSample.
        vec3 diff = prevSample.rgb - currSample.rgb;
        float diffSq = dot(diff, diff);
        float bias = clamp(diffSq * 6.25 - 1.0, 0.0, 1.0);
        result = mix(mix(prevSample, currSample, 0.5), currSample, bias);
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
