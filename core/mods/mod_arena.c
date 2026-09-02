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
//   - Timeline state unless a row is marked OP_GALLOC_IMMUTABLE: the
//     arena's used prefix is carried by every snapshot the way guest RAM
//     is. Until the carry lands, a mod holding a mutable row makes the
//     timeline features refuse rather than silently desync
//     (openpete_mods_arena_carry_safe).
//   - Recompiled game sites that canonicalize addresses (mask to 2MB)
//     cannot see arena data — the same per-site limitation every
//     relocated buffer has (see openpete-ram-virtualization.md). The
//     standard accessor paths (cpu->read_*/write_*, api->guest) all
//     resolve it.
//   - Arena data readable by gameplay is divergence, as with any guest
//     write — content mods only; the modset save tag covers the rest.
//
// Live toggle posture: FREEZE, never tear down. Disabling a mod does not
// free, unregister, or rewind anything here, and that is a safety
// requirement rather than laziness — an allocation's vaddr may already be
// stored in guest structures (a model row pointing at a parked payload),
// so dropping the region would turn those into reads of unmapped memory
// while the game still chases them. So:
//   - a disabled mod's regions stay guest-readable and its bytes stay put;
//   - undoing the guest-side WRITES that point at them is the mod's job,
//     in its on_toggle hook (the engine cannot know which stock bytes a
//     mod overwrote unless the mod saved them);
//   - re-enable reuses the same vaddr the mod already holds — nothing is
//     re-allocated, so a mod must keep its handle across a toggle cycle
//     rather than allocating per enable, which would walk the bump cursor
//     forward on every cycle with no way to reclaim.
// Allocation itself is legal at a first enable: the region registers
// lazily on the first successful call, and the tick boundary that runs a
// deferred entry is a quiescent guest-access point like any other.

#include "openpete_mods.h"
#include "openpete_log.h"

#include "psx_runtime.h" /* psx_runtime_register_region */
#include "openpete_psx.h" /* openpete_psx_get_ram — low-window views */

#include <stdio.h>
#include <string.h>

#define ARENA_BASE 0x80700000u /* clear of the no-lod/polybuf windows */
#define ARENA_SIZE (1u * 1024u * 1024u)

/* The arena is never realloc'd: every host_view handed out must stay
 * valid for the process lifetime, and region-per-chunk growth would run
 * into the runtime's region table. Growth, when it comes, is one
 * reserve-then-commit mapping in place of this array. */
static uint8_t  s_arena[ARENA_SIZE];
static uint32_t s_used = 0;
static int      s_state = 0; /* 0 unregistered, 1 ok, -1 failed */

/* ── High-arena ledger ──────────────────────────────────────────────
 *
 * Every allocation is a row keyed by (owner, per-owner call index) and
 * recording (size, align, flags, offset). The ledger serves three
 * consumers with one structure and one mismatch semantic:
 *   - the carry gate: which enabled mods hold NON-immutable rows;
 *   - replay on re-entry: a reloaded mod's i-th call with matching
 *     (size, align, flags) is served its existing row — same vaddr,
 *     same view, bytes as last written — so a reload never orphans
 *     memory or walks the cursor. A mismatching call falls through to
 *     a fresh bump and marks the old row SUPERSEDED;
 *   - the savestate verdict: a file's rows must prefix-match the live
 *     ledger exactly, superseded marks included — a file whose rows
 *     reference a row the live session has since superseded would
 *     restore bytes into an orphan while the mod's handle points at the
 *     replacement.
 * Overflow REFUSES the allocation: a ledger that silently stops
 * recording turns every later verdict into a guess. */
#define ARENA_ROWS_MAX 256

typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t call_idx;   /* per-owner allocation sequence number */
    uint32_t size;
    uint32_t align;
    uint32_t flags;      /* OP_GALLOC_* as passed */
    uint32_t off;        /* arena offset; vaddr = ARENA_BASE + off */
    uint32_t superseded; /* replaced by a later row on a mismatching replay */
} arena_row_t;

static arena_row_t s_rows[ARENA_ROWS_MAX];
static uint32_t    s_rows_n = 0;

/* Per-owner call cursors: reset to 0 when the owner (re)enters so its
 * allocation sequence replays from the first row. */
#define ARENA_OWNERS_MAX 64
typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t next_call;
} arena_owner_t;
static arena_owner_t s_owners[ARENA_OWNERS_MAX];
static uint32_t      s_owners_n = 0;

static arena_owner_t* owner_slot(const char* owner, int create) {
    for (uint32_t i = 0; i < s_owners_n; i++)
        if (strcmp(s_owners[i].owner, owner) == 0) return &s_owners[i];
    if (!create || s_owners_n >= ARENA_OWNERS_MAX) return NULL;
    arena_owner_t* o = &s_owners[s_owners_n++];
    snprintf(o->owner, sizeof o->owner, "%s", owner);
    o->next_call = 0;
    return o;
}

static int arena_register(void) {
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
    return s_state > 0;
}

uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   uint32_t flags, void** host_view,
                                   const char* owner) {
    if (host_view) *host_view = NULL;
    if (!owner || !*owner) return 0;
    if (size == 0 || size > ARENA_SIZE) return 0;
    if (align == 0) align = 8;
    if (align & (align - 1)) return 0; /* power of two only */
    if (!arena_register()) return 0;

    arena_owner_t* o = owner_slot(owner, 1);
    if (!o) {
        OPL_ERROR_F("mods", "guest arena: owner table full (%d) — %s "
                    "refused", ARENA_OWNERS_MAX, owner);
        return 0;
    }
    uint32_t call_idx = o->next_call;

    if (s_rows_n >= ARENA_ROWS_MAX) {
        OPL_ERROR_F("mods", "guest arena ledger full (%d rows) — %s call "
                    "#%u refused; allocate at entry and reuse, the cursor "
                    "never returns", ARENA_ROWS_MAX, owner, call_idx);
        return 0;
    }
    uint32_t off = (s_used + (align - 1)) & ~(align - 1);
    if (off + size > ARENA_SIZE) {
        OPL_ERROR_F("mods", "guest arena exhausted (%s wanted %u B, "
                    "%u/%u used)", owner, size, s_used, ARENA_SIZE);
        return 0;
    }
    o->next_call = call_idx + 1;
    s_used = off + size;
    memset(&s_arena[off], 0, size); /* zero on first allocation */

    arena_row_t* r = &s_rows[s_rows_n++];
    memset(r, 0, sizeof *r);
    snprintf(r->owner, sizeof r->owner, "%s", owner);
    r->call_idx = call_idx;
    r->size = size; r->align = align; r->flags = flags; r->off = off;

    if (host_view) *host_view = &s_arena[off];
    return ARENA_BASE + off;
}

int openpete_mods_arena_owner_mutable(const char* owner) {
    if (!owner) return 0;
    for (uint32_t i = 0; i < s_rows_n; i++)
        if (!s_rows[i].superseded && !(s_rows[i].flags & OP_GALLOC_IMMUTABLE)
            && strcmp(s_rows[i].owner, owner) == 0)
            return 1;
    return 0;
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

/* Low-park ledger (savestate P2b): every low allocation is recorded so a
 * savestate can carry {owner, size, align, offset} + the watermark. On
 * load the live ledger must PREFIX-match the saved one (same mods
 * allocating in the same order); the cursor is then re-anchored to
 * max(live, saved) so the restored RAM's parked payloads can never be
 * handed out again by the live bump. */
#define ARENA_LEDGER_MAX 128
typedef struct {
    char     owner[24];
    uint32_t size;
    uint32_t align;
    uint32_t off;
} arena_low_rec_t;
static arena_low_rec_t s_low_ledger[ARENA_LEDGER_MAX];
static uint32_t        s_low_ledger_n = 0;
static int             s_low_ledger_overflow = 0;

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
    if (s_low_ledger_n < ARENA_LEDGER_MAX) {
        arena_low_rec_t* r = &s_low_ledger[s_low_ledger_n++];
        memset(r, 0, sizeof(*r));
        snprintf(r->owner, sizeof(r->owner), "%s", owner ? owner : "?");
        r->size = size; r->align = align; r->off = off;
    } else if (!s_low_ledger_overflow) {
        s_low_ledger_overflow = 1;
        OPL_ERROR_F("mods", "low-park ledger overflow (%d) — savestate "
                    "arena verification degraded", ARENA_LEDGER_MAX);
    }
    uint8_t* ram = openpete_psx_get_ram();
    if (host_view && ram)
        *host_view = ram + ((s_low_base + off) & 0x00FFFFFFu);
    OPL_INFO_F("mods", "low park: %u B at 0x%08X (%s; %u/%u used)",
               size, s_low_base + off, owner ? owner : "?",
               s_low_used, s_low_size);
    return s_low_base + off;
}

/* ── Ledger serialization (see openpete_mods.h; consumed by savestate.c) */

typedef struct {
    uint32_t ver;        /* 1 */
    uint32_t count;
    uint32_t low_used;   /* watermark at save */
    uint32_t overflow;   /* ledger overflowed at save (verify degraded) */
} arena_state_hdr_t;

uint32_t openpete_mods_arena_state_size(void) {
    return (uint32_t)(sizeof(arena_state_hdr_t)
                      + s_low_ledger_n * sizeof(arena_low_rec_t));
}

void openpete_mods_arena_state_save(void* dst) {
    arena_state_hdr_t h;
    h.ver = 1u;
    h.count = s_low_ledger_n;
    h.low_used = s_low_used;
    h.overflow = (uint32_t)s_low_ledger_overflow;
    memcpy(dst, &h, sizeof(h));
    memcpy((uint8_t*)dst + sizeof(h), s_low_ledger,
           s_low_ledger_n * sizeof(arena_low_rec_t));
}

int openpete_mods_arena_state_verdict(const void* src, uint32_t size) {
    if (!src || size < sizeof(arena_state_hdr_t)) return 0; /* no section */
    arena_state_hdr_t h;
    memcpy(&h, src, sizeof(h));
    if (h.ver != 1u ||
        size < sizeof(h) + h.count * sizeof(arena_low_rec_t)) {
        OPL_ERROR_F("mods", "arena ledger: malformed section — REFUSING");
        return -1;
    }
    const arena_low_rec_t* saved =
        (const arena_low_rec_t*)((const uint8_t*)src + sizeof(h));
    uint32_t common = h.count < s_low_ledger_n ? h.count : s_low_ledger_n;
    for (uint32_t i = 0; i < common; i++) {
        const arena_low_rec_t* a = &saved[i];
        const arena_low_rec_t* b = &s_low_ledger[i];
        if (a->off != b->off || a->size != b->size || a->align != b->align ||
            strncmp(a->owner, b->owner, sizeof(a->owner)) != 0) {
            OPL_ERROR_F("mods", "arena ledger: allocation #%u diverges "
                        "(saved %s %uB@+%u vs live %s %uB@+%u) — parked "
                        "guest data would be inconsistent; REFUSING the "
                        "state load", i,
                        a->owner, a->size, a->off,
                        b->owner, b->size, b->off);
            return -1;
        }
    }
    if (h.overflow || s_low_ledger_overflow)
        OPL_WARN_F("mods", "arena ledger overflowed (save=%u live=%d) — "
                   "verification is prefix-only", h.overflow,
                   s_low_ledger_overflow);
    return 0;
}

void openpete_mods_arena_state_apply(const void* src, uint32_t size) {
    if (!src || size < sizeof(arena_state_hdr_t)) return;
    arena_state_hdr_t h;
    memcpy(&h, src, sizeof(h));
    if (h.ver != 1u ||
        size < sizeof(h) + h.count * sizeof(arena_low_rec_t)) return;
    const arena_low_rec_t* saved =
        (const arena_low_rec_t*)((const uint8_t*)src + sizeof(h));
    /* Adopt saved entries the live session hasn't made yet (verdict
     * proved the common prefix matches), then re-anchor the cursor to
     * whichever timeline consumed more — future allocations must clobber
     * NEITHER the restored payloads NOR live ones. */
    for (uint32_t i = s_low_ledger_n;
         i < h.count && i < ARENA_LEDGER_MAX; i++) {
        s_low_ledger[i] = saved[i];
        s_low_ledger_n = i + 1;
    }
    if (h.low_used > s_low_used) s_low_used = h.low_used;
    OPL_INFO_F("mods", "arena ledger applied: %u entries, cursor %u/%u",
               s_low_ledger_n, s_low_used, s_low_size);
}
