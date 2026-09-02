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
static openpete_mods_sfx_canon_fn    s_canon;

void openpete_mods_sfx_set_canon(openpete_mods_sfx_canon_fn fn) { s_canon = fn; }

void openpete_mods_sfx_set_fns(openpete_mods_sfx_resolver_fn resolve,
                               openpete_mods_sfx_upload_fn upload) {
    s_resolve = resolve;
    s_upload = upload;
}

/* Registration-time owner gate. A mod's `enabled` flag is only raised
 * AFTER openpete_mod_entry returns (both at boot and on a deferred first
 * enable), so a bare enabled check would refuse the mod's own entry-time
 * service calls. Entered-and-disabled is the real "off" state; an owner
 * the loader doesn't know (engine-internal caller) is never gated. */
static int sfx_owner_active(const char* owner) {
    if (!owner || !owner[0]) return 1;
    if (openpete_mods_is_enabled(owner)) return 1;
    for (int i = 0, n = openpete_mods_count(); i < n; i++) {
        const char* id = openpete_mods_id(i);
        if (!id || strcmp(id, owner) != 0) continue;
        openpete_mods_menu_info_t info;
        if (!openpete_mods_menu_info(i, &info)) return 0;
        return !info.entered;
    }
    return 1;
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

    if (s_canon) name = s_canon(name); /* alias -> slot name */
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

uint32_t openpete_mods_sfx_stage(openpete_mod_t* self, const void* body,
                                 uint32_t len) {
    /* The ONE free-SPU-tail allocator. Mods that place samples
     * themselves (e.g. SoundDefinition repoints) MUST allocate here —
     * a private tail pointer collides with the overlay's lazy staging
     * (spyro3-gems' chime overwrote spyro3-chests' basket sample: the
     * user heard the collect chime on chest breaks). Addresses die
     * with the level bank (openpete_mods_sfx_bank_reset) — re-stage
     * per level. */
    if (!body || !len || len > (1u << 20) || !s_upload) return 0;
    /* A disabled mod gets no tail space: the allocation is permanent for
     * the bank generation, so handing it out would let an off mod starve
     * the enabled ones (the tail is a few hundred KB, shared). */
    const char* stage_owner = openpete_mods_id_of(self);
    if (!sfx_owner_active(stage_owner)) {
        OPL_DEBUG_F("mods", "sfx: stage refused — %s is disabled",
                    stage_owner ? stage_owner : "?");
        return 0;
    }
    uint32_t addr = s_upload(body, len);
    if (addr)
        OPL_INFO_F("mods", "sfx: %s staged %u bytes at SPU %#x",
                   openpete_mods_id_of(self) ? openpete_mods_id_of(self) : "?",
                   len, addr);
    return addr;
}

int openpete_mods_sfx_hook(openpete_mod_t* self, const char* name,
                           void (*fn)(openpete_sfx_event_t* ev)) {
    if (s_canon && name) name = s_canon(name); /* alias -> slot name */
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

    /* Owner gate at CONSUME time, not registration: a disabled mod keeps
     * its rows (re-enable is instant) but contributes no substitution —
     * neither the replacement sample nor its pitch, and no hook, so a
     * hook-driven pitch rewrite or suppress stops with it. */
    sfx_replace_t* r = NULL;
    for (int i = 0; i < s_repl_count; i++)
        if (strcmp(s_repl[i].name, name) == 0 &&
            openpete_mods_is_enabled(s_repl[i].owner)) {
            r = &s_repl[i];
            break;
        }

    /* Runahead lookahead: the guest-side outcome of this key-on rolls back,
     * but host effects would not — per-sound stats would inflate x(1+N),
     * and mod hooks may host-play audio (mod_sound OpenAL sources are NOT
     * covered by the driver's SPU mute). Return the pure-replacement
     * verdict with NO stats, NO hooks, NO staging; the committed re-run
     * does the real work one tick later. */
    {
        extern int openpete_speculative;
        if (openpete_speculative) {
            if (r && r->staged_addr) {
                *out_addr = r->staged_addr;
                *out_pitch = r->pitch ? r->pitch : stock_pitch;
                *out_suppress = 0;
                return 1;
            }
            return 0;
        }
    }

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
        if (!openpete_mods_is_enabled(s_hooks[i].owner)) continue;
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
