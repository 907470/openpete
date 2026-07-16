/* core/include/openpete_discontinuity.h — guest-state discontinuity broadcast.
 *
 * Fired whenever guest state (CPU regs + RAM + scratch + VRAM) is REPLACED
 * out from under the running process — savestate load today; rewind restore
 * and runahead rollback later — rather than advanced by the sim. Host-side
 * state DERIVED from guest RAM subscribes a resync action here so it never
 * carries pre-replacement snapshots across the jump (e.g. the interp
 * prev/curr tables, which would otherwise lerp a full-scene smear).
 *
 * Subscriber contract:
 *   - Called on the host thread at the savestate quiescent point (game fiber
 *     yielded inside VSync), AFTER the new guest state is fully committed
 *     (CPU + RAM + scratch + VRAM + mod participation blobs).
 *   - Do the CHEAPEST correct action. Content-keyed caches that self-verify
 *     against live RAM per lookup (moby faceblock cache, terrain sector
 *     cache) must NOT subscribe a blanket clear — same-level restores are
 *     zero-cost by design and that property is what keeps high-rate restore
 *     (rewind/runahead) affordable.
 *   - No ordering guarantees between subscribers.
 *   - Registration is append-only for the process lifetime (no unregister);
 *     register from module init, guarded so re-init doesn't double-add.
 *
 * Rate note: today this fires at user quick-load rate. The rewind/runahead
 * phases will fire their own lighter restore path for per-present
 * speculation; before wiring THIS broadcast into any high-rate path, cost-
 * audit the subscribers (texture_pack's rescan copies 1 MiB of VRAM).
 */

#ifndef OPENPETE_DISCONTINUITY_H
#define OPENPETE_DISCONTINUITY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openpete_discontinuity_fn)(void);

/* Register a resync action. Returns 0 on success, non-zero if the
 * subscriber table is full (logged; bump the cap in discontinuity.c). */
int openpete_on_guest_discontinuity(openpete_discontinuity_fn fn);

/* Fire all subscribers. Called by the savestate load path after commit. */
void openpete_guest_discontinuity_notify(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_DISCONTINUITY_H */
