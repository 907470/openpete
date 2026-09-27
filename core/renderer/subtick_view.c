/* core/renderer/subtick_view.c — see openpete_subtick_view.h.
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

#include <stdlib.h>
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
/* Entries are per (block, span) pair and duplicates are kept, so a dense pool
 * costs ~2 entries per record. Sized so the load factor stays under ~0.25 at
 * the worst staged set: a crowded table lengthens every probe chain, and a
 * full one silently drops spans to the canonical path. */
#define SV_IDX_SLOTS   32768u
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

/** @brief Insert a (block, span) entry into the open-addressed block index for the current generation.
 *
 * Linear probing from sv_hash(block) takes the first slot whose generation is
 * not s_gen. Entries are never merged by block, because one block can belong
 * to several spans: adjacent pool records share a boundary block whenever the
 * record stride is not a whole number of blocks or the pool base is
 * unaligned, and the transient-scratch rows overlap in PSX space. A merged
 * entry would hide the shadowed span, whose lookups would then read the
 * canonical bytes in guest memory instead of the staged copy. When every slot
 * is taken the entry is dropped: the span stays staged, and reads of that
 * block fall through to guest memory.
 * @param block Block number, addr >> SV_BLOCK_SHIFT.
 * @param span Index into s_span. */
static void sv_idx_insert(uint32_t block, uint32_t span)
{
    uint32_t i = sv_hash(block);
    for (uint32_t probe = 0; probe < SV_IDX_SLOTS; probe++) {
        if (s_idx[i].gen != s_gen) {        /* stale slots only; never merge */
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
static void     (*s_orig_write_half)(uint32_t, uint16_t);
static void     (*s_orig_write_byte)(uint32_t, uint8_t);
static void     (*s_orig_swl)(uint32_t, uint32_t);
static void     (*s_orig_swr)(uint32_t, uint32_t);
static uint32_t (*s_orig_lwl)(uint32_t, uint32_t);
static uint32_t (*s_orig_lwr)(uint32_t, uint32_t);

/* Unaligned loads that landed inside a staged span. They are NOT served from
 * staging -- lwl/lwr merge with the destination register and reimplementing
 * that here would be a second copy of subtle semantics. Counting them instead
 * turns "feeders probably do not use unaligned loads" from an assumption into
 * a measurement: while this is zero the gap is empty, and if it ever is not,
 * the reads it names are silently canonical and must be dealt with. */
static uint32_t s_stat_unaligned_reads;

/* Writes that landed in a staged span instead of guest memory, for the run. A
 * producer whose span is staged publishes into the copy, so the write never
 * reaches gameplay memory; counting them separately keeps s_stat_writes
 * meaning "writes that still reached guest RAM", and a run-wide total is what
 * says the redirect executed at all. */
static uint32_t s_stat_writes_staged;

/* Bytes a poke could route neither to a staged span nor to a span it could
 * stage. They are dropped rather than written to guest memory, so the present
 * shows canonical bytes for that range -- visible as a stale region, never as
 * a gameplay-state write. */
static uint32_t s_stat_poke_dropped;

/* stage() refusals. Every caller falls back to writing guest RAM when staging
 * is unavailable, which is correct but silent -- a producer that never manages
 * to stage would keep writing gameplay memory while every gate stayed green.
 * The refusal that is easiest to miss is a PARTIAL overlap: an address already
 * covered by a shorter span cannot be extended, so it returns NULL. Counting
 * refusals is what makes "no producer writes guest RAM any more" a measurement
 * rather than a hope. */
static uint32_t s_stat_stage_fail;

/* One-shot so an exhausted arena is loud once rather than per resolve. */
static int s_warned_full;

/* OPENPETE_SUBTICK_WRITE_TRACE=1 */
static int s_trace = -1;

/* Distinct guest addresses written while a view was open, for the whole run.
 * Per-present tracing is capped and shows only what one present did; a census
 * needs the union, so producers that fire rarely or only in certain scenes
 * still appear. Linear set -- it is tiny by construction, and if it ever is
 * not, that is itself the finding. */
#define SV_WSET_MAX 256u
static struct { uint32_t addr; uint32_t hits; } s_wset[SV_WSET_MAX];
static uint32_t s_wset_n;
static uint32_t s_wset_overflow;

static void sv_wset_note(uint32_t addr)
{
    for (uint32_t i = 0; i < s_wset_n; i++)
        if (s_wset[i].addr == addr) { s_wset[i].hits++; return; }
    if (s_wset_n < SV_WSET_MAX) {
        s_wset[s_wset_n].addr = addr;
        s_wset[s_wset_n].hits = 1;
        s_wset_n++;
    } else {
        s_wset_overflow++;
    }
}

void openpete_subtick_view_report_writes(void)
{
    if (s_stat_stage_fail)
        OPL_WARN_F("subtick-view",
                   "%u stage() refusal(s) -- that many producer writes went to "
                   "guest RAM instead of the view",
                   s_stat_stage_fail);
    if (s_stat_poke_dropped)
        OPL_WARN_F("subtick-view",
                   "%u byte(s) of producer output could not be staged and were "
                   "DROPPED -- that range shows canonical bytes this present",
                   s_stat_poke_dropped);
    if (s_stat_unaligned_reads)
        OPL_WARN_F("subtick-view",
                   "%u unaligned load(s) hit a staged span and read CANONICAL "
                   "bytes -- lwl/lwr are not served from staging",
                   s_stat_unaligned_reads);
    if (!s_wset_n) return;
    OPL_WARN_F("subtick-view",
               "%u distinct guest address(es) written during sub-tick presents%s:",
               s_wset_n, s_wset_overflow ? " (set full, more elided)" : "");
    for (uint32_t i = 0; i < s_wset_n; i++)
        OPL_WARN_F("subtick-view", "  [%08x] x%u", s_wset[i].addr, s_wset[i].hits);
}

/* Lifetime count of presents this view was opened for. An A/B that leaves it
 * at zero never exercised the view, and a neutral result from such a run says
 * nothing — the view resolves every address to real memory until a producer
 * stages a span, so "no pixels changed" is its expected output either way. */
static uint32_t s_opens;

uint32_t openpete_subtick_view_opens(void) { return s_opens; }

/* Index of the span covering `addr`, or -1. Declared ahead of the callbacks. */
static int sv_find(uint32_t addr);
static void sv_guard_begin(void);
static void sv_guard_end(void);

/** @brief Copy `n` bytes at `addr` from the staged span that covers them, when the view is active.
 *
 * Returns 0 when the view is inactive, no span covers `addr`, or the read
 * straddles the span's end; the caller then reads through the displaced
 * callback. A straddling read is not spliced: staged spans are whole records,
 * so a straddling read means the span was sized wrong, and a spliced value
 * would hide that. A served read increments s_stat_served.
 * @param addr Guest address of the first byte.
 * @param n Byte count.
 * @param out Destination for the bytes.
 * @return 1 when served from staging, else 0. */
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

/** @brief Store `n` bytes at `addr` into the staged span that covers the whole range, when the view is active.
 *
 * A store straddling a span's end is refused whole, for the reason
 * sv_staged_bytes refuses a straddling read. An absorbed store increments
 * s_stat_writes_staged.
 * @param addr Guest address of the first byte.
 * @param n Byte count.
 * @param src Source bytes.
 * @return 1 when the store was absorbed, 0 when the caller must write guest
 *         memory. */
static int sv_staged_store(uint32_t addr, uint32_t n, const void* src)
{
    if (!s_active) return 0;
    int hit = sv_find(addr);
    if (hit < 0) return 0;
    const sv_span_t* sp = &s_span[hit];
    if (sp->len - (addr - sp->addr) < n) return 0;
    memcpy(s_arena + sp->off + (addr - sp->addr), src, n);
    s_stat_writes_staged++;
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

/** @brief Guest halfword store during a present: absorbed by a covering staged span, else counted and performed.
 *
 * Every store width (word, half, byte, swl, swr) is counted, so a producer
 * writing guest memory in any width appears in the write census; the count
 * measures producers that still write guest memory during a present. A store
 * outside the staged spans is performed rather than refused, since refusing
 * it would change the present without protecting gameplay state.
 * @param addr Guest address.
 * @param v    Halfword to store. */
static void sv_write_half(uint32_t addr, uint16_t v)
{ if (sv_staged_store(addr, 2, &v)) return;
  s_stat_writes++; sv_wset_note(addr); s_orig_write_half(addr, v); }

static void sv_write_byte(uint32_t addr, uint8_t v)
{ if (sv_staged_store(addr, 1, &v)) return;
  s_stat_writes++; sv_wset_note(addr); s_orig_write_byte(addr, v); }

static void sv_swl(uint32_t addr, uint32_t v)
{ s_stat_writes++; sv_wset_note(addr); s_orig_swl(addr, v); }

static void sv_swr(uint32_t addr, uint32_t v)
{ s_stat_writes++; sv_wset_note(addr); s_orig_swr(addr, v); }

static uint32_t sv_lwl(uint32_t addr, uint32_t rt)
{ if (s_active && sv_find(addr) >= 0) s_stat_unaligned_reads++;
  return s_orig_lwl(addr, rt); }

static uint32_t sv_lwr(uint32_t addr, uint32_t rt)
{ if (s_active && sv_find(addr) >= 0) s_stat_unaligned_reads++;
  return s_orig_lwr(addr, rt); }

static void sv_write_word(uint32_t addr, uint32_t value)
{
    /* A staged span is the value this present publishes, so a write into one
     * belongs in the copy. Guest memory is reached only by a write nothing
     * staged, which is what the census and the RAM guard exist to report. */
    if (sv_staged_store(addr, 4, &value)) return;
    s_stat_writes++;
    sv_wset_note(addr);
    /* OPENPETE_SUBTICK_WRITE_TRACE names the addresses, which is what turns
     * "something wrote during a present" into a producer you can go and fix.
     * Capped per present so a runaway writer cannot flood the log. */
    if (s_trace && s_stat_writes <= 16)
        OPL_WARN_F("subtick-view", "guest write during present: [%08x] <- %08x",
                   addr, value);
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
    if (s_trace < 0) {
        const char* e = getenv("OPENPETE_SUBTICK_WRITE_TRACE");
        s_trace = (e && *e && *e != '0');
    }
    sv_guard_begin();

    if (!cpu) return;
    s_orig_read_word  = cpu->read_word;
    s_orig_read_half  = cpu->read_half;
    s_orig_read_byte  = cpu->read_byte;
    s_orig_write_word = cpu->write_word;
    s_orig_write_half = cpu->write_half;
    s_orig_write_byte = cpu->write_byte;
    s_orig_swl        = cpu->swl;
    s_orig_swr        = cpu->swr;
    s_orig_lwl        = cpu->lwl;
    s_orig_lwr        = cpu->lwr;
    cpu->read_word  = sv_read_word;
    cpu->read_half  = sv_read_half;
    cpu->read_byte  = sv_read_byte;
    cpu->write_word = sv_write_word;
    cpu->write_half = sv_write_half;
    cpu->write_byte = sv_write_byte;
    cpu->swl        = sv_swl;
    cpu->swr        = sv_swr;
    cpu->lwl        = sv_lwl;
    cpu->lwr        = sv_lwr;
}

void openpete_subtick_view_end(CPUState* cpu)
{
    sv_guard_end();
    s_active = 0;
    if (!cpu || !s_orig_read_word) return;
    cpu->read_word  = s_orig_read_word;
    cpu->read_half  = s_orig_read_half;
    cpu->read_byte  = s_orig_read_byte;
    cpu->write_word = s_orig_write_word;
    cpu->write_half = s_orig_write_half;
    cpu->write_byte = s_orig_write_byte;
    cpu->swl        = s_orig_swl;
    cpu->swr        = s_orig_swr;
    cpu->lwl        = s_orig_lwl;
    cpu->lwr        = s_orig_lwr;
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

uint32_t openpete_subtick_view_staged_writes(void) { return s_stat_writes_staged; }
uint32_t openpete_subtick_view_poke_drops(void)    { return s_stat_poke_dropped; }

static int sv_find(uint32_t addr)
{
    if (!s_span_count) return -1;
    const uint32_t block = addr >> SV_BLOCK_SHIFT;
    uint32_t i = sv_hash(block);
    int best = -1;
    for (uint32_t probe = 0; probe < SV_IDX_SLOTS; probe++) {
        if (s_idx[i].gen != s_gen) break;        /* stale slot ends the chain */
        if (s_idx[i].block == block) {
            uint32_t sp = s_idx[i].span;
            /* A span need not cover the whole of its first or last block, so
             * every candidate is coverage-tested rather than trusted. */
            if (addr >= s_span[sp].addr && addr - s_span[sp].addr < s_span[sp].len
                && (int)sp > best)
                best = (int)sp;                  /* latest staged span wins */
        }
        i = (i + 1u) & SV_IDX_MASK;
    }
    return best;
}

void* openpete_subtick_view_stage(uint32_t addr, uint32_t len)
{
    /* OPENPETE_SUBTICK_VIEW=0 refuses every staging request, so every producer
     * falls back to its write-and-restore path against guest RAM. That is the
     * pre-view behaviour, kept reachable as the A/B for this whole change --
     * and it is what makes the RAM guard falsifiable, since a guard that has
     * only ever reported zero has not been shown capable of reporting more. */
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("OPENPETE_SUBTICK_VIEW");
        enabled = (e && *e) ? (*e != '0') : 1;
        if (!enabled)
            OPL_WARN_F("subtick-view", "%s",
                       "DISABLED — producers write guest RAM directly (legacy path)");
    }
    if (!enabled) return NULL;
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
        s_stat_stage_fail++;
        return NULL;   /* caller keeps the value canonical rather than guessing */
    }

    /* Real bytes, deliberately not via the view — staging seeds from
     * canonical memory, and a span never seeds from another span. */
    const void* live = openpete_psx_to_host_span(addr, len);
    if (!live) { s_stat_stage_fail++; return NULL; }

    if (s_span_count >= SV_MAX_SPANS || len > SV_ARENA_BYTES - s_arena_used) {
        s_stat_stage_fail++;
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

/* Walks the window span by span; see the header. */
uint32_t openpete_subtick_view_poke(uint32_t addr, const void* src, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)src;
    uint32_t dropped = 0;
    /* Closed view: nothing is staged and guest memory is not this call's to
     * write, so the whole window is reported dropped rather than landing
     * anywhere. The span index still holds the last present's entries, so it
     * must not be consulted either. */
    if (!s_active) { s_stat_poke_dropped += len; return len; }
    while (len) {
        int hit = sv_find(addr);
        if (hit >= 0) {
            const sv_span_t* sp = &s_span[hit];
            uint32_t avail = sp->len - (addr - sp->addr);
            uint32_t n = avail < len ? avail : len;
            memcpy(s_arena + sp->off + (addr - sp->addr), p, n);
            s_stat_writes_staged++;
            addr += n; p += n; len -= n;
            continue;
        }
        /* Length of the uncovered run, probed a block at a time because a
         * span boundary can only fall on a block the index knows about. */
        uint32_t run = SV_BLOCK - (addr & (SV_BLOCK - 1u));
        if (run > len) run = len;
        while (run < len && sv_find(addr + run) < 0) {
            uint32_t step = SV_BLOCK;
            if (run + step > len) step = len - run;
            run += step;
        }
        uint8_t* st = (uint8_t*)openpete_subtick_view_stage(addr, run);
        if (st) memcpy(st, p, run);
        else { dropped += run; s_stat_poke_dropped += run; }
        addr += run; p += run; len -= run;
    }
    return dropped;
}

/* Staged where a span covers, canonical elsewhere; see the header. */
void openpete_subtick_view_peek(uint32_t addr, void* dst, uint32_t len)
{
    uint8_t* p = (uint8_t*)dst;
    while (len) {
        uint32_t n = len;
        int hit = s_active ? sv_find(addr) : -1;
        const uint8_t* srcp = NULL;
        if (hit >= 0) {
            const sv_span_t* sp = &s_span[hit];
            uint32_t avail = sp->len - (addr - sp->addr);
            if (avail < n) n = avail;
            srcp = s_arena + sp->off + (addr - sp->addr);
            s_stat_served++;
        } else {
            uint32_t run = SV_BLOCK - (addr & (SV_BLOCK - 1u));
            if (run > n) run = n;
            while (run < n && sv_find(addr + run) < 0) {
                uint32_t step = SV_BLOCK;
                if (run + step > n) step = n - run;
                run += step;
            }
            n = run;
            srcp = (const uint8_t*)openpete_psx_to_host_span(addr, n);
        }
        if (srcp) memcpy(p, srcp, n);
        else memset(p, 0, n);
        addr += n; p += n; len -= n;
    }
}

void* openpete_subtick_host_span(uint32_t addr, uint32_t len)
{
    if (s_active) {
        int hit = sv_find(addr);
        if (hit >= 0) {
            const sv_span_t* sp = &s_span[hit];
            if (sp->len - (addr - sp->addr) < len) return NULL;
            s_stat_served++;
            return s_arena + sp->off + (addr - sp->addr);
        }
    }
    return openpete_psx_to_host_span(addr, len);
}

void* openpete_subtick_host_ptr(uint32_t addr)
{
    return openpete_subtick_host_span(addr, 1u);
}

/* ---- guest-RAM write guard ------------------------------------------------
 * The census above sees CPU-path writes. It is blind to a write through a host
 * pointer, which is how the bracket producers always wrote, so "no producer
 * writes guest RAM any more" rested on having read the right source files --
 * and that enumeration had already missed one producer.
 *
 * This settles it by construction instead: with OPENPETE_SUBTICK_RAM_GUARD=1,
 * main guest RAM is mapped read-only for the duration of every sub-tick
 * present. Any writer faults, whatever path it took to get the address, and
 * the handler names the faulting address before letting the write proceed, so
 * a run reports rather than dies.
 *
 * Resolution limit, because the number it prints is easy to over-read: letting
 * the write proceed means unprotecting for the REST of that present, so the
 * guard reports the FIRST writer per present, not all of them. Across many
 * presents different writers surface; a run that reports one violation has not
 * shown there is only one. Zero across many presents is the meaningful
 * result.
 *
 * Debug-gated and POSIX-only: it is an auditing tool for the correctness
 * machine, not something the shipped renderer pays for. Scratchpad is NOT
 * covered -- it is a sub-page allocation, and it holds the .s renderer's
 * transient workspace rather than gameplay state.
 */
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

static int  s_guard = -1;
static int  s_guard_armed;
static uint32_t s_guard_hits;
static struct sigaction s_prev_segv;

static void sv_guard_unprotect(void)
{
    uint8_t* ram = openpete_psx_get_ram();
    if (ram) mprotect(ram, OPENPETE_PSX_RAM_SIZE, PROT_READ | PROT_WRITE);
    s_guard_armed = 0;
}

static void sv_segv(int sig, siginfo_t* si, void* uc)
{
    uint8_t* ram = openpete_psx_get_ram();
    uint8_t* fa  = (uint8_t*)si->si_addr;
    if (s_guard_armed && ram && fa >= ram && fa < ram + OPENPETE_PSX_RAM_SIZE) {
        s_guard_hits++;
        OPL_WARN_F("subtick-view",
                   "GUEST RAM WRITTEN during a sub-tick present: [%08x] "
                   "(host %p) — a producer is still publishing through "
                   "gameplay memory",
                   OPENPETE_PSX_KSEG0_BASE + (uint32_t)(fa - ram), (void*)fa);
        /* Let it through: the point is to enumerate every writer in one run,
         * not to halt at the first. */
        sv_guard_unprotect();
        return;
    }
    if (s_prev_segv.sa_flags & SA_SIGINFO) s_prev_segv.sa_sigaction(sig, si, uc);
    else if (s_prev_segv.sa_handler)       s_prev_segv.sa_handler(sig);
    else                                   _exit(139);
}

static void sv_guard_begin(void)
{
    if (s_guard < 0) {
        const char* e = getenv("OPENPETE_SUBTICK_RAM_GUARD");
        s_guard = (e && *e && *e != '0');
        if (s_guard) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = sv_segv;
            sa.sa_flags = SA_SIGINFO | SA_NODEFER;
            sigaction(SIGSEGV, &sa, &s_prev_segv);
            OPL_INFO_F("subtick-view", "%s",
                       "RAM guard armed: guest RAM is read-only during presents");
        }
    }
    if (!s_guard) return;
    uint8_t* ram = openpete_psx_get_ram();
    if (ram && mprotect(ram, OPENPETE_PSX_RAM_SIZE, PROT_READ) == 0)
        s_guard_armed = 1;
}

static void sv_guard_end(void)
{
    if (s_guard > 0 && s_guard_armed) sv_guard_unprotect();
}

uint32_t openpete_subtick_view_guard_hits(void) { return s_guard_hits; }
#else
static void sv_guard_begin(void) {}
static void sv_guard_end(void)   {}
uint32_t openpete_subtick_view_guard_hits(void) { return 0; }
#endif
