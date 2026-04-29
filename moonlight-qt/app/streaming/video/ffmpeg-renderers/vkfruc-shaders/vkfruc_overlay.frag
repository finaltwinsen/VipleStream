#version 450
// VipleStream §J.3.e.2.i — overlay composite fragment shader.
//
// Draws a moonlight-qt OverlayManager surface (RGBA8 SDL_Surface uploaded
// as VkImage) onto the swapchain.  Reuses the fullscreen-triangle vert
// shader (vkfruc.vert) — frag computes overlay UV from screen UV via
// push-constant rect, and discards fragments outside the rect.  Alpha
// blending in the pipeline state composites onto the underlying video
// frame already drawn this render pass.
//
// Push constants:
//   rectMin / rectMax  — overlay rectangle in screen-UV [0,1]² space.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D overlayTex;

layout(push_constant) uniform PC {
    vec2 rectMin;
    vec2 rectMax;
} pc;

void main() {
    if (vUV.x < pc.rectMin.x || vUV.x > pc.rectMax.x ||
        vUV.y < pc.rectMin.y || vUV.y > pc.rectMax.y) {
        discard;
    }
    vec2 localUV = (vUV - pc.rectMin) / (pc.rectMax - pc.rectMin);
    outColor = texture(overlayTex, localUV);
}
