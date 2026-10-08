/* games/spyro-1/renderer/moby_game.c - Spyro 1's half of the shared moby
 * feeder: the two stream decoders, the cull-record reader, the model-kind
 * test, the object identity the interp ring keys on, the classifications the
 * walk consults, and the addresses that live outside game_layout.moby.
 *
 * The walk itself is games/common/renderer/moby.c; the contract is
 * renderer/moby_game.h. */

#include "renderer/moby_game.h"
#include "renderer/interp_regions.h"   /* game_interp_moby_key */
#include "renderer/native_gte.h"       /* ngte_read_matrix */
#include "openpete_vstream.h"          /* frame-word field -> stream address (handles) */
#include "spyro1_symbols.gen.h"
#include "game_layout.h"
#include "openpete_game_structs.h"   /* offsets pinned to the probed MIPS layout */
#include "openpete_interp.h"         /* openpete_interp_present_now */
#include "openpete_interp_objs.h"    /* the g_DragonCutscene tick pair */
#include "openpete_subtick_view.h"   /* openpete_subtick_view_active */
#include "openpete_psx.h"            /* openpete_psx_to_host_span */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** @brief Opens one keyframe's vertex stream.
 *
 * The stream holds packed 11/11/10 literal words interleaved with 5/5/5
 * short deltas, the bit-0 flag of each item selecting the form of the
 * next. The keyframe's first word carries a 21-bit field, a byte address
 * or a stream handle that openpete_vstream_resolve maps to the literal
 * cursor (read through KSEG0); the short cursor starts N words after it,
 * N being the high byte of the keyframe's second word. The model
 * has no static vertex array, so `header` is unused.
 * @param cpu The guest CPU state.
 * @param s The stream state to initialise.
 * @param header The AnimationHeader (unused).
 * @param frame The keyframe's guest address.
 * @param s5 The per-model shift applied to each short delta. */
static void s1_vstream_init(CPUState* cpu, moby_vstream_t* s,
                            uint32_t header, uint32_t frame, int s5) {
    (void)header;
    uint32_t fw0 = cpu->read_word(frame), fw1 = cpu->read_word(frame + 4);
    /* The frame word's 21-bit field is a byte address or a stream handle;
     * the resolver decides (openpete_vstream.h). Stock values come back as
     * the KUSEG byte address, which the game reads through KSEG0. */
    uint32_t va = openpete_vstream_resolve(fw0 & 0x1FFFFFu, 0, &s->base, &s->len);
    s->litp   = va | 0x80000000u;
    s->base_va = s->litp;
    s->shortp = s->litp + ((fw1 >> 24) & 0xFFu) * 4u;
    s->VX = s->VY = s->VZ = s->flag = 0; s->s5 = s5;
}

/** @brief Decodes the next vertex of the stream into s->VX, s->VY and s->VZ.
 *
 * A literal (the first item, or any item after a clear flag) resets the
 * accumulator; a short delta adds to it, shifted left by `s5`.
 * @param cpu The guest CPU state.
 * @param s The stream state.
 * @param first Nonzero to read the stream's first item. */
static void s1_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    if (first || s->flag == 0) {
        uint32_t w = moby_vs_read_word(cpu, s, s->litp); s->litp += 4;
        s->VZ = (int32_t)w >> 21;
        s->VX = (int32_t)(w << 10) >> 21;
        s->VY = (int32_t)(w << 20) >> 19;
        s->flag = (int)(w & 1u);
    } else {
        int16_t d = (int16_t)moby_vs_read_half(cpu, s, s->shortp); s->shortp += 2;
        s->VZ += ((int32_t)d >> 11) << s->s5;                       /* sra11 */
        s->VX -= ((int32_t)((uint32_t)(int32_t)d << 21) >> 27) << s->s5;
        s->VY -= ((int32_t)((uint32_t)(int32_t)d << 26) >> 27) << s->s5;
        s->flag = (int)((uint16_t)d & 1u);
    }
}

/** @brief Decodes a whole animated-model faceblock into emittable faces.
 *
 * The face stream is byte-variable and self-describing. Word 0: sign = fresh (its own indices)
 * against strip (advancing), bit 0 = quad enable / winding, bit 1 = textured,
 * bit 2 = billboard sprite on a fresh face and a special-material flag on a
 * strip face, bit 3 = skip. The record advance follows those flags. Vertex
 * indices are 9-bit fields of word 0 (scaled by 4 as scratchpad byte offsets),
 * the fourth vertex's in the low field of the third word; colour indices are
 * 9-bit fields of word 1 and the high field of the third word. A textured face
 * carries its Tiledef in the three words before the next record. Skip faces
 * and billboard sprites advance the cursor and emit nothing; the walk stops
 * after 8192 records.
 * @param cpu The guest CPU state.
 * @param fb The faceblock's guest address (its length word).
 * @param bytecount The faceblock's length word.
 * @param out Receives up to `cap` faces.
 * @param cap The capacity of `out`.
 * @param st Receives the raw record and sprite counts and the end
 *     alignment: 0 when the cursor stopped at fb + bytecount, 1 at
 *     fb + 4 + bytecount, 2 otherwise.
 * @return The number of faces decoded, at most `cap`. */
static int s1_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                           moby_wface_t* out, int cap, moby_face_stats_t* st) {
    uint32_t end = fb + 4u + bytecount;   /* end_hi (generous; alignment classified below) */
    uint32_t fp  = fb + 4u;
    int n = 0, raw = 0, sprites = 0, skips = 0, guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        raw++;
        int fresh    = ((int32_t)w0) < 0;
        int textured = (w0 & 0x2u) != 0;
        int sprite   = fresh && (w0 & 0x4u);
        int skip     = (w0 & 0x8u) != 0;
        uint32_t adv = sprite ? 0x14u
                     : fresh  ? (textured ? 0x18u : 0x0Cu)
                     :          (textured ? 0x14u : 0x08u);   /* strip */
        if (sprite) sprites++;
        skips += skip;
        if (skip || sprite) { fp += adv; continue; }   /* sprite (billboard FT4, .L8002256C) TODO; advance to stay aligned */
        if (n < cap) {
            moby_wface_t* F = &out[n];
            F->i0 = (uint16_t)(((w0 >> 20) & 0x7FCu) >> 2);
            F->i1 = (uint16_t)(((w0 >> 11) & 0x7FCu) >> 2);
            F->i2 = (uint16_t)(((w0 >>  2) & 0x7FCu) >> 2);
            uint32_t w1  = cpu->read_word(fp + 4u);
            uint32_t v4w = cpu->read_word(fp + 8u);   /* 4th-vertex SXY idx low field, colour idx >>9 */
            F->i3 = (uint16_t)((v4w & 0x7FCu) >> 2);
            F->c0 = (uint16_t)((w1 >> 17) & 0x7FCu);
            F->c1 = (uint16_t)((w1 >>  8) & 0x7FCu);
            F->c2 = (uint16_t)((w1 <<  1) & 0x7FCu);
            F->c3 = (uint16_t)((v4w >> 9) & 0x7FCu);
            F->fresh = (uint8_t)(fresh ? 1 : 0); F->bit0 = (uint8_t)(w0 & 1u);
            F->textured = (uint8_t)(textured ? 1 : 0);
            F->hl = (uint8_t)((!fresh && (w0 & 0x4u)) ? 1 : 0);
            F->blendbit = (uint8_t)(w1 & 1u);
            F->otbias = (int8_t)((int32_t)w1 >> 28);   /* the walk adds 2*bias local buckets */
            F->tpx = F->tpy = F->clx = F->cly = 0; F->bpp = 16; F->abr = 0;
            memset(F->u, 0, 4); memset(F->v, 0, 4);
            if (textured) {
                uint32_t td0 = cpu->read_word(fp + adv - 0xCu);
                uint32_t td1 = cpu->read_word(fp + adv - 0x8u);
                uint32_t td2 = cpu->read_word(fp + adv - 0x4u);
                F->u[0]=(uint8_t)(td0&0xFFu); F->v[0]=(uint8_t)((td0>>8)&0xFFu);  unsigned cl=(td0>>16)&0xFFFFu;
                F->u[1]=(uint8_t)(td1&0xFFu); F->v[1]=(uint8_t)((td1>>8)&0xFFu);  unsigned tp=(td1>>16)&0xFFFFu;
                F->u[2]=(uint8_t)(td2&0xFFu); F->v[2]=(uint8_t)((td2>>8)&0xFFu);
                F->u[3]=(uint8_t)((td2>>16)&0xFFu); F->v[3]=(uint8_t)((td2>>24)&0xFFu);   /* quad u3v3 */
                /* NGTE_TP_REPLACED_ID: the PSX-reserved tpage mode names a
                 * mod-texture material (api->texture_register) whose id the
                 * CLUT u16 carries; native-only (og shows garbage). */
                ngte_tpage t;
                ngte_decode_tpage(tp, cl, NGTE_TP_REPLACED_ID, &t);
                F->abr=(uint8_t)t.abr;   /* texpage semi-transparency mode (ABR 0-3) */
                F->tpx=(int16_t)t.tpx; F->tpy=(int16_t)t.tpy;
                F->bpp=(uint8_t)t.bpp;
                F->clx=(int16_t)t.clx; F->cly=(int16_t)t.cly;
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = sprites; st->skips = skips;
    st->align = (fp == fb + bytecount) ? 0 : (fp == end) ? 1 : 2;
    return n;
}

/** @brief Reads one moby cull record from the render scratch.
 *
 * Stride 0x38 in the render scratch: flags word, AnimationHeader*, the two
 * keyframes, the view-space translation, the 3x3 model->view matrix (whose
 * unused high half of +0x2C carries the LOD verdict), the specular colour
 * word the specular cull copies, and the Moby*.
 *
 * A zero flags word terminates the queue: both cull builders close the queue
 * by storing zero into word 0 alone and leave every other word of that record
 * as the previous tick left it (r_moby.s:8001F744, 80020EE0), and the draw
 * stops on exactly that word (r_moby.s:8001F7FC-8001F804). Testing any other
 * word runs the walk past the terminator into stale bytes, whose header can
 * pass the decode guard.
 * @param cpu The guest CPU state.
 * @param rec The record's guest address.
 * @param R Receives the record's fields; one header serves both keyframes.
 * @return 1 for a record, 0 at the terminator. */
static int s1_read_record(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    const uint32_t w0 = cpu->read_word(rec);
    if (w0 == 0) return 0;
    const uint32_t hdr  = cpu->read_word(rec + game_layout.moby.rec_header_off);
    const uint32_t moby = cpu->read_word(rec + game_layout.moby.rec_moby_off);
    R->header   = hdr;
    R->header_b = hdr;   /* one header serves both keyframes on this game */
    R->frame_a  = cpu->read_word(rec + game_layout.moby.rec_frame_a_off);
    R->frame_b  = cpu->read_word(rec + game_layout.moby.rec_frame_b_off);
    R->w0       = w0;
    R->moby     = moby;
    R->material = cpu->read_word(rec + game_layout.moby.rec_material_off);
    for (int i = 0; i < 3; i++)
        R->TR_raw[i] = (int32_t)cpu->read_word(rec + game_layout.moby.rec_tr_off + (uint32_t)i * 4u);
    ngte_read_matrix(cpu, rec + game_layout.moby.rec_matrix_off, R->M);
    R->tier = (int)(int16_t)(cpu->read_word(rec + game_layout.moby.rec_tier_off) >> 16);
    return 1;
}

/** @brief Reports whether a model-table entry is an animated model.
 *
 * A negative animation count (the entry's word 0) marks the second model
 * kind (SimpleModel), which the shaded pass draws from its own Moby* list.
 * @param cpu The guest CPU state.
 * @param model The model's guest address.
 * @return 1 for an animated model, 0 for a SimpleModel. */
static int s1_model_is_animated(CPUState* cpu, uint32_t model) {
    return (int32_t)cpu->read_word(model + 0x00u) >= 0;
}

/* ---- the cutscene actor set ----------------------------------------
 * g_DragonCutscene holds the rescued dragon and the cutscene Spyro/dragon
 * mobys. These are EXACTLY what func_8001CFDC draws during states 1..6, so
 * matching a moby pointer against them is the game's own selector - no class
 * or heuristic. m_CutsceneSpyro @+0x88, m_CutsceneDragon @+0x8C,
 * m_RescuedDragonMoby @+0x90, anchored off m_State @D_80077058 == base+0x28. */
#define G_DRAGONCUTSCENE_CUTSPYRO   0x800770B8u
#define G_DRAGONCUTSCENE_CUTDRAGON  0x800770BCu
#define G_DRAGONCUTSCENE_RESCUED    0x800770C0u
#define G_DRAGONCUTSCENE_STATE      0x80077058u
#define G_GAMESTATE                 0x800757D8u

/** @brief Reports whether a moby is one of the three mobys func_8001CFDC draws in the dragon cutscene.
 *
 * Compares mob against g_DragonCutscene's m_RescuedDragonMoby, m_CutsceneDragon
 * and m_CutsceneSpyro words. The moby feeder routes a match to the front
 * pass; the caller has already rejected addresses below KSEG0.
 * @param cpu The guest CPU state.
 * @param mob The moby's guest address.
 * @return 1 when mob equals one of the three words, else 0. */
static int s1_cutscene_actor(CPUState* cpu, uint32_t mob) {
    return mob == cpu->read_word(G_DRAGONCUTSCENE_RESCUED) ||
           mob == cpu->read_word(G_DRAGONCUTSCENE_CUTDRAGON) ||
           mob == cpu->read_word(G_DRAGONCUTSCENE_CUTSPYRO);
}

/** @brief Reports whether the dragon cutscene renderer owns the actor set's draw this tick.
 *
 * func_8001CFDC owns it while g_Gamestate is GS_Dragon (8) and
 * g_DragonCutscene.m_State is 1..6. State 0 is the ordinary full draw; past
 * state 6 the full level moby set is re-queued, so the actors are no longer
 * drawn by a producer of their own.
 * @param cpu The guest CPU state.
 * @return 1 while the cutscene renderer owns the draw, else 0. */
static int s1_cutscene_owns_draw(CPUState* cpu) {
    uint32_t st = cpu->read_word(G_DRAGONCUTSCENE_STATE);
    return cpu->read_word(G_GAMESTATE) == 8u && st >= 1u && st <= 6u;
}

/** @brief The rescue cutscene's departure factors for record 0, as func_8001CFDC applies them in m_State 5.
 *
 * In m_State 5 (draw.c state 5) row 0 is scaled by D_8006F3C0[m_CutsceneTicks],
 * row 1 by Cos((m_CutsceneTicks - 8) * 42) once the ticks pass 8, and row 2 not
 * at all; every other state scales nothing. `cs` is g_DragonCutscene from
 * m_State (+0x28), so m_CutsceneTicks is at +0x14. Cos is the game's
 * 4096-per-turn angle over the 256-entry table with its 16-step linear
 * interpolation, taken in double; the factors are Q12 values over 4096 with no
 * FIXED_MUL truncation.
 * @param cpu guest CPU state (reads D_8006F3C0 and the sine table)
 * @param cs  g_DragonCutscene bytes from m_State (+0x28), live or captured
 * @param f   receives the three row factors, 1.0 for an unscaled row
 * @return 1 in state 5, else 0 with every factor 1.0 */
static int s1_departure_factors(CPUState* cpu, const uint8_t* cs, double f[3]) {
    uint32_t st, ticks;
    memcpy(&st, cs, 4);
    memcpy(&ticks, cs + 0x14, 4);
    f[0] = f[1] = f[2] = 1.0;
    if (st != 5u) return 0;
    f[0] = (double)(int16_t)cpu->read_half(D_DRAGON_SHRINK + ticks * 2u) / 4096.0;
    if ((int32_t)ticks > 8) {
        double s, c;
        ngte_sincos_f(cpu, (double)(((ticks - 8u) * 42u) & 0xFFFu) / 16.0, &s, &c);
        f[1] = c / 4096.0;
    }
    return 1;
}

/** @brief Record 0's draw-time row scale in the rescue cutscene's departure (g_Gamestate 8, m_State 5).
 *
 * func_8001CFDC scales rows 0 and 1 of the first cull record (the queue holds
 * m_CutsceneSpyro alone) after func_8001F158 builds it. A canonical present
 * takes the live g_DragonCutscene; a sub-tick present lerps each factor between
 * the captured tick pair at the pair's alpha clamped to [0, 1], a factor of
 * the previous tick outside state 5 being 1. Without a captured pair the live
 * factors stand.
 * @param cpu   guest CPU state
 * @param rec   the cull record's guest address
 * @param scale receives the row factors
 * @return 1 when the draw scales `rec` (the first record, g_Gamestate 8,
 *         m_State 5), else 0 */
static int s1_record_row_scale(CPUState* cpu, uint32_t rec, double scale[3]) {
    if (rec != game_layout.moby.rec_base || cpu->read_word(G_GAMESTATE) != 8u) return 0;
    const uint8_t* live = (const uint8_t*)openpete_psx_to_host_span(G_DRAGONCUTSCENE_STATE, 0x18u);
    if (!live || !s1_departure_factors(cpu, live, scale)) return 0;
    if (!openpete_subtick_view_active()) return 1;
    const openpete_interp_obj_t* o = openpete_interp_objs_find(G_DRAGONCUTSCENE_STATE);
    if (!o || !o->have_prev || o->curr_ns <= o->prev_ns) return 1;
    double fp[3], fc[3];
    s1_departure_factors(cpu, o->prev, fp);
    if (!s1_departure_factors(cpu, o->curr, fc)) return 1;
    double a = (double)(openpete_interp_present_now() - o->curr_ns) / (double)(o->curr_ns - o->prev_ns);
    a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
    for (int r = 0; r < 3; r++) scale[r] = fp[r] + (fc[r] - fp[r]) * a;
    return 1;
}

extern int spyro_cutpath_jump(uint32_t m, const uint8_t* prev_rec, const uint8_t* curr_rec);
extern int spyro_moby_cut_held(uint32_t moby);

/** @brief Reports whether flat HUD and text draw under the player model this tick.
 *
 * They draw over the player by default and under it only during the "THE
 * ADVENTURE BEGINS" reveal, where Spyro flies in front of the text being
 * revealed, which runs under GS_TitleScreen (13). GS_LevelTransition (1)
 * is excluded: the "ENTERING" level-name transition has no Spyro reveal, so
 * the text stays on top there.
 * @param cpu The guest CPU state.
 * @return 1 under GS_TitleScreen, else 0. */
static int s1_hud_under_player(CPUState* cpu) {
    return cpu->read_word(G_GAMESTATE) == 13u;
}

/** @brief Reports whether a moby is a run-once instance the walk and the reveal must not draw.
 *
 * Instances whose first update does a one-shot job and then retires them
 * (m_State >= 0x80). The update-list builder (80052040) auto-includes any
 * moby whose m_WasDrawn is set, ahead of its m_UpdateDistance path, so
 * canonically "drawn" implies "updated next tick" and such an instance is on
 * screen for at most the single tick that admits it - never long enough to be
 * intended as scenery.
 *
 * Two display bugs follow. The extended-LOD reveal must not write m_WasDrawn
 * (gameplay-readable), so a revealed instance is drawn without ever entering
 * the update list: it never runs, never retires, and stands there until the
 * draw range finally reaches it and kills it. And even in range, the canonical
 * queue still yields that one-tick flash. Both are display-side, so the fix is
 * too: the walk emits no triangles for these and the reveal never admits them,
 * while the cull, m_WasDrawn, the update list and the moby's one-shot job all
 * proceed untouched.
 *
 * Identity is the level author's own spawn parameter: m_Props (+0x00) points
 * at a per-instance block whose word at +0x04 selects the class's behaviour
 * variant. Jacques' fools ship four instances at variant 6 - the ones that
 * only set a moving platform's height - against eight at variants 0/1/2, the
 * ordinary hopping fools. Nothing in the Moby record itself distinguishes
 * them: an ordinary fool sits at m_State 0 with a zero rotation matrix and
 * animation frame 0 exactly like an unreached run-once one, and stays at
 * m_State 0 after being drawn, so a liveness test would hide every distant
 * fool in the level. The table is keyed per level because class ids index the
 * level overlay's own g_Models, and the props layout is class-specific.
 * @param cpu The guest CPU state.
 * @param m The moby's guest address.
 * @param cls The moby's class.
 * @return 1 for a run-once instance, else 0. */
static int s1_run_once_hidden(CPUState* cpu, uint32_t m, unsigned cls) {
    static const struct { uint16_t level, cls; uint32_t variant; } k_run_once[] = {
        /* jacques: the fools that set the moving platforms' heights */
        { 54u, 0x7Eu, 6u },
    };
    for (size_t i = 0; i < sizeof k_run_once / sizeof k_run_once[0]; i++) {
        if (k_run_once[i].cls != cls) continue;
        if (cpu->read_word(G_LEVELID) != k_run_once[i].level) continue;
        uint32_t props = cpu->read_word(m + offsetof(Moby, m_Props));
        if (props < 0x80000000u || props >= 0x80200000u) continue;
        if (cpu->read_word(props + 0x04u) == k_run_once[i].variant) return 1;
    }
    return 0;
}

/** @brief Decodes an animated record's paint-key fields for the MOBY_OTK_S1_REGULAR row.
 *
 * The moby_rank.h rows MOBY_OTK_S1_REGULAR, MOBY_OTK_S1_SPECULAR and
 * MOBY_OTK_S1_SHADED are decoded here and in s1_shaded_otkey_face.
 * The regular and specular walks' record: both draws read a1 from w0 bits
 * 24-28 [r_moby.s 476-479, 2114-2117], where the cull packs the animation
 * header's byte 0xB, raised to the next animation's header byte 0xB on a
 * blending record (the larger of the two) [r_moby.s 210-225, 229-240]. A
 * queued record carries that field but names one header for both keyframes;
 * a record built from gamestate carries the two headers and a zero field, so
 * a1 is the largest of the field and the header bytes the cull compares.
 * The translation the walk loads is (TR << 2) >> m_Scale, or on a record with
 * a GPF scale byte (w0 & 0xFF) (byte * int16((TR << 2) >> m_Scale)) >> 5
 * [r_moby.s 506-517]. The face bias is the signed top nibble of the
 * colour-index word, decoded into moby_wface_t.otbias (r_moby.s 1018, 1141).
 * @param cpu The guest CPU state.
 * @param header The record's AnimationHeader.
 * @param header_b The next animation's header on a blending record.
 * @param w0 The record's flags word.
 * @param TR_raw The record's view-space translation.
 * @param tr Receives the translation the draw loads into TRX/TRY/TRZ.
 * @param a Receives a1, the 5-bit header byte the row scales by.
 * @return MOBY_OTK_S1_REGULAR. */
static moby_otk_row_t s1_otkey_record(CPUState* cpu, uint32_t header, uint32_t header_b,
                                      uint32_t w0, const int TR_raw[3], int tr[3], int* a) {
    int ms = cpu->read_byte(header + game_layout.moby.anim_scale_off) & 31;
    int gpf = (int)(w0 & 0xFFu);
    for (int k = 0; k < 3; k++)
        tr[k] = gpf ? (gpf * (int)(int16_t)((TR_raw[k] << 2) >> ms)) >> 5
                    : (TR_raw[k] << 2) >> ms;
    int a1 = (int)((w0 >> 24) & 0x1Fu);
    int ha = cpu->read_byte(header + 0xBu) & 0xFF;
    if (ha > a1) a1 = ha;
    if (((w0 >> 8) & 0xFFu) && header_b >= 0x80000000u) {
        int hb = cpu->read_byte(header_b + 0xBu) & 0xFF;
        if (hb > a1) a1 = hb;
    }
    *a = a1 & 0x1F;
    return MOBY_OTK_S1_REGULAR;
}

/** @brief Decodes one shaded-walk face's two words into its paint-key fields.
 *
 * Gouraud faces (word 0 bit 0 clear) carry the bias in word 1's low two
 * bits (r_moby.s 4619-4620, 4679-4680); flat and metal faces carry a face
 * normal there and take bias 0. A metal face (word 0 bits 0 and 1) keeps
 * both facings while TRZ < 0x800 (r_moby.s 4841-4847) and takes the
 * back-face offset (r_moby.s 4894-4899).
 * @param w0 The face's first word.
 * @param w1 The face's second word.
 * @param f Receives the bias, back-face and near-keep fields. */
static void s1_shaded_otkey_face(uint32_t w0, uint32_t w1, moby_otk_face_t* f) {
    int metal = (w0 & 3u) == 3u;
    f->bias = (w0 & 1u) ? 0 : (int)(w1 & 3u);
    f->back_adds = (int8_t)metal;
    f->near_limit = metal ? 0x800 : INT32_MIN;
}

/** @brief Returns the faceblock patch list a keyframe carries.
 *
 * The low halfword of the keyframe's second word is the list's byte offset
 * from the keyframe, divided by four; zero means none. The list lives
 * inside the keyframe, so `rec` and `model` are unused.
 * @param cpu The guest CPU state.
 * @param rec The cull record (unused).
 * @param model The moby's model (unused).
 * @param frame_a The keyframe's guest address.
 * @return The list's guest address, or 0 when the keyframe carries none. */
static uint32_t s1_frame_patch_list(CPUState* cpu, uint32_t rec, uint32_t model,
                                    uint32_t frame_a) {
    (void)rec; (void)model;   /* the list is inside the keyframe, not the record */
    uint32_t off = (cpu->read_word(frame_a + 4u) & 0xFFFFu) << 2;
    return off ? frame_a + off : 0u;
}

/** @brief Returns how many keyframes an AnimationHeader's keyframe array holds.
 *
 * Reads m_NumFrames; the array at m_Frames holds that many 8-byte records.
 * @param cpu The guest CPU state.
 * @param header The AnimationHeader's guest address.
 * @return The keyframe count. */
static int s1_anim_frame_count(CPUState* cpu, uint32_t header) {
    return (int)(int16_t)(uint16_t)cpu->read_half(header + offsetof(AnimationHeader, m_NumFrames));
}

const moby_game_t moby_game = {
    .vstream_init      = s1_vstream_init,
    .vstream_next      = s1_vstream_next,
    .anim_frame_count  = s1_anim_frame_count,
    .decode_faces      = s1_decode_faces,
    .read_record       = s1_read_record,
    .model_is_animated = s1_model_is_animated,
    .simple_pass       = NULL,   /* the second model kind has its own draw hook */
    /* The keyframe patch list lives inside the keyframe: its low halfword,
     * scaled by four, is the list's byte offset from the keyframe itself. */
    .frame_patch_list  = s1_frame_patch_list,
    /* The record fields rebuild from gamestate here, which is what lets the
     * walk feed mobys the 4:3 cull skipped and lets a sub-tick present
     * re-derive each record against the interpolated camera. */
    .host_reveal       = 1,
    .interp_key        = game_interp_moby_key,
    .otkey_record        = s1_otkey_record,
    .specular_otkey_row  = MOBY_OTK_S1_SPECULAR,
    .shaded_otkey_row    = MOBY_OTK_S1_SHADED,
    .shaded_otkey_face   = s1_shaded_otkey_face,

    .cutscene_actor     = s1_cutscene_actor,
    .cutscene_owns_draw = s1_cutscene_owns_draw,
    .cutpath_jump       = spyro_cutpath_jump,
    .moby_held          = spyro_moby_cut_held,
    .run_once_hidden    = s1_run_once_hidden,
    .hud_under_player   = s1_hud_under_player,
    .record_row_scale   = s1_record_row_scale,

    .scratch         = G_SCRATCH,
    .colour_scratch  = G_SCRATCH + 0x1100u,  /* per-vertex depth-cued RGB (.s DPCS output) */
    .shaded_list     = G_SCRATCH + 0x2400u,  /* shaded renderer: flat Moby* array (stride 4) */
    .shaded_colour   = G_SCRATCH + 0x2A00u,  /* shaded per-vertex lit colours (block_80023258) */
    /* The specular pass rebuilds the record queue with the specular set -
     * including specular-only mobys the regular pass did not draw - and
     * overwrites the colour scratch, so the regular snapshot cannot reproduce
     * them. The window is the colour rows [0x1100,0x1600) plus the FULL record
     * region [0x1600,0x2400): the records are stride 0x38, so record 14 spills
     * past 0x1900 and a shorter window silently truncates the replayed queue. */
    .spec_window_off = 0x1100u,
    .spec_window_len = 0x1300u,
    .skip_lowpoly    = G_SKIP_LOWPOLY_WORLD,
    .player_pos      = G_SPYRO,
    .spec_light      = D_SPEC_LIGHT,
    .metal_tiledef   = D_METAL_TILEDEF,

    .props_off         = offsetof(Moby, m_Props),
    .renderer_off      = offsetof(Moby, m_Renderer),   /* low 6 bits = the LOD band */
    .renderer_mask     = 0x3Fu,
    /* m_ShinyRenderer. moby_lists.s func_800521C0 loads the word at m_State
     * and sorts the moby on the high byte, m_Renderer: zero to the shaded
     * list at scratch + 0x2400 (@80052278), bit 7 set to the specular list at
     * scratch + 0x2200 (@8005224C), otherwise to the regular list at scratch
     * + 0 (@80052220). */
    .renderer_shiny_mask = 0x80u,
    .spec_colour_off   = offsetof(Moby, m_SpecularMetalColor),
    .render_radius_off = offsetof(Moby, m_RenderRadius),
    .update_dist_off   = offsetof(Moby, m_UpdateDistance),
    .substate_off      = offsetof(Moby, m_Substate),
    .cam_heading       = G_CAMERA + 0x50u,
    .shade_coltab      = D_SHADE_COLTAB,
    .metal_coltab      = D_METAL_COLTAB,
};
