/* games/spyro-3/renderer/player_game.c — Spyro 3's half of the shared player
 * feeder: the skeleton's per-part transform, the seam that hangs the head off
 * the body, the vertex count field, and the face and colour streams.
 *
 * The walk itself is games/common/renderer/player.c; the contract is
 * renderer/player_game.h. Every claim below carries the asm/spyrodraw.s line
 * it was read from: the draw (func_8003CDA0) is one handwritten routine with
 * no decompiled C.
 *
 * Two parts, not three: a body and a head, whose transform records the routine
 * builds at draw scratch + 0x1400, 0x3C bytes apart, followed by a zero flags
 * word that terminates the block (spyrodraw.s:20,115,175,201). The vertex and
 * face encodings are the animated moby's, so those two slots point straight at
 * renderer/moby_game.c (renderer/moby_game_s3.h).
 *
 * The one unit that is neither moby unit: the translation is the raw
 * camera-relative world delta rotated by the camera matrix at G_CAMERA+0x00
 * and stored UNSHIFTED (spyrodraw.s:207-234,262-267), so it and every vertex
 * added to it are in full world units — where the animated moby kind's are
 * world >> AnimationHeader+2 and the static kind's are world/2.
 */

#include "renderer/player_game.h"
#include "renderer/player.h"
#include "renderer/moby_game_s3.h"
#include "renderer/native_gte.h"
#include "renderer/orchestrator.h"   /* openpete_native_interp_pos */
#include "game_layout.h"
#include "spyro3_symbols.gen.h"
#include "openpete_recomp_module.h"

#include <string.h>

#define PL (game_layout.player)

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
static void s3_run_draw(CPUState* cpu) {
    openpete_recomp_module_get()->dispatch(cpu, F_PLAYER_DRAW);
}

/* The Animation of part `part`. The animation word packs animation A in bits
 * 0-7, animation B in bits 8-15, keyframe A in bits 16-23 and keyframe B in
 * bits 24-31 (spyrodraw.s:37,44,51-54,86-88,118-121). */
static uint32_t s3_animation(CPUState* cpu, int part) {
    uint32_t tab = cpu->read_word(D_SPYRO_ANIMS);
    if (tab < 0x80000000u) return 0;
    uint32_t w = cpu->read_word(PL.base + PL.anim_off[part]);
    uint32_t a = cpu->read_word(tab + ((w << 2) & 0x3FCu));
    return (a >= 0x80000000u) ? a : 0u;
}

/* Bits 16-23 of the flags word are the CUMULATIVE projected-vertex end, four
 * bytes per vertex: the routine's scratchpad cursor runs continuously across
 * the parts and stops at 0x1F800000 + ((flags >> 14) & 0x3FC)
 * (spyrodraw.s:376,388-391). A part's own count is the difference from the
 * previous part's end. */
static int s3_part_vcount(uint32_t flags, int* cursor) {
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
static void s3_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot,
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

static int s3_part_xform(CPUState* cpu, const ngte_view* v, int part,
                         const player_xform_t* parent, player_xform_t* out) {
    memset(out, 0, sizeof *out);
    if (part == 0) {
        /* The swizzled-centre MVMVA (spyrodraw.s:207-234): IR1 = cam.y - s.y,
         * IR2 = cam.z - s.z, IR3 = s.x - cam.x through the camera matrix at
         * G_CAMERA+0x00, sf = 1 so the result is (M . IR) >> 12, and the
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
        s3_compose_euler(cpu, PM, cpu->read_word(PL.base + PL.rot_off[0]), out->M, 0);
        out->present = 1;
        return 1;
    }
    if (!parent) return 0;
    /* The head keeps the body's matrix unless it has a rotation of its own AND
     * this is not the mirrored pass (spyrodraw.s:316-318,326-330). */
    uint32_t rot = cpu->read_word(PL.base + SPYRO_MIRROR_GATE)
                 ? 0u : cpu->read_word(PL.base + PL.rot_off[1]);
    if (rot) s3_compose_euler(cpu, parent->M, rot, out->M, part);
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
static int32_t s3_seam_word(CPUState* cpu, uint32_t anim, unsigned frame) {
    if (!anim) return 0;
    return (int32_t)cpu->read_word(anim + (uint32_t)frame * 0x14u + 0x1Cu);
}
static inline int s3_sat16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }
static int s3_seam_field(int32_t jw, int32_t jwB, int lsh, int ir0, int dual) {
    int a = (int)((int32_t)((uint32_t)jw << lsh) >> 21);
    if (!dual) return a;
    int diff = (int)((int32_t)((uint32_t)jwB << lsh) >> 21) - a;
    diff = s3_sat16(diff);
    return (int)((((int64_t)a << 12) + (int64_t)ir0 * diff) >> 12);
}

static void s3_seam_joint(CPUState* cpu, const player_xform_t* parent,
                          int part, int ir0, int dual, double out[3]) {
    (void)part;
    out[0] = out[1] = out[2] = 0.0;
    if (!parent) return;
    uint32_t anim = s3_animation(cpu, 0);
    uint32_t w = cpu->read_word(PL.base + PL.anim_off[0]);
    int32_t jA = s3_seam_word(cpu, anim, (w >> 16) & 0xFFu);
    int32_t jB = 0;
    if (dual) {
        uint32_t tab = cpu->read_word(D_SPYRO_ANIMS);
        uint32_t animB = (tab >= 0x80000000u)
                       ? cpu->read_word(tab + ((w >> 6) & 0x3FCu)) : 0u;
        if (animB >= 0x80000000u) jB = s3_seam_word(cpu, animB, (w >> 24) & 0xFFu);
    }
    int VZ =  s3_seam_field(jA, jB, 0,  ir0, dual);
    int VX = -s3_seam_field(jA, jB, 11, ir0, dual);
    int VY = -s3_seam_field(jA, jB, 22, ir0, dual);
    for (int i = 0; i < 3; i++)
        out[i] = parent->TR[i]
               + ((double)parent->M[i][0]*VX + (double)parent->M[i][1]*VY
                + (double)parent->M[i][2]*VZ) / 4096.0;
}

/* One face stream and one colour block serve the whole skeleton: the body
 * Animation's +0x08 and +0x0C (spyrodraw.s:737-741,883,888-891). The colour
 * filter swaps the block for the pre-blend table the routine builds at draw
 * scratch + 0x1800 (spyrodraw.s:743-749). */
static int s3_part_faces(CPUState* cpu, int part, player_faces_t* out) {
    (void)part;
    uint32_t anim = s3_animation(cpu, 0);
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
static void s3_player_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    s3_vstream_next_lit(cpu, s, first, 1);
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
static int s3_player_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                                  moby_wface_t* out, int cap, moby_face_stats_t* st) {
    return s3_decode_faces_stride(cpu, fb, bytecount, out, cap, st, 0x14u, 1);
}

/* The per-face depth admission (spyrodraw.s:997-1006 for a quad, 1096-1109 for
 * a triangle): the summed vertex depths minus the face's own ordering bias
 * must stay above a threshold measured from the model's view-space origin, or
 * the draw drops the face before it emits anything. The threshold is
 * 4 * (originZ - 0x800), floored at 1 (spyrodraw.s:254,863-871: the origin's
 * MAC3 less 0x800, stashed in a GTE control register and shifted left two in
 * the face loop's preamble). The bias is the face word's low three bits,
 * signed, times 128 (spyrodraw.s:1001-1002). */
static int s3_face_depth_keep(CPUState* cpu, const player_xform_t* root,
                              const moby_wface_t* f, double depth_sum) {
    (void)cpu;
    double thr = root ? 4.0 * (root->TR[2] - 2048.0) : 0.0;
    if (thr <= 0.0) thr = 1.0;
    return (depth_sum - (double)f->otbias * 128.0 - thr) > 0.0;
}

/* The routine runs twice a tick: the mirrored copy first, with the gate word
 * set, then the avatar the gameplay drives with it clear (spyrodraw.s:268,
 * 282). Only the second call is fed; the first still gets a record, so the
 * pairing sees the same call sequence every tick. */
static int s3_call_is_player(CPUState* cpu) {
    return cpu->read_word(PL.base + SPYRO_MIRROR_GATE) == 0u;
}

const player_game_t player_game = {
    .run_draw         = s3_run_draw,
    .part_count       = NULL,            /* the fixed two of game_layout.player */
    .part_xform       = s3_part_xform,
    .seam_joint       = s3_seam_joint,
    .vstream_next     = s3_player_vstream_next,  /* the moby codec, player Y field */
    .part_vcount      = s3_part_vcount,
    .part_faces       = s3_part_faces,
    .decode_faces     = s3_player_decode_faces,
    /* The draw branches on the face word's sign before it reads either NCLIP
     * result, so a face carrying that bit is admitted untested
     * (spyrodraw.s:921,1060). */
    .face_flag_skips_nclip = 1,
    .face_depth_keep  = s3_face_depth_keep,
    .sort_depth_front = 0,               /* no full-front sentinel: the byte at
                                          * +0x1E is only ever an ordering bias
                                          * (spyrodraw.s:250-255) */
    .call_is_player   = s3_call_is_player,
    .verify           = NULL,
    .verify_enabled   = NULL,
};
