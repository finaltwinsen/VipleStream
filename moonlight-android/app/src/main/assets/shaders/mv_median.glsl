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

void sort9(inout int a[9]) {
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 9; j++) {
            if (a[j] < a[i]) {
                int t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
}

void main() {
    uint gx = gl_GlobalInvocationID.x;
    uint gy = gl_GlobalInvocationID.y;
    if (gx >= mvWidth || gy >= mvHeight) return;

    int sx[9];
    int sy[9];
    int n = 0;
    ivec2 dims = ivec2(int(mvWidth), int(mvHeight));

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 p = clamp(ivec2(int(gx) + dx, int(gy) + dy),
                            ivec2(0), dims - 1);
            int packed = texelFetch(mvIn, p, 0).r;
            // Upper 16 bits = X, lower 16 bits = Y (Q1 fixed point).
            sx[n] = packed >> 16;
            sy[n] = (packed << 16) >> 16;   // sign-extend
            n++;
        }
    }
    sort9(sx);
    sort9(sy);
    int medX = sx[4];
    int medY = sy[4];
    int packedOut = (medX << 16) | (medY & 0xFFFF);
    imageStore(mvOut, ivec2(gx, gy), ivec4(packedOut, 0, 0, 0));
}
