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
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D videoTex;
layout(push_constant) uniform PC {
    vec2 uvScale;   // per-axis crop (= visibleSize / paddedSize)
} pc;
void main() {
    // 90° CW rotation of texture into display:
    //   display(x, y) ⇐ texture(y, 1-x)
    // texture-X (1920) has no padding → uvScale.x = 1.0 (drives display Y)
    // texture-Y (1088 padded for visible 1080) → uvScale.y crops (drives display X)
    vec2 uv = vec2(vUV.y          * pc.uvScale.x,
                   (1.0 - vUV.x)  * pc.uvScale.y);
    fragColor = vec4(texture(videoTex, uv).rgb, 1.0);
}
