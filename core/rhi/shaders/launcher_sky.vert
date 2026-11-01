#version 450
#extension GL_GOOGLE_include_directive : require
#include "launcher_sky.glsl"

/* The launcher sky's dome: each triangle of a level's decoded cyclorama,
 * vertex-coloured, seen from the dome's centre. inPos is the unit direction
 * with y up; the camera yaws with the pan and pitches up from level. The
 * fill draw (pc.bg.w = 1) is one triangle over the whole target from
 * gl_VertexIndex, coloured with the level's clear colour. Clip space is the
 * GL convention: the offscreen pass's viewport flips y. */

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec3 vCol;

void main()
{
    if (pc.bg.w > 0.5) {
        vec2 q = vec2(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1));
        vCol = pc.bg.rgb;
        gl_Position = vec4(q, 0.0, 1.0);
        return;
    }
    vec3 d = vec3(inPos.x, -inPos.y, inPos.z);   /* game axes: y down */
    float yaw = pc.cam.x, pitch = pc.cam.y;
    vec3 f = vec3(cos(pitch) * cos(yaw), -sin(pitch), cos(pitch) * sin(yaw));
    vec3 r = normalize(cross(vec3(0.0, 1.0, 0.0), f));   /* down x forward = right */
    vec3 dn = cross(f, r);
    float cz = dot(d, f);
    vCol = inColor.rgb;
    /* z = cz - 0.02 against w = cz clips the directions within 0.02 of the
     * view plane and those behind it. */
    gl_Position = vec4(dot(d, r) / pc.cam.z, -dot(d, dn) / pc.cam.w, cz - 0.02, cz);
}
