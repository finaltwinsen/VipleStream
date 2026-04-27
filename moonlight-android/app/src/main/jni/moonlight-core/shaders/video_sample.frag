#version 450
// VipleStream §I.B.2c.3c.3 — sample the imported AHardwareBuffer image
// (bound as a combined image sampler with YCbCr conversion). The hardware
// does YUV (NV12 from MediaCodec) → RGB conversion via the immutable
// VkSamplerYcbcrConversion attached to the sampler.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 fragColor;
layout(set = 0, binding = 0) uniform sampler2D videoTex;
void main() {
    fragColor = vec4(texture(videoTex, vUV).rgb, 1.0);
}
