#version 450

/* Depth-aware shadow decal (UNTEXTURED) — the Spyro silhouette shadow.
 *
 * Keeps the FLAT fan's look unchanged; only the keep/discard decision changes.
 * The flat fan sits behind the bumpy near-terrain it's cast on, so a pure depth
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
 *  (2) STEEPNESS (gated OFF by steep_ny=0 until W2.3) — the notch pixels
 *      differ from own-ground by surface ORIENTATION: walkable ground has
 *      |world normal.y| >> 0, a wall face ≈ 0. The normal comes from
 *      neighbouring pre-pass texels (world-position differences — camera
 *      position constants cancel). Correct for a self-consistent depth field,
 *      but the TERRAIN feeder's fed view-Z is skewed off-anchor relative to
 *      the camera model (W2.3's cross-class depth skew, first measured here),
 *      which garbles reconstructed normals — so the clause ships disabled.
 *
 * Reconstruction (the inverse of the game's GTE path, swizzle V=(-dy,-dz,dx)):
 *   viewZ from the pre-pass depth;  MAC1=(SX-ofx)*viewZ/h;  MAC2=(SY-ofy)*viewZ/h
 *   V_j = (pushed_colJ · MAC) / 4096 where pushed_colJ = 4096² · C⁻¹[j] — a TRUE
 *   numerical inverse: the Spyro camera matrix is NOT orthonormal (its Y row
 *   bakes a ~0.625 PSX aspect scale), so the orthonormal transpose-inverse
 *   silently under-weights MAC2 terms 2.56×.
 *   worldY = cam_pos_y - V0;  world P (camera-relative) = (V2, -V0, -V1)
 *
 * The pre-pass depth is SHARED across every consumer pass (native_rtt / SSAA /
 * offscreen panel / screenshots): it is rasterized ONCE per present in the
 * FULL-EXTENT PSX screen mapping, and looked up here by the PSX screen coord
 * (vScreenPSX, carried per-vertex in u,v and interpolated SCREEN-LINEAR /
 * noperspective) through the SAME affine the pre-pass rasterized with (uax..vby
 * push constants). No gl_FragCoord use at all, so full-extent (screenshot),
 * panel (live window), and SSAA paths run identical math by construction. The
 * normal uses EXPLICIT neighbour texels (not dFdx/dFdy), so it is identical in
 * every pass and immune to same-texel quad derivatives at high render res.
 *
 * NB (viz mode 2): the pipe blends SUBTRACTIVE, so on screen the viz colours
 * are subtracted from the framebuffer — read them with a diff tool, not eyes. */

layout(set = 0, binding = 0) uniform sampler2D uOpaqueDepth; /* WORLD-opaque pre-pass depth */

layout(push_constant) uniform PC {
    float col0x, col0y, col0z;   /* camera rotation matrix columns (Q12 int16) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_y;
    float ofx, ofy, h;           /* GTE projection regs */
    float ground_y;              /* the shadow's ground plane (SpyroShadow world Y) */
    float wall_height;           /* world-Y slack above ground before "wall" */
    float steep_ny;              /* |normal.y| below which a surface is a wall face */
    float mode;                  /* 0 = draw all, 1 = height+steep test, 2 = viz */
    float band;                  /* fallback view-Z band (mode 0/invalid params) */
    float uax, ubx, vay, vby;    /* PSX screen -> pre-pass uv affine (u=uax*SX+ubx) */
    float pvw, pvh;              /* pre-pass raster extent within the depth image */
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) noperspective in vec2 vScreenPSX;  /* PSX screen coord (SX,SY) */

layout(location = 0) out vec4 outColor;

/* canonical scene curve (the FAN's own gl_FragCoord.z): z = vz/(vz+4096) */
float viewz_from_scene_depth(float d) { return 4096.0 * d / max(1.0 - d, 1e-6); }
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

void main()
{
    vec2 uv = vec2(pc.uax * vScreenPSX.x + pc.ubx,
                   pc.vay * vScreenPSX.y + pc.vby);
    ivec2 hi = ivec2(int(pc.pvw) - 1, int(pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(pc.pvw, pc.pvh)), hi);
    float dT = texelFetch(uOpaqueDepth, tx, 0).r;
    float viewZ = viewz_from_prepass_depth(dT);
    float dS = gl_FragCoord.z;

    /* reconstruct the terrain point's world-Y at this pixel */
    float SX   = vScreenPSX.x;
    float SY   = vScreenPSX.y;
    float MAC1 = (SX - pc.ofx) * viewZ / pc.h;
    float MAC2 = (SY - pc.ofy) * viewZ / pc.h;
    float MAC3 = viewZ;
    float V0   = (pc.col0x * MAC1 + pc.col0y * MAC2 + pc.col0z * MAC3) / 4096.0;
    float worldY = pc.cam_pos_y - V0;
    float height = worldY - pc.ground_y;      /* terrain height above the shadow's ground */

    /* terrain surface normal from neighbouring pre-pass texels */
    vec3 P0 = world_of_texel(tx);
    vec3 Px = world_of_texel(min(tx + ivec2(1, 0), hi));
    vec3 Py = world_of_texel(min(tx + ivec2(0, 1), hi));
    vec3 N  = cross(Px - P0, Py - P0);
    float nlen = length(N);
    float ny = (nlen > 1e-6) ? abs(N.y) / nlen : 1.0;  /* degenerate -> "flat" (keep) */

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
        vec2 rt = screen_of_texel(tx) - vScreenPSX;   /* expect |.| < ~1 PSX px */
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
                        0.4 * ny, 1.0);
        return;
    }
    if (pc.mode > 1.5) {                       /* CALIBRATION VIZ (machine-read only) */
        /* Small-amplitude channels so the SUBTRACTIVE blend rarely clamps at 0
         * on dark surfaces: recover as (terrain_px - viz_px) / 0.4 per channel.
         * r = ny, g = height above plane / 1024, b = height below plane / 1024. */
        outColor = vec4(0.4 * ny,
                        0.4 * clamp(height / 1024.0, 0.0, 1.0),
                        0.4 * clamp(-height / 1024.0, 0.0, 1.0), 1.0);
        return;
    }
    if (pc.mode > 0.5) {
        if (pc.steep_ny > 0.0) {
            /* ORIENTATION-AWARE tests — DISABLED BY DEFAULT (steep_ny=0) until
             * W2.3 unifies feeder depth: the normal reconstruction is exact for
             * a self-consistent depth field (verified synthetically and against
             * the shadow anchor), but the TERRAIN feeder's fed view-Z is skewed
             * off-anchor relative to the camera model (the depth audit's
             * "suspected cross-class depth skew", measured here for the first
             * time), so reconstructed normals read flat floors as steep and
             * over-discard. Re-enable via OPENPETE_SHADOW_STEEP_NY once W2.3
             * lands; it retires the wall-base-notch carve-out below.
             * (1) wall face receiver: steep surface — unconditional (the fan
             *     oval can poke THROUGH a wall, putting its near rim in front
             *     of the wall base, so no depth guard);
             * (2) rising occluder, FLATNESS-SCALED: the flatter the receiver,
             *     the more rise above the shadow plane is forgiven — catches
             *     chamfered wall SKIRTS (ny~0.5 rising 200-340). */
            if (ny < pc.steep_ny) discard;
            float flat01 = clamp((ny - pc.steep_ny) / (0.9 - pc.steep_ny), 0.0, 1.0);
            if (height > pc.wall_height * flat01) discard;
        } else {
            /* HEIGHT-ONLY (shipping default): terrain standing more than
             * wall_height above the shadow's ground plane is a wall/ledge —
             * discard; at-or-below (bumps, slopes, pits) keeps the fan.
             * CARVE-OUT: the bottom wall_height units of a wall face rise from
             * ground level and pass this test (a small dark notch where the
             * fan meets a wall base — dark-hollow f2700); killing it needs the
             * orientation tests above, blocked on W2.3. */
            if (height > pc.wall_height) discard;
        }
    } else {                                   /* fallback: view-Z band (params invalid) */
        if (viewZ + pc.band < viewz_from_scene_depth(dS)) discard;
    }
    outColor = vColor;
}
