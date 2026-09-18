// games/common/renderer/text_sprt.c — native renderer feed for the SPRT text
// chain.
//
// Most of a game's 2D layer reaches the shared feeder through the
// ordering-table chokepoint (ui_game.ot_insert). The glyph emitters do not:
// each builds its own tag-to-tag chain of one DR_MODE plus one 0x14-byte SPRT
// per glyph in the primitive buffer, then links the whole chain into the 2D
// ordering table with an inlined insert, so the chokepoint never sees a glyph.
//
// Reproducing the glyph walk would mean re-implementing the emitters' escape
// handling, per-colour line heights and clip range. Instead this module runs
// the real emitter and reads the chain it just left in the primitive buffer:
// the packets are final when the emitter returns, so one linear walk over the
// bytes the primitive cursor advanced past yields every glyph exactly as the
// GPU will see it. An SPRT carries no texture page, so the walk tracks the
// DR_MODE that precedes it (the per-glyph-DR_MODE variant rewrites that page
// inside every packet, which the same tracking picks up).
//
// Display-only: the hook reads the primitive buffer and the real emitter runs
// unchanged, so no gameplay-readable byte moves.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_recomp_module.h"
#include "game_layout.h"
#include "renderer/ui_game.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Chains walked and glyphs fed since boot: the engagement counters a capture
 * reads to tell "no text drew" from "text drew identically". */
static unsigned s_chains, s_glyphs;

static int census_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TEXT_SPRT_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/* Walk [start, end) as a run of contiguous primitive packets. A packet's tag
 * word holds its length in the top byte, so the next packet starts
 * (len + 1) * 4 bytes on; the command byte is the top byte of word 1. */
static void feed_chain(CPUState* cpu, uint32_t start, uint32_t end) {
    unsigned tpage = 0;                 /* set by the chain's own DR_MODE */
    int have_tpage = 0;
    uint32_t p = start;
    while (p + 8u <= end) {
        uint32_t w0 = cpu->read_word(p);
        unsigned len = (w0 >> 24) & 0xFFu;
        if (len == 0u) break;           /* not a tagged packet: stop rather than drift */
        uint32_t size = (len + 1u) * 4u;
        if (p + size > end) break;
        unsigned code = (cpu->read_word(p + 4) >> 24) & 0xFFu;
        if (code == 0xE1u) {            /* DR_MODE: bits 0-10 are the draw mode */
            tpage = cpu->read_word(p + 4) & 0x7FFu;
            have_tpage = 1;
        } else if ((code & 0xFCu) == 0x64u && have_tpage) {   /* SPRT 0x64..0x67 */
            ui_sprite_feed_sprt(cpu, p, tpage);
            s_glyphs++;
        }
        p += size;
    }
    s_chains++;
}

static int on_text_emitter(CPUState* cpu, uint32_t addr) {
    if (!openpete_native_scene_enabled()) return 0;   /* inert: normal dispatch runs it */
    static int s_in = 0;
    if (s_in) return 0;                               /* re-entry from our own call */
    s_in = 1;

    uint32_t start = cpu->read_word(game_layout.polybuf.cursor);
    openpete_recomp_module_get()->dispatch(cpu, addr); /* the real emitter (ref unchanged) */
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);

    if (end > start) feed_chain(cpu, start, end);

    if (census_on()) {
        static unsigned last = 0xFFFFFFFFu;
        extern unsigned openpete_frame;
        if (openpete_frame / 100u != last) {
            last = openpete_frame / 100u;
            fprintf(stderr, "[census f=%u TEXT-SPRT] chains=%u glyphs=%u bytes=%u\n",
                    openpete_frame, s_chains, s_glyphs, end - start);
        }
    }
    s_in = 0;
    return 1;
}

/* The hook addresses are the descriptor's, bound at boot: a static
 * initializer cannot read another TU's const. Each entry carries the overlay
 * its emitter lives in beside the address, so an emitter inside the overlay
 * window is dispatched only while that overlay is resident and the unrelated
 * function sharing its address in the other overlays runs untouched. */
static feature_dispatch_entry_t text_sprt_dispatch[] = {
    { 0u, on_text_emitter, 0u },
    { 0u, on_text_emitter, 0u },
    { 0u, on_text_emitter, 0u },
    { 0u, NULL, 0u },
};

static void text_sprt_apply_options(const openpete_options_t* opts) {
    (void)opts;
    int n = 0;
    for (int i = 0; i < 3; i++)
        if (ui_game.sprt_chain_emitter[i]) {
            text_sprt_dispatch[n].addr    = ui_game.sprt_chain_emitter[i];
            text_sprt_dispatch[n].fn      = on_text_emitter;
            text_sprt_dispatch[n].overlay = ui_game.sprt_chain_overlay[i];
            n++;
        }
    text_sprt_dispatch[n].addr    = 0u;
    text_sprt_dispatch[n].fn      = NULL;
    text_sprt_dispatch[n].overlay = 0u;
}

const feature_module_t text_sprt_module = {
    .name           = "text_sprt_native",
    .apply_options  = text_sprt_apply_options,
    .dispatch_table = text_sprt_dispatch,
};
