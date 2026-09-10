// core/mods/mod_registry.c — namespaced content registries.
//
// Namespaces: LEVEL SLOTS (a new level id cloned from a donor level) and
// CUSTOM LEVELS (a mod-supplied WAD/OVL served over a slot), plus the
// per-level event hooks both ride.
//
// Inertness: the per-tick chain link is added lazily on the FIRST
// registration — nothing registered means no hook, no guest reads, no
// behavior change. All registry guest writes are gameplay-divergent by
// design (content mods), gated behind mods being loaded at all.

#include "openpete_mods.h"
#include "openpete_log.h"
#include "openpete_str.h"
#include "../host/wav_loader.h" /* op_wav_probe_seconds (custom-level music) */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Chain plumbing (defined in mod_chain.c; deliberately not in the public
 * header — the registry is the one engine-internal chain client). */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
void openpete_mods_base(CPUState* cpu);

#define MOBY_NAME_MAX   48
#define MOBY_OWNER_MAX  64 /* mirrors the loader's MOD_ID_MAX */

static const openpete_moby_ops_t* s_ops = NULL;
static int s_hooked = 0;
static int s_level  = -1;

/* Level id of an in-flight transition INTO a registered slot (0 = none).
 * The transition window is invisible to s_level — the game ops report
 * level 0 for every non-GS_Playing tick — yet it is exactly when the
 * id-keyed loader functions run, so the busy query below needs it. */
static int s_slot_entering = 0;

/* Last level that was LIVE, regardless of the current gamestate. The
 * ops report level 0 for every non-GS_Playing tick — pause, demo
 * rollover, level teardown — which are exactly the windows where the
 * id-keyed loaders may still run against the old id, so residency only
 * moves when a DIFFERENT level goes live. The busy query keys on this,
 * never on the instantaneous playing-state view. */
static int s_resident_level = 0;

/* ── Level-ready event (api->on_level) ───────────────────────────────
 * Fires each hook once per level, on the first master-update tick with
 * gameplay live (level_id > 0): the model table is installed, the CD
 * stream is idle, and — because the anchor is the master update, not a
 * camera function — the event is immune to freecam freezes (the trap
 * that killed two generations of spyro3's per-level SFX re-staging).
 * Tick context: guest reads/writes and call_addr are legal inside. */
#define LEVEL_HOOKS_MAX 32

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    openpete_mod_level_fn fn;
    int  fired_level; /* last level this hook fired for (0 = never) */
    int  disabled;
} level_hook_t;

static level_hook_t s_level_hooks[LEVEL_HOOKS_MAX];
static int          s_level_hook_count = 0;

void openpete_mods_set_moby_ops(const openpete_moby_ops_t* ops) {
    s_ops = ops;
}

/* ── Loaded moby classes ────────────────────────────────────────────────
 * Which classes a level loaded is a fact about guest RAM, but its consumer
 * is a mod's UI section, which runs inside a present where guest access is
 * refused. So the engine walks it at the tick boundary and mods read this
 * copy — the same shape as openpete_mods_set_level. */
static uint16_t s_moby_classes[OPENPETE_MOBY_CLASSES_MAX];
static int      s_moby_class_count;

void openpete_mods_moby_class_census(CPUState* cpu) {
    if (!s_ops || !s_ops->classes_loaded) return;
    int n = s_ops->classes_loaded(cpu, s_moby_classes,
                                  OPENPETE_MOBY_CLASSES_MAX);
    /* -1 means mid-load: keep the last good answer rather than blanking the
     * set every time the player walks through a portal. */
    if (n >= 0) s_moby_class_count = n;
}

int openpete_mods_moby_classes(uint16_t* out, int cap) {
    if (!out || cap <= 0) return s_moby_class_count;
    int n = s_moby_class_count < cap ? s_moby_class_count : cap;
    for (int i = 0; i < n; i++) out[i] = s_moby_classes[i];
    return n;
}

/* The shared per-tick driver hook, added lazily on the FIRST
 * registration in ANY namespace (anchor = the moby ops' master-update
 * address; the game registers both ops structs at boot). */
static void registry_tick(CPUState* cpu);
static void level_slots_tick(CPUState* cpu);
static void custom_tick(CPUState* cpu);
static void counts_tick(CPUState* cpu);
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
 * wraps it). The original update runs first (via base()), so registry
 * work always sees post-update vanilla state. */
static void registry_tick(CPUState* cpu) {
    openpete_mods_base(cpu);
    if (!s_ops) return;

    level_slots_tick(cpu);
    custom_tick(cpu);
    /* After custom_tick, not inside it: the warp path returns early on
     * several branches, and a claim made this tick must reach the tables
     * this tick — the all-gems test reads them before the next one. */
    counts_tick(cpu);

    int lvl = s_ops->level_id(cpu);
    /* Any live level ends an in-flight slot transition: either we arrived
     * (lvl == the slot id) or the load went somewhere else. */
    if (lvl > 0) { s_slot_entering = 0; s_resident_level = lvl; }
    if (lvl != s_level) s_level = lvl;
    if (lvl <= 0) return;

    for (int i = 0; i < s_level_hook_count; i++) {
        level_hook_t* h = &s_level_hooks[i];
        if (h->disabled || h->fired_level == lvl) continue;
        h->fired_level = lvl;
        h->fn(cpu, lvl);
    }
}

/* ── Level-slot namespace (rung 12) ─────────────────────────────────── */

#define LEVEL_SLOTS_MAX 4 /* v1: game ops expose one free slot anyway */

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    char name[MOBY_NAME_MAX];
    int  donor_level_id;
    int  slot_id;  /* 0 = not claimed yet */
    int  disabled; /* live toggle */
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
        if (s->disabled || s->slot_id) continue;
        s->slot_id = s_level_ops->claim_slot(cpu, s->donor_level_id);
        if (s->slot_id)
            OPL_INFO_F("mods", "level slot %s:%s live (id=%d, donor %d)",
                       s->mod_id, s->name, s->slot_id, s->donor_level_id);
    }
}

/* Arm the per-tick driver from OUTSIDE this file (mod_sfx's def rows
 * apply from the level-ready point but register through mod_sfx.c —
 * without this, def rows on a mod with no other registry use would
 * never see a tick). */
int openpete_mods_registry_ensure_driver(const char* log_tag) {
    return ensure_hooked(log_tag);
}

int openpete_mods_on_level(const char* mod_id, openpete_mod_level_fn fn,
                           const char* log_tag) {
    if (!mod_id || !fn) return -1;
    if (s_level_hook_count >= LEVEL_HOOKS_MAX) {
        OPL_ERROR_F(log_tag, "on_level: hook table full (%d)",
                    LEVEL_HOOKS_MAX);
        return -1;
    }
    if (ensure_hooked(log_tag) != 0) return -1;
    level_hook_t* h = &s_level_hooks[s_level_hook_count++];
    snprintf(h->mod_id, sizeof h->mod_id, "%s", mod_id);
    h->fn = fn;
    h->fired_level = 0; /* fires on the next live tick — including for a
                         * level already in progress (hot reload) */
    OPL_INFO_F(log_tag, "level-ready hook registered");
    return 0;
}

/* Hot reload: REMOVE a mod's registry rows (it re-registers fresh). */
void openpete_mods_registry_remove_owner(const char* mod_id) {
    if (!mod_id) return;
    int w = 0;
    for (int i = 0; i < s_level_count; i++) {
        if (strcmp(s_levels[i].mod_id, mod_id) == 0) continue;
        s_levels[w++] = s_levels[i];
    }
    s_level_count = w;
    w = 0;
    for (int i = 0; i < s_level_hook_count; i++) {
        if (strcmp(s_level_hooks[i].mod_id, mod_id) == 0) continue;
        s_level_hooks[w++] = s_level_hooks[i];
    }
    s_level_hook_count = w;
}

/* Live toggle: gate a mod's registry rows (level slots + level hooks).
 *
 * A disabled level slot stops claiming and refuses entry. It does NOT
 * unclaim: the cloned TOC entry and the donor alias are what make the id
 * loadable at all, and the player may be standing in the level — see
 * openpete_mods_registry_owner_busy, which callers must consult before
 * applying a toggle-off. */
void openpete_mods_registry_set_owner_enabled(const char* mod_id, int on) {
    if (!mod_id) return;
    for (int i = 0; i < s_level_count; i++)
        if (strcmp(s_levels[i].mod_id, mod_id) == 0)
            s_levels[i].disabled = !on;
    for (int i = 0; i < s_level_hook_count; i++)
        if (strcmp(s_level_hooks[i].mod_id, mod_id) == 0) {
            s_level_hooks[i].disabled = !on;
            /* Enable = (re)apply: clear the once-per-level latch so the
             * hook fires for the level IN PROGRESS on the next live tick.
             * Level hooks are once-per-EVERY-level by contract, so they
             * are already written idempotent; a re-enabled mod re-runs
             * its per-level work instead of staying silently inert until
             * the next level change. */
            if (on) s_level_hooks[i].fired_level = 0;
        }
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
    /* Entering a slot commits the session to a level its owner defines.
     * With the owner disabled the id still loads — the donor alias that
     * services the id-keyed load functions is engine-owned, not mod-owned
     * — but it arrives stripped of the mod's content and of any load-path
     * hooks the mod installs, i.e. as a bare clone of the donor. Refuse
     * the warp instead of shipping the player a half-level. Retail ids
     * match no slot row and pass straight through. */
    for (int i = 0; i < s_level_count; i++) {
        if (s_levels[i].slot_id != level_id) continue;
        if (s_levels[i].disabled) {
            OPL_ERROR_F("mods", "level %d belongs to disabled mod \"%s\" — "
                        "entry refused", level_id, s_levels[i].mod_id);
            return -1;
        }
        break;
    }
    int rc = s_level_ops->enter_level(cpu, level_id);
    if (rc == 0) {
        for (int i = 0; i < s_level_count; i++)
            if (s_levels[i].slot_id == level_id) {
                s_slot_entering = level_id;
                break;
            }
    }
    return rc;
}

/* Engine-facing toggle guard: 1 while disabling mod_id would strand the
 * session. The one such state is a registered LEVEL SLOT the player is
 * currently inside, or loading into. The engine's donor alias keeps the
 * slot id serviceable whatever the owner's state, but everything that
 * makes the slot ITS level — the owner's chain links on the load path,
 * its level-ready hook, its registered content — stops dead at the flip,
 * leaving the player inside a level the live mod set no longer defines.
 * No on_toggle hook can undo it: a mod cannot un-enter a level from
 * inside it. Callers must refuse or defer the toggle-off until the
 * player has left the slot.
 *
 * Host-side statics only (the level id is the one the per-tick driver
 * last observed), so the menu thread may call it without guest access. */

/* ── Declarative custom levels ([[level]] manifest blocks) ───────────
 * Hand-authored level mods with no C: the loader registers each block
 * here, and this driver owns everything the guest needs — the shared
 * slot (claim/retarget to the row's donor), the guest TOC length
 * declaration for the row's data file, replace-mode declarations on a
 * donor's own entry, and warps. The WAD overlay asks
 * openpete_mods_custom_files_for() which files serve a level id.
 * v1 capacity: rows TIME-SHARE the one free slot (id 65); per-level
 * progress on the shared index is common to all of them until the
 * capacity arc lifts the guest tables. */
#define CUSTOM_MAX 64

typedef struct {
    char mod_id[MOBY_OWNER_MAX];
    char name[64];
    char stem[64];
    char dir[900];         /* owning mod's assets dir */
    int  donor_id;
    int  replace_mode;     /* 1 = serve over the donor's own entry */
    uint32_t data_size;    /* stat at registration; 0 = files missing */
    int  declared_replace; /* donor entry currently carries our length */
    char music[1088];      /* custom song WAV, "" = none */
    double music_sec;      /* its duration; 0 = file missing/bad */
    char title[40];        /* in-game title override; "" = use name */
    openpete_level_counts_t counts; /* declared target totals; a field is
                                     * -1 when the manifest declared none,
                                     * leaving that table on its stock row */
    uint8_t counts_clash_logged;    /* lost a donor to a lower-ordinal row;
                                     * latched so the clash logs once */
} custom_level_t;

/* The target-egg table covers the first three worlds only, so a level
 * index at or above this has no egg row. */
#define EGG_TABLE_ROWS 18

static custom_level_t s_customs[CUSTOM_MAX];
static int s_custom_count = 0;
static int s_custom_order[CUSTOM_MAX]; /* alphabetical by name */
static int s_custom_active = -1;       /* row owning the shared slot */
static int s_custom_slot_id = 0;       /* claimed slot id (65), 0 = none */
static int s_custom_warp = -1;         /* queued warp (real row index) */

/* ── Custom-level asset files ────────────────────────────────────────
 * A declarative level ships two files under <dir>/levels/: <stem>.WAD
 * (the level's WAD data segment) and, optionally, <stem>.OVL (the
 * donor's code overlay). Those are the extensions the level editors
 * emit; the case they emit them in varies (and case matters on Linux
 * but not on Windows), so the lookup matches the name case-insensitively
 * by scanning the directory when the literal lowercase path misses. */
int openpete_mods_custom_asset_path(const char* dir, const char* stem,
                                    int is_ovl, char* out, size_t cap) {
    const char* ext = is_ovl ? "ovl" : "wad";
    if (!dir || !stem || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/levels/%s.%s", dir, stem, ext);
    struct stat st;
    if (stat(out, &st) == 0) return 0;

    char want[160];
    snprintf(want, sizeof want, "%s.%s", stem, ext);
    char ldir[1024];
    snprintf(ldir, sizeof ldir, "%s/levels", dir);
    DIR* d = opendir(ldir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            if (op_strcasecmp(e->d_name, want) != 0) continue;
            snprintf(out, cap, "%s/%s", ldir, e->d_name);
            closedir(d);
            return 0;
        }
        closedir(d);
    }
    /* Canonical (missing) path stays in `out` for the caller's log. */
    snprintf(out, cap, "%s/levels/%s.%s", dir, stem, ext);
    return -1;
}

int openpete_mods_custom_register(const char* mod_id, const char* name,
                                  const char* stem, const char* dir,
                                  const char* donor, int replace,
                                  const char* music, const char* title,
                                  const openpete_level_counts_t* counts,
                                  const char* log_tag) {
    if (!mod_id || !name || !name[0] || !stem || !stem[0] || !dir)
        return -1;
    if (s_custom_count >= CUSTOM_MAX) {
        OPL_ERROR_F(log_tag, "custom-level table full (%d)", CUSTOM_MAX);
        return -1;
    }
    int donor_id = 0;
    if (s_level_ops && s_level_ops->resolve_name)
        donor_id = s_level_ops->resolve_name(donor ? donor : "");
    if (donor_id < 10 || donor_id > 64) {
        OPL_ERROR_F(log_tag, "level \"%s\": donor \"%s\" is not a retail "
                    "level (lower_snake name or id) — skipped", name,
                    donor ? donor : "");
        return -1;
    }
    custom_level_t* r = &s_customs[s_custom_count];
    snprintf(r->mod_id, sizeof r->mod_id, "%s", mod_id);
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->stem, sizeof r->stem, "%s", stem);
    snprintf(r->dir, sizeof r->dir, "%s", dir);
    r->donor_id = donor_id;
    r->replace_mode = replace ? 1 : 0;
    r->declared_replace = 0;
    r->counts.dragons = r->counts.gems = r->counts.eggs = -1;
    if (counts) {
        r->counts = *counts;
        /* An egg total needs a row in the 18-entry table. A replace row
         * takes the donor's index; a slot row lands on the shared free
         * slot, which is past the table entirely. Drop the declaration
         * rather than carry a value nothing can display — the row still
         * registers, and its gem and dragon totals still apply. */
        int idx = replace ? (donor_id / 10 - 1) * 6 + donor_id % 10 : -1;
        if (r->counts.eggs >= 0 && (idx < 0 || idx >= EGG_TABLE_ROWS)) {
            if (idx < 0)
                OPL_WARN_F(log_tag, "level \"%s\": eggs = %d dropped — a "
                           "slot-mode level has no row in the %d-entry "
                           "egg table", name, r->counts.eggs,
                           EGG_TABLE_ROWS);
            else
                OPL_WARN_F(log_tag, "level \"%s\": eggs = %d dropped — "
                           "donor index %d is outside the %d-entry egg "
                           "table", name, r->counts.eggs, idx,
                           EGG_TABLE_ROWS);
            r->counts.eggs = -1;
        }
    }
    char p[1088];
    struct stat st;
    r->data_size = openpete_mods_custom_asset_path(dir, stem, 0, p, sizeof p)
                   == 0 && stat(p, &st) == 0 ? (uint32_t)st.st_size : 0;
    if (!r->data_size)
        OPL_WARN_F(log_tag, "level \"%s\": %s missing — listed but not "
                   "playable until derived", name, p);
    snprintf(r->title, sizeof r->title, "%s",
             title && title[0] ? title : "");
    r->music[0] = '\0';
    r->music_sec = 0.0;
    if (music && music[0]) {
        snprintf(r->music, sizeof r->music, "%s/music/%s", dir, music);
        r->music_sec = op_wav_probe_seconds(r->music);
        if (r->music_sec <= 0.0) {
            OPL_WARN_F(log_tag, "level \"%s\": music %s missing/unreadable "
                       "— donor's track plays until derived", name,
                       r->music);
            r->music[0] = '\0';
        }
    }
    int row = s_custom_count++;
    /* The per-tick driver does all the work — make sure it's armed. */
    (void)ensure_hooked(log_tag);
    /* Alphabetical order (the menu's Custom World buckets). */
    int at = row;
    for (int i = 0; i < row; i++)
        if (op_strcasecmp(s_customs[s_custom_order[i]].name, name) > 0) {
            at = i;
            break;
        }
    for (int i = row; i > at; i--) s_custom_order[i] = s_custom_order[i - 1];
    s_custom_order[at] = row;
    OPL_INFO_F(log_tag, "custom level \"%s\" registered (donor %d%s)",
               name, donor_id, replace ? ", replace" : "");
    return 0;
}

int openpete_mods_custom_count(void) { return s_custom_count; }

int openpete_mods_custom_info(int i, openpete_mods_custom_info_t* out) {
    if (i < 0 || i >= s_custom_count || !out) return 0;
    const custom_level_t* r = &s_customs[s_custom_order[i]];
    out->name = r->name;
    out->owner = r->mod_id;
    out->donor_id = r->donor_id;
    out->replace_mode = r->replace_mode;
    out->active = s_custom_active == s_custom_order[i];
    out->level_id = r->replace_mode ? r->donor_id
                  : (out->active ? s_custom_slot_id : 0);
    out->files_ok = r->data_size != 0;
    return 1;
}

int openpete_mods_custom_warp(int i) {
    if (i < 0 || i >= s_custom_count) return -1;
    s_custom_warp = s_custom_order[i];
    return 0;
}

void openpete_mods_custom_set_mode(int i, int replace) {
    if (i < 0 || i >= s_custom_count) return;
    s_customs[s_custom_order[i]].replace_mode = replace ? 1 : 0;
}

int openpete_mods_custom_files_for(int level_id, const char** dir_out,
                                   const char** stem_out) {
    if (s_custom_slot_id && level_id == s_custom_slot_id &&
        s_custom_active >= 0) {
        const custom_level_t* r = &s_customs[s_custom_active];
        if (openpete_mods_is_enabled(r->mod_id)) {
            *dir_out = r->dir;
            *stem_out = r->stem;
            return 1;
        }
    }
    for (int i = 0; i < s_custom_count; i++) {
        const custom_level_t* r = &s_customs[i];
        if (r->replace_mode && r->declared_replace &&
            r->donor_id == level_id &&
            openpete_mods_is_enabled(r->mod_id)) {
            *dir_out = r->dir;
            *stem_out = r->stem;
            return 1;
        }
    }
    return 0;
}

const char* openpete_mods_custom_music_for(int level_id,
                                           double* dur_sec_out) {
    const custom_level_t* r = NULL;
    if (s_custom_slot_id && level_id == s_custom_slot_id &&
        s_custom_active >= 0)
        r = &s_customs[s_custom_active];
    else
        for (int i = 0; i < s_custom_count && !r; i++)
            if (s_customs[i].replace_mode && s_customs[i].declared_replace &&
                s_customs[i].donor_id == level_id)
                r = &s_customs[i];
    if (!r || !r->music[0] || !openpete_mods_is_enabled(r->mod_id))
        return NULL;
    if (dur_sec_out) *dur_sec_out = r->music_sec;
    return r->music;
}

/* Test/dev driver: OPENPETE_CUSTOM_WARP="tick:Level Name" queues a warp
 * into the named row at the Nth registry tick. */
static void custom_env_schedule(unsigned tick_no) {
    static int s_parsed = 0;
    static unsigned s_at = 0;
    static char s_name[64];
    if (!s_parsed) {
        s_parsed = 1;
        const char* e = getenv("OPENPETE_CUSTOM_WARP");
        if (e && *e) {
            const char* c = strchr(e, ':');
            if (c) {
                s_at = (unsigned)atoi(e);
                snprintf(s_name, sizeof s_name, "%s", c + 1);
            }
        }
    }
    if (!s_name[0] || tick_no != s_at) return;
    for (int i = 0; i < s_custom_count; i++)
        if (strcmp(s_customs[i].name, s_name) == 0) {
            s_custom_warp = i;
            OPL_INFO_F("mods", "custom warp scheduled: \"%s\"", s_name);
        }
}

/* A row is live when its data file was found and its mod is enabled.
 * Mode-independent, unlike custom_tick's `want`, which also asks whether
 * the row is currently a replace row. */
static int custom_row_live(const custom_level_t* r) {
    return r->data_size && openpete_mods_is_enabled(r->mod_id);
}

/* Bytes the count reconcile has written this session — the engagement
 * counter a gate needs to tell a passing run from one that never ran. */
static unsigned long s_counts_writes = 0;

/* Which registered row, if any, owns a level's target totals this tick.
 *
 * Every slot-mode row resolves to the SAME shared-slot index, so the
 * owner there is the row that actually holds the slot, not every row
 * that could hold it; without that distinction two enabled slot rows
 * would both claim the index and loop order would decide what the
 * player sees.
 *
 * Two live replace rows on one donor is a conflict between manifests.
 * The lowest-ordinal row wins, so the choice is stable across ticks
 * rather than dependent on iteration order, and the clash is logged once
 * per losing row instead of every tick. */
static int counts_declarer_for(int level_id) {
    if (s_custom_slot_id && level_id == s_custom_slot_id) {
        if (s_custom_active < 0) return -1;
        const custom_level_t* a = &s_customs[s_custom_active];
        if (a->replace_mode || !custom_row_live(a)) return -1;
        return s_custom_active;
    }
    int found = -1;
    for (int i = 0; i < s_custom_count; i++) {
        custom_level_t* r = &s_customs[i];
        if (!r->replace_mode || r->donor_id != level_id) continue;
        if (!custom_row_live(r)) continue;
        if (found < 0) { found = i; continue; }
        if (!r->counts_clash_logged) {
            r->counts_clash_logged = 1;
            OPL_WARN_F("mods", "custom level \"%s\" and \"%s\" both "
                       "replace level %d — \"%s\" declares its target "
                       "totals", s_customs[found].name, r->name,
                       level_id, s_customs[found].name);
        }
    }
    return found;
}

/* Reconcile the target totals of every level index a registered row
 * could own. Idempotent: the op writes only bytes that differ, so this
 * costs three guest reads per index once the tables agree.
 *
 * Reconciling beats writing on the edges where rows go live and stop
 * being live. The shared slot has no undeclare edge at all, a mode flip
 * from the Mods panel is unguarded, and a savestate load replaces the
 * tables with no edge firing in either direction; a rule evaluated every
 * tick has none of those holes, and needs no hook on the state path. */
static void counts_tick(CPUState* cpu) {
    if (!s_level_ops || !s_level_ops->set_level_counts) return;

    int ids[CUSTOM_MAX + 1];
    int n = 0;
    if (s_custom_slot_id) ids[n++] = s_custom_slot_id;
    for (int i = 0; i < s_custom_count; i++) {
        int id = s_customs[i].donor_id;
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (ids[j] == id) { dup = 1; break; }
        if (!dup) ids[n++] = id;
    }

    for (int k = 0; k < n; k++) {
        int d = counts_declarer_for(ids[k]);
        const openpete_level_counts_t* c = d >= 0 ? &s_customs[d].counts
                                                  : NULL;
        int wrote = s_level_ops->set_level_counts(
            cpu, ids[k], c ? c->dragons : -1, c ? c->gems : -1,
            c ? c->eggs : -1);
        if (wrote > 0) {
            s_counts_writes += (unsigned long)wrote;
            /* Only an ownership change reaches this: the op writes
             * nothing once the tables hold what the rule asks for. A
             * line per tick would mean that idempotence had broken. */
            OPL_INFO_F("mods", "level %d target totals: %s (dragons=%d "
                       "gems=%d eggs=%d, %d B, %lu total)", ids[k],
                       c ? s_customs[d].name : "stock",
                       c ? c->dragons : -1, c ? c->gems : -1,
                       c ? c->eggs : -1, wrote, s_counts_writes);
        }
    }
}

unsigned long openpete_mods_registry_counts_writes(void);
unsigned long openpete_mods_registry_counts_writes(void) {
    return s_counts_writes;
}

/* Per-tick driver: keep replace declarations in sync with each row's
 * mode + owner toggle, then consume a queued warp. */
static void custom_tick(CPUState* cpu) {
    static unsigned s_tickno = 0;
    custom_env_schedule(++s_tickno);
    if (!s_level_ops || !s_custom_count) return;

    for (int i = 0; i < s_custom_count; i++) {
        custom_level_t* r = &s_customs[i];
        int want = r->data_size && r->replace_mode &&
                   openpete_mods_is_enabled(r->mod_id);
        if (want && !r->declared_replace) {
            if (s_level_ops->declare_data_len &&
                s_level_ops->declare_data_len(cpu, r->donor_id,
                                              r->data_size) == 0) {
                r->declared_replace = 1;
                if (s_level_ops->set_level_name)
                    (void)s_level_ops->set_level_name(
                        cpu, r->donor_id,
                        r->title[0] ? r->title : r->name);
                OPL_INFO_F("mods", "custom level \"%s\": replacing level "
                           "%d (data %u)", r->name, r->donor_id,
                           r->data_size);
            }
        } else if (!want && r->declared_replace) {
            if (s_level_ops->declare_data_len &&
                s_level_ops->declare_data_len(cpu, r->donor_id, 0) == 0) {
                r->declared_replace = 0;
                if (s_level_ops->set_level_name)
                    (void)s_level_ops->set_level_name(cpu, r->donor_id,
                                                      NULL);
                OPL_INFO_F("mods", "custom level \"%s\": level %d back "
                           "to stock", r->name, r->donor_id);
            }
        }
        /* Portal preview: while the replace is declared, the donor's
         * homeworld portal shows the custom level's own sky. */
        if (r->replace_mode && s_level_ops->replace_sky_tick)
            (void)s_level_ops->replace_sky_tick(cpu, r->donor_id, r->dir,
                                                r->stem,
                                                r->declared_replace);
    }

    if (s_custom_warp < 0) return;
    custom_level_t* r = &s_customs[s_custom_warp];
    if (!r->data_size || !openpete_mods_is_enabled(r->mod_id)) {
        s_custom_warp = -1;
        return;
    }
    if (r->replace_mode) {
        if (s_level_ops->enter_level(cpu, r->donor_id) == 0)
            s_custom_warp = -1;
        return; /* refused = not in gameplay yet; retry next tick */
    }
    if (!s_custom_slot_id) {
        int got = s_level_ops->claim_slot(cpu, r->donor_id);
        if (!got) return; /* TOC not read yet, or the slot is another
                           * mod's — keep retrying quietly */
        s_custom_slot_id = got;
    } else if (s_custom_active != (int)(r - s_customs)) {
        if (!s_level_ops->retarget_slot ||
            s_level_ops->retarget_slot(cpu, r->donor_id) != 0) {
            s_custom_warp = -1;
            return;
        }
    }
    s_custom_active = (int)(r - s_customs);
    if (s_level_ops->declare_data_len)
        (void)s_level_ops->declare_data_len(cpu, s_custom_slot_id,
                                            r->data_size);
    if (s_level_ops->set_level_name)
        (void)s_level_ops->set_level_name(cpu, s_custom_slot_id,
                                          r->title[0] ? r->title
                                                      : r->name);
    if (s_level_ops->enter_level(cpu, s_custom_slot_id) == 0) {
        s_slot_entering = s_custom_slot_id;
        s_custom_warp = -1;
        OPL_INFO_F("mods", "custom level \"%s\" entering (id %d, donor "
                   "%d)", r->name, s_custom_slot_id, r->donor_id);
    }
}

int openpete_mods_registry_owner_busy(const char* mod_id);
int openpete_mods_registry_owner_busy(const char* mod_id) {
    if (!mod_id) return 0;
    for (int i = 0; i < s_level_count; i++) {
        const level_slot_t* s = &s_levels[i];
        if (!s->slot_id || strcmp(s->mod_id, mod_id) != 0) continue;
        if (s_resident_level == s->slot_id ||
            s_slot_entering == s->slot_id)
            return 1;
    }
    if (s_custom_active >= 0 && s_custom_slot_id &&
        strcmp(s_customs[s_custom_active].mod_id, mod_id) == 0 &&
        (s_resident_level == s_custom_slot_id ||
         s_slot_entering == s_custom_slot_id))
        return 1;
    return 0;
}

int openpete_mods_read_level_data(int level_id, void* buf, uint32_t cap) {
    if (!s_level_ops || !s_level_ops->read_level_data || level_id <= 0) return -1;
    return s_level_ops->read_level_data(level_id, buf, cap);
}
