#version 450
// VipleStream §J.3.e.2.i.3.c — fragment shader for VkFrucRenderer.
//
// Samples the imported AVVkFrame NV12 image via a combined image+sampler
// whose VkSamplerYcbcrConversion does the YCbCr→RGB conversion in driver
// (BT.709 narrow range, matches our DECODER_COLORSPACE = COLORSPACE_REC_709).
//
// Unlike moonlight-android's video_sample.frag (Android does a 90° CW
// rotation in shader because phone display is portrait while AHB layout is
// landscape), the PC desktop swapchain orientation matches the source —
// so we sample directly with no rotation.  No HDR PQ encoding either at
// i.3.c first ship; HDR support deferred to i.6+.
//
// Push constants reserved for future i.4 integration (interpolation slot
// select, blend factor, etc.); no push constants for the i.3.c skeleton.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D videoTex;

void main() {
    outColor = vec4(texture(videoTex, vUV).rgb, 1.0);
}
