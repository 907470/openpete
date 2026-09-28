/* games/spyro-1/renderer/player_game.c — Spyro 1's half of the shared player
 * feeder: the delta-accumulated vertex codec, the face-stream decode, the
 * skeleton's per-part transform and its seam joints, and the game's own draw.
 *
 * The walk itself is games/common/renderer/player.c; the contract is
 * renderer/player_game.h. Three shapes separate this avatar from a moby:
 *   - vertices are DELTA-ACCUMULATED — each one is a signed (dx, dy, dz) added
 *     to a running accumulator in a dual 1-byte / 2-byte encoding with a wide
 *     escape, not a random-access packed triple;
 *   - the skeleton is three parts (body, head, tail), each with its own
 *     view x rotation matrix and a seam joint carried in the body's own
 *     keyframe stream;
 *   - the keyframe blend weight is 12 bits of the part record's flags word.
 *
 * Every field the walk reads is a game_layout.player field; the sites named in
 * the comments are r_pete.s, the handwritten routine at func_80023AC4.
 */

#include "renderer/player_game.h"
#include "renderer/player.h"
#include "renderer/moby.h"          /* moby_tiledef_decode: the same texture triple */
#include "renderer/native_gte.h"
#include "renderer/orchestrator.h"  /* openpete_native_interp_pos */
#include "game_layout.h"
#include "spyro1_symbols.gen.h"

#include <stdlib.h>
#include <string.h>

extern void func_80023AC4(CPUState* cpu);   /* the game's player renderer */

/* The numerical harness that verifies this decode against the routine's own
 * output; a Spyro 1 carve-out (renderer/spyro.c). */
int  spyro_verify_enabled(void);
void spyro_verify_run(CPUState* cpu, uint32_t startp, uint32_t endp);

#define PL (game_layout.player)

/** @brief Runs the game's own player renderer, func_80023AC4.
 *
 * The walk brackets the polybuf cursor around the call and reads back what
 * the routine leaves; the routine's output is never replaced.
 * @param cpu The guest CPU state. */
static void s1_run_draw(CPUState* cpu) { func_80023AC4(cpu); }

/** @brief Extracts a signed bit field as the MIPS sll-then-sra pair `(v << lsh) >> rsh` does.
 *
 * The left shift is unsigned, so it discards the high bits without
 * overflow; the right shift is arithmetic.
 * @param v The packed word.
 * @param lsh The left shift.
 * @param rsh The arithmetic right shift.
 * @return The sign-extended field. */
static inline int32_t sra_field(int32_t v, int lsh, int rsh) {
    return (int32_t)((uint32_t)v << lsh) >> rsh;
}

/* ---- vertex stream ------------------------------------------------------ */
/** @brief Decodes one vertex of one keyframe stream into the running accumulator.
 *
 * Advances whichever packed cursor this vertex uses. The per-vertex
 * correction word comes from the model's own animation-vertex array and is
 * consumed every vertex. The width is per vertex, carried in a flag bit of
 * the vertex just consumed: bit 0 of the byte in the 1-byte form
 * (.L800246C8 `andi $t0,$s1,0x1`), bit 14 of the halfword in the 2-byte
 * form (.L800247F0 `andi $t0,$s1,0x4000`, set before the escape branch so
 * it applies to escapes too). The first vertex's width comes from the part
 * record's flags word. A fixed per-part width desyncs the prefix sum at the
 * first switch.
 *
 * X and Y deltas subtract, Z adds. The 1-byte form indexes D_VERT_LUT and
 * adds the correction word to the entry. The 2-byte form's sign bit is a
 * wide escape that assigns the accumulator from two halfwords instead of
 * adding, and takes no correction word into the result; otherwise the
 * halfword's split and the correction word's split both add.
 *
 * Stream state: litp = the 1-byte cursor, shortp = the 2-byte cursor,
 * scratch_u[0] = the correction-word cursor, flag = the next vertex's width.
 * @param cpu The guest CPU state.
 * @param s The stream state, updated in place.
 * @param first Unused: the part record's flags word already set the opening width. */
static void s1_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    (void)first;   /* the part record's flags word already said which width opens */
    int32_t corr = (int32_t)cpu->read_word(s->scratch_u[0]); s->scratch_u[0] += 4u;
    if (!s->flag) {
        uint32_t b = cpu->read_byte(s->litp); s->litp += 1u;
        int32_t d = (int32_t)cpu->read_word(D_VERT_LUT + ((b << 1) & 0x1FCu)) + corr;
        s->VZ += sra_field(d, 0, 21);
        s->VX -= sra_field(d, 11, 21);
        s->VY -= sra_field(d, 22, 21);
        s->flag = (int)(b & 1u);
        return;
    }
    int16_t h = (int16_t)cpu->read_half(s->shortp); s->shortp += 2u;
    int next2 = ((uint16_t)h & 0x4000u) ? 1 : 0;
    if (h < 0) {                                     /* wide escape: absolute set */
        uint32_t s2 = cpu->read_half(s->shortp); s->shortp += 2u;
        s->VZ =  sra_field((int32_t)h, 18, 21);
        s->VX = -sra_field((int32_t)((s2 << 12) | (((uint32_t)(int32_t)h) << 28)), 0, 21);
        s->VY = -sra_field((int32_t)(s2 << 22), 0, 21);
    } else {                                         /* h-split plus corr-split */
        s->VZ += sra_field((int32_t)h, 18, 25);
        s->VX -= sra_field((int32_t)h, 23, 25);
        s->VY -= sra_field((int32_t)h, 28, 25);
        s->VZ += sra_field(corr, 0, 21);
        s->VX -= sra_field(corr, 11, 21);
        s->VY -= sra_field(corr, 22, 21);
    }
    s->flag = next2;
}

/** @brief Returns a part's vertex count from its flags word.
 *
 * Bits 14-23 of the flags word are the cumulative scratchpad end byte
 * offset: the projection cursor runs continuously across the parts and does
 * not reset, so a part's vertex count is the difference from the previous
 * part's end, divided by the four bytes one projected vertex occupies.
 * @param flags The part record's flags word.
 * @param cursor The previous part's end offset; receives this part's end.
 * @return The vertex count, 0 when the end offset went backward. */
static int s1_part_vcount(uint32_t flags, int* cursor) {
    int cur_end = (int)((flags >> 14) & 0x3FFu);
    int count = (cur_end - *cursor) >> 2;
    *cursor = cur_end;
    return count < 0 ? 0 : count;
}

/* ---- the skeleton's transforms ------------------------------------------
 * Re-derived from gamestate: the camera's projection matrix against the
 * avatar's pose and the seam joints, rather than read back from the records
 * the draw built (r_pete.s .L80023EE0 for the translation, .L800241E0 /
 * .L80024290 / .L80024344 for the eulers, D_80024054 for the seam).
 *
 *   TR_body = the swizzled-centre MVMVA: IR1 = cam.y - s.y, IR2 = cam.z - s.z,
 *     IR3 = s.x - cam.x, MAC = (PM . IR) >> 12 (sf = 1, no doubling — unlike a
 *     moby).
 *   M_body  = PM . Ry.Rx.Rz from the body euler word.
 *   M_head / M_tail = M_body . euler(their own word); with a zero word the
 *     part is STICKY and keeps M_body: the routine leaves the body matrix in
 *     the GTE and reloads only when the angle word is non-zero, which is
 *     exactly what sets the part record's flags sign bit. Keying on "the
 *     record's matrix slot is non-zero" instead is wrong — the setup pass can
 *     leave a stale non-zero matrix there with the sign bit CLEAR.
 *   TR_head / TR_tail = the seam joints below.
 */

/** @brief Reports whether OPENPETE_SPYRO_SCRATCH_MAT selects the part transforms read back from the part records.
 *
 * When set, s1_part_xform reads each part's matrix and translation out of
 * the records the draw built instead of deriving them from gamestate. The
 * derived path is the default. The variable is read once and cached.
 * @return 1 when the variable is set and not "0", else 0. */
static int s1_scratch_mat(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SPYRO_SCRATCH_MAT"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Composes M = start * Ry * Rx * Rz from a packed euler word.
 *
 * A zero word gives the identity (sin 0 = 0, cos 0 = 4096), so M = start,
 * the sticky case. When player_rot_interp holds sub-tick float angles for
 * `part`, they replace the word's bytes and render rotation below the 8-bit
 * euler grid; otherwise the byte-exact table lookup is used.
 * @param cpu The guest CPU state.
 * @param start The matrix to compose onto.
 * @param rot The packed euler word (Z at byte 0, X at byte 1, Y at byte 2).
 * @param out Receives the composed matrix.
 * @param part The skeleton part index, 0 = body. */
static void s1_compose_euler(CPUState* cpu, const int start[3][3], uint32_t rot,
                             int out[3][3], int part) {
    ngte_euler_sc e;
    double ang[3];
    if (player_rot_interp(part, ang)) {
        ngte_sincos_interp(cpu, ang[0], &e.sz, &e.cz);   /* Z (+0 byte) */
        ngte_sincos_interp(cpu, ang[1], &e.sx, &e.cx);   /* X (+1 byte) */
        ngte_sincos_interp(cpu, ang[2], &e.sy, &e.cy);   /* Y (+2 byte) */
    } else {
        ngte_euler_sincos(cpu, rot, &e);
    }
    ngte_compose_euler(start, &e, out);
}

/** @brief Derives one skeleton part's model-to-view transform from gamestate.
 *
 * Part 0 (the body) takes M = PM * Ry * Rx * Rz from the body euler word and
 * TR = PM * (camY - sy, camZ - sz, sx - camX) / 4096, using the interpolated
 * camera and avatar position when the view carries them. A later part takes
 * its own euler composed onto the parent's matrix, or the parent's matrix
 * when its euler word is zero, and leaves TR zero: the seam joint supplies
 * the origin. With OPENPETE_SPYRO_SCRATCH_MAT set, the matrix and
 * translation are read from the part records the draw built instead.
 * @param cpu The guest CPU state.
 * @param v The view the transform is expressed in.
 * @param part The part index, 0 = body.
 * @param parent Part 0's transform, NULL when part is 0.
 * @param out Receives the transform; zeroed first, present set on success.
 * @return 1 when the part exists in this pose, 0 otherwise. */
static int s1_part_xform(CPUState* cpu, const ngte_view* v, int part,
                         const player_xform_t* parent, player_xform_t* out) {
    memset(out, 0, sizeof *out);
    if (s1_scratch_mat()) {
        uint32_t block = player_part_block_base(cpu);
        if (!block) return 0;
        uint32_t e = block + (uint32_t)part * PL.part_stride;
        uint32_t fd = cpu->read_word(e + PL.part_flags_off);
        if (fd == 0) return 0;
        if (part == 0 || (fd & PL.part_matrix_sign))
            ngte_read_matrix(cpu, e + PL.part_matrix_off, out->M);
        else if (parent)
            memcpy(out->M, parent->M, sizeof out->M);
        for (int i = 0; i < 3; i++)
            out->TR[i] = (double)(int32_t)cpu->read_word(e + PL.part_tr_off + (uint32_t)i * 4u);
        out->present = 1;
        return 1;
    }

    if (part == 0) {
        int PM[3][3]; memcpy(PM, v->cam_M, sizeof PM);
        int32_t sx = (int32_t)cpu->read_word(PL.base + PL.pos_off + 0u);
        int32_t sy = (int32_t)cpu->read_word(PL.base + PL.pos_off + 4u);
        int32_t sz = (int32_t)cpu->read_word(PL.base + PL.pos_off + 8u);
        /* Prefer the FRACTIONAL interpolated camera and avatar positions and
         * drop the int16 truncation, so the body's view-space origin — and so
         * the whole model — is continuous below the one-world-unit grid. At
         * the canonical lockstep the resolver returns 0, the deltas are
         * integers and the derived translation is the same value carried as a
         * double; the int16 path never wrapped, since the camera follows the
         * avatar. */
        double cfx = v->cam[0], cfy = v->cam[1], cfz = v->cam[2];
        double sfx = (double)sx, sfy = (double)sy, sfz = (double)sz;
        { double sf[3];
          if (openpete_native_interp_pos(PL.base, sf)) { sfx = sf[0]; sfy = sf[1]; sfz = sf[2]; } }
        double IR1 = cfy - sfy, IR2 = cfz - sfz, IR3 = sfx - cfx;
        for (int i = 0; i < 3; i++)
            out->TR[i] = (PM[i][0]*IR1 + PM[i][1]*IR2 + PM[i][2]*IR3) / 4096.0;
        s1_compose_euler(cpu, PM, cpu->read_word(PL.base + PL.rot_off[0]), out->M, 0);
        out->present = 1;
        return 1;
    }

    if (!parent || !PL.rot_off[part]) return 0;
    uint32_t rot = cpu->read_word(PL.base + PL.rot_off[part]);
    if (rot) s1_compose_euler(cpu, parent->M, rot, out->M, part);
    else     memcpy(out->M, parent->M, sizeof out->M);
    out->present = 1;   /* the origin comes from the seam joint */
    return 1;
}

/** @brief Saturates an int to the signed 16-bit range [-32768, 32767]. */
static inline int s1_sat16(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

/** @brief Extracts one seam-joint field and blends it between the two keyframes as the GTE INTPL does.
 *
 * Result = (IR << 12 + IR0 * sat16(FC - IR)) >> 12, with IR the field of
 * keyframe A and FC the field of keyframe B. A single keyframe is the raw
 * field. Integer-exact, so the seam translation matches the record the
 * draw built byte for byte; a float lerp and truncate drifts by up to two
 * units at the near-plane tail tip.
 * @param jw Keyframe A's joint word.
 * @param jwB Keyframe B's joint word.
 * @param lsh The field's left shift before the arithmetic >> 21.
 * @param ir0 The keyframe blend weight (12-bit fraction).
 * @param dual 1 when two keyframes blend, 0 for keyframe A alone.
 * @return The blended field. */
static int s1_seam_field(int32_t jw, int32_t jwB, int lsh, int ir0, int dual) {
    int a = sra_field(jw, lsh, 21);
    if (!dual) return a;
    int diff = sra_field(jwB, lsh, 21) - a;      /* 11-bit fields, so this fits int16 */
    diff = s1_sat16(diff);
    return (int)((((int64_t)a << 12) + (int64_t)ir0 * diff) >> 12);
}

/** @brief Computes a head or tail part's origin from the body's seam joint.
 *
 * The joint is the body keyframe stream's word at [0x10] (head) or [0x14]
 * (tail), read at cursor - 8 or cursor - 4 since the cursor sits 0x18 past
 * the keyframe block's base. Its field layout matches a vertex delta (Z =
 * >>21, X = <<11>>21, Y = <<22>>21) with X and Y negated; it is blended by
 * the body's keyframe weight and transformed through the body's matrix and
 * translation (the seam RTPS at D_80024054), in double precision, without
 * the routine's per-component saturation. The stream cursor is a static
 * asset pointer, so reading the keyframe block through it is an asset read.
 *
 * The caller passes the body record's blend weight, not the 16-level
 * integer progress byte: the sub-tick re-extract writes a fine
 * interpolated weight into that record, and the byte would step the seam in
 * coarse increments while the vertices are smooth.
 * @param cpu The guest CPU state.
 * @param parent The body's transform.
 * @param part 1 = head, 2 = tail.
 * @param ir0 The body's keyframe blend weight.
 * @param dual 1 when two keyframes blend, else 0.
 * @param out Receives the part's view-space translation; zero when the part block or the parent is absent. */
static void s1_seam_joint(CPUState* cpu, const player_xform_t* parent,
                          int part, int ir0, int dual, double out[3]) {
    uint32_t block = player_part_block_base(cpu);
    if (!block || !parent) { out[0] = out[1] = out[2] = 0.0; return; }
    uint32_t curA = cpu->read_word(block + PL.part_stream_a_off + 8u);
    uint32_t curB = cpu->read_word(block + PL.part_stream_b_off + 8u);
    /* The cursor sits 0x18 past the keyframe block's base, so the head word is
     * at cursor - 8 and the tail word at cursor - 4. */
    const uint32_t off = (part == 1) ? 8u : 4u;
    int32_t jA = (int32_t)cpu->read_word(curA - off);
    int32_t jB = dual ? (int32_t)cpu->read_word(curB - off) : 0;
    int VZ =  s1_seam_field(jA, jB, 0,  ir0, dual);
    int VX = -s1_seam_field(jA, jB, 11, ir0, dual);
    int VY = -s1_seam_field(jA, jB, 22, ir0, dual);
    /* FRACTIONAL: the body's translation plus (M . V) / 4096. Carrying the
     * body's fractional translation through to the seam keeps the head and
     * tail origins tracking the continuous body below the one-unit grid; the
     * projection's own IR16 saturation handles off-screen, so the per-
     * component saturation the routine applies is dropped. */
    for (int i = 0; i < 3; i++)
        out[i] = parent->TR[i]
               + ((double)parent->M[i][0]*VX + (double)parent->M[i][1]*VY
                + (double)parent->M[i][2]*VZ) / 4096.0;
}

/* ---- face stream -------------------------------------------------------- */
/** @brief Decodes the avatar's face stream into walk faces.
 *
 * All three parts stitch into one mesh with one face stream and one colour
 * table, so the walk decodes it once and the face indices slot straight
 * into the projection.
 *
 * Word 0: sign < 0 = FRESH (quad-capable), >= 0 = STRIP (one triangle);
 * bit 0 is the emit-rule flag; three vertex indices at
 * ((w0 >> {20,11,2}) & 0x7FC) >> 2. Word 1: three colour byte-offsets at
 * ((w1 >> {17,8}) & 0x7FC) and ((w1 << 1) & 0x7FC), and bit 0 is the
 * semi-transparency enable. A FRESH record is 0x18 bytes with the fourth
 * index and colour in word 2 and the texture triple at +0x0C; a STRIP
 * record is 0x14 with the triple at +0x08 and no fourth index (i3 =
 * 0xFFFF, c3 = c2). The walk stops at 8192 records.
 * @param cpu The guest CPU state.
 * @param fb The face stream's base; its first word is the byte count and the records follow.
 * @param bytecount The records' byte count.
 * @param out Receives the decoded faces.
 * @param cap The capacity of out; records past it are counted but not stored.
 * @param st Receives the raw record count and align = 0 when the walk ended exactly at the stream end, else 2.
 * @return The number of faces stored. */
static int s1_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                           moby_wface_t* out, int cap, moby_face_stats_t* st) {
    uint32_t fp = fb + 4u, end = fb + 4u + bytecount;
    int n = 0, raw = 0, guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        uint32_t w1 = cpu->read_word(fp + 4u);
        int fresh = ((int32_t)w0) < 0;
        uint32_t adv = fresh ? 0x18u : 0x14u;
        uint32_t uvbase = fresh ? fp + 0x0Cu : fp + 0x08u;
        raw++;
        if (n < cap) {
            moby_wface_t* F = &out[n];
            memset(F, 0, sizeof *F);
            F->i0 = (uint16_t)(((w0 >> 20) & 0x7FCu) >> 2);
            F->i1 = (uint16_t)(((w0 >> 11) & 0x7FCu) >> 2);
            F->i2 = (uint16_t)(((w0 >>  2) & 0x7FCu) >> 2);
            F->c0 = (uint16_t)((w1 >> 17) & 0x7FCu);
            F->c1 = (uint16_t)((w1 >>  8) & 0x7FCu);
            F->c2 = (uint16_t)((w1 <<  1) & 0x7FCu);
            F->fresh = (uint8_t)(fresh ? 1 : 0);
            F->bit0  = (uint8_t)(w0 & 1u);
            F->textured = 1;
            F->blendbit = (uint8_t)(w1 & 1u);
            moby_tiledef_decode(cpu->read_word(uvbase),
                                cpu->read_word(uvbase + 4u),
                                cpu->read_word(uvbase + 8u), F);
            if (fresh) {
                uint32_t w2 = cpu->read_word(fp + 8u);
                F->i3 = (uint16_t)((w2 & 0x7FCu) >> 2);
                F->c3 = (uint16_t)((w2 >> 9) & 0x7FCu);
                /* The fourth corner's texel rides in the high half of the
                 * third texture word, which moby_tiledef_decode already put
                 * in u[3]/v[3]. */
            } else {
                F->i3 = 0xFFFFu;   /* no fourth vertex: the emit tree stays on the strip rule */
                F->c3 = F->c2;
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = 0; st->skips = 0;
    st->align = (fp == end) ? 0 : 2;
    return n;
}

/** @brief Returns the face and colour streams the whole skeleton shares, from the body's animation header.
 *
 * The header is the model's +0x38 table entry for the body's animation
 * index; faces are at +0x14, colours at +0x18. When the colour filter's
 * top byte is non-zero (the filter is interpolating), the colours come from
 * the pre-blend table the draw built in the render scratch instead of the
 * model's static block.
 * @param cpu The guest CPU state.
 * @param part Unused: every part shares the body's streams.
 * @param out Receives the face and colour stream addresses.
 * @return 1 when the header is a KSEG0 pointer, else 0. */
static int s1_part_faces(CPUState* cpu, int part, player_faces_t* out) {
    (void)part;
    uint32_t model = cpu->read_word(game_layout.moby.model_table) | 0x80000000u;
    uint32_t animIdx = cpu->read_byte(PL.base + PL.anim_off[0]);
    uint32_t animHdr = cpu->read_word(model + 0x38u + animIdx * 4u);
    if (animHdr < 0x80000000u) return 0;
    out->faces   = cpu->read_word(animHdr + 0x14u);
    out->colours = cpu->read_word(animHdr + 0x18u);
    if (PL.colour_filter_off &&
        (cpu->read_word(PL.base + PL.colour_filter_off) >> 24) != 0)
        out->colours = PL.part_base + PL.colour_scratch_off;
    return 1;
}

const player_game_t player_game = {
    .run_draw         = s1_run_draw,
    .part_count       = NULL,          /* the fixed three of game_layout.player */
    .part_xform       = s1_part_xform,
    .seam_joint       = s1_seam_joint,
    .vstream_next     = s1_vstream_next,
    .part_vcount      = s1_part_vcount,
    .part_faces       = s1_part_faces,
    .decode_faces     = s1_decode_faces,
    /* The routine's own "ordering slot 0 — in front of everything" sentinel,
     * saturated during the level-transition flight (r_pete.s 80023F8C). */
    .sort_depth_front = 127,
    /* The strip face's flag bit exempts it from the back-face test, so a
     * flagged face draws at either winding. */
    .face_winding_flag = 1,
    .call_is_player   = NULL,          /* every call is the avatar */
    .verify           = spyro_verify_run,
    .verify_enabled   = spyro_verify_enabled,
};
