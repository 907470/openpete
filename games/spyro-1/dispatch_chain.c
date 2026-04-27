// games/spyro-1/dispatch_chain.c — overlay-aware dispatch.
//
// `psx_dispatch_compiled` fans out to per-binary dispatchers. Only the
// overlay whose bytes are actually in PSX RAM at vaddr 0x8007AA38 may
// match — the others share the same address space, so dispatching them
// concurrently would run code that isn't really there.
//
// Active-overlay tracking: psyq_CdRead reads the first word at
// 0x8007AA38 after each disc transfer and calls
// spyro_set_active_overlay_by_magic(id). The id-to-dispatcher mapping
// lives in the auto-generated spyro_overlay_table.c (driven by
// scripts/regen-recomp.sh from build/wad/*.ovl).

#include "psx_runtime.h"
#include "openpete_log.h"
#include "spyro_overlay_table.h"

#include <stddef.h>
#include <stdint.h>

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
    OPL_INFO_F("dispatch", "active overlay: %s -> %s",
               s_active ? s_active->name : "none", e->name);
    s_active = e;
}

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;
    if (s_active) return s_active->fn(cpu, addr);
    return 0;
}
