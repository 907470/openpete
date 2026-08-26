/* core/runtime/subtick_view.c — see openpete_subtick_view.h.
 *
 * Lookup is on the hot path: every guest read a feeder makes during an
 * intermediate present goes through it, and the per-object pools stage on the
 * order of a thousand spans (one per live record). A scan over that per read
 * is not viable, so spans are indexed by 32-byte block.
 *
 * Every staged span is at least SV_BLOCK bytes and spans never overlap, so a
 * block maps to at most one span and an open-addressed table keyed on
 * addr >> 5 answers in O(1). Staging inserts one entry per block a span
 * covers; a span of 0x58 bytes costs 2-3 entries.
 */

#include "openpete_subtick_view.h"

#include <string.h>

#include "openpete_psx.h"
#include "openpete_log.h"

/* Sized for the per-object pools: one span per interpolated record, plus the
 * fixed regions and the transient scratch rows. */
#define SV_MAX_SPANS   2048u
/* Backing arena. The pools dominate: ~1024 object records plus ~512 particle
 * slots, at their natural strides, plus the fixed regions. */
#define SV_ARENA_BYTES (256u * 1024u)

typedef struct {
    uint32_t addr;    /* PSX vaddr of the span's first byte */
    uint32_t len;
    uint32_t off;     /* offset into s_arena */
} sv_span_t;

/* Block index. Power of two, open addressed, linear probe. Sized so the load
 * factor stays under ~0.35 at the worst staged set (every pool record live),
 * because a full table degrades to a scan of exactly the kind this replaces. */
#define SV_BLOCK       32u
#define SV_BLOCK_SHIFT 5u
#define SV_IDX_SLOTS   16384u
#define SV_IDX_MASK    (SV_IDX_SLOTS - 1u)

typedef struct {
    uint32_t block;   /* addr >> SV_BLOCK_SHIFT */
    uint32_t span;    /* index into s_span */
    uint32_t gen;     /* present this entry was written for */
} sv_idx_t;

static sv_idx_t s_idx[SV_IDX_SLOTS];
/* Generation stamp instead of clearing 16K slots per present. Starts at 1 so
 * the zero-initialised table reads as stale on the first present. */
static uint32_t s_gen = 1;

static uint32_t sv_hash(uint32_t block)
{
    /* Knuth multiplicative; block numbers are dense and highly correlated
     * (pool records are contiguous), so the low bits alone would clump. */
    return (block * 2654435761u) & SV_IDX_MASK;
}

/* A later span covering a block WINS. Spans are normally disjoint, but the
 * transient-scratch rows deliberately overlap in PSX space and are applied in
 * a defined order — last-writer-wins reproduces that ordering exactly, where
 * refusing the overlap would not. */
static void sv_idx_insert(uint32_t block, uint32_t span)
{
    uint32_t i = sv_hash(block);
    for (uint32_t probe = 0; probe < SV_IDX_SLOTS; probe++) {
        if (s_idx[i].gen != s_gen || s_idx[i].block == block) {
            s_idx[i].gen   = s_gen;
            s_idx[i].block = block;
            s_idx[i].span  = span;
            return;
        }
        i = (i + 1u) & SV_IDX_MASK;
    }
    /* Table full: the span stays staged but this block is unfindable, so reads
     * of it fall through to real memory -- canonical, never wrong. */
}

static int sv_idx_lookup(uint32_t block)
{
    uint32_t i = sv_hash(block);
    for (uint32_t probe = 0; probe < SV_IDX_SLOTS; probe++) {
        if (s_idx[i].gen != s_gen) return -1;      /* stale slot ends the probe */
        if (s_idx[i].block == block) return (int)s_idx[i].span;
        i = (i + 1u) & SV_IDX_MASK;
    }
    return -1;
}

static sv_span_t s_span[SV_MAX_SPANS];
static uint32_t  s_span_count;
static uint8_t   s_arena[SV_ARENA_BYTES];
static uint32_t  s_arena_used;
static int       s_active;

/* Engagement counters, reported per present. */
static uint32_t s_stat_spans;
static uint32_t s_stat_served;
static uint32_t s_stat_writes;

/* The CPU callbacks displaced for the duration of the view. */
static uint32_t (*s_orig_read_word)(uint32_t);
static uint16_t (*s_orig_read_half)(uint32_t);
static uint8_t  (*s_orig_read_byte)(uint32_t);
static void     (*s_orig_write_word)(uint32_t, uint32_t);

/* One-shot so an exhausted arena is loud once rather than per resolve. */
static int s_warned_full;

/* Lifetime count of presents this view was opened for. An A/B that leaves it
 * at zero never exercised the view, and a neutral result from such a run says
 * nothing — the view resolves every address to real memory until a producer
 * stages a span, so "no pixels changed" is its expected output either way. */
static uint32_t s_opens;

uint32_t openpete_subtick_view_opens(void) { return s_opens; }

/* Index of the span covering `addr`, or -1. Declared ahead of the callbacks. */
static int sv_find(uint32_t addr);

/* Read `n` bytes at `addr` from staging if covered, else from the displaced
 * callback. A read straddling the end of a span falls through to real memory
 * whole rather than splicing: staged spans are whole records, so a straddling
 * read means the span was sized wrong, and a spliced value would hide that. */
static int sv_staged_bytes(uint32_t addr, uint32_t n, void* out)
{
    if (!s_active) return 0;
    int hit = sv_find(addr);
    if (hit < 0) return 0;
    const sv_span_t* sp = &s_span[hit];
    if (sp->len - (addr - sp->addr) < n) return 0;
    memcpy(out, s_arena + sp->off + (addr - sp->addr), n);
    s_stat_served++;
    return 1;
}

static uint32_t sv_read_word(uint32_t addr)
{
    uint32_t v;
    if (sv_staged_bytes(addr, 4, &v)) return v;
    return s_orig_read_word(addr);
}

static uint16_t sv_read_half(uint32_t addr)
{
    uint16_t v;
    if (sv_staged_bytes(addr, 2, &v)) return v;
    return s_orig_read_half(addr);
}

static uint8_t sv_read_byte(uint32_t addr)
{
    uint8_t v;
    if (sv_staged_bytes(addr, 1, &v)) return v;
    return s_orig_read_byte(addr);
}

/* Writes are counted, then performed. See the header: the count is the
 * measure of un-migrated producers, and refusing the write would damage the
 * present rather than protect anything. */
static void sv_write_word(uint32_t addr, uint32_t value)
{
    s_stat_writes++;
    s_orig_write_word(addr, value);
}

void openpete_subtick_view_begin(CPUState* cpu)
{
    s_span_count  = 0;
    s_arena_used  = 0;
    s_gen++;
    if (s_gen == 0) s_gen = 1;   /* skip 0: it means "never written" */
    s_stat_spans  = 0;
    s_stat_served = 0;
    s_stat_writes = 0;
    s_active      = 1;
    s_opens++;

    if (!cpu) return;
    s_orig_read_word  = cpu->read_word;
    s_orig_read_half  = cpu->read_half;
    s_orig_read_byte  = cpu->read_byte;
    s_orig_write_word = cpu->write_word;
    cpu->read_word  = sv_read_word;
    cpu->read_half  = sv_read_half;
    cpu->read_byte  = sv_read_byte;
    cpu->write_word = sv_write_word;
}

void openpete_subtick_view_end(CPUState* cpu)
{
    s_active = 0;
    if (!cpu || !s_orig_read_word) return;
    cpu->read_word  = s_orig_read_word;
    cpu->read_half  = s_orig_read_half;
    cpu->read_byte  = s_orig_read_byte;
    cpu->write_word = s_orig_write_word;
    s_orig_read_word = NULL;
}

int openpete_subtick_view_active(void)
{
    return s_active;
}

void openpete_subtick_view_stats(uint32_t* out_spans, uint32_t* out_served,
                                 uint32_t* out_writes)
{
    if (out_spans)  *out_spans  = s_stat_spans;
    if (out_served) *out_served = s_stat_served;
    if (out_writes) *out_writes = s_stat_writes;
}

static int sv_find(uint32_t addr)
{
    if (!s_span_count) return -1;
    int sp = sv_idx_lookup(addr >> SV_BLOCK_SHIFT);
    if (sp < 0) return -1;
    /* The block matched, but a span need not cover the whole of its first or
     * last block — verify the address really lands inside it. */
    if (addr >= s_span[sp].addr && addr - s_span[sp].addr < s_span[sp].len)
        return sp;
    return -1;
}

void* openpete_subtick_view_stage(uint32_t addr, uint32_t len)
{
    if (!s_active || !len) return NULL;

    /* Already covered: hand back the existing copy so several producers can
     * refine one region without each getting a private (and divergent) one.
     * Only exact-prefix coverage is safe to reuse — a partial overlap would
     * hand back a buffer shorter than the caller intends to write. */
    int hit = sv_find(addr);
    if (hit >= 0) {
        const sv_span_t* sp = &s_span[hit];
        uint32_t avail = sp->len - (addr - sp->addr);
        if (avail >= len)
            return s_arena + sp->off + (addr - sp->addr);
        return NULL;   /* caller keeps the value canonical rather than guessing */
    }

    /* Real bytes, deliberately not via the view — staging seeds from
     * canonical memory, and a span never seeds from another span. */
    const void* live = openpete_psx_to_host_ptr(addr);
    if (!live) return NULL;

    if (s_span_count >= SV_MAX_SPANS || len > SV_ARENA_BYTES - s_arena_used) {
        if (!s_warned_full) {
            s_warned_full = 1;
            OPL_WARN_F("subtick-view",
                       "staging exhausted (%u spans, %u/%u bytes) — regions past "
                       "this point stay canonical this present",
                       s_span_count, s_arena_used, (unsigned)SV_ARENA_BYTES);
        }
        return NULL;
    }

    uint32_t si = s_span_count++;
    sv_span_t* sp = &s_span[si];
    sp->addr = addr;
    sp->len  = len;
    sp->off  = s_arena_used;
    s_arena_used += len;
    s_stat_spans++;

    for (uint32_t b = addr >> SV_BLOCK_SHIFT;
         b <= (addr + len - 1u) >> SV_BLOCK_SHIFT; b++)
        sv_idx_insert(b, si);

    memcpy(s_arena + sp->off, live, len);
    return s_arena + sp->off;
}

void* openpete_subtick_host_ptr(uint32_t addr)
{
    if (s_active) {
        int hit = sv_find(addr);
        if (hit >= 0) {
            s_stat_served++;
            return s_arena + s_span[hit].off + (addr - s_span[hit].addr);
        }
    }
    return openpete_psx_to_host_ptr(addr);
}
