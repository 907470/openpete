#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#include "psx_viewz.glsl"

/* Depth-aware shadow decal (UNTEXTURED) — the Spyro silhouette shadow.
 *
 * On the fan's own surface (term A below) the flat fan's look is unchanged;
 * the depth rule decides keep or discard. The flat fan sits behind the bumpy near-terrain it's cast on, so a pure depth
 * test/band can't separate "my own ground" (keep) from "a wall in front"
 * (discard) — both put terrain nearer than the fan, by the same view-Z. Instead
 * the shader reconstructs the opaque terrain per pixel from the depth pre-pass
 * and rejects two occluder classes:
 *
 *  (1) HEIGHT (shipping default) — terrain standing more than wall_height
 *      ABOVE the shadow's ground plane (a tall wall/ledge). Bumps/ramps/pits
 *      near ground height stay shadowed. (Validated: reconstructing the shadow
 *      anchor's world position from its own screen+depth recovers SpyroShadow
 *      exactly.) Carve-out: the bottom wall_height units of a wall face pass
 *      this test (small dark notch at a wall base).
 *  (2) STEEPNESS (opt-in via steep_nup) — the notch pixels differ from
 *      own-ground by surface ORIENTATION: walkable ground has a large up
 *      normal, a wall face ≈ 0. The normal comes from neighbouring pre-pass
 *      texels (world-position differences — camera position constants
 *      cancel). The old "terrain view-Z skew" blocker was DISPROVEN
 *      2026-07-02 (axis misread in the measuring harness — Spyro's vertical
 *      is the z field/V1, not y/V0); fed terrain floors reconstruct flat to
 *      the unit, so the clause is sound.
 *
 * Reconstruction (the inverse of the game's GTE path, swizzle V=(-dy,-dz,dx)):
 *   viewZ from the pre-pass depth;  MAC1=(SX-ofx)*viewZ/h;  MAC2=(SY-ofy)*viewZ/h
 *   V_j = (pushed_colJ · MAC) / 4096 where pushed_colJ = 4096² · C⁻¹[j] — a TRUE
 *   numerical inverse: the Spyro camera matrix is NOT orthonormal (its Y row
 *   bakes a ~0.625 PSX aspect scale), so the orthonormal transpose-inverse
 *   silently under-weights MAC2 terms 2.56×.
 *   worldUp = cam_pos_up - V1 (z field = vertical);  P = (V2, -V0, -V1)
 *
 * The pre-pass depth is SHARED across every consumer pass (native_rtt / SSAA /
 * offscreen panel / screenshots): it is rasterized ONCE per present in the
 * full-extent PSX screen mapping, and looked up here through the same affine it
 * rasterized with (uax..vby push constants).
 *
 * The lookup's input is the fragment's OWN PSX screen position, recovered from
 * gl_FragCoord through this pass's inverse affine (fax..fby, from
 * scene_fragcoord_affine). Carrying that position as an interpolated vertex
 * attribute instead read the wrong texel wherever the rasterizer's
 * interpolation of the attribute disagreed with the fragment's own position —
 * on a triangle clipped to a thin sliver, by 1-2 PSX pixels — and the decal
 * then took a neighbouring surface's normal, so the steep test discarded a
 * 2-3 px band along the clipped edge and left a pale line across the shadow.
 * Both affines are exact and the viewport is the whole pass, so full-extent
 * (screenshot), panel (live window) and SSAA paths agree. The normal uses
 * EXPLICIT neighbour texels (not dFdx/dFdy), so it is identical in every pass
 * and immune to same-texel quad derivatives at high render res.
 *
 * Footprint. The feed draws the screen convex hull of the ring's vertical
 * prism from the fan less a margin up to the caster height, and hands the fan
 * itself to this stage (the ShadowFan block). A pixel is shadowed when
 *  (A) the fan's own surface covers it and the depth rule keeps it: the fan is
 *      re-rasterized here from its fed screen points, with the ordering view-Z
 *      the scene pipe would have written and the gouraud weight it would have
 *      interpolated, so (A) is the fan draw of the plain depth rule; or
 *  (B) the opaque terrain at the pixel stands under the ring's vertical
 *      projection (its world x, y inside the ring), is not steep, lies no
 *      higher than the caster and no lower than the fan less the margin. The
 *      colour is the fan's gouraud at the fan point vertically over or under
 *      the receiver.
 * (A) takes the pixel when both hold: the fan's own surface is then the
 * visible receiver. OPENPETE_SHADOW_FOOTPRINT=0 zeroes the block's term flag
 * and leaves (A) alone.
 *
 * NB (viz mode 2): the pipe blends SUBTRACTIVE, so on screen the viz colours
 * are subtracted from the framebuffer — read them with a diff tool, not eyes.
 * The viz and census modes mark every hull fragment. */

layout(set = 0, binding = 0) uniform sampler2D uOpaqueDepth; /* WORLD-opaque pre-pass depth */
layout(set = 1, binding = 1) uniform sampler2D uTerrainKey;  /* terrain og-key image, this target */

/* The player fan (openpete_shadow_footprint): point 0 the anchor, 1..16 the
 * ring. scr = PSX screen x, y, ordering view-Z, perspective W as fed; wrel =
 * camera-relative world x, y, up through this stage's reconstruction. */
layout(std140, set = 2, binding = 0) uniform ShadowFan {
    vec4 hdr;          /* caster world height, footprint on, triangle mask, margin below */
    vec4 anchor_rgb;   /* the gouraud at the anchor; the rim is 0 */
    vec4 scr[17];
    vec4 wrel[17];
} uf;

layout(push_constant) uniform PC {
    float col0x, col0y, col0z;   /* camera rotation matrix columns (Q12 int16) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;            /* camera world HEIGHT (the game z field) */
    float ofx, ofy, h;           /* GTE projection regs */
    float ground_up;             /* the shadow's ground plane height (SpyroShadow z) */
    float wall_height;           /* height slack above ground before "wall" */
    float steep_nup;             /* |up normal| below which a surface is a wall face */
    float mode;                  /* 0 = draw all, 1 = height+steep test, 2 = viz */
    float band;                  /* fallback view-Z band (mode 0/invalid params) */
    float plat_eps;              /* min rise above the shadow plane for a raised
                                  * in-front surface to occlude (platform/curb) */
    float plat_slack;            /* view-Z slack for "meaningfully in front" */
    float key_contest;           /* 1 = uTerrainKey holds this target's keys; unread */
    float uax, ubx, vay, vby;    /* PSX screen -> pre-pass uv affine (u=uax*SX+ubx) */
    float pvw, pvh;              /* pre-pass raster extent within the depth image */
    float fax, fbx, fay, fby;    /* gl_FragCoord -> PSX screen affine, this pass */
} pc;

layout(location = 0) in vec4 vColor;  /* alpha x vQ: 1 on the fan, 0 on the hull */
layout(location = 1) in vec2 vUV;
layout(location = 3) in float vQ;
layout(location = 4) flat in float vKoff;

layout(location = 0) out vec4 outColor;

/* Scene depth convention of the FAN's own gl_FragCoord.z (RHI spec constant
 * 0, written by quad.vert through op_scene_clip): reversed z = 1/vz, forward
 * z = 1 - 1/vz. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
/* --texture-mapping affine: the scene pipe interpolates the gouraud screen-
 * linearly (quad.vert), so the fan's re-raster does the same. */
layout(constant_id = 1) const bool AFFINE_UV = false;
/* pre-pass curve (prepass.vert): z = 1 - 1/vz — TRUE perspective, so the
 * rasterizer's screen-linear interpolation is exact on planes and the
 * neighbour-texel normal reconstruction below is trustworthy. */
float viewz_from_prepass_depth(float d) { return 1.0 / max(1.0 - d, 1e-9); }

/* PSX screen coord of a pre-pass texel centre (the uv affine, inverted). */
vec2 screen_of_texel(ivec2 t)
{
    return vec2(((float(t.x) + 0.5) / pc.pvw - pc.ubx) / pc.uax,
                ((float(t.y) + 0.5) / pc.pvh - pc.vby) / pc.vay);
}

/* Camera-relative world position of the opaque surface at a pre-pass texel
 * (constant camera-position terms omitted — used only differentially). */
vec3 world_of_texel(ivec2 t)
{
    float vz = viewz_from_prepass_depth(texelFetch(uOpaqueDepth, t, 0).r);
    vec2  s  = screen_of_texel(t);
    float M1 = (s.x - pc.ofx) * vz / pc.h;
    float M2 = (s.y - pc.ofy) * vz / pc.h;
    float M3 = vz;
    float V0 = (pc.col0x * M1 + pc.col0y * M2 + pc.col0z * M3) / 4096.0;
    float V1 = (pc.col1x * M1 + pc.col1y * M2 + pc.col1z * M3) / 4096.0;
    float V2 = (pc.col2x * M1 + pc.col2y * M2 + pc.col2z * M3) / 4096.0;
    return vec3(V2, -V0, -V1);
}

/* The fan's own surface at PSX screen point s: the first linked triangle
 * (anchor, ring k, ring k+1) whose fed screen triangle holds s. vz is the
 * view-Z the scene pipe's depth would carry there (1/vz screen-linear over the
 * ordering view-Z), wa the anchor's gouraud weight it would interpolate. */
bool fan_hit(vec2 s, out float vz, out float wa)
{
    int mask = int(uf.hdr.z + 0.5);
    vec4 a = uf.scr[0];
    for (int k = 0; k < 16; k++) {
        if ((mask & (1 << k)) == 0) continue;
        vec4 b = uf.scr[1 + k], c = uf.scr[1 + ((k + 1) & 15)];
        float d = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (abs(d) < 1e-9) continue;
        float l1 = ((s.x - a.x) * (c.y - a.y) - (c.x - a.x) * (s.y - a.y)) / d;
        float l2 = ((b.x - a.x) * (s.y - a.y) - (s.x - a.x) * (b.y - a.y)) / d;
        float l0 = 1.0 - l1 - l2;
        if (min(l0, min(l1, l2)) < -1e-5) continue;
        vec3 z = vec3(op_scene_vz(a.z), op_scene_vz(b.z), op_scene_vz(c.z));
        vz = 1.0 / (l0 / z.x + l1 / z.y + l2 / z.z);
        vec3 w = AFFINE_UV ? vec3(1.0) : max(vec3(a.w, b.w, c.w), vec3(1.0));
        wa = (l0 / w.x) / (l0 / w.x + l1 / w.y + l2 / w.z);
        return true;
    }
    return false;
}

/* The ring's vertical projection at camera-relative world point p: the first
 * linked triangle whose world (x, y) holds p.xy. wa is the anchor's weight at
 * that (x, y), up the fan's camera-relative height there. */
bool foot_hit(vec3 p, out float wa, out float up)
{
    int mask = int(uf.hdr.z + 0.5);
    vec3 a = uf.wrel[0].xyz;
    for (int k = 0; k < 16; k++) {
        if ((mask & (1 << k)) == 0) continue;
        vec3 b = uf.wrel[1 + k].xyz, c = uf.wrel[1 + ((k + 1) & 15)].xyz;
        float d = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (abs(d) < 1e-6) continue;
        float l1 = ((p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y)) / d;
        float l2 = ((b.x - a.x) * (p.y - a.y) - (p.x - a.x) * (b.y - a.y)) / d;
        float l0 = 1.0 - l1 - l2;
        if (min(l0, min(l1, l2)) < -1e-5) continue;
        wa = l0;
        up = l0 * a.z + l1 * b.z + l2 * c.z;
        return true;
    }
    return false;
}

/* The depth rule on the fan's own surface (fanVz its view-Z at this pixel):
 * the raised occluder in front, then the steepness and wall-height tests. */
bool own_plane_keeps(float viewZ, float fanVz, float height, float nup)
{
    /* Raised occluder in front (curb / raised platform): terrain between
     * the camera and the fan covers it by depth. A flat raised path
     * passes both the steep test (top is flat) and the wall-height test
     * (dark-hollow platform: +368 < 400), yet must occlude. "In front" =
     * the pre-pass terrain is meaningfully nearer than the fan fragment
     * itself; "raised" = meaningfully above the shadow plane. The
     * receiving ground keeps the shadow: its bumps are below plat_eps,
     * and draped far slopes are farther, not nearer. */
    if (viewZ + pc.plat_slack < fanVz && height > pc.plat_eps) return false;
    if (pc.steep_nup > 0.0) {
        /* ORIENTATION-AWARE tests (the default; OPENPETE_SHADOW_STEEP_NY=0
         * selects height-only).
         * (1) wall face receiver: steep surface — unconditional (the fan
         *     oval can poke THROUGH a wall, putting its near rim in front
         *     of the wall base, so no depth guard);
         * (2) rising occluder, FLATNESS-SCALED: the flatter the receiver,
         *     the more rise above the shadow plane is forgiven — catches
         *     chamfered wall SKIRTS (nup~0.5 rising 200-340). */
        if (nup < pc.steep_nup) return false;
        float flat01 = clamp((nup - pc.steep_nup) / (0.9 - pc.steep_nup), 0.0, 1.0);
        return height <= pc.wall_height * flat01;
    }
    /* HEIGHT-ONLY: terrain standing more than wall_height above the
     * shadow's ground plane is a wall/ledge; at-or-below (bumps, slopes,
     * pits) keeps the fan. The bottom wall_height units of a wall face
     * pass this test (a small dark notch where the fan meets a wall base). */
    return height <= pc.wall_height;
}

/* Term (B): the terrain point p (camera-relative world, worldUp its absolute
 * height) under the ring, not steep, between the fan less the margin and the
 * caster. wa is the anchor's weight at the receiver's (x, y). */
bool footprint_keeps(float dT, vec3 p, float worldUp, float nup, out float wa)
{
    wa = 0.0;
    if (uf.hdr.y < 0.5 || dT >= 1.0) return false;
    if (pc.steep_nup > 0.0 && nup < pc.steep_nup) return false;
    if (worldUp > uf.hdr.x) return false;
    float up;
    if (!foot_hit(p, wa, up)) return false;
    return p.z >= up - uf.hdr.w;
}

void main()
{
    /* The feed carries the fan itself (vertex alpha 1) for the depth-tested
     * fallback pipe; this stage shades the prism hull (alpha 0) alone. */
    if (vColor.a > 0.5 * vQ) discard;
    /* This fragment's own PSX screen position, from its window coordinate
     * through the pass's inverse affine (exact; see scene_fragcoord_affine). */
    vec2 sOwn = vec2(pc.fax * gl_FragCoord.x + pc.fbx,
                     pc.fay * gl_FragCoord.y + pc.fby);
    vec2 uv = vec2(pc.uax * sOwn.x + pc.ubx,
                   pc.vay * sOwn.y + pc.vby);
    ivec2 hi = ivec2(int(pc.pvw) - 1, int(pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(pc.pvw, pc.pvh)), hi);
    float dT = texelFetch(uOpaqueDepth, tx, 0).r;
    float viewZ = viewz_from_prepass_depth(dT);

    /* Reconstruct the terrain point's world HEIGHT at this pixel. AXIS NOTE
     * (2026-07-02): Spyro's vertical world axis is the z FIELD, carried by V1
     * (swizzle V=(-dy,-dz,dx) => slot 1 = -dz). The original decal used V0
     * (world y — HORIZONTAL), a proximity test that rejected walls
     * incidentally and left the wall-base notch. */
    float SX   = sOwn.x;
    float SY   = sOwn.y;
    float MAC1 = (SX - pc.ofx) * viewZ / pc.h;
    float MAC2 = (SY - pc.ofy) * viewZ / pc.h;
    float MAC3 = viewZ;
    float V0   = (pc.col0x * MAC1 + pc.col0y * MAC2 + pc.col0z * MAC3) / 4096.0;
    float V1   = (pc.col1x * MAC1 + pc.col1y * MAC2 + pc.col1z * MAC3) / 4096.0;
    float V2   = (pc.col2x * MAC1 + pc.col2y * MAC2 + pc.col2z * MAC3) / 4096.0;
    float worldUp = pc.cam_pos_up - V1;
    float height  = worldUp - pc.ground_up;   /* terrain height above the shadow's ground */
    vec3  Tw      = vec3(V2, -V0, -V1);       /* camera-relative world point */

    /* terrain surface normal from neighbouring pre-pass texels */
    vec3 P0 = world_of_texel(tx);
    vec3 Px = world_of_texel(min(tx + ivec2(1, 0), hi));
    vec3 Py = world_of_texel(min(tx + ivec2(0, 1), hi));
    vec3 N  = cross(Px - P0, Py - P0);
    float nlen = length(N);
    /* Up-component of the surface normal: P carries the vertical in .z
     * (P = (V2, -V0, -V1), vertical = -V1). Degenerate -> "flat" (keep). */
    float nup = (nlen > 1e-6) ? abs(N.z) / nlen : 1.0;

    float fanVz = 0.0, wA = 0.0;
    bool onFan = fan_hit(sOwn, fanVz, wA);
    if ((pc.mode > 0.5 && pc.mode < 1.5) || pc.mode > 9.5) {
        /* the rule: 1 = shade, 10 = term marker, 11 = shade with the
         * footprint's colour taken from the fan at this screen pixel */
        bool keepA = onFan && own_plane_keeps(viewZ, fanVz, height, nup);
        float wB;
        bool keepB = footprint_keeps(dT, Tw, worldUp, nup, wB);
        if (pc.mode > 9.5 && pc.mode < 10.5) {
            /* TERM MARKER (machine-read only; subtractive): r = (A) keeps,
             * g = (B) keeps, b = every hull fragment, 0.4 on the fan's own
             * raster and 0.15 off it. */
            outColor = vec4(keepA ? 0.4 : 0.0, keepB ? 0.4 : 0.0, onFan ? 0.4 : 0.15, 1.0);
            return;
        }
        if (!keepA && !keepB) discard;
        float w = keepA ? wA : (pc.mode > 10.5 ? (onFan ? wA : 0.0) : wB);
        outColor = vec4(uf.anchor_rgb.rgb * w, 1.0);
        return;
    }
    if (pc.mode > 7.5) {                       /* lookup census (machine-read only) */
        /* Does the sampled pre-pass texel cover this fragment's own screen
         * position? The texel centre is within half a texel of it whenever the
         * lookup coordinate is the fragment's own, so >= 1 texel of error means
         * the decal read a neighbouring surface's depth and normal. g and
         * b mark every decal fragment, r only a fragment whose lookup missed;
         * mode 9 is the same marker with r forced off. Fragments that missed =
         * the r difference between the two legs; decal fragments = the
         * difference between the mode-9 leg and a stock capture. The pipe
         * blends subtractive, so each marker is read as a capture difference,
         * and neither count needs a per-game feature knob. */
        vec2 err = (screen_of_texel(tx) - sOwn) *
                   vec2(pc.uax * pc.pvw, pc.vay * pc.pvh);
        float miss = (pc.mode < 8.5 &&
                      any(greaterThanEqual(abs(err), vec2(1.0)))) ? 0.4 : 0.0;
        outColor = vec4(miss, 0.4, 0.4, 1.0);
        return;
    }
    if (pc.mode > 6.5) {                       /* RAW VIZ: vz + both neighbour dvz */
        float vzy = viewz_from_prepass_depth(texelFetch(uOpaqueDepth, min(tx + ivec2(0, 1), hi), 0).r);
        float vzx = viewz_from_prepass_depth(texelFetch(uOpaqueDepth, min(tx + ivec2(1, 0), hi), 0).r);
        outColor = vec4(0.4 * clamp(viewZ / 8192.0, 0.0, 1.0),
                        0.4 * clamp((vzy - viewZ) / 64.0 + 0.5, 0.0, 1.0),
                        0.4 * clamp((vzx - viewZ) / 64.0 + 0.5, 0.0, 1.0), 1.0);
        return;
    }
    if (pc.mode > 5.5) {                       /* Y-STEP VIZ: |Py-P0| components (world axes) */
        vec3 dy = Py - P0; float dl = length(dy);
        vec3 dn = (dl > 1e-6) ? abs(dy) / dl : vec3(0.0);
        outColor = vec4(0.4 * dn.x, 0.4 * dn.y, 0.4 * dn.z, 1.0);
        return;
    }
    if (pc.mode > 4.5) {                       /* NORMAL VIZ: |N| components (world axes) */
        vec3 nn = (nlen > 1e-6) ? abs(N) / nlen : vec3(0.0);
        outColor = vec4(0.4 * nn.x, 0.4 * nn.y, 0.4 * nn.z, 1.0);
        return;
    }
    if (pc.mode > 3.5) {                       /* ROUNDTRIP VIZ: screen_of_texel error */
        vec2 rt = screen_of_texel(tx) - sOwn;   /* expect |.| < ~1 PSX px */
        outColor = vec4(0.4 * clamp(abs(rt.x) / 4.0, 0.0, 1.0),
                        0.4 * clamp(abs(rt.y) / 4.0, 0.0, 1.0),
                        0.0, 1.0);
        return;
    }
    if (pc.mode > 2.5) {                       /* DELTA VIZ: neighbour depth deltas */
        float dzx = abs(texelFetch(uOpaqueDepth, min(tx + ivec2(1, 0), hi), 0).r - dT);
        float dzy = abs(texelFetch(uOpaqueDepth, min(tx + ivec2(0, 1), hi), 0).r - dT);
        outColor = vec4(0.4 * clamp(dzy * 100.0, 0.0, 1.0),
                        0.4 * clamp(dzx * 100.0, 0.0, 1.0),
                        0.4 * nup, 1.0);
        return;
    }
    if (pc.mode > 1.5) {                       /* CALIBRATION VIZ (machine-read only) */
        /* Small-amplitude channels so the SUBTRACTIVE blend rarely clamps at 0
         * on dark surfaces: recover as (terrain_px - viz_px) / 0.4 per channel.
         * r = nup, g = height above plane / 1024, b = height below plane / 1024. */
        outColor = vec4(0.4 * nup,
                        0.4 * clamp(height / 1024.0, 0.0, 1.0),
                        0.4 * clamp(-height / 1024.0, 0.0, 1.0), 1.0);
        return;
    }
    /* mode 0 (parameters invalid): the fan, less terrain nearer than it by
     * more than the fallback band */
    if (!onFan || viewZ + pc.band < fanVz) discard;
    outColor = vec4(uf.anchor_rgb.rgb * wA, 1.0);
}

