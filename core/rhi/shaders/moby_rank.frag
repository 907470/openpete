#version 450

/* UNTEXTURED moby RANK pass fragment stage: encode the face's og OT walk
 * rank + present-local moby ordinal into the RGBA8 rank image (bytes:
 * r = rank & 255, g = rank >> 8, b = ordinal, a = the face's TRUE view-Z
 * at this pixel / 128, clamped - the select's near-tie margin input). The
 * depth attachment (moby_rank.vert's 1-(rank+1)/65536) already resolved
 * the per-pixel winner to the HIGHEST rank; this stage writes its identity. */

layout(location = 0) flat in vec2 vRank;  /* x = rank (>=1), y = ordinal */
layout(location = 3) in float vVz;        /* TRUE view-Z */

layout(location = 0) out vec4 outColor;

void main()
{
    float r = clamp(vRank.x, 0.0, 65534.0);
    float o = clamp(vRank.y, 0.0, 255.0);
    outColor = vec4(mod(r, 256.0) / 255.0, floor(r / 256.0) / 255.0,
                    o / 255.0, clamp(vVz / 32640.0, 0.0, 1.0));
}
