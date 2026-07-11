// core/mods/mod_registry.c — namespaced content registries (PoC rungs
// 11-12 of notes/plans/openpete-modding-framework.md).
//
// v1 namespace: MOBY TYPES. A mod registers `modid:name` with a donor
// class and a per-tick update_fn; the registry claims a level-free class
// id (game ops seed the model row from the donor — BattleShip's
// fighter_registry model), dispatches the update_fn for every live
// instance after the game's own update, and re-allocates ids on level
// change (the loader repopulates the model table per level, so ids are
// level-scoped by construction).
//
// Inertness: the per-tick chain link is added lazily on the FIRST
// registration — no registered types means no hook, no guest reads, no
// behavior change. All registry guest writes are gameplay-divergent by
// design (content mods), gated behind mods being loaded at all.

#include "openpete_mods.h"
#include "openpete_log.h"

#include <stdio.h>
#include <string.h>

/* Chain plumbing (defined in mod_chain.c; deliberately not in the public
 * header — the registry is the one engine-internal chain client). */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

#define MOBY_TYPES_MAX  32
#define MOBY_NAME_MAX   48
#define MOBY_OWNER_MAX  64 /* mirrors the loader's MOD_ID_MAX */

typedef struct {
    char                    mod_id[MOBY_OWNER_MAX];
    char                    name[MOBY_NAME_MAX];
    int                     donor_class;
    openpete_moby_update_fn update_fn;
    int                     class_id; /* 0 = not live in this level */
} moby_type_t;

static moby_type_t s_types[MOBY_TYPES_MAX];
static int         s_type_count = 0;

static const openpete_moby_ops_t* s_ops = NULL;
static int s_hooked = 0;
static int s_level  = -1;

void openpete_mods_set_moby_ops(const openpete_moby_ops_t* ops) {
    s_ops = ops;
}

/* The shared per-tick driver hook, added lazily on the FIRST
 * registration in ANY namespace (anchor = the moby ops' master-update
 * address; the game registers both ops structs at boot). */
static void registry_tick(CPUState* cpu);
static void level_slots_tick(CPUState* cpu);
static int ensure_hooked(const char* log_tag) {
    if (s_hooked) return 0;
    if (!s_ops) {
        OPL_ERROR_F(log_tag, "content registry: no game ops registered");
        return -1;
    }
    if (openpete_mods_chain_add(s_ops->update_hook_addr, registry_tick,
                                "content-registry") != 0)
        return -1;
    s_hooked = 1;
    return 0;
}

/* Per-tick registry driver: runs as the DEEPEST link on the game's
 * master-update chain (registered at mod-entry time, so every mod hook
 * wraps it). Order per tick: original update runs first (via base()),
 * then ids are (re)allocated, then each registered type's update_fn
 * walks its live instances — mod behavior always sees post-update
 * vanilla state. */
static void registry_tick(CPUState* cpu) {
    openpete_mods_base(cpu);
    if (!s_ops) return;

    level_slots_tick(cpu);

    int lvl = s_ops->level_id(cpu);
    if (lvl != s_level) {
        /* Level changed: the loader repopulates the model table, so
         * every claimed id is stale (its row may now be a REAL class of
         * the new level). Invalidate; re-claim below once gameplay is
         * live. */
        for (int i = 0; i < s_type_count; i++) s_types[i].class_id = 0;
        s_level = lvl;
    }
    if (lvl <= 0) return;

    for (int i = 0; i < s_type_count; i++) {
        moby_type_t* t = &s_types[i];
        if (!t->class_id) {
            t->class_id = s_ops->alloc_seed_class(cpu, t->donor_class);
            if (t->class_id)
                OPL_INFO_F("mods", "moby type %s:%s live as class %d "
                           "(donor %d, level %d)",
                           t->mod_id, t->name, t->class_id,
                           t->donor_class, lvl);
        }
        if (t->class_id && t->update_fn)
            s_ops->walk_class(cpu, t->class_id, t->update_fn);
    }
}

/* ── Level-slot namespace (rung 12) ─────────────────────────────────── */

#define LEVEL_SLOTS_MAX 4 /* v1: game ops expose one free slot anyway */

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    char name[MOBY_NAME_MAX];
    int  donor_level_id;
    int  slot_id; /* 0 = not claimed yet */
} level_slot_t;

static level_slot_t s_levels[LEVEL_SLOTS_MAX];
static int          s_level_count = 0;
static const openpete_level_ops_t* s_level_ops = NULL;

void openpete_mods_set_level_ops(const openpete_level_ops_t* ops) {
    s_level_ops = ops;
}

/* Lazy claims ride the same per-tick driver as moby types (below). */
static void level_slots_tick(CPUState* cpu) {
    if (!s_level_ops) return;
    for (int i = 0; i < s_level_count; i++) {
        level_slot_t* s = &s_levels[i];
        if (s->slot_id) continue;
        s->slot_id = s_level_ops->claim_slot(cpu, s->donor_level_id);
        if (s->slot_id)
            OPL_INFO_F("mods", "level slot %s:%s live (id=%d, donor %d)",
                       s->mod_id, s->name, s->slot_id, s->donor_level_id);
    }
}

int openpete_mods_moby_register(const char* mod_id, const char* name,
                                int donor_class,
                                openpete_moby_update_fn update_fn,
                                const char* log_tag) {
    if (!mod_id || !name || !name[0]) return -1;
    if (!s_ops) {
        OPL_ERROR_F(log_tag, "moby registry: no game ops registered");
        return -1;
    }
    for (int i = 0; i < s_type_count; i++) {
        if (strcmp(s_types[i].mod_id, mod_id) == 0 &&
            strcmp(s_types[i].name, name) == 0) {
            OPL_ERROR_F(log_tag, "moby type \"%s\" already registered",
                        name);
            return -1;
        }
    }
    if (s_type_count >= MOBY_TYPES_MAX) {
        OPL_ERROR_F(log_tag, "moby type registry full (%d)",
                    MOBY_TYPES_MAX);
        return -1;
    }
    if (ensure_hooked(log_tag) != 0) return -1;
    moby_type_t* t = &s_types[s_type_count++];
    snprintf(t->mod_id, sizeof t->mod_id, "%s", mod_id);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->donor_class = donor_class;
    t->update_fn   = update_fn;
    t->class_id    = 0;
    OPL_INFO_F(log_tag, "moby type \"%s\" registered (donor class %d)",
               name, donor_class);
    return 0;
}

static moby_type_t* type_find(const char* mod_id, const char* name) {
    for (int i = 0; i < s_type_count; i++)
        if (strcmp(s_types[i].mod_id, mod_id) == 0 &&
            strcmp(s_types[i].name, name) == 0)
            return &s_types[i];
    return NULL;
}

int openpete_mods_moby_type_id(const char* mod_id, const char* name) {
    const moby_type_t* t = type_find(mod_id, name);
    return (t && t->class_id) ? t->class_id : -1;
}

uint32_t openpete_mods_moby_spawn(CPUState* cpu, const char* mod_id,
                                  const char* name) {
    const moby_type_t* t = type_find(mod_id, name);
    if (!t || !t->class_id || !s_ops || !cpu) return 0;
    return s_ops->spawn(cpu, t->class_id, t->donor_class);
}

int openpete_mods_level_register(const char* mod_id, const char* name,
                                 int donor_level_id, const char* log_tag) {
    if (!mod_id || !name || !name[0]) return -1;
    if (!s_level_ops) {
        OPL_ERROR_F(log_tag, "level registry: no game ops registered");
        return -1;
    }
    for (int i = 0; i < s_level_count; i++) {
        if (strcmp(s_levels[i].mod_id, mod_id) == 0 &&
            strcmp(s_levels[i].name, name) == 0) {
            OPL_ERROR_F(log_tag, "level slot \"%s\" already registered",
                        name);
            return -1;
        }
    }
    if (s_level_count >= LEVEL_SLOTS_MAX) {
        OPL_ERROR_F(log_tag, "level slot registry full (%d)",
                    LEVEL_SLOTS_MAX);
        return -1;
    }
    if (ensure_hooked(log_tag) != 0) return -1;
    level_slot_t* s = &s_levels[s_level_count++];
    snprintf(s->mod_id, sizeof s->mod_id, "%s", mod_id);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->donor_level_id = donor_level_id;
    s->slot_id = 0;
    OPL_INFO_F(log_tag, "level slot \"%s\" registered (donor level %d)",
               name, donor_level_id);
    return 0;
}

int openpete_mods_level_slot_id(const char* mod_id, const char* name) {
    for (int i = 0; i < s_level_count; i++)
        if (strcmp(s_levels[i].mod_id, mod_id) == 0 &&
            strcmp(s_levels[i].name, name) == 0)
            return s_levels[i].slot_id ? s_levels[i].slot_id : -1;
    return -1;
}

int openpete_mods_level_enter(CPUState* cpu, int level_id) {
    if (!s_level_ops || !cpu || level_id <= 0) return -1;
    return s_level_ops->enter_level(cpu, level_id);
}
