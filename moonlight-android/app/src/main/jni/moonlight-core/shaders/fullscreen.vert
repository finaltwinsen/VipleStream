#version 450
// VipleStream §I.B.2c.3c — fullscreen triangle, no vertex buffer needed.
// vkCmdDraw(3, 1, 0, 0) → 3 vertices that fully cover the screen with one
// triangle. UV coords come out 0..1 with proper interpolation.
layout(location = 0) out vec2 vUV;
void main() {
    // Vertex 0 → UV (0,0), pos (-1,-1)
    // Vertex 1 → UV (2,0), pos ( 3,-1)
    // Vertex 2 → UV (0,2), pos (-1, 3)
    // The triangle fully covers [-1,1] in both axes; rasterizer clips.
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
