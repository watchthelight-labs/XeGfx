#version 450
// one triangle covering the middle of the image, no vertex buffer
void main() {
    vec2 p[3] = vec2[](vec2(-0.5, 0.5), vec2(0.5, 0.5), vec2(0.0, -0.5));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
