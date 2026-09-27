/* games/common/renderer/moby_game_spyro23.c — the per-game half of the shared
 * moby feeder for both games whose moby draw is the same three handwritten
 * routines: the vertex-stream and face-stream decoders, the cull-record
 * reader, the model-kind test, the keyframe patch list and the second model
 * kind's own pass.
 *
 * The walk itself is games/common/renderer/moby.c; the contract is
 * renderer/moby_game.h. Every claim below carries the asm/mobydraw.s line it
 * was read from: the draw is three handwritten routines with no decompiled C.
 * The two games run them instruction for instruction apart from the special
 * material's mode 0, so the line numbers are one disassembly's and every
 * address, plus the one behavioural switch, comes from the per-game
 * moby_game_s23 block (renderer/moby_game_spyro23.h).
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
#include "renderer/moby_game_spyro23.h"   /* the per-game constants and the shared decoders */
#include "renderer/moby.h"            /* the second kind's shared emit */
#include "renderer/native_gte.h"       /* ngte_read_matrix, ngte_mac, ngte_mat_mul3, ngte_dpcs_ch */
#include "game_layout.h"
#include "no_lod.h"                    /* openpete_no_lod — the extended-LOD clause */
#include "openpete_recomp_module.h"    /* the game's own static draw */
#include "renderer/interp_regions_gen.h"  /* game_interp_moby_stash_begin/_put */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one-byte vertex form indexes a 32-bit adjustment table
 * (mobydraw.s:914), moby_game_s23.vertex_adjust_table. The draw XORs the
 * table pointer's sign bit with record word-0 bit 0 (mobydraw.s:913-915),
 * which moves it between KSEG0 and KUSEG: the same physical words either way,
 * and the sign only selects whether the draw also stores per-vertex clip
 * flags (mobydraw.s:1094). The decode reads the words, so it reads one
 * address. */

/* AnimationHeader fields the stream decode needs and game_layout.moby does not
 * name, because only this codec has them: the frame-stream data base the two
 * halfword offsets in a keyframe word are relative to (mobydraw.s:967) and the
 * per-model static vertex array every form accumulates onto (mobydraw.s:975).
 */
#define ANIM_DATA_BASE   0x20u
#define ANIM_STATIC_VERT 0x0Cu

/** @brief Opens one keyframe's vertex stream: the block, adjust and static-vertex cursors and the first item's form.
 *
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
 * @param cpu The guest CPU state, read only.
 * @param s Receives the stream state, zeroed first.
 * @param header The AnimationHeader; +0x20 is the data base, +0x0C the static vertex array.
 * @param frame The keyframe word: bits 0-15 the block offset in halfwords,
 *        bits 16-24 the adjust-stream offset from the block in halfwords, bit 25
 *        the first item's form.
 * @param s5 The per-model shift, stored unused: this codec has none.
 */
static void s23_vstream_init(CPUState* cpu, moby_vstream_t* s,
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

void s23_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved) {
    (void)first;   /* the keyframe word already said which form opens */
    uint32_t sw = cpu->read_word(s->scratch_u[0]);
    s->scratch_u[0] += 4u;
    if (s->flag == 0) {
        uint32_t b = cpu->read_byte(s->litp); s->litp += 1u;
        uint32_t tw = cpu->read_word(moby_game_s23.vertex_adjust_table + (((b << 1) & 0x1FCu)));
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

void s23_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    s23_vstream_next_lit(cpu, s, first, 0);
}

/* The face-stream decode; the record layout is documented at the declaration. */
int s23_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
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
                ngte_tpage t;
                ngte_decode_tpage(tp, cl, 0u, &t);
                F->abr = (uint8_t)t.abr;
                F->tpx = (int16_t)t.tpx;
                F->tpy = (int16_t)t.tpy;
                F->bpp = (uint8_t)t.bpp;
                F->clx = (int16_t)t.clx;
                F->cly = (int16_t)t.cly;
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = 0; st->skips = 0;
    st->align = (fp == fb + 4u + bytecount) ? 0 : (fp == end) ? 1 : 2;
    /* OPENPETE_MOBY_FACEBITS=1: per face stream, how many triangles carry each
     * of the two flag bits that leave the ordinary emit, so a material or
     * colour divergence can be tested against the bit that would explain it. */
    {
        static int s_fb = -1;
        if (s_fb < 0) { const char* e = getenv("OPENPETE_MOBY_FACEBITS"); s_fb = (e && *e && *e != '0'); }
        if (s_fb) {
            int tris = 0, b4 = 0, b5 = 0;
            for (int i = 0; i < n; i++) {
                if (out[i].i2 != out[i].i3) continue;
                tris++;
                if (out[i].hl == 2u) b4++; else if (out[i].hl == 1u) b5++;
            }
            fprintf(stderr, "  [facebits] fb=%08X faces=%d tris=%d bit4=%d bit5=%d\n",
                    fb, n, tris, b4, b5);
        }
    }
    return n;
}

/* The moby draw's own stride: two words untextured, five textured. */
int s23_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                    moby_wface_t* out, int cap, moby_face_stats_t* st) {
    return s23_decode_faces_stride(cpu, fb, bytecount, out, cap, st, 0u, 0);
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
 *     from its own colour. IR0 zero leaves the colours alone. This mode also
 *     substitutes a texture: its setup loads the global word at
 *     `game_layout.moby.special_tiledef`, biased by the record's CLUT row, into
 *     GTE control 16 (mobydraw.s:2085-2087,2098-2099), and the emit writes that
 *     register's halves over the primitive's texture page and CLUT
 *     (mobydraw.s:2296-2299), keeping the face's own UVs. The substitution is
 *     unconditional, so a zero IR0 leaves a face drawn from the substitute page
 *     with the model's own colours.
 *   mode 2 (mobydraw.s:2551-2564) — the specular sheen. The channels are the
 *     GTE background colour, bits 24-27 doubled is IR0 (mobydraw.s:2024-2045),
 *     and the light matrix is three copies of one constant vector
 *     (mobydraw.s:2048-2057). The face normal rotates into view by the model
 *     matrix, scales by IR0 and projects onto that vector on top of the
 *     background colour, giving one colour for the whole face.
 *
 * All three replace the face's vertex colours; mode 1 replaces the texture page
 * and CLUT as well, and none of them touches the face's UVs or its geometry.
 * The special emit's command word starts at
 * the untextured code (mobydraw.s:2562) and the shared emit the three treatments
 * fall into adds the texture bit back from the face record, so a treatment that
 * forced its face untextured would drop the texture the draw keeps.
 *
 * CARVE-OUT: flag bit 4 takes a different dispatcher (mobydraw.s:2113) that
 * this decode does not carry; the walk leaves those faces on the ordinary
 * path.
 */

/** @brief Saturate a MAC value to a signed 16-bit IR register, as the GTE does with lm = 0.
 *
 * @param v The MAC value.
 * @return v clamped to [-32768, 32767]. */
static int s23_clamp_ir(long v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int)v;
}

/* ---- mode-1 texture coordinates (mobydraw.s:2377-2547) -----------------
 *
 * Mode 1 does not keep the face's own texture coordinates: the emit calls a
 * per-vertex routine three times, once per corner, and stores the halfword it
 * returns into the primitive's three coordinate slots (mobydraw.s:2326,
 * 2332-2336, 2338). The routine builds a spherical environment coordinate.
 *
 * Its input is the vector the vertex stage stored for that vertex: the three
 * depth-cued colour channels the interpolation leaves in MAC1-3
 * (mobydraw.s:1295-1301, stored at +0x2, +0x400 and +0x402 of the vertex
 * slot), read back permuted into IR1-3 as (MAC2, MAC3, MAC1)
 * (mobydraw.s:2378-2384). That vector rotates by the Moby's own matrix at
 * +0x20, which the mode-1 setup loads into the GTE's rotation registers
 * (mobydraw.s:2080-2095).
 *
 * From the rotated vector (x, y, z) the routine takes the azimuth from the
 * ratio of the minor to the major of |z| and |x| through an arctangent table,
 * placed in its octant by the two signs (mobydraw.s:2404-2418, 2484-2496,
 * 2497-2502); the elevation from -y divided by the vector's length, the length
 * itself coming from a square-root table indexed by the squared length
 * normalized by its leading-zero count (mobydraw.s:2423-2452); and turns the
 * pair into a disc coordinate by looking the elevation up as a radius and
 * multiplying it by the sine and cosine of the azimuth, offset to the page's
 * centre texel 0x40 (mobydraw.s:2518-2544).
 *
 * The routine memoizes its result in the vertex slot and marks the slot done,
 * so a vertex shared by several faces is transformed once; the walk computes
 * per face corner instead, which gives the same coordinate for the same
 * vertex.
 *
 * UNRESOLVED: the azimuth carries a per-moby bias the setup leaves in GTE
 * control 17 (mobydraw.s:2100-2112), built from the record's view-space depth
 * shifted by a per-model byte pair and a global word. The walk holds it at 0,
 * which leaves the map unrotated.
 */

/** @brief The GTE's LZCS/LZCR count: leading zeroes of a positive value, leading ones of a negative one.
 *
 * @param v The value, read as signed.
 * @return The count, 1..32. */
static int s23_lzcr(uint32_t v) {
    if (v & 0x80000000u) v = ~v;
    int n = 0;
    while (n < 32 && !(v & 0x80000000u)) { n++; v <<= 1; }
    return n;
}

/* The Moby's own rotation matrix and the azimuth bias, resolved once per
 * record rather than per face. */
typedef struct { int bias; } s23_env_setup;

static void s23_env_setup_read(CPUState* cpu, uint32_t rec, s23_env_setup* E) {
    /* The azimuth carries a per-record bias the setup leaves in GTE control 17
     * (mobydraw.s:2100-2112): the record's view-space depth, which its own
     * setup left in the translation register, shifted left by the difference of
     * two bytes of the record's word at +0x18, shifted right by 6, plus an
     * eighth of the global word at `env_bias_word`. Reading it back that way
     * rotates the coordinates further from the draw's than leaving it out, so
     * one of the three terms is not what it reads as and the bias stays 0 until
     * it is resolved; the measured gap is a constant rotation, which is what a
     * bias is. */
    (void)cpu; (void)rec;
    E->bias = 0;
}

/** @brief One face corner's mode-1 environment coordinate pair, from that corner's rotated position.
 *
 * The per-vertex routine of mobydraw.s:2377-2547: the azimuth from the
 * arctangent table in the octant the signs of z and -x place it, the elevation
 * from -y over the vector's length through the radius table, and the disc
 * coordinate from that radius times the azimuth's cosine and sine, centred on
 * texel 0x40.
 * @param cpu The guest CPU state, read only (the square-root, arctangent, radius and sine tables).
 * @param E The per-record setup; its azimuth bias is added before the sine lookup.
 * @param e The corner's model-to-view rotated position.
 * @param pu Receives U.
 * @param pv Receives V. */
static void s23_env_uv(CPUState* cpu, const s23_env_setup* E, const float e[3],
                      uint8_t* pu, uint8_t* pv) {
    /* The walk already holds the rotated vector the draw's MVMVA produces: the
     * model-to-view rotation applied to the vertex, shifted by 12. */
    long mac[3], ir[3];
    for (int i = 0; i < 3; i++) {
        mac[i] = (long)(e[i] < 0.0f ? -(long)(-e[i] + 0.5f) : (long)(e[i] + 0.5f));
        ir[i] = s23_clamp_ir(mac[i]);
    }
    long z = mac[2], nx = -mac[0], ny = -mac[1];    /* mobydraw.s:2392-2396 */

    /* The azimuth ratio: the minor of |z| and |x| over the major, in 6
     * fractional bits (mobydraw.s:2404-2419). */
    long a = z < 0 ? -z : z, b = nx < 0 ? -nx : nx;
    if (a > b) { long t = a; a = b; b = t; }
    if (b == 0) b = 1;
    long q = (a << 6) / b;
    if (q > 64) q = 64;

    /* SQR 0 then the square root of the sum (mobydraw.s:2397, 2423-2452). */
    unsigned long len2 = 0;
    for (int i = 0; i < 3; i++) len2 += (unsigned long)(ir[i] * ir[i]);
    long len = 0;
    if (len2) {
        int lz = s23_lzcr((uint32_t)len2) & ~1;
        int sh = (0x1F - lz) >> 1;
        int e2 = lz - 0x18;
        uint32_t t = e2 >= 0 ? ((uint32_t)len2 << e2) : ((uint32_t)len2 >> (-e2));
        int idx = ((int)t - 0x40) * 2;
        int w = (int16_t)cpu->read_half(game_layout.moby.env_sqrt_table + (uint32_t)idx);
        len = ((long)w << sh) >> 12;
    }

    /* The octant the two signs place the ratio in (mobydraw.s:2484-2502). */
    int sub, base;
    if (z >= 0) {
        if (nx >= 0) { if (z - nx < 0) { sub = 1; base = 0x40; } else { sub = 0; base = 0x00; } }
        else         { long m = -nx; if (z - m < 0) { sub = 1; base = 0x100; } else { sub = 0; base = 0xC0; } }
    } else {
        long zm = -z;
        if (nx >= 0) { if (zm - nx < 0) { sub = 1; base = 0x80; } else { sub = 0; base = 0x40; } }
        else         { long m = -nx; if (zm - m < 0) { sub = 1; base = 0xC0; } else { sub = 0; base = 0x80; } }
    }
    int at = (int)cpu->read_byte(game_layout.moby.env_atan_table + (uint32_t)q);
    int ang = sub ? (base - at) : (base + at);

    /* The elevation, clamped to the radius table's range (mobydraw.s:2508-2521). */
    long evq = len ? ((ny << 12) / len) : 0;
    long el = evq >> 5;
    if (el < 0) el = 0; else if (el > 0x80) el = 0x80;
    int rad = (int)cpu->read_byte(game_layout.moby.env_radius_table + (uint32_t)el) >> 1;

    /* The disc coordinate, centred on texel 0x40 (mobydraw.s:2522-2544). */
    int az = (ang + E->bias) & 0xFF;
    uint32_t sp = game_layout.sin_table + (uint32_t)(az << 1);
    int cs = (int16_t)cpu->read_half(sp + 0x80u);
    int sn = (int16_t)cpu->read_half(sp);
    *pu = (uint8_t)(((((long)rad * cs) >> 12) + 0x40) & 0xFF);
    *pv = (uint8_t)(((((long)rad * sn) >> 12) + 0x40) & 0xFF);
}

/** @brief Which of the two forms the special material's mode 0 takes.
 *
 * OPENPETE_MOBY_MODE0_IR0_SUM=0|1, read once, overrides
 * moby_game_s23.mode0_ir0_from_colour_sum for one run, so the two coded forms
 * can be priced against the guest's own primitive colours on the same pose.
 * Unset or empty, the game's own switch decides.
 * @return 1 for IR0 from the vertex colour sum, 0 for the per-moby IR0. */
static int s23_mode0_ir0_sum(void) {
    static int s = -2;
    if (s == -2) { const char* e = getenv("OPENPETE_MOBY_MODE0_IR0_SUM");
                   s = (e && *e) ? (*e != '0') : -1; }
    return s >= 0 ? s : moby_game_s23.mode0_ir0_from_colour_sum;
}

static int s23_special_face(CPUState* cpu, uint32_t mat, const int M[3][3],
                           uint32_t moby, uint32_t rec, const moby_wface_t* F,
                           uint32_t c[4], const float ev[4][3], int* textured,
                           moby_face_tex_t* tex) {
    (void)textured;   /* every treatment keeps the face's own textured flag */
    if (F->hl != 1) return 0;                      /* bit 4: the other dispatcher */
    int mode = mat ? (int)((mat >> 28) & 7u) : 0;
    if (mode > 2) return 0;                        /* the treatment is disarmed */

    /* The three 12-bit channels the material word carries. */
    int mc[3] = { (int)((mat <<  4) & 0xFF0u),
                  (int)((mat >>  4) & 0xFF0u),
                  (int)((mat >> 12) & 0xFF0u) };

    if (mode == 0) {
        /* The two draws part here, and only here. One zeroes the source colour
         * once for the whole block (mtc2 $zero, RGBC) and sets IR0 per vertex
         * from the sum of that vertex's three colour bytes shifted left 3, so
         * each channel interpolates from 0 toward the far colour by the
         * vertex's own brightness. The other loads the colour word itself into
         * RGBC per vertex and leaves IR0 at the per-moby value the material
         * setup stored, which is the treatment mode 1 gives its colour half.
         * The per-moby IR0 is the material word's bits 24-27 shifted into
         * 0xF00, the same field both setups write to GTE data 8. */
        const int per_moby_ir0 = (int)((mat >> 16) & 0xF00u);
        for (int k = 0; k < 4; k++) {
            int src[3], ir0;
            if (s23_mode0_ir0_sum()) {
                int sum = (int)(c[k] & 0xFFu) + (int)((c[k] >> 8) & 0xFFu)
                        + (int)((c[k] >> 16) & 0xFFu);
                ir0 = sum << 3;
                src[0] = src[1] = src[2] = 0;
            } else {
                ir0 = per_moby_ir0;
                for (int ch = 0; ch < 3; ch++) src[ch] = (int)((c[k] >> (ch * 8)) & 0xFFu);
            }
            uint32_t o = 0u;
            for (int ch = 0; ch < 3; ch++)
                o |= (uint32_t)ngte_dpcs_ch(src[ch], mc[ch], ir0) << (ch * 8);
            c[k] = o;
        }
        return 1;
    }

    if (mode == 1) {
        /* The substitute page. The per-moby setup loads one global word into
         * GTE control 16 whenever the mode is non-zero (mobydraw.s:2085-2087,
         * 2098-2099), and the per-face emit writes that register's low half
         * over the primitive's texture page and its high half over the CLUT
         * (mobydraw.s:2296-2299), so the face keeps its own UVs and draws from
         * the substitute page. The emit builds its command word from the
         * textured code alone (mobydraw.s:2293-2294): it adds no
         * semi-transparency bit, whatever the face record's own texture word
         * carries, so the face draws opaque. */
        uint32_t g = game_layout.moby.special_tiledef
                   ? cpu->read_word(game_layout.moby.special_tiledef) : 0u;
        if (g) {
            unsigned tp = g & 0xFFFFu, cl = (g >> 16) & 0xFFFFu;
            ngte_tpage t;
            ngte_decode_tpage(tp, cl, 0u, &t);
            tex->tpx = (int16_t)t.tpx;
            tex->tpy = (int16_t)t.tpy;
            tex->bpp = (uint8_t)t.bpp;
            tex->abr = (uint8_t)t.abr;
            tex->clx = (int16_t)t.clx;
            tex->cly = (int16_t)t.cly;
            tex->set = 1u;
        }
        /* The coordinates the emit generates per corner in place of the face's
         * own, from that corner's depth-cued colour before the treatment
         * rewrites it. */
        {
            s23_env_setup E; s23_env_setup_read(cpu, rec, &E);
            for (int k = 0; k < 4; k++)
                s23_env_uv(cpu, &E, ev[k], &tex->u[k], &tex->v[k]);
            tex->uv_set = 1u;
        }
        /* The colour half runs off one IR0 for the whole moby, bits 24-27 << 8
         * (mobydraw.s:2079), carried in GTE control 12. The emit reads that
         * register first and skips every DPCS when it is zero
         * (mobydraw.s:2284), so a zero IR0 leaves the vertex colours as the
         * model stored them and the substitute page is the whole treatment. */
        int ir0 = (int)((mat >> 16) & 0xF00u);
        if (ir0) {
            for (int k = 0; k < 4; k++) {
                uint32_t o = 0u;
                for (int ch = 0; ch < 3; ch++)
                    o |= (uint32_t)ngte_dpcs_ch((int)((c[k] >> (ch * 8)) & 0xFFu),
                                                mc[ch], ir0) << (ch * 8);
                c[k] = o;
            }
        }
        return (tex->set || tex->uv_set || ir0) ? 2 : 0;
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
        ir[i] = s23_clamp_ir(m >> 12);
    }
    for (int i = 0; i < 3; i++) ir[i] = s23_clamp_ir((long)ir0 * ir[i]);
    long dot = (long)moby_game_s23.spec_light[0] * ir[0]
             + (long)moby_game_s23.spec_light[1] * ir[1]
             + (long)moby_game_s23.spec_light[2] * ir[2];
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

/** @brief Reads one animated cull record into the walk's record form.
 *
 * Stride 0x44 in the tick's draw scratch, terminated by a zero flags word
 * (mobydraw.s:888). The flags word carries the keyframe blend byte and, in its
 * top half, the LOD verdict whose sign picks the low-poly vertex count and
 * face list (mobydraw.s:918-921). The two keyframes are the stream words
 * themselves with their own headers at +0x04 and +0x08 (mobydraw.s:888-889,
 * 911,988-989).
 * @param cpu The guest CPU state, read only.
 * @param rec The record's address.
 * @param R Receives the record; its tier is the flags word's signed top half.
 * @return 0 at the zero flags word that terminates the list, 1 otherwise. */
static int s23_read_record(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    const uint32_t w0 = moby_rec_word(cpu, rec);
    if (w0 == 0) return 0;
    R->w0       = w0;
    R->header   = moby_rec_word(cpu, rec + game_layout.moby.rec_header_off);
    R->header_b = moby_rec_word(cpu, rec + game_layout.moby.rec_header_b_off);
    R->frame_a  = moby_rec_word(cpu, rec + game_layout.moby.rec_frame_a_off);
    R->frame_b  = moby_rec_word(cpu, rec + game_layout.moby.rec_frame_b_off);
    R->moby     = moby_rec_word(cpu, rec + game_layout.moby.rec_moby_off);
    R->material = moby_rec_word(cpu, rec + game_layout.moby.rec_material_off);
    for (int i = 0; i < 3; i++)
        R->TR_raw[i] = (int32_t)moby_rec_word(cpu, rec + game_layout.moby.rec_tr_off + (uint32_t)i * 4u);
    moby_rec_matrix(cpu, rec + game_layout.moby.rec_matrix_off, R->M);
    R->tier = (int)(int16_t)(w0 >> 16);
    return 1;
}

/** @brief Whether a model-table entry is an animated model.
 *
 * A KSEG0 pointer is an animated model; a KUSEG one is the second kind, drawn
 * by its own pass (mobydraw.s:116).
 * @param cpu Unused.
 * @param model The model-table entry.
 * @return 1 for an animated model, 0 for the second kind. */
static int s23_model_is_animated(CPUState* cpu, uint32_t model) {
    (void)cpu;
    return (int32_t)model < 0;
}

/** @brief The keyframe's faceblock patch list, or 0 when the keyframe patches nothing.
 *
 * The cull resolves the list through the Model's own table and stores the
 * pointer at record +0x14, and marks a keyframe that patches nothing by
 * setting the stored keyframe word's top bits (mobydraw.s:532-534,561-568); the
 * draw skips the apply on exactly that sign (mobydraw.s:935). A keyframe the
 * record does not name is resolved through the model's table the way the cull
 * resolves it.
 * @param cpu The guest CPU state, read only.
 * @param rec The cull record, whose +0x14 holds the list the cull resolved.
 * @param model The moby's model for a keyframe the record does not name, else 0.
 * @param frame_a The keyframe word; a negative one patches nothing.
 * @return The list's address, or 0. */
static uint32_t s23_frame_patch_list(CPUState* cpu, uint32_t rec, uint32_t model,
                                     uint32_t frame_a) {
    if ((int32_t)frame_a < 0) return 0u;
    if (model >= 0x80000000u) {
        /* A keyframe no record names. Resolve the list the cull resolves:
         * Model+0x34 is the table base, the keyframe word's top byte masked to
         * 0x7C indexes it, and the word read there is the list's byte offset
         * from that same base (mobydraw.s:563-569). */
        uint32_t base = cpu->read_word(model + 0x34u);
        if (base < 0x80000000u) return 0u;
        return base + cpu->read_word(base + ((frame_a >> 24) & 0x7Cu));
    }
    return moby_rec_word(cpu, rec + 0x14u);
}

/** @brief The record's blend byte for a Moby+0x40 animation-state index, looked up in the game's blend table.
 *
 * Moby+0x40 is an animation-state index, not the keyframe blend numerator:
 * the cull reads the byte, indexes this table with it and stores the table
 * entry as the record's blend byte, taking the no-blend path (frame A alone,
 * no frame B header or keyframe) when the entry is zero
 * (mobydraw.s:454,458-461,466,487). The draw then scales that byte by 16 into
 * the GTE's IR0 (mobydraw.s:922-923).
 * @param cpu The guest CPU state, read only.
 * @param state4 The animation-state byte; bits above the low 8 are ignored.
 * @return The blend byte. */
static uint32_t s23_anim_blend_byte(CPUState* cpu, uint32_t state4) {
    return cpu->read_byte(moby_game_s23.anim_blend_table + (state4 & 0xFFu)) & 0xFFu;
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
 * camera transform and takes the record's own halves as the GTE screen offsets
 * (mobydraw.s:3915,4010-4022), which s23_simple_place mirrors. A second builder
 * writes that kind's record, out of the moby's own three position words rather
 * than any camera-relative quantity (mobydraw.s:604-628), so its halves are the
 * low 16 bits of Moby+0x0C, +0x10 and +0x14 and its far half is 0xFFFF.
 *
 * The draw transforms the record by the camera matrix at the descriptor's cam.base+0x00 with
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

#define S23_SIMPLE_RECS   63
/* The 63 above is the cursor limit the cull keeps in GTE control 8 and 9
 * (mobydraw.s:44,46), so it bounds what the capture can read. The host queue
 * is sized for the reveal instead, which has no such cursor: a camera high
 * enough over a level admits most of its moby table, and a queue that fills
 * drops the tail of the level silently. */
#define S23_SIMPLE_QCAP   1024
#define S23_SIMPLE_VERTS  256
#define S23_SIMPLE_COLS   256
#define S23_SIMPLE_FACES  4096

/* The cull's own queue, copied at the hook. The queue lives in the tick's
 * draw scratch, which the passes after this one build their own data in, so
 * the bytes are taken while they are still the cull's. */
static uint32_t s_simple_q[S23_SIMPLE_QCAP][4];
static int      s_simple_qn;
/* The same record's camera deltas and far-cull distance at full width. The
 * cull packs the three deltas as int16 halves and the distance as a halfword,
 * which cannot express a centre or a range past ~131000 world units; the
 * capture fills these by sign-extending its own halves, so a canonical record
 * decodes exactly as it did, while a host-built reveal record carries the
 * numbers untruncated and a far distance of -1 for "no range limit". */
static int32_t  s_simple_d[S23_SIMPLE_QCAP][3];
static int32_t  s_simple_far[S23_SIMPLE_QCAP];

/* Per-frame engagement counts: a pass that walked nothing proves nothing, so
 * the verify harness prints what this one actually read. */
static long s_simple_recs, s_simple_verts, s_simple_faces, s_simple_tris;
static long s_simple_flat, s_simple_screen, s_simple_culled;
/* Screen-space records queued, and the subset of them the placement admitted:
 * the kind's own engagement count, since every other counter mixes the kinds. */
static long s_simple_screen_drawn;
/* Records decoded on the low-poly lists (tier <= 0), and records whose colour
 * block took the depth cue (IR0 > 0). */
static long s_simple_lp, s_simple_fade;
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
#define S23_FLAT_SCRATCH 0x1F8003F0u
static uint32_t s_flat_entry[4];

static void s23_simple_capture(CPUState* cpu) {
    s_simple_qn = 0;
    /* The cull left the draw scratch base in GTE data 2 (mobydraw.s:35,3893)
     * and parked this queue 0x1C00 into it (mobydraw.s:44,3895). */
    uint32_t q = cpu->gte_data[2] + 0x1C00u;
    for (int i = 0; i < S23_SIMPLE_RECS; i++, q += 0x10u) {
        uint32_t m = cpu->read_word(q);
        if (m == 0u) break;                     /* the cull's terminator (mobydraw.s:584) */
        s_simple_q[i][0] = m;
        s_simple_q[i][1] = cpu->read_word(q + 4u);
        s_simple_q[i][2] = cpu->read_word(q + 8u);
        s_simple_q[i][3] = cpu->read_word(q + 12u);
        s_simple_d[i][0] = (int16_t)(s_simple_q[i][1] & 0xFFFFu);
        s_simple_d[i][1] = (int16_t)(s_simple_q[i][2] >> 16);
        s_simple_d[i][2] = (int16_t)(s_simple_q[i][1] >> 16);
        s_simple_far[i]  = (int32_t)(s_simple_q[i][2] & 0xFFFFu);
        s_simple_qn = i + 1;
    }
}

/** @brief The flat-lit material's face colour, from the normal packed in face word 1 (mobydraw.s:4758-4813).
 *
 * Word 1's bits 7-30 are a
 * face normal instead of four colour indices; the draw rotates it into view by
 * the model matrix, projects it onto the light vector it loads into the GTE
 * colour matrix at entry (all three rows of that matrix are the same vector,
 * mobydraw.s:3874-3880, so the result is one dot product), and blends the two
 * colours of the moby's entry in the flat-colour table by it: the second word
 * scaled by the projection, the first added flat, plus a highlight that opens
 * above 1472. The back side halves the flat word and quarters the projection
 * (mobydraw.s:4790-4795).
 * @param R The model-to-view rotation.
 * @param w1 Face word 1.
 * @param cbase The table word added flat.
 * @param clit The table word scaled by the projection.
 * @param backface 1 for the back side.
 * @return The colour, one byte per channel in bits 0-23. */
static uint32_t s23_simple_flat_colour(const int R[3][3], uint32_t w1,
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

static double s23_nclip(const float s[3][2], int a, int b, int c) {
    return (double)(s[b][0]-s[a][0]) * (double)(s[c][1]-s[a][1])
         - (double)(s[c][0]-s[a][0]) * (double)(s[b][1]-s[a][1]);
}

/* What one face contributes: which of the two triangle halves survive the
 * winding tests, whether the face draws textured, its blend mode, and the
 * colour each of its four vertex slots takes. */
typedef struct {
    int      emit_a, emit_b;   /* the halves (v0,v1,v2) and (v1,v3,v2) */
    int      textured;
    int      blend;
    uint32_t col[4];
} s23_shade;

/** @brief The flat-lit material's face treatment: the winding verdict, the back side's blend and one colour.
 *
 * Two-sided, untextured F3/F4 whatever
 * the face's own texture words say, and one colour off a face normal instead of
 * four out of the colour block (mobydraw.s:4720-4724,4756-4765,4849-4862).
 *
 * It is two-sided in the sense that whichever way it faces it draws whole, and
 * only the ABE bit changes. The draw takes the back-facing verdict from the sign
 * of the NCLIP register it carries into the emit (mobydraw.s:4747-4748, srl/sll
 * 31/25 at 0x80034930), which the four-way test above it sets: a triangle keeps
 * its own NCLIP and drops on zero (mobydraw.s:4714-4718); a quad forces +1 when
 * the second NCLIP is positive, forces -1 when the first is non-negative and the
 * second negative, drops when the first is non-negative and the second zero, and
 * otherwise keeps the first, which is negative (mobydraw.s:4719-4746). Every
 * surviving quad case therefore reduces to: front exactly when the second NCLIP
 * is positive.
 * @param R The model-to-view rotation the normal rotates by.
 * @param f1 Face word 1.
 * @param tri 1 for a triangle.
 * @param nA The NCLIP of (v0,v1,v2).
 * @param nB The NCLIP of (v3,v1,v2), on a quad.
 * @param trz The record's translation depth the back side is tested against.
 * @param flat_w The four material words in force.
 * @param sh Receives the halves, the blend flag and the colour; `textured` is cleared.
 * @return 0 when the face emits nothing, 1 otherwise. */
static int s23_simple_flat_face(const int R[3][3], uint32_t f1, int tri,
                                double nA, double nB, double trz,
                                const uint32_t flat_w[4], s23_shade* sh) {
    s_simple_flat++;
    sh->textured = 0;
    int back;
    if (tri) {
        if (nA == 0.0) return 0;
        back = (nA < 0.0);
        sh->emit_a = 1;
    } else {
        if (nA >= 0.0 && nB == 0.0) return 0;
        back = !(nB > 0.0);
        sh->emit_a = sh->emit_b = 1;
    }
    /* The back side survives only within the near threshold the draw tests the
     * translation Z against (mobydraw.s:4788-4797), and draws semi-transparent
     * (mobydraw.s:4783-4784). */
    if (back) {
        if (trz - ((f1 & 1u) ? 2048.0 : 0.0) >= 0.0) { s_flat_back_dropped++; return 0; }
        s_flat_back_emitted++;
        sh->blend = 1;
    }
    /* Bit 0 picks which of the entry's two colour pairs (mobydraw.s:4772-4776). */
    uint32_t c = s23_simple_flat_colour(R, f1, flat_w[(f1 & 1u) ? 0 : 2],
                                        flat_w[(f1 & 1u) ? 1 : 3], back);
    if ((c & 0xFFFFFFu) == 0u) s_flat_zero_colour++;
    sh->col[0] = sh->col[1] = sh->col[2] = sh->col[3] = c;
    return 1;
}

/** @brief One second-kind face's material, winding verdict and colours, the step between the vertex walk and the emit.
 *
 * Word 1 bit 1 selects the flat-lit material (s23_simple_flat_face); bit 0
 * alone selects neither (mobydraw.s:4721,4666). A standard quad keeps each
 * half on its own NCLIP sign test and a standard triangle keeps on a positive
 * NCLIP; its four colours are word 1's 7-bit indices into `ctab`, an index
 * past `ncol` taking entry 0.
 * @param R The model-to-view rotation the flat material's normal rotates by.
 * @param f1 Face word 1; its sign is the textured flag.
 * @param tri 1 for a triangle.
 * @param nA The NCLIP of (v0,v1,v2).
 * @param nB The NCLIP of (v3,v1,v2), on a quad.
 * @param trz The record's translation depth the back side is tested against.
 * @param flat_w The four material words in force.
 * @param ctab The record's decoded colours.
 * @param ncol The number of entries in `ctab`.
 * @param sh Receives the halves, the textured and blend flags and the colours.
 * @return 0 when the face emits nothing, 1 otherwise. */
static int s23_simple_shade_face(const int R[3][3], uint32_t f1, int tri,
                                 double nA, double nB, double trz,
                                 const uint32_t flat_w[4],
                                 const uint32_t* ctab, int ncol,
                                 s23_shade* sh) {
    sh->emit_a = sh->emit_b = 0;
    sh->textured = ((int32_t)f1) < 0;
    sh->blend = 0;
    if (f1 & 2u) return s23_simple_flat_face(R, f1, tri, nA, nB, trz, flat_w, sh);

    if (!tri) {
        /* mobydraw.s:4444-4460,4529-4533: the quad's two halves keep on opposite
         * NCLIP signs, the Z vertex order giving them opposite winding. */
        if (nA >= 0.0) { if (nB > 0.0) sh->emit_b = 1; }
        else if (nB > 0.0) { sh->emit_a = sh->emit_b = 1; }
        else sh->emit_a = 1;
    } else if (nA > 0.0) {
        sh->emit_a = 1;                          /* mobydraw.s:4592-4600 */
    }
    if (!sh->emit_a && !sh->emit_b) return 0;
    int ci[4] = { (int)((f1 >> 24) & 0x7Fu), (int)((f1 >> 17) & 0x7Fu),
                  (int)((f1 >> 10) & 0x7Fu), (int)((f1 >>  3) & 0x7Fu) };
    for (int k = 0; k < 4; k++)
        sh->col[k] = ctab[ci[k] < ncol ? ci[k] : 0];
    return 1;
}

/* Where one queue record puts its model: the translation the vertex walk adds,
 * the matrix the moby's own rotations compose onto, the geometry header, and —
 * for the screen-space kind — the projection origin the record names. The two
 * kinds part at one branch on the record (mobydraw.s:3915) and rejoin at the
 * shared vertex and face walk (mobydraw.s:4036), so everything between them is
 * here and nothing else in the decode differs. Returns 0 when the record draws
 * nothing. */
typedef struct {
    double   TR[3];     /* the GTE translation, world/2 (mobydraw.s:4039-4041) */
    int      R[3][3];   /* the matrix installed before the per-moby rotations */
    uint32_t header;    /* the 16-byte geometry header at slot+0x14 */
    int      screen;    /* the screen-space kind */
    double   ofx, ofy;  /* its projection origin, PSX px */
} s23_place;

static int s23_simple_place(CPUState* cpu, int r, const int Rcam[3][3],
                            const double dcam[3], const double dmob[3],
                            s23_place* pl) {
    const uint32_t slot = s_simple_q[r][3];
    /* The far-cull distance's bit 15 sorts the record (mobydraw.s:3914-3915). */
    pl->screen = (s_simple_q[r][2] & 0x8000u) != 0;
    pl->ofx = pl->ofy = 0.0;
    memcpy(pl->R, Rcam, sizeof pl->R);
    double m1 = 0.0, m2 = 0.0, m3 = 0.0;

    if (pl->screen) {
        /* The screen-space kind (mobydraw.s:4010-4022). The record's depth is
         * the high half of +0x08 and a non-positive one drops the record
         * (mobydraw.s:4011). The two halves of +0x04 go into the GTE screen
         * offsets, high into OFX and low into OFY, each scaled by 65536 into
         * that register's 16.16 pixels (mobydraw.s:4016-4019). The view X and
         * Y translation are zero (mobydraw.s:4020-4021) and the Z translation
         * is the depth over four (mobydraw.s:4022), all three doubled at the
         * rejoin (mobydraw.s:4039-4041). No camera rotation, no range test and
         * no bounding-box test: the matrix is the constant the branch installs,
         * the identity with its Y row at 0xA00 (mobydraw.s:4024-4028,
         * 4031-4035). */
        /* The cull builds this record's three halves straight out of the moby's
         * own position words at +0x0C, +0x10 and +0x14 (mobydraw.s:606-608 the
         * loads, :621-626 the halves of +0x04, :627-628 the depth half), so the
         * moby's own displacement enters each of them directly: X into OFX, Y
         * into OFY and Z into the depth, in the same units and with no sign
         * change. */
        const double dz = (double)s_simple_d[r][1] + dmob[2];
        if (dz <= 0.0) return 0;
        pl->ofx = (double)s_simple_d[r][2] + dmob[0];
        pl->ofy = (double)s_simple_d[r][0] + dmob[1];
        pl->TR[0] = pl->TR[1] = 0.0;
        pl->TR[2] = (double)(((int)dz >> 2) * 2);
        memset(pl->R, 0, sizeof pl->R);
        pl->R[0][0] = 0x1000; pl->R[1][1] = 0x0A00; pl->R[2][2] = 0x1000;
    } else {
        /* view-space centre, world/2 after the doubling (mobydraw.s:3946-3952,4058-4060).
         * The record's deltas are (cam - moby) >> 2 on the first two components
         * and (moby - cam) >> 2 on the third (mobydraw.s:81,87,94), so the
         * moby's own displacement enters in quarter-world units with the
         * opposite sign per component, beside the camera's. */
        ngte_mac(Rcam, (double)s_simple_d[r][0] + dcam[1] - dmob[1] * 0.25,
                       (double)s_simple_d[r][1] + dcam[2] - dmob[2] * 0.25,
                       (double)s_simple_d[r][2] + dcam[0] + dmob[0] * 0.25, &m1, &m2, &m3);
        /* The record's own draw range (mobydraw.s:3958). Extended LOD removes
         * the range rather than widening it: the threshold is a per-moby byte
         * scaled by 256, so any raised value is still a ceiling a high camera
         * clears, while terrain carries no distance term at all. */
        if (!openpete_no_lod && s_simple_far[r] >= 0 &&
            m3 - (double)s_simple_far[r] >= 0.0) return 0;
        pl->TR[0] = m1*2.0; pl->TR[1] = m2*2.0; pl->TR[2] = m3*2.0;
    }

    /* A header outside RAM is not a pointer, and the vertex and face walks read
     * through it. */
    pl->header = cpu->read_word(slot + 0x14u);
    if (pl->header < 0x80000000u) return 0;
    if (pl->screen) return 1;

    /* The cull tests the draw repeats, on the header's four bounding-box
     * bytes scaled by 16 (mobydraw.s:3963-3975,3976-3981,3993-4002). */
    const uint32_t h1 = cpu->read_word(pl->header + 4u);
    double bx3 = (double)((h1 >> 20) & 0xFF0u), bx2 = (double)((h1 >> 12) & 0xFF0u);
    double bx1 = (double)((h1 >>  4) & 0xFF0u), bx0 = (double)((h1 <<  4) & 0xFF0u);
    double ax = m1 < 0 ? -m1 : m1, ay = m2 < 0 ? -m2 : m2;
    /* The lateral arm's 3:4 slope is the draw's 4:3 half-angle
     * (mobydraw.s:3976-3981), so the frame's own horizontal widening scales
     * it: at 4:3 with the game's FOV the factor is 1.0 and the test is the
     * draw's, bit for bit. The vertical arm below keeps the game's slope,
     * since Hor+ leaves the vertical FOV alone. */
    if (4.0*(ax - bx3) - 3.0*moby_native_view_widen()*(m3 + bx2) >= 0.0) return 0;
    if (m3 + bx1 <= 0.0) return 0;
    if ((m3 + bx0) - 3.0*(ay - bx1) <= 0.0) return 0;
    return 1;
}

/** @brief One second-kind face's bucket word in the record's paint order.
 *
 * moby_rank.h rows MOBY_OTK_S23_SIMPLE and MOBY_OTK_S23_SIMPLE_FLAT:
 * face word 1 bit 1 selects the flat-lit branch (mobydraw.s 4720-4722,
 * 4743-4745), whose back faces the walk keeps while TRZ, less 0x800 under word
 * 1 bit 0, is negative (4748-4760). A standard face's bias is the signed top
 * nibble of word 0 (4436, 4584), and word 1 bit 0 lets a standard triangle
 * pass the facing test either way round (4602-4605, 4743-4745).
 * @param ork The record's two row states, standard then flat-lit.
 * @param iv The record's per-vertex key inputs.
 * @param idx The face's four vertex indices.
 * @param tri 1 for a triangle; idx[3] is then unused.
 * @param f0 Face word 0.
 * @param f1 Face word 1.
 * @return The bucket word moby_otk_face_bucket returns. */
static int32_t s23_simple_face_otk(const moby_otk_rec_t ork[2], const moby_otk_vert_t* iv,
                                   const int idx[4], int tri, uint32_t f0, uint32_t f1) {
    const int flat = (f1 & 2u) != 0;
    moby_otk_face_t f = { { &iv[idx[0]], &iv[idx[1]], &iv[idx[2]], tri ? NULL : &iv[idx[3]] },
                          flat ? 0 : (int32_t)f0 >> 28,
                          flat ? ((f1 & 1u) ? 0x800 : 0) : INT32_MIN,
                          (int8_t)(!flat && tri && (f1 & 1u)), (int8_t)flat };
    return moby_otk_face_bucket(&ork[flat], &f);
}

static int s23_simple_decode(CPUState* cpu) {
    int Rcam[3][3];
    ngte_read_matrix(cpu, game_layout.cam.base, Rcam);   /* mobydraw.s:3898-3908 */
    /* The far-geometry suppression word the cull parks in GTE data 3
     * (mobydraw.s:34,36), consulted at mobydraw.s:4336-4341. */
    int skip_lp = cpu->read_word(moby_game_s23.simple_skip_lowpoly) != 0u;
    int emitted = 0;

    /* The camera position the cull subtracted is baked into every record's
     * delta (mobydraw.s:53 the camera position, :81,87,94 the three
     * subtractions, :621-627 the store), so replaying a record under a staged
     * camera re-rotates it but keeps the tick's translation.
     * Re-base the delta by the camera-position change. The cull stores
     * (moby - cam) >> 2 on the first component and (cam - moby) >> 2 on the
     * other two, so a camera that moved by e shifts the stored halves by
     * -e/4, +e/4 and +e/4. Held in double and added before the transform, so a
     * staged position equal to the tick's is a no-op bit for bit.
     *
     * A tick camera is installed only for a sub-tick present, so its absence is
     * what keeps every canonical decode — the inline pass and the canonical lag
     * leg alike — bit for bit what the cull wrote: no camera delta, no
     * per-record own motion and the tick's own rotation word. */
    double dcam[3] = { 0.0, 0.0, 0.0 };
    int32_t tickpos[3];
    const int bracketed = moby_native_tick_camera_pos(tickpos);
    if (bracketed) {
        static const double sign[3] = { -0.25, 0.25, 0.25 };
        for (int i = 0; i < 3; i++) {
            int32_t staged = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)i);
            dcam[i] = ((double)staged - (double)tickpos[i]) * sign[i];
        }
    }

    static double vpos[S23_SIMPLE_VERTS][3];
    static float  vscr[S23_SIMPLE_VERTS][2];
    static moby_otk_vert_t iv[S23_SIMPLE_VERTS];   /* the draw's integer RTPS (paint key) */
    static uint32_t ctab[S23_SIMPLE_COLS];
    uint32_t flat_w[4];
    memcpy(flat_w, s_flat_entry, sizeof flat_w);

    for (int r = 0; r < s_simple_qn; r++) {
        uint32_t moby = s_simple_q[r][0];
        /* The moby's own world displacement over the fraction of the tick this
         * present shows. The cull composed the record at the tick's position,
         * which both kinds encode directly, so without this term the record
         * holds that position for the whole tick and steps at the tick rate. */
        double dmob[3] = { 0.0, 0.0, 0.0 };
        if (bracketed) moby_record_world_offset(moby, dmob);
        s23_place pl;
        const int placed = s23_simple_place(cpu, r, Rcam, dcam, dmob, &pl);
        if (pl.screen) s_simple_screen++;
        s_simple_recs++;
        if (!placed) { s_simple_culled++; continue; }
        if (pl.screen) s_simple_screen_drawn++;

        const uint32_t H = pl.header;
        uint32_t h0 = cpu->read_word(H);

        const double* TR = pl.TR;           /* the stored translation, world/2 */
        int trz = (int)TR[2];

        /* The ordering-table key the draw drops the record on
         * (mobydraw.s:4193-4207): its far end is a cull, not paint order. */
        uint32_t rot = cpu->read_word(moby + game_layout.moby.angle_off);
        int key = (2*trz >> 7) - (int)(int8_t)(rot >> 24);
        if (key < 0) key = 0;
        /* The far end of that key is where the record runs out of ordering
         * table, which is a range limit and not paint order, so extended LOD
         * lifts it with the other one; the native path sorts by depth and
         * needs no bucket. */
        if (!openpete_no_lod && key >= 272 && (key << 3) + 256 >= 3840) {
            s_simple_culled++; continue;
        }

        /* The fade/LOD tier: the moby's size byte against twice the depth
         * (mobydraw.s:4193-4196). Its sign picks the low-poly lists. */
        const int size_term = (int)(cpu->read_byte(moby + moby_game.renderer_off)
                                    & moby_game.renderer_mask) << 10;
        int tier = size_term - 2*trz;
        /* Extended LOD removes the depth term, the rule the animated kind's
         * tier takes: every test below then reads the value the draw computes
         * at depth 0, so the vertex count, the list pair, the fade and the
         * CLUT bias all take the draw's own near branch (tier >= 4096 at
         * mobydraw.s:4285-4289). A size term below that band keeps the depth
         * term: such a record fades at depth 0 as well, which is the moby's
         * own authoring and not a range limit. */
        if (openpete_no_lod && size_term >= 4096) tier = size_term;

        int nv = (int)((tier >= 0 ? (h0 >> 8) : (h0 >> 16)) & 0xFFu);
        if (nv < 3) continue;
        if (nv > S23_SIMPLE_VERTS) nv = S23_SIMPLE_VERTS;

        /* The moby's three rotations, composed onto the placement matrix in the
         * draw's order: Y, then X, then Z (mobydraw.s:4063-4110, 4111-4158,
         * 4159-4189, one column pair replaced per axis through two MVMVAs).
         * Within a sub-tick present the angles come from the object's own pair,
         * so a moby turning a byte per tick sweeps instead of stepping; the
         * canonical pass takes the tick's word, where the composition is the
         * table lookup the draw itself does. */
        int R[3][3];
        ngte_euler_sc esc;
        if (!bracketed || !moby_record_euler_at_present(cpu, moby, &esc))
            ngte_euler_sincos(cpu, rot, &esc);
        ngte_compose_euler(pl.R, &esc, R);

        moby_simple_record(cpu, moby);
        if (pl.screen) moby_simple_screen_origin(pl.ofx, pl.ofy);
        /* The translation the draw loads into TRX/TRY/TRZ (mobydraw.s
         * 4190-4192): the MVMVA (sf=1) truncates the rotated delta to an
         * integer before the rejoin doubles it (mobydraw.s 3946-3952,
         * 4058-4060), where TR keeps the fraction. With it, the record's
         * paint key for both face families. */
        const int itr[3] = { 2 * (int)floor(TR[0] * 0.5), 2 * (int)floor(TR[1] * 0.5),
                             2 * (int)floor(TR[2] * 0.5) };
        moby_otk_rec_t ork[2];
        moby_simple_otk_record(MOBY_OTK_S23_SIMPLE, itr[2], &ork[0]);
        moby_simple_otk_record(MOBY_OTK_S23_SIMPLE_FLAT, itr[2], &ork[1]);

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
            moby_simple_otk_vertex(&ork[0], itr, R, vx, vy, vz, &iv[i]);
        }
        s_simple_verts += nv;

        /* OPENPETE_MOBY_POSTRACE_CLS=<class>: the submitted screen-space
         * centroid of each simple-kind record of that class, the counterpart
         * of the animated kind's trace in the shared feeder. 0xFFFF traces
         * every class; the two kinds read the variable the same way, so one
         * setting follows a moby across the kind boundary. */
        {
            static int s_cls = -2;
            if (s_cls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_cls = e ? (int)strtol(e, NULL, 0) : -1; }
            /* The class read stays inside the enable: this loop runs per
             * record per present. */
            const uint16_t s_tcls = s_cls >= 0
                ? (uint16_t)cpu->read_half(moby + game_layout.moby.class_off) : 0u;
            if (s_cls >= 0 && (s_cls == 0xFFFF || s_tcls == (uint16_t)s_cls)) {
                double cx = 0.0, cy = 0.0;
                float lo[2] = { vscr[0][0], vscr[0][1] }, hi[2] = { lo[0], lo[1] };
                for (int i = 0; i < nv; i++) {
                    cx += vscr[i][0]; cy += vscr[i][1];
                    for (int k = 0; k < 2; k++) {
                        if (vscr[i][k] < lo[k]) lo[k] = vscr[i][k];
                        if (vscr[i][k] > hi[k]) hi[k] = vscr[i][k];
                    }
                }
                /* The centroid alone is blind to the rotation: a record turning
                 * about its own centre moves no centroid. The projected extent
                 * and the composed matrix's first row carry the rotation. */
                fprintf(stderr, "[simple vbl=%u] m=%08x cls=%03x n=%d c=(%.2f,%.2f) "
                                "ext=(%.2f,%.2f) r0=(%d,%d,%d) p=(%d,%d,%d) rot=%08x "
                                "dmob=(%.2f,%.2f,%.2f)\n",
                        openpete_frame, moby, s_tcls, nv, cx / nv, cy / nv,
                        (double)(hi[0] - lo[0]), (double)(hi[1] - lo[1]),
                        R[0][0], R[0][1], R[0][2],
                        (int)cpu->read_word(moby + game_layout.moby.pos_off),
                        (int)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
                        (int)cpu->read_word(moby + game_layout.moby.pos_off + 8u), rot,
                        dmob[0], dmob[1], dmob[2]);
            }
        }

        /* The face and colour lists, and the fade the near band applies
         * (mobydraw.s:4285-4346). */
        /* The vertex count switches on the tier's sign, the face and colour
         * lists one step earlier: tier 0 takes the high-poly count and the
         * low-poly lists (mobydraw.s:4215-4219,4285-4287). */
        if (tier <= 0 && skip_lp) continue;
        if (tier <= 0) s_simple_lp++;
        uint32_t pair = cpu->read_word(H + (tier > 0 ? 0x08u : 0x0Cu));
        uint32_t faces = H + (pair & 0xFFFFu);
        uint32_t cols  = H + (pair >> 16);
        int ncol = (int)(h0 & 0xFFu);
        if (ncol > S23_SIMPLE_COLS) ncol = S23_SIMPLE_COLS;
        int clut_bias = 0;
        uint32_t far_cols = H + cpu->read_half(H + 0x0Eu);
        int fade = (tier > 0 && tier < 4096 && far_cols != cols);
        int ir0 = fade ? 4096 - tier : 0;
        if (fade) clut_bias = ir0 >> 9;
        if (ir0 > 0) s_simple_fade++;
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
            uint32_t e = moby_game_s23.simple_flat_coltab + (mcol << 4);
            flat_w[0] = cpu->read_word(e);
            flat_w[1] = cpu->read_word(e + 4u);
            flat_w[2] = cpu->read_word(e + 8u);
            flat_w[3] = cpu->read_word(e + 12u);
        }

        uint32_t bytecount = cpu->read_word(faces);
        uint32_t fp = faces + 4u, end = fp + bytecount;
        int guard = 0;
        while (fp + 8u <= end && guard++ < S23_SIMPLE_FACES) {
            uint32_t f0 = cpu->read_word(fp);
            uint32_t f1 = cpu->read_word(fp + 4u);
            /* The stride is the face's own texture bit; the flat-lit material
             * draws untextured but still carries the three words. */
            uint32_t adv = ((int32_t)f1) < 0 ? 0x14u : 0x08u;
            int idx[4] = { (int)((f0 >> 21) & 0x7Fu), (int)((f0 >> 14) & 0x7Fu),
                           (int)((f0 >>  7) & 0x7Fu), (int)( f0        & 0x7Fu) };
            int tri = (idx[2] == idx[3]);
            s_simple_faces++;
            if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv || (!tri && idx[3] >= nv)) {
                fp += adv; continue;
            }
            int u[4] = {0,0,0,0}, v[4] = {0,0,0,0};
            int tpx = 0, tpy = 0, clx = 0, cly = 0, bpp = 16;
            float sc[4][2];
            for (int k = 0; k < 4; k++) {
                sc[k][0] = vscr[idx[k]][0]; sc[k][1] = vscr[idx[k]][1];
            }
            s23_shade sh;
            if (!s23_simple_shade_face(R, f1, tri,
                                       s23_nclip(sc, 0, 1, 2),
                                       tri ? 0.0 : s23_nclip(sc, 3, 1, 2),
                                       TR[2], flat_w, ctab, ncol, &sh)) {
                fp += adv; continue;
            }
            const uint32_t* col = sh.col;
            int blend = sh.blend;

            if (sh.textured) {
                uint32_t t0 = cpu->read_word(fp + 0x08u);
                uint32_t t1 = cpu->read_word(fp + 0x0Cu);
                uint32_t t2 = cpu->read_word(fp + 0x10u);
                u[0]=(int)(t0 & 0xFFu); v[0]=(int)((t0 >> 8) & 0xFFu);
                u[1]=(int)(t1 & 0xFFu); v[1]=(int)((t1 >> 8) & 0xFFu);
                u[2]=(int)(t2 & 0xFFu); v[2]=(int)((t2 >> 8) & 0xFFu);
                u[3]=(int)((t2 >> 16) & 0xFFu); v[3]=(int)((t2 >> 24) & 0xFFu);
                unsigned cl = (t0 >> 16) & 0xFFFFu;
                unsigned tp = (t1 >> 16) & 0x7FFFu;
                ngte_tpage t;
                ngte_decode_tpage(tp, cl, 0u, &t);
                tpx = t.tpx;
                tpy = t.tpy;
                bpp = t.bpp;
                clx = t.clx;
                cly = t.cly + clut_bias;
                if ((t1 >> 31) & 1u)
                    blend = moby_abr_blend(t.abr);
            }

            const int32_t otk = s23_simple_face_otk(ork, iv, idx, tri, f0, f1);
            double p[3][3]; uint32_t rgb[3]; int uu[3], vv[3];
            if (sh.emit_a) {
                const int o[3] = {0,1,2};
                for (int k = 0; k < 3; k++) {
                    memcpy(p[k], vpos[idx[o[k]]], sizeof p[k]);
                    rgb[k] = col[o[k]]; uu[k] = u[o[k]]; vv[k] = v[o[k]];
                }
                const moby_otk_vert_t* const ia[3] = { &iv[idx[0]], &iv[idx[1]], &iv[idx[2]] };
                moby_simple_tri(p, rgb, uu, vv, sh.textured, tpx, tpy, clx, cly, bpp, blend,
                                (uint32_t)(idx[0] | (idx[1] << 10) | (idx[2] << 20)), otk, ia);
                emitted++; s_simple_tris++;
            }
            if (sh.emit_b) {
                const int o[3] = {1,3,2};
                for (int k = 0; k < 3; k++) {
                    memcpy(p[k], vpos[idx[o[k]]], sizeof p[k]);
                    rgb[k] = col[o[k]]; uu[k] = u[o[k]]; vv[k] = v[o[k]];
                }
                const moby_otk_vert_t* const ib[3] = { &iv[idx[1]], &iv[idx[3]], &iv[idx[2]] };
                moby_simple_tri(p, rgb, uu, vv, sh.textured, tpx, tpy, clx, cly, bpp, blend,
                                (uint32_t)(idx[1] | (idx[3] << 10) | (idx[2] << 20)),
                                moby_otk_second(otk, sh.emit_a), ib);
                emitted++; s_simple_tris++;
            }
            fp += adv;
        }
    }
    return emitted;
}

/** @brief A sub-tick present's replay of the second kind: the captured queue decoded again under the present's camera.
 *
 * Makes no call into the game's draw. Resets the pass counters, and prints its
 * own census line under moby_verify_vbl, since the pass census covers the
 * canonical tick alone.
 * @param cpu The guest CPU state, read only.
 * @return The number of triangles emitted. */
static int s23_simple_replay(CPUState* cpu) {
    s_simple_recs = s_simple_verts = s_simple_faces = s_simple_tris = 0;
    s_simple_flat = s_simple_screen = s_simple_culled = s_simple_screen_drawn = 0;
    s_simple_lp = s_simple_fade = 0;
    int n = s23_simple_decode(cpu);
    /* The replay's own census: the queue it read and what that queue decoded
     * to. The pass census covers the canonical tick alone, so without this a
     * replay of a queue no canonical pass built this tick reads as silence. */
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-simple-replay vbl=%u] queued=%d drawn=%ld culled=%ld "
                        "screenkind=%ld screendrawn=%ld "
                        "verts=%ld faces=%ld tris=%d lp=%ld fade=%ld\n",
                openpete_frame, s_simple_qn, s_simple_recs, s_simple_culled,
                s_simple_screen, s_simple_screen_drawn,
                s_simple_verts, s_simple_faces, n, s_simple_lp, s_simple_fade);
    return n;
}

/* ---- extended-LOD reveal ------------------------------------------------ */
/** @brief The half-width of the distance box the cull tests a moby against (mobydraw.s:75-100).
 *
 * The signed low byte of the halfword at Moby+0x4C scaled by 256, widened by
 * 256 per unit of the low two drawn-verdict bits that halfword's high byte
 * carries. A byte that is zero or negative leaves a degenerate box and the
 * cull skips the moby, which is the unpopulated-slot sentinel.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param drawn_bits The drawn-verdict byte the cull tested.
 * @return The half-width, or 0 for the sentinel. */
static int s23_cull_box_threshold(CPUState* cpu, uint32_t moby, unsigned drawn_bits) {
    const int rad = (int)(int8_t)cpu->read_byte(moby + 0x4Cu);
    if (rad <= 0) return 0;
    return (rad << 8) + (int)((drawn_bits & 3u) << 8);
}

/** @brief The animated kind's admission as the cull runs it, with the lateral slope scaled by `widen`.
 *
 * mobydraw.s:62-187; Spyro 2 0x80043920-0x80043AFC, the same instructions. The cull clears the drawn-verdict byte and writes 1 or 2
 * only on a moby that passes every test (mobydraw.s:73,224), so a byte of 0
 * inside the box names a moby one of these tests rejected; under a view wider
 * than 4:3 the lateral arm is the one whose answer changes.
 *
 * The box is the cull's own (s23_cull_box_threshold) on the quarter-world
 * deltas (moby - cam) >> 2, (cam - moby) >> 2 and (cam - moby) >> 2
 * (mobydraw.s:77-100). A sector byte at Moby+0x52 below 0xFE must name a
 * sector whose byte in the table $gp holds (D_80072330, mobydraw.s:49-50, the
 * layout's env_anim.live_table) is non-zero (mobydraw.s:95-106); a
 * KUSEG model-table entry is the simple kind, which the second queue tests
 * (mobydraw.s:109-116). The draw radius is the animation header's byte 7
 * scaled by 16 (mobydraw.s:118-124), and the three thresholds are its sums of
 * shifts (mobydraw.s:128-138, 158-163). The centre is the GTE MVMVA of the
 * deltas through the camera matrix at G_CAMERA+0x00, IR1 the Y delta, IR2 the
 * Z and IR3 the X, each loaded into a 16-bit IR register (mobydraw.s:119-141).
 * The arms, in the cull's order:
 *   range     MAC3 - box >= 0 rejects                     (mobydraw.s:142-143)
 *   near      MAC3 + r <= 0 rejects                       (mobydraw.s:144-145)
 *   lateral   4(|MAC1| - a2) - 3(MAC3 + a3) >= 0 rejects  (mobydraw.s:147-157)
 *   vertical  (MAC3 + t1) - 3(|MAC2| - t0) <= 0 rejects   (mobydraw.s:178-187)
 * The lateral arm's 3:4 slope is the 4:3 half-angle, so the widened view
 * multiplies its MAC3 term by `widen`; the vertical arm keeps the game's
 * slope, since Hor+ leaves the vertical field of view alone. At `widen` 1.0 the
 * widened and canonical lateral arms are one test and nothing is admitted.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param drawn_bits The drawn-verdict byte the box threshold widens by.
 * @param widen The lateral slope factor, 1.0 at 4:3.
 * @return The first test that rejects the moby, MOBY_WIDE_CANONICAL when the
 *         game's own lateral arm also admits it, or MOBY_WIDE_ADMIT when only
 *         the widened arm does. */
static moby_wide_verdict_t s23_cull_view_admits(CPUState* cpu, uint32_t moby,
                                                unsigned drawn_bits, double widen) {
    const int64_t t = s23_cull_box_threshold(cpu, moby, drawn_bits);
    if (t <= 0) return MOBY_WIDE_OUTSIDE;
    const uint32_t cam = game_layout.cam.base;
    const int32_t dx = (int32_t)(cpu->read_word(moby + 0x0Cu) - cpu->read_word(cam + 0x28u)) >> 2;
    const int32_t dy = (int32_t)(cpu->read_word(cam + 0x2Cu) - cpu->read_word(moby + 0x10u)) >> 2;
    const int32_t dz = (int32_t)(cpu->read_word(cam + 0x30u) - cpu->read_word(moby + 0x14u)) >> 2;
    if (dx <= -t || dx >= t || dy <= -t || dy >= t || dz <= -t || dz >= t)
        return MOBY_WIDE_OUTSIDE;
    const uint32_t sector = cpu->read_byte(moby + 0x52u);
    if (sector < 0xFEu && game_layout.env_anim.live_table &&
        cpu->read_byte(game_layout.env_anim.live_table + sector) == 0u)
        return MOBY_WIDE_SECTOR;
    const uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model < 0x80000000u) return MOBY_WIDE_OTHER_KIND;
    const uint32_t id     = cpu->read_byte(moby + game_layout.moby.anim_state_off);
    const uint32_t header = cpu->read_word(model + game_layout.moby.model_anims_off + id * 4u);
    if (header < 0x80000000u) return MOBY_WIDE_OTHER_KIND;
    const int64_t r  = (int64_t)cpu->read_byte(header + 7u) << 4;
    const int64_t a2 = (r >> 1) + (r >> 2) + (r >> 5), a3 = (r >> 1) + (r >> 4) + (r >> 5);
    const int64_t t0 = (r >> 1) + (r >> 4),            t1 = (r >> 2) + (r >> 4);
    int R[3][3];
    ngte_read_matrix(cpu, cam, R);
    const int64_t ir1 = (int16_t)dy, ir2 = (int16_t)dz, ir3 = (int16_t)dx;
    int64_t mac[3];
    for (int i = 0; i < 3; i++)
        mac[i] = ((int64_t)R[i][0] * ir1 + (int64_t)R[i][1] * ir2 + (int64_t)R[i][2] * ir3) >> 12;
    const int64_t ax = mac[0] < 0 ? -mac[0] : mac[0], ay = mac[1] < 0 ? -mac[1] : mac[1];
    if (mac[2] - t >= 0) return MOBY_WIDE_RANGE;
    if (mac[2] + r <= 0) return MOBY_WIDE_NEAR;
    const double lat = 4.0 * (double)(ax - a2);
    if (lat - 3.0 * widen * (double)(mac[2] + a3) >= 0.0) return MOBY_WIDE_LATERAL;
    if ((mac[2] + t1) - 3 * (ay - t0) <= 0) return MOBY_WIDE_VERTICAL;
    if (lat - 3.0 * (double)(mac[2] + a3) < 0.0) return MOBY_WIDE_CANONICAL;
    return MOBY_WIDE_ADMIT;
}

/** @brief Saturating narrow of a full-width intermediate to the range the cull's register or field holds.
 *
 * Every intermediate the cull stores lands in an int16 GTE
 * register or an int16 record field, which wraps once extended LOD reveals a
 * moby past the range the cull could ever queue. Inside that range this is the
 * identity, past it a saturated value stays at the end of the range instead of
 * reappearing at the other end.
 * @param v The value.
 * @param lo The range's low end.
 * @param hi The range's high end.
 * @return v clamped to [lo, hi]. */
static int64_t s23_sat(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** @brief The animated record's translation, composed as the cull composes it at full width.
 *
 * The quarter-world view-space centre shifted left 2 and right by the model's
 * scale byte, then rescaled by the moby's size byte through a GPF and a shift
 * of 5
 * (mobydraw.s:510,537-539,546-551,572-574). The size byte scales the moby on
 * screen by moving it in depth, 32 being unity.
 *
 * The GTE holds both the GPF's input and its product in int16 registers.
 * Inside the cull's own distance box neither bound is reachable: the box caps
 * the centre at the render-radius byte scaled by 256, so the shift left 2
 * lands inside an int16 and the size byte is a fraction near unity. Past the
 * box — which is the whole of what this builder composes for — the input bound
 * alone collapses all three axes to the same 1023 and stands the moby on the
 * near plane at the size of the screen. Carrying the rescale at full width is
 * the treatment the centre's own MVMVA already takes, and is identical to the
 * GTE everywhere the GTE's own range holds.
 *
 * The cull drops a record whose result passes 63000 (mobydraw.s:546-551); that
 * is a range limit, which is what extended LOD lifts, so it is not applied.
 * @param f The animated fields, whose centre is used.
 * @param scale The model's scale byte, clamped to [0, 31].
 * @param size The moby's size byte; 0 skips the rescale.
 * @param tr Receives the translation, saturated to int32. */
static void s23_anim_translation(const moby_anim_fields_t* f, int scale, int size,
                                 int32_t tr[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t v = ((int64_t)f->centre[i] << 2) >> (int)s23_sat(scale, 0, 31);
        if (size) v = (v * size) >> 5;
        tr[i] = (int32_t)s23_sat(v, INT32_MIN, INT32_MAX);
    }
}

/** @brief Composes the animated record's 0x44 bytes, the inverse of s23_read_record.
 *
 * The flags word carries the cull's draw verdict in byte 0, the keyframe-blend
 * byte in byte 1 (mobydraw.s:459-461,522) and the LOD verdict in its top half
 * (mobydraw.s:501,918-921). The matrix is five words, and the ordering-table
 * key shares the spare half of the last (mobydraw.s:344-348,559). The verdict
 * byte is the near one: the far verdict only ever narrows what the record
 * draws, and extended LOD is the rule that it is drawn.
 * @param w Receives the record's words.
 * @param f The animated fields: headers, keyframe B, blend byte and matrix.
 * @param moby The Moby address.
 * @param material The record's material word.
 * @param frame_a Keyframe A's reference.
 * @param patch The faceblock patch list, or 0.
 * @param tier The LOD verdict.
 * @param key The ordering-table key.
 * @param tr The translation. */
static void s23_write_anim_record(uint32_t* w, const moby_anim_fields_t* f,
                                  uint32_t moby, uint32_t material,
                                  uint32_t frame_a, uint32_t patch,
                                  int tier, int key, const int32_t tr[3]) {
    const uint32_t L = game_layout.moby.rec_matrix_off / 4u;
    w[0] = ((uint32_t)(tier & 0xFFFF) << 16) | (f->blend << 8) | 1u;
    w[game_layout.moby.rec_header_off   / 4u] = f->header;
    w[game_layout.moby.rec_header_b_off / 4u] = f->header_b;
    w[game_layout.moby.rec_frame_a_off  / 4u] = frame_a;
    w[game_layout.moby.rec_frame_b_off  / 4u] = f->frame_b;
    w[0x14u / 4u]                             = patch;
    for (int i = 0; i < 3; i++)
        w[game_layout.moby.rec_tr_off / 4u + (uint32_t)i] = (uint32_t)tr[i];
    w[L + 0u] = ((uint32_t)(f->M[0][1] & 0xFFFF) << 16) | (uint32_t)(f->M[0][0] & 0xFFFF);
    w[L + 1u] = ((uint32_t)(f->M[1][0] & 0xFFFF) << 16) | (uint32_t)(f->M[0][2] & 0xFFFF);
    w[L + 2u] = ((uint32_t)(f->M[1][2] & 0xFFFF) << 16) | (uint32_t)(f->M[1][1] & 0xFFFF);
    w[L + 3u] = ((uint32_t)(f->M[2][1] & 0xFFFF) << 16) | (uint32_t)(f->M[2][0] & 0xFFFF);
    w[L + 4u] = ((uint32_t)(uint16_t)key      << 16) | (uint32_t)(f->M[2][2] & 0xFFFF);
    w[game_layout.moby.rec_material_off / 4u] = material;
    w[game_layout.moby.rec_moby_off     / 4u] = moby;
}

/** @brief The keyframe reference the record stores, and the faceblock patch list that rides with it.
 *
 * A record on the low-poly tier carries the keyframe word with its top bits
 * set, which is how the draw skips the patch apply
 * (mobydraw.s:501,504-505); otherwise the list is resolved through the Model's
 * own table, whose base is Model+0x34, indexed by the keyframe word's top byte
 * masked to 0x7C, and the word read there is the list's byte offset from that
 * same base (mobydraw.s:511-517).
 * @param cpu The guest CPU state, read only.
 * @param f The animated fields.
 * @param tier The record's LOD verdict; negative is the low-poly tier.
 * @param patch Receives the patch list, or 0 when there is none.
 * @return The keyframe reference. */
static uint32_t s23_anim_keyframe_ref(CPUState* cpu, const moby_anim_fields_t* f,
                                      int tier, uint32_t* patch) {
    *patch = 0u;
    if (tier < 0) return f->frame_a | 0xFC000000u;
    const uint32_t base = cpu->read_word(f->model + 0x34u);
    if (base >= 0x80000000u)
        *patch = base + cpu->read_word(base + ((f->frame_a >> 24) & 0x7Cu));
    return f->frame_a;
}

/** @brief Builds the animated kind's cull record for one moby the box rejected and appends it to the host record list.
 *
 * The record is composed the way the cull composes it and appended to the host copy of the cull's own
 * record list. The regular walk reads that copy, so an appended record decodes
 * through exactly the path a queued one does and a sub-tick present replays it
 * with the rest.
 *
 * The LOD verdict the record carries is
 *
 *   tier = (Moby+0x4E low byte << 8) - the view-space depth before the
 *          translation rescale                              (mobydraw.s:450-456)
 *
 * and its sign picks the low-poly vertex count and face list. The cull stores
 * it in a halfword, which a revealed moby's depth overruns; a wrapped verdict
 * re-enters the near band and re-fades a record that is unambiguously
 * far-tier, so it saturates. The walk applies the extended model rule to the
 * stored verdict per fed record, as it does for a queued one.
 *
 * The ordering-table key is the rescaled depth less 0x200 shifted by the
 * model's span byte, halved (mobydraw.s:554-559). Like the far drop it is a
 * range limit rather than paint order, so it saturates rather than wrapping;
 * the native draw reads neither it nor the ordering-table slot, which the
 * depth buffer replaces.
 *
 * Nothing here writes guest memory: the copy is the host's and the moby's own
 * verdict byte stays the cull's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param why Receives the reason: MOBY_REVEAL_OK, or why no record was appended.
 * @return 1 when a record was appended, 0 otherwise. */
static int s23_reveal_anim_record(CPUState* cpu, uint32_t moby, moby_reveal_reason_t* why) {
    /* A moby whose colour word has bit 30 set takes a second composition path
     * for its rotation and colour (mobydraw.s:228,633-640), which this builder
     * does not compose. */
    const uint32_t colour = cpu->read_word(moby + game_layout.moby.colour_off);
    if (colour & 0x40000000u) { *why = MOBY_REVEAL_OTHER_FORM; return 0; }
    moby_anim_fields_t f;
    if (!moby_reveal_anim_fields(cpu, moby, &f)) { *why = MOBY_REVEAL_NO_POSE; return 0; }
    if (moby_rec_holds_moby(cpu, moby)) { *why = MOBY_REVEAL_DUPLICATE; return 0; }
    uint32_t* w = moby_rec_append(NULL);
    if (!w) { *why = MOBY_REVEAL_QUEUE_FULL; return 0; }

    const uint32_t lod = cpu->read_half(moby + 0x4Eu);   /* the LOD byte, then the size byte */
    const int tier = (int)s23_sat(((int64_t)(lod & 0xFFu) << 8) - (int64_t)f.centre[2],
                                  -32768, 32767);
    int32_t tr[3];
    s23_anim_translation(&f, (int)(cpu->read_byte(f.header + game_layout.moby.anim_scale_off) & 0xFFu),
                         (int)((lod >> 8) & 0xFFu), tr);
    uint32_t patch = 0u;
    const uint32_t frame_a = s23_anim_keyframe_ref(cpu, &f, tier, &patch);
    /* The span the ordering-table key subtracts is the larger of the two
     * keyframes' own (mobydraw.s:495-498). */
    int span = (int)cpu->read_byte(f.header + 0x03u);
    const int span_b = (int)cpu->read_byte(f.header_b + 0x03u);
    if (f.blend && span_b > span) span = span_b;
    const int key = (int)s23_sat((((int64_t)tr[2] - ((int64_t)0x200 << (int)s23_sat(span, 0, 30))) >> 1),
                                 -32768, 32767);
    s23_write_anim_record(w, &f, moby, colour, frame_a, patch, tier, key, tr);
    *why = MOBY_REVEAL_OK;
    return 1;
}

/** @brief Builds one rejected moby's record for the queue of its model kind.
 *
 * An animated model goes to s23_reveal_anim_record. A second-kind model's queue
 * record is built the way the cull composes it (mobydraw.s:604-630): the Moby, the three quarter-world
 * camera deltas — (moby - cam) >> 2 on the first component and (cam - moby)
 * >> 2 on the other two (mobydraw.s:81,87,94) — and the model's animation slot
 * at Model + (Moby+0x3C byte 0) * 4. A model-table entry that is KSEG0 is the
 * animated kind and belongs to the other queue (mobydraw.s:116).
 *
 * The deltas also go into the full-width fields, since a revealed moby sits
 * past what the packed halves can hold, and the far-cull distance is -1: a
 * host-built record carries no range limit, which is what extended LOD means.
 * Nothing here writes guest memory — the queue is the host copy taken at the
 * cull hook, and the moby's own verdict byte stays the cull's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param kind The queue being filled; a moby of the other kind is refused.
 * @param why Receives the reason: MOBY_REVEAL_OK, or why no record was built.
 * @return 1 when a record was built, 0 otherwise. */
static int s23_reveal_record(CPUState* cpu, uint32_t moby, moby_reveal_kind_t kind,
                             moby_reveal_reason_t* why) {
    const uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model == 0u) { *why = MOBY_REVEAL_NO_MODEL; return 0; }
    if (s23_model_is_animated(cpu, model) != (kind == MOBY_REVEAL_ANIMATED)) {
        *why = MOBY_REVEAL_OTHER_KIND; return 0;
    }
    if (kind == MOBY_REVEAL_ANIMATED) return s23_reveal_anim_record(cpu, moby, why);
    if (s_simple_qn >= S23_SIMPLE_QCAP) { *why = MOBY_REVEAL_QUEUE_FULL; return 0; }
    /* The cull admits on more than the box, so a moby outside the box can
     * already hold a captured record; a second one would draw it twice. */
    for (int k = 0; k < s_simple_qn; k++)
        if (s_simple_q[k][0] == moby) { *why = MOBY_REVEAL_DUPLICATE; return 0; }
    const uint32_t anim = cpu->read_word(moby + game_layout.moby.anim_state_off);
    const int32_t cx = (int32_t)cpu->read_word(game_layout.cam.position + 0u),
                  cy = (int32_t)cpu->read_word(game_layout.cam.position + 4u),
                  cz = (int32_t)cpu->read_word(game_layout.cam.position + 8u);
    const int32_t mx = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 0u),
                  my = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
                  mz = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    const int32_t dx = (mx - cx) >> 2, dy = (cy - my) >> 2, dz = (cz - mz) >> 2;
    const int i = s_simple_qn;
    s_simple_q[i][0] = moby;
    s_simple_q[i][1] = ((uint32_t)dx << 16) | ((uint32_t)dy & 0xFFFFu);
    /* The low half stays clear of bit 15, which marks the screen-space kind
     * the draw reads its own halves as (mobydraw.s:4030-4049). */
    s_simple_q[i][2] = ((uint32_t)dz << 16);
    s_simple_q[i][3] = model + (anim & 0xFFu) * 4u;
    s_simple_d[i][0] = dy;
    s_simple_d[i][1] = dz;
    s_simple_d[i][2] = dx;
    s_simple_far[i]  = -1;
    s_simple_qn = i + 1;
    return 1;
}

/** @brief The second kind's pass on its draw hook: capture the cull's queue, run the game's draw, decode the queue natively.
 *
 * The game's own draw writes the drawn verdict and the prims. Before it runs,
 * the pass appends the extended-LOD reveal records to the host queue and
 * snapshots the four flat-material scratchpad words the draw starts from;
 * after it, the queued Mobys are stashed for interpolation and the queue is
 * decoded. Prints a census line under moby_verify_vbl.
 * @param cpu The guest CPU state; the game's draw writes guest memory.
 * @return The number of triangles emitted. */
static int s23_simple_pass(CPUState* cpu) {
    s_simple_recs = s_simple_verts = s_simple_faces = s_simple_tris = 0;
    s_simple_flat = s_simple_screen = s_simple_culled = s_simple_screen_drawn = 0;
    s_simple_lp = s_simple_fade = 0;
    s_flat_zero_colour = s_flat_back_emitted = s_flat_back_dropped = 0;
    s23_simple_capture(cpu);
    /* Extended LOD: the cull's distance box is what keeps a distant moby out
     * of the queue, so the records it rejected are rebuilt here and decoded
     * with the rest. The queue this appends to is the host copy above; the
     * game's own draw below still reads its own, unchanged. */
    int revealed = moby_reveal_candidates(cpu, MOBY_REVEAL_SIMPLE);
    for (int i = 0; i < 4; i++)
        s_flat_entry[i] = cpu->read_word(S23_FLAT_SCRATCH + (uint32_t)i * 4u);
    openpete_recomp_module_get()->dispatch(cpu, game_layout.moby.hook_draw_simple);
    /* The records as the decode below reads them, after the draw's own stores
     * (byte +0x4D) and before any later routine of the tick writes a queued
     * moby (the HUD update advances a HUD moby's angle after this draw), so
     * the object ring's pair takes the drawn records, not the tick-end ones. */
    game_interp_moby_stash_begin();
    for (int i = 0; i < s_simple_qn; i++) game_interp_moby_stash_put(s_simple_q[i][0]);
    int n = s23_simple_decode(cpu);
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-simple vbl=%u] queued=%d drawn=%ld culled=%ld screenkind=%ld "
                        "screendrawn=%ld "
                        "verts=%ld faces=%ld flat=%ld tris=%d revealed=%d lp=%ld fade=%ld "
                        "flat_zero_colour=%ld flat_back_emitted=%ld flat_back_dropped=%ld\n",
                openpete_frame, s_simple_qn, s_simple_recs, s_simple_culled, s_simple_screen,
                s_simple_screen_drawn,
                s_simple_verts, s_simple_faces, s_simple_flat, n, revealed,
                s_simple_lp, s_simple_fade,
                s_flat_zero_colour, s_flat_back_emitted, s_flat_back_dropped);
    return n;
}

/** @brief The animated record's paint-key inputs for moby_rank.h row MOBY_OTK_S23_REGULAR.
 *
 * A = the animation header's byte 3, raised to keyframe B's header byte 3 on
 * a blending record (w0 bits 8-15 non-zero, keyframe B's header a pointer) (mobydraw.s 474-483, 496, 499);
 * the translation the draw loads is the record's own (the cull stored the
 * final one, mobydraw.s 557-560). The face bias is the signed low three bits
 * of the face's second word, decoded into moby_wface_t.otbias. The
 * mirrored-winding material modes and the plane walk (record +0x3C) key the
 * same here.
 * @param cpu The guest CPU state, read only.
 * @param header Keyframe A's AnimationHeader.
 * @param header_b Keyframe B's AnimationHeader.
 * @param w0 The record's flags word.
 * @param TR_raw The record's translation.
 * @param tr Receives the translation the key reads.
 * @param a Receives A.
 * @return MOBY_OTK_S23_REGULAR, or MOBY_OTK_NONE when `header` is not a pointer. */
static moby_otk_row_t s23_otkey_record(CPUState* cpu, uint32_t header, uint32_t header_b,
                                       uint32_t w0, const int TR_raw[3], int tr[3], int* a) {
    if (header < 0x80000000u) return MOBY_OTK_NONE;
    for (int k = 0; k < 3; k++) tr[k] = TR_raw[k];
    int h = cpu->read_byte(header + 3u) & 0xFF;
    if (((w0 >> 8) & 0xFFu) && header_b >= 0x80000000u) {
        int b = cpu->read_byte(header_b + 3u) & 0xFF;
        if (b > h) h = b;
    }
    *a = h;
    return MOBY_OTK_S23_REGULAR;
}

const moby_game_t moby_game = {
    .vstream_init      = s23_vstream_init,
    .vstream_next      = s23_vstream_next,
    .decode_faces      = s23_decode_faces,
    .read_record       = s23_read_record,
    .frame_patch_list  = s23_frame_patch_list,
    .anim_blend_byte   = s23_anim_blend_byte,
    .model_is_animated = s23_model_is_animated,
    .simple_pass       = s23_simple_pass,
    .simple_replay     = s23_simple_replay,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so the
     * shared field reveal stays off (host_reveal below); these two let the
     * shared candidate walk offer the mobys the cull's distance box rejected
     * to this game's own queue builder instead. */
    .cull_box_threshold = s23_cull_box_threshold,
    .reveal_record      = s23_reveal_record,
    .cull_view_admits   = s23_cull_view_admits,
    .special_face      = s23_special_face,
    .otkey_record      = s23_otkey_record,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so
     * nothing the host widens may reach it and the walk feeds the records the
     * cull queued, no more. */
    .host_reveal       = 0,
    /* The LOD/fade byte both model kinds' tiers read, whole
     * (mobydraw.s:453-457 animated, 4193-4196 simple). */
    .renderer_off      = 0x4Eu,
    .renderer_mask     = 0xFFu,
    /* The moby array is fixed with a live sentinel, so a record's Moby address
     * is its slot and is stable across ticks: the walk's default identity. */
    .interp_key        = NULL,
};
