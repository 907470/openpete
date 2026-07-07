#version 450

/* Textured depth-aware shadow decal — the MOBY drop-shadows.
 *
 * Combines the PSX VRAM sprite decode (psx_scene.frag) with the depth-aware
 * occlusion of shadow_decal.frag — the same fix Spyro's silhouette shadow got.
 * Draws depth-test OFF; per pixel it reconstructs the opaque terrain's world
 * height from the shared WORLD-opaque depth pre-pass and rejects the occluder
 * classes a flat fan can't separate with a plain depth test/bias:
 *   (1) HEIGHT — terrain standing more than wall_height ABOVE the shadow's
 *       ground plane (a wall/ledge); bumps/ramps/pits near ground stay shadowed.
 *   (2) STEEPNESS (opt-in) — a wall FACE by surface orientation (small up-normal).
 *   (3) RAISED-IN-FRONT — a curb/platform nearer than the fan yet above its plane.
 * This stops the flat textured fan clipping through slanted floors (the old
 * depth-test path Z-fought the slope) and painting up wall bases.
 *
 * Ground plane: unlike the single Spyro shadow (one plane fed as a push
 * constant), many mobys sit at many heights, so the plane is recovered PER PIXEL
 * from the fan's OWN gl_FragCoord.z — the fan sits on the ground it is cast on,
 * so its interpolated depth reconstructs the local ground world-height, which is
 * then differenced against the terrain (both through the same camera inverse, so
 * systematic reconstruction error cancels). gl_FragCoord.z is the NDC depth,
 * viewport-independent, so full-extent / panel / SSAA paths agree.
 *
 * NB: the pipe blends SUBTRACTIVE (dst = B - F), so darker sprite texels darken
 * the ground more; the modulated grey is the shadow. */

layout(set = 0, binding = 0) uniform sampler2D  uOpaqueDepth; /* WORLD-opaque pre-pass depth */
layout(set = 1, binding = 0) uniform usampler2D u_vram;       /* R16_UINT 1024x512 VRAM      */

layout(push_constant) uniform PC {
    float col0x, col0y, col0z;   /* camera rows 4096^2 * C^-1 (world reconstruct) */
    float col1x, col1y, col1z;
    float col2x, col2y, col2z;
    float cam_pos_up;            /* camera world HEIGHT (the game z field) */
    float ofx, ofy, h;           /* GTE projection regs */
    float wall_height;           /* height slack above ground before "wall" */
    float steep_nup;             /* |up normal| below which a surface is a wall face */
    float mode;                  /* 0 = draw all (band), 1 = height+steep test */
    float band;                  /* fallback view-Z band (mode 0) */
    float plat_eps;              /* raised-occluder-in-front: min rise above the plane */
    float plat_slack;            /* raised-occluder-in-front: view-Z slack */
    float uax, ubx, vay, vby;    /* PSX screen -> pre-pass uv affine (u=uax*SX+ubx) */
    float pvw, pvh;              /* pre-pass raster extent within the depth image */
    int   tpage_x, tpage_y;      /* PSX texture page origin (VRAM 16bpp-pixel units) */
    int   clut_x,  clut_y;       /* CLUT origin (paletted only) */
    int   bpp;                   /* 4, 8, or 16 */
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) noperspective in vec2 vScreenPSX;

layout(location = 0) out vec4 outColor;

/* ---- PSX VRAM decode (psx_scene.frag) ------------------------------------- */
uint  vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }
float expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }
vec4  decode555(uint p)
{
    if (p == 0u) return vec4(0.0);                /* PSX transparent sentinel */
    return vec4(expand5( p        & 31u),
                expand5((p >> 5)  & 31u),
                expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}
uint sample_word(int u, int v)
{
    if (pc.bpp == 16) {
        return vram_at(pc.tpage_x + u, pc.tpage_y + v);
    } else if (pc.bpp == 8) {
        uint word = vram_at(pc.tpage_x + (u >> 1), pc.tpage_y + v);
        uint idx  = (word >> (8 * (u & 1))) & 0xFFu;
        return vram_at(pc.clut_x + int(idx), pc.clut_y);
    } else { /* 4bpp */
        uint word = vram_at(pc.tpage_x + (u >> 2), pc.tpage_y + v);
        uint nib  = (word >> (4 * (u & 3))) & 0xFu;
        return vram_at(pc.clut_x + int(nib), pc.clut_y);
    }
}

/* ---- world reconstruction (shadow_decal.frag) ----------------------------- */
/* canonical scene curve (the FAN's own gl_FragCoord.z): z = vz/(vz+4096) */
float viewz_from_scene_depth(float d)   { return 4096.0 * d / max(1.0 - d, 1e-6); }
/* pre-pass curve (prepass.vert): z = 1 - 1/vz — TRUE perspective (planar-exact). */
float viewz_from_prepass_depth(float d) { return 1.0 / max(1.0 - d, 1e-9); }

vec2 screen_of_texel(ivec2 t)
{
    return vec2(((float(t.x) + 0.5) / pc.pvw - pc.ubx) / pc.uax,
                ((float(t.y) + 0.5) / pc.pvh - pc.vby) / pc.vay);
}
/* camera-relative world position at a pre-pass texel (constant camera-position
 * terms omitted — used only differentially, for the surface normal). */
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
/* world vertical (the game z field, = V1 slot of the swizzle) at a screen point
 * given its view-Z. cam_pos_up - V1 is the absolute world height. */
float worldup_at(vec2 s, float vz)
{
    float M1 = (s.x - pc.ofx) * vz / pc.h;
    float M2 = (s.y - pc.ofy) * vz / pc.h;
    float M3 = vz;
    float V1 = (pc.col1x * M1 + pc.col1y * M2 + pc.col1z * M3) / 4096.0;
    return pc.cam_pos_up - V1;
}

void main()
{
    /* Sprite texel: gates the soft fan shape (transparent sentinel -> discard)
     * and carries the grey the subtract blend removes from the ground. */
    vec4 c = decode555(sample_word(int(vUV.x), int(vUV.y)));
    if (c.a == 0.0) discard;

    /* Depth-aware occlusion — mirror shadow_decal.frag. */
    vec2  uv = vec2(pc.uax * vScreenPSX.x + pc.ubx, pc.vay * vScreenPSX.y + pc.vby);
    ivec2 hi = ivec2(int(pc.pvw) - 1, int(pc.pvh) - 1);
    ivec2 tx = min(ivec2(clamp(uv, vec2(0.0), vec2(1.0)) * vec2(pc.pvw, pc.pvh)), hi);
    float viewZ   = viewz_from_prepass_depth(texelFetch(uOpaqueDepth, tx, 0).r); /* terrain */
    float fanViewZ = viewz_from_scene_depth(gl_FragCoord.z);                     /* the fan */

    /* Terrain height above the shadow's own ground plane (the fan sits on it). */
    float worldUpTerrain = worldup_at(vScreenPSX, viewZ);
    float worldUpGround  = worldup_at(vScreenPSX, fanViewZ);
    float height = worldUpTerrain - worldUpGround;

    /* terrain surface normal from neighbouring pre-pass texels (world-position
     * differences; camera-position constants cancel). */
    vec3  P0 = world_of_texel(tx);
    vec3  Px = world_of_texel(min(tx + ivec2(1, 0), hi));
    vec3  Py = world_of_texel(min(tx + ivec2(0, 1), hi));
    vec3  N  = cross(Px - P0, Py - P0);
    float nlen = length(N);
    float nup  = (nlen > 1e-6) ? abs(N.z) / nlen : 1.0;   /* vertical = P.z = -V1 */

    if (pc.mode > 0.5) {
        /* Raised occluder IN FRONT (curb / platform): terrain meaningfully nearer
         * than the fan AND meaningfully above the shadow plane -> the PSX painter
         * covered the fan with it. Receiving ground keeps the fan (bumps below
         * plat_eps; draped far slopes are farther, not nearer). */
        if (viewZ + pc.plat_slack < fanViewZ && height > pc.plat_eps) discard;
        if (pc.steep_nup > 0.0) {
            /* (1) wall FACE receiver: steep surface -> unconditional (the fan oval
             *     can poke THROUGH a wall, its near rim in front of the base).
             * (2) rising occluder, FLATNESS-SCALED: the flatter the receiver, the
             *     more rise above the plane is forgiven (chamfered wall skirts). */
            if (nup < pc.steep_nup) discard;
            float flat01 = clamp((nup - pc.steep_nup) / (0.9 - pc.steep_nup), 0.0, 1.0);
            if (height > pc.wall_height * flat01) discard;
        } else {
            /* HEIGHT-ONLY: terrain more than wall_height above the plane is a
             * wall/ledge -> discard; at-or-below (bumps, slopes, pits) keeps it. */
            if (height > pc.wall_height) discard;
        }
    } else {                                   /* fallback: plain view-Z band */
        if (viewZ + pc.band < fanViewZ) discard;
    }

    /* PSX texture modulation (texel * vertexColour / 128) -> subtract blend. */
    vec3 rgb = clamp(c.rgb * vColor.rgb * (255.0 / 128.0), 0.0, 1.0);
    outColor = vec4(rgb, c.a);
}
