// games/common/renderer/text_sprt.c - native renderer feed for the SPRT text
// chain.
//
// Most of a game's 2D layer reaches the shared feeder through the
// ordering-table chokepoint (ui_game.ot_insert). The glyph emitters do not:
// each builds its own tag-to-tag chain of glyph packets in the primitive
// buffer, then links the whole chain into the 2D ordering table with an
// inlined insert, so the chokepoint never sees a glyph. Two packet forms are
// emitted: one 0xC-byte DR_MODE followed by one 0x14-byte SPRT per glyph, and
// one 0x28-byte POLY_FT4 per glyph carrying its own texture page and CLUT
// (Spyro 2 `func_8004A59C`, mobyfunc.s 8004A778; Spyro 3 `func_8002E970`,
// mobyfunc.s 8002EB4C, both storing the 0x2C command byte at packet offset 7).
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

/** @brief Reports whether the text chain census is enabled by OPENPETE_TEXT_SPRT_CENSUS.
 *
 * Latched on the first call.
 * @return 1 when the variable is set to a non-empty value not starting with
 *         '0', else 0. */
static int census_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TEXT_SPRT_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Walks a primitive-buffer range as contiguous packets and feeds each glyph packet to the native UI sprite feeder.
 *
 * A packet's tag word holds its length in words in the top byte, so the
 * next packet starts (len + 1) * 4 bytes on; the command byte is the top
 * byte of word 1. A DR_MODE (0xE1) sets the texture page for the SPRTs
 * (0x64..0x67) after it; a SPRT before any DR_MODE is skipped. A POLY_FT4
 * (0x2C..0x2F) carries its own page. The walk stops at a zero length or a
 * packet that overruns the range. Counts the chain and each glyph fed.
 * @param cpu The guest CPU state.
 * @param start The first packet's guest address.
 * @param end The primitive cursor after the emitter ran. */
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
        } else if ((code & 0xFCu) == 0x2Cu) {   /* POLY_FT4 0x2C..0x2F, len 9 -> 0x28 bytes */
            ui_sprite_feed_ft4(cpu, p);         /* the packet carries its own tpage */
            s_glyphs++;
        }
        p += size;
    }
    s_chains++;
}

/** @brief Dispatch hook for a glyph chain emitter: runs it, then feeds the chain it left in the primitive buffer.
 *
 * Inert (returns 0, so the normal dispatch runs the emitter) while the native
 * scene is disabled and on re-entry from its own dispatch. Otherwise reads the
 * polybuf cursor, dispatches the real emitter, reads the cursor again and
 * walks the advanced range with feed_chain. Reads guest RAM only. Prints a
 * census line at most once per 100 openpete_frame values under
 * OPENPETE_TEXT_SPRT_CENSUS.
 * @param cpu The guest CPU state.
 * @param addr The emitter's guest address.
 * @return 1 when the emitter ran here, 0 to leave dispatch to the caller. */
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

/** @brief Fills the dispatch table from the game's SPRT chain emitter list.
 *
 * Each of the first three ui_game.sprt_chain_emitter entries that is
 * non-zero becomes a table row with its overlay id; the table is then
 * terminated.
 * @param opts Unused. */
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
