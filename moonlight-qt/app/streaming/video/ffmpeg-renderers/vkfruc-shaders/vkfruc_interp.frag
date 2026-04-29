#version 450
// VipleStream §J.3.e.2.i.4.2 — fragment shader for interpRGB display path.
//
// Samples our planar fp32 RGB storage buffer (output of §J.3.e.2.h.c warp
// compute) and writes RGB to swapchain.  Buffer layout is 3 contiguous
// planes [R-plane, G-plane, B-plane] each W*H floats — same format that
// PlVkRenderer's §J.3.e.2.c shader emits and ME/Median/Warp consume.
//
// Used by the second of two render passes per renderFrameSw call when
// VIPLE_VKFRUC_DUAL=1 + VIPLE_VKFRUC_FRUC=1: first pass draws interp via
// this shader, second draws real currRGB via vkfruc.frag's ycbcr sampler
// path.  Together they implement §J.3.e.2.i.5 dual-present.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std430) readonly buffer InterpRGB {
    float rgb[];
};

layout(push_constant) uniform PC {
    int srcW;
    int srcH;
} pc;

void main() {
    // vUV is in [0,1]² from the fullscreen-triangle vert shader.  Map to
    // source-image pixel coords — clamp because vUV may be slightly OOB
    // due to fp interpolation at edges.
    int x = clamp(int(vUV.x * float(pc.srcW)), 0, pc.srcW - 1);
    int y = clamp(int(vUV.y * float(pc.srcH)), 0, pc.srcH - 1);
    int planeSize = pc.srcW * pc.srcH;
    int idx = y * pc.srcW + x;
    outColor = vec4(rgb[idx],
                    rgb[idx + planeSize],
                    rgb[idx + 2 * planeSize],
                    1.0);
}
