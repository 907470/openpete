/* games/spyro-1/renderer/moby_game.c — Spyro 1's half of the shared moby
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

#include <stddef.h>
#include <stdlib.h>

/* ---- vertex stream -------------------------------------------------
 * Packed 11/11/10 literals interleaved with 5/5/5 short-deltas, the bit-0
 * flag of each item selecting the form of the next. A literal RESETS the
 * accumulator; a short ADDS. The literal and short cursors run from the same
 * base, the shorts starting one word per the frame's second word high byte.
 * The model has no static vertex array, so `header` is unused here. */
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

/* ---- face stream ---------------------------------------------------
 * Byte-variable and self-describing. Word 0: sign = fresh (its own indices)
 * against strip (advancing), bit 0 = quad enable / winding, bit 1 = textured,
 * bit 2 = billboard sprite on a fresh face and a special-material flag on a
 * strip face, bit 3 = skip. The record advance follows those flags. Vertex
 * indices are 9-bit fields of word 0 (scaled by 4 as scratchpad byte offsets),
 * the fourth vertex's in the low field of the third word; colour indices are
 * 9-bit fields of word 1 and the high field of the third word. A textured face
 * carries its Tiledef in the three words before the next record. */
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
            F->blendbit = (uint8_t)(w1 & 1u); F->pad0 = 0;
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
                int mode=(int)((tp>>7)&3u);
                F->abr=(uint8_t)((tp>>5)&3u);   /* texpage semi-transparency mode (ABR 0-3) */
                if (mode == 3) {
                    /* PSX-RESERVED tpage mode = mod-texture material
                     * (api->texture_register): the CLUT u16 carries the
                     * registered id; native-only (og shows garbage). */
                    F->bpp = 32;
                    F->tpx = (int16_t)cl;   /* the mod texture id */
                    F->tpy = 0; F->clx = 0; F->cly = 0;
                } else {
                    F->tpx=(int16_t)((tp&0xFu)*64); F->tpy=(int16_t)(((tp>>4)&1u)*256);
                    F->bpp=(uint8_t)((mode==0)?4:(mode==1)?8:16);
                    F->clx=(int16_t)((cl&0x3Fu)*16); F->cly=(int16_t)((cl>>6)&0x1FFu);
                }
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = sprites; st->skips = skips;
    st->align = (fp == fb + bytecount) ? 0 : (fp == end) ? 1 : 2;
    return n;
}

/* ---- cull record ---------------------------------------------------
 * Stride 0x38 in the render scratch: flags word, AnimationHeader*, the two
 * keyframes, the view-space translation, the 3x3 model->view matrix (whose
 * unused high half of +0x2C carries the LOD verdict), the specular colour
 * word the specular cull copies, and the Moby*. A record whose header and
 * Moby* are both zero terminates the queue. */
static int s1_read_record(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    const uint32_t hdr  = cpu->read_word(rec + game_layout.moby.rec_header_off);
    const uint32_t moby = cpu->read_word(rec + game_layout.moby.rec_moby_off);
    if (hdr == 0 && moby == 0) return 0;
    R->header   = hdr;
    R->frame_a  = cpu->read_word(rec + game_layout.moby.rec_frame_a_off);
    R->frame_b  = cpu->read_word(rec + game_layout.moby.rec_frame_b_off);
    R->w0       = cpu->read_word(rec);
    R->moby     = moby;
    R->material = cpu->read_word(rec + game_layout.moby.rec_material_off);
    for (int i = 0; i < 3; i++)
        R->TR_raw[i] = (int32_t)cpu->read_word(rec + game_layout.moby.rec_tr_off + (uint32_t)i * 4u);
    ngte_read_matrix(cpu, rec + game_layout.moby.rec_matrix_off, R->M);
    R->tier = (int)(int16_t)(cpu->read_word(rec + game_layout.moby.rec_tier_off) >> 16);
    return 1;
}

/* A negative animation count marks the second model kind (SimpleModel), which
 * the shaded pass draws from its own Moby* list. */
static int s1_model_is_animated(CPUState* cpu, uint32_t model) {
    return (int32_t)cpu->read_word(model + 0x00u) >= 0;
}

/* ---- the cutscene actor set ----------------------------------------
 * g_DragonCutscene holds the rescued dragon and the cutscene Spyro/dragon
 * mobys. These are EXACTLY what func_8001CFDC draws during states 1..6, so
 * matching a moby pointer against them is the game's own selector — no class
 * or heuristic. m_CutsceneSpyro @+0x88, m_CutsceneDragon @+0x8C,
 * m_RescuedDragonMoby @+0x90, anchored off m_State @D_80077058 == base+0x28. */
#define G_DRAGONCUTSCENE_CUTSPYRO   0x800770B8u
#define G_DRAGONCUTSCENE_CUTDRAGON  0x800770BCu
#define G_DRAGONCUTSCENE_RESCUED    0x800770C0u
#define G_DRAGONCUTSCENE_STATE      0x80077058u
#define G_GAMESTATE                 0x800757D8u

static int s1_cutscene_actor(CPUState* cpu, uint32_t mob) {
    return mob == cpu->read_word(G_DRAGONCUTSCENE_RESCUED) ||
           mob == cpu->read_word(G_DRAGONCUTSCENE_CUTDRAGON) ||
           mob == cpu->read_word(G_DRAGONCUTSCENE_CUTSPYRO);
}

/* The cutscene renderer (func_8001CFDC) owns the actor set above while
 * g_Gamestate is GS_Dragon and m_State is 1..6. State 0 is the ordinary full
 * draw; past state 6 the full level moby set is re-queued, so the actors are
 * no longer drawn by a producer of their own. */
static int s1_cutscene_owns_draw(CPUState* cpu) {
    uint32_t st = cpu->read_word(G_DRAGONCUTSCENE_STATE);
    return cpu->read_word(G_GAMESTATE) == 8u && st >= 1u && st <= 6u;
}

extern int spyro_cutpath_is_actor(uint32_t m);
static int s1_cutpath_actor(uint32_t m) { return spyro_cutpath_is_actor(m); }

/* Flat HUD and text draw OVER the player by default; UNDER it only during the
 * "THE ADVENTURE BEGINS" reveal where Spyro flies in front of the text being
 * revealed — that runs under GS_TitleScreen (13). NOT GS_LevelTransition (1):
 * the "ENTERING <level>" portal transition has no Spyro reveal, so text stays
 * on top there like every other state. */
static int s1_hud_under_player(CPUState* cpu) {
    return cpu->read_word(G_GAMESTATE) == 13u;
}

/* ---- run-once instances --------------------------------------------
 * Instances whose FIRST update does a one-shot job and then retires them
 * (m_State >= 0x80). The update-list builder (@80052040) auto-includes any
 * moby whose m_WasDrawn is set, ahead of its m_UpdateDistance path, so
 * canonically "drawn" implies "updated next tick" and such an instance is on
 * screen for at most the single tick that admits it — never long enough to be
 * intended as scenery.
 *
 * Two display bugs follow. The extended-LOD reveal must not write m_WasDrawn
 * (gameplay-readable), so a revealed instance is drawn WITHOUT ever entering
 * the update list: it never runs, never retires, and stands there until the
 * draw range finally reaches it and kills it. And even in range, the canonical
 * queue still yields that one-tick flash. Both are display-side, so the fix is
 * too: the walk emits no triangles for these and the reveal never admits them,
 * while the cull, m_WasDrawn, the update list and the moby's one-shot job all
 * proceed untouched.
 *
 * Identity is the level author's own spawn parameter: m_Props (+0x00) points
 * at a per-instance block whose word at +0x04 selects the class's behaviour
 * variant. Jacques' fools ship four instances at variant 6 — the ones that
 * only set a moving platform's height — against eight at variants 0/1/2, the
 * ordinary hopping fools. Nothing in the Moby record itself distinguishes
 * them: an ordinary fool sits at m_State 0 with a zero rotation matrix and
 * animation frame 0 exactly like an unreached run-once one, and STAYS at
 * m_State 0 after being drawn, so a liveness test would hide every distant
 * fool in the level. The table is keyed per level because class ids index the
 * level overlay's own g_Models, and the props layout is class-specific. */
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

const moby_game_t moby_game = {
    .vstream_init      = s1_vstream_init,
    .vstream_next      = s1_vstream_next,
    .decode_faces      = s1_decode_faces,
    .read_record       = s1_read_record,
    .model_is_animated = s1_model_is_animated,
    .simple_pass       = NULL,   /* the second model kind has its own draw hook */
    .interp_key        = game_interp_moby_key,

    .cutscene_actor     = s1_cutscene_actor,
    .cutscene_owns_draw = s1_cutscene_owns_draw,
    .cutpath_actor      = s1_cutpath_actor,
    .run_once_hidden    = s1_run_once_hidden,
    .hud_under_player   = s1_hud_under_player,

    .scratch         = G_SCRATCH,
    .colour_scratch  = G_SCRATCH + 0x1100u,  /* per-vertex depth-cued RGB (.s DPCS output) */
    .shaded_list     = G_SCRATCH + 0x2400u,  /* shaded renderer: flat Moby* array (stride 4) */
    .shaded_colour   = G_SCRATCH + 0x2A00u,  /* shaded per-vertex lit colours (block_80023258) */
    /* The specular pass rebuilds the record queue with the specular set —
     * including specular-only mobys the regular pass did not draw — and
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
    .spec_colour_off   = offsetof(Moby, m_SpecularMetalColor),
    .render_radius_off = offsetof(Moby, m_RenderRadius),
    .update_dist_off   = offsetof(Moby, m_UpdateDistance),
    .substate_off      = offsetof(Moby, m_Substate),
    .cam_heading       = G_CAMERA + 0x50u,
    .shade_coltab      = D_SHADE_COLTAB,
    .metal_coltab      = D_METAL_COLTAB,
};
