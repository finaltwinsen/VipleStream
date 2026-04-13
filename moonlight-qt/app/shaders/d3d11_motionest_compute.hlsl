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
#elif QUALITY_LEVEL == 1  // Balanced: 8-neighbor, 4x4 sample, no sub-pixel, with temporal
  #define SEARCH_NEIGHBORS 8
  #define SAMPLE_COUNT 4
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
    uint x = a ^ b;
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 0x0F;
}

float computeCensusCost(int2 refCenter, int2 curCenter)
{
    float cost = 0;
    int half_s = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    [loop] for (int y = 0; y < SAMPLE_COUNT; y++) {
        [loop] for (int x = 0; x < SAMPLE_COUNT; x++) {
            int2 offset = int2(x * SAMPLE_STRIDE - half_s, y * SAMPLE_STRIDE - half_s);
            int2 rp = clamp(refCenter + offset, int2(0,0), int2(frameWidth-1, frameHeight-1));
            int2 cp = clamp(curCenter + offset, int2(0,0), int2(frameWidth-1, frameHeight-1));
            cost += (float)hammingDist(censusDescriptor(prevFrame, rp), censusDescriptor(currFrame, cp));
        }
    }
    return cost;
}

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

    int2 bestMV = int2(0, 0);
    float bestCost = computeCensusCost(blockCenter, blockCenter);

    if (bestCost < 1.0) {
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
            float tmpCost = computeCensusCost(refCenterTmp, blockCenter);
            if (tmpCost < bestCost) { bestCost = tmpCost; bestMV = temporalMV; }
        }
    }
#endif

    // --- Diamond search ---
    [loop] for (int step = 4; step >= 1; step /= 2) {
        int2 prevBestMV = bestMV;
        [loop] for (int i = 0; i < SEARCH_NEIGHBORS; i++) {
            int2 candidate = prevBestMV + searchPattern[i] * step;
            int2 refCenter = blockCenter + candidate;
            if (any(refCenter < int2(halfSample, halfSample)) ||
                any(refCenter >= int2(frameWidth - halfSample, frameHeight - halfSample)))
                continue;
            float cost = computeCensusCost(refCenter, blockCenter);
            if (cost < bestCost) { bestCost = cost; bestMV = candidate; }
        }
    }

    // --- Sub-pixel refinement (Quality only) ---
#if ENABLE_SUBPIXEL
    int2 bestMV_Q1 = bestMV * 2;
    float bestSubCost = bestCost;
    [loop] for (int si = 0; si < SEARCH_NEIGHBORS; si++) {
        int2 candidate_Q1 = bestMV_Q1 + searchPattern[si];
        int2 candidatePixel = int2(
            (candidate_Q1.x >= 0 ? candidate_Q1.x + 1 : candidate_Q1.x - 1) / 2,
            (candidate_Q1.y >= 0 ? candidate_Q1.y + 1 : candidate_Q1.y - 1) / 2
        );
        int2 refCenter = blockCenter + candidatePixel;
        if (any(refCenter < int2(halfSample, halfSample)) ||
            any(refCenter >= int2(frameWidth - halfSample, frameHeight - halfSample)))
            continue;
        float cost = computeCensusCost(refCenter, blockCenter);
        if (cost < bestSubCost) { bestSubCost = cost; bestMV_Q1 = candidate_Q1; }
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
