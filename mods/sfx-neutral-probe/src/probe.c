/* sfx-neutral-probe — exercise the whole SFX overlay path with zero
 * guest writes, so the oracle gate can prove it sim-neutral: replace
 * the gem-pickup sample with a synthesized silent ADPCM body and hook
 * every play with a pitch rewrite. See mod.toml for the contract. */

#include "openpete_mod_api.h"

#include <string.h>

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

/* Minimal valid SPU-ADPCM body: four all-zero frames (shift 0, filter
 * 0, silent samples), end flag (bit 0) on the last frame's flags byte. */
static unsigned char s_body[64];

static void on_play(openpete_sfx_event_t* ev)
{
    /* Rewrite the pitch every play (cycles a small ladder) — exercises
     * the per-voice persistence across the game's per-tick attr
     * refresh. Host-side only; no guest reads or writes. */
    ev->pitch = (unsigned short)(0x800 + (ev->play_index & 7u) * 0x40u);
    if (ev->play_index == 0)
        A->log(SELF, OP_MOD_LOG_INFO, "sfx-probe: substitution live (%s)",
               ev->name);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    A = api; SELF = self;
    memset(s_body, 0, sizeof s_body);
    s_body[3 * 16 + 1] = 0x01;                /* end flag, final frame */
    if (api->sfx_replace(self, "spyro_charge", s_body, sizeof s_body, 0x800) ||
        api->sfx_hook(self, "spyro_charge", on_play)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "sfx registration failed");
        return 1;
    }
    return 0;
}
