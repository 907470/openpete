// games/common/overlay_dispatch.c — overlay-aware dispatch.
//
// `psx_dispatch_compiled` fans out to per-binary dispatchers. Only the overlay
// whose bytes are actually resident may match — every overlay of a game shares
// one vaddr, so dispatching them concurrently would run code that isn't there.
//
// The id-to-dispatcher mapping lives in the auto-generated
// spyro_overlay_table.c, driven by scripts/regen-recomp.sh from the decomp's
// build/wad/*.ovl.

#include "psx_runtime_internal.h"
#include "spyro_overlay_table.h"

#include <stddef.h>
#include <stdint.h>

/* Local declaration instead of openpete_log.h: this TU is part of the
 * recomp-module source kit, which must stay self-contained (openpete_log.h
 * drags in openpete.h). The level value matches OPL_INFO. */
void openpete_logf(int level, const char* subsystem, const char* fmt, ...);
#define DISPATCH_LOG_INFO 2

extern int psx_dispatch_main_exe(CPUState* cpu, uint32_t addr);

static const spyro_overlay_entry_t* s_active = NULL;

const spyro_overlay_entry_t* spyro_overlay_find(uint32_t magic) {
    for (size_t i = 0; i < spyro_overlay_table_count; ++i) {
        if (spyro_overlay_table[i].magic == magic) {
            return &spyro_overlay_table[i];
        }
    }
    return NULL;
}

void spyro_set_active_overlay_by_magic(uint32_t magic) {
    const spyro_overlay_entry_t* e = spyro_overlay_find(magic);
    if (!e) return;          /* unknown id — leave current active alone */
    if (e == s_active) return;
    openpete_logf(DISPATCH_LOG_INFO, "dispatch", "active overlay: %s -> %s",
                  s_active ? s_active->name : "none", e->name);
    s_active = e;
}

uint32_t spyro_get_active_overlay_magic(void) {
    return s_active ? s_active->magic : 0u;
}

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;
    if (s_active) return s_active->fn(cpu, addr);
    return 0;
}
