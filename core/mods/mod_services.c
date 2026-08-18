// core/mods/mod_services.c — engine services for mods (P2 contract).
//
// v1 surface (notes/plans/openpete-modding-framework.md,
// "engine-services contract"):
//   - savestate trigger: mods QUEUE a save/load; run.c executes it at
//     the same quiescent host-loop point as the user's hotkeys (the game
//     fiber is yielded — snapshot/restore can't race recompiled code).
//   - savestate participation: registered mods serialize their own
//     host-side state into a trailing "mods" section of the state file
//     (backward compatible: old files simply end before it), and get it
//     back on load — so a rewind rewinds the MOD too, not just the
//     guest. This is the same serialize pair the plan earmarks for hot
//     reload and process handoff.
//
// Section framing (after the engine's fixed header/cpu/ram/scratch/vram
// sections):  u32 payload_len | payload
// payload = u32 count, then per mod: char id[64] | u32 len | bytes.
//
// Live toggle and savestate participation: a DISABLED mod keeps its
// state hooks registered and keeps participating — write, verdict,
// dispatch and the reinit-signal sweep all run over every registered
// hook, with no enabled filter. That is the freeze-not-teardown posture
// the toggle machinery is built on (openpete_mods_tick_boundary flips
// owner-keyed flags; nothing is unregistered): a state file written
// while a mod is off still carries its blob, so re-enabling it lands on
// host state that matches the restored world instead of a blob-shaped
// hole. Any state load moves the guest world under every mod, enabled
// or not, so every hook must hear about it. What a disabled mod must
// NOT do is push guest-side effects back out from its load_fn — a mod
// learns its enable state from its on_toggle hook and gates its own
// guest writes on it.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "openpete_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_HOOKS_MAX   32
#define STATE_BLOB_CAP    (64 * 1024)
#define STATE_ID_LEN      64

typedef struct {
    const char*                id; /* loader-owned mod id string */
    openpete_mod_state_save_fn save;
    openpete_mod_state_load_fn load;
} state_hook_t;

static state_hook_t s_hooks[STATE_HOOKS_MAX];
static int          s_hook_count = 0;

/* Index of the hook able to RECEIVE a blob for `id` (-1 = none). Ids are
 * compared over the on-disk field width, so a truncated id in a state
 * file still resolves. */
static int state_hook_load_index(const char* id) {
    if (!id) return -1;
    for (int i = 0; i < s_hook_count; i++)
        if (s_hooks[i].load && s_hooks[i].id &&
            strncmp(s_hooks[i].id, id, STATE_ID_LEN) == 0)
            return i;
    return -1;
}

static int         s_pending_op = 0; /* 0 none, 1 save, 2 load */
static unsigned    s_pending_slot = 0;
static const char* s_pending_owner = NULL; /* loader-owned mod id */

/* bk2 status mirror: run.c publishes per sim frame while a movie is
 * loaded; zero-initialized = "no movie" for movie-less sessions. The
 * mirror is owner-free and read-only, so live toggles neither change
 * what it reports nor leave anything to undo — a disabled mod simply
 * stops running the hooks that would query it. */
static openpete_mods_bk2_status_t s_bk2_status;

void openpete_mods_bk2_publish(const openpete_mods_bk2_status_t* st) {
    if (st) s_bk2_status = *st;
}

void openpete_mods_bk2_query(openpete_mods_bk2_status_t* out) {
    if (out) *out = s_bk2_status;
}

/* Hot reload: run the owner's SAVE hook into a caller buffer (returns
 * bytes or -1), replace registrations, then feed the blob to the NEW
 * load hook — the mod's state crosses the reload. */
int openpete_mods_state_capture(const char* owner, void* buf, int cap) {
    for (int i = 0; i < s_hook_count; i++)
        if (s_hooks[i].id && strcmp(s_hooks[i].id, owner) == 0 &&
            s_hooks[i].save)
            return s_hooks[i].save(buf, cap);
    return -1;
}

void openpete_mods_state_restore(const char* owner, const void* buf,
                                 int len) {
    for (int i = 0; i < s_hook_count; i++)
        if (s_hooks[i].id && strcmp(s_hooks[i].id, owner) == 0 &&
            s_hooks[i].load) {
            s_hooks[i].load(buf, len);
            return;
        }
}

void openpete_mods_state_remove(const char* owner) {
    int w = 0;
    for (int i = 0; i < s_hook_count; i++) {
        if (s_hooks[i].id && strcmp(s_hooks[i].id, owner) == 0) continue;
        s_hooks[w++] = s_hooks[i];
    }
    s_hook_count = w;
}

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

/* run.c consumes exactly one pending op per host iteration.
 *
 * A queue-then-toggle race is possible: the request is made from a tick
 * hook, the tick boundary can disable the requesting mod, and only then
 * does the host loop reach this consume point. Executing it anyway would
 * let a mod that is already off rewind or overwrite the timeline, so the
 * op is dropped when its owner is a loaded mod that is no longer
 * enabled. Non-mod owners (no loader entry) are never filtered. */
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

int openpete_mods_state_register(openpete_mod_state_save_fn save,
                                 openpete_mod_state_load_fn load,
                                 const char* owner) {
    if (!save && !load) return -1;
    if (s_hook_count >= STATE_HOOKS_MAX) {
        OPL_ERROR_F("mods", "savestate-hook table full (%d) — cannot add "
                    "%s", STATE_HOOKS_MAX, owner ? owner : "?");
        return -1;
    }
    s_hooks[s_hook_count].id = owner;
    s_hooks[s_hook_count].save = save;
    s_hooks[s_hook_count].load = load;
    s_hook_count++;
    return 0;
}

/* ---- state-file section (called from core/runtime/savestate.c) ------ */

/* Per-entry metadata written ahead of each blob (v2 framing, magic
 * 'MSv2'): the saving mod's state-ABI identity (or opaque tier + build
 * key), so the load-side verdict ladder can verify compatibility BEFORE
 * guest state is touched. */
#define STATE_SECTION_MAGIC 0x3276534Du /* 'MSv2' LE */
typedef struct {
    uint32_t has_abi;
    uint32_t abi_ver;
    uint32_t abi_size;
    uint32_t abi_flags;
    uint64_t abi_fp;
    char     bkey[16];   /* build-key hex prefix (opaque-tier gate) */
} state_entry_meta_t;

static void state_entry_meta_fill(const char* id, state_entry_meta_t* m) {
    memset(m, 0, sizeof(*m));
    m->has_abi = (uint32_t)openpete_mods_stateabi_query(
        id, &m->abi_ver, &m->abi_size, &m->abi_flags, &m->abi_fp);
    const char* bk = openpete_mods_build_key(id);
    if (bk) snprintf(m->bkey, sizeof(m->bkey), "%.15s", bk);
}

/* Every registered save hook contributes, including one owned by a
 * currently DISABLED mod: its host state is frozen, not discarded, so
 * the blob has to travel with the state file for a later re-enable to
 * land coherently on the restored world. */
int openpete_mods_state_write_section(FILE* fp) {
    /* Compose the payload first so the length prefix is exact. */
    size_t cap = 12;
    for (int i = 0; i < s_hook_count; i++)
        cap += STATE_ID_LEN + sizeof(state_entry_meta_t) + 4 + STATE_BLOB_CAP;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (!buf) return -1;

    uint32_t magic = STATE_SECTION_MAGIC;
    uint32_t count = 0;
    size_t off = 8; /* magic + count patched at the end */
    for (int i = 0; i < s_hook_count; i++) {
        if (!s_hooks[i].save) continue;
        unsigned char* entry = buf + off;
        memset(entry, 0, STATE_ID_LEN);
        snprintf((char*)entry, STATE_ID_LEN, "%s",
                 s_hooks[i].id ? s_hooks[i].id : "?");
        state_entry_meta_t meta;
        state_entry_meta_fill(s_hooks[i].id, &meta);
        memcpy(entry + STATE_ID_LEN, &meta, sizeof(meta));
        int len = s_hooks[i].save(entry + STATE_ID_LEN + sizeof(meta) + 4,
                                  STATE_BLOB_CAP);
        if (len < 0) len = 0;
        if (len > STATE_BLOB_CAP) len = STATE_BLOB_CAP;
        uint32_t len32 = (uint32_t)len;
        memcpy(entry + STATE_ID_LEN + sizeof(meta), &len32, 4);
        off += STATE_ID_LEN + sizeof(meta) + 4 + (size_t)len;
        count++;
    }
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &count, 4);

    uint32_t payload_len = (uint32_t)off;
    int rc = 0;
    if (fwrite(&payload_len, 4, 1, fp) != 1) rc = -1;
    if (rc == 0 && fwrite(buf, 1, off, fp) != off) rc = -1;
    free(buf);
    return rc;
}

/* Read buffers the section (file still open); dispatch runs AFTER the
 * engine committed guest RAM/CPU/VRAM, so a mod's load_fn that peeks at
 * guest state sees the restored world. */
static unsigned char* s_read_buf = NULL;
static uint32_t       s_read_len = 0;

int openpete_mods_state_read_section(FILE* fp) {
    free(s_read_buf);
    s_read_buf = NULL;
    s_read_len = 0;
    uint32_t payload_len = 0;
    if (fread(&payload_len, 4, 1, fp) != 1)
        return 0; /* pre-mods state file: nothing to restore — fine */
    if (payload_len < 4 || payload_len > 64u * 1024u * 1024u) return 0;
    unsigned char* buf = (unsigned char*)malloc(payload_len);
    if (!buf) return -1;
    if (fread(buf, 1, payload_len, fp) != payload_len) {
        free(buf);
        OPL_WARN_F("mods", "state file: truncated mods section — skipped");
        return 0;
    }
    s_read_buf = buf;
    s_read_len = payload_len;
    return 0;
}

/* Parsed per-entry verdicts (built pre-commit by verdict(), consumed
 * post-commit by dispatch()). */
enum { SV_DELIVER = 0, SV_DROP_REINIT, SV_NOT_LOADED };
typedef struct {
    const char*          id;    /* points into s_read_buf */
    const unsigned char* blob;
    uint32_t             len;
    int                  action;
} state_verdict_t;
static state_verdict_t s_verdicts[STATE_HOOKS_MAX * 2];
static int             s_verdict_n = 0;
static int             s_verdict_valid = 0;

int openpete_mods_state_verdict(void) {
    s_verdict_n = 0;
    s_verdict_valid = 1;
    if (!s_read_buf) return 0;   /* no section: dispatch reinit-signals all */

    const unsigned char* buf = s_read_buf;
    const uint32_t payload_len = s_read_len;
    if (payload_len < 8) return 0;
    uint32_t magic = 0, count = 0;
    memcpy(&magic, buf, 4);
    memcpy(&count, buf + 4, 4);
    if (magic != STATE_SECTION_MAGIC) {
        OPL_WARN_F("mods", "state file: pre-P2b mods section framing — "
                   "mod blobs ignored (mods will reinit)");
        free(s_read_buf); s_read_buf = NULL; s_read_len = 0;
        return 0;
    }

    size_t off = 8;
    int refuse = 0;
    for (uint32_t e = 0; e < count; e++) {
        if (off + STATE_ID_LEN + sizeof(state_entry_meta_t) + 4 > payload_len)
            break;
        const char* id = (const char*)(buf + off);
        state_entry_meta_t saved;
        memcpy(&saved, buf + off + STATE_ID_LEN, sizeof(saved));
        uint32_t len = 0;
        memcpy(&len, buf + off + STATE_ID_LEN + sizeof(saved), 4);
        const unsigned char* blob = buf + off + STATE_ID_LEN + sizeof(saved) + 4;
        if (off + STATE_ID_LEN + sizeof(saved) + 4 + len > payload_len) break;
        off += STATE_ID_LEN + sizeof(saved) + 4 + len;

        if (s_verdict_n >= (int)(sizeof(s_verdicts) / sizeof(s_verdicts[0])))
            break;
        state_verdict_t* v = &s_verdicts[s_verdict_n++];
        v->id = id; v->blob = blob; v->len = len;

        /* Nothing can receive this blob: the mod is absent, it is loaded
         * with its entry deferred (a boot-disabled mod registers no
         * hooks and resolves no state-ABI descriptor until its first
         * enable), or it registered save-only. Skip the entry BEFORE the
         * ABI ladder — undeliverable data cannot corrupt anything, while
         * a REFUSE here would reject every state load in a session where
         * the mod is simply turned off. */
        if (state_hook_load_index(id) < 0) {
            v->action = SV_NOT_LOADED;
            continue;
        }
        state_entry_meta_t now;
        state_entry_meta_fill(id, &now);
        const char* bk_now = openpete_mods_build_key(id);
        if (!bk_now) {
            v->action = SV_NOT_LOADED;
            continue;
        }
        if (saved.has_abi && now.has_abi) {
            if (saved.abi_fp == now.abi_fp) {
                v->action = SV_DELIVER;
            } else if (now.abi_flags & OP_MOD_STATE_DROP_OK) {
                OPL_ERROR_F("mods", "%s: state-ABI changed (fp %016llx -> "
                            "%016llx) — blob dropped, mod will reinit",
                            id, (unsigned long long)saved.abi_fp,
                            (unsigned long long)now.abi_fp);
                v->action = SV_DROP_REINIT;
            } else {
                OPL_ERROR_F("mods", "%s: state-ABI changed (v%u/%uB fp "
                            "%016llx -> v%u/%uB fp %016llx) and the mod "
                            "does not declare OP_MOD_STATE_DROP_OK — "
                            "REFUSING the state load to avoid corruption",
                            id, saved.abi_ver, saved.abi_size,
                            (unsigned long long)saved.abi_fp,
                            now.abi_ver, now.abi_size,
                            (unsigned long long)now.abi_fp);
                refuse = 1;
            }
        } else if (saved.has_abi && !now.has_abi) {
            /* Descriptor removed since save — contract downgrade. */
            OPL_ERROR_F("mods", "%s: saved state declares a state-ABI but "
                        "the running build has none — REFUSING the load",
                        id);
            refuse = 1;
        } else {
            /* Opaque tier: exact build-key prefix gates delivery. */
            if (strncmp(saved.bkey, bk_now, 15) == 0) {
                v->action = SV_DELIVER;
            } else {
                OPL_ERROR_F("mods", "%s: opaque mod state from a different "
                            "build (key %.15s vs %.15s) — blob dropped, "
                            "mod will reinit (declare OPENPETE_MOD_STATE "
                            "for cross-build states)",
                            id, saved.bkey, bk_now);
                v->action = SV_DROP_REINIT;
            }
        }
    }
    return refuse ? -1 : 0;
}

void openpete_mods_state_dispatch(void) {
    if (!s_verdict_valid) (void)openpete_mods_state_verdict();

    char delivered[STATE_HOOKS_MAX] = {0};
    for (int e = 0; e < s_verdict_n; e++) {
        const state_verdict_t* v = &s_verdicts[e];
        int hook = state_hook_load_index(v->id);
        switch (v->action) {
        case SV_DELIVER:
            if (hook >= 0) { s_hooks[hook].load(v->blob, (int)v->len);
                             delivered[hook] = 1; }
            break;
        case SV_DROP_REINIT:
            if (hook >= 0) { s_hooks[hook].load(NULL, 0);
                             delivered[hook] = 1; }
            break;
        case SV_NOT_LOADED:
            OPL_WARN_F("mods", "state file carries data for mod \"%.*s\", "
                       "which has no state-load hook (not loaded, or "
                       "loaded with its entry deferred) — skipped",
                       STATE_ID_LEN, v->id);
            break;
        }
    }
    /* Hook-mods with NO delivered blob: the restored world has no memory
     * of them (state predates the mod, or their blob was dropped/
     * absent). Reinit-signal so their host statics don't stay coherent
     * with the WRONG timeline. Unconditional, and deliberately blind to
     * the live-toggle state — any state load means the guest world
     * changed under every mod, disabled ones included, and a disabled
     * mod that re-enables later must not wake up holding host state from
     * the abandoned timeline. */
    for (int i = 0; i < s_hook_count; i++) {
        if (!s_hooks[i].load || delivered[i]) continue;
        OPL_WARN_F("mods", "state file has no blob for mod \"%s\" — "
                   "reinit-signaled (load(NULL,0))",
                   s_hooks[i].id ? s_hooks[i].id : "?");
        s_hooks[i].load(NULL, 0);
    }

    free(s_read_buf);
    s_read_buf = NULL;
    s_read_len = 0;
    s_verdict_n = 0;
    s_verdict_valid = 0;
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

#define ASSET_ROOTS_MAX 16

static struct {
    char        dir[1024];
    const char* owner;
} s_roots[ASSET_ROOTS_MAX];
static int s_root_n = 0;

int openpete_mods_register_asset_root(const char* dir, const char* owner) {
    if (!dir || !dir[0]) return -1;
    if (s_root_n >= ASSET_ROOTS_MAX) {
        OPL_WARN_F("mods", "asset-root table full (%d) — %s's assets "
                   "ignored", ASSET_ROOTS_MAX, owner ? owner : "?");
        return -1;
    }
    snprintf(s_roots[s_root_n].dir, sizeof s_roots[s_root_n].dir, "%s", dir);
    s_roots[s_root_n].owner = owner;
    s_root_n++;
    OPL_INFO_F("mods", "asset root registered: %s (%s)", dir,
               owner ? owner : "?");
    /* Declarative own-textures ride the root registration (boot-scoped,
     * like every asset overlay). The owner string passed here is the
     * loader's session-lifetime id — the registry keeps that pointer. */
    {
        int n = openpete_mods_texture_scan_root(
            s_roots[s_root_n - 1].dir, s_roots[s_root_n - 1].owner);
        if (n > 0)
            OPL_INFO_F("mods", "asset root %s: %d own-texture PNG(s)",
                       s_roots[s_root_n - 1].dir, n);
    }
    return 0;
}

int openpete_mods_asset_root_count(void) { return s_root_n; }

const char* openpete_mods_asset_root(int i, const char** owner) {
    if (i < 0 || i >= s_root_n) return NULL;
    if (owner) *owner = s_roots[i].owner;
    return s_roots[i].dir;
}
