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
void     openpete_osd_set_enabled(int on);
int      openpete_osd_enabled(void);
void     openpete_osd_set_anchor(int anchor);       /* openpete_anchor_t */
int      openpete_osd_anchor(void);
void     openpete_osd_set_default_timeout_ms(uint32_t ms);
uint32_t openpete_osd_default_timeout_ms(void);

/* ---- posting ------------------------------------------------------------ */

/* Post a notification.
 *
 * `source` names the rate-limit bucket — "host" for everything the engine
 * itself raises, the mod id for a mod. NULL reads as "host".
 *
 * `key` (may be NULL) coalesces: a live, not-yet-fading toast with the same
 * key is REPAINTED — new text, timer restarted, same slot and same id — rather
 * than stacking a second one. Holding a key through the FPS ladder repaints one
 * toast instead of flooding the stack. A NULL key always stacks.
 *
 * `timeout_ms` is milliseconds; 0 takes the configured default and anything
 * over OPENPETE_OSD_TIMEOUT_MAX_MS is clamped. A repaint takes the RETARGETING
 * call's timeout, not the one the toast was created with — the repaint is the
 * more recent intent, so a recurring notification can escalate its own dwell
 * time without giving up its key (and so its slot).
 *
 * Safe from any thread. A no-op while the OSD is off, and while the tick being
 * executed is speculative (runahead) or replayed (rewind) — those ticks are
 * rolled back or re-executed, and a notification from one would announce an
 * event that never committed, or announce it again. */
void openpete_osd_notify(const char *source, const char *key,
                         uint32_t timeout_ms, const char *fmt, ...);

/* ---- the sim-thread reader: what to build geometry for ------------------ */
typedef struct {
    uint32_t id;
    char     text[OPENPETE_OSD_TEXT_MAX];
} openpete_osd_item_t;

/* Copy the live toasts, oldest first, for the glyph builder. Returns the count
 * written. Text only — no animation state: the builder bakes the shimmer, not
 * the fade, and the fade is a present-rate quantity. */
int openpete_osd_tick_items(openpete_osd_item_t *out, int cap);

/* 1 if any toast is live — the builder's cheap early-out. */
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

/* Drop every toast and reset the ring. Used when notifications are switched
 * off (leaving them queued would strand them: nothing publishes geometry for
 * them, so nothing starts their timers, and the whole pile would reappear the
 * moment they were switched back on) and for a test's fresh start.
 *
 * NOT called on a savestate load. A toast announces something the PLAYER did
 * — a setting they changed, a key they pressed — and that is still true after
 * a load; clearing it would read as the load having eaten their input. */
void openpete_osd_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_OSD_H */
