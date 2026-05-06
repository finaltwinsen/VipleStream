#version 450
// VipleStream §J.3.e.2.i.4.2 — fragment shader for interpRGB display path.
//
// Samples our planar fp32 RGB storage buffer (output of §J.3.e.2.h.c warp
// compute) and writes RGB to swapchain.  Buffer layout is 3 contiguous
// planes [R-plane, G-plane, B-plane] each W*H floats — same format that
// PlVkRenderer's §J.3.e.2.c shader emits and ME/Median/Warp consume.
//
// §B-quality (d) 2026-05-06 — switched from int(vUV*srcW) floor-truncation
// (= nearest-neighbor) to manual 4-tap bilinear.  vkfruc.frag (real path)
// uses sampler2D's hardware bilinear.  When swapchain extent != source
// (e.g. 1465×824 vs 1920×1080), nearest-neighbor on interp + bilinear on
// real produces ±1 pixel boundary jitter on object edges → visible "Y-axis
// jitter" (上下抖動) at 30Hz dual-present alternation.  Manual bilinear on
// the storage buffer brings interp sampling back into agreement with real.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, std430) readonly buffer InterpRGB {
    float rgb[];
};

layout(push_constant) uniform PC {
    int srcW;
    int srcH;
} pc;

vec3 fetchRGB(int x, int y) {
    int xx = clamp(x, 0, pc.srcW - 1);
    int yy = clamp(y, 0, pc.srcH - 1);
    int planeSize = pc.srcW * pc.srcH;
    int idx = yy * pc.srcW + xx;
    return vec3(rgb[idx],
                rgb[idx + planeSize],
                rgb[idx + 2 * planeSize]);
}

void main() {
    // vUV in [0,1]² → source pixel coordinates.  Subtract 0.5 because
    // pixel centres are at 0.5, 1.5, … in pixel-coord space.
    vec2 fp = vUV * vec2(float(pc.srcW), float(pc.srcH)) - 0.5;
    int x0 = int(floor(fp.x));
    int y0 = int(floor(fp.y));
    vec2 f = fp - vec2(float(x0), float(y0));

    vec3 c00 = fetchRGB(x0,     y0);
    vec3 c10 = fetchRGB(x0 + 1, y0);
    vec3 c01 = fetchRGB(x0,     y0 + 1);
    vec3 c11 = fetchRGB(x0 + 1, y0 + 1);
    vec3 col = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);

    outColor = vec4(col, 1.0);
}
