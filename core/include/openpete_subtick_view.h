/* core/include/openpete_subtick_view.h — read-only address view for the
 * sub-tick present path.
 *
 * PROBLEM. Sub-tick (intermediate) presents show interpolated state, and the
 * feeders that extract that state read guest memory through
 * openpete_psx_to_host_ptr(). Historically the only way to show a feeder
 * something other than canonical RAM was to WRITE the value into guest RAM,
 * run the feeder, and restore the canonical bytes afterwards. Two kinds of
 * value needed that treatment:
 *
 *   - interpolated poses (camera, player, per-object records), and
 *   - transient scratchpad regions that the guest renderer rebuilds each
 *     canonical pass and that are already overwritten by the time an
 *     intermediate runs (cull verdicts, segment scratch).
 *
 * That works, but it means a *display* path mutates gameplay-readable memory,
 * on a path the state-hash gates never execute — so a missed restore is
 * silent. It also makes concurrent extraction impossible: the writes race
 * whatever else looks at guest RAM.
 *
 * THIS VIEW. For the duration of an intermediate present the CPU's memory
 * read callbacks are swapped for view-aware ones, so every read a feeder
 * makes — whether through cpu->read_word() or openpete_subtick_host_ptr() —
 * resolves against staged bytes where a span covers the address and against
 * real memory everywhere else. The feeder sees the value the presenter wants
 * it to see, and guest memory is never touched.
 *
 * Swapping the callbacks is sound because the game fiber is PARKED for the
 * whole of an intermediate present: no guest code can observe them. It is
 * what makes this a ~zero-call-site change rather than an edit to the ~950
 * places feeders read guest memory.
 *
 * The view is READ-ONLY with respect to guest memory: staging copies bytes
 * out, never back.
 *
 * NOT a replacement for openpete_psx_to_host_ptr(). Recompiled guest code
 * must keep resolving against real memory — only the display-side feeders
 * resolve through the view.
 */

#ifndef OPENPETE_SUBTICK_VIEW_H
#define OPENPETE_SUBTICK_VIEW_H

#include <stdint.h>

#include "psx_runtime_internal.h"   /* CPUState */

#ifdef __cplusplus
extern "C" {
#endif

/* Open/close the view around one intermediate present. begin() drops every
 * span staged by the previous present and installs the view-aware read
 * callbacks; end() restores the originals. Calls do not nest, and every
 * begin() must be paired — leaving the view open would let guest code read
 * staged bytes on the next tick. */
void openpete_subtick_view_begin(CPUState* cpu);
void openpete_subtick_view_end(CPUState* cpu);

/* 1 while a view is open. Feeders that must behave differently on canonical
 * vs intermediate presents can ask, but most should not need to: resolving
 * through openpete_subtick_host_ptr() is the whole contract. */
int  openpete_subtick_view_active(void);

/* Stage [addr, addr+len) and return a writable host pointer to the copy,
 * pre-filled with the current bytes at `addr` so a caller may overwrite only
 * the fields it interpolates. Every subsequent resolve landing inside the
 * span returns a pointer into this copy for as long as the view is open.
 *
 * Returns NULL if the view is closed, the address is unmapped, or the staging
 * arena is exhausted; callers must fall back to leaving the value canonical
 * rather than writing guest memory. Re-staging an address already covered
 * returns the existing copy, so a region may be staged once and refined by
 * several producers.
 *
 * `len` must cover everything a feeder will read from the returned pointer —
 * a feeder that indexes past the staged span silently reads canonical bytes
 * from the next allocation, which is the one failure mode here that is not
 * self-announcing. */
void* openpete_subtick_view_stage(uint32_t addr, uint32_t len);

/* The feeder-facing resolver. Passthrough when the view is closed or the
 * address is unstaged.
 *
 * It resolves ONE address. A producer that writes a window wider than the
 * span covering its first address must use openpete_subtick_view_poke()
 * instead: the pointer returned here carries no length, so a memcpy past the
 * covering span's end lands in whatever the arena staged next. */
void* openpete_subtick_host_ptr(uint32_t addr);

/* Publish `len` bytes of producer output at guest `addr` through the view.
 * Bytes a staged span covers land in that copy; an uncovered run is staged
 * first, and a run staging refuses is DROPPED (that range keeps canonical
 * bytes for this present). Guest memory is never written. Returns the number
 * of dropped bytes, 0 when the whole window was routed. */
uint32_t openpete_subtick_view_poke(uint32_t addr, const void* src, uint32_t len);

/* Read `len` bytes at guest `addr` as the view presents them: staged where a
 * span covers, canonical elsewhere. The counterpart of poke for a producer
 * that saves a window before overwriting it. */
void openpete_subtick_view_peek(uint32_t addr, void* dst, uint32_t len);

/* Diagnostics: spans staged and reads served from staging during the most
 * recent present. Both are zero when nothing engaged — a green that reports
 * zero served reads has not exercised the view.
 *
 * `out_writes` counts writes to guest memory attempted while the view was
 * open. The target is zero: a write during an intermediate present is the
 * display path mutating gameplay-readable memory, which is the defect this
 * view exists to retire. Until every producer is migrated the count is the
 * honest measure of what is left, so it is reported rather than blocked —
 * blocking would corrupt the present instead of the state. */
void openpete_subtick_view_stats(uint32_t* out_spans, uint32_t* out_served,
                                 uint32_t* out_writes);

/* Presents this view has been opened for, for the run. Zero means an A/B or
 * gate never reached the sub-tick path, which makes any neutral result from it
 * vacuous rather than passing. */
uint32_t openpete_subtick_view_opens(void);

/* Writes absorbed by a staged span (so they never reached guest memory) and
 * bytes a poke could not route. The first is the engagement counter for the
 * write redirect: a zero means no producer wrote into staging this run, which
 * makes "no producer writes guest RAM" a statement about a path that may not
 * have executed. */
uint32_t openpete_subtick_view_staged_writes(void);
uint32_t openpete_subtick_view_poke_drops(void);

/* Log every distinct guest address written while a view was open, for the run.
 * This is the census that says which producers still publish through gameplay
 * memory -- note it sees only CPU-path writes, so a producer that writes a host
 * pointer directly is invisible to it and must be found another way. */
void openpete_subtick_view_report_writes(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SUBTICK_VIEW_H */
