// core/mods/mod_arena.c - the guest arena (api->guest_alloc) and the
// stream-handle ledger (api->vstream_handle).
//
// Mods need data at a GUEST-addressable vaddr - marshalled calls whose
// parameters are guest pointers, model payloads the game chases. openpete
// owns the memory map, so the arena is a host buffer registered as a
// psx_runtime region in the >2MB expansion window: guest reads/writes
// reach it through the standard cpu accessors' region cold path, and the
// mod gets a typed HOST view of the same bytes. It is the ONLY home for
// mod memory: the moby format's one narrow address field (the 21-bit
// vertex-stream field) is bridged by stream handles, so nothing needs to
// sit inside canonical RAM.
//
// Contracts and caveats (documented, not hidden):
//   - Allocation is a session-lifetime bump (no free): the arena is
//     scratch, not a heap. 8 MB v1 (static); growth is one reserve-then-commit mapping.
//   - Timeline state: the arena's used prefix rides every snapshot
//     (in-memory keyframes and runahead, file savestates, handoff) the
//     way guest RAM does, restored as a re-anchor (bytes allocated
//     after the snapshot keep their contents; the cursor never moves
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
// Allocation itself is legal at a first enable: the region registers
// lazily on the first successful call, and the tick boundary that runs a
// deferred entry is a quiescent guest-access point like any other.

#include "openpete_mods.h"
#include "openpete_log.h"
#include "openpete_fnv1a64.h"
#include "mod_vec.h"

#include "psx_runtime_internal.h" /* psx_runtime_register_region */
#include "openpete_vstream.h" /* stream-handle table (vstream_handle) */

#include <stdio.h>
#include <string.h>

#define ARENA_BASE 0x80700000u /* clear of the no-lod/polybuf windows */
#define ARENA_SIZE (8u * 1024u * 1024u) /* v1 static; a total conversion at ~3000 streams fits */

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
    uint32_t off;        /* alloc: arena offset (vaddr = ARENA_BASE + off); handle: index */
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

/** @brief Register the arena as a psx_runtime region on first use and report whether it is live.
 *
 * The first call attempts the registration once and logs its outcome; a
 * failure is latched, so every later call returns 0 without retrying.
 * @return 1 when the region is registered, else 0. */
static int arena_register(void) {
    if (s_state == 0) {
        s_state = (psx_runtime_register_region(ARENA_BASE, ARENA_SIZE,
                                               s_arena) == 0) ? 1 : -1;
        if (s_state < 0)
            OPL_ERROR_F("mods", "guest arena: region registration failed "
                        "(window overlap?) - allocations disabled");
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
            if (host_view) *host_view = &s_arena[r->off];
            OPL_INFO_F("mods", "guest arena: %s call #%u replayed "
                       "(%u B @0x%08X, bytes kept)", owner, call_idx,
                       size, ARENA_BASE + r->off);
            return ARENA_BASE + r->off;
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

    uint32_t off = (s_used + (align - 1)) & ~(align - 1);
    if (off + size > ARENA_SIZE) {
        OPL_ERROR_F("mods", "guest arena exhausted (%s wanted %u B, "
                    "%u/%u used)", owner, size, s_used, ARENA_SIZE);
        return 0;
    }
    o->next_call = call_idx + 1;
    s_used = off + size;
    memset(&s_arena[off], 0, size); /* zero on first allocation */

    arena_row_t* r = row_push();
    if (!r) {
        OPL_ERROR_F("mods", "guest arena ledger allocation failed - %s "
                    "call #%u refused", owner, call_idx);
        return 0;
    }
    snprintf(r->owner, sizeof r->owner, "%s", owner);
    r->kind = ROW_ALLOC;
    r->call_idx = call_idx;
    r->size = size; r->align = align; r->flags = flags; r->off = off;

    if (host_view) *host_view = &s_arena[off];
    return ARENA_BASE + off;
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

void openpete_mods_arena_entry_begin(const char* owner) {
    if (!owner || !*owner) return;
    arena_owner_t* o = owner_slot(owner, 0);
    if (o) o->next_call = 0;
}

uint32_t       openpete_mods_arena_used(void)  { return s_used; }
const uint8_t* openpete_mods_arena_bytes(void) { return s_arena; }

void openpete_mods_arena_mem_restore(const void* bytes, uint32_t used) {
    if (!bytes || !used) return;
    if (used > ARENA_SIZE) used = ARENA_SIZE;
    /* Re-anchor, never replace: bytes allocated after the snapshot keep
     * their contents and the cursor never moves backwards, so a handle
     * a mod took mid-window stays valid across the restore. */
    memcpy(s_arena, bytes, used);
    if (used > s_used) s_used = used;
}

/* Immutable rows carry a checksum captured the first time they are
 * checked (the first snapshot after the allocating tick). A later
 * mismatch is the mod's promise broken - warned once per row, with the
 * owner and call index, so the flag is a falsifiable claim rather than
 * a hint the engine takes on faith. */
void openpete_mods_arena_immutable_check(const char* when) {
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_row_t* r = row_at(i);
        if (!(r->flags & OP_GALLOC_IMMUTABLE) || r->superseded) continue;
        if (r->sum_state == 2) continue;
        uint64_t h = openpete_fnv1a64(&s_arena[r->off], r->size);
        if (r->sum_state == 0) {
            r->sum = h;
            r->sum_state = 1;
        } else if (h != r->sum) {
            r->sum_state = 2;
            OPL_WARN_F("mods", "guest arena: %s call #%u is marked "
                       "IMMUTABLE but its bytes changed (%u B @0x%08X, "
                       "seen at %s) - the promise is broken; drop the "
                       "flag or stop writing the block", r->owner,
                       r->call_idx, r->size, ARENA_BASE + r->off,
                       when ? when : "snapshot");
        }
    }
}


/* ── Ledger serialization (see openpete_mods.h; consumed by savestate.c)
 *
 * Section layout (v3):
 *   header
 *   rows [count]           - the ledger, superseded marks included
 *   arena bytes [used]     - the arena's used prefix
 * Verdict runs PRE-COMMIT: the ledger must prefix-match the live session
 * exactly, or the restored bytes would land under a different layout and
 * every vaddr (and every handle) parked inside them would be garbage. */

typedef struct {
    uint32_t ver;        /* 3 */
    uint32_t count;      /* rows */
    uint32_t used;       /* arena watermark at save == byte payload size */
} arena_state_hdr_t;

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

uint32_t openpete_mods_arena_state_size(void) {
    return (uint32_t)(sizeof(arena_state_hdr_t)
                      + row_count() * sizeof(arena_rec_t) + s_used);
}

void openpete_mods_arena_state_save(void* dst) {
    arena_state_hdr_t h;
    h.ver = 3u;
    h.count = row_count();
    h.used = s_used;
    uint8_t* p = (uint8_t*)dst;
    memcpy(p, &h, sizeof h);                       p += sizeof h;
    for (uint32_t i = 0; i < row_count(); i++) {
        arena_rec_t rec;
        rec_from_row(&rec, row_at(i));
        memcpy(p, &rec, sizeof rec);               p += sizeof rec;
    }
    memcpy(p, s_arena, s_used);
}

/** @brief Split a saved arena section into its header, row records and byte payload, checking bounds.
 *
 * The outputs point into src; nothing is copied except the header.
 * @param src the section bytes; NULL is treated as no section.
 * @param size the section size in bytes.
 * @param h receives the header.
 * @param rows receives the first row record.
 * @param bytes receives the first payload byte.
 * @return 1 when parsed, 0 for no section (NULL src or a size below the
 *         header), -1 for a version other than 3, a section too short for
 *         its rows and payload, or a payload over the arena size. */
static int arena_section_parse(const void* src, uint32_t size,
                               arena_state_hdr_t* h,
                               const arena_rec_t** rows,
                               const uint8_t** bytes) {
    if (!src || size < sizeof *h) return 0;
    memcpy(h, src, sizeof *h);
    if (h->ver != 3u) return -1;
    uint64_t need = sizeof *h + (uint64_t)h->count * sizeof(arena_rec_t)
                  + h->used;
    if (need > size || h->used > ARENA_SIZE) return -1;
    const uint8_t* p = (const uint8_t*)src + sizeof *h;
    *rows  = (const arena_rec_t*)p;  p += h->count * sizeof(arena_rec_t);
    *bytes = p;
    return 1;
}

int openpete_mods_arena_state_verdict(const void* src, uint32_t size) {
    arena_state_hdr_t h;
    const arena_rec_t* rows; const uint8_t* bytes;
    int rc = arena_section_parse(src, size, &h, &rows, &bytes);
    if (rc == 0) return 0; /* no section */
    if (rc < 0) {
        OPL_ERROR_F("mods", "arena ledger: malformed section - REFUSING");
        return -1;
    }
    /* Exact prefix, superseded marks included. A saved row the live
     * ledger has since superseded is the reload case: the file's bytes (or
     * frame words) describe a block or handle the mod no longer holds. */
    uint32_t common = h.count < row_count() ? h.count : row_count();
    for (uint32_t i = 0; i < common; i++) {
        const arena_rec_t* a = &rows[i];
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
                        b->kind == ROW_HANDLE ? "stream" : "bytes @+",
                        b->kind == ROW_HANDLE ? b->size : b->off);
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
    arena_state_hdr_t h;
    const arena_rec_t* rows; const uint8_t* bytes;
    if (arena_section_parse(src, size, &h, &rows, &bytes) != 1)
        return;
    /* Rows beyond the live count are adopted (the verdict proved the
     * common prefix matches), so a mod's later call at that index replays
     * the row the file holds - bytes restored below, handles bound here -
     * instead of bumping into a fresh block or a fresh index. */
    for (uint32_t i = row_count(); i < h.count; i++) {
        arena_row_t* r = row_push();
        if (!r) {
            OPL_ERROR_F("mods", "arena ledger: row allocation failed at "
                        "#%u - the rest of the saved rows are not adopted",
                        i);
            break;
        }
        memcpy(r->owner, rows[i].owner, sizeof r->owner);
        r->owner[sizeof r->owner - 1] = '\0';
        r->kind = rows[i].kind; r->call_idx = rows[i].call_idx;
        r->size = rows[i].size; r->align = rows[i].align;
        r->flags = rows[i].flags; r->off = rows[i].off;
        r->superseded = rows[i].superseded;
        if (r->kind == ROW_HANDLE) {
            (void)openpete_vstream_table_set(r->off, r->size);
            if (r->off >= s_next_handle) s_next_handle = r->off + 1;
        }
    }
    if (s_state == 0 && h.used) (void)arena_register();
    openpete_mods_arena_mem_restore(bytes, h.used);
    OPL_INFO_F("mods", "arena ledger applied: %u entries, %u B restored, "
               "cursor %u/%u", row_count(), h.used, s_used, ARENA_SIZE);
}
