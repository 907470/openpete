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
#include "renderer/moby_game_s3.h"   /* the two decoders the player feeder shares */
#include "renderer/moby.h"            /* the second kind's shared emit */
#include "renderer/native_gte.h"       /* ngte_read_matrix, ngte_mac, ngte_mat_mul3, ngte_dpcs_ch */
#include "game_layout.h"
#include "spyro3_symbols.gen.h"        /* G_CAMERA, F_MOBY_DRAW_STATIC */
#include "openpete_recomp_module.h"    /* the game's own static draw */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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

void s3_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved) {
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
            /* The literal Y field is where the two draws' codecs part. The
             * moby draw clears the two form bits and sign-extends bits 2-11 as
             * a 12-bit value (mobydraw.s:1076-1078); the player draw keeps
             * them and arithmetic-shifts the sign-extended low 12 bits right
             * by one, which is the 11-bit field at bits 1-11 with the form bit
             * folded into its LSB (spyrodraw.s:498-499,659-660,682-683). */
            s->VY = y_lit_halved ? ((int32_t)((uint32_t)h << 20) >> 21)
                                 : ((int32_t)(((uint32_t)h & 0xFFCu) << 20) >> 20);
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

void s3_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    s3_vstream_next_lit(cpu, s, first, 0);
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
int s3_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                           moby_wface_t* out, int cap, moby_face_stats_t* st,
                           uint32_t fixed_stride, int all_textured) {
    uint32_t end = fb + 4u + bytecount;
    uint32_t fp  = fb + 4u;
    int n = 0, raw = 0, guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        uint32_t w1 = cpu->read_word(fp + 4u);
        raw++;
        int sign = ((int32_t)w1) < 0;
        int textured = all_textured ? 1 : sign;
        uint32_t adv = fixed_stride ? fixed_stride : (textured ? 0x14u : 0x08u);
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
            F->untested = (uint8_t)(sign ? 1 : 0);
            /* Bits 4-5 of a triangle's flag field arm the special-material
             * dispatch (mobydraw.s:1707,1710): bit 4 takes a second mechanism
             * this decode does not carry, bit 5 alone takes the three
             * treatments the record's material word selects between. The same
             * word's bits 7-30 are then a signed face normal instead of the
             * three colour indices (mobydraw.s:2552-2570). */
            F->hl = (uint8_t)(tri ? ((w1 & 0x10u) ? 2u : (w1 & 0x20u) ? 1u : 0u) : 0u);
            F->n1 = (int8_t)((w1 >> 15) & 0xFFu);
            F->n2 = (int8_t)((w1 >>  7) & 0xFFu);
            F->n3 = (int8_t)((w1 >> 23) & 0xFFu);
            F->blendbit = 0; F->abr = 0; F->bpp = 16;
            /* Bits 0-2, signed: the depth key's per-face ordering bias
             * (mobydraw.s:1712-1713, spyrodraw.s:1000-1001). */
            F->otbias = (int8_t)(((int32_t)(w1 << 29)) >> 29);
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

/* The moby draw's own stride: two words untextured, five textured. */
int s3_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                    moby_wface_t* out, int cap, moby_face_stats_t* st) {
    return s3_decode_faces_stride(cpu, fb, bytecount, out, cap, st, 0u, 0);
}

/* ---- per-face special materials (mobydraw.s:1870-1875) ----------------
 *
 * A triangle whose flag bit 5 is set leaves the ordinary emit and takes one of
 * three treatments, selected not by the face but by the record's material word
 * at +0x3C: the draw stashes that word in GTE control 8 at entry
 * (mobydraw.s:987), and where it is non-zero replaces it with its own bits
 * 28-30 as a 0..3 mode and runs that mode's per-moby setup
 * (mobydraw.s:1457-1460,1984-1998). Mode 3 and above disarm the treatment and
 * draw the face normally; a zero material word leaves control 8 at zero, which
 * is mode 0 with no setup.
 *
 * The material word packs three 8-bit colour channels at bits 0-7, 8-15 and
 * 16-23, each widened to the GTE's 12-bit form, plus a scalar in bits 24-27.
 *
 *   mode 0 (mobydraw.s:2215-2262) — the channels are the far colour RFC/GFC/BFC
 *     (mobydraw.s:2073-2076); each vertex depth-cues from BLACK toward it at
 *     IR0 = (the vertex colour's three channels summed) << 3, so the face
 *     grades by its own luminance.
 *   mode 1 (mobydraw.s:2273-2301) — the same far colour, but one IR0 for the
 *     whole moby, bits 24-27 << 8 (mobydraw.s:2079), and each vertex depth-cues
 *     from its own colour. IR0 zero leaves the colours alone.
 *   mode 2 (mobydraw.s:2551-2564) — the specular sheen. The channels are the
 *     GTE background colour, bits 24-27 doubled is IR0 (mobydraw.s:2024-2045),
 *     and the light matrix is three copies of one constant vector
 *     (mobydraw.s:2048-2057). The face normal rotates into view by the model
 *     matrix, scales by IR0 and projects onto that vector on top of the
 *     background colour, giving one colour for the whole face.
 *
 * All three replace the face's vertex colours and nothing else: the face keeps
 * the texture its own record carries. The special emit's command word starts at
 * the untextured code (mobydraw.s:2562) and the shared emit the three treatments
 * fall into adds the texture bit back from the face record, so a treatment that
 * forced its face untextured would drop the texture the draw keeps.
 *
 * CARVE-OUT: flag bit 4 takes a different dispatcher (mobydraw.s:2113) that
 * this decode does not carry; the walk leaves those faces on the ordinary
 * path.
 */

/* The light matrix mode 2 loads, all three rows equal (mobydraw.s:2048-2057:
 * control 16-20 = F666F4E0, F4E0F62B, F62BF666, F666F4E0, F62B). */
static const int S3_SPEC_LIGHT[3] = { -2848, -2458, -2517 };

static int s3_clamp_ir(long v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int)v;
}

static int s3_special_face(uint32_t mat, const int M[3][3],
                           const moby_wface_t* F, uint32_t c[4], int* textured) {
    (void)textured;   /* every treatment replaces colours only */
    if (F->hl != 1) return 0;                      /* bit 4: the other dispatcher */
    int mode = mat ? (int)((mat >> 28) & 7u) : 0;
    if (mode > 2) return 0;                        /* the treatment is disarmed */

    /* The three 12-bit channels the material word carries. */
    int mc[3] = { (int)((mat <<  4) & 0xFF0u),
                  (int)((mat >>  4) & 0xFF0u),
                  (int)((mat >> 12) & 0xFF0u) };

    if (mode == 0) {
        for (int k = 0; k < 4; k++) {
            int sum = (int)(c[k] & 0xFFu) + (int)((c[k] >> 8) & 0xFFu)
                    + (int)((c[k] >> 16) & 0xFFu);
            int ir0 = sum << 3;
            uint32_t o = 0u;
            for (int ch = 0; ch < 3; ch++)
                o |= (uint32_t)ngte_dpcs_ch(0, mc[ch], ir0) << (ch * 8);
            c[k] = o;
        }
        return 1;
    }

    if (mode == 1) {
        int ir0 = (int)((mat >> 16) & 0xF00u);
        if (!ir0) return 0;                        /* mobydraw.s:2288 */
        for (int k = 0; k < 4; k++) {
            uint32_t o = 0u;
            for (int ch = 0; ch < 3; ch++)
                o |= (uint32_t)ngte_dpcs_ch((int)((c[k] >> (ch * 8)) & 0xFFu),
                                            mc[ch], ir0) << (ch * 8);
            c[k] = o;
        }
        return 2;
    }

    /* mode 2. The normal through the model matrix, scaled by IR0, projected on
     * the light vector over the background colour. Every stage truncates the
     * way its GTE op does: MVMVA and CC shift by 12, GPF does not, and CC's
     * second stage multiplies by a white 8-bit modulating colour. */
    int ir0 = (int)((mat >> 23) & 0x1Eu);
    int ir[3];
    for (int i = 0; i < 3; i++) {
        long m = (long)M[i][0] * F->n1 + (long)M[i][1] * F->n2
               + (long)M[i][2] * F->n3;
        ir[i] = s3_clamp_ir(m >> 12);
    }
    for (int i = 0; i < 3; i++) ir[i] = s3_clamp_ir((long)ir0 * ir[i]);
    long dot = (long)S3_SPEC_LIGHT[0] * ir[0] + (long)S3_SPEC_LIGHT[1] * ir[1]
             + (long)S3_SPEC_LIGHT[2] * ir[2];
    uint32_t o = 0u;
    for (int ch = 0; ch < 3; ch++) {
        long mac = (((long)mc[ch] * 0x1000) + dot) >> 12;
        if (mac < 0) mac = 0; else if (mac > 32767) mac = 32767;
        long v = (((long)(255 << 4) * mac) >> 12) >> 4;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        o |= (uint32_t)v << (ch * 8);
    }
    c[0] = c[1] = c[2] = c[3] = o;
    return 3;
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


/* ---- the second model kind (the static draw, mobydraw.s:3862) ----------
 *
 * The cull sorts each drawn moby by the sign of its model-table entry
 * (mobydraw.s:116) and writes the KUSEG half into a second, stride-0x10 queue
 * at draw scratch +0x1C00, bounded at 63 records by the cursor limit it keeps
 * in GTE control 8 and 9 (mobydraw.s:44,46). One record is
 *
 *   +0x00  Moby*
 *   +0x04  (camX - mobyX) >> 2 in the high half, (camY - mobyY) >> 2 in the low
 *   +0x08  (camZ - mobyZ) >> 2 in the high half, the far-cull distance in the low
 *   +0x0C  Model + (Moby+0x3C byte 0) * 4, whose +0x14 is the geometry header
 *
 * (mobydraw.s:617-630; the three offsets are the camera-relative quarter-world
 * position the cull already computed, mobydraw.s:82,87,96). A record whose
 * far-distance half has bit 15 set is the screen-space kind: the draw skips the
 * transform and takes the record's own halves as the GTE screen offsets
 * (mobydraw.s:4030-4049). CARVE-OUT: that kind is not fed.
 *
 * The draw transforms the record by the camera matrix at G_CAMERA+0x00 with
 * the vector (+0x04 low, +0x08 high, +0x04 high) in IR1/IR2/IR3
 * (mobydraw.s:3946-3952), then doubles the result into the GTE translation
 * (mobydraw.s:4058-4060,4190-4192). The record holds quarter-world units, so
 * the translation — and every model vertex added to it — is in world/2: full
 * world units are twice the transformed value, which is what this pass hands
 * the shared walk.
 *
 * The 16-byte geometry header is
 *
 *   +0x00  colour count in byte 0, high-poly vertex count in byte 1,
 *          low-poly vertex count in byte 2
 *   +0x04  four bounding-box bytes, each a multiple of 16 in the cull tests;
 *          its sign selects the low-poly vertex count
 *   +0x08  high-poly face-stream offset in the low half, high-poly colour-block
 *          offset in the high half, both relative to the header
 *   +0x0C  the same pair for the low-poly lists
 *   +0x10  the vertex words, one per vertex
 *
 * (mobydraw.s:4196,4216-4219,4291-4297,4336-4346,4222). A vertex word packs
 * X in bits 11-21 (11 bits), Y in bits 0-11 (12 bits) and Z in bits 21-31
 * (11 bits), absolute, with no accumulator (mobydraw.s:4222-4229).
 *
 * The face stream opens with its own byte count (mobydraw.s:4368). A face is
 * two words, five when word 1's sign marks it textured (mobydraw.s:4421-4431).
 * Word 0 holds four 7-bit vertex indices at bits 21, 14, 7 and 0
 * (mobydraw.s:4387-4390) and a signed ordering-table bias in its top nibble;
 * the face is a triangle when the last two indices match (mobydraw.s:4397).
 * Word 1 holds four 7-bit colour indices at bits 24, 17, 10 and 3
 * (mobydraw.s:4446,4438,4444,4611-4613); its bottom two bits instead select
 * the flat-lit material, whose colour comes from a face normal rather than the
 * colour block (mobydraw.s:4440,4720). The three texture words are the same
 * {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16}, {u2|v2<<8|u3<<16|v3<<24} triple
 * the animated stream uses, with word 1's sign the semi-transparency enable
 * (mobydraw.s:4519-4520,4536).
 *
 * Colours are indexed per vertex out of the header's colour block. Inside the
 * fade band the draw depth-cues each entry of the high-poly block toward the
 * matching entry of the low-poly block by IR0 = 4096 - tier
 * (mobydraw.s:4285-4290,4314-4321) and biases the CLUT row by
 * (4096 - tier) >> 9 (mobydraw.s:4303,4335,4542); past the band it reads the
 * low-poly block directly. */

#define D_SIMPLE_SKIP_LOWPOLY 0x8006C5DCu  /* the far-geometry suppression word the cull
                                            * parks in GTE data 3 (mobydraw.s:34,36) */
#define D_SIMPLE_FLAT_COLTAB  0x80066998u  /* four words per Moby+0x54 entry: the flat-lit
                                            * material's base and lit colours (mobydraw.s:4353-4364) */

#define S3_SIMPLE_RECS   63
#define S3_SIMPLE_VERTS  256
#define S3_SIMPLE_COLS   256
#define S3_SIMPLE_FACES  4096

/* The cull's own queue, copied at the hook. The queue lives in the tick's
 * draw scratch, which the passes after this one build their own data in, so
 * the bytes are taken while they are still the cull's. */
static uint32_t s_simple_q[S3_SIMPLE_RECS][4];
static int      s_simple_qn;

/* Per-frame engagement counts: a pass that walked nothing proves nothing, so
 * the verify harness prints what this one actually read. */
static long s_simple_recs, s_simple_verts, s_simple_faces, s_simple_tris;
static long s_simple_flat, s_simple_screen, s_simple_culled;
static long s_flat_zero_colour, s_flat_back_emitted, s_flat_back_dropped;
extern unsigned openpete_frame;

/* The four flat-material words the draw keeps in PSX scratchpad at
 * 0x1F8003F0..0x1F8003FC (mobydraw.s:4360-4363, $gp = 0x1F800000). They are
 * reloaded from the table only for a record whose Moby+0x54 is non-zero
 * (mobydraw.s:4350-4352), so a record with a zero index draws with whatever
 * the previous record — or the previous draw call — left there. Snapshotting
 * the words before the game's own draw gives the decode the same entry state
 * that draw started from, and a sub-tick replay the same state again, without
 * ever writing the scratchpad. */
#define S3_FLAT_SCRATCH 0x1F8003F0u
static uint32_t s_flat_entry[4];

static void s3_simple_capture(CPUState* cpu) {
    s_simple_qn = 0;
    /* The cull left the draw scratch base in GTE data 2 (mobydraw.s:35,3893)
     * and parked this queue 0x1C00 into it (mobydraw.s:44,3895). */
    uint32_t q = cpu->gte_data[2] + 0x1C00u;
    for (int i = 0; i < S3_SIMPLE_RECS; i++, q += 0x10u) {
        uint32_t m = cpu->read_word(q);
        if (m == 0u) break;                     /* the cull's terminator (mobydraw.s:584) */
        s_simple_q[i][0] = m;
        s_simple_q[i][1] = cpu->read_word(q + 4u);
        s_simple_q[i][2] = cpu->read_word(q + 8u);
        s_simple_q[i][3] = cpu->read_word(q + 12u);
        s_simple_qn = i + 1;
    }
}

/* R := R · Rot(axis, angle), the column pair the draw replaces through two
 * MVMVAs per axis: Y keeps column 2, X keeps column 1, Z keeps column 3
 * (mobydraw.s:4063-4110, 4111-4158, 4159-4189). */
static void s3_simple_rotate(CPUState* cpu, int R[3][3], int axis, unsigned angle) {
    if (angle == 0u) return;
    uint32_t t = game_layout.sin_table + (angle << 1);
    int sn = (int16_t)cpu->read_half(t);
    int cs = (int16_t)cpu->read_half(t + 0x80u);   /* cos is 64 entries on (mobydraw.s:4066) */
    int A[3][3] = { {4096,0,0}, {0,4096,0}, {0,0,4096} }, O[3][3];
    if (axis == 1) { A[0][0]=cs; A[0][2]=-sn; A[2][0]=sn; A[2][2]=cs; }        /* Y */
    else if (axis == 0) { A[1][1]=cs; A[1][2]=-sn; A[2][1]=sn; A[2][2]=cs; }   /* X */
    else { A[0][0]=cs; A[0][1]=-sn; A[1][0]=sn; A[1][1]=cs; }                  /* Z */
    ngte_mat_mul3(R, A, O);
    memcpy(R, O, sizeof O);
}

/* The flat-lit face colour (mobydraw.s:4758-4813). Word 1's bits 7-30 are a
 * face normal instead of four colour indices; the draw rotates it into view by
 * the model matrix, projects it onto the light vector it loads into the GTE
 * colour matrix at entry (all three rows of that matrix are the same vector,
 * mobydraw.s:3874-3880, so the result is one dot product), and blends the two
 * colours of the moby's entry in the flat-colour table by it: the second word
 * scaled by the projection, the first added flat, plus a highlight that opens
 * above 1472. The back side halves the flat word and quarters the projection
 * (mobydraw.s:4790-4795). */
static uint32_t s3_simple_flat_colour(const int R[3][3], uint32_t w1,
                                      uint32_t cbase, uint32_t clit, int backface) {
    int n1 = (int8_t)((w1 >> 15) & 0xFFu);
    int n2 = (int8_t)((w1 >>  7) & 0xFFu);
    int n3 = (int8_t)((w1 >> 23) & 0xFFu);
    int ir[3];
    for (int i = 0; i < 3; i++) {
        long long m = (long long)R[i][0]*n1 + (long long)R[i][1]*n2 + (long long)R[i][2]*n3;
        ir[i] = (int)(m >> 12);
    }
    int ir0 = (-1916*ir[0] + -3065*ir[1] + -3072*ir[2]) >> 8;
    if (backface) { ir0 >>= 2; cbase >>= 1; }
    int hi = ir0 > 1472 ? (ir0 - 1472) << 3 : 0;
    int out = 0;
    for (int ch = 0; ch < 3; ch++) {
        int shift = ch * 8;
        int base = (int)((cbase >> shift) & 0xFFu) << 4;
        int lit  = (int)((clit  >> shift) & 0xFFu) << 4;
        int v = base + ((ir0 * lit) >> 12) + hi;
        if (v < 0) v = 0; else if (v > 4095) v = 4095;
        out |= (v >> 4) << shift;
    }
    return (uint32_t)out;
}

static double s3_nclip(const float s[3][2], int a, int b, int c) {
    return (double)(s[b][0]-s[a][0]) * (double)(s[c][1]-s[a][1])
         - (double)(s[c][0]-s[a][0]) * (double)(s[b][1]-s[a][1]);
}

static int s3_simple_decode(CPUState* cpu) {
    int Rcam[3][3];
    ngte_read_matrix(cpu, G_CAMERA, Rcam);     /* mobydraw.s:3898-3908 */
    int skip_lp = cpu->read_word(D_SIMPLE_SKIP_LOWPOLY) != 0u;  /* mobydraw.s:4336-4341 */
    int emitted = 0;

    /* The camera position the cull subtracted is baked into every record's
     * delta (mobydraw.s:53 the camera position, :81,87,94 the three
     * subtractions, :621-627 the store), so replaying a record under a staged
     * camera re-rotates it but keeps the tick's translation.
     * Re-base the delta by the camera-position change. The cull stores
     * (moby - cam) >> 2 on the first component and (cam - moby) >> 2 on the
     * other two, so a camera that moved by e shifts the stored halves by
     * -e/4, +e/4 and +e/4. Held in double and added before the transform, so a
     * staged position equal to the tick's is a no-op bit for bit. */
    double dcam[3] = { 0.0, 0.0, 0.0 };
    int32_t tickpos[3];
    if (moby_native_tick_camera_pos(tickpos)) {
        static const double sign[3] = { -0.25, 0.25, 0.25 };
        for (int i = 0; i < 3; i++) {
            int32_t staged = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)i);
            dcam[i] = ((double)staged - (double)tickpos[i]) * sign[i];
        }
    }

    static double vpos[S3_SIMPLE_VERTS][3];
    static float  vscr[S3_SIMPLE_VERTS][2];
    static uint32_t ctab[S3_SIMPLE_COLS];
    uint32_t flat_w[4];
    memcpy(flat_w, s_flat_entry, sizeof flat_w);

    for (int r = 0; r < s_simple_qn; r++) {
        uint32_t moby = s_simple_q[r][0];
        uint32_t w1 = s_simple_q[r][1], w2 = s_simple_q[r][2], slot = s_simple_q[r][3];
        unsigned fardist = w2 & 0xFFFFu;
        if (fardist & 0x8000u) { s_simple_screen++; continue; }   /* screen-space kind */
        s_simple_recs++;

        /* view-space centre, world/2 after the doubling (mobydraw.s:3946-3952,4058-4060) */
        double m1, m2, m3;
        ngte_mac(Rcam, (double)(int16_t)(w1 & 0xFFFFu) + dcam[1],
                       (double)(int16_t)(w2 >> 16)    + dcam[2],
                       (double)(int16_t)(w1 >> 16)    + dcam[0], &m1, &m2, &m3);
        if (m3 - (double)fardist >= 0.0) { s_simple_culled++; continue; }  /* mobydraw.s:3958 */

        uint32_t H = cpu->read_word(slot + 0x14u);
        if (H < 0x80000000u) { s_simple_culled++; continue; }
        uint32_t h0 = cpu->read_word(H);
        uint32_t h1 = cpu->read_word(H + 4u);

        /* The cull tests the draw repeats, on the header's four bounding-box
         * bytes scaled by 16 (mobydraw.s:3963-3975,3976-3981,3993-4002). */
        double bx3 = (double)((h1 >> 20) & 0xFF0u), bx2 = (double)((h1 >> 12) & 0xFF0u);
        double bx1 = (double)((h1 >>  4) & 0xFF0u), bx0 = (double)((h1 <<  4) & 0xFF0u);
        double ax = m1 < 0 ? -m1 : m1, ay = m2 < 0 ? -m2 : m2;
        if (4.0*(ax - bx3) - 3.0*(m3 + bx2) >= 0.0) { s_simple_culled++; continue; }
        if (m3 + bx1 <= 0.0) { s_simple_culled++; continue; }
        if ((m3 + bx0) - 3.0*(ay - bx1) <= 0.0) { s_simple_culled++; continue; }

        double TR[3] = { m1*2.0, m2*2.0, m3*2.0 };   /* the stored translation, world/2 */
        int trz = (int)TR[2];

        /* The ordering-table key the draw drops the record on
         * (mobydraw.s:4193-4207): its far end is a cull, not paint order. */
        uint32_t rot = cpu->read_word(moby + game_layout.moby.angle_off);
        int key = (2*trz >> 7) - (int)(int8_t)(rot >> 24);
        if (key < 0) key = 0;
        if (key >= 272 && (key << 3) + 256 >= 3840) { s_simple_culled++; continue; }

        /* The fade/LOD tier: the moby's size byte against twice the depth
         * (mobydraw.s:4193-4196). Its sign picks the low-poly lists. */
        int tier = ((int)cpu->read_byte(moby + 0x4Eu) << 10) - 2*trz;

        int nv = (int)((tier >= 0 ? (h0 >> 8) : (h0 >> 16)) & 0xFFu);
        if (nv < 3) continue;
        if (nv > S3_SIMPLE_VERTS) nv = S3_SIMPLE_VERTS;

        int R[3][3];
        memcpy(R, Rcam, sizeof R);
        s3_simple_rotate(cpu, R, 1, (rot >> 16) & 0xFFu);
        s3_simple_rotate(cpu, R, 0, (rot >>  8) & 0xFFu);
        s3_simple_rotate(cpu, R, 2,  rot        & 0xFFu);

        moby_simple_record(cpu, moby);

        for (int i = 0; i < nv; i++) {
            uint32_t w = cpu->read_word(H + 0x10u + (uint32_t)i * 4u);
            double vx = (double)((int32_t)(w << 10) >> 21);   /* bits 11-21 */
            double vy = (double)((int32_t)(w << 20) >> 20);   /* bits 0-11 */
            double vz = (double)((int32_t)w >> 21);           /* bits 21-31 */
            double e1, e2, e3;
            ngte_mac(R, vx, vy, vz, &e1, &e2, &e3);
            /* world/2 in the game's space; the shared walk takes full world. */
            vpos[i][0] = (e1 + TR[0]) * 2.0;
            vpos[i][1] = (e2 + TR[1]) * 2.0;
            vpos[i][2] = (e3 + TR[2]) * 2.0;
            moby_simple_project(vpos[i], &vscr[i][0], &vscr[i][1]);
        }
        s_simple_verts += nv;

        /* OPENPETE_MOBY_POSTRACE_CLS: the submitted screen-space centroid of
         * each simple-kind record, the counterpart of the animated kind's
         * trace in the shared feeder. */
        {
            static int s_cls = -2;
            if (s_cls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_cls = e ? (int)strtol(e, NULL, 0) : -1; }
            if (s_cls >= 0) {
                double cx = 0.0, cy = 0.0;
                for (int i = 0; i < nv; i++) { cx += vscr[i][0]; cy += vscr[i][1]; }
                fprintf(stderr, "[simple f=%u] m=%08x n=%d c=(%.2f,%.2f) p=(%d,%d,%d) rot=%08x\n",
                        openpete_frame, moby, nv, cx / nv, cy / nv,
                        (int)cpu->read_word(moby + game_layout.moby.pos_off),
                        (int)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
                        (int)cpu->read_word(moby + game_layout.moby.pos_off + 8u), rot);
            }
        }

        /* The face and colour lists, and the fade the near band applies
         * (mobydraw.s:4285-4346). */
        /* The vertex count switches on the tier's sign, the face and colour
         * lists one step earlier: tier 0 takes the high-poly count and the
         * low-poly lists (mobydraw.s:4215-4219,4285-4287). */
        if (tier <= 0 && skip_lp) continue;
        uint32_t pair = cpu->read_word(H + (tier > 0 ? 0x08u : 0x0Cu));
        uint32_t faces = H + (pair & 0xFFFFu);
        uint32_t cols  = H + (pair >> 16);
        int ncol = (int)(h0 & 0xFFu);
        if (ncol > S3_SIMPLE_COLS) ncol = S3_SIMPLE_COLS;
        int clut_bias = 0;
        uint32_t far_cols = H + cpu->read_half(H + 0x0Eu);
        int fade = (tier > 0 && tier < 4096 && far_cols != cols);
        int ir0 = fade ? 4096 - tier : 0;
        if (fade) clut_bias = ir0 >> 9;
        for (int i = 0; i < ncol; i++) {
            uint32_t c = cpu->read_word(cols + (uint32_t)i * 4u);
            if (fade) {
                uint32_t f = cpu->read_word(far_cols + (uint32_t)i * 4u);
                int rr = ngte_dpcs_ch((int)(c & 0xFFu),        (int)((f <<  4) & 0xFF0u), ir0);
                int gg = ngte_dpcs_ch((int)((c >> 8) & 0xFFu), (int)((f >>  4) & 0xFF0u), ir0);
                int bb = ngte_dpcs_ch((int)((c >> 16) & 0xFFu),(int)((f >> 12) & 0xFF0u), ir0);
                c = (uint32_t)(rr | (gg << 8) | (bb << 16));
            }
            ctab[i] = c & 0xFFFFFFu;
        }

        /* The flat-colour entry is reloaded only for a record that names one;
         * a record whose Moby+0x54 is zero draws with the last entry loaded
         * (mobydraw.s:4351-4352,4353-4364). */
        uint32_t mcol = cpu->read_word(moby + game_layout.moby.colour_off);
        if (mcol) {
            uint32_t e = D_SIMPLE_FLAT_COLTAB + (mcol << 4);
            flat_w[0] = cpu->read_word(e);
            flat_w[1] = cpu->read_word(e + 4u);
            flat_w[2] = cpu->read_word(e + 8u);
            flat_w[3] = cpu->read_word(e + 12u);
        }

        uint32_t bytecount = cpu->read_word(faces);
        uint32_t fp = faces + 4u, end = fp + bytecount;
        int guard = 0;
        while (fp + 8u <= end && guard++ < S3_SIMPLE_FACES) {
            uint32_t f0 = cpu->read_word(fp);
            uint32_t f1 = cpu->read_word(fp + 4u);
            int textured = ((int32_t)f1) < 0;
            uint32_t adv = textured ? 0x14u : 0x08u;
            int idx[4] = { (int)((f0 >> 21) & 0x7Fu), (int)((f0 >> 14) & 0x7Fu),
                           (int)((f0 >>  7) & 0x7Fu), (int)( f0        & 0x7Fu) };
            int tri = (idx[2] == idx[3]);
            s_simple_faces++;
            if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv || (!tri && idx[3] >= nv)) {
                fp += adv; continue;
            }
            int u[4] = {0,0,0,0}, v[4] = {0,0,0,0};
            int tpx = 0, tpy = 0, clx = 0, cly = 0, bpp = 16, blend = 0;
            float sc[4][2];
            for (int k = 0; k < 4; k++) {
                sc[k][0] = vscr[idx[k]][0]; sc[k][1] = vscr[idx[k]][1];
            }
            double nA = s3_nclip(sc, 0, 1, 2);
            double nB = tri ? 0.0 : s3_nclip(sc, 3, 1, 2);
            int emit_a = 0, emit_b = 0, back = 0;
            /* Word 1 bit 1 is the flat-lit material: the face is two-sided,
             * draws untextured F3/F4 whatever its own texture words say, and
             * takes its colour from a normal rather than the colour block
             * (mobydraw.s:4720-4724,4756-4765,4849-4862). Bit 0 alone changes
             * neither (mobydraw.s:4721,4666). */
            int flat = (f1 & 2u) != 0;
            if (flat) {
                s_simple_flat++;
                textured = 0;
                /* A flat-lit face is two-sided: whichever way it faces it draws
                 * whole, and only the ABE bit changes. The draw takes the
                 * back-facing verdict from the sign of the NCLIP register it
                 * carries into the emit (mobydraw.s:4747-4748, srl/sll 31/25 at
                 * 0x80034930), which the four-way test above it sets: a triangle
                 * keeps its own NCLIP and drops on zero (mobydraw.s:4714-4718);
                 * a quad forces +1 when the second NCLIP is positive, forces -1
                 * when the first is non-negative and the second negative, drops
                 * when the first is non-negative and the second zero, and
                 * otherwise keeps the first, which is negative
                 * (mobydraw.s:4719-4746). Every surviving quad case therefore
                 * reduces to: front exactly when the second NCLIP is positive. */
                if (tri) {
                    if (nA == 0.0) { fp += adv; continue; }
                    back = (nA < 0.0);
                    emit_a = 1;
                } else {
                    if (nA >= 0.0 && nB == 0.0) { fp += adv; continue; }
                    back = !(nB > 0.0);
                    emit_a = emit_b = 1;
                }
                /* The back side survives only within the near threshold the
                 * draw tests the translation Z against (mobydraw.s:4788-4797). */
                if (back) {
                    if (TR[2] - ((f1 & 1u) ? 2048.0 : 0.0) < 0.0) s_flat_back_emitted++;
                    else { s_flat_back_dropped++; emit_a = emit_b = 0; }
                }
                /* and draws semi-transparent (mobydraw.s:4783-4784). */
                blend = back ? 1 : 0;
            } else if (!tri) {
                /* mobydraw.s:4444-4460,4529-4533: the quad's two halves keep on
                 * opposite NCLIP signs, the Z vertex order giving them opposite
                 * winding. */
                if (nA >= 0.0) { if (nB > 0.0) emit_b = 1; }
                else if (nB > 0.0) { emit_a = emit_b = 1; }
                else emit_a = 1;
            } else {
                if (nA > 0.0) emit_a = 1;      /* mobydraw.s:4592-4600 */
            }
            if (!emit_a && !emit_b) { fp += adv; continue; }

            uint32_t col[4];
            if (flat) {
                /* Bit 0 picks which of the entry's two colour pairs
                 * (mobydraw.s:4772-4776). */
                uint32_t c = s3_simple_flat_colour(R, f1,
                                flat_w[(f1 & 1u) ? 0 : 2],
                                flat_w[(f1 & 1u) ? 1 : 3], back);
                if ((c & 0xFFFFFFu) == 0u) s_flat_zero_colour++;
                col[0] = col[1] = col[2] = col[3] = c;
            } else {
                int ci[4] = { (int)((f1 >> 24) & 0x7Fu), (int)((f1 >> 17) & 0x7Fu),
                              (int)((f1 >> 10) & 0x7Fu), (int)((f1 >>  3) & 0x7Fu) };
                for (int k = 0; k < 4; k++)
                    col[k] = ctab[ci[k] < ncol ? ci[k] : 0];
            }

            if (textured) {
                uint32_t t0 = cpu->read_word(fp + 0x08u);
                uint32_t t1 = cpu->read_word(fp + 0x0Cu);
                uint32_t t2 = cpu->read_word(fp + 0x10u);
                u[0]=(int)(t0 & 0xFFu); v[0]=(int)((t0 >> 8) & 0xFFu);
                u[1]=(int)(t1 & 0xFFu); v[1]=(int)((t1 >> 8) & 0xFFu);
                u[2]=(int)(t2 & 0xFFu); v[2]=(int)((t2 >> 8) & 0xFFu);
                u[3]=(int)((t2 >> 16) & 0xFFu); v[3]=(int)((t2 >> 24) & 0xFFu);
                unsigned cl = (t0 >> 16) & 0xFFFFu;
                unsigned tp = (t1 >> 16) & 0x7FFFu;
                int mode = (int)((tp >> 7) & 3u);
                int abr  = (int)((tp >> 5) & 3u);
                tpx = (int)((tp & 0xFu) * 64u);
                tpy = (int)(((tp >> 4) & 1u) * 256u);
                bpp = (mode == 0) ? 4 : (mode == 1) ? 8 : 16;
                clx = (int)((cl & 0x3Fu) * 16u);
                cly = (int)(((cl >> 6) & 0x1FFu) + (unsigned)clut_bias);
                if ((t1 >> 31) & 1u)
                    blend = (abr == 0) ? 1 : (abr == 1) ? 2 : 3;
            }

            double p[3][3]; uint32_t rgb[3]; int uu[3], vv[3];
            if (emit_a) {
                const int o[3] = {0,1,2};
                for (int k = 0; k < 3; k++) {
                    memcpy(p[k], vpos[idx[o[k]]], sizeof p[k]);
                    rgb[k] = col[o[k]]; uu[k] = u[o[k]]; vv[k] = v[o[k]];
                }
                moby_simple_tri(p, rgb, uu, vv, textured, tpx, tpy, clx, cly, bpp, blend,
                                (uint32_t)(idx[0] | (idx[1] << 10) | (idx[2] << 20)));
                emitted++; s_simple_tris++;
            }
            if (emit_b) {
                const int o[3] = {1,3,2};
                for (int k = 0; k < 3; k++) {
                    memcpy(p[k], vpos[idx[o[k]]], sizeof p[k]);
                    rgb[k] = col[o[k]]; uu[k] = u[o[k]]; vv[k] = v[o[k]];
                }
                moby_simple_tri(p, rgb, uu, vv, textured, tpx, tpy, clx, cly, bpp, blend,
                                (uint32_t)(idx[1] | (idx[3] << 10) | (idx[2] << 20)));
                emitted++; s_simple_tris++;
            }
            fp += adv;
        }
    }
    return emitted;
}

/* A sub-tick present's replay: the captured queue decoded again under the
 * present's camera, with no second call into the game's draw. */
static int s3_simple_replay(CPUState* cpu) {
    return s3_simple_decode(cpu);
}

/* The pass the walk's second-kind hook runs: capture the cull's queue, run the
 * game's own draw (which writes the drawn verdict and the prims), then decode
 * the captured queue natively. */
static int s3_simple_pass(CPUState* cpu) {
    s_simple_recs = s_simple_verts = s_simple_faces = s_simple_tris = 0;
    s_simple_flat = s_simple_screen = s_simple_culled = 0;
    s_flat_zero_colour = s_flat_back_emitted = s_flat_back_dropped = 0;
    s3_simple_capture(cpu);
    for (int i = 0; i < 4; i++)
        s_flat_entry[i] = cpu->read_word(S3_FLAT_SCRATCH + (uint32_t)i * 4u);
    openpete_recomp_module_get()->dispatch(cpu, F_MOBY_DRAW_STATIC);
    int n = s3_simple_decode(cpu);
    static int s_dbg = -1;
    if (s_dbg < 0) { const char* e = getenv("OPENPETE_MOBY_VERIFY"); s_dbg = (e && *e && *e != '0'); }
    if (s_dbg)
        fprintf(stderr, "[moby-simple f=%u] queued=%d drawn=%ld culled=%ld screenkind=%ld "
                        "verts=%ld faces=%ld flat=%ld tris=%d "
                        "flat_zero_colour=%ld flat_back_emitted=%ld flat_back_dropped=%ld\n",
                openpete_frame, s_simple_qn, s_simple_recs, s_simple_culled, s_simple_screen,
                s_simple_verts, s_simple_faces, s_simple_flat, n,
                s_flat_zero_colour, s_flat_back_emitted, s_flat_back_dropped);
    return n;
}

const moby_game_t moby_game = {
    .vstream_init      = s3_vstream_init,
    .vstream_next      = s3_vstream_next,
    .decode_faces      = s3_decode_faces,
    .read_record       = s3_read_record,
    .frame_patch_list  = s3_frame_patch_list,
    .model_is_animated = s3_model_is_animated,
    .simple_pass       = s3_simple_pass,
    .simple_replay     = s3_simple_replay,
    .special_face      = s3_special_face,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so
     * nothing the host widens may reach it and the walk feeds the records the
     * cull queued, no more. */
    .host_reveal       = 0,
    /* The moby array is fixed with a live sentinel, so a record's Moby address
     * is its slot and is stable across ticks: the walk's default identity. */
    .interp_key        = NULL,
};
