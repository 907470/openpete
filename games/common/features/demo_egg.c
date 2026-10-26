// games/common/features/demo_egg.c - the title demo launcher turned into a
// sound; see demo_egg.h.

#include "demo_egg.h"

#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_fiber.h"          /* openpete_frame, openpete_speculative, openpete_replaying */
#include "openpete_log.h"
#include "openpete_mods.h"           /* openpete_mod_sound_play_pcm */
#include "psx_runtime_internal.h"    /* CPUState */
#include "spu_adpcm.h"
#include "spyro_overlay_table.h"     /* spyro_get_active_overlay_magic */

#include <stdlib.h>

/* Branch calls this many openpete_frame yields or fewer after the previous
 * one belong to the same held press. The branch runs once per title tick,
 * a few yields apart. */
#define PRESS_GAP_YIELDS 8u

uint16_t demo_egg_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t demo_egg_le32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void demo_egg_load(demo_egg_t* egg, const char* disc_path) {
    if (egg->loaded || !disc_path || !disc_path[0]) return;
    egg->loaded = 1;
    int scope = openpete_disc_trace_scope_begin(OPENPETE_DISC_TAG_WOAH);
    disc_image_t* img = disc_image_open(disc_path, DISC_READ_CACHED, OPENPETE_DISC_TAG_NONE);
    demo_egg_sample_t s = { NULL, 0u, 0u };
    int ok = img && egg->locate(img, &s) == 0;
    disc_image_close(img);
    openpete_disc_trace_scope_end(scope);
    if (!ok) {
        free(s.adpcm);
        return;   /* the locator logged why */
    }
    int16_t* pcm = NULL;
    uint32_t frames = spu_adpcm_decode(s.adpcm, s.len, &pcm);
    free(s.adpcm);
    if (!frames) {
        free(pcm);
        OPL_WARN_F("game", "%s: ADPCM decode produced no audio", egg->name);
        return;
    }
    egg->pcm = pcm;
    egg->frames = frames;
    egg->rate_hz = s.rate_hz;
    OPL_INFO_F("game", "%s: decoded %u frames at %u Hz (%.3f s) from %u ADPCM bytes",
               egg->name, frames, s.rate_hz, (double)frames / (double)s.rate_hz, s.len);
}

/** @brief Reports whether a guest call is one of the launch branch's calls.
 *
 * @param egg The game's egg.
 * @param cpu The guest CPU state; ra holds the call's return address.
 * @param addr The entered function's vaddr.
 * @return 1 when the title overlay is resident and (addr, ra) is a site. */
static int is_branch_call(const demo_egg_t* egg, const CPUState* cpu, uint32_t addr) {
    if (spyro_get_active_overlay_magic() != egg->overlay) return 0;
    for (uint32_t i = 0; i < egg->n_sites; i++)
        if (egg->sites[i].callee == addr && egg->sites[i].ra == cpu->ra) return 1;
    return 0;
}

int demo_egg_on_call(demo_egg_t* egg, CPUState* cpu, uint32_t addr) {
    if (!is_branch_call(egg, cpu, addr)) return 0;
    if (openpete_speculative || openpete_replaying) return 1;
    unsigned now = openpete_frame;
    int new_press = !egg->seen || now - egg->last_yield > PRESS_GAP_YIELDS;
    egg->seen = 1;
    egg->last_yield = now;
    if (!new_press) return 1;
    if (!egg->pcm) {
        OPL_INFO_F("game", "%s: launch combination pressed; no sample decoded, demo launch skipped",
                   egg->name);
        return 1;
    }
    int rc = openpete_mod_sound_play_pcm(egg->name, egg->pcm, egg->frames, 1,
                                         egg->rate_hz, 1.0f);
    egg->plays++;
    OPL_INFO_F("game", "%s: launch combination pressed at yield %u; demo launch skipped, "
               "played %u frames at %u Hz (press %u, rc %d)",
               egg->name, now, egg->frames, egg->rate_hz, egg->plays, rc);
    return 1;
}
