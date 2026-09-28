// games/common/renderer/shadow.c — the MOBY DROP-SHADOW renderer re-derived
// from gamestate in float, plus its A/B harness. Every engine address and
// every decode switch comes from game_layout.shadow; the game's own shadow
// draw runs through the recomp module dispatch at game_layout.shadow.hook.
//
// The draw is a textured FT3 fan (GPU code 0x26) per drawn moby: a ground
// anchor projected from the moby's position and its packed ground word, four
// diamond corners tilted to the ground normal by two 6-bit angles out of the
// same word, four triangles fanned from the anchor, and a grey that fades
// with camera distance. The semi-transparency mode is the ABR field of the
// shadow sprite's tpage word, which is level data, so the blend is read per
// level rather than assumed.
//
// Structure of the game's shadow routine:
//   * the descriptor's list terminator is at +0x08, after the two UV words.
//   * the shadow is 4 separate FT3 tris fanned from the ground anchor (not
//     one quad); word 1 of the 8-byte drawn-moby list record is the shadow
//     radius/scale, not an index. The moby's position and ground word are
//     read DIRECTLY from the Moby, not from any render record.
//   * the corner order is V0 = (0, 0, +r), V1 = (r.lo, r.hi, 0),
//     V2 = (0, 0, -r), V3 = (-r, 0, 0), the first three by one RTPT and the
//     fourth by a further RTPS.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to normal dispatch) then only READS state afterwards.
// Gated by OPENPETE_SHADOW_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/shadow.h"
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "psx_runtime_internal.h"   /* full CPUState (gte_data, read_word, ..) */
#include "openpete_native_render.h"
#include "openpete_recomp_module.h" /* the game's own shadow draw, through the module dispatch */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/orchestrator.h"  /* openpete_native_interp_pos */
#include "renderer/moby.h"        /* the record own-motion offsets */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"
#include "openpete_profile.h"       /* OP_PROF_ACCUM_SCOPE — fiber-hook cost split */
#include "renderer/env_knob.h"

extern unsigned openpete_frame;

/* shadow_s16 and shadow_rd_s16 are documented in shadow.h. */
int shadow_s16(int v) { return (int)(int16_t)(uint32_t)v; }
int shadow_rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}

/* ---- captured prims (ground truth) --------------------------------------- */
#define MAX_PRIMS 4096
static shadow_prim s_gte[MAX_PRIMS]; static int s_gte_n;
static uint32_t s_code_hist[256];

/* shadow_prim_count and shadow_prim_at are documented in shadow.h. */
int                shadow_prim_count(void) { return s_gte_n; }
const shadow_prim* shadow_prim_at(int i)   { return (i >= 0 && i < s_gte_n) ? &s_gte[i] : NULL; }

/** @brief Stores one walked primitive into the capture buffer.
 *
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored; 0 when the buffer's 4096 entries are full. */
static int shadow_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_gte_n >= MAX_PRIMS) return 0;
    shadow_prim* t = &s_gte[s_gte_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->len = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud; t->quad = s->quad;
    memcpy(t->raw, s->raw, sizeof(t->raw));
    t->nraw = s->nraw;
    s_gte_n++;
    return 1;
}

/* Walks the polybuf into s_gte; see shadow.h. */
void shadow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("shadow.capture");
    s_gte_n = 0;
    memset(s_code_hist, 0, sizeof(s_code_hist));
    const ngte_walk_policy pol = {
        .tag_len_max = 24, .raw_words_max = 20, .guard = MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = NULL,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, shadow_prim_sink, NULL);
}

void shadow_dump_prims(const char* tag) {
    fprintf(stderr, "  [%s] prim code histogram:", tag);
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%u", c, s_code_hist[c]);
    fprintf(stderr, "  (total=%d)\n", s_gte_n);
    int n = s_gte_n < 6 ? s_gte_n : 6;
    for (int p = 0; p < n; p++) {
        shadow_prim* t = &s_gte[p];
        fprintf(stderr, "    prim[%d] code=%02X len=%d nv=%d tex=%d gour=%d  raw:",
                p, t->code, t->len, t->nv, t->textured, t->gouraud);
        for (int i = 0; i < t->nraw; i++) fprintf(stderr, " %08X", t->raw[i]);
        fprintf(stderr, "\n      decoded:");
        for (int k = 0; k < t->nv; k++)
            fprintf(stderr, " v%d(xy=%.0f,%.0f rgb=%06X uv=%d,%d)",
                    k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
        if (t->textured) fprintf(stderr, "  clut=%04X tpage=%04X", t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
}

/* ---- the drawn-moby list base -------------------------------------------- */
/* SHADOW_LIST_GTE_DATA carries the base in a GTE data register the moby draw
 * left behind; the shadow draw overwrites the GTE data registers as it runs,
 * so the base is latched at hook entry and reused by the sub-tick re-extract. */
static uint32_t s_list_base;

/* The list records, copied at hook entry: a game that keeps the list in the
 * primitive buffer's tail overwrites it later in the tick, so that source
 * walks this copy; a fixed-address list is re-read live at each walk. */
#define SHADOW_LIST_MAX 1024
static struct { uint32_t moby; uint32_t size; } s_list[SHADOW_LIST_MAX];
static int s_list_n;

/** @brief Latches the drawn-moby list base and copies the list records.
 *
 * The base is the GTE data register game_layout.shadow.list_reg plus the list
 * offset, or the layout's fixed address. A register index outside 0..31 empties
 * the list. Copies up to SHADOW_LIST_MAX 8-byte records from the base up to the
 * end pointer at descriptor + 0x08; without a descriptor or base the copy is
 * empty.
 * @param cpu The guest CPU state. */
static void shadow_latch_list_base(CPUState* cpu) {
    if (game_layout.shadow.list_source == SHADOW_LIST_GTE_DATA) {
        int r = game_layout.shadow.list_reg;
        if (r < 0 || r >= 32) { s_list_base = 0; s_list_n = 0; return; }
        s_list_base = cpu->gte_data[r] + game_layout.shadow.list_offset;
    } else {
        s_list_base = game_layout.shadow.list_base;
    }
    s_list_n = 0;
    const uint32_t desc = game_layout.shadow.desc;
    if (!desc || !s_list_base) return;
    uint32_t list_end = cpu->read_word(desc + 0x08);
    for (uint32_t e = s_list_base; e + 8 <= list_end && s_list_n < SHADOW_LIST_MAX; e += 8) {
        s_list[s_list_n].moby = cpu->read_word(e + 0);
        s_list[s_list_n].size = cpu->read_word(e + 4);
        s_list_n++;
    }
}

/* ---- moby-shadow dump ----------------------------------------------------- */
/** @brief Prints the moby-shadow descriptor, the camera and the first six drawn-moby list entries, then the capture.
 *
 * The descriptor line decodes the tpage and its ABR from uv1's high half; each
 * list entry prints its moby's position and packed ground word (ground
 * height, idxB, idxA), or "bad ptr" for an entry outside main RAM. The list is
 * read live from the latched base.
 * @param cpu The guest CPU state. */
static void shadow_moby_dump(CPUState* cpu) {
    const uint32_t desc = game_layout.shadow.desc;
    fprintf(stderr, "\n=== [shadow-moby-dump f=%u] ===\n", openpete_frame);
    uint32_t uv0 = cpu->read_word(desc + 0x00);
    uint32_t uv1 = cpu->read_word(desc + 0x04);
    uint32_t list_end = cpu->read_word(desc + 0x08);
    fprintf(stderr, "  descriptor: uv0=%08X uv1=%08X shadow_list(end)=%08X  tpage=%04X abr=%d\n",
            uv0, uv1, list_end, (unsigned)(uv1 >> 16), (int)((uv1 >> 21) & 3u));
    fprintf(stderr, "  camera matrix (+0x00):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(game_layout.cam.base + (uint32_t)i*4));
    fprintf(stderr, "\n  camera pos = (%d, %d, %d)\n",
            (int)cpu->read_word(game_layout.cam.position + 0),
            (int)cpu->read_word(game_layout.cam.position + 4),
            (int)cpu->read_word(game_layout.cam.position + 8));
    fprintf(stderr, "  drawn-moby list [%08X, %08X):\n", s_list_base, list_end);
    int shown = 0;
    for (uint32_t e = s_list_base; s_list_base && e < list_end && shown < 6; e += 8, shown++) {
        uint32_t mob = cpu->read_word(e + 0);
        uint32_t v1  = cpu->read_word(e + 4);
        if (mob < 0x80000000u || mob >= 0x80800000u) { fprintf(stderr, "    [%d] mob=%08X v1=%08X (bad ptr)\n", shown, mob, v1); continue; }
        int px = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 0);
        int py = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 4);
        int pz = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 8);
        uint32_t sd = cpu->read_word(mob + game_layout.shadow.moby_ground);
        fprintf(stderr, "    [%d] mob=%08X entry1=%08X  pos=(%d,%d,%d) ground=%08X(z=%d idxB=%d idxA=%d)\n",
                shown, mob, v1, px, py, pz, sd, sd & 0xFFFF, (sd >> 16) & 0x3F, (sd >> 22) & 0x3F);
    }
    shadow_dump_prims("shadow-moby");
    fprintf(stderr, "=== end shadow-moby-dump ===\n\n");
}

/* ===========================================================================
 *  NATIVE WALK — re-derive the moby shadows from gamestate in float.
 *  Reproduces the per-moby ground-point projection, the oriented-quad matrix
 *  assembly (two MVMVA passes per packed orientation index), the 4 fan corners
 *  and the distance fade, projecting in float so the geometry carries
 *  sub-pixel/HD precision and sub-tick interpolation. The VERIFY path compares
 *  the SXY shape against the captured FT3 prims (the .s int16 output as the
 *  oracle).
 * =========================================================================== */
/** @brief Saturates `v` to -32768..32767.
 *
 * @param v The value.
 * @return The saturated value. */
static int clamp16i(long v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int)v); }

void shadow_mvmva_ir(const int R[3][3], int vx, int vy, int vz, int ir[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t m = (int64_t)R[i][0]*vx + (int64_t)R[i][1]*vy + (int64_t)R[i][2]*vz;
        ir[i] = clamp16i((long)(m >> 12));
    }
}

void shadow_ground_tr(const int C[3][3], double vx, double vy, double vz, double TR[3]) {
    ngte_mac(C, vx, vy, vz, &TR[0], &TR[1], &TR[2]);
}

void shadow_project(const int R[3][3], const double TR[3], double ofx, double ofy,
                    const ngte_view* v, double vx, double vy, double vz, shadow_pt* o) {
    ngte_proj r;   /* shared GTE-faithful pipeline, ±1024 SXY bound ON */
    ngte_project_gte(R, TR, vx, vy, vz, ofx, ofy, v, 1, &r);
    o->sx = (float)r.sx; o->sy = (float)r.sy; o->sz = (float)r.m3;
    o->m1 = (float)r.m1; o->m2 = (float)r.m2;
}

/** @brief Signed twice-area of a screen triangle on the float SXY, the sign of GTE NCLIP's MAC0.
 *
 * The GTE's integer SXY is not applied: rounding flips a near-zero-area fan's
 * sign at grazing angles, which culls the whole moby drop-shadow.
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @return The signed twice-area; negative is a back face. */
static double sh_nclip(const shadow_pt* a, const shadow_pt* b, const shadow_pt* c) {
    return ngte_nclip(a->sx, a->sy, b->sx, b->sy, c->sx, c->sy, NGTE_NCLIP_FLOAT);
}

/** @brief Assembles the oriented-quad matrix R' from the two packed orientation indices.
 *
 * A nonzero idxB replaces the camera matrix's columns 1 and 2 with
 * C·(0, cosB, sinB) and C·(0, -sinB, cosB); a nonzero idxA then replaces
 * columns 0 and 1 of that result with R'·(cosA, sinA, 0) and
 * R'·(-sinA, cosA, 0). Each product is shadow_mvmva_ir. The sine of index i
 * is the halfword at game_layout.sin_table + i*8, its cosine 0x80 bytes on.
 *
 * game_layout.shadow.matrix_fold_a selects the idxB block's handwritten
 * variant: it discards the new column-1 X term and instead adds idxA*8 into
 * the R11/R12 word, so R12 becomes 0 (or the carry of that addition) and R11
 * is biased before the idxA rotation reads it.
 * @param cpu The guest CPU state; supplies the sine table.
 * @param C The camera matrix, Q12.
 * @param idxA The A index, bits 22..27 of the ground word.
 * @param idxB The B index, bits 16..21 of the ground word.
 * @param Rp Receives R'. */
static void assemble_matrix(CPUState* cpu, const int C[3][3], int idxA, int idxB, int Rp[3][3]) {
    const uint32_t sinb = game_layout.sin_table;
    memcpy(Rp, C, sizeof(int)*9);
    if (idxB != 0) {
        int sinB = shadow_rd_s16(cpu, sinb + (uint32_t)idxB*8);
        int cosB = shadow_rd_s16(cpu, sinb + (uint32_t)idxB*8 + 0x80);
        int P[3], Q[3];
        shadow_mvmva_ir(Rp, 0,  cosB, sinB, P);     /* C·(0, cosB, sinB) */
        shadow_mvmva_ir(Rp, 0, -sinB, cosB, Q);     /* C·(0, -sinB, cosB) */
        int n[3][3];
        if (game_layout.shadow.matrix_fold_a) {
            int packed11 = (Rp[0][0] & 0xFFFF) + (idxA << 3);
            n[0][0]=shadow_s16(packed11 & 0xFFFF); n[0][1]=shadow_s16((packed11>>16)&0xFFFF);
        } else {
            n[0][0]=Rp[0][0];               n[0][1]=P[0];
        }
        n[0][2]=Q[0];
        n[1][0]=Rp[1][0];               n[1][1]=P[1];                       n[1][2]=Q[1];
        n[2][0]=Rp[2][0];               n[2][1]=P[2];                       n[2][2]=Q[2];
        memcpy(Rp, n, sizeof n);
    }
    if (idxA != 0) {
        int sinA = shadow_rd_s16(cpu, sinb + (uint32_t)idxA*8);
        int cosA = shadow_rd_s16(cpu, sinb + (uint32_t)idxA*8 + 0x80);
        int P[3], Q[3];
        shadow_mvmva_ir(Rp,  cosA, sinA, 0, P);     /* Rp·(cosA, sinA, 0) */
        shadow_mvmva_ir(Rp, -sinA, cosA, 0, Q);     /* Rp·(-sinA, cosA, 0) */
        int n[3][3];
        n[0][0]=P[0];     n[0][1]=Q[0];     n[0][2]=Rp[0][2];
        n[1][0]=P[1];     n[1][1]=Q[1];     n[1][2]=Rp[1][2];
        n[2][0]=P[2];     n[2][1]=Q[2];     n[2][2]=Rp[2][2];
        memcpy(Rp, n, sizeof n);
    }
}

/** @brief 4096² · C⁻¹ via the adjugate: the rows the decal reconstructs a camera-relative GTE input vector with.
 *
 * A view-space point's input vector is V_j = (inv[j] · MAC) / 4096. A
 * determinant within 1e-6 of zero is replaced by ±1e-6, keeping its sign.
 * @param C The camera matrix, Q12.
 * @param inv Receives the scaled inverse. */
static void shadow_cam_inverse(const int C[3][3], double inv[3][3]) {
    double a = C[0][0], b = C[0][1], c = C[0][2];
    double d = C[1][0], e = C[1][1], f = C[1][2];
    double g = C[2][0], hh = C[2][1], ii = C[2][2];
    double det = a*(e*ii - f*hh) - b*(d*ii - f*g) + c*(d*hh - e*g);
    if (det > -1e-6 && det < 1e-6) det = det < 0.0 ? -1e-6 : 1e-6;
    double k = 4096.0 * 4096.0 / det;
    const double r[3][3] = {
        { (e*ii - f*hh) * k, (c*hh - b*ii) * k, (b*f - c*e) * k },
        { (f*g  - d*ii) * k, (a*ii - c*g)  * k, (c*d - a*f) * k },
        { (d*hh - e*g)  * k, (b*g  - a*hh) * k, (a*e - b*d) * k },
    };
    memcpy(inv, r, sizeof r);
}

/* Pushes the shadow decal's camera-reconstruction parameters; see shadow.h. */
void shadow_push_decal_params(CPUState* cpu, const ngte_view* v, const int C[3][3],
                              double ofx, double ofy, double H,
                              openpete_shadow_decal_params* out) {
    openpete_shadow_decal_params sp;
    openpete_shadow_decal_params* p = &sp;
    double inv[3][3];
    shadow_cam_inverse(C, inv);
    for (int i = 0; i < 3; i++) {
        p->cam_col0[i] = (float)inv[0][i];
        p->cam_col1[i] = (float)inv[1][i];
        p->cam_col2[i] = (float)inv[2][i];
    }
    p->cam_pos_up = (float)v->cam_ram[2];                                  /* game z field */
    p->ofx = (float)ofx; p->ofy = (float)ofy; p->h = (float)H;
    p->ground_up = game_layout.shadow.ground_plane
                 ? (float)(int32_t)cpu->read_word(game_layout.shadow.ground_plane) : 0.0f;
    const char* wh = getenv("OPENPETE_SHADOW_WALL_HEIGHT");
    p->wall_height = (wh && *wh) ? (float)atof(wh) : 400.0f;
    /* |up normal| threshold below which a surface is a wall face (walkable
     * slopes stay >~0.64; wall faces ~0). OPENPETE_SHADOW_STEEP_NY=0 = height-only. */
    const char* sn = getenv("OPENPETE_SHADOW_STEEP_NY");
    p->steep_nup = (sn && *sn) ? (float)atof(sn) : 0.5f;
    /* Raised-occluder-in-front (curb/platform) clause; PLAT_EPS=1e5 disables. */
    const char* pe = getenv("OPENPETE_SHADOW_PLAT_EPS");
    const char* ps = getenv("OPENPETE_SHADOW_PLAT_SLACK");
    p->plat_eps = (pe && *pe) ? (float)atof(pe) : 150.0f;
    p->plat_slack = (ps && *ps) ? (float)atof(ps) : 48.0f;
    p->valid = 1;
    openpete_native_set_shadow_decal_params(p);
    if (out) *out = *p;
}

/** @brief Rounds a view-Z to the nearest integer clamped to 0..0xFFFF.
 *
 * @param sz The view-Z.
 * @return The clamped value. */
static uint32_t shadow_sz16(float sz) {
    long z = lrintf(sz);
    return z < 0 ? 0u : (z > 0xFFFF ? 0xFFFFu : (uint32_t)z);
}

int shadow_ot_slot(float sza, float sz1, float sz2, int shift, int bias) {
    uint32_t key = shadow_sz16(sz1) + shadow_sz16(sz2);
    key += key >> 1;
    key += shadow_sz16(sza);
    return (int)(key >> shift) - bias;
}

float shadow_slot_koff(int slot) {
    return slot < 0 ? 0.0f : (float)slot * 128.0f + 0.5f;
}

#define MOBY_SHADOW_SKIP (-2)
/** @brief The ordering-table slot of one moby shadow triangle (anchor, p1, p2).
 *
 * The slot is the quarter-world key >> 7 less the moby's signed depth byte
 * less the game's constant. Returns -1 (not keyed) on a game whose layout
 * does not mirror it (moby_ot_bias 0). A mirrored slot below zero is returned
 * as MOBY_SHADOW_SKIP: the game does not insert that triangle (the bltz after
 * the subtraction, Spyro 1 r_shadows.s 0x8005A414, Spyro 2 mobyfunc.s
 * 0x80049E7C, Spyro 3 mobyfunc.s 0x8002E250), so the feed does not draw it;
 * the build keeps it, where the GTE A/B compares four triangles per moby.
 * @param cpu The guest CPU state.
 * @param mob The moby's address.
 * @param anchor The projected ground anchor.
 * @param p1 The fan triangle's second vertex.
 * @param p2 The fan triangle's third vertex.
 * @return The slot, -1, or MOBY_SHADOW_SKIP. */
static int moby_shadow_slot(CPUState* cpu, uint32_t mob, const shadow_pt* anchor,
                            const shadow_pt* p1, const shadow_pt* p2) {
    const uint32_t off = game_layout.shadow.moby_ot_bias;
    if (!off) return -1;
    int bias = (int)(int8_t)cpu->read_byte(mob + off) - game_layout.shadow.moby_ot_bias_sub;
    int slot = shadow_ot_slot(anchor->sz, p1->sz, p2->sz, 7, bias);
    return slot < 0 ? MOBY_SHADOW_SKIP : slot;
}

/* native moby-shadow tris (4 FT3 fan tris per moby) */
typedef struct { float sx[3], sy[3], sz[3]; int u[3], v[3]; uint32_t rgb;
                 int tpx, tpy, clx, cly, bpp, blend, slot; } moshtri;
#define MAX_MOSH 1024
static moshtri s_mosh[MAX_MOSH]; static int s_mosh_n;

/** @brief Packs a projected point as the GTE's (SY << 16) | (SX & 0xFFFF) word.
 *
 * The screen-extent rejections below are integer tests on that packed word,
 * so the float corners are packed back to it first. Each component is
 * MAC0 >> 16, an arithmetic shift, which rounds toward negative infinity, so
 * the float is floored and not rounded to nearest: a corner whose true X lies
 * in (-1, 0) packs as -1 with its sign bit set, and that sign bit is what the
 * left-of-column-0 rejection reads.
 * @param p The point.
 * @return The packed word. */
static int32_t packed_sxy(const shadow_pt* p) {
    int xi = (int)floorf(p->sx), yi = (int)floorf(p->sy);
    return (int32_t)(((uint32_t)yi << 16) | ((uint32_t)xi & 0xFFFFu));
}

/** @brief The anchor-extent rejection, used when the layout's cull form is not the four-corner one.
 *
 * The anchor alone must be on screen. Its packed word is rejected when it is
 * at most 0 (a negative SY, or SX and SY both 0) or at least 0xF00000, and
 * its sign-extended SX when it is at most -8 or at least 0x208.
 * @param anchor The projected anchor.
 * @return 1 to reject the shadow, else 0. */
static int cull_anchor(const shadow_pt* anchor) {
    int32_t packed = packed_sxy(anchor);
    if (packed <= 0) return 1;
    if (packed - 0xF00000 >= 0) return 1;
    int xext = shadow_s16((int)((uint32_t)packed & 0xFFFFu));
    if (xext + 8 <= 0) return 1;
    if (xext - 0x208 >= 0) return 1;
    return 0;
}

/** @brief The four-corner rejection of SHADOW_CULL_SPYRO3 layouts.
 *
 * Rejects when every corner is above the screen, when corners 1..3 are all at
 * or below row 0xE4, when corners 1..3 are all left of column 0, or when
 * every corner is at or right of column 0x200 and not every corner is
 * exactly at it. Corner 0 is absent from the second and third tests and the
 * first bound is folded into the running OR: the handwritten routine keeps
 * the constant in the register it accumulates into, so those two tests answer
 * for three corners only.
 * @param c The four projected corners.
 * @return 1 to reject the shadow, else 0. */
static int cull_corners(const shadow_pt c[4]) {
    int32_t p[4], xs[4];
    for (int k = 0; k < 4; k++) {
        p[k]  = packed_sxy(&c[k]);
        xs[k] = (int32_t)((uint32_t)p[k] << 16);      /* X into the high half */
    }
    if ((p[0] & p[1] & p[2] & p[3]) < 0) return 1;
    const int32_t ybound = 0x00E40000;
    int32_t ory = ybound | (p[1] - ybound) | (p[2] - ybound) | (p[3] - ybound);
    if (ory >= 0) return 1;
    if ((ory & xs[1] & xs[2] & xs[3]) < 0) return 1;
    const int32_t xbound = 0x02000000;
    int32_t orx = (xs[0] - xbound) | (xs[1] - xbound) | (xs[2] - xbound) | (xs[3] - xbound);
    if (orx > 0) return 1;
    return 0;
}

/** @brief Reports whether the moby-shadow walk applies the record own-motion offsets; OPENPETE_SHADOW_INTERP=0 turns them off.
 *
 * Latched on the first call. Off, every entry the spine does not interpolate
 * stays at the newest tick's world pose. It is the engagement leg of
 * tests/harness/check_feeder_subtick.sh --path shadow: the harness reads the
 * whole tick step back under it.
 * @return 1 when on, else 0. */
static int shadow_interp_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_INTERP"); v = !(e && *e && *e == '0'); }
    return v;
}

/* The world pose of one drawn-moby list entry; see shadow_entry_world. */
typedef struct { double px, py, gnd; uint32_t sd; } shadow_entry_pose;

/** @brief Reads the world pose one drawn-moby list entry projects from.
 *
 * The pose is the moby's position in world units and the low halfword of its
 * packed ground word, the ground height in quarter-world units
 * (mobyfunc.s:930-933 reads all three straight out of the Moby, not out of any
 * render record).
 *
 * Those live reads are the state the newest tick left, so a present under an
 * interpolated camera would place a moving moby's shadow one whole tick ahead
 * of the world it falls on. Two sources carry the entry back onto the pose the
 * present shows. A game whose interpolation spine answers for this object
 * hands over its lerped position directly. Otherwise the record own-motion
 * offsets do it: moby_record_world_offset on the position, and its sibling on
 * the ground halfword, which a moby crossing a step in the terrain changes as
 * fast as it changes its position. Both are zero at the end of the tick's own
 * interval and neither is read under the tick's own camera, so the canonical
 * feed is the unmodified one.
 *
 * The entry's radius word and the list membership stay the tick's: both live
 * in the list record rather than in the Moby, and no per-tick copy of the list
 * exists to pair them against.
 *
 * @param cpu The guest CPU state.
 * @param v The projection view; supplies the camera-interpolation flag.
 * @param mob The moby's address.
 * @param off_pos The position's offset in the Moby.
 * @param off_gnd The packed ground word's offset in the Moby.
 * @param e Receives the ground word, the position and the ground height.
 * @return 0 when the ground halfword is zero, the game's own verdict that this
 *         moby casts no shadow, else 1. */
static int shadow_entry_world(CPUState* cpu, const ngte_view* v, uint32_t mob,
                              uint32_t off_pos, uint32_t off_gnd,
                              shadow_entry_pose* e) {
    e->sd = cpu->read_word(mob + off_gnd);
    e->gnd = (double)(int)(e->sd & 0xFFFFu);
    if (e->gnd == 0.0) return 0;
    e->px = (double)(int32_t)cpu->read_word(mob + off_pos + 0);
    e->py = (double)(int32_t)cpu->read_word(mob + off_pos + 4);
    double mf[3];
    if (openpete_native_interp_pos(mob, mf)) { e->px = mf[0]; e->py = mf[1]; return 1; }
    if (!v->cam_interp || !shadow_interp_on()) return 1;
    double mw[3];
    if (moby_record_world_offset(mob, mw)) { e->px += mw[0]; e->py += mw[1]; }
    double dg;
    if (moby_record_halfword_offset(mob, off_gnd, &dg)) e->gnd += dg;
    return 1;
}

/** @brief Rebuilds the moby drop-shadow triangles from the drawn-moby list.
 *
 * Empties s_mosh, then for each list record with a moby inside the first
 * 8 MiB and a nonzero ground word: projects the ground anchor and the four
 * diamond corners, rejects on the Z cull, the backface test and the
 * layout's screen-extent form, and stores four fan triangles (anchor, c[k],
 * c[k+1]) with the moby's grey, the shadow sprite's material and the ordering
 * slot, up to MAX_MOSH triangles. A list in the primitive buffer's tail is
 * walked from the copy taken at hook entry; a fixed-address list is re-read
 * here. Pushes the decal camera parameters. Reads PSX RAM only.
 * @param cpu The guest CPU state. */
static void shadow_moby_build(CPUState* cpu) {
    s_mosh_n = 0;
    const uint32_t desc = game_layout.shadow.desc;
    if (!desc || !s_list_base) return;
    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per walk */
    const ngte_view* v = &view;
    int C[3][3]; memcpy(C, v->cam_M, sizeof C);
    uint32_t list_end = cpu->read_word(desc + 0x08);
    uint32_t uv0 = cpu->read_word(desc + 0x00);
    uint32_t uv1 = cpu->read_word(desc + 0x04);
    if (game_layout.shadow.uv1_bottom_right) uv1 -= 0x1F00u;   /* bottom-right -> top-right */
    double ofx = v->ofx, ofy = v->ofy;
    double H   = v->H_eff;
    /* Hand the depth-aware decal the camera-reconstruction params so the TEXTURED
     * moby-shadow decal (shadow_decal_tex.frag) can reject wall/steep/raised
     * terrain per pixel. Set here (not only in a player-shadow feeder) so moby
     * shadows are depth-aware even when the player casts no shadow of his own.
     * Identical params to the player feeder's (same camera; ground_up read from
     * the layout), so feeder order is irrelevant. Display-only. */
    shadow_push_decal_params(cpu, v, C, ofx, ofy, H, NULL);
    /* The camera is taken in world/4 units as a fraction (not >>2), keeping the
     * low 2 bits: a floored /4 integer would step the shadow in 4-world-unit
     * chunks under camera motion. The view's interpolated camera is used
     * un-rounded, so the shadow also tracks it below the 1-world-unit grid. */
    double cam0 = v->cam[0] / 4.0, cam1 = v->cam[1] / 4.0, cam2 = v->cam[2] / 4.0;
    /* FT3 material (constant across the fan): UV0=uv0, UV1=uv1, UV2=uv0+0x1F00 */
    int u0=(int)(uv0&0xFF), v0t=(int)((uv0>>8)&0xFF);
    int u1=(int)(uv1&0xFF), v1t=(int)((uv1>>8)&0xFF);
    int u2=u0, v2t=(int)(((uv0>>8)&0xFF)+0x1F);
    int clut=(int)(uv0>>16), tpage=(int)(uv1>>16);
    ngte_tpage tp;
    ngte_decode_tpage((unsigned)tpage, (unsigned)clut, 0u, &tp);
    int tpx=tp.tpx, tpy=tp.tpy;
    int bpp=tp.bpp;
    /* The shadow sprite's semi-transparency rate is level data in the tpage's
     * ABR field, not a constant. Rate 3 (B+F/4) has no native pipeline. */
    int blend = ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD);
    int clx=tp.clx, cly=tp.cly;
    double ZERO[3] = {0,0,0};
    const uint32_t off_pos = game_layout.shadow.moby_pos;
    const uint32_t off_gnd = game_layout.shadow.moby_ground;
    const int size_shl = game_layout.shadow.size_shl;

    /* A list at a fixed scratch address stays the game's live list until
     * the next draw, and a sub-tick present reads it as it stands then; a
     * list in the primitive buffer's tail is overwritten later in the tick,
     * so that source walks the copy taken at hook entry. */
    if (game_layout.shadow.list_source != SHADOW_LIST_GTE_DATA) {
        s_list_n = 0;
        for (uint32_t e = s_list_base; e + 8 <= list_end && s_list_n < SHADOW_LIST_MAX; e += 8) {
            s_list[s_list_n].moby = cpu->read_word(e + 0);
            s_list[s_list_n].size = cpu->read_word(e + 4);
            s_list_n++;
        }
    }
    for (int li = 0; li < s_list_n; li++) {
        uint32_t mob = s_list[li].moby;
        uint32_t radius = s_list[li].size << size_shl;
        if (mob < 0x80000000u || mob >= 0x80800000u) continue;
        shadow_entry_pose ep;
        if (!shadow_entry_world(cpu, v, mob, off_pos, off_gnd, &ep)) continue;
        uint32_t sd = ep.sd;
        double vx = cam1 - ep.py / 4.0;
        double vy = cam2 - ep.gnd;
        double vz = ep.px / 4.0 - cam0;
        double TR[3]; shadow_ground_tr(C, vx, vy, vz, TR);
        if (TR[2] - 4096.0 >= 0.0) continue;                /* Z cull (floor(x)>=N ⟺ x>=N) */
        shadow_pt anchor; shadow_project(C, ZERO, ofx, ofy, v, vx, vy, vz, &anchor);

        int idxB = (int)((sd >> 16) & 0x3F);
        int idxA = (int)((sd >> 22) & 0x3F);
        int Rp[3][3]; assemble_matrix(cpu, C, idxA, idxB, Rp);

        int rlo = shadow_s16((int)(radius & 0xFFFF)), rhi = shadow_s16((int)((radius >> 16) & 0xFFFF));
        shadow_pt c[4];
        shadow_project(Rp, TR, ofx, ofy, v,  0,    0,    rlo, &c[0]);   /* RTPT V0 */
        shadow_project(Rp, TR, ofx, ofy, v,  rlo,  rhi,  0,   &c[1]);   /* RTPT V1 */
        shadow_project(Rp, TR, ofx, ofy, v,  0,    0,   -rlo, &c[2]);   /* RTPT V2 */
        shadow_project(Rp, TR, ofx, ofy, v, -rlo,  0,    0,   &c[3]);   /* RTPS 4th */

        if (sh_nclip(&c[0], &c[1], &c[2]) < 0.0) continue;       /* backface */
        /* screen-extent rejections (match the .s prim count) */
        if (game_layout.shadow.cull_layout == SHADOW_CULL_SPYRO3) {
            if (cull_corners(c)) continue;
        } else {
            if (cull_anchor(&anchor)) continue;
        }

        int trz = (int)TR[2];
        int g = (trz < 0xC00) ? 0x80 : ((0x1000 - trz) >> 3);
        if (g < 0) g = 0;
        uint32_t grey = ((uint32_t)g) | ((uint32_t)g << 8) | ((uint32_t)g << 16);

        /* 4 fan tris: (anchor, c[i], c[i+1]) */
        for (int k = 0; k < 4; k++) {
            if (s_mosh_n >= MAX_MOSH) break;
            shadow_pt* p1 = &c[k]; shadow_pt* p2 = &c[(k+1)&3];
            moshtri* t = &s_mosh[s_mosh_n++];
            t->slot = moby_shadow_slot(cpu, mob, &anchor, p1, p2);
            t->sx[0]=anchor.sx; t->sy[0]=anchor.sy; t->sz[0]=anchor.sz; t->u[0]=u0; t->v[0]=v0t;
            t->sx[1]=p1->sx;    t->sy[1]=p1->sy;    t->sz[1]=p1->sz;    t->u[1]=u1; t->v[1]=v1t;
            t->sx[2]=p2->sx;    t->sy[2]=p2->sy;    t->sz[2]=p2->sz;    t->u[2]=u2; t->v[2]=v2t;
            t->rgb = grey;
            t->tpx=tpx; t->tpy=tpy; t->clx=clx; t->cly=cly; t->bpp=bpp; t->blend=blend;
        }
    }
}

/* View-Z to native scene depth; see shadow.h. */
float shadow_depth(float viewz, float scale, float bias) {
    return openpete_native_scene_viewz(
        openpete_worldvz_bias(openpete_worldvz_scaled(viewz, scale), bias));
}
/* Both view-Z biases default to 1.0 (true depth); the env vars scale them.
 *  - Player shadow: terrain standing above the fan's ground plane by more than
 *    OPENPETE_SHADOW_WALL_HEIGHT (default 400) is rejected per pixel by the
 *    depth-aware decal (shadow_decal.frag with the world-opaque depth pre-pass,
 *    native_render.c); everything else keeps the fan, so bumps and slopes
 *    drape. The fan feeds true depth and its pipe has the depth test off.
 *  - Moby shadows: small, distant coplanar decals; the tie-break is the
 *    hardware slope-scaled depth bias on the subtractive pipelines
 *    (native_render.c shadow_hw_bias, OPENPETE_SHADOW_HW_BIAS). A
 *    multiplicative view-Z bias grows with distance, the wrong dimension for a
 *    tie-break: a shadow ten times farther would get a ten times larger
 *    world-space offset. */
/* shadow_bias_spyro and shadow_bias_moby are documented in shadow.h. */
float shadow_bias_spyro(void) {
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BIAS_SPYRO"); b = (e && *e) ? (float)atof(e) : 1.0f; }
    return b;
}
int shadow_player_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_PLAYER"); v = !(e && *e && *e == '0'); }
    return v;
}

float shadow_bias_moby(void) {
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BIAS_MOBY"); b = (e && *e) ? (float)atof(e) : 1.0f; }
    return b;
}

/* ---- the player fan's footprint decal ------------------------------------- */

/* A footprint receiver lies between the fan's own surface less this margin and
 * the caster height. The margin holds the pre-pass reconstruction error (a few
 * world units) on a receiver that is the fan's own ground; the prism hull the
 * feed draws spans the same interval, so no receiver the rule admits falls
 * outside the drawn cover. */
#define SHADOW_FOOT_BELOW 16.0
/* A hull vertex nearer the eye plane than this view-Z, or projecting farther
 * off screen than this bound, replaces the hull by a full-screen cover: the
 * fragment stage decides every pixel from the fan data, so a larger cover
 * costs fragments and never changes the image. */
#define SHADOW_HULL_NEAR  16.0
#define SHADOW_HULL_BOUND 4096.0

/** @brief Reports whether the footprint term is on; OPENPETE_SHADOW_FOOTPRINT=0 turns it off.
 *
 * Read on every call. Off, the decal keeps the fan's own surface under the
 * depth rule alone: the negative control of the term's count.
 * @return 1 when on, else 0. */
static int shadow_footprint_on(void) {
    const char* e = getenv("OPENPETE_SHADOW_FOOTPRINT");
    return !(e && *e == '0');
}

/* The caster's world height; see shadow.h. */
double shadow_caster_up(CPUState* cpu, double anchor_up) {
    const uint32_t pl = game_layout.player.base;
    if (!pl) return anchor_up;
    return (double)(int32_t)cpu->read_word(pl + game_layout.player.pos_off + 8u);
}

/** @brief Camera-relative world position (x, y, up) of a projected point.
 *
 * Uses the same 4096² · C⁻¹ rows the decal reconstructs the pre-pass terrain
 * with, so both sides of the footprint test share one coordinate system. The
 * GTE input vector V maps to (V2, -V0, -V1).
 * @param inv The rows from shadow_cam_inverse.
 * @param p The projected point; its MAC1, MAC2 and sz are read.
 * @param w Receives (x, y, up). */
static void fan_wrel(const double inv[3][3], const shadow_pt* p, double w[3]) {
    const double m[3] = { p->m1, p->m2, p->sz };
    double V[3];
    for (int j = 0; j < 3; j++)
        V[j] = (inv[j][0] * m[0] + inv[j][1] * m[1] + inv[j][2] * m[2]) / 4096.0;
    w[0] = V[2]; w[1] = -V[0]; w[2] = -V[1];
}

typedef struct { double x, y; } fan_xy;

/** @brief The cross product of o->a and o->b.
 *
 * @param o The origin.
 * @param a The first point.
 * @param b The second point.
 * @return The signed twice-area; positive when o, a, b turn counter-clockwise
 *         in a y-up frame. */
static double hull_cross(fan_xy o, fan_xy a, fan_xy b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
/** @brief The qsort comparator ordering points by x, then y.
 *
 * @param pa The first fan_xy.
 * @param pb The second fan_xy.
 * @return -1, 0 or 1. */
static int hull_cmp(const void* pa, const void* pb) {
    const fan_xy* a = (const fan_xy*)pa; const fan_xy* b = (const fan_xy*)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    return 0;
}
/** @brief Andrew's monotone chain: the convex hull of p[0..n) into h.
 *
 * Sorts `p` in place by x, then y. The lower chain is built first, then the
 * upper; collinear points are dropped and the closing duplicate is not
 * counted.
 * @param p The points; reordered.
 * @param n The point count.
 * @param h Receives the hull; room for 2n points.
 * @return The hull's vertex count. */
static int convex_hull(fan_xy* p, int n, fan_xy* h) {
    qsort(p, (size_t)n, sizeof *p, hull_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && hull_cross(h[k - 2], h[k - 1], p[i]) <= 0.0) k--;
        h[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && hull_cross(h[k - 2], h[k - 1], p[i]) <= 0.0) k--;
        h[k++] = p[i];
    }
    return k > 1 ? k - 1 : k;
}

/** @brief The screen position of a view-space point by the GTE's divide without its saturation.
 *
 * @param f The fan; supplies the projection registers ofx, ofy and H.
 * @param m The point's MAC1..MAC3.
 * @param o Receives the position; unwritten when the view-Z is below 16.
 * @return 1 when the point can stand as a hull vertex; 0 when its view-Z is
 *         below 16 or either coordinate reaches 4096 in magnitude. */
static int hull_project(const shadow_fan* f, const double m[3], fan_xy* o) {
    if (m[2] < SHADOW_HULL_NEAR) return 0;
    o->x = f->ofx + f->H * m[0] / m[2];
    o->y = f->ofy + f->H * m[1] / m[2];
    return fabs(o->x) < SHADOW_HULL_BOUND && fabs(o->y) < SHADOW_HULL_BOUND;
}

/** @brief Fills the footprint block's point i and the three hull candidates it contributes.
 *
 * Point 0 is the anchor and 1..16 the ring. The block receives the screen
 * position (floored under vertex snap), the biased and unbiased depths, and
 * the camera-relative world position. `out` receives the screen point and
 * the two points vertically over it: the prism floor, 16 world units under
 * the fan, and its roof at the caster height.
 * @param f The fan.
 * @param inv The rows from shadow_cam_inverse.
 * @param w0 The anchor's camera-relative world position.
 * @param caster The caster's world height.
 * @param i The point index.
 * @param fp The footprint block.
 * @param out Receives the screen point, the floor and the roof.
 * @return 1 when both hull points project usably, else 0. */
static int fan_point(const shadow_fan* f, const double inv[3][3], const double w0[3],
                     double caster, int i, openpete_shadow_footprint* fp, fan_xy out[3]) {
    const shadow_pt* p = i ? &f->ring[i - 1] : &f->anchor;
    const int snap = openpete_native_vertex_snap_active();
    const float sx = snap ? floorf(p->sx) : p->sx, sy = snap ? floorf(p->sy) : p->sy;
    fp->scr[i][0] = sx; fp->scr[i][1] = sy;
    fp->scr[i][2] = shadow_depth(p->sz, 1.0f, shadow_bias_spyro());
    fp->scr[i][3] = shadow_depth(p->sz, 1.0f, 1.0f);
    double w[3]; fan_wrel(inv, p, w);
    for (int c = 0; c < 3; c++) fp->wrel[i][c] = (float)w[c];
    out[0].x = sx; out[0].y = sy;
    const double up = f->anchor_up + (w[2] - w0[2]);
    const double dz[2] = { -SHADOW_FOOT_BELOW, caster - up };
    int ok = 1;
    for (int e = 0; e < 2; e++) {
        /* a world rise dz lowers the GTE input's second component by dz */
        const double m[3] = { p->m1 - f->C[0][1] * dz[e] / 4096.0,
                              p->m2 - f->C[1][1] * dz[e] / 4096.0,
                              p->sz - f->C[2][1] * dz[e] / 4096.0 };
        ok &= hull_project(f, m, &out[1 + e]);
    }
    return ok;
}

/** @brief Feeds the linked fan triangles as the guest draws them.
 *
 * Each triangle k whose bit is set in tri_mask is (anchor, ring k,
 * ring k+1), gouraud from anchor_rgb (0x608080 from both player feeders) at
 * the anchor to 0 at the rim, with vertex alpha 1 marking it as the fan. The
 * depth is biased by shadow_bias_spyro; pvz is unbiased.
 * @param f The fan. */
static void fan_feed_tris(const shadow_fan* f) {
    const float bias = shadow_bias_spyro();
    for (int k = 0; k < SHADOW_FAN_RING; k++) {
        if (!(f->tri_mask & (1u << k))) continue;
        const shadow_pt* p[3] = { &f->anchor, &f->ring[k], &f->ring[(k + 1) & (SHADOW_FAN_RING - 1)] };
        openpete_scene_tri st = {0};
        for (int i = 0; i < 3; i++) {
            st.v[i].sx = p[i]->sx; st.v[i].sy = p[i]->sy;
            st.v[i].z  = shadow_depth(p[i]->sz, 1.0f, bias);   /* already full-world MAC3 */
            st.v[i].pvz = shadow_depth(p[i]->sz, 1.0f, 1.0f);  /* true (unbiased) view-Z */
            st.v[i].rgba = (i == 0 ? (f->anchor_rgb & 0xFFFFFFu) : 0u) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        st.koff = shadow_slot_koff(f->slot[k]);        /* the triangle's ordering-table slot; unread */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
}

void shadow_fan_feed(CPUState* cpu, const shadow_fan* f) {
    (void)cpu;
    double inv[3][3]; shadow_cam_inverse(f->C, inv);
    const double caster = f->caster_up;
    openpete_shadow_footprint fp; memset(&fp, 0, sizeof fp);
    fp.caster_up = (float)caster;
    fp.footprint = shadow_footprint_on() ? 1.0f : 0.0f;
    fp.tri_mask  = (float)(f->tri_mask & 0xFFFFu);
    fp.below     = (float)SHADOW_FOOT_BELOW;
    for (int c = 0; c < 3; c++) fp.anchor_rgb[c] = (float)((f->anchor_rgb >> (8 * c)) & 0xFFu) / 255.0f;
    fp.anchor_rgb[3] = 1.0f;

    enum { NPT = 3 * (SHADOW_FAN_RING + 1) };
    fan_xy pts[NPT], hull[2 * NPT];
    double w0[3]; fan_wrel(inv, &f->anchor, w0);
    int cover = 1;
    for (int i = 0; i <= SHADOW_FAN_RING; i++)
        cover &= fan_point(f, inv, w0, caster, i, &fp, &pts[3 * i]);
    openpete_native_set_shadow_footprint(&fp);

    int h;
    if (cover) {
        h = convex_hull(pts, NPT, hull);
    } else {
        const double b = SHADOW_HULL_BOUND;
        const fan_xy screen[4] = { { -b, -b }, { b, -b }, { b, b }, { -b, b } };
        memcpy(hull, screen, sizeof screen);
        h = 4;
    }
    fan_feed_tris(f);
    /* The fragment stage colours every hull pixel from the block; the vertex
     * colour is black with alpha 0, so the depth-tested fallback pipe (no
     * pre-pass) subtracts nothing and the decal tells hull from fan. The
     * depth test is off on the decal pipe, so the vertex depth only has to be
     * a valid view-Z. */
    float vz = shadow_depth(f->anchor.sz, 1.0f, 1.0f);
    if (!(vz >= 1.0f)) vz = 1.0f;
    for (int i = 1; i + 1 < h; i++) {
        const fan_xy* tv[3] = { &hull[0], &hull[i], &hull[i + 1] };
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = (float)tv[k]->x; st.v[k].sy = (float)tv[k]->y;
            st.v[k].z = vz; st.v[k].pvz = vz;
            st.v[k].rgba = 0u;
        }
        st.textured = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
}

/** @brief Walks the drawn-moby list and feeds the native moby shadows.
 *
 * Each shadow is four textured FT3 fan triangles, blended by the tpage's ABR,
 * under the moby-shadow scene tag. A triangle the game does not insert
 * (MOBY_SHADOW_SKIP) is not fed. Depth is the world/4 MAC3 scaled by 4,
 * biased by shadow_bias_moby; pvz is unbiased. Nothing is fed, and the scene
 * frame is not begun, when the walk yields no triangle.
 * @param cpu The guest CPU state. */
static void shadow_moby_feed_native(CPUState* cpu) {
    shadow_moby_build(cpu);
    if (s_mosh_n <= 0) return;
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_SHADOW(OPENPETE_TAG_SHADOW_MOBY, 0));
    for (int i = 0; i < s_mosh_n; i++) {
        moshtri* m = &s_mosh[i];
        if (m->slot == MOBY_SHADOW_SKIP) continue;
        openpete_scene_tri st = {0};
        float bias = shadow_bias_moby();
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = m->sx[k]; st.v[k].sy = m->sy[k];
            st.v[k].z  = shadow_depth(m->sz[k], 4.0f, bias);  /* world/4 MAC3 -> full-world */
            st.v[k].pvz = shadow_depth(m->sz[k], 4.0f, 1.0f); /* true (unbiased) view-Z */
            st.v[k].u = (float)m->u[k]; st.v[k].v = (float)m->v[k];
            st.v[k].rgba = (m->rgb & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 1; st.tpage_x = m->tpx; st.tpage_y = m->tpy;
        st.clut_x = m->clx; st.clut_y = m->cly; st.bpp = m->bpp; st.blend = m->blend;
        st.koff = shadow_slot_koff(m->slot);           /* the fan triangle's ordering-table slot; the decal does not read it */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native moby-shadow triangles against the captured FT3 prims, the .s int16 SXY reference.
 *
 * Both produce four triangles per moby in the same fan order, so they compare
 * element-wise over the shorter list. One stderr line reports the mean and
 * worst SXY distance, the floored-X and floored-Y agreement counts, and the
 * colour differences. Reports VACUOUS when either side has no triangle: a run
 * with nothing to compare is not a pass. OPENPETE_SHADOW_VERIFY_DETAIL prints
 * the first eight triangle pairs once.
 * @param cpu The guest CPU state. */
static void shadow_moby_compare(CPUState* cpu) {
    shadow_moby_build(cpu);
    int ft3 = 0;
    for (int p = 0; p < s_gte_n; p++) if (s_gte[p].textured && s_gte[p].nv == 3) ft3++;
    if (ft3 == 0 || s_mosh_n == 0) {
        fprintf(stderr, "[shadow-moby f=%u] VACUOUS native=%d FT3=%d (nothing compared)\n",
                openpete_frame, s_mosh_n, ft3);
        return;
    }
    int n = s_mosh_n < ft3 ? s_mosh_n : ft3;
    double worst = 0, sum = 0; int over2 = 0, nv = 0, qi = 0;
    int cmax = 0, cbad = 0;
    /* The captured SXY is the GTE's truncated 16.16 output, so a float walk
     * that agrees exactly still reads ~0.5 px off per axis. `floor_x` counts
     * vertices whose floored X is the captured X (the walk agrees to the
     * truncation), and `floor_y1` those whose floored Y is within one row of
     * the captured Y — the row the .s's nearest/farthest corner nudge moves. */
    int floor_x = 0, floor_y1 = 0;
    for (int p = 0; p < s_gte_n && qi < n; p++) {
        shadow_prim* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 3)) continue;
        moshtri* m = &s_mosh[qi];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = m->sx[k]-pr->x[k], dy = m->sy[k]-pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > tw) tw = d;
            double fy = floor(m->sy[k]) - pr->y[k];
            if (floor(m->sx[k]) == pr->x[k]) floor_x++;
            if (fy >= -1.0 && fy <= 1.0) floor_y1++;
        }
        for (int ch = 0; ch < 3; ch++) {
            int a = (int)((m->rgb >> (ch*8)) & 0xFF);
            int b = (int)((pr->rgb[0] >> (ch*8)) & 0xFF);
            int d = a > b ? a - b : b - a;
            if (d > cmax) cmax = d;
        }
        if (m->rgb != (pr->rgb[0] & 0xFFFFFFu)) cbad++;
        if (tw > worst) worst = tw;
        if (tw > 2.0) over2++;
        qi++;
    }
    fprintf(stderr, "[shadow-moby f=%u] native=%d FT3=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx "
                    "over2px=%d/%d floorX=%d/%d floorY+-1=%d/%d dRGBmax=%d rgbMismatch=%d\n",
            openpete_frame, s_mosh_n, ft3, n, nv?sum/nv:0, worst, over2, n,
            floor_x, nv, floor_y1, nv, cmax, cbad);
    if (getenv("OPENPETE_SHADOW_VERIFY_DETAIL")) {
        static int once = 0;
        if (!once) {
            once = 1;
            int q = 0;
            for (int p = 0; p < s_gte_n && q < n && q < 8; p++) {
                shadow_prim* pr = &s_gte[p];
                if (!(pr->textured && pr->nv == 3)) continue;
                moshtri* m = &s_mosh[q];
                fprintf(stderr, "  tri[%d]", q);
                for (int k = 0; k < 3; k++)
                    fprintf(stderr, "  v%d nat=(%.2f,%.2f) gte=(%.0f,%.0f)",
                            k, m->sx[k], m->sy[k], pr->x[k], pr->y[k]);
                fprintf(stderr, "\n");
                q++;
            }
        }
    }
}

/* ---- gating --------------------------------------------------------------- */
int shadow_verify_enabled(void) {
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_SHADOW_VERIFY");
    return v;
}
/* Documented at its declaration in shadow.h. */
unsigned shadow_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SHADOW_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/** @brief The bracket's pre-draw step: latches the drawn-moby list before the draw overwrites the GTE registers.
 *
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void shadow_pre(CPUState* cpu, int feed, int verify) {
    (void)feed; (void)verify;
    shadow_latch_list_base(cpu);
}

static hook_bracket_state s_shadow_hb;
static const hook_bracket_desc k_shadow_bracket = {
    .path = RP_MOBY_SHADOW, .prof_tag = "shadow.srend",
    .verify_enabled = shadow_verify_enabled, .pre = shadow_pre,
    .st = &s_shadow_hb,
};

/** @brief Moby drop-shadow draw hook.
 *
 * Runs the game's draw once through the hook bracket, which records its
 * polybuf interval under RP_MOBY_SHADOW. When the draw emitted prims, they are
 * captured for the feed or the verify harness, the native shadows are fed from
 * gamestate while RP_MOBY_SHADOW is selected, and under OPENPETE_SHADOW_VERIFY
 * the first eligible tick dumps once and the first 60 compare.
 * @param cpu  guest CPU state
 * @param addr the hooked draw's address
 * @return 1 after running the draw, 0 when the bracket declines and the
 *         ordinary dispatch runs it */
static int on_moby_shadows(CPUState* cpu, uint32_t addr) {
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_shadow_bracket, &startp, &endp, &feed, &verify))
        return 0;

    if ((feed || verify) && endp > startp) shadow_capture_prims(cpu, startp, endp);

    if (feed && endp > startp && render_path_enabled(RP_MOBY_SHADOW)) {
        shadow_moby_feed_native(cpu);                /* native gamestate walk */
    }

    if (verify && endp > startp && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { shadow_moby_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) { shadow_moby_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_shadow_bracket);
}

/* Sub-tick moby drop-shadow re-extract; see shadow.h. */
void shadow_reextract(CPUState* cpu) {
    if (!hook_reextract_ready(RP_MOBY_SHADOW)) return;
    shadow_moby_feed_native(cpu);
}

/* The hook address is the layout's moby-shadow draw site, bound at boot: a
 * static initializer cannot read another TU's const. */
static feature_dispatch_entry_t shadow_dispatch[] = {
    { 0u, on_moby_shadows },
    { 0u, NULL },
};

/** @brief Binds the dispatch entry to the layout's moby-shadow draw address.
 *
 * @param opts Unused. */
static void shadow_apply_options(const openpete_options_t* opts) {
    (void)opts;
    shadow_dispatch[0].addr = game_layout.shadow.hook;
}

const feature_module_t shadow_module = {
    .name           = "shadow_native",
    .apply_options  = shadow_apply_options,
    .dispatch_table = shadow_dispatch,
};
