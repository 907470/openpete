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
