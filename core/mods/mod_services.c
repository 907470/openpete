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
