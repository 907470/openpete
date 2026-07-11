// core/mods/mod_arena.c — the guest scratch arena (op_guest_alloc; the
// modding framework's P3 on-demand item).
//
// Mods sometimes need data at a GUEST-addressable vaddr — marshalled
// calls whose parameters are guest pointers (the practice ROM stole game
// buffers and expanded RAM for this). openpete owns the memory map, so
// the arena is a host buffer registered as a psx_runtime region in the
// >2MB expansion window: guest reads/writes reach it through the
// standard cpu accessors' region cold path, and the mod gets a typed
// HOST view of the same bytes.
//
// Contracts and caveats (documented, not hidden):
//   - Allocation is a session-lifetime bump (no free): the arena is
//     scratch, not a heap. 1MB v1; raise when a consumer needs more.
//   - NOT part of the engine savestate snapshot (that is canonical 2MB
//     RAM): a mod whose arena data must survive save/load rehydrates it
//     in its savestate-participation callbacks.
//   - Recompiled game sites that canonicalize addresses (mask to 2MB)
//     cannot see arena data — the same per-site limitation every
//     relocated buffer has (see openpete-ram-virtualization.md). The
//     standard accessor paths (cpu->read_*/write_*, api->guest) all
//     resolve it.
//   - Arena data readable by gameplay is divergence, as with any guest
//     write — content mods only; the modset save tag covers the rest.

#include "openpete_mods.h"
#include "openpete_log.h"

#include "psx_runtime.h" /* psx_runtime_register_region */
#include "openpete_psx.h" /* openpete_psx_get_ram — low-window views */

#define ARENA_BASE 0x80700000u /* clear of the no-lod/polybuf windows */
#define ARENA_SIZE (1u * 1024u * 1024u)

static uint8_t  s_arena[ARENA_SIZE];
static uint32_t s_used = 0;
static int      s_state = 0; /* 0 unregistered, 1 ok, -1 failed */

uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   void** host_view, const char* owner) {
    if (host_view) *host_view = NULL;
    if (size == 0 || size > ARENA_SIZE) return 0;
    if (align == 0) align = 8;
    if (align & (align - 1)) return 0; /* power of two only */

    if (s_state == 0) {
        s_state = (psx_runtime_register_region(ARENA_BASE, ARENA_SIZE,
                                               s_arena) == 0) ? 1 : -1;
        if (s_state < 0)
            OPL_ERROR_F("mods", "guest arena: region registration failed "
                        "(window overlap?) — allocations disabled");
        else
            OPL_INFO_F("mods", "guest arena live @ 0x%08X (%u KiB)",
                       ARENA_BASE, ARENA_SIZE / 1024u);
    }
    if (s_state < 0) return 0;

    uint32_t off = (s_used + (align - 1)) & ~(align - 1);
    if (off + size > ARENA_SIZE) {
        OPL_ERROR_F("mods", "guest arena exhausted (%s wanted %u B, "
                    "%u/%u used)", owner ? owner : "?", size, s_used,
                    ARENA_SIZE);
        return 0;
    }
    s_used = off + size;
    if (host_view) *host_view = &s_arena[off];
    return ARENA_BASE + off;
}

/* ── Low park window (guest_alloc_low) ─────────────────────────────────
 *
 * Some guest data formats address memory through NARROW fields — Spyro's
 * animated-model frame records hold a 21-bit absolute vertex-stream
 * address — so their payloads must live in the LOW 2MB of canonical
 * RAM, below the EXE image. The window is real guest RAM (savestate-
 * carried, visible to every access path including masked sites), owned
 * by nothing in openpete's recomp environment (the PSX kernel area the
 * BIOS would use; there is no BIOS here). The GAME registers the window
 * bounds at boot (they are game knowledge: below ITS load address);
 * before that, allocation fails loudly.
 *
 * Same contract as the high arena otherwise: session-lifetime bump, no
 * free. Mods previously coordinated fixed parks in this window by hand
 * (spyro3's gems and chests units — a collision waiting to happen);
 * this allocator is the coordinator. */

static uint32_t s_low_base, s_low_size, s_low_used;

void openpete_mods_arena_low_init(uint32_t base, uint32_t size) {
    if ((base >> 24) != 0x80u || (base & 0x00FFFFFFu) + size > 0x200000u) {
        OPL_ERROR_F("mods", "low park window rejected (%08X+%u)", base,
                    size);
        return;
    }
    s_low_base = base;
    s_low_size = size;
    OPL_INFO_F("mods", "low park window @ 0x%08X (%u KiB)", base,
               size / 1024u);
}

uint32_t openpete_mods_guest_alloc_low(uint32_t size, uint32_t align,
                                       void** host_view,
                                       const char* owner) {
    if (host_view) *host_view = NULL;
    if (!s_low_base) {
        OPL_ERROR_F("mods", "guest_alloc_low: no window registered "
                    "(game boot registration missing)");
        return 0;
    }
    if (size == 0 || size > s_low_size) return 0;
    if (align == 0) align = 16;
    if (align & (align - 1)) return 0;

    uint32_t off = (s_low_used + (align - 1)) & ~(align - 1);
    if (off + size > s_low_size) {
        OPL_ERROR_F("mods", "low park window exhausted (%s wanted %u B, "
                    "%u/%u used)", owner ? owner : "?", size, s_low_used,
                    s_low_size);
        return 0;
    }
    s_low_used = off + size;
    uint8_t* ram = openpete_psx_get_ram();
    if (host_view && ram)
        *host_view = ram + ((s_low_base + off) & 0x00FFFFFFu);
    OPL_INFO_F("mods", "low park: %u B at 0x%08X (%s; %u/%u used)",
               size, s_low_base + off, owner ? owner : "?",
               s_low_used, s_low_size);
    return s_low_base + off;
}
