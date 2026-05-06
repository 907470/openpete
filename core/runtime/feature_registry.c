// core/runtime/feature_registry.c — see feature_registry.h.

#include "feature_registry.h"

#include <stddef.h>

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
                int rc = e->fn(cpu, addr);
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
