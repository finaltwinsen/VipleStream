#version 450
// VipleStream §J.3.e.2.i.3.c — fullscreen-triangle vertex shader for
// VkFrucRenderer.  Mirrors moonlight-android's fullscreen.vert: no vertex
// buffer, vkCmdDraw(3, 1, 0, 0) → 3 vertices that fully cover the screen.
//
// Vertex 0 → UV (0,0), pos (-1,-1)
// Vertex 1 → UV (2,0), pos ( 3,-1)
// Vertex 2 → UV (0,2), pos (-1, 3)
// The triangle fully covers the [-1,1]² NDC; rasterizer clips outside to
// produce a UV interpolation of [0,1]² inside the visible viewport.
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
