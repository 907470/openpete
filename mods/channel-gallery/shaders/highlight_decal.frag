/* The gallery's highlight, on the DECAL material profile.
 *
 * Moby drop-shadows draw through a pass whose stock fragment stage decides
 * per pixel whether the shadow is occluded — it reconstructs the terrain
 * height from a depth pre-pass and rejects walls, steep faces and raised
 * platforms. A material replaces the fragment stage, so it has to apply
 * that verdict rather than silently drop it; op_decal_visible() is the
 * engine handing it over.
 *
 * Everything else is the plain highlight: take the stock look, push it
 * toward the row's colour. The draw blends SUBTRACTIVE, so a brighter
 * tint darkens the ground MORE — the row reads inverted compared with the
 * opaque channels, which is the material inheriting its group's
 * compositing exactly as it should.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_decal.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 tint;     /* rgb = highlight colour, a = strength 0..1 */
};
layout(location = 0) out vec4 outColor;

void main()
{
    if (!op_decal_visible()) discard;
    vec4 base = op_decal_shade();
    if (base.a == 0.0) discard;          /* PSX transparent sentinel */
    outColor = vec4(mix(base.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0)), base.a);
}
