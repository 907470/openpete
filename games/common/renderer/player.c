// games/common/renderer/player.c — the shared player-avatar feeder: the draw
// hook and its polybuf bracket, the per-call record store, the skeleton decode
// and projection, the face-stream walk, the scene feed and the sub-tick
// re-extract. The contract for the per-game half is renderer/player_game.h and
// every engine address comes from game_layout.player.
//
// The avatar is not a moby: a dedicated routine renders it from a dedicated
// state block, and its geometry is a skeleton of parts whose per-part
// transform records that routine builds in the tick's render scratch. Each
// part's vertices are delta-accumulated across two keyframe streams and blended
// by the part record's weight, and the parts stitch into one mesh whose faces
// index the whole projection.
//
// Determinism: the hook calls the game's own draw exactly once and only READS
// state afterwards; nothing here writes PSX RAM. The record store copies guest
// bytes into host copies on the game fiber; the present side hands the copies
// to the re-extract, which stages them through the sub-tick view.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_psx.h"
#include "openpete_subtick_view.h"
#include "openpete_interp.h"
#include "openpete_fiber.h"          /* openpete_frame, openpete_speculative */
#include "openpete.h"                /* openpete_runahead_get */
#include "openpete_profile.h"
#include "game_layout.h"
#include "renderer/orchestrator.h"
#include "renderer/interp_regions_gen.h"
#include "renderer/native_gte.h"
#include "renderer/render_paths.h"
#include "renderer/moby.h"           /* the shared NCLIP, emit tree and tiledef decode */
#include "renderer/player.h"
#include "renderer/player_game.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned openpete_frame;
extern op_time_t g_openpete_tick_stamp;      /* per-VBL stamp (PLL) */
extern op_time_t g_openpete_tick_period_ns;  /* one wall-time tick */

#define PL (game_layout.player)

/* ---- the part-transform block ------------------------------------------
 * A game with a fixed render scratch names its base; a game that carves the
 * scratch out of the polybuf every tick leaves the base 0 and the block starts
 * at *polybuf.end - part_base_polybuf_off, recomputed per pass. */
uint32_t player_part_block_base(CPUState* cpu) {
    if (PL.part_base) return PL.part_base + PL.part_off;
    if (!game_layout.polybuf.end || !PL.part_base_polybuf_off) return 0;
    uint32_t end = cpu->read_word(game_layout.polybuf.end);
    if (end < 0x80000000u) return 0;
    return end - PL.part_base_polybuf_off + PL.part_off;
}

static uint32_t player_colour_scratch(CPUState* cpu) {
    if (!PL.colour_scratch_len) return 0;
    if (PL.part_base) return PL.part_base + PL.colour_scratch_off;
    if (!game_layout.polybuf.end || !PL.part_base_polybuf_off) return 0;
    uint32_t end = cpu->read_word(game_layout.polybuf.end);
    if (end < 0x80000000u) return 0;
    return end - PL.part_base_polybuf_off + PL.colour_scratch_off;
}

static int player_parts(CPUState* cpu) {
    int n = player_game.part_count ? player_game.part_count(cpu) : PL.part_count;
    if (n > PLAYER_PARTS_MAX) n = PLAYER_PARTS_MAX;
    return n < 0 ? 0 : n;
}

/* ---- sub-tick keyframe-blend interpolation -----------------------------
 * A part's vertices blend keyframe A to B by the weight its record carries,
 * and that record is built at the canonical tick rate, so without a sub-tick
 * blend the animation steps at the tick rate while movement is smooth. The
 * orchestrator rolls prev/curr copies of the whole part block at the canonical
 * hook and hands the pair here; the re-extract decodes the two REAL rendered
 * poses and lerps the projected vertices. Decoding one mixed block instead
 * would interpolate at blends the game never rendered, which whips
 * seam-relative vertices off-model whenever a part advances a whole keyframe
 * in a tick, and the cumulative vertex counts of a chained block would desync
 * besides. */
static const uint8_t* s_seg_prev = NULL;
static const uint8_t* s_seg_curr = NULL;
static int     s_seg_valid = 0;       /* 0 none, 1 only curr, 2 prev+curr */
static double  s_anim_alpha = 0.0;
static int     s_anim_on    = 0;
static int     s_player_slot = 0;     /* record index k: the PLAYER tag slot */

void player_native_set_anim_interp(double alpha, int on) { s_anim_alpha = alpha; s_anim_on = on ? 1 : 0; }
void player_native_set_anim_pair(const uint8_t* prev_seg, const uint8_t* curr_seg) {
    s_seg_prev = prev_seg; s_seg_curr = curr_seg;
    s_seg_valid = curr_seg ? (prev_seg ? 2 : 1) : 0;
}
void player_native_set_player_slot(int k) { s_player_slot = k < 0 ? 0 : k; }

/* The front-layer flatten (the sort-depth sentinel) pulls the model onto the
 * near plane, capped so the nearest vertex lands ON the near floor. With N
 * avatars per tick a per-CALL cap flattens each to the floor independently and
 * the z-test interleaves their bodies. The cap is therefore shared across the
 * calls of one tick: the running minimum over the tick's calls so far, and —
 * for the first calls, before the nearest avatar has drawn — the previous
 * tick's all-calls minimum when that tick had more than one call. One call per
 * tick is the call's own minimum. */
static struct { uint32_t key; int calls; double min; } s_flat_cur = { 0, 0, 1e30 }, s_flat_prev = { 0, 0, 1e30 };
void player_native_set_flat_key(uint32_t key) {
    if (s_flat_cur.key == key) return;
    s_flat_prev = s_flat_cur;
    s_flat_cur.key = key; s_flat_cur.calls = 0; s_flat_cur.min = 1e30;
}
static double player_flat_min(double own_min) {
    s_flat_cur.calls++;
    if (own_min < s_flat_cur.min) s_flat_cur.min = own_min;
    double m = s_flat_cur.min;
    if (s_flat_prev.calls > 1 && s_flat_prev.min < m) m = s_flat_prev.min;
    return m;
}

/* Sub-tick ROTATION interpolation: per-part float byte-angles (axis k = 0 Z /
 * 1 X / 2 Y) the orchestrator interpolated from the prev/curr euler snapshots,
 * so the composer renders rotation below the 8-bit euler grid (256/circle)
 * instead of stepping on it. s_rot_active[p] gates each part. */
static int    s_rot_active[PLAYER_PARTS_MAX] = {0,0,0,0};
static double s_rot_ang[PLAYER_PARTS_MAX][3];
void player_native_set_rot_interp(const int* active, const double* ang) {
    if (!active) { for (int p = 0; p < PLAYER_PARTS_MAX; p++) s_rot_active[p] = 0; return; }
    for (int p = 0; p < PLAYER_PARTS_MAX; p++) {
        s_rot_active[p] = active[p];
        for (int k = 0; k < 3; k++) s_rot_ang[p][k] = ang[p*3 + k];
    }
}
/* Read by a provider's euler composer: the interpolated angles for part p, or
 * 0 when the canonical byte-exact table lookup is the one to use. */
int player_rot_interp(int part, double out[3]) {
    if (part < 0 || part >= PLAYER_PARTS_MAX || !s_rot_active[part]) return 0;
    for (int k = 0; k < 3; k++) out[k] = s_rot_ang[part][k];
    return 1;
}

/* ---- projection --------------------------------------------------------
 * Float RTPS mirror WITH translation, reproducing the GTE's near-plane
 * behaviour in float. For a vertex close to the camera the hardware caps the
 * perspective divide, which pins a near vertex near the screen centre; using
 * the true (much larger) reciprocal instead makes such vertices shoot off the
 * screen edge. Mirroring the cap (and the IR16 and SXY clamps) removes that
 * while leaving every in-range vertex byte-identical, where the quotient is
 * exact. The UNR reciprocal's sub-pixel wobble is not reproduced: the true
 * divide is strictly closer to ideal. */
static double project(const int M[3][3], const double TR[3], double vx, double vy, double vz,
                      double ofx_px, double ofy_px, const ngte_view* v,
                      double* out_sx, double* out_sy, int* out_sat) {
    ngte_proj o;
    ngte_project_gte(M, TR, vx, vy, vz, ofx_px, ofy_px, v, 1, &o);
    *out_sx = o.sx;
    *out_sy = o.sy;
    if (out_sat) *out_sat = o.sat;
    return o.m3;
}

/* OPENPETE_PLAYER_VERIFY_BITSHIFT: the negative control of the probe below.
 * Reading the model-space X one bit over is what a mis-placed field boundary
 * in the vertex stream does, and it must collapse the probe's precision; a
 * probe that still passes with it armed is measuring nothing. */
static int s_probe_bitshift;

typedef struct { float sx, sy, vz; int sat, seg; } spxy;
#define MAX_SPROJ 2048
static spxy s_sproj[MAX_SPROJ]; static int s_sproj_n;
static spxy s_sproj_prev[MAX_SPROJ];   /* prev-pose projection for the two-pass shape lerp */

void player_proj_vert(int i, float* sx, float* sy, float* vz, int* sat, int* part) {
    if (i < 0 || i >= s_sproj_n) return;
    if (sx) *sx = s_sproj[i].sx;
    if (sy) *sy = s_sproj[i].sy;
    if (vz) *vz = s_sproj[i].vz;
    if (sat) *sat = s_sproj[i].sat;
    if (part) *part = s_sproj[i].seg;
}

/* Decode one part's vertices out of the two keyframe streams, project each and
 * append to s_sproj. The cursors and the opening stream form come out of the
 * part record at the descriptor's offsets; only the per-vertex step is
 * per-game (player_game.vstream_next). */
static int player_project_part(CPUState* cpu, uint32_t entry, const int M[3][3], const double TR[3],
                               double ofx, double ofy, const ngte_view* v, int part, int count) {
    uint32_t fd = cpu->read_word(entry + PL.part_flags_off);
    uint32_t blend_raw = fd & PL.blend_mask;
    int dual = (blend_raw != 0);
    double blend = (double)blend_raw / 4096.0;

    moby_vstream_t A, B;
    memset(&A, 0, sizeof A); memset(&B, 0, sizeof B);
    A.scratch_u[0] = cpu->read_word(entry + PL.part_stream_a_off);
    A.litp         = cpu->read_word(entry + PL.part_stream_a_off + 4u);
    A.shortp       = cpu->read_word(entry + PL.part_stream_a_off + 8u);
    A.flag         = (int)((fd >> PL.part_stream_flag_bit[0]) & 1u);
    B.scratch_u[0] = cpu->read_word(entry + PL.part_stream_b_off);
    B.litp         = cpu->read_word(entry + PL.part_stream_b_off + 4u);
    B.shortp       = cpu->read_word(entry + PL.part_stream_b_off + 8u);
    B.flag         = (int)((fd >> PL.part_stream_flag_bit[1]) & 1u);

    int emitted = 0;
    for (int i = 0; i < count && s_sproj_n < MAX_SPROJ; i++) {
        player_game.vstream_next(cpu, &A, i == 0);
        double VX = A.VX, VY = A.VY, VZ = A.VZ;
        if (dual) {
            player_game.vstream_next(cpu, &B, i == 0);
            VX = (double)A.VX + blend * (B.VX - A.VX);
            VY = (double)A.VY + blend * (B.VY - A.VY);
            VZ = (double)A.VZ + blend * (B.VZ - A.VZ);
        }
        if (s_probe_bitshift) VX *= 2.0;   /* the negative control below */
        double sx, sy; int sat;
        double vz = project(M, TR, VX, VY, VZ, ofx, ofy, v, &sx, &sy, &sat);
        s_sproj[s_sproj_n].sx = (float)sx; s_sproj[s_sproj_n].sy = (float)sy;
        s_sproj[s_sproj_n].vz = (float)vz; s_sproj[s_sproj_n].sat = sat;
        s_sproj[s_sproj_n].seg = part;
        s_sproj_n++; emitted++;
    }
    return emitted;
}

/* ---- the skeleton's transforms ----------------------------------------
 * Derived from gamestate by the provider (the camera's projection matrix
 * against the avatar's pose, plus the seam joint that hangs each child part
 * off its parent), never read back from the record the draw built: the record
 * is the canonical tick's, and a sub-tick present needs the transform under
 * the interpolated camera. */
static player_xform_t s_xf[PLAYER_PARTS_MAX];

const player_xform_t* player_part_xform(int p) {
    return (p >= 0 && p < PLAYER_PARTS_MAX && s_xf[p].present) ? &s_xf[p] : NULL;
}

static void player_derive_xforms(CPUState* cpu, const ngte_view* v) {
    memset(s_xf, 0, sizeof s_xf);
    const int n = player_parts(cpu);
    if (n <= 0 || !player_game.part_xform) return;
    if (!player_game.part_xform(cpu, v, 0, NULL, &s_xf[0])) return;
    /* The seam blends on the ROOT part's keyframe weight, not on a per-part
     * progress byte: the sub-tick re-extract writes a fine interpolated weight
     * into the root record, and a coarse per-part byte would step the seam
     * while the vertices around it stay smooth. */
    uint32_t block = player_part_block_base(cpu);
    uint32_t fd0 = block ? cpu->read_word(block + PL.part_flags_off) : 0u;
    int ir0 = (int)(fd0 & PL.blend_mask);
    int dual = (ir0 != 0);
    for (int p = 1; p < n; p++) {
        if (!player_game.part_xform(cpu, v, p, &s_xf[0], &s_xf[p])) continue;
        if (player_game.seam_joint)
            player_game.seam_joint(cpu, &s_xf[0], p, ir0, dual, s_xf[p].TR);
    }
}

/* Walk the part records, decode and project every vertex. */
/* Per-tick engagement counts: a pass that decoded nothing proves nothing, so
 * the probe prints what this walk actually read. */
static long s_eng_parts, s_eng_verts, s_eng_faces, s_eng_tris;

int player_project_all(CPUState* cpu) {
    s_sproj_n = 0;
    s_eng_parts = 0; s_eng_verts = 0;
    if (!player_game.vstream_next) return 0;
    uint32_t block = player_part_block_base(cpu);
    if (!block) return 0;
    ngte_view view; ngte_view_build(cpu, &view);
    const ngte_view* v = &view;
    player_derive_xforms(cpu, v);
    const int nparts = player_parts(cpu);
    int cursor = 0;                 /* the running value a cumulative count advances */
    int nseg = 0;
    for (int p = 0; p < nparts; p++) {
        uint32_t entry = block + (uint32_t)p * PL.part_stride;
        uint32_t fd = cpu->read_word(entry + PL.part_flags_off);
        if (fd == 0) break;                          /* the block's terminator */
        int count = player_game.part_vcount ? player_game.part_vcount(fd, &cursor) : 0;
        if (count <= 0) continue;
        if (!s_xf[p].present) continue;
        s_eng_verts += player_project_part(cpu, entry, s_xf[p].M, s_xf[p].TR,
                                          v->ofx, v->ofy, v, p, count);
        s_eng_parts++;
        nseg++;
    }
    return nseg ? s_sproj_n : 0;
}

/* ---- the face-stream walk ---------------------------------------------
 * Topology, per-vertex colour and UVs come from the model's own face and
 * colour streams, decoded by the provider into the shared face record, and the
 * emit runs the dual-NCLIP tree the moby walk already owns. Nothing here reads
 * the primitives the game's draw emitted. */
typedef struct { float sx,sy,vz; int u,v; uint32_t rgb; int idx; } swv;
typedef struct {
    swv v[3];
    int textured, tpage_x, tpage_y, clut_x, clut_y, bpp, blend;
} swtri;
#define MAX_SWTRI (MAX_SPROJ * 4)
#define MAX_SWFACE 8192
static swtri s_swtri[MAX_SWTRI]; static int s_swtri_n;
static moby_wface_t s_face[MAX_SWFACE];

void player_walk_tri(int k, int idx[3], uint32_t rgb[3], int u[3], int v[3]) {
    if (k < 0 || k >= s_swtri_n) return;
    for (int j = 0; j < 3; j++) {
        if (idx) idx[j] = s_swtri[k].v[j].idx;
        if (rgb) rgb[j] = s_swtri[k].v[j].rgb;
        if (u)   u[j]   = s_swtri[k].v[j].u;
        if (v)   v[j]   = s_swtri[k].v[j].v;
    }
}

/* Signed 2*area on the native FLOAT screen coordinates: the GTE's int16 floor
 * flips a near-zero-area face's sign at grazing angles and culls genuinely
 * front-facing thin geometry (a tail tip, a wing edge) so the background shows
 * through. The Z-buffer sorts the survivors. */
static double player_nclip(int i0, int i1, int i2) {
    if (i0 >= s_sproj_n || i1 >= s_sproj_n || i2 >= s_sproj_n) return -1.0;
    return moby_tri_nclip(s_sproj[i0].sx, s_sproj[i0].sy,
                          s_sproj[i1].sx, s_sproj[i1].sy,
                          s_sproj[i2].sx, s_sproj[i2].sy);
}

/* The depth key the draw sums before its per-face admission: the four
 * vertices of a quad, or — where the NCLIP tree left one triangle — that
 * triangle's three with the middle vertex counted twice, so both shapes share
 * one scale. SZ3 is the projected view-Z saturated to the GTE's 16-bit range,
 * which is what the draw reads back per vertex. */
static double player_depth_sum(const uint16_t idx[4], const int order[2][3], int nemit) {
    double sz[4];
    int n = (nemit >= 2) ? 4 : 3;
    const int pos[4] = { nemit >= 2 ? 0 : order[0][0], nemit >= 2 ? 1 : order[0][1],
                         nemit >= 2 ? 2 : order[0][2], 3 };
    for (int k = 0; k < n; k++) {
        int i = idx[pos[k]];
        double z = (i < s_sproj_n) ? (double)s_sproj[i].vz : 0.0;
        sz[k] = z < 0.0 ? 0.0 : (z > 65535.0 ? 65535.0 : z);
    }
    return (nemit >= 2) ? (sz[0] + sz[1] + sz[2] + sz[3])
                        : (sz[0] + sz[1] + sz[1] + sz[2]);
}

static void emit_swtri(int i0,int i1,int i2, uint32_t c0,uint32_t c1,uint32_t c2,
                       int u0,int v0,int u1,int v1,int u2,int v2,
                       int tex,int tpx,int tpy,int clx,int cly,int bpp,int blend) {
    if (s_swtri_n >= MAX_SWTRI) return;
    if (i0 >= s_sproj_n || i1 >= s_sproj_n || i2 >= s_sproj_n) return;
    swtri* t = &s_swtri[s_swtri_n++];
    t->v[0]=(swv){s_sproj[i0].sx,s_sproj[i0].sy,s_sproj[i0].vz,u0,v0,c0,i0};
    t->v[1]=(swv){s_sproj[i1].sx,s_sproj[i1].sy,s_sproj[i1].vz,u1,v1,c1,i1};
    t->v[2]=(swv){s_sproj[i2].sx,s_sproj[i2].sy,s_sproj[i2].vz,u2,v2,c2,i2};
    t->textured=tex; t->tpage_x=tpx; t->tpage_y=tpy; t->clut_x=clx; t->clut_y=cly;
    t->bpp=bpp; t->blend=blend;
}

int player_build_walk(CPUState* cpu) {
    s_swtri_n = 0;
    s_eng_faces = 0;
    if (s_sproj_n <= 0 || !player_game.part_faces || !player_game.decode_faces) return 0;
    const int nparts = player_parts(cpu);
    /* A game whose whole skeleton shares one stitched mesh answers the same
     * streams for every part; decode each distinct pair once. */
    uint32_t seen_faces[PLAYER_PARTS_MAX]; int nseen = 0;
    for (int p = 0; p < nparts; p++) {
        player_faces_t F;
        if (!player_game.part_faces(cpu, p, &F)) continue;
        if (F.faces < 0x80000000u || F.colours < 0x80000000u) continue;
        int dup = 0;
        for (int q = 0; q < nseen; q++) if (seen_faces[q] == F.faces) { dup = 1; break; }
        if (dup) continue;
        seen_faces[nseen++] = F.faces;

        uint32_t len = cpu->read_word(F.faces);
        if (len == 0 || len > 0x20000u) continue;
        moby_face_stats_t st;
        int nf = player_game.decode_faces(cpu, F.faces, len, s_face, MAX_SWFACE, &st);
        s_eng_faces += nf;
        for (int fi = 0; fi < nf; fi++) {
            const moby_wface_t* f = &s_face[fi];
            uint32_t c0 = cpu->read_word(F.colours + f->c0) & 0xFFFFFFu;
            uint32_t c1 = cpu->read_word(F.colours + f->c1) & 0xFFFFFFu;
            uint32_t c2 = cpu->read_word(F.colours + f->c2) & 0xFFFFFFu;
            uint32_t c3 = cpu->read_word(F.colours + f->c3) & 0xFFFFFFu;
            int blend = f->blendbit ? moby_abr_blend(f->abr) : 0;
            double n1 = player_nclip(f->i0, f->i1, f->i2);
            double n2 = (f->i3 < s_sproj_n) ? player_nclip(f->i3, f->i1, f->i2) : -1.0;
            int order[2][3];
            /* A game whose draw admits a flagged face untested takes the emit
             * tree's unconditional arm, which keeps the face's whole topology
             * without reading either NCLIP. */
            int uncond = player_game.face_flag_skips_nclip && f->untested;
            int fbit0 = uncond ? 1 : (player_game.face_winding_flag ? f->bit0 : 0);
            int nemit = moby_face_emit(f->fresh, fbit0,
                                       1 /* the flag exempts, it never re-tests */,
                                       n1, n2, f->i3 < s_sproj_n, order);
            const uint16_t idx[4] = { f->i0, f->i1, f->i2, f->i3 };
            const uint32_t col[4] = { c0, c1, c2, c3 };
            if (nemit && player_game.face_depth_keep &&
                !player_game.face_depth_keep(cpu, s_xf[0].present ? &s_xf[0] : NULL,
                                             f, player_depth_sum(idx, order, nemit)))
                continue;
            for (int e = 0; e < nemit; e++) {
                const int a = order[e][0], b = order[e][1], c = order[e][2];
                emit_swtri(idx[a], idx[b], idx[c], col[a], col[b], col[c],
                           f->u[a], f->v[a], f->u[b], f->v[b], f->u[c], f->v[c],
                           f->textured, f->tpx, f->tpy, f->clx, f->cly, f->bpp, blend);
            }
        }
    }
    s_eng_tris = s_swtri_n;
    return s_swtri_n;
}

/* ---- scene feed --------------------------------------------------------
 * Native float vertices carry the geometry; the face stream carries topology,
 * per-vertex colour and texture; the texture itself comes from VRAM, a static
 * asset. */
static void player_feed_walk_ex(CPUState* cpu, int reproject) {
    if (reproject) player_project_all(cpu);
    player_build_walk(cpu);
    if (s_swtri_n <= 0) return;

    /* The draw keys every part at max(0, (part-origin viewZ >> 7) -
     * sortDepth); one ordering slot is 128 full-world view-Z units. Like the
     * moby walks' depth offset, sortDepth is an ordering KEY, not geometry —
     * it never enters stored depth, and mirroring it as a world pull would
     * show the avatar through any wall within a constant distance. Per-pixel Z
     * over TRUE depth mirrors the per-face sort; the sub-slot tie pull below
     * resolves coincident surfaces to the avatar (ground contact, and a
     * carrier the rider sits inside).
     *
     * EXCEPTION: the saturated sentinel is the draw's own "ordering slot 0 —
     * draw in front of everything". That is authored full-front LAYERING, not
     * contact depth: keep the near-plane-capped whole-model pull for exactly
     * that value. OPENPETE_MOBY_OTBIAS=0 disables both. */
    double sp_pull = 0.0;
    double tie_pull = 0.0;
    {
        static int s_on = -1;
        if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_OTBIAS"); s_on = (e && *e) ? (*e != '0') : 1; }
        int sd = PL.sort_depth_off ? (int)(int8_t)cpu->read_byte(PL.base + PL.sort_depth_off) : 0;
        if (s_on && player_game.sort_depth_front && sd == player_game.sort_depth_front) {
            sp_pull = 128.0 * (double)sd;
            /* Cap the pull per tick so the nearest vertex lands ON the near
             * floor instead of clamping each vertex at it: a per-vertex clamp
             * flattens everything past the floor to ONE depth and kills
             * self-occlusion. */
            double minvz = 1e30;
            for (int i = 0; i < s_swtri_n; i++)
                for (int k = 0; k < 3; k++)
                    if (s_swtri[i].v[k].vz > 0.0f && (double)s_swtri[i].v[k].vz < minvz)
                        minvz = (double)s_swtri[i].v[k].vz;
            double fmin = player_flat_min(minvz);
            if (fmin - sp_pull < 1.0) sp_pull = fmin > 1.0 ? fmin - 1.0 : 0.0;
            if (minvz - sp_pull < 1.0) sp_pull = minvz > 1.0 ? minvz - 1.0 : 0.0;
        } else if (s_on) {
            /* One-slot tie pull: 127 world is under one ordering slot, so it
             * can only flip pairs the original called a tie. */
            tie_pull = 127.0;
        }
    }
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_PLAYER((uint32_t)s_player_slot));
    for (int i = 0; i < s_swtri_n; i++) {
        swtri* w = &s_swtri[i];
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = w->v[k].sx; st.v[k].sy = w->v[k].sy;
            double fvz = (double)w->v[k].vz - sp_pull - tie_pull;
            st.v[k].z  = openpete_native_depth_from_viewz(openpete_worldvz_full((float)fvz));
            /* Perspective W from the UNSHIFTED view-Z: the front-layer pull
             * puts the whole model on the near plane, and a W taken from that
             * depth varies across one model's faces and smears a texel over
             * them. */
            st.v[k].pvz = w->v[k].vz;
            st.v[k].u  = (float)w->v[k].u; st.v[k].v = (float)w->v[k].v;
            st.v[k].rgba = w->v[k].rgb | 0xFF000000u;
        }
        /* OPENPETE_SPYRO_UNTEX=1 draws the whole player feed untextured
         * (gouraud only), separating vertex-path losses from fragment-path
         * losses when bisecting a rendering defect. */
        { static int s_ut = -1;
          if (s_ut < 0) { const char* e = getenv("OPENPETE_SPYRO_UNTEX"); s_ut = (e && *e && *e != '0'); }
          if (s_ut) w->textured = 0; }
        st.textured = w->textured; st.tpage_x = w->tpage_x; st.tpage_y = w->tpage_y;
        st.clut_x = w->clut_x; st.clut_y = w->clut_y; st.bpp = w->bpp; st.blend = w->blend;
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_PLAYER, 0);
    }
    openpete_native_scene_set_tag(0);
}

static void player_feed_walk(CPUState* cpu) { player_feed_walk_ex(cpu, 1); }

/* ---- OPENPETE_PLAYER_VERIFY -------------------------------------------
 * The probe: bracket the draw's own polybuf window, decode the primitives it
 * emitted there, and match them against the triangles this walk built from the
 * same pose. Precision is the share of walk triangles a primitive covers,
 * recall the share of primitives a walk triangle covers, both on a 4-pixel
 * per-vertex bound; the mean is over the matched pairs' vertex distances and
 * the colour rate over their per-vertex packed RGB. Every line carries the
 * parts, vertices, faces and triangles the walk read, so a run that decoded
 * nothing reports zero engagement instead of a vacuous pass.
 *
 * OPENPETE_PLAYER_VERIFY_BITSHIFT arms the negative control. */
#define PV_MAX_PRIM 4096
typedef struct { float x[3], y[3]; uint32_t rgb[3]; uint8_t code; } pv_tri;
static pv_tri s_pv[PV_MAX_PRIM]; static int s_pv_n;

static void pv_capture(CPUState* cpu, uint32_t start, uint32_t end) {
    s_pv_n = 0;
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < PV_MAX_PRIM) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t len = (w0 >> 24) & 0xFFu;
        if (len == 0 || len > 24) break;
        uint32_t stride = (len + 1) * 4;
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        if ((code & 0xE0u) == 0x20u) {
            int gouraud = (code & 0x10) != 0, quad = (code & 0x08) != 0, tex = (code & 0x04) != 0;
            int nv = quad ? 4 : 3;
            uint32_t raw[20];
            int nraw = (int)(stride / 4); if (nraw > 20) nraw = 20;
            for (int i = 0; i < nraw; i++) raw[i] = cpu->read_word(p + (uint32_t)i*4);
            float x[4], y[4]; uint32_t rgb[4];
            uint32_t rgb0 = raw[1] & 0xFFFFFFu;
            int c = 2, ok = 1;
            for (int k = 0; k < nv; k++) {
                rgb[k] = (gouraud && k > 0) ? (c < nraw ? raw[c++] & 0xFFFFFFu : rgb0) : rgb0;
                if (c >= nraw) { ok = 0; break; }
                uint32_t xy = raw[c++];
                x[k] = (float)(int16_t)(xy & 0xFFFF);
                y[k] = (float)(int16_t)(xy >> 16);
                if (tex) { if (c >= nraw) { ok = 0; break; } c++; }
            }
            if (ok) {
                const int split[2][3] = { {0,1,2}, {1,3,2} };   /* the GPU quad split */
                for (int sidx = 0; sidx < (quad ? 2 : 1) && s_pv_n < PV_MAX_PRIM; sidx++) {
                    pv_tri* t = &s_pv[s_pv_n++];
                    t->code = code;
                    for (int k = 0; k < 3; k++) {
                        int j = split[sidx][k];
                        t->x[k] = x[j]; t->y[k] = y[j]; t->rgb[k] = rgb[j];
                    }
                }
            }
        }
        p += stride;
    }
}

static int player_verify_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_PLAYER_VERIFY"); v = (e && *e && *e != '0');
        const char* b = getenv("OPENPETE_PLAYER_VERIFY_BITSHIFT");
        s_probe_bitshift = (b && *b && *b != '0');
    }
    return v;
}

static void player_verify_report(CPUState* cpu, uint32_t startp, uint32_t endp) {
    pv_capture(cpu, startp, endp);
    /* One-to-one assignment, not nearest-neighbour: the avatar packs dozens of
     * sub-pixel triangles into a few pixels at range, and a nearest-centroid
     * match lets several walk triangles claim the same primitive while its
     * neighbours go unclaimed, which reads as a recall loss that is the
     * matcher's, not the walk's. Every pair within the 4-pixel per-vertex
     * bound is scored, the pairs are taken cheapest first, and each triangle
     * and each primitive is used once. */
    typedef struct { float cost; int16_t w, p; uint8_t rot; } pv_pair;
    static pv_pair pairs[PV_MAX_PRIM * 8];
    int npair = 0;
    for (int i = 0; i < s_swtri_n; i++) {
        const swtri* w = &s_swtri[i];
        for (int j = 0; j < s_pv_n; j++) {
            double br = 1e30; int rot = 0;
            for (int r = 0; r < 3; r++) {
                double mx = 0.0;
                for (int k = 0; k < 3; k++) {
                    double dx = w->v[k].sx - s_pv[j].x[(k + r) % 3];
                    double dy = w->v[k].sy - s_pv[j].y[(k + r) % 3];
                    double dd = sqrt(dx*dx + dy*dy);
                    if (dd > mx) mx = dd;
                }
                if (mx < br) { br = mx; rot = r; }
            }
            if (br <= 4.0 && npair < (int)(sizeof pairs / sizeof pairs[0])) {
                pairs[npair].cost = (float)br; pairs[npair].w = (int16_t)i;
                pairs[npair].p = (int16_t)j;   pairs[npair].rot = (uint8_t)rot;
                npair++;
            }
        }
    }
    for (int a = 1; a < npair; a++) {          /* insertion sort: npair is small */
        pv_pair t = pairs[a]; int b = a - 1;
        while (b >= 0 && pairs[b].cost > t.cost) { pairs[b+1] = pairs[b]; b--; }
        pairs[b+1] = t;
    }
    static uint8_t w_used[PV_MAX_PRIM * 8], p_used[PV_MAX_PRIM];
    memset(w_used, 0, sizeof w_used); memset(p_used, 0, sizeof p_used);
    long walk_hit = 0, prim_hit = 0, col_hit = 0, col_tot = 0, mat_hit = 0;
    double errsum = 0.0; long errn = 0;
    for (int a = 0; a < npair; a++) {
        int i = pairs[a].w, j = pairs[a].p, rot = pairs[a].rot;
        if (w_used[i] || p_used[j]) continue;
        w_used[i] = p_used[j] = 1;
        walk_hit++; prim_hit++;
        const swtri* w = &s_swtri[i];
        /* The material leg. A walk triangle can sit on its primitive to the
         * pixel and carry its exact vertex colours and still be the wrong
         * surface: the colour legs compare what modulates the texture, not
         * whether there is one. GPU primitive codes 0x20-0x3F set bit 2 for a
         * textured polygon, so the code the draw wrote is the draw's own
         * verdict on the material, and a walk that drops the texture reads as
         * a colour-perfect match without it. */
        if ((int)(w->textured ? 1 : 0) == (int)((s_pv[j].code & 0x04u) ? 1 : 0)) mat_hit++;
        for (int k = 0; k < 3; k++) {
            double dx = w->v[k].sx - s_pv[j].x[(k + rot) % 3];
            double dy = w->v[k].sy - s_pv[j].y[(k + rot) % 3];
            errsum += sqrt(dx*dx + dy*dy); errn++;
            col_tot++;
            if ((w->v[k].rgb & 0xFFFFFFu) == s_pv[j].rgb[(k + rot) % 3]) col_hit++;
        }
    }
    /* The per-index leg: where the draw leaves every projected vertex's own
     * int16 screen position, index j of this walk is index j of that block, so
     * the comparison has no matching ambiguity and separates a decode error
     * from a culling difference. */
    long ix_hit = 0, ix_tot = 0; double ix_err = 0.0;
    if (PL.sxy_base) {
        for (int j = 0; j < s_sproj_n && (uint32_t)j * 4u < PL.sxy_len; j++) {
            uint32_t w = cpu->read_word(PL.sxy_base + (uint32_t)j * 4u);
            double gx = (double)(int16_t)(w & 0xFFFF), gy = (double)(int16_t)(w >> 16);
            double dx = s_sproj[j].sx - gx, dy = s_sproj[j].sy - gy;
            double d = sqrt(dx*dx + dy*dy);
            ix_tot++; ix_err += d; if (d <= 4.0) ix_hit++;
        }
    }
    fprintf(stderr,
        "[player-index f=%u] n=%ld <=4px=%.1f%% mean=%.2fpx\n",
        openpete_frame, ix_tot, ix_tot ? 100.0*(double)ix_hit/ix_tot : 0.0,
        ix_tot ? ix_err/(double)ix_tot : 0.0);
    fprintf(stderr,
        "[player-verify f=%u] parts=%ld verts=%ld faces=%ld tris=%ld prims=%d  "
        "precision=%.1f%% recall=%.1f%% mean=%.2fpx colour=%.1f%% material=%.1f%%%s\n",
        openpete_frame, s_eng_parts, s_eng_verts, s_eng_faces, s_eng_tris, s_pv_n,
        s_swtri_n ? 100.0 * (double)walk_hit / s_swtri_n : 0.0,
        s_pv_n ? 100.0 * (double)prim_hit / s_pv_n : 0.0,
        errn ? errsum / (double)errn : 0.0,
        col_tot ? 100.0 * (double)col_hit / col_tot : 0.0,
        walk_hit ? 100.0 * (double)mat_hit / (double)walk_hit : 0.0,
        s_probe_bitshift ? "  [NEGATIVE CONTROL: model X one bit over]" : "");
}

/* ---- the hook ----------------------------------------------------------- */

static int on_player_draw(CPUState* cpu, uint32_t addr) {
    (void)addr;
    if (!player_game.run_draw) return 0;
    int verify = player_game.verify_enabled ? player_game.verify_enabled() : 0;
    int feed   = openpete_native_scene_enabled();
    /* --render-paths needs the bracket so the ordering-table filter knows this
     * path's polybuf range even when not feeding or verifying. A stock run
     * keeps the inert one-branch fall-through. */
    if (!verify && !feed && !player_verify_on() && !render_paths_active())
        return 0;
    static int s_in = 0; if (s_in) return 0; s_in = 1;

    uint32_t startp = cpu->read_word(game_layout.polybuf.cursor);
    { OP_PROF_ACCUM_SCOPE("player.draw");
      player_game.run_draw(cpu); }
    uint32_t endp = cpu->read_word(game_layout.polybuf.cursor);

    render_path_record(RP_SPYRO, startp, endp);
    /* One record per call: the state bytes and scratch rows this call rendered
     * from. Its index is the tag slot of the triangles the walk emits. */
    int slot = 0;
    if (endp > startp) slot = player_records_model_append(cpu, startp, endp);
    player_native_set_player_slot(slot);
    player_native_set_flat_key(openpete_frame << 1);

    int is_player = player_game.call_is_player ? player_game.call_is_player(cpu) : 1;
    if (feed && is_player && endp > startp && render_path_enabled(RP_SPYRO)) {
        player_feed_walk(cpu);
        /* Sub-tick spine: snapshot the part records (keyframe stream cursors
         * and vertex counts) and the projected SXY (the cull verdict), both
         * live now, so the intermediates can re-derive the avatar. */
        spyro_native_canon_capture_range(cpu, PL.part_off, PL.part_block_len);
        /* The pre-blend colour table the draw builds above when the state's
         * colour filter selects it. It lives inside the record region the
         * sub-tick restore takes at an EARLIER hook, so that restore leaves it
         * stale and the avatar's colours strobe above the tick rate. Capture
         * it fresh here and overlay it for this feeder. */
        if (PL.colour_scratch_len)
            spyro_native_canon_capture_range(cpu, PL.colour_scratch_off, PL.colour_scratch_len);
        spyro_native_canon_capture_sxy(cpu);
        spyro_native_canon_capture_camera(cpu, SPYRO_PROD_SPYRO);
    }

    if (verify && player_game.verify) player_game.verify(cpu, startp, endp);
    if (player_verify_on() && is_player && endp > startp) {
        player_project_all(cpu);
        player_build_walk(cpu);
        player_verify_report(cpu, startp, endp);
    }

    s_in = 0;
    return 1;
}

/* ---- sub-tick re-extract ------------------------------------------------ */

void player_reextract(CPUState* cpu) {
    if (!openpete_native_scene_enabled()) return;
    /* Only re-extract on sub-ticks if the canonical tick actually drew the
     * avatar. A cutscene that suppresses the gameplay avatar and shows it as a
     * moby instead would otherwise get a SECOND avatar painted over the
     * cutscene one, at the same position in a different pose. */
    if (!render_path_canon_drawn(RP_SPYRO)) return;

    /* Sub-tick animation = two-pass keyframe-SHAPE interpolation: decode the
     * two REAL rendered poses, each internally consistent with its own seam,
     * and lerp the projected vertices. Rotation and position interpolate
     * through their own brackets, applied identically to both passes, so this
     * lerps only the SHAPE; the lerp of two real poses is always on-model. */
    int two_pass = 0;
    const uint32_t block_len = PL.part_block_len;
    if (s_anim_on && s_seg_valid >= 2 && block_len && block_len <= PLAYER_REC_PART_MAX) {
        uint32_t block = player_part_block_base(cpu);
        void* h = block ? openpete_subtick_host_ptr(block) : NULL;
        if (h) {
            uint8_t live[PLAYER_REC_PART_MAX];
            memcpy(live, h, block_len);                 /* the canonical (curr) block */
            memcpy(h, s_seg_prev, block_len);           /* pass 1: PREV rendered pose */
            player_project_all(cpu);
            int pn = s_sproj_n;
            memcpy(s_sproj_prev, s_sproj, (size_t)pn * sizeof(s_sproj[0]));
            memcpy(h, s_seg_curr, block_len);           /* pass 2: CURR rendered pose */
            player_project_all(cpu);
            if (s_sproj_n == pn) {                      /* index-matched → lerp shape */
                double a = s_anim_alpha;
                for (int i = 0; i < pn; i++) {
                    s_sproj[i].sx = (float)(s_sproj_prev[i].sx + (s_sproj[i].sx - s_sproj_prev[i].sx) * a);
                    s_sproj[i].sy = (float)(s_sproj_prev[i].sy + (s_sproj[i].sy - s_sproj_prev[i].sy) * a);
                    s_sproj[i].vz = (float)(s_sproj_prev[i].vz + (s_sproj[i].vz - s_sproj_prev[i].vz) * a);
                }
            }
            memcpy(h, live, block_len);                 /* restore the live scratch */
            player_feed_walk_ex(cpu, 0);
            two_pass = 1;
        }
    }
    if (!two_pass) player_feed_walk(cpu);
}

/* ======================================================================== *
 *  The per-call record store                                                *
 * ======================================================================== */

typedef struct {
    int      n;
    uint32_t stamp;     /* canon epoch of the first append */
    uint32_t frame;     /* openpete_frame of the first append */
    player_model_rec_t r[PLAYER_REC_MAX];
    int      nsh;
    player_shadow_rec_t sh[PLAYER_REC_MAX];
    int      nfl;
    player_flame_rec_t  fl[PLAYER_REC_MAX];
} rec_list_t;

static rec_list_t s_list;             /* accumulating: the committed tick's hook calls */
static rec_list_t s_led;              /* accumulating: the speculative fires of one led frame */
static uint32_t   s_cap_refused_epoch, s_spec_refused_epoch, s_led_fires_epoch;

/* The list is keyed by (epoch stamp, VBL): the stamp advances only at a
 * commit, which a fast-forward and a CD stall skip, so a stamp-only key would
 * accumulate a fast-forward's calls to the cap. */
static void list_open(void) {
    uint32_t stamp = spyro_native_canon_epoch_stamp();
    int empty = (s_list.n == 0 && s_list.nsh == 0 && s_list.nfl == 0);
    if (!empty && (s_list.stamp != stamp || s_list.frame != openpete_frame)) { s_list.n = 0; s_list.nsh = 0; s_list.nfl = 0; empty = 1; }
    if (empty) { s_list.stamp = stamp; s_list.frame = openpete_frame; }
}

/* One speculative frame's fires: runahead runs speculative VBLs after the
 * committed tick, and the last draw among them is the ring's capture tick. */
static void led_open(void) {
    if (s_led.frame != openpete_frame) { s_led.n = 0; s_led.nsh = 0; s_led.nfl = 0; s_led.frame = openpete_frame; }
    s_led_fires_epoch++;
}

typedef struct {
    rec_list_t list;
    op_time_t  ns;
    int        valid;
} store_slot_t;

static store_slot_t s_store[3];       /* [0] curr, [1] prev, [2] prev2 */
static uint32_t     s_store_epoch;
static op_time_t    s_latched_ring_ns;
static uint32_t     s_reset_gen_seen;

/* Each feeder's guest record, plus the name of the interp-ring region that
 * captures it. The ring INDEX is resolved by name at first use: a restated
 * index selects the wrong region's bytes as soon as a row is inserted. */
static struct { const char* name; uint32_t addr; uint32_t len; const char* region; } s_reg[PLAYER_REC_FEEDERS];
static int s_reg_ready;

static void reg_init(void) {
    if (s_reg_ready) return;
    s_reg_ready = 1;
    s_reg[PLAYER_REC_MODEL].name = "model";
    s_reg[PLAYER_REC_MODEL].addr = PL.base;
    s_reg[PLAYER_REC_MODEL].len  = PL.rec_len;
    s_reg[PLAYER_REC_MODEL].region = "g_Spyro";
    s_reg[PLAYER_REC_SHADOW].name = "shadow";
    s_reg[PLAYER_REC_SHADOW].addr = PL.shadow_base;
    s_reg[PLAYER_REC_SHADOW].len  = PL.shadow_len;
    s_reg[PLAYER_REC_SHADOW].region = "g_SpyroShadow";
    s_reg[PLAYER_REC_FLAME].name = "flame";
    s_reg[PLAYER_REC_FLAME].addr = PL.flame_base;
    s_reg[PLAYER_REC_FLAME].len  = PL.flame_len;
    s_reg[PLAYER_REC_FLAME].region = "g_SpyroFlame";
}

/* The ring region index of feeder f, resolved once. -1 when the game did not
 * register the region; a caller then has no ring bytes to compare against. */
static int reg_region(int f) {
    static int cache[PLAYER_REC_FEEDERS] = { -2, -2, -2 };
    reg_init();
    if (cache[f] == -2) cache[f] = s_reg[f].addr ? game_interp_row_index(s_reg[f].region) : -1;
    return cache[f];
}

#define REC_OBS_MAX   16

typedef struct {
    int      n, over;
    int32_t  pos[REC_OBS_MAX][3];
    uint8_t  cf[REC_OBS_MAX];
    uint8_t  last[PLAYER_REC_STATE_MAX];
    int      last_ok;
} feeder_obs_t;

static feeder_obs_t s_obs[PLAYER_REC_FEEDERS];
static feeder_obs_t s_prev_model;

static unsigned s_epochs, s_spec_refused, s_cap_refused, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets;
static unsigned s_mismatch[PLAYER_REC_FEEDERS];
static unsigned s_no_ring, s_pairing_only, s_unpaired, s_pass2;
static unsigned s_led_fires, s_led_match, s_led_miss;
static int      s_max_calls[PLAYER_REC_FEEDERS];
static int      s_max_epochs_vbl, s_epochs_this_vbl;
static uint32_t s_last_commit_frame = 0xFFFFFFFFu;
static double   s_maxB_single = 0.0, s_maxB_multi = 0.0, s_minsep = -1.0, s_max_model_delta = 0.0;

int player_records_dbg(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_PLAYER_RECDBG"); on = (e && *e && *e != '0'); }
    return on;
}

static double dist3(const int32_t a[3], const int32_t b[3]) {
    double dx = (double)a[0] - b[0], dy = (double)a[1] - b[1], dz = (double)a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}
static void rec_pos(const player_model_rec_t* r, int32_t out[3]) { memcpy(out, r->spyro + PL.pos_off, 12); }

static void observe(int feeder) {
    reg_init();
    feeder_obs_t* o = &s_obs[feeder];
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(PL.base);
    const uint8_t* rb = s_reg[feeder].addr ? (const uint8_t*)openpete_psx_to_host_ptr(s_reg[feeder].addr) : NULL;
    if (!sp || !rb) return;
    if (o->n < REC_OBS_MAX) { memcpy(o->pos[o->n], sp + PL.pos_off, 12); o->cf[o->n] = sp[0x2B]; o->n++; }
    else o->over++;
    memcpy(o->last, rb, s_reg[feeder].len);
    o->last_ok = 1;
}

int player_records_model_append(CPUState* cpu, uint32_t rp_start, uint32_t rp_end) {
    /* Runahead lookahead: the hook fires on speculative ticks too. Those fires
     * are not the canonical epoch — they build the led list, the lerp source
     * of the committed records at commit. */
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (player_records_dbg()) observe(PLAYER_REC_MODEL); list_open(); }
    if (L->n >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp  = (const uint8_t*)openpete_psx_to_host_ptr(PL.base);
    uint32_t block = player_part_block_base(cpu);
    uint32_t colb  = player_colour_scratch(cpu);
    const uint8_t* seg = block ? (const uint8_t*)openpete_psx_to_host_ptr(block) : NULL;
    const uint8_t* fk  = colb  ? (const uint8_t*)openpete_psx_to_host_ptr(colb)  : NULL;
    const uint8_t* sxy = PL.sxy_base ? (const uint8_t*)openpete_psx_to_host_ptr(PL.sxy_base) : NULL;
    if (!sp || !seg) return -1;
    player_model_rec_t* r = &L->r[L->n];
    memset(r, 0, sizeof *r);
    memcpy(r->spyro, sp,  PL.rec_len);
    memcpy(r->src,   sp,  PL.rec_len);
    memcpy(r->seg,   seg, PL.part_block_len);
    if (fk)  memcpy(r->fk,  fk,  PL.colour_scratch_len);
    if (sxy) memcpy(r->sxy, sxy, PL.sxy_len);
    r->rp_start = rp_start; r->rp_end = rp_end;
    r->drawn = 1; r->pair = -1; r->pair2 = -1;
    r->is_player = (uint8_t)(player_game.call_is_player ? (player_game.call_is_player(cpu) ? 1 : 0) : 1);
    return L->n++;
}

/* The model record drawn at the live position — the latest, since a caller
 * draws model, shadow and flame per avatar — or, when none was (hidden,
 * respawn blink), a pairing-only model record appended at this call's ordinal:
 * the next drawn model pairs with it and the shadow or flame renders under its
 * state fields; never rendered itself. -1 at the cap. */
static int bind_model(rec_list_t* L, const uint8_t* sp) {
    for (int k = L->n - 1; k >= 0; k--)
        if (memcmp(L->r[k].spyro + PL.pos_off, sp + PL.pos_off, 12) == 0) return k;
    if (L->n >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    player_model_rec_t* r = &L->r[L->n];
    memset(r, 0, sizeof *r);
    memcpy(r->spyro, sp, PL.rec_len);
    memcpy(r->src,   sp, PL.rec_len);
    r->drawn = 0; r->pair = -1; r->pair2 = -1; r->is_player = 1;
    if (L == &s_list) s_pairing_only++;
    return L->n++;
}

int player_records_shadow_append(CPUState* cpu) {
    (void)cpu;
    if (!PL.shadow_base) return -1;
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (player_records_dbg()) observe(PLAYER_REC_SHADOW); list_open(); }
    if (L->nsh >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(PL.base);
    const uint8_t* sh = (const uint8_t*)openpete_psx_to_host_ptr(PL.shadow_base);
    if (!sp || !sh) return -1;
    int m = bind_model(L, sp);
    if (m < 0) return -1;
    player_shadow_rec_t* d = &L->sh[L->nsh];
    memcpy(d->sh,  sh, PL.shadow_len);
    memcpy(d->src, sh, PL.shadow_len);
    d->model = (int8_t)m; d->drawn = 1;
    L->nsh++;
    return m;
}

int player_records_flame_append(CPUState* cpu) {
    (void)cpu;
    if (!PL.flame_base) return -1;
    rec_list_t* L = openpete_speculative ? &s_led : &s_list;
    if (openpete_speculative) led_open();
    else { if (player_records_dbg()) observe(PLAYER_REC_FLAME); list_open(); }
    if (L->nfl >= PLAYER_REC_MAX) { s_cap_refused_epoch++; return -1; }
    const uint8_t* sp = (const uint8_t*)openpete_psx_to_host_ptr(PL.base);
    const uint8_t* fl = (const uint8_t*)openpete_psx_to_host_ptr(PL.flame_base);
    if (!sp || !fl) return -1;
    int m = bind_model(L, sp);
    if (m < 0) return -1;
    player_flame_rec_t* d = &L->fl[L->nfl];
    memcpy(d->fl,  fl, PL.flame_len);
    memcpy(d->src, fl, PL.flame_len);
    d->model = (int8_t)m; d->drawn = 1;
    L->nfl++;
    return m;
}

static int lists_differ(const rec_list_t* a, const rec_list_t* b) {
    if (a->n != b->n) return 1;
    for (int k = 0; k < a->n; k++) {
        const player_model_rec_t *x = &a->r[k], *y = &b->r[k];
        if (x->drawn != y->drawn) return 1;
        if (memcmp(x->spyro, y->spyro, PL.rec_len) != 0) return 1;
        if (x->drawn && (memcmp(x->seg, y->seg, PL.part_block_len) != 0 ||
                         memcmp(x->fk,  y->fk,  PL.colour_scratch_len)  != 0 ||
                         memcmp(x->sxy, y->sxy, PL.sxy_len) != 0)) return 1;
    }
    if (a->nsh != b->nsh) return 1;
    for (int j = 0; j < a->nsh; j++)
        if (a->sh[j].model != b->sh[j].model || a->sh[j].drawn != b->sh[j].drawn ||
            memcmp(a->sh[j].sh, b->sh[j].sh, PL.shadow_len) != 0) return 1;
    if (a->nfl != b->nfl) return 1;
    for (int j = 0; j < a->nfl; j++)
        if (a->fl[j].model != b->fl[j].model || a->fl[j].drawn != b->fl[j].drawn ||
            memcmp(a->fl[j].fl, b->fl[j].fl, PL.flame_len) != 0) return 1;
    return 0;
}

/* Two passes: index first, position-verified within the bound; then nearest
 * still-unpaired previous record within it, ties to the lower index. Runs on
 * the host loop's thread and reads no clock. */
static void pair_lists(rec_list_t* cur, const rec_list_t* prv) {
    int taken[PLAYER_REC_MAX] = {0};
    const double B = PL.pair_bound;
    for (int k = 0; k < cur->n; k++) { cur->r[k].pair = -1; cur->r[k].pair2 = -1; }
    if (!prv) { s_unpaired += (unsigned)cur->n; return; }
    /* One record each side: nothing to disambiguate, and the singleton this
     * replaced paired the ring's prev/curr unconditionally (the camera cut
     * guard, not the bound, decides a teleport there). */
    if (cur->n == 1 && prv->n == 1) { cur->r[0].pair = 0; cur->r[0].pair2 = prv->r[0].pair; return; }
    for (int k = 0; k < cur->n && k < prv->n; k++) {
        int32_t a[3], b[3]; rec_pos(&cur->r[k], a); rec_pos(&prv->r[k], b);
        if (dist3(a, b) <= B) { cur->r[k].pair = (int8_t)k; taken[k] = 1; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) continue;
        int32_t a[3]; rec_pos(&cur->r[k], a);
        int best = -1; double bd = 0.0;
        for (int j = 0; j < prv->n; j++) {
            if (taken[j]) continue;
            int32_t b[3]; rec_pos(&prv->r[j], b);
            double d = dist3(a, b);
            if (d <= B && (best < 0 || d < bd)) { best = j; bd = d; }
        }
        if (best >= 0) { cur->r[k].pair = (int8_t)best; taken[best] = 1; s_pass2++; }
    }
    for (int k = 0; k < cur->n; k++) {
        if (cur->r[k].pair >= 0) cur->r[k].pair2 = prv->r[cur->r[k].pair].pair;
        else s_unpaired++;
    }
}

static void summary(void) {
    fprintf(stderr,
        "[recdbg-summary] epochs=%u no_ring=%u max_calls m=%d s=%d f=%d spec_refused=%u cap_refused=%u "
        "max_epochs_per_vbl=%d roll_ring=%u roll_records=%u roll_both=%u dedupe=%u resets=%u "
        "pairing_only=%u unpaired=%u pass2=%u led_fires=%u led_match=%u led_miss=%u "
        "mismatch m=%u s=%u f=%u max_model_pos_delta=%.0f "
        "maxB n=1:%.0f n>=2:%.0f minsep=%.0f\n",
        s_epochs, s_no_ring, s_max_calls[0], s_max_calls[1], s_max_calls[2], s_spec_refused, s_cap_refused,
        s_max_epochs_vbl, s_roll_ring, s_roll_records, s_roll_both, s_dedupe, s_resets,
        s_pairing_only, s_unpaired, s_pass2, s_led_fires, s_led_match, s_led_miss,
        s_mismatch[0], s_mismatch[1], s_mismatch[2], s_max_model_delta,
        s_maxB_single, s_maxB_multi, s_minsep);
}

void player_records_epoch_commit(void) {
    if (!PL.base) return;
    const int dbg = player_records_dbg();
    static int armed = 0;
    if (dbg && !armed) { armed = 1; atexit(summary); }
    s_epochs++;

    /* Reset wherever the ring reset (savestate load, rewind, a runahead depth
     * switch): the store's history describes the pre-jump timeline. */
    uint32_t gen = openpete_interp_reset_generation();
    if (gen != s_reset_gen_seen) {
        s_reset_gen_seen = gen;
        for (int i = 0; i < 3; i++) s_store[i].valid = 0;
        s_latched_ring_ns = 0;
        s_resets++;
    }

    if (openpete_frame == s_last_commit_frame) s_epochs_this_vbl++;
    else { s_epochs_this_vbl = 1; s_last_commit_frame = openpete_frame; }
    if (s_epochs_this_vbl > s_max_epochs_vbl) s_max_epochs_vbl = s_epochs_this_vbl;

    static rec_list_t epoch;
    if ((s_list.n || s_list.nsh || s_list.nfl) && spyro_native_canon_epoch_fresh(s_list.stamp)) epoch = s_list;
    else { epoch.n = 0; epoch.nsh = 0; epoch.nfl = 0; }
    s_list.n = 0; s_list.nsh = 0; s_list.nfl = 0;

    const openpete_interp_snapshot_t* curr = openpete_interp_curr_snapshot();
    op_time_t ns = openpete_interp_curr_tick_ns();
    int ring_rolled = (ns != 0 && ns != s_latched_ring_ns);
    s_latched_ring_ns = ns;
    if (!curr) s_no_ring++;

    /* Pairing-only record: no hook observed the ring's tick-end position — the
     * avatar was not drawn this tick (hidden, blink) or the epoch had no call
     * at all. Appended last, never rendered; shadow and flame records bind to
     * it and the next drawn record pairs with it. */
    if (curr && reg_region(PLAYER_REC_MODEL) >= 0) {
        const uint8_t* ring = curr->data + curr->region_offsets[reg_region(PLAYER_REC_MODEL)];
        int seen = 0;
        for (int k = 0; k < epoch.n; k++)
            if (memcmp(epoch.r[k].spyro + PL.pos_off, ring + PL.pos_off, 12) == 0) { seen = 1; break; }
        /* --runahead: the hooks fire on the committed tick, the ring holds the
         * led one. Every record takes its lerp SOURCE from the led list — the
         * speculative fires of the ring's capture tick, index for index — the
         * committed bytes staying the staged base. A led list from another
         * frame or of another shape leaves the records committed and is
         * counted. */
        if (openpete_runahead_get() > 0 && epoch.n > 0 && s_led_fires_epoch > 0) {
            int led_ok = (s_led.frame > epoch.frame &&
                          s_led.frame <= curr->tick && s_led.n == epoch.n);
            for (int k = 0; led_ok && k < epoch.n; k++)
                if (s_led.r[k].drawn != epoch.r[k].drawn) led_ok = 0;
            if (led_ok) {
                for (int k = 0; k < epoch.n; k++) {
                    memcpy(epoch.r[k].src, s_led.r[k].spyro, PL.rec_len);
                    if (epoch.r[k].drawn) memcpy(epoch.r[k].seg, s_led.r[k].seg, PL.part_block_len);
                }
                if (s_led.nsh == epoch.nsh)
                    for (int j = 0; j < epoch.nsh; j++) memcpy(epoch.sh[j].src, s_led.sh[j].sh, PL.shadow_len);
                if (s_led.nfl == epoch.nfl)
                    for (int j = 0; j < epoch.nfl; j++) memcpy(epoch.fl[j].src, s_led.fl[j].fl, PL.flame_len);
                s_led_match++;
            } else s_led_miss++;
            seen = 1;
        }
        if (!seen && epoch.n < PLAYER_REC_MAX) {
            player_model_rec_t* r = &epoch.r[epoch.n++];
            memset(r, 0, sizeof *r);
            memcpy(r->spyro, ring, PL.rec_len);
            memcpy(r->src,   ring, PL.rec_len);
            r->drawn = 0; r->pair = -1; r->pair2 = -1; r->is_player = 1;
            s_pairing_only++;
        }
        /* The ring's avatar's shadow and flame records when their hooks did
         * not fire this tick: pairing-only, the ring's tick-end bytes (the
         * anchor and the ribbon state are written every tick, drawn or not),
         * so one that appears next tick lerps from them. Never rendered. */
        int rk = -1;
        for (int k = epoch.n - 1; k >= 0; k--)
            if (memcmp(epoch.r[k].src + PL.pos_off, ring + PL.pos_off, 12) == 0) { rk = k; break; }
        if (rk >= 0) {
            int has_sh = 0, has_fl = 0;
            for (int j = 0; j < epoch.nsh; j++) if (epoch.sh[j].model == rk) { has_sh = 1; break; }
            for (int j = 0; j < epoch.nfl; j++) if (epoch.fl[j].model == rk) { has_fl = 1; break; }
            if (!has_sh && epoch.nsh < PLAYER_REC_MAX && reg_region(PLAYER_REC_SHADOW) >= 0) {
                player_shadow_rec_t* d = &epoch.sh[epoch.nsh++];
                const uint8_t* rb = curr->data + curr->region_offsets[reg_region(PLAYER_REC_SHADOW)];
                memcpy(d->sh, rb, PL.shadow_len); memcpy(d->src, rb, PL.shadow_len);
                d->model = (int8_t)rk; d->drawn = 0;
            }
            if (!has_fl && epoch.nfl < PLAYER_REC_MAX && reg_region(PLAYER_REC_FLAME) >= 0) {
                player_flame_rec_t* d = &epoch.fl[epoch.nfl++];
                const uint8_t* rb = curr->data + curr->region_offsets[reg_region(PLAYER_REC_FLAME)];
                memcpy(d->fl, rb, PL.flame_len); memcpy(d->src, rb, PL.flame_len);
                d->model = (int8_t)rk; d->drawn = 0;
            }
        }
    }

    /* Roll or dedupe. */
    int rec_changed = !s_store[0].valid || lists_differ(&epoch, &s_store[0].list);
    if (ring_rolled && rec_changed) s_roll_both++;
    else if (ring_rolled)           s_roll_ring++;
    else if (rec_changed)           s_roll_records++;
    else                            s_dedupe++;
    if (ring_rolled || rec_changed) {
        s_store[2] = s_store[1];
        s_store[1] = s_store[0];
        s_store[0].list = epoch;
        s_store[0].valid = 1;
        /* One clock per store: the ring's stamp when it rolled (the store
         * carries the ring's grid exactly, PLL and deterministic alike); a
         * record-only roll stamps the VBL under the PLL and the store's own
         * grid, prev + P, under deterministic capture. */
        if (ring_rolled) s_store[0].ns = ns;
        else if (openpete_interp_deterministic() && s_store[1].valid)
            s_store[0].ns = s_store[1].ns + openpete_interp_det_period();
        else s_store[0].ns = g_openpete_tick_stamp ? g_openpete_tick_stamp : ns;
        pair_lists(&s_store[0].list, s_store[1].valid ? &s_store[1].list : NULL);
    }
    s_store_epoch = spyro_native_canon_epoch_stamp() - 1u;

    s_led_fires += s_led_fires_epoch;
    s_spec_refused += s_spec_refused_epoch;
    s_cap_refused  += s_cap_refused_epoch;
    if (dbg) {
        feeder_obs_t* m = &s_obs[PLAYER_REC_MODEL];
        int mm[PLAYER_REC_FEEDERS] = { 0, 0, 0 };
        double model_delta = 0.0;
        for (int f = 0; f < PLAYER_REC_FEEDERS; f++) {
            if (!curr || !s_obs[f].last_ok) continue;
            const int fr = reg_region(f);
            if (fr < 0) continue;
            const uint8_t* ring = curr->data + curr->region_offsets[fr];
            mm[f] = memcmp(s_obs[f].last, ring, s_reg[f].len) != 0;
            if (mm[f]) s_mismatch[f]++;
            if (f == PLAYER_REC_MODEL && mm[f]) {
                int32_t rp[3]; memcpy(rp, ring + PL.pos_off, 12);
                model_delta = dist3(s_obs[f].pos[s_obs[f].n - 1], rp);
                if (model_delta > s_max_model_delta) s_max_model_delta = model_delta;
            }
        }
        double B = 0.0, sep = -1.0;
        if (m->n > 0 && m->n == s_prev_model.n) {
            for (int k = 0; k < m->n; k++) { double d = dist3(m->pos[k], s_prev_model.pos[k]); if (d > B) B = d; }
            if (m->n == 1) { if (B > s_maxB_single) s_maxB_single = B; }
            else           { if (B > s_maxB_multi)  s_maxB_multi  = B; }
        }
        for (int i = 0; i < m->n; i++)
            for (int j = i + 1; j < m->n; j++) { double d = dist3(m->pos[i], m->pos[j]); if (sep < 0.0 || d < sep) sep = d; }
        if (sep >= 0.0 && (s_minsep < 0.0 || sep < s_minsep)) s_minsep = sep;
        for (int f = 0; f < PLAYER_REC_FEEDERS; f++)
            if (s_obs[f].n + s_obs[f].over > s_max_calls[f]) s_max_calls[f] = s_obs[f].n + s_obs[f].over;

        fprintf(stderr, "[recdbg f=%u e=%u] calls m=%d s=%d f=%d spec=%u led=%d@%u cap=%u epochs_vbl=%d ring=%d recchg=%d "
                "mm=%d/%d/%d mdelta=%.0f B=%.0f sep=%.0f store=%d pos=",
                openpete_frame, s_epochs, m->n + m->over, s_obs[1].n + s_obs[1].over, s_obs[2].n + s_obs[2].over,
                s_spec_refused_epoch, s_led_fires_epoch ? s_led.n : 0, s_led_fires_epoch ? s_led.frame : 0u,
                s_cap_refused_epoch, s_epochs_this_vbl, ring_rolled, rec_changed,
                mm[0], mm[1], mm[2], model_delta, B, sep, s_store[0].list.n);
        for (int k = 0; k < m->n; k++)
            fprintf(stderr, "%s(%d,%d,%d)c%u", k ? ";" : "", m->pos[k][0], m->pos[k][1], m->pos[k][2], m->cf[k]);
        fprintf(stderr, " pairs=");
        for (int k = 0; k < s_store[0].list.n; k++)
            fprintf(stderr, "%s%d%s", k ? "," : "", s_store[0].list.r[k].pair, s_store[0].list.r[k].drawn ? "" : "p");
        fprintf(stderr, " shadows=");
        for (int j = 0; j < s_store[0].list.nsh; j++)
            fprintf(stderr, "%s%d%s", j ? "," : "", s_store[0].list.sh[j].model, s_store[0].list.sh[j].drawn ? "" : "p");
        fprintf(stderr, " flames=");
        for (int j = 0; j < s_store[0].list.nfl; j++)
            fprintf(stderr, "%s%d%s", j ? "," : "", s_store[0].list.fl[j].model, s_store[0].list.fl[j].drawn ? "" : "p");
        fputc('\n', stderr);
        s_prev_model = *m;
    }
    memset(s_obs, 0, sizeof s_obs);
    s_spec_refused_epoch = 0;
    s_led_fires_epoch = 0;
    s_cap_refused_epoch = 0;
}

/* ---- present side -------------------------------------------------------- */

static int    s_pres_n = 0;
static int    s_pres_lerp = 0;
static double s_pres_alpha = 0.0;

int player_records_present_begin(int ring_paired, int ring_cut, double ring_alpha, double* out_alpha) {
    s_pres_n = 0; s_pres_lerp = 0; s_pres_alpha = 0.0;
    if (out_alpha) *out_alpha = 0.0;
    if (!s_store[0].valid) return 0;
    if (!spyro_native_canon_epoch_fresh(s_store_epoch)) return 0;
    s_pres_n = s_store[0].list.n;
    if (ring_paired && !ring_cut && s_store[1].valid) {
        s_pres_lerp = 1;
        if (s_store[0].ns == openpete_interp_curr_tick_ns()) {
            /* The store rolled with the ring: one clock, the ring's alpha. */
            s_pres_alpha = ring_alpha;
        } else {
            op_time_t p = s_store[0].ns - s_store[1].ns;
            if (p > g_openpete_tick_period_ns) p = g_openpete_tick_period_ns;
            double a = (p > 0) ? (double)((int64_t)openpete_interp_present_now() - (int64_t)s_store[0].ns) / (double)p : 1.0;
            if (a < -0.35) a = -0.35; else if (a > 1.0) a = 1.0;
            s_pres_alpha = a;
        }
    }
    if (out_alpha) *out_alpha = s_pres_alpha;
    return s_pres_n;
}

const player_model_rec_t* player_records_curr(int k) {
    return (k >= 0 && k < s_pres_n) ? &s_store[0].list.r[k] : NULL;
}
const player_model_rec_t* player_records_prev(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    if (!c || c->pair < 0 || !s_store[1].valid || c->pair >= s_store[1].list.n) return NULL;
    return &s_store[1].list.r[c->pair];
}
const player_model_rec_t* player_records_prev2(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    if (!c || c->pair2 < 0 || !s_store[2].valid || c->pair2 >= s_store[2].list.n) return NULL;
    return &s_store[2].list.r[c->pair2];
}
int player_records_lerps(int k) {
    const player_model_rec_t* c = player_records_curr(k);
    return c && s_pres_lerp && c->pair >= 0;
}
int player_records_shadow_count(void) { return s_pres_n ? s_store[0].list.nsh : 0; }
const player_shadow_rec_t* player_records_shadow_curr(int j) {
    return (s_pres_n && j >= 0 && j < s_store[0].list.nsh) ? &s_store[0].list.sh[j] : NULL;
}
const player_shadow_rec_t* player_records_shadow_prev(int j) {
    const player_shadow_rec_t* d = player_records_shadow_curr(j);
    const player_model_rec_t* m = d ? player_records_curr(d->model) : NULL;
    if (!m || m->pair < 0 || !s_store[1].valid) return NULL;
    const rec_list_t* prv = &s_store[1].list;
    for (int i = 0; i < prv->nsh; i++)
        if (prv->sh[i].model == m->pair) return &prv->sh[i];
    return NULL;
}
void player_records_shadow_note(int j, const int32_t* staged_anchor) {
    if (!player_records_dbg()) return;
    const player_shadow_rec_t* d = player_records_shadow_curr(j);
    if (!d) return;
    const player_shadow_rec_t* p = player_records_shadow_prev(j);
    fprintf(stderr, "[recdbg shadow f=%u j=%d] model=%d drawn=%d lerp=%d anchor=(%d,%d,%d)\n",
            openpete_frame, j, d->model, d->drawn, player_records_lerps(d->model) && p != NULL,
            staged_anchor ? staged_anchor[0] : 0, staged_anchor ? staged_anchor[1] : 0, staged_anchor ? staged_anchor[2] : 0);
}
int player_records_flame_count(void) { return s_pres_n ? s_store[0].list.nfl : 0; }
const player_flame_rec_t* player_records_flame_curr(int j) {
    return (s_pres_n && j >= 0 && j < s_store[0].list.nfl) ? &s_store[0].list.fl[j] : NULL;
}
const player_flame_rec_t* player_records_flame_prev(int j) {
    const player_flame_rec_t* d = player_records_flame_curr(j);
    const player_model_rec_t* m = d ? player_records_curr(d->model) : NULL;
    if (!m || m->pair < 0 || !s_store[1].valid) return NULL;
    const rec_list_t* prv = &s_store[1].list;
    for (int i = 0; i < prv->nfl; i++)
        if (prv->fl[i].model == m->pair) return &prv->fl[i];
    return NULL;
}
void player_records_flame_note(int j, const int32_t* staged_origin) {
    if (!player_records_dbg()) return;
    const player_flame_rec_t* d = player_records_flame_curr(j);
    if (!d) return;
    const player_flame_rec_t* p = player_records_flame_prev(j);
    fprintf(stderr, "[recdbg flame f=%u j=%d] model=%d drawn=%d lerp=%d origin=(%d,%d,%d)\n",
            openpete_frame, j, d->model, d->drawn, player_records_lerps(d->model) && p != NULL,
            staged_origin ? staged_origin[0] : 0, staged_origin ? staged_origin[1] : 0, staged_origin ? staged_origin[2] : 0);
}
void player_records_present_note(int k, const int32_t* staged_pos) {
    if (!player_records_dbg()) return;
    const player_model_rec_t* c = player_records_curr(k);
    if (!c) return;
    fprintf(stderr, "[recdbg present f=%u k=%d] drawn=%d pair=%d pair2=%d lerp=%d alpha=%.3f pos=(%d,%d,%d)\n",
            openpete_frame, k, c->drawn, c->pair, c->pair2, player_records_lerps(k), s_pres_alpha,
            staged_pos ? staged_pos[0] : 0, staged_pos ? staged_pos[1] : 0, staged_pos ? staged_pos[2] : 0);
}
void player_records_present_end(void) { s_pres_n = 0; s_pres_lerp = 0; }

op_time_t player_records_curr_ns(void) { return s_store[0].valid ? s_store[0].ns : 0; }

/* ---- module -------------------------------------------------------------- */

static feature_dispatch_entry_t s_dispatch[2];

static void player_apply_options(const openpete_options_t* o) {
    (void)o;
    s_dispatch[0].addr = 0u; s_dispatch[0].fn = NULL;
    s_dispatch[1].addr = 0u; s_dispatch[1].fn = NULL;
    if (!PL.hook_draw || !player_game.run_draw) return;
    s_dispatch[0].addr = PL.hook_draw;
    s_dispatch[0].fn   = on_player_draw;
}

const feature_module_t player_module = {
    .name           = "player_native",
    .apply_options  = player_apply_options,
    .dispatch_table = s_dispatch,
};
