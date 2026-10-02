#version 450

/* Freeze-frame green overlay (pause / inventory / flight-results backdrop).
 *
 * The game freezes the rendered frame, desaturates it to grayscale (func_80017E98
 * -> a 32-entry grayscale CLUT) and re-displays it tinted green via a full-screen
 * POLY_FT4 with setRGB0(0x4C,0x80,0x40). The native renderer reproduces that effect
 * from ITS OWN last world frame instead of PsyCross's VRAM: it samples a snapshot
 * render-target (frozen_tex), desaturates with the game's exact luma, and applies
 * the same green tint - so the green overlay shows the native HQ render (and works
 * in the end-state where PsyCross's VRAM is gone).
 *
 * Pairs with psx_sample.vert (passthrough pos + UV); the green tint rides the
 * per-vertex colour (vColor = the FT4's setRGB0). */

layout(set = 0, binding = 0) uniform sampler2D u_tex;   /* frozen native frame */

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor; /* rgb = green tint (setRGB0 0x4C,0x80,0x40);
                                      * a   = fade-to-black shift (0 = none) */

layout(location = 0) out vec4 outColor;

/* func_80017E98 (asm: games/spyro-1/spyro-1/asm/images.s) - the EXACT desaturation:
 * per RGB555 pixel the 8-bit grayscale CLUT index = (R*4 + G*3 + B) >> 3, R,G,B in
 * 0..31. Normalized luma weights = (4,3,1)/8 = (0.5, 0.375, 0.125). The 32-entry CLUT
 * (g_GrayscalePalette, math.data.s) is a near-linear FULL-range gray ramp (index i ->
 * (i,i,i)), so the grey value IS this luma directly - no extra range scaling, no
 * fudge.
 * Continuous here rather than 5-bit-in / 32-level-out quantized (HQ). */
const vec3 PSX_GRAY = vec3(4.0, 3.0, 1.0) / 8.0;

void main()
{
    float lum = dot(texture(u_tex, vUV).rgb, PSX_GRAY);
    /* Fade-to-black (GS_ExitLevel case 0). The game shrinks the grayscale CLUT load
     * each tick (setRECT width 32-tick at x 512+tick), so a frozen pixel of luma-index
     * i samples palette[i-tick] - a SUBTRACTIVE luma shift, not a multiply (darks reach
     * black first, then mids, then highlights). vColor.a carries shift = tick/31 (0 for
     * the static pause/inventory backdrop). Continuous here, like the luma above. */
    lum = max(0.0, lum - vColor.a);
    /* PSX texture modulation: grey * colour / 128 (colour 0x80 == 1.0x), the same
     * (255/128) factor psx_scene.frag uses. The green tint rides the vertex colour. */
    vec3 rgb = clamp(vec3(lum) * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, 1.0);
}
