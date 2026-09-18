/* games/common/renderer/player_game_spyro23.c — the per-game half of the
 * shared player feeder for the two games whose player draw is the same
 * handwritten routine: the skeleton's per-part transform, the seam that hangs
 * the head off the body, the vertex count field, and the face and colour
 * streams.
 *
 * The walk itself is games/common/renderer/player.c; the contract is
 * renderer/player_game.h and the two per-game tables are
 * renderer/player_game_spyro23.h. Every claim below carries the asm/spyrodraw.s
 * line it was read from, in the first game's numbering; the second game's line
 * is two higher throughout, its file holding the draw alone. The draw has no
 * decompiled C.
 *
 * Two parts, not three: a body and a head, whose transform records the routine
 * builds at draw scratch + 0x1400, 0x3C bytes apart, followed by a zero flags
 * word that terminates the block (spyrodraw.s:20,115,175,201). The vertex and
 * face encodings are the animated moby's, so those two slots point straight at
 * renderer/moby_game_spyro23.h.
 *
 * The one unit that is neither moby unit: the translation is the raw
 * camera-relative world delta rotated by the camera matrix at cam.base+0x00
 * and stored UNSHIFTED (spyrodraw.s:207-234,262-267), so it and every vertex
 * added to it are in full world units — where the animated moby kind's are
 * world >> AnimationHeader+2 and the static kind's are world/2.
 */

#include "renderer/player_game.h"
#include "renderer/player.h"
#include "renderer/moby_game_spyro23.h"
#include "renderer/native_gte.h"
#include "renderer/orchestrator.h"   /* openpete_native_interp_pos */
#include "renderer/player_game_spyro23.h"
#include "game_layout.h"
#include "openpete_recomp_module.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PL (game_layout.player)
#define PG (player_game_s23)

/* g_Spyro fields only this draw reads. +0x2C non-zero is the mirrored-copy
 * pass: it suppresses the head matrix compose (spyrodraw.s:268,282,316-318)
 * and the routine runs a second time with it clear for the avatar itself.
 * +0x20 byte 3 non-zero selects the pre-blend colour table the routine builds
 * at draw scratch + 0x1800 instead of the model's own block
 * (spyrodraw.s:740-751). */
#define SPYRO_MIRROR_GATE  0x2Cu
#define SPYRO_COLOUR_FILT  0x20u
#define COLOUR_SCRATCH_OFF 0x1800u

/* The routine's own entry: the walk's hook wraps it, so the dispatch re-enters
 * the hook, whose re-entrancy guard is already set and lets the real function
 * run. */
static void s23_run_draw(CPUState* cpu) {
    openpete_recomp_module_get()->dispatch(cpu, PL.hook_draw);
}

/* The Animation of part `part`. The animation word packs animation A in bits
 * 0-7, animation B in bits 8-15, keyframe A in bits 16-23 and keyframe B in
 * bits 24-31 (spyrodraw.s:37,44,51-54,86-88,118-121). */
static uint32_t s23_animation(CPUState* cpu, int part) {
    uint32_t tab = cpu->read_word(PG.anim_table);
    if (tab < 0x80000000u) return 0;
    uint32_t w = cpu->read_word(PL.base + PL.anim_off[part]);
    uint32_t a = cpu->read_word(tab + ((w << 2) & 0x3FCu));
    return (a >= 0x80000000u) ? a : 0u;
}

/* ---- the part records --------------------------------------------------
 * The draw composes a 0x3C-byte record per part at draw scratch + 0x1400 from
 * gamestate and the Animation table (spyrodraw.s:41-107 for the body,
 * 109-175 for the head), consumes it before the face loop, and then reuses
 * part 0's first twelve words as the straddle clip's own save area
 * (spyrodraw.s:1210-1248 writes them for a face the water plane cuts,
 * 1250-1264 reads them back). After the call returns, the body's record is
 * whatever the last cut face left there, so the walk composes the words
 * itself rather than reading the block back.
 *
 * Per part: the animation word (g_Spyro+0x14 body, +0x18 head) selects the
 * keyframe A Animation in bits 0-7 and keyframe B's in bits 8-15, with the
 * keyframe indices in bits 16-23 and 24-31. A keyframe record is 0x14 bytes
 * and the first one starts 0x14 into its Animation, so the fields load at
 * +0x14 and +0x18 of `entry + index * 0x14`. Keyframe word 0's low half is
 * the shared stream offset and one bit of its high half the stream's form
 * (bit 16 for the body, bit 17 for the head); word 1 packs the per-part
 * cursor offsets, the body reading bits 10-19 and the head bits 0-9 and
 * 20-31. The blend byte (+0x1C body, +0x1D head) indexes the blend table, and
 * the looked-up weight shifted left four is the record's IR0; zero there
 * leaves the record single-keyframe. The head's static cursor starts one word
 * per body vertex along, from the body Animation's own count byte. */
#define SPYRO_BLEND_OFF   0x1Cu   /* the head's is the next byte */

typedef struct { uint32_t entry, base, w0, w1; } s23_kf_t;

/* Animation `sel` of the table, at keyframe `idx`. */
static int s23_kf_read(CPUState* cpu, uint32_t tab, uint32_t sel, uint32_t idx, s23_kf_t* k) {
    uint32_t e = cpu->read_word(tab + sel);
    if (e < 0x80000000u) return 0;
    uint32_t r = e + idx * 0x14u;
    k->entry = e;
    k->base  = cpu->read_word(e + 0x10u);
    k->w0    = cpu->read_word(r + 0x14u);
    k->w1    = cpu->read_word(r + 0x18u);
    return 1;
}

static int s23_part_record(CPUState* cpu, int part, player_record_t* out) {
    if (part < 0 || part > 1) return 0;
    uint32_t tab = cpu->read_word(PG.anim_table);
    if (tab < 0x80000000u) return 0;
    uint32_t w  = cpu->read_word(PL.base + PL.anim_off[part]);
    uint32_t wt = cpu->read_byte(PG.blend_table
                                + cpu->read_byte(PL.base + SPYRO_BLEND_OFF + (uint32_t)part));
    s23_kf_t A;
    if (!s23_kf_read(cpu, tab, (w << 2) & 0x3FCu, (w >> 16) & 0xFFu, &A)) return 0;

    /* The head's streams start one word per body vertex in, from the body
     * Animation's own count byte (spyrodraw.s:57,140-141,169). */
    uint32_t head_skip = 0u;
    if (part == 1) {
        uint32_t wb = cpu->read_word(PL.base + PL.anim_off[0]);
        uint32_t eb = cpu->read_word(tab + ((wb << 2) & 0x3FCu));
        if (eb >= 0x80000000u) head_skip = (uint32_t)cpu->read_byte(eb + 0x02u) << 2;
    }

    uint32_t count = cpu->read_byte(A.entry + (part == 0 ? 0x02u : 0x03u));
    out->flags = (wt << 4) | (count << 16)
               | (part == 0 ? (0x80000000u | ((A.w0 >> 4) & 0x1000u))
                            : ((A.w0 >> 5) & 0x1000u));

    for (int kfr = 0; kfr < 2; kfr++) {
        const s23_kf_t* K = &A;
        s23_kf_t B;
        if (kfr) {
            if (!wt) break;
            if (!s23_kf_read(cpu, tab, (w >> 6) & 0x3FCu, (w >> 24) & 0xFFu, &B)) break;
            out->flags |= (part == 0 ? ((B.w0 >> 3) & 0x2000u) : ((B.w0 >> 4) & 0x2000u));
            K = &B;
        }
        uint32_t lo = K->w0 & 0xFFFFu;
        uint32_t* d = kfr ? out->stream_b : out->stream_a;
        d[0] = cpu->read_word(K->entry + 0x04u) + head_skip;
        if (part == 0) {
            d[1] = ((K->w1 >> 10) & 0x3FFu) + lo + K->base;
            d[2] = lo + K->base;
        } else {
            d[1] = (K->w1 >> 20) + lo + K->base;
            d[2] = lo + (K->w1 & 0x3FFu) + K->base;
        }
    }
    return 1;
}

/* Bits 16-23 of the flags word are the CUMULATIVE projected-vertex end, four
 * bytes per vertex: the routine's scratchpad cursor runs continuously across
 * the parts and stops at 0x1F800000 + ((flags >> 14) & 0x3FC)
 * (spyrodraw.s:376,388-391). A part's own count is the difference from the
 * previous part's end. */
static int s23_part_vcount(uint32_t flags, int* cursor) {
    int cur_end = (int)((flags >> 14) & 0x3FCu);
    int count = (cur_end - *cursor) >> 2;
    *cursor = cur_end;
    return count < 0 ? 0 : count;
}

/* M = start . Ry.Rx.Rz from a packed byte euler, which is what func_8004EA90
 * composes (asm/stdutil.s: the Y byte at bits 16-23 first, then X at 8-15,
 * then Z at 0-7, each against the running matrix). `part` selects the sub-tick
 * float angles the orchestrator handed in so rotation renders below the 8-bit
 * euler grid. */
static void s23_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot,
                             int out[3][3], int part) {
    ngte_euler_sc e;
    double ang[3];
    if (player_rot_interp(part, ang)) {
        ngte_sincos_interp(cpu, ang[0], &e.sz, &e.cz);
        ngte_sincos_interp(cpu, ang[1], &e.sx, &e.cx);
        ngte_sincos_interp(cpu, ang[2], &e.sy, &e.cy);
    } else {
        ngte_euler_sincos(cpu, rot, &e);
    }
    ngte_compose_euler(start, &e, out);
}

static int s23_part_xform(CPUState* cpu, const ngte_view* v, int part,
                         const player_xform_t* parent, player_xform_t* out) {
    memset(out, 0, sizeof *out);
    if (part == 0) {
        /* The swizzled-centre MVMVA (spyrodraw.s:207-234): IR1 = cam.y - s.y,
         * IR2 = cam.z - s.z, IR3 = s.x - cam.x through the camera matrix at
         * cam.base+0x00, sf = 1 so the result is (M . IR) >> 12, and the
         * routine stores it as the translation with no further shift
         * (spyrodraw.s:262-267): full world units. */
        int PM[3][3]; memcpy(PM, v->cam_M, sizeof PM);
        int32_t sx = (int32_t)cpu->read_word(PL.base + PL.pos_off + 0u);
        int32_t sy = (int32_t)cpu->read_word(PL.base + PL.pos_off + 4u);
        int32_t sz = (int32_t)cpu->read_word(PL.base + PL.pos_off + 8u);
        double sfx = (double)sx, sfy = (double)sy, sfz = (double)sz;
        { double sf[3];
          if (openpete_native_interp_pos(PL.base, sf)) { sfx = sf[0]; sfy = sf[1]; sfz = sf[2]; } }
        double IR1 = v->cam[1] - sfy, IR2 = v->cam[2] - sfz, IR3 = sfx - v->cam[0];
        for (int i = 0; i < 3; i++)
            out->TR[i] = (PM[i][0]*IR1 + PM[i][1]*IR2 + PM[i][2]*IR3) / 4096.0;
        s23_compose_euler(cpu, PM, cpu->read_word(PL.base + PL.rot_off[0]), out->M, 0);
        /* The mirrored copy (spyrodraw.s:283-302): with the gate set the draw
         * negates the composed matrix's second column in place — the high half
         * of the record word at +0x10, the low half of +0x18 and the high half
         * of +0x1C, which are m01, m11 and m21 — and writes the three GTE
         * rotation registers those halves live in. The reflection is in the
         * model's own second axis; the driver has already moved the avatar's
         * world Z to twice its height below the surface. */
        /* OPENPETE_PLAYER_VERIFY_NOMIRROR: the negative control of that
         * reflection — the copy is drawn from the unreflected matrix, leaving
         * every other mirrored-pass difference in place. */
        static int s_neg = -1;
        if (s_neg < 0) { const char* e = getenv("OPENPETE_PLAYER_VERIFY_NOMIRROR");
                         s_neg = !(e && *e && *e != '0'); }
        if (player_mirror_pass() && s_neg)
            for (int i = 0; i < 3; i++) out->M[i][1] = -out->M[i][1];
        out->present = 1;
        return 1;
    }
    if (!parent) return 0;
    /* The head keeps the body's matrix unless it has a rotation of its own AND
     * this is not the mirrored pass (spyrodraw.s:316-318,326-330). */
    uint32_t rot = player_mirror_pass()
                 ? 0u : cpu->read_word(PL.base + PL.rot_off[1]);
    if (rot) s23_compose_euler(cpu, parent->M, rot, out->M, part);
    else     memcpy(out->M, parent->M, sizeof out->M);
    out->present = 1;   /* the origin comes from the seam joint */
    return 1;
}

/* The seam joint (spyrodraw.s:176-199,307-325): the body keyframe record's
 * word at +0x08 carries the head's offset with a vertex delta's field layout
 * (Z = >>21, X = <<11>>21, Y = <<22>>21), INTPL'd between the two keyframes by
 * the body record's own blend weight and then transformed through the body's
 * matrix and translation. The keyframe records are 0x14 bytes and start 0x14
 * into the Animation (spyrodraw.s:51-54: the index is multiplied by 16 and a
 * quarter of that added, so 20 bytes, and every field load is +0x14 on). */
static int32_t s23_seam_word(CPUState* cpu, uint32_t anim, unsigned frame) {
    if (!anim) return 0;
    return (int32_t)cpu->read_word(anim + (uint32_t)frame * 0x14u + 0x1Cu);
}
static inline int s23_sat16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
static int s23_seam_field(int32_t jw, int32_t jwB, int lsh, int ir0, int dual) {
    int a = (int)((int32_t)((uint32_t)jw << lsh) >> 21);
    if (!dual) return a;
    int diff = (int)((int32_t)((uint32_t)jwB << lsh) >> 21) - a;
    diff = s23_sat16(diff);
    return (int)((((int64_t)a << 12) + (int64_t)ir0 * diff) >> 12);
}

static void s23_seam_joint(CPUState* cpu, const player_xform_t* parent,
                          int part, int ir0, int dual, double out[3]) {
    (void)part;
    out[0] = out[1] = out[2] = 0.0;
    if (!parent) return;
    uint32_t anim = s23_animation(cpu, 0);
    uint32_t w = cpu->read_word(PL.base + PL.anim_off[0]);
    int32_t jA = s23_seam_word(cpu, anim, (w >> 16) & 0xFFu);
    int32_t jB = 0;
    if (dual) {
        uint32_t tab = cpu->read_word(PG.anim_table);
        uint32_t animB = (tab >= 0x80000000u)
                       ? cpu->read_word(tab + ((w >> 6) & 0x3FCu)) : 0u;
        if (animB >= 0x80000000u) jB = s23_seam_word(cpu, animB, (w >> 24) & 0xFFu);
    }
    int VZ =  s23_seam_field(jA, jB, 0,  ir0, dual);
    int VX = -s23_seam_field(jA, jB, 11, ir0, dual);
    int VY = -s23_seam_field(jA, jB, 22, ir0, dual);
    for (int i = 0; i < 3; i++)
        out[i] = parent->TR[i]
               + ((double)parent->M[i][0]*VX + (double)parent->M[i][1]*VY
                + (double)parent->M[i][2]*VZ) / 4096.0;
}

/* One face stream and one colour block serve the whole skeleton: the body
 * Animation's +0x08 and +0x0C (spyrodraw.s:737-741,883,888-891). The colour
 * filter swaps the block for the pre-blend table the routine builds at draw
 * scratch + 0x1800 (spyrodraw.s:743-749). */
static int s23_part_faces(CPUState* cpu, int part, player_faces_t* out) {
    (void)part;
    uint32_t anim = s23_animation(cpu, 0);
    if (!anim) return 0;
    out->faces   = cpu->read_word(anim + 0x08u);
    out->colours = cpu->read_word(anim + 0x0Cu);
    if ((cpu->read_word(PL.base + SPYRO_COLOUR_FILT) >> 24) != 0) {
        uint32_t end = cpu->read_word(game_layout.polybuf.end);
        if (end >= 0x80000000u)
            out->colours = end - PL.part_base_polybuf_off + COLOUR_SCRATCH_OFF;
    }
    return 1;
}

/* The vertex stream is the animated moby's, with one difference: the literal
 * two-halfword form's Y field is 11 bits at bits 1-11 here
 * (spyrodraw.s:498-499) where the moby draw reads 12 bits at bits 2-11
 * (mobydraw.s:1076-1078), so a literal vertex decoded the moby way lands at
 * twice its model Y and every delta vertex after it inherits that origin. */
static void s23_player_vstream_step(CPUState* cpu, moby_vstream_t* s, int first) {
    s23_vstream_next_lit(cpu, s, first, 1);
}

/* The face record is the animated moby's, with two differences. The player
 * draw advances a fixed 0x14 bytes whether or not word 1's sign is set
 * (spyrodraw.s:936-937,960-961), where the moby draw sizes the record by that
 * sign; walking this stream with the variable stride desyncs it after the
 * first such face. And every face it keeps is emitted textured: the only
 * primitive codes the routine writes are POLY_GT3 and POLY_GT4
 * (spyrodraw.s:1064,1023 for the code, 1057,1018 for the 9- and 12-word
 * headers), and the three texture words sit in every record because the stride
 * is fixed. Word 1's sign is therefore the back-face exemption here, not a
 * texture flag, so it is decoded into `untested` and every face is textured.
 */
static int s23_player_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                                  moby_wface_t* out, int cap, moby_face_stats_t* st) {
    return s23_decode_faces_stride(cpu, fb, bytecount, out, cap, st, 0x14u, 1);
}

/* The per-face depth admission (spyrodraw.s:997-1006 for a quad, 1096-1109 for
 * a triangle): the summed vertex depths minus the face's own ordering bias
 * must stay above a threshold measured from the model's view-space origin, or
 * the draw drops the face before it emits anything. The threshold is
 * 4 * (originZ - 0x800), floored at 1 (spyrodraw.s:254,863-871: the origin's
 * MAC3 less 0x800, stashed in a GTE control register and shifted left two in
 * the face loop's preamble). The bias is the face word's low three bits,
 * signed, times 128 (spyrodraw.s:1001-1002). */
static int s23_face_depth_keep(CPUState* cpu, const player_xform_t* root,
                              const moby_wface_t* f, double depth_sum) {
    (void)cpu;
    double thr = root ? 4.0 * (root->TR[2] - 2048.0) : 0.0;
    if (thr <= 0.0) thr = 1.0;
    return (depth_sum - (double)f->otbias * 128.0 - thr) > 0.0;
}

/* The water-plane split (spyrodraw.s:776-841 the setup, 847-857 the per-vertex
 * distance pass, 1085-1092 the wholly-above test and 1151-1155 the
 * wholly-below one, 1157-1191 the below policy, 1192 on the cut).
 *
 * g_Spyro+0x28 is the split's offset below the avatar's own Z, written by each
 * level's animation overlay while the animation is a surface swim; zero every
 * other tick, and the whole pass is then inert. The plane sits at
 * position.z - |+0x28| (spyrodraw.s:776-782: the offset is subtracted at
 * either sign). Each vertex's distance is that constant less the second row of
 * the matrix at g_Spyro+0x30 applied to the decoded model vertex in the
 * routine's (Y, Z, X) register order (spyrodraw.s:847-854). The part's own
 * world origin — the three words at draw scratch + 0x1478 + 16*part, negated
 * in the first two components — goes through the same row as the GTE
 * translation of that pass (spyrodraw.s:817-840), so it truncates on its own
 * before the vertex term is added to it.
 *
 * +0x28 > 0 drops every wholly-below face outright; below zero the face is
 * kept and each of its vertex colours is DPCS'd at IR0 = 0x800 toward the
 * environment's far colour at G_ENVIRONMENT+0x34 (spyrodraw.s:794-801,
 * 1157-1189). The same branch loads L33 with +0x300 or -0x300 by the sign of
 * cam.base+0x30 plus the offset, and the negative case also carries -0x1800
 * into the face-admission threshold (spyrodraw.s:783-791,866-873): L33 << 3 is
 * added to a below face's depth sum (spyrodraw.s:1186-1191). */
#define SPYRO_PLANE_OFF   0x28u
#define SPYRO_PLANE_MAT   0x30u
#define PART_ORIGIN_OFF   0x78u   /* from the part block base, stride 16 */
#define PLANE_DIST_OFF    0x1000u /* the distance array, from the scratch base */

static void s23_plane_begin(CPUState* cpu, player_plane_t* pl) {
    int32_t off = (int32_t)cpu->read_word(PL.base + SPYRO_PLANE_OFF);
    if (off == 0) return;
    uint32_t block = player_part_block_base(cpu);
    if (block < 0x80000000u) return;

    int M[3][3];
    ngte_read_matrix(cpu, PL.base + SPYRO_PLANE_MAT, M);
    for (int i = 0; i < 3; i++) pl->row[i] = (double)M[1][i];

    pl->plane = (double)(int32_t)cpu->read_word(PL.base + PL.pos_off + 8u)
              - (double)(off < 0 ? -off : off);
    for (int p = 0; p < PLAYER_PARTS_MAX; p++) {
        uint32_t rec = block + PART_ORIGIN_OFF + (uint32_t)p * 16u;
        double o0 = -(double)(int32_t)cpu->read_word(rec + 4u);
        double o1 = -(double)(int32_t)cpu->read_word(rec + 8u);
        double o2 =  (double)(int32_t)cpu->read_word(rec + 0u);
        pl->tr[p] = floor((pl->row[0]*o0 + pl->row[1]*o1 + pl->row[2]*o2) / 4096.0);
    }

    for (int i = 0; i < 3; i++)
        pl->fc[i] = (int32_t)cpu->read_word(game_layout.fog.far_colour + (uint32_t)i*4u);
    pl->ir0 = 0x800;                     /* spyrodraw.s:1158-1159 */

    pl->drop_below = off > 0;
    if (!pl->drop_below) {
        int32_t camz = (int32_t)cpu->read_word(game_layout.cam.base + 0x30u);
        int l33 = ((camz + off) > 0) ? 0x300 : -0x300;
        pl->key_bias = (double)(l33 * 8);
        pl->thr_bias = (l33 < 0) ? -6144.0 : 0.0;
    }
    pl->dist_block = block - PL.part_off + PLANE_DIST_OFF;
    pl->active = 1;
}

/* The routine runs twice a tick: the mirrored copy first, with the gate word
 * set, then the avatar the gameplay drives with it clear (spyrodraw.s:268,
 * 282). Only the second call is fed; the first still gets a record, so the
 * pairing sees the same call sequence every tick. */
static int s23_call_is_player(CPUState* cpu) {
    return cpu->read_word(PL.base + SPYRO_MIRROR_GATE) == 0u;
}

/* The same gate read the other way: the call the pass driver makes with the
 * height above the reflective surface still in the state word
 * (drawutil.s/func_8001EC5C:39-90 sets it, draws, zeroes it, draws again). */
static int s23_call_is_mirror(CPUState* cpu) {
    return cpu->read_word(PL.base + SPYRO_MIRROR_GATE) != 0u;
}

const player_game_t player_game = {
    .run_draw         = s23_run_draw,
    .part_count       = NULL,            /* the fixed two of game_layout.player */
    .part_record      = s23_part_record,  /* the draw's own block does not survive
                                          * the call (spyrodraw.s:1210-1248) */
    .part_xform       = s23_part_xform,
    .seam_joint       = s23_seam_joint,
    .vstream_next     = s23_player_vstream_step,  /* the moby codec, player Y field */
    .part_vcount      = s23_part_vcount,
    .part_faces       = s23_part_faces,
    .decode_faces     = s23_player_decode_faces,
    /* The draw branches on the face word's sign before it reads either NCLIP
     * result, so a face carrying that bit is admitted untested
     * (spyrodraw.s:921,1060). */
    .face_flag_skips_nclip = 1,
    .face_depth_keep  = s23_face_depth_keep,
    .plane_begin      = s23_plane_begin,
    .sort_depth_front = 0,               /* no full-front sentinel: the byte at
                                          * +0x1E is only ever an ordering bias
                                          * (spyrodraw.s:250-255) */
    .call_is_player   = s23_call_is_player,
    .call_is_mirror   = s23_call_is_mirror,
    .verify           = NULL,
    .verify_enabled   = NULL,
};
