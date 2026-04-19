// VipleStream FRUC: 3x3 component-wise median filter on the
// block-level MV field.
//
// Why: the motion-estimation shader emits per-block MVs that look
// plausible in isolation but can be wrong in 1-2-block outliers —
// a single block sitting on a texture-less region, or at a motion
// boundary where the census match is ambiguous. Bilinear MV
// sampling in warp then drags those outlier MVs across several
// full-res pixels and they show up as shimmer.
//
// A 3x3 median per component (independent X and Y) is edge-
// preserving (unlike a blur) so real motion boundaries survive.
// The MV field is tiny (30x17 at 1080p with 64 px blocks) so this
// is essentially free — one compute dispatch, <0.1 ms measured.
//
// Format matches the source: R16G16_SINT, Q1 fixed-point pixels.

Texture2D<int2>   mvIn  : register(t0);
RWTexture2D<int2> mvOut : register(u0);

cbuffer Constants : register(b0)
{
    uint mvWidth;
    uint mvHeight;
    uint _pad0;
    uint _pad1;
};

// Optimal 9-element sorting network (25 compare-swaps in 7
// parallel layers, Knuth TAOCP vol. 3). Branchless on GPU and
// ~30% fewer comparisons than the unrolled selection sort we
// had before — with the variance-gate skip added above, this
// path only runs on blocks that actually need sorting so the
// saving matters.
#define S9(a, i, j) { int _m = min(a[i], a[j]); int _M = max(a[i], a[j]); a[i] = _m; a[j] = _M; }
void sort9(inout int a[9])
{
    S9(a, 0, 3) S9(a, 1, 7) S9(a, 2, 5) S9(a, 4, 8)
    S9(a, 0, 7) S9(a, 2, 4) S9(a, 3, 8) S9(a, 5, 6)
    S9(a, 0, 2) S9(a, 1, 3) S9(a, 4, 5) S9(a, 7, 8)
    S9(a, 1, 4) S9(a, 3, 6) S9(a, 5, 7)
    S9(a, 0, 1) S9(a, 2, 4) S9(a, 3, 5) S9(a, 6, 8)
    S9(a, 2, 3) S9(a, 4, 5) S9(a, 6, 7)
    S9(a, 1, 2) S9(a, 3, 4) S9(a, 5, 6)
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mvWidth || id.y >= mvHeight) return;

    int sx[9], sy[9];
    int n = 0;
    // Track min/max during the gather — we'll use these to skip
    // the sort when the 3x3 window is already uniform (static
    // background on a typical frame — a majority of MV blocks).
    int minX = 0x7FFF, maxX = -0x7FFF;
    int minY = 0x7FFF, maxY = -0x7FFF;
    [unroll]
    for (int dy = -1; dy <= 1; dy++) {
        [unroll]
        for (int dx = -1; dx <= 1; dx++) {
            int2 p = clamp(int2(id.xy) + int2(dx, dy),
                           int2(0, 0),
                           int2(mvWidth - 1, mvHeight - 1));
            int2 v = mvIn.Load(int3(p, 0));
            sx[n] = v.x;
            sy[n] = v.y;
            minX = min(minX, v.x); maxX = max(maxX, v.x);
            minY = min(minY, v.y); maxY = max(maxY, v.y);
            n++;
        }
    }
    // Variance gate: if both components are constant across the
    // neighbourhood, the median is just any sample — no need to
    // sort. On a typical frame this skips the sort for well over
    // half of MV blocks (all the static-background region).
    if (minX == maxX && minY == maxY) {
        mvOut[id.xy] = int2(minX, minY);
        return;
    }
    sort9(sx);
    sort9(sy);
    mvOut[id.xy] = int2(sx[4], sy[4]);
}
