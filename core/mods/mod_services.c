// core/mods/mod_services.c — engine services for mods (P2 contract).
//
// v1 surface (the engine-services contract):
//   - savestate trigger: mods QUEUE a save/load; run.c executes it at
//     the same quiescent host-loop point as the user's hotkeys (the game
//     fiber is yielded — snapshot/restore can't race recompiled code).

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "openpete_state.h"
#include "mod_vec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* bk2 status mirror: run.c publishes per sim frame while a movie is
 * loaded; zero-initialized = "no movie" for movie-less sessions. The
 * mirror is owner-free and read-only, so live toggles neither change
 * what it reports nor leave anything to undo — a disabled mod simply
 * stops running the hooks that would query it. */
static openpete_mods_bk2_status_t s_bk2_status;

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_bk2_publish(const openpete_mods_bk2_status_t* st) {
    if (st) s_bk2_status = *st;
}

/* Documented at the declaration in openpete_mods.h. */
void openpete_mods_bk2_query(openpete_mods_bk2_status_t* out) {
    if (out) *out = s_bk2_status;
}

static int         s_pending_op = 0; /* 0 none, 1 save, 2 load */
static unsigned    s_pending_slot = 0;
static const char* s_pending_owner = NULL; /* loader-owned mod id */

int openpete_mods_savestate_request(int op, unsigned slot,
                                    const char* owner) {
    if (op != 1 && op != 2) return -1;
    if (slot >= OPENPETE_STATE_NUM_SLOTS) {
        OPL_ERROR_F("mods", "%s: savestate slot %u out of range (0..%d)",
                    owner ? owner : "?", slot,
                    OPENPETE_STATE_NUM_SLOTS - 1);
        return -1;
    }
    if (s_pending_op != 0) {
        OPL_WARN_F("mods", "%s: a savestate op is already pending — "
                   "request dropped", owner ? owner : "?");
        return -1;
    }
    s_pending_op = op;
    s_pending_slot = slot;
    s_pending_owner = owner;
    OPL_INFO_F("mods", "%s queued savestate %s on slot %u",
               owner ? owner : "?", op == 1 ? "SAVE" : "LOAD", slot);
    return 0;
}

/* Drops an op whose owner mod was disabled after the request. */
int openpete_mods_pending_state_op(unsigned* out_slot) {
    int         op    = s_pending_op;
    const char* owner = s_pending_owner;
    s_pending_op    = 0;
    s_pending_owner = NULL;
    if (op && owner && openpete_mods_build_key(owner) &&
        !openpete_mods_is_enabled(owner)) {
        OPL_INFO_F("mods", "%s was disabled before its queued savestate "
                   "%s on slot %u ran — dropped", owner,
                   op == 1 ? "SAVE" : "LOAD", s_pending_slot);
        return 0;
    }
    if (out_slot) *out_slot = s_pending_slot;
    return op;
}

/* ---- asset-overlay roots (P2: logical-ID asset overlay v1) ----------- */
/* One root per entered mod with an assets/ dir, registered by the loader
 * in ENABLED ORDER (= priority; later wins). Consumers (texture pack,
 * music override) layer these over their base directories.
 *
 * The table is append-only and deliberately NOT filtered by live-toggle
 * state: a root outlives its owner's disable so the priority order (and
 * every index derived from it) stays stable, and so a re-enable needs no
 * re-registration. Owner enablement is a CONSUME-TIME question, and each
 * consumer answers it itself — carry the owner returned here alongside
 * the resolved asset, skip it while openpete_mods_is_enabled(owner) is
 * false, and key any memo of the resolution on
 * openpete_mods_toggle_epoch() so a flip invalidates it. A consumer that
 * instead bakes the roots into a one-shot scan is restart-scoped, and
 * its Mods-panel row has to say so. A first enable during the session
 * appends its root here at the tail (= highest priority). */

/* Roots the table allocates up front; it grows past this. */
#define ASSET_ROOTS_INIT 16

typedef struct {
    char        dir[1024];
    const char* owner;
} asset_root_t;
static mod_fvec s_roots;   /* asset_root_t, append-only, index-stable */

int openpete_mods_register_asset_root(const char* dir, const char* owner) {
    if (!dir || !dir[0]) return -1;
    if (!s_roots.elem_size)
        mod_fvec_init(&s_roots, sizeof(asset_root_t), ASSET_ROOTS_INIT,
                      "asset_roots", ASSET_ROOTS_INIT);
    asset_root_t* r = (asset_root_t*)mod_fvec_push(&s_roots);
    if (!r) {
        OPL_WARN_F("mods", "asset-root table allocation failed — %s's "
                   "assets ignored", owner ? owner : "?");
        return -1;
    }
    snprintf(r->dir, sizeof r->dir, "%s", dir);
    r->owner = owner;
    OPL_INFO_F("mods", "asset root registered: %s (%s)", dir,
               owner ? owner : "?");
    /* Declarative own-textures ride the root registration (boot-scoped,
     * like every asset overlay). The owner string passed here is the
     * loader's session-lifetime id — the registry keeps that pointer. */
    {
        int n = openpete_mods_texture_scan_root(r->dir, r->owner);
        if (n > 0)
            OPL_INFO_F("mods", "asset root %s: %d own-texture PNG(s)",
                       r->dir, n);
    }
    return 0;
}

/* Documented at the declaration in openpete_mods.h. */
int openpete_mods_asset_root_count(void) {
    return (int)mod_fvec_count(&s_roots);
}

/* Documented at the declaration in openpete_mods.h. Every caller copies
 * the returned dir inside the call. */
const char* openpete_mods_asset_root(int i, const char** owner) {
    const asset_root_t* r;
    if (i < 0) return NULL;
    r = (const asset_root_t*)mod_fvec_at(&s_roots, (size_t)i);
    if (!r) return NULL;
    if (owner) *owner = r->owner;
    return r->dir;
}
