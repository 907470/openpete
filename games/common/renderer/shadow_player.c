// games/common/renderer/shadow_player.c — the PLAYER GROUND-SHADOW leg of the
// shared shadow feeder, re-derived from gamestate in float, plus its A/B
// harness. Every engine address comes from game_layout.shadow.player, so a
// game without the leg leaves `hook` at 0 and never registers this module.
//
// The draw is an untextured gouraud fan (GPU code 0x32, semi-transparent) of
// one triangle per ring point, fanned from the ground anchor and flat in the
// shadow's own plane, drawn with PSX semi-transparency mode 2 (B-F,
// SUBTRACTIVE: the grey darkens the ground). Each triangle carries its own
// bundled draw-mode mini-packet (0xE1000640, whose ABR bits are 0b10).
//
// STATIC ASM FINDINGS (asm/mobyfunc.s:622-884, asm/stdutil.s:func_8004EA90):
//   * the anchor is the camera-relative shadow position swizzled into the
//     GTE's IR vector as (camY - shY, camZ - shZ, shX - camX) and rotated by
//     the camera matrix; its MAC1..3 become the TR every ring point projects
//     through, so the ring is local to the anchor.
//   * the plane's orientation is three optional byte-angle rotations of the
//     camera matrix, applied Y then X then Z, each replacing two columns with
//     one MVMVA pair.
//   * the 16 ring radii are eight silhouette bytes at the head of the
//     avatar's animation frame, INTPL-interpolated towards a second frame's
//     eight, then doubled by inserting each neighbouring pair's midpoint.
//   * the fan is (anchor, strip[k], strip[k+1]) with color0 = 0x608080 and
//     color1 = color2 = 0, so the shadow is darkest under the avatar and
//     fades to nothing at the rim.
//   * a triangle whose ordering key is negative is overwritten by the next
//     one instead of being linked, so the fan can emit fewer than 16.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to normal dispatch) then only READS state afterwards.
// Gated by OPENPETE_SHADOW_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/shadow.h"        /* shared prim capture, projection, decal params */
#include "renderer/player.h"        /* per-call records of the player hooks */
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;

/* State-block offsets (asm/mobyfunc.s:625-715). Held here rather than in the
 * descriptor because they are the shape of one routine, not a per-game datum:
 * a second game running this leg runs the same routine. */
#define PLSH_SCALE_FLAG 0x00u   /* non-zero: every radius scaled by three quarters */
#define PLSH_POS        0x04u   /* world position, three words */
#define PLSH_ROT        0x10u   /* packed byte angles; the top byte suppresses */
#define PLSH_SUPPRESS   0x13u
#define PLSH_OT_BIAS    0x14u

#define PLSH_ANIM_HDR    0x20u  /* bytes from an animation to its first frame */
#define PLSH_FRAME_LEN   0x14u  /* bytes per frame record */
#define PLSH_RING_N      16     /* ring points, and so triangles, per fan */
#define PLSH_SIL_N       8      /* silhouette bytes a frame record carries */

/* Rotate the camera matrix into the shadow's plane. Each non-zero byte angle
 * replaces two columns of the running matrix with one MVMVA pair, in the
 * order Y (bits 16-23), X (bits 8-15), Z (bits 0-7). */
static void plsh_matrix(CPUState* cpu, const int C[3][3], uint32_t rot, int R[3][3]) {
    const uint32_t sinb = game_layout.sin_table;
    memcpy(R, C, sizeof(int) * 9);
    if (!sinb) return;
    const int ang[3] = { (int)((rot >> 16) & 0xFFu), (int)((rot >> 8) & 0xFFu), (int)(rot & 0xFFu) };
    for (int axis = 0; axis < 3; axis++) {
        int a = ang[axis];
        if (a == 0) continue;
        int s = shadow_rd_s16(cpu, sinb + (uint32_t)a * 2u);
        int c = shadow_rd_s16(cpu, sinb + 0x80u + (uint32_t)a * 2u);
        int P[3], Q[3];
        int ci, qi;   /* the two columns the pair replaces */
        if (axis == 0)      { shadow_mvmva_ir(R,  c, 0, s, P); shadow_mvmva_ir(R, -s, 0, c, Q); ci = 0; qi = 2; }
        else if (axis == 1) { shadow_mvmva_ir(R, 0,  c, s, P); shadow_mvmva_ir(R, 0, -s, c, Q); ci = 1; qi = 2; }
        else                { shadow_mvmva_ir(R,  c, s, 0, P); shadow_mvmva_ir(R, -s, c, 0, Q); ci = 0; qi = 1; }
        for (int i = 0; i < 3; i++) { R[i][ci] = P[i]; R[i][qi] = Q[i]; }
    }
}

/* native player-shadow fan tris (untextured G3) */
typedef struct { float sx[3], sy[3], sz[3]; uint32_t rgb[3]; } plshtri;
static plshtri s_tri[PLSH_RING_N];
static int     s_tri_n;

/* SZ3: the GTE's view-Z saturated to 16 bits unsigned, the value the ordering
 * key below is computed from. */
static int plsh_sz3(float m3) {
    int z = (int)lrintf(m3);
    return z < 0 ? 0 : (z > 0xFFFF ? 0xFFFF : z);
}

/* Read the eight silhouette bytes of one animation frame. Returns 0 when the
 * animation pointer is not RAM. */
static int plsh_frame_bytes(CPUState* cpu, uint32_t table, uint32_t anim_sel,
                            int anim_shift, int frame_shift, uint8_t out[PLSH_SIL_N]) {
    uint32_t a = cpu->read_word(table + (((anim_sel >> anim_shift) & 0xFFu) << 2));
    if (a < 0x80000000u || a >= 0x80800000u) return 0;
    uint32_t f = a + PLSH_ANIM_HDR + ((anim_sel >> frame_shift) & 0xFFu) * PLSH_FRAME_LEN;
    for (int i = 0; i < PLSH_SIL_N; i++) out[i] = cpu->read_byte(f + (uint32_t)i);
    return 1;
}

static void shadow_player_build(CPUState* cpu) {
    s_tri_n = 0;
    const uint32_t st = game_layout.shadow.player.state;
    if (!st) return;
    if (cpu->read_byte(st + PLSH_SUPPRESS) != 0) return;   /* swimming, or another avatar */

    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per walk */
    const ngte_view* v = &view;
    int C[3][3]; memcpy(C, v->cam_M, sizeof C);
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;

    /* Ground anchor: the camera-relative shadow position, swizzled the way the
     * routine loads it into IR1..IR3, projected with TR = 0. Its MAC1..3 are
     * the TR the ring points then project through. The camera comes from the
     * view's interpolated position, so the anchor tracks a camera that moves
     * between ticks rather than stepping with it. */
    double shx = (double)(int32_t)cpu->read_word(st + PLSH_POS + 0);
    double shy = (double)(int32_t)cpu->read_word(st + PLSH_POS + 4);
    double shz = (double)(int32_t)cpu->read_word(st + PLSH_POS + 8);
    double vx = v->cam[1] - shy;
    double vy = v->cam[2] - shz;
    double vz = shx - v->cam[0];
    double TR[3]; shadow_ground_tr(C, vx, vy, vz, TR);
    double ZERO[3] = {0, 0, 0};
    shadow_pt anchor; shadow_project(C, ZERO, ofx, ofy, v, vx, vy, vz, &anchor);

    /* The depth-aware decal's camera reconstruction: the fan is drawn with the
     * depth test off, and the shader recovers each terrain pixel's world height
     * from the opaque depth pre-pass to reject wall faces standing above the
     * shadow's ground plane. */
    shadow_push_decal_params(cpu, v, C, ofx, ofy, H, NULL);

    int R[3][3];
    plsh_matrix(cpu, C, cpu->read_word(st + PLSH_ROT), R);

    /* Silhouette radii: the current animation frame's eight bytes, interpolated
     * towards a second frame's eight by the weight the blend table maps the
     * avatar's blend byte to (the routine's IR0, the byte times 16 against
     * 4096 as one). */
    const uint32_t pl    = game_layout.player.base;
    const uint32_t tabp  = game_layout.shadow.player.anim_table;
    if (!pl || !tabp) return;
    uint32_t table = cpu->read_word(tabp);
    if (table < 0x80000000u || table >= 0x80800000u) return;
    uint32_t sel = cpu->read_word(pl + game_layout.shadow.player.avatar_anim);
    int bi = (int)cpu->read_byte(pl + game_layout.shadow.player.avatar_blend);
    int w  = (int)cpu->read_byte(game_layout.shadow.player.blend_table + (uint32_t)bi) << 4;

    uint8_t silA[PLSH_SIL_N], silB[PLSH_SIL_N];
    if (!plsh_frame_bytes(cpu, table, sel, 0, 16, silA)) return;
    double sil[PLSH_SIL_N + 1];
    if (w != 0 && plsh_frame_bytes(cpu, table, sel, 8, 24, silB)) {
        for (int i = 0; i < PLSH_SIL_N; i++)
            sil[i] = (double)silA[i] + ((double)silB[i] - (double)silA[i]) * (double)w / 4096.0;
    } else {
        for (int i = 0; i < PLSH_SIL_N; i++) sil[i] = (double)silA[i];
    }
    sil[PLSH_SIL_N] = sil[0];                       /* the doubling loop's wrap */

    /* Double eight source radii into sixteen ring radii (point + midpoint). */
    const int three_quarter = cpu->read_word(st + PLSH_SCALE_FLAG) != 0;
    double ring_r[PLSH_RING_N];
    for (int i = 0; i < PLSH_SIL_N; i++) {
        double r0 = sil[i], r1 = sil[i + 1];
        if (three_quarter) { r0 = r0 * 3.0 / 4.0; r1 = r1 * 3.0 / 4.0; }
        ring_r[2 * i]     = r0;
        ring_r[2 * i + 1] = (r0 + r1) / 2.0;
    }

    /* Project the ring: radius k scales the packed direction pair, the local Y
     * stays 0 because the plane's tilt is already in R. Float (not the .s's
     * >>10 floor) keeps the corner off the one-unit grid. */
    const uint32_t dirs = game_layout.shadow.player.ring_dirs;
    shadow_pt strip[PLSH_RING_N];
    for (int k = 0; k < PLSH_RING_N; k++) {
        uint32_t d = dirs ? cpu->read_word(dirs + (uint32_t)k * 4u) : 0u;
        int dlo = shadow_s16((int)(d & 0xFFFFu)), dhi = shadow_s16((int)(d >> 16));
        shadow_project(R, TR, ofx, ofy, v,
                       ring_r[k] * dhi / 1024.0, 0.0, ring_r[k] * dlo / 1024.0, &strip[k]);
    }

    /* Fan: (anchor, strip[k], strip[k+1]). The routine drops a triangle whose
     * ordering key falls below the table, which is what keeps a fan that has
     * swallowed the camera from painting over the frame; reproduce it so the
     * fed count is the count the guest linked. */
    const int32_t bias = (int32_t)cpu->read_word(st + PLSH_OT_BIAS);
    const int sa = plsh_sz3(anchor.sz);
    for (int k = 0; k < PLSH_RING_N; k++) {
        shadow_pt* p1 = &strip[k];
        shadow_pt* p2 = &strip[(k + 1) & (PLSH_RING_N - 1)];
        uint32_t key = (uint32_t)(plsh_sz3(p1->sz) + plsh_sz3(p2->sz));
        key += key >> 1;
        key += (uint32_t)sa;
        if ((int32_t)(key >> 9) - bias < 0) continue;
        plshtri* t = &s_tri[s_tri_n++];
        t->sx[0] = anchor.sx; t->sy[0] = anchor.sy; t->sz[0] = anchor.sz; t->rgb[0] = 0x608080u;
        t->sx[1] = p1->sx;    t->sy[1] = p1->sy;    t->sz[1] = p1->sz;    t->rgb[1] = 0;
        t->sx[2] = p2->sx;    t->sy[2] = p2->sy;    t->sz[2] = p2->sz;    t->rgb[2] = 0;
    }
}

/* Feed the native player shadow (untextured G3 fan, ABR mode 2 -> blend 3).
 * `slot`: the model record the shadow renders under, the PLAYER slot of its
 * scene tag. */
/* OPENPETE_SHADOW_PLAYER=0 suppresses this leg's feed alone, leaving the moby
 * drop-shadows on the same render path: the A/B that prices the fan's painted
 * pixels without a second binary. The guest draw still runs. */
static int shadow_player_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_PLAYER"); v = !(e && *e && *e == '0'); }
    return v;
}

static void shadow_player_feed_native(CPUState* cpu, int slot) {
    if (!shadow_player_feed_enabled()) return;
    shadow_player_build(cpu);
    if (s_tri_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_SHADOW(OPENPETE_TAG_SHADOW_PLAYER,
                                                      (uint32_t)(slot < 0 ? 0 : slot)));
    for (int i = 0; i < s_tri_n; i++) {
        plshtri* c = &s_tri[i];
        openpete_scene_tri st = {0};
        float bias = shadow_bias_spyro();
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = c->sx[k]; st.v[k].sy = c->sy[k];
            st.v[k].z  = shadow_depth(c->sz[k], 1.0f, bias);   /* already full-world MAC3 */
            st.v[k].u = 0.0f; st.v[k].v = 0.0f;
            st.v[k].rgba = (c->rgb[k] & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
    openpete_native_scene_set_tag(0);
}

/* ---- verify --------------------------------------------------------------- */
static void shadow_player_dump(CPUState* cpu) {
    const uint32_t st = game_layout.shadow.player.state;
    const uint32_t pl = game_layout.player.base;
    fprintf(stderr, "\n=== [shadow-player-dump f=%u] ===\n", openpete_frame);
    fprintf(stderr, "  state %08X: flag=%08X pos=(%d,%d,%d) rot=%08X suppress=%u bias=%d\n",
            st, cpu->read_word(st + PLSH_SCALE_FLAG),
            (int)cpu->read_word(st + PLSH_POS + 0), (int)cpu->read_word(st + PLSH_POS + 4),
            (int)cpu->read_word(st + PLSH_POS + 8), cpu->read_word(st + PLSH_ROT),
            cpu->read_byte(st + PLSH_SUPPRESS), (int)cpu->read_word(st + PLSH_OT_BIAS));
    fprintf(stderr, "  avatar %08X: anim=%08X blend byte=%u -> %u   anim table=%08X\n",
            pl, cpu->read_word(pl + game_layout.shadow.player.avatar_anim),
            cpu->read_byte(pl + game_layout.shadow.player.avatar_blend),
            cpu->read_byte(game_layout.shadow.player.blend_table +
                           cpu->read_byte(pl + game_layout.shadow.player.avatar_blend)),
            cpu->read_word(game_layout.shadow.player.anim_table));
    fprintf(stderr, "  camera pos = (%d, %d, %d)\n",
            (int)cpu->read_word(game_layout.cam.position + 0),
            (int)cpu->read_word(game_layout.cam.position + 4),
            (int)cpu->read_word(game_layout.cam.position + 8));
    shadow_dump_prims("shadow-player");
    fprintf(stderr, "=== end shadow-player-dump ===\n\n");
}

/* A/B: the native fan against the captured G3 prims (the .s int16 SXY oracle).
 * Same fan order, so compare element-wise. Reports VACUOUS when the routine
 * emitted no untextured triangle: the two negative-control rows read here as
 * native=0 G3=0, which is the count the row asserts, not a pass. */
static void shadow_player_compare(CPUState* cpu) {
    shadow_player_build(cpu);
    int np = shadow_prim_count(), g3 = 0;
    for (int p = 0; p < np; p++) {
        const shadow_prim* pr = shadow_prim_at(p);
        if (!pr->textured && pr->nv == 3) g3++;
    }
    if (g3 == 0 || s_tri_n == 0) {
        fprintf(stderr, "[shadow-player f=%u] VACUOUS native=%d G3=%d (nothing compared)\n",
                openpete_frame, s_tri_n, g3);
        return;
    }
    int n = s_tri_n < g3 ? s_tri_n : g3;
    double worst = 0, sum = 0; int over2 = 0, nv = 0, qi = 0, cbad = 0;
    for (int p = 0; p < np && qi < n; p++) {
        const shadow_prim* pr = shadow_prim_at(p);
        if (!(!pr->textured && pr->nv == 3)) continue;
        plshtri* c = &s_tri[qi];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = c->sx[k] - pr->x[k], dy = c->sy[k] - pr->y[k];
            double d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            sum += d; nv++; if (d > tw) tw = d;
            if (c->rgb[k] != pr->rgb[k]) cbad++;
        }
        if (tw > worst) worst = tw;
        if (tw > 2.0) over2++;
        qi++;
    }
    /* The fan's own screen box and depth window, native against the captured
     * prims: a fan that agrees per-vertex yet paints nothing is either outside
     * the frame or outside the pipeline's depth range, and those two are the
     * numbers that tell them apart. */
    float nx0 = 1e9f, nx1 = -1e9f, ny0 = 1e9f, ny1 = -1e9f;
    float gx0 = 1e9f, gx1 = -1e9f, gy0 = 1e9f, gy1 = -1e9f;
    float dz0 = 1e9f, dz1 = -1e9f, area = 0.0f;
    const float bias = shadow_bias_spyro();
    for (int i = 0; i < s_tri_n; i++) {
        const plshtri* c = &s_tri[i];
        for (int k = 0; k < 3; k++) {
            if (c->sx[k] < nx0) nx0 = c->sx[k];
            if (c->sx[k] > nx1) nx1 = c->sx[k];
            if (c->sy[k] < ny0) ny0 = c->sy[k];
            if (c->sy[k] > ny1) ny1 = c->sy[k];
            float z = shadow_depth(c->sz[k], 1.0f, bias);
            if (z < dz0) dz0 = z;
            if (z > dz1) dz1 = z;
        }
        area += (c->sx[1]-c->sx[0])*(c->sy[2]-c->sy[0]) - (c->sx[2]-c->sx[0])*(c->sy[1]-c->sy[0]);
    }
    for (int p = 0; p < np; p++) {
        const shadow_prim* pr = shadow_prim_at(p);
        if (pr->textured || pr->nv != 3) continue;
        for (int k = 0; k < 3; k++) {
            if (pr->x[k] < gx0) gx0 = pr->x[k];
            if (pr->x[k] > gx1) gx1 = pr->x[k];
            if (pr->y[k] < gy0) gy0 = pr->y[k];
            if (pr->y[k] > gy1) gy1 = pr->y[k];
        }
    }
    fprintf(stderr, "[shadow-player f=%u] native=%d G3=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx "
                    "over2px=%d/%d rgbMismatch=%d nbox=x[%.0f,%.0f]y[%.0f,%.0f] "
                    "gbox=x[%.0f,%.0f]y[%.0f,%.0f] depth=[%.5f,%.5f] 2A=%.0f\n",
            openpete_frame, s_tri_n, g3, n, nv ? sum / nv : 0, worst, over2, n, cbad,
            nx0, nx1, ny0, ny1, gx0, gx1, gy0, gy1, dz0, dz1, area);
}

/* ---- hook ----------------------------------------------------------------- */
static hook_bracket_state s_hb;
static const hook_bracket_desc k_bracket = {
    .path = RP_SHADOW, .prof_tag = "shadow.srend",
    .verify_enabled = shadow_verify_enabled,
    .st = &s_hb,
};

static int on_player_shadow(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_bracket, &startp, &endp, &feed, &verify))
        return 0;

    int slot = 0;
    if (endp > startp) {
        int m = player_records_shadow_append(cpu);   /* bound to its model record */
        if (m >= 0) slot = m;
    }

    if (feed || verify) shadow_capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_SHADOW))
        shadow_player_feed_native(cpu, slot);

    if (verify && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped && endp > startp) { shadow_player_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) { shadow_player_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_bracket);
}

/* Sub-tick re-extract: one shadow record, its state block already staged by
 * the record replay, so the anchor reads the bracketed position. */
void shadow_player_reextract(CPUState* cpu, int slot) {
    if (!hook_reextract_ready(RP_SHADOW)) return;
    shadow_player_feed_native(cpu, slot);
}

/* The hook address is the layout's player-shadow draw site, bound at boot: a
 * static initializer cannot read another TU's const. */
static feature_dispatch_entry_t shadow_player_dispatch[] = {
    { 0u, on_player_shadow },
    { 0u, NULL },
};

static void shadow_player_apply_options(const openpete_options_t* opts) {
    (void)opts;
    shadow_player_dispatch[0].addr = game_layout.shadow.player.hook;
}

const feature_module_t shadow_player_module = {
    .name           = "shadow_player_native",
    .apply_options  = shadow_player_apply_options,
    .dispatch_table = shadow_player_dispatch,
};
