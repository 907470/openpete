/* core/include/openpete_rewind_pages.h - the rewind ring's mod arena copies.
 *
 * A rewind keyframe carries the mod arena's three used ranges (upper and
 * lower DMA-reachable segment, general pool) as a table of 4 KiB pages
 * held in a host page store: slabs mapped with mmap / VirtualAlloc,
 * never freed to the allocator while rewind is on, each page
 * reference-counted. A capture copies every used page into fresh pages.
 * The compare that follows walks the new table against another keyframe's,
 * a slice of pages per call, and replaces each page whose 4 KiB equal the
 * other table's page at the same range and offset by a reference to that
 * page, returning the copy to the store. Every table names every page of
 * its ranges, so a table restores on its own, compared or not.
 *
 * A table is pending from its capture until its compare completes or is
 * discarded; while pending it holds one page id per page. Completing
 * encodes it as extent runs (first page id, count), consecutive pages
 * with consecutive ids sharing a run (ids index pages across slabs; a run
 * names ids, not host addresses). One table is pending at
 * a time.
 *
 * Single-threaded: every call runs on the thread that drives rewind. */

#ifndef OPENPETE_REWIND_PAGES_H
#define OPENPETE_REWIND_PAGES_H

#include <stdint.h>

#include "openpete_mods.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RWP_PAGE_BYTES 4096u

/** One extent run: count pages with consecutive page ids from src. */
typedef struct {
    uint32_t src;
    uint32_t count;
} rwp_run_t;

/** One keyframe's arena table. Zero-initialised is the empty table. */
typedef struct {
    uint32_t   used[OPENPETE_ARENA_RANGES];   /* bytes per range */
    uint32_t   pages[OPENPETE_ARENA_RANGES];  /* ceil(used / 4 KiB) */
    uint32_t   npages;                         /* sum of pages[] */
    uint32_t*  flat;     /* pending: one page id per page; NULL once encoded */
    rwp_run_t* runs;     /* encoded: runs covering npages in range order */
    uint32_t   nruns;
    uint32_t   runs_cap;
} rwp_table_t;

/** Counters since process start; read by the rewind report and the tests. */
typedef struct {
    uint64_t captures;         /* tables captured */
    uint64_t capture_fails;    /* captures refused by page-store growth */
    uint64_t pages_copied;     /* pages copied by captures */
    uint64_t slices;           /* compare calls that compared at least one page */
    uint64_t pages_compared;   /* pages compared against a target */
    uint64_t pages_shared;     /* pages replaced by a reference to the target's */
    uint64_t compares_begun;   /* compares started against a target */
    uint64_t compares_no_target; /* captures encoded with no compare target */
    uint64_t compares_done;    /* compares that reached the last page */
    uint64_t discards;         /* compares discarded before the last page */
    uint64_t restores;         /* tables restored */
    uint64_t restores_pending; /* restores of a table whose compare was pending */
    uint64_t trims;            /* store releases that unmapped slabs */
    uint32_t slabs;            /* slabs mapped now */
    uint32_t slabs_peak;       /* most slabs mapped at once */
    uint32_t pages_live;       /* store pages with a nonzero count */
    uint32_t pages_live_peak;
} rwp_stats_t;

/** @brief Copy the arena's used ranges into fresh store pages, making @p t the pending table.
 *
 * Finishes the previous pending table's compare first. @p t must be empty
 * (released). The last page of each range is zero past the range's used
 * bytes.
 * @param t Table to fill.
 * @return 0 on success; -1 when the page store cannot grow or a table
 *         array cannot be allocated, with every page taken released and
 *         @p t left empty. */
int  rwp_capture(rwp_table_t* t);

/** @brief Start the compare of the pending table @p k against @p target, or encode @p k now when @p target is NULL.
 *
 * @param k The pending table from the last rwp_capture.
 * @param target A table other than @p k, not pending, or NULL. */
void rwp_compare_begin(rwp_table_t* k, const rwp_table_t* target);

/** @brief Report the pages the running compare has left.
 * @return Pages not yet compared; 0 when no compare runs. */
uint32_t rwp_compare_remaining(void);

/** @brief Compare up to @p max_pages pages of the running compare, sharing the equal ones; encode the table at the last page.
 * @param max_pages Pages to compare in this call.
 * @return Pages compared. */
uint32_t rwp_compare_step(uint32_t max_pages);

/** @brief Drop every reference @p t holds and leave it empty; a compare that reads or writes @p t is discarded first.
 *
 * A discarded compare encodes its pending table as compared so far. The
 * runs array keeps its allocation for the next capture.
 * @param t Table to release; an empty table is a no-op. */
void rwp_release(rwp_table_t* t);

/** @brief Free a released table's runs array.
 * @param t Table whose runs array is freed; it must be released. */
void rwp_table_free(rwp_table_t* t);

/** @brief Write @p t's pages back over the arena's ranges.
 *
 * Re-anchors each range (openpete_mods_arena_range_reanchor): bytes past
 * the table's used count keep their contents and no cursor decreases.
 * @param t Table to restore; may be pending. */
void rwp_restore(const rwp_table_t* t);

/** @brief Unmap every slab when no page is referenced.
 * @return Slabs unmapped; 0 when a page is still referenced or none is mapped. */
uint32_t rwp_store_trim(void);

/** @brief Report the page store's counters.
 * @return The counters. */
rwp_stats_t rwp_stats(void);

/** @brief Report the table whose compare is pending.
 * @return The pending table, or NULL when none is pending. */
const rwp_table_t* rwp_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_REWIND_PAGES_H */
