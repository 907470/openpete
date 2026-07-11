// core/mods/mod_sfx.c — the mod-facing SFX overlay service.
//
// Grown out of the spyro3-gems stress test, which had to hand-roll all
// of this guest-side: call_addr'd SpuWrite uploads, SoundDefinition
// repoints, and pitch-variance zeroing that shifted the gameplay rand()
// stream. This service moves the whole substitution host-side, at the
// voice key-on chokepoint in the game's SPU thunk layer:
//
//   - the game layer registers a RESOLVER (sample SPU addr -> stable
//     sound name, read from the game's own sound table) and an
//     UPLOADER (stage an ADPCM body into free SPU tail space);
//   - at key-on the thunk asks on_key() whether the play is replaced,
//     and applies the verdict to a HOST COPY of SpuVoiceAttr only.
//
// Guest RAM, the SPU sample bank image, sound tables and rand() all
// stay byte-stock, so an SFX mod is sim-neutral by construction — the
// sim never reads voice state back. (SpuRead exists but Spyro never
// calls it on the sample region; a game that did would need a shadow.)
//
// Hooks run on the sim thread inside a psyq thunk. They're expected to
// stay pure (no guest writes) — same spirit as present hooks, enforced
// socially rather than mechanically for now.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "openpete_time.h"

#include <stdlib.h>
#include <string.h>

#define SFX_NAME_MAX   32
#define SFX_REPLACE_MAX 32
#define SFX_HOOKS_MAX   32

typedef struct {
    char     name[SFX_NAME_MAX];
    void*    body;          /* engine-owned copy of the ADPCM frames */
    uint32_t len;
    uint16_t pitch;         /* 0 = keep the game's pitch */
    uint32_t staged_addr;   /* SPU addr this bank-generation, 0 = not yet */
    char     owner[24];
} sfx_replace_t;

typedef struct {
    char name[SFX_NAME_MAX];
    void (*fn)(openpete_sfx_event_t* ev);
    char owner[24];
} sfx_hook_t;

typedef struct {
    char      name[SFX_NAME_MAX];
    uint32_t  plays;
    op_time_t last_ns;
} sfx_stat_t;

static sfx_replace_t s_repl[SFX_REPLACE_MAX];
static int           s_repl_count;
static sfx_hook_t    s_hooks[SFX_HOOKS_MAX];
static int           s_hook_count;
static sfx_stat_t    s_stats[SFX_REPLACE_MAX + SFX_HOOKS_MAX];
static int           s_stat_count;

static openpete_mods_sfx_resolver_fn s_resolve;
static openpete_mods_sfx_upload_fn   s_upload;

void openpete_mods_sfx_set_fns(openpete_mods_sfx_resolver_fn resolve,
                               openpete_mods_sfx_upload_fn upload) {
    s_resolve = resolve;
    s_upload = upload;
}

void openpete_mods_sfx_bank_reset(void) {
    for (int i = 0; i < s_repl_count; i++) s_repl[i].staged_addr = 0;
}

int openpete_mods_sfx_replace(openpete_mod_t* self, const char* name,
                              const void* body, uint32_t len,
                              uint16_t pitch) {
    if (!name || !body || !len || len > (1u << 20) ||
        strlen(name) >= SFX_NAME_MAX)
        return -1;

    sfx_replace_t* r = NULL;
    for (int i = 0; i < s_repl_count; i++)
        if (strcmp(s_repl[i].name, name) == 0) { r = &s_repl[i]; break; }
    if (!r) {
        if (s_repl_count >= SFX_REPLACE_MAX) return -1;
        r = &s_repl[s_repl_count];
        snprintf(r->name, sizeof r->name, "%s", name);
        s_repl_count++;
    }

    void* copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, body, len);
    free(r->body);
    r->body = copy;
    r->len = len;
    r->pitch = pitch;
    r->staged_addr = 0;   /* (re)stage lazily at next key-on */
    snprintf(r->owner, sizeof r->owner, "%s",
             openpete_mods_id_of(self) ? openpete_mods_id_of(self) : "?");
    OPL_INFO_F("mods", "sfx: \"%s\" replaced by %s (%u bytes, pitch %#x)",
               name, r->owner, len, pitch);
    return 0;
}

int openpete_mods_sfx_hook(openpete_mod_t* self, const char* name,
                           void (*fn)(openpete_sfx_event_t* ev)) {
    if (!name || !fn || strlen(name) >= SFX_NAME_MAX) return -1;
    if (s_hook_count >= SFX_HOOKS_MAX) return -1;
    sfx_hook_t* h = &s_hooks[s_hook_count++];
    snprintf(h->name, sizeof h->name, "%s", name);
    h->fn = fn;
    snprintf(h->owner, sizeof h->owner, "%s",
             openpete_mods_id_of(self) ? openpete_mods_id_of(self) : "?");
    return 0;
}

static sfx_stat_t* stat_for(const char* name) {
    for (int i = 0; i < s_stat_count; i++)
        if (strcmp(s_stats[i].name, name) == 0) return &s_stats[i];
    if (s_stat_count >= (int)(sizeof s_stats / sizeof s_stats[0]))
        return NULL;
    sfx_stat_t* st = &s_stats[s_stat_count++];
    snprintf(st->name, sizeof st->name, "%s", name);
    return st;
}

int openpete_mods_sfx_on_key(uint32_t spu_addr, uint16_t stock_pitch,
                             uint32_t* out_addr, uint16_t* out_pitch,
                             int* out_suppress) {
    if (!s_resolve || (!s_repl_count && !s_hook_count)) return 0;
    const char* name = s_resolve(spu_addr);
    if (!name) return 0;

    sfx_replace_t* r = NULL;
    for (int i = 0; i < s_repl_count; i++)
        if (strcmp(s_repl[i].name, name) == 0) { r = &s_repl[i]; break; }

    openpete_sfx_event_t ev;
    memset(&ev, 0, sizeof ev);
    ev.name = name;
    ev.pitch = (r && r->pitch) ? r->pitch : stock_pitch;

    sfx_stat_t* st = stat_for(name);
    if (st) {
        op_time_t now = op_now();
        ev.play_index = st->plays;
        ev.ms_since_prev = st->plays
            ? (uint32_t)((now - st->last_ns) / OP_NS_PER_MS) : 0;
        st->plays++;
        st->last_ns = now;
    }

    int hit = 0;
    for (int i = 0; i < s_hook_count; i++) {
        if (strcmp(s_hooks[i].name, name) != 0) continue;
        s_hooks[i].fn(&ev);
        hit = 1;
    }

    if (ev.suppress) { *out_suppress = 1; return 1; }

    if (r) {
        if (!r->staged_addr && s_upload) {
            r->staged_addr = s_upload(r->body, r->len);
            if (r->staged_addr)
                OPL_INFO_F("mods", "sfx: \"%s\" staged at SPU %#x (%u bytes)",
                           name, r->staged_addr, r->len);
        }
        if (!r->staged_addr) {
            OPL_WARN_F("mods", "sfx: \"%s\" staging failed — stock plays",
                       name);
            return 0;
        }
        *out_addr = r->staged_addr;
        *out_pitch = ev.pitch;
        *out_suppress = 0;
        return 1;
    }

    if (hit && ev.pitch != stock_pitch) {
        *out_addr = spu_addr;      /* stock sample, hooked pitch */
        *out_pitch = ev.pitch;
        *out_suppress = 0;
        return 1;
    }
    return 0;
}
