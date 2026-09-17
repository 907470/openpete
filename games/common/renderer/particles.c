// games/common/renderer/particles.c — the particle feeder for a game whose
// particle renderer links its primitives into the 3D ordering table itself.
//
// The routine (asm/partsdraw.s) is handwritten: it loads the camera matrix
// into the GTE with a zero translation, walks a pool of fixed-stride records
// and appends one primitive per drawn record to the polybuf. Six classes:
// a 4-word LINE_F2 dot, a 5-word LINE_G2 streak, and 10-word POLY_FT4
// billboards. Because it links each primitive itself, none of them reaches
// the 2D ordering-table chokepoint the sprite feeder hooks, so without this
// feeder the native scene shows no particles at all.
//
// The geometry is taken from the primitives the routine wrote, not
// re-derived: the routine's screen positions are already the GTE's int16
// output, and reading them back costs one polybuf walk instead of a second
// copy of a six-class projection. What the primitives do not carry is depth —
// the PSX orders them by ordering-table bucket — so the feeder recovers each
// primitive's view-Z by projecting its own record's position, pairing
// primitives to records through the stamp byte the routine maintains
// (partsdraw.s:223 clears it when a record's processing starts, :358, :415,
// :528, :693 and :745 set it once the primitive has been appended), which
// makes the pairing the routine's own invariant rather than a match on
// geometry. Class `fixed_layer_class` and up link at a fixed front bucket
// (partsdraw.s:202) and stamp nothing, so those primitives take the near
// depth that bucket means and are paired by class alone.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the stamp bytes and
// the polybuf cursor; this feeder only reads them afterwards.
//
// Gated by OPENPETE_PARTICLE_CENSUS (census) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "renderer/particles.h"
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

/* The pool walk's own shape, held here rather than in the descriptor because
 * it is the shape of one routine: the selector's class field and the sentinel
 * that ends the pool are how the routine dispatches, not a per-game datum. */
#define PART_CLASS_SHIFT 8
#define PART_CLASS_MASK  0xF
#define PART_POOL_END    (-1)   /* selector >> 8 at the end-of-pool sentinel */

#define PART_MAX_PRIMS 2048

typedef struct {
    ngte_prim p;
    float     depth;
    float     psx, psy;   /* the record's own projected point, or the sentinel */
    int       cls;        /* the record's class, or -1 when unpaired */
} part_item;

static part_item s_item[PART_MAX_PRIMS];
static int       s_item_n;

static uint32_t s_code_hist[256];
static uint32_t s_tag_hist[256];
static uint32_t s_fed_tris;      /* native triangles the last feed emitted */

static int part_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= PART_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].psx = s_item[s_item_n].psy = 0.0f;
    s_item[s_item_n].cls = -1;
    s_item_n++;
    return 1;
}

/* Walk the interval the routine allocated. The longest primitive it appends
 * is the ten-word POLY_FT4 (tag length 9), and the line classes need their
 * raw words because a line is not a polygon and decodes here. */
static void part_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 9, .raw_words_max = 10, .guard = PART_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, part_prim_sink, NULL);
}

/* Pair each captured primitive with the record that appended it and store the
 * record's view-Z as a scene depth. The routine appends in pool order and at
 * most one primitive per record, so walking the pool and the captured list
 * together aligns them; a record emitted when its stamp is set, or when its
 * class links at the fixed front bucket, where the routine stamps nothing and
 * always appends.
 *
 * The record position is three unsigned halves in quarter world units
 * (partsdraw.s:709-712), and the routine builds the GTE vector from it the
 * way every draw in this engine does: (camY - Y, camZ - Z, X - camX) with the
 * camera position shifted right by two, rotated by the camera matrix at
 * +0x00 with a zero translation. */
static void part_pair_depths(CPUState* cpu, const ngte_view* v) {
    const uint32_t base = cpu->read_word(game_layout.particles.array_ptr);
    if (base < 0x80000000u || base >= 0x80800000u) return;
    const uint32_t stride = game_layout.particles.rec_stride;
    const double cx = v->cam[0] / 4.0, cy = v->cam[1] / 4.0, cz = v->cam[2] / 4.0;
    /* The fixed front bucket is nearer than bucket 0, which the near guard in
     * openpete_native_depth_from_viewz already maps to the smallest depth the
     * pipeline carries. */
    const float front = openpete_native_depth_from_viewz(openpete_worldvz_full(1.0f));

    int i = 0;
    for (uint32_t rec = base; i < s_item_n; rec += stride) {
        const int sel = (int)(int16_t)(uint16_t)cpu->read_half(rec + game_layout.particles.sel_off);
        if (sel < 0) {
            if ((sel >> PART_CLASS_SHIFT) == PART_POOL_END) break;
            continue;                                    /* inactive slot */
        }
        const int cls = (sel >> PART_CLASS_SHIFT) & PART_CLASS_MASK;
        if ((uint32_t)cls >= game_layout.particles.fixed_layer_class) {
            s_item[i].cls = cls;
            s_item[i++].depth = front;
            continue;
        }
        if (cpu->read_byte(rec + game_layout.particles.drawn_off) == 0) continue;

        const uint32_t w0 = cpu->read_word(rec + game_layout.particles.pos_off);
        const uint32_t w1 = cpu->read_word(rec + game_layout.particles.pos_off + 4u);
        const double px = (double)(w0 & 0xFFFFu);
        const double py = (double)((w0 >> 16) & 0xFFFFu);
        const double pz = (double)(w1 & 0xFFFFu);
        ngte_proj o;
        ngte_project_gte(v->cam_M, NULL, cy - py, cz - pz, px - cx,
                         v->ofx, v->ofy, v, 0, &o);
        s_item[i].cls = cls;
        s_item[i].psx = (float)o.sx;
        s_item[i].psy = (float)o.sy;
        s_item[i++].depth =
            openpete_native_depth_from_viewz(openpete_worldvz_scaled((float)o.m3, 4.0f));
    }
    /* A polybuf that filled mid-pool ends the routine early, so fewer
     * primitives than stamped records is normal; the loop above simply stops
     * at the last captured primitive. Any primitive the loop could not reach
     * keeps depth 0, the nearest value, which is the fixed bucket's meaning. */
}

/* OPENPETE_PARTICLES=0 suppresses this feed alone. The guest draw still runs,
 * so the A/B prices the particles' painted pixels from one binary. */
static int part_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PARTICLES"); v = !(e && *e && *e == '0'); }
    return v;
}

static void part_add_quad(const float x[4], const float y[4], const float u[4],
                          const float vv[4], const uint32_t rgb[4], float depth,
                          const ngte_tpage* tp, int textured, int blend) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].u = u[j];  st.v[k].v = vv[j];
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = textured;
        if (textured) {
            st.tpage_x = tp->tpx; st.tpage_y = tp->tpy;
            st.clut_x  = tp->clx; st.clut_y  = tp->cly; st.bpp = tp->bpp;
        }
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        s_fed_tris++;
    }
}

/* A line primitive has no area, so it feeds as a one-pixel-wide quad across
 * its own perpendicular, the width the GPU rasterizes it at. */
static void part_feed_line(const ngte_prim* p, float depth, int gouraud) {
    const uint32_t w1 = p->raw[1];
    const int stp = (int)((w1 >> 24) & 0x02u);
    const uint32_t rgb0 = w1 & 0xFFFFFFu;
    uint32_t xy0, xy1, rgb1;
    if (gouraud) { xy0 = p->raw[2]; rgb1 = p->raw[3] & 0xFFFFFFu; xy1 = p->raw[4]; }
    else         { xy0 = p->raw[2]; rgb1 = rgb0;                   xy1 = p->raw[3]; }
    const float ax = (float)(int16_t)(xy0 & 0xFFFFu), ay = (float)(int16_t)(xy0 >> 16);
    const float bx = (float)(int16_t)(xy1 & 0xFFFFu), by = (float)(int16_t)(xy1 >> 16);
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float ox = 0.5f, oy = 0.0f;
    if (len > 1e-3f) { ox = -dy / len * 0.5f; oy = dx / len * 0.5f; }
    const float qx[4] = { ax - ox, ax + ox, bx - ox, bx + ox };
    const float qy[4] = { ay - oy, ay + oy, by - oy, by + oy };
    const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const uint32_t cc[4] = { rgb0, rgb0, rgb1, rgb1 };
    part_add_quad(qx, qy, zu, zu, cc, depth, NULL, 0, stp ? 1 : 0);
}

static void part_feed_native(void) {
    if (!part_feed_enabled()) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        const float depth = s_item[i].depth;
        if (p->nv == 4 || p->nv == 3) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_QUAD, (uint32_t)p->code, 0));
            ngte_tpage tp;
            int blend = p->stp ? 1 : 0;
            if (p->textured) {
                ngte_decode_tpage((unsigned)p->tpage, (unsigned)p->clut, 0u, &tp);
                if (p->stp) blend = ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD);
            } else {
                memset(&tp, 0, sizeof tp);
            }
            /* A triangle repeats its last corner, so one split covers both. */
            const int last = p->nv - 1;
            const float x[4] = { p->x[0], p->x[1], p->x[2], p->x[last] };
            const float y[4] = { p->y[0], p->y[1], p->y[2], p->y[last] };
            const float u[4] = { (float)p->u[0], (float)p->u[1], (float)p->u[2], (float)p->u[last] };
            const float vt[4] = { (float)p->v[0], (float)p->v[1], (float)p->v[2], (float)p->v[last] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[last] };
            part_add_quad(x, y, u, vt, c, depth, &tp, p->textured, blend);
        } else if ((p->code & 0xF8u) == 0x40u || (p->code & 0xF8u) == 0x50u) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_LINE, (uint32_t)p->code, 0));
            part_feed_line(p, depth, (p->code & 0x10u) != 0);
        }
    }
    openpete_native_scene_set_tag(0);
}

/* OPENPETE_PARTICLE_CENSUS=LO-HI (or a single frame): per call in that frame
 * range, the primitives the routine appended by GPU code, the native triangles
 * this feeder emitted from them, and the calibration the depth pairing rests
 * on — the distance between each paired record's own projected point and the
 * centroid of the primitive the pairing gave it. The routine projects the
 * record and then offsets the primitive's corners around that point, so a
 * correct pairing (and a correct camera matrix) keeps this small and a wrong
 * one scatters it across the frame. A capture whose fed count is zero measured
 * nothing. */
static void part_census(uint32_t start, uint32_t end, uint32_t fed_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_PARTICLE_CENSUS", openpete_frame)) return;
    fprintf(stderr, "[part-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    fprintf(stderr, " fed_tris=%u pairs:", s_fed_tris - fed_before);
    for (int cl = 0; cl < 16; cl++) {
        int n = 0; double sum = 0, worst = 0;
        for (int i = 0; i < s_item_n; i++) {
            const part_item* it = &s_item[i];
            if (it->cls != cl || (uint32_t)cl >= game_layout.particles.fixed_layer_class) continue;
            const ngte_prim* p = &it->p;
            double gx = 0, gy = 0; int nv = 0;
            if (p->nv) {
                for (int k = 0; k < p->nv; k++) { gx += p->x[k]; gy += p->y[k]; }
                nv = p->nv;
            } else {
                const int words = (p->code & 0x10u) ? 3 : 2;   /* gouraud line carries a colour per endpoint */
                for (int k = 0; k < 2; k++) {
                    const uint32_t xy = p->raw[2 + (uint32_t)k * (uint32_t)(words - 1)];
                    gx += (double)(int16_t)(xy & 0xFFFFu);
                    gy += (double)(int16_t)(xy >> 16);
                }
                nv = 2;
            }
            gx /= nv; gy /= nv;
            const double d = fabs(gx - it->psx) + fabs(gy - it->psy);
            sum += d; if (d > worst) worst = d; n++;
        }
        if (n) fprintf(stderr, " c%d:n=%d mean=%.2f worst=%.1f", cl, n, sum / n, worst);
    }
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++) if (s_item[i].cls < 0) unpaired++;
    fprintf(stderr, " unpaired=%d\n", unpaired);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, at the depths paired then. Their screen positions are the tick's;
 * at alpha 1 the replay reproduces the canonical present exactly, which is what
 * the spine's identity leg measures. */
void particles_reextract(CPUState* cpu) {
    (void)cpu;
    if (!hook_reextract_ready(RP_PARTICLES)) return;
    part_feed_native();
}

/* ---- hook ----------------------------------------------------------------- */
static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_PARTICLES, .prof_tag = "particles.srend",
    .st = &s_hb,
};

static int on_particles_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    if (endp > startp) {
        part_capture_prims(cpu, startp, endp);
        const uint32_t fed_before = s_fed_tris;
        ngte_view view;
        ngte_view_build(cpu, &view);
        part_pair_depths(cpu, &view);
        if (feed && render_path_enabled(RP_PARTICLES)) part_feed_native();
        part_census(startp, endp, fed_before);
    }
    return hook_bracket_end(&k_bracket);
}

/* The hook address is the layout's particle draw site, bound at boot: a static
 * initializer cannot read another translation unit's const. */
static feature_dispatch_entry_t particles_dispatch[] = {
    { 0u, on_particles_draw },
    { 0u, NULL },
};

static void particles_apply_options(const openpete_options_t* opts) {
    (void)opts;
    particles_dispatch[0].addr = game_layout.particles.hook;
}

const feature_module_t particles_module = {
    .name           = "particles_native",
    .apply_options  = particles_apply_options,
    .dispatch_table = particles_dispatch,
};
