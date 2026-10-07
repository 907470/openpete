#ifndef OPENPETE_PREPASS_PACK_GLSL
#define OPENPETE_PREPASS_PACK_GLSL

/* The depth pre-pass colour encoding: a fragment's depth packed as 24-bit
 * fixed point into RGB (R = high byte), the value occl_downsample.frag
 * decodes. Every pre-pass fragment stage that writes colour writes this. */
vec4 op_prepass_pack_depth(float z)
{
    uint q = uint(clamp(z, 0.0, 1.0) * 16777215.0 + 0.5);
    return vec4(float((q >> 16) & 255u) / 255.0,
                float((q >>  8) & 255u) / 255.0,
                float( q        & 255u) / 255.0, 1.0);
}

#endif /* OPENPETE_PREPASS_PACK_GLSL */
