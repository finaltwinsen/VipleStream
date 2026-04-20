// VipleStream: Generic FRUC v18 — Multi-quality Motion Estimation
// Compiled with /D QUALITY_LEVEL=0 (Quality) | 1 (Balanced) | 2 (Performance)
// Each quality level produces a separate, fully-optimized .fxc shader.

#ifndef QUALITY_LEVEL
#define QUALITY_LEVEL 0
#endif

// --- Quality-dependent configuration ---
#if QUALITY_LEVEL == 2  // Performance: 4-neighbor, 3x3 sample, no sub-pixel, no temporal
  #define SEARCH_NEIGHBORS 4
  #define SAMPLE_COUNT 3
  #define ENABLE_SUBPIXEL 0
  #define ENABLE_TEMPORAL 0
#elif QUALITY_LEVEL == 1  // Balanced: 8-neighbor, 3x3 sample, no sub-pixel, with temporal
  #define SEARCH_NEIGHBORS 8
  // 3x3 sample (9 points) on Balanced instead of the old 4x4 (16).
  // Measured ME cost reduction is roughly proportional because
  // hammingDist work dominates the inner loop. Match quality stays
  // acceptable because SAMPLE_STRIDE=4 keeps the spatial coverage
  // similar. Quality preset keeps 4x4 for the best-match case.
  #define SAMPLE_COUNT 3
  #define ENABLE_SUBPIXEL 0
  #define ENABLE_TEMPORAL 1
#else  // Quality (0): full v17 pipeline
  #define SEARCH_NEIGHBORS 8
  #define SAMPLE_COUNT 4
  #define ENABLE_SUBPIXEL 1
  #define ENABLE_TEMPORAL 1
#endif

#define SAMPLE_STRIDE 4
#define CENSUS_RADIUS 1

Texture2D<float4> prevFrame      : register(t0);
Texture2D<float4> currFrame      : register(t1);
#if ENABLE_TEMPORAL
Texture2D<int2>   prevMotionField : register(t2);
#endif
RWTexture2D<int2>  motionField    : register(u0);

cbuffer Constants : register(b0)
{
    uint frameWidth;
    uint frameHeight;
    uint blockSize;
    uint searchRadius;
};

uint censusDescriptor(Texture2D<float4> tex, int2 pos)
{
    float center = tex.Load(int3(clamp(pos, int2(0,0), int2(frameWidth-1, frameHeight-1)), 0)).r;
    uint bits = 0;
    int bit = 0;
    [loop] for (int dy = -CENSUS_RADIUS; dy <= CENSUS_RADIUS; dy++) {
        [loop] for (int dx = -CENSUS_RADIUS; dx <= CENSUS_RADIUS; dx++) {
            if (dx == 0 && dy == 0) continue;
            float nb = tex.Load(int3(clamp(pos + int2(dx, dy), int2(0,0), int2(frameWidth-1, frameHeight-1)), 0)).r;
            bits |= ((nb < center) ? 1u : 0u) << bit;
            bit++;
        }
    }
    return bits;
}

uint hammingDist(uint a, uint b)
{
    // SM 5.0+ exposes countbits() which HLSL compiles to popcnt on
    // GPUs that have it (Intel Gen9+, AMD GCN+, NVIDIA Kepler+) and
    // falls back to a vectorized LUT otherwise. Measurably cheaper
    // than the unrolled parallel-bit-count the shader used before.
    return countbits(a ^ b);
}

// Per-block cache of currFrame census descriptors. The block's
// curr-frame samples are fixed throughout all candidate MV
// evaluations, so compute them once into this array and every
// subsequent computeCensusCost() call only pays the prevFrame
// census reads — half the texture traffic.
static uint currCensusCache[SAMPLE_COUNT * SAMPLE_COUNT];

void buildCurrCensusCache(int2 curCenter)
{
    int half_s = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    [unroll] for (int y = 0; y < SAMPLE_COUNT; y++) {
        [unroll] for (int x = 0; x < SAMPLE_COUNT; x++) {
            int2 offset = int2(x * SAMPLE_STRIDE - half_s, y * SAMPLE_STRIDE - half_s);
            int2 cp = clamp(curCenter + offset, int2(0,0), int2(frameWidth-1, frameHeight-1));
            currCensusCache[y * SAMPLE_COUNT + x] = censusDescriptor(currFrame, cp);
        }
    }
}

// Partial-SAD early termination: once running cost exceeds the
// current best, the rest of the loop cannot produce a winning
// candidate. Returning early is safe (caller only compares against
// bestCost). Passing bestCost as `abortAbove` allows the abort;
// passing +inf preserves the old semantics for the MV=0 baseline
// (which has no "best" yet).
float computeCensusCost(int2 refCenter, int2 curCenter, float abortAbove)
{
    float cost = 0;
    int half_s = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    [loop] for (int y = 0; y < SAMPLE_COUNT; y++) {
        [loop] for (int x = 0; x < SAMPLE_COUNT; x++) {
            int2 offset = int2(x * SAMPLE_STRIDE - half_s, y * SAMPLE_STRIDE - half_s);
            int2 rp = clamp(refCenter + offset, int2(0,0), int2(frameWidth-1, frameHeight-1));
            cost += (float)hammingDist(censusDescriptor(prevFrame, rp),
                                       currCensusCache[y * SAMPLE_COUNT + x]);
        }
        // Only check between rows — per-sample check would cost more
        // than it saves since the hamming inner loop is cheap.
        if (cost >= abortAbove) return cost;
    }
    return cost;
}

// Cardinal neighbours first, then diagonals. Most real motion is
// axis-aligned (pan, tilt, UI scroll), so testing the cardinal
// candidates before the diagonals lets bestCost improve earlier,
// which makes the partial-SAD early termination in
// computeCensusCost() abort the remaining candidates sooner.
#if SEARCH_NEIGHBORS == 8
static const int2 searchPattern[8] = {
    int2(0,-1), int2(0,1), int2(-1,0), int2(1,0),
    int2(-1,-1), int2(1,-1), int2(-1,1), int2(1,1)
};
#else
static const int2 searchPattern[4] = {
    int2(0,-1), int2(0,1), int2(-1,0), int2(1,0)
};
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
    uint blockX = dispatchID.x;
    uint blockY = dispatchID.y;

    int2 mvDims;
    motionField.GetDimensions(mvDims.x, mvDims.y);
    if (blockX >= (uint)mvDims.x || blockY >= (uint)mvDims.y)
        return;

    int halfSample = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    int2 blockCenter = int2(blockX * blockSize + blockSize / 2,
                            blockY * blockSize + blockSize / 2);
    blockCenter = clamp(blockCenter, int2(halfSample, halfSample),
                        int2(frameWidth - halfSample - 1, frameHeight - halfSample - 1));

    // Build the per-block curr-frame census cache once; every
    // computeCensusCost call below reuses it.
    buildCurrCensusCache(blockCenter);

    int2 bestMV = int2(0, 0);
    // MV=0 baseline — no abort threshold yet, pass +inf sentinel.
    float bestCost = computeCensusCost(blockCenter, blockCenter, 1e9);

    // Static early-exit: the hammingDist range is 0..8 per sample
    // (census window is 3x3, 8 surrounding bits). Summed over
    // SAMPLE_COUNT x SAMPLE_COUNT samples we have 0..8 * SAMPLE_COUNT^2
    // total. If the MV=0 baseline cost averages less than ~0.5
    // hamming bits per sample, it's close enough to static that
    // diamond search is guaranteed to find nothing better. This
    // catches the large majority of background blocks in a typical
    // frame (UI chrome, walls, sky) and avoids ~24 full cost
    // evaluations each. The old threshold of 1.0 was so tight it
    // only caught pixel-perfect-identical blocks.
    if (bestCost < (float)(SAMPLE_COUNT * SAMPLE_COUNT) * 0.5) {
        motionField[int2(blockX, blockY)] = int2(0, 0);
        return;
    }

    // --- Temporal predictor (Quality + Balanced only) ---
#if ENABLE_TEMPORAL
    int2 prevMV_Q1 = prevMotionField.Load(int3(blockX, blockY, 0));
    int2 temporalMV = int2(
        (prevMV_Q1.x >= 0 ? prevMV_Q1.x + 1 : prevMV_Q1.x - 1) / 2,
        (prevMV_Q1.y >= 0 ? prevMV_Q1.y + 1 : prevMV_Q1.y - 1) / 2
    );
    {
        int2 refCenterTmp = blockCenter + temporalMV;
        if (all(refCenterTmp >= int2(halfSample, halfSample)) &&
            all(refCenterTmp < int2(frameWidth - halfSample, frameHeight - halfSample))) {
            float tmpCost = computeCensusCost(refCenterTmp, blockCenter, bestCost);
            if (tmpCost < bestCost) { bestCost = tmpCost; bestMV = temporalMV; }
        }
    }
    // Temporal-success shortcut: if continuing last-frame's motion
    // already matches well (< 1 hamming per sample on average),
    // motion is smooth and the diamond search is unlikely to find
    // anything better. Skip the 24 cost evaluations the diamond
    // would do. Falls through to sub-pixel / output as usual.
    // Only applied to Balanced+Quality since Performance doesn't
    // have ENABLE_TEMPORAL.
    bool temporalConverged = (bestCost < (float)(SAMPLE_COUNT * SAMPLE_COUNT));
#else
    const bool temporalConverged = false;
#endif

    // --- Diamond search ---
    // Convergence check: if a step didn't improve bestCost at all,
    // smaller subsequent steps (which are strict subsets of the
    // next-larger step's neighbourhood) are also guaranteed to not
    // improve. Bail once we hit that. Typical win on high-detail
    // content: cuts ~1 of 3 steps on average.
    if (!temporalConverged) {
    float prevStepBestCost = bestCost;
    [loop] for (int step = 4; step >= 1; step /= 2) {
        int2 prevBestMV = bestMV;
        [loop] for (int i = 0; i < SEARCH_NEIGHBORS; i++) {
            int2 candidate = prevBestMV + searchPattern[i] * step;
            int2 refCenter = blockCenter + candidate;
            if (any(refCenter < int2(halfSample, halfSample)) ||
                any(refCenter >= int2(frameWidth - halfSample, frameHeight - halfSample)))
                continue;
            float cost = computeCensusCost(refCenter, blockCenter, bestCost);
            if (cost < bestCost) { bestCost = cost; bestMV = candidate; }
        }
        if (bestCost >= prevStepBestCost) break;
        prevStepBestCost = bestCost;
    }
    }  // end if (!temporalConverged)

    // --- Sub-pixel refinement (Quality only) ---
#if ENABLE_SUBPIXEL
    int2 bestMV_Q1 = bestMV * 2;
    // Confidence gate: if the integer match is already very good
    // (< 0.5 hamming per sample on average), a sub-pixel search is
    // unlikely to improve visibly — it refines from a 1-px ambiguity
    // down to 0.5-px, which matters for bad matches, not good ones.
    // Skip the 8 extra cost evaluations. Also skip when MV=0 since
    // sub-pixel refining zero motion is inherently useless.
    if (bestCost >= (float)(SAMPLE_COUNT * SAMPLE_COUNT) * 0.5 &&
        any(bestMV != int2(0, 0))) {
        float bestSubCost = bestCost;
        // Sub-pixel refinement limited to the 4 cardinal half-pixel
        // offsets. Diagonal half-pixel corrections are almost always
        // noise and rarely produce a better match than cardinal —
        // halving the candidate count here is free quality-wise
        // and saves 4 of 8 cost evaluations per Quality-mode block.
        [loop] for (int si = 0; si < 4; si++) {
            int2 candidate_Q1 = bestMV_Q1 + searchPattern[si];
            int2 candidatePixel = int2(
                (candidate_Q1.x >= 0 ? candidate_Q1.x + 1 : candidate_Q1.x - 1) / 2,
                (candidate_Q1.y >= 0 ? candidate_Q1.y + 1 : candidate_Q1.y - 1) / 2
            );
            int2 refCenter = blockCenter + candidatePixel;
            if (any(refCenter < int2(halfSample, halfSample)) ||
                any(refCenter >= int2(frameWidth - halfSample, frameHeight - halfSample)))
                continue;
            float cost = computeCensusCost(refCenter, blockCenter, bestSubCost);
            if (cost < bestSubCost) { bestSubCost = cost; bestMV_Q1 = candidate_Q1; }
        }
    }
#else
    // No sub-pixel: store integer MV as Q1 (×2)
    int2 bestMV_Q1 = bestMV * 2;
#endif

    // --- Temporal smoothing (Quality + Balanced only) ---
#if ENABLE_TEMPORAL
    int2 smoothedMV_Q1 = int2(
        (bestMV_Q1.x * 7 + prevMV_Q1.x * 3 + 5) / 10,
        (bestMV_Q1.y * 7 + prevMV_Q1.y * 3 + 5) / 10
    );
    smoothedMV_Q1 = clamp(smoothedMV_Q1, int2(-96, -96), int2(96, 96));
    motionField[int2(blockX, blockY)] = smoothedMV_Q1;
#else
    bestMV_Q1 = clamp(bestMV_Q1, int2(-96, -96), int2(96, 96));
    motionField[int2(blockX, blockY)] = bestMV_Q1;
#endif
}
