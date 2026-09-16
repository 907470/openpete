/* games/spyro-3/renderer/moby_game.c — Spyro 3's half of the shared moby
 * feeder: the vertex-stream and face-stream decoders, the cull-record reader,
 * the model-kind test and the keyframe patch list.
 *
 * The walk itself is games/common/renderer/moby.c; the contract is
 * renderer/moby_game.h. Every claim below carries the asm/mobydraw.s line it
 * was read from: the draw is three handwritten routines with no decompiled C,
 * and Spyro 2 runs the same three instruction for instruction.
 *
 * Three shapes differ from Spyro 1 and drive everything here:
 *   - the cull writes the FINAL view-space translation (it applies the
 *     per-moby size rescale and drops the records that rescale pushes past
 *     the far limit itself, mobydraw.s:546-551,572-574), so the draw loads
 *     the record's translation straight into the GTE (mobydraw.s:898-900);
 *   - the cull copies each keyframe's leading stream word into the record
 *     rather than a pointer to it (mobydraw.s:532-535);
 *   - the vertex stream accumulates onto a per-model static vertex array and
 *     has a third codec form, a byte indexing a 32-bit adjustment table.
 */

#include "renderer/moby_game.h"
#include "renderer/native_gte.h"       /* ngte_read_matrix */
#include "game_layout.h"

#include <stddef.h>
#include <string.h>

/* The 32-bit adjustment table the one-byte vertex form indexes
 * (mobydraw.s:914). The draw XORs the table pointer's sign bit with record
 * word-0 bit 0 (mobydraw.s:913-915), which moves it between KSEG0 and KUSEG:
 * the same physical words either way, and the sign only selects whether the
 * draw also stores per-vertex clip flags (mobydraw.s:1094). The decode reads
 * the words, so it reads one address. */
#define D_VERTEX_ADJUST_TABLE  0x80066274u

/* AnimationHeader fields the stream decode needs and game_layout.moby does not
 * name, because only this codec has them: the frame-stream data base the two
 * halfword offsets in a keyframe word are relative to (mobydraw.s:967) and the
 * per-model static vertex array every form accumulates onto (mobydraw.s:975).
 */
#define ANIM_DATA_BASE   0x20u
#define ANIM_STATIC_VERT 0x0Cu

/* ---- vertex stream -------------------------------------------------
 * Three forms, chained: bit 0 of the item just consumed picks the next one,
 * and the keyframe word's bit 25 picks the first (mobydraw.s:968,986-988).
 * Bit 0 clear takes the ADJUST form — one byte from the adjust stream whose
 * (byte >> 1) & 0x7F indexes the 32-bit table; the table word is ADDED to this
 * vertex's static word and the sum unpacked (mobydraw.s:1011-1024). Bit 0 set
 * takes a halfword from the block stream, whose own bit 1 picks between the
 * 7/7/5 DELTA form, which accumulates the delta AND the static word
 * (mobydraw.s:1044-1057), and the two-halfword LITERAL form, which ASSIGNS and
 * so resets the accumulator (mobydraw.s:1073-1086). Every form consumes one
 * static word (mobydraw.s:1010).
 *
 * The unpack of a packed word is Z += w >> 21, X -= (w << 11) >> 21,
 * Y -= (w << 22) >> 21 — an 11-bit Z, an 11-bit X and a 10-bit Y at twice its
 * stored scale (mobydraw.s:1019-1024).
 *
 * vstream state: litp = the adjust byte cursor, shortp = the block halfword
 * cursor, scratch_u[0] = the static vertex cursor, flag = which form is next.
 */
static void s3_vstream_init(CPUState* cpu, moby_vstream_t* s,
                            uint32_t header, uint32_t frame, int s5) {
    memset(s, 0, sizeof *s);
    uint32_t data = cpu->read_word(header + ANIM_DATA_BASE);
    uint32_t block = data + ((frame & 0xFFFFu) << 1);
    s->shortp = block;
    s->litp   = block + ((frame >> 15) & 0x3FEu);
    s->scratch_u[0] = cpu->read_word(header + ANIM_STATIC_VERT);
    s->flag = (int)((frame >> 25) & 1u);   /* the first item's form */
    s->s5 = s5;                            /* no per-model shift in this codec */
}

static void s3_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    (void)first;   /* the keyframe word already said which form opens */
    uint32_t sw = cpu->read_word(s->scratch_u[0]);
    s->scratch_u[0] += 4u;
    if (s->flag == 0) {
        uint32_t b = cpu->read_byte(s->litp); s->litp += 1u;
        uint32_t tw = cpu->read_word(D_VERTEX_ADJUST_TABLE + (((b << 1) & 0x1FCu)));
        uint32_t w = tw + sw;
        s->VZ += (int32_t)w >> 21;
        s->VX -= (int32_t)(w << 11) >> 21;
        s->VY -= (int32_t)(w << 22) >> 21;
        s->flag = (int)(b & 1u);
    } else {
        int32_t h = (int16_t)cpu->read_half(s->shortp); s->shortp += 2u;
        s->flag = (int)((uint32_t)h & 1u);
        if (h & 2) {
            int32_t h2 = (int16_t)cpu->read_half(s->shortp); s->shortp += 2u;
            s->VY = (int32_t)(((uint32_t)h & 0xFFCu) << 20) >> 20;   /* bits 2-11 */
            s->VX = (int32_t)((uint32_t)((int32_t)((uint32_t)h2 << 26) >> 21)
                              | (((uint32_t)h & 0xF000u) >> 11));      /* bits 0-5 x32, bits 12-15 */
            s->VZ = h2 >> 5;
        } else {
            s->VZ += h >> 9;                                     /* bits 9-15 */
            s->VX -= (int32_t)((uint32_t)h << 21) >> 25;         /* bits 4-10 */
            s->VY -= ((int32_t)((uint32_t)h << 26) >> 27) << 2;  /* bits 1-5, x4 */
            s->VZ += (int32_t)sw >> 21;
            s->VX -= (int32_t)(sw << 11) >> 21;
            s->VY -= (int32_t)(sw << 22) >> 21;
        }
    }
}

/* ---- face stream ---------------------------------------------------
 * Fixed records, two words untextured and five textured, the textured flag
 * being word 1's sign (mobydraw.s:1530-1531,1536,1544). Word 0 holds four
 * 8-bit vertex indices, and the face is a triangle when the last two match
 * (mobydraw.s:1495-1506). Word 1 holds four 7-bit colour indices at bits
 * 24-30, 17-23, 10-16 and 3-9 (mobydraw.s:1554,1561-1563,1564-1566); on a
 * triangle the fourth index's field is flags instead — bit 3 accepts either
 * winding where a quad's face is front-facing only (mobydraw.s:1707,1731-1734),
 * and bits 4 and 5 select alternate colour sources for the same geometry
 * (mobydraw.s:1710,1793-1800). Bits 0-2 are a signed per-face ordering-table
 * bias, which is paint order, not geometry.
 *
 * The three texture words are {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16} and
 * {u2|v2<<8|u3<<16|v3<<24} (mobydraw.s:1655-1662); the second word's sign is
 * the GPU semi-transparency enable (mobydraw.s:1660-1661).
 *
 * The walk's own topology vocabulary carries this exactly: a quad face is a
 * "fresh" face with the winding flag clear, whose dual-NCLIP emit tree is the
 * same four-way one this draw runs (mobydraw.s:1564-1573); a triangle is a
 * "strip" face, whose keep rule is the same NCLIP sign test. */
static int s3_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                           moby_wface_t* out, int cap, moby_face_stats_t* st) {
    uint32_t end = fb + 4u + bytecount;
    uint32_t fp  = fb + 4u;
    int n = 0, raw = 0, guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        uint32_t w1 = cpu->read_word(fp + 4u);
        raw++;
        int textured = ((int32_t)w1) < 0;
        uint32_t adv = textured ? 0x14u : 0x08u;
        unsigned i0 = (w0 >> 24) & 0xFFu, i1 = (w0 >> 16) & 0xFFu;
        unsigned i2 = (w0 >>  8) & 0xFFu, i3 =  w0        & 0xFFu;
        int tri = (i2 == i3);
        if (n < cap) {
            moby_wface_t* F = &out[n];
            memset(F, 0, sizeof *F);
            F->i0 = (uint16_t)i0; F->i1 = (uint16_t)i1;
            F->i2 = (uint16_t)i2; F->i3 = (uint16_t)i3;
            F->c0 = (uint16_t)((w1 >> 22) & 0x1FCu);
            F->c1 = (uint16_t)((w1 >> 15) & 0x1FCu);
            F->c2 = (uint16_t)((w1 >>  8) & 0x1FCu);
            F->c3 = (uint16_t)(tri ? F->c2 : ((w1 >> 1) & 0x1FCu));
            F->fresh    = (uint8_t)(tri ? 0 : 1);
            F->bit0     = (uint8_t)(tri ? ((w1 >> 3) & 1u) : 0u);
            F->textured = (uint8_t)(textured ? 1 : 0);
            F->hl = 0; F->blendbit = 0; F->abr = 0; F->bpp = 16;
            if (textured) {
                uint32_t td0 = cpu->read_word(fp + 0x08u);
                uint32_t td1 = cpu->read_word(fp + 0x0Cu);
                uint32_t td2 = cpu->read_word(fp + 0x10u);
                F->u[0]=(uint8_t)(td0 & 0xFFu); F->v[0]=(uint8_t)((td0 >> 8) & 0xFFu);
                F->u[1]=(uint8_t)(td1 & 0xFFu); F->v[1]=(uint8_t)((td1 >> 8) & 0xFFu);
                F->u[2]=(uint8_t)(td2 & 0xFFu); F->v[2]=(uint8_t)((td2 >> 8) & 0xFFu);
                F->u[3]=(uint8_t)((td2 >> 16) & 0xFFu); F->v[3]=(uint8_t)((td2 >> 24) & 0xFFu);
                unsigned cl = (td0 >> 16) & 0xFFFFu;
                unsigned tp = (td1 >> 16) & 0x7FFFu;
                F->blendbit = (uint8_t)((td1 >> 31) & 1u);
                int mode = (int)((tp >> 7) & 3u);
                F->abr = (uint8_t)((tp >> 5) & 3u);
                F->tpx = (int16_t)((tp & 0xFu) * 64);
                F->tpy = (int16_t)(((tp >> 4) & 1u) * 256);
                F->bpp = (uint8_t)((mode == 0) ? 4 : (mode == 1) ? 8 : 16);
                F->clx = (int16_t)((cl & 0x3Fu) * 16);
                F->cly = (int16_t)((cl >> 6) & 0x1FFu);
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = 0; st->skips = 0;
    st->align = (fp == fb + 4u + bytecount) ? 0 : (fp == end) ? 1 : 2;
    return n;
}

/* ---- cull record ---------------------------------------------------
 * Stride 0x44 in the tick's draw scratch, terminated by a zero flags word
 * (mobydraw.s:888). The flags word carries the keyframe blend byte and, in its
 * top half, the LOD verdict whose sign picks the low-poly vertex count and
 * face list (mobydraw.s:918-921). The two keyframes are the stream words
 * themselves with their own headers at +0x04 and +0x08 (mobydraw.s:888-889,
 * 911,988-989). */
static int s3_read_record(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    const uint32_t w0 = cpu->read_word(rec);
    if (w0 == 0) return 0;
    R->w0       = w0;
    R->header   = cpu->read_word(rec + game_layout.moby.rec_header_off);
    R->header_b = cpu->read_word(rec + game_layout.moby.rec_header_b_off);
    R->frame_a  = cpu->read_word(rec + game_layout.moby.rec_frame_a_off);
    R->frame_b  = cpu->read_word(rec + game_layout.moby.rec_frame_b_off);
    R->moby     = cpu->read_word(rec + game_layout.moby.rec_moby_off);
    R->material = cpu->read_word(rec + game_layout.moby.rec_material_off);
    for (int i = 0; i < 3; i++)
        R->TR_raw[i] = (int32_t)cpu->read_word(rec + game_layout.moby.rec_tr_off + (uint32_t)i * 4u);
    ngte_read_matrix(cpu, rec + game_layout.moby.rec_matrix_off, R->M);
    R->tier = (int)(int16_t)(w0 >> 16);
    return 1;
}

/* A model-table entry that is a KSEG0 pointer is an animated model; a KUSEG
 * one is the second kind, drawn by its own pass (mobydraw.s:116). */
static int s3_model_is_animated(CPUState* cpu, uint32_t model) {
    (void)cpu;
    return (int32_t)model < 0;
}

/* The keyframe's faceblock patch list. The cull resolves it through the
 * Model's own table and stores the pointer at record +0x14, and marks a
 * keyframe that patches NOTHING by setting the stored keyframe word's top bits
 * (mobydraw.s:532-534,561-568); the draw skips the apply on exactly that sign
 * (mobydraw.s:935). */
static uint32_t s3_frame_patch_list(CPUState* cpu, uint32_t rec, uint32_t frame_a) {
    if ((int32_t)frame_a < 0) return 0u;
    return cpu->read_word(rec + 0x14u);
}

const moby_game_t moby_game = {
    .vstream_init      = s3_vstream_init,
    .vstream_next      = s3_vstream_next,
    .decode_faces      = s3_decode_faces,
    .read_record       = s3_read_record,
    .frame_patch_list  = s3_frame_patch_list,
    .model_is_animated = s3_model_is_animated,
    .simple_pass       = NULL,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so
     * nothing the host widens may reach it and the walk feeds the records the
     * cull queued, no more. */
    .host_reveal       = 0,
    /* The moby array is fixed with a live sentinel, so a record's Moby address
     * is its slot and is stable across ticks: the walk's default identity. */
    .interp_key        = NULL,
};
