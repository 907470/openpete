#version 450

/* Textured shadow-decal vertex stage — the MOBY drop-shadows.
 *
 * Perspective-correct sprite UV/colour (identical to psx_scene.vert: recover the
 * homogeneous W from the NDC depth and re-homogenise), PLUS the PSX screen coord
 * carried as a NOPERSPECTIVE varying for the fragment stage's depth pre-pass
 * lookup + terrain reconstruction (the same trick the untextured Spyro decal
 * shadow_decal.frag uses). The Spyro decal parks the screen coord in u,v because
 * it is untextured; the moby shadow is a textured sprite, so u,v carry the TEXEL
 * coord and the screen coord rides its own attribute (location 3). */

layout(location = 0) in vec3 inPos;       /* xy = clip-space screen, z = NDC depth */
layout(location = 1) in vec2 inUV;        /* sprite texel coords within the page   */
layout(location = 2) in vec4 inColor;     /* R8G8B8A8 -> 0..1 modulate             */
layout(location = 3) in vec2 inScreenPSX; /* PSX screen coord (SX,SY)              */
layout(location = 4) in float inW;        /* true view-Z when z carries an OT shift
                                           * (0 = unshifted; see psx_scene.vert)   */

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) noperspective out vec2 vScreenPSX;

void main()
{
    float z   = clamp(inPos.z, 0.0, 0.9999);         /* bound W (z->1 => W->inf) */
    float vzo = 4096.0 * z / max(1.0 - z, 1e-6);      /* view-Z the DEPTH encodes */
    vzo = max(vzo, 1.0);                               /* near-plane / z==0 guard  */
    float W = inW > 0.0 ? max(inW, 1.0) : vzo;        /* perspective W (true vz)  */
    gl_Position = vec4(inPos.xy * W, z * W, W);       /* window depth = z, as fed */
    vUV = inUV;
    vColor = inColor;
    vScreenPSX = inScreenPSX;
}
