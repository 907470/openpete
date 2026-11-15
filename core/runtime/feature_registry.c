// core/runtime/feature_registry.c - see feature_registry.h.

#include "feature_registry.h"

#include "openpete_game_runtime.h"
#include "openpete_pace_log.h"
#include "openpete_profile.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Per-module "hook.<name>" profiler region (--profile-out only). The dispatch
 * fns are the fiber hooks: they run on the game fiber inside the sim tick, and
 * each one is a capture/native-feed wrapper around a call back into the guest
 * renderer, which the feeders time as a guest region "<feature>.srend" nested
 * in this one; the hook's self time is openpete's own work outside its nested
 * regions. Ids are interned on first dispatch of each module and cached by
 * module index. */
#define FR_PROF_MAX 64
static int s_hook_id[FR_PROF_MAX];
static int s_hook_id_init = 0;

/** @brief Return the profiler bucket id of module i's "hook.<name>" timer, interning it on first use.
 *
 * The cache holds FR_PROF_MAX entries and is initialised lazily on the
 * thread that dispatches, so the function is not safe to call concurrently.
 * A null name interns "hook.?".
 * @param i Module index in the registered feature list.
 * @param name The module's name.
 * @return The bucket id, or -1 when i is FR_PROF_MAX or more. */
static int hook_prof_id(size_t i, const char* name) {
    if (i >= FR_PROF_MAX) return -1;
    if (!s_hook_id_init) {
        for (int k = 0; k < FR_PROF_MAX; k++) s_hook_id[k] = -1;
        s_hook_id_init = 1;
    }
    if (s_hook_id[i] < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "hook.%s", name ? name : "?");
        s_hook_id[i] = op_prof_intern(buf);
    }
    return s_hook_id[i];
}

/* Per-module pace-split slot (OP_PACE_SPLIT_*), cached by module index; -1
 * until resolved. */
static signed char s_hook_slot[FR_PROF_MAX];
static int s_hook_slot_init = 0;

/** @brief Return the OP_PACE_SPLIT_* slot module i's hook time is charged to, resolving it from the name on first use.
 *
 * Not safe to call concurrently, as hook_prof_id.
 * @param i Module index in the registered feature list.
 * @param name The module's name.
 * @return TERRAIN for terrain_native, MOBY for moby_native, else HOST. */
static int hook_split_slot(size_t i, const char* name) {
    if (i >= FR_PROF_MAX) return OP_PACE_SPLIT_HOST;
    if (!s_hook_slot_init) {
        for (int k = 0; k < FR_PROF_MAX; k++) s_hook_slot[k] = -1;
        s_hook_slot_init = 1;
    }
    if (s_hook_slot[i] < 0) {
        const char* n = name ? name : "";
        s_hook_slot[i] = (signed char)(strcmp(n, "terrain_native") == 0 ? OP_PACE_SPLIT_TERRAIN
                                     : strcmp(n, "moby_native") == 0    ? OP_PACE_SPLIT_MOBY
                                                                        : OP_PACE_SPLIT_HOST);
    }
    return s_hook_slot[i];
}

/** @brief Run one dispatch entry inside its module's profiler region and pace-split scope.
 *
 * The profiler region is "hook.<name>" (open only while a trace is open).
 * The pace split charges the hook's time, less the scopes nested in it, to
 * the module's slot: terrain_native to TERRAIN, moby_native to MOBY, every
 * other module to HOST.
 * @param cpu Guest CPU state.
 * @param addr The dispatched guest address.
 * @param e The matched entry.
 * @param i The module's index in the feature list.
 * @param name The module's name.
 * @return The entry's return value. */
static int fr_dispatch_timed(CPUState* cpu, uint32_t addr, const feature_dispatch_entry_t* e,
                             size_t i, const char* name) {
    OP_PACE_SPLIT_SCOPE(hook_split_slot(i, name));
    if (!op_prof_on()) return e->fn(cpu, addr);
    const int id = hook_prof_id(i, name);
    const int tok = op_prof_enter(id, 0u);
    const uint64_t t0 = op_prof_now_us();
    const int rc = e->fn(cpu, addr);
    op_prof_exit(tok, id, op_prof_now_us() - t0);
    return rc;
}

/* Resident-overlay probe (feature_registry.h). Registered by a game whose
 * overlays share one vaddr; NULL for a game with no overlays, which keeps
 * every qualified entry inert. */
static uint32_t (*s_overlay_probe)(void);

/** @brief Return the registered game's feature list, or the empty list when its descriptor has none. */
static const openpete_feature_list_t* feature_list(void) {
    static const openpete_feature_list_t k_none = { NULL, 0u };
    const openpete_feature_list_t* l = openpete_game_runtime()->features;
    return l ? l : &k_none;
}

void feature_registry_set_overlay_probe(uint32_t (*fn)(void)) {
    s_overlay_probe = fn;
}

uint32_t (*feature_registry_overlay_probe(void))(void) {
    return s_overlay_probe;
}

void feature_registry_apply_options(const openpete_options_t* opts) {
    const openpete_feature_list_t* fl = feature_list();
    for (size_t i = 0; i < fl->count; i++) {
        const feature_module_t* m = fl->modules[i];
        if (m && m->apply_options) m->apply_options(opts);
    }
}

void feature_registry_shutdown(void) {
    const openpete_feature_list_t* fl = feature_list();
    for (size_t i = 0; i < fl->count; i++) {
        const feature_module_t* m = fl->modules[i];
        if (m && m->shutdown) m->shutdown();
    }
}

/* Address prefilter for feature_registry_dispatch: one bit per word address
 * modulo 2^18 bytes ((addr >> 2) & 0xFFFF), set for every dispatch-table
 * entry. The module list and its tables are const, so the filter is built
 * once, on the first dispatch, on the thread that runs the guest. A clear
 * bit proves no entry has the address; a set bit falls through to the
 * table walk, which decides. */
static uint8_t s_fr_filter[0x10000u / 8u];
static int     s_fr_filter_ready;

/** @brief Bit index of addr in the dispatch prefilter. */
static uint32_t fr_filter_bit(uint32_t addr) { return (addr >> 2) & 0xFFFFu; }

/** @brief Set the prefilter bit of every dispatch-table entry of every module. */
static void fr_filter_build(void) {
    const openpete_feature_list_t* fl = feature_list();
    for (size_t i = 0; i < fl->count; i++) {
        const feature_module_t* m = fl->modules[i];
        if (!m || !m->dispatch_table) continue;
        for (const feature_dispatch_entry_t* e = m->dispatch_table; e->addr != 0u; e++) {
            const uint32_t b = fr_filter_bit(e->addr);
            s_fr_filter[b >> 3] |= (uint8_t)(1u << (b & 7u));
        }
    }
    s_fr_filter_ready = 1;
}

int feature_registry_dispatch(CPUState* cpu, uint32_t addr) {
    if (!s_fr_filter_ready) fr_filter_build();
    const uint32_t fb = fr_filter_bit(addr);
    if (!(s_fr_filter[fb >> 3] & (1u << (fb & 7u)))) return 0;   /* no entry has this address */
    const openpete_feature_list_t* fl = feature_list();
    for (size_t i = 0; i < fl->count; i++) {
        const feature_module_t* m = fl->modules[i];
        if (!m || !m->dispatch_table) continue;
        for (const feature_dispatch_entry_t* e = m->dispatch_table;
             e->addr != 0u; e++) {
            if (e->addr == addr) {
                int rc;
                /* An overlay-qualified entry dispatches only while its own
                 * overlay is resident: the same address is a different
                 * function in every other overlay, and the recompiled body
                 * that reached this gate is that other function's. Skipping
                 * leaves the recompiled body to run, which is what that
                 * function's callers expect. The probe reads guest RAM, so it
                 * runs only here, after the address has already matched. */
                if (e->overlay &&
                    (!s_overlay_probe || s_overlay_probe() != e->overlay))
                    continue;
                rc = fr_dispatch_timed(cpu, addr, e, i, m->name);
                if (rc) return 1;
                /* fn returned 0 = "saw it but want the recompiled body
                 * to run too" (titlescreen-pre-shift pattern). Keep
                 * walking in case another feature also hooks this
                 * addr; the first handler returning nonzero ends it. */
            }
        }
    }
    return 0;
}
