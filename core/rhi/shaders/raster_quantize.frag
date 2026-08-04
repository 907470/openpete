/* raster_quantize.frag — engine SCENE-point pass (`--raster-quantize N`):
 * resample the composed 3D scene on the 512Nx240N virtual PSX raster, taking
 * ONE POINT SAMPLE per cell with a DEPTH-VOTED choice of tap. The base grid
 * comes from op_scene_rect/op_scene_grid and N is an integer MULTIPLIER of it,
 * so cell boundaries always contain the integer lattice --vertex-precision
 * integer snaps vertices to, under any window size, aspect, or Hor+ widening.
 *
 * Point sampling is the authentic half: the PSX GPU rasterized exactly one
 * sample per output pixel and had no downsample filter, so box-averaging a
 * cell would add an antialias pass hardware never ran — it softens every
 * texture and every polygon edge the snap was supposed to make hard.
 *
 * The vote is the seam treatment: the integer snap opens sub-PSX-pixel slivers
 * along unstitched edges (silhouette retreat + T-junctions) where the surface
 * BEHIND leaks through. A 4x4 depth probe classifies the cell; on a depth
 * discontinuity the near side wins the cell, and if the cell CENTRE sits on
 * the losing (far) side the colour is read from the winning side's probe point
 * nearest the centre instead. Slivers are therefore overwritten by the surface
 * that owns the cell rather than merely diluted, while genuine edges resolve to
 * whichever surface dominates: hard pixel edges, as hardware drew them.
 * Depth comes from the WORLD-opaque pre-pass (op_scene_depth), addressed
 * through its own PSX-screen->uv affine (rq_od_affine) — the pre-pass
 * rasterizes a sub-rect of a grow-only allocation, so plain screen uv reads
 * outside it (all-far). Slivers read far there too (the pre-pass draws the same
 * snapped geometry), which is exactly what the vote needs. Cells are non-square
 * by construction: 512N across the 4:3 width, 224N visible rows. */
#version 450

layout(location = 0) in  vec2 op_uv;
layout(location = 0) out vec4 op_color;

layout(set = 0, binding = 0) uniform sampler2D op_prev_pass;
layout(set = 1, binding = 0) uniform sampler2D op_scene_depth;

/* Engine frame state — must stay layout-identical to op_engine_ubo in
 * openpete_shader_api.glsl (std140; only the fields this pass reads are
 * named past the header). */
layout(set = 3, binding = 0, std140) uniform op_engine_ubo {
    float op_time;
    float op_alpha;
    uint  op_tick;
    uint  op_present;
    uint  op_canonical;
    float op_aspect;
    vec2  op_resolution;
    vec2  op_window;
    vec4  op_scene_rect;   /* uv rect of the 4:3 content sub-rect */
    vec4  op_scene_grid;   /* disp_w, disp_h, clip_h, Hor+ factor */
};

layout(set = 3, binding = 1, std140) uniform rq_params {
    float rq_scale;        /* N: integer grid multiplier (1 = 512x240) */
    float rq_od_ok;        /* 1 = pre-pass depth valid this present    */
    vec2  rq_od_uvmax;     /* last valid alloc uv of the rastered sub-rect */
    vec4  rq_od_affine;    /* PSX screen -> op_scene_depth ALLOC uv:
                            * uv = (x*sx + y, z*sy + w)                */
};

/* The pre-pass stores d = 1 - 1/viewZ (viewZ in FULL-WORLD units), so
 * absolute-d thresholds are meaningless at distance — decode viewZ and
 * split on the RATIO instead, which is scale-invariant: cell-interior
 * spread of a smooth surface stays near 1x while a leak-through sliver
 * or a real silhouette jumps well past the threshold (the far-clear
 * sentinel decodes to ~1e7). Steep near slopes can false-trigger —
 * harmless: the centre tap is on the winning side of a smooth gradient
 * almost always, so the cell keeps its own centre colour. */
const float RQ_VZ_RATIO = 1.3;

/* Shared PSX-screen basis for the pass, filled once in main(). */
vec2  g_r0, g_rs;
float g_dispw, g_cliph, g_band;

/* PSX screen coords -> this pass's input uv. */
vec2 rq_screen_uv(vec2 sp)
{
    return vec2(g_r0.x +  sp.x / g_dispw            * g_rs.x,
                g_r0.y + (sp.y - g_band) / g_cliph  * g_rs.y);
}

/* Full-world view-Z of the opaque pre-pass at a PSX screen position; the
 * far sentinel when the pre-pass has no valid depth this present. */
float rq_view_z(vec2 sp)
{
    if (rq_od_ok <= 0.5) return 1.0e7;
    vec2 duv = vec2(rq_od_affine.x * sp.x + rq_od_affine.y,
                    rq_od_affine.z * sp.y + rq_od_affine.w);
    float d = texture(op_scene_depth, clamp(duv, vec2(0.0), rq_od_uvmax)).r;
    return 1.0 / max(1.0 - d, 1.0e-7);
}

void main() {
    g_r0    = op_scene_rect.xy;
    g_rs    = op_scene_rect.zw;
    g_dispw = op_scene_grid.x;
    g_cliph = op_scene_grid.z;
    g_band  = (op_scene_grid.y - g_cliph) * 0.5;
    float k = max(rq_scale, 1e-3);

    /* pass uv -> scaled virtual raster coords (1 unit = one raster cell) */
    float vx = (op_uv.x - g_r0.x) / g_rs.x * g_dispw * k;
    float vy = (g_band + (op_uv.y - g_r0.y) / g_rs.y * g_cliph) * k;
    vec2  c0 = floor(vec2(vx, vy));

    /* The sample point, in cell-relative units: the centre unless the vote
     * below moves it off a sliver. */
    vec2 pick = vec2(0.5);

    if (rq_od_ok > 0.5) {
        /* 4x4 depth probe across the cell interior */
        vec2  off[16];
        float dep[16];
        float dmin = 1.0e9, dmax = -1.0e9;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                int n = j * 4 + i;
                off[n] = (vec2(float(i), float(j)) + 0.5) / 4.0;
                dep[n] = rq_view_z((c0 + off[n]) / k);
                dmin = min(dmin, dep[n]); dmax = max(dmax, dep[n]);
            }

        if (dmax > dmin * RQ_VZ_RATIO) {
            /* Depth discontinuity: NEAR-BIASED vote (geometric split matches
             * the ratio clustering). A leak sliver can dominate its cell —
             * inside a wedge between two near surfaces even an all-far cell is
             * still a hole — so any small near quorum wins the cell, not the
             * majority. True silhouettes shift by at most ~1 cell (one PSX
             * pixel) toward the far side; leaks lose everywhere the near
             * surface has a foothold. */
            float mid = sqrt(dmin * dmax);
            int nearCnt = 0;
            for (int n = 0; n < 16; n++) if (dep[n] < mid) nearCnt++;
            bool keepNear = nearCnt >= 3;

            /* Keep the centre when it already belongs to the winning side —
             * that is the no-shift case and covers every cell interior. */
            if ((rq_view_z((c0 + pick) / k) < mid) != keepNear) {
                float best = 1.0e9;
                for (int n = 0; n < 16; n++) {
                    if ((dep[n] < mid) != keepNear) continue;
                    vec2  e  = off[n] - vec2(0.5);
                    float d2 = dot(e, e);
                    if (d2 < best) { best = d2; pick = off[n]; }
                }
            }
        }
    }

    vec2 uv = rq_screen_uv((c0 + pick) / k);
    op_color = vec4(texture(op_prev_pass, clamp(uv, vec2(0.0), vec2(1.0))).rgb, 1.0);
}
