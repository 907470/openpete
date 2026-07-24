/* raster_quantize.frag — engine SCENE-point pass (`--raster-quantize N`):
 * resample the composed 3D scene on the 512Nx240N virtual PSX raster,
 * BOX-AVERAGING each cell (16 taps) with a DEPTH-MAJORITY vote. The base
 * grid comes from op_scene_rect/op_scene_grid and N is an integer
 * MULTIPLIER of it, so cell boundaries always contain the integer lattice
 * --vertex-precision integer snaps vertices to, under any window size,
 * aspect, or Hor+ widening.
 *
 * The vote is the seam treatment: the integer snap opens sub-PSX-pixel
 * slivers along unstitched edges (silhouette retreat + T-junctions) where
 * the surface BEHIND leaks through. Inside a cell those leak taps are a
 * depth minority far behind the cell's dominant surface, so on a depth
 * discontinuity the cell averages only the majority side's taps — slivers
 * vanish entirely, while genuine edges (an honest near/far split) resolve
 * to whichever surface dominates: hard pixel edges, as hardware drew them.
 * Depth comes from the WORLD-opaque pre-pass (op_scene_depth), addressed
 * through its own PSX-screen->uv affine (rq_od_affine) — the pre-pass
 * rasterizes a sub-rect of a grow-only allocation, so plain screen uv
 * reads outside it (all-far). Slivers read far there too (the pre-pass
 * draws the same snapped geometry), which is exactly what the vote needs.
 * Cells are non-square by construction: 512N across the 4:3 width, 224N
 * visible rows. */
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
 * harmless: majority-averaging one side of a smooth gradient is
 * visually identical to averaging all of it. */
const float RQ_VZ_RATIO = 1.3;

void main() {
    vec2  r0    = op_scene_rect.xy;
    vec2  rs    = op_scene_rect.zw;
    float dispw = op_scene_grid.x;
    float cliph = op_scene_grid.z;
    float band  = (op_scene_grid.y - cliph) * 0.5;
    float k     = max(rq_scale, 1e-3);

    /* pass uv -> scaled virtual raster coords (1 unit = one raster cell) */
    float vx = (op_uv.x - r0.x) / rs.x * dispw * k;
    float vy = (band + (op_uv.y - r0.y) / rs.y * cliph) * k;
    vec2  c0 = floor(vec2(vx, vy));

    /* gather: 4x4 point taps across the cell interior */
    vec3  col[16];
    float dep[16];
    float dmin = 1.0e9, dmax = -1.0e9;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            vec2  v   = c0 + (vec2(float(i), float(j)) + 0.5) / 4.0;
            float sxp = v.x / k;               /* tap in PSX screen coords */
            float syp = v.y / k;
            vec2 uv = vec2(r0.x +  sxp / dispw          * rs.x,
                           r0.y + (syp - band) / cliph  * rs.y);
            int n = j * 4 + i;
            col[n] = texture(op_prev_pass,
                             clamp(uv, vec2(0.0), vec2(1.0))).rgb;
            float vz = 1.0e7;
            if (rq_od_ok > 0.5) {
                vec2 duv = vec2(rq_od_affine.x * sxp + rq_od_affine.y,
                                rq_od_affine.z * syp + rq_od_affine.w);
                float d = texture(op_scene_depth,
                                  clamp(duv, vec2(0.0), rq_od_uvmax)).r;
                vz = 1.0 / max(1.0 - d, 1.0e-7);   /* full-world view-Z */
            }
            dep[n] = vz;
            dmin = min(dmin, vz); dmax = max(dmax, vz);
        }

    vec3  acc = vec3(0.0);
    float cnt = 0.0;
    if (rq_od_ok > 0.5 && dmax > dmin * RQ_VZ_RATIO) {
        /* Depth discontinuity: NEAR-BIASED vote (geometric split matches the
         * ratio clustering). A leak sliver can dominate its cell — inside a
         * wedge between two near surfaces even an all-far cell is still a
         * hole — so any small near quorum wins the cell, not the majority.
         * True silhouettes dilate by at most ~1 cell (one PSX pixel) toward
         * the far side; leaks lose everywhere the near surface has a foothold. */
        float mid = sqrt(dmin * dmax);
        int nearCnt = 0;
        for (int n = 0; n < 16; n++) if (dep[n] < mid) nearCnt++;
        bool keepNear = nearCnt >= 3;
        for (int n = 0; n < 16; n++)
            if ((dep[n] < mid) == keepNear) { acc += col[n]; cnt += 1.0; }
    } else {
        for (int n = 0; n < 16; n++) acc += col[n];
        cnt = 16.0;
    }
    op_color = vec4(acc / cnt, 1.0);
}
