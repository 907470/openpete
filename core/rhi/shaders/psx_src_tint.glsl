#ifndef OPENPETE_PSX_SRC_TINT_GLSL
#define OPENPETE_PSX_SRC_TINT_GLSL

/* Terrain source tint (OPENPETE_TERRAIN_SOURCE_TINT): a diagnostic that
 * mixes every terrain fragment toward green when the retained terrain cache
 * drew it and toward red when the CPU walk drew it, after the stock shading.
 *
 * Constant 6 is the blend in thousandths, 0 when the instrument is off
 * (rhi_terrain_source_tint). At 0 every helper below returns its input,
 * so the stock stages compile to their untinted form.
 *
 * Walked terrain is marked per vertex by the CPU packer: with the tint on
 * it stores a walked terrain face's true view-Z (rhi_vertex.w) negated and
 * every other face's clamped to at least 1, so the sign is the mark and
 * |w| is the value every stage's max(w, 1) reads. */
layout(constant_id = 6) const int OP_SRC_TINT = 0;

/* The true view-Z a vertex stage reads from its rhi_vertex.w input. */
float op_src_tint_w(float w) { return OP_SRC_TINT != 0 ? abs(w) : w; }

/* 1 for a vertex of a walked terrain face, else 0 (flat varying). */
float op_src_tint_walked(float w) { return (OP_SRC_TINT != 0 && w < 0.0) ? 1.0 : 0.0; }

/* c mixed toward `to` by the instrument's blend; c when the tint is off. */
vec4 op_src_tint_mix(vec4 c, vec3 to)
{
    if (OP_SRC_TINT == 0) return c;
    return vec4(mix(c.rgb, to, float(OP_SRC_TINT) / 1000.0), c.a);
}

/* A walked-scene fragment: red when its face is walked terrain, else c. */
vec4 op_src_tint_scene(vec4 c, float walked)
{
    return walked > 0.5 ? op_src_tint_mix(c, vec3(1.0, 0.0, 0.0)) : c;
}

/* A retained terrain fragment: green. */
vec4 op_src_tint_cache(vec4 c) { return op_src_tint_mix(c, vec3(0.0, 1.0, 0.0)); }

#endif /* OPENPETE_PSX_SRC_TINT_GLSL */
