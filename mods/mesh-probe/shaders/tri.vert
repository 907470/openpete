#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 rgba;
layout(location = 0) out vec4 vcol;
void main() { vcol = rgba; gl_Position = vec4(pos, 1.0); }
