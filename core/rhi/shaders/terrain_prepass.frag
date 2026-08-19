#version 450

/* Depth-only prepass fragment for the retained-terrain pipelines (colour
 * writes are disabled at the pipeline; unconsumed terrain.vert varyings are
 * legal and simply dropped). */

layout(location = 0) out vec4 outColor;

void main() { outColor = vec4(0.0); }
