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
    uint32_t addr = s_upload(body, len);
    if (addr)
        OPL_INFO_F("mods", "sfx: %s staged %u bytes at SPU %#x",
                   openpete_mods_id_of(self) ? openpete_mods_id_of(self) : "?",
                   len, addr);
    return addr;
}

/* ── SoundDefinition replacement (sfx_def_replace) ────────────────────
 * For sounds the name overlay can NOT cover: some plays reference a
 * PER-LEVEL SoundDefinition index straight from a model's m_Sounds[]
 * (Spyro's chest breaks) — no stable name exists. The mod registers
 * (model_class, sound_slot, adpcm) ONCE at entry; every level-ready
 * tick the engine stages the body into the fresh SPU bank's tail and
 * asks the game's def-repoint op to resolve the live index and rewrite
 * the def. Guest def words change ⇒ registering makes a mod
 * sim-divergent by design (unlike sfx_replace).
 *
 * This retires two generations of mod-side lifecycle code: an install-
 * hook one-shot (missed reloads), then a camera poll (dead under
 * freecam — CameraUpdate is frozen there). The level-ready driver in
 * mod_registry.c is anchored on the master update and has neither
 * failure mode. */

#define SFX_DEFS_MAX 16

typedef struct {
    uint16_t model_class;
    uint8_t  sound_slot;
    void*    body;
    uint32_t len;
    char     owner[24];
    int      applied_level; /* last level attempted (one try per level) */
} sfx_def_t;

static sfx_def_t s_defs[SFX_DEFS_MAX];
static int       s_def_count;
static openpete_mods_sfx_def_repoint_fn s_def_repoint;

void openpete_mods_sfx_set_def_repoint(openpete_mods_sfx_def_repoint_fn fn) {
    s_def_repoint = fn;
}

int openpete_mods_sfx_def_replace(openpete_mod_t* self,
                                  uint16_t model_class, uint8_t sound_slot,
                                  const void* body, uint32_t len) {
    if (!body || !len || len > (1u << 20)) return -1;
    if (!s_def_repoint) {
        OPL_ERROR_F("mods", "sfx_def_replace: no game def-repoint op "
                    "registered");
        return -1;
    }
    /* rows apply from the registry's level-ready tick — arm its driver */
    if (openpete_mods_registry_ensure_driver("mods") != 0) return -1;
    const char* owner = openpete_mods_id_of(self);

    sfx_def_t* d = NULL;
    for (int i = 0; i < s_def_count; i++)
        if (s_defs[i].model_class == model_class &&
            s_defs[i].sound_slot == sound_slot &&
            strcmp(s_defs[i].owner, owner ? owner : "?") == 0) {
            d = &s_defs[i];
            break;
        }
    if (!d) {
        if (s_def_count >= SFX_DEFS_MAX) return -1;
        d = &s_defs[s_def_count];
        d->model_class = model_class;
        d->sound_slot = sound_slot;
        snprintf(d->owner, sizeof d->owner, "%s", owner ? owner : "?");
        s_def_count++;
    }
    void* copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, body, len);
    free(d->body);
    d->body = copy;
    d->len = len;
    d->applied_level = 0; /* (re)apply at the next level-ready tick —
                           * including the level already in progress */
    OPL_INFO_F("mods", "sfx: def for class %u slot %u replaced by %s "
               "(%u bytes)", model_class, sound_slot, d->owner, len);
    return 0;
}

/* Driven from the registry's level-ready point (master-update tick,
 * gameplay live). One attempt per row per level: a class absent from
 * the level resolves to "nothing to repoint" — not an error, no retry
 * churn. */
void openpete_mods_sfx_defs_apply(CPUState* cpu, int level_id) {
    if (!s_def_count || !s_def_repoint || !s_upload || level_id <= 0)
        return;
    for (int i = 0; i < s_def_count; i++) {
        sfx_def_t* d = &s_defs[i];
        if (d->applied_level == level_id) continue;
        d->applied_level = level_id;
        if (!openpete_mods_is_enabled(d->owner)) continue;
        uint32_t addr = s_upload(d->body, d->len);
        if (!addr) {
            OPL_WARN_F("mods", "sfx: def stage failed (class %u slot %u, "
                       "%s) — stock plays this level", d->model_class,
                       d->sound_slot, d->owner);
            continue;
        }
        if (s_def_repoint(cpu, d->model_class, d->sound_slot, addr) != 0) {
            OPL_DEBUG_F("mods", "sfx: def class %u not present in level "
                        "%d (%s)", d->model_class, level_id, d->owner);
            continue;
        }
        OPL_INFO_F("mods", "sfx: def class %u slot %u -> SPU %#x "
                   "(level %d, %u bytes, %s)", d->model_class,
                   d->sound_slot, addr, level_id, d->len, d->owner);
    }
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
