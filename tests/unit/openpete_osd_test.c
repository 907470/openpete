/* tests/unit/openpete_osd_test.c — the OSD toast ring on a synthetic clock.
 *
 * openpete_osd.c makes every timing decision from the `now` handed to
 * openpete_osd_present_update, so the whole state machine is exercisable
 * without a window, a present or a wall clock. What is checked here is the
 * part that is genuinely hard to see live:
 *
 *   1. coalescing — a repeated key repaints one toast instead of stacking,
 *      keeps its id (the two readers correlate on it), and takes the
 *      REPAINT's timeout;
 *   2. the ttl starts on the first present WITH GEOMETRY, not at notify() —
 *      a toast posted before the glyph font caches must be late, not lost;
 *   3. ID SKEW — a toast evicted between the tick that emitted its geometry
 *      and the present that composites it must not hand its slot to whoever
 *      inherited its ring index;
 *   4. eviction names a victim in every state, including "every entry is
 *      still rising";
 *   5. the rate limit prices new entries only, never repaints;
 *   6. the stack glides up when a toast above it retires.
 *
 * SDL's mutex API is faked below so the link line stays SDL-free like every
 * other console test here — the same trick keymap_test uses for its input
 * seams. The source under test is compiled unmodified. */

#include "../../core/include/openpete_osd.h"
#include "../../core/include/openpete_overlay_anchor.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- fakes ------------------------------------------------------------- */
/* The tests are single-threaded, so a counting no-op lock is enough — and it
 * doubles as an assertion that every path leaves the lock balanced. */
static int s_lock_depth, s_lock_max;
typedef struct SDL_Mutex SDL_Mutex;
SDL_Mutex *SDL_CreateMutex(void) { return (SDL_Mutex *)(void *)&s_lock_depth; }
void SDL_LockMutex(SDL_Mutex *m)   { (void)m; if (++s_lock_depth > s_lock_max) s_lock_max = s_lock_depth; }
void SDL_UnlockMutex(SDL_Mutex *m) { (void)m; --s_lock_depth; }
void SDL_DestroyMutex(SDL_Mutex *m) { (void)m; }
/* The mutex-once slot: single-threaded here, so a plain load and store. */
void *SDL_GetAtomicPointer(void **a) { return *a; }
bool SDL_CompareAndSwapAtomicPointer(void **a, void *oldval, void *newval) {
    if (*a != oldval) return false;
    *a = newval;
    return true;
}

/* The suppression predicates the notify path reads. */
int openpete_speculative = 0;
int openpete_replaying   = 0;

/* Not linked: overlay_anchor.c would drag the settings menu in. Only the
 * enum bound matters to openpete_osd.c. */
const char *const openpete_anchor_names[OPENPETE_ANCHOR_COUNT] = {
    "top-left", "top-center", "top-right",
    "bottom-left", "bottom-center", "bottom-right",
};

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); return 1; } while (0)

#define MS(n) ((op_time_t)(n) * OP_NS_PER_MS)
#define BOX_H 0.06f
/* A toast is still DRAWN while it fades out, so "expired" means the timeout
 * plus the fade-out. Padded generously — the tests assert the ordering of
 * events, not the exact easing durations. */
#define FADE_OUT_PAD_MS 600

/* Drive one present at `t`, declaring geometry for every live toast (the
 * normal steady state: the builder has emitted everything the ring holds). */
static int present_all(op_time_t t, openpete_osd_view_t *views, int cap) {
    openpete_osd_item_t items[OPENPETE_OSD_MAX];
    openpete_osd_ready_t ready[OPENPETE_OSD_MAX];
    int n = openpete_osd_tick_items(items, OPENPETE_OSD_MAX);
    for (int i = 0; i < n; i++) { ready[i].id = items[i].id; ready[i].box_h = BOX_H; }
    return openpete_osd_present_update(t, ready, n, views, cap);
}

/* Run presents at ~60 Hz for `ms` and report what the last one drew. State
 * transitions are edge-triggered per present (hold -> falling on one, retire
 * on a later one), which is how it actually runs; a single huge jump would
 * test a cadence nothing produces. */
static int advance(op_time_t *t, int ms, openpete_osd_view_t *views, int cap) {
    int n = 0;
    for (int e = 0; e < ms; e += 16) { *t += MS(16); n = present_all(*t, views, cap); }
    return n;
}

/* ---- 1 + 5: coalescing, and the bucket prices entries not repaints ------ */
static int test_coalesce(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(1000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];
    op_time_t t = MS(1000);

    openpete_osd_notify("host", "row.fps", 0, "FPS 60");
    int n = present_all(t, v, OPENPETE_OSD_MAX);
    if (n != 1) FAIL("first notify produced %d toast(s), want 1\n", n);
    const uint32_t id = v[0].id;

    /* 40 repaints on the same key — well past the burst of 4. A bucket that
     * charged repaints would stop updating the text a quarter second in and
     * leave a STALE value on screen while the menu showed the live one. */
    for (int i = 0; i < 40; i++) {
        openpete_osd_notify("host", "row.fps", 0, "FPS %d", 61 + i);
        t += MS(16);
        n = present_all(t, v, OPENPETE_OSD_MAX);
        if (n != 1) FAIL("repaint %d stacked a toast (n=%d)\n", i, n);
        if (v[0].id != id) FAIL("repaint %d changed the toast id\n", i);
    }
    openpete_osd_item_t items[OPENPETE_OSD_MAX];
    (void)openpete_osd_tick_items(items, OPENPETE_OSD_MAX);
    if (strcmp(items[0].text, "FPS 100") != 0)
        FAIL("repaint text not applied: got \"%s\"\n", items[0].text);

    /* The repaint's own timeout wins: the last one asked for 5 s, so the
     * toast must still be alive well past the 1 s default it was born with. */
    openpete_osd_notify("host", "row.fps", 5000, "FPS 100");
    if (advance(&t, 2000, v, OPENPETE_OSD_MAX) != 1)
        FAIL("toast expired on its ORIGINAL timeout, not the repaint's\n");
    if (advance(&t, 4000 + FADE_OUT_PAD_MS, v, OPENPETE_OSD_MAX) != 0)
        FAIL("toast outlived the repaint's timeout\n");
    return 0;
}

/* ---- 2: the ttl starts on the first present with geometry --------------- */
static int test_ttl_starts_at_first_geometry(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(1000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];
    openpete_osd_ready_t none = { 0, 0.0f };

    openpete_osd_notify("host", NULL, 0, "EARLY");
    /* Ten seconds of presents with NO geometry — the glyph font has not
     * cached yet. The toast must neither draw nor age. */
    for (int i = 0; i < 100; i++) {
        int n = openpete_osd_present_update(MS(1000 + 100 * i), &none, 0,
                                            v, OPENPETE_OSD_MAX);
        if (n != 0) FAIL("drew a toast with no published geometry\n");
    }
    if (!openpete_osd_any_live()) FAIL("toast was LOST while waiting for the font\n");

    /* Font arrives: it now runs its full life from here, not from notify(). */
    op_time_t t = MS(11000);
    if (present_all(t, v, OPENPETE_OSD_MAX) != 1) FAIL("toast did not appear once geometry existed\n");
    if (advance(&t, 900, v, OPENPETE_OSD_MAX) != 1)
        FAIL("toast expired early (ttl started at notify)\n");
    if (advance(&t, 1000 + FADE_OUT_PAD_MS, v, OPENPETE_OSD_MAX) != 0)
        FAIL("toast never expired\n");
    return 0;
}

/* ---- 3: id skew — geometry from a tick whose toast is already gone ------ */
static int test_id_skew(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(1000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];

    openpete_osd_notify("host", NULL, 0, "ONE");
    op_time_t t = MS(1000);
    present_all(t, v, OPENPETE_OSD_MAX);

    /* The sim thread publishes geometry for what it saw. */
    openpete_osd_item_t emitted[OPENPETE_OSD_MAX];
    int en = openpete_osd_tick_items(emitted, OPENPETE_OSD_MAX);
    if (en != 1) FAIL("expected one emitted toast, got %d\n", en);
    const uint32_t stale_id = emitted[0].id;

    /* Before the present that would composite it, the toast expires (timeout
     * AND fade-out) and a different one takes its ring slot. */
    if (advance(&t, 2000 + FADE_OUT_PAD_MS, v, OPENPETE_OSD_MAX) != 0)
        FAIL("setup: ONE did not retire\n");
    openpete_osd_notify("host", NULL, 0, "TWO");
    t += MS(16);

    /* Present with the STALE slot table. The new toast's id is not in it, so
     * nothing may draw — in particular the new toast must NOT be drawn into
     * the retired one's slot, which is where an index-keyed design would put
     * it (both are ring index 0). */
    openpete_osd_ready_t stale = { stale_id, BOX_H };
    int n = openpete_osd_present_update(t, &stale, 1, v, OPENPETE_OSD_MAX);
    if (n != 0) FAIL("drew %d toast(s) from a stale slot table\n", n);

    /* And the survivor is still there, waiting for its own geometry. */
    if (!openpete_osd_any_live()) FAIL("the new toast was dropped by the skew\n");
    n = present_all(t + MS(16), v, OPENPETE_OSD_MAX);
    if (n != 1) FAIL("new toast never drew once its geometry arrived\n");
    return 0;
}

/* ---- 4: eviction always names a victim ---------------------------------- */
static int test_evict_all_rising(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(5000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];

    /* Fill the ring and one more, with no present in between — every entry is
     * still RISING (indeed unstamped), the state the "oldest non-rising" rule
     * alone cannot resolve. The burst is 4 per source, so spread the posts
     * across sources: the point here is the ring rule, not the rate limit. */
    char src[8];
    for (int i = 0; i < OPENPETE_OSD_MAX + 3; i++) {
        snprintf(src, sizeof src, "s%d", i);
        openpete_osd_notify(src, NULL, 0, "T%d", i);
    }
    int n = present_all(MS(1000), v, OPENPETE_OSD_MAX);
    if (n != OPENPETE_OSD_MAX)
        FAIL("ring holds %d after an over-fill, want %d\n", n, OPENPETE_OSD_MAX);

    /* The NEWEST post must have survived: it is the one the user just caused,
     * and dropping it is the failure mode the fallback exists to prevent. */
    openpete_osd_item_t items[OPENPETE_OSD_MAX];
    int in = openpete_osd_tick_items(items, OPENPETE_OSD_MAX);
    if (strcmp(items[in - 1].text, "T10") != 0)
        FAIL("newest toast was evicted (last is \"%s\")\n", items[in - 1].text);
    return 0;
}

/* ---- 5b: the bucket does stop a runaway source -------------------------- */
static int test_rate_limit(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(5000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];
    /* Distinct keys, one source, no time passing: only the burst gets in. */
    for (int i = 0; i < 50; i++)
        openpete_osd_notify("mod:spam", NULL, 0, "S%d", i);
    int n = present_all(MS(1000), v, OPENPETE_OSD_MAX);
    if (n != 4) FAIL("burst let %d toast(s) through, want 4\n", n);
    return 0;
}

/* ---- 6: the stack glides up when a toast above retires ------------------ */
static int test_stack_glide(void) {
    openpete_osd_reset();
    openpete_osd_set_default_timeout_ms(1000);
    openpete_osd_view_t v[OPENPETE_OSD_MAX];
    op_time_t t = MS(1000);

    openpete_osd_notify("a", NULL, 500,  "TOP");
    openpete_osd_notify("b", NULL, 5000, "BELOW");
    int n = present_all(t, v, OPENPETE_OSD_MAX);
    if (n != 2) FAIL("expected 2 toasts, got %d\n", n);
    if (!(v[1].y > v[0].y)) FAIL("second toast did not stack below the first\n");
    const float stacked_y = v[1].y;

    /* Let the top one expire; the one below must MOVE toward the top slot
     * over several presents rather than teleport on the first. */
    /* Step one present at a time so the FIRST present after the retire can be
     * inspected: that is where a teleport would show. */
    n = 2;
    for (int e = 0; e < 3000 && n == 2; e += 16) { t += MS(16); n = present_all(t, v, OPENPETE_OSD_MAX); }
    if (n != 1) FAIL("top toast did not retire (n=%d)\n", n);
    if (v[0].y >= stacked_y) FAIL("survivor did not begin gliding up\n");
    if (v[0].y <= 0.5f * stacked_y)
        FAIL("survivor jumped most of the way in one present (y=%f of %f)\n",
             (double)v[0].y, (double)stacked_y);
    for (int i = 0; i < 60; i++) { t += MS(16); n = present_all(t, v, OPENPETE_OSD_MAX); }
    if (n != 1 || v[0].y > 0.002f)
        FAIL("survivor never settled into the top slot (y=%f)\n", (double)v[0].y);
    return 0;
}

/* ---- speculative / replayed ticks post nothing -------------------------- */
static int test_suppressed(void) {
    openpete_osd_reset();
    openpete_speculative = 1;
    openpete_osd_notify("host", NULL, 0, "SPECULATIVE");
    openpete_speculative = 0;
    openpete_replaying = 1;
    openpete_osd_notify("host", NULL, 0, "REPLAYED");
    openpete_replaying = 0;
    if (openpete_osd_any_live())
        FAIL("a speculative or replayed tick posted a toast\n");
    openpete_osd_notify("host", NULL, 0, "LIVE");
    if (!openpete_osd_any_live()) FAIL("a live tick failed to post\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_coalesce();
    rc |= test_ttl_starts_at_first_geometry();
    rc |= test_id_skew();
    rc |= test_evict_all_rising();
    rc |= test_rate_limit();
    rc |= test_stack_glide();
    rc |= test_suppressed();
    if (s_lock_depth != 0) { fprintf(stderr, "FAIL: lock left held (%d)\n", s_lock_depth); rc = 1; }
    if (s_lock_max == 0)   { fprintf(stderr, "FAIL: the ring was never locked\n"); rc = 1; }
    printf("openpete_osd_test: %s\n", rc ? "FAILED" : "ok");
    return rc;
}
