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

static int      s_pending_op = 0; /* 0 none, 1 save, 2 load */
static unsigned s_pending_slot = 0;

/* bk2 status mirror: run.c publishes per sim frame while a movie is
 * loaded; zero-initialized = "no movie" for movie-less sessions. */
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
    OPL_INFO_F("mods", "%s queued savestate %s on slot %u",
               owner ? owner : "?", op == 1 ? "SAVE" : "LOAD", slot);
    return 0;
}

/* run.c consumes exactly one pending op per host iteration. */
int openpete_mods_pending_state_op(unsigned* out_slot) {
    int op = s_pending_op;
    if (out_slot) *out_slot = s_pending_slot;
    s_pending_op = 0;
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

int openpete_mods_state_write_section(FILE* fp) {
    /* Compose the payload first so the length prefix is exact. */
    size_t cap = 8;
    for (int i = 0; i < s_hook_count; i++)
        cap += STATE_ID_LEN + 4 + STATE_BLOB_CAP;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (!buf) return -1;

    uint32_t count = 0;
    size_t off = 4; /* count patched at the end */
    for (int i = 0; i < s_hook_count; i++) {
        if (!s_hooks[i].save) continue;
        unsigned char* entry = buf + off;
        memset(entry, 0, STATE_ID_LEN);
        snprintf((char*)entry, STATE_ID_LEN, "%s",
                 s_hooks[i].id ? s_hooks[i].id : "?");
        int len = s_hooks[i].save(entry + STATE_ID_LEN + 4, STATE_BLOB_CAP);
        if (len < 0) len = 0;
        if (len > STATE_BLOB_CAP) len = STATE_BLOB_CAP;
        uint32_t len32 = (uint32_t)len;
        memcpy(entry + STATE_ID_LEN, &len32, 4);
        off += STATE_ID_LEN + 4 + (size_t)len;
        count++;
    }
    memcpy(buf, &count, 4);

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

void openpete_mods_state_dispatch(void) {
    if (!s_read_buf) return;
    const unsigned char* buf = s_read_buf;
    const uint32_t payload_len = s_read_len;
    uint32_t count = 0;
    memcpy(&count, buf, 4);
    size_t off = 4;
    for (uint32_t e = 0; e < count; e++) {
        if (off + STATE_ID_LEN + 4 > payload_len) break;
        const char* id = (const char*)(buf + off);
        uint32_t len = 0;
        memcpy(&len, buf + off + STATE_ID_LEN, 4);
        const unsigned char* blob = buf + off + STATE_ID_LEN + 4;
        if (off + STATE_ID_LEN + 4 + len > payload_len) break;
        int matched = 0;
        for (int i = 0; i < s_hook_count; i++) {
            if (s_hooks[i].load && s_hooks[i].id &&
                strncmp(s_hooks[i].id, id, STATE_ID_LEN) == 0) {
                s_hooks[i].load(blob, (int)len);
                matched = 1;
                break;
            }
        }
        if (!matched)
            OPL_WARN_F("mods", "state file carries data for mod \"%.*s\" "
                       "which is not loaded — skipped", STATE_ID_LEN, id);
        off += STATE_ID_LEN + 4 + len;
    }
    free(s_read_buf);
    s_read_buf = NULL;
    s_read_len = 0;
}

/* ---- asset-overlay roots (P2: logical-ID asset overlay v1) ----------- */
/* One root per enabled mod with an assets/ dir, registered by the loader
 * in ENABLED ORDER (= priority; later wins). Consumers (texture pack,
 * music override) layer these over their base directories. */

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
    return 0;
}

int openpete_mods_asset_root_count(void) { return s_root_n; }

const char* openpete_mods_asset_root(int i, const char** owner) {
    if (i < 0 || i >= s_root_n) return NULL;
    if (owner) *owner = s_roots[i].owner;
    return s_roots[i].dir;
}
