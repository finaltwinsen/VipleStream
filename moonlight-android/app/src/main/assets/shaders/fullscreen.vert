#version 310 es
in vec2 aPosition;
in vec2 aTexCoord;
out vec2 vTexCoord;
uniform mat4 uTexMatrix;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = (uTexMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;
}
