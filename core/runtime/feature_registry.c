// core/runtime/feature_registry.c — see feature_registry.h.

#include "feature_registry.h"

#include "openpete_profile.h"

#include <stddef.h>
#include <stdio.h>

/* Per-module "hook.<name>" trace bucket (--profile-out only). The dispatch fns
 * are the fiber hooks: they run on the game fiber inside the sim tick, and each
 * one is a capture/native-feed wrapper around a call back into the guest
 * renderer — so this bucket is INCLUSIVE of the guest work. The feeders name a
 * matching "<feature>.srend" bucket around that inner call; the difference is
 * openpete's own extraction cost. Ids are interned on first dispatch of each
 * module and cached by module index. */
#define FR_PROF_MAX 64
static int s_hook_id[FR_PROF_MAX];
static int s_hook_id_init = 0;

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

/* Resident-overlay probe (feature_registry.h). Registered by a game whose
 * overlays share one vaddr; NULL for a game with no overlays, which keeps
 * every qualified entry inert. */
static uint32_t (*s_overlay_probe)(void);

void feature_registry_set_overlay_probe(uint32_t (*fn)(void)) {
    s_overlay_probe = fn;
}

void feature_registry_apply_options(const openpete_options_t* opts) {
    for (size_t i = 0; i < openpete_feature_count; i++) {
        const feature_module_t* m = openpete_features[i];
        if (m && m->apply_options) m->apply_options(opts);
    }
}

int feature_registry_dispatch(CPUState* cpu, uint32_t addr) {
    for (size_t i = 0; i < openpete_feature_count; i++) {
        const feature_module_t* m = openpete_features[i];
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
                if (op_prof_on()) {
                    uint64_t t0 = op_prof_now_us();
                    rc = e->fn(cpu, addr);
                    op_prof_accum(hook_prof_id(i, m->name),
                                  op_prof_now_us() - t0);
                } else {
                    rc = e->fn(cpu, addr);
                }
                if (rc) return 1;
                /* fn returned 0 = "saw it but want the recompiled body
                 * to run too" (titlescreen-pre-shift pattern). Keep
                 * walking in case another feature also hooks this
                 * addr — last-handler-wins on the return code. */
            }
        }
    }
    return 0;
}
