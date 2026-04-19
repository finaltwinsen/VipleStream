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

// Branchless sort network for 9 ints would be ideal but a simple
// unrolled selection sort is trivially cheap on ~500 total threads.
void sort9(inout int a[9])
{
    [unroll]
    for (int i = 0; i < 9; i++) {
        [unroll]
        for (int j = i + 1; j < 9; j++) {
            if (a[j] < a[i]) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= mvWidth || id.y >= mvHeight) return;

    int sx[9], sy[9];
    int n = 0;
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
            n++;
        }
    }
    sort9(sx);
    sort9(sy);
    mvOut[id.xy] = int2(sx[4], sy[4]);
}
