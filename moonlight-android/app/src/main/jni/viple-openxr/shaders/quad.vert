#version 450
// 一個三角形蓋滿裁剪空間，不需要 vertex buffer。
// Vulkan 的裁剪空間 y=-1 在畫面上緣，所以 vUV.y=0 就是最上面一列，和貼圖的 v 同方向，不必翻。
layout(location = 0) out vec2 vUV;
void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
