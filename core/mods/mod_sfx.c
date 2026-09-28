// core/mods/mod_sfx.c — the mod-facing sound replacement verb.
//
// sfx_replace registers, from memory, exactly what a symbolic
// <name>.wav dropped in the mod's assets/sfx registers from disk: an
// entry in the sfx pack, substituted inside PsyCross at the voice's PCM
// upload. Everything the guest can observe — SPU RAM, the voice
// attribute mirror and the one-shot duration the game polls through
// SpuGetKeyStatus (which frees the sound slot and writes the sound-ref
// byte on the tick the STOCK sample ends) — stays byte-stock; only the
// audible buffer changes, and a longer replacement rings out on an
// orphan source. Routing the verb through the pack rather than
// re-pointing the voice's address and pitch is what keeps the end tick
// on the guest's timeline; a substituted address/pitch moved it.
//
// Names are the game's SoundTable slot names, optionally through the
// game's alias canonicaliser ("gem_pickup" -> "gemPickup").

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "../host/sfx_pack.h"

#include <string.h>

static openpete_mods_sfx_canon_fn s_canon;

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_sfx_set_canon(openpete_mods_sfx_canon_fn fn) { s_canon = fn; }

int openpete_mods_sfx_replace(openpete_mod_t* self, const char* name,
                              const void* body, uint32_t len,
                              uint16_t pitch) {
    if (!name || !body || !len) return -1;
    if (s_canon) name = s_canon(name); /* alias -> slot name */
    const char* owner = openpete_mods_id_of(self);
    int rc = sfx_pack_register_symbolic_adpcm(owner, name, body, len, pitch);
    if (rc == 0)
        OPL_INFO_F("mods", "sfx: \"%s\" replaced by %s (%u bytes, pitch %#x)",
                   name, owner ? owner : "?", len, pitch);
    else
        OPL_WARN_F("mods", "sfx: \"%s\" replacement by %s refused (%u bytes)",
                   name, owner ? owner : "?", len);
    return rc;
}
