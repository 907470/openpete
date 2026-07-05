#version 450

/* Far-band seam INPAINT (native-renderer plan, distant-water dash class).
 *
 * The native float raster sits +0.2..+1.0px off the PSX truncating GTE with
 * per-row variance, so the far-band terrain (the fogged water/horizon rows)
 * opens ~1-2px-tall horizontal DASHES where the sky/cyclorama shows through.
 * Geometry-copy fills (grown LP re-emission, v2-v4) were structurally blind —
 * stencil==0 alone cannot tell a seam pixel INSIDE terrain from a sky pixel
 * ABOVE a silhouette, so growth ghosted above ridgelines and per-present
 * quantization crawled under the sub-tick spine.
 *
 * This pass fills IN IMAGE SPACE with a structural test instead: a pixel is a
 * seam iff no WORLD geometry covered it (hardware stencil TEST_ZERO confines
 * the pass to those) AND far-band WORLD terrain covers BOTH sides of it along
 * an axis within `reach` texels — terrain above AND below a horizontal dash
 * (or left AND right of a vertical sliver). Sky above a ridgeline fails by
 * construction: its upward neighbour is sky. Coverage and depth come from the
 * WORLD-opaque depth PRE-PASS (the shadow decal's, prepass.vert z = 1-1/vz);
 * the fill COLOUR is the pre-pass COLOUR attachment — the same gouraud world
 * the WORLD group rasterized, distance-weighted between the two sides — so the
 * fill follows the rendered image and is temporally stable by construction.
 *
 * Drawn as one band-rect quad (quad.vert; u,v carry the PSX screen coord →
 * vScreenPSX) right after the WORLD(+WORLD_FILL) group, before anything
 * that draws over terrain; depth-test/write OFF (actors drawn later cover it
 * normally). Both prepass aspects are sampled through the recorded affine, so
 * one raster serves the offscreen, SSAA, native_rtt and screenshot contexts. */

layout(set = 0, binding = 0) uniform sampler2D uOpaqueDepth;  /* pre-pass depth (1 - 1/vz) */
layout(set = 1, binding = 0) uniform sampler2D uOpaqueColor;  /* pre-pass gouraud colour   */

layout(push_constant) uniform PC {
    float uax, ubx, vay, vby;   /* PSX screen -> pre-pass uv affine (u=uax*SX+ubx) */
    float pvw, pvh;             /* pre-pass raster extent within the image */
    float vz_lo;                /* neighbour "far-band" gate: view-Z >= vz_lo */
    float vz_hi;                /* covered gate: view-Z < vz_hi (excludes the
                                 * pre-pass far BACKSTOP quad at vz ~4.09e6) */
    float reach;                /* max texel distance searched per side */
    float simfrac;              /* same-surface gate: |vzA-vzB| <= simfrac*max */
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) noperspective in vec2 vScreenPSX;

layout(location = 0) out vec4 outColor;

/* pre-pass curve (prepass.vert): z = 1 - 1/vz */
float viewz_at(ivec2 t) { return 1.0 / max(1.0 - texelFetch(uOpaqueDepth, t, 0).r, 1e-9); }

bool covered_far(ivec2 t)
{
    float vz = viewz_at(t);
    return vz >= pc.vz_lo && vz < pc.vz_hi;
}

/* Nearest covered far-band texel from `tx` along `step`, within pc.reach.
 * Returns the distance (1-based) or 0 if none. */
int side_hit(ivec2 tx, ivec2 step, ivec2 hi)
{
    for (int d = 1; d <= 4; d++) {
        if (d > int(pc.reach)) break;
        ivec2 t = clamp(tx + step * d, ivec2(0), hi);
        if (covered_far(t)) return d;
    }
    return 0;
}

void main()
{
    /* Debug (OPENPETE_TERRAIN_INPAINT_REACH=99): paint the whole stencil-0
     * band magenta — separates pass plumbing from the neighbour logic. */
    if (pc.reach > 90.0) { outColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    vec2 uv = vec2(pc.uax * vScreenPSX.x + pc.ubx,
                   pc.vay * vScreenPSX.y + pc.vby);
    /* Debug (REACH=89): visualize the CENTER texel's own pre-pass state —
     * red = raw depth (grayscale-ish), green flag = covered_far. */
    if (pc.reach > 80.0) {
        ivec2 hi_ = ivec2(int(pc.pvw) - 1, int(pc.pvh) - 1);
        ivec2 tx_ = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(pc.pvw, pc.pvh)), hi_);
        float d = texelFetch(uOpaqueDepth, tx_, 0).r;
        float vz = 1.0 / max(1.0 - d, 1e-9);
        outColor = vec4(covered_far(tx_) ? 0.0 : 1.0,
                        covered_far(tx_) ? 1.0 : 0.0,
                        clamp(vz / 100000.0, 0.0, 1.0), 1.0);
        return;
    }
    ivec2 hi = ivec2(int(pc.pvw) - 1, int(pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(pc.pvw, pc.pvh)), hi);

    /* Both axes, each under the SAME-SURFACE gate. HOW THE WATER DASHES
     * ACTUALLY FILL (measured, not the naive picture): the pre-pass raster's
     * row phase differs slightly from the main raster's (full-extent vs panel
     * mapping), so at a main-raster dash pixel the pre-pass usually has the
     * SAME water row covered at the pixel's own row — the HORIZONTAL d=1
     * anchors carry the fill (vertical anchors often land in the pre-pass's
     * own gap rows and miss). Vertical handles mountain-interior seams (rows
     * tall enough that ±1..reach clears both rasters' gaps).
     * SAME-SURFACE gate per axis: a raster seam separates two samples of ONE
     * far surface (depths within a fraction of a percent); a silhouette
     * notch is bounded by DIFFERENT surfaces (cliff-vs-plain at tick 4888,
     * factors apart). NOTE the threshold must be generous (default 0.5):
     * at the horizon, consecutive 1px rows span 12-50% in view-Z (the 1/y
     * divergence) — a tight 12% gate silently rejected every water fill. */
    ivec2 axes[2] = ivec2[2](ivec2(0, 1), ivec2(1, 0));
    for (int a = 0; a < 2; a++) {
        int dn = side_hit(tx, -axes[a], hi);
        if (dn == 0) continue;
        int dp = side_hit(tx, axes[a], hi);
        if (dp == 0) continue;
        ivec2 tn = clamp(tx - axes[a] * dn, ivec2(0), hi);
        ivec2 tp = clamp(tx + axes[a] * dp, ivec2(0), hi);
        float vzn = viewz_at(tn), vzp = viewz_at(tp);
        if (abs(vzn - vzp) > pc.simfrac * max(vzn, vzp)) continue;   /* cross-surface */
        vec3 cn = texelFetch(uOpaqueColor, tn, 0).rgb;
        vec3 cp = texelFetch(uOpaqueColor, tp, 0).rgb;
        /* Distance-weighted: nearer side dominates (dn==dp -> average). */
        outColor = vec4(mix(cn, cp, float(dn) / float(dn + dp)), 1.0);
        return;
    }
    discard;   /* not a bounded same-surface seam (open sky / cross-surface) */
}
