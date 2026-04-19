#version 310 es
// VipleStream: 3x3 component-wise median filter on the block-level
// MV field. Port of the desktop v1.1.136 HLSL shader
// (d3d11_mv_median.hlsl).
//
// Why: motion estimation occasionally emits 1-2 block outlier MVs
// (texture-less region, ambiguous census match). Bilinear MV
// sampling in warp then drags those outliers across several
// full-res pixels and produces visible shimmer in neighbouring
// static regions. A 3x3 median per component (independent X
// and Y) is edge-preserving so real motion boundaries survive.
//
// Format matches the Android motion-est output: GL_R32I where
// the upper 16 bits hold X and lower 16 bits hold Y (both in Q1
// fixed-point pixels).

precision highp float;
precision highp int;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

uniform highp isampler2D mvIn;
layout(r32i, binding = 0) writeonly uniform highp iimage2D mvOut;

uniform uint mvWidth;
uniform uint mvHeight;

// Optimal 9-element sorting network (25 compare-swaps, 7 parallel
// layers). Branchless, ~30% fewer comparisons than bubble sort.
#define S9(i, j) { int _m = min(a[i], a[j]); int _M = max(a[i], a[j]); a[i] = _m; a[j] = _M; }
void sort9(inout int a[9]) {
    S9(0, 3) S9(1, 7) S9(2, 5) S9(4, 8)
    S9(0, 7) S9(2, 4) S9(3, 8) S9(5, 6)
    S9(0, 2) S9(1, 3) S9(4, 5) S9(7, 8)
    S9(1, 4) S9(3, 6) S9(5, 7)
    S9(0, 1) S9(2, 4) S9(3, 5) S9(6, 8)
    S9(2, 3) S9(4, 5) S9(6, 7)
    S9(1, 2) S9(3, 4) S9(5, 6)
}
#undef S9

void main() {
    uint gx = gl_GlobalInvocationID.x;
    uint gy = gl_GlobalInvocationID.y;
    if (gx >= mvWidth || gy >= mvHeight) return;

    int sx[9];
    int sy[9];
    int n = 0;
    int minX = 0x7FFF, maxX = -0x7FFF;
    int minY = 0x7FFF, maxY = -0x7FFF;
    ivec2 dims = ivec2(int(mvWidth), int(mvHeight));

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 p = clamp(ivec2(int(gx) + dx, int(gy) + dy),
                            ivec2(0), dims - 1);
            int packed = texelFetch(mvIn, p, 0).r;
            int xv = packed >> 16;
            int yv = (packed << 16) >> 16;   // sign-extend
            sx[n] = xv; sy[n] = yv;
            minX = min(minX, xv); maxX = max(maxX, xv);
            minY = min(minY, yv); maxY = max(maxY, yv);
            n++;
        }
    }
    // Variance gate — if the 3x3 window is already uniform, skip
    // the sort. Saves the bulk of the shader's work on the static
    // majority of MV blocks in a typical frame.
    if (minX == maxX && minY == maxY) {
        int packedOut = (minX << 16) | (minY & 0xFFFF);
        imageStore(mvOut, ivec2(gx, gy), ivec4(packedOut, 0, 0, 0));
        return;
    }
    sort9(sx);
    sort9(sy);
    int medX = sx[4];
    int medY = sy[4];
    int packedOut = (medX << 16) | (medY & 0xFFFF);
    imageStore(mvOut, ivec2(gx, gy), ivec4(packedOut, 0, 0, 0));
}
