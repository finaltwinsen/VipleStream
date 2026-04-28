#version 450
// VipleStream §I.B.2c.3c.3 — sample the imported AHardwareBuffer image
// (bound as a combined image sampler with YCbCr conversion). The hardware
// does YUV (NV12 from MediaCodec) → RGB conversion via the immutable
// VkSamplerYcbcrConversion attached to the sampler.
//
// §I.B.2c.3c.4 (v1.2.150): UV transform via push constant.
//   uvScale = (srcW/ahbW, srcH/ahbH) crops away macroblock padding
//             (AHB rounds H up to multiples of 16, so for 1080 src the
//              AHB is 1088 → uvScale.y ≈ 0.9926).
//   The (1.0 - v) flip compensates for the convention mismatch between
//   ImageReader's AHB layout and our fullscreen-triangle UVs — GLES path
//   handled this via SurfaceTexture's getTransformMatrix; we have no
//   such matrix on the AHB import path.
//
// §I.E.b Phase 3 (v1.2.189): hdrActive push constant gates a sRGB →
// linear → ST.2084 PQ conversion path. When the swapchain was built
// in HDR10 mode (A2B10G10R10 + HDR10_ST2084), the driver expects
// PQ-encoded values; sampling an SDR stream returns sRGB-encoded RGB
// which would display dim/desaturated without the conversion.
// SDR stream is mapped to 100 nits (0.01 in PQ 0..10000 scale) per
// the BT.2408-standard "SDR reference white" → HDR mapping convention.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D videoTex;
layout(push_constant) uniform PC {
    vec2 uvScale;     // per-axis crop (= visibleSize / paddedSize)
    uint hdrActive;   // 1 = HDR10 swapchain (apply PQ encoding); 0 = SDR
    uint _pad;        // explicit padding to keep layout deterministic
} pc;

// sRGB EOTF (gamma decode): non-linear sRGB → linear-light RGB.
vec3 sRGBToLinear(vec3 c) {
    return mix(c / 12.92,
               pow((c + 0.055) / 1.055, vec3(2.4)),
               step(0.04045, c));
}

// ST.2084 PQ OETF (gamma encode): linear-light → PQ-encoded.
// Input: linear-light values where 1.0 = 10000 nits (the PQ peak).
// Reference: SMPTE ST.2084 / Rec.ITU-R BT.2100.
vec3 linearToPQ(vec3 c) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 * 128.0 / 4096.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 * 32.0 / 4096.0;
    const float c3 = 2392.0 * 32.0 / 4096.0;
    vec3 Y = pow(max(c, 0.0), vec3(m1));
    vec3 num = c1 + c2 * Y;
    vec3 den = 1.0 + c3 * Y;
    return pow(num / den, vec3(m2));
}

void main() {
    // 90° CW rotation of texture into display:
    //   display(x, y) ⇐ texture(y, 1-x)
    // texture-X (1920) has no padding → uvScale.x = 1.0 (drives display Y)
    // texture-Y (1088 padded for visible 1080) → uvScale.y crops (drives display X)
    vec2 uv = vec2(vUV.y          * pc.uvScale.x,
                   (1.0 - vUV.x)  * pc.uvScale.y);
    vec3 sampled = texture(videoTex, uv).rgb;

    if (pc.hdrActive == 1u) {
        // SDR content → HDR10 PQ pipeline:
        //   sRGB-encoded RGB  →  linear-light  →  scale 1.0 = 100 nits
        //                                          (BT.2408 SDR ref white)
        //                     →  PQ-encoded for HDR10_ST2084 swapchain.
        // Genuine HDR streams (P010 / BT.2020 / PQ) come out of the YCbCr
        // sampler already PQ-encoded; this branch would mis-handle them.
        // Phase 3.b will gate on AHB externalFormat to skip the conversion
        // for true HDR input.
        vec3 linear   = sRGBToLinear(sampled);
        vec3 pqInput  = linear * 0.01;   // 1.0 sRGB → 100 nits HDR
        fragColor     = vec4(linearToPQ(pqInput), 1.0);
    } else {
        fragColor = vec4(sampled, 1.0);
    }
}
