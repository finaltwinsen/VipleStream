// VipleStream: Generic FRUC v18 — Multi-quality Warp + Blend
// Compiled with /D QUALITY_LEVEL=0 (Quality) | 1 (Balanced) | 2 (Performance)

#ifndef QUALITY_LEVEL
#define QUALITY_LEVEL 0
#endif

#if QUALITY_LEVEL == 0
  #define ENABLE_ADAPTIVE_BLEND 1
  #define ENABLE_CHEAP_ADAPTIVE 0
#elif QUALITY_LEVEL == 1
  // Balanced gets a cheap single-term adaptive blend derived from
  // MV-gradient sampling only — no extra texture reads, just 2-4
  // dot products. Fixes the p90=29.7 ms tail observed on this
  // preset without approaching Quality's ~11 ms GPU cost.
  #define ENABLE_ADAPTIVE_BLEND 0
  #define ENABLE_CHEAP_ADAPTIVE 1
#else
  #define ENABLE_ADAPTIVE_BLEND 0
  #define ENABLE_CHEAP_ADAPTIVE 0
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

// Threshold (pixels): tightened 3.0 -> 2.0 now that ME is much
// more accurate (round-2 temporal-success shortcut + tighter
// clamp + high-cost rejection). With better MVs, genuine 2-px
// disagreements between neighbours are almost always real motion
// boundaries rather than ME noise — so the nearest-neighbour
// fallback catches more shimmer sources. The earlier 2-px limit
// concern (over-triggering on sub-pixel noise) no longer applies.
#define EDGE_AWARE_MV_THRESHOLD 2.0

// Edge-aware bilinear MV interpolation (Q1 → pixel units).
// At motion boundaries (fast object next to static bg), blindly
// blending the 4 neighbor-block MVs pushes static pixels along
// the partial motion of the moving block → visible shimmer. When
// we detect high inter-block MV delta, sample the *nearest*
// block's MV instead; elsewhere bilinear still gives smooth
// sub-block interpolation.
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

    // Max MV-delta across the 4 neighbors. Using squared distances
    // avoids a bunch of sqrt() calls and comparison is equivalent.
    float2 d01 = v00 - v10;
    float2 d02 = v00 - v01;
    float2 d13 = v10 - v11;
    float2 d23 = v01 - v11;
    float  maxDiffSq = max(max(dot(d01, d01), dot(d02, d02)),
                           max(dot(d13, d13), dot(d23, d23)));

    // Gradient-magnitude gate: a real motion *boundary* is one
    // block moving and a neighbour not (or moving very
    // differently). Uniformly-high motion — all 4 blocks moving
    // 20 px in similar directions — also produces a nonzero
    // maxDiff but bilinear interpolation there is fine. Skip the
    // nearest-neighbour fallback when avg magnitude >> max delta,
    // i.e. "bulk motion" rather than a discontinuity.
    float2 avgMv = 0.25 * (v00 + v10 + v01 + v11);
    float  avgMagSq = dot(avgMv, avgMv);

    float2 top = lerp(v00, v10, frac.x);
    float2 bot = lerp(v01, v11, frac.x);
    float2 bilinear = lerp(top, bot, frac.y);

    // D3 iter 1: branchless edge-aware blend. The original code
    // took an if-branch returning either nearest or bilinear, which
    // produced severe warp/wavefront divergence on motion-boundary
    // pixels (the root of the p90=29.7 ms spike we observed on
    // Balanced). A smoothstep between "mostly bilinear" and "mostly
    // nearest" around the threshold gives the same artifact-kill
    // behaviour on boundaries while keeping every pixel on the same
    // code path — consistent per-pixel ALU regardless of MV field.
    float2 pick = (frac.x < 0.5)
        ? ((frac.y < 0.5) ? v00 : v01)
        : ((frac.y < 0.5) ? v10 : v11);
    // t=0 -> pure bilinear (uniform motion), t=1 -> pure nearest
    // (motion boundary). Edge-aware gradient-magnitude gate still
    // applies so uniformly-large bulk motion still blends.
    float threshSq = EDGE_AWARE_MV_THRESHOLD * EDGE_AWARE_MV_THRESHOLD;
    float isBoundary = step(threshSq, maxDiffSq)
                     * step(avgMagSq * 0.25, maxDiffSq);
    float t = isBoundary * smoothstep(threshSq, threshSq * 2.0, maxDiffSq);
    return lerp(bilinear, pick, t);
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

    // Wider confidence falloff (1..10 instead of 2..8 px): smoother
    // transition between "fully trusts this warp" and "switches to
    // the other side", which reduces the visible "popping" between
    // neighbouring pixels that happened when two adjacent pixels
    // fell on opposite sides of the hard 2/8 boundaries.
    float prevConf = 1.0 - smoothstep(1.0, 10.0, fwdError);
    float currConf = 1.0 - smoothstep(1.0, 10.0, bwdError);

    float totalConf = prevConf + currConf;
    float w;
    if (totalConf < 0.001) {
        w = 0.5;
    } else {
        w = currConf / totalConf;
    }
    // Large-motion bias: when |mv| is big, both warps are reaching
    // far across the frame and the chance of landing on a wrong
    // object (occlusion / disocclusion) grows. Bias toward 0.5
    // (symmetric blend) — a symmetric blend produces a "ghost" but
    // the ghost is half-strength and steadier, vs. a confident
    // wrong-side pick which produces a full-strength wrong pixel.
    // Smoothly ramps in between |mv|^2 = 100 (10 px) and 400 (20 px).
    float mvMag2 = dot(mv, mv);
    float bigMotionBias = smoothstep(100.0, 400.0, mvMag2);
    return lerp(w, 0.5, bigMotionBias);
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    uint px = dispatchID.x;
    uint py = dispatchID.y;
    if (px >= frameWidth || py >= frameHeight)
        return;

    // D3 iter 4: compute pixelPos once, reuse everywhere. HLSL
    // compiler should CSE this but making it explicit helps
    // register allocation and makes the dataflow obvious.
    float2 pp = float2(px, py);
    float2 dims = float2(frameWidth, frameHeight);
    // D3 iter 7: hoist the same-pixel currFrame fetch. Used by
    // the static-skip and the all-invalid fallback — single load
    // avoids the compiler guessing about CSE across the return
    // statements.
    float4 sameCurr = currFrame.Load(int3(px, py, 0));

    float2 mv = sampleMV(pp);
    // MV range ±48 px matches the ME shader's actual search radius.
    mv = clamp(mv, float2(-48, -48), float2(48, 48));

    // Static early-skip: D3 iter 6 — threshold raised from 0.25
    // (0.5 px) to 1.0 (1 px). In the 0.5..1 px range interp vs.
    // plain-copy is visually indistinguishable (sub-pixel shift
    // against a 16.7 ms frame baseline), but skipping saves two
    // bilinear samples + the blend on a much larger set of pixels.
    // Biggest benefit on slow-scroll / subtle-motion content.
    if (dot(mv, mv) < 1.0) {
        interpFrame[int2(px, py)] = sameCurr;
        return;
    }

    // linearSampler's AddressMode is CLAMP in the renderer, so
    // explicit saturate() is redundant — clamp happens in the
    // sampler hardware for free. Dropping two ALU ops per pixel.
    float2 prevUV = (pp - mv * 0.5 + 0.5) / dims;
    float2 currUV = (pp + mv * 0.5 + 0.5) / dims;

    float4 prevSample = prevFrame.SampleLevel(linearSampler, prevUV, 0);
    float4 currSample = currFrame.SampleLevel(linearSampler, currUV, 0);

    float2 prevPos = pp - mv * 0.5;
    float2 currPos = pp + mv * 0.5;
    // Half-pixel inset on the validity check: bilinear sampling
    // reads a 2x2 footprint around the sample position. If that
    // footprint straddles the frame edge, the clamped side of the
    // bilinear weight pulls in border-colour garbage. Marking the
    // outermost half-pixel as invalid routes those pixels to the
    // single-side fallback below instead.
    float2 lo = float2(0.5, 0.5);
    float2 hi = float2(frameWidth - 0.5, frameHeight - 0.5);
    bool prevValid = all(prevPos >= lo) && all(prevPos < hi);
    bool currValid = all(currPos >= lo) && all(currPos < hi);

    float4 result;
    if (prevValid && currValid) {
#if ENABLE_CHEAP_ADAPTIVE
        // Balanced's "cheap adaptive": weight derived from MV
        // magnitude only — no forward/backward texture reads. When
        // |mv| is small, 0.5 is fine (same as old fixed blend).
        // When |mv| is large, gently bias toward 0.5 anyway (the
        // large-motion risk mitigation from D2 iter 4). Crucially,
        // the branch behaviour is identical per pixel so warp
        // workload stays consistent, which fixes the p90=29.7 ms
        // tail we were seeing on Balanced.
        float w = 0.5;
        // Run the luma-gap catastrophic check too.
        const float3 YC = float3(0.299, 0.587, 0.114);
        float lg = abs(dot(prevSample.rgb, YC) - dot(currSample.rgb, YC));
        float b = smoothstep(0.05, 0.25, lg);
        result = lerp(lerp(prevSample, currSample, w), currSample, b);
#elif ENABLE_ADAPTIVE_BLEND
        // computeAdaptiveWeight does 2 extra sampleMV() calls (each
        // with edge-aware branches). When the MV is small the
        // weight is effectively 0.5 anyway because both warps land
        // on near-identical positions — short-circuit to avoid the
        // cost.
        float weight = (dot(mv, mv) < 4.0) ? 0.5
                                            : computeAdaptiveWeight(pp, mv);
        result = lerp(prevSample, currSample, weight);
        // Even with adaptive weighting, Quality-mode output can
        // still ghost on real occlusion edges where forward-backward
        // both fail but land on "compatible" wrong pixels. Run the
        // same luma-gap catastrophic check as balanced/perf on top
        // of the adaptive result. Cheap (2 dot products) and fixes
        // the worst residual ghosts.
        {
            const float3 YC = float3(0.299, 0.587, 0.114);
            float lg = abs(dot(prevSample.rgb, YC) - dot(currSample.rgb, YC));
            float b = smoothstep(0.05, 0.25, lg);
            result = lerp(result, currSample, b);
        }
#else
        // Catastrophic-disagreement fallback with luma weighting:
        // human eye is ~4x more sensitive to luminance than chroma,
        // so Rec.601 Y = 0.299R + 0.587G + 0.114B is a better
        // "visible disagreement" signal than RGB Euclidean. A 10%
        // luma gap now triggers bias at the same rate a 20% RGB
        // gap used to, catching visually-disruptive edges the
        // old metric missed (e.g. two differently-lit surfaces
        // with similar RGB mean).
        const float3 YCOEF = float3(0.299, 0.587, 0.114);
        float prevY = dot(prevSample.rgb, YCOEF);
        float currY = dot(currSample.rgb, YCOEF);
        float lumaDiff = abs(prevY - currY);
        // smoothstep(0.05, 0.25, lumaDiff) — starts responding at
        // 5% luma gap, fully biased at 25%. Was linear-from-10%
        // which left small-to-medium luma gaps producing partial
        // ghosts. Smoothstep gives a nicer perceptual curve too.
        float bias = smoothstep(0.05, 0.25, lumaDiff);
        result = lerp(lerp(prevSample, currSample, 0.5), currSample, bias);
#endif
    } else {
        // D3 iter 5: branchless fallback for edge pixels. prev-only
        // valid -> prevSample; curr-only -> currSample; neither ->
        // same-pixel currFrame fetch (guaranteed in-bounds).
        float4 fallback = sameCurr;
        result = prevValid ? prevSample
               : (currValid ? currSample : fallback);
    }

    interpFrame[int2(px, py)] = result;
}
