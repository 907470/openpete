// games/spyro-1/dispatch_chain.c — Phase 2 overlay-aware dispatch.
//
// `psx_dispatch_compiled` fans out to per-overlay dispatchers, but only the
// overlay whose bytes are actually in PSX RAM at vaddr 0x8007AA38 may match.
// The previous "first hit wins" chain ran level_10 code from the title-screen
// (whenever a function pointer happened to land in level_10's address range)
// even though only the titlescreen overlay had been loaded.
//
// Active-overlay tracking: psyq_CdRead reads the first word at 0x8007AA38
// after each disc transfer; LEVEL_BALLOON's overlay-id (0x0B for level_10,
// 0x30 for titlescreen) tells us which overlay's bytes are now in RAM.
// Dispatch consults the active slot and skips the others.
//
// The OT bucket array (`m_WorldOTStart = m_SharedAnimations - 0x6000`) lands
// inside the overlay region in our build, so live RAM at 0x8007AA38 gets
// stomped by the world OT every frame. Jump-table dispatch used to read
// table entries out of that RAM and stub-miss on garbage; psxrecomp now
// bakes table targets into the C source (see plans/psxrecomp-jumptable-baking.md),
// so the loaded value is ignored at jr and dispatch is corruption-immune.
// No shadow buffer is needed.

#include "psx_runtime.h"
#include "openpete_log.h"

#include <stdint.h>

extern int psx_dispatch_main_exe   (CPUState* cpu, uint32_t addr);
extern int psx_dispatch_titlescreen(CPUState* cpu, uint32_t addr);
extern int psx_dispatch_level_10   (CPUState* cpu, uint32_t addr);

typedef enum {
    SPYRO_OVERLAY_NONE = 0,
    SPYRO_OVERLAY_TITLESCREEN,
    SPYRO_OVERLAY_LEVEL_10,
} spyro_overlay_t;

static spyro_overlay_t s_active_overlay = SPYRO_OVERLAY_NONE;

void spyro_set_active_overlay(int which) {
    spyro_overlay_t now = (spyro_overlay_t)which;
    if (now == s_active_overlay) return;
    static const char* names[] = {
        "none", "titlescreen", "level_10",
    };
    const char* prev = s_active_overlay < 3 ? names[s_active_overlay] : "?";
    const char* curr = now             < 3 ? names[now]              : "?";
    OPL_INFO_F("dispatch", "active overlay: %s -> %s", prev, curr);
    s_active_overlay = now;
}

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;

    switch (s_active_overlay) {
    case SPYRO_OVERLAY_TITLESCREEN:
        if (psx_dispatch_titlescreen(cpu, addr)) return 1;
        break;
    case SPYRO_OVERLAY_LEVEL_10:
        if (psx_dispatch_level_10(cpu, addr)) return 1;
        break;
    case SPYRO_OVERLAY_NONE:
    default:
        break;
    }
    return 0;
}
