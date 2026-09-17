// games/spyro-3/renderer/glint.c — the gem-glint feeder: a handwritten routine
// (asm/mobyfunc.s:2195-2507) that draws one screen-facing four-point star per
// live record of a 16-slot pool and links each star into the 3D ordering table
// itself, so none of its primitives reaches the 2D chokepoint the sprite feeder
// hooks.
//
// Read from the routine (asm/mobyfunc.s):
//   * the pool is 16 records of 0x10 bytes: world XYZ at +0x00, +0x04, +0x08
//     and a packed word at +0x0C holding the remaining lifetime in byte 0, the
//     size in byte 1, a spin phase in byte 2 and a colour-table row in byte 3
//     (:2231-2247).
//   * the routine subtracts the frame's elapsed count from the lifetime byte and
//     stores it back, dropping the record when it reaches zero (:2236-2240), so
//     the post-call lifetime byte is exactly the accept verdict for this pass.
//   * the record's camera-relative offset is halved before the rotation
//     (:2252-2258), so every view-space quantity the routine computes, the
//     near/far threshold included, is in half-world units.
//   * three cull stages: the halved offset must stay inside a +/-0x7800 box
//     (:2266-2276), the rotated depth must be at least 0x80 and under 0x7800
//     (:2294-2296), and the rotated X and Y must stay inside the view pyramid
//     4|X| - 3Z < 0x100 and 3|Y| - Z < 0xC0 (:2310, :2320).
//   * the size is a triangular ramp on the remaining lifetime times the size
//     byte, and the spin phase indexes the sine table for a 2x2 rotation-scale
//     basis loaded into the GTE rotation registers (:2324-2330), so the star
//     always faces the screen.
//   * the colour-table row is three words, each carrying 0x3A in its top byte:
//     the pale centre, the spike tip and the near-black waist, in that order
//     (:2330).
//   * under half-world depth 0x2000 the routine takes its near branch
//     (:2347-2348): one bundled draw-mode mini-packet (0xE1000620, whose ABR
//     bits are 0b01, additive) at :2383, then eight POLY_G4 semi-transparent
//     quads at :2387-2417, each spanning one rim segment of a seventeen-point
//     outline (a centre plus sixteen rim points, four spikes at radius 512 and
//     three shoulder points between each pair) that the GTE transformed into
//     the scratchpad at :2366-2373. Waist corners take the dark word, the spike
//     corner the tip word and the centre corner the pale word, so the star is
//     bright at its middle and fades out along each spike.
//   * at or above that depth the far branch (:2432) emits two opaque LINE_F2
//     instead, a plain cross through four projected endpoints, colouring them
//     with the pale word raised by 0x06000000 so its 0x3A code byte becomes
//     0x40 (:2480-2487).
//   * the ordering-table bucket is (Z >> 6) - 6 entries of eight bytes off the
//     base, clamped at zero (:2338-2344), with the far branch pushed a further
//     0x100 bytes back above depth 0x4000 (:2435).
//
// The geometry is the guest's own int16 output, read back out of the polybuf,
// not a second projection. What the primitives do not carry is depth - the PSX
// orders them by ordering-table bucket - so each star's view-Z comes from
// projecting its own pool record, paired to the star through the routine's own
// bookkeeping: a near record emits exactly one draw-mode packet and a far record
// emits exactly two lines, both in pool order, and the record walk here mirrors
// the routine's own rejects to say which records were accepted in which mode.
// The pairing is used only when the predicted near and far counts both agree
// with the captured ones; otherwise every primitive of the call is left unpaired
// and the census reports it.
//
// Spyro 1's sparkle renderer is fed by a float re-derivation in
// games/spyro-1/renderer/particle.c over a different record shape (a 0x18-byte
// record carrying a speed, a fade and a style byte) and a different star (a
// nine-point outline synthesized from the stock cross endpoints), with its
// primitives serving only as its A/B oracle. What the two share - the hook
// bracket, the polybuf walk, the primitive decode and the projection helpers -
// is already shared and is reused here unchanged.
//
// Determinism: never writes PSX RAM. The guest draw runs exactly once through
// the shared bracket and keeps its own verdicts, including the lifetime byte it
// decrements in each record; this feeder only reads them afterwards.

#include "feature_registry.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"
#include "renderer/env_knob.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "game_layout.h"
#include "spyro3_symbols.gen.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

#define GLINT_SLOTS    16u
#define GLINT_STRIDE   0x10u
#define GLINT_POS      0x00u    /* :2252-2254 three world words */
#define GLINT_PACKED   0x0Cu    /* :2231 lw 0xC: lifetime, size, spin, colour row */
#define GLINT_NEAR_VZ  0x2000   /* :2347 the half-world depth the branch switches at */

#define GLINT_MAX_PRIMS 256
#define GLINT_DRMODE    0xE1u   /* GP0 draw-mode packet, one per accepted near record */
#define GLINT_QUAD      0x3Au   /* POLY_G4 semi-transparent, eight per near record */
#define GLINT_LINE      0x40u   /* LINE_F2 opaque, two per accepted far record */
#define GLINT_QUADS_PER_STAR 8
#define GLINT_LINES_PER_CROSS 2

typedef struct {
    ngte_prim p;
    float     depth;
    int       paired;
    int       blend;
} glint_item;

static glint_item s_item[GLINT_MAX_PRIMS];
static int        s_item_n;
static int        s_stars;        /* draw-mode packets the call appended */
static int        s_crosses;      /* line pairs the call appended */
static int        s_pred_near;    /* near records the pool walk predicted */
static int        s_pred_far;     /* far records the pool walk predicted */
static uint32_t   s_code_hist[256];
static uint32_t   s_tag_hist[256];
static uint32_t   s_fed_star_tris;
static uint32_t   s_fed_cross_tris;

static int glint_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_item_n >= GLINT_MAX_PRIMS) return 0;
    s_item[s_item_n].p = *s;
    s_item[s_item_n].depth = 0.0f;
    s_item[s_item_n].paired = 0;
    s_item[s_item_n].blend = 0;
    s_item_n++;
    return 1;
}

/* Walk the interval the call allocated: 3-word draw-mode packets (tag length 2),
 * 9-word quads (tag length 8) and 4-word lines (tag length 3). */
static void glint_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    s_item_n = 0;
    const ngte_walk_policy pol = {
        .tag_len_max = 8, .raw_words_max = 9, .guard = GLINT_MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = s_tag_hist,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, glint_prim_sink, NULL);
}

typedef struct { float depth; int near_mode; } glint_rec;

/* Re-walk the pool and mirror the routine's rejects, appending one entry per
 * record it would have drawn. Returns the count. */
static int glint_predict(CPUState* cpu, const ngte_view* v, glint_rec* out, int cap) {
    int n = 0;
    const double cx = v->cam[0], cy = v->cam[1], cz = v->cam[2];

    for (uint32_t s = 0; s < GLINT_SLOTS; s++) {
        const uint32_t rec = D_GLINT_POOL + s * GLINT_STRIDE;
        const uint32_t packed = cpu->read_word(rec + GLINT_PACKED);
        if ((packed & 0xFFu) == 0u) continue;          /* :2236-2240 expired or free */

        const double px = (double)(int32_t)cpu->read_word(rec + GLINT_POS + 0u);
        const double py = (double)(int32_t)cpu->read_word(rec + GLINT_POS + 4u);
        const double pz = (double)(int32_t)cpu->read_word(rec + GLINT_POS + 8u);
        /* :2252-2258 the camera-relative offset, halved with an arithmetic
         * shift, which floors rather than truncating toward zero. */
        const double vx = floor((px - cx) / 2.0);      /* the routine's IR3 */
        const double vy = floor((cy - py) / 2.0);      /* IR1 */
        const double vz = floor((cz - pz) / 2.0);      /* IR2 */
        if (vx >= 0x7800 || vx <= -0x7800 || vy >= 0x7800 || vy <= -0x7800 ||
            vz >= 0x7800 || vz <= -0x7800) continue;   /* :2266-2276 */

        double m1, m2, m3;
        ngte_mac(v->cam_M, vy, vz, vx, &m1, &m2, &m3);
        if (m3 < 128.0 || m3 >= 0x7800) continue;      /* :2294-2296 */
        if (4.0 * fabs(m1) - 3.0 * m3 - 256.0 >= 0.0) continue;   /* :2310 */
        if (3.0 * fabs(m2) - m3 - 192.0 >= 0.0) continue;         /* :2320 */

        if (n < cap) {
            out[n].near_mode = (m3 < (double)GLINT_NEAR_VZ);
            out[n].depth = openpete_native_depth_from_viewz(
                openpete_worldvz_scaled((float)m3, 2.0f));
        }
        n++;
    }
    return n;
}

/* Attach each captured primitive to its record: a draw-mode packet opens a near
 * star and its ABR bits give the star's blend, the eight quads until the next
 * packet belong to it, and each consecutive pair of lines is one far cross. */
static void glint_pair_depths(CPUState* cpu, const ngte_view* v) {
    static glint_rec pred[GLINT_MAX_PRIMS];
    int lines = 0;
    s_stars = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_DRMODE) s_stars++;
        else if (s_item[i].p.code == GLINT_LINE) lines++;
    }
    s_crosses = lines / GLINT_LINES_PER_CROSS;

    const int predicted = glint_predict(cpu, v, pred, GLINT_MAX_PRIMS);
    s_pred_near = 0; s_pred_far = 0;
    for (int i = 0; i < predicted && i < GLINT_MAX_PRIMS; i++) {
        if (pred[i].near_mode) s_pred_near++; else s_pred_far++;
    }
    const int pair = (predicted <= GLINT_MAX_PRIMS &&
                      s_pred_near == s_stars && s_pred_far == s_crosses &&
                      lines % GLINT_LINES_PER_CROSS == 0);
    if (!pair) {
        /* Leave every primitive unpaired and at the nearest depth rather than
         * dropping it: an unpaired star still draws, and the census names the
         * call. */
        for (int i = 0; i < s_item_n; i++)
            s_item[i].blend = s_item[i].p.stp ? 1 : 0;
        return;
    }

    /* The pool order the routine drew in is the order the predictions came out
     * in, and the two branches interleave in that order, so one cursor over the
     * predictions serves both. */
    int r = -1, blend = 0, line_in_cross = 0;
    float depth = 0.0f;
    for (int i = 0; i < s_item_n; i++) {
        const uint8_t code = s_item[i].p.code;
        if (code == GLINT_DRMODE) {
            while (++r < predicted && !pred[r].near_mode) { }
            if (r >= predicted) return;
            /* The draw-mode word's semi-transparency rate, bits 5 and 6. */
            blend = ngte_abr_blend((int)((s_item[i].p.raw[1] >> 5) & 3u),
                                   NGTE_ABR_QUARTER_AS_ADD);
            depth = pred[r].depth;
            continue;
        }
        if (code == GLINT_LINE) {
            if (line_in_cross == 0) {
                while (++r < predicted && pred[r].near_mode) { }
                if (r >= predicted) return;
                blend = 0;                      /* :2480 the far cross is opaque */
                depth = pred[r].depth;
            }
            line_in_cross = (line_in_cross + 1) % GLINT_LINES_PER_CROSS;
        } else if (r < 0) {
            continue;                           /* a quad before any draw mode */
        }
        s_item[i].blend = blend;
        s_item[i].depth = depth;
        s_item[i].paired = 1;
    }
}

/* OPENPETE_GLINT selects what this feeder emits: 0 nothing, 1 the near stars
 * (the default), 2 the near stars and the far crosses. The guest draw runs at
 * every setting, so the A/B prices the painted pixels from one binary.
 *
 * The far cross is captured and censused at every setting but fed only at 2:
 * its ordering-table bucket loses to the world's on the PSX, where the
 * reference paints terrain over it, while a true depth test puts it in front,
 * so feeding it by default would draw geometry the reference does not show. */
static int glint_feed_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_GLINT");
        v = (e && *e) ? atoi(e) : 1;
        if (v < 0) v = 0;
    }
    return v;
}

static void glint_add_quad(const float x[4], const float y[4],
                           const uint32_t rgb[4], float depth, int blend) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    for (int s = 0; s < 2; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].u = 0.0f;  st.v[k].v = 0.0f;
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
    }
}

/* A line primitive has no area, so it feeds as a one-pixel-wide quad across its
 * own perpendicular, the width the GPU rasterizes it at. */
static void glint_feed_line(const ngte_prim* p, float depth, int blend) {
    const uint32_t rgb = p->raw[1] & 0xFFFFFFu;
    const uint32_t xy0 = p->raw[2], xy1 = p->raw[3];
    const float ax = (float)(int16_t)(xy0 & 0xFFFFu), ay = (float)(int16_t)(xy0 >> 16);
    const float bx = (float)(int16_t)(xy1 & 0xFFFFu), by = (float)(int16_t)(xy1 >> 16);
    const float dx = bx - ax, dy = by - ay;
    const float len = sqrtf(dx * dx + dy * dy);
    float ox = 0.5f, oy = 0.0f;
    if (len > 1e-3f) { ox = -dy / len * 0.5f; oy = dx / len * 0.5f; }
    const float qx[4] = { ax - ox, ax + ox, bx - ox, bx + ox };
    const float qy[4] = { ay - oy, ay + oy, by - oy, by + oy };
    const uint32_t cc[4] = { rgb, rgb, rgb, rgb };
    glint_add_quad(qx, qy, cc, depth, blend);
}

static void glint_feed_native(void) {
    const int mode = glint_feed_mode();
    if (mode <= 0) return;
    if (s_item_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    for (int i = 0; i < s_item_n; i++) {
        const ngte_prim* p = &s_item[i].p;
        if (p->code == GLINT_QUAD && p->nv == 4) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_FLARE, GLINT_QUAD, 0));
            const float x[4] = { p->x[0], p->x[1], p->x[2], p->x[3] };
            const float y[4] = { p->y[0], p->y[1], p->y[2], p->y[3] };
            const uint32_t c[4] = { p->rgb[0], p->rgb[1], p->rgb[2], p->rgb[3] };
            glint_add_quad(x, y, c, s_item[i].depth, s_item[i].blend);
            s_fed_star_tris += 2;
        } else if (p->code == GLINT_LINE && mode >= 2) {
            openpete_native_scene_set_tag(
                OPENPETE_TAG_PARTICLE(OPENPETE_TAG_PART_SPARK, GLINT_LINE, 0));
            glint_feed_line(p, s_item[i].depth, s_item[i].blend);
            s_fed_cross_tris += 2;
        }
    }
    openpete_native_scene_set_tag(0);
}

/* OPENPETE_GLINT_CENSUS=LO-HI (or a single frame): per call in that frame range,
 * the primitives the routine appended by GPU code, the stars its draw-mode
 * packets delimit and the crosses its line pairs delimit, the near and far
 * records the pool walk predicted, the native triangles this feeder emitted from
 * each class, and how many primitives were left unpaired. A capture whose fed
 * count is zero measured nothing. */
/* Structural check of one star's captured geometry: its eight quads must share
 * a single centre corner, and their remaining corners must reduce to at most the
 * sixteen rim points of the outline the routine transformed. A decode that
 * mis-slices the quad words, or a walk that straddles a primitive boundary,
 * breaks the shared centre. Returns the number of distinct rim points, or -1
 * when the centre is not shared; a small star on screen quantizes neighbouring
 * rim points onto the same pixel, so fewer than sixteen is normal. */
static int glint_star_topology_ok(int first, int count) {
    if (count != GLINT_QUADS_PER_STAR) return -1;
    float rx[GLINT_QUADS_PER_STAR * 3], ry[GLINT_QUADS_PER_STAR * 3];
    int rim = 0;
    for (int q = 0; q < count; q++) {
        const ngte_prim* p = &s_item[first + q].p;
        if (p->nv != 4) return -1;
        if (p->x[2] != s_item[first].p.x[2] || p->y[2] != s_item[first].p.y[2]) return -1;
        static const int corner[3] = { 0, 1, 3 };
        for (int k = 0; k < 3; k++) {
            const float cx = p->x[corner[k]], cy = p->y[corner[k]];
            int seen = 0;
            for (int j = 0; j < rim; j++) if (rx[j] == cx && ry[j] == cy) { seen = 1; break; }
            if (!seen && rim < (int)(sizeof rx / sizeof rx[0])) { rx[rim] = cx; ry[rim] = cy; rim++; }
        }
    }
    return rim;
}

/* Stars whose eight quads share one centre corner, and the largest distinct rim
 * count any star of the call reached. */
static void glint_stars_ok(int* ok, int* rim_max) {
    int first = -1, n = 0;
    *ok = 0; *rim_max = 0;
    for (int i = 0; i <= s_item_n; i++) {
        const int end = (i == s_item_n) || s_item[i].p.code == GLINT_DRMODE;
        if (end) {
            if (first >= 0) {
                const int rim = glint_star_topology_ok(first, n);
                if (rim >= 0) { (*ok)++; if (rim > *rim_max) *rim_max = rim; }
            }
            first = i + 1; n = 0;
        } else if (s_item[i].p.code == GLINT_QUAD && first >= 0) {
            n++;
        }
    }
}

static void glint_census(uint32_t start, uint32_t end,
                         uint32_t star_before, uint32_t cross_before) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_GLINT_CENSUS", openpete_frame)) return;
    int unpaired = 0;
    for (int i = 0; i < s_item_n; i++)
        if (s_item[i].p.code != GLINT_DRMODE && !s_item[i].paired) unpaired++;
    fprintf(stderr, "[glint-census f=%u] polybuf [%08X,%08X) %u bytes prims=%d codes:",
            openpete_frame, start, end, end - start, s_item_n);
    uint32_t seen[256];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < s_item_n; i++) seen[s_item[i].p.code]++;
    for (int c = 0; c < 256; c++) if (seen[c]) fprintf(stderr, " %02X:%u", c, seen[c]);
    int stars_ok = 0, rim_max = 0;
    glint_stars_ok(&stars_ok, &rim_max);
    fprintf(stderr, " stars=%d centre_ok=%d rim_max=%d crosses=%d pred_near=%d pred_far=%d"
                    " fed_star_tris=%u fed_cross_tris=%u unpaired=%d\n",
            s_stars, stars_ok, rim_max, s_crosses, s_pred_near, s_pred_far,
            s_fed_star_tris - star_before, s_fed_cross_tris - cross_before, unpaired);
    /* Each star's centre corner and each cross's first endpoint, with the scene
     * depth the pairing gave it: the two quantities the feed places them by. */
    int line_i = 0;
    for (int i = 0; i < s_item_n; i++) {
        if (s_item[i].p.code == GLINT_QUAD &&
            (i == 0 || s_item[i - 1].p.code != GLINT_QUAD)) {
            fprintf(stderr, "[glint-star f=%u] centre=(%.0f,%.0f) depth=%.6f blend=%d\n",
                    openpete_frame, s_item[i].p.x[2], s_item[i].p.y[2],
                    s_item[i].depth, s_item[i].blend);
        } else if (s_item[i].p.code == GLINT_LINE) {
            if (line_i % GLINT_LINES_PER_CROSS == 0)
                fprintf(stderr, "[glint-cross f=%u] a=(%d,%d) depth=%.6f\n", openpete_frame,
                        (int)(int16_t)(s_item[i].p.raw[2] & 0xFFFFu),
                        (int)(int16_t)(s_item[i].p.raw[2] >> 16), s_item[i].depth);
            line_i++;
        }
    }
}

/* ---- hook ---------------------------------------------------------------- */

static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_SPARKLE, .prof_tag = "glint.srend", .st = &s_hb,
};

/* The world draw's call (asm/nonmatchings/draw/func_8001E460.s:30) is the only
 * one in the image, so the hook takes every entry. */
static int on_glint_draw(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    s_item_n = 0; s_stars = 0; s_crosses = 0; s_pred_near = 0; s_pred_far = 0;
    const uint32_t star_before = s_fed_star_tris, cross_before = s_fed_cross_tris;
    if (endp > startp) {
        glint_capture_prims(cpu, startp, endp);
        ngte_view view;
        ngte_view_build(cpu, &view);
        glint_pair_depths(cpu, &view);
        if (feed && render_path_enabled(RP_SPARKLE)) glint_feed_native();
    }
    glint_census(startp, endp, star_before, cross_before);
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract. The primitives are the guest's own output and are gone
 * once the polybuf is reused, so the replay re-feeds the set the canonical tick
 * captured, at the depths paired then. At alpha 1 it reproduces the canonical
 * present exactly, which is what the spine's identity leg measures. */
void glint_reextract(CPUState* cpu) {
    (void)cpu;
    if (!hook_reextract_ready(RP_SPARKLE)) return;
    glint_feed_native();
}

static const feature_dispatch_entry_t glint_dispatch[] = {
    { F_GLINT_DRAW, on_glint_draw },
    { 0u, NULL },
};

const feature_module_t glint_module = {
    .name           = "glint_native",
    .apply_options  = NULL,
    .dispatch_table = glint_dispatch,
};
