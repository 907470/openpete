/* core/include/openpete_subtick_view.h - read-only address view for the
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
 * on a path the state-hash gates never execute - so a missed restore is
 * silent. It also makes concurrent extraction impossible: the writes race
 * whatever else looks at guest RAM.
 *
 * THIS VIEW. For the duration of an intermediate present the CPU's memory
 * read callbacks are swapped for view-aware ones, so every read a feeder
 * makes - whether through cpu->read_word() or openpete_subtick_host_ptr() -
 * resolves against staged bytes where a span covers the address and against
 * real memory everywhere else. The feeder sees the value the presenter wants
 * it to see, and staged bytes never reach guest memory.
 *
 * Swapping the callbacks is sound because the game fiber is PARKED for the
 * whole of an intermediate present: no guest code can observe them. It is
 * what makes this a ~zero-call-site change rather than an edit to the ~950
 * places feeders read guest memory.
 *
 * Staging copies bytes out of guest memory, never back. A CPU store during
 * the view (word, half, byte, swl, swr) lands in the staged span covering
 * its bytes; one that no span covers is dropped and counted per host call
 * site, so guest memory is never written through the CPU callbacks.
 *
 * NOT a replacement for openpete_psx_to_host_ptr(). Recompiled guest code
 * must keep resolving against real memory - only the display-side feeders
 * resolve through the view.
 */

#ifndef OPENPETE_SUBTICK_VIEW_H
#define OPENPETE_SUBTICK_VIEW_H

#include <stdint.h>

#include "psx_runtime_internal.h"   /* CPUState */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Open the view around one re-extract pass.
 *
 * games/common/renderer/reextract_view.c brackets each game re-extract with
 * begin and openpete_subtick_view_end. Drops every span staged by the
 * previous pass (a generation bump invalidates the block index), zeroes the
 * per-present counters, counts the open, arms the RAM guard when
 * OPENPETE_SUBTICK_RAM_GUARD is set, and swaps the CPU's read, write, swl,
 * swr, lwl and lwr callbacks for view-aware ones. Calls do not nest, and
 * every begin must be paired: a view left open would let guest code read
 * staged bytes on the next tick.
 * @param cpu CPU whose callbacks are swapped; NULL opens the view without
 *        swapping them. */
void openpete_subtick_view_begin(CPUState* cpu);
/** @brief Close the view and restore the CPU callbacks begin displaced.
 *
 * Disarms the RAM guard. Staged spans stay in the arena until the next
 * begin but are no longer served.
 * @param cpu CPU to restore; NULL, or no preceding swap, restores nothing. */
void openpete_subtick_view_end(CPUState* cpu);

/** @brief Report whether a view is open.
 *
 * Feeders that behave differently on canonical and intermediate presents
 * can ask; resolving through openpete_subtick_host_ptr() needs no check.
 * @return 1 while open, else 0. */
int  openpete_subtick_view_active(void);

/** @brief Stage a guest range and return a writable host pointer to its copy.
 *
 * The copy is seeded from canonical guest memory, never from another span,
 * so a caller may overwrite only the fields it interpolates. Every later
 * resolve inside the span returns a pointer into this copy while the view
 * is open; where spans overlap the latest staged one wins. An address
 * already covered by a span with at least @p len bytes left returns the
 * existing copy, so several producers may refine one region.
 *
 * @p len must cover everything a feeder reads from the returned pointer: a
 * feeder that indexes past the span reads whatever the arena holds next,
 * the one failure mode here that is not reported.
 * @param addr Guest address of the first byte.
 * @param len Bytes to stage.
 * @return The copy, or NULL when the view is closed, @p len is 0, an
 *         existing span covers @p addr with fewer bytes (a partial
 *         overlap), the address is unmapped, the span or arena capacity is
 *         exhausted, or OPENPETE_SUBTICK_VIEW=0 disables staging. The
 *         partial-overlap, unmapped and capacity refusals are counted. On
 *         NULL the caller keeps the value canonical. */
void* openpete_subtick_view_stage(uint32_t addr, uint32_t len);

/** @brief Resolve one guest address through the view.
 *
 * Returns a pointer into the latest staged span covering @p addr (counted
 * as a served read) while the view is open, else openpete_psx_to_host_ptr().
 * The pointer carries no length: a producer that writes a window wider than
 * the span covering its first address uses openpete_subtick_view_poke(),
 * since a memcpy past the span's end lands in whatever the arena staged
 * next.
 * @param addr Guest address.
 * @return Host pointer, or NULL for an unmapped unstaged address. */
void* openpete_subtick_host_ptr(uint32_t addr);

/** @brief Resolve a guest span of @p len bytes through the view.
 *
 * While the view is open and a staged span covers @p addr, returns the
 * staged copy when that span holds all @p len bytes and NULL when it
 * holds fewer (a partial cover would mix staged and canonical bytes past
 * its end). Otherwise returns openpete_psx_to_host_span(), which refuses
 * a span crossing the end of RAM or the scratchpad.
 * @param addr Guest address of the first byte.
 * @param len Bytes the caller reads from the returned pointer.
 * @return Host pointer valid for @p len bytes, or NULL. */
void* openpete_subtick_host_span(uint32_t addr, uint32_t len);

/** @brief Publish producer output at a guest address through the view, span by span.
 *
 * Bytes a staged span covers land in that copy; each uncovered run is
 * staged first, and a run staging refuses is dropped, keeping canonical
 * bytes for this present. Guest memory is never written. With the view
 * closed the whole window is dropped. Dropped bytes add to
 * openpete_subtick_view_poke_drops().
 * @param addr Guest address of the first byte.
 * @param src Bytes to publish.
 * @param len Byte count.
 * @return Bytes dropped; 0 when the whole window was routed. */
uint32_t openpete_subtick_view_poke(uint32_t addr, const void* src, uint32_t len);

/** @brief Read guest bytes as the view presents them.
 *
 * Staged where a span covers, canonical elsewhere, and canonical throughout
 * while the view is closed; an unmapped run reads as zeros. The counterpart
 * of poke for a producer that saves a window before overwriting it.
 * @param addr Guest address of the first byte.
 * @param dst Receives @p len bytes.
 * @param len Byte count. */
void openpete_subtick_view_peek(uint32_t addr, void* dst, uint32_t len);

/** @brief Read the view's engagement counters for the most recent present.
 *
 * All three reset at openpete_subtick_view_begin(). Spans and served reads
 * are zero when the view did not engage, so a check that sees zero served
 * reads has not exercised the view. The write count covers every CPU store
 * (word, half, byte, swl, swr) no staged span covered while the view was
 * open; such a store is dropped, not performed, and a store into a staged
 * span lands in the copy and is not counted. A counted store is the display
 * path trying to mutate gameplay-readable memory, and the target is zero.
 * @param out_spans Receives the spans staged; may be NULL.
 * @param out_served Receives the reads served from staging; may be NULL.
 * @param out_writes Receives the dropped CPU stores; may be NULL. */
void openpete_subtick_view_stats(uint32_t* out_spans, uint32_t* out_served,
                                 uint32_t* out_writes);

/** @brief Return the number of times the view has been opened this run.
 *
 * Zero means an A/B or gate never reached the re-extract path, which makes
 * a neutral result from it vacuous rather than passing.
 * @return Lifetime open count. */
uint32_t openpete_subtick_view_opens(void);

/** @brief Return the run's count of writes absorbed by staged spans.
 *
 * Counts CPU stores a staged span absorbed and each chunk a poke copied
 * into an existing span; none of them reached guest memory. It is the
 * engagement counter for the write redirect: zero means no producer wrote
 * into staging this run, and a claim that no producer writes guest RAM
 * then describes a path that may not have executed.
 * @return Lifetime absorbed-write count. */
uint32_t openpete_subtick_view_staged_writes(void);
/** @brief Return the run's count of bytes a poke could not route.
 *
 * @return Lifetime dropped-byte count. */
uint32_t openpete_subtick_view_poke_drops(void);

/** @brief Log the run's staging refusals, poke drops, unaligned staged reads and dropped CPU stores.
 *
 * Called by openpete_native_render_shutdown. Each nonzero counter logs a
 * warning; dropped CPU stores are listed per host call site with width,
 * first guest address and count (64 sites at most), then every distinct
 * guest address a dropped store named is listed with its hit count (256
 * addresses at most, overflow noted). These cover CPU-path stores only; a producer
 * that writes through a host pointer is found by the RAM guard
 * (OPENPETE_SUBTICK_RAM_GUARD), whose row `RAM guard: armed_presents=N hits=M`
 * this logs first when the guard is on, followed by the store count of the
 * OPENPETE_SUBTICK_FAULT_STORE fault injection when that is set. */
void openpete_subtick_view_report_writes(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SUBTICK_VIEW_H */
