// core/include/openpete_osd.h — the on-screen display: transient toast
// notifications, in the game's own gold-bordered chrome.
//
// A ring of at most OPENPETE_OSD_MAX toasts. Anyone host-side posts one with
// openpete_osd_notify(); they stack at a user-chosen anchor
// (openpete_overlay_anchor.h), fade and slide in, hold for their timeout, then
// fade out and let the ones below glide up.
//
// TWO READERS, ON TWO THREADS, AT TWO RATES. The glyph geometry is rebuilt on
// the SIM thread once per tick (the shimmer is baked into vertex colours at
// emit time, so it cannot be built once and reused); the layout, alpha and
// draw run on the PRESENT thread once per present. Neither is a snapshot of
// the other, so this module hands out two of them — openpete_osd_tick_items()
// for the builder and openpete_osd_present_update() for the compositor — and
// correlates them by TOAST ID, never by index. A toast can be evicted or
// repainted between the tick that emitted its geometry and the present that
// composites it; an index-keyed design samples the wrong toast at the wrong
// alpha when that happens.
//
// The clock is injected, not read: every timing decision is made from the
// `now` passed to openpete_osd_present_update(). Callers pass
// openpete_interp_present_now() (the PLL'd, deterministic-capture-pinned
// present clock), never raw op_now(). It also makes the whole state machine
// testable on a synthetic clock.
//
// Host-side only. Nothing here reads or writes gameplay state.

#ifndef OPENPETE_OSD_H
#define OPENPETE_OSD_H

#include <stdint.h>
#include "openpete_time.h"   /* op_time_t */

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_OSD_MAX        8    /* live toasts; the ring size            */
#define OPENPETE_OSD_TEXT_MAX  64    /* one toast's text, NUL included        */
#define OPENPETE_OSD_KEY_MAX   40    /* coalescing key (mod id prefix + key)  */
#define OPENPETE_OSD_SRC_MAX   24    /* rate-limit bucket name ("host", mod)  */

/* A timeout above this is clamped to it. The ring holds 8 toasts, so an
 * unclamped timeout would let one caller pin slots for the rest of the
 * session. */
#define OPENPETE_OSD_TIMEOUT_MAX_MS 30000u

/* ---- configuration (CLI / openpete.toml / the M overlay) ---------------- */
/** @brief Switch notifications on or off.
 *
 * Switching off calls openpete_osd_reset, so the queued toasts are dropped
 * rather than left without geometry and untimed, to reappear when switched
 * back on. While off, openpete_osd_notify posts nothing.
 * @param on Non-zero enables. */
void     openpete_osd_set_enabled(int on);
int      openpete_osd_enabled(void);
void     openpete_osd_set_anchor(int anchor);       /* openpete_anchor_t */
int      openpete_osd_anchor(void);
void     openpete_osd_set_default_timeout_ms(uint32_t ms);
uint32_t openpete_osd_default_timeout_ms(void);

/* ---- posting ------------------------------------------------------------ */

/** @brief Post a toast, or repaint the live toast that carries the same key.
 *
 * A live toast not yet fading out with the same non-empty key is repainted:
 * new text and the new timeout, its hold restarted at the next present, same
 * slot and same id. Otherwise a new toast takes a free slot, else the oldest
 * toast past its rise, else the oldest outright; a new toast debits one token
 * from its source's rate-limit bucket (burst 4, refill 2 per second) and is
 * dropped when the bucket is empty. A repaint is never charged. Safe from any
 * thread. A no-op while the OSD is off, when @p fmt is NULL, and while the
 * executing tick is speculative (runahead) or replayed (rewind), since those
 * ticks are rolled back or re-executed.
 * @param source The rate-limit bucket: "host" for the engine, the mod id for
 *        a mod; NULL or empty reads as "host".
 * @param key The coalescing key; NULL or empty always stacks a new toast.
 * @param timeout_ms The hold in milliseconds; 0 takes the configured
 *        default, and a value over OPENPETE_OSD_TIMEOUT_MAX_MS is clamped.
 * @param fmt printf format of the text, truncated to
 *        OPENPETE_OSD_TEXT_MAX - 1 bytes. */
void openpete_osd_notify(const char *source, const char *key,
                         uint32_t timeout_ms, const char *fmt, ...);

/* ---- the sim-thread reader: what to build geometry for ------------------ */
typedef struct {
    uint32_t id;
    char     text[OPENPETE_OSD_TEXT_MAX];
} openpete_osd_item_t;

/** @brief Copy the live toasts' ids and text, oldest first, for the glyph builder.
 *
 * Text only, no animation state: the builder bakes the shimmer, and the fade
 * is a present-rate quantity. Toasts still fading out are included.
 * @param out Receives up to @p cap items.
 * @param cap Capacity of @p out.
 * @return The count written; 0 when @p out is NULL or @p cap is 0 or less. */
int openpete_osd_tick_items(openpete_osd_item_t *out, int cap);

/** @brief Report whether any toast occupies the ring, for the compositor's early-out.
 *
 * @return 1 when a slot holds a toast in any state, else 0. */
int openpete_osd_any_live(void);

/* ---- the present-thread reader: where and how solid ---------------------- */
typedef struct {
    uint32_t id;
    float    alpha;   /* 0..1, the fade                                     */
    float    slide;   /* 0..1: 0 = fully off its edge, 1 = home             */
    float    y;       /* eased stack offset from the anchor edge, overlay u */
} openpete_osd_view_t;

/* Advance the state machine to `now` and report what to draw.
 *
 * `ready` lists the toast ids the CURRENT published geometry actually covers,
 * with the on-screen height of each box (overlay units) — the heights are what
 * the stack glide eases over. A toast whose id is absent has no geometry yet:
 * it is skipped entirely (drawing a slot that holds another toast's glyphs is
 * the failure this list exists to prevent) and its timer does not start. A
 * toast's ttl therefore begins on its first present WITH GEOMETRY, not at
 * notify() — an early toast, posted before the glyph font has ever cached, is
 * late, never lost.
 *
 * Expired toasts are retired here. Returns the number of views written, in the
 * same oldest-first order, and only for ids present in `ready`. */
typedef struct {
    uint32_t id;
    float    box_h;
} openpete_osd_ready_t;

int openpete_osd_present_update(op_time_t now,
                                const openpete_osd_ready_t *ready, int n_ready,
                                openpete_osd_view_t *out, int cap);

/** @brief Drop every toast and reset the rate-limit buckets, the id and age counters, and the stored clock.
 *
 * Called when notifications are switched off, where queued toasts would get
 * no geometry and no timer and would reappear when switched back on, and for
 * a test's fresh start. Not called on a savestate load: a toast announces a
 * setting changed or a key pressed, which is still true after the load. */
void openpete_osd_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_OSD_H */
