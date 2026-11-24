#version 450
// YCbCr conversion 出來的是非線性 R'G'B'。目標若是 sRGB attachment，
// 寫進去的值必須是線性的，驅動才會再編成 sRGB；用 2.2 次方會和規格的分段不一致。
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uImage;
// std140 會把這個 block 補到 16 的倍數。欄位只到 24 時，編譯器仍可能把 block 做成 32，
// pipeline 的 push constant range 若只報 24 會建不起來。後面兩個 int 把大小釘死。
layout(push_constant) uniform Push {
    vec4 uvRect;
    int linearize;
    int pad0;
    int pad1;
    int pad2;
} pc;

float srgbToLinear(float c) {
    if (c <= 0.04045) {
        return c / 12.92;
    }
    return pow((c + 0.055) / 1.055, 2.4);
}

void main() {
    vec2 uv = vec2(pc.uvRect.x + vUV.x * pc.uvRect.z,
                   pc.uvRect.y + vUV.y * pc.uvRect.w);
    vec3 rgb = texture(uImage, uv).rgb;
    if (pc.linearize != 0) {
        rgb = vec3(srgbToLinear(rgb.r), srgbToLinear(rgb.g), srgbToLinear(rgb.b));
    }
    outColor = vec4(rgb, 1.0);
}
