// core/mods/mod_arena.c - the guest arena (api->guest_alloc) and the
// stream-handle ledger (api->vstream_handle).
//
// Mods need data at a GUEST-addressable vaddr - marshalled calls whose
// parameters are guest pointers, model payloads the game chases. openpete
// owns the memory map, so the arena is host memory registered as
// psx_runtime regions above the 2 MB of main RAM: guest reads/writes
// reach it through the standard cpu accessors' region cold path, and the
// mod gets a typed HOST view of the same bytes. Below physical 8 MB the
// bus otherwise answers with the RAM mirror (cpu.c, RAM_MIRROR_END); a
// registered region takes the address first, so a segment's committed
// bytes in [0x80200000, 0x80800000) replace those mirror copies for every
// access, the game's included. It is the ONLY home for
// mod memory: the moby format's one narrow address field (the 21-bit
// vertex-stream field) is bridged by stream handles, so nothing needs to
// sit inside canonical RAM.
//
// Two pools:
//   - DMA-reachable [0x80200000, 0x81000000): every physical byte below
//     the 24-bit bound a GPU packet's next pointer can name that RAM does
//     not use. Two segments with one cursor each, upper [0x80700000,
//     0x81000000) (9 MiB) first and lower [0x80200000, 0x80700000)
//     (5 MiB): a block goes upper when it fits at the upper cursor, else
//     lower when it fits at the lower cursor, else the call fails. Every
//     call without OP_GALLOC_NO_DMA lands here, so an allocation sequence
//     that fit the former single 8 MiB arena at 0x80700000 gets the same
//     vaddrs.
//   - general [0x81000000, 0x9F000000) (480 MiB): OP_GALLOC_NO_DMA calls,
//     whose blocks nothing truncates to 24 bits. It ends below expansion
//     region 1, the scratchpad, I/O and the BIOS.
// Each pool is one host reservation at its full size whose pages are
// committed in 64 KiB granules as the cursors advance (mmap PROT_NONE +
// mprotect, or VirtualAlloc reserve + commit), so host views never move.
// Each segment is a region registered at its committed extent on its
// first block and grown in place, so a guest access past the committed
// extent never touches an uncommitted page: below physical 8 MB it
// reaches the RAM mirror (guest RAM at phys & 0x1FFFFF), at or above it
// an unmapped address (a read answers 0, since no pool reaches the BIOS
// ROM window, and a store is dropped).
//
// Contracts and caveats (documented, not hidden):
//   - Allocation is a session-lifetime bump (no free): the arena is
//     scratch, not a heap.
//   - Timeline state: the three used ranges ride every snapshot
//     (in-memory keyframes and runahead, file savestates, handoff) the
//     way guest RAM does, restored as a re-anchor (bytes allocated
//     after the snapshot keep their contents; no cursor moves
//     backwards). A row marked OP_GALLOC_IMMUTABLE is a promise the
//     engine checksums at each keyframe and file save.
//   - Recompiled game sites that canonicalize addresses (mask to 2MB)
//     cannot see arena data - the same per-site limitation every
//     relocated buffer has. The
//     standard accessor paths (cpu->read_*/write_*, api->guest) all
//     resolve it.
//   - Arena data readable by gameplay is divergence, as with any guest
//     write - content mods only; the modset save tag covers the rest.
//
// Live toggle posture: FREEZE, never tear down. Disabling a mod does not
// free, unregister, or rewind anything here, and that is a safety
// requirement rather than laziness - an allocation's vaddr may already be
// stored in guest structures (a model row pointing at a parked payload),
// so dropping the region would turn those into reads of unmapped memory
// while the game still chases them. So:
//   - a disabled mod's regions stay guest-readable and its bytes stay put;
//   - undoing the guest-side WRITES that point at them is the mod's job,
//     in its on_toggle hook (the engine cannot know which stock bytes a
//     mod overwrote unless the mod saved them);
//   - re-enable reuses the same vaddr the mod already holds - nothing is
//     re-allocated, so a mod must keep its handle across a toggle cycle
//     rather than allocating per enable, which would walk the bump cursor
//     forward on every cycle with no way to reclaim.
// Allocation itself is legal at a first enable: a segment registers
// lazily on its first block, and the tick boundary that runs a deferred
// entry is a quiescent guest-access point like any other.

#include "openpete_mods.h"
#include "openpete_log.h"
#include "openpete_fnv1a64.h"
#include "mod_vec.h"

#include "psx_runtime_internal.h" /* psx_runtime_register_region */
#include "openpete_vstream.h" /* stream-handle table (vstream_handle) */

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* Guest layout. A DMA-pool row's `off` counts from DMA_BASE (an upper-
 * segment row from DMA_BASE too, so it keeps the former arena offset plus
 * UPPER_OFF); a general-pool row's `off` counts from GEN_BASE. */
#define DMA_BASE    0x80200000u            /* physical 2 MiB: the end of RAM */
#define DMA_SIZE    0x00E00000u            /* 14 MiB, up to physical 16 MiB */
#define UPPER_OFF   0x00500000u            /* upper segment at 0x80700000 */
#define UPPER_SIZE  (DMA_SIZE - UPPER_OFF) /* 9 MiB */
#define GEN_BASE    0x81000000u            /* physical 16 MiB */
#define GEN_SIZE    0x1E000000u            /* 480 MiB, up to 0x1F000000 */
#define GRANULE     0x10000u               /* commit step: Windows' granularity */

/* One host reservation per pool. */
enum { MAP_DMA, MAP_GENERAL, MAPS };
typedef struct {
    const char* name;
    uint32_t    size;
    uint8_t*    base;   /* NULL until reserved */
    int         failed; /* the reservation was refused; not retried */
} arena_map_t;
static arena_map_t s_map[MAPS] = {
    { "DMA-reachable", DMA_SIZE, NULL, 0 },
    { "general",       GEN_SIZE, NULL, 0 },
};

/* One cursor per segment, indexed by OPENPETE_ARENA_RANGE_*. */
typedef struct {
    const char* name;
    uint32_t    vbase;     /* guest base */
    uint32_t    cap;       /* segment bytes */
    uint32_t    row_base;  /* row `off` of the segment's first byte */
    unsigned    map;       /* MAP_* */
    uint32_t    map_off;   /* the segment's offset in its reservation */
    uint32_t    used;      /* cursor: end of the last block, padding included */
    uint32_t    committed; /* committed and registered bytes from vbase */
} arena_seg_t;
static arena_seg_t s_seg[OPENPETE_ARENA_RANGES] = {
    { "upper",   DMA_BASE + UPPER_OFF, UPPER_SIZE, UPPER_OFF, MAP_DMA,     UPPER_OFF, 0, 0 },
    { "lower",   DMA_BASE,             UPPER_OFF,  0u,        MAP_DMA,     0u,        0, 0 },
    { "general", GEN_BASE,             GEN_SIZE,   0u,        MAP_GENERAL, 0u,        0, 0 },
};

/** @brief Reserve a pool's host address range with no pages committed, once.
 *
 * A refusal is logged and latched, so the pool's allocations fail without
 * retrying.
 * @param m the pool's reservation.
 * @return 1 when reserved, else 0. */
static int map_reserve(arena_map_t* m) {
    if (m->base) return 1;
    if (m->failed) return 0;
#ifdef _WIN32
    void* p = VirtualAlloc(NULL, m->size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(NULL, m->size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) {
        m->failed = 1;
        OPL_ERROR_F("mods", "guest arena: %s pool: reserving %u KiB of host "
                    "address space failed - its allocations are refused",
                    m->name, m->size / 1024u);
        return 0;
    }
    m->base = (uint8_t*)p;
    return 1;
}

/** @brief Commit read-write pages over part of a reservation.
 *
 * @param p the first byte, granule-aligned within the reservation.
 * @param n the byte count.
 * @return 1 on success, else 0. */
static int map_commit(uint8_t* p, uint32_t n) {
#ifdef _WIN32
    return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return mprotect(p, n, PROT_READ | PROT_WRITE) == 0;
#endif
}

/** @brief Return a segment's host base, or NULL while its pool is unreserved. */
static uint8_t* seg_host(const arena_seg_t* s) {
    uint8_t* b = s_map[s->map].base;
    return b ? b + s->map_off : NULL;
}

/** @brief Commit and register a segment through at least @p end bytes from its base.
 *
 * Commits whole granules, clamped to the segment's capacity. The first
 * commit registers the segment as a psx_runtime region at the committed
 * extent; a later one grows that region in place.
 * @param s the segment.
 * @param end the byte count from the segment base that must be backed,
 *        at most the capacity.
 * @return 1 when backed and registered, else 0 (logged). */
static int seg_commit(arena_seg_t* s, uint32_t end) {
    if (end <= s->committed) return 1;
    if (!map_reserve(&s_map[s->map])) return 0;
    uint32_t c = end > s->cap - (GRANULE - 1u) ? s->cap
                                               : (end + GRANULE - 1u) & ~(GRANULE - 1u);
    uint8_t* host = seg_host(s);
    if (!map_commit(host + s->committed, c - s->committed)) {
        OPL_ERROR_F("mods", "guest arena: %s segment: committing %u KiB "
                    "failed", s->name, (c - s->committed) / 1024u);
        return 0;
    }
    const int rc = s->committed == 0u
        ? psx_runtime_register_region(s->vbase, c, host)
        : psx_runtime_grow_region(s->vbase, c);
    if (rc != 0) {
        OPL_ERROR_F("mods", "guest arena: %s segment: region %s failed "
                    "(code %d, window overlap?)", s->name,
                    s->committed ? "growth" : "registration", rc);
        return 0;
    }
    if (s->committed == 0u) {
        if (s == &s_seg[OPENPETE_ARENA_RANGE_UPPER])
            OPL_INFO_F("mods", "guest arena live @ 0x%08X (upper segment, "
                       "%u KiB)", s->vbase, s->cap / 1024u);
        else
            OPL_INFO_F("mods", "guest arena %s %s live @ 0x%08X (%u KiB)",
                       s->name, s->map == MAP_GENERAL ? "pool" : "segment",
                       s->vbase, s->cap / 1024u);
    }
    s->committed = c;
    return 1;
}

/* ── High-arena ledger ──────────────────────────────────────────────
 *
 * Every allocation is a row keyed by (owner, per-owner call index) and
 * recording (size, align, flags, offset). The ledger serves three
 * consumers with one structure and one mismatch semantic:
 *   - the carry gate: which enabled mods hold NON-immutable rows;
 *   - replay on re-entry: a reloaded mod's i-th call with matching
 *     (size, align, flags) is served its existing row - same vaddr,
 *     same view, bytes as last written - so a reload never orphans
 *     memory or walks the cursor. A mismatching call falls through to
 *     a fresh bump and marks the old row SUPERSEDED;
 *   - the savestate verdict: a file's rows must prefix-match the live
 *     ledger exactly, superseded marks included - a file whose rows
 *     reference a row the live session has since superseded would
 *     restore bytes into an orphan while the mod's handle points at the
 *     replacement.
 * Overflow REFUSES the allocation: a ledger that silently stops
 * recording turns every later verdict into a guess. */
/* Rows the ledger allocates up front; it grows past this. */
#define ARENA_ROWS_INIT 8192

/* Two row kinds share the ledger and its replay/verdict semantics: an
 * ALLOCATION row records (size, align, flags, off); a HANDLE row records
 * a vstream_handle call - `size` holds the stream vaddr requested and
 * `off` the handle index returned (align and flags are 0). */
#define ROW_ALLOC  0u
#define ROW_HANDLE 1u
typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t kind;       /* ROW_ALLOC / ROW_HANDLE */
    uint32_t call_idx;   /* per-owner sequence number, both kinds share it */
    uint32_t size;       /* alloc: bytes; handle: stream vaddr */
    uint32_t align;
    uint32_t flags;      /* OP_GALLOC_* as passed */
    uint32_t off;        /* alloc: offset from 0x80200000 (DMA-reachable) or
                          * 0x81000000 (OP_GALLOC_NO_DMA); handle: index */
    uint32_t superseded; /* replaced by a later row on a mismatching replay */
    /* Immutable digest, held on the row rather than in a parallel array
     * so a row and its promise cannot drift apart: 0 none, 1 captured,
     * 2 broken. Not part of the saved record. */
    uint64_t sum;
    uint8_t  sum_state;
} arena_row_t;

static mod_fvec s_rows;   /* arena_row_t */

/** @brief Return the number of ledger rows. */
static uint32_t row_count(void) { return (uint32_t)mod_fvec_count(&s_rows); }
/** @brief Return ledger row i.
 *
 * The pointer is valid until the next row_push, which can move the table.
 * @param i the row index, below row_count(). */
static arena_row_t* row_at(uint32_t i) {
    return (arena_row_t*)mod_fvec_at(&s_rows, (size_t)i);
}

/** @brief Append a zeroed ledger row, initializing the ledger on first use.
 *
 * @return the new row, or NULL when the allocation fails. */
static arena_row_t* row_push(void) {
    if (!s_rows.elem_size)
        mod_fvec_init(&s_rows, sizeof(arena_row_t), ARENA_ROWS_INIT,
                      "arena_rows", ARENA_ROWS_INIT);
    return (arena_row_t*)mod_fvec_push(&s_rows);
}

/* Per-owner call cursors: reset to 0 when the owner (re)enters so its
 * allocation sequence replays from the first row. */
/* Cursors the table allocates up front; it grows past this. */
#define ARENA_OWNERS_INIT 64
typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t next_call;
    int      in_entry;  /* between entry_begin and entry_end */
    int      reported;  /* the session use line was logged */
} arena_owner_t;
static mod_fvec s_owners;   /* arena_owner_t, one per owner seen */

/** @brief Find an owner's call cursor, or create one starting at call 0.
 *
 * The returned pointer is call-local: the next create can move the table
 * and invalidates it.
 * @param owner the mod id.
 * @param create nonzero to create a missing cursor.
 * @return the cursor, or NULL when it is missing and create is 0 or the
 *         allocation fails. */
static arena_owner_t* owner_slot(const char* owner, int create) {
    for (size_t i = 0; i < mod_fvec_count(&s_owners); i++) {
        arena_owner_t* o = (arena_owner_t*)mod_fvec_at(&s_owners, i);
        if (strcmp(o->owner, owner) == 0) return o;
    }
    if (!create) return NULL;
    if (!s_owners.elem_size)
        mod_fvec_init(&s_owners, sizeof(arena_owner_t), ARENA_OWNERS_INIT,
                      "arena_owners", ARENA_OWNERS_INIT);
    arena_owner_t* o = (arena_owner_t*)mod_fvec_push(&s_owners);
    if (!o) return NULL;
    snprintf(o->owner, sizeof o->owner, "%s", owner);
    return o;
}

/** @brief Return the segment an allocation row's block lies in.
 *
 * The pool follows from the row's flags; inside the DMA-reachable pool the
 * segment follows from the offset. */
static arena_seg_t* row_seg(const arena_row_t* r) {
    if (r->flags & OP_GALLOC_NO_DMA) return &s_seg[OPENPETE_ARENA_RANGE_GENERAL];
    return r->off >= UPPER_OFF ? &s_seg[OPENPETE_ARENA_RANGE_UPPER]
                               : &s_seg[OPENPETE_ARENA_RANGE_LOWER];
}

/** @brief Return an allocation row's guest vaddr. */
static uint32_t row_vaddr(const arena_row_t* r) {
    const arena_seg_t* s = row_seg(r);
    return s->vbase + (r->off - s->row_base);
}

/** @brief Return an allocation row's host bytes; the segment holding the row is committed. */
static uint8_t* row_host(const arena_row_t* r) {
    const arena_seg_t* s = row_seg(r);
    return seg_host(s) + (r->off - s->row_base);
}

/* ── Per-owner use, readable from any thread ─────────────────────────
 *
 * The ledger and the owner cursors live in growable tables that only the
 * allocating thread touches. The Mods panel reads each owner's use from
 * the present thread, so the figures are mirrored into this fixed table:
 * a slot's name is written before the count is published with a release
 * store, and its counters are atomics, so a reader that acquires the
 * count sees whole names and current or slightly stale counters. */
#define USE_SLOTS 256u
enum { USE_DMA, USE_DMA_IMM, USE_GEN, USE_GEN_IMM, USE_FIELDS };
typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t bytes[USE_FIELDS];
} arena_use_slot_t;
static arena_use_slot_t s_use[USE_SLOTS];
static uint32_t         s_use_count; /* published with release stores */

/** @brief Find an owner's use slot, creating it on the allocating thread when missing.
 *
 * @param owner the mod id.
 * @param create nonzero to create a missing slot.
 * @return the slot, or NULL when it is missing and create is 0 or the
 *         table is full. */
static arena_use_slot_t* use_slot(const char* owner, int create) {
    const uint32_t n = __atomic_load_n(&s_use_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++)
        if (strncmp(s_use[i].owner, owner, sizeof s_use[i].owner) == 0)
            return &s_use[i];
    if (!create || n >= USE_SLOTS) return NULL;
    snprintf(s_use[n].owner, sizeof s_use[n].owner, "%s", owner);
    __atomic_store_n(&s_use_count, n + 1u, __ATOMIC_RELEASE);
    return &s_use[n];
}

/** @brief Add an allocation row's bytes to its owner's use. */
static void use_add(const arena_row_t* r) {
    if (r->kind != ROW_ALLOC) return;
    arena_use_slot_t* u = use_slot(r->owner, 1);
    if (!u) return;
    const int gen = (r->flags & OP_GALLOC_NO_DMA) != 0;
    __atomic_fetch_add(&u->bytes[gen ? USE_GEN : USE_DMA], r->size, __ATOMIC_RELAXED);
    if (r->flags & OP_GALLOC_IMMUTABLE)
        __atomic_fetch_add(&u->bytes[gen ? USE_GEN_IMM : USE_DMA_IMM], r->size,
                           __ATOMIC_RELAXED);
}

int openpete_mods_arena_owner_use(const char* owner,
                                  openpete_mods_arena_use_t* out) {
    if (out) memset(out, 0, sizeof *out);
    if (!owner || !out) return 0;
    const arena_use_slot_t* u = use_slot(owner, 0);
    if (!u) return 0;
    out->dma_bytes         = __atomic_load_n(&u->bytes[USE_DMA], __ATOMIC_RELAXED);
    out->dma_immutable     = __atomic_load_n(&u->bytes[USE_DMA_IMM], __ATOMIC_RELAXED);
    out->general_bytes     = __atomic_load_n(&u->bytes[USE_GEN], __ATOMIC_RELAXED);
    out->general_immutable = __atomic_load_n(&u->bytes[USE_GEN_IMM], __ATOMIC_RELAXED);
    return (out->dma_bytes | out->general_bytes) != 0u;
}

/** @brief Log an owner's pool use once per session, when it holds a block and has not been logged.
 *
 * @param o the owner's cursor, whose reported flag latches the line. */
static void use_report(arena_owner_t* o) {
    openpete_mods_arena_use_t u;
    if (o->reported || !openpete_mods_arena_owner_use(o->owner, &u)) return;
    o->reported = 1;
    OPL_INFO_F("mods", "guest arena: %s holds %u B DMA-reachable (%u B "
               "IMMUTABLE), %u B general (%u B IMMUTABLE)", o->owner,
               u.dma_bytes, u.dma_immutable, u.general_bytes,
               u.general_immutable);
}

/** @brief Log a refused allocation in an exhausted pool, then every owner's bytes in that pool.
 *
 * @param owner the refused caller.
 * @param size the requested size.
 * @param align the requested alignment.
 * @param general nonzero for the general pool, else the DMA-reachable pool. */
static void log_exhausted(const char* owner, uint32_t size, uint32_t align,
                          int general) {
    if (general) {
        const arena_seg_t* g = &s_seg[OPENPETE_ARENA_RANGE_GENERAL];
        OPL_ERROR_F("mods", "guest arena exhausted: %s wanted %u B (align %u) "
                    "in the general pool, %u/%u used", owner, size, align,
                    g->used, g->cap);
    } else {
        const arena_seg_t* u = &s_seg[OPENPETE_ARENA_RANGE_UPPER];
        const arena_seg_t* l = &s_seg[OPENPETE_ARENA_RANGE_LOWER];
        OPL_ERROR_F("mods", "guest arena exhausted: %s wanted %u B (align %u) "
                    "in the DMA-reachable pool, upper %u/%u used, lower "
                    "%u/%u used", owner, size, align, u->used, u->cap,
                    l->used, l->cap);
    }
    const uint32_t n = __atomic_load_n(&s_use_count, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t b = s_use[i].bytes[general ? USE_GEN : USE_DMA];
        if (b)
            OPL_ERROR_F("mods", "guest arena exhausted: %s pool held by %s: "
                        "%u B", general ? "general" : "DMA-reachable",
                        s_use[i].owner, b);
    }
}

/** @brief Choose the segment and offset of a new block: upper then lower for the DMA-reachable pool, the general pool for OP_GALLOC_NO_DMA.
 *
 * Each segment aligns its own offset, as the single arena did, so an
 * upper-segment block lands where the former arena placed it.
 * @param general nonzero for the general pool.
 * @param size the block size, within the pool's per-call limit.
 * @param align the alignment, a power of two.
 * @param soff receives the block's offset from the segment base.
 * @return the segment, or NULL when no segment of the pool fits it. */
static arena_seg_t* place(int general, uint32_t size, uint32_t align,
                          uint32_t* soff) {
    static const unsigned k_dma[] = { OPENPETE_ARENA_RANGE_UPPER,
                                      OPENPETE_ARENA_RANGE_LOWER };
    static const unsigned k_gen[] = { OPENPETE_ARENA_RANGE_GENERAL };
    const unsigned* order = general ? k_gen : k_dma;
    const unsigned n = general ? 1u : 2u;
    for (unsigned i = 0; i < n; i++) {
        arena_seg_t* s = &s_seg[order[i]];
        const uint64_t off = ((uint64_t)s->used + (align - 1u)) & ~(uint64_t)(align - 1u);
        if (off + size > s->cap) continue;
        *soff = (uint32_t)off;
        return s;
    }
    return NULL;
}

uint32_t openpete_mods_guest_alloc(uint32_t size, uint32_t align,
                                   uint32_t flags, void** host_view,
                                   const char* owner) {
    if (host_view) *host_view = NULL;
    if (!owner || !*owner) return 0;
    const int general = (flags & OP_GALLOC_NO_DMA) != 0;
    if (size == 0 || size > (general ? GEN_SIZE : UPPER_SIZE)) return 0;
    if (align == 0) align = 8;
    if (align & (align - 1)) return 0; /* power of two only */

    arena_owner_t* o = owner_slot(owner, 1);
    if (!o) {
        OPL_ERROR_F("mods", "guest arena: owner table allocation failed "
                    "- %s refused", owner);
        return 0;
    }
    uint32_t call_idx = o->next_call;

    /* Replay: an existing live row for this (owner, call) is served as
     * is when the request matches it - same vaddr, same view, bytes as
     * last written. A mismatch means the mod's allocation pattern
     * changed (a config-driven size, an order that depends on runtime
     * state): the old row is marked superseded so no savestate written
     * against it can ever restore into an orphan, and the call falls
     * through to a fresh bump. */
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_row_t* r = row_at(i);
        if (r->superseded || r->call_idx != call_idx ||
            strcmp(r->owner, owner) != 0)
            continue;
        if (r->kind == ROW_ALLOC && r->size == size && r->align == align &&
            r->flags == flags) {
            o->next_call = call_idx + 1;
            if (host_view) *host_view = row_host(r);
            OPL_INFO_F("mods", "guest arena: %s call #%u replayed "
                       "(%u B @0x%08X, bytes kept)", owner, call_idx,
                       size, row_vaddr(r));
            return row_vaddr(r);
        }
        r->superseded = 1;
        OPL_WARN_F("mods", "guest arena: %s call #%u changed shape "
                   "(%s %u B/%u/%#x -> alloc %u B/%u/%#x) - row #%u "
                   "superseded; savestates written against it are now "
                   "refused", owner, call_idx,
                   r->kind == ROW_HANDLE ? "handle" : "alloc", r->size,
                   r->align, r->flags, size, align, flags, i);
        break;
    }

    uint32_t soff = 0;
    arena_seg_t* s = place(general, size, align, &soff);
    if (!s) {
        log_exhausted(owner, size, align, general);
        return 0;
    }
    if (!seg_commit(s, soff + size)) {
        OPL_ERROR_F("mods", "guest arena: %s call #%u refused (%u B in the "
                    "%s segment)", owner, call_idx, size, s->name);
        return 0;
    }
    uint8_t* host = seg_host(s) + soff;
    o->next_call = call_idx + 1;
    s->used = soff + size;
    /* Zero on first allocation: guest code may already have written the
     * committed bytes past the cursor. */
    memset(host, 0, size);

    arena_row_t* r = row_push();
    if (!r) {
        OPL_ERROR_F("mods", "guest arena ledger allocation failed - %s "
                    "call #%u refused", owner, call_idx);
        return 0;
    }
    snprintf(r->owner, sizeof r->owner, "%s", owner);
    r->kind = ROW_ALLOC;
    r->call_idx = call_idx;
    r->size = size; r->align = align; r->flags = flags;
    r->off = s->row_base + soff;
    use_add(r);
    if (!o->in_entry) use_report(o);

    if (host_view) *host_view = host;
    return s->vbase + soff;
}

/* ── Stream handles ─────────────────────────────────────────────────
 *
 * A handle is an index into the engine's vstream table (openpete_vstream.h)
 * standing in for a stream vaddr in a frame word's 21-bit field. Numbering
 * is what a savestate's frame words depend on, so every request is a
 * ledger row with the same replay and verdict rules as an allocation: a
 * reloaded mod gets the same handle for the same vaddr at the same call
 * index; a different vaddr at that index supersedes the row and takes a
 * fresh handle; a file written against a superseded row is refused. The
 * same vaddr requested twice (by anyone) yields the same handle, and both
 * requests are rows, so call indices do not drift between a session that
 * repeats a request and one that does not. Handle 0 is never issued. */
static uint32_t s_next_handle = 1;

/** @brief Return the vstream handle bound to a stream vaddr, binding the next unused handle when none is.
 *
 * @param vaddr the stream's guest vaddr.
 * @return the handle, or 0 when the handle range is exhausted or the
 *         table refuses the binding. */
static uint32_t handle_bind(uint32_t vaddr) {
    uint32_t h = openpete_vstream_table_find(vaddr);
    if (h) return h;
    if (s_next_handle >= VSTREAM_HANDLE_MAX) return 0;
    h = s_next_handle++;
    return openpete_vstream_table_set(h, vaddr) ? h : 0;
}

uint32_t openpete_mods_vstream_handle(uint32_t vaddr, const char* owner) {
    if (!owner || !*owner || !vaddr) return 0;
    if ((vaddr & 0x1FFFFFFFu) < 0x10000u) {
        OPL_ERROR_F("mods", "%s: vstream_handle(0x%08X): a stream cannot "
                    "live in the kernel area (the handle range)", owner,
                    vaddr);
        return 0;
    }
    arena_owner_t* o = owner_slot(owner, 1);
    if (!o) {
        OPL_ERROR_F("mods", "vstream_handle: owner table allocation "
                    "failed - %s refused", owner);
        return 0;
    }
    uint32_t call_idx = o->next_call;
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_row_t* r = row_at(i);
        if (r->superseded || r->call_idx != call_idx ||
            strcmp(r->owner, owner) != 0)
            continue;
        if (r->kind == ROW_HANDLE && r->size == vaddr) {
            if (!openpete_vstream_table_set(r->off, vaddr)) return 0;
            o->next_call = call_idx + 1;
            return r->off;
        }
        r->superseded = 1;
        OPL_WARN_F("mods", "guest arena: %s call #%u changed shape "
                   "(%s -> handle for 0x%08X) - row #%u superseded; "
                   "savestates written against it are now refused",
                   owner, call_idx,
                   r->kind == ROW_HANDLE ? "handle" : "alloc", vaddr, i);
        break;
    }
    uint32_t h = handle_bind(vaddr);
    if (!h) {
        OPL_ERROR_F("mods", "%s: vstream_handle(0x%08X): handle table "
                    "exhausted (%u)", owner, vaddr, VSTREAM_HANDLE_MAX);
        return 0;
    }
    o->next_call = call_idx + 1;
    arena_row_t* r = row_push();
    if (!r) {
        OPL_ERROR_F("mods", "guest arena ledger allocation failed - %s "
                    "vstream_handle call #%u refused", owner, call_idx);
        return 0;
    }
    snprintf(r->owner, sizeof r->owner, "%s", owner);
    r->kind = ROW_HANDLE;
    r->call_idx = call_idx;
    r->size = vaddr; r->off = h;
    return h;
}

/* Runahead's per-tick snapshot cost, shown beside each mod's pool use. */
static uint32_t s_ra_snap_us;

void openpete_mods_runahead_snapshot_note(uint64_t us) {
    const uint32_t v = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us;
    const uint32_t m = __atomic_load_n(&s_ra_snap_us, __ATOMIC_RELAXED);
    const uint32_t n = m == 0u ? v : (uint32_t)(((uint64_t)m * 15u + v) / 16u);
    __atomic_store_n(&s_ra_snap_us, n ? n : 1u, __ATOMIC_RELAXED);
}

uint32_t openpete_mods_runahead_snapshot_us(void) {
    return __atomic_load_n(&s_ra_snap_us, __ATOMIC_RELAXED);
}

void openpete_mods_arena_entry_begin(const char* owner) {
    if (!owner || !*owner) return;
    arena_owner_t* o = owner_slot(owner, 0);
    if (o) o->next_call = 0;
    o = owner_slot(owner, 1);
    if (o) o->in_entry = 1;
}

void openpete_mods_arena_entry_end(const char* owner) {
    if (!owner || !*owner) return;
    arena_owner_t* o = owner_slot(owner, 0);
    if (!o) return;
    o->in_entry = 0;
    use_report(o);
}

uint32_t openpete_mods_arena_used(uint32_t used[OPENPETE_ARENA_RANGES]) {
    uint32_t total = 0;
    for (unsigned i = 0; i < OPENPETE_ARENA_RANGES; i++) {
        if (used) used[i] = s_seg[i].used;
        total += s_seg[i].used;
    }
    return total;
}

uint32_t openpete_mods_arena_range_vaddr(unsigned range) {
    return range < OPENPETE_ARENA_RANGES ? s_seg[range].vbase : 0u;
}

const uint8_t* openpete_mods_arena_range_bytes(unsigned range) {
    return range < OPENPETE_ARENA_RANGES ? seg_host(&s_seg[range]) : NULL;
}

void openpete_mods_arena_mem_save(void* dst,
                                  uint32_t used[OPENPETE_ARENA_RANGES]) {
    uint8_t* p = (uint8_t*)dst;
    for (unsigned i = 0; i < OPENPETE_ARENA_RANGES; i++) {
        const uint32_t n = s_seg[i].used;
        if (used) used[i] = n;
        if (!n) continue;
        memcpy(p, seg_host(&s_seg[i]), n);
        p += n;
    }
}

uint8_t* openpete_mods_arena_range_reanchor(unsigned range, uint32_t* n) {
    if (range >= OPENPETE_ARENA_RANGES || !n || !*n) return NULL;
    /* Re-anchor, never replace: bytes allocated after the snapshot keep
     * their contents and no cursor moves backwards, so a handle a mod
     * took mid-window stays valid across the restore. */
    arena_seg_t* s = &s_seg[range];
    if (*n > s->cap) *n = s->cap;
    if (!*n) return NULL;
    if (!seg_commit(s, *n)) {
        OPL_ERROR_F("mods", "guest arena: %s segment: %u B of snapshot "
                    "bytes not restored", s->name, *n);
        return NULL;
    }
    if (*n > s->used) s->used = *n;
    return seg_host(s);
}

void openpete_mods_arena_mem_restore(const void* bytes,
                                     const uint32_t used[OPENPETE_ARENA_RANGES]) {
    if (!bytes || !used) return;
    const uint8_t* p = (const uint8_t*)bytes;
    for (unsigned i = 0; i < OPENPETE_ARENA_RANGES; i++) {
        uint32_t n = used[i];
        uint8_t* dst = openpete_mods_arena_range_reanchor(i, &n);
        if (dst) memcpy(dst, p, n);
        p += used[i];
    }
}

/* Immutable rows carry a checksum captured the first time they are
 * checked (the first snapshot after the allocating tick). A later
 * mismatch is the mod's promise broken - warned once per row, with the
 * owner and call index, so the flag is a falsifiable claim rather than
 * a hint the engine takes on faith. */
void openpete_mods_arena_immutable_check(const char* when) {
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_row_t* r = row_at(i);
        if (r->kind != ROW_ALLOC || !(r->flags & OP_GALLOC_IMMUTABLE) ||
            r->superseded)
            continue;
        if (r->sum_state == 2) continue;
        uint64_t h = openpete_fnv1a64(row_host(r), r->size);
        if (r->sum_state == 0) {
            r->sum = h;
            r->sum_state = 1;
        } else if (h != r->sum) {
            r->sum_state = 2;
            OPL_WARN_F("mods", "guest arena: %s call #%u is marked "
                       "IMMUTABLE but its bytes changed (%u B @0x%08X, "
                       "seen at %s) - the promise is broken; drop the "
                       "flag or stop writing the block", r->owner,
                       r->call_idx, r->size, row_vaddr(r),
                       when ? when : "snapshot");
        }
    }
}


/* ── Ledger serialization (see openpete_mods.h; consumed by savestate.c)
 *
 * Section layout (v4):
 *   header                 - ver, count, used upper/lower/general
 *   rows [count]           - the ledger, superseded marks included
 *   bytes                  - the upper, lower and general used ranges
 * Version 3, the single 8 MiB arena at 0x80700000, is still read: its
 * header is (ver, count, used), its allocation rows are upper-segment rows
 * at the same distance from 0x80700000 (offset plus UPPER_OFF), and its
 * bytes are the upper range.
 * Verdict runs PRE-COMMIT: the ledger must prefix-match the live session
 * exactly, or the restored bytes would land under a different layout and
 * every vaddr (and every handle) parked inside them would be garbage. */

#define ARENA_SECTION_VER 4u
typedef struct {
    uint32_t ver;        /* 4 */
    uint32_t count;      /* rows */
    uint32_t used[OPENPETE_ARENA_RANGES]; /* per range == byte payload */
} arena_state_hdr_t;
typedef struct {
    uint32_t ver;        /* 3 */
    uint32_t count;
    uint32_t used;       /* the single arena's watermark */
} arena_state_hdr_v3_t;

/* On-disk row: fixed width, no host state. */
typedef struct {
    char     owner[OPENPETE_ARENA_OWNER_MAX];
    uint32_t kind;
    uint32_t call_idx;
    uint32_t size;
    uint32_t align;
    uint32_t flags;
    uint32_t off;
    uint32_t superseded;
} arena_rec_t;

/* A parsed section: rows still point into the source buffer. */
typedef struct {
    uint32_t       ver;
    uint32_t       count;
    uint32_t       used[OPENPETE_ARENA_RANGES];
    const uint8_t* rows;   /* count records, possibly unaligned */
    const uint8_t* bytes;  /* the ranges, upper, lower, general */
} arena_section_t;

/** @brief Copy a live row into a zero-padded on-disk record.
 *
 * @param d the record written.
 * @param r the live row; its host-only digest is not copied. */
static void rec_from_row(arena_rec_t* d, const arena_row_t* r) {
    memset(d, 0, sizeof *d);
    memcpy(d->owner, r->owner, sizeof d->owner);
    d->kind = r->kind; d->call_idx = r->call_idx; d->size = r->size;
    d->align = r->align; d->flags = r->flags; d->off = r->off;
    d->superseded = r->superseded;
}

/** @brief Read record i of a parsed section in the version-4 meaning.
 *
 * A version-3 allocation record's offset moves by UPPER_OFF, onto the
 * upper segment at the same vaddr; a handle record is unchanged.
 * @param sec the parsed section.
 * @param i the record index, below sec->count.
 * @param d receives the record. */
static void rec_get(const arena_section_t* sec, uint32_t i, arena_rec_t* d) {
    memcpy(d, sec->rows + (size_t)i * sizeof *d, sizeof *d);
    if (sec->ver == 3u && d->kind == ROW_ALLOC) d->off += UPPER_OFF;
}

/** @brief Test whether a saved record equals a live row in every field the record carries.
 *
 * @param a the saved record.
 * @param b the live row; its host-only digest is not compared.
 * @return 1 when equal, else 0. */
static int rec_matches(const arena_rec_t* a, const arena_row_t* b) {
    return a->kind == b->kind && a->call_idx == b->call_idx &&
           a->size == b->size && a->align == b->align &&
           a->flags == b->flags && a->off == b->off &&
           a->superseded == b->superseded &&
           strncmp(a->owner, b->owner, sizeof a->owner) == 0;
}

/** @brief Test that a saved allocation record lies inside its segment's saved used range.
 *
 * A record that does not would make a replayed row's host view point
 * outside the bytes the section restores, or outside the reservation.
 * @param a the record, in the version-4 meaning.
 * @param used the section's used bytes per range.
 * @return 1 when it lies inside, or the record is a handle; else 0. */
static int rec_in_bounds(const arena_rec_t* a,
                         const uint32_t used[OPENPETE_ARENA_RANGES]) {
    if (a->kind == ROW_HANDLE) return 1;
    if (a->kind != ROW_ALLOC) return 0;
    arena_row_t probe;
    memset(&probe, 0, sizeof probe);
    probe.flags = a->flags; probe.off = a->off;
    const arena_seg_t* s = row_seg(&probe);
    if (a->off < s->row_base) return 0;
    const uint64_t end = (uint64_t)(a->off - s->row_base) + a->size;
    return end <= used[s - s_seg];
}

uint32_t openpete_mods_arena_state_size(void) {
    return (uint32_t)(sizeof(arena_state_hdr_t)
                      + row_count() * sizeof(arena_rec_t)
                      + openpete_mods_arena_used(NULL));
}

void openpete_mods_arena_state_save(void* dst) {
    arena_state_hdr_t h;
    h.ver = ARENA_SECTION_VER;
    h.count = row_count();
    (void)openpete_mods_arena_used(h.used);
    uint8_t* p = (uint8_t*)dst;
    memcpy(p, &h, sizeof h);                       p += sizeof h;
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_rec_t rec;
        rec_from_row(&rec, row_at(i));
        memcpy(p, &rec, sizeof rec);               p += sizeof rec;
    }
    openpete_mods_arena_mem_save(p, NULL);
}

/** @brief Split a saved arena section into its header, row records and byte payload, checking bounds.
 *
 * Reads version 4 and version 3; nothing is copied.
 * @param src the section bytes; NULL is treated as no section.
 * @param size the section size in bytes.
 * @param sec receives the parsed section.
 * @return 1 when parsed, 0 for no section (NULL src or a size below the
 *         version word), -1 for another version, a section too short for
 *         its rows and payload, a range over its capacity, a version-3
 *         row carrying OP_GALLOC_NO_DMA, or an allocation row outside its
 *         segment's saved range. */
static int arena_section_parse(const void* src, uint32_t size,
                               arena_section_t* sec) {
    if (!src || size < sizeof(uint32_t)) return 0;
    memset(sec, 0, sizeof *sec);
    memcpy(&sec->ver, src, sizeof sec->ver);
    size_t hdr_bytes;
    if (sec->ver == ARENA_SECTION_VER) {
        arena_state_hdr_t h;
        if (size < sizeof h) return -1;
        memcpy(&h, src, sizeof h);
        sec->count = h.count;
        memcpy(sec->used, h.used, sizeof sec->used);
        hdr_bytes = sizeof h;
    } else if (sec->ver == 3u) {
        arena_state_hdr_v3_t h;
        if (size < sizeof h) return -1;
        memcpy(&h, src, sizeof h);
        sec->count = h.count;
        sec->used[OPENPETE_ARENA_RANGE_UPPER] = h.used;
        hdr_bytes = sizeof h;
    } else {
        return -1;
    }
    uint64_t need = hdr_bytes + (uint64_t)sec->count * sizeof(arena_rec_t);
    for (unsigned i = 0; i < OPENPETE_ARENA_RANGES; i++) {
        if (sec->used[i] > s_seg[i].cap) return -1;
        need += sec->used[i];
    }
    if (need > size) return -1;
    sec->rows  = (const uint8_t*)src + hdr_bytes;
    sec->bytes = sec->rows + (size_t)sec->count * sizeof(arena_rec_t);
    for (uint32_t i = 0; i < sec->count; i++) {
        arena_rec_t a;
        rec_get(sec, i, &a);
        if (sec->ver == 3u && (a.flags & OP_GALLOC_NO_DMA)) return -1;
        if (!rec_in_bounds(&a, sec->used)) return -1;
    }
    return 1;
}

int openpete_mods_arena_state_verdict(const void* src, uint32_t size) {
    arena_section_t sec;
    int rc = arena_section_parse(src, size, &sec);
    if (rc == 0) return 0; /* no section */
    if (rc < 0) {
        OPL_ERROR_F("mods", "arena ledger: malformed section - REFUSING");
        return -1;
    }
    /* Exact prefix, superseded marks included. A saved row the live
     * ledger has since superseded is the reload case: the file's bytes (or
     * frame words) describe a block or handle the mod no longer holds. */
    uint32_t common = sec.count < row_count() ? sec.count : row_count();
    for (uint32_t i = 0; i < common; i++) {
        arena_rec_t rec;
        rec_get(&sec, i, &rec);
        const arena_rec_t* a = &rec;
        const arena_row_t* b = row_at(i);
        if (rec_matches(a, b)) continue;
        const char* what = b->kind == ROW_HANDLE ? "handle" : "entry";
        if (a->call_idx == b->call_idx && a->kind == b->kind &&
            strncmp(a->owner, b->owner, sizeof a->owner) == 0 &&
            !a->superseded && b->superseded) {
            OPL_ERROR_F("mods", "arena ledger: mod \"%s\" %s #%u (%s "
                        "0x%08X) was superseded by a reload after this "
                        "state was saved - REFUSING the load; load with the "
                        "same mod config and version the state was saved "
                        "under", b->owner, what, b->call_idx,
                        b->kind == ROW_HANDLE ? "stream" : "bytes @",
                        b->kind == ROW_HANDLE ? b->size : row_vaddr(b));
            return -1;
        }
        OPL_ERROR_F("mods", "arena ledger: row #%u diverges (saved %s %s "
                    "#%u %u/0x%08X vs live %s %s #%u %u/0x%08X) - REFUSING "
                    "the load; load with the same mod config and version "
                    "the state was saved under", i,
                    a->owner, a->kind == ROW_HANDLE ? "handle" : "entry",
                    a->call_idx, a->size, a->off,
                    b->owner, what, b->call_idx, b->size, b->off);
        return -1;
    }
    return 0;
}

void openpete_mods_arena_state_apply(const void* src, uint32_t size) {
    arena_section_t sec;
    if (arena_section_parse(src, size, &sec) != 1)
        return;
    /* Rows beyond the live count are adopted (the verdict proved the
     * common prefix matches), so a mod's later call at that index replays
     * the row the file holds - bytes restored below, handles bound here -
     * instead of bumping into a fresh block or a fresh index. */
    for (uint32_t i = row_count(); i < sec.count; i++) {
        arena_rec_t rec;
        rec_get(&sec, i, &rec);
        arena_row_t* r = row_push();
        if (!r) {
            OPL_ERROR_F("mods", "arena ledger: row allocation failed at "
                        "#%u - the rest of the saved rows are not adopted",
                        i);
            break;
        }
        memcpy(r->owner, rec.owner, sizeof r->owner);
        r->owner[sizeof r->owner - 1] = '\0';
        r->kind = rec.kind; r->call_idx = rec.call_idx;
        r->size = rec.size; r->align = rec.align;
        r->flags = rec.flags; r->off = rec.off;
        r->superseded = rec.superseded;
        if (r->kind == ROW_HANDLE) {
            (void)openpete_vstream_table_set(r->off, r->size);
            if (r->off >= s_next_handle) s_next_handle = r->off + 1;
        }
        use_add(r);
    }
    openpete_mods_arena_mem_restore(sec.bytes, sec.used);
    const uint32_t restored = sec.used[0] + sec.used[1] + sec.used[2];
    OPL_INFO_F("mods", "arena ledger applied: %u entries, %u B restored, "
               "section v%u, cursors upper %u/%u lower %u/%u general %u/%u",
               row_count(), restored, sec.ver,
               s_seg[OPENPETE_ARENA_RANGE_UPPER].used, UPPER_SIZE,
               s_seg[OPENPETE_ARENA_RANGE_LOWER].used, UPPER_OFF,
               s_seg[OPENPETE_ARENA_RANGE_GENERAL].used, GEN_SIZE);
}
