// core/mods/mod_sites.c — the instruction-site hook tier (PoC rung 14).
//
// The escape hatch of notes/plans/openpete-modding-framework.md: mid-
// function tweaks that have no function-boundary expression (the practice
// ROM's instruction-granular patch class). A mod declares [[site]] blocks
// in mod.toml; tools/gen_mod_sites.py bakes them into the recompiled C as
// GUARDED substitutions (see ChannelSite::ModReplace, code_generator.h):
//
//   if (openpete_modsite_active[idx]) openpete_modsite_call(cpu, idx);
//   else { <verbatim instruction translation> }
//
// This file owns both symbols plus the (mod_id, name) → idx registry the
// loader resolves attachments through. Inertness ladder:
//   - site set empty            → regen emits stock C (byte-identical)
//   - baked, mod not loaded     → active[idx] == 0 → verbatim translation
//   - baked, mod toggled off    → active[idx] == 0 → verbatim translation
//   - baked, handler registered → the mod's handler runs at the site
// The middle tier is what the oracle gate proves for a baked binary.

#include "openpete_mods.h"
#include "openpete_log.h"

#include <string.h>

/* Read by the generated C on every baked-site execution — keep it a plain
 * byte array (one load + branch when inactive). */
uint8_t openpete_modsite_active[OPENPETE_MODSITE_MAX];

typedef void (*modsite_fn)(CPUState* cpu, uint32_t site_addr);

static modsite_fn s_fns[OPENPETE_MODSITE_MAX];
static uint32_t   s_addrs[OPENPETE_MODSITE_MAX];

/* Live-toggle state per baked index: 1 = the owning mod is toggled OFF.
 * Deliberately NOT cleared by detach — a hot reload of a disabled mod
 * sweeps its handlers and re-runs entry, and the re-attach must land in
 * the state the user last chose, not re-arm the site behind their back. */
static uint8_t s_off[OPENPETE_MODSITE_MAX];

static const openpete_modsite_desc_t* s_registry = NULL;
static int s_registry_count = 0;

void openpete_mods_set_site_registry(const openpete_modsite_desc_t* table,
                                     int count) {
    s_registry = table;
    s_registry_count = count;
    if (count > 0)
        OPL_INFO_F("mods", "site registry: %d baked instruction site(s)",
                   count);
}

void openpete_modsite_call(CPUState* cpu, int idx) {
    /* Every writer of active[]/s_fns[] runs at the tick boundary with the
     * sim quiescent, so the pair cannot disagree while guest code runs and
     * a site cannot change arm-state between two executions within one
     * tick. The NULL check is defensive: skipping the call also skips the
     * guarded else-arm for that one execution. */
    if ((unsigned)idx < OPENPETE_MODSITE_MAX && s_fns[idx])
        s_fns[idx](cpu, s_addrs[idx]);
}

int openpete_mods_site_attach(const char* mod_id, const char* name,
                              void (*fn)(CPUState* cpu, uint32_t site_addr),
                              const char* log_tag) {
    if (!mod_id || !name || !fn) return -1;
    /* Entry-time only. An attach from inside an override chain frame is
     * mid-tick: it would arm the site between two executions of the same
     * instruction within one tick, which no consumer of the guarded
     * emission is allowed to observe. */
    if (openpete_mods_crash_attribution(NULL)) {
        OPL_ERROR_F(log_tag, "site \"%s\": attach refused mid-tick — "
                    "site_handler is entry-time only", name);
        return -1;
    }
    for (int i = 0; i < s_registry_count; i++) {
        const openpete_modsite_desc_t* d = &s_registry[i];
        if (!d->mod_id || !d->name) continue;
        if (strcmp(d->mod_id, mod_id) != 0 || strcmp(d->name, name) != 0)
            continue;
        if (d->idx < 0 || d->idx >= OPENPETE_MODSITE_MAX) {
            OPL_ERROR_F(log_tag, "site \"%s\": baked index %d out of range",
                        name, d->idx);
            return -1;
        }
        if (s_fns[d->idx] && s_fns[d->idx] != fn) {
            OPL_ERROR_F(log_tag, "site \"%s\": handler already attached",
                        name);
            return -1;
        }
        s_fns[d->idx]   = fn;
        s_addrs[d->idx] = d->addr;
        /* Arm unless the owner is currently toggled off (re-entry after a
         * hot reload, or an attach that lands after a disable). */
        openpete_modsite_active[d->idx] = s_off[d->idx] ? 0 : 1;
        OPL_INFO_F(log_tag, "site \"%s\" %s @ 0x%08X (idx %d)", name,
                   s_off[d->idx] ? "attached (mod disabled)" : "live",
                   d->addr, d->idx);
        return 0;
    }
    OPL_ERROR_F(log_tag,
                "site \"%s\" is not baked into this binary — sites bake from "
                "the mods listed in mods/enabled.txt: add the mod there, run "
                "scripts/regen-mod-sites.sh --build, and restart; the site "
                "stays inert this run", name);
    return -1;
}

/* Live toggle: gate owned sites on/off (handlers stay attached — the
 * guarded emission's identity arm covers the off state, so a disabled
 * site executes the verbatim instruction translation and is byte-
 * identical to stock). Keyed on the baked registry rather than on the
 * attached set, so the state is recorded even for a mod whose handlers
 * are not attached yet (deferred entry) or were swept (hot reload). */
void openpete_mods_site_set_enabled(const char* mod_id, int on) {
    if (!mod_id) return;
    for (int i = 0; i < s_registry_count; i++) {
        const openpete_modsite_desc_t* d = &s_registry[i];
        if (!d->mod_id || strcmp(d->mod_id, mod_id) != 0) continue;
        if (d->idx < 0 || d->idx >= OPENPETE_MODSITE_MAX) continue;
        s_off[d->idx] = on ? 0 : 1;
        openpete_modsite_active[d->idx] = (on && s_fns[d->idx]) ? 1 : 0;
    }
}

/* Sweep the mod's handlers (unload/reload). The toggle state survives:
 * re-entry re-attaches into it. */
void openpete_mods_site_detach_all(const char* mod_id) {
    if (!mod_id) return;
    for (int i = 0; i < s_registry_count; i++) {
        const openpete_modsite_desc_t* d = &s_registry[i];
        if (!d->mod_id || strcmp(d->mod_id, mod_id) != 0) continue;
        if (d->idx < 0 || d->idx >= OPENPETE_MODSITE_MAX) continue;
        openpete_modsite_active[d->idx] = 0;
        s_fns[d->idx] = NULL;
    }
}
