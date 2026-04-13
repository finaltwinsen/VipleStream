#version 310 es
precision mediump float;
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D sTexture;
void main() {
    fragColor = texture(sTexture, vTexCoord);
}
