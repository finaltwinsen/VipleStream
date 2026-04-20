#version 310 es
// VipleStream v18: Multi-quality Motion Estimation (GLES 3.1)
// QUALITY_LEVEL injected at compile time: 0=Quality, 1=Balanced, 2=Performance
//
// Port of the desktop HLSL's Round-2 ME optimization pass:
//   I1  static-block exit threshold raised to SAMPLE_COUNT^2 * 0.5
//   I2  temporal-success shortcut (skip diamond if temporal already good)
//   I3  sub-pixel confidence gate
//   I4  bitCount() intrinsic for Hamming distance
//   I5  per-block cache of curr-frame census descriptors
//   I6  diamond convergence check (break if a step didn't improve)
//   I7  partial-SAD early termination via abortAbove param
//   I8  Balanced: SAMPLE_COUNT 4 -> 3
//   I9  cardinal-first search pattern (enables I7 sooner)
//   I10 sub-pixel pattern limited to 4 cardinal offsets

#ifndef QUALITY_LEVEL
#define QUALITY_LEVEL 0
#endif

#if QUALITY_LEVEL == 2
  #define SEARCH_NEIGHBORS 4
  #define SAMPLE_COUNT 3
  #define ENABLE_SUBPIXEL 0
  #define ENABLE_TEMPORAL 0
#elif QUALITY_LEVEL == 1
  #define SEARCH_NEIGHBORS 8
  // 3x3 sample (9 points) on Balanced — was 4x4. Stride=4 preserves
  // spatial coverage. Quality preset keeps 4x4 for best-match case.
  #define SAMPLE_COUNT 3
  #define ENABLE_SUBPIXEL 0
  #define ENABLE_TEMPORAL 1
#else
  #define SEARCH_NEIGHBORS 8
  #define SAMPLE_COUNT 4
  #define ENABLE_SUBPIXEL 1
  #define ENABLE_TEMPORAL 1
#endif

#define SAMPLE_STRIDE 4
#define CENSUS_RADIUS 1
#define SAMPLE_COUNT_SQ (SAMPLE_COUNT * SAMPLE_COUNT)

precision highp float;
precision highp int;
precision highp image2D;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform sampler2D prevFrame;
uniform sampler2D currFrame;
#if ENABLE_TEMPORAL
uniform highp isampler2D prevMotionField;
#endif
layout(r32i, binding = 0) writeonly uniform highp iimage2D motionField;

uniform uint frameWidth;
uniform uint frameHeight;
uniform uint blockSize;

// Per-invocation cache of curr-frame census descriptors (I5). The
// block's curr-frame samples don't change during search, so compute
// them once and reuse across all candidate MVs — halves census-
// related texture reads.
uint currCensusCache[SAMPLE_COUNT_SQ];

uint censusDesc(sampler2D tex, ivec2 pos) {
    float center = texelFetch(tex, clamp(pos, ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1)), 0).r;
    uint bits = 0u;
    int bit = 0;
    for (int dy = -CENSUS_RADIUS; dy <= CENSUS_RADIUS; dy++) {
        for (int dx = -CENSUS_RADIUS; dx <= CENSUS_RADIUS; dx++) {
            if (dx == 0 && dy == 0) continue;
            float nb = texelFetch(tex, clamp(pos + ivec2(dx,dy), ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1)), 0).r;
            if (nb < center) bits |= (1u << uint(bit));
            bit++;
        }
    }
    return bits;
}

// I4: GLSL 4.0+'s bitCount() intrinsic maps to popcnt on hw that has
// it, fallback LUT otherwise. Cheaper than manual parallel bit-count.
uint hammingDist(uint a, uint b) {
    return uint(bitCount(a ^ b));
}

void buildCurrCensusCache(ivec2 curCenter) {
    int half_size = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    for (int y = 0; y < SAMPLE_COUNT; y++) {
        for (int x = 0; x < SAMPLE_COUNT; x++) {
            ivec2 offset = ivec2(x * SAMPLE_STRIDE - half_size, y * SAMPLE_STRIDE - half_size);
            ivec2 cp = clamp(curCenter + offset, ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1));
            currCensusCache[y * SAMPLE_COUNT + x] = censusDesc(currFrame, cp);
        }
    }
}

// I7: Partial-SAD early termination. Caller passes the current
// bestCost — if our running sum exceeds it, this candidate can't
// win, return early. Passing a +inf sentinel preserves the old
// semantics for the MV=0 baseline.
float computeCensusCost(ivec2 refCenter, ivec2 curCenter, float abortAbove) {
    float cost = 0.0;
    int half_size = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    for (int y = 0; y < SAMPLE_COUNT; y++) {
        for (int x = 0; x < SAMPLE_COUNT; x++) {
            ivec2 offset = ivec2(x * SAMPLE_STRIDE - half_size, y * SAMPLE_STRIDE - half_size);
            ivec2 rp = clamp(refCenter + offset, ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1));
            cost += float(hammingDist(censusDesc(prevFrame, rp),
                                      currCensusCache[y * SAMPLE_COUNT + x]));
        }
        if (cost >= abortAbove) return cost;
    }
    return cost;
}

// I9: Cardinal neighbours first, diagonals last. Lets partial-SAD
// abort earlier when the best match is axis-aligned (most motion).
#if SEARCH_NEIGHBORS == 8
const ivec2 searchPattern[8] = ivec2[8](
    ivec2(0,-1), ivec2(0,1), ivec2(-1,0), ivec2(1,0),
    ivec2(-1,-1), ivec2(1,-1), ivec2(-1,1), ivec2(1,1)
);
#else
const ivec2 searchPattern[4] = ivec2[4](
    ivec2(0,-1), ivec2(0,1), ivec2(-1,0), ivec2(1,0)
);
#endif

void main() {
    uint blockX = gl_GlobalInvocationID.x;
    uint blockY = gl_GlobalInvocationID.y;

    ivec2 mvDims = imageSize(motionField);
    if (blockX >= uint(mvDims.x) || blockY >= uint(mvDims.y))
        return;

    int halfSample = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    ivec2 blockCenter = ivec2(int(blockX * blockSize + blockSize / 2u),
                              int(blockY * blockSize + blockSize / 2u));
    blockCenter = clamp(blockCenter, ivec2(halfSample), ivec2(int(frameWidth) - halfSample - 1, int(frameHeight) - halfSample - 1));

    // I5: build per-block curr-frame census cache once.
    buildCurrCensusCache(blockCenter);

    ivec2 bestMV = ivec2(0, 0);
    float bestCost = computeCensusCost(blockCenter, blockCenter, 1e9);

    // I1: raised static early-exit threshold. Catches the large
    // majority of background blocks, skips their whole search.
    if (bestCost < float(SAMPLE_COUNT_SQ) * 0.5) {
        imageStore(motionField, ivec2(blockX, blockY), ivec4(0));
        return;
    }

#if ENABLE_TEMPORAL
    int prevPacked = texelFetch(prevMotionField, ivec2(blockX, blockY), 0).r;
    ivec2 prevMV_Q1 = ivec2(prevPacked >> 16, (prevPacked << 16) >> 16);
    // D2 iter 10: sanity-clamp the temporal predictor so a corrupt
    // prev field can't propagate wild MVs forward.
    prevMV_Q1 = clamp(prevMV_Q1, ivec2(-96), ivec2(96));
    ivec2 temporalMV = ivec2(
        (prevMV_Q1.x >= 0 ? prevMV_Q1.x + 1 : prevMV_Q1.x - 1) / 2,
        (prevMV_Q1.y >= 0 ? prevMV_Q1.y + 1 : prevMV_Q1.y - 1) / 2
    );
    {
        ivec2 refCenterTmp = blockCenter + temporalMV;
        if (all(greaterThanEqual(refCenterTmp, ivec2(halfSample))) &&
            all(lessThan(refCenterTmp, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample)))) {
            float tmpCost = computeCensusCost(refCenterTmp, blockCenter, bestCost);
            if (tmpCost < bestCost) { bestCost = tmpCost; bestMV = temporalMV; }
        }
    }
    // I2: skip diamond if temporal predictor already found a good match.
    bool temporalConverged = (bestCost < float(SAMPLE_COUNT_SQ));
#else
    bool temporalConverged = false;
#endif

    if (!temporalConverged) {
        // I6: diamond convergence check.
        // D3 iter 2: hard workload cap — prevents worst-case blocks
        // (texture-less / ambiguous) from consuming the full search.
        // D3 iter 10: Balanced uses (3, 1) step sequence, Quality
        // keeps (4, 2, 1), Performance uses (2, 1).
        const int MAX_CANDIDATES = 16;
        int candidateCount = 0;
        float prevStepBestCost = bestCost;
#if QUALITY_LEVEL == 1
        const int DIAMOND_STEPS[2] = int[2](3, 1);
        const int DIAMOND_STEP_COUNT = 2;
#elif QUALITY_LEVEL == 2
        const int DIAMOND_STEPS[2] = int[2](2, 1);
        const int DIAMOND_STEP_COUNT = 2;
#else
        const int DIAMOND_STEPS[3] = int[3](4, 2, 1);
        const int DIAMOND_STEP_COUNT = 3;
#endif
        for (int ds = 0; ds < DIAMOND_STEP_COUNT; ds++) {
            int step = DIAMOND_STEPS[ds];
            ivec2 prevBestMV = bestMV;
            for (int i = 0; i < SEARCH_NEIGHBORS; i++) {
                if (candidateCount >= MAX_CANDIDATES) break;
                ivec2 candidate = prevBestMV + searchPattern[i] * step;
                ivec2 refCenter = blockCenter + candidate;
                if (any(lessThan(refCenter, ivec2(halfSample))) ||
                    any(greaterThanEqual(refCenter, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample))))
                    continue;
                float cost = computeCensusCost(refCenter, blockCenter, bestCost);
                candidateCount++;
                if (cost < bestCost) { bestCost = cost; bestMV = candidate; }
            }
            if (candidateCount >= MAX_CANDIDATES) break;
            if (bestCost >= prevStepBestCost) break;
            prevStepBestCost = bestCost;
        }
    }

#if ENABLE_SUBPIXEL
    ivec2 bestMV_Q1 = bestMV * 2;
    // I3 + D3 iter 3: skip sub-pixel on marginal matches. Refine only
    // when the integer match is confident (< 1 hamming/sample) — a
    // half-pixel correction on a noisy integer match doesn't improve
    // visually, just burns 4 cost evaluations.
    if (bestCost >= float(SAMPLE_COUNT_SQ) * 0.5 &&
        bestCost < float(SAMPLE_COUNT_SQ) &&
        (bestMV.x != 0 || bestMV.y != 0)) {
        float bestSubCost = bestCost;
        // I10: sub-pixel limited to 4 cardinal half-pixel offsets
        // (diagonals rarely help and cost 4 more evaluations each).
        for (int si = 0; si < 4; si++) {
            ivec2 candidate_Q1 = bestMV_Q1 + searchPattern[si];
            ivec2 candidatePixel = ivec2(
                (candidate_Q1.x >= 0 ? candidate_Q1.x + 1 : candidate_Q1.x - 1) / 2,
                (candidate_Q1.y >= 0 ? candidate_Q1.y + 1 : candidate_Q1.y - 1) / 2
            );
            ivec2 refCenter = blockCenter + candidatePixel;
            if (any(lessThan(refCenter, ivec2(halfSample))) ||
                any(greaterThanEqual(refCenter, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample))))
                continue;
            float cost = computeCensusCost(refCenter, blockCenter, bestSubCost);
            if (cost < bestSubCost) { bestSubCost = cost; bestMV_Q1 = candidate_Q1; }
        }
    }
#else
    ivec2 bestMV_Q1 = bestMV * 2;
#endif

    // D2 iter 1: reject high-cost matches. Write MV=0 instead of
    // propagating a random direction into warp.
    if (bestCost > float(SAMPLE_COUNT_SQ) * 3.0) {
        imageStore(motionField, ivec2(blockX, blockY), ivec4(0));
        return;
    }

#if ENABLE_TEMPORAL
    // D3 iter 9: zero/zero fast-path. When both inputs are zero the
    // 60/40 blend is pure waste — majority of static blocks stay at
    // MV=0 every frame.
    ivec2 smoothedMV_Q1;
    if (bestMV_Q1.x == 0 && bestMV_Q1.y == 0 && prevMV_Q1.x == 0 && prevMV_Q1.y == 0) {
        smoothedMV_Q1 = ivec2(0);
    } else {
        // D2 iter 2: 60/40 current/prev (was 70/30) — heavier prev
        // weighting reduces per-block MV flicker on small motion.
        smoothedMV_Q1 = ivec2(
            (bestMV_Q1.x * 6 + prevMV_Q1.x * 4 + 5) / 10,
            (bestMV_Q1.y * 6 + prevMV_Q1.y * 4 + 5) / 10
        );
        smoothedMV_Q1 = clamp(smoothedMV_Q1, ivec2(-96), ivec2(96));
    }
    int packed = (smoothedMV_Q1.x << 16) | (smoothedMV_Q1.y & 0xFFFF);
#else
    bestMV_Q1 = clamp(bestMV_Q1, ivec2(-96), ivec2(96));
    int packed = (bestMV_Q1.x << 16) | (bestMV_Q1.y & 0xFFFF);
#endif
    imageStore(motionField, ivec2(blockX, blockY), ivec4(packed, 0, 0, 0));
}
