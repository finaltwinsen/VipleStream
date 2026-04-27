#version 450
// VipleStream §I.B.2c.3c.2 — UV gradient test pattern. Confirms the
// graphics pipeline (render pass, framebuffer, vertex stage) works
// before c.3c.3 swaps in the YCbCr sampler. Visual: corners go
// black (0,0), yellow (1,1), red (1,0), green (0,1).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = vec4(vUV.x, vUV.y, 0.0, 1.0);
}
