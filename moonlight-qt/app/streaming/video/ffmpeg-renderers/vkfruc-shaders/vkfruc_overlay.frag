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
//   uvMax              — sample only [0,uvMax] portion of overlayTex.
//                        v1.4.104 (§J.3.e.2.i.40) — image alloc with slack
//                        capacity to avoid frequent vkDeviceWaitIdle on text
//                        size flicker (±12 px); logical text area is at
//                        image left-top, uvMax = logical_size / capacity.
//                        Old code (cap=logical) → uvMax=(1,1) preserves behavior.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D overlayTex;

layout(push_constant) uniform PC {
    vec2 rectMin;
    vec2 rectMax;
    vec2 uvMax;
} pc;

void main() {
    if (vUV.x < pc.rectMin.x || vUV.x > pc.rectMax.x ||
        vUV.y < pc.rectMin.y || vUV.y > pc.rectMax.y) {
        discard;
    }
    vec2 localUV = (vUV - pc.rectMin) / (pc.rectMax - pc.rectMin);
    outColor = texture(overlayTex, localUV * pc.uvMax);
}
