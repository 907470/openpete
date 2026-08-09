/* sfx-neutral-probe — exercise the whole SFX overlay path with zero
 * guest writes, so the oracle gate can prove it sim-neutral: replace
 * the gem-pickup sample with a synthesized silent ADPCM body and hook
 * every play with a pitch rewrite. See mod.toml for the contract. */

#include "openpete_mod_api.h"

#include <string.h>

static const openpete_mod_api_t* A;
static openpete_mod_t* SELF;

#define PROBE_SOUND "spyro_charge"

/* Last-play trace, written by the hook and read by the overlay section.
 * Host-side only, so it never participates in gameplay state. */
static unsigned int s_plays;
static unsigned int s_last_pitch;
static unsigned int s_last_gap_ms;

/* Minimal valid SPU-ADPCM body: four all-zero frames (shift 0, filter
 * 0, silent samples), end flag (bit 0) on the last frame's flags byte. */
static unsigned char s_body[64];

static void on_play(openpete_sfx_event_t* ev)
{
    /* Rewrite the pitch every play (cycles a small ladder) — exercises
     * the per-voice persistence across the game's per-tick attr
     * refresh. Host-side only; no guest reads or writes. */
    ev->pitch = (unsigned short)(0x800 + (ev->play_index & 7u) * 0x40u);
    s_plays = ev->play_index + 1u;
    s_last_pitch = ev->pitch;
    s_last_gap_ms = ev->ms_since_prev;
    if (ev->play_index == 0)
        A->log(SELF, OP_MOD_LOG_INFO, "sfx-probe: substitution live (%s)",
               ev->name);
}

static void ui_draw(const openpete_mod_ui_t* ui, void* user)
{
    (void)user;
    ui->text("hooked sound: %s", PROBE_SOUND);
    ui->text_disabled("sample replaced with %u bytes of silent ADPCM",
                      (unsigned)sizeof s_body);
    ui->separator();
    ui->text("plays seen: %u", s_plays);
    if (s_plays) {
        ui->text("last pitch: 0x%03X", s_last_pitch);
        ui->tooltip("Pitch the hook wrote for the most recent play; the "
                    "ladder cycles 0x800..0x9C0 with the play index.");
        ui->text("gap before it: %u ms", s_last_gap_ms);
    } else {
        ui->text_disabled("no play intercepted yet");
    }
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self)
{
    A = api; SELF = self;
    memset(s_body, 0, sizeof s_body);
    s_body[3 * 16 + 1] = 0x01;                /* end flag, final frame */
    if (api->sfx_replace(self, PROBE_SOUND, s_body, sizeof s_body, 0x800) ||
        api->sfx_hook(self, PROBE_SOUND, on_play)) {
        A->log(SELF, OP_MOD_LOG_ERROR, "sfx registration failed");
        return 1;
    }
    api->register_ui(self, ui_draw, NULL);
    return 0;
}
