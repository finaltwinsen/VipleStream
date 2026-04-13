#version 310 es
// VipleStream v18: Multi-quality Motion Estimation (GLES 3.1)
// QUALITY_LEVEL injected at compile time: 0=Quality, 1=Balanced, 2=Performance

// QUALITY_LEVEL is #defined by FrucRenderer.java before compilation
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
  #define SAMPLE_COUNT 4
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

uint hammingDist(uint a, uint b) {
    uint x = a ^ b;
    x = x - ((x >> 1u) & 0x55u);
    x = (x & 0x33u) + ((x >> 2u) & 0x33u);
    return (x + (x >> 4u)) & 0x0Fu;
}

float computeCensusCost(ivec2 refCenter, ivec2 curCenter) {
    float cost = 0.0;
    int half_size = (SAMPLE_COUNT * SAMPLE_STRIDE) / 2;
    for (int y = 0; y < SAMPLE_COUNT; y++) {
        for (int x = 0; x < SAMPLE_COUNT; x++) {
            ivec2 offset = ivec2(x * SAMPLE_STRIDE - half_size, y * SAMPLE_STRIDE - half_size);
            ivec2 rp = clamp(refCenter + offset, ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1));
            ivec2 cp = clamp(curCenter + offset, ivec2(0), ivec2(int(frameWidth)-1, int(frameHeight)-1));
            cost += float(hammingDist(censusDesc(prevFrame, rp), censusDesc(currFrame, cp)));
        }
    }
    return cost;
}

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

    ivec2 bestMV = ivec2(0, 0);
    float bestCost = computeCensusCost(blockCenter, blockCenter);

    if (bestCost < 1.0) {
        imageStore(motionField, ivec2(blockX, blockY), ivec4(0));
        return;
    }

#if ENABLE_TEMPORAL
    // Temporal predictor
    int prevPacked = texelFetch(prevMotionField, ivec2(blockX, blockY), 0).r;
    ivec2 prevMV_Q1 = ivec2(prevPacked >> 16, (prevPacked << 16) >> 16);
    ivec2 temporalMV = ivec2(
        (prevMV_Q1.x >= 0 ? prevMV_Q1.x + 1 : prevMV_Q1.x - 1) / 2,
        (prevMV_Q1.y >= 0 ? prevMV_Q1.y + 1 : prevMV_Q1.y - 1) / 2
    );
    {
        ivec2 refCenterTmp = blockCenter + temporalMV;
        if (all(greaterThanEqual(refCenterTmp, ivec2(halfSample))) &&
            all(lessThan(refCenterTmp, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample)))) {
            float tmpCost = computeCensusCost(refCenterTmp, blockCenter);
            if (tmpCost < bestCost) { bestCost = tmpCost; bestMV = temporalMV; }
        }
    }
#endif

    // Diamond search
    for (int step = 4; step >= 1; step /= 2) {
        ivec2 prevBestMV = bestMV;
        for (int i = 0; i < SEARCH_NEIGHBORS; i++) {
            ivec2 candidate = prevBestMV + searchPattern[i] * step;
            ivec2 refCenter = blockCenter + candidate;
            if (any(lessThan(refCenter, ivec2(halfSample))) ||
                any(greaterThanEqual(refCenter, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample))))
                continue;
            float cost = computeCensusCost(refCenter, blockCenter);
            if (cost < bestCost) { bestCost = cost; bestMV = candidate; }
        }
    }

#if ENABLE_SUBPIXEL
    // Sub-pixel refinement (Q1)
    ivec2 bestMV_Q1 = bestMV * 2;
    float bestSubCost = bestCost;
    for (int si = 0; si < SEARCH_NEIGHBORS; si++) {
        ivec2 candidate_Q1 = bestMV_Q1 + searchPattern[si];
        ivec2 candidatePixel = ivec2(
            (candidate_Q1.x >= 0 ? candidate_Q1.x + 1 : candidate_Q1.x - 1) / 2,
            (candidate_Q1.y >= 0 ? candidate_Q1.y + 1 : candidate_Q1.y - 1) / 2
        );
        ivec2 refCenter = blockCenter + candidatePixel;
        if (any(lessThan(refCenter, ivec2(halfSample))) ||
            any(greaterThanEqual(refCenter, ivec2(int(frameWidth) - halfSample, int(frameHeight) - halfSample))))
            continue;
        float cost = computeCensusCost(refCenter, blockCenter);
        if (cost < bestSubCost) { bestSubCost = cost; bestMV_Q1 = candidate_Q1; }
    }
#else
    ivec2 bestMV_Q1 = bestMV * 2;
#endif

#if ENABLE_TEMPORAL
    // Temporal smoothing (70/30)
    ivec2 smoothedMV_Q1 = ivec2(
        (bestMV_Q1.x * 7 + prevMV_Q1.x * 3 + 5) / 10,
        (bestMV_Q1.y * 7 + prevMV_Q1.y * 3 + 5) / 10
    );
    smoothedMV_Q1 = clamp(smoothedMV_Q1, ivec2(-96), ivec2(96));
    int packed = (smoothedMV_Q1.x << 16) | (smoothedMV_Q1.y & 0xFFFF);
#else
    bestMV_Q1 = clamp(bestMV_Q1, ivec2(-96), ivec2(96));
    int packed = (bestMV_Q1.x << 16) | (bestMV_Q1.y & 0xFFFF);
#endif
    imageStore(motionField, ivec2(blockX, blockY), ivec4(packed, 0, 0, 0));
}
