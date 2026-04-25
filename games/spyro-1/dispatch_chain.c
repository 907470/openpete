// games/spyro-1/dispatch_chain.c — Phase 2 overlay-aware dispatch.
//
// `psx_dispatch_compiled` fans out to per-overlay dispatchers, but only the
// overlay whose bytes are actually in PSX RAM at vaddr 0x8007AA38 may match.
// The previous "first hit wins" chain ran level_10 code from the title-screen
// (whenever a function pointer happened to land in level_10's address range)
// even though only the titlescreen overlay had been loaded — translated code
// then read its jump tables out of stale RAM and stub-missed on garbage.
//
// Active-overlay tracking: psyq_CdRead reads the first word at 0x8007AA38
// after each disc transfer; LEVEL_BALLOON's overlay-id (0x0B for level_10,
// 0x30 for titlescreen) tells us which overlay's bytes are now in RAM.
// Dispatch consults the active slot and skips the others.
//
// Shadow buffer: cutscene-style scratch reads + the OT (m_WorldOTStart =
// m_SharedAnimations - 0x6000, which lands inside the overlay region in
// our build) clobber level_10's bytes after the load. Snapshot the bytes
// when the overlay activates, then re-restore on every dispatch into the
// overlay so jump-table reads see clean data. The restore is a no-op when
// the bytes already match, so steady-state cost is one memcmp per call.

#include "psx_runtime.h"
#include "openpete_log.h"
#include "openpete_psx.h"

#include <stdint.h>
#include <string.h>

extern int psx_dispatch_main_exe   (CPUState* cpu, uint32_t addr);
extern int psx_dispatch_titlescreen(CPUState* cpu, uint32_t addr);
extern int psx_dispatch_level_10   (CPUState* cpu, uint32_t addr);

typedef enum {
    SPYRO_OVERLAY_NONE = 0,
    SPYRO_OVERLAY_TITLESCREEN,
    SPYRO_OVERLAY_LEVEL_10,
} spyro_overlay_t;

#define OVERLAY_VADDR  0x8007AA38u
#define OVERLAY_OFFSET 0x7AA38u  /* OVERLAY_VADDR & 0x1FFFFFFF */
/* level_10's .ovl is 0xE000 bytes; titlescreen is 0x3800. Allocate the
 * larger so the same buffer serves both. */
#define OVERLAY_MAX    0xE000u

static spyro_overlay_t s_active_overlay = SPYRO_OVERLAY_NONE;
static uint8_t  s_shadow[OVERLAY_MAX];
static uint32_t s_shadow_size = 0;

static void overlay_snapshot(spyro_overlay_t which) {
    uint8_t* ram = openpete_psx_get_ram();
    uint32_t size = (which == SPYRO_OVERLAY_LEVEL_10) ? 0xE000u
                  : (which == SPYRO_OVERLAY_TITLESCREEN) ? 0x3800u
                  : 0u;
    if (!size) {
        s_shadow_size = 0;
        return;
    }
    memcpy(s_shadow, ram + OVERLAY_OFFSET, size);
    s_shadow_size = size;
}

void spyro_set_active_overlay(int which) {
    spyro_overlay_t now = (spyro_overlay_t)which;
    if (now == s_active_overlay) {
        /* Same overlay reloaded — refresh the shadow regardless. */
        overlay_snapshot(now);
        return;
    }
    static const char* names[] = {
        "none", "titlescreen", "level_10",
    };
    const char* prev = s_active_overlay < 3 ? names[s_active_overlay] : "?";
    const char* curr = now             < 3 ? names[now]              : "?";
    OPL_INFO_F("dispatch", "active overlay: %s -> %s", prev, curr);
    s_active_overlay = now;
    overlay_snapshot(now);
}

/* Restore the overlay region from the shadow if it has been clobbered.
 * Cheap-out path: the bytes that read as POLY-stride OT garbage have
 * different first-16 — bail fast when the prefix already matches. */
static void overlay_restore_if_needed(void) {
    if (s_shadow_size == 0) return;
    uint8_t* ram = openpete_psx_get_ram();
    if (memcmp(ram + OVERLAY_OFFSET, s_shadow, 16) == 0) return;
    memcpy(ram + OVERLAY_OFFSET, s_shadow, s_shadow_size);
}

int psx_dispatch_compiled(CPUState* cpu, uint32_t addr) {
    if (psx_dispatch_main_exe(cpu, addr)) return 1;

    switch (s_active_overlay) {
    case SPYRO_OVERLAY_TITLESCREEN:
        overlay_restore_if_needed();
        if (psx_dispatch_titlescreen(cpu, addr)) return 1;
        break;
    case SPYRO_OVERLAY_LEVEL_10:
        overlay_restore_if_needed();
        if (psx_dispatch_level_10(cpu, addr)) return 1;
        break;
    case SPYRO_OVERLAY_NONE:
    default:
        break;
    }
    return 0;
}
